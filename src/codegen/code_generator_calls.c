#include "code_generator_internal.h"
#include <stdio.h>
#include <string.h>

// Function call implementation functions
void code_generator_generate_function_call(CodeGenerator *generator,
                                           ASTNode *call_expression) {
  if (!generator || !call_expression ||
      call_expression->type != AST_FUNCTION_CALL) {
    code_generator_set_error(generator, "Invalid function call AST node");
    return;
  }

  CallExpression *call_data = (CallExpression *)call_expression->data;
  if (!call_data || !call_data->function_name) {
    code_generator_set_error(generator, "Malformed function call expression");
    return;
  }

  // Check if this is a method call (has an object)
  if (call_data->object) {
    // Method call with name mangling
    // Try to determine the struct type for proper name mangling
    char mangled_name[256];
    const char *struct_name = NULL;

    if (call_data->object->type == AST_IDENTIFIER) {
      Identifier *id_data = (Identifier *)call_data->object->data;
      if (id_data && id_data->name) {
        Symbol *obj_symbol =
            symbol_table_lookup(generator->symbol_table, id_data->name);
        if (obj_symbol && obj_symbol->type && obj_symbol->type->name) {
          struct_name = obj_symbol->type->name;
        }
      }
    }

    if (struct_name) {
      snprintf(mangled_name, sizeof(mangled_name), "%s_%s", struct_name,
               call_data->function_name);
    } else {
      snprintf(mangled_name, sizeof(mangled_name), "%s",
               call_data->function_name);
    }

    code_generator_emit(generator, "    ; Method call: %s (mangled: %s)\n",
                        call_data->function_name, mangled_name);

    // Generate the method call using the existing helper
    code_generator_generate_method_call(generator, call_expression,
                                        call_data->object);

    // Patch the emitted call target to use the mangled name
    // (The method_call function already emitted the call, so we handle name
    // mangling there)
    return;
  }

  code_generator_emit(generator, "    ; Function call: %s\n",
                      call_data->function_name);

  // Get calling convention spec from register allocator
  CallingConventionSpec *conv_spec =
      generator->register_allocator->calling_convention;
  if (!conv_spec) {
    code_generator_set_error(generator, "No calling convention configured");
    return;
  }

  // Look up function symbol to get return type
  Symbol *func_symbol =
      symbol_table_lookup(generator->symbol_table, call_data->function_name);
  Type *return_type = NULL;
  if (func_symbol && func_symbol->kind == SYMBOL_FUNCTION &&
      func_symbol->type) {
    return_type = func_symbol->type;
  }
  const char *call_target =
      code_generator_get_link_symbol_name(generator, call_data->function_name);
  if (!call_target) {
    code_generator_set_error(generator, "Invalid call target name");
    return;
  }
  if (func_symbol && func_symbol->is_extern) {
    if (!code_generator_emit_extern_symbol(generator, call_target)) {
      return;
    }
  }

  // 1. Align stack for function call (x86-64 requires 16-byte alignment)
  code_generator_align_stack_for_call(generator);

  // 2. Pass parameters according to calling convention
  code_generator_generate_parameter_passing(
      generator, call_data->arguments, call_data->argument_count, func_symbol);

  // 3. Generate the call instruction
  code_generator_emit(generator, "    call %s\n", call_target);

  // 4. Clean up stack if needed (for parameters passed on stack)
  code_generator_cleanup_stack_after_call(generator, call_data->arguments,
                                          call_data->argument_count);

  // 5. Handle return value based on type
  code_generator_handle_return_value(generator, return_type);
  // 6. Return value is now properly positioned
  if (return_type && code_generator_is_floating_point_type(return_type)) {
    code_generator_emit(generator, "    ; Return value in xmm0 (float)\n");
  } else if (return_type && return_type->name &&
             strcmp(return_type->name, "void") != 0) {
    code_generator_emit(generator,
                        "    ; Return value in rax (integer/pointer)\n");
  } else {
    code_generator_emit(generator, "    ; Void function - no return value\n");
  }
}

void code_generator_generate_parameter_passing(CodeGenerator *generator,
                                               ASTNode **arguments,
                                               size_t argument_count,
                                               Symbol *func_symbol) {
  if (!generator || !arguments || argument_count == 0) {
    return;
  }

  CallingConventionSpec *conv_spec =
      generator->register_allocator->calling_convention;
  if (!conv_spec) {
    return;
  }

  code_generator_emit(generator, "    ; Passing %zu parameters\n",
                      argument_count);

  // Track register usage for parameter passing
  int int_reg_index = 0;
  int float_reg_index = 0;
  int stack_param_count = 0;

  // Pass parameters in reverse order for stack parameters (right-to-left)
  // But register parameters are passed in forward order

  // First pass: count stack parameters and prepare
  for (size_t i = 0; i < argument_count; i++) {
    if (arguments[i]) {
      // Determine parameter type (simplified type inference)
      Type *param_type =
          code_generator_infer_expression_type(generator, arguments[i]);
      int is_float = code_generator_is_floating_point_type(param_type);

      if (is_float) {
        if (float_reg_index >= (int)conv_spec->float_param_count) {
          stack_param_count++;
        }
        float_reg_index++;
      } else {
        if (int_reg_index >= (int)conv_spec->int_param_count) {
          stack_param_count++;
        }
        int_reg_index++;
      }
    }
  }

  // Reset counters for actual parameter passing
  int_reg_index = 0;
  float_reg_index = 0;

  // Second pass: generate parameters in correct order
  // Stack parameters need to be pushed in reverse order (right-to-left)
  if (stack_param_count > 0) {
    // Process stack parameters in reverse order
    for (int i = (int)argument_count - 1; i >= 0; i--) {
      if (arguments[i]) {
        Type *param_type =
            code_generator_infer_expression_type(generator, arguments[i]);
        int is_float = code_generator_is_floating_point_type(param_type);

        // Check if this parameter goes on stack
        int goes_on_stack = 0;
        if (is_float && float_reg_index >= (int)conv_spec->float_param_count) {
          goes_on_stack = 1;
        } else if (!is_float &&
                   int_reg_index >= (int)conv_spec->int_param_count) {
          goes_on_stack = 1;
        }

        if (goes_on_stack) {
          code_generator_generate_parameter(generator, arguments[i], i,
                                            param_type, func_symbol);
        }

        // Update counters (counting backwards)
        if (is_float) {
          float_reg_index++;
        } else {
          int_reg_index++;
        }
      }
    }
  }

  // Reset counters for register parameters
  int_reg_index = 0;
  float_reg_index = 0;

  // Third pass: generate register parameters in forward order
  for (size_t i = 0; i < argument_count; i++) {
    if (arguments[i]) {
      Type *param_type =
          code_generator_infer_expression_type(generator, arguments[i]);
      int is_float = code_generator_is_floating_point_type(param_type);

      // Check if this parameter goes in register
      int goes_in_register = 0;
      if (is_float && float_reg_index < (int)conv_spec->float_param_count) {
        goes_in_register = 1;
      } else if (!is_float && int_reg_index < (int)conv_spec->int_param_count) {
        goes_in_register = 1;
      }

      if (goes_in_register) {
        code_generator_generate_parameter(generator, arguments[i], (int)i,
                                          param_type, func_symbol);
      }

      // Update counters
      if (is_float) {
        float_reg_index++;
      } else {
        int_reg_index++;
      }
    }
  }
}

void code_generator_generate_parameter(CodeGenerator *generator,
                                       ASTNode *argument, int param_index,
                                       Type *param_type, Symbol *func_symbol) {
  if (!generator || !argument) {
    return;
  }

  CallingConventionSpec *conv_spec =
      generator->register_allocator->calling_convention;
  if (!conv_spec) {
    return;
  }

  // Calls may implicitly pass string.chars to cstring parameters.
  int is_string_to_cstring = 0;
  if (func_symbol && func_symbol->kind == SYMBOL_FUNCTION &&
      param_index < (int)func_symbol->data.function.parameter_count) {
    Type *expected_type =
        func_symbol->data.function.parameter_types[param_index];
    Type *argument_type =
        code_generator_infer_expression_type(generator, argument);
    if (expected_type && expected_type->name && argument_type &&
        strcmp(expected_type->name, "cstring") == 0 &&
        argument_type->kind == TYPE_STRING) {
      is_string_to_cstring = 1;
    }
  }

  // Generate the argument expression
  if (is_string_to_cstring && argument->type == AST_STRING_LITERAL) {
    StringLiteral *str_data = (StringLiteral *)argument->data;
    if (str_data && str_data->value) {
      code_generator_load_string_literal_as_cstring(generator, str_data->value);
    } else {
      code_generator_set_error(generator, "Malformed string literal");
    }
  } else {
    code_generator_generate_expression(generator, argument);
    if (is_string_to_cstring && !generator->has_error) {
      code_generator_emit(
          generator,
          "    mov rax, qword [rax]  ; Implicit extern string -> cstring\n");
    }
  }

  // Determine parameter type if not provided
  if (!param_type) {
    param_type = code_generator_infer_expression_type(generator, argument);
  }

  int is_float = code_generator_is_floating_point_type(param_type);

  // Track register indices separately for int and float parameters
  static int int_reg_index = 0;
  static int float_reg_index = 0;

  // Reset counters at start of function call (this is a simplification)
  if (param_index == 0) {
    int_reg_index = 0;
    float_reg_index = 0;
  }

  if (is_float) {
    // Floating-point parameter
    if (float_reg_index < (int)conv_spec->float_param_count) {
      // Parameter goes in floating-point register
      x86Register param_reg = conv_spec->float_param_registers[float_reg_index];
      const char *reg_name = code_generator_get_register_name(param_reg);

      if (reg_name) {
        code_generator_emit(
            generator,
            "    movsd xmm0, %s    ; Float parameter %d in register\n",
            reg_name, param_index + 1);
      }
      float_reg_index++;
    } else {
      // Parameter goes on stack
      code_generator_emit(
          generator,
          "    sub rsp, 8        ; Allocate stack space for float\n");
      code_generator_emit(
          generator, "    movsd [rsp], xmm0 ; Float parameter %d on stack\n",
          param_index + 1);
    }
  } else {
    // Integer/pointer parameter
    if (int_reg_index < (int)conv_spec->int_param_count) {
      // Parameter goes in integer register
      x86Register param_reg = conv_spec->int_param_registers[int_reg_index];
      const char *reg_name = code_generator_get_register_name(param_reg);

      if (reg_name) {
        // Handle different parameter sizes
        const char *src_reg = "rax";
        const char *dst_reg = reg_name;

        if (param_type && param_type->size == 4) {
          // 32-bit parameter - use 32-bit register names
          src_reg = "eax";
          if (strcmp(reg_name, "rdi") == 0)
            dst_reg = "edi";
          else if (strcmp(reg_name, "rsi") == 0)
            dst_reg = "esi";
          else if (strcmp(reg_name, "rdx") == 0)
            dst_reg = "edx";
          else if (strcmp(reg_name, "rcx") == 0)
            dst_reg = "ecx";
          else if (strcmp(reg_name, "r8") == 0)
            dst_reg = "r8d";
          else if (strcmp(reg_name, "r9") == 0)
            dst_reg = "r9d";
        }

        code_generator_emit(
            generator, "    mov %s, %s    ; Integer parameter %d in register\n",
            dst_reg, src_reg, param_index + 1);
      }
      int_reg_index++;
    } else {
      // Parameter goes on stack
      code_generator_emit(
          generator,
          "    push rax          ; Integer parameter %d on stack\n",
          param_index + 1);
    }
  }
}

// Clean up stack after function call
void code_generator_cleanup_stack_after_call(CodeGenerator *generator,
                                             ASTNode **arguments,
                                             size_t argument_count) {
  if (!generator) {
    return;
  }

  CallingConventionSpec *conv_spec =
      generator->register_allocator->calling_convention;
  if (!conv_spec) {
    return;
  }

  // Calculate how many parameters were pushed on stack
  int stack_bytes = 0;
  int int_reg_used = 0;
  int float_reg_used = 0;

  for (size_t i = 0; i < argument_count; i++) {
    if (arguments[i]) {
      Type *param_type =
          code_generator_infer_expression_type(generator, arguments[i]);
      int is_float = code_generator_is_floating_point_type(param_type);

      if (is_float) {
        if (float_reg_used >= (int)conv_spec->float_param_count) {
          stack_bytes += 8; // Each stack parameter is 8 bytes
        }
        float_reg_used++;
      } else {
        if (int_reg_used >= (int)conv_spec->int_param_count) {
          stack_bytes += 8; // Each stack parameter is 8 bytes
        }
        int_reg_used++;
      }
    }
  }

  // Clean up stack parameters
  if (stack_bytes > 0) {
    code_generator_emit(
        generator,
        "    add rsp, %d    ; Clean up %d bytes of stack parameters\n",
        stack_bytes, stack_bytes);
  }

  // Clean up shadow space for Microsoft x64
  if (conv_spec->convention == CALLING_CONV_MS_X64 &&
      conv_spec->shadow_space_size > 0) {
    code_generator_emit(generator,
                        "    add rsp, %d      ; Clean up shadow space\n",
                        conv_spec->shadow_space_size);
  }

  // Clean up alignment padding if it was added
  // This is a simplified approach - in practice, we'd track the exact padding
  code_generator_emit(generator, "    ; Stack cleanup complete\n");
}

// Handle return value based on type
void code_generator_handle_return_value(CodeGenerator *generator,
                                        Type *return_type) {
  if (!generator) {
    return;
  }

  if (!return_type || !return_type->name) {
    code_generator_emit(generator,
                        "    ; Unknown return type - assuming integer\n");
    return;
  }

  if (strcmp(return_type->name, "void") == 0) {
    code_generator_emit(generator, "    ; Void return - no value to handle\n");
    return;
  }

  if (code_generator_is_floating_point_type(return_type)) {
    code_generator_emit(generator, "    ; Float return value in xmm0\n");
    // If we need to store it somewhere else, we can move it
    // For now, leave it in XMM0 as that's where expressions expect it
  } else {
    code_generator_emit(generator,
                        "    ; Integer/pointer return value in rax\n");
    // Handle different integer sizes if needed
    if (return_type->size == 1) {
      code_generator_emit(
          generator, "    movzx eax, al    ; Zero-extend 8-bit return value\n");
    } else if (return_type->size == 2) {
      code_generator_emit(
          generator,
          "    movzx eax, ax    ; Zero-extend 16-bit return value\n");
    } else if (return_type->size == 4) {
      code_generator_emit(generator,
                          "    ; 32-bit return value already in eax\n");
    }
    // 64-bit values are already in full RAX
  }
}
void code_generator_align_stack_for_call(CodeGenerator *generator) {
  if (!generator) {
    return;
  }

  CallingConventionSpec *conv_spec =
      generator->register_allocator->calling_convention;
  if (!conv_spec) {
    return;
  }

  // Add shadow space for Microsoft x64 calling convention
  if (conv_spec->convention == CALLING_CONV_MS_X64 &&
      conv_spec->shadow_space_size > 0) {
    code_generator_emit(generator,
                        "    sub rsp, %d      ; Allocate shadow space\n",
                        conv_spec->shadow_space_size);
  }
}

// Helper function to infer expression type (simplified implementation)
Type *code_generator_infer_expression_type(CodeGenerator *generator,
                                           ASTNode *expression) {
  if (!generator || !expression) {
    return NULL;
  }

  if (generator->type_checker) {
    Type *semantic_type =
        type_checker_infer_type(generator->type_checker, expression);
    if (semantic_type) {
      return semantic_type;
    }
  }

  // Create a basic type structure for return
  static Type int_type = {
      .kind = TYPE_INT32, .name = "int32", .size = 4, .alignment = 4};
  static Type float_type = {
      .kind = TYPE_FLOAT64, .name = "float64", .size = 8, .alignment = 8};
  static Type string_type = {.kind = TYPE_STRING,
                             .name = "string",
                             .size = 8,
                             .alignment = 8}; // pointer size

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *num = (NumberLiteral *)expression->data;
    if (num && num->is_float) {
      return &float_type;
    } else {
      return &int_type;
    }
  }
  case AST_STRING_LITERAL:
    return &string_type;

  case AST_IDENTIFIER: {
    // Look up variable type in symbol table
    Identifier *id = (Identifier *)expression->data;
    if (id && id->name) {
      Symbol *symbol = symbol_table_lookup(generator->symbol_table, id->name);
      if (symbol && symbol->type) {
        return symbol->type;
      }
    }
    return &int_type; // Default fallback
  }

  case AST_FUNCTION_CALL: {
    // Look up function return type
    CallExpression *call = (CallExpression *)expression->data;
    if (call && call->function_name) {
      Symbol *func_symbol =
          symbol_table_lookup(generator->symbol_table, call->function_name);
      if (func_symbol && func_symbol->kind == SYMBOL_FUNCTION &&
          func_symbol->type) {
        return func_symbol->type;
      }
    }
    return &int_type; // Default fallback
  }

  case AST_BINARY_EXPRESSION: {
    // For binary expressions, use the type of the left operand (simplified)
    BinaryExpression *bin = (BinaryExpression *)expression->data;
    if (bin && bin->left) {
      return code_generator_infer_expression_type(generator, bin->left);
    }
    return &int_type;
  }

  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index_expr = (ArrayIndexExpression *)expression->data;
    if (index_expr && index_expr->array) {
      Type *array_type =
          code_generator_infer_expression_type(generator, index_expr->array);
      if (array_type &&
          (array_type->kind == TYPE_ARRAY ||
           array_type->kind == TYPE_POINTER) &&
          array_type->base_type) {
        return array_type->base_type;
      }
    }
    return &int_type;
  }

  default:
    return &int_type; // Default fallback
  }
}

/* True for struct-by-value and fixed-size array-by-value types.
 * String, pointer, and pointer-like 8-byte scalars are NOT aggregates. */
int code_generator_type_is_aggregate(const Type *type) {
  if (!type) {
    return 0;
  }
  /* Tagged enums have a fixed struct-like layout (int32 tag + payload union)
   * and follow the same ABI rules as a struct of the same size/alignment. */
  return type->kind == TYPE_STRUCT || type->kind == TYPE_ARRAY ||
         type->kind == TYPE_TAGGED_ENUM;
}

size_t code_generator_abi_type_size(const Type *type) {
  if (!type) {
    return 0;
  }
  if (type->size > 0) {
    return type->size;
  }
  /* Scalars without explicit size: fall back to pointer width. */
  return 8;
}

/* Microsoft x64 ABI classification. See docs/struct-abi-design.md.
 * Aggregates are INDIRECT when sizeof > 8, or when sizeof <= 8 but not a
 * power of two. Scalars are always DIRECT. */
AbiPassKind code_generator_abi_classify(const Type *type) {
  if (!type) {
    return ABI_PASS_DIRECT;
  }
  if (!code_generator_type_is_aggregate(type)) {
    return ABI_PASS_DIRECT;
  }
  size_t s = code_generator_abi_type_size(type);
  if (s == 0) {
    /* Opaque/forward-declared aggregate: be conservative, pass indirectly. */
    return ABI_PASS_INDIRECT;
  }
  if (s > 8) {
    return ABI_PASS_INDIRECT;
  }
  if (s == 1 || s == 2 || s == 4 || s == 8) {
    return ABI_PASS_DIRECT;
  }
  return ABI_PASS_INDIRECT;
}

// Helper function to check if a type is floating-point
int code_generator_is_floating_point_type(Type *type) {
  if (!type || !type->name) {
    return 0;
  }

  return (strcmp(type->name, "float32") == 0 ||
          strcmp(type->name, "float64") == 0 ||
          strcmp(type->name, "double") == 0 ||
          strcmp(type->name, "float") == 0);
}

const char *code_generator_get_register_name(x86Register reg) {
  switch (reg) {
  case REG_RAX:
    return "rax";
  case REG_RBX:
    return "rbx";
  case REG_RCX:
    return "rcx";
  case REG_RDX:
    return "rdx";
  case REG_RSI:
    return "rsi";
  case REG_RDI:
    return "rdi";
  case REG_RSP:
    return "rsp";
  case REG_RBP:
    return "rbp";
  case REG_R8:
    return "r8";
  case REG_R9:
    return "r9";
  case REG_R10:
    return "r10";
  case REG_R11:
    return "r11";
  case REG_R12:
    return "r12";
  case REG_R13:
    return "r13";
  case REG_R14:
    return "r14";
  case REG_R15:
    return "r15";
  case REG_XMM0:
    return "xmm0";
  case REG_XMM1:
    return "xmm1";
  case REG_XMM2:
    return "xmm2";
  case REG_XMM3:
    return "xmm3";
  case REG_XMM4:
    return "xmm4";
  case REG_XMM5:
    return "xmm5";
  case REG_XMM6:
    return "xmm6";
  case REG_XMM7:
    return "xmm7";
  case REG_XMM8:
    return "xmm8";
  case REG_XMM9:
    return "xmm9";
  case REG_XMM10:
    return "xmm10";
  case REG_XMM11:
    return "xmm11";
  case REG_XMM12:
    return "xmm12";
  case REG_XMM13:
    return "xmm13";
  case REG_XMM14:
    return "xmm14";
  case REG_XMM15:
    return "xmm15";
  default:
    return NULL;
  }
}
