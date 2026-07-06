#include "code_generator_internal.h"
#include "compiler/compiler_context.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CodeGenerator *code_generator_create(SymbolTable *symbol_table,
                                     TypeChecker *type_checker,
                                     RegisterAllocator *allocator) {
  CodeGenerator *generator = malloc(sizeof(CodeGenerator));
  if (!generator) {
    return NULL;
  }

  generator->symbol_table = symbol_table;
  generator->type_checker = type_checker;
  generator->register_allocator = allocator;
  generator->debug_info = NULL;
  generator->current_label_id = 0;
  generator->current_function_name = NULL;
  generator->generate_debug_info = 0;
  generator->generate_stack_trace_support = 0;
  generator->eliminate_unreachable_functions = 0;
  generator->has_error = 0;
  generator->error_message = NULL;
  generator->ir_program = NULL;
  generator->last_runtime_location_line = 0;
  generator->last_runtime_location_column = 0;
  generator->profile_runtime = 0;
  generator->profile_function_names = NULL;
  generator->profile_function_count = 0;
  generator->profile_function_capacity = 0;
  generator->binary_emitter =
      binary_emitter_create(binary_target_format_host_default());

  if (!generator->binary_emitter) {
    free(generator);
    return NULL;
  }

  return generator;
}

CodeGenerator *code_generator_create_with_debug(SymbolTable *symbol_table,
                                                TypeChecker *type_checker,
                                                RegisterAllocator *allocator,
                                                DebugInfo *debug_info) {
  CodeGenerator *generator =
      code_generator_create(symbol_table, type_checker, allocator);
  if (!generator) {
    return NULL;
  }

  generator->debug_info = debug_info;
  generator->generate_debug_info = 0;

  return generator;
}

void code_generator_destroy(CodeGenerator *generator) {
  if (!generator) {
    return;
  }

  free(generator->current_function_name);
  free(generator->error_message);
  binary_emitter_destroy(generator->binary_emitter);
  for (size_t i = 0; i < generator->profile_function_count; i++) {
    free(generator->profile_function_names[i]);
  }
  free(generator->profile_function_names);
  free(generator);
}

void code_generator_set_ir_program(CodeGenerator *generator,
                                   IRProgram *ir_program) {
  if (!generator) {
    return;
  }
  generator->ir_program = ir_program;
}

int code_generator_generate_program(CodeGenerator *generator, ASTNode *program) {
  return code_generator_generate_program_binary_object(generator, program);
}

void code_generator_set_error(CodeGenerator *generator, const char *format,
                              ...) {
  if (!generator || !format || generator->has_error) {
    return;
  }

  if (generator->current_function_name) {
    mettle_compiler_ctx_set_function_name(generator->current_function_name);
  }
  mettle_compiler_ctx_set_phase(METTLE_COMPILER_PHASE_CODEGEN);

  generator->has_error = 1;
  free(generator->error_message);
  generator->error_message = NULL;

  va_list args;
  va_start(args, format);

  va_list args_copy;
  va_copy(args_copy, args);
  int size = vsnprintf(NULL, 0, format, args_copy);
  va_end(args_copy);

  if (size > 0) {
    generator->error_message = malloc((size_t)size + 1);
    if (generator->error_message) {
      vsnprintf(generator->error_message, (size_t)size + 1, format, args);
    }
  }

  va_end(args);
}

void code_generator_set_stack_trace_support(CodeGenerator *generator,
                                            int enable) {
  if (generator) {
    generator->generate_stack_trace_support = enable ? 1 : 0;
  }
}

void code_generator_set_debug_sidecar_emission(CodeGenerator *generator,
                                               int enable) {
  if (generator) {
    generator->generate_debug_info = enable ? 1 : 0;
  }
}

void code_generator_set_eliminate_unreachable_functions(
    CodeGenerator *generator, int enable) {
  if (generator) {
    generator->eliminate_unreachable_functions = enable ? 1 : 0;
  }
}

void code_generator_set_profile_runtime(CodeGenerator *generator, int enable) {
  if (generator) {
    generator->profile_runtime = enable ? 1 : 0;
  }
}

void code_generator_set_debug_hooks(CodeGenerator *generator, int enable) {
  if (generator) {
    generator->debug_hooks = enable ? 1 : 0;
  }
}

int code_generator_register_profile_function(CodeGenerator *generator,
                                             const char *name,
                                             uint32_t *id_out) {
  if (!generator || !name || !id_out) {
    return 0;
  }

  size_t new_index = generator->profile_function_count;
  if (generator->profile_function_count >= generator->profile_function_capacity) {
    size_t new_capacity = generator->profile_function_capacity == 0
                              ? 16u
                              : generator->profile_function_capacity * 2u;
    char **names =
        realloc(generator->profile_function_names,
                new_capacity * sizeof(char *));
    if (!names) {
      return 0;
    }
    generator->profile_function_names = names;
    generator->profile_function_capacity = new_capacity;
  }

  char *name_copy = strdup(name);
  if (!name_copy) {
    return 0;
  }

  generator->profile_function_names[new_index] = name_copy;
  generator->profile_function_count = new_index + 1u;
  *id_out = (uint32_t)new_index;
  return 1;
}

BinaryEmitter *code_generator_get_binary_emitter(CodeGenerator *generator) {
  return generator ? generator->binary_emitter : NULL;
}

const char *code_generator_runtime_filename(CodeGenerator *generator,
                                            const char *node_filename) {
  if (node_filename && node_filename[0] != '\0') {
    return node_filename;
  }
  if (generator && generator->debug_info &&
      generator->debug_info->source_filename) {
    return generator->debug_info->source_filename;
  }
  return "";
}

void code_generator_record_runtime_trap_site(
    CodeGenerator *generator, const char *trap_pc_label, uint32_t kind,
    size_t line, size_t column, const char *filename,
    const char *message_template, const char *static_context) {
  if (!generator || !generator->debug_info ||
      !generator->generate_stack_trace_support || !trap_pc_label ||
      !generator->current_function_name || line == 0) {
    return;
  }

  const char *resolved_filename =
      code_generator_runtime_filename(generator, filename);
  char *source_line = debug_info_read_source_line(resolved_filename, line);
  debug_info_add_runtime_trap_site_mapping(
      generator->debug_info, trap_pc_label, kind,
      generator->current_function_name, resolved_filename, line, column,
      source_line, message_template, static_context);
  free(source_line);
}

void code_generator_add_runtime_function_mapping(CodeGenerator *generator,
                                                 const char *function_name,
                                                 const char *start_label,
                                                 const char *end_label,
                                                 size_t source_line,
                                                 size_t source_column,
                                                 const char *filename) {
  if (!generator || !generator->debug_info || !function_name || !start_label ||
      !end_label) {
    return;
  }

  debug_info_add_runtime_function_mapping(generator->debug_info, function_name,
                                          start_label, end_label, filename,
                                          source_line, source_column);
}

const char *code_generator_get_link_symbol_name(CodeGenerator *generator,
                                                const char *symbol_name) {
  if (!symbol_name || symbol_name[0] == '\0') {
    return NULL;
  }
  if (!generator || !generator->symbol_table) {
    return symbol_name;
  }

  Symbol *symbol = symbol_table_lookup(generator->symbol_table, symbol_name);
  if (symbol && symbol->is_extern && symbol->link_name &&
      symbol->link_name[0] != '\0') {
    return symbol->link_name;
  }
  return symbol_name;
}

char *code_generator_generate_label(CodeGenerator *generator,
                                    const char *prefix) {
  if (!generator || !prefix) {
    return NULL;
  }

  enum { LABEL_BUFFER_SIZE = 64 };
  char *label = malloc(LABEL_BUFFER_SIZE);
  if (label) {
    snprintf(label, LABEL_BUFFER_SIZE, "L%s%d", prefix,
             generator->current_label_id++);
  }
  return label;
}

Type *code_generator_infer_expression_type(CodeGenerator *generator,
                                           ASTNode *expression) {
  static Type int_type = {
      .kind = TYPE_INT32, .name = "int32", .size = 4, .alignment = 4};
  static Type float_type = {
      .kind = TYPE_FLOAT64, .name = "float64", .size = 8, .alignment = 8};
  static Type string_type = {
      .kind = TYPE_STRING, .name = "string", .size = 8, .alignment = 8};

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

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *num = (NumberLiteral *)expression->data;
    return (num && num->is_float) ? &float_type : &int_type;
  }
  case AST_STRING_LITERAL:
    return &string_type;
  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)expression->data;
    if (id && id->name && generator->symbol_table) {
      Symbol *symbol = symbol_table_lookup(generator->symbol_table, id->name);
      if (symbol && symbol->type) {
        return symbol->type;
      }
    }
    return &int_type;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    if (call && call->function_name && generator->symbol_table) {
      Symbol *func_symbol =
          symbol_table_lookup(generator->symbol_table, call->function_name);
      if (func_symbol && func_symbol->kind == SYMBOL_FUNCTION &&
          func_symbol->type) {
        return func_symbol->type;
      }
    }
    return &int_type;
  }
  case AST_BINARY_EXPRESSION: {
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
    return &int_type;
  }
}

int code_generator_type_is_aggregate(const Type *type) {
  if (!type) {
    return 0;
  }
  return type->kind == TYPE_STRUCT || type->kind == TYPE_ARRAY ||
         type->kind == TYPE_TAGGED_ENUM;
}

size_t code_generator_abi_type_size(const Type *type) {
  if (!type) {
    return 0;
  }
  return type->size > 0 ? type->size : 8;
}

AbiPassKind code_generator_abi_classify(const Type *type) {
  if (!type || !code_generator_type_is_aggregate(type)) {
    return ABI_PASS_DIRECT;
  }

  size_t size = code_generator_abi_type_size(type);
  if (size == 0 || size > 8) {
    return ABI_PASS_INDIRECT;
  }
  return (size == 1 || size == 2 || size == 4 || size == 8)
             ? ABI_PASS_DIRECT
             : ABI_PASS_INDIRECT;
}

int code_generator_is_floating_point_type(Type *type) {
  if (!type || !type->name) {
    return 0;
  }

  return strcmp(type->name, "float32") == 0 ||
         strcmp(type->name, "float64") == 0 ||
         strcmp(type->name, "double") == 0 ||
         strcmp(type->name, "float") == 0;
}
