
#include "ir_optimize_internal.h"
#include "../ir_explain_safety.h"
#include "../ir_safety.h"
#include <time.h>

#define SAFETY_TEMP_PREFIX ".safe"
#define SAFETY_LABEL_PREFIX "ir_safe_ok_"

static unsigned g_safety_next_id;

IRSafetyIntrinsic ir_safety_intrinsic(const IRInstruction *in) {
  if (!in) return IR_SAFETY_INTRINSIC_NONE;
  if (in->op == IR_OP_SAFETY_CHECK) return IR_SAFETY_INTRINSIC_CHECK;
  if (in->op != IR_OP_CALL || !in->text) return IR_SAFETY_INTRINSIC_NONE;
  static const struct {
    const char *name;
    size_t arguments;
    IRSafetyIntrinsic kind;
  } entries[] = {
      {"mettle_safety_check", 5, IR_SAFETY_INTRINSIC_CHECK},
      {"mettle_safety_check_identity", 6, IR_SAFETY_INTRINSIC_CHECK},
      {"mettle_safety_check_affine", 8, IR_SAFETY_INTRINSIC_CHECK},
      {"mettle_safety_buffer_check", 5, IR_SAFETY_INTRINSIC_CHECK},
      {"mettle_safety_identity", 1, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_span", 1, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_span_identity", 2, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_loop_length", 5, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_value_load", 3, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_merge_identity", 2, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_subtract_identity", 2, IR_SAFETY_INTRINSIC_READ_ORIGIN},
      {"mettle_safety_value_store", 4, IR_SAFETY_INTRINSIC_WRITE_ORIGIN},
      {"mettle_safety_value_copy", 3, IR_SAFETY_INTRINSIC_WRITE_ORIGIN},
      {"mettle_safety_value_clear", 2, IR_SAFETY_INTRINSIC_WRITE_ORIGIN},
      {"mettle_safety_register", 2, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_register_static", 2, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_unregister", 1, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_reregister", 3, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_reregister_held", 4, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_free_identity", 2, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_region_begin", 2, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_region_end", 1, IR_SAFETY_INTRINSIC_LIFETIME},
      {"mettle_safety_entry_arguments", 1, IR_SAFETY_INTRINSIC_LIFETIME},
  };
  for (size_t i = 0; i < IR_ARRAY_COUNT(entries); i++)
    if (in->argument_count == entries[i].arguments &&
        !strcmp(in->text, entries[i].name)) return entries[i].kind;
  return IR_SAFETY_INTRINSIC_NONE;
}

static int safety_env_flag(const char *name, int *cache) {
  if (*cache < 0) {
    const char *value = getenv(name);
    *cache = value && value[0] && value[0] != '0';
  }
  return *cache;
}

static int safety_trace_enabled(void) {
  static int state = -1;
  return safety_env_flag("METTLE_SAFETY_TRACE", &state);
}

static int safety_time_enabled(void) {
  static int state = -1;
  return safety_env_flag("METTLE_SAFETY_TIME", &state);
}

static void safety_trace(const char *reason, size_t line) {
  if (safety_trace_enabled()) {
    fprintf(stderr, "safety: line %zu unproven: %s\n", line, reason);
  }
}

typedef struct {
  const IROperand *base;
  const IROperand *offset;
  const IROperand *identity;
  long long diagnostic_size;
  long long size;
  long long extent;
  long long access_kind;
  const char *what;
  SourceLocation location;
} SafetyAccess;

static int safety_read(const IRInstruction *instruction, SafetyAccess *access) {
  if ((instruction->argument_count != IR_SAFETY_ARG_COUNT &&
       instruction->argument_count != IR_SAFETY_TRACKED_ARG_COUNT &&
       instruction->argument_count != IR_SAFETY_ANALYZED_ARG_COUNT) ||
      !instruction->arguments) {
    return 0;
  }
  const IROperand *size = &instruction->arguments[IR_SAFETY_ARG_SIZE];
  const IROperand *extent = &instruction->arguments[IR_SAFETY_ARG_EXTENT];
  const IROperand *kind = &instruction->arguments[IR_SAFETY_ARG_ACCESS];
  if (size->kind != IR_OPERAND_INT || extent->kind != IR_OPERAND_INT ||
      kind->kind != IR_OPERAND_INT) {
    return 0;
  }

  access->base = &instruction->arguments[IR_SAFETY_ARG_BASE];
  access->offset = &instruction->arguments[IR_SAFETY_ARG_OFFSET];
  access->identity = instruction->argument_count >= IR_SAFETY_TRACKED_ARG_COUNT
      ? &instruction->arguments[IR_SAFETY_ARG_IDENTITY] : NULL;
  access->diagnostic_size = instruction->argument_count == IR_SAFETY_ANALYZED_ARG_COUNT
      ? instruction->arguments[IR_SAFETY_ARG_DIAGNOSTIC_SIZE].int_value : 0;
  access->size = size->int_value;
  access->extent = extent->int_value;
  access->access_kind = kind->int_value;
  access->what = instruction->text ? instruction->text : "?";
  access->location = instruction->location;
  return 1;
}

static int safety_emit_binary(IRInstructionVector *out, SourceLocation location,
                              const char *op_text, const char *dest_temp,
                              const IROperand *lhs, const IROperand *rhs,
                              int is_unsigned) {
  IRInstruction insn = {0};
  insn.op = IR_OP_BINARY;
  insn.location = location;
  insn.text = mettle_strdup(op_text);
  insn.dest = ir_operand_temp(dest_temp);
  insn.is_unsigned = is_unsigned;
  if (!insn.text || !insn.dest.name || !ir_operand_clone(lhs, &insn.lhs) ||
      !ir_operand_clone(rhs, &insn.rhs) ||
      !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

static int safety_emit_branch_zero(IRInstructionVector *out,
                                   SourceLocation location,
                                   const char *condition_temp,
                                   const char *label) {
  IRInstruction insn = {0};
  insn.op = IR_OP_BRANCH_ZERO;
  insn.location = location;
  insn.text = mettle_strdup(label);
  insn.lhs = ir_operand_temp(condition_temp);
  if (!insn.text || !insn.lhs.name ||
      !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

static int safety_emit_label(IRInstructionVector *out, SourceLocation location,
                             const char *label) {
  IRInstruction insn = {0};
  insn.op = IR_OP_LABEL;
  insn.location = location;
  insn.text = mettle_strdup(label);
  if (!insn.text || !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

static int safety_emit_call(IRInstructionVector *out, SourceLocation location,
                            const char *callee, const IROperand *arguments,
                            size_t count) {
  IRInstruction insn = {0};
  insn.op = IR_OP_CALL;
  insn.location = location;
  insn.text = mettle_strdup(callee);
  if (!insn.text) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  if (count > 0) {
    insn.arguments = calloc(count, sizeof(IROperand));
    if (!insn.arguments) {
      ir_instruction_destroy_storage(&insn);
      return 0;
    }
  }
  insn.argument_count = count;
  for (size_t i = 0; i < count; i++) {
    if (!ir_operand_clone(&arguments[i], &insn.arguments[i])) {
      ir_instruction_destroy_storage(&insn);
      return 0;
    }
  }
  if (!ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

typedef struct {
  size_t header_index;
  IRWhileLoopBounds bounds;
  int has_index;
  const char *iv;
  long long step;
  long long adjust;
  const IROperand *bound;
  size_t step_first;
  size_t step_last;
} SafetyLoopForm;

typedef struct {
  SafetyLoopForm *items;
  size_t count;
  size_t capacity;
} SafetyLoopList;

static const SafetyLoopForm *safety_enclosing_loop(const SafetyLoopList *loops,
                                                  size_t index);

typedef struct {
  char **names;
  long long *values;
  size_t count;
} SafetyConstGlobals;

static SafetyConstGlobals g_safety_const_globals;

static int safety_const_global_value(const char *name, long long *out) {
  size_t lo = 0;
  size_t hi = g_safety_const_globals.count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int order = strcmp(g_safety_const_globals.names[mid], name);
    if (order == 0) {
      *out = g_safety_const_globals.values[mid];
      return 1;
    }
    if (order < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return 0;
}

static int safety_constant_value(const IRFunction *function, size_t before,
                                 const IROperand *operand, int depth,
                                 long long *out) {
  if (operand->kind == IR_OPERAND_INT) {
    *out = operand->int_value;
    return 1;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    return safety_const_global_value(operand->name, out);
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name || depth > 4) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, operand->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  if (producer->op == IR_OP_ASSIGN) {
    return safety_constant_value(function, before, &producer->lhs, depth + 1,
                                 out);
  }
  if (producer->op == IR_OP_CAST) {
    if (!producer->text) {
      return 0;
    }
    int to_signed = strcmp(producer->text, "int64") == 0;
    if (!to_signed && strcmp(producer->text, "uint64") != 0) {
      return 0;
    }
    long long inner = 0;
    if (!safety_constant_value(function, before, &producer->lhs, depth + 1,
                               &inner) ||
        (!to_signed && inner < 0)) {
      return 0;
    }
    *out = inner;
    return 1;
  }
  if (producer->op != IR_OP_BINARY || !producer->text) {
    return 0;
  }
  long long lhs = 0;
  long long rhs = 0;
  if (!safety_constant_value(function, before, &producer->lhs, depth + 1,
                             &lhs) ||
      !safety_constant_value(function, before, &producer->rhs, depth + 1,
                             &rhs)) {
    return 0;
  }
  if (strcmp(producer->text, "*") == 0) {
    if (lhs != 0 && (lhs > INT32_MAX || lhs < INT32_MIN || rhs > INT32_MAX ||
                     rhs < INT32_MIN)) {
      return 0;
    }
    *out = lhs * rhs;
    return 1;
  }
  if (strcmp(producer->text, "+") == 0) {
    *out = lhs + rhs;
    return 1;
  }
  if (strcmp(producer->text, "-") == 0) {
    *out = lhs - rhs;
    return 1;
  }
  return 0;
}

static int safety_prove_constant(const IRFunction *function, size_t check_index,
                                 const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN) {
    return 0;
  }
  long long offset = 0;
  if (!safety_constant_value(function, check_index, access->offset, 0,
                             &offset)) {
    return 0;
  }
  if (offset < 0 || access->size <= 0 || access->size > access->extent) {
    return 0;
  }
  return offset <= access->extent - access->size;
}

static int safety_index_is_affine(const IRFunction *function, size_t before,
                                  const IROperand *index, const char **name_out,
                                  long long *addend_out) {
  const char *name = NULL;
  long long coeff = 0;
  long long addend = 0;
  if (!ir_affine_index_decompose(function, before, index, &name, &coeff,
                                 &addend) ||
      !name || coeff != 1) {
    return 0;
  }
  *name_out = name;
  *addend_out = addend;
  return 1;
}

static int safety_index_upper_bound(const IRFunction *function, size_t before,
                                    const IROperand *index, int depth,
                                    long long *upper_out) {
  if (index->kind == IR_OPERAND_INT) {
    if (index->int_value < 0) {
      return 0;
    }
    *upper_out = index->int_value;
    return 1;
  }
  if (index->kind != IR_OPERAND_TEMP || !index->name || depth > 4) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, index->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  if (producer->op == IR_OP_ASSIGN) {
    return safety_index_upper_bound(function, before, &producer->lhs, depth + 1,
                                    upper_out);
  }
  if (producer->op != IR_OP_BINARY || !producer->text ||
      strcmp(producer->text, "&") != 0) {
    return 0;
  }
  if (producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value >= 0) {
    *upper_out = producer->rhs.int_value;
    return 1;
  }
  if (producer->lhs.kind == IR_OPERAND_INT && producer->lhs.int_value >= 0) {
    *upper_out = producer->lhs.int_value;
    return 1;
  }
  return 0;
}

static int safety_offset_scaling(const IRFunction *function, size_t check_index,
                                 const IROperand *offset,
                                 const IROperand **index_out,
                                 long long *stride_out);

static int safety_offset_upper_bound(const IRFunction *function,
                                     size_t check_index,
                                     const IROperand *offset,
                                     long long *stride_out,
                                     long long *upper_out) {
  const IROperand *index = NULL;
  return safety_offset_scaling(function, check_index, offset, &index, stride_out) &&
      safety_index_upper_bound(function, check_index, index, 0, upper_out);
}

static int safety_offset_scaling(const IRFunction *function, size_t check_index,
                                 const IROperand *offset,
                                 const IROperand **index_out,
                                 long long *stride_out) {
  *index_out = offset;
  *stride_out = 1;
  if (offset->kind == IR_OPERAND_SYMBOL || offset->kind == IR_OPERAND_INT) return 1;
  if (offset->kind != IR_OPERAND_TEMP || !offset->name) return 0;
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, check_index, offset->name);
  if (!producer || producer->op != IR_OP_BINARY || producer->is_float ||
      !producer->text) {
    return 1;
  }
  if (strcmp(producer->text, "*") == 0 &&
      producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value > 0) {
    *index_out = &producer->lhs;
    *stride_out = producer->rhs.int_value;
    return 1;
  }
  if (strcmp(producer->text, "<<") == 0 &&
      producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value >= 0 &&
      producer->rhs.int_value < 32) {
    *index_out = &producer->lhs;
    *stride_out = 1LL << producer->rhs.int_value;
    return 1;
  }
  *index_out = offset;
  *stride_out = 1;
  return 1;
}

static int safety_offset_is_scaled_symbol(const IRFunction *function,
                                          size_t check_index,
                                          const IROperand *offset,
                                          const char **iv_out,
                                          long long *stride_out,
                                          long long *addend_out) {
  const IROperand *index = NULL;
  return safety_offset_scaling(function, check_index, offset, &index, stride_out) &&
         safety_index_is_affine(function, check_index, index, iv_out, addend_out);
}

static int safety_symbol_written_between(const IRFunction *function,
                                         size_t start, size_t end,
                                         const char *symbol, size_t step_first,
                                         size_t step_last) {
  for (size_t i = start; i < end && i < function->instruction_count; i++) {
    if (i >= step_first && i <= step_last) {
      continue;
    }
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        strcmp(instruction->dest.name, symbol) == 0) {
      return 1;
    }
  }
  return 0;
}

static int safety_read_step_add(const IRFunction *function, size_t before,
                                const IRInstruction *instruction,
                                const char *iv, long long *step_out) {
  if (!instruction || instruction->op != IR_OP_BINARY ||
      instruction->is_float || !instruction->text ||
      strcmp(instruction->text, "+") != 0 ||
      !ir_operand_is_symbol_named(&instruction->lhs, iv)) {
    return 0;
  }
  long long step = 0;
  if (!safety_constant_value(function, before, &instruction->rhs, 0, &step) ||
      step <= 0) {
    return 0;
  }
  *step_out = step;
  return 1;
}

static int safety_loop_step(const IRFunction *function,
                            const IRWhileLoopBounds *loop, const char *iv,
                            long long *step_out, size_t *step_first,
                            size_t *step_last) {
  size_t write_index = 0;
  size_t write_count = 0;
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name && strcmp(instruction->dest.name, iv) == 0) {
      write_index = i;
      write_count++;
    }
  }
  if (write_count != 1) {
    return 0;
  }

  const IRInstruction *write = &function->instructions[write_index];
  if (safety_read_step_add(function, write_index, write, iv, step_out)) {
    *step_first = write_index;
    *step_last = write_index;
    return 1;
  }

  if (write->op != IR_OP_ASSIGN ||
      write->lhs.kind != IR_OPERAND_TEMP ||
      !write->lhs.name) {
    return 0;
  }
  const IRInstruction *add =
      ir_find_temp_producer_before(function, write_index, write->lhs.name);
  if (!add) {
    return 0;
  }
  size_t add_index = (size_t)(add - function->instructions);
  if (!safety_read_step_add(function, add_index, add, iv, step_out)) {
    return 0;
  }
  if (add_index <= loop->branch_index || add_index >= loop->jump_index) {
    return 0;
  }
  *step_first = add_index;
  *step_last = write_index;
  return 1;
}

static int safety_prove_loop_bound(IRFunction *function,
                                   const SafetyLoopList *loops,
                                   size_t check_index,
                                   const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }

  const char *iv = NULL;
  long long stride = 0;
  long long addend = 0;
  if (!safety_offset_is_scaled_symbol(function, check_index, access->offset,
                                      &iv, &stride, &addend)) {
    safety_trace("the offset is not an index scaled by a constant width",
                 access->location.line);
    return 0;
  }
  if (addend < 0) {
    return 0;
  }

  for (size_t i = loops->count; i-- > 0;) {
    const SafetyLoopForm *loop = &loops->items[i];
    if (check_index <= loop->bounds.branch_index ||
        check_index >= loop->bounds.jump_index) {
      continue;
    }
    if (!loop->has_index) {
      continue;
    }
    if (strcmp(loop->iv, iv) != 0) {
      continue;
    }

    long long bound = 0;
    if (!safety_constant_value(function, loop->bounds.compare_index,
                               loop->bound, 0, &bound)) {
      safety_trace("the loop bound is not a constant", access->location.line);
      return 0;
    }
    long long highest_index = bound + loop->adjust;
    if (highest_index < 0) {
      return 1;
    }

    if (!ir_iv_zero_at_header(function, loop->header_index, iv)) {
      safety_trace("the loop index does not start at zero",
                   access->location.line);
      return 0;
    }
    size_t step_first = 0;
    size_t step_last = 0;
    long long step = 0;
    if (!safety_loop_step(function, &loop->bounds, iv, &step, &step_first,
                          &step_last)) {
      safety_trace("the loop index does not step by a constant",
                   access->location.line);
      return 0;
    }
    if (safety_symbol_written_between(function, loop->bounds.branch_index + 1,
                                      loop->bounds.jump_index, iv, step_first,
                                      step_last)) {
      safety_trace("the loop index is assigned inside the body",
                   access->location.line);
      return 0;
    }

    if (highest_index > LLONG_MAX - addend || stride <= 0 ||
        highest_index + addend > LLONG_MAX / stride) return 0;
    long long highest = (highest_index + addend) * stride;
    if (highest < 0 || access->size > access->extent) {
      return 0;
    }
    if (highest <= access->extent - access->size) {
      return 1;
    }
    safety_trace("the loop can reach past the end of the object",
                 access->location.line);
    return 0;
  }

  safety_trace("no enclosing loop bounds this index", access->location.line);
  return 0;
}

static int safety_prove_masked_index(const IRFunction *function,
                                     size_t check_index,
                                     const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }
  long long stride = 0;
  long long upper = 0;
  if (!safety_offset_upper_bound(function, check_index, access->offset, &stride,
                                 &upper)) {
    return 0;
  }
  long long highest = upper * stride;
  if (highest < 0 || access->size > access->extent) {
    return 0;
  }
  return highest <= access->extent - access->size;
}

static int safety_prove(IRFunction *function, const SafetyLoopList *loops,
                        size_t check_index, const SafetyAccess *access) {
  return safety_prove_constant(function, check_index, access) ||
         safety_prove_loop_bound(function, loops, check_index, access) ||
         safety_prove_masked_index(function, check_index, access);
}

typedef struct {
  size_t header_index;
  long long constant_length;
  long long diagnostic_size;
  long long stride;
  long long primary_step;
  long long index_step;
  long long primary_start;
  long long index_start;
  long long adjust;
  long long coeff;
  long long constant;
  long long size;
  long long access_kind;
  IROperand base;
  IROperand bound;
  const IROperand *identity;
  IROperand invariant;
  int has_invariant;
  IROperand index_start_value;
  int has_index_start;
  size_t invariant_at;
  const IRFunction *function;
  IRWhileLoopBounds bounds;
  SourceLocation location;
} SafetyHoist;

static int safety_parse_loop_form(const IRFunction *function,
                                  size_t header_index, SafetyLoopForm *form) {
  if (header_index + 4 >= function->instruction_count) {
    return 0;
  }
  const IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 0;
  }

  size_t branch_index = 0;
  int found_branch = 0;
  for (size_t i = header_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_BRANCH_ZERO) {
      branch_index = i;
      found_branch = 1;
      break;
    }
    if (instruction->op == IR_OP_LABEL || instruction->op == IR_OP_JUMP ||
        instruction->op == IR_OP_BRANCH_EQ) {
      return 0;
    }
  }
  if (!found_branch) {
    return 0;
  }

  const IRInstruction *branch = &function->instructions[branch_index];
  if (!branch->text || branch->lhs.kind != IR_OPERAND_TEMP ||
      !branch->lhs.name) {
    return 0;
  }
  const IRInstruction *compare =
      ir_find_temp_producer_before(function, branch_index, branch->lhs.name);
  if (!compare || compare->op != IR_OP_BINARY || compare->is_float ||
      !compare->text) {
    return 0;
  }
  size_t compare_index = (size_t)(compare - function->instructions);
  if (compare_index <= header_index) {
    return 0;
  }

  form->header_index = header_index;
  form->bounds.compare_index = compare_index;
  form->bounds.branch_index = branch_index;
  form->bounds.loop_label = header->text;
  form->bounds.exit_label = branch->text;
  form->bounds.jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_JUMP && instruction->text &&
        strcmp(instruction->text, form->bounds.loop_label) == 0) {
      form->bounds.jump_index = i;
      break;
    }
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, form->bounds.exit_label) == 0) {
      break;
    }
  }
  if (form->bounds.jump_index == (size_t)-1) {
    return 0;
  }

  form->has_index = 0;
  long long index_addend = 0;
  if (safety_index_is_affine(function, compare_index, &compare->lhs, &form->iv,
                             &index_addend)) {
    if (strcmp(compare->text, "<") == 0) {
      form->adjust = -index_addend - 1;
      form->has_index = 1;
    } else if (strcmp(compare->text, "<=") == 0) {
      form->adjust = -index_addend;
      form->has_index = 1;
    }
  }
  form->bound = &compare->rhs;
  return 1;
}

static int safety_operand_invariant_in(const IRFunction *function, size_t start,
                                       size_t end, const IROperand *operand) {
  if (operand->kind == IR_OPERAND_INT) {
    return 1;
  }
  if ((operand->kind != IR_OPERAND_SYMBOL && operand->kind != IR_OPERAND_TEMP) ||
      !operand->name) {
    return 0;
  }
  for (size_t i = start; i < end && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.name &&
        strcmp(instruction->dest.name, operand->name) == 0) {
      return 0;
    }
  }
  return 1;
}

static const IRProgram *g_safety_program;

static int safety_callee_can_release(const char *name, int depth);

static int safety_extern_cannot_release(const char *name) {
  static const char *const known[] = {
      "memcmp",  "memchr", "memcpy", "memmove", "memset",  "strlen",
      "strcmp",  "strncmp", "strchr", "strrchr", "strstr",  "strcpy",
      "strncpy", "strcat",  "abs",    "labs",    "llabs",
      "sin",     "cos",     "tan",    "asin",    "acos",    "atan",
      "atan2",   "sinh",    "cosh",   "tanh",    "exp",     "exp2",
      "log",     "log2",    "log10",  "pow",     "sqrt",    "cbrt",
      "fabs",    "floor",   "ceil",   "round",   "trunc",   "fmod",
      "fmin",    "fmax",    "sinf",   "cosf",    "tanf",    "expf",
      "logf",    "powf",    "sqrtf",  "fabsf",   "floorf",  "ceilf",
      "roundf",  "truncf",  "fmodf"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (strcmp(name, known[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int safety_name_releases_memory(const char *callee) {
  return strcmp(callee, "free") == 0 || strcmp(callee, "realloc") == 0 ||
         strcmp(callee, "mettle_heap_free") == 0 ||
         strcmp(callee, "mettle_heap_realloc") == 0;
}

static const IRFunction *safety_find_function(const char *name) {
  if (!g_safety_program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < g_safety_program->function_count; i++) {
    const IRFunction *function = g_safety_program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function;
    }
  }
  return NULL;
}

static int safety_name_is_functions_own(const IRFunction *function,
                                        const char *name) {
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names && function->parameter_names[i] &&
        strcmp(function->parameter_names[i], name) == 0) {
      return 1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

enum { SAFETY_CALLEE_CACHE_MAX = 512 };
typedef struct {
  const char *name;
  int verdict;
  int visiting;
} SafetyCalleeVerdict;
static SafetyCalleeVerdict g_safety_callee_cache[SAFETY_CALLEE_CACHE_MAX];
static int g_safety_callee_cache_count;

static SafetyCalleeVerdict *safety_callee_slot(const char *name) {
  for (int i = 0; i < g_safety_callee_cache_count; i++) {
    if (strcmp(g_safety_callee_cache[i].name, name) == 0) {
      return &g_safety_callee_cache[i];
    }
  }
  if (g_safety_callee_cache_count >= SAFETY_CALLEE_CACHE_MAX) {
    return NULL;
  }
  SafetyCalleeVerdict *slot =
      &g_safety_callee_cache[g_safety_callee_cache_count++];
  slot->name = name;
  slot->verdict = -1;
  slot->visiting = 0;
  return slot;
}

static int safety_function_can_release(const IRFunction *function, int depth) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    switch (in->op) {
    case IR_OP_CALL_INDIRECT:
    case IR_OP_GPU_LAUNCH:
    case IR_OP_INLINE_ASM:
      return 1;
    case IR_OP_CALL:
      if (!in->text || safety_callee_can_release(in->text, depth + 1)) {
        return 1;
      }
      break;
    default:
      break;
    }
    if (ir_instruction_writes_destination(in) &&
        ir_operand_is_symbol(&in->dest) &&
        !safety_name_is_functions_own(function, in->dest.name)) {
      return 1;
    }
  }
  return 0;
}

static int safety_callee_can_release(const char *name, int depth) {
  if (!name || depth > 16) {
    return 1;
  }
  IRInstruction probe = {0};
  probe.op = IR_OP_CALL;
  probe.text = (char *)name;
  for (size_t arity = 1; arity <= 8; arity++) {
    probe.argument_count = arity;
    IRSafetyIntrinsic kind = ir_safety_intrinsic(&probe);
    if (kind != IR_SAFETY_INTRINSIC_NONE)
      return kind == IR_SAFETY_INTRINSIC_LIFETIME;
  }
  if (strncmp(name, "mettle_safety_", 14) == 0) {
    return 0;
  }
  if (safety_name_releases_memory(name)) {
    return 1;
  }
  const IRFunction *callee = safety_find_function(name);
  if (!callee) {
    return !safety_extern_cannot_release(name);
  }
  SafetyCalleeVerdict *slot = safety_callee_slot(name);
  if (!slot) {
    return 1;
  }
  if (slot->verdict >= 0) {
    return slot->verdict;
  }
  if (slot->visiting) {
    return 0;
  }
  slot->visiting = 1;
  int verdict = safety_function_can_release(callee, depth);
  slot->visiting = 0;
  slot->verdict = verdict;
  return verdict;
}

static int safety_symbol_escapes(const IRFunction *function, const char *name) {
  if (!name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name && strcmp(in->lhs.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int safety_operand_escapes(const IRFunction *function,
                                  const IROperand *operand) {
  return operand && operand->kind == IR_OPERAND_SYMBOL &&
         safety_symbol_escapes(function, operand->name);
}

static int safety_body_calls_out(const IRFunction *function,
                                 const IRWhileLoopBounds *loop) {
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if ((in->op == IR_OP_CALL &&
         (!in->text || strncmp(in->text, "mettle_safety_", 14) != 0)) ||
        in->op == IR_OP_CALL_INDIRECT || in->op == IR_OP_NEW) {
      return 1;
    }
  }
  return 0;
}

static int safety_body_instruction_can_release(const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_CALL_INDIRECT:
  case IR_OP_GPU_LAUNCH:
  case IR_OP_INLINE_ASM:
    return 1;
  case IR_OP_CALL:
    return !in->text || safety_callee_can_release(in->text, 0);
  default:
    return 0;
  }
}

static int safety_body_has_no_calls(const IRFunction *function,
                                    const IRWhileLoopBounds *loop) {
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    if (safety_body_instruction_can_release(&function->instructions[i])) {
      return 0;
    }
  }
  return 1;
}

static int safety_label_is_in_body(const IRFunction *function,
                                   const IRWhileLoopBounds *loop,
                                   const char *label) {
  if (!label) {
    return 0;
  }
  for (size_t i = loop->branch_index + 1; i < loop->jump_index; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, label) == 0) {
      return 1;
    }
  }
  return 0;
}

static int safety_body_runs_access_every_iteration(
    const IRFunction *function, const IRWhileLoopBounds *loop,
    size_t access_index) {
  int seen_branch = 0;
  for (size_t i = loop->branch_index + 1; i < loop->jump_index; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    switch (instruction->op) {
    case IR_OP_RETURN:
      return 0;
    case IR_OP_LABEL:
      seen_branch = 1;
      continue;
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!safety_label_is_in_body(function, loop, instruction->text)) {
        return 0;
      }
      seen_branch = 1;
      continue;
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_GPU_LAUNCH:
    case IR_OP_INLINE_ASM:
      if (safety_body_instruction_can_release(instruction)) {
        return 0;
      }
      continue;
    default:
      continue;
    }
  }
  if (seen_branch) {
    for (size_t i = loop->branch_index + 1; i < loop->jump_index; i++) {
      IROpcode op = function->instructions[i].op;
      if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
          op == IR_OP_BRANCH_EQ) {
        return access_index < i;
      }
    }
  }
  return 1;
}

static int safety_symbol_written_in_body(const IRFunction *function,
                                         const IRWhileLoopBounds *loop,
                                         const char *symbol) {
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        strcmp(instruction->dest.name, symbol) == 0) {
      return 1;
    }
  }
  return 0;
}

static int safety_value_varies(const IRFunction *function, size_t before,
                               const IRWhileLoopBounds *loop,
                               const IROperand *operand, int depth) {
  if (!operand || depth > 8) {
    return 1;
  }
  if (operand->kind == IR_OPERAND_INT) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    return safety_symbol_written_in_body(function, loop, operand->name);
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 1;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, operand->name);
  if (!producer) {
    return 1;
  }
  size_t at = (size_t)(producer - function->instructions);
  if (at <= loop->branch_index) {
    return 0;
  }
  if (producer->op != IR_OP_BINARY && producer->op != IR_OP_ASSIGN &&
      producer->op != IR_OP_CAST) {
    return 1;
  }
  return safety_value_varies(function, at, loop, &producer->lhs, depth + 1) ||
         (producer->rhs.kind != IR_OPERAND_NONE &&
          safety_value_varies(function, at, loop, &producer->rhs, depth + 1));
}

static int safety_invariant_available(const IRFunction *function, size_t before,
                                      size_t header,
                                      const IRWhileLoopBounds *loop,
                                      const IROperand *operand, int depth) {
  if (!operand || depth > 6) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_INT) {
    return 1;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    return safety_operand_invariant_in(function, header, loop->jump_index,
                                       operand);
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, operand->name);
  if (!producer) {
    return 0;
  }
  size_t at = (size_t)(producer - function->instructions);
  if (at < header) {
    return 1;
  }
  if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST) {
    return safety_invariant_available(function, at, header, loop,
                                      &producer->lhs, depth + 1);
  }
  if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text) {
    return 0;
  }
  if (strcmp(producer->text, "+") != 0 && strcmp(producer->text, "-") != 0 &&
      strcmp(producer->text, "*") != 0 && strcmp(producer->text, "<<") != 0) {
    return 0;
  }
  return safety_invariant_available(function, at, header, loop, &producer->lhs,
                                    depth + 1) &&
         safety_invariant_available(function, at, header, loop, &producer->rhs,
                                    depth + 1);
}

typedef struct {
  long long coeff;
  const char *varying;
  const IROperand *invariant;
  long long constant;
} SafetyIndexForm;

static int safety_index_form_merge(SafetyIndexForm *into,
                                   const SafetyIndexForm *add, long long sign) {
  if (add->varying) {
    if (into->varying) {
      if (strcmp(into->varying, add->varying) != 0) {
        return 0;
      }
      into->coeff += sign * add->coeff;
    } else {
      into->varying = add->varying;
      into->coeff = sign * add->coeff;
    }
  }
  if (add->invariant) {
    if (into->invariant) {
      return 0;
    }
    if (sign < 0) {
      return 0;
    }
    into->invariant = add->invariant;
  }
  into->constant += sign * add->constant;
  return 1;
}

static int safety_read_index_form(const IRFunction *function, size_t before,
                                  size_t header, const IRWhileLoopBounds *loop,
                                  const IROperand *index, int depth,
                                  SafetyIndexForm *out) {
  if (!index || depth > 6) {
    return 0;
  }
  memset(out, 0, sizeof(*out));

  if (index->kind == IR_OPERAND_INT) {
    out->constant = index->int_value;
    return 1;
  }
  if (!safety_value_varies(function, before, loop, index, 0)) {
    if (!safety_invariant_available(function, before, header, loop, index, 0)) {
      return 0;
    }
    out->invariant = index;
    return 1;
  }
  if (index->kind == IR_OPERAND_SYMBOL && index->name) {
    out->varying = index->name;
    out->coeff = 1;
    return 1;
  }
  if (index->kind != IR_OPERAND_TEMP || !index->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, index->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  size_t at = (size_t)(producer - function->instructions);
  if (producer->op == IR_OP_ASSIGN) {
    return safety_read_index_form(function, at, header, loop, &producer->lhs,
                                  depth + 1, out);
  }
  if (producer->op != IR_OP_BINARY || !producer->text) {
    return 0;
  }

  SafetyIndexForm lhs;
  SafetyIndexForm rhs;
  if (!safety_read_index_form(function, at, header, loop, &producer->lhs,
                              depth + 1, &lhs) ||
      !safety_read_index_form(function, at, header, loop, &producer->rhs,
                              depth + 1, &rhs)) {
    return 0;
  }

  if (strcmp(producer->text, "+") == 0) {
    *out = lhs;
    return safety_index_form_merge(out, &rhs, 1);
  }
  if (strcmp(producer->text, "-") == 0) {
    *out = lhs;
    return safety_index_form_merge(out, &rhs, -1);
  }
  const SafetyIndexForm *scaled = NULL;
  long long factor = 0;
  if (strcmp(producer->text, "*") == 0) {
    if (!lhs.varying && !lhs.invariant) {
      scaled = &rhs;
      factor = lhs.constant;
    } else if (!rhs.varying && !rhs.invariant) {
      scaled = &lhs;
      factor = rhs.constant;
    }
  } else if (strcmp(producer->text, "<<") == 0 && !rhs.varying &&
             !rhs.invariant && rhs.constant >= 0 && rhs.constant < 32) {
    scaled = &lhs;
    factor = 1LL << rhs.constant;
  }
  if (!scaled || (scaled->invariant && factor != 1)) {
    return 0;
  }
  out->varying = scaled->varying;
  out->coeff = scaled->coeff * factor;
  out->invariant = scaled->invariant;
  out->constant = scaled->constant * factor;
  return 1;
}

static int safety_iv_start_at_header(const IRFunction *function,
                                     size_t header_index,
                                     const IRWhileLoopBounds *loop,
                                     const char *iv, long long *start_out,
                                     const IROperand **start_operand_out) {
  *start_out = 0;
  *start_operand_out = NULL;
  for (size_t i = header_index; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
      return 0;
    }
    if (!ir_instruction_writes_destination(ins) ||
        !ir_operand_is_symbol_named(&ins->dest, iv)) {
      continue;
    }
    if (ins->op == IR_OP_CAST) {
      if (ins->is_float || !ins->text ||
          (strcmp(ins->text, "int64") != 0 &&
           strcmp(ins->text, "uint64") != 0)) {
        return 0;
      }
    } else if (ins->op != IR_OP_ASSIGN) {
      return 0;
    }
    if (ins->lhs.kind == IR_OPERAND_INT) {
      *start_out = ins->lhs.int_value;
      return 1;
    }
    const IROperand *source = &ins->lhs;
    if (source->kind == IR_OPERAND_TEMP && source->name) {
      const IRInstruction *p =
          ir_find_temp_producer_before(function, i, source->name);
      if (p && (p->op == IR_OP_CAST || p->op == IR_OP_ASSIGN) && !p->is_float) {
        if (p->lhs.kind == IR_OPERAND_INT) {
          *start_out = p->lhs.int_value;
          return 1;
        }
        source = &p->lhs;
      }
    }
    if (safety_invariant_available(function, i, header_index, loop, source, 0)) {
      *start_operand_out = source;
      return 1;
    }
    return 0;
  }
  return 0;
}

static int safety_hoist_masked(IRFunction *function,
                               const SafetyLoopList *loops,
                               size_t check_index,
                               const SafetyAccess *access, long long stride,
                               long long upper, SafetyHoist *out) {
  const SafetyLoopForm *innermost = safety_enclosing_loop(loops, check_index);
  if (!innermost) {
    safety_trace("the index is bounded but there is no loop to lift the "
                 "check out of",
                 access->location.line);
    return 0;
  }
  long long start = 0;
  const IROperand *start_operand = NULL;
  if (!innermost->has_index ||
      !safety_iv_start_at_header(function, innermost->header_index,
          &innermost->bounds, innermost->iv, &start, &start_operand) ||
      start_operand ||
      (innermost->adjust < 0 && start > LLONG_MAX + innermost->adjust) ||
      (innermost->adjust > 0 && start < LLONG_MIN + innermost->adjust) ||
      stride <= 0 || upper < 0 ||
      upper > (LLONG_MAX - access->size) / stride ||
      !safety_operand_invariant_in(function, innermost->header_index,
          innermost->bounds.jump_index, innermost->bound) ||
      !safety_body_runs_access_every_iteration(function, &innermost->bounds,
                                               check_index) ||
      !safety_operand_invariant_in(function, innermost->header_index,
                                   innermost->bounds.jump_index,
                                   access->base)) {
    return 0;
  }
  out->header_index = innermost->header_index;
  out->primary_start = start;
  out->adjust = innermost->adjust;
  if (!ir_operand_clone(innermost->bound, &out->bound)) return 0;
  out->constant_length = upper * stride + access->size;
  out->access_kind = access->access_kind;
  out->location = access->location;
  return ir_operand_clone(access->base, &out->base);
}

static int safety_try_hoist(IRFunction *function, const SafetyLoopList *loops,
                            size_t check_index, const SafetyAccess *access,
                            SafetyHoist *out) {
  if (access->extent != IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }
  const SafetyLoopForm *identity_loop = safety_enclosing_loop(loops, check_index);
  if (access->identity && (!identity_loop ||
      !safety_operand_invariant_in(function, identity_loop->header_index,
          identity_loop->bounds.jump_index, access->identity))) return 0;
  out->identity = access->identity;
  out->diagnostic_size = access->diagnostic_size;

  long long masked_stride = 0;
  long long masked_upper = 0;
  int masked = safety_offset_upper_bound(function, check_index, access->offset,
                                         &masked_stride, &masked_upper);

  const IROperand *index = NULL;
  long long stride = 0;
  if (!masked && !safety_offset_scaling(function, check_index, access->offset,
                                        &index, &stride)) {
    safety_trace("the offset is not an index scaled by a constant width",
                 access->location.line);
    return 0;
  }

  if (masked) {
    return safety_hoist_masked(function, loops, check_index, access,
                               masked_stride, masked_upper, out);
  }

  int saw_loop = 0;
  for (size_t loop_index = loops->count; loop_index-- > 0;) {
    SafetyLoopForm form = loops->items[loop_index];
    size_t header = form.header_index;
    if (check_index <= form.bounds.branch_index ||
        check_index >= form.bounds.jump_index || !form.has_index) {
      continue;
    }
    saw_loop = 1;

    SafetyIndexForm shape;
    if (!safety_read_index_form(function, check_index, header, &form.bounds,
                                index, 0, &shape)) {
      safety_trace("the index is neither a line in one counter nor bounded by "
                   "its own arithmetic",
                   access->location.line);
      return 0;
    }
    if (!shape.varying || shape.coeff == 0) {
      continue;
    }
    if (access->diagnostic_size && shape.coeff < 0) return 0;

    size_t primary_first = 0;
    size_t primary_last = 0;
    long long primary_start = 0;
    const IROperand *primary_start_operand = NULL;
    if (!safety_iv_start_at_header(function, header, &form.bounds, form.iv,
                                   &primary_start, &primary_start_operand) ||
        primary_start_operand ||
        !safety_loop_step(function, &form.bounds, form.iv, &form.step,
                          &primary_first, &primary_last)) {
      safety_trace("the tested variable is not a counter that starts at a "
                   "known value and steps by a constant",
                   access->location.line);
      return 0;
    }

    long long index_step = form.step;
    long long index_start = primary_start;
    const IROperand *index_start_operand = NULL;
    if (strcmp(form.iv, shape.varying) != 0) {
      size_t index_first = 0;
      size_t index_last = 0;
      if (!safety_iv_start_at_header(function, header, &form.bounds,
                                     shape.varying, &index_start,
                                     &index_start_operand)) {
        safety_trace("the indexing variable does not start from a value "
                     "settled before the loop",
                     access->location.line);
        return 0;
      }
      if (!safety_loop_step(function, &form.bounds, shape.varying, &index_step,
                            &index_first, &index_last)) {
        safety_trace("the indexing variable does not step by a constant",
                     access->location.line);
        return 0;
      }
    }

    if (!safety_body_runs_access_every_iteration(function, &form.bounds,
                                                 check_index)) {
      safety_trace("the loop body can skip this access or leave early, or "
                   "calls something that could free what it walks",
                   access->location.line);
      return 0;
    }
    if (safety_body_calls_out(function, &form.bounds) &&
        (safety_operand_escapes(function, access->base) ||
         safety_operand_escapes(function, form.bound) ||
         safety_symbol_escapes(function, form.iv) ||
         safety_symbol_escapes(function, shape.varying))) {
      safety_trace("the loop hands out the address of its pointer, bound or "
                   "counter, so a call in the body could move it",
                   access->location.line);
      return 0;
    }
    if (!safety_operand_invariant_in(function, header, form.bounds.jump_index,
                                     access->base) ||
        (access->identity && !safety_operand_invariant_in(function, header,
            form.bounds.jump_index, access->identity)) ||
        !safety_operand_invariant_in(function, header, form.bounds.jump_index,
                                     form.bound)) {
      safety_trace("the pointer or the loop bound is not settled before the "
                   "loop starts",
                   access->location.line);
      return 0;
    }

    out->header_index = header;
    if (shape.coeff == LLONG_MIN || stride <= 0 || index_step <= 0 ||
        (form.adjust < 0 && primary_start > LLONG_MAX + form.adjust) ||
        (form.adjust > 0 && primary_start < LLONG_MIN + form.adjust)) return 0;
    unsigned long long reach = (unsigned long long)(shape.coeff < 0 ?
                                                    -shape.coeff : shape.coeff);
    if (reach > (unsigned long long)LLONG_MAX / (unsigned long long)stride ||
        reach * (unsigned long long)stride >
            (unsigned long long)LLONG_MAX / (unsigned long long)index_step) return 0;
    out->stride = stride;
    out->primary_step = form.step;
    out->index_step = index_step;
    out->primary_start = primary_start;
    out->index_start = index_start;
    out->adjust = form.adjust;
    out->coeff = shape.coeff;
    out->constant = shape.constant;
    out->size = access->size;
    out->access_kind = access->access_kind;
    out->location = access->location;
    out->function = function;
    out->bounds = form.bounds;
    out->has_invariant = 0;
    out->has_index_start = 0;
    out->invariant_at = check_index;
    if (!ir_operand_clone(access->base, &out->base)) {
      return 0;
    }
    if (!ir_operand_clone(form.bound, &out->bound)) {
      ir_operand_destroy(&out->base);
      return 0;
    }
    if (shape.invariant) {
      if (!ir_operand_clone(shape.invariant, &out->invariant)) {
        ir_operand_destroy(&out->base);
        ir_operand_destroy(&out->bound);
        return 0;
      }
      out->has_invariant = 1;
    }
    if (index_start_operand) {
      if (!ir_operand_clone(index_start_operand, &out->index_start_value)) {
        ir_operand_destroy(&out->base);
        ir_operand_destroy(&out->bound);
        if (out->has_invariant) {
          ir_operand_destroy(&out->invariant);
        }
        return 0;
      }
      out->has_index_start = 1;
    }
    return 1;
  }
  safety_trace(saw_loop ? "the enclosing loop does not step this index"
                        : "no enclosing loop this pass can read",
               access->location.line);
  return 0;
}

enum { SAFETY_BUILD_MAX = 32 };

typedef struct {
  IRInstructionVector *out;
  SourceLocation location;
  unsigned id;
  int seq;
  int failed;
  IROperand owned[SAFETY_BUILD_MAX];
  int owned_count;
  IROperand nowhere;
} SafetyBuild;

static void safety_build_init(SafetyBuild *b, IRInstructionVector *out,
                              SourceLocation location) {
  memset(b, 0, sizeof(*b));
  b->out = out;
  b->location = location;
  b->id = g_safety_next_id++;
}

static void safety_build_release(SafetyBuild *b) {
  for (int i = 0; i < b->owned_count; i++) {
    ir_operand_destroy(&b->owned[i]);
  }
  b->owned_count = 0;
}

static int safety_operand_is_usable(const IROperand *operand) {
  if (operand->kind == IR_OPERAND_INT || operand->kind == IR_OPERAND_FLOAT) {
    return 1;
  }
  return (operand->kind == IR_OPERAND_TEMP ||
          operand->kind == IR_OPERAND_SYMBOL) &&
         operand->name != NULL;
}

static const IROperand *safety_build_keep(SafetyBuild *b, IROperand *value) {
  if (b->failed || b->owned_count >= SAFETY_BUILD_MAX ||
      !safety_operand_is_usable(value)) {
    b->failed = 1;
    ir_operand_destroy(value);
    return &b->nowhere;
  }
  b->owned[b->owned_count] = *value;
  return &b->owned[b->owned_count++];
}

static const IROperand *safety_build(SafetyBuild *b, const char *op,
                                     const IROperand *lhs,
                                     const IROperand *rhs) {
  if (b->failed) {
    return lhs;
  }
  char name[64];
  snprintf(name, sizeof(name), SAFETY_TEMP_PREFIX "h%u_%d", b->id, b->seq++);
  if (!safety_emit_binary(b->out, b->location, op, name, lhs, rhs, 0)) {
    b->failed = 1;
    return lhs;
  }
  IROperand fresh = ir_operand_temp(name);
  return safety_build_keep(b, &fresh);
}

static const IROperand *safety_build_invariant(SafetyBuild *b,
                                               const IRFunction *function,
                                               size_t before, size_t header,
                                               const IRWhileLoopBounds *loop,
                                               const IROperand *operand,
                                               int depth) {
  if (b->failed || depth > 6) {
    b->failed = 1;
    return operand;
  }
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    const IRInstruction *producer =
        ir_find_temp_producer_before(function, before, operand->name);
    if (producer) {
      size_t at = (size_t)(producer - function->instructions);
      if (at >= header) {
        if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST) {
          return safety_build_invariant(b, function, at, header, loop,
                                        &producer->lhs, depth + 1);
        }
        if (producer->op == IR_OP_BINARY && producer->text) {
          const IROperand *lhs = safety_build_invariant(
              b, function, at, header, loop, &producer->lhs, depth + 1);
          const IROperand *rhs = safety_build_invariant(
              b, function, at, header, loop, &producer->rhs, depth + 1);
          return safety_build(b, producer->text, lhs, rhs);
        }
        b->failed = 1;
        return operand;
      }
    }
  }
  IROperand copy;
  if (!ir_operand_clone(operand, &copy)) {
    b->failed = 1;
    return operand;
  }
  return safety_build_keep(b, &copy);
}

static int safety_emit_hoisted(IRInstructionVector *out,
                               const SafetyHoist *hoist) {
  if (hoist->constant_length > 0) {
    SafetyBuild build;
    safety_build_init(&build, out, hoist->location);
    IROperand threshold = ir_operand_int(hoist->primary_start - hoist->adjust);
    IROperand length = ir_operand_int(hoist->constant_length);
    const IROperand *runs = safety_build(&build, ">=", &hoist->bound, &threshold);
    const IROperand *guarded = safety_build(&build, "*", runs, &length);
    IROperand arguments[8];
    if (build.failed || !ir_operand_clone(&hoist->base, &arguments[0])) {
      safety_build_release(&build);
      return 0;
    }
    arguments[1] = ir_operand_int(0);
    arguments[2] = *guarded;
    arguments[3] = ir_operand_int(hoist->access_kind);
    arguments[4] = ir_operand_int((long long)hoist->location.line);
    if (hoist->identity) arguments[5] = *hoist->identity;
    int emitted = safety_emit_call(out, hoist->location,
        hoist->identity ? "mettle_safety_check_identity" : "mettle_safety_check",
        arguments, hoist->identity ? 6 : 5);
    ir_operand_destroy(&arguments[0]);
    safety_build_release(&build);
    return emitted;
  }

  SafetyBuild build;
  safety_build_init(&build, out, hoist->location);

  IROperand adjust = ir_operand_int(hoist->adjust);
  IROperand primary_start = ir_operand_int(hoist->primary_start);
  IROperand primary_step = ir_operand_int(hoist->primary_step);
  IROperand index_step = ir_operand_int(hoist->index_step);
  IROperand stride = ir_operand_int(hoist->stride);
  IROperand size = ir_operand_int(hoist->size);
  const IROperand *top =
      safety_build(&build, "+", &hoist->bound, &adjust);
  const IROperand *span = top;
  if (hoist->primary_start != 0) {
    span = safety_build(&build, "-", top, &primary_start);
  }

  const IROperand *rounds = span;
  if (hoist->primary_step > 1) {
    rounds = safety_build(&build, "/", span, &primary_step);
  }
  const IROperand *travel = rounds;
  if (hoist->index_step != 1) {
    travel = safety_build(&build, "*", rounds, &index_step);
  }

  long long reach = hoist->coeff < 0 ? -hoist->coeff : hoist->coeff;
  IROperand per_step = ir_operand_int(reach * hoist->stride);
  IROperand length_args[5] = {
      hoist->bound, ir_operand_int(hoist->primary_start - hoist->adjust),
      primary_step, ir_operand_int(per_step.int_value * hoist->index_step), size};
  if (!safety_emit_call(out, hoist->location, "mettle_safety_loop_length",
                        length_args, 5)) {
    safety_build_release(&build);
    return 0;
  }
  char length_name[64];
  snprintf(length_name, sizeof(length_name), SAFETY_TEMP_PREFIX "length%u", g_safety_next_id++);
  out->items[out->count - 1].dest = ir_operand_temp(length_name);
  if (!out->items[out->count - 1].dest.name) {
    safety_build_release(&build);
    return 0;
  }
  IROperand guarded_length = ir_operand_temp(length_name);
  const IROperand *guarded = safety_build_keep(&build, &guarded_length);

  IROperand index_start = ir_operand_int(hoist->index_start);
  const IROperand *counter = &index_start;
  if (hoist->has_index_start) {
    const IROperand *from = safety_build_invariant(
        &build, hoist->function, hoist->invariant_at, hoist->header_index,
        &hoist->bounds, &hoist->index_start_value, 0);
    counter = hoist->index_start != 0
                  ? safety_build(&build, "+", from, &index_start)
                  : from;
  }
  if (hoist->coeff < 0) {
    counter = safety_build(&build, "+", counter, travel);
  }

  const IROperand *low = counter;
  if (hoist->coeff != 1) {
    IROperand coeff = ir_operand_int(hoist->coeff);
    low = safety_build(&build, "*", counter, &coeff);
  }
  if (hoist->constant != 0) {
    IROperand constant = ir_operand_int(hoist->constant);
    low = safety_build(&build, "+", low, &constant);
  }
  if (hoist->has_invariant) {
    const IROperand *displacement = safety_build_invariant(
        &build, hoist->function, hoist->invariant_at, hoist->header_index,
        &hoist->bounds, &hoist->invariant, 0);
    low = safety_build(&build, "+", low, displacement);
  }
  const IROperand *offset = low;
  if (hoist->stride != 1) {
    offset = safety_build(&build, "*", low, &stride);
  }

  int ok = 0;
  if (!build.failed) {
    IROperand arguments[8];
    if (ir_operand_clone(&hoist->base, &arguments[0])) {
      arguments[1] = *offset;
      arguments[2] = *guarded;
      arguments[3] = ir_operand_int(hoist->access_kind);
      arguments[4] = ir_operand_int((long long)hoist->location.line);
      if (hoist->identity) arguments[5] = *hoist->identity;
      if (hoist->diagnostic_size && hoist->identity) {
        arguments[6] = ir_operand_int(hoist->diagnostic_size);
        arguments[7] = ir_operand_int(per_step.int_value * hoist->index_step);
        ok = safety_emit_call(out, hoist->location,
            "mettle_safety_check_affine", arguments, 8);
      } else {
        ok = safety_emit_call(out, hoist->location,
            hoist->identity ? "mettle_safety_check_identity" : "mettle_safety_check",
            arguments, hoist->identity ? 6 : 5);
      }
      ir_operand_destroy(&arguments[0]);
    }
  }
  safety_build_release(&build);
  return ok;
}

static int safety_expand_extent(IRInstructionVector *out,
                                const SafetyAccess *access) {
  char ok_label[64];
  char condition[64];
  char index_temp[64];
  unsigned id = g_safety_next_id++;
  snprintf(ok_label, sizeof(ok_label), SAFETY_LABEL_PREFIX "%u", id);
  snprintf(condition, sizeof(condition), SAFETY_TEMP_PREFIX "c%u", id);
  snprintf(index_temp, sizeof(index_temp), SAFETY_TEMP_PREFIX "i%u", id);

  char message[192];
  snprintf(message, sizeof(message), "Fatal error: `%s` is outside its bounds",
           access->what);

  if (access->size > access->extent) {
    IROperand arguments[4];
    arguments[0] = ir_operand_int(2);
    arguments[1] = ir_operand_string(message);
    arguments[2] = ir_operand_int(0);
    arguments[3] = ir_operand_int(0);
    int ok = safety_emit_call(out, access->location, "mettle_crash_trap_ex",
                              arguments, 4);
    for (size_t i = 0; i < 4; i++) {
      ir_operand_destroy(&arguments[i]);
    }
    return ok;
  }

  IROperand limit = ir_operand_int(access->extent - access->size);
  int emitted = safety_emit_binary(out, access->location, ">", condition,
                                   access->offset, &limit, 1) &&
                safety_emit_branch_zero(out, access->location, condition,
                                        ok_label);
  ir_operand_destroy(&limit);
  if (!emitted) {
    return 0;
  }

  IROperand element_size = ir_operand_int(access->size);
  int trapped = safety_emit_binary(out, access->location, "/", index_temp,
                                   access->offset, &element_size, 0);
  ir_operand_destroy(&element_size);
  if (!trapped) {
    return 0;
  }

  IROperand arguments[4];
  arguments[0] = ir_operand_int(2);
  arguments[1] = ir_operand_string(message);
  arguments[2] = ir_operand_temp(index_temp);
  arguments[3] = ir_operand_int(access->extent / access->size);
  int ok = arguments[1].kind == IR_OPERAND_STRING && arguments[2].name &&
           safety_emit_call(out, access->location, "mettle_crash_trap_ex",
                            arguments, 4);
  for (size_t i = 0; i < 4; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  if (!ok) {
    return 0;
  }

  return safety_emit_label(out, access->location, ok_label);
}

static int safety_expand_region(IRInstructionVector *out,
                                const SafetyAccess *access) {
  IROperand arguments[6];
  size_t built = 0;
  int ok = 0;

  if (!ir_operand_clone(access->base, &arguments[0])) {
    return 0;
  }
  built = 1;
  if (!ir_operand_clone(access->offset, &arguments[1])) {
    goto done;
  }
  built = 2;
  arguments[2] = ir_operand_int(access->size);
  arguments[3] = ir_operand_int(access->access_kind);
  arguments[4] = ir_operand_int((long long)access->location.line);
  built = 5;

  if (access->identity) arguments[5] = *access->identity;
  ok = safety_emit_call(out, access->location,
      access->identity ? "mettle_safety_check_identity" : "mettle_safety_check",
      arguments, access->identity ? 6 : 5);

done:
  for (size_t i = 0; i < built; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

static int safety_expand_region(IRInstructionVector *out,
                                const SafetyAccess *access);

typedef struct {
  size_t header_index;
  IROperand base;
  const IROperand *identity;
  char temp[64];
  SourceLocation location;
} SafetySpan;

static const IROperand *safety_base_root(const IRFunction *function,
                                         size_t before, const IROperand *base,
                                         const IROperand **delta_out,
                                         size_t *delta_from, int depth) {
  if (base->kind != IR_OPERAND_TEMP || !base->name || depth > 4) {
    return base;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, base->name);
  if (!producer || producer->is_float) {
    return base;
  }
  size_t producer_index = (size_t)(producer - function->instructions);

  if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST) {
    if (producer->lhs.kind != IR_OPERAND_SYMBOL &&
        producer->lhs.kind != IR_OPERAND_TEMP) {
      return base;
    }
    return safety_base_root(function, producer_index, &producer->lhs,
                            delta_out, delta_from, depth + 1);
  }

  if (producer->op == IR_OP_BINARY && producer->text &&
      strcmp(producer->text, "+") == 0 && !*delta_out &&
      (producer->lhs.kind == IR_OPERAND_SYMBOL ||
       producer->lhs.kind == IR_OPERAND_TEMP)) {
    *delta_out = &producer->rhs;
    *delta_from = producer_index;
    return safety_base_root(function, producer_index, &producer->lhs,
                            delta_out, delta_from, depth + 1);
  }
  return base;
}

static int safety_operand_same(const IROperand *a, const IROperand *b) {
  if (a->kind != b->kind) {
    return 0;
  }
  if (a->kind == IR_OPERAND_INT) {
    return a->int_value == b->int_value;
  }
  return a->name && b->name && ir_operand_names_match(a, b);
}

static int safety_emit_span_resolve(IRInstructionVector *out,
                                    const SafetySpan *span) {
  IRInstruction call = {0};
  call.op = IR_OP_CALL;
  call.location = span->location;
  call.text = mettle_strdup(span->identity ? "mettle_safety_span_identity" :
                                           "mettle_safety_span");
  call.dest = ir_operand_temp(span->temp);
  call.arguments = calloc(span->identity ? 2 : 1, sizeof(IROperand));
  if (!call.text || !call.dest.name || !call.arguments) {
    ir_instruction_destroy_storage(&call);
    return 0;
  }
  call.argument_count = span->identity ? 2 : 1;
  if (span->identity && !ir_operand_clone(span->identity, &call.arguments[1])) {
    ir_instruction_destroy_storage(&call);
    return 0;
  }
  if (!ir_operand_clone(&span->base, &call.arguments[0]) ||
      !ir_instruction_vector_append_move(out, &call)) {
    ir_instruction_destroy_storage(&call);
    return 0;
  }
  return 1;
}

static int safety_emit_span_check(IRInstructionVector *out,
                                  const SafetyAccess *access,
                                  const char *span_temp,
                                  const IROperand *delta) {
  unsigned id = g_safety_next_id++;
  char limit[64];
  char total[64];
  char bad[64];
  char short_span[64];
  char both[64];
  char ok_label[64];
  snprintf(limit, sizeof(limit), SAFETY_TEMP_PREFIX "sl%u", id);
  snprintf(total, sizeof(total), SAFETY_TEMP_PREFIX "st%u", id);
  snprintf(bad, sizeof(bad), SAFETY_TEMP_PREFIX "sb%u", id);
  snprintf(short_span, sizeof(short_span), SAFETY_TEMP_PREFIX "ss%u", id);
  snprintf(both, sizeof(both), SAFETY_TEMP_PREFIX "sx%u", id);
  snprintf(ok_label, sizeof(ok_label), "ir_safe_in_%u", id);

  IROperand span_operand = ir_operand_temp(span_temp);
  IROperand limit_operand = ir_operand_temp(limit);
  IROperand total_operand = ir_operand_temp(total);
  IROperand size_operand = ir_operand_int(access->size);
  int ok = 0;

  if (!span_operand.name || !limit_operand.name || !total_operand.name) {
    goto done;
  }
  if (!safety_emit_binary(out, access->location, "-", limit, &span_operand,
                          &size_operand, 0)) {
    goto done;
  }

  IROperand zero_offset = ir_operand_int(0);
  const IROperand *offset =
      (access->offset && access->offset->kind != IR_OPERAND_NONE)
          ? access->offset
          : &zero_offset;

  const IROperand *measured = offset;
  if (delta && delta->kind != IR_OPERAND_NONE) {
    if (!safety_emit_binary(out, access->location, "+", total, offset, delta,
                            0)) {
      goto done;
    }
    measured = &total_operand;
  }

  if (!safety_emit_binary(out, access->location, ">", bad, measured,
                          &limit_operand, 1)) {
    goto done;
  }

  const char *condition = bad;
  if (access->identity) {
    IROperand bad_operand = ir_operand_temp(bad);
    IROperand short_operand = ir_operand_temp(short_span);
    IROperand zero = ir_operand_int(0);
    int guarded = bad_operand.name && short_operand.name &&
        safety_emit_binary(out, access->location, "<", short_span,
                           &limit_operand, &zero, 0) &&
        safety_emit_binary(out, access->location, "|", both, &bad_operand,
                           &short_operand, 0);
    ir_operand_destroy(&bad_operand);
    ir_operand_destroy(&short_operand);
    if (!guarded) {
      goto done;
    }
    condition = both;
  }
  if (!safety_emit_branch_zero(out, access->location, condition, ok_label)) {
    goto done;
  }
  ok = safety_expand_region(out, access) &&
       safety_emit_label(out, access->location, ok_label);

done:
  ir_operand_destroy(&span_operand);
  ir_operand_destroy(&limit_operand);
  ir_operand_destroy(&total_operand);
  return ok;
}

static size_t safety_block_start(const IRFunction *function, size_t index) {
  for (size_t i = index; i-- > 0;) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
        op == IR_OP_BRANCH_EQ || op == IR_OP_RETURN) {
      return i + 1;
    }
  }
  return 0;
}

static const IROperand *safety_identity_root(const IRFunction *function,
                                             const IROperand *identity,
                                             size_t before, int depth) {
  if (!identity || !identity->name || depth > 16 ||
      (identity->kind != IR_OPERAND_TEMP && identity->kind != IR_OPERAND_SYMBOL)) {
    return identity;
  }
  const IRInstruction *definition = NULL;
  size_t at = 0;
  for (size_t i = safety_block_start(function, before); i < before; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL || !ir_instruction_writes_destination(in) ||
        !safety_operand_same(&in->dest, identity)) {
      continue;
    }
    definition = in;
    at = i;
  }
  if (!definition) {
    return identity;
  }
  if (definition->op == IR_OP_ASSIGN) {
    return safety_identity_root(function, &definition->lhs, at, depth + 1);
  }
  if (definition->op == IR_OP_CALL && definition->text &&
      definition->argument_count == 2 && definition->arguments &&
      (strcmp(definition->text, "mettle_safety_merge_identity") == 0 ||
       strcmp(definition->text, "mettle_safety_subtract_identity") == 0)) {
    const IROperand *left = &definition->arguments[0];
    const IROperand *right = &definition->arguments[1];
    if (right->kind == IR_OPERAND_INT && right->int_value == 0) {
      return safety_identity_root(function, left, at, depth + 1);
    }
    if (left->kind == IR_OPERAND_INT && left->int_value == 0) {
      return safety_identity_root(function, right, at, depth + 1);
    }
  }
  return identity;
}

static const SafetyLoopForm *safety_enclosing_loop(const SafetyLoopList *loops,
                                                   size_t index) {
  for (size_t i = loops->count; i-- > 0;) {
    const SafetyLoopForm *loop = &loops->items[i];
    if (index > loop->bounds.branch_index && index < loop->bounds.jump_index) {
      return loop;
    }
  }
  return NULL;
}

static void safety_loop_list_destroy(SafetyLoopList *loops) {
  free(loops->items);
  loops->items = NULL;
  loops->count = 0;
  loops->capacity = 0;
}

static int safety_loop_list_build(IRFunction *function, SafetyLoopList *loops) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_LABEL ||
        !ir_label_is_while_header(instruction->text)) {
      continue;
    }
    SafetyLoopForm form;
    if (!safety_parse_loop_form(function, i, &form)) {
      continue;
    }
    if (loops->count == loops->capacity) {
      size_t capacity = loops->capacity ? loops->capacity * 2 : 8;
      SafetyLoopForm *grown =
          realloc(loops->items, capacity * sizeof(SafetyLoopForm));
      if (!grown) {
        safety_loop_list_destroy(loops);
        return 0;
      }
      loops->items = grown;
      loops->capacity = capacity;
    }
    loops->items[loops->count++] = form;
  }
  return 1;
}

static const char *safety_allocator_source(const IRProgram *program) {
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *function = program->functions[i];
    if (function && function->name &&
        strncmp(function->name, "mettle_heap_", 12) == 0) {
      return function->location.filename;
    }
  }
  return NULL;
}

static int safety_function_is_allocator(const IRFunction *function,
                                        const char *allocator_source) {
  if (!allocator_source || !function) {
    return 0;
  }
  if (function->name && strncmp(function->name, "mettle_heap_", 12) == 0) {
    return 1;
  }
  return function->location.filename &&
         strcmp(function->location.filename, allocator_source) == 0;
}

static int safety_strip_function(IRFunction *function, IRSafetyStats *stats) {
  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_SAFETY_CHECK) {
      if (stats) {
        stats->emitted++;
        stats->exempt++;
      }
      continue;
    }
    if (!ir_instruction_vector_append_move(&out, instruction)) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  return 1;
}

static int safety_resolve_function(IRFunction *function, IRSafetyStats *stats) {
  size_t check_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_SAFETY_CHECK) {
      check_count++;
    }
  }
  if (check_count == 0) {
    return 1;
  }

  enum {
    SAFETY_KEEP = 0,
    SAFETY_PROVED = 1,
    SAFETY_HOISTED = 2,
    SAFETY_SPANNED = 3
  };
  unsigned char *outcome = calloc(function->instruction_count, 1);
  SafetyHoist *hoists = calloc(check_count, sizeof(SafetyHoist));
  SafetySpan *spans = calloc(check_count, sizeof(SafetySpan));
  size_t *span_of = calloc(function->instruction_count, sizeof(size_t));
  const IROperand **span_delta =
      calloc(function->instruction_count, sizeof(const IROperand *));
  size_t hoist_count = 0;
  size_t span_count = 0;
  SafetyLoopList loops = {0};
  if (!outcome || !hoists || !spans || !span_of || !span_delta) {
    free(outcome);
    free(hoists);
    free(spans);
    free(span_of);
    free(span_delta);
    return 0;
  }
  if (!safety_loop_list_build(function, &loops)) {
    free(outcome);
    free(hoists);
    free(spans);
    free(span_of);
    free(span_delta);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_SAFETY_CHECK) {
      continue;
    }
    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      goto fail;
    }
    if (safety_prove(function, &loops, i, &access)) {
      outcome[i] = SAFETY_PROVED;
      continue;
    }
    if (safety_try_hoist(function, &loops, i, &access, &hoists[hoist_count])) {
      hoist_count++;
      outcome[i] = SAFETY_HOISTED;
      continue;
    }

    const SafetyLoopForm *loop = safety_enclosing_loop(&loops, i);
    if (!loop) {
      safety_trace("not in any loop, so there is nothing to resolve against",
                   access.location.line);
      continue;
    }
    const IROperand *identity_root =
        safety_identity_root(function, access.identity, i, 0);
    if (identity_root && !safety_operand_invariant_in(function,
        loop->header_index, loop->bounds.jump_index, identity_root)) {
      safety_trace("the pointer origin is re-read inside the loop, so one "
                   "resolution would not describe every iteration",
                   access.location.line);
      continue;
    }
    if (!safety_body_has_no_calls(function, &loop->bounds)) {
      safety_trace("the loop calls something that could free what it walks",
                   access.location.line);
      continue;
    }
    if (safety_body_calls_out(function, &loop->bounds) &&
        safety_operand_escapes(function, access.base)) {
      safety_trace("the loop hands out the address of the pointer it walks, so "
                   "a call in the body could move it",
                   access.location.line);
      continue;
    }
    const IROperand *delta = NULL;
    size_t delta_from = 0;
    const IROperand *root =
        safety_base_root(function, i, access.base, &delta, &delta_from, 0);
    if (!safety_operand_invariant_in(function, loop->header_index,
                                     loop->bounds.jump_index, root)) {
      safety_trace("the pointer moves inside the loop and is not a fixed one "
                   "displaced, so one resolution would not describe it",
                   access.location.line);
      continue;
    }
    if (delta && !safety_operand_invariant_in(function, delta_from + 1, i,
                                              delta)) {
      safety_trace("the displacement changes between forming the pointer and "
                   "using it",
                   access.location.line);
      continue;
    }

    size_t found = span_count;
    for (size_t s = 0; s < span_count; s++) {
      if (spans[s].header_index == loop->header_index &&
          safety_operand_same(&spans[s].base, root) &&
          ((!spans[s].identity && !identity_root) ||
           (spans[s].identity && identity_root &&
            safety_operand_same(spans[s].identity, identity_root)))) {
        found = s;
        break;
      }
    }
    if (found == span_count) {
      SafetySpan *fresh = &spans[span_count];
      fresh->header_index = loop->header_index;
      fresh->identity = identity_root;
      fresh->location = access.location;
      snprintf(fresh->temp, sizeof(fresh->temp), SAFETY_TEMP_PREFIX "sp%u",
               g_safety_next_id++);
      if (!ir_operand_clone(root, &fresh->base)) {
        goto fail;
      }
      span_count++;
    }
    span_of[i] = found;
    span_delta[i] = delta;
    outcome[i] = SAFETY_SPANNED;
  }
  safety_loop_list_destroy(&loops);

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count + 16)) {
    goto fail;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    for (size_t h = 0; h < hoist_count; h++) {
      if (hoists[h].header_index == i &&
          !safety_emit_hoisted(&out, &hoists[h])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
    }
    for (size_t s = 0; s < span_count; s++) {
      if (spans[s].header_index == i &&
          !safety_emit_span_resolve(&out, &spans[s])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
    }

    if (instruction->op != IR_OP_SAFETY_CHECK) {
      if (!ir_instruction_vector_append_move(&out, instruction)) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
      continue;
    }

    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      ir_instruction_vector_destroy(&out);
      goto fail;
    }
    if (stats) {
      stats->emitted++;
    }

    if (outcome[i] == SAFETY_PROVED) {
      if (stats) {
        stats->proved++;
      }
      continue;
    }
    if (outcome[i] == SAFETY_HOISTED) {
      if (stats) {
        stats->hoisted++;
      }
      continue;
    }
    if (outcome[i] == SAFETY_SPANNED) {
      if (!safety_emit_span_check(&out, &access, spans[span_of[i]].temp,
                                  span_delta[i])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
      if (stats) {
        stats->spanned++;
      }
      ir_explain_safety_note(access.location.filename, access.location.line,
                             function->name, IR_SAFETY_SURVIVOR_SPAN);
      continue;
    }

    int expanded;
    if (access.extent == IR_SAFETY_EXTENT_UNKNOWN) {
      expanded = safety_expand_region(&out, &access);
      if (expanded) {
        if (stats) {
          stats->region_calls++;
        }
        ir_explain_safety_note(access.location.filename, access.location.line,
                               function->name, IR_SAFETY_SURVIVOR_REGION);
      }
    } else {
      expanded = safety_expand_extent(&out, &access);
      if (expanded) {
        if (stats) {
          stats->extent_tests++;
        }
        ir_explain_safety_note(access.location.filename, access.location.line,
                               function->name, IR_SAFETY_SURVIVOR_EXTENT);
      }
    }
    if (!expanded) {
      ir_instruction_vector_destroy(&out);
      goto fail;
    }
  }

  for (size_t h = 0; h < hoist_count; h++) {
    ir_operand_destroy(&hoists[h].base);
    ir_operand_destroy(&hoists[h].bound);
    if (hoists[h].has_invariant) {
      ir_operand_destroy(&hoists[h].invariant);
    }
    if (hoists[h].has_index_start) {
      ir_operand_destroy(&hoists[h].index_start_value);
    }
  }
  for (size_t s = 0; s < span_count; s++) {
    ir_operand_destroy(&spans[s].base);
  }
  free(spans);
  free(span_of);
  free(span_delta);
  free(hoists);
  free(outcome);
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  return 1;

fail:
  safety_loop_list_destroy(&loops);
  for (size_t h = 0; h < hoist_count; h++) {
    ir_operand_destroy(&hoists[h].base);
    ir_operand_destroy(&hoists[h].bound);
    if (hoists[h].has_invariant) {
      ir_operand_destroy(&hoists[h].invariant);
    }
    if (hoists[h].has_index_start) {
      ir_operand_destroy(&hoists[h].index_start_value);
    }
  }
  for (size_t s = 0; s < span_count; s++) {
    ir_operand_destroy(&spans[s].base);
  }
  free(spans);
  free(span_of);
  free(span_delta);
  free(hoists);
  free(outcome);
  return 0;
}

static int safety_declare_runtime(IRProgram *program) {
  const MtlcType *pointer = mtlc_type_pointer(mtlc_type_scalar(MTLC_TYPE_UINT64));
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  const MtlcType *u32 = mtlc_type_scalar(MTLC_TYPE_UINT32);
  const MtlcType *u64 = mtlc_type_scalar(MTLC_TYPE_UINT64);
  const MtlcType *nothing = mtlc_type_scalar(MTLC_TYPE_VOID);
  if (!pointer || !i64 || !u32 || !u64 || !nothing) {
    return 1;
  }

  const MtlcType *check_params[5] = {pointer, i64, i64, u32, u32};
  const MtlcType *loop_length_params[5] = {i64, i64, i64, i64, i64};
  const MtlcType *span_params[1] = {pointer};
  const MtlcType *register_params[2] = {pointer, u64};
  const MtlcType *unregister_params[1] = {pointer};
  const MtlcType *reregister_params[3] = {pointer, pointer, u64};
  const MtlcType *reregister_held_params[4] = {pointer, pointer, u64, u64};
  const MtlcType *identity_check_params[6] = {pointer, i64, i64, u32, u32, u64};
  const MtlcType *affine_check_params[8] = {pointer, i64, i64, u32, u32, u64, i64, i64};
  const MtlcType *pointer_u64[2] = {pointer, u64};
  const MtlcType *string_u64[2] = {pointer, u64};
  const MtlcType *two_u64[2] = {u64, u64};
  const MtlcType *value_store_params[4] = {pointer, u64, u64, u64};
  const MtlcType *call_arg_params[3] = {pointer, u64, u64};
  const MtlcType *copy_params[3] = {pointer, pointer, u64};
  const MtlcType *arg_copy_params[4] = {pointer, u64, pointer, u64};
  const MtlcType *buffer_params[5] = {pointer, i64, u32, u32, u64};

  const struct {
    const char *name;
    const MtlcType *return_type;
    const MtlcType **params;
    size_t param_count;
  } entries[] = {
      {"mettle_safety_check", nothing, check_params, 5},
      {"mettle_safety_span", i64, span_params, 1},
      {"mettle_safety_register", nothing, register_params, 2},
      {"mettle_safety_register_static", nothing, register_params, 2},
      {"mettle_safety_unregister", nothing, unregister_params, 1},
      {"mettle_safety_reregister", nothing, reregister_params, 3},
      {"mettle_safety_reregister_held", nothing, reregister_held_params, 4},
      {"mettle_safety_enter_allocator", nothing, NULL, 0},
      {"mettle_safety_leave_allocator", nothing, NULL, 0},
      {"mettle_safety_identity", u64, span_params, 1},
      {"mettle_safety_loop_length", i64, loop_length_params, 5},
      {"mettle_safety_check_identity", nothing, identity_check_params, 6},
      {"mettle_safety_check_affine", nothing, affine_check_params, 8},
      {"mettle_safety_span_identity", i64, pointer_u64, 2},
      {"mettle_safety_merge_identity", u64, two_u64, 2},
      {"mettle_safety_subtract_identity", u64, two_u64, 2},
      {"mettle_safety_value_load", u64, call_arg_params, 3},
      {"mettle_safety_value_store", nothing, value_store_params, 4},
      {"mettle_safety_call_push", pointer, pointer_u64, 2},
      {"mettle_safety_call_enter", pointer, span_params, 1},
      {"mettle_safety_call_arg", nothing, call_arg_params, 3},
      {"mettle_safety_call_param", u64, pointer_u64, 2},
      {"mettle_safety_call_param_value", u64, call_arg_params, 3},
      {"mettle_safety_call_return", nothing, pointer_u64, 2},
      {"mettle_safety_call_pop", u64, span_params, 1},
      {"mettle_safety_call_arg_copy", nothing, arg_copy_params, 4},
      {"mettle_safety_call_param_copy", nothing, arg_copy_params, 4},
      {"mettle_safety_call_return_copy", nothing, copy_params, 3},
      {"mettle_safety_call_result_copy", nothing, copy_params, 3},
      {"mettle_safety_value_copy", nothing, copy_params, 3},
      {"mettle_safety_value_clear", nothing, pointer_u64, 2},
      {"mettle_safety_free_identity", nothing, pointer_u64, 2},
      {"mettle_safety_buffer_check", nothing, buffer_params, 5},
      {"mettle_safety_region_begin", nothing, pointer_u64, 2},
      {"mettle_safety_region_end", nothing, span_params, 1},
      {"mettle_safety_entry_arguments", nothing, span_params, 1},
      {"mettle_safety_literal_identity", u64, pointer_u64, 2},
      {"mettle_safety_string_identity", u64, string_u64, 2},
      {"mettle_safety_string_contents", nothing, pointer_u64, 2},
      {"mettle_safety_string_fresh", nothing, span_params, 1},
      {"mettle_safety_string_release", nothing, span_params, 1},
      {"mettle_safety_c_string_identity", u64, span_params, 1},
      {"mettle_safety_mapping_identity", u64, pointer_u64, 2},
      {"mettle_safety_global_pointer", nothing, call_arg_params, 3},
  };

  for (size_t e = 0; e < sizeof(entries) / sizeof(entries[0]); e++) {
    if (ir_program_lookup_symbol(program, entries[e].name)) {
      continue;
    }
    IRModuleSymbol entry = {0};
    entry.name = (char *)entries[e].name;
    entry.kind = IR_MODSYM_FUNCTION;
    entry.is_extern = 1;
    entry.return_type = (MtlcType *)entries[e].return_type;
    entry.type = (MtlcType *)entries[e].return_type;
    entry.param_types = (MtlcType **)entries[e].params;
    entry.param_count = entries[e].param_count;
    if (!ir_program_add_symbol(program, &entry)) {
      return 0;
    }
  }
  return 1;
}

static int safety_global_is_settled(const IRProgram *program,
                                    const char *name) {
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *in = &function->instructions[i];
      if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
          in->lhs.name && strcmp(in->lhs.name, name) == 0) {
        return 0;
      }
      if (ir_instruction_writes_destination(in) &&
          ir_operand_is_symbol(&in->dest) &&
          strcmp(in->dest.name, name) == 0) {
        return 0;
      }
      if (in->op == IR_OP_INLINE_ASM &&
          ir_inline_asm_binds_symbol(in->text, name)) {
        return 0;
      }
    }
  }
  return 1;
}

static int safety_const_globals_order(const void *a, const void *b) {
  const size_t *left = (const size_t *)a;
  const size_t *right = (const size_t *)b;
  return strcmp(g_safety_const_globals.names[*left],
                g_safety_const_globals.names[*right]);
}

static void safety_const_globals_destroy(void) {
  for (size_t i = 0; i < g_safety_const_globals.count; i++) {
    free(g_safety_const_globals.names[i]);
  }
  free(g_safety_const_globals.names);
  free(g_safety_const_globals.values);
  memset(&g_safety_const_globals, 0, sizeof(g_safety_const_globals));
}

static void safety_const_globals_build(const IRProgram *program) {
  memset(&g_safety_const_globals, 0, sizeof(g_safety_const_globals));
  size_t capacity = program->module_symbol_count;
  if (capacity == 0) {
    return;
  }
  g_safety_const_globals.names = (char **)calloc(capacity, sizeof(char *));
  g_safety_const_globals.values =
      (long long *)calloc(capacity, sizeof(long long));
  if (!g_safety_const_globals.names || !g_safety_const_globals.values) {
    safety_const_globals_destroy();
    return;
  }
  for (size_t s = 0; s < program->module_symbol_count; s++) {
    const IRModuleSymbol *symbol = &program->module_symbols[s];
    if (symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
        !symbol->has_initializer || symbol->init_is_float ||
        symbol->init_string || symbol->init_bytes || !symbol->name) {
      continue;
    }
    if (symbol->is_exported) {
      continue;
    }
    if (!safety_global_is_settled(program, symbol->name)) {
      continue;
    }
    char *copy = mettle_strdup(symbol->name);
    if (!copy) {
      safety_const_globals_destroy();
      return;
    }
    g_safety_const_globals.names[g_safety_const_globals.count] = copy;
    g_safety_const_globals.values[g_safety_const_globals.count] =
        symbol->init_bits;
    g_safety_const_globals.count++;
  }
  size_t count = g_safety_const_globals.count;
  if (count < 2) {
    return;
  }
  size_t *order = (size_t *)malloc(count * sizeof(size_t));
  if (!order) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    order[i] = i;
  }
  qsort(order, count, sizeof(size_t), safety_const_globals_order);
  char **names = (char **)malloc(count * sizeof(char *));
  long long *values = (long long *)malloc(count * sizeof(long long));
  if (names && values) {
    for (size_t i = 0; i < count; i++) {
      names[i] = g_safety_const_globals.names[order[i]];
      values[i] = g_safety_const_globals.values[order[i]];
    }
    free(g_safety_const_globals.names);
    free(g_safety_const_globals.values);
    g_safety_const_globals.names = names;
    g_safety_const_globals.values = values;
  } else {
    free(names);
    free(values);
    g_safety_const_globals.count = 0;
  }
  free(order);
}

#include "ir_safety_plain_storage.inc"

int ir_safety_analyze_origins(IRProgram *program) {
  if (!program) return 1;
  g_safety_program = program;
  g_safety_callee_cache_count = 0;
  safety_const_globals_build(program);
  int ok = 1;
  for (size_t f = 0; ok && f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    SafetyLoopList loops = {0};
    if (!safety_loop_list_build(fn, &loops)) { ok = 0; break; }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      if (in->op != IR_OP_SAFETY_CHECK ||
          in->argument_count != IR_SAFETY_TRACKED_ARG_COUNT) continue;
      SafetyAccess access = {0};
      SafetyHoist hoist = {0};
      if (!safety_read(in, &access)) { ok = 0; break; }
      int ranged = safety_try_hoist(fn, &loops, i, &access, &hoist);
      ir_operand_destroy(&hoist.base);
      ir_operand_destroy(&hoist.bound);
      ir_operand_destroy(&hoist.invariant);
      ir_operand_destroy(&hoist.index_start_value);
      IROperand *args = realloc(in->arguments,
          IR_SAFETY_ANALYZED_ARG_COUNT * sizeof(*args));
      if (!args) { ok = 0; break; }
      in->arguments = args;
      args[IR_SAFETY_ARG_DIAGNOSTIC_SIZE] = ir_operand_int(ranged ? 0 : access.size);
      in->argument_count = IR_SAFETY_ANALYZED_ARG_COUNT;
    }
    safety_loop_list_destroy(&loops);
  }
  safety_const_globals_destroy();
  g_safety_program = NULL;
  return ok && safety_simplify_plain_storage(program);
}

int ir_safety_resolve_program(IRProgram *program, IRSafetyStats *stats) {
  if (!program) {
    return 1;
  }
  clock_t started = safety_time_enabled() ? clock() : 0;
  if (!safety_declare_runtime(program)) {
    return 0;
  }
  g_safety_program = program;
  g_safety_callee_cache_count = 0;
  safety_const_globals_build(program);
  const char *allocator_source = safety_allocator_source(program);
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    if (function->is_rule || function->is_naked) {
      continue;
    }
    int resolved =
        safety_function_is_allocator(function, allocator_source)
            ? safety_strip_function(function, stats)
            : safety_resolve_function(function, stats);
    if (!resolved) {
      safety_const_globals_destroy();
      g_safety_program = NULL;
      return 0;
    }
  }
  safety_const_globals_destroy();
  g_safety_program = NULL;
  if (!safety_simplify_plain_storage(program)) return 0;
  if (safety_time_enabled()) {
    fprintf(stderr, "safety: resolving took %lld ticks\n",
            (long long)(clock() - started));
  }
  return 1;
}

typedef enum {
  SAFETY_ALLOC_NONE = 0,
  SAFETY_ALLOC_SIZE,
  SAFETY_ALLOC_PRODUCT,
  SAFETY_ALLOC_REALLOC,
  SAFETY_ALLOC_FREE
} SafetyAllocKind;

static const IRProgram *g_safety_alloc_program = NULL;
static const char *g_safety_alloc_source = NULL;

static int safety_name_is_std_alloc(const char *name) {
  const IRProgram *program = g_safety_alloc_program;
  if (!program || !g_safety_alloc_source) {
    return 0;
  }
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *function = program->functions[f];
    if (function && function->name && !strcmp(function->name, name)) {
      return function->location.filename &&
             !strcmp(function->location.filename, g_safety_alloc_source);
    }
  }
  return 0;
}

static int safety_callee_takes_heap(const IRInstruction *instruction) {
  const char *callee = instruction->text;
  return callee && strncmp(callee, "mem_", 4) == 0 &&
         (!strcmp(callee, "mem_alloc") || !strcmp(callee, "mem_calloc") ||
          !strcmp(callee, "mem_realloc") || !strcmp(callee, "mem_free")) &&
         safety_name_is_std_alloc(callee);
}

static int safety_callee_is_direct_allocator(const IRInstruction *instruction) {
  const char *callee = instruction->text;
  return callee && strncmp(callee, "mx_", 3) == 0 &&
         (!strcmp(callee, "mx_alloc") || !strcmp(callee, "mx_calloc") ||
          !strcmp(callee, "mx_realloc") || !strcmp(callee, "mx_free")) &&
         safety_name_is_std_alloc(callee);
}

static size_t safety_alloc_lead(const IRInstruction *instruction) {
  return safety_callee_takes_heap(instruction) ? 1 : 0;
}

static int safety_callee_is_mettle_allocator(const IRInstruction *instruction) {
  return instruction->op == IR_OP_CALL && instruction->text &&
         (strncmp(instruction->text, "mettle_heap_", 12) == 0 ||
          safety_callee_takes_heap(instruction) ||
          safety_callee_is_direct_allocator(instruction));
}

static SafetyAllocKind safety_classify_call(const IRInstruction *instruction) {
  if (instruction->op != IR_OP_CALL || !instruction->text) {
    return SAFETY_ALLOC_NONE;
  }
  const char *callee = instruction->text;
  size_t arguments = instruction->argument_count;

  if (safety_callee_takes_heap(instruction)) {
    if (arguments == 2 && !strcmp(callee, "mem_alloc")) return SAFETY_ALLOC_SIZE;
    if (arguments == 3 && !strcmp(callee, "mem_calloc")) return SAFETY_ALLOC_PRODUCT;
    if (arguments == 3 && !strcmp(callee, "mem_realloc")) return SAFETY_ALLOC_REALLOC;
    if (arguments == 2 && !strcmp(callee, "mem_free")) return SAFETY_ALLOC_FREE;
    return SAFETY_ALLOC_NONE;
  }
  if (safety_callee_is_direct_allocator(instruction)) {
    if (arguments == 1 && !strcmp(callee, "mx_alloc")) return SAFETY_ALLOC_SIZE;
    if (arguments == 2 && !strcmp(callee, "mx_calloc")) return SAFETY_ALLOC_PRODUCT;
    if (arguments == 2 && !strcmp(callee, "mx_realloc")) return SAFETY_ALLOC_REALLOC;
    if (arguments == 1 && !strcmp(callee, "mx_free")) return SAFETY_ALLOC_FREE;
    return SAFETY_ALLOC_NONE;
  }

  if (arguments == 1 &&
      (strcmp(callee, "malloc") == 0 ||
       strcmp(callee, "mettle_heap_alloc") == 0 ||
       strcmp(callee, "mettle_heap_zeroed") == 0)) {
    return SAFETY_ALLOC_SIZE;
  }
  if (arguments == 2 && (strcmp(callee, "calloc") == 0 ||
                         strcmp(callee, "mettle_heap_calloc") == 0)) {
    return SAFETY_ALLOC_PRODUCT;
  }
  if (arguments == 2 && (strcmp(callee, "realloc") == 0 ||
                         strcmp(callee, "mettle_heap_realloc") == 0)) {
    return SAFETY_ALLOC_REALLOC;
  }
  if (arguments == 1 && (strcmp(callee, "free") == 0 ||
                         strcmp(callee, "mettle_heap_free") == 0)) {
    return SAFETY_ALLOC_FREE;
  }
  return SAFETY_ALLOC_NONE;
}

static int safety_emit_register(IRInstructionVector *out,
                                SourceLocation location,
                                const IROperand *pointer,
                                const IROperand *size) {
  IROperand arguments[2];
  if (!ir_operand_clone(pointer, &arguments[0])) {
    return 0;
  }
  if (!ir_operand_clone(size, &arguments[1])) {
    ir_operand_destroy(&arguments[0]);
    return 0;
  }
  int ok = safety_emit_call(out, location, "mettle_safety_register", arguments,
                            2);
  ir_operand_destroy(&arguments[0]);
  ir_operand_destroy(&arguments[1]);
  return ok;
}

static int safety_emit_one_pointer_call(IRInstructionVector *out,
                                        SourceLocation location,
                                        const char *callee,
                                        const IROperand *pointer) {
  IROperand argument;
  if (!ir_operand_clone(pointer, &argument)) {
    return 0;
  }
  int ok = safety_emit_call(out, location, callee, &argument, 1);
  ir_operand_destroy(&argument);
  return ok;
}

static int safety_emit_hold_identity(IRInstructionVector *out,
                                     SourceLocation location,
                                     const IROperand *pointer,
                                     const char *held) {
  IRInstruction insn = {0};
  insn.op = IR_OP_CALL;
  insn.location = location;
  insn.text = mettle_strdup("mettle_safety_identity");
  insn.arguments = calloc(1, sizeof(IROperand));
  insn.dest = ir_operand_temp(held);
  if (!insn.text || !insn.arguments || !insn.dest.name ||
      !ir_operand_clone(pointer, &insn.arguments[0])) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  insn.argument_count = 1;
  if (!ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

static int safety_emit_reregister(IRInstructionVector *out,
                                  SourceLocation location,
                                  const IROperand *old_pointer,
                                  const IROperand *new_pointer,
                                  const IROperand *size,
                                  const IROperand *held) {
  IROperand arguments[4];
  size_t built = 0;
  int ok = 0;

  if (!ir_operand_clone(old_pointer, &arguments[0])) {
    return 0;
  }
  built = 1;
  if (!ir_operand_clone(new_pointer, &arguments[1])) {
    goto done;
  }
  built = 2;
  if (!ir_operand_clone(size, &arguments[2])) {
    goto done;
  }
  built = 3;
  if (!ir_operand_clone(held, &arguments[3])) {
    goto done;
  }
  built = 4;
  ok = safety_emit_call(out, location, "mettle_safety_reregister_held", arguments,
                        4);

done:
  for (size_t i = 0; i < built; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

static IROperand safety_new_size(const IRInstruction *instruction) {
  if (instruction->rhs.kind == IR_OPERAND_NONE ||
      (instruction->rhs.kind == IR_OPERAND_INT &&
       instruction->rhs.int_value <= 0)) {
    return ir_operand_int(8);
  }
  return instruction->rhs;
}

static int safety_register_function(IRFunction *function) {
  int found = 0;
  for (size_t i = 0; i < function->instruction_count && !found; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    found = instruction->op == IR_OP_NEW ||
            safety_classify_call(instruction) != SAFETY_ALLOC_NONE;
  }
  if (!found) {
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count + 16)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    SafetyAllocKind kind = safety_classify_call(instruction);
    int is_new = instruction->op == IR_OP_NEW;
    SourceLocation location = instruction->location;

    if (kind == SAFETY_ALLOC_NONE && !is_new) {
      if (!ir_instruction_vector_append_move(&out, instruction)) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
      continue;
    }

    size_t lead = is_new ? 0 : safety_alloc_lead(instruction);

    if (kind == SAFETY_ALLOC_FREE) {
      if (!safety_emit_one_pointer_call(&out, location,
                                        "mettle_safety_unregister",
                                        &instruction->arguments[lead])) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
    }

    IROperand size = ir_operand_none();
    IROperand product_operand = ir_operand_none();
    if (is_new) {
      size = safety_new_size(instruction);
    } else if (kind == SAFETY_ALLOC_SIZE) {
      size = instruction->arguments[lead];
    } else if (kind == SAFETY_ALLOC_REALLOC) {
      size = instruction->arguments[lead + 1];
    } else if (kind == SAFETY_ALLOC_PRODUCT) {
      char product[64];
      snprintf(product, sizeof(product), SAFETY_TEMP_PREFIX "n%u",
               g_safety_next_id++);
      product_operand = ir_operand_temp(product);
      if (!product_operand.name ||
          !safety_emit_binary(&out, location, "*", product,
                              &instruction->arguments[lead],
                              &instruction->arguments[lead + 1], 1)) {
        ir_operand_destroy(&product_operand);
        ir_instruction_vector_destroy(&out);
        return 0;
      }
      size = product_operand;
    }

    IROperand result = instruction->dest;
    IROperand old_pointer = kind == SAFETY_ALLOC_REALLOC
                                ? instruction->arguments[lead]
                                : ir_operand_none();
    int bracket = safety_callee_is_mettle_allocator(instruction);

    IROperand held = ir_operand_none();
    if (kind == SAFETY_ALLOC_REALLOC) {
      char held_name[64];
      snprintf(held_name, sizeof(held_name), SAFETY_TEMP_PREFIX "h%u",
               g_safety_next_id++);
      held = ir_operand_temp(held_name);
      if (!held.name || !safety_emit_hold_identity(&out, location, &old_pointer, held_name)) {
        ir_operand_destroy(&held);
        ir_operand_destroy(&product_operand);
        ir_instruction_vector_destroy(&out);
        return 0;
      }
    }

    if (bracket && !safety_emit_call(&out, location,
                                     "mettle_safety_enter_allocator", NULL,
                                     0)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    if (!ir_instruction_vector_append_move(&out, instruction)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    if (bracket && !safety_emit_call(&out, location,
                                     "mettle_safety_leave_allocator", NULL,
                                     0)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    int ok = 1;
    if (result.kind != IR_OPERAND_NONE && kind != SAFETY_ALLOC_FREE) {
      ok = kind == SAFETY_ALLOC_REALLOC
               ? safety_emit_reregister(&out, location, &old_pointer, &result,
                                        &size, &held)
               : safety_emit_register(&out, location, &result, &size);
    }
    ir_operand_destroy(&held);
    ir_operand_destroy(&product_operand);
    if (!ok) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }

  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(function);
  return 1;
}

#define SAFETY_MAX_ESCAPE_TEMPS 64

typedef struct {
  const char *names[SAFETY_MAX_ESCAPE_TEMPS];
  size_t count;
  int overflowed;
} SafetyTempSet;

static int safety_temp_set_has(const SafetyTempSet *set, const IROperand *op) {
  if (op->kind != IR_OPERAND_TEMP || !op->name) {
    return 0;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], op->name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void safety_temp_set_add(SafetyTempSet *set, const IROperand *op) {
  if (op->kind != IR_OPERAND_TEMP || !op->name || safety_temp_set_has(set, op)) {
    return;
  }
  if (set->count == SAFETY_MAX_ESCAPE_TEMPS) {
    set->overflowed = 1;
    return;
  }
  set->names[set->count++] = op->name;
}

static int safety_local_address_escapes(const IRFunction *function,
                                        const char *local) {
  SafetyTempSet addresses = {{0}, 0, 0};

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_ADDRESS_OF &&
        ir_operand_is_symbol_named(&instruction->lhs, local)) {
      safety_temp_set_add(&addresses, &instruction->dest);
      continue;
    }
    if (addresses.count == 0) {
      continue;
    }

    switch (instruction->op) {
    case IR_OP_BINARY:
    case IR_OP_ASSIGN:
    case IR_OP_CAST:
      if (safety_temp_set_has(&addresses, &instruction->lhs) ||
          safety_temp_set_has(&addresses, &instruction->rhs)) {
        if (instruction->dest.kind == IR_OPERAND_SYMBOL) {
          return 1;
        }
        safety_temp_set_add(&addresses, &instruction->dest);
      }
      break;
    case IR_OP_LOAD:
      break;
    case IR_OP_STORE:
      if (safety_temp_set_has(&addresses, &instruction->lhs)) {
        return 1;
      }
      break;
    case IR_OP_SAFETY_CHECK:
      if (instruction->arguments &&
          instruction->argument_count > IR_SAFETY_ARG_EXTENT &&
          safety_temp_set_has(&addresses,
                              &instruction->arguments[IR_SAFETY_ARG_BASE]) &&
          instruction->arguments[IR_SAFETY_ARG_EXTENT].kind == IR_OPERAND_INT &&
          instruction->arguments[IR_SAFETY_ARG_EXTENT].int_value ==
              IR_SAFETY_EXTENT_UNKNOWN) {
        return 1;
      }
      break;
    case IR_OP_RETURN:
      if (safety_temp_set_has(&addresses, &instruction->lhs)) {
        return 1;
      }
      break;
    default:
      for (size_t a = 0; a < instruction->argument_count; a++) {
        if (safety_temp_set_has(&addresses, &instruction->arguments[a])) {
          return 1;
        }
      }
      if (safety_temp_set_has(&addresses, &instruction->lhs) ||
          safety_temp_set_has(&addresses, &instruction->rhs)) {
        return 1;
      }
      break;
    }
  }

  return addresses.overflowed && addresses.count > 0;
}

static int safety_emit_local_note(IRInstructionVector *out, const char *callee,
                                  const char *local, long long size,
                                  SourceLocation location) {
  char address[64];
  snprintf(address, sizeof(address), SAFETY_TEMP_PREFIX "k%u",
           g_safety_next_id++);

  IRInstruction take = {0};
  take.op = IR_OP_ADDRESS_OF;
  take.location = location;
  take.dest = ir_operand_temp(address);
  take.lhs = ir_operand_symbol(local);
  if (!take.dest.name || !take.lhs.name) {
    ir_instruction_destroy_storage(&take);
    return 0;
  }
  if (!ir_instruction_vector_append_move(out, &take)) {
    ir_instruction_destroy_storage(&take);
    return 0;
  }

  IROperand arguments[2];
  arguments[0] = ir_operand_temp(address);
  arguments[1] = ir_operand_int(size);
  int ok = arguments[0].name &&
           safety_emit_call(out, location, callee, arguments,
                            size > 0 ? 2u : 1u);
  ir_operand_destroy(&arguments[0]);
  return ok;
}

typedef struct {
  const char *name;
  long long size;
  SourceLocation location;
} SafetyStackLocal;

static int safety_describe_local(IRProgram *program, IRFunction *function,
                                 size_t i, SafetyStackLocal *locals,
                                 size_t *count) {
  const IRInstruction *instruction = &function->instructions[i];
  if (instruction->op != IR_OP_DECLARE_LOCAL ||
      instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
      !instruction->text) {
    return 1;
  }
  MtlcType *type = instruction->value_type
                       ? instruction->value_type
                       : ir_program_lookup_type(program, instruction->text);
  if (!type || type->size == 0) {
    return 1;
  }
  if (!safety_local_address_escapes(function, instruction->dest.name)) {
    return 1;
  }
  locals[*count].name = instruction->dest.name;
  locals[*count].size = (long long)type->size;
  locals[*count].location = instruction->location;
  (*count)++;
  if (safety_trace_enabled()) {
    fprintf(stderr, "safety: describing local %s (%zu bytes) in %s\n",
            instruction->dest.name, type->size,
            function->name ? function->name : "?");
  }
  return 1;
}

static int safety_describe_stack(IRProgram *program, IRFunction *function) {
  size_t declared = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_DECLARE_LOCAL) {
      declared++;
    }
  }
  declared += function->parameter_count;
  if (declared == 0) {
    return 1;
  }

  SafetyStackLocal *locals = calloc(declared, sizeof(SafetyStackLocal));
  if (!locals) {
    return 0;
  }
  size_t count = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!safety_describe_local(program, function, i, locals, &count)) {
      return 0;
    }
  }

  for (size_t p = 0; p < function->parameter_count; p++) {
    const char *name = function->parameter_names ? function->parameter_names[p]
                                                 : NULL;
    MtlcType *type = name && function->parameter_types
        ? ir_program_lookup_type(program, function->parameter_types[p]) : NULL;
    if (!name || !type || type->size == 0) {
      continue;
    }
    if (ir_function_find_declaration(function, name, 1)) {
      continue;
    }
    if (!safety_local_address_escapes(function, name)) {
      continue;
    }
    locals[count].name = name;
    locals[count].size = (long long)type->size;
    locals[count].location = function->location;
    count++;
    if (safety_trace_enabled()) {
      fprintf(stderr, "safety: describing parameter %s (%zu bytes) in %s\n",
              name, type->size, function->name ? function->name : "?");
    }
  }
  if (count == 0) {
    free(locals);
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out,
                                     function->instruction_count + count * 6)) {
    free(locals);
    return 0;
  }

  for (size_t l = 0; l < count; l++) {
    if (!safety_emit_local_note(&out, "mettle_safety_register_static", locals[l].name,
                                locals[l].size, locals[l].location)) {
      ir_instruction_vector_destroy(&out);
      free(locals);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_RETURN) {
      for (size_t l = 0; l < count; l++) {
        if (!safety_emit_local_note(&out, "mettle_safety_unregister",
                                    locals[l].name, 0, locals[l].location)) {
          ir_instruction_vector_destroy(&out);
          free(locals);
          return 0;
        }
      }
    }
    if (!ir_instruction_vector_append_move(&out, instruction)) {
      ir_instruction_vector_destroy(&out);
      free(locals);
      return 0;
    }
  }

  const IRInstruction *last =
      out.count > 0 ? &out.items[out.count - 1] : NULL;
  if (!last || last->op != IR_OP_RETURN) {
    for (size_t l = 0; l < count; l++) {
      if (!safety_emit_local_note(&out, "mettle_safety_unregister",
                                  locals[l].name, 0, locals[l].location)) {
        ir_instruction_vector_destroy(&out);
        free(locals);
        return 0;
      }
    }
  }

  free(locals);
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(function);
  return 1;
}

static int safety_seed_global_pointer(IRInstructionVector *out, SourceLocation location,
                                       const char *name, size_t offset, int mode, size_t size) {
  char base_name[64], slot_name[64];
  snprintf(base_name, sizeof(base_name), ".safe_global_%u", g_safety_next_id++);
  snprintf(slot_name, sizeof(slot_name), ".safe_global_%u", g_safety_next_id++);
  IRInstruction take = {0};
  take.op = IR_OP_ADDRESS_OF;
  take.location = location;
  take.dest = ir_operand_temp(base_name);
  take.lhs = ir_operand_symbol(name);
  if (!take.dest.name || !take.lhs.name || !ir_instruction_vector_append_move(out, &take)) {
    ir_instruction_destroy_storage(&take);
    return 0;
  }
  IRInstruction add = {0};
  add.op = IR_OP_BINARY;
  add.location = location;
  add.text = mettle_strdup("+");
  add.dest = ir_operand_temp(slot_name);
  add.lhs = ir_operand_temp(base_name);
  add.rhs = ir_operand_int((long long)offset);
  if (!add.text || !add.dest.name || !add.lhs.name || !ir_instruction_vector_append_move(out, &add)) {
    ir_instruction_destroy_storage(&add);
    return 0;
  }
  IROperand args[3] = {ir_operand_temp(slot_name), ir_operand_int(mode), ir_operand_int((long long)size)};
  int ok = args[0].name && safety_emit_call(out, location, "mettle_safety_global_pointer", args, 3);
  ir_operand_destroy(&args[0]);
  return ok;
}

static int safety_describe_entry_arguments(IRProgram *program, IRFunction *entry) {
  if (entry->parameter_count < 2 || !entry->parameter_names ||
      !entry->parameter_names[1] || !entry->parameter_types) {
    return 1;
  }
  const MtlcType *type = ir_program_lookup_type(program, entry->parameter_types[1]);
  if (!type || type->kind != MTLC_TYPE_POINTER) {
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, entry->instruction_count + 1)) {
    return 0;
  }
  IROperand vector = ir_operand_symbol(entry->parameter_names[1]);
  int ok = vector.name != NULL &&
           safety_emit_call(&out, entry->location, "mettle_safety_entry_arguments",
                            &vector, 1);
  ir_operand_destroy(&vector);
  for (size_t i = 0; ok && i < entry->instruction_count; i++) {
    ok = ir_instruction_vector_append_move(&out, &entry->instructions[i]);
  }
  if (!ok || !ir_function_replace_instructions(entry, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(entry);
  return 1;
}

static int safety_describe_globals(IRProgram *program, IRFunction *entry) {
  size_t described = 0;
  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &program->module_symbols[i];
    if (symbol->kind == IR_MODSYM_VARIABLE && !symbol->is_extern &&
        symbol->name && symbol->type && symbol->type->size > 0) {
      described++;
    }
  }
  if (described == 0) {
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out,
                                     entry->instruction_count + described * 2)) {
    return 0;
  }

  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &program->module_symbols[i];
    if (symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
        !symbol->name || !symbol->type || symbol->type->size == 0) {
      continue;
    }

    char address[96];
    snprintf(address, sizeof(address), SAFETY_TEMP_PREFIX "g%u",
             g_safety_next_id++);
    IROperand size_operand = ir_operand_int((long long)symbol->type->size);

    IRInstruction take = {0};
    take.op = IR_OP_ADDRESS_OF;
    take.location = entry->location;
    take.dest = ir_operand_temp(address);
    take.lhs = ir_operand_symbol(symbol->name);
    if (!take.dest.name || !take.lhs.name) {
      ir_instruction_destroy_storage(&take);
      ir_instruction_vector_destroy(&out);
      return 0;
    }
    if (!ir_instruction_vector_append_move(&out, &take)) {
      ir_instruction_destroy_storage(&take);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    IROperand address_operand = ir_operand_temp(address);
    IROperand static_args[2] = {address_operand, size_operand};
    int ok = address_operand.name &&
             safety_emit_call(&out, entry->location, "mettle_safety_register_static",
                              static_args, 2);
    ir_operand_destroy(&address_operand);
    if (!ok) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }

  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &program->module_symbols[i];
    if (symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern || !symbol->type) continue;
    if (symbol->init_symbol_ref || symbol->init_string) {
      int mode = symbol->init_string ? (symbol->type->kind == MTLC_TYPE_STRING ? 3 : 1) : 0;
      if (!safety_seed_global_pointer(&out, entry->location, symbol->name, 0, mode,
                                      symbol->init_string_length + 1)) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
    }
    for (size_t r = 0; r < symbol->init_reloc_count; r++) {
      const IRInitReloc *reloc = &symbol->init_relocs[r];
      int mode = reloc->string ? (reloc->string_wants_record ? 3 : 1) : 0;
      if (!safety_seed_global_pointer(&out, entry->location, symbol->name, reloc->offset,
                                      mode, reloc->string_length + 1)) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
    }
  }
  for (size_t i = 0; i < entry->instruction_count; i++) {
    if (!ir_instruction_vector_append_move(&out, &entry->instructions[i])) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }
  if (!ir_function_replace_instructions(entry, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(entry);
  return 1;
}

static int safety_retire_stack_notes(const IRProgram *program,
                                     IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *call = &function->instructions[i];
    if (call->op != IR_OP_CALL || !call->text || call->argument_count < 1 ||
        !call->arguments) {
      continue;
    }
    if (strcmp(call->text, "mettle_safety_register") != 0 &&
        strcmp(call->text, "mettle_safety_register_static") != 0 &&
        strcmp(call->text, "mettle_safety_unregister") != 0) {
      continue;
    }
    const IROperand *argument = &call->arguments[0];
    if (argument->kind != IR_OPERAND_TEMP || !argument->name) {
      continue;
    }

    IRInstruction *take = NULL;
    for (size_t back = i; back-- > 0;) {
      IRInstruction *candidate = &function->instructions[back];
      if (candidate->op == IR_OP_NOP) {
        continue;
      }
      if (candidate->op == IR_OP_ADDRESS_OF &&
          ir_operand_is_temp(&candidate->dest) &&
          strcmp(candidate->dest.name, argument->name) == 0) {
        take = candidate;
      }
      break;
    }
    if (!take || take->lhs.kind != IR_OPERAND_SYMBOL || !take->lhs.name) {
      continue;
    }
    if (ir_program_lookup_symbol(program, take->lhs.name)) {
      continue;
    }

    int declared = 0;
    for (size_t d = 0; d < function->parameter_count && !declared; d++) {
      if (function->parameter_names && function->parameter_names[d] &&
          strcmp(function->parameter_names[d], take->lhs.name) == 0) {
        declared = 1;
      }
    }
    for (size_t d = 0; d < function->instruction_count && !declared; d++) {
      const IRInstruction *candidate = &function->instructions[d];
      if (candidate->op == IR_OP_DECLARE_LOCAL &&
          ir_operand_is_symbol(&candidate->dest) &&
          ir_operand_names_match(&candidate->dest, &take->lhs)) {
        declared = 1;
      }
    }
    if (declared) {
      continue;
    }
    if (safety_trace_enabled()) {
      fprintf(stderr, "safety: retiring note for %s in %s\n", take->lhs.name,
              function->name ? function->name : "?");
    }
    ir_instruction_make_nop(take);
    ir_instruction_make_nop(call);
  }
  return 1;
}

int ir_safety_retire_dangling_notes(IRProgram *program) {
  if (!program) {
    return 1;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && !safety_retire_stack_notes(program, function)) {
      return 0;
    }
  }
  return 1;
}

#include "ir_safety_provenance.inc"

static void safety_rename_operand(IROperand *operand, const char *from,
                                  const char *to) {
  if (operand->kind != IR_OPERAND_SYMBOL || !operand->name ||
      strcmp(operand->name, from) != 0) {
    return;
  }
  ir_operand_destroy(operand);
  *operand = ir_operand_symbol(to);
}

static int safety_unshadow_own_name(IRFunction *function) {
  char renamed[256];
  if (!function->name ||
      (!ir_function_symbol_is_parameter(function, function->name) &&
       !ir_function_local_declared_type(function, function->name))) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_INLINE_ASM) {
      return 1;
    }
  }
  snprintf(renamed, sizeof(renamed), "%s.own", function->name);
  for (size_t p = 0; p < function->parameter_count; p++) {
    if (function->parameter_names[p] &&
        strcmp(function->parameter_names[p], function->name) == 0) {
      char *copy = mettle_strdup(renamed);
      if (!copy) {
        return 0;
      }
      mettle_free_string(function->parameter_names[p]);
      function->parameter_names[p] = copy;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *in = &function->instructions[i];
    safety_rename_operand(&in->dest, function->name, renamed);
    safety_rename_operand(&in->lhs, function->name, renamed);
    safety_rename_operand(&in->rhs, function->name, renamed);
    for (size_t a = 0; a < in->argument_count; a++) {
      safety_rename_operand(&in->arguments[a], function->name, renamed);
    }
  }
  ir_function_touch_structure(function);
  return 1;
}

int ir_safety_register_allocations(IRProgram *program) {
  if (!program) {
    return 1;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && !program->functions[i]->is_rule &&
        !safety_unshadow_own_name(program->functions[i])) {
      return 0;
    }
  }
  g_safety_next_id = 0;
  if (!safety_declare_runtime(program)) return 0;
  if (!safety_normalize_external_calls(program) || !safety_wrap_allocator_addresses(program) ||
      !safety_normalize_external_calls(program)) return 0;
  const char *allocator_source = safety_allocator_source(program);
  IRFunction *entry = NULL;
  g_safety_alloc_program = program;
  g_safety_alloc_source = allocator_source;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    if (function->name && strcmp(function->name, "main") == 0) {
      entry = function;
    }
    if (function->is_rule ||
        safety_function_is_allocator(function, allocator_source)) {
      continue;
    }
    if (!safety_register_function(function) ||
        !safety_describe_stack(program, function)) {
      return 0;
    }
  }
  if (entry && (!safety_describe_globals(program, entry) ||
                !safety_describe_entry_arguments(program, entry))) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && !function->is_rule && !function->is_naked &&
        !safety_function_is_allocator(function, allocator_source) &&
        !safety_origins_function(program, function)) return 0;
  }
  return 1;
}
