#include "codegen/binary/internal.h"
#include "../../common.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

BinaryGlobalConstTable g_binary_global_consts = {0};
void binary_global_const_table_reset(void) {
  for (size_t i = 0; i < g_binary_global_consts.count; i++) {
    free(g_binary_global_consts.items[i].name);
  }
  free(g_binary_global_consts.items);
  free(g_binary_global_consts.slots);
  g_binary_global_consts.items = NULL;
  g_binary_global_consts.count = 0;
  g_binary_global_consts.capacity = 0;
  g_binary_global_consts.slots = NULL;
  g_binary_global_consts.slot_count = 0;
}

int binary_global_const_table_rebuild(size_t needed_count) {
  size_t slot_count = 16;
  size_t *slots = NULL;

  while (slot_count < needed_count * 2) {
    slot_count *= 2;
  }

  slots = calloc(slot_count, sizeof(size_t));
  if (!slots) {
    return 0;
  }

  for (size_t i = 0; i < g_binary_global_consts.count; i++) {
    BinaryGlobalConstEntry *entry = &g_binary_global_consts.items[i];
    if (!entry->name) {
      continue;
    }
    size_t slot = mettle_fnv1a_hash(entry->name) & (slot_count - 1);
    while (slots[slot] != 0) {
      slot = (slot + 1) & (slot_count - 1);
    }
    slots[slot] = i + 1;
  }

  free(g_binary_global_consts.slots);
  g_binary_global_consts.slots = slots;
  g_binary_global_consts.slot_count = slot_count;
  return 1;
}

static BinaryGlobalConstEntry *
binary_global_const_table_find_entry(const char *name) {
  if (!name) {
    return NULL;
  }

  if (g_binary_global_consts.slots && g_binary_global_consts.slot_count > 0) {
    size_t mask = g_binary_global_consts.slot_count - 1;
    size_t slot = mettle_fnv1a_hash(name) & mask;
    while (g_binary_global_consts.slots[slot] != 0) {
      size_t index = g_binary_global_consts.slots[slot] - 1;
      BinaryGlobalConstEntry *entry = &g_binary_global_consts.items[index];
      if (entry->name && strcmp(entry->name, name) == 0) {
        return entry;
      }
      slot = (slot + 1) & mask;
    }
    return NULL;
  }

  for (size_t i = 0; i < g_binary_global_consts.count; i++) {
    BinaryGlobalConstEntry *entry = &g_binary_global_consts.items[i];
    if (entry->name && strcmp(entry->name, name) == 0) {
      return entry;
    }
  }
  return NULL;
}

uint64_t binary_global_const_bits(long long int_value, double float_value,
                                         int is_float) {
  uint64_t bits = 0;
  if (is_float) {
    memcpy(&bits, &float_value, sizeof(bits));
  } else {
    bits = (uint64_t)int_value;
  }
  return bits;
}

int binary_global_const_table_add(const char *name, long long int_value,
                                         double float_value, int is_float,
                                         int can_inline_load) {
  if (!name) {
    return 0;
  }

  if (!g_binary_global_consts.slots ||
      ((g_binary_global_consts.count + 1) * 10 >=
       g_binary_global_consts.slot_count * 7)) {
    if (!binary_global_const_table_rebuild(g_binary_global_consts.count + 1)) {
      return 0;
    }
  }

  BinaryGlobalConstEntry *existing = binary_global_const_table_find_entry(name);
  if (existing) {
    existing->int_value = int_value;
    existing->float_value = float_value;
    existing->is_float = is_float ? 1 : 0;
    existing->bits =
        binary_global_const_bits(int_value, float_value, existing->is_float);
    existing->can_inline_load =
        (existing->is_float ? 0 : (can_inline_load ? 1 : 0));
    return 1;
  }

  if (g_binary_global_consts.count >= g_binary_global_consts.capacity) {
    size_t new_capacity =
        g_binary_global_consts.capacity ? g_binary_global_consts.capacity * 2 : 16;
    BinaryGlobalConstEntry *new_items =
        realloc(g_binary_global_consts.items,
                new_capacity * sizeof(BinaryGlobalConstEntry));
    if (!new_items) {
      return 0;
    }
    g_binary_global_consts.items = new_items;
    g_binary_global_consts.capacity = new_capacity;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return 0;
  }

  /* Float globals are never inline-loaded as a GP immediate: that path emits
   * `mov GP_reg, imm64`, leaving the bits in a general-purpose register, but a
   * float consumer reads its value from an XMM register. Force the RIP-relative
   * load path (which is XMM-aware) so the value reaches an XMM lane. */
  if (is_float) {
    can_inline_load = 0;
  }

  size_t index = g_binary_global_consts.count;
  g_binary_global_consts.items[index].name = name_copy;
  g_binary_global_consts.items[index].int_value = int_value;
  g_binary_global_consts.items[index].float_value = float_value;
  g_binary_global_consts.items[index].is_float = is_float ? 1 : 0;
  g_binary_global_consts.items[index].bits =
      binary_global_const_bits(int_value, float_value, is_float);
  g_binary_global_consts.items[index].can_inline_load =
      can_inline_load ? 1 : 0;
  g_binary_global_consts.count++;

  size_t slot = mettle_fnv1a_hash(name_copy) & (g_binary_global_consts.slot_count - 1);
  while (g_binary_global_consts.slots[slot] != 0) {
    slot = (slot + 1) & (g_binary_global_consts.slot_count - 1);
  }
  g_binary_global_consts.slots[slot] = index + 1;
  return 1;
}

int binary_global_const_table_get(const char *name, uint64_t *value_out) {
  if (!name || !value_out) {
    return 0;
  }

  BinaryGlobalConstEntry *entry = binary_global_const_table_find_entry(name);
  if (entry && entry->can_inline_load) {
    *value_out = entry->bits;
    return 1;
  }

  return 0;
}

int binary_global_const_table_get_numeric(
    const char *name, BinaryNumericConstant *value_out) {
  if (!name || !value_out) {
    return 0;
  }

  BinaryGlobalConstEntry *entry = binary_global_const_table_find_entry(name);
  if (!entry) {
    return 0;
  }

  value_out->int_value = entry->int_value;
  value_out->float_value = entry->float_value;
  value_out->is_float = entry->is_float;
  return 1;
}

BinaryIRFunctionIndex g_binary_ir_function_index = {0};
void binary_ir_function_index_reset(void) {
  free(g_binary_ir_function_index.slots);
  g_binary_ir_function_index.slots = NULL;
  g_binary_ir_function_index.slot_count = 0;
  g_binary_ir_function_index.program = NULL;
  g_binary_ir_function_index.function_count = 0;
}

void binary_ir_function_index_insert(BinaryIRFunctionIndex *index,
                                            IRFunction *function) {
  size_t mask = index->slot_count - 1;
  size_t i = mettle_fnv1a_hash(function->name) & mask;
  while (index->slots[i].name) {
    /* First definition of a given name wins, matching the old linear scan
     * which returned the earliest matching function. */
    if (strcmp(index->slots[i].name, function->name) == 0) {
      return;
    }
    i = (i + 1) & mask;
  }
  index->slots[i].name = function->name;
  index->slots[i].function = function;
}

/* Returns 1 on success (index ready to query), 0 on allocation failure (caller
 * should fall back to a linear scan rather than miss real functions). */
int binary_ir_function_index_ensure(const IRProgram *program) {
  if (g_binary_ir_function_index.program == program &&
      g_binary_ir_function_index.function_count == program->function_count &&
      g_binary_ir_function_index.slots) {
    return 1;
  }

  binary_ir_function_index_reset();

  /* Size to >=2x function count, power of two, min 16, to keep load factor
   * under 0.5 and probe chains short. */
  size_t slot_count = 16;
  while (slot_count < program->function_count * 2) {
    slot_count *= 2;
  }

  BinaryIRFunctionSlot *slots =
      calloc(slot_count, sizeof(BinaryIRFunctionSlot));
  if (!slots) {
    return 0;
  }

  g_binary_ir_function_index.slots = slots;
  g_binary_ir_function_index.slot_count = slot_count;
  g_binary_ir_function_index.program = program;
  g_binary_ir_function_index.function_count = program->function_count;

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name) {
      binary_ir_function_index_insert(&g_binary_ir_function_index, function);
    }
  }

  return 1;
}

IRFunction *code_generator_find_ir_function_binary(CodeGenerator *generator,
                                                          const char *name) {
  if (!generator || !generator->ir_program || !name) {
    return NULL;
  }

  const IRProgram *program = generator->ir_program;

  if (binary_ir_function_index_ensure(program)) {
    const BinaryIRFunctionIndex *index = &g_binary_ir_function_index;
    size_t mask = index->slot_count - 1;
    size_t i = mettle_fnv1a_hash(name) & mask;
    while (index->slots[i].name) {
      if (strcmp(index->slots[i].name, name) == 0) {
        return index->slots[i].function;
      }
      i = (i + 1) & mask;
    }
    return NULL;
  }

  /* Fallback: index allocation failed; behave as before. */
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function;
    }
  }

  return NULL;
}
int code_generator_binary_numeric_constant_is_float(
    const BinaryNumericConstant *value, ASTNode *expression) {
  if (value && value->is_float) {
    return 1;
  }
  return expression && expression->resolved_type &&
         code_generator_binary_resolved_type_float_bits(
             expression->resolved_type) != 0;
}

void code_generator_binary_numeric_constant_from_double(
    BinaryNumericConstant *out, double value) {
  out->is_float = 1;
  out->float_value = value;
  out->int_value = (long long)value;
}

void code_generator_binary_numeric_constant_from_int(
    BinaryNumericConstant *out, long long value) {
  out->is_float = 0;
  out->int_value = value;
  out->float_value = (double)value;
}

int code_generator_binary_eval_numeric_global_initializer(
    ASTNode *expression, BinaryNumericConstant *out_value) {
  if (!expression || !out_value) {
    return 0;
  }

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal) {
      return 0;
    }
    if (literal->is_float) {
      code_generator_binary_numeric_constant_from_double(out_value,
                                                         literal->float_value);
    } else {
      code_generator_binary_numeric_constant_from_int(out_value,
                                                      literal->int_value);
    }
    return 1;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    return identifier && identifier->name &&
           binary_global_const_table_get_numeric(identifier->name, out_value);
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    BinaryNumericConstant operand = {0};
    if (!unary || !unary->operator || !unary->operand ||
        !code_generator_binary_eval_numeric_global_initializer(unary->operand,
                                                               &operand)) {
      return 0;
    }

    if (strcmp(unary->operator, "+") == 0) {
      *out_value = operand;
      return 1;
    }
    if (strcmp(unary->operator, "-") == 0) {
      if (code_generator_binary_numeric_constant_is_float(&operand,
                                                          expression)) {
        code_generator_binary_numeric_constant_from_double(
            out_value, -(operand.is_float ? operand.float_value
                                          : (double)operand.int_value));
      } else {
        code_generator_binary_numeric_constant_from_int(out_value,
                                                        -operand.int_value);
      }
      return 1;
    }
    if (strcmp(unary->operator, "!") == 0) {
      int is_zero =
          operand.is_float ? (operand.float_value == 0.0)
                           : (operand.int_value == 0);
      code_generator_binary_numeric_constant_from_int(out_value, is_zero);
      return 1;
    }
    if (strcmp(unary->operator, "~") == 0 && !operand.is_float) {
      code_generator_binary_numeric_constant_from_int(out_value,
                                                      ~operand.int_value);
      return 1;
    }
    return 0;
  }

  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    BinaryNumericConstant left = {0};
    BinaryNumericConstant right = {0};
    int result_is_float = 0;
    if (!binary || !binary->operator || !binary->left || !binary->right ||
        !code_generator_binary_eval_numeric_global_initializer(binary->left,
                                                               &left) ||
        !code_generator_binary_eval_numeric_global_initializer(binary->right,
                                                               &right)) {
      return 0;
    }

    result_is_float =
        left.is_float || right.is_float ||
        code_generator_binary_numeric_constant_is_float(NULL, expression);
    if (result_is_float) {
      double lhs = left.is_float ? left.float_value : (double)left.int_value;
      double rhs = right.is_float ? right.float_value : (double)right.int_value;
      if (strcmp(binary->operator, "+") == 0) {
        code_generator_binary_numeric_constant_from_double(out_value, lhs + rhs);
      } else if (strcmp(binary->operator, "-") == 0) {
        code_generator_binary_numeric_constant_from_double(out_value, lhs - rhs);
      } else if (strcmp(binary->operator, "*") == 0) {
        code_generator_binary_numeric_constant_from_double(out_value, lhs * rhs);
      } else if (strcmp(binary->operator, "/") == 0) {
        code_generator_binary_numeric_constant_from_double(out_value, lhs / rhs);
      } else if (strcmp(binary->operator, "==") == 0) {
        code_generator_binary_numeric_constant_from_int(out_value, lhs == rhs);
      } else if (strcmp(binary->operator, "!=") == 0) {
        code_generator_binary_numeric_constant_from_int(out_value, lhs != rhs);
      } else if (strcmp(binary->operator, "<") == 0) {
        code_generator_binary_numeric_constant_from_int(out_value, lhs < rhs);
      } else if (strcmp(binary->operator, "<=") == 0) {
        code_generator_binary_numeric_constant_from_int(out_value, lhs <= rhs);
      } else if (strcmp(binary->operator, ">") == 0) {
        code_generator_binary_numeric_constant_from_int(out_value, lhs > rhs);
      } else if (strcmp(binary->operator, ">=") == 0) {
        code_generator_binary_numeric_constant_from_int(out_value, lhs >= rhs);
      } else {
        return 0;
      }
      return 1;
    }

    if (strcmp(binary->operator, "+") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value + right.int_value);
    } else if (strcmp(binary->operator, "-") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value - right.int_value);
    } else if (strcmp(binary->operator, "*") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value * right.int_value);
    } else if (strcmp(binary->operator, "/") == 0) {
      if (right.int_value == 0) {
        return 0;
      }
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value / right.int_value);
    } else if (strcmp(binary->operator, "%") == 0) {
      if (right.int_value == 0) {
        return 0;
      }
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value % right.int_value);
    } else if (strcmp(binary->operator, "==") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value == right.int_value);
    } else if (strcmp(binary->operator, "!=") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value != right.int_value);
    } else if (strcmp(binary->operator, "<") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value < right.int_value);
    } else if (strcmp(binary->operator, "<=") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value <= right.int_value);
    } else if (strcmp(binary->operator, ">") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value > right.int_value);
    } else if (strcmp(binary->operator, ">=") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value >= right.int_value);
    } else if (strcmp(binary->operator, "&&") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value != 0 && right.int_value != 0);
    } else if (strcmp(binary->operator, "||") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value != 0 || right.int_value != 0);
    } else if (strcmp(binary->operator, "&") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value & right.int_value);
    } else if (strcmp(binary->operator, "|") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value | right.int_value);
    } else if (strcmp(binary->operator, "^") == 0) {
      code_generator_binary_numeric_constant_from_int(
          out_value, left.int_value ^ right.int_value);
    } else {
      return 0;
    }
    return 1;
  }

  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    BinaryNumericConstant operand = {0};
    int target_float_bits = 0;
    if (!cast || !cast->operand ||
        !code_generator_binary_eval_numeric_global_initializer(cast->operand,
                                                               &operand)) {
      return 0;
    }
    target_float_bits = expression->resolved_type
                            ? code_generator_binary_resolved_type_float_bits(
                                  expression->resolved_type)
                            : 0;
    if (target_float_bits != 0) {
      code_generator_binary_numeric_constant_from_double(
          out_value, operand.is_float ? operand.float_value
                                      : (double)operand.int_value);
    } else {
      code_generator_binary_numeric_constant_from_int(
          out_value, operand.is_float ? (long long)operand.float_value
                                      : operand.int_value);
    }
    return 1;
  }

  default:
    return 0;
  }
}

int code_generator_emit_binary_global_variable(CodeGenerator *generator,
                                                      VarDeclaration *var_data) {
  BinaryEmitter *emitter = NULL;
  Symbol *symbol = NULL;
  Type *type = NULL;
  const char *link_name = NULL;
  const char *section_name = NULL;
  BinarySectionKind section_kind = BINARY_SECTION_DATA;
  size_t section_index = 0;
  size_t value_offset = 0;
  size_t alignment = 1;
  int size = 0;
  unsigned char bytes[8] = {0};

  if (!generator || !var_data || !var_data->name) {
    return 0;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }

  symbol = generator->symbol_table
               ? symbol_table_lookup(generator->symbol_table, var_data->name)
               : NULL;
  type = symbol ? symbol->type
                : code_generator_binary_get_resolved_type(generator,
                                                          var_data->type_name,
                                                          0);
  if (!type) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports scalar integer/pointer/string/"
        "float64 global variables (encountered '%s')",
        var_data->name);
    return 0;
  }

  link_name = code_generator_get_link_symbol_name(generator, var_data->name);
  if (!link_name || link_name[0] == '\0') {
    code_generator_set_error(generator, "Invalid global symbol '%s'",
                             var_data->name);
    return 0;
  }

  if (type->kind == TYPE_STRING) {
    const char *initializer_value = NULL;
    StringLiteral *literal = NULL;

    if (var_data->initializer) {
      if (var_data->initializer->type != AST_STRING_LITERAL) {
        code_generator_set_error(
            generator,
            "Direct object backend only supports string-literal global "
            "initializers for string globals (encountered '%s')",
            var_data->name);
        return 0;
      }

      literal = (StringLiteral *)var_data->initializer->data;
      if (!literal) {
        code_generator_set_error(generator,
                                 "Malformed string global initializer '%s'",
                                 var_data->name);
        return 0;
      }
      initializer_value = literal->value ? literal->value : "";
    }

    return code_generator_binary_emit_global_string_variable(generator,
                                                             link_name,
                                                             initializer_value);
  }

  if (!code_generator_binary_resolved_type_is_supported(type, 0)) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports scalar integer/pointer/string/"
        "float64 global variables (encountered '%s')",
        var_data->name);
    return 0;
  }

  size = code_generator_binary_resolved_type_scalar_size(type);
  if (size <= 0 || size > 8) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports global variables up to 8 bytes "
        "(encountered '%s')",
        var_data->name);
    return 0;
  }

  section_kind =
      var_data->initializer ? BINARY_SECTION_DATA : BINARY_SECTION_BSS;
  section_name = section_kind == BINARY_SECTION_DATA ? ".data" : ".bss";
  alignment = type->alignment ? type->alignment : (size_t)size;
  if (alignment == 0) {
    alignment = 1;
  }

  section_index = binary_emitter_get_or_create_section(
      emitter, section_name, section_kind, 0, alignment);
  if (section_index == (size_t)-1) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to create global variable section");
    return 0;
  }

  if (!binary_emitter_align_section(emitter, section_index, alignment, 0)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to align global variable section");
    return 0;
  }

  if (var_data->initializer) {
    BinaryNumericConstant constant = {0};
    int float_bits = code_generator_binary_resolved_type_float_bits(type);
    if (!code_generator_binary_eval_numeric_global_initializer(
            var_data->initializer, &constant)) {
      code_generator_set_error(
          generator,
          "Direct object backend only supports constant numeric global "
          "initializers "
          "(encountered '%s')",
          var_data->name);
      return 0;
    }

    if (float_bits == 64) {
      double value = constant.is_float ? constant.float_value
                                       : (double)constant.int_value;
      memcpy(bytes, &value, sizeof(value));
    } else if (float_bits == 32) {
      float value = (float)(constant.is_float ? constant.float_value
                                              : (double)constant.int_value);
      memcpy(bytes, &value, sizeof(value));
    } else {
      uint64_t encoded = (uint64_t)constant.int_value;
      if (constant.is_float) {
        code_generator_set_error(
            generator,
            "Direct object backend does not support floating global "
            "initializers for non-float globals (encountered '%s')",
            var_data->name);
        return 0;
      }
      memcpy(bytes, &encoded, (size_t)size);
    }

    if (!binary_emitter_append_bytes(emitter, section_index, bytes, (size_t)size,
                                     &value_offset)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit global initializer");
      return 0;
    }
  } else if (!binary_emitter_append_zeros(emitter, section_index, (size_t)size,
                                          &value_offset)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to reserve global storage");
    return 0;
  }

  if (!binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                    section_index, value_offset, (size_t)size)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to define global variable symbol");
    return 0;
  }

  return 1;
}

int code_generator_binary_global_is_written(IRProgram *ir_program,
                                                   const char *name) {
  if (!ir_program || !name) {
    return 1;
  }

  for (size_t fn_i = 0; fn_i < ir_program->function_count; fn_i++) {
    IRFunction *function = ir_program->functions[fn_i];
    if (!function) {
      continue;
    }

    for (size_t insn_i = 0; insn_i < function->instruction_count; insn_i++) {
      IRInstruction *instruction = &function->instructions[insn_i];
      if (!instruction) {
        continue;
      }

      if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
          instruction->dest.name && strcmp(instruction->dest.name, name) == 0) {
        return 1;
      }
      if (instruction->op == IR_OP_ADDRESS_OF &&
          instruction->lhs.kind == IR_OPERAND_SYMBOL &&
          instruction->lhs.name && strcmp(instruction->lhs.name, name) == 0) {
        return 1;
      }
      if (instruction->op == IR_OP_STORE &&
          instruction->dest.kind == IR_OPERAND_SYMBOL &&
          instruction->dest.name && strcmp(instruction->dest.name, name) == 0) {
        return 1;
      }
    }
  }

  return 0;
}

int code_generator_binary_collect_global_constants(
    CodeGenerator *generator, Program *program_data) {
  if (!generator || !program_data) {
    return 0;
  }

  for (size_t i = 0; i < program_data->declaration_count; i++) {
    ASTNode *declaration = program_data->declarations[i];
    if (!declaration || declaration->type != AST_VAR_DECLARATION) {
      continue;
    }

    VarDeclaration *var_data = (VarDeclaration *)declaration->data;
    if (!var_data || !var_data->name || var_data->is_extern ||
        !var_data->initializer) {
      continue;
    }

    Type *type = code_generator_binary_get_resolved_type(generator,
                                                         var_data->type_name, 0);
    if (!type || !code_generator_binary_resolved_type_is_supported(type, 0) ||
        type->kind == TYPE_STRING || type->kind == TYPE_VOID ||
        type->size == 0 || type->size > 8) {
      continue;
    }

    BinaryNumericConstant constant = {0};
    if (!code_generator_binary_eval_numeric_global_initializer(
            var_data->initializer, &constant)) {
      continue;
    }

    if (!binary_global_const_table_add(
            var_data->name, constant.int_value, constant.float_value,
            constant.is_float,
            !code_generator_binary_global_is_written(generator->ir_program,
                                                     var_data->name))) {
      code_generator_set_error(
          generator, "Out of memory while tracking constant global '%s'",
          var_data->name);
      return 0;
    }
  }

  return 1;
}

int code_generator_declare_binary_externs(CodeGenerator *generator,
                                          Program *program_data) {
  BinaryEmitter *emitter = NULL;

  if (!generator || !program_data) {
    return 0;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }

  for (size_t i = 0; i < program_data->declaration_count; i++) {
    ASTNode *declaration = program_data->declarations[i];
    const char *extern_name = NULL;

    if (!declaration) {
      continue;
    }

    if (declaration->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *function_data =
          (FunctionDeclaration *)declaration->data;
      if (!function_data || !function_data->is_extern || !function_data->name) {
        continue;
      }
      extern_name =
          code_generator_get_link_symbol_name(generator, function_data->name);
    } else if (declaration->type == AST_VAR_DECLARATION) {
      VarDeclaration *var_data = (VarDeclaration *)declaration->data;
      if (!var_data || !var_data->is_extern || !var_data->name) {
        continue;
      }
      extern_name =
          code_generator_get_link_symbol_name(generator, var_data->name);
    } else {
      continue;
    }

    if (code_generator_binary_active_abi()->shadow_space_size > 0 &&
        (strcmp(extern_name, "malloc") == 0 ||
         strcmp(extern_name, "calloc") == 0 ||
         strcmp(extern_name, "realloc") == 0 ||
         strcmp(extern_name, "free") == 0 ||
         strcmp(extern_name, "memset") == 0 ||
         strcmp(extern_name, "memcpy") == 0 ||
         strcmp(extern_name, "memmove") == 0 ||
         strcmp(extern_name, "memcmp") == 0)) {
      continue;
    }

    if (!binary_emitter_declare_external(emitter, extern_name)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to declare external symbol");
      return 0;
    }
  }

  return 1;
}

