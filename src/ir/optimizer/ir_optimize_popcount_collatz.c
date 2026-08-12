#include "ir_optimize_internal.h"
#include "../../common.h" // mettle_free_string

#define IR_POPCOUNT_BYTE_UNROLL 8

static int ir_popcount_body_is_v_shift_step(const IRInstruction *instruction,
                                            const char *v_symbol) {
  if (!instruction || instruction->op != IR_OP_BINARY || instruction->is_float ||
      !instruction->text || !v_symbol) {
    return 0;
  }

  if (strcmp(instruction->text, "/") == 0 &&
      ir_operand_is_symbol_named(&instruction->lhs, v_symbol) &&
      ir_operand_is_int_value(&instruction->rhs, 2)) {
    return 1;
  }

  if (strcmp(instruction->text, ">>") == 0 &&
      ir_operand_is_symbol_named(&instruction->lhs, v_symbol) &&
      ir_operand_is_int_value(&instruction->rhs, 1)) {
    return 1;
  }

  return 0;
}

static int ir_popcount_body_matches(const IRFunction *function, size_t body_start,
                                    size_t body_end, const char *v_symbol,
                                    const char **count_symbol_out,
                                    int *use_int32_cast_out,
                                    int *use_uint8_cast_out) {
  if (!function || !v_symbol || !count_symbol_out || !use_int32_cast_out ||
      !use_uint8_cast_out || body_start >= body_end) {
    return 0;
  }

  int saw_and = 0;
  int saw_add = 0;
  int saw_shift = 0;
  const char *count_symbol = NULL;
  int use_int32_cast = 0;
  int use_uint8_cast = 0;

  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction || instruction->op == IR_OP_NOP) {
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        strcmp(instruction->text, "&") == 0 &&
        ir_operand_is_symbol_named(&instruction->lhs, v_symbol) &&
        ir_operand_is_int_value(&instruction->rhs, 1)) {
      saw_and = 1;
      continue;
    }

    if (instruction->op == IR_OP_CAST && instruction->text &&
        strstr(instruction->text, "int32") &&
        instruction->dest.kind == IR_OPERAND_TEMP) {
      use_int32_cast = 1;
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        strcmp(instruction->text, "+") == 0 &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name) {
      if (count_symbol && strcmp(count_symbol, instruction->dest.name) != 0) {
        return 0;
      }
      count_symbol = instruction->dest.name;
      if (ir_operand_is_symbol_named(&instruction->lhs, count_symbol) ||
          ir_operand_is_symbol_named(&instruction->rhs, count_symbol)) {
        saw_add = 1;
        continue;
      }
      return 0;
    }

    if (ir_popcount_body_is_v_shift_step(instruction, v_symbol)) {
      saw_shift = 1;
      continue;
    }

    if (instruction->op == IR_OP_BINARY && !instruction->is_float && instruction->text &&
        strcmp(instruction->text, ">>") == 0 &&
        ir_operand_is_symbol_named(&instruction->lhs, v_symbol) &&
        ir_operand_is_int_value(&instruction->rhs, 1) &&
        instruction->dest.kind == IR_OPERAND_TEMP) {
      saw_shift = 1;
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        strcmp(instruction->text, "/") == 0 &&
        ir_operand_is_symbol_named(&instruction->lhs, v_symbol) &&
        ir_operand_is_int_value(&instruction->rhs, 2) &&
        instruction->dest.kind == IR_OPERAND_TEMP) {
      saw_shift = 1;
      continue;
    }

    if (instruction->op == IR_OP_CAST && instruction->text &&
        strstr(instruction->text, "uint8") &&
        ir_operand_is_symbol_named(&instruction->dest, v_symbol)) {
      use_uint8_cast = 1;
      continue;
    }

    if (instruction->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&instruction->dest, v_symbol)) {
      continue;
    }

    if (ir_instruction_writes_destination(instruction) &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name) {
      if (!ir_operand_is_symbol_named(&instruction->dest, v_symbol) &&
          !(count_symbol &&
            ir_operand_is_symbol_named(&instruction->dest, count_symbol))) {
        return 0;
      }
    }
  }

  if (!saw_and || !saw_add || !saw_shift || !count_symbol) {
    return 0;
  }

  *count_symbol_out = count_symbol;
  *use_int32_cast_out = use_int32_cast;
  *use_uint8_cast_out = use_uint8_cast;
  return 1;
}

static int ir_popcount_emit_unrolled_step(IRInstructionVector *vector,
                                          const char *prefix, int step,
                                          const char *v_symbol,
                                          const char *count_symbol,
                                          const char *done_label,
                                          int use_int32_cast,
                                          int use_uint8_cast) {
  if (!vector || !prefix || !v_symbol || !count_symbol || !done_label) {
    return 0;
  }

  char bit_temp[64];
  char cast_temp[64];
  char shift_temp[64];
  snprintf(bit_temp, sizeof(bit_temp), "%s_b%d", prefix, step);
  snprintf(cast_temp, sizeof(cast_temp), "%s_c%d", prefix, step);
  snprintf(shift_temp, sizeof(shift_temp), "%s_s%d", prefix, step);

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.lhs = ir_operand_symbol(v_symbol);
  branch.text = mettle_strdup(done_label);
  if (!branch.lhs.name || !branch.text ||
      !ir_instruction_vector_append_move(vector, &branch)) {
    ir_instruction_destroy_storage(&branch);
    return 0;
  }

  IRInstruction and_ins = {0};
  and_ins.op = IR_OP_BINARY;
  and_ins.text = mettle_strdup("&");
  and_ins.dest = ir_operand_temp(bit_temp);
  and_ins.lhs = ir_operand_symbol(v_symbol);
  and_ins.rhs = ir_operand_int(1);
  if (!and_ins.text || !and_ins.dest.name || !and_ins.lhs.name ||
      !ir_instruction_vector_append_move(vector, &and_ins)) {
    ir_instruction_destroy_storage(&and_ins);
    return 0;
  }

  const char *add_operand = bit_temp;
  if (use_int32_cast) {
    IRInstruction cast_ins = {0};
    cast_ins.op = IR_OP_CAST;
    cast_ins.text = mettle_strdup("(int32)");
    cast_ins.dest = ir_operand_temp(cast_temp);
    cast_ins.lhs = ir_operand_temp(bit_temp);
    if (!cast_ins.text || !cast_ins.dest.name || !cast_ins.lhs.name ||
        !ir_instruction_vector_append_move(vector, &cast_ins)) {
      ir_instruction_destroy_storage(&cast_ins);
      return 0;
    }
    add_operand = cast_temp;
  }

  IRInstruction add_ins = {0};
  add_ins.op = IR_OP_BINARY;
  add_ins.text = mettle_strdup("+");
  add_ins.dest = ir_operand_symbol(count_symbol);
  add_ins.lhs = ir_operand_symbol(count_symbol);
  add_ins.rhs = ir_operand_temp(add_operand);
  if (!add_ins.text || !add_ins.dest.name || !add_ins.lhs.name ||
      !add_ins.rhs.name ||
      !ir_instruction_vector_append_move(vector, &add_ins)) {
    ir_instruction_destroy_storage(&add_ins);
    return 0;
  }

  if (use_uint8_cast) {
    IRInstruction shift_ins = {0};
    shift_ins.op = IR_OP_BINARY;
    shift_ins.text = mettle_strdup(">>");
    shift_ins.dest = ir_operand_temp(shift_temp);
    shift_ins.lhs = ir_operand_symbol(v_symbol);
    shift_ins.rhs = ir_operand_int(1);
    if (!shift_ins.text || !shift_ins.dest.name || !shift_ins.lhs.name ||
        !ir_instruction_vector_append_move(vector, &shift_ins)) {
      ir_instruction_destroy_storage(&shift_ins);
      return 0;
    }

    IRInstruction cast_v = {0};
    cast_v.op = IR_OP_CAST;
    cast_v.text = mettle_strdup("(uint8)");
    cast_v.dest = ir_operand_symbol(v_symbol);
    cast_v.lhs = ir_operand_temp(shift_temp);
    if (!cast_v.text || !cast_v.dest.name || !cast_v.lhs.name ||
        !ir_instruction_vector_append_move(vector, &cast_v)) {
      ir_instruction_destroy_storage(&cast_v);
      return 0;
    }
  } else {
    IRInstruction shift_ins = {0};
    shift_ins.op = IR_OP_BINARY;
    shift_ins.text = mettle_strdup(">>");
    shift_ins.dest = ir_operand_symbol(v_symbol);
    shift_ins.lhs = ir_operand_symbol(v_symbol);
    shift_ins.rhs = ir_operand_int(1);
    if (!shift_ins.text || !shift_ins.dest.name || !shift_ins.lhs.name ||
        !ir_instruction_vector_append_move(vector, &shift_ins)) {
      ir_instruction_destroy_storage(&shift_ins);
      return 0;
    }
  }

  return 1;
}

static int ir_try_fold_popcount_byte_loop_at(IRFunction *function,
                                             size_t header_index,
                                             int *changed) {
  if (!function || header_index >= function->instruction_count) {
    return 1;
  }

  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !header->text) {
    return 1;
  }

  size_t branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &branch_index)) {
    return 1;
  }

  IRInstruction *branch = &function->instructions[branch_index];
  if (branch->op != IR_OP_BRANCH_ZERO ||
      branch->lhs.kind != IR_OPERAND_SYMBOL || !branch->lhs.name ||
      !branch->text) {
    return 1;
  }

  const char *v_symbol = branch->lhs.name;
  const char *done_label = branch->text;

  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    IRInstruction *probe = &function->instructions[i];
    if (probe->op == IR_OP_JUMP && probe->text &&
        strcmp(probe->text, header->text) == 0) {
      jump_index = i;
      break;
    }
    if (probe->op == IR_OP_LABEL) {
      break;
    }
  }
  if (jump_index == (size_t)-1) {
    return 1;
  }

  const char *count_symbol = NULL;
  int use_int32_cast = 0;
  int use_uint8_cast = 0;
  if (!ir_popcount_body_matches(function, branch_index + 1, jump_index,
                                v_symbol, &count_symbol, &use_int32_cast,
                                &use_uint8_cast)) {
    return 1;
  }

  size_t done_index = 0;
  if (!ir_find_label_index(function, done_label, &done_index) ||
      done_index <= jump_index) {
    return 1;
  }
  /* The rebuild below splices [0..header) + unrolled steps + [done..end),
   * DROPPING everything in (jump..done). That is only the empty fallthrough
   * gap when the exit label directly follows the loop; a threaded exit label
   * further away would take live code (e.g. an else branch) with it. */
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, done_label)) {
    return 1;
  }

  char prefix[32];
  snprintf(prefix, sizeof(prefix), "pc%zu", header_index);

  IRInstructionVector vector = {0};
  for (size_t i = 0; i < header_index; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  for (int step = 0; step < IR_POPCOUNT_BYTE_UNROLL; step++) {
    if (!ir_popcount_emit_unrolled_step(&vector, prefix, step, v_symbol,
                                        count_symbol, done_label,
                                        use_int32_cast, use_uint8_cast)) {
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  for (size_t i = done_index; i < function->instruction_count; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  if (!ir_function_replace_instructions(function, &vector)) {
    ir_instruction_vector_destroy(&vector);
    return 0;
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_fold_popcount_byte_loop_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  int local_changed = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL) {
      continue;
    }

    int folded = 0;
    if (!ir_try_fold_popcount_byte_loop_at(function, i, &folded)) {
      return 0;
    }
    if (folded) {
      local_changed = 1;
      break;
    }
  }

  if (local_changed && changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_instruction_is_profile_hook(const IRInstruction *instruction) {
  if (!instruction || instruction->op != IR_OP_CALL || !instruction->text) {
    return 0;
  }
  return strcmp(instruction->text, "mettle_profile_enter") == 0 ||
         strcmp(instruction->text, "mettle_profile_exit") == 0;
}

static int ir_try_find_loop_latch(const IRFunction *function, size_t header_index,
                                  const char *header_label, size_t *branch_index_out,
                                  size_t *body_start_out, size_t *increment_index_out,
                                  size_t *jump_index_out) {
  if (!function || !header_label || !branch_index_out || !body_start_out ||
      !increment_index_out || !jump_index_out ||
      header_index >= function->instruction_count) {
    return 0;
  }

  size_t branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &branch_index)) {
    return 0;
  }

  const IRInstruction *branch = &function->instructions[branch_index];
  if (branch->op != IR_OP_BRANCH_ZERO) {
    size_t compare_index = branch_index;
    if (!ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
      return 0;
    }
    branch = &function->instructions[branch_index];
    if (branch->op != IR_OP_BRANCH_ZERO) {
      return 0;
    }
  }

  const char *exit_label =
      (branch->op == IR_OP_BRANCH_ZERO && branch->text) ? branch->text : NULL;

  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *probe = &function->instructions[i];
    if (probe->op == IR_OP_JUMP && probe->text &&
        strcmp(probe->text, header_label) == 0) {
      jump_index = i;
      break;
    }
    if (probe->op == IR_OP_LABEL && probe->text && exit_label &&
        strcmp(probe->text, exit_label) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1 || jump_index <= branch_index + 1) {
    return 0;
  }

  size_t increment_index = (size_t)-1;
  for (size_t i = jump_index; i > branch_index + 1; ) {
    i--;
    const IRInstruction *probe = &function->instructions[i];
    if (probe->op == IR_OP_NOP || ir_instruction_is_profile_hook(probe)) {
      continue;
    }
    if (probe->op == IR_OP_BINARY && probe->text &&
        strcmp(probe->text, "+") == 0 &&
        probe->dest.kind == IR_OPERAND_SYMBOL && probe->dest.name &&
        probe->lhs.kind == IR_OPERAND_SYMBOL && probe->lhs.name &&
        strcmp(probe->dest.name, probe->lhs.name) == 0 &&
        ir_operand_is_int_value(&probe->rhs, 1)) {
      increment_index = i;
      break;
    }
    return 0;
  }
  if (increment_index == (size_t)-1 || increment_index <= branch_index + 1) {
    return 0;
  }

  *branch_index_out = branch_index;
  *body_start_out = branch_index + 1;
  *increment_index_out = increment_index;
  *jump_index_out = jump_index;
  return 1;
}

static int ir_try_match_popcount_buffer_call_body(const IRFunction *function,
                                                 size_t body_start,
                                                 size_t body_end,
                                                 const char **total_symbol_out,
                                                 const char **load_source_out,
                                                 int *load_via_ptr_out) {
  if (!function || !total_symbol_out || !load_source_out || !load_via_ptr_out ||
      body_start >= body_end) {
    return 0;
  }

  size_t call_index = (size_t)-1;
  const char *total_symbol = NULL;
  const char *load_source = NULL;
  int load_via_ptr = 0;

  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction || instruction->op == IR_OP_NOP ||
        instruction->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }

    if (ir_instruction_is_profile_hook(instruction)) {
      continue;
    }

    if (instruction->op == IR_OP_LOAD && instruction->dest.kind == IR_OPERAND_TEMP &&
        instruction->dest.name && instruction->rhs.kind == IR_OPERAND_INT &&
        instruction->rhs.int_value == 1) {
      if (instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name) {
        load_source = instruction->lhs.name;
        load_via_ptr = 1;
      } else if (instruction->lhs.kind == IR_OPERAND_TEMP &&
                 instruction->lhs.name) {
        size_t producer_index = 0;
        const IRInstruction *addr = NULL;
        if (ir_find_last_writer_before(function, i, IR_OPERAND_TEMP,
                                       instruction->lhs.name, &producer_index)) {
          addr = &function->instructions[producer_index];
        }
        if (addr && addr->op == IR_OP_BINARY && addr->text &&
            strcmp(addr->text, "+") == 0 &&
            addr->lhs.kind == IR_OPERAND_SYMBOL && addr->lhs.name &&
            addr->dest.kind == IR_OPERAND_TEMP &&
            ir_operand_is_temp_named(&addr->dest, instruction->lhs.name)) {
          load_source = addr->lhs.name;
          load_via_ptr = 0;
        }
      }
      continue;
    }

    if (instruction->op == IR_OP_CALL && instruction->text &&
        strcmp(instruction->text, "popcount_byte") == 0) {
      call_index = i;
      continue;
    }

    if (ir_instruction_is_profile_hook(instruction)) {
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        strcmp(instruction->text, "+") == 0 &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
        (ir_operand_is_symbol_named(&instruction->lhs, instruction->dest.name) ||
         ir_operand_is_symbol_named(&instruction->rhs, instruction->dest.name))) {
      total_symbol = instruction->dest.name;
      continue;
    }

    if (instruction->op == IR_OP_CAST || instruction->op == IR_OP_ASSIGN) {
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        strcmp(instruction->text, "+") == 0 &&
        instruction->dest.kind == IR_OPERAND_TEMP) {
      continue;
    }

    return 0;
  }

  if (call_index == (size_t)-1 || !total_symbol || !load_source) {
    return 0;
  }

  *total_symbol_out = total_symbol;
  *load_source_out = load_source;
  *load_via_ptr_out = load_via_ptr;
  return 1;
}

static int ir_try_match_popcount_buffer_inlined_body(
    const IRFunction *function, size_t body_start, size_t body_end,
    const char **total_symbol_out, const char **load_source_out,
    int *load_via_ptr_out, const char **v_symbol_out) {
  if (!function || !total_symbol_out || !load_source_out || !load_via_ptr_out ||
      !v_symbol_out || body_start >= body_end) {
    return 0;
  }

  const char *total_symbol = NULL;
  const char *load_source = NULL;
  int load_via_ptr = 0;
  const char *v_symbol = NULL;
  const char *count_symbol = NULL;
  size_t popcount_start = body_end;
  size_t popcount_end = body_start;

  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction || instruction->op == IR_OP_NOP ||
        instruction->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }

    if (ir_instruction_is_profile_hook(instruction)) {
      continue;
    }

    if (instruction->op == IR_OP_LOAD && instruction->dest.kind == IR_OPERAND_TEMP &&
        instruction->dest.name && instruction->rhs.kind == IR_OPERAND_INT &&
        instruction->rhs.int_value == 1) {
      if (instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name) {
        load_source = instruction->lhs.name;
        load_via_ptr = 1;
      } else if (instruction->lhs.kind == IR_OPERAND_TEMP &&
                 instruction->lhs.name) {
        size_t producer_index = 0;
        if (ir_find_last_writer_before(function, i, IR_OPERAND_TEMP,
                                       instruction->lhs.name, &producer_index)) {
          const IRInstruction *addr = &function->instructions[producer_index];
          if (addr->op == IR_OP_BINARY && addr->text &&
              strcmp(addr->text, "+") == 0 &&
              addr->lhs.kind == IR_OPERAND_SYMBOL && addr->lhs.name) {
            load_source = addr->lhs.name;
            load_via_ptr = 0;
          }
        }
      }
      continue;
    }

    if (instruction->op == IR_OP_ASSIGN && instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name && instruction->lhs.kind == IR_OPERAND_INT &&
        instruction->lhs.int_value == 0) {
      count_symbol = instruction->dest.name;
      continue;
    }

    if (instruction->op == IR_OP_ASSIGN && instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        (instruction->lhs.kind == IR_OPERAND_TEMP ||
         instruction->lhs.kind == IR_OPERAND_SYMBOL)) {
      continue;
    }

    if (instruction->op == IR_OP_BRANCH_ZERO &&
        instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name) {
      v_symbol = instruction->lhs.name;
      if (popcount_start == body_end) {
        popcount_start = i;
      }
      popcount_end = i + 1;
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        strcmp(instruction->text, "+") == 0 &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
        count_symbol &&
        ir_operand_is_symbol_named(&instruction->dest, count_symbol)) {
      if (popcount_end <= i) {
        popcount_end = i + 1;
      }
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        strcmp(instruction->text, "+") == 0 &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
        (ir_operand_is_symbol_named(&instruction->lhs, instruction->dest.name) ||
         ir_operand_is_symbol_named(&instruction->rhs, instruction->dest.name))) {
      total_symbol = instruction->dest.name;
      continue;
    }

    if (instruction->op == IR_OP_CAST || instruction->op == IR_OP_LABEL) {
      continue;
    }

    if (ir_instruction_is_profile_hook(instruction)) {
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        (strcmp(instruction->text, "&") == 0 ||
         strcmp(instruction->text, ">>") == 0 ||
         strcmp(instruction->text, "/") == 0)) {
      if (popcount_end <= i) {
        popcount_end = i + 1;
      }
      continue;
    }

    if (instruction->op == IR_OP_ASSIGN) {
      continue;
    }

    if (instruction->op == IR_OP_BINARY && instruction->text &&
        strcmp(instruction->text, "+") == 0 &&
        instruction->dest.kind == IR_OPERAND_TEMP) {
      continue;
    }

    return 0;
  }

  if (!total_symbol || !load_source || !v_symbol || !count_symbol ||
      popcount_start >= body_end) {
    return 0;
  }

  int saw_unrolled_shift = 0;
  for (size_t i = popcount_start; i < body_end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_BINARY && !instruction->is_float && instruction->text &&
        strcmp(instruction->text, ">>") == 0 &&
        ir_operand_is_symbol_named(&instruction->lhs, v_symbol) &&
        ir_operand_is_int_value(&instruction->rhs, 1)) {
      saw_unrolled_shift = 1;
      break;
    }
  }
  if (!saw_unrolled_shift) {
    return 0;
  }

  *total_symbol_out = total_symbol;
  *load_source_out = load_source;
  *load_via_ptr_out = load_via_ptr;
  *v_symbol_out = v_symbol;
  return 1;
}

static int ir_popcount_emit_fused_load_byte(IRInstructionVector *vector,
                                            const char *prefix,
                                            int load_via_ptr,
                                            const char *load_source,
                                            const char *advance_symbol,
                                            const char *v_temp) {
  if (!vector || !prefix || !load_source || !v_temp) {
    return 0;
  }

  char raw_temp[64];
  char addr_temp[64];
  snprintf(raw_temp, sizeof(raw_temp), "%s_raw", prefix);
  snprintf(addr_temp, sizeof(addr_temp), "%s_ad", prefix);

  if (load_via_ptr) {
    IRInstruction load_ins = {0};
    load_ins.op = IR_OP_LOAD;
    load_ins.dest = ir_operand_temp(raw_temp);
    load_ins.lhs = ir_operand_symbol(load_source);
    load_ins.rhs = ir_operand_int(1);
    if (!load_ins.dest.name || !load_ins.lhs.name ||
        !ir_instruction_vector_append_move(vector, &load_ins)) {
      ir_instruction_destroy_storage(&load_ins);
      return 0;
    }
  } else {
    if (!advance_symbol) {
      return 0;
    }
    IRInstruction addr_ins = {0};
    addr_ins.op = IR_OP_BINARY;
    addr_ins.text = mettle_strdup("+");
    addr_ins.dest = ir_operand_temp(addr_temp);
    addr_ins.lhs = ir_operand_symbol(load_source);
    addr_ins.rhs = ir_operand_symbol(advance_symbol);
    if (!addr_ins.text || !addr_ins.dest.name || !addr_ins.lhs.name ||
        !addr_ins.rhs.name ||
        !ir_instruction_vector_append_move(vector, &addr_ins)) {
      ir_instruction_destroy_storage(&addr_ins);
      return 0;
    }

    IRInstruction load_ins = {0};
    load_ins.op = IR_OP_LOAD;
    load_ins.dest = ir_operand_temp(raw_temp);
    load_ins.lhs = ir_operand_temp(addr_temp);
    load_ins.rhs = ir_operand_int(1);
    if (!load_ins.dest.name || !load_ins.lhs.name ||
        !ir_instruction_vector_append_move(vector, &load_ins)) {
      ir_instruction_destroy_storage(&load_ins);
      return 0;
    }
  }

  IRInstruction cast_ins = {0};
  cast_ins.op = IR_OP_CAST;
  cast_ins.text = mettle_strdup("(uint8)");
  cast_ins.dest = ir_operand_temp(v_temp);
  cast_ins.lhs = ir_operand_temp(raw_temp);
  if (!cast_ins.text || !cast_ins.dest.name || !cast_ins.lhs.name ||
      !ir_instruction_vector_append_move(vector, &cast_ins)) {
    ir_instruction_destroy_storage(&cast_ins);
    return 0;
  }

  return 1;
}

static int ir_try_fuse_popcount_buffer_at(IRFunction *function,
                                            size_t header_index,
                                            int *changed) {
  if (!function || header_index >= function->instruction_count) {
    return 1;
  }

  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !header->text) {
    return 1;
  }

  size_t branch_index = 0;
  size_t body_start = 0;
  size_t increment_index = 0;
  size_t jump_index = 0;
  if (!ir_try_find_loop_latch(function, header_index, header->text, &branch_index,
                              &body_start, &increment_index, &jump_index)) {
    return 1;
  }

  const char *total_symbol = NULL;
  const char *load_source = NULL;
  int load_via_ptr = 0;

  if (ir_try_match_popcount_buffer_call_body(function, body_start, increment_index,
                                             &total_symbol, &load_source,
                                             &load_via_ptr)) {
    /* call form */
  } else {
    const char *v_symbol = NULL;
    if (!ir_try_match_popcount_buffer_inlined_body(
            function, body_start, increment_index, &total_symbol, &load_source,
            &load_via_ptr, &v_symbol)) {
      return 1;
    }
    (void)v_symbol;
  }

  const IRInstruction *increment = &function->instructions[increment_index];
  if (increment->op != IR_OP_BINARY || !increment->dest.name) {
    return 1;
  }
  const char *advance_symbol = increment->dest.name;

  char prefix[32];
  char v_temp[64];
  snprintf(prefix, sizeof(prefix), "pbf%zu", header_index);
  snprintf(v_temp, sizeof(v_temp), "%s_v", prefix);

  IRInstructionVector vector = {0};
  for (size_t i = 0; i < body_start; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  if (!ir_popcount_emit_fused_load_byte(&vector, prefix, load_via_ptr,
                                        load_source,
                                        load_via_ptr ? NULL : advance_symbol,
                                        v_temp)) {
    ir_instruction_vector_destroy(&vector);
    return 0;
  }

  {
    char pop_temp[64];
    char pop_i64_temp[64];
    IRInstruction pop = {0};
    IRInstruction cast = {0};
    IRInstruction add_total = {0};

    snprintf(pop_temp, sizeof(pop_temp), "%s_pc", prefix);
    snprintf(pop_i64_temp, sizeof(pop_i64_temp), "%s_pc64", prefix);

    pop.op = IR_OP_UNARY;
    pop.text = mettle_strdup("popcnt");
    pop.dest = ir_operand_temp(pop_temp);
    pop.lhs = ir_operand_temp(v_temp);
    if (!pop.text || !pop.dest.name || !pop.lhs.name ||
        !ir_instruction_vector_append_move(&vector, &pop)) {
      ir_instruction_destroy_storage(&pop);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }

    cast.op = IR_OP_CAST;
    cast.text = mettle_strdup("(int64)");
    cast.dest = ir_operand_temp(pop_i64_temp);
    cast.lhs = ir_operand_temp(pop_temp);
    if (!cast.text || !cast.dest.name || !cast.lhs.name ||
        !ir_instruction_vector_append_move(&vector, &cast)) {
      ir_instruction_destroy_storage(&cast);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }

    add_total.op = IR_OP_BINARY;
    add_total.text = mettle_strdup("+");
    add_total.dest = ir_operand_symbol(total_symbol);
    add_total.lhs = ir_operand_symbol(total_symbol);
    add_total.rhs = ir_operand_temp(pop_i64_temp);
    if (!add_total.text || !add_total.dest.name || !add_total.lhs.name ||
        !add_total.rhs.name || !ir_instruction_vector_append_move(&vector, &add_total)) {
      ir_instruction_destroy_storage(&add_total);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  for (size_t i = increment_index; i < function->instruction_count; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  if (!ir_function_replace_instructions(function, &vector)) {
    ir_instruction_vector_destroy(&vector);
    return 0;
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_fuse_popcount_buffer_loop_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  int local_changed = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL) {
      continue;
    }

    int fused = 0;
    if (!ir_try_fuse_popcount_buffer_at(function, i, &fused)) {
      return 0;
    }
    if (fused) {
      local_changed = 1;
      break;
    }
  }

  if (local_changed && changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_instruction_is_parity_source(const IRInstruction *ins,
                                           const char *x_symbol) {
  if (!ins || ins->op != IR_OP_BINARY || ins->is_float || !ins->text ||
      !ins->dest.name || !x_symbol) {
    return 0;
  }

  if (strcmp(ins->text, "&") == 0) {
    return ir_operand_is_symbol_named(&ins->lhs, x_symbol) &&
           ir_operand_is_int_value(&ins->rhs, 1);
  }

  if (strcmp(ins->text, "%") == 0) {
    return ir_operand_is_symbol_named(&ins->lhs, x_symbol) &&
           ir_operand_is_int_value(&ins->rhs, 2);
  }

  return 0;
}

static int ir_try_fold_collatz_odd_step_at(IRFunction *function,
                                           size_t header_index, int *changed) {
  if (!function || header_index >= function->instruction_count) {
    return 1;
  }

  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !header->text) {
    return 1;
  }

  size_t guard_index = 0;
  size_t guard_branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &guard_index) ||
      !ir_find_next_non_nop(function, guard_index + 1, &guard_branch_index)) {
    return 1;
  }

  IRInstruction *guard = &function->instructions[guard_index];
  IRInstruction *guard_branch = &function->instructions[guard_branch_index];
  if (guard->op != IR_OP_BINARY || guard->is_float || !guard->text ||
      strcmp(guard->text, ">") != 0 || guard->lhs.kind != IR_OPERAND_SYMBOL ||
      !guard->lhs.name || !ir_operand_is_int_value(&guard->rhs, 1) ||
      guard->dest.kind != IR_OPERAND_TEMP || !guard->dest.name ||
      guard_branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&guard_branch->lhs, guard->dest.name)) {
    return 1;
  }

  const char *x_symbol = guard->lhs.name;
  size_t parity_index = 0;
  size_t even_compare_index = 0;
  size_t even_branch_index = 0;
  if (!ir_find_next_non_nop_in_block(function, guard_branch_index + 1,
                                     &parity_index) ||
      !ir_find_next_non_nop_in_block(function, parity_index + 1,
                                     &even_compare_index) ||
      !ir_find_next_non_nop_in_block(function, even_compare_index + 1,
                                     &even_branch_index)) {
    return 1;
  }

  IRInstruction *parity = &function->instructions[parity_index];
  IRInstruction *even_compare = &function->instructions[even_compare_index];
  IRInstruction *even_branch = &function->instructions[even_branch_index];
  if (!ir_instruction_is_parity_source(parity, x_symbol) ||
      even_compare->op != IR_OP_BINARY || even_compare->is_float ||
      !even_compare->text || strcmp(even_compare->text, "==") != 0 ||
      even_compare->dest.kind != IR_OPERAND_TEMP || !even_compare->dest.name ||
      !ir_operand_is_temp_named(&even_compare->lhs, parity->dest.name) ||
      !ir_operand_is_int_value(&even_compare->rhs, 0) ||
      even_branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&even_branch->lhs, even_compare->dest.name) ||
      !even_branch->text) {
    return 1;
  }

  size_t even_step_index = 0;
  size_t even_jump_index = 0;
  if (!ir_find_next_non_nop_in_block(function, even_branch_index + 1,
                                     &even_step_index) ||
      !ir_find_next_non_nop_in_block(function, even_step_index + 1,
                                     &even_jump_index)) {
    return 1;
  }

  IRInstruction *even_step = &function->instructions[even_step_index];
  IRInstruction *even_jump = &function->instructions[even_jump_index];
  if (even_step->op != IR_OP_BINARY || even_step->is_float || !even_step->text ||
      !ir_operand_is_symbol_named(&even_step->dest, x_symbol) ||
      !ir_operand_is_symbol_named(&even_step->lhs, x_symbol) ||
      even_jump->op != IR_OP_JUMP || !even_jump->text) {
    return 1;
  }
  if (!(strcmp(even_step->text, ">>") == 0 &&
        ir_operand_is_int_value(&even_step->rhs, 1)) &&
      !(strcmp(even_step->text, "/") == 0 &&
        ir_operand_is_int_value(&even_step->rhs, 2))) {
    return 1;
  }

  size_t odd_label_index = 0;
  size_t join_label_index = 0;
  if (!ir_find_label_index(function, even_branch->text, &odd_label_index) ||
      !ir_find_label_index(function, even_jump->text, &join_label_index) ||
      odd_label_index <= even_jump_index || join_label_index <= odd_label_index) {
    return 1;
  }

  size_t odd_mul_index = 0;
  size_t odd_add_index = 0;
  size_t odd_next_index = 0;
  if (!ir_find_next_non_nop_in_block(function, odd_label_index + 1,
                                     &odd_mul_index) ||
      !ir_find_next_non_nop_in_block(function, odd_mul_index + 1,
                                     &odd_add_index) ||
      !ir_find_next_non_nop(function, odd_add_index + 1, &odd_next_index)) {
    return 1;
  }

  IRInstruction *odd_mul = &function->instructions[odd_mul_index];
  IRInstruction *odd_add = &function->instructions[odd_add_index];
  if (odd_next_index != join_label_index || odd_mul->op != IR_OP_BINARY ||
      odd_mul->is_float || !odd_mul->text || strcmp(odd_mul->text, "*") != 0 ||
      odd_mul->dest.kind != IR_OPERAND_TEMP || !odd_mul->dest.name ||
      !((ir_operand_is_int_value(&odd_mul->lhs, 3) &&
         ir_operand_is_symbol_named(&odd_mul->rhs, x_symbol)) ||
        (ir_operand_is_int_value(&odd_mul->rhs, 3) &&
         ir_operand_is_symbol_named(&odd_mul->lhs, x_symbol))) ||
      odd_add->op != IR_OP_BINARY || odd_add->is_float || !odd_add->text ||
      strcmp(odd_add->text, "+") != 0 ||
      !ir_operand_is_symbol_named(&odd_add->dest, x_symbol) ||
      !((ir_operand_is_temp_named(&odd_add->lhs, odd_mul->dest.name) &&
         ir_operand_is_int_value(&odd_add->rhs, 1)) ||
        (ir_operand_is_temp_named(&odd_add->rhs, odd_mul->dest.name) &&
         ir_operand_is_int_value(&odd_add->lhs, 1)))) {
    return 1;
  }

  size_t count_inc_index = 0;
  size_t backedge_index = 0;
  if (!ir_find_next_non_nop_in_block(function, join_label_index + 1,
                                     &count_inc_index) ||
      !ir_find_next_non_nop_in_block(function, count_inc_index + 1,
                                     &backedge_index)) {
    return 1;
  }

  IRInstruction *count_inc = &function->instructions[count_inc_index];
  IRInstruction *backedge = &function->instructions[backedge_index];
  if (count_inc->op != IR_OP_BINARY || count_inc->is_float || !count_inc->text ||
      strcmp(count_inc->text, "+") != 0 ||
      count_inc->dest.kind != IR_OPERAND_SYMBOL || !count_inc->dest.name ||
      !((ir_operand_is_symbol_named(&count_inc->lhs, count_inc->dest.name) &&
         ir_operand_is_int_value(&count_inc->rhs, 1)) ||
        (ir_operand_is_symbol_named(&count_inc->rhs, count_inc->dest.name) &&
         ir_operand_is_int_value(&count_inc->lhs, 1))) ||
      backedge->op != IR_OP_JUMP || !backedge->text ||
      strcmp(backedge->text, header->text) != 0) {
    return 1;
  }

  IRInstruction odd_fold_shift = {0};
  IRInstruction odd_fold_count = {0};
  IRInstruction even_fold_count = {0};
  if (!ir_clone_instruction_plain(even_step, &odd_fold_shift) ||
      !ir_clone_instruction_plain(count_inc, &odd_fold_count) ||
      !ir_clone_instruction_plain(count_inc, &even_fold_count)) {
    ir_instruction_destroy_storage(&odd_fold_shift);
    ir_instruction_destroy_storage(&odd_fold_count);
    ir_instruction_destroy_storage(&even_fold_count);
    return 0;
  }

  mettle_free_string(odd_fold_shift.text);
  odd_fold_shift.text = mettle_strdup(">>");
  ir_operand_destroy(&odd_fold_shift.rhs);
  odd_fold_shift.rhs = ir_operand_int(1);
  if (!odd_fold_shift.text) {
    ir_instruction_destroy_storage(&odd_fold_shift);
    ir_instruction_destroy_storage(&odd_fold_count);
    ir_instruction_destroy_storage(&even_fold_count);
    return 0;
  }

  if (ir_operand_is_int_value(&odd_fold_count.rhs, 1)) {
    ir_operand_destroy(&odd_fold_count.rhs);
    odd_fold_count.rhs = ir_operand_int(2);
  } else if (ir_operand_is_int_value(&odd_fold_count.lhs, 1)) {
    ir_operand_destroy(&odd_fold_count.lhs);
    odd_fold_count.lhs = ir_operand_int(2);
  } else {
    ir_instruction_destroy_storage(&odd_fold_shift);
    ir_instruction_destroy_storage(&odd_fold_count);
    ir_instruction_destroy_storage(&even_fold_count);
    return 0;
  }

  if (!ir_instruction_insert_move(function, join_label_index, &odd_fold_shift)) {
    ir_instruction_destroy_storage(&odd_fold_shift);
    ir_instruction_destroy_storage(&odd_fold_count);
    ir_instruction_destroy_storage(&even_fold_count);
    return 0;
  }
  if (!ir_instruction_insert_move(function, join_label_index + 1,
                                  &odd_fold_count)) {
    ir_instruction_destroy_storage(&odd_fold_count);
    ir_instruction_destroy_storage(&even_fold_count);
    return 0;
  }
  if (!ir_instruction_insert_move(function, even_jump_index, &even_fold_count)) {
    ir_instruction_destroy_storage(&even_fold_count);
    return 0;
  }

  if (count_inc_index + 3 >= function->instruction_count) {
    return 0;
  }
  ir_instruction_make_nop(&function->instructions[count_inc_index + 3]);

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_collatz_odd_step_fold_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_try_fold_collatz_odd_step_at(function, i, changed)) {
      return 0;
    }
  }

  return 1;
}

