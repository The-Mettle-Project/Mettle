#include "ir_optimize_internal.h"

static int ir_symbol_zero_initialized_before(const IRFunction *function,
                                             size_t before_index,
                                             const char *symbol_name) {
  if (!function || !symbol_name) {
    return 0;
  }

  for (size_t i = before_index; i > 0;) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      break;
    }
    if (instruction->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&instruction->dest, symbol_name)) {
      return ir_operand_is_int_value(&instruction->lhs, 0);
    }
    if (ir_instruction_writes_destination(instruction) &&
        ir_operand_is_symbol_named(&instruction->dest, symbol_name)) {
      return 0;
    }
  }

  return 0;
}

static int ir_try_match_word_count_load(const IRFunction *function,
                                        size_t body_start, size_t body_end,
                                        const char *iv_symbol,
                                        const char **buf_symbol_out,
                                        const char **char_symbol_out) {
  if (!function || !iv_symbol || !buf_symbol_out || !char_symbol_out) {
    return 0;
  }

  for (size_t i = body_start; i + 2 < body_end; i++) {
    const IRInstruction *addr = &function->instructions[i];
    if (addr->op == IR_OP_NOP) {
      continue;
    }
    if (addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
        strcmp(addr->text, "+") != 0 ||
        addr->dest.kind != IR_OPERAND_TEMP || !addr->dest.name) {
      continue;
    }

    const char *buf_symbol = NULL;
    if (addr->lhs.kind == IR_OPERAND_SYMBOL && addr->lhs.name &&
        ir_operand_is_symbol_named(&addr->rhs, iv_symbol)) {
      buf_symbol = addr->lhs.name;
    } else if (addr->rhs.kind == IR_OPERAND_SYMBOL && addr->rhs.name &&
               ir_operand_is_symbol_named(&addr->lhs, iv_symbol)) {
      buf_symbol = addr->rhs.name;
    } else {
      continue;
    }

    size_t load_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &load_index) ||
        load_index >= body_end) {
      return 0;
    }

    const IRInstruction *load = &function->instructions[load_index];
    if (load->op != IR_OP_LOAD ||
        !ir_operand_is_temp_named(&load->lhs, addr->dest.name) ||
        load->rhs.kind != IR_OPERAND_INT || load->rhs.int_value != 1) {
      continue;
    }

    /* Direct-to-symbol byte load: `@c <- *addr [1]`. Copy propagation +
     * eliminate_load_symbol_copy fold the old load-to-temp-then-cast into a
     * single load whose destination is the char symbol itself. */
    if (load->dest.kind == IR_OPERAND_SYMBOL && load->dest.name) {
      *buf_symbol_out = buf_symbol;
      *char_symbol_out = load->dest.name;
      return 1;
    }

    /* Legacy shape: `%t <- *addr [1]; @c = (uint8)%t` (load to temp, then a
     * cast into the char symbol). */
    if (load->dest.kind != IR_OPERAND_TEMP || !load->dest.name) {
      continue;
    }
    size_t cast_index = 0;
    if (!ir_find_next_non_nop_in_block(function, load_index + 1,
                                       &cast_index) ||
        cast_index >= body_end) {
      return 0;
    }

    const IRInstruction *cast = &function->instructions[cast_index];
    if (cast->op == IR_OP_CAST &&
        ir_operand_is_temp_named(&cast->lhs, load->dest.name) &&
        cast->dest.kind == IR_OPERAND_SYMBOL && cast->dest.name) {
      *buf_symbol_out = buf_symbol;
      *char_symbol_out = cast->dest.name;
      return 1;
    }
  }

  return 0;
}

static int ir_try_match_word_count_whitespace_chain(
    const IRFunction *function, size_t body_start, size_t body_end,
    const char *char_symbol, const char **whitespace_label_out) {
  if (!function || !char_symbol || !whitespace_label_out) {
    return 0;
  }

  int seen_space = 0;
  int seen_tab = 0;
  int seen_lf = 0;
  int seen_cr = 0;
  const char *ws_label = NULL;

  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    long long value = 0;
    const char *target = NULL;

    if (instruction->op == IR_OP_BRANCH_EQ &&
        ir_operand_is_symbol_named(&instruction->lhs, char_symbol) &&
        instruction->rhs.kind == IR_OPERAND_INT && instruction->text) {
      value = instruction->rhs.int_value;
      target = instruction->text;
    } else if (instruction->op == IR_OP_BINARY && !instruction->is_float &&
               instruction->text && strcmp(instruction->text, "==") == 0 &&
               ir_operand_is_symbol_named(&instruction->lhs, char_symbol) &&
               instruction->rhs.kind == IR_OPERAND_INT &&
               instruction->dest.kind == IR_OPERAND_TEMP &&
               instruction->dest.name) {
      size_t branch_index = 0;
      if (!ir_find_next_non_nop_in_block(function, i + 1, &branch_index) ||
          branch_index >= body_end) {
        continue;
      }
      const IRInstruction *branch = &function->instructions[branch_index];
      if (branch->op != IR_OP_BRANCH_ZERO ||
          !ir_operand_is_temp_named(&branch->lhs, instruction->dest.name) ||
          !branch->text) {
        continue;
      }
      value = instruction->rhs.int_value;
      target = NULL;
      for (size_t label_index = branch_index + 1; label_index < body_end;
           label_index++) {
        const IRInstruction *label = &function->instructions[label_index];
        if (label->op == IR_OP_NOP) {
          continue;
        }
        if (label->op == IR_OP_LABEL && label->text &&
            strcmp(label->text, branch->text) != 0) {
          target = label->text;
        }
        break;
      }
      if (!target) {
        continue;
      }
    } else {
      continue;
    }

    if (value != 32 && value != 9 && value != 10 && value != 13) {
      continue;
    }

    if (!ws_label) {
      ws_label = target;
    } else if (!target || strcmp(ws_label, target) != 0) {
      return 0;
    }

    if (value == 32) seen_space = 1;
    if (value == 9) seen_tab = 1;
    if (value == 10) seen_lf = 1;
    if (value == 13) seen_cr = 1;
  }

  if (seen_space && seen_tab && seen_lf && seen_cr && ws_label) {
    *whitespace_label_out = ws_label;
    return 1;
  }

  return 0;
}

static int ir_try_match_word_count_state_updates(
    const IRFunction *function, size_t body_start, size_t body_end,
    const char *whitespace_label, const char **count_symbol_out,
    const char **in_word_symbol_out) {
  if (!function || !whitespace_label || !count_symbol_out ||
      !in_word_symbol_out) {
    return 0;
  }

  const char *in_word = NULL;
  const char *count = NULL;

  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *label = &function->instructions[i];
    if (label->op != IR_OP_LABEL || !label->text ||
        strcmp(label->text, whitespace_label) != 0) {
      continue;
    }

    size_t assign_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &assign_index) ||
        assign_index >= body_end) {
      return 0;
    }
    const IRInstruction *assign = &function->instructions[assign_index];
    if (assign->op != IR_OP_ASSIGN ||
        assign->dest.kind != IR_OPERAND_SYMBOL || !assign->dest.name ||
        !ir_operand_is_int_value(&assign->lhs, 0)) {
      return 0;
    }
    in_word = assign->dest.name;
    break;
  }

  if (!in_word) {
    return 0;
  }

  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *test = &function->instructions[i];
    if (test->op != IR_OP_BINARY || test->is_float || !test->text ||
        strcmp(test->text, "==") != 0 ||
        !ir_operand_is_symbol_named(&test->lhs, in_word) ||
        !ir_operand_is_int_value(&test->rhs, 0) ||
        test->dest.kind != IR_OPERAND_TEMP || !test->dest.name) {
      continue;
    }

    size_t branch_index = 0;
    if (!ir_find_next_non_nop_in_block(function, i + 1, &branch_index) ||
        branch_index >= body_end) {
      return 0;
    }
    const IRInstruction *branch = &function->instructions[branch_index];
    if (branch->op != IR_OP_BRANCH_ZERO ||
        !ir_operand_is_temp_named(&branch->lhs, test->dest.name)) {
      continue;
    }

    size_t add_index = 0;
    if (!ir_find_next_non_nop_in_block(function, branch_index + 1,
                                       &add_index) ||
        add_index >= body_end) {
      return 0;
    }
    const IRInstruction *add = &function->instructions[add_index];
    if (add->op == IR_OP_BINARY && !add->is_float && add->text &&
        strcmp(add->text, "+") == 0 &&
        add->dest.kind == IR_OPERAND_SYMBOL && add->dest.name &&
        ir_operand_is_symbol_named(&add->lhs, add->dest.name) &&
        ir_operand_is_int_value(&add->rhs, 1)) {
      count = add->dest.name;
    }
    break;
  }

  if (!count) {
    return 0;
  }

  int saw_set_one = 0;
  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&instruction->dest, in_word) &&
        ir_operand_is_int_value(&instruction->lhs, 1)) {
      saw_set_one = 1;
      break;
    }
  }

  if (!saw_set_one) {
    return 0;
  }

  *count_symbol_out = count;
  *in_word_symbol_out = in_word;
  return 1;
}

int ir_try_parse_direct_unit_increment(const IRInstruction *instruction,
                                              const char *iv_symbol) {
  return instruction && instruction->op == IR_OP_BINARY &&
         !instruction->is_float && instruction->text &&
         strcmp(instruction->text, "+") == 0 &&
         ir_operand_is_symbol_named(&instruction->dest, iv_symbol) &&
         ir_operand_is_symbol_named(&instruction->lhs, iv_symbol) &&
         ir_operand_is_int_value(&instruction->rhs, 1);
}

static int ir_try_vectorize_word_count_at(IRFunction *function,
                                          size_t header_index,
                                          int *changed) {
  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }

  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !header->text) {
    return 1;
  }

  size_t compare_index = 0;
  size_t branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }

  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }

  const char *iv_symbol = compare->lhs.name;
  const IROperand *len_operand = &compare->rhs;
  const char *loop_label = header->text;
  const char *exit_label = branch->text;

  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_JUMP && instruction->text &&
        strcmp(instruction->text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, exit_label) == 0) {
      break;
    }
  }

  if (jump_index == (size_t)-1) {
    return 1;
  }

  size_t increment_index = jump_index;
  while (increment_index > branch_index + 1) {
    increment_index--;
    if (function->instructions[increment_index].op != IR_OP_NOP) {
      break;
    }
  }
  if (!ir_try_parse_direct_unit_increment(&function->instructions[increment_index],
                                          iv_symbol)) {
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    if (ir_instruction_has_side_effect(&function->instructions[i])) {
      return 1;
    }
  }

  const char *buf_symbol = NULL;
  const char *char_symbol = NULL;
  const char *ws_label = NULL;
  const char *count_symbol = NULL;
  const char *in_word_symbol = NULL;

  if (!ir_try_match_word_count_load(function, branch_index + 1, jump_index,
                                    iv_symbol, &buf_symbol, &char_symbol) ||
      !ir_try_match_word_count_whitespace_chain(function, branch_index + 1,
                                                jump_index, char_symbol,
                                                &ws_label) ||
      !ir_try_match_word_count_state_updates(function, branch_index + 1,
                                             jump_index, ws_label,
                                             &count_symbol,
                                             &in_word_symbol)) {
    return 1;
  }

  if (!ir_symbol_zero_initialized_before(function, header_index, count_symbol) ||
      !ir_symbol_zero_initialized_before(function, header_index, iv_symbol) ||
      !ir_symbol_zero_initialized_before(function, header_index,
                                         in_word_symbol)) {
    return 1;
  }

  if (ir_symbol_read_after(function, jump_index + 1, iv_symbol) ||
      ir_symbol_read_after(function, jump_index + 1, in_word_symbol) ||
      ir_symbol_read_after(function, jump_index + 1, char_symbol)) {
    return 1;
  }

  IRInstruction fused = {0};
  fused.op = IR_OP_COUNT_WORD_STARTS;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(count_symbol);
  fused.lhs = ir_operand_symbol(buf_symbol);
  if (!ir_operand_clone(len_operand, &fused.rhs)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  if (!fused.dest.name || !fused.lhs.name ||
      fused.rhs.kind == IR_OPERAND_NONE) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }

  ir_instruction_destroy_storage(header);
  *header = fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_count_word_starts_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL) {
      continue;
    }
    if (!ir_try_vectorize_word_count_at(function, i, changed)) {
      return 0;
    }
    if (function->instructions[i].op == IR_OP_COUNT_WORD_STARTS) {
      break;
    }
  }

  return 1;
}

int ir_positive_loop_div2_to_shift_pass(IRFunction *function,
                                               int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t h = 0; h < function->instruction_count; h++) {
    IRInstruction *header = &function->instructions[h];
    if (header->op != IR_OP_LABEL || !header->text) {
      continue;
    }

    size_t compare_index = 0;
    size_t branch_index = 0;
    if (!ir_find_next_non_nop(function, h + 1, &compare_index) ||
        !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
      continue;
    }

    IRInstruction *compare = &function->instructions[compare_index];
    IRInstruction *branch = &function->instructions[branch_index];
    if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
        strcmp(compare->text, ">") != 0 ||
        compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
        !ir_operand_is_int_value(&compare->rhs, 1) ||
        compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
        branch->op != IR_OP_BRANCH_ZERO ||
        !ir_operand_is_temp_named(&branch->lhs, compare->dest.name)) {
      continue;
    }

    const char *positive_symbol = compare->lhs.name;
    size_t jump_index = (size_t)-1;
    for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
      IRInstruction *probe = &function->instructions[i];
      if (probe->op == IR_OP_JUMP && probe->text &&
          strcmp(probe->text, header->text) == 0) {
        jump_index = i;
        break;
      }
    }
    if (jump_index == (size_t)-1) {
      continue;
    }

    int symbol_written_before_div = 0;
    for (size_t i = branch_index + 1; i < jump_index; i++) {
      IRInstruction *instruction = &function->instructions[i];
      if (instruction->op == IR_OP_BINARY && !instruction->is_float &&
          instruction->text && strcmp(instruction->text, "/") == 0 &&
          ir_operand_is_symbol_named(&instruction->dest, positive_symbol) &&
          ir_operand_is_symbol_named(&instruction->lhs, positive_symbol) &&
          ir_operand_is_int_value(&instruction->rhs, 2) &&
          !symbol_written_before_div) {
        char *op = mettle_strdup(">>");
        if (!op) {
          return 0;
        }
        free(instruction->text);
        instruction->text = op;
        ir_operand_destroy(&instruction->rhs);
        instruction->rhs = ir_operand_int(1);
        if (changed) {
          *changed = 1;
        }
        continue;
      }

      if (ir_instruction_writes_destination(instruction) &&
          ir_operand_is_symbol_named(&instruction->dest, positive_symbol)) {
        symbol_written_before_div = 1;
      }
    }
  }

  return 1;
}

int ir_operand_resolve_symbol_int(const IRSymbolValueMap *symbol_map,
                                         const IROperand *operand,
                                         long long *out_value) {
  if (!operand || !out_value) {
    return 0;
  }

  if (operand->kind == IR_OPERAND_INT) {
    *out_value = operand->int_value;
    return 1;
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && symbol_map) {
    const IROperand *mapped =
        ir_temp_value_map_lookup(symbol_map, operand->name);
    if (mapped && mapped->kind == IR_OPERAND_INT) {
      *out_value = mapped->int_value;
      return 1;
    }
  }

  return 0;
}

int ir_build_symbol_int_map_before(const IRFunction *function,
                                          size_t before_index,
                                          IRSymbolValueMap *symbol_map) {
  if (!function || !symbol_map) {
    return 0;
  }

  ir_temp_value_map_clear(symbol_map);

  for (size_t i = 0; i < before_index && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }

    if (instruction->op == IR_OP_CALL || instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_STORE || instruction->op == IR_OP_INLINE_ASM) {
      ir_temp_value_map_clear(symbol_map);
      continue;
    }

    if (instruction->op == IR_OP_ASSIGN &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name) {
      if (instruction->lhs.kind == IR_OPERAND_INT) {
        if (!ir_temp_value_map_set(symbol_map, instruction->dest.name,
                                   &instruction->lhs)) {
          return 0;
        }
      } else if (instruction->lhs.kind == IR_OPERAND_SYMBOL &&
                 instruction->lhs.name) {
        const IROperand *mapped =
            ir_temp_value_map_lookup(symbol_map, instruction->lhs.name);
        if (mapped && mapped->kind == IR_OPERAND_INT) {
          if (!ir_temp_value_map_set(symbol_map, instruction->dest.name, mapped)) {
            return 0;
          }
        } else {
          ir_temp_value_map_remove(symbol_map, instruction->dest.name);
        }
      } else {
        ir_temp_value_map_remove(symbol_map, instruction->dest.name);
      }
    }

    if (instruction->op == IR_OP_ROTATE_ADD) {
      if (instruction->dest.name) {
        ir_temp_value_map_remove(symbol_map, instruction->dest.name);
      }
      if (instruction->lhs.name) {
        ir_temp_value_map_remove(symbol_map, instruction->lhs.name);
      }
      if (instruction->rhs.name) {
        ir_temp_value_map_remove(symbol_map, instruction->rhs.name);
      }
    }

    /* Any other instruction that writes a symbol destination produces a value
     * we are not tracking as a constant (BINARY, UNARY, LOAD, CAST, NEW, ...).
     * It MUST invalidate the symbol's stale constant, otherwise a later
     * mutation like `binary i = i + 4` leaves `i` recorded at its pre-loop
     * value and downstream consumers (e.g. the constant-bound loop unroller's
     * trip-count computation) read a counter value that is no longer correct.
     * ASSIGN and ROTATE_ADD are fully handled above; everything else is
     * conservatively dropped here. */
    if (instruction->op != IR_OP_ASSIGN &&
        instruction->op != IR_OP_ROTATE_ADD &&
        ir_instruction_writes_destination(instruction) &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name) {
      ir_temp_value_map_remove(symbol_map, instruction->dest.name);
    }
  }

  return 1;
}

int ir_loop_body_opcode_is_unroll_safe(IROpcode op) {
  switch (op) {
  case IR_OP_NOP:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ASSIGN:
  case IR_OP_CAST:
  case IR_OP_ROTATE_ADD:
  case IR_OP_DECLARE_LOCAL:
    return 1;
  default:
    return 0;
  }
}

int ir_find_last_writer_before(const IRFunction *function, size_t before_index,
                                      IROperandKind kind, const char *name,
                                      size_t *writer_index) {
  if (!function || !name || !writer_index || before_index == 0) {
    return 0;
  }

  for (size_t i = before_index; i > 0; ) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }

    if (ir_instruction_writes_destination(instruction) &&
        instruction->dest.kind == kind && instruction->dest.name &&
        strcmp(instruction->dest.name, name) == 0) {
      *writer_index = i;
      return 1;
    }
  }

  return 0;
}

