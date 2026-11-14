#include "ir_optimize_internal.h"
#include "../../common.h"

static int ir_builtin_integer_type_info(const char *name, int *size_out,
                                        int *is_unsigned_out) {
  int size = 0;
  int is_unsigned = 0;

  if (!name) {
    return 0;
  }

  if (strcmp(name, "int8") == 0) {
    size = 1;
  } else if (strcmp(name, "uint8") == 0) {
    size = 1;
    is_unsigned = 1;
  } else if (strcmp(name, "int16") == 0) {
    size = 2;
  } else if (strcmp(name, "uint16") == 0) {
    size = 2;
    is_unsigned = 1;
  } else if (strcmp(name, "int32") == 0) {
    size = 4;
  } else if (strcmp(name, "uint32") == 0) {
    size = 4;
    is_unsigned = 1;
  } else if (strcmp(name, "int64") == 0) {
    size = 8;
  } else if (strcmp(name, "uint64") == 0) {
    size = 8;
    is_unsigned = 1;
  } else {
    return 0;
  }

  if (size_out) {
    *size_out = size;
  }
  if (is_unsigned_out) {
    *is_unsigned_out = is_unsigned;
  }
  return 1;
}

static int ir_find_temp_producer_index_in_current_block(
    const IRFunction *function, size_t before_index, const char *temp_name,
    size_t *producer_index_out) {
  if (!function || !temp_name || !producer_index_out ||
      before_index > function->instruction_count) {
    return 0;
  }

  for (size_t i = before_index; i > 0;) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (instruction->op == IR_OP_LABEL) {
      return 0;
    }
    if (ir_instruction_writes_temp(instruction) && instruction->dest.name &&
        strcmp(instruction->dest.name, temp_name) == 0) {
      *producer_index_out = i;
      return 1;
    }
  }

  return 0;
}

static int ir_try_compose_single_use_cast(IRFunction *function,
                                          const IRTempUseMap *uses,
                                          size_t cast_index, int *changed) {
  IRInstruction *cast = NULL;
  IRInstruction *producer = NULL;
  size_t producer_index = 0;
  int producer_size = 0;
  int producer_unsigned = 0;
  int cast_size = 0;
  int cast_unsigned = 0;
  const char *composed_type = NULL;
  char *type_copy = NULL;
  IROperand source = ir_operand_none();

  if (!function || !uses || cast_index >= function->instruction_count) {
    return 0;
  }

  cast = &function->instructions[cast_index];
  if (cast->op != IR_OP_CAST || cast->is_float || !cast->text ||
      cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name ||
      ir_temp_use_map_get(uses, cast->lhs.name) != 1 ||
      !ir_builtin_integer_type_info(cast->text, &cast_size, &cast_unsigned) ||
      !ir_find_temp_producer_index_in_current_block(function, cast_index,
                                                    cast->lhs.name,
                                                    &producer_index)) {
    return 1;
  }

  producer = &function->instructions[producer_index];
  if (producer->op != IR_OP_CAST || producer->is_float || !producer->text ||
      !ir_builtin_integer_type_info(producer->text, &producer_size,
                                    &producer_unsigned)) {
    return 1;
  }

  if (cast_size > producer_size && producer_size == 4 && !producer_unsigned) {
    return 1;
  }
  if (cast_size > producer_size && cast_unsigned != producer_unsigned) {
    return 1;
  }

  composed_type = (cast_size <= producer_size) ? cast->text : producer->text;
  type_copy = mettle_strdup(composed_type);
  if (!type_copy || !ir_operand_clone(&producer->lhs, &source)) {
    free(type_copy);
    ir_operand_destroy(&source);
    return 0;
  }

  ir_operand_destroy(&cast->lhs);
  cast->lhs = source;
  mettle_free_string(cast->text);
  cast->text = type_copy;
  ir_instruction_make_nop(producer);

  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_try_coalesce_unsigned_load_cast(IRFunction *function,
                                              const IRTempUseMap *uses,
                                              size_t cast_index,
                                              int *changed) {
  IRInstruction *cast = NULL;
  IRInstruction *producer = NULL;
  size_t producer_index = 0;
  int cast_size = 0;
  int is_unsigned = 0;
  IROperand rewritten_dest = ir_operand_none();

  if (!function || !uses || cast_index >= function->instruction_count) {
    return 0;
  }

  cast = &function->instructions[cast_index];
  if (cast->op != IR_OP_CAST || cast->is_float || !cast->text ||
      cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name ||
      ir_temp_use_map_get(uses, cast->lhs.name) != 1 ||
      !ir_builtin_integer_type_info(cast->text, &cast_size, &is_unsigned) ||
      !is_unsigned || cast_size > 2 ||
      !ir_find_temp_producer_index_in_current_block(function, cast_index,
                                                    cast->lhs.name,
                                                    &producer_index)) {
    return 1;
  }

  producer = &function->instructions[producer_index];
  if (producer->op != IR_OP_LOAD || producer->is_float ||
      producer->rhs.kind != IR_OPERAND_INT ||
      producer->rhs.int_value != cast_size) {
    return 1;
  }

  if (!ir_operand_clone(&cast->dest, &rewritten_dest)) {
    return 0;
  }

  ir_operand_destroy(&producer->dest);
  producer->dest = rewritten_dest;
  producer->is_unsigned = 1;
  if (cast->value_type) {
    producer->value_type = cast->value_type;
  }
  ir_instruction_make_nop(cast);

  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_symbol_is_function_local(const IRFunction *function,
                                       const char *name) {
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names[i] &&
        strcmp(function->parameter_names[i], name) == 0) {
      return 1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_DECLARE_LOCAL &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
        strcmp(instruction->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_call_result_is_aggregate(const IRInstruction *producer) {
  if ((producer->op != IR_OP_CALL && producer->op != IR_OP_CALL_INDIRECT) ||
      !producer->value_type) {
    return 0;
  }
  switch (producer->value_type->kind) {
  case MTLC_TYPE_STRING:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_STRUCT:
  case MTLC_TYPE_TAGGED_ENUM:
    return 1;
  default:
    return 0;
  }
}

int ir_coalesce_single_use_temp_assign_pass(IRFunction *function,
                                                   int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap uses;
  if (!ir_temp_use_map_init(&uses)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 1; i < function->instruction_count; i++) {
    IRInstruction *assign_instruction = &function->instructions[i];
    if (assign_instruction->op != IR_OP_ASSIGN ||
        assign_instruction->lhs.kind != IR_OPERAND_TEMP ||
        !assign_instruction->lhs.name ||
        assign_instruction->dest.kind == IR_OPERAND_NONE) {
      continue;
    }

    if (ir_temp_use_map_get(&uses, assign_instruction->lhs.name) != 1) {
      continue;
    }

    size_t producer_index = i;
    IRInstruction *producer = NULL;
    while (producer_index > 0) {
      producer_index--;
      if (function->instructions[producer_index].op == IR_OP_NOP) {
        continue;
      }
      producer = &function->instructions[producer_index];
      break;
    }
    if (!producer || !ir_instruction_writes_destination(producer) ||
        producer->dest.kind != IR_OPERAND_TEMP || !producer->dest.name ||
        !ir_operand_names_match(&producer->dest, &assign_instruction->lhs)) {
      continue;
    }

    if (ir_call_result_is_aggregate(producer) &&
        assign_instruction->dest.kind == IR_OPERAND_SYMBOL &&
        assign_instruction->dest.name &&
        !ir_symbol_is_function_local(function, assign_instruction->dest.name)) {
      continue;
    }

    if (assign_instruction->is_float) {
      int assign_bits = (assign_instruction->float_bits == 32) ? 32 : 64;
      int producer_bits =
          producer->is_float ? ((producer->float_bits == 32) ? 32 : 64) : 0;
      if (!producer->is_float || producer_bits != assign_bits) {
        continue;
      }
    }

    IROperand rewritten_dest = ir_operand_none();
    if (!ir_operand_clone(&assign_instruction->dest, &rewritten_dest)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }

    ir_operand_destroy(&producer->dest);
    producer->dest = rewritten_dest;
    ir_instruction_make_nop(assign_instruction);
    if (changed) {
      *changed = 1;
    }
  }

  for (size_t i = 1; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_CAST || instruction->is_float ||
        instruction->lhs.kind != IR_OPERAND_TEMP ||
        !instruction->lhs.name) {
      continue;
    }

    if (!ir_try_compose_single_use_cast(function, &uses, i, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 1; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_CAST || instruction->is_float ||
        instruction->lhs.kind != IR_OPERAND_TEMP ||
        !instruction->lhs.name) {
      continue;
    }

    if (!ir_try_coalesce_unsigned_load_cast(function, &uses, i, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
}

static void ir_symbol_value_map_invalidate_name(IRSymbolValueMap *map,
                                              const char *symbol_name) {
  if (!map || !symbol_name) {
    return;
  }

  ir_temp_value_map_remove(map, symbol_name);

  if (!ir_temp_value_map_any_value_symbol(map, symbol_name)) {
    return;
  }

  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    IRTempValueEntry *entry = &map->items[read];
    int remove = 0;
    if (entry->value.kind == IR_OPERAND_SYMBOL && entry->value.name &&
        strcmp(entry->value.name, symbol_name) == 0) {
      remove = 1;
    }
    if (entry->value.kind == IR_OPERAND_TEMP && entry->value.name) {
    }

    if (remove) {
      ir_temp_value_map_note_value_removed(map, &entry->value);
      mettle_free_string(entry->name);
      ir_operand_destroy(&entry->value);
      continue;
    }

    if (write != read) {
      map->items[write] = map->items[read];
    }
    write++;
  }
  if (map->count != write) {
    map->count = write;
    ir_temp_value_map_reindex(map);
  }
}

typedef struct {
  const IRFunction *function;
  int kept_bits;
} IRCpConsumer;

static int ir_declared_integer_width(const char *type, int *bits_out,
                                     int *unsigned_out);

static int ir_cp_value_fits_home(const IRFunction *function, const char *type,
                                 int bits, int is_unsigned,
                                 const IROperand *value) {
  if (value->kind == IR_OPERAND_INT) {
    int shift = 64 - bits;
    long long wrapped =
        is_unsigned
            ? (long long)(((unsigned long long)value->int_value << shift) >>
                          shift)
            : (value->int_value << shift) >> shift;
    return wrapped == value->int_value;
  }
  if (value->kind == IR_OPERAND_SYMBOL && value->name) {
    const char *source = ir_function_local_declared_type(function, value->name);
    return source && strcmp(source, type) == 0;
  }
  return 0;
}

static const IROperand *ir_cp_symbol_value(const IRSymbolValueMap *symbol_map,
                                           const IRCpConsumer *consumer,
                                           const char *name) {
  const IROperand *mapped = ir_temp_value_map_lookup(symbol_map, name);
  const char *type;
  int bits = 0;
  int is_unsigned = 0;
  if (!mapped || !consumer || !consumer->function) {
    return mapped;
  }
  type = ir_function_local_declared_type(consumer->function, name);
  if (!ir_declared_integer_width(type, &bits, &is_unsigned) || bits == 0) {
    return mapped;
  }
  if (consumer->kept_bits > 0 && consumer->kept_bits <= bits) {
    return mapped;
  }
  return ir_cp_value_fits_home(consumer->function, type, bits, is_unsigned,
                               mapped)
             ? mapped
             : NULL;
}

static int ir_resolve_propagated_value(const IRTempValueMap *temp_map,
                                       const IRSymbolValueMap *symbol_map,
                                       const IRCpConsumer *consumer,
                                       const IROperand *operand, IROperand *out,
                                       int depth) {
  if (!operand || !out) {
    return 0;
  }

  if (depth > 64) {
    return ir_operand_clone(operand, out);
  }

  if (operand->kind == IR_OPERAND_TEMP && operand->name && temp_map) {
    const IROperand *mapped =
        ir_temp_value_map_lookup(temp_map, operand->name);
    if (mapped) {
      return ir_resolve_propagated_value(temp_map, symbol_map, consumer, mapped,
                                         out, depth + 1);
    }
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && symbol_map) {
    const IROperand *mapped =
        ir_cp_symbol_value(symbol_map, consumer, operand->name);
    if (mapped) {
      return ir_resolve_propagated_value(temp_map, symbol_map, consumer, mapped,
                                         out, depth + 1);
    }
  }

  return ir_operand_clone(operand, out);
}

static int ir_try_propagate_operand(IRTempValueMap *temp_map,
                                    IRSymbolValueMap *symbol_map,
                                    const IRCpConsumer *consumer,
                                    IROperand *operand, int *changed) {
  if (!operand) {
    return 1;
  }

  if (operand->kind == IR_OPERAND_TEMP && operand->name && temp_map) {
    const IROperand *mapped =
        ir_temp_value_map_lookup(temp_map, operand->name);
    if (!mapped) {
      return 1;
    }

    IROperand resolved = ir_operand_none();
    if (!ir_resolve_propagated_value(temp_map, symbol_map, consumer, mapped,
                                     &resolved, 0)) {
      return 0;
    }

    ir_operand_destroy(operand);
    *operand = resolved;
    if (changed) {
      *changed = 1;
    }
    return 1;
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && symbol_map) {
    const IROperand *mapped =
        ir_cp_symbol_value(symbol_map, consumer, operand->name);
    if (!mapped) {
      return 1;
    }

    IROperand resolved = ir_operand_none();
    if (!ir_resolve_propagated_value(temp_map, symbol_map, consumer, mapped,
                                     &resolved, 0)) {
      return 0;
    }

    ir_operand_destroy(operand);
    *operand = resolved;
    if (changed) {
      *changed = 1;
    }
    return 1;
  }

  return 1;
}

static int ir_cp_assign_kept_bits(const IRFunction *function,
                                  const IRInstruction *instruction) {
  int bits = 0;
  int is_unsigned = 0;
  if (instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
      instruction->is_float) {
    return 0;
  }
  if (!ir_declared_integer_width(
          ir_function_local_declared_type(function, instruction->dest.name),
          &bits, &is_unsigned)) {
    return 0;
  }
  return bits == 0 ? 64 : bits;
}

static int ir_cp_store_kept_bits(const IRInstruction *instruction) {
  if (instruction->op != IR_OP_STORE ||
      instruction->rhs.kind != IR_OPERAND_INT ||
      instruction->rhs.int_value < 1 || instruction->rhs.int_value > 8) {
    return 0;
  }
  return (int)instruction->rhs.int_value * 8;
}

static int ir_propagate_instruction_operands(const IRFunction *function,
                                             IRTempValueMap *temp_map,
                                             IRSymbolValueMap *symbol_map,
                                             IRInstruction *instruction,
                                             int *changed) {
  IRCpConsumer whole = {function, 0};
  IRCpConsumer stored = {function, 0};
  IRCpConsumer assigned = {function, 0};
  if (!instruction) {
    return 0;
  }
  stored.kept_bits = ir_cp_store_kept_bits(instruction);

  switch (instruction->op) {
  case IR_OP_TILE:
    if (!ir_try_propagate_operand(temp_map, symbol_map, &whole,
                                  &instruction->dest, changed)) {
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      if (!ir_try_propagate_operand(temp_map, symbol_map, &whole,
                                    &instruction->arguments[i], changed)) {
        return 0;
      }
    }
    break;

  case IR_OP_ASYNC_COPY:
  case IR_OP_TENSOR_EPILOGUE:
    for (size_t i = 0; i < instruction->argument_count; i++) {
      if (!ir_try_propagate_operand(temp_map, symbol_map, &whole,
                                    &instruction->arguments[i], changed)) {
        return 0;
      }
    }
    break;

  case IR_OP_TENSOR_MMA:
  case IR_OP_TENSOR_MATMUL:
  case IR_OP_TENSOR_COMMIT:
  case IR_OP_TENSOR_SCALE_ROWS:
  case IR_OP_STORE:
    if (!ir_try_propagate_operand(temp_map, symbol_map, &whole,
                                  &instruction->dest, changed) ||
        !ir_try_propagate_operand(temp_map, symbol_map, &stored,
                                  &instruction->lhs, changed) ||
        !ir_try_propagate_operand(temp_map, symbol_map, &whole,
                                  &instruction->rhs, changed)) {
      return 0;
    }
    break;

  case IR_OP_ROTATE_ADD:
    break;

  case IR_OP_BRANCH_ZERO:
    if (!ir_try_propagate_operand(temp_map, NULL, &whole, &instruction->lhs,
                                  changed)) {
      return 0;
    }
    break;

  case IR_OP_BRANCH_EQ:
    if (!ir_try_propagate_operand(temp_map, NULL, &whole, &instruction->lhs,
                                  changed) ||
        !ir_try_propagate_operand(temp_map, NULL, &whole, &instruction->rhs,
                                  changed)) {
      return 0;
    }
    break;

  case IR_OP_ASSIGN:
    assigned.kept_bits = ir_cp_assign_kept_bits(function, instruction);
    if (!ir_try_propagate_operand(temp_map, symbol_map, &assigned,
                                  &instruction->lhs, changed)) {
      return 0;
    }
    break;

  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_NEW:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_RETURN:
    if (!ir_try_propagate_operand(temp_map, NULL, &whole, &instruction->lhs,
                                  changed) ||
        !ir_try_propagate_operand(temp_map, NULL, &whole, &instruction->rhs,
                                  changed)) {
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      if (!ir_try_propagate_operand(temp_map, symbol_map, &whole,
                                    &instruction->arguments[i], changed)) {
        return 0;
      }
    }
    break;

  default:
    break;
  }

  return 1;
}

static int ir_cp_note_operand_uses(const IRInstruction *ins, size_t i,
                                   IRTempValueMap *temp_last,
                                   IRTempValueMap *sym_last) {
  const IROperand *ops[3] = {&ins->lhs, &ins->rhs, &ins->dest};
  IROperand pos = {.kind = IR_OPERAND_INT, .int_value = (long long)i};
  for (int k = 0; k < 3; k++) {
    if (!ops[k]->name) {
      continue;
    }
    if (ops[k]->kind == IR_OPERAND_TEMP) {
      if (!ir_temp_value_map_set(temp_last, ops[k]->name, &pos)) {
        return 0;
      }
    } else if (ops[k]->kind == IR_OPERAND_SYMBOL) {
      if (!ir_temp_value_map_set(sym_last, ops[k]->name, &pos)) {
        return 0;
      }
    }
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    const IROperand *op = &ins->arguments[a];
    if (!op->name) {
      continue;
    }
    if (op->kind == IR_OPERAND_TEMP) {
      if (!ir_temp_value_map_set(temp_last, op->name, &pos)) {
        return 0;
      }
    } else if (op->kind == IR_OPERAND_SYMBOL) {
      if (!ir_temp_value_map_set(sym_last, op->name, &pos)) {
        return 0;
      }
    }
  }
  return 1;
}

static void ir_cp_prune_dead_entries(IRTempValueMap *map,
                                     const IRTempValueMap *last_use,
                                     size_t i) {
  if (!map || map->count == 0) {
    return;
  }
  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    IRTempValueEntry *entry = &map->items[read];
    const IROperand *lu =
        entry->name ? ir_temp_value_map_lookup(last_use, entry->name) : NULL;
    if (!lu || lu->int_value <= (long long)i) {
      ir_temp_value_map_note_value_removed(map, &entry->value);
      mettle_free_string(entry->name);
      ir_operand_destroy(&entry->value);
      continue;
    }
    if (write != read) {
      map->items[write] = map->items[read];
    }
    write++;
  }
  if (map->count != write) {
    map->count = write;
    ir_temp_value_map_reindex(map);
  }
}

static int ir_try_evaluate_integer_binary(const char *op, long long lhs,
                                          long long rhs, long long *result,
                                          int *folded);

static int ir_cp_operand_constant(const IRTempValueMap *temp_map,
                                  const IRTempValueMap *symbol_map,
                                  const IROperand *operand, long long *out) {
  if (operand->kind == IR_OPERAND_INT) {
    *out = operand->int_value;
    return 1;
  }
  const IROperand *known = NULL;
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    known = ir_temp_value_map_lookup(temp_map, operand->name);
  } else if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    known = ir_temp_value_map_lookup(symbol_map, operand->name);
  }
  if (known && known->kind == IR_OPERAND_INT) {
    *out = known->int_value;
    return 1;
  }
  return 0;
}

static int ir_mtlc_type_is_unsigned_integer(const MtlcType *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
    return 1;
  default:
    return 0;
  }
}

static long long ir_narrow_folded(const MtlcType *type, long long value) {
  if (!type) {
    return value;
  }
  switch (type->kind) {
  case MTLC_TYPE_INT8: return (long long)(signed char)value;
  case MTLC_TYPE_INT16: return (long long)(short)value;
  case MTLC_TYPE_INT32: return (long long)(int)value;
  case MTLC_TYPE_UINT8: return (long long)(unsigned char)value;
  case MTLC_TYPE_UINT16: return (long long)(unsigned short)value;
  case MTLC_TYPE_UINT32: return (long long)(unsigned int)value;
  case MTLC_TYPE_BOOL: return value ? 1 : 0;
  default: return value;
  }
}

static int ir_declared_integer_width(const char *type, int *bits_out,
                                     int *unsigned_out) {
  static const struct {
    const char *name;
    int bits;
    int is_unsigned;
  } WIDTHS[] = {
      {"int64", 0, 0},  {"uint64", 0, 1},  {"int32", 32, 0}, {"uint32", 32, 1},
      {"int16", 16, 0}, {"uint16", 16, 1}, {"int8", 8, 0},   {"uint8", 8, 1},
      {"bool", 8, 1},
  };
  if (!type) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(WIDTHS) / sizeof(WIDTHS[0]); i++) {
    if (strcmp(type, WIDTHS[i].name) == 0) {
      *bits_out = WIDTHS[i].bits;
      *unsigned_out = WIDTHS[i].is_unsigned;
      return 1;
    }
  }
  return 0;
}

static void ir_cp_wrap_assign_to_home(const IRFunction *function,
                                      IRInstruction *instruction,
                                      int *any_changed) {
  if (!instruction || instruction->op != IR_OP_ASSIGN ||
      instruction->is_float ||
      instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
      instruction->lhs.kind != IR_OPERAND_INT) {
    return;
  }
  int bits = 0;
  int is_unsigned = 0;
  const char *type =
      ir_function_local_declared_type((IRFunction *)function,
                                      instruction->dest.name);
  if (!ir_declared_integer_width(type, &bits, &is_unsigned) || bits == 0) {
    return;
  }
  long long value = instruction->lhs.int_value;
  int shift = 64 - bits;
  long long wrapped =
      is_unsigned ? (long long)(((unsigned long long)value << shift) >> shift)
                  : (value << shift) >> shift;
  if (wrapped != value) {
    instruction->lhs.int_value = wrapped;
    if (any_changed) {
      *any_changed = 1;
    }
  }
}

static void ir_cp_fold_constant_binary(const IRFunction *function,
                                       const IRTempValueMap *temp_map,
                                       const IRTempValueMap *symbol_map,
                                       IRInstruction *instruction,
                                       int *any_changed) {
  if (!instruction || instruction->op != IR_OP_BINARY ||
      instruction->is_float || !instruction->text) {
    return;
  }
  long long lhs = 0, rhs = 0;
  if (!ir_cp_operand_constant(temp_map, symbol_map, &instruction->lhs, &lhs) ||
      !ir_cp_operand_constant(temp_map, symbol_map, &instruction->rhs, &rhs)) {
    return;
  }
  const char *op = instruction->text;
  if (instruction->is_unsigned) {
    if (strcmp(op, ">>") == 0 || strcmp(op, "/") == 0 ||
        strcmp(op, "%") == 0 || strcmp(op, "<") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, ">=") == 0) {
      return;
    }
  }

  int narrow_bits = 0;
  int narrow_unsigned = 0;
  int is_compare = strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                   strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0;
  if (instruction->dest.kind == IR_OPERAND_SYMBOL && !is_compare) {
    const char *type = instruction->dest.name
                           ? ir_function_local_declared_type(
                                 (IRFunction *)function,
                                 instruction->dest.name)
                           : NULL;
    if (!ir_declared_integer_width(type, &narrow_bits, &narrow_unsigned)) {
      return;
    }
  }
  if (narrow_unsigned &&
      (strcmp(op, ">>") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0 ||
       strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
       strcmp(op, ">=") == 0)) {
    return;
  }

  long long result = 0;
  int folded = 0;
  if (!ir_try_evaluate_integer_binary(op, lhs, rhs, &result, &folded) ||
      !folded) {
    return;
  }
  if (narrow_bits > 0) {
    int shift = 64 - narrow_bits;
    if (narrow_unsigned) {
      result = (long long)(((unsigned long long)result << shift) >> shift);
    } else {
      result = (result << shift) >> shift;
    }
  }
  if (narrow_bits == 0 && instruction->dest.kind != IR_OPERAND_SYMBOL) {
    result = ir_narrow_folded(instruction->value_type, result);
  }
  ir_rewrite_to_assign_int(instruction, result, any_changed);
}

typedef struct {
  IRTempValueMap map;
  IRTempValueMap symbol_map;
  IRTempValueMap addr_taken;
  IRTempValueMap temp_last_use;
  IRTempValueMap sym_last_use;
  IRTempValueMap storage_syms;
  IRTempValueMap label_index;
  IRLabelValueMap label_in;
  int label_in_live;
} IRCopyPropState;

static void ir_cp_state_destroy(IRCopyPropState *state) {
  if (state->label_in_live) {
    ir_label_value_map_destroy(&state->label_in);
    state->label_in_live = 0;
  }
  ir_temp_value_map_destroy(&state->map);
  ir_temp_value_map_destroy(&state->symbol_map);
  ir_temp_value_map_destroy(&state->addr_taken);
  ir_temp_value_map_destroy(&state->temp_last_use);
  ir_temp_value_map_destroy(&state->sym_last_use);
  ir_temp_value_map_destroy(&state->storage_syms);
  ir_temp_value_map_destroy(&state->label_index);
}

static int ir_cp_state_init(IRCopyPropState *state, IRFunction *function) {
  memset(state, 0, sizeof(*state));
  if (!ir_temp_value_map_init(&state->map) ||
      !ir_temp_value_map_init(&state->symbol_map) ||
      !ir_temp_value_map_init(&state->addr_taken) ||
      !ir_temp_value_map_init(&state->temp_last_use) ||
      !ir_temp_value_map_init(&state->sym_last_use) ||
      !ir_temp_value_map_init(&state->storage_syms) ||
      !ir_temp_value_map_init(&state->label_index) ||
      !ir_label_value_map_init(&state->label_in)) {
    ir_cp_state_destroy(state);
    return 0;
  }
  state->label_in_live = 1;
  if (!ir_addr_taken_set_build(function, &state->addr_taken) ||
      !ir_storage_symbol_set_build(function, &state->storage_syms)) {
    ir_cp_state_destroy(state);
    return 0;
  }
  return 1;
}

static int ir_cp_reset_label_flow(IRCopyPropState *state) {
  ir_label_value_map_destroy(&state->label_in);
  state->label_in_live = 0;
  if (!ir_label_value_map_init(&state->label_in)) {
    return 0;
  }
  state->label_in_live = 1;
  return 1;
}

static int ir_cp_index_labels(const IRFunction *function,
                              IRCopyPropState *state) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    IROperand at;

    if (in->op != IR_OP_LABEL || !in->text) {
      continue;
    }
    at = ir_operand_int((long long)i);
    if (!ir_temp_value_map_set(&state->label_index, in->text, &at)) {
      return 0;
    }
  }
  return 1;
}

static int ir_cp_note_uses(const IRFunction *function,
                           IRCopyPropState *state) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_cp_note_operand_uses(&function->instructions[i], i,
                                 &state->temp_last_use,
                                 &state->sym_last_use)) {
      return 0;
    }
  }
  return 1;
}

static int ir_cp_branches_forward(const IRFunction *function, size_t at,
                                  const IRCopyPropState *state) {
  const IRInstruction *br = &function->instructions[at];
  const IROperand *target;

  if ((br->op != IR_OP_JUMP && br->op != IR_OP_BRANCH_ZERO &&
       br->op != IR_OP_BRANCH_EQ) ||
      !br->text) {
    return 1;
  }
  target = ir_temp_value_map_lookup(&state->label_index, br->text);
  return target && target->kind == IR_OPERAND_INT &&
         target->int_value > (long long)at;
}

static int ir_cp_all_edges_forward(const IRFunction *function,
                                   const IRCopyPropState *state) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_cp_branches_forward(function, i, state)) {
      return 0;
    }
  }
  return 1;
}

static int ir_cp_falls_into_label(const IRFunction *function, size_t at) {
  if (at == 0) {
    return 0;
  }
  for (size_t pi = at; pi > 0;) {
    IROpcode prev_op;
    pi--;
    prev_op = function->instructions[pi].op;
    if (prev_op == IR_OP_NOP) {
      continue;
    }
    return prev_op != IR_OP_JUMP && prev_op != IR_OP_RETURN;
  }
  return 1;
}

static int ir_cp_enter_label(const IRFunction *function,
                             IRCopyPropState *state, size_t at,
                             const IRInstruction *instruction,
                             int forward_only, int *flow_changed) {
  const IRLabelValueEntry *entry;

  if (forward_only) {
    ir_cp_prune_dead_entries(&state->map, &state->temp_last_use, at);
    ir_cp_prune_dead_entries(&state->symbol_map, &state->sym_last_use, at);
  }
  if (ir_cp_falls_into_label(function, at) &&
      !ir_label_value_map_merge_incoming(&state->label_in, instruction->text,
                                         &state->map, flow_changed)) {
    return 0;
  }
  entry = ir_label_value_map_lookup(&state->label_in, instruction->text);
  if (entry && entry->initialized) {
    if (!ir_temp_value_map_clone(&state->map, &entry->in_map)) {
      return 0;
    }
  } else {
    ir_temp_value_map_clear(&state->map);
  }
  ir_temp_value_map_clear(&state->symbol_map);
  return 1;
}

static int ir_cp_record_temp_write(IRCopyPropState *state,
                                   IRInstruction *instruction) {
  if (!ir_instruction_writes_temp(instruction) || !instruction->dest.name) {
    return 1;
  }
  ir_temp_value_map_remove(&state->map, instruction->dest.name);
  if (instruction->op != IR_OP_ASSIGN || instruction->is_volatile ||
      !ir_operand_is_propagatable_value(&instruction->lhs)) {
    return 1;
  }
  if (ir_operand_is_temp(&instruction->lhs) &&
      ir_operand_names_match(&instruction->lhs, &instruction->dest)) {
    ir_temp_value_map_remove(&state->map, instruction->dest.name);
    return 1;
  }
  return ir_temp_value_map_set(&state->map, instruction->dest.name,
                               &instruction->lhs);
}

static int ir_cp_record_symbol_write(IRCopyPropState *state,
                                     IRInstruction *instruction) {
  if (!ir_instruction_writes_symbol(instruction) || !instruction->dest.name) {
    return 1;
  }
  ir_temp_value_map_remove_symbol_values(&state->map, instruction->dest.name);
  ir_symbol_value_map_invalidate_name(&state->symbol_map,
                                      instruction->dest.name);
  if (instruction->op != IR_OP_ASSIGN || instruction->is_volatile ||
      !ir_operand_is_propagatable_value(&instruction->lhs) ||
      instruction->dest.kind != IR_OPERAND_SYMBOL ||
      ir_temp_value_map_lookup(&state->storage_syms, instruction->dest.name)) {
    return 1;
  }
  if (ir_operand_is_symbol(&instruction->lhs) &&
      ir_operand_names_match(&instruction->lhs, &instruction->dest)) {
    ir_symbol_value_map_invalidate_name(&state->symbol_map,
                                        instruction->dest.name);
    return 1;
  }
  return ir_temp_value_map_set(&state->symbol_map, instruction->dest.name,
                               &instruction->lhs);
}

static void ir_cp_invalidate_for(IRCopyPropState *state,
                                 const IRInstruction *instruction) {
  if (instruction->op == IR_OP_ROTATE_ADD && instruction->dest.name) {
    ir_symbol_value_map_invalidate_name(&state->symbol_map,
                                        instruction->dest.name);
    if (instruction->lhs.name) {
      ir_symbol_value_map_invalidate_name(&state->symbol_map,
                                          instruction->lhs.name);
    }
    if (instruction->rhs.name) {
      ir_symbol_value_map_invalidate_name(&state->symbol_map,
                                          instruction->rhs.name);
    }
  }
  if (instruction->op == IR_OP_INLINE_ASM && instruction->asm_operands) {
    ir_temp_value_map_invalidate_after_store(&state->map, &state->addr_taken);
    ir_temp_value_map_invalidate_after_store(&state->symbol_map,
                                             &state->addr_taken);
  } else if (instruction->op == IR_OP_STORE) {
    ir_temp_value_map_invalidate_after_store(&state->map, &state->addr_taken);
    ir_temp_value_map_invalidate_after_store(&state->symbol_map,
                                             &state->addr_taken);
  } else if (instruction->op == IR_OP_CALL ||
             instruction->op == IR_OP_CALL_INDIRECT ||
             instruction->op == IR_OP_INLINE_ASM) {
    ir_temp_value_map_remove_symbol_values(&state->map, NULL);
    ir_temp_value_map_clear(&state->symbol_map);
  } else if (instruction->op != IR_OP_RETURN &&
             ir_instruction_has_side_effect(instruction)) {
    ir_temp_value_map_invalidate_after_store(&state->map, &state->addr_taken);
    ir_temp_value_map_invalidate_after_store(&state->symbol_map,
                                             &state->addr_taken);
  }
}

static int ir_cp_leave_block(IRCopyPropState *state,
                             const IRInstruction *instruction, size_t at,
                             int forward_only, int *flow_changed) {
  int branches = instruction->op == IR_OP_JUMP ||
                 instruction->op == IR_OP_BRANCH_ZERO ||
                 instruction->op == IR_OP_BRANCH_EQ;

  if (branches && instruction->text) {
    if (forward_only) {
      ir_cp_prune_dead_entries(&state->map, &state->temp_last_use, at);
    }
    if (!ir_label_value_map_merge_incoming(&state->label_in, instruction->text,
                                           &state->map, flow_changed)) {
      return 0;
    }
  }
  if (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN) {
    ir_temp_value_map_clear(&state->map);
    ir_temp_value_map_clear(&state->symbol_map);
  }
  return 1;
}

static int ir_cp_visit(IRFunction *function, IRCopyPropState *state, size_t at,
                       int rewrite, int forward_only, int *flow_changed,
                       int *round_changed) {
  IRInstruction *instruction = &function->instructions[at];

  if (instruction->op == IR_OP_LABEL && instruction->text &&
      !ir_cp_enter_label(function, state, at, instruction, forward_only,
                         flow_changed)) {
    return 0;
  }
  if (rewrite) {
    if (!ir_propagate_instruction_operands(function, &state->map,
                                           &state->symbol_map,
                                           instruction, round_changed)) {
      return 0;
    }
    ir_cp_fold_constant_binary(function, &state->map, &state->symbol_map,
                               instruction, round_changed);
    ir_cp_wrap_assign_to_home(function, instruction, round_changed);
  }
  if (!ir_cp_record_temp_write(state, instruction) ||
      !ir_cp_record_symbol_write(state, instruction)) {
    return 0;
  }
  ir_cp_invalidate_for(state, instruction);
  return ir_cp_leave_block(state, instruction, at, forward_only, flow_changed);
}

static int ir_cp_prepare_round(IRFunction *function, IRCopyPropState *state,
                               int *forward_only) {
  ir_temp_value_map_clear(&state->temp_last_use);
  ir_temp_value_map_clear(&state->sym_last_use);
  ir_temp_value_map_clear(&state->label_index);
  if (!ir_cp_reset_label_flow(state) || !ir_cp_index_labels(function, state)) {
    return 0;
  }
  *forward_only = ir_cp_all_edges_forward(function, state);
  return ir_cp_note_uses(function, state);
}

static int ir_cp_flow_pass(IRFunction *function, IRCopyPropState *state,
                           int rewrite, int forward_only, int *flow_changed,
                           int *round_changed) {
  ir_temp_value_map_clear(&state->map);
  ir_temp_value_map_clear(&state->symbol_map);
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_cp_visit(function, state, i, rewrite, forward_only, flow_changed,
                     round_changed)) {
      return 0;
    }
  }
  return 1;
}

static int ir_cp_round(IRFunction *function, IRCopyPropState *state,
                       int *round_changed) {
  int forward_only = 0;

  *round_changed = 0;
  if (!ir_cp_prepare_round(function, state, &forward_only)) {
    return 0;
  }
  int flow_settled = forward_only;
  for (int iteration = 0; iteration < 12; iteration++) {
    int flow_changed = 0;
    const int rewrite = flow_settled;

    if (!ir_cp_flow_pass(function, state, rewrite, forward_only, &flow_changed,
                         round_changed)) {
      return 0;
    }
    if (rewrite) {
      break;
    }
    if (!flow_changed) {
      flow_settled = 1;
    }
  }
  return 1;
}

int ir_copy_and_constant_propagation_pass(IRFunction *function, int *changed) {
  IRCopyPropState state;
  int any_changed = 0;

  if (!function) {
    return 0;
  }
  if (!ir_cp_state_init(&state, function)) {
    return 0;
  }
  for (int round = 0; round < 4; round++) {
    int round_changed = 0;
    if (!ir_cp_round(function, &state, &round_changed)) {
      ir_cp_state_destroy(&state);
      return 0;
    }
    if (!round_changed) {
      break;
    }
    any_changed = 1;
  }
  if (changed && any_changed) {
    *changed = 1;
  }
  ir_cp_state_destroy(&state);
  return 1;
}

static int ir_try_evaluate_integer_binary(const char *op, long long lhs,
                                          long long rhs, long long *result,
                                          int *folded) {
  if (!op || !result || !folded) {
    return 0;
  }

  *folded = 1;

  if (strcmp(op, "+") == 0) {
    *result = (long long)((unsigned long long)lhs + (unsigned long long)rhs);
  } else if (strcmp(op, "-") == 0) {
    *result = (long long)((unsigned long long)lhs - (unsigned long long)rhs);
  } else if (strcmp(op, "*") == 0) {
    *result = (long long)((unsigned long long)lhs * (unsigned long long)rhs);
  } else if (strcmp(op, "/") == 0) {
    if (rhs == 0) {
      *folded = 0;
    } else if (rhs == -1) {
      *result = (long long)(0ULL - (unsigned long long)lhs);
    } else {
      *result = lhs / rhs;
    }
  } else if (strcmp(op, "%") == 0) {
    if (rhs == 0) {
      *folded = 0;
    } else if (rhs == -1) {
      *result = 0;
    } else {
      *result = lhs % rhs;
    }
  } else if (strcmp(op, "==") == 0) {
    *result = lhs == rhs;
  } else if (strcmp(op, "!=") == 0) {
    *result = lhs != rhs;
  } else if (strcmp(op, "<") == 0) {
    *result = lhs < rhs;
  } else if (strcmp(op, "<=") == 0) {
    *result = lhs <= rhs;
  } else if (strcmp(op, ">") == 0) {
    *result = lhs > rhs;
  } else if (strcmp(op, ">=") == 0) {
    *result = lhs >= rhs;
  } else if (strcmp(op, "&&") == 0) {
    *result = lhs && rhs;
  } else if (strcmp(op, "||") == 0) {
    *result = lhs || rhs;
  } else if (strcmp(op, "&") == 0) {
    *result = lhs & rhs;
  } else if (strcmp(op, "|") == 0) {
    *result = lhs | rhs;
  } else if (strcmp(op, "^") == 0) {
    *result = lhs ^ rhs;
  } else if (strcmp(op, "<<") == 0) {
    if (rhs < 0 || rhs >= 64) {
      *folded = 0;
    } else {
      *result = (long long)((unsigned long long)lhs << (unsigned long long)rhs);
    }
  } else if (strcmp(op, ">>") == 0) {
    if (rhs < 0 || rhs >= 64) {
      *folded = 0;
    } else {
      *result = lhs >> rhs;
    }
  } else {
    *folded = 0;
  }

  return 1;
}

static int ir_pow2_reciprocal(double c, int float_bits, double *out) {
  uint64_t bits = 0;
  uint64_t sign = 0;
  int biased = 0;
  int limit = float_bits == 32 ? 126 : 1022;
  memcpy(&bits, &c, sizeof(bits));
  sign = bits & 0x8000000000000000ull;
  if ((bits & 0x000FFFFFFFFFFFFFull) != 0) {
    return 0;
  }
  biased = (int)((bits >> 52) & 0x7FFull);
  if (biased == 0 || biased == 0x7FF) {
    return 0;
  }
  {
    int k = biased - 1023;
    if (k > limit || k < -limit) {
      return 0;
    }
    bits = sign | ((uint64_t)(1023 - k) << 52);
  }
  memcpy(out, &bits, sizeof(bits));
  return 1;
}
static int ir_float_divisor_value(const IROperand *rhs, double *out) {
  const IRModuleSymbol *symbol;
  if (rhs->kind == IR_OPERAND_FLOAT) {
    *out = rhs->float_value;
    return 1;
  }
  if (rhs->kind != IR_OPERAND_SYMBOL || !rhs->name) {
    return 0;
  }
  symbol = ir_optimize_module_symbol(rhs->name);
  if (!symbol || symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
      !symbol->is_immutable || !symbol->has_initializer ||
      !symbol->init_is_float || symbol->init_symbol_ref) {
    return 0;
  }
  memcpy(out, &symbol->init_bits, sizeof(*out));
  return 1;
}

static int ir_try_fold_float_reciprocal(IRInstruction *instruction,
                                        int *changed) {
  double divisor = 0.0;
  double reciprocal = 0.0;
  if (!instruction || instruction->op != IR_OP_BINARY ||
      !instruction->is_float || !instruction->text ||
      strcmp(instruction->text, "/") != 0) {
    return 1;
  }
  if (!ir_float_divisor_value(&instruction->rhs, &divisor)) {
    return 1;
  }
  if (!ir_pow2_reciprocal(divisor, instruction->float_bits, &reciprocal)) {
    return 1;
  }
  mettle_free_string(instruction->text);
  instruction->text = mettle_strdup("*");
  if (!instruction->text) {
    return 0;
  }
  ir_operand_destroy(&instruction->rhs);
  instruction->rhs = ir_operand_float_sized(reciprocal,
                                            instruction->float_bits);
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_try_fold_integer_binary(IRInstruction *instruction,
                                      IRValueRangeCtx *ranges, size_t at,
                                      int *changed) {
  if (!instruction || instruction->op != IR_OP_BINARY || instruction->is_float ||
      !instruction->text) {
    return 1;
  }

  if ((instruction->is_unsigned ||
       ir_mtlc_type_is_unsigned_integer(instruction->value_type)) &&
      (strcmp(instruction->text, ">>") == 0 ||
       strcmp(instruction->text, "/") == 0 ||
       strcmp(instruction->text, "%") == 0 ||
       strcmp(instruction->text, "<") == 0 ||
       strcmp(instruction->text, "<=") == 0 ||
       strcmp(instruction->text, ">") == 0 ||
       strcmp(instruction->text, ">=") == 0)) {
    return ir_rewrite_apply_binary_identities(instruction, ranges, at, changed);
  }

  if (instruction->lhs.kind == IR_OPERAND_INT &&
      instruction->rhs.kind == IR_OPERAND_INT) {
    long long result = 0;
    int folded = 0;
    if (!ir_try_evaluate_integer_binary(instruction->text,
                                        instruction->lhs.int_value,
                                        instruction->rhs.int_value, &result,
                                        &folded)) {
      return 0;
    }
    if (folded) {
      return ir_rewrite_to_assign_int(
          instruction, ir_narrow_folded(instruction->value_type, result),
          changed);
    }
  }

  return ir_rewrite_apply_binary_identities(instruction, ranges, at, changed);
}

int ir_find_next_non_nop_in_block(const IRFunction *function,
                                         size_t start_index, size_t *out_index) {
  if (!function || !out_index || start_index >= function->instruction_count) {
    return 0;
  }

  size_t i = start_index;
  while (i < function->instruction_count) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      return 0;
    }
    if (instruction->op != IR_OP_NOP) {
      *out_index = i;
      return 1;
    }
    i++;
  }
  return 0;
}

int ir_find_next_non_nop(const IRFunction *function, size_t start_index,
                                size_t *out_index) {
  if (!function || !out_index || start_index >= function->instruction_count) {
    return 0;
  }

  size_t i = start_index;
  while (i < function->instruction_count) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_NOP) {
      *out_index = i;
      return 1;
    }
    i++;
  }
  return 0;
}

int ir_find_label_index(const IRFunction *function, const char *label,
                               size_t *out_index) {
  if (!function || !label || !out_index) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, label) == 0) {
      *out_index = i;
      return 1;
    }
  }

  return 0;
}

static int ir_branch_zero_not_equal_zero_forwarding_pass(IRFunction *function,
                                                         int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap uses;
  if (!ir_temp_use_map_init(&uses)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *cmp = &function->instructions[i];
    if (cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text ||
        strcmp(cmp->text, "!=") != 0 || cmp->dest.kind != IR_OPERAND_TEMP ||
        !cmp->dest.name) {
      continue;
    }

    if (ir_temp_use_map_get(&uses, cmp->dest.name) != 1) {
      continue;
    }

    const IROperand *forward = NULL;
    if (cmp->rhs.kind == IR_OPERAND_INT && cmp->rhs.int_value == 0) {
      forward = &cmp->lhs;
    } else if (cmp->lhs.kind == IR_OPERAND_INT && cmp->lhs.int_value == 0) {
      forward = &cmp->rhs;
    } else {
      continue;
    }

    size_t branch_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &branch_index)) {
      continue;
    }

    IRInstruction *branch = &function->instructions[branch_index];
    if (branch->op != IR_OP_BRANCH_ZERO || branch->lhs.kind != IR_OPERAND_TEMP ||
        !branch->lhs.name || !ir_operand_names_match(&branch->lhs, &cmp->dest)) {
      continue;
    }

    IROperand cloned = ir_operand_none();
    if (!ir_operand_clone(forward, &cloned)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
    ir_operand_destroy(&branch->lhs);
    branch->lhs = cloned;

    ir_instruction_make_nop(cmp);
    if (changed) {
      *changed = 1;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
}

static int ir_rewrite_branch_eq_shortcut(IRInstruction *producer,
                                         IRInstruction *branch_zero,
                                         IRInstruction *jump_true,
                                         int *changed) {
  if (!producer || !branch_zero || !jump_true || !jump_true->text) {
    return 0;
  }

  IROperand lhs = ir_operand_none();
  IROperand rhs = ir_operand_none();
  if (!ir_operand_clone(&producer->lhs, &lhs) ||
      !ir_operand_clone(&producer->rhs, &rhs)) {
    ir_operand_destroy(&lhs);
    ir_operand_destroy(&rhs);
    return 0;
  }

  char *target = mettle_strdup(jump_true->text);
  if (!target) {
    ir_operand_destroy(&lhs);
    ir_operand_destroy(&rhs);
    return 0;
  }

  ir_operand_destroy(&branch_zero->lhs);
  ir_operand_destroy(&branch_zero->rhs);
  mettle_free_string(branch_zero->text);
  ir_instruction_clear_arguments(branch_zero);
  branch_zero->op = IR_OP_BRANCH_EQ;
  branch_zero->lhs = lhs;
  branch_zero->rhs = rhs;
  branch_zero->text = target;
  branch_zero->is_float = producer->is_float;
  branch_zero->ast_ref = NULL;

  ir_instruction_make_nop(producer);
  ir_instruction_make_nop(jump_true);
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_branch_eq_chain_shortcut_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap uses;
  if (!ir_temp_use_map_init(&uses)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *producer = &function->instructions[i];
    if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text ||
        strcmp(producer->text, "==") != 0 ||
        producer->dest.kind != IR_OPERAND_TEMP || !producer->dest.name) {
      continue;
    }

    if (ir_temp_use_map_get(&uses, producer->dest.name) != 1) {
      continue;
    }

    size_t branch_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &branch_index)) {
      continue;
    }
    IRInstruction *branch = &function->instructions[branch_index];
    if (branch->op != IR_OP_BRANCH_ZERO || branch->lhs.kind != IR_OPERAND_TEMP ||
        !branch->lhs.name ||
        !ir_operand_names_match(&branch->lhs, &producer->dest) || !branch->text) {
      continue;
    }

    size_t jump_index = 0;
    if (!ir_find_next_non_nop_in_block(function, branch_index + 1, &jump_index)) {
      continue;
    }
    IRInstruction *jump_true = &function->instructions[jump_index];
    if (jump_true->op != IR_OP_JUMP || !jump_true->text) {
      continue;
    }

    size_t false_label_index = 0;
    if (!ir_find_next_non_nop(function, jump_index + 1, &false_label_index)) {
      continue;
    }
    IRInstruction *false_label = &function->instructions[false_label_index];
    if (false_label->op != IR_OP_LABEL || !false_label->text ||
        strcmp(false_label->text, branch->text) != 0) {
      continue;
    }

    if (!ir_rewrite_branch_eq_shortcut(producer, branch, jump_true, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
}

static int ir_bitset_same_operand(const IROperand *a, const IROperand *b) {
  return a->kind == b->kind && a->name && b->name &&
         ir_operand_names_match(a, b) &&
         (a->kind == IR_OPERAND_TEMP || a->kind == IR_OPERAND_SYMBOL);
}

static int ir_simplify_redundant_assign(IRInstruction *instruction,
                                        int *changed) {
  if (!instruction || instruction->op != IR_OP_ASSIGN ||
      instruction->dest.kind == IR_OPERAND_NONE ||
      instruction->lhs.kind == IR_OPERAND_NONE) {
    return 1;
  }

  if (ir_operand_equals(&instruction->dest, &instruction->lhs)) {
    ir_instruction_make_nop(instruction);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}

static int ir_try_fold_integer_unary(IRInstruction *instruction, int *changed) {
  if (!instruction || instruction->op != IR_OP_UNARY || instruction->is_float ||
      !instruction->text || instruction->lhs.kind != IR_OPERAND_INT) {
    return 1;
  }

  long long value = instruction->lhs.int_value;
  long long result = 0;
  int fold = 1;

  if (strcmp(instruction->text, "+") == 0) {
    result = value;
  } else if (strcmp(instruction->text, "-") == 0) {
    result = (long long)(-(unsigned long long)value);
  } else if (strcmp(instruction->text, "!") == 0) {
    result = !value;
  } else if (strcmp(instruction->text, "~") == 0) {
    result = ~value;
  } else {
    fold = 0;
  }

  if (fold) {
    return ir_rewrite_to_assign_int(instruction, result, changed);
  }

  return 1;
}

static int ir_try_fold_integer_cast(IRInstruction *instruction, int *changed) {
  int bits = 0;
  int is_unsigned = 0;
  unsigned long long value;
  if (!instruction || instruction->op != IR_OP_CAST || instruction->is_float ||
      !instruction->text || instruction->lhs.kind != IR_OPERAND_INT ||
      instruction->dest.kind == IR_OPERAND_NONE ||
      !ir_int_type_name_info(instruction->text, &bits, &is_unsigned) ||
      bits <= 0 || bits > 64) {
    return 1;
  }
  value = (unsigned long long)instruction->lhs.int_value;
  if (bits < 64) {
    value &= (1ULL << bits) - 1ULL;
    if (!is_unsigned && (value & (1ULL << (bits - 1)))) {
      value |= ~((1ULL << bits) - 1ULL);
    }
  }
  return ir_rewrite_to_assign_int(instruction, (long long)value, changed);
}

static int ir_simplify_branch(IRInstruction *instruction, int *changed) {
  if (!instruction) {
    return 0;
  }

  if (instruction->op == IR_OP_BRANCH_ZERO &&
      instruction->lhs.kind == IR_OPERAND_INT) {
    if (instruction->lhs.int_value == 0) {
      ir_instruction_make_jump(instruction);
    } else {
      ir_instruction_make_nop(instruction);
    }
    if (changed) {
      *changed = 1;
    }
  } else if (instruction->op == IR_OP_BRANCH_EQ &&
             instruction->lhs.kind == IR_OPERAND_INT &&
             instruction->rhs.kind == IR_OPERAND_INT) {
    if (instruction->lhs.int_value == instruction->rhs.int_value) {
      ir_instruction_make_jump(instruction);
    } else {
      ir_instruction_make_nop(instruction);
    }
    if (changed) {
      *changed = 1;
    }
  } else if (instruction->op == IR_OP_BRANCH_EQ &&
             ir_operand_equals(&instruction->lhs, &instruction->rhs)) {
    ir_instruction_make_jump(instruction);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}

static int ir_float_pow2_reciprocal(const IROperand *operand, double *out) {
  unsigned long long bits = 0;
  double value = operand->float_value;
  int exponent;
  memcpy(&bits, &value, sizeof(bits));
  if ((bits & 0x000FFFFFFFFFFFFFULL) != 0ULL) {
    return 0;
  }
  exponent = (int)((bits >> 52) & 0x7FFULL);
  if (operand->float_bits == 32) {
    if (exponent < 923 || exponent > 1123) {
      return 0;
    }
  } else if (exponent < 1 || exponent > 2045) {
    return 0;
  }
  *out = 1.0 / value;
  return 1;
}

int ir_float_divide_by_power_of_two_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    double reciprocal = 0.0;
    char *product;
    if (instruction->op != IR_OP_BINARY || !instruction->is_float ||
        !instruction->text || strcmp(instruction->text, "/") != 0 ||
        instruction->rhs.kind != IR_OPERAND_FLOAT ||
        !ir_float_pow2_reciprocal(&instruction->rhs, &reciprocal)) {
      continue;
    }
    product = mettle_strdup("*");
    if (!product) {
      return 0;
    }
    mettle_free_string(instruction->text);
    instruction->text = product;
    instruction->rhs.float_value = reciprocal;
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}

int ir_constant_and_branch_simplify_pass(IRFunction *function,
                                                int *changed) {
  if (!function) {
    return 0;
  }

  int has_mod = 0;
  int has_eq = 0;
  int has_ne = 0;
  int has_branch_zero = 0;
  int has_jump = 0;
  int has_label = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    switch (instruction->op) {
    case IR_OP_BINARY:
      if (instruction->text) {
        if (strcmp(instruction->text, "%") == 0) {
          has_mod = 1;
        } else if (strcmp(instruction->text, "==") == 0) {
          has_eq = 1;
        } else if (strcmp(instruction->text, "!=") == 0) {
          has_ne = 1;
        }
      }
      break;
    case IR_OP_BRANCH_ZERO:
      has_branch_zero = 1;
      break;
    case IR_OP_JUMP:
      has_jump = 1;
      break;
    case IR_OP_LABEL:
      has_label = 1;
      break;
    default:
      break;
    }
  }

  if (has_mod && (has_eq || has_ne || has_branch_zero)) {
    if (!ir_remainder_zero_test_to_mask_pass(function, changed)) {
      return 0;
    }
  }
  if (has_eq && has_branch_zero && has_jump && has_label) {
    if (!ir_branch_eq_chain_shortcut_pass(function, changed)) {
      return 0;
    }
  }
  if (has_ne && has_branch_zero) {
    if (!ir_branch_zero_not_equal_zero_forwarding_pass(function, changed)) {
      return 0;
    }
  }

  IRValueRangeCtx ranges;
  ir_value_range_ctx_init(&ranges, function);

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (!ir_try_fold_integer_binary(instruction, &ranges, i, changed) ||
        !ir_try_fold_float_reciprocal(instruction, changed) ||
        !ir_try_fold_integer_unary(instruction, changed) ||
        !ir_try_fold_integer_cast(instruction, changed) ||
        !ir_value_range_simplify(&ranges, i, instruction, changed) ||
        !ir_simplify_redundant_assign(instruction, changed) ||
        !ir_simplify_branch(instruction, changed)) {
      ir_value_range_ctx_destroy(&ranges);
      return 0;
    }
  }

  ir_value_range_ctx_destroy(&ranges);
  return 1;
}

int ir_remove_redundant_jumps_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_JUMP || !instruction->text) {
      continue;
    }

    size_t next = i + 1;
    while (next < function->instruction_count &&
           function->instructions[next].op == IR_OP_NOP) {
      next++;
    }

    if (next < function->instruction_count &&
        function->instructions[next].op == IR_OP_LABEL &&
        function->instructions[next].text &&
        strcmp(function->instructions[next].text, instruction->text) == 0) {
      ir_instruction_make_nop(instruction);
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

typedef struct {
  const char **names;
  size_t *indices;
  size_t capacity;
} IRLabelPosIndex;

static void ir_label_pos_index_destroy(IRLabelPosIndex *index) {
  if (!index) {
    return;
  }
  free(index->names);
  free(index->indices);
  index->names = NULL;
  index->indices = NULL;
  index->capacity = 0;
}

static int ir_label_pos_index_build(const IRFunction *function,
                                    IRLabelPosIndex *index) {
  size_t capacity = 16;
  size_t mask;

  index->names = NULL;
  index->indices = NULL;
  index->capacity = 0;
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  while (capacity < function->instruction_count * 2) {
    capacity *= 2;
  }
  index->names = (const char **)calloc(capacity, sizeof(*index->names));
  index->indices = (size_t *)calloc(capacity, sizeof(*index->indices));
  if (!index->names || !index->indices) {
    ir_label_pos_index_destroy(index);
    return 0;
  }
  index->capacity = capacity;
  mask = capacity - 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    size_t slot;
    if (instruction->op != IR_OP_LABEL || !instruction->text) {
      continue;
    }
    slot = (size_t)mettle_fnv1a_hash(instruction->text) & mask;
    while (index->names[slot]) {
      if (strcmp(index->names[slot], instruction->text) == 0) {
        break;
      }
      slot = (slot + 1) & mask;
    }
    if (!index->names[slot]) {
      index->names[slot] = instruction->text;
      index->indices[slot] = i;
    }
  }
  return 1;
}

static int ir_label_pos_index_find(const IRLabelPosIndex *index,
                                   const char *name, size_t *out_index) {
  size_t mask;
  size_t slot;

  if (!index->capacity || !name) {
    return 0;
  }
  mask = index->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(name) & mask;
  while (index->names[slot]) {
    if (strcmp(index->names[slot], name) == 0) {
      *out_index = index->indices[slot];
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

int ir_thread_jump_targets_pass(IRFunction *function, int *changed) {
  IRLabelPosIndex labels;

  if (!function) {
    return 0;
  }
  if (!ir_label_pos_index_build(function, &labels)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if ((instruction->op != IR_OP_JUMP &&
         instruction->op != IR_OP_BRANCH_ZERO &&
         instruction->op != IR_OP_BRANCH_EQ) ||
        !instruction->text) {
      continue;
    }

    const char *current_target = instruction->text;
    for (int depth = 0; depth < 32; depth++) {
      size_t label_index = 0;
      if (!ir_label_pos_index_find(&labels, current_target, &label_index)) {
        break;
      }

      size_t next_index = 0;
      if (!ir_find_next_non_nop(function, label_index + 1, &next_index)) {
        break;
      }

      IRInstruction *next = &function->instructions[next_index];
      if (next->op != IR_OP_JUMP || !next->text ||
          strcmp(next->text, current_target) == 0) {
        break;
      }

      current_target = next->text;
    }

    if (strcmp(current_target, instruction->text) != 0) {
      char *target_copy = mettle_strdup(current_target);
      if (!target_copy) {
        ir_label_pos_index_destroy(&labels);
        return 0;
      }
      mettle_free_string(instruction->text);
      instruction->text = target_copy;
      if (changed) {
        *changed = 1;
      }
    }
  }

  ir_label_pos_index_destroy(&labels);
  return 1;
}

int ir_remove_redundant_fallthrough_branches_pass(IRFunction *function,
                                                         int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if ((instruction->op != IR_OP_BRANCH_ZERO &&
         instruction->op != IR_OP_BRANCH_EQ) ||
        !instruction->text) {
      continue;
    }

    size_t next = i + 1;
    while (next < function->instruction_count &&
           function->instructions[next].op == IR_OP_NOP) {
      next++;
    }

    if (next < function->instruction_count &&
        function->instructions[next].op == IR_OP_LABEL &&
        function->instructions[next].text &&
        strcmp(function->instructions[next].text, instruction->text) == 0) {
      ir_instruction_make_nop(instruction);
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

int ir_remove_empty_conditional_diamonds_pass(IRFunction *function,
                                                     int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *branch = &function->instructions[i];
    if ((branch->op != IR_OP_BRANCH_ZERO && branch->op != IR_OP_BRANCH_EQ) ||
        !branch->text) {
      continue;
    }

    size_t jump_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &jump_index)) {
      continue;
    }

    IRInstruction *jump = &function->instructions[jump_index];
    if (jump->op != IR_OP_JUMP || !jump->text) {
      continue;
    }

    if (strcmp(branch->text, jump->text) == 0) {
      size_t after_jump = jump_index + 1;
      while (after_jump < function->instruction_count &&
             function->instructions[after_jump].op == IR_OP_NOP) {
        after_jump++;
      }
      if (after_jump < function->instruction_count &&
          function->instructions[after_jump].op == IR_OP_LABEL &&
          function->instructions[after_jump].text &&
          strcmp(function->instructions[after_jump].text, jump->text) == 0) {
        ir_instruction_make_nop(branch);
        ir_instruction_make_nop(jump);
        if (changed) {
          *changed = 1;
        }
      }
      continue;
    }

    size_t cursor = jump_index + 1;
    while (cursor < function->instruction_count &&
           function->instructions[cursor].op == IR_OP_NOP) {
      cursor++;
    }

    if (cursor >= function->instruction_count ||
        function->instructions[cursor].op != IR_OP_LABEL ||
        !function->instructions[cursor].text ||
        strcmp(function->instructions[cursor].text, branch->text) != 0) {
      continue;
    }

    cursor++;
    int reaches_jump_target_with_no_body = 0;
    while (cursor < function->instruction_count) {
      IRInstruction *probe = &function->instructions[cursor];
      if (probe->op == IR_OP_NOP) {
        cursor++;
        continue;
      }
      if (probe->op != IR_OP_LABEL || !probe->text) {
        break;
      }
      if (strcmp(probe->text, jump->text) == 0) {
        reaches_jump_target_with_no_body = 1;
        break;
      }
      cursor++;
    }

    if (!reaches_jump_target_with_no_body) {
      continue;
    }

    ir_instruction_make_nop(branch);
    ir_instruction_make_nop(jump);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}

static int ir_note_symbol_name(IRNameMap *names, const IROperand *operand) {
  return !ir_operand_is_symbol(operand) ||
         ir_name_map_add(names, operand->name, operand->name);
}

static int ir_note_symbol_names(IRNameMap *names, const IRInstruction *in) {
  if (!ir_note_symbol_name(names, &in->dest) ||
      !ir_note_symbol_name(names, &in->lhs) ||
      !ir_note_symbol_name(names, &in->rhs)) {
    return 0;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    if (!ir_note_symbol_name(names, &in->arguments[a])) {
      return 0;
    }
  }
  return 1;
}

static int ir_is_symbol_declaration(const IRInstruction *in) {
  return in->op == IR_OP_DECLARE_LOCAL && ir_operand_is_symbol(&in->dest);
}

static int ir_keep_declarations_still_named(const IRFunction *function,
                                            unsigned char *reachable) {
  int any_dead = 0;
  for (size_t i = 0; i < function->instruction_count && !any_dead; i++) {
    any_dead = !reachable[i] && ir_is_symbol_declaration(&function->instructions[i]);
  }
  if (!any_dead) {
    return 1;
  }
  IRNameMap names = {0};
  int ok = 1;
  for (size_t i = 0; i < function->instruction_count && ok; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (reachable[i] && !ir_is_symbol_declaration(in)) {
      ok = ir_note_symbol_names(&names, in);
    }
  }
  for (size_t i = 0; i < function->instruction_count && ok; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (!reachable[i] && ir_is_symbol_declaration(in) &&
        ir_name_map_lookup(&names, in->dest.name)) {
      reachable[i] = 1;
    }
  }
  ir_name_map_destroy(&names);
  return ok;
}

int ir_eliminate_unreachable_blocks_pass(IRFunction *function,
                                                int *changed) {
  if (!function) {
    return 0;
  }
  if (function->instruction_count == 0) {
    return 1;
  }

  if (!ir_function_rebuild_cfg(function)) {
    return 0;
  }

  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!blocks || block_count == 0) {
    return block_count == 0;
  }

  unsigned char *reachable_blocks = calloc(block_count, 1);
  unsigned char *reachable_instructions = calloc(function->instruction_count, 1);
  if (!reachable_blocks || !reachable_instructions) {
    free(reachable_blocks);
    free(reachable_instructions);
    return 0;
  }

  IRIndexVector worklist = {0};
  if (!ir_index_vector_append(&worklist, function->entry_block)) {
    ir_index_vector_destroy(&worklist);
    free(reachable_blocks);
    free(reachable_instructions);
    return 0;
  }

  for (size_t work_index = 0; work_index < worklist.count; work_index++) {
    size_t block_index = worklist.items[work_index];
    if (block_index >= block_count || reachable_blocks[block_index]) {
      continue;
    }

    const IRBasicBlock *block = &blocks[block_index];
    reachable_blocks[block_index] = 1;
    for (size_t i = 0; i < block->instruction_count; i++) {
      size_t instruction_index = block->first_instruction + i;
      if (instruction_index < function->instruction_count) {
        reachable_instructions[instruction_index] = 1;
      }
    }

    for (size_t i = 0; i < block->successor_count; i++) {
      if (!ir_index_vector_append(&worklist, block->successors[i])) {
        ir_index_vector_destroy(&worklist);
        free(reachable_blocks);
        free(reachable_instructions);
        return 0;
      }
    }
  }

  if (!ir_keep_declarations_still_named(function, reachable_instructions)) {
    ir_index_vector_destroy(&worklist);
    free(reachable_blocks);
    free(reachable_instructions);
    return 0;
  }

  int local_changed = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!reachable_instructions[i] &&
        function->instructions[i].op != IR_OP_NOP) {
      ir_instruction_make_nop(&function->instructions[i]);
      local_changed = 1;
      if (changed) {
        *changed = 1;
      }
    }
  }

  if (local_changed) {
    ir_function_clear_cfg(function);
  }

  ir_index_vector_destroy(&worklist);
  free(reachable_blocks);
  free(reachable_instructions);
  return 1;
}

static int ir_instruction_references_label(const IRInstruction *instruction,
                                           const char *label) {
  if (!instruction || !label || !instruction->text) {
    return 0;
  }

  if (instruction->op != IR_OP_JUMP && instruction->op != IR_OP_BRANCH_ZERO &&
      instruction->op != IR_OP_BRANCH_EQ) {
    return 0;
  }

  return strcmp(instruction->text, label) == 0;
}

static int ir_label_only_referenced_by(const IRFunction *function,
                                       const char *label,
                                       size_t expected_index) {
  int found = 0;
  if (!function || !label || expected_index >= function->instruction_count) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_instruction_references_label(&function->instructions[i], label)) {
      continue;
    }
    if (i != expected_index) {
      return 0;
    }
    found = 1;
  }
  return found;
}

static int ir_ascii_range_compare(const IRInstruction *instruction,
                                  const char *op, long long bound,
                                  const IROperand *value) {
  return instruction && instruction->op == IR_OP_BINARY &&
         !instruction->is_float && !instruction->is_unsigned &&
         instruction->text && strcmp(instruction->text, op) == 0 &&
         instruction->dest.kind == IR_OPERAND_TEMP &&
         instruction->dest.name && instruction->rhs.kind == IR_OPERAND_INT &&
         instruction->rhs.int_value == bound &&
         ir_bitset_same_operand(&instruction->lhs, value);
}

static int ir_ascii_range_branch(const IRInstruction *branch,
                                 const IRInstruction *compare) {
  return branch && compare && branch->op == IR_OP_BRANCH_ZERO &&
         branch->text && branch->lhs.kind == IR_OPERAND_TEMP &&
         branch->lhs.name && compare->dest.kind == IR_OPERAND_TEMP &&
         compare->dest.name &&
         ir_operand_names_match(&branch->lhs, &compare->dest);
}

static int ir_ascii_next(const IRFunction *function, size_t after,
                         size_t *index) {
  return after < function->instruction_count &&
         ir_find_next_non_nop(function, after, index);
}

static int ir_ascii_casefold_rewrite(IRFunction *function,
                                     const size_t index[14], int *changed) {
  IRInstruction *lo1 = &function->instructions[index[0]];
  IRInstruction *lo1_branch = &function->instructions[index[1]];
  IRInstruction *hi1 = &function->instructions[index[2]];
  IRInstruction *hi1_branch = &function->instructions[index[3]];
  IRInstruction *lo2 = &function->instructions[index[7]];
  IRInstruction *miss_label = &function->instructions[index[13]];
  IROperand value = ir_operand_copy(&lo1->lhs);
  IROperand fold = ir_operand_copy(&lo1->dest);
  IROperand offset = ir_operand_copy(&lo2->dest);
  IROperand guard = ir_operand_copy(&hi1->dest);
  char *or_text = mettle_strdup("|");
  char *sub_text = mettle_strdup("-");
  char *cmp_text = mettle_strdup("<=");
  char *miss = mettle_strdup(miss_label->text);

  if (!value.name || !fold.name || !offset.name || !guard.name || !or_text ||
      !sub_text || !cmp_text || !miss) {
    ir_operand_destroy(&value);
    ir_operand_destroy(&fold);
    ir_operand_destroy(&offset);
    ir_operand_destroy(&guard);
    mettle_free_string(or_text);
    mettle_free_string(sub_text);
    mettle_free_string(cmp_text);
    mettle_free_string(miss);
    return 0;
  }

  ir_instruction_make_nop(lo1);
  lo1->op = IR_OP_BINARY;
  lo1->text = or_text;
  lo1->dest = fold;
  lo1->lhs = value;
  lo1->rhs = ir_operand_int(32);

  ir_instruction_make_nop(lo1_branch);
  lo1_branch->op = IR_OP_BINARY;
  lo1_branch->text = sub_text;
  lo1_branch->dest = offset;
  lo1_branch->lhs = ir_operand_copy(&lo1->dest);
  lo1_branch->rhs = ir_operand_int(97);

  ir_instruction_make_nop(hi1);
  hi1->op = IR_OP_BINARY;
  hi1->text = cmp_text;
  hi1->dest = guard;
  hi1->lhs = ir_operand_copy(&lo1_branch->dest);
  hi1->rhs = ir_operand_int(25);
  hi1->is_unsigned = 1;

  ir_instruction_make_nop(hi1_branch);
  hi1_branch->op = IR_OP_BRANCH_ZERO;
  hi1_branch->lhs = ir_operand_copy(&hi1->dest);
  hi1_branch->text = miss;

  for (size_t i = index[5]; i <= index[12]; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

static size_t ir_label_reference_count(const IRFunction *function,
                                       const char *name) {
  size_t n = 0;
  if (!name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if ((in->op == IR_OP_JUMP || in->op == IR_OP_BRANCH_ZERO ||
         in->op == IR_OP_BRANCH_EQ) &&
        in->text && strcmp(in->text, name) == 0) {
      n++;
    }
  }
  return n;
}

static int ir_range_test_is_foldable(const IRInstruction *lo,
                                     const IRInstruction *hi) {
  if (lo->op != IR_OP_BINARY || hi->op != IR_OP_BINARY || lo->is_float ||
      hi->is_float || !lo->text || !hi->text) {
    return 0;
  }
  if (strcmp(lo->text, "<") != 0 || strcmp(hi->text, ">") != 0) {
    return 0;
  }
  if (lo->rhs.kind != IR_OPERAND_INT || hi->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  if (lo->is_unsigned || hi->is_unsigned) {
    return 0;
  }
  if (lo->rhs.int_value > hi->rhs.int_value) {
    return 0;
  }
  if ((unsigned long long)hi->rhs.int_value -
          (unsigned long long)lo->rhs.int_value >
      2147483646ULL) {
    return 0;
  }
  if (lo->dest.kind != IR_OPERAND_TEMP || hi->dest.kind != IR_OPERAND_TEMP) {
    return 0;
  }
  if (lo->lhs.kind != IR_OPERAND_TEMP && lo->lhs.kind != IR_OPERAND_SYMBOL) {
    return 0;
  }
  return ir_operand_equals(&lo->lhs, &hi->lhs);
}

int ir_fold_range_test_pass(IRFunction *function, int *changed) {
  IRTempUseMap uses;
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  if (!ir_temp_use_map_init(&uses)) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 1;
    }
  }
  for (size_t i = 0; i + 6 < function->instruction_count; i++) {
    size_t at[7];
    at[0] = i;
    if (function->instructions[at[0]].op != IR_OP_BINARY) {
      continue;
    }
    {
      int ok = 1;
      for (size_t k = 1; k < 7 && ok; k++) {
        ok = ir_ascii_next(function, at[k - 1] + 1, &at[k]);
      }
      if (!ok) {
        continue;
      }
    }
    {
      IRInstruction *lo = &function->instructions[at[0]];
      IRInstruction *lo_branch = &function->instructions[at[1]];
      IRInstruction *shortcut = &function->instructions[at[2]];
      IRInstruction *else_label = &function->instructions[at[3]];
      IRInstruction *hi = &function->instructions[at[4]];
      IRInstruction *hi_branch = &function->instructions[at[5]];
      IRInstruction *body_label = &function->instructions[at[6]];
      long long span;

      if (lo_branch->op != IR_OP_BRANCH_ZERO ||
          !ir_operand_is_temp_named(&lo_branch->lhs, lo->dest.name) ||
          !lo_branch->text) {
        continue;
      }
      if (shortcut->op != IR_OP_JUMP || !shortcut->text) {
        continue;
      }
      if (else_label->op != IR_OP_LABEL || !else_label->text ||
          strcmp(else_label->text, lo_branch->text) != 0) {
        continue;
      }
      if (hi_branch->op != IR_OP_BRANCH_ZERO ||
          !ir_operand_is_temp_named(&hi_branch->lhs, hi->dest.name) ||
          !hi_branch->text) {
        continue;
      }
      if (!body_label->text ||
          (body_label->op != IR_OP_LABEL && body_label->op != IR_OP_JUMP) ||
          strcmp(body_label->text, shortcut->text) != 0) {
        continue;
      }
      if (ir_label_reference_count(function, else_label->text) != 1) {
        continue;
      }
      if (!ir_range_test_is_foldable(lo, hi)) {
        continue;
      }
      if (ir_temp_use_map_get(&uses, lo->dest.name) != 1 ||
          ir_temp_use_map_get(&uses, hi->dest.name) != 1) {
        continue;
      }
      span = hi->rhs.int_value - lo->rhs.int_value;

      {
        long long low = lo->rhs.int_value;
        IROperand value = ir_operand_copy(&lo->lhs);
        char *sub_text = mettle_strdup("-");
        char *cmp_text = mettle_strdup(">");
        if (!sub_text || !cmp_text) {
          ir_operand_destroy(&value);
          mettle_free_string(sub_text);
          mettle_free_string(cmp_text);
          return 0;
        }
        mettle_free_string(lo->text);
        lo->text = sub_text;
        ir_operand_destroy(&lo->rhs);
        lo->rhs = ir_operand_int(low);

        mettle_free_string(hi->text);
        hi->text = cmp_text;
        ir_operand_destroy(&hi->lhs);
        hi->lhs = ir_operand_copy(&lo->dest);
        ir_operand_destroy(&hi->rhs);
        hi->rhs = ir_operand_int(span);
        hi->is_unsigned = 1;
        ir_operand_destroy(&value);
      }

      ir_instruction_make_nop(&function->instructions[at[1]]);
      ir_instruction_make_nop(&function->instructions[at[2]]);
      ir_instruction_make_nop(&function->instructions[at[3]]);
      if (changed) {
        *changed = 1;
      }
    }
  }
  ir_temp_use_map_destroy(&uses);
  return 1;
}
int ir_ascii_casefold_range_pass(IRFunction *function, int *changed) {
  IRTempUseMap uses;
  int local_changed = 0;
  if (!function || !ir_temp_use_map_init(&uses)) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 1;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    size_t at[14];
    IRInstruction *ins;
    int first_lowercase;
    at[0] = i;
    ins = &function->instructions[at[0]];
    if (ins->op != IR_OP_BINARY || !ins->text ||
        strcmp(ins->text, ">=") != 0 || ins->rhs.kind != IR_OPERAND_INT ||
        (ins->rhs.int_value != 65 && ins->rhs.int_value != 97) ||
        (ins->lhs.kind != IR_OPERAND_TEMP &&
         ins->lhs.kind != IR_OPERAND_SYMBOL) ||
        !ins->lhs.name) {
      continue;
    }
    first_lowercase = ins->rhs.int_value == 97;
    for (size_t k = 1; k < 14; k++) {
      if (!ir_ascii_next(function, at[k - 1] + 1, &at[k])) {
        at[0] = function->instruction_count;
        break;
      }
    }
    if (at[0] >= function->instruction_count) {
      continue;
    }

    IRInstruction *lo1 = &function->instructions[at[0]];
    IRInstruction *lo1_branch = &function->instructions[at[1]];
    IRInstruction *hi1 = &function->instructions[at[2]];
    IRInstruction *hi1_branch = &function->instructions[at[3]];
    IRInstruction *success1 = &function->instructions[at[4]];
    IRInstruction *hi1_fail = &function->instructions[at[5]];
    IRInstruction *lo1_fail = &function->instructions[at[6]];
    IRInstruction *lo2 = &function->instructions[at[7]];
    IRInstruction *lo2_branch = &function->instructions[at[8]];
    IRInstruction *hi2 = &function->instructions[at[9]];
    IRInstruction *hi2_branch = &function->instructions[at[10]];
    IRInstruction *success2 = &function->instructions[at[11]];
    IRInstruction *hi2_fail = &function->instructions[at[12]];
    IRInstruction *lo2_fail = &function->instructions[at[13]];
    long long lo1_bound = first_lowercase ? 97 : 65;
    long long hi1_bound = first_lowercase ? 122 : 90;
    long long lo2_bound = first_lowercase ? 65 : 97;
    long long hi2_bound = first_lowercase ? 90 : 122;

    if (!ir_ascii_range_compare(lo1, ">=", lo1_bound, &lo1->lhs) ||
        !ir_ascii_range_branch(lo1_branch, lo1) ||
        !ir_ascii_range_compare(hi1, "<=", hi1_bound, &lo1->lhs) ||
        !ir_ascii_range_branch(hi1_branch, hi1) ||
        success1->op != IR_OP_JUMP || !success1->text ||
        hi1_fail->op != IR_OP_LABEL || !hi1_fail->text ||
        strcmp(hi1_fail->text, hi1_branch->text) != 0 ||
        lo1_fail->op != IR_OP_LABEL || !lo1_fail->text ||
        strcmp(lo1_fail->text, lo1_branch->text) != 0 ||
        !ir_ascii_range_compare(lo2, ">=", lo2_bound, &lo1->lhs) ||
        !ir_ascii_range_branch(lo2_branch, lo2) ||
        !ir_ascii_range_compare(hi2, "<=", hi2_bound, &lo1->lhs) ||
        !ir_ascii_range_branch(hi2_branch, hi2) ||
        success2->op != IR_OP_JUMP || !success2->text ||
        strcmp(success1->text, success2->text) != 0 ||
        hi2_fail->op != IR_OP_LABEL || !hi2_fail->text ||
        strcmp(hi2_fail->text, hi2_branch->text) != 0 ||
        lo2_fail->op != IR_OP_LABEL || !lo2_fail->text ||
        strcmp(lo2_fail->text, lo2_branch->text) != 0 ||
        ir_temp_use_map_get(&uses, lo1->dest.name) != 1 ||
        ir_temp_use_map_get(&uses, hi1->dest.name) != 1 ||
        ir_temp_use_map_get(&uses, lo2->dest.name) != 1 ||
        ir_temp_use_map_get(&uses, hi2->dest.name) != 1 ||
        !ir_label_only_referenced_by(function, hi1_fail->text, at[3]) ||
        !ir_label_only_referenced_by(function, lo1_fail->text, at[1]) ||
        !ir_label_only_referenced_by(function, hi2_fail->text, at[10])) {
      continue;
    }

    if (!ir_ascii_casefold_rewrite(function, at, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 1;
    }
    local_changed = 1;
  }

  ir_temp_use_map_destroy(&uses);
  if (local_changed) {
    ir_function_clear_cfg(function);
  }
  return 1;
}

typedef struct {
  const char **names;
  size_t capacity;
} IRLabelRefSet;

static void ir_label_ref_set_destroy(IRLabelRefSet *set) {
  if (!set) {
    return;
  }
  free(set->names);
  set->names = NULL;
  set->capacity = 0;
}

static int ir_label_ref_set_build(const IRFunction *function,
                                  IRLabelRefSet *set) {
  size_t capacity = 16;
  size_t mask;

  set->names = NULL;
  set->capacity = 0;
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  while (capacity < function->instruction_count * 2) {
    capacity *= 2;
  }
  set->names = (const char **)calloc(capacity, sizeof(*set->names));
  if (!set->names) {
    return 0;
  }
  set->capacity = capacity;
  mask = capacity - 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    size_t slot;
    if (!instruction->text ||
        (instruction->op != IR_OP_JUMP &&
         instruction->op != IR_OP_BRANCH_ZERO &&
         instruction->op != IR_OP_BRANCH_EQ)) {
      continue;
    }
    slot = (size_t)mettle_fnv1a_hash(instruction->text) & mask;
    while (set->names[slot]) {
      if (strcmp(set->names[slot], instruction->text) == 0) {
        break;
      }
      slot = (slot + 1) & mask;
    }
    set->names[slot] = instruction->text;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    for (size_t j = 0; j < instruction->argument_count; j++) {
      const IROperand *operand =
          instruction->arguments ? &instruction->arguments[j] : NULL;
      if (!operand || operand->kind != IR_OPERAND_LABEL || !operand->name) {
        continue;
      }
      size_t slot = (size_t)mettle_fnv1a_hash(operand->name) & mask;
      while (set->names[slot]) {
        if (strcmp(set->names[slot], operand->name) == 0) {
          break;
        }
        slot = (slot + 1) & mask;
      }
      set->names[slot] = operand->name;
    }
  }
  return 1;
}

static int ir_label_ref_set_contains(const IRLabelRefSet *set,
                                     const char *name) {
  size_t mask;
  size_t slot;

  if (!set->capacity || !name) {
    return 0;
  }
  mask = set->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(name) & mask;
  while (set->names[slot]) {
    if (strcmp(set->names[slot], name) == 0) {
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

int ir_remove_unused_labels_pass(IRFunction *function, int *changed) {
  IRLabelRefSet referenced;

  if (!function) {
    return 0;
  }
  if (!ir_label_ref_set_build(function, &referenced)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        !ir_label_ref_set_contains(&referenced, instruction->text)) {
      ir_instruction_make_nop(instruction);
      if (changed) {
        *changed = 1;
      }
    }
  }

  ir_label_ref_set_destroy(&referenced);
  return 1;
}

int ir_eliminate_unreachable_straightline_pass(IRFunction *function,
                                                       int *changed) {
  if (!function) {
    return 0;
  }

  unsigned char *live = malloc(function->instruction_count + 1);
  if (!live) {
    return 0;
  }

  int reachable = 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      reachable = 1;
    }
    live[i] = (unsigned char)reachable;
    if (reachable &&
        (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN)) {
      reachable = 0;
    }
  }

  if (!ir_keep_declarations_still_named(function, live)) {
    free(live);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (!live[i] && instruction->op != IR_OP_NOP) {
      ir_instruction_make_nop(instruction);
      if (changed) {
        *changed = 1;
      }
    }
  }

  free(live);
  return 1;
}

int ir_operand_is_symbol_named(const IROperand *operand,
                                      const char *name) {
  return operand && operand->kind == IR_OPERAND_SYMBOL && operand->name &&
         name && operand->name[0] == name[0] &&
         strcmp(operand->name, name) == 0;
}

static int ir_temp_read_outside(const IRFunction *function, const char *temp,
                                size_t first, size_t last) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i >= first && i <= last) {
      continue;
    }
    if (ir_operand_is_temp_named(&ins->lhs, temp) ||
        ir_operand_is_temp_named(&ins->rhs, temp)) {
      return 1;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_temp_named(&ins->arguments[a], temp)) {
        return 1;
      }
    }
  }
  return 0;
}

static int ir_try_fuse_rotate_add_at(IRFunction *function, size_t index,
                                     int *changed) {
  if (!function || index + 3 >= function->instruction_count) {
    return 1;
  }

  IRInstruction *add_inst = &function->instructions[index];
  IRInstruction *assign_next = &function->instructions[index + 1];
  IRInstruction *assign_a = &function->instructions[index + 2];
  IRInstruction *assign_b = &function->instructions[index + 3];

  if (add_inst->op != IR_OP_BINARY || add_inst->is_float || add_inst->ast_ref ||
      !add_inst->text || strcmp(add_inst->text, "+") != 0 ||
      add_inst->dest.kind != IR_OPERAND_TEMP || !add_inst->dest.name ||
      add_inst->lhs.kind != IR_OPERAND_SYMBOL || !add_inst->lhs.name ||
      add_inst->rhs.kind != IR_OPERAND_SYMBOL || !add_inst->rhs.name) {
    return 1;
  }

  const char *sym_a = add_inst->lhs.name;
  const char *sym_b = add_inst->rhs.name;
  const char *temp_sum = add_inst->dest.name;

  if (assign_next->op != IR_OP_ASSIGN ||
      assign_next->dest.kind != IR_OPERAND_SYMBOL || !assign_next->dest.name ||
      assign_a->op != IR_OP_ASSIGN ||
      assign_a->dest.kind != IR_OPERAND_SYMBOL || !assign_a->dest.name ||
      assign_b->op != IR_OP_ASSIGN ||
      assign_b->dest.kind != IR_OPERAND_SYMBOL || !assign_b->dest.name) {
    return 1;
  }

  const char *sym_next = assign_next->dest.name;
  int next_from_temp =
      ir_operand_is_temp(&assign_next->lhs) &&
      strcmp(assign_next->lhs.name, temp_sum) == 0;
  if (!next_from_temp) {
    return 1;
  }

  if (!ir_operand_is_symbol_named(&assign_a->lhs, sym_b) ||
      !ir_operand_is_symbol_named(&assign_a->dest, sym_a)) {
    return 1;
  }

  int b_from_next =
      ir_operand_is_symbol_named(&assign_b->dest, sym_b) &&
      ir_operand_is_symbol_named(&assign_b->lhs, sym_next);
  int b_from_temp =
      ir_operand_is_symbol_named(&assign_b->dest, sym_b) &&
      ir_operand_is_temp(&assign_b->lhs) &&
      strcmp(assign_b->lhs.name, temp_sum) == 0;
  if (!b_from_next && !b_from_temp) {
    return 1;
  }
  if (strcmp(sym_a, sym_b) == 0 || strcmp(sym_next, sym_a) == 0 ||
      strcmp(sym_next, sym_b) == 0 ||
      ir_temp_read_outside(function, temp_sum, index + 1, index + 3)) {
    return 1;
  }

  IRInstruction fused = {0};
  fused.op = IR_OP_ROTATE_ADD;
  fused.location = add_inst->location;
  fused.dest = ir_operand_symbol(sym_next);
  fused.lhs = ir_operand_symbol(sym_a);
  fused.rhs = ir_operand_symbol(sym_b);
  fused.text = mettle_strdup("+");
  if (!fused.dest.name || !fused.lhs.name || !fused.rhs.name || !fused.text) {
    ir_operand_destroy(&fused.dest);
    ir_operand_destroy(&fused.lhs);
    ir_operand_destroy(&fused.rhs);
    mettle_free_string(fused.text);
    return 0;
  }

  ir_instruction_destroy_storage(add_inst);
  *add_inst = fused;
  ir_instruction_make_nop(assign_next);
  ir_instruction_make_nop(assign_a);
  ir_instruction_make_nop(assign_b);

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_fuse_rotate_add_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i + 3 < function->instruction_count; i++) {
    if (!ir_try_fuse_rotate_add_at(function, i, changed)) {
      return 0;
    }
    if (function->instructions[i].op == IR_OP_ROTATE_ADD) {
      i += 3;
    }
  }

  return 1;
}

int ir_strength_reduce_rotate_loops_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *inst = &function->instructions[i];
    if (inst->op != IR_OP_LABEL || !inst->text) {
      continue;
    }

    const char *loop_label = inst->text;
    size_t body_start = i + 1;
    size_t body_end = function->instruction_count;
    size_t jump_index = (size_t)-1;

    for (size_t j = body_start; j < function->instruction_count; j++) {
      IRInstruction *probe = &function->instructions[j];
      if (probe->op == IR_OP_JUMP && probe->text &&
          strcmp(probe->text, loop_label) == 0) {
        jump_index = j;
        body_end = j;
        break;
      }
      if (probe->op == IR_OP_LABEL) {
        break;
      }
    }

    if (jump_index == (size_t)-1) {
      continue;
    }

    int only_rotate_or_nop = 1;
    for (size_t j = body_start; j < body_end; j++) {
      IROpcode op = function->instructions[j].op;
      if (op == IR_OP_NOP || op == IR_OP_ROTATE_ADD) {
        continue;
      }
      if (op == IR_OP_BINARY || op == IR_OP_ASSIGN) {
        only_rotate_or_nop = 0;
        break;
      }
      if (op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ) {
        continue;
      }
      only_rotate_or_nop = 0;
      break;
    }

    if (!only_rotate_or_nop) {
      continue;
    }

    for (size_t j = body_start; j < body_end; j++) {
      if (!ir_try_fuse_rotate_add_at(function, j, changed)) {
        return 0;
      }
      if (function->instructions[j].op == IR_OP_ROTATE_ADD) {
        j += 3;
      }
    }
  }

  return 1;
}

static const int IR_HAS_SIDE_EFFECT[IR_OP_KIND_COUNT] = {
    [IR_OP_BARRIER] = 1,
    [IR_OP_ASYNC_COPY] = 1,
    [IR_OP_ASYNC_COMMIT] = 1,
    [IR_OP_ASYNC_WAIT] = 1,
    [IR_OP_TENSOR_TRANSFER] = 1,
    [IR_OP_GPU_LAUNCH] = 1,
    [IR_OP_TENSOR_MMA] = 1,
    [IR_OP_TENSOR_MATMUL] = 1,
    [IR_OP_TENSOR_EPILOGUE] = 1,
    [IR_OP_TENSOR_COMMIT] = 1,
    [IR_OP_TENSOR_SCALE_ROWS] = 1,
    [IR_OP_TILE] = 1,
    [IR_OP_STORE] = 1,
    [IR_OP_CALL] = 1,
    [IR_OP_CALL_INDIRECT] = 1,
    [IR_OP_MEMCPY_INLINE] = 1,
    [IR_OP_SIMD_COPY] = 1,
    [IR_OP_COUNT_WORD_STARTS] = 1,
    [IR_OP_SIMD_SUM_I32] = 1,
    [IR_OP_SIMD_SUM_U8] = 1,
    [IR_OP_SIMD_BYTE_MAP] = 1,
    [IR_OP_SIMD_FILL] = 1,
    [IR_OP_SIMD_MATMUL_N32] = 1,
    [IR_OP_SIMD_INSERTION_SORT_I32] = 1,
    [IR_OP_SIMD_DOT_I32] = 1,
    [IR_OP_SIMD_DOT_I8] = 1,
    [IR_OP_SIMD_SLP_MAC_I32] = 1,
    [IR_OP_SIMD_SLP_MAC_I8] = 1,
    [IR_OP_SIMD_SCALE_I32] = 1,
    [IR_OP_SIMD_CLAMP_I32] = 1,
    [IR_OP_SIMD_REVERSE_COPY_I32] = 1,
    [IR_OP_LOWER_BOUND_I32] = 1,
    [IR_OP_PREFIX_SUM_I32] = 1,
    [IR_OP_SIMD_MINMAX_I32] = 1,
    [IR_OP_SIMD_SUM_F64] = 1,
    [IR_OP_SIMD_SUM_F32] = 1,
    [IR_OP_SIMD_DOT_F64] = 1,
    [IR_OP_SIMD_DOT_F32] = 1,
    [IR_OP_SIMD_AFFINE_MAP_F64] = 1,
    [IR_OP_SIMD_AFFINE_MAP_F32] = 1,
    [IR_OP_SIMD_EXP_F32] = 1,
    [IR_OP_SIMD_SILU_F32] = 1,
    [IR_OP_SIMD_I2F_REDUCE_F64] = 1,
    [IR_OP_SIMD_VLOOP_F64] = 1,
    [IR_OP_SIMD_VLOOP_I32] = 1,
    [IR_OP_SIMD_FIND] = 1,
    [IR_OP_SIMD_LANES] = 1,
    [IR_OP_SIMD_OUTER_LANE_F64] = 1,
    [IR_OP_RETURN] = 1,
    [IR_OP_INLINE_ASM] = 1,
    [IR_OP_ASM_RESULT] = 1,
};

int ir_instruction_has_side_effect(const IRInstruction *instruction) {
  if (!instruction) {
    return 0;
  }

  if (instruction->is_volatile) {
    return 1;
  }

  return (unsigned)instruction->op < (unsigned)IR_OP_KIND_COUNT
             ? IR_HAS_SIDE_EFFECT[instruction->op]
             : 1;
}


int ir_operand_is_temp_named(const IROperand *operand,
                                    const char *name) {
  return operand && operand->kind == IR_OPERAND_TEMP && operand->name &&
         name && operand->name[0] == name[0] &&
         strcmp(operand->name, name) == 0;
}

int ir_operand_is_int_value(const IROperand *operand,
                                   long long value) {
  return operand && operand->kind == IR_OPERAND_INT &&
         operand->int_value == value;
}

static int ir_instruction_reads_symbol_operand(const IRInstruction *instruction,
                                               const char *symbol_name) {
  if (!instruction || !symbol_name) {
    return 0;
  }

  if (ir_operand_is_symbol_named(&instruction->lhs, symbol_name) ||
      ir_operand_is_symbol_named(&instruction->rhs, symbol_name)) {
    return 1;
  }

  for (size_t i = 0; i < instruction->argument_count; i++) {
    if (ir_operand_is_symbol_named(&instruction->arguments[i], symbol_name)) {
      return 1;
    }
  }

  return 0;
}

int ir_symbol_read_after(const IRFunction *function, size_t start_index,
                                const char *symbol_name) {
  if (!function || !symbol_name) {
    return 0;
  }

  for (size_t i = start_index; i < function->instruction_count; i++) {
    if (ir_instruction_reads_symbol_operand(&function->instructions[i],
                                            symbol_name)) {
      return 1;
    }
  }
  return 0;
}

int ir_symbol_live_after_loop(const IRFunction *function, size_t exit_index,
                              const char *symbol_name) {
  if (!function || !symbol_name) {
    return 0;
  }
  int labels_seen = 0;
  for (size_t i = exit_index; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ir_instruction_reads_symbol_operand(ins, symbol_name)) {
      return 1;
    }
    if (ins->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&ins->dest, symbol_name)) {
      return 0;
    }
    if (ins->op == IR_OP_RETURN) {
      return 0;
    }
    if (ins->op == IR_OP_LABEL) {
      if (labels_seen++ > 0) {
        return ir_symbol_read_after(function, i, symbol_name);
      }
      continue;
    }
    if (ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ) {
      return ir_symbol_read_after(function, i, symbol_name);
    }
  }
  return 0;
}

static int ir_symbol_read_count_after_until_write_or_control(
    const IRFunction *function, size_t start_index, const char *symbol_name,
    size_t *only_read_index_out) {
  int count = 0;

  if (only_read_index_out) {
    *only_read_index_out = (size_t)-1;
  }
  if (!function || !symbol_name) {
    return 0;
  }

  for (size_t i = start_index; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_JUMP) {
      return -1;
    }
    if (instruction->op == IR_OP_RETURN) {
      if (ir_instruction_reads_symbol_operand(instruction, symbol_name)) {
        count++;
        if (only_read_index_out) {
          *only_read_index_out = i;
        }
      }
      return count;
    }
    if (instruction->op == IR_OP_LABEL || instruction->op == IR_OP_BRANCH_ZERO ||
        instruction->op == IR_OP_BRANCH_EQ ||
        instruction->op == IR_OP_CALL || instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_STORE || instruction->op == IR_OP_INLINE_ASM) {
      return -1;
    }
    if (ir_instruction_reads_symbol_operand(instruction, symbol_name)) {
      count++;
      if (only_read_index_out) {
        *only_read_index_out = i;
      }
      if (count > 1) {
        return count;
      }
    }
    if (ir_instruction_writes_symbol(instruction) &&
        ir_operand_is_symbol_named(&instruction->dest, symbol_name)) {
      break;
    }
  }

  return count;
}

static int ir_instruction_replace_symbol_reads(IRInstruction *instruction,
                                               const char *symbol_name,
                                               const IROperand *replacement,
                                               int *changed) {
  IROperand *operands[2];
  if (!instruction || !symbol_name || !replacement) {
    return 0;
  }

  operands[0] = &instruction->lhs;
  operands[1] = &instruction->rhs;
  for (size_t i = 0; i < sizeof(operands) / sizeof(operands[0]); i++) {
    if (ir_operand_is_symbol_named(operands[i], symbol_name)) {
      IROperand cloned = ir_operand_none();
      if (!ir_operand_clone(replacement, &cloned)) {
        return 0;
      }
      ir_operand_destroy(operands[i]);
      *operands[i] = cloned;
      if (changed) {
        *changed = 1;
      }
    }
  }

  for (size_t i = 0; i < instruction->argument_count; i++) {
    if (ir_operand_is_symbol_named(&instruction->arguments[i], symbol_name)) {
      IROperand cloned = ir_operand_none();
      if (!ir_operand_clone(replacement, &cloned)) {
        return 0;
      }
      ir_operand_destroy(&instruction->arguments[i]);
      instruction->arguments[i] = cloned;
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

static char *ir_make_float_symbol_copy_temp_name(size_t instruction_index) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "__float_local_%zu", instruction_index);
  return mettle_strdup(buffer);
}

int ir_eliminate_single_use_float_symbol_copies_pass(IRFunction *function,
                                                            int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (!instruction->is_float ||
        instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
        !ir_instruction_is_trivially_dead_if_dest_unused(instruction)) {
      continue;
    }

    if (instruction->float_bits == 32) {
      continue;
    }

    if (ir_instruction_reads_symbol_operand(instruction, instruction->dest.name) ||
        ir_symbol_address_taken(function, instruction->dest.name)) {
      continue;
    }

    size_t only_read_index = (size_t)-1;
    if (ir_symbol_read_count_after_until_write_or_control(
            function, i + 1, instruction->dest.name, &only_read_index) != 1 ||
        only_read_index == (size_t)-1) {
      continue;
    }

    char *temp_name = ir_make_float_symbol_copy_temp_name(i);
    if (!temp_name) {
      return 0;
    }
    IROperand replacement = ir_operand_temp(temp_name);
    if (!ir_instruction_replace_symbol_reads(
            &function->instructions[only_read_index], instruction->dest.name,
            &replacement, changed)) {
      ir_operand_destroy(&replacement);
      free(temp_name);
      return 0;
    }
    ir_operand_destroy(&instruction->dest);
    instruction->dest = ir_operand_temp(temp_name);
    ir_operand_destroy(&replacement);
    free(temp_name);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}
