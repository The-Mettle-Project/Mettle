#include "code_generator_internal.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CODEGEN_PTR_SIZE 8

void code_generator_generate_global_variable(CodeGenerator *generator,
                                             ASTNode *var_declaration) {
  if (!generator || !var_declaration ||
      var_declaration->type != AST_VAR_DECLARATION) {
    return;
  }

  VarDeclaration *var_data = (VarDeclaration *)var_declaration->data;
  if (!var_data || !var_data->name) {
    return;
  }
  // `const` declarations are compile-time values folded at every use site;
  // they carry no storage, so emit nothing for them.
  if (var_data->is_const) {
    return;
  }
  if (var_data->is_extern) {
    const char *extern_name =
        code_generator_get_link_symbol_name(generator, var_data->name);
    code_generator_emit_extern_symbol(generator, extern_name);
    return;
  }

  // Add debug symbol for global variable
  if (generator->generate_debug_info) {
    code_generator_add_debug_symbol(
        generator, var_data->name, DEBUG_SYMBOL_VARIABLE, var_data->type_name,
        var_declaration->location.line, var_declaration->location.column);
  }

  // Calculate variable size
  int var_size = 0;
  Symbol *symbol = symbol_table_lookup(generator->symbol_table, var_data->name);
  if (symbol && symbol->type && symbol->type->size > 0) {
    var_size = (int)symbol->type->size;
  } else {
    var_size =
        code_generator_calculate_variable_size(generator, var_data->type_name);
  }
  if (var_size <= 0) {
    var_size = CODEGEN_PTR_SIZE;
  }

  code_generator_emit_to_global_buffer(
      generator, "; Global variable: %s (%s, %d bytes)\n", var_data->name,
      var_data->type_name ? var_data->type_name : "unknown", var_size);

  if (var_data->initializer) {
    // Initialized data goes in .data section
    code_generator_emit_to_global_buffer(generator, "%s:\n", var_data->name);

    // Generate initialization based on type and initializer
    if (var_data->initializer->type == AST_NUMBER_LITERAL) {
      NumberLiteral *num = (NumberLiteral *)var_data->initializer->data;
      if (num) {
        if (num->is_float) {
          // Handle different float types based on variable type
          if (var_data->type_name &&
              strcmp(var_data->type_name, "float32") == 0) {
            float float32_val = (float)num->float_value;
            code_generator_emit_to_global_buffer(generator, "    dd 0x%08x  ; float32: %f\n",
                                  *(int *)&float32_val, float32_val);
          } else {
            // Default to float64
            code_generator_emit_to_global_buffer(
                generator, "    dq 0x%016llx  ; float64: %f\n",
                *(long long *)&num->float_value, num->float_value);
          }
        } else {
          // Handle different integer sizes with proper bounds checking
          long long value = num->int_value;
          if (var_size == 1) {
            if (value < -128 || value > 255) {
              code_generator_emit_to_global_buffer(
                  generator,
                  "    ; Warning: Value %lld truncated to fit in %d bytes\n",
                  value, var_size);
            }
            code_generator_emit_to_global_buffer(generator, "    db %lld\n", value & 0xFF);
          } else if (var_size == 2) {
            if (value < -32768 || value > 65535) {
              code_generator_emit_to_global_buffer(
                  generator,
                  "    ; Warning: Value %lld truncated to fit in %d bytes\n",
                  value, var_size);
            }
            code_generator_emit_to_global_buffer(generator, "    dw %lld\n", value & 0xFFFF);
          } else if (var_size == 4) {
            if (value < -2147483648LL || value > 4294967295LL) {
              code_generator_emit_to_global_buffer(
                  generator,
                  "    ; Warning: Value %lld truncated to fit in %d bytes\n",
                  value, var_size);
            }
            code_generator_emit_to_global_buffer(generator, "    dd %lld\n",
                                  value & 0xFFFFFFFF);
          } else {
            code_generator_emit_to_global_buffer(generator, "    dq %lld\n", value);
          }
        }
      }
    } else if (var_data->initializer->type == AST_STRING_LITERAL) {
      StringLiteral *str = (StringLiteral *)var_data->initializer->data;
      if (str && str->value) {
        // For string variables, store pointer to string data
        if (var_data->type_name && strcmp(var_data->type_name, "string") == 0) {
          char *str_label = code_generator_generate_label(generator, "str");
          if (str_label) {
            code_generator_emit_to_global_buffer(
                generator, "    dq %s  ; Pointer to string data\n", str_label);
            code_generator_emit_to_global_buffer(
                generator, "    dq %zu  ; String length\n",
                strlen(str->value));
            code_generator_emit_to_global_buffer(generator, "%s:\n", str_label);
            if (!code_generator_emit_escaped_string_bytes(generator, str->value,
                                                          1)) {
              free(str_label);
              return;
            }
            free(str_label);
          }
        } else {
          // Direct string storage
          if (!code_generator_emit_escaped_string_bytes(generator, str->value,
                                                        1)) {
            return;
          }
        }
      }
    } else if (var_data->initializer->type == AST_IDENTIFIER) {
      // Initialize with another variable's value (copy initialization)
      code_generator_emit_to_global_buffer(
          generator, "    resb %d  ; Initialize from variable at runtime\n",
          var_size);
      code_generator_emit_to_global_buffer(
          generator,
          "    ; Runtime initialization needed for variable reference\n");
    } else {
      // For complex initializers (expressions, function calls, etc.), reserve
      // space and initialize at runtime
      code_generator_emit_to_global_buffer(
          generator,
          "    resb %d  ; Complex initializer - runtime initialization\n",
          var_size);
    }
  } else {
    // Uninitialized data goes in .bss section
    code_generator_emit_to_global_buffer(generator, "section .bss\n");
    code_generator_emit_to_global_buffer(generator, "%s:\n", var_data->name);
    code_generator_emit_to_global_buffer(generator, "    resb %d\n", var_size);
    code_generator_emit_to_global_buffer(generator, "section .data\n");
  }

  code_generator_emit_to_global_buffer(generator, "\n");
}

void code_generator_generate_local_variable(CodeGenerator *generator,
                                            ASTNode *var_declaration) {
  if (!generator || !var_declaration ||
      var_declaration->type != AST_VAR_DECLARATION) {
    return;
  }

  VarDeclaration *var_data = (VarDeclaration *)var_declaration->data;
  if (!var_data || !var_data->name) {
    return;
  }

  Symbol *symbol = symbol_table_lookup(generator->symbol_table, var_data->name);
  if (!symbol) {
    Type *var_type = NULL;
    if (var_data->type_name) {
      var_type = type_checker_get_type_by_name(generator->type_checker,
                                               var_data->type_name);
    } else if (var_data->initializer) {
      var_type = type_checker_infer_type(generator->type_checker,
                                         var_data->initializer);
    }
    if (!var_type) {
      var_type = type_checker_get_type_by_name(generator->type_checker, "int64");
    }

    Symbol *new_symbol = symbol_create(var_data->name, SYMBOL_VARIABLE, var_type);
    if (new_symbol && symbol_table_declare(generator->symbol_table, new_symbol)) {
      symbol = new_symbol;
    } else {
      symbol_destroy(new_symbol);
      symbol = symbol_table_lookup(generator->symbol_table, var_data->name);
    }
  }

  const char *resolved_type_name = var_data->type_name;
  if ((!resolved_type_name || resolved_type_name[0] == '\0') && symbol &&
      symbol->type && symbol->type->name) {
    resolved_type_name = symbol->type->name;
  }
  if (!resolved_type_name) {
    resolved_type_name = "unknown";
  }

  // Add debug symbol for local variable
  if (generator->generate_debug_info) {
    code_generator_add_debug_symbol(
        generator, var_data->name, DEBUG_SYMBOL_VARIABLE, resolved_type_name,
        var_declaration->location.line, var_declaration->location.column);
  }

  // Calculate variable size and allocate stack space
  int var_size = 0;
  if (symbol && symbol->type && symbol->type->size > 0) {
    var_size = (int)symbol->type->size;
  } else {
    var_size =
        code_generator_calculate_variable_size(generator, resolved_type_name);
  }
  if (var_size <= 0) {
    var_size = CODEGEN_PTR_SIZE;
  }

  int alignment = (var_size > 4) ? CODEGEN_PTR_SIZE : var_size;
  int offset =
      code_generator_allocate_stack_space(generator, var_size, alignment);

  code_generator_emit(
      generator, "    ; Local variable: %s (%s, %d bytes) at offset -%d\n",
      var_data->name, resolved_type_name, var_size, offset);

  // Update symbol table with memory offset if symbol exists
  if (symbol && symbol->kind == SYMBOL_VARIABLE) {
    symbol->data.variable.memory_offset = offset; // Positive offset for [rbp - offset]
    symbol->data.variable.is_in_register = 0;

    // Update debug symbol with stack offset
    if (generator->generate_debug_info) {
      debug_info_set_symbol_stack_offset(generator->debug_info, var_data->name,
                                         -offset);
    }
  }

  // Generate initialization code
  if (var_data->initializer) {
    code_generator_generate_variable_initialization(generator, symbol,
                                                    var_data->initializer);
  } else {
    // Zero-initialize the variable if no explicit initializer
    code_generator_generate_variable_zero_initialization(generator, symbol);
  }
}

void code_generator_generate_variable_initialization(CodeGenerator *generator,
                                                     Symbol *symbol,
                                                     ASTNode *initializer) {
  if (!generator || !symbol || !initializer) {
    return;
  }

  code_generator_emit(generator, "    ; Initialize variable %s\n",
                      symbol->name);

  // Generate the initializer expression
  code_generator_generate_expression(generator, initializer);
  if (symbol->type && symbol->type->kind == TYPE_POINTER &&
      symbol->type->name && strcmp(symbol->type->name, "cstring") == 0) {
    Type *initializer_type =
        code_generator_infer_expression_type(generator, initializer);
    if (initializer_type && initializer_type->kind == TYPE_STRING) {
      code_generator_emit(
          generator,
          "    mov rax, qword [rax]  ; Implicit string -> cstring init\n");
    }
  }

  // Store the result (in rax) to the variable's memory location
  if (symbol->data.variable.is_in_register) {
    // Handle register allocation - store to assigned register
    x86Register reg = (x86Register)symbol->data.variable.register_id;
    const char *reg_name = code_generator_get_register_name(reg);
    if (reg_name) {
      // Check if it's a floating-point register
      if (reg >= REG_XMM0 && reg <= REG_XMM15) {
        code_generator_emit(generator,
                            "    movsd xmm0, %s    ; Store float to register\n",
                            reg_name);
      } else {
        code_generator_emit(
            generator, "    mov %s, rax      ; Store to register\n", reg_name);
      }
    } else {
      code_generator_emit(generator, "    ; Error: Invalid register for %s\n",
                          symbol->name);
    }
  } else {
    // Store to stack location
    int offset = symbol->data.variable.memory_offset;

    // Handle different types and sizes properly
    if (symbol->type) {
      if (symbol->type->kind >= TYPE_FLOAT32 &&
          symbol->type->kind <= TYPE_FLOAT64) {
        // Floating-point types - use XMM registers
        if (symbol->type->size == 4) {
          code_generator_emit(generator,
                              "    movss [rbp - %d], xmm0  ; Store float32\n",
                              offset);
        } else {
          code_generator_emit(generator,
                              "    movsd [rbp - %d], xmm0  ; Store float64\n",
                              offset);
        }
      } else if (symbol->type->kind == TYPE_STRING) {
        // String type - store pointer
        code_generator_emit(
            generator, "    mov [rbp - %d], rax     ; Store string pointer\n",
            offset);
      } else {
        // Integer types - use appropriate register size
        if (symbol->type->size == 1) {
          code_generator_emit(
              generator, "    mov [rbp - %d], al      ; Store int8\n", offset);
        } else if (symbol->type->size == 2) {
          code_generator_emit(
              generator, "    mov [rbp - %d], ax      ; Store int16\n", offset);
        } else if (symbol->type->size == 4) {
          code_generator_emit(
              generator, "    mov [rbp - %d], eax     ; Store int32\n", offset);
        } else {
          code_generator_emit(
              generator, "    mov [rbp - %d], rax     ; Store int64\n", offset);
        }
      }
    } else {
      // Fallback for unknown types
      code_generator_emit(generator,
                          "    mov [rbp - %d], rax     ; Store (unknown type)\n",
                          offset);
    }
  }
}

void code_generator_generate_variable_zero_initialization(
    CodeGenerator *generator, Symbol *symbol) {
  if (!generator || !symbol) {
    return;
  }

  code_generator_emit(generator, "    ; Zero-initialize variable %s\n",
                      symbol->name);

  if (symbol->data.variable.is_in_register) {
    // Zero-initialize register
    x86Register reg = (x86Register)symbol->data.variable.register_id;
    const char *reg_name = code_generator_get_register_name(reg);
    if (reg_name) {
      if (reg >= REG_XMM0 && reg <= REG_XMM15) {
        code_generator_emit(generator,
                            "    xorps %s, %s      ; Zero float register\n",
                            reg_name, reg_name);
      } else {
        code_generator_emit(generator,
                            "    xor %s, %s        ; Zero integer register\n",
                            reg_name, reg_name);
      }
    }
  } else {
    // Zero-initialize memory location
    int offset = symbol->data.variable.memory_offset;

    if (symbol->type) {
      if (symbol->type->kind == TYPE_ARRAY && symbol->type->size > 0) {
        char *zero_loop = code_generator_generate_label(generator, "zero_loop");
        char *zero_done = code_generator_generate_label(generator, "zero_done");
        if (!zero_loop || !zero_done) {
          free(zero_loop);
          free(zero_done);
          code_generator_set_error(generator,
                                   "Failed to allocate labels for zero init");
          return;
        }

        // Avoid clobbering non-volatile registers (e.g. rdi) during local
        // array zero-initialization.
        code_generator_emit(generator, "    lea rax, [rbp - %d]  ; Array start\n",
                            offset);
        code_generator_emit(generator, "    mov rcx, %zu\n", symbol->type->size);
        code_generator_emit(generator, "    test rcx, rcx\n");
        code_generator_emit(generator, "    jz %s\n", zero_done);
        code_generator_emit(generator, "%s:\n", zero_loop);
        code_generator_emit(generator, "    mov byte [rax], 0\n");
        code_generator_emit(generator, "    inc rax\n");
        code_generator_emit(generator, "    dec rcx\n");
        code_generator_emit(generator, "    jnz %s\n", zero_loop);
        code_generator_emit(generator, "%s:\n", zero_done);
        free(zero_loop);
        free(zero_done);
        return;
      }

      if (symbol->type->kind >= TYPE_FLOAT32 &&
          symbol->type->kind <= TYPE_FLOAT64) {
        // Zero floating-point variable
        if (symbol->type->size == 4) {
          code_generator_emit(generator,
                              "    mov dword [rbp - %d], 0       ; Zero float32\n",
                              offset);
        } else {
          code_generator_emit(generator,
                              "    mov qword [rbp - %d], 0       ; Zero float64\n",
                              offset);
        }
      } else {
        // Zero integer or pointer variable
        if (symbol->type->size == 1) {
          code_generator_emit(
              generator, "    mov byte [rbp - %d], 0       ; Zero int8\n", offset);
        } else if (symbol->type->size == 2) {
          code_generator_emit(
              generator, "    mov word [rbp - %d], 0       ; Zero int16\n", offset);
        } else if (symbol->type->size == 4) {
          code_generator_emit(
              generator, "    mov dword [rbp - %d], 0       ; Zero int32\n", offset);
        } else {
          code_generator_emit(
              generator, "    mov qword [rbp - %d], 0       ; Zero int64/pointer\n",
              offset);
        }
      }
    } else {
      // Default zero initialization
      code_generator_emit(generator,
                          "    mov qword [rbp - %d], 0       ; Zero (unknown type)\n",
                          offset);
    }
  }
}

int code_generator_calculate_variable_size(CodeGenerator *generator,
                                           const char *type_name) {
  (void)generator; // Suppress unused parameter warning

  if (!type_name) {
    return CODEGEN_PTR_SIZE;
  }

  // Basic type size mapping - comprehensive coverage
  if (strcmp(type_name, "int8") == 0 || strcmp(type_name, "uint8") == 0) {
    return 1;
  } else if (strcmp(type_name, "int16") == 0 ||
             strcmp(type_name, "uint16") == 0) {
    return 2;
  } else if (strcmp(type_name, "int32") == 0 ||
             strcmp(type_name, "uint32") == 0) {
    return 4;
  } else if (strcmp(type_name, "int64") == 0 ||
             strcmp(type_name, "uint64") == 0) {
    return 8;
  } else if (strcmp(type_name, "float32") == 0) {
    return 4;
  } else if (strcmp(type_name, "float64") == 0) {
    return 8;
  } else if (strcmp(type_name, "string") == 0) {
    return CODEGEN_PTR_SIZE;
  } else if (strcmp(type_name, "void") == 0) {
    return 0; // Void type has no size
  } else if (strstr(type_name, "*") != NULL) {
    return CODEGEN_PTR_SIZE;
  } else if (strstr(type_name, "[") != NULL) {
    const char *lbracket = strchr(type_name, '[');
    const char *rbracket = lbracket ? strchr(lbracket, ']') : NULL;
    if (lbracket && rbracket && rbracket > lbracket + 1) {
      size_t base_len = (size_t)(lbracket - type_name);
      char base_type[128];
      if (base_len > 0 && base_len < sizeof(base_type)) {
        memcpy(base_type, type_name, base_len);
        base_type[base_len] = '\0';
        long long count = atoll(lbracket + 1);
        if (count > 0) {
          int base_size =
              code_generator_calculate_variable_size(generator, base_type);
          if (base_size > 0 && count <= (LLONG_MAX / base_size)) {
            return (int)(base_size * count);
          }
        }
      }
    }
    return CODEGEN_PTR_SIZE;
  }

  // Check if it's a struct type by looking it up in symbol table
  if (generator && generator->symbol_table) {
    Symbol *type_symbol =
        symbol_table_lookup(generator->symbol_table, type_name);
    if (type_symbol && type_symbol->kind == SYMBOL_STRUCT &&
        type_symbol->type) {
      return (int)type_symbol->type->size;
    }
  }

  return CODEGEN_PTR_SIZE;
}

int code_generator_allocate_stack_space(CodeGenerator *generator, int size,
                                        int alignment) {
  if (!generator) {
    return 0;
  }

  // Ensure minimum alignment of 1
  if (alignment < 1) {
    alignment = 1;
  }

  // Align current offset to the required alignment
  int aligned_offset = generator->current_stack_offset;
  if (alignment > 1) {
    aligned_offset = (aligned_offset + alignment - 1) & ~(alignment - 1);
  }

  // Allocate space for the variable
  aligned_offset += size;

  // Update the current stack offset
  generator->current_stack_offset = aligned_offset;

  return aligned_offset;
}

