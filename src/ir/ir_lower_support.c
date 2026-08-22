// AST->IR lowering: runtime checks and control-flow (break/continue) frames.
#include "ir_lowering_internal.h"

/* Local name bindings. See IRLocalBinding for why a redeclaration at a
 * different type needs a name of its own. */

void ir_local_scope_enter(IRLoweringContext *context) {
  if (context) {
    context->local_scope_depth++;
  }
}

void ir_local_scope_leave(IRLoweringContext *context) {
  if (!context) {
    return;
  }
  for (size_t i = context->local_binding_count; i-- > 0;) {
    if (context->local_bindings[i].depth < context->local_scope_depth) {
      break;
    }
    context->local_bindings[i].active = 0;
  }
  if (context->local_scope_depth > 0) {
    context->local_scope_depth--;
  }
}

void ir_local_bindings_reset(IRLoweringContext *context) {
  if (!context) {
    return;
  }
  for (size_t i = 0; i < context->local_binding_count; i++) {
    if (context->local_bindings[i].owns_ir_name) {
      free((char *)context->local_bindings[i].ir_name);
    }
  }
  free(context->local_bindings);
  context->local_bindings = NULL;
  context->local_binding_count = 0;
  context->local_binding_capacity = 0;
  context->local_scope_depth = 0;
  context->local_rename_serial = 0;
}

static int ir_local_type_text_matches(const char *a, const char *b) {
  if (!a || !b) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

const char *ir_local_bind(IRLoweringContext *context, const char *name,
                          const char *type_text) {
  if (!context || !name) {
    return name;
  }

  const char *ir_name = NULL;
  const char *reusable = NULL;
  int seen = 0;
  int owns = 0;
  int active = 0;
  for (size_t i = 0; i < context->local_binding_count; i++) {
    const IRLocalBinding *b = &context->local_bindings[i];
    if (strcmp(b->name, name) != 0) {
      continue;
    }
    seen = 1;
    /* A binding whose scope has ended can lend its slot: two `var x: int32` in
     * sibling blocks are never live at once, so sharing costs nothing and the
     * emitted IR is unchanged. One that is still active cannot. A `var x`
     * nested inside another `var x`'s scope is a second variable, and sharing
     * the slot made the inner one write through -- the outer read 5 back from
     * an inner block that set 5, and a loop body's `var x` survived the loop. */
    if (b->active) {
      active = 1;
      continue;
    }
    if (!reusable && ir_local_type_text_matches(b->type_text, type_text)) {
      reusable = b->ir_name;
    }
  }
  if (!active) {
    ir_name = reusable;
  }
  if (!ir_name) {
    if (!seen) {
      ir_name = name;
    } else {
      size_t len = strlen(name) + 24;
      char *renamed = (char *)malloc(len);
      if (!renamed) {
        /* Out of memory: keep the source name. The declaration still lowers;
         * it just shares a slot the way it did before, as no rename happened. */
        return name;
      }
      /* `$$`, not `$`: SROA names a split field `<member>$<offset>`, so a
       * single `$` could collide with the scalars of a same-named struct in
       * the same function. Source identifiers cannot contain either. */
      snprintf(renamed, len, "%s$$%d", name, ++context->local_rename_serial);
      ir_name = renamed;
      owns = 1;
    }
  }

  if (context->local_binding_count == context->local_binding_capacity) {
    size_t grown = context->local_binding_capacity
                       ? context->local_binding_capacity * 2
                       : 8;
    IRLocalBinding *items = (IRLocalBinding *)realloc(
        context->local_bindings, grown * sizeof(IRLocalBinding));
    if (!items) {
      if (owns) {
        free((char *)ir_name);
      }
      return name;
    }
    context->local_bindings = items;
    context->local_binding_capacity = grown;
  }

  IRLocalBinding *slot = &context->local_bindings[context->local_binding_count++];
  slot->name = name;
  slot->ir_name = ir_name;
  slot->type_text = type_text;
  slot->depth = context->local_scope_depth;
  slot->active = 1;
  slot->owns_ir_name = owns;
  return ir_name;
}

const IRLocalBinding *ir_local_binding_find(IRLoweringContext *context,
                                            const char *name) {
  if (!context || !name) {
    return NULL;
  }
  for (size_t i = context->local_binding_count; i-- > 0;) {
    const IRLocalBinding *b = &context->local_bindings[i];
    if (b->active && strcmp(b->name, name) == 0) {
      return b;
    }
  }
  return NULL;
}

const char *ir_local_ir_name(IRLoweringContext *context, const char *name) {
  const IRLocalBinding *b = ir_local_binding_find(context, name);
  return b ? b->ir_name : name;
}

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

int ir_emit_safety_check(IRLoweringContext *context, IRFunction *function,
                         SourceLocation location, const IROperand *base,
                         const IROperand *offset, long long access_size,
                         long long extent, int access_kind, const char *what) {
  if (!context || !function || !base || !offset) {
    return 0;
  }
  if (!context->emit_safety_checks) {
    return 1;
  }
  /* A zero-width access reads nothing, and an object of unknown element size
   * gives the check no range to test. Neither can fail, so neither is worth a
   * check. */
  if (access_size <= 0) {
    return 1;
  }

  IRInstruction check = {0};
  check.op = IR_OP_SAFETY_CHECK;
  check.location = location;
  check.text = (char *)what;
  check.arguments = calloc(IR_SAFETY_ARG_COUNT, sizeof(IROperand));
  if (!check.arguments) {
    ir_set_error(context, "Out of memory while lowering safety check");
    return 0;
  }
  check.argument_count = IR_SAFETY_ARG_COUNT;
  check.arguments[IR_SAFETY_ARG_BASE] = ir_operand_copy(base);
  check.arguments[IR_SAFETY_ARG_OFFSET] = ir_operand_copy(offset);
  check.arguments[IR_SAFETY_ARG_SIZE] = ir_operand_int(access_size);
  check.arguments[IR_SAFETY_ARG_EXTENT] = ir_operand_int(extent);
  check.arguments[IR_SAFETY_ARG_ACCESS] = ir_operand_int(access_kind);

  int emitted = ir_emit(context, function, &check);
  for (size_t i = 0; i < IR_SAFETY_ARG_COUNT; i++) {
    ir_operand_destroy(&check.arguments[i]);
  }
  free(check.arguments);
  return emitted;
}

int ir_push_labeled_control_frame(IRLoweringContext *context,
                                         const char *break_label,
                                         const char *continue_label,
                                         const char *user_label,
                                         IRDeferScope *defers) {
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
  frame->defers = defers;
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
                                 const char *continue_label,
                                 IRDeferScope *defers) {
  return ir_push_labeled_control_frame(context, break_label, continue_label,
                                       NULL, defers);
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

/* The label lookups above answer where the jump goes. These answer which
   frame owns it, which is what the deferred statements between here and there
   are measured against. The search rules match one for one: a bare `break`
   takes the innermost frame, a bare `continue` the innermost frame that has a
   continue label (a switch has none), and a labeled form the frame carrying
   that name. */
const IRControlFrame *ir_break_target_frame(IRLoweringContext *context,
                                            const char *user_label) {
  if (!context || context->control_count == 0) {
    return NULL;
  }
  if (!user_label) {
    return &context->control_stack[context->control_count - 1];
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
      return frame->break_label ? frame : NULL;
    }
  }
  return NULL;
}

const IRControlFrame *ir_continue_target_frame(IRLoweringContext *context,
                                               const char *user_label) {
  if (!context || context->control_count == 0) {
    return NULL;
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (user_label) {
      if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
        return frame->continue_label ? frame : NULL;
      }
      continue;
    }
    if (frame->continue_label) {
      return frame;
    }
  }
  return NULL;
}
