#include "ir_optimize_internal.h"

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
  int cast_size = 0;
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
      !ir_builtin_integer_type_info(cast->text, &cast_size, NULL) ||
      !ir_find_temp_producer_index_in_current_block(function, cast_index,
                                                    cast->lhs.name,
                                                    &producer_index)) {
    return 1;
  }

  producer = &function->instructions[producer_index];
  if (producer->op != IR_OP_CAST || producer->is_float || !producer->text ||
      !ir_builtin_integer_type_info(producer->text, &producer_size, NULL)) {
    return 1;
  }

  /* Compose integer cast chains by keeping only the narrower conversion.
   * Equal-width chains keep the later cast so signed/unsigned reinterpretation
   * at that width remains visible. A later dead-temp pass removes the first
   * cast, but nopping it here lets this same pass continue coalescing through
   * the new source. */
  composed_type = (cast_size <= producer_size) ? cast->text : producer->text;
  type_copy = mettle_strdup(composed_type);
  if (!type_copy || !ir_operand_clone(&producer->lhs, &source)) {
    free(type_copy);
    ir_operand_destroy(&source);
    return 0;
  }

  ir_operand_destroy(&cast->lhs);
  cast->lhs = source;
  free(cast->text);
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
  ir_instruction_make_nop(cast);

  if (changed) {
    *changed = 1;
  }
  return 1;
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
        strcmp(producer->dest.name, assign_instruction->lhs.name) != 0) {
      continue;
    }

    /* A float ASSIGN may encode an IEEE-754 width conversion (e.g. a float64
     * expression narrowed into a float32 destination on return/assignment).
     * Folding the producer's dest forward would drop that cvtsd2ss/cvtss2sd and
     * store the wrong half of the value. Only coalesce when no width change is
     * implied: the producer must itself be float at the same width as the
     * assign's target. */
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

  /* O(1) fast path: no surviving entry values this symbol, so the compaction
   * scan below would remove nothing. */
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
      /* Temp values may embed propagated symbols; conservatively keep. */
    }

    if (remove) {
      ir_temp_value_map_note_value_removed(map, &entry->value);
      free(entry->name);
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

static int ir_resolve_propagated_value(const IRTempValueMap *temp_map,
                                       const IRSymbolValueMap *symbol_map,
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
      return ir_resolve_propagated_value(temp_map, symbol_map, mapped, out,
                                         depth + 1);
    }
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && symbol_map) {
    const IROperand *mapped =
        ir_temp_value_map_lookup(symbol_map, operand->name);
    if (mapped) {
      return ir_resolve_propagated_value(temp_map, symbol_map, mapped, out,
                                         depth + 1);
    }
  }

  return ir_operand_clone(operand, out);
}

static int ir_try_propagate_operand(IRTempValueMap *temp_map,
                                    IRSymbolValueMap *symbol_map,
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
    if (!ir_resolve_propagated_value(temp_map, symbol_map, mapped, &resolved,
                                     0)) {
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
        ir_temp_value_map_lookup(symbol_map, operand->name);
    if (!mapped) {
      return 1;
    }

    IROperand resolved = ir_operand_none();
    if (!ir_resolve_propagated_value(temp_map, symbol_map, mapped, &resolved,
                                     0)) {
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

static int ir_propagate_instruction_operands(IRTempValueMap *temp_map,
                                             IRSymbolValueMap *symbol_map,
                                             IRInstruction *instruction,
                                             int *changed) {
  if (!instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_STORE:
    if (!ir_try_propagate_operand(temp_map, symbol_map, &instruction->dest,
                                  changed) ||
        !ir_try_propagate_operand(temp_map, symbol_map, &instruction->lhs,
                                  changed) ||
        !ir_try_propagate_operand(temp_map, symbol_map, &instruction->rhs,
                                  changed)) {
      return 0;
    }
    break;

  case IR_OP_ROTATE_ADD:
    break;

  case IR_OP_BRANCH_ZERO:
    if (!ir_try_propagate_operand(temp_map, NULL, &instruction->lhs, changed)) {
      return 0;
    }
    break;

  case IR_OP_BRANCH_EQ:
    if (!ir_try_propagate_operand(temp_map, NULL, &instruction->lhs, changed) ||
        !ir_try_propagate_operand(temp_map, NULL, &instruction->rhs, changed)) {
      return 0;
    }
    break;

  case IR_OP_ASSIGN:
    if (!ir_try_propagate_operand(temp_map, symbol_map, &instruction->lhs,
                                  changed)) {
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
    if (!ir_try_propagate_operand(temp_map, NULL, &instruction->lhs, changed) ||
        !ir_try_propagate_operand(temp_map, NULL, &instruction->rhs, changed)) {
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      if (!ir_try_propagate_operand(temp_map, symbol_map,
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

/* Record `i` as the last position reading each temp/symbol operand of the
 * instruction. dest is included: for stores it is an address read, and for
 * writes the entry gets invalidated anyway, so overcounting is harmless. */
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

/* Drop entries whose key is not read anywhere past `i`: they can never be
 * looked up again, but without pruning every label SNAPSHOT copies them --
 * which made this pass O(labels x live-entries), the dominant compile cost
 * on big inlined functions (4M cloned entries per iteration on a 4000-call
 * main). Pruning loses no soundness: it only removes facts. */
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
      free(entry->name);
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

/* Constant value of an operand under the current propagation maps, WITHOUT
 * rewriting the operand (a persistent rewrite would perturb the shape the
 * SIMD recognizers pattern-match downstream). */
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

/* Fold `dest = a op b` to `dest <- C` the moment the propagation maps prove
 * both operands constant, INSIDE the propagation walk. Recording the result
 * immediately lets an entire arithmetic chain over one variable
 * (`m = m ^ K; m = m * P; ...` - the shape inlining a keyed helper with
 * constant arguments produces) collapse in a single pass instead of one
 * step per fixpoint round. Operands are consulted, never rewritten, so
 * partially-constant expressions keep their recognizable shape.
 *
 * Width discipline (the canonical-homes contract): a temp computes at the
 * full 64-bit width; a typed local's result wraps to its declared width on
 * the write. So a TEMP dest folds as-is, a SYMBOL dest folds only when its
 * declared type is known, with the result narrowed accordingly, and
 * comparisons fold at operand width regardless of dest. Sign-sensitive ops
 * are skipped when the instruction is unsigned: the evaluator computes
 * signed semantics. */
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

  /* Result-width narrowing for typed symbol destinations. */
  int narrow_bits = 0; /* 0 = keep 64-bit */
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
    if (!type) {
      return; /* global or untracked local: width unknown, don't fold */
    }
    if (strcmp(type, "int64") == 0) {
      narrow_bits = 0;
    } else if (strcmp(type, "uint64") == 0) {
      narrow_bits = 0;
      narrow_unsigned = 1;
    } else if (strcmp(type, "int32") == 0) {
      narrow_bits = 32;
    } else if (strcmp(type, "int16") == 0) {
      narrow_bits = 16;
    } else if (strcmp(type, "int8") == 0) {
      narrow_bits = 8;
    } else if (strcmp(type, "uint32") == 0) {
      narrow_bits = 32;
      narrow_unsigned = 1;
    } else if (strcmp(type, "uint16") == 0) {
      narrow_bits = 16;
      narrow_unsigned = 1;
    } else if (strcmp(type, "uint8") == 0 || strcmp(type, "bool") == 0) {
      narrow_bits = 8;
      narrow_unsigned = 1;
    } else {
      return; /* pointer/float/struct-typed home: not an integer fold */
    }
  }
  (void)narrow_unsigned;

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
  ir_rewrite_to_assign_int(instruction, result, any_changed);
}

int ir_copy_and_constant_propagation_pass(IRFunction *function,
                                                 int *changed) {
  if (!function) {
    return 0;
  }

  IRTempValueMap map;
  IRSymbolValueMap symbol_map;
  if (!ir_temp_value_map_init(&map) || !ir_temp_value_map_init(&symbol_map)) {
    ir_temp_value_map_destroy(&map);
    ir_temp_value_map_destroy(&symbol_map);
    return 0;
  }

  IRLabelValueMap label_in;
  if (!ir_label_value_map_init(&label_in)) {
    ir_temp_value_map_destroy(&map);
    ir_temp_value_map_destroy(&symbol_map);
    return 0;
  }

  /* Address-taken symbol set for store invalidation, built once: this pass
   * never introduces ADDRESS_OF instructions, so it stays valid across every
   * iteration. (Per-store function rescans were a cubic term here.) Last-use
   * indexes (name -> last instruction position reading it) power the dead-
   * entry pruning at labels; rebuilt per iteration because propagation
   * rewrites operands. */
  IRTempValueMap addr_taken, temp_last_use, sym_last_use;
  if (!ir_temp_value_map_init(&addr_taken) ||
      !ir_temp_value_map_init(&temp_last_use) ||
      !ir_temp_value_map_init(&sym_last_use) ||
      !ir_addr_taken_set_build(function, &addr_taken)) {
    ir_label_value_map_destroy(&label_in);
    ir_temp_value_map_destroy(&map);
    ir_temp_value_map_destroy(&symbol_map);
    ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
    return 0;
  }

  int any_changed = 0;
  for (int iteration = 0; iteration < 8; iteration++) {
    int flow_changed = 0;
    ir_temp_value_map_clear(&map);
    ir_temp_value_map_clear(&symbol_map);
    ir_temp_value_map_clear(&temp_last_use);
    ir_temp_value_map_clear(&sym_last_use);
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (!ir_cp_note_operand_uses(&function->instructions[i], i,
                                   &temp_last_use, &sym_last_use)) {
        ir_label_value_map_destroy(&label_in);
        ir_temp_value_map_destroy(&map);
        ir_temp_value_map_destroy(&symbol_map);
        ir_temp_value_map_destroy(&addr_taken);
        ir_temp_value_map_destroy(&temp_last_use);
        ir_temp_value_map_destroy(&sym_last_use);
        return 0;
      }
    }

    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *instruction = &function->instructions[i];

      if (instruction->op == IR_OP_LABEL && instruction->text) {
        /* Entries nobody reads past this point would only bloat the label
         * snapshots below (every label clones the live map; unpruned, that
         * was O(labels x entries) -- the dominant compile cost on big
         * inlined functions). */
        ir_cp_prune_dead_entries(&map, &temp_last_use, i);
        ir_cp_prune_dead_entries(&symbol_map, &sym_last_use, i);

        /* The label is reachable from explicit jumps/branches *and* from
         * fall-through if the previous non-nop instruction is not a JUMP or
         * RETURN. Merge the fall-through map into label_in[L] first so the
         * load below is the intersection of every incoming flow. Without
         * this, a label after two writes "x <- 1 / jump L / x <- 0 / L:"
         * would inherit only the jump's map and wrongly conclude x == 1. */
        int fall_through = 1;
        for (size_t pi = i; pi > 0;) {
          pi--;
          IROpcode prev_op = function->instructions[pi].op;
          if (prev_op == IR_OP_NOP) {
            continue;
          }
          if (prev_op == IR_OP_JUMP || prev_op == IR_OP_RETURN) {
            fall_through = 0;
          }
          break;
        }
        if (i == 0) {
          fall_through = 0; /* first instruction has no predecessor */
        }

        if (fall_through) {
          if (!ir_label_value_map_merge_incoming(&label_in, instruction->text,
                                                 &map, &flow_changed)) {
            ir_label_value_map_destroy(&label_in);
            ir_temp_value_map_destroy(&map);
            ir_temp_value_map_destroy(&symbol_map);
            ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
            return 0;
          }
        }

        const IRLabelValueEntry *entry =
            ir_label_value_map_lookup(&label_in, instruction->text);
        if (entry && entry->initialized) {
          if (!ir_temp_value_map_clone(&map, &entry->in_map)) {
            ir_label_value_map_destroy(&label_in);
            ir_temp_value_map_destroy(&map);
            ir_temp_value_map_destroy(&symbol_map);
            ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
            return 0;
          }
        } else {
          ir_temp_value_map_clear(&map);
        }
        ir_temp_value_map_clear(&symbol_map);
      }

      if (!ir_propagate_instruction_operands(&map, &symbol_map, instruction,
                                             &any_changed)) {
        ir_label_value_map_destroy(&label_in);
        ir_temp_value_map_destroy(&map);
        ir_temp_value_map_destroy(&symbol_map);
        ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
        return 0;
      }

      ir_cp_fold_constant_binary(function, &map, &symbol_map, instruction,
                                 &any_changed);

      if (ir_instruction_writes_temp(instruction) && instruction->dest.name) {
        ir_temp_value_map_remove(&map, instruction->dest.name);

        if (instruction->op == IR_OP_ASSIGN &&
            ir_operand_is_propagatable_value(&instruction->lhs)) {
          if (instruction->lhs.kind == IR_OPERAND_TEMP &&
              instruction->lhs.name &&
              strcmp(instruction->lhs.name, instruction->dest.name) == 0) {
            ir_temp_value_map_remove(&map, instruction->dest.name);
          } else if (!ir_temp_value_map_set(&map, instruction->dest.name,
                                            &instruction->lhs)) {
            ir_label_value_map_destroy(&label_in);
            ir_temp_value_map_destroy(&map);
            ir_temp_value_map_destroy(&symbol_map);
            ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
            return 0;
          }
        }
      }

      if (ir_instruction_writes_symbol(instruction) && instruction->dest.name) {
        ir_temp_value_map_remove_symbol_values(&map, instruction->dest.name);
        ir_symbol_value_map_invalidate_name(&symbol_map,
                                            instruction->dest.name);

        if (instruction->op == IR_OP_ASSIGN &&
            ir_operand_is_propagatable_value(&instruction->lhs) &&
            instruction->dest.kind == IR_OPERAND_SYMBOL) {
          if (instruction->lhs.kind == IR_OPERAND_SYMBOL &&
              instruction->lhs.name &&
              strcmp(instruction->lhs.name, instruction->dest.name) == 0) {
            ir_symbol_value_map_invalidate_name(&symbol_map,
                                                instruction->dest.name);
          } else if (!ir_temp_value_map_set(&symbol_map, instruction->dest.name,
                                            &instruction->lhs)) {
            ir_label_value_map_destroy(&label_in);
            ir_temp_value_map_destroy(&map);
            ir_temp_value_map_destroy(&symbol_map);
            ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
            return 0;
          }
        }
      }

      if (instruction->op == IR_OP_ROTATE_ADD && instruction->dest.name) {
        ir_symbol_value_map_invalidate_name(&symbol_map, instruction->dest.name);
        if (instruction->lhs.name) {
          ir_symbol_value_map_invalidate_name(&symbol_map, instruction->lhs.name);
        }
        if (instruction->rhs.name) {
          ir_symbol_value_map_invalidate_name(&symbol_map, instruction->rhs.name);
        }
      }

      if (instruction->op == IR_OP_STORE) {
        ir_temp_value_map_invalidate_after_store(&map, &addr_taken);
        ir_temp_value_map_invalidate_after_store(&symbol_map, &addr_taken);
      } else if (instruction->op == IR_OP_CALL ||
                 instruction->op == IR_OP_CALL_INDIRECT ||
                 instruction->op == IR_OP_INLINE_ASM) {
        ir_temp_value_map_remove_symbol_values(&map, NULL);
        ir_temp_value_map_clear(&symbol_map);
      }


      if ((instruction->op == IR_OP_JUMP || instruction->op == IR_OP_BRANCH_ZERO ||
           instruction->op == IR_OP_BRANCH_EQ) &&
          instruction->text) {
        /* Same reasoning as the label prune: don't snapshot dead entries. */
        ir_cp_prune_dead_entries(&map, &temp_last_use, i);
        if (!ir_label_value_map_merge_incoming(&label_in, instruction->text,
                                               &map, &flow_changed)) {
          ir_label_value_map_destroy(&label_in);
          ir_temp_value_map_destroy(&map);
          ir_temp_value_map_destroy(&symbol_map);
          ir_temp_value_map_destroy(&addr_taken);
    ir_temp_value_map_destroy(&temp_last_use);
    ir_temp_value_map_destroy(&sym_last_use);
          return 0;
        }
      }

      if (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN) {
        ir_temp_value_map_clear(&map);
        ir_temp_value_map_clear(&symbol_map);
      }
    }

    if (!flow_changed) {
      break;
    }
  }

  if (changed && any_changed) {
    *changed = 1;
  }

  ir_label_value_map_destroy(&label_in);
  ir_temp_value_map_destroy(&map);
  ir_temp_value_map_destroy(&symbol_map);
  ir_temp_value_map_destroy(&addr_taken);
  ir_temp_value_map_destroy(&temp_last_use);
  ir_temp_value_map_destroy(&sym_last_use);
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
    if (rhs == 0 || (lhs == LLONG_MIN && rhs == -1)) {
      *folded = 0;
    } else {
      *result = lhs / rhs;
    }
  } else if (strcmp(op, "%") == 0) {
    if (rhs == 0 || (lhs == LLONG_MIN && rhs == -1)) {
      *folded = 0;
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

static int ir_try_get_positive_pow2_shift(long long value, long long *shift) {
  if (!shift || value <= 0) {
    return 0;
  }

  unsigned long long u = (unsigned long long)value;
  if ((u & (u - 1ull)) != 0ull) {
    return 0;
  }

  long long amount = 0;
  while (u > 1ull) {
    u >>= 1u;
    amount++;
  }
  *shift = amount;
  return 1;
}

static int ir_try_fold_integer_binary(IRInstruction *instruction, int *changed) {
  if (!instruction || instruction->op != IR_OP_BINARY || instruction->is_float ||
      !instruction->text) {
    return 1;
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
      return ir_rewrite_to_assign_int(instruction, result, changed);
    }
  }

  /* Every algebraic identity now lives in the declarative table in
   * ir_optimize_rewrite.c; add a rule there to teach a new one. */
  return ir_rewrite_apply_binary_identities(instruction, changed);
}

static int ir_find_temp_producer_in_block(const IRFunction *function,
                                          size_t before_index,
                                          const char *temp_name,
                                          size_t *producer_index_out) {
  if (!function || !temp_name || !producer_index_out ||
      before_index > function->instruction_count) {
    return 0;
  }

  size_t i = before_index;
  while (i > 0) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (instruction->op == IR_OP_LABEL) {
      break;
    }
    if (ir_instruction_writes_temp(instruction) && instruction->dest.name &&
        strcmp(instruction->dest.name, temp_name) == 0) {
      *producer_index_out = i;
      return 1;
    }
  }

  return 0;
}

static int ir_try_rewrite_mod_pow2_compare_zero(IRFunction *function,
                                                const IRTempUseMap *uses,
                                                size_t compare_index,
                                                int *changed) {
  if (!function || !uses || compare_index >= function->instruction_count) {
    return 0;
  }

  IRInstruction *compare = &function->instructions[compare_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text) {
    return 1;
  }
  if (strcmp(compare->text, "==") != 0 && strcmp(compare->text, "!=") != 0) {
    return 1;
  }

  const IROperand *temp_operand = NULL;
  if (compare->lhs.kind == IR_OPERAND_TEMP && compare->lhs.name &&
      compare->rhs.kind == IR_OPERAND_INT && compare->rhs.int_value == 0) {
    temp_operand = &compare->lhs;
  } else if (compare->rhs.kind == IR_OPERAND_TEMP && compare->rhs.name &&
             compare->lhs.kind == IR_OPERAND_INT &&
             compare->lhs.int_value == 0) {
    temp_operand = &compare->rhs;
  } else {
    return 1;
  }

  if (ir_temp_use_map_get(uses, temp_operand->name) != 1) {
    return 1;
  }

  size_t producer_index = 0;
  if (!ir_find_temp_producer_in_block(function, compare_index,
                                      temp_operand->name, &producer_index)) {
    return 1;
  }

  IRInstruction *producer = &function->instructions[producer_index];
  if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text ||
      strcmp(producer->text, "%") != 0 ||
      producer->rhs.kind != IR_OPERAND_INT) {
    return 1;
  }

  long long shift = 0;
  if (!ir_try_get_positive_pow2_shift(producer->rhs.int_value, &shift) ||
      shift <= 0 || shift >= 63) {
    return 1;
  }

  long long mask = ((long long)1 << shift) - 1;
  producer->rhs.int_value = mask;
  free(producer->text);
  producer->text = mettle_strdup("&");
  if (!producer->text) {
    return 0;
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_mod_even_bitcheck_pass(IRFunction *function, int *changed) {
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
    if (!ir_try_rewrite_mod_pow2_compare_zero(function, &uses, i, changed)) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
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
        !branch->lhs.name || strcmp(branch->lhs.name, cmp->dest.name) != 0) {
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
  free(branch_zero->text);
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
        strcmp(branch->lhs.name, producer->dest.name) != 0 || !branch->text) {
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

  if (has_mod && (has_eq || has_ne)) {
    if (!ir_mod_even_bitcheck_pass(function, changed)) {
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

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (!ir_try_fold_integer_binary(instruction, changed) ||
        !ir_try_fold_integer_unary(instruction, changed) ||
        !ir_simplify_redundant_assign(instruction, changed) ||
        !ir_simplify_branch(instruction, changed)) {
      return 0;
    }
  }

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

int ir_thread_jump_targets_pass(IRFunction *function, int *changed) {
  if (!function) {
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
      if (!ir_find_label_index(function, current_target, &label_index)) {
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
        return 0;
      }
      free(instruction->text);
      instruction->text = target_copy;
      if (changed) {
        *changed = 1;
      }
    }
  }

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
      /* `branch -> L; jump L` looks redundant (both paths reach L), but only if
       * the `jump` is genuinely this branch's diamond-closer -- i.e. the very
       * next non-nop instruction is `label L` itself. If some OTHER label
       * intervenes (e.g. this is an empty nested then-arm immediately followed
       * by an enclosing if's else-entry label), the `jump` is the then-arm's
       * skip-over-the-else and removing it makes the else run unconditionally
       * (silent miscompile). Guard against that. */
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

static int ir_label_is_referenced(const IRFunction *function,
                                  const char *label) {
  if (!function || !label) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (ir_instruction_references_label(&function->instructions[i], label)) {
      return 1;
    }
  }

  return 0;
}

int ir_remove_unused_labels_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        !ir_label_is_referenced(function, instruction->text)) {
      ir_instruction_make_nop(instruction);
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

int ir_eliminate_unreachable_straightline_pass(IRFunction *function,
                                                       int *changed) {
  if (!function) {
    return 0;
  }

  int reachable = 1;

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_LABEL) {
      reachable = 1;
      continue;
    }

    if (!reachable) {
      if (instruction->op != IR_OP_NOP) {
        ir_instruction_make_nop(instruction);
        if (changed) {
          *changed = 1;
        }
      }
      continue;
    }

    if (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN) {
      reachable = 0;
    }
  }

  return 1;
}

int ir_operand_is_symbol_named(const IROperand *operand,
                                      const char *name) {
  return operand && operand->kind == IR_OPERAND_SYMBOL && operand->name &&
         name && strcmp(operand->name, name) == 0;
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
      assign_next->lhs.kind == IR_OPERAND_TEMP && assign_next->lhs.name &&
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
      assign_b->lhs.kind == IR_OPERAND_TEMP && assign_b->lhs.name &&
      strcmp(assign_b->lhs.name, temp_sum) == 0;
  if (!b_from_next && !b_from_temp) {
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
    free(fused.text);
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

int ir_instruction_has_side_effect(const IRInstruction *instruction) {
  if (!instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_STORE:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
  case IR_OP_SIMD_BYTE_MAP:
  case IR_OP_SIMD_FILL:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
  case IR_OP_SIMD_DOT_I8:
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32:
  case IR_OP_LOWER_BOUND_I32:
  case IR_OP_PREFIX_SUM_I32:
  case IR_OP_SIMD_MINMAX_I32:
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32:
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32:
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_FIND:
  case IR_OP_SIMD_OUTER_LANE_F64:
  case IR_OP_RETURN:
  case IR_OP_INLINE_ASM:
    return 1;
  default:
    return 0;
  }
}

int ir_operand_is_temp_named(const IROperand *operand,
                                    const char *name) {
  return operand && operand->kind == IR_OPERAND_TEMP && operand->name &&
         name && strcmp(operand->name, name) == 0;
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

/* Liveness of a loop's induction variable PAST the loop, for the SIMD
 * recognizers (the fused kernels drop the iv). `exit_index` is the first
 * instruction after the back jump -- usually the loop's own exit label.
 * Unlike ir_symbol_read_after, a full redefinition (`i <- 0` starting the
 * NEXT loop -- iv reuse is everywhere in real code) kills the value: later
 * reads see the new definition, not the loop's final value. The scan is
 * conservative: it trusts a redefinition only while control flow is still
 * straight-line from the exit (one leading label allowed -- the exit label
 * itself); any further label/branch/jump means other paths could observe
 * the old value, and the answer falls back to "live". */
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
      return 0; /* fully redefined before any read: the old value is dead */
    }
    if (ins->op == IR_OP_RETURN) {
      return 0;
    }
    if (ins->op == IR_OP_LABEL) {
      if (labels_seen++ > 0) {
        /* A join: another path may enter here and read the old value via
         * code we will not scan in order. Fall back to the whole-function
         * read scan. */
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
    if (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN) {
      return count;
    }
    if (instruction->op == IR_OP_LABEL || instruction->op == IR_OP_BRANCH_ZERO ||
        instruction->op == IR_OP_BRANCH_EQ ||
        instruction->op == IR_OP_CALL || instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_STORE || instruction->op == IR_OP_INLINE_ASM) {
      return -1;
    }
    if (ir_instruction_writes_symbol(instruction) &&
        ir_operand_is_symbol_named(&instruction->dest, symbol_name)) {
      break;
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
