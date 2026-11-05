#include "ir_optimize_internal.h"
#include "../../common.h"

#define IR_POPCOUNT_BYTE_UNROLL 8

typedef struct {
  const char *v;
  const char *count;
  const char *bit;
  const char *bit_cast;
  const char *half;
  int saw_add;
  int saw_shift;
  int v_written;
  int use_int32_cast;
  int use_uint8_cast;
} IRPopcountBody;

static int ir_popcount_is_binary(const IRInstruction *in, const char *op) {
  return in->op == IR_OP_BINARY && !in->is_float && in->text &&
         strcmp(in->text, op) == 0;
}

static int ir_popcount_is_half(const IRInstruction *in, const char *v) {
  return ir_operand_is_symbol_named(&in->lhs, v) &&
         ((ir_popcount_is_binary(in, "/") &&
           ir_operand_is_int_value(&in->rhs, 2)) ||
          (ir_popcount_is_binary(in, ">>") &&
           ir_operand_is_int_value(&in->rhs, 1)));
}

static int ir_popcount_take_add(IRPopcountBody *body, const IRInstruction *in) {
  const IROperand *other = NULL;
  if (!body->bit || body->saw_add || body->saw_shift ||
      ir_operand_is_symbol_named(&in->dest, body->v)) {
    return 0;
  }
  if (ir_operand_is_symbol_named(&in->lhs, in->dest.name)) {
    other = &in->rhs;
  } else if (ir_operand_is_symbol_named(&in->rhs, in->dest.name)) {
    other = &in->lhs;
  }
  if (!other || !(ir_operand_is_temp_named(other, body->bit) ||
                  (body->bit_cast &&
                   ir_operand_is_temp_named(other, body->bit_cast)))) {
    return 0;
  }
  body->count = in->dest.name;
  body->saw_add = 1;
  return 1;
}

static int ir_popcount_take_half(IRPopcountBody *body, const IRInstruction *in) {
  if (!body->saw_add || body->saw_shift) {
    return 0;
  }
  body->saw_shift = 1;
  if (ir_operand_is_symbol_named(&in->dest, body->v)) {
    body->v_written = 1;
    return 1;
  }
  if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name) {
    return 0;
  }
  body->half = in->dest.name;
  return 1;
}

static int ir_popcount_take_write_back(IRPopcountBody *body,
                                       const IRInstruction *in) {
  if (body->v_written || !body->half ||
      !ir_operand_is_temp_named(&in->lhs, body->half)) {
    return 0;
  }
  if (in->op == IR_OP_CAST) {
    if (!in->text || !strstr(in->text, "uint8")) {
      return 0;
    }
    body->use_uint8_cast = 1;
  }
  body->v_written = 1;
  return 1;
}

static int ir_popcount_take(IRPopcountBody *body, const IRInstruction *in) {
  if (in->op == IR_OP_NOP) {
    return 1;
  }
  if (ir_popcount_is_binary(in, "&") &&
      ir_operand_is_symbol_named(&in->lhs, body->v) &&
      ir_operand_is_int_value(&in->rhs, 1) && in->dest.kind == IR_OPERAND_TEMP &&
      in->dest.name && !body->bit && !body->saw_shift) {
    body->bit = in->dest.name;
    return 1;
  }
  if (in->op == IR_OP_CAST && in->text && strstr(in->text, "int32") &&
      in->dest.kind == IR_OPERAND_TEMP && in->dest.name && body->bit &&
      !body->bit_cast && ir_operand_is_temp_named(&in->lhs, body->bit)) {
    body->bit_cast = in->dest.name;
    body->use_int32_cast = 1;
    return 1;
  }
  if (ir_popcount_is_binary(in, "+") && ir_operand_is_symbol(&in->dest)) {
    return ir_popcount_take_add(body, in);
  }
  if (ir_popcount_is_half(in, body->v)) {
    return ir_popcount_take_half(body, in);
  }
  if ((in->op == IR_OP_ASSIGN || in->op == IR_OP_CAST) &&
      ir_operand_is_symbol_named(&in->dest, body->v)) {
    return ir_popcount_take_write_back(body, in);
  }
  return in->dest.kind == IR_OPERAND_TEMP &&
         ir_instruction_is_trivially_dead_if_dest_unused(in);
}

static int ir_type_name_is_byte(const char *name) {
  return name && (strcmp(name, "uint8") == 0 || strcmp(name, "char") == 0);
}

static int ir_symbol_is_byte_wide(const IRFunction *function,
                                  const char *symbol) {
  if (!function || !symbol) {
    return 0;
  }
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names && function->parameter_names[i] &&
        strcmp(function->parameter_names[i], symbol) == 0) {
      return function->parameter_types &&
             ir_type_name_is_byte(function->parameter_types[i]);
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol(&instruction->dest) &&
        strcmp(instruction->dest.name, symbol) == 0) {
      return ir_type_name_is_byte(instruction->text);
    }
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

  IRPopcountBody body = {0};
  body.v = v_symbol;
  for (size_t i = body_start; i < body_end; i++) {
    if (!ir_popcount_take(&body, &function->instructions[i])) {
      return 0;
    }
  }
  if (!body.saw_add || !body.saw_shift || !body.v_written) {
    return 0;
  }

  *count_symbol_out = body.count;
  *use_int32_cast_out = body.use_int32_cast;
  *use_uint8_cast_out = body.use_uint8_cast;
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

  if (!ir_symbol_is_byte_wide(function, v_symbol)) {
    return 1;
  }

  size_t done_index = 0;
  if (!ir_find_label_index(function, done_label, &done_index) ||
      done_index <= jump_index) {
    return 1;
  }
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
        ir_operand_is_symbol(&probe->dest) &&
        ir_operand_is_symbol(&probe->lhs) &&
        ir_operand_names_match(&probe->dest, &probe->lhs) &&
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

typedef struct {
  const char *advance;
  const char *addr_temp;
  const char *addr_base;
  const char *load_temp;
  const char *byte_temp;
  const char *load_source;
  int load_via_ptr;
  const char *v;
  const char *count;
  const char *done;
  const char *count_cast;
  const char *total;
  IRPopcountBody step;
  int in_step;
  int steps;
  int phase;
} IRPopcountBuffer;

static int ir_pbf_skippable(const IRInstruction *in) {
  return in->op == IR_OP_NOP || in->op == IR_OP_DECLARE_LOCAL ||
         ir_instruction_is_profile_hook(in);
}

static int ir_pbf_is_byte_source(const IRPopcountBuffer *m, const IROperand *op) {
  return (m->load_temp && ir_operand_is_temp_named(op, m->load_temp)) ||
         (m->byte_temp && ir_operand_is_temp_named(op, m->byte_temp));
}

static int ir_pbf_take_load(IRPopcountBuffer *m, const IRInstruction *in) {
  int into_symbol = ir_operand_is_symbol(&in->dest);
  if (m->load_temp || m->v || !in->dest.name ||
      (in->dest.kind != IR_OPERAND_TEMP && !into_symbol) ||
      in->rhs.kind != IR_OPERAND_INT || in->rhs.int_value != 1) {
    return 0;
  }
  if (ir_operand_is_symbol(&in->lhs)) {
    m->load_source = in->lhs.name;
    m->load_via_ptr = 1;
  } else if (m->addr_temp && ir_operand_is_temp_named(&in->lhs, m->addr_temp)) {
    m->load_source = m->addr_base;
    m->load_via_ptr = 0;
  } else {
    return 0;
  }
  if (into_symbol) {
    m->v = in->dest.name;
  } else {
    m->load_temp = in->dest.name;
  }
  return 1;
}

static int ir_pbf_take_prefix(IRPopcountBuffer *m, const IRInstruction *in) {
  if (ir_popcount_is_binary(in, "+") && in->dest.kind == IR_OPERAND_TEMP &&
      in->dest.name && ir_operand_is_symbol(&in->lhs) &&
      ir_operand_is_symbol_named(&in->rhs, m->advance) && !m->addr_temp) {
    m->addr_temp = in->dest.name;
    m->addr_base = in->lhs.name;
    return 1;
  }
  if (in->op == IR_OP_LOAD) {
    return ir_pbf_take_load(m, in);
  }
  if (in->op == IR_OP_CAST && in->text && strstr(in->text, "uint8") &&
      in->dest.kind == IR_OPERAND_TEMP && in->dest.name && !m->byte_temp &&
      m->load_temp && ir_operand_is_temp_named(&in->lhs, m->load_temp)) {
    m->byte_temp = in->dest.name;
    return 1;
  }
  if (in->op == IR_OP_ASSIGN && ir_operand_is_symbol(&in->dest)) {
    if (ir_operand_is_int_value(&in->lhs, 0) && !m->count) {
      m->count = in->dest.name;
      return 1;
    }
    if (ir_pbf_is_byte_source(m, &in->lhs) && !m->v) {
      m->v = in->dest.name;
      return 1;
    }
    return 0;
  }
  if (in->op == IR_OP_BRANCH_ZERO && m->v && m->count && in->text &&
      ir_operand_is_symbol_named(&in->lhs, m->v) &&
      strcmp(m->v, m->count) != 0) {
    m->done = in->text;
    m->phase = 1;
    memset(&m->step, 0, sizeof(m->step));
    m->step.v = m->v;
    m->in_step = 1;
    return 1;
  }
  return 0;
}

static int ir_pbf_close_step(IRPopcountBuffer *m) {
  if (!m->in_step) {
    return 1;
  }
  m->in_step = 0;
  if (!m->step.saw_add || !m->step.saw_shift || !m->step.v_written ||
      !m->step.count || strcmp(m->step.count, m->count) != 0) {
    return 0;
  }
  m->steps++;
  return 1;
}

static int ir_pbf_take_steps(IRPopcountBuffer *m, const IRInstruction *in) {
  if (in->op == IR_OP_BRANCH_ZERO || in->op == IR_OP_LABEL) {
    if (!ir_pbf_close_step(m)) {
      return 0;
    }
    if (in->op == IR_OP_LABEL) {
      if (!in->text || strcmp(in->text, m->done) != 0) {
        return 0;
      }
      m->phase = 2;
      return 1;
    }
    if (!in->text || strcmp(in->text, m->done) != 0 ||
        !ir_operand_is_symbol_named(&in->lhs, m->v)) {
      return 0;
    }
    memset(&m->step, 0, sizeof(m->step));
    m->step.v = m->v;
    m->in_step = 1;
    return 1;
  }
  return m->in_step && ir_popcount_take(&m->step, in);
}

static int ir_pbf_take_suffix(IRPopcountBuffer *m, const IRInstruction *in) {
  if (in->op == IR_OP_CAST && in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
      !m->count_cast && !m->total &&
      ir_operand_is_symbol_named(&in->lhs, m->count)) {
    m->count_cast = in->dest.name;
    return 1;
  }
  if (ir_popcount_is_binary(in, "+") && ir_operand_is_symbol(&in->dest) &&
      !m->total && strcmp(in->dest.name, m->count) != 0 &&
      strcmp(in->dest.name, m->v) != 0) {
    const IROperand *other = NULL;
    if (ir_operand_is_symbol_named(&in->lhs, in->dest.name)) {
      other = &in->rhs;
    } else if (ir_operand_is_symbol_named(&in->rhs, in->dest.name)) {
      other = &in->lhs;
    }
    if (!other || !(ir_operand_is_symbol_named(other, m->count) ||
                    (m->count_cast &&
                     ir_operand_is_temp_named(other, m->count_cast)))) {
      return 0;
    }
    m->total = in->dest.name;
    return 1;
  }
  return 0;
}

static int ir_pbf_symbol_read_outside(const IRFunction *function,
                                      const char *name, size_t first,
                                      size_t last) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (i >= first && i < last) {
      continue;
    }
    if (ir_operand_is_symbol_named(&in->lhs, name) ||
        ir_operand_is_symbol_named(&in->rhs, name) ||
        (in->op == IR_OP_ADDRESS_OF &&
         ir_operand_is_symbol_named(&in->dest, name))) {
      return 1;
    }
    for (size_t a = 0; a < in->argument_count; a++) {
      if (ir_operand_is_symbol_named(&in->arguments[a], name)) {
        return 1;
      }
    }
  }
  return 0;
}

static int ir_try_match_popcount_buffer_inlined_body(
    const IRFunction *function, size_t body_start, size_t body_end,
    const char *advance_symbol, const char **total_symbol_out,
    const char **load_source_out, int *load_via_ptr_out) {
  IRPopcountBuffer m;
  memset(&m, 0, sizeof(m));
  m.advance = advance_symbol;
  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *in = &function->instructions[i];
    int ok = 0;
    if (ir_pbf_skippable(in)) {
      continue;
    }
    if (m.phase == 0) {
      ok = ir_pbf_take_prefix(&m, in);
    } else if (m.phase == 1) {
      ok = ir_pbf_take_steps(&m, in);
    } else {
      ok = ir_pbf_take_suffix(&m, in);
    }
    if (!ok) {
      return 0;
    }
  }
  if (m.phase != 2 || !m.total || !m.load_source ||
      m.steps < IR_POPCOUNT_BYTE_UNROLL ||
      !ir_symbol_is_byte_wide(function, m.v) ||
      ir_pbf_symbol_read_outside(function, m.v, body_start, body_end) ||
      ir_pbf_symbol_read_outside(function, m.count, body_start, body_end)) {
    return 0;
  }
  *total_symbol_out = m.total;
  *load_source_out = m.load_source;
  *load_via_ptr_out = m.load_via_ptr;
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
    load_ins.is_unsigned = 1;
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
    load_ins.is_unsigned = 1;
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

  const IRInstruction *increment = &function->instructions[increment_index];
  if (increment->op != IR_OP_BINARY || !increment->dest.name) {
    return 1;
  }
  const char *advance_symbol = increment->dest.name;
  if (!ir_try_match_popcount_buffer_inlined_body(
          function, body_start, increment_index, advance_symbol, &total_symbol,
          &load_source, &load_via_ptr)) {
    return 1;
  }

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

  enum { ODD_COUNT, ODD_GUARD, ODD_EXIT, ODD_SHIFT, ODD_COUNT_AGAIN, EVEN_COUNT,
         FOLD_COUNT };
  IRInstruction fold[FOLD_COUNT];
  char guard_temp[64];
  int cloned = 1;
  memset(fold, 0, sizeof(fold));
  snprintf(guard_temp, sizeof(guard_temp), "collatz%zu_%zu_g", header_index,
           function->instruction_count);
  cloned = ir_clone_instruction_plain(count_inc, &fold[ODD_COUNT]) &&
           ir_clone_instruction_plain(guard, &fold[ODD_GUARD]) &&
           ir_clone_instruction_plain(guard_branch, &fold[ODD_EXIT]) &&
           ir_clone_instruction_plain(even_step, &fold[ODD_SHIFT]) &&
           ir_clone_instruction_plain(count_inc, &fold[ODD_COUNT_AGAIN]) &&
           ir_clone_instruction_plain(count_inc, &fold[EVEN_COUNT]);
  if (cloned) {
    ir_operand_destroy(&fold[ODD_GUARD].dest);
    fold[ODD_GUARD].dest = ir_operand_temp(guard_temp);
    ir_operand_destroy(&fold[ODD_EXIT].lhs);
    fold[ODD_EXIT].lhs = ir_operand_temp(guard_temp);
    mettle_free_string(fold[ODD_SHIFT].text);
    fold[ODD_SHIFT].text = mettle_strdup(">>");
    ir_operand_destroy(&fold[ODD_SHIFT].rhs);
    fold[ODD_SHIFT].rhs = ir_operand_int(1);
    cloned = fold[ODD_GUARD].dest.name && fold[ODD_EXIT].lhs.name &&
             fold[ODD_SHIFT].text;
  }
  for (int k = 0; cloned && k < EVEN_COUNT; k++) {
    if (!ir_instruction_insert_move(function, join_label_index + (size_t)k,
                                    &fold[k])) {
      cloned = 0;
    }
  }
  if (cloned &&
      !ir_instruction_insert_move(function, even_jump_index, &fold[EVEN_COUNT])) {
    cloned = 0;
  }
  if (!cloned) {
    for (int k = 0; k < FOLD_COUNT; k++) {
      ir_instruction_destroy_storage(&fold[k]);
    }
    return 0;
  }

  if (count_inc_index + FOLD_COUNT >= function->instruction_count) {
    return 0;
  }
  ir_instruction_make_nop(&function->instructions[count_inc_index + FOLD_COUNT]);

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

static int ir_kernighan_type_width(const char *type) {
  if (!type) {
    return 0;
  }
  if (strcmp(type, "int32") == 0 || strcmp(type, "uint32") == 0) {
    return 4;
  }
  if (strcmp(type, "int64") == 0 || strcmp(type, "uint64") == 0) {
    return 8;
  }
  return 0;
}

static int ir_kernighan_is_binary(const IRInstruction *in, const char *op) {
  return in->op == IR_OP_BINARY && !in->is_float && in->text &&
         strcmp(in->text, op) == 0;
}

static int ir_kernighan_label_has_one_source(const IRFunction *function,
                                             const char *label,
                                             size_t back_edge_index) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (i == back_edge_index) {
      continue;
    }
    if (in->op != IR_OP_LABEL && in->text && strcmp(in->text, label) == 0) {
      return 0;
    }
  }
  return 1;
}

typedef struct {
  const char *x_name;
  const char *n_name;
  const char *n_type;
  int width;
  size_t jump_at;
} IRKernighanLoop;

static int ir_kernighan_is_mask_decrement(const IRInstruction *in,
                                          const char *x_name) {
  return ir_kernighan_is_binary(in, "-") && in->dest.kind == IR_OPERAND_TEMP &&
         in->dest.name && ir_operand_is_symbol(&in->lhs) &&
         strcmp(in->lhs.name, x_name) == 0 &&
         in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 1;
}

static int ir_kernighan_is_mask_cast(const IRInstruction *in,
                                     const char *masked) {
  return in->op == IR_OP_CAST && in->dest.kind == IR_OPERAND_TEMP &&
         in->dest.name && ir_operand_is_temp(&in->lhs) &&
         strcmp(in->lhs.name, masked) == 0;
}

static int ir_kernighan_is_mask_and(const IRInstruction *in,
                                    const char *x_name, const char *masked) {
  return ir_kernighan_is_binary(in, "&") &&
         ir_operand_is_symbol(&in->dest) &&
         strcmp(in->dest.name, x_name) == 0 &&
         ir_operand_is_symbol(&in->lhs) &&
         strcmp(in->lhs.name, x_name) == 0 &&
         ir_operand_is_temp(&in->rhs) &&
         strcmp(in->rhs.name, masked) == 0;
}

static int ir_kernighan_is_counter_bump(const IRInstruction *in) {
  return ir_kernighan_is_binary(in, "+") &&
         ir_operand_is_symbol(&in->dest) &&
         ir_operand_is_symbol(&in->lhs) &&
         ir_operand_names_match(&in->lhs, &in->dest) &&
         in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 1;
}

static int ir_kernighan_closes_loop(const IRFunction *function, size_t after,
                                    const char *header_label,
                                    const char *done_label, size_t *jump_at) {
  const IRInstruction *jump;

  if (!ir_find_next_non_nop(function, after + 1, jump_at)) {
    return 0;
  }
  jump = &function->instructions[*jump_at];
  if (jump->op != IR_OP_JUMP || !jump->text ||
      strcmp(jump->text, header_label) != 0) {
    return 0;
  }
  return ir_fused_loop_exit_is_adjacent(function, *jump_at, done_label) &&
         ir_kernighan_label_has_one_source(function, header_label, *jump_at);
}

static int ir_kernighan_match(const IRFunction *function, size_t header_index,
                              IRKernighanLoop *loop) {
  const IRInstruction *header = &function->instructions[header_index];
  size_t at[6];
  const IRInstruction *ins[6];
  const char *x_type;
  const char *masked;
  const char *done_label;
  int next;

  if (header->op != IR_OP_LABEL || !header->text) {
    return 0;
  }
  at[0] = header_index;
  for (int k = 1; k < 6; k++) {
    if (!ir_find_next_non_nop(function, at[k - 1] + 1, &at[k])) {
      return 0;
    }
    ins[k] = &function->instructions[at[k]];
  }
  if (ins[1]->op != IR_OP_BRANCH_ZERO || !ins[1]->text ||
      ins[1]->lhs.kind != IR_OPERAND_SYMBOL || !ins[1]->lhs.name) {
    return 0;
  }
  loop->x_name = ins[1]->lhs.name;
  done_label = ins[1]->text;
  if (!ir_kernighan_is_mask_decrement(ins[2], loop->x_name)) {
    return 0;
  }
  masked = ins[2]->dest.name;
  x_type = ir_function_local_declared_type(function, loop->x_name);
  loop->width = ir_kernighan_type_width(x_type);
  if (loop->width == 0) {
    return 0;
  }
  next = 3;
  if (ir_kernighan_is_mask_cast(ins[3], masked)) {
    if (!ins[3]->text || strcmp(ins[3]->text, x_type) != 0) {
      return 0;
    }
    masked = ins[3]->dest.name;
    next = 4;
  }
  if (!ir_kernighan_is_mask_and(ins[next], loop->x_name, masked) ||
      !ir_kernighan_is_counter_bump(ins[next + 1])) {
    return 0;
  }
  loop->n_name = ins[next + 1]->dest.name;
  if (strcmp(loop->n_name, loop->x_name) == 0) {
    return 0;
  }
  loop->n_type = ir_function_local_declared_type(function, loop->n_name);
  if (ir_kernighan_type_width(loop->n_type) == 0) {
    return 0;
  }
  return ir_kernighan_closes_loop(function, at[next + 1], header->text,
                                  done_label, &loop->jump_at);
}

static int ir_kpc_append(IRInstructionVector *vector, IRInstruction *in,
                         int ready) {
  if (ready && ir_instruction_vector_append_move(vector, in)) {
    return 1;
  }
  ir_instruction_destroy_storage(in);
  return 0;
}

static int ir_kpc_clone_range(IRInstructionVector *vector,
                              const IRFunction *function, size_t lo,
                              size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    IRInstruction cloned = {0};
    int cloned_ok = ir_clone_instruction_plain(&function->instructions[i],
                                               &cloned);
    if (!ir_kpc_append(vector, &cloned, cloned_ok)) {
      return 0;
    }
  }
  return 1;
}

static int ir_kpc_emit_popcount(IRInstructionVector *vector, const char *dest,
                                const char *source, int width) {
  IRInstruction pop = {0};

  pop.op = IR_OP_UNARY;
  pop.text = mettle_strdup(width == 8 ? "popcnt64" : "popcnt");
  pop.dest = ir_operand_temp(dest);
  pop.lhs = ir_operand_symbol(source);
  return ir_kpc_append(vector, &pop,
                       pop.text && pop.dest.name && pop.lhs.name);
}

static int ir_kpc_emit_cast(IRInstructionVector *vector, const char *dest,
                            const char *source, const char *type) {
  IRInstruction cast = {0};

  cast.op = IR_OP_CAST;
  cast.text = mettle_strdup(type);
  cast.dest = ir_operand_temp(dest);
  cast.lhs = ir_operand_temp(source);
  return ir_kpc_append(vector, &cast,
                       cast.text && cast.dest.name && cast.lhs.name);
}

static int ir_kpc_emit_accumulate(IRInstructionVector *vector,
                                  const char *counter, const char *addend) {
  IRInstruction acc = {0};

  acc.op = IR_OP_BINARY;
  acc.text = mettle_strdup("+");
  acc.dest = ir_operand_symbol(counter);
  acc.lhs = ir_operand_symbol(counter);
  acc.rhs = ir_operand_temp(addend);
  return ir_kpc_append(vector, &acc,
                       acc.text && acc.dest.name && acc.lhs.name &&
                           acc.rhs.name);
}

static int ir_kpc_emit_zero(IRInstructionVector *vector, const char *name) {
  IRInstruction clear = {0};

  clear.op = IR_OP_ASSIGN;
  clear.dest = ir_operand_symbol(name);
  clear.lhs = ir_operand_int(0);
  return ir_kpc_append(vector, &clear, clear.dest.name != NULL);
}

static int ir_kpc_rewrite(IRFunction *function, size_t header_index,
                          const IRKernighanLoop *loop) {
  IRInstructionVector vector = {0};
  char pop_temp[64];
  char sum_temp[64];

  snprintf(pop_temp, sizeof(pop_temp), "kpc%zu_c", header_index);
  snprintf(sum_temp, sizeof(sum_temp), "kpc%zu_s", header_index);
  if (!ir_kpc_clone_range(&vector, function, 0, header_index) ||
      !ir_kpc_emit_popcount(&vector, pop_temp, loop->x_name, loop->width) ||
      !ir_kpc_emit_cast(&vector, sum_temp, pop_temp, loop->n_type) ||
      !ir_kpc_emit_accumulate(&vector, loop->n_name, sum_temp) ||
      !ir_kpc_emit_zero(&vector, loop->x_name) ||
      !ir_kpc_clone_range(&vector, function, loop->jump_at + 1,
                          function->instruction_count) ||
      !ir_function_replace_instructions(function, &vector)) {
    ir_instruction_vector_destroy(&vector);
    return 0;
  }
  return 1;
}

static int ir_try_fold_kernighan_popcount_at(IRFunction *function,
                                             size_t header_index,
                                             int *changed) {
  IRKernighanLoop loop = {0};

  if (!ir_kernighan_match(function, header_index, &loop)) {
    return 1;
  }
  if (!ir_kpc_rewrite(function, header_index, &loop)) {
    return 0;
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_fold_kernighan_popcount_pass(IRFunction *function, int *changed) {
  size_t i = 0;
  if (!function) {
    return 1;
  }
  while (function->instruction_count > 6 && i + 6 < function->instruction_count) {
    int folded = 0;
    if (!ir_try_fold_kernighan_popcount_at(function, i, &folded)) {
      return 0;
    }
    if (folded) {
      if (changed) {
        *changed = 1;
      }
      i = 0;
      continue;
    }
    i++;
  }
  return 1;
}
