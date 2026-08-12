#include "ir_optimize_internal.h"

static int ir_operand_is_pointerish(const IROperand *operand) {
  return operand &&
         (operand->kind == IR_OPERAND_SYMBOL || operand->kind == IR_OPERAND_TEMP);
}

static int ir_try_resolve_memcpy_size_const(const IRFunction *function,
                                            size_t before_index,
                                            const IROperand *operand,
                                            long long *out_value) {
  const IRInstruction *producer = NULL;

  if (!function || !operand || !out_value) {
    return 0;
  }

  if (operand->kind == IR_OPERAND_INT) {
    *out_value = operand->int_value;
    return 1;
  }

  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 0;
  }

  producer = ir_find_temp_producer_before(function, before_index, operand->name);
  if (!producer) {
    return 0;
  }

  if (producer->op == IR_OP_ASSIGN && producer->lhs.kind == IR_OPERAND_INT) {
    *out_value = producer->lhs.int_value;
    return 1;
  }

  if (producer->op == IR_OP_CAST && !producer->is_float) {
    return ir_try_resolve_memcpy_size_const(function, before_index,
                                            &producer->lhs, out_value);
  }

  if (producer->op == IR_OP_BINARY && producer->text && !producer->is_float &&
      producer->dest.kind == IR_OPERAND_TEMP) {
    long long lhs_value = 0;
    long long rhs_value = 0;
    if (!ir_try_resolve_memcpy_size_const(function, before_index, &producer->lhs,
                                          &lhs_value) ||
        !ir_try_resolve_memcpy_size_const(function, before_index, &producer->rhs,
                                          &rhs_value)) {
      return 0;
    }
    if (strcmp(producer->text, "*") == 0) {
      *out_value = lhs_value * rhs_value;
      return 1;
    }
    if (strcmp(producer->text, "<<") == 0) {
      *out_value = lhs_value << rhs_value;
      return 1;
    }
  }

  return 0;
}

static int ir_try_lower_memcpy_call(IRFunction *function, size_t index,
                                    int *changed) {
  IRInstruction *call = &function->instructions[index];
  IRInstruction lowered = {0};
  long long byte_count = 0;

  if (!call || call->op != IR_OP_CALL || !call->text ||
      strcmp(call->text, "memcpy") != 0 || call->argument_count != 3) {
    return 1;
  }

  /* Only specialize when the byte count is provably constant at this call
   * site. Do not consult loop-local symbol values (@chunk, etc.): the
   * symbol-int map can report a stale constant from an earlier assignment on
   * a different control-flow path. */
  if (!ir_try_resolve_memcpy_size_const(function, index, &call->arguments[2],
                                      &byte_count) ||
      byte_count <= 0 || byte_count > 8192) {
    return 1;
  }

  if (!ir_operand_is_pointerish(&call->arguments[0]) ||
      !ir_operand_is_pointerish(&call->arguments[1])) {
    return 1;
  }

  lowered.op = IR_OP_MEMCPY_INLINE;
  lowered.location = call->location;
  if (!ir_operand_clone(&call->arguments[0], &lowered.dest) ||
      !ir_operand_clone(&call->arguments[1], &lowered.lhs) ||
      !ir_operand_clone(&call->arguments[2], &lowered.rhs)) {
    ir_instruction_destroy_storage(&lowered);
    return 0;
  }
  if (lowered.rhs.kind != IR_OPERAND_INT) {
    ir_operand_destroy(&lowered.rhs);
    lowered.rhs = ir_operand_int(byte_count);
  }

  ir_instruction_destroy_storage(call);
  *call = lowered;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_memcpy_inline_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_CALL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, "memcpy") == 0) {
      if (!ir_try_lower_memcpy_call(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}
