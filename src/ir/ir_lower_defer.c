// AST->IR lowering: defer-statement capture and replay.
#include "ir_lowering_internal.h"

int ir_defer_stack_push(IRLoweringContext *context, IRDeferStack *stack,
                               ASTNode *node, int is_err) {
  (void)context;
  if (!stack || !node) {
    return 0;
  }
  if (stack->count >= stack->capacity) {
    size_t new_capacity = stack->capacity == 0 ? 8 : stack->capacity * 2;
    void *grown =
        realloc(stack->entries, new_capacity * sizeof(*stack->entries));
    if (!grown) {
      return 0;
    }
    stack->entries = grown;
    stack->capacity = new_capacity;
  }
  stack->entries[stack->count].node = node;
  stack->entries[stack->count].is_err = is_err;
  stack->entries[stack->count].capture_call_name = NULL;
  stack->entries[stack->count].capture_arg_temps = NULL;
  stack->entries[stack->count].capture_arg_count = 0;
  stack->count++;
  return 1;
}

// Release a defer stack's entries along with any by-value capture metadata.
void ir_defer_stack_free(IRDeferStack *stack) {
  if (!stack) {
    return;
  }
  for (size_t i = 0; i < stack->count; i++) {
    if (stack->entries[i].capture_arg_temps) {
      for (size_t j = 0; j < stack->entries[i].capture_arg_count; j++) {
        free(stack->entries[i].capture_arg_temps[j]);
      }
      free(stack->entries[i].capture_arg_temps);
    }
    free(stack->entries[i].capture_call_name);
  }
  free(stack->entries);
  stack->entries = NULL;
  stack->count = 0;
  stack->capacity = 0;
}

// If `defer_node` defers a direct function call with arguments, snapshot each
// argument value into a fresh temp local at the current (defer-point) position
// and record the call name + temp names for by-value replay. Returns 1 when the
// call was captured; 0 means the deferred statement should be replayed by
// re-lowering its AST (no arguments, a method/indirect call, or an argument
// whose type we cannot snapshot). Returns -1 if a real error occurred while
// emitting the snapshots (an error has been set on the context).
int ir_defer_capture_call(IRLoweringContext *context,
                                 IRFunction *function, ASTNode *defer_node,
                                 char **out_call_name, char ***out_temps,
                                 size_t *out_count) {
  *out_call_name = NULL;
  *out_temps = NULL;
  *out_count = 0;

  DeferStatement *defer_stmt = (DeferStatement *)defer_node->data;
  if (!defer_stmt || !defer_stmt->statement ||
      defer_stmt->statement->type != AST_FUNCTION_CALL) {
    return 0;
  }
  CallExpression *call = (CallExpression *)defer_stmt->statement->data;
  if (!call || !call->function_name || call->argument_count == 0) {
    return 0;
  }
  // Method calls (with a receiver) and indirect calls keep by-reference replay.
  if (call->object || call->is_indirect_call) {
    return 0;
  }
  if (strcmp(call->function_name, "sizeof") == 0 ||
      strcmp(call->function_name, "static_assert") == 0) {
    return 0;
  }
  // Each argument needs a concrete resolved type so the snapshot local can be
  // declared with an explicit type (required by the binary backend).
  for (size_t i = 0; i < call->argument_count; i++) {
    if (!call->arguments[i] || !call->arguments[i]->resolved_type ||
        !call->arguments[i]->resolved_type->name) {
      return 0;
    }
  }

  char **temps = calloc(call->argument_count, sizeof(char *));
  if (!temps) {
    ir_set_error(context, "Out of memory capturing deferred call arguments");
    return -1;
  }

  for (size_t i = 0; i < call->argument_count; i++) {
    const char *type_name = call->arguments[i]->resolved_type->name;
    char *temp_name = ir_new_label_name(context, "defer_cap");
    if (!temp_name) {
      goto fail;
    }
    temps[i] = temp_name;

    IRInstruction decl = {0};
    decl.op = IR_OP_DECLARE_LOCAL;
    decl.location = defer_node->location;
    decl.dest = ir_operand_symbol(temp_name);
    decl.text = (char *)type_name;
    if (!decl.dest.name || !ir_emit(context, function, &decl)) {
      ir_operand_destroy(&decl.dest);
      goto fail;
    }
    ir_operand_destroy(&decl.dest);

    IROperand value = ir_operand_none();
    if (!ir_lower_expression(context, function, call->arguments[i], &value)) {
      goto fail;
    }
    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = defer_node->location;
    assign.dest = ir_operand_symbol(temp_name);
    assign.lhs = value;
    ir_assign_apply_float_bits(&assign, &assign.lhs,
                               ir_named_type_float_bits(context, type_name));
    if (!assign.dest.name || !ir_emit(context, function, &assign)) {
      ir_operand_destroy(&assign.dest);
      ir_operand_destroy(&value);
      goto fail;
    }
    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(&value);
  }

  *out_call_name = strdup(call->function_name);
  if (!*out_call_name) {
    goto fail;
  }
  *out_temps = temps;
  *out_count = call->argument_count;
  return 1;

fail:
  for (size_t i = 0; i < call->argument_count; i++) {
    free(temps[i]);
  }
  free(temps);
  return -1;
}

int ir_emit_deferred_calls_filtered(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferStack *stack,
                                           int include_err) {
  if (!context || !function || !stack) {
    return 1;
  }
  for (size_t i = stack->count; i > 0; i--) {
    ASTNode *defer_node = stack->entries[i - 1].node;
    int is_err = stack->entries[i - 1].is_err;
    if (!include_err && is_err) {
      continue;
    }
    if (!defer_node || (defer_node->type != AST_DEFER_STATEMENT &&
                        defer_node->type != AST_ERRDEFER_STATEMENT)) {
      continue;
    }

    // By-value capture: replay the call against the argument snapshots taken at
    // the defer point instead of re-evaluating the original argument exprs.
    if (stack->entries[i - 1].capture_call_name) {
      size_t argc = stack->entries[i - 1].capture_arg_count;
      char **temps = stack->entries[i - 1].capture_arg_temps;
      IROperand dest = ir_operand_none();
      if (!ir_make_temp_operand(context, &dest)) {
        return 0;
      }
      IROperand *args = NULL;
      if (argc > 0) {
        args = calloc(argc, sizeof(IROperand));
        if (!args) {
          ir_operand_destroy(&dest);
          ir_set_error(context, "Out of memory replaying deferred call");
          return 0;
        }
        for (size_t a = 0; a < argc; a++) {
          args[a] = ir_operand_symbol(temps[a]);
        }
      }
      IRInstruction call_inst = {0};
      call_inst.op = IR_OP_CALL;
      call_inst.location = defer_node->location;
      call_inst.dest = dest;
      call_inst.text = stack->entries[i - 1].capture_call_name;
      call_inst.arguments = args;
      call_inst.argument_count = argc;
      int ok = ir_emit(context, function, &call_inst);
      for (size_t a = 0; a < argc; a++) {
        ir_operand_destroy(&args[a]);
      }
      free(args);
      ir_operand_destroy(&dest);
      if (!ok) {
        return 0;
      }
      continue;
    }

    DeferStatement *defer_stmt = (DeferStatement *)defer_node->data;
    if (!defer_stmt || !defer_stmt->statement) {
      continue;
    }
    if (!ir_lower_deferred_statement(context, function,
                                     defer_stmt->statement)) {
      return 0;
    }
  }
  return 1;
}

int ir_lower_deferred_statement(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *statement) {
  if (!statement) {
    return 1;
  }

  IRDeferScope deferred_scope = {0};
  int ok = ir_lower_statement_with_defers(context, function, statement,
                                          &deferred_scope);
  if (ok) {
    ok = ir_emit_deferred_scopes_non_err(context, function, &deferred_scope);
  }
  ir_defer_stack_free(&deferred_scope.stack);
  return ok;
}

int ir_emit_deferred_calls(IRLoweringContext *context,
                                  IRFunction *function,
                                  const IRDeferStack *stack) {
  return ir_emit_deferred_calls_filtered(context, function, stack, 1);
}

int ir_emit_deferred_calls_non_err(IRLoweringContext *context,
                                          IRFunction *function,
                                          const IRDeferStack *stack) {
  return ir_emit_deferred_calls_filtered(context, function, stack, 0);
}

int ir_emit_deferred_scopes(IRLoweringContext *context,
                                   IRFunction *function,
                                   const IRDeferScope *scope) {
  for (const IRDeferScope *current = scope; current;
       current = current->parent) {
    if (!ir_emit_deferred_calls(context, function, &current->stack)) {
      return 0;
    }
  }
  return 1;
}

int ir_emit_deferred_scopes_non_err(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferScope *scope) {
  for (const IRDeferScope *current = scope; current;
       current = current->parent) {
    if (!ir_emit_deferred_calls_non_err(context, function, &current->stack)) {
      return 0;
    }
  }
  return 1;
}

int ir_emit_return_with_defers(IRLoweringContext *context,
                                      IRFunction *function,
                                      IRDeferScope *defers, IROperand *value,
                                      SourceLocation location) {
  if (!context || !function || !value) {
    return 0;
  }

  if (defers) {
    IROperand is_error = ir_operand_none();
    if (!ir_make_temp_operand(context, &is_error)) {
      return 0;
    }

    IRInstruction set_error = {0};
    set_error.op = IR_OP_ASSIGN;
    set_error.location = location;
    set_error.dest = is_error;
    set_error.lhs =
        (value->kind == IR_OPERAND_NONE) ? ir_operand_int(0) : *value;
    if (!ir_emit(context, function, &set_error)) {
      ir_operand_destroy(&is_error);
      return 0;
    }

    char *success_label = ir_new_label_name(context, "errdefer_ok");
    char *end_label = ir_new_label_name(context, "errdefer_end");
    if (!success_label || !end_label) {
      free(success_label);
      free(end_label);
      ir_operand_destroy(&is_error);
      ir_set_error(context,
                   "Out of memory while allocating errdefer return labels");
      return 0;
    }

    IRInstruction branch = {0};
    branch.op = IR_OP_BRANCH_ZERO;
    branch.location = location;
    branch.lhs = is_error;
    branch.text = success_label;
    if (!ir_emit(context, function, &branch)) {
      free(success_label);
      free(end_label);
      ir_operand_destroy(&is_error);
      return 0;
    }

    if (!ir_emit_deferred_scopes(context, function, defers) ||
        !ir_emit_jump_instruction(context, function, end_label, location) ||
        !ir_emit_label_instruction(context, function, success_label,
                                   location) ||
        !ir_emit_deferred_scopes_non_err(context, function, defers) ||
        !ir_emit_label_instruction(context, function, end_label, location)) {
      free(success_label);
      free(end_label);
      ir_operand_destroy(&is_error);
      return 0;
    }

    free(success_label);
    free(end_label);
    ir_operand_destroy(&is_error);
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_RETURN;
  instruction.location = location;
  instruction.lhs = *value;
  if (value->kind != IR_OPERAND_NONE) {
    int return_bits =
        ir_named_type_float_bits(context, context->current_return_type_name);
    if (return_bits) {
      ir_assign_apply_float_bits(&instruction, &instruction.lhs, return_bits);
      value->float_bits = instruction.lhs.float_bits;
    }
  }
  if (!ir_emit(context, function, &instruction)) {
    return 0;
  }

  *value = ir_operand_none();
  return 1;
}
