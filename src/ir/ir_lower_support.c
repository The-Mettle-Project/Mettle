// AST->IR lowering: runtime checks and control-flow (break/continue) frames.
#include "ir_lowering_internal.h"

int ir_emit_runtime_trap_ex(IRLoweringContext *context,
                                   IRFunction *function,
                                   SourceLocation location, uint32_t kind,
                                   const char *message, const IROperand *arg0,
                                   const IROperand *arg1) {
  if (!context || !function || !message) {
    return 0;
  }

  IRInstruction trap_call = {0};
  trap_call.op = IR_OP_CALL;
  trap_call.location = location;
  trap_call.text = "mettle_crash_trap_ex";
  trap_call.argument_count = 4;
  trap_call.arguments = calloc(4, sizeof(IROperand));
  if (!trap_call.arguments) {
    ir_set_error(context, "Out of memory while lowering runtime trap");
    return 0;
  }
  trap_call.arguments[0] = ir_operand_int((long long)kind);
  trap_call.arguments[1] = ir_operand_string(message);
  trap_call.arguments[2] = arg0 ? ir_operand_copy(arg0) : ir_operand_int(0);
  trap_call.arguments[3] = arg1 ? ir_operand_copy(arg1) : ir_operand_int(0);
  if (!ir_emit(context, function, &trap_call)) {
    ir_operand_destroy(&trap_call.arguments[0]);
    ir_operand_destroy(&trap_call.arguments[1]);
    ir_operand_destroy(&trap_call.arguments[2]);
    ir_operand_destroy(&trap_call.arguments[3]);
    free(trap_call.arguments);
    return 0;
  }
  ir_operand_destroy(&trap_call.arguments[0]);
  ir_operand_destroy(&trap_call.arguments[1]);
  ir_operand_destroy(&trap_call.arguments[2]);
  ir_operand_destroy(&trap_call.arguments[3]);
  free(trap_call.arguments);
  return 1;
}

int ir_emit_null_check(IRLoweringContext *context, IRFunction *function,
                              SourceLocation location, const IROperand *value) {
  if (!context || !function || !value) {
    return 0;
  }
  if (!context->emit_runtime_checks) {
    return 1;
  }

  char *trap_label = ir_new_label_name(context, "trap_null");
  char *ok_label = ir_new_label_name(context, "nonnull");
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    ir_set_error(context, "Out of memory while lowering null check");
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = *value;
  branch.text = trap_label;
  if (!ir_emit(context, function, &branch)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction jump = {0};
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = ok_label;
  if (!ir_emit(context, function, &jump)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction trap = {0};
  trap.op = IR_OP_LABEL;
  trap.location = location;
  trap.text = trap_label;
  if (!ir_emit(context, function, &trap) ||
      !ir_emit_runtime_trap_ex(
          context, function, location, 1u,
          "Fatal error: Null pointer dereference", NULL, NULL)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction ok = {0};
  ok.op = IR_OP_LABEL;
  ok.location = location;
  ok.text = ok_label;
  if (!ir_emit(context, function, &ok)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  free(trap_label);
  free(ok_label);
  return 1;
}

int ir_emit_bounds_check(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *index, size_t array_size) {
  if (!context || !function || !index) {
    return 0;
  }
  if (!context->emit_runtime_checks) {
    return 1;
  }

  IROperand in_bounds = ir_operand_none();
  if (!ir_make_temp_operand(context, &in_bounds)) {
    return 0;
  }

  IRInstruction compare = {0};
  compare.op = IR_OP_BINARY;
  compare.location = location;
  compare.dest = in_bounds;
  compare.lhs = *index;
  compare.rhs = ir_operand_int((long long)array_size);
  compare.text = "<";
  if (!ir_emit(context, function, &compare)) {
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  char *trap_label = ir_new_label_name(context, "trap_bounds");
  char *ok_label = ir_new_label_name(context, "in_bounds");
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    ir_set_error(context, "Out of memory while lowering bounds check");
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = in_bounds;
  branch.text = trap_label;
  if (!ir_emit(context, function, &branch)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction jump = {0};
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = ok_label;
  if (!ir_emit(context, function, &jump)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction trap = {0};
  trap.op = IR_OP_LABEL;
  trap.location = location;
  trap.text = trap_label;
  if (!ir_emit(context, function, &trap) ||
      !ir_emit_runtime_trap_ex(context, function, location, 2u,
                               "Fatal error: Array index out of bounds", index,
                               &compare.rhs)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction ok = {0};
  ok.op = IR_OP_LABEL;
  ok.location = location;
  ok.text = ok_label;
  if (!ir_emit(context, function, &ok)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  free(trap_label);
  free(ok_label);
  ir_operand_destroy(&in_bounds);
  return 1;
}

int ir_push_labeled_control_frame(IRLoweringContext *context,
                                         const char *break_label,
                                         const char *continue_label,
                                         const char *user_label) {
  if (!context) {
    return 0;
  }

  if (context->control_count >= context->control_capacity) {
    size_t new_capacity =
        context->control_capacity == 0 ? 8 : context->control_capacity * 2;
    IRControlFrame *new_stack =
        realloc(context->control_stack, new_capacity * sizeof(IRControlFrame));
    if (!new_stack) {
      ir_set_error(context,
                   "Out of memory while growing IR control-flow stack");
      return 0;
    }
    context->control_stack = new_stack;
    context->control_capacity = new_capacity;
  }

  IRControlFrame *frame = &context->control_stack[context->control_count++];
  frame->break_label = break_label ? mettle_strdup(break_label) : NULL;
  frame->continue_label =
      continue_label ? mettle_strdup(continue_label) : NULL;
  frame->user_label = user_label ? mettle_strdup(user_label) : NULL;
  if ((break_label && !frame->break_label) ||
      (continue_label && !frame->continue_label) ||
      (user_label && !frame->user_label)) {
    free(frame->break_label);
    free(frame->continue_label);
    free(frame->user_label);
    frame->break_label = NULL;
    frame->continue_label = NULL;
    frame->user_label = NULL;
    context->control_count--;
    ir_set_error(context, "Out of memory while setting up control-flow labels");
    return 0;
  }
  return 1;
}

int ir_push_control_frame(IRLoweringContext *context,
                                 const char *break_label,
                                 const char *continue_label) {
  return ir_push_labeled_control_frame(context, break_label, continue_label,
                                       NULL);
}

void ir_pop_control_frame(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return;
  }

  IRControlFrame *frame = &context->control_stack[context->control_count - 1];
  free(frame->break_label);
  free(frame->continue_label);
  free(frame->user_label);
  frame->break_label = NULL;
  frame->continue_label = NULL;
  frame->user_label = NULL;
  context->control_count--;
}

const char *ir_current_break_label(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return NULL;
  }
  return context->control_stack[context->control_count - 1].break_label;
}

const char *ir_current_continue_label(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return NULL;
  }

  for (size_t i = context->control_count; i > 0; i--) {
    const char *label = context->control_stack[i - 1].continue_label;
    if (label) {
      return label;
    }
  }
  return NULL;
}

const char *ir_find_labeled_break(IRLoweringContext *context,
                                         const char *user_label) {
  if (!context || !user_label) {
    return NULL;
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
      return frame->break_label;
    }
  }
  return NULL;
}

const char *ir_find_labeled_continue(IRLoweringContext *context,
                                            const char *user_label) {
  if (!context || !user_label) {
    return NULL;
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
      return frame->continue_label;
    }
  }
  return NULL;
}
