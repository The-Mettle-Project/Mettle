// AST->IR lowering: statement lowering (with defer scopes).
#include "ir_lowering_internal.h"

int ir_lower_statement_with_defers(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *statement,
                                          IRDeferScope *defers) {
  if (!context || !function || !statement) {
    return 0;
  }

  switch (statement->type) {
  case AST_PROGRAM: {
    Program *program = (Program *)statement->data;
    if (!program) {
      return 1;
    }
    if (!defers) {
      for (size_t i = 0; i < program->declaration_count; i++) {
        if (!ir_lower_statement_with_defers(context, function,
                                            program->declarations[i], NULL)) {
          return 0;
        }
      }
      return 1;
    }

    IRDeferScope block_scope = {0};
    block_scope.parent = defers;
    for (size_t i = 0; i < program->declaration_count; i++) {
      if (!ir_lower_statement_with_defers(
              context, function, program->declarations[i], &block_scope)) {
        ir_defer_stack_free(&block_scope.stack);
        return 0;
      }
    }

    int ok =
        ir_emit_deferred_calls_non_err(context, function, &block_scope.stack);
    ir_defer_stack_free(&block_scope.stack);
    return ok;
  }

  case AST_VAR_DECLARATION: {
    VarDeclaration *declaration = (VarDeclaration *)statement->data;
    if (!declaration || !declaration->name) {
      ir_set_error(context, "Malformed variable declaration");
      return 0;
    }

    // Top-level `const` is folded at use sites (SYMBOL_CONSTANT) and never
    // reaches this local-statement path. A local `const` is an immutable local
    // variable: it gets normal storage and initialization here, and the type
    // checker rejects reassignment.

    IRInstruction local = {0};
    local.op = IR_OP_DECLARE_LOCAL;
    local.location = statement->location;
    local.dest = ir_operand_symbol(declaration->name);
    local.text = declaration->type_name;
    // For inferred-type locals (`var x = expr;`) the declaration carries no
    // type_name. The binary/direct-object backend resolves a local's type from
    // this textual payload, so fall back to the name of the type the checker
    // inferred for the initializer. The Type (and its name) outlives codegen,
    // matching the lifetime of the type_name pointer used above, and `text` is
    // never freed by the IR. Leaving it NULL is harmless for the asm backend.
    if (!local.text && declaration->initializer &&
        declaration->initializer->resolved_type) {
      local.text = declaration->initializer->resolved_type->name;
    }
    if (!local.dest.name) {
      ir_set_error(context,
                   "Out of memory while lowering variable declaration");
      return 0;
    }
    if (!ir_emit(context, function, &local)) {
      ir_operand_destroy(&local.dest);
      return 0;
    }
    ir_operand_destroy(&local.dest);

    if (declaration->initializer) {
      IROperand value = ir_operand_none();
      if (!ir_lower_expression(context, function, declaration->initializer,
                               &value)) {
        return 0;
      }
      Type *decl_type = ir_resolve_named_type(context, declaration->type_name);
      if (!decl_type) {
        decl_type = declaration->initializer
                        ? declaration->initializer->resolved_type
                        : NULL;
      }
      if (ir_should_coerce_string_to_cstring(context, decl_type,
                                             declaration->initializer) &&
          !ir_coerce_string_operand_to_cstring(
              context, function, &value, declaration->initializer->location)) {
        ir_operand_destroy(&value);
        return 0;
      }
      if (ir_try_emit_aggregate_symbol_memcpy(context, function,
                                              declaration->name, &value,
                                              decl_type, statement->location)) {
        ir_operand_destroy(&value);
      } else {
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = statement->location;
        assign.dest = ir_operand_symbol(declaration->name);
        assign.lhs = value;
        ir_assign_apply_float_bits(
            &assign, &assign.lhs,
            ir_named_type_float_bits(context, declaration->type_name));
        if (!assign.dest.name) {
          ir_operand_destroy(&value);
          ir_set_error(context,
                       "Out of memory while lowering variable initializer");
          return 0;
        }
        if (!ir_emit(context, function, &assign)) {
          ir_operand_destroy(&assign.dest);
          ir_operand_destroy(&value);
          return 0;
        }
        ir_operand_destroy(&assign.dest);
        ir_operand_destroy(&value);
      }
    }
    return 1;
  }

  case AST_ASSIGNMENT: {
    Assignment *assignment = (Assignment *)statement->data;
    if (!assignment || !assignment->value) {
      ir_set_error(context, "Malformed assignment statement");
      return 0;
    }

    IROperand value = ir_operand_none();
    if (!ir_lower_expression(context, function, assignment->value, &value)) {
      return 0;
    }

    if (assignment->variable_name) {
      Type *assign_type =
          ir_lookup_symbol_type(context, assignment->variable_name);
      if (!assign_type && assignment->value) {
        assign_type = assignment->value->resolved_type;
      }
      if (ir_should_coerce_string_to_cstring(context, assign_type,
                                             assignment->value) &&
          !ir_coerce_string_operand_to_cstring(
              context, function, &value, assignment->value->location)) {
        ir_operand_destroy(&value);
        return 0;
      }
      if (ir_try_emit_aggregate_symbol_memcpy(
              context, function, assignment->variable_name, &value,
              assign_type, statement->location)) {
        ir_operand_destroy(&value);
        return 1;
      }

      {
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = statement->location;
        assign.dest = ir_operand_symbol(assignment->variable_name);
        assign.lhs = value;
        /* Target float width for the narrowing/widening on store. Prefer the
         * symbol table, but its function scope is usually popped by lowering
         * time, so fall back to the declared type recorded on the emitted
         * DECLARE_LOCAL. Without this, a float64 expression assigned into a
         * float32 local kept the float64 width and stored the wrong 4 bytes.
         * Gate the IR scan on a floating RHS so non-float assigns stay O(1). */
        int target_float_bits =
            ir_symbol_float_bits(context, assignment->variable_name);
        if (target_float_bits == 0 && assignment->value &&
            assignment->value->resolved_type &&
            (assignment->value->resolved_type->kind == TYPE_FLOAT32 ||
             assignment->value->resolved_type->kind == TYPE_FLOAT64)) {
          target_float_bits = ir_local_declared_float_bits(
              context, function, assignment->variable_name);
        }
        ir_assign_apply_float_bits(&assign, &assign.lhs, target_float_bits);
        if (!assign.dest.name) {
          ir_operand_destroy(&value);
          ir_set_error(context, "Out of memory while lowering assignment target");
          return 0;
        }

        if (!ir_emit(context, function, &assign)) {
          ir_operand_destroy(&assign.dest);
          ir_operand_destroy(&value);
          return 0;
        }

        ir_operand_destroy(&assign.dest);
        ir_operand_destroy(&value);
        return 1;
      }
    }

    if (!assignment->target) {
      ir_operand_destroy(&value);
      ir_set_error(context, "Assignment target is missing");
      return 0;
    }

    IROperand address = ir_operand_none();
    Type *target_type = NULL;
    if (!ir_lower_lvalue_address(context, function, assignment->target,
                                 &address, &target_type)) {
      ir_operand_destroy(&value);
      return 0;
    }

    if (!target_type) {
      ir_operand_destroy(&address);
      ir_operand_destroy(&value);
      ir_set_error(context, "Cannot assign to unknown target type");
      return 0;
    }

    if (ir_should_coerce_string_to_cstring(context, target_type,
                                           assignment->value) &&
        !ir_coerce_string_operand_to_cstring(
            context, function, &value, assignment->value->location)) {
      ir_operand_destroy(&address);
      ir_operand_destroy(&value);
      return 0;
    }

    /* Aggregate destinations (struct fields, indexed struct elements) must copy
     * the whole struct. A plain IR_OP_STORE of an aggregate RHS only moves one
     * word, silently dropping everything past the first 8 bytes. */
    if (ir_try_emit_aggregate_address_memcpy(context, function, &address, &value,
                                             target_type,
                                             statement->location)) {
      ir_operand_destroy(&address);
      ir_operand_destroy(&value);
      return 1;
    }

    IRInstruction store = {0};
    store.op = IR_OP_STORE;
    store.location = statement->location;
    store.dest = address;
    store.lhs = value;
    store.rhs = ir_operand_int(ir_type_storage_size(target_type));
    if (target_type->kind == TYPE_FLOAT32 ||
        target_type->kind == TYPE_FLOAT64) {
      ir_assign_apply_float_bits(&store, &store.lhs,
                                 ir_type_float_bits(target_type));
    }
    if (!ir_emit(context, function, &store)) {
      ir_operand_destroy(&address);
      ir_operand_destroy(&value);
      return 0;
    }

    ir_operand_destroy(&address);
    ir_operand_destroy(&value);
    return 1;
  }

  case AST_FUNCTION_CALL: {
    IROperand ignored = ir_operand_none();
    int ok = ir_lower_expression(context, function, statement, &ignored);
    ir_operand_destroy(&ignored);
    return ok;
  }

  case AST_RETURN_STATEMENT: {
    ReturnStatement *ret = (ReturnStatement *)statement->data;
    IROperand value = ir_operand_none();
    if (ret && ret->value) {
      if (!ir_lower_expression(context, function, ret->value, &value)) {
        return 0;
      }
      Type *return_type =
          ir_resolve_named_type(context, context->current_return_type_name);
      if (ir_should_coerce_string_to_cstring(context, return_type,
                                             ret->value) &&
          !ir_coerce_string_operand_to_cstring(context, function, &value,
                                               ret->value->location)) {
        ir_operand_destroy(&value);
        return 0;
      }
    }
    if (!ir_emit_return_with_defers(context, function, defers, &value,
                                    statement->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
    ir_operand_destroy(&value);
    return 1;
  }

  case AST_INLINE_ASM: {
    InlineAsm *inline_asm = (InlineAsm *)statement->data;
    if (!inline_asm || !inline_asm->assembly_code) {
      ir_set_error(context, "Malformed inline assembly statement");
      return 0;
    }
    IRInstruction instruction = {0};
    instruction.op = IR_OP_INLINE_ASM;
    instruction.location = statement->location;
    instruction.text = inline_asm->assembly_code;
    return ir_emit(context, function, &instruction);
  }

  case AST_IF_STATEMENT: {
    IfStatement *if_data = (IfStatement *)statement->data;
    if (!if_data || !if_data->condition || !if_data->then_branch) {
      ir_set_error(context, "Malformed if statement");
      return 0;
    }

    char *end_label = ir_new_label_name(context, "if_end");
    if (!end_label) {
      ir_set_error(context, "Out of memory while allocating if labels");
      return 0;
    }

    ASTNode *current_cond = if_data->condition;
    ASTNode *current_body = if_data->then_branch;

    for (size_t i = 0; i <= if_data->else_if_count; i++) {
      char *next_label = ir_new_label_name(context, "if_next");
      if (!next_label) {
        free(end_label);
        return 0;
      }

      if (!ir_emit_condition_false_branch(context, function, current_cond,
                                          next_label)) {
        free(next_label);
        free(end_label);
        return 0;
      }

      if (!ir_lower_statement_with_defers(context, function, current_body,
                                          defers)) {
        free(next_label);
        free(end_label);
        return 0;
      }

      if (!ir_emit_jump_instruction(context, function, end_label,
                                    current_cond->location)) {
        free(next_label);
        free(end_label);
        return 0;
      }

      if (!ir_emit_label_instruction(context, function, next_label,
                                     current_cond->location)) {
        free(next_label);
        free(end_label);
        return 0;
      }
      free(next_label);

      if (i < if_data->else_if_count) {
        current_cond = if_data->else_ifs[i].condition;
        current_body = if_data->else_ifs[i].body;
      }
    }

    if (if_data->else_branch &&
        !ir_lower_statement_with_defers(context, function, if_data->else_branch,
                                        defers)) {
      free(end_label);
      return 0;
    }

    if (!ir_emit_label_instruction(context, function, end_label,
                                   statement->location)) {
      free(end_label);
      return 0;
    }

    free(end_label);
    return 1;
  }

  case AST_WHILE_STATEMENT: {
    WhileStatement *while_data = (WhileStatement *)statement->data;
    if (!while_data || !while_data->condition || !while_data->body) {
      ir_set_error(context, "Malformed while statement");
      return 0;
    }

    char *loop_start = ir_new_label_name(context, "while");
    char *loop_end = ir_new_label_name(context, "while_end");
    if (!loop_start || !loop_end) {
      free(loop_start);
      free(loop_end);
      ir_set_error(context, "Out of memory while allocating while labels");
      return 0;
    }

    int while_simd_mode = while_data->simd_mode != SIMD_ATTR_NONE
                              ? while_data->simd_mode
                              : context->current_function_simd_default;
    if (while_simd_mode == SIMD_ATTR_NONE && g_ir_lowering_explain) {
      while_simd_mode = SIMD_ATTR_REPORT;
    }
    int while_simd_id = -1;
    if (while_simd_mode != SIMD_ATTR_NONE) {
      while_simd_id = context->next_simd_request_id++;
      if (!ir_emit_simd_marker(context, function, 'B', while_simd_id,
                               while_simd_mode, statement->location)) {
        free(loop_start);
        free(loop_end);
        return 0;
      }
    }

    if (!ir_emit_label_instruction(context, function, loop_start,
                                   statement->location)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    if (!ir_emit_condition_false_branch(context, function,
                                        while_data->condition, loop_end)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    if (!ir_push_labeled_control_frame(context, loop_end, loop_start,
                                       while_data->label)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    int body_ok = ir_lower_statement_with_defers(context, function,
                                                 while_data->body, defers);
    ir_pop_control_frame(context);
    if (!body_ok) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    if (!ir_emit_jump_instruction(context, function, loop_start,
                                  statement->location) ||
        !ir_emit_label_instruction(context, function, loop_end,
                                   statement->location)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    if (while_simd_id >= 0 &&
        !ir_emit_simd_marker(context, function, 'E', while_simd_id, 0,
                             statement->location)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    free(loop_start);
    free(loop_end);
    return 1;
  }

  case AST_FOR_STATEMENT: {
    ForStatement *for_data = (ForStatement *)statement->data;
    if (!for_data || !for_data->body) {
      ir_set_error(context, "Malformed for statement");
      return 0;
    }

    char *condition_label = ir_new_label_name(context, "for_cond");
    char *step_label = ir_new_label_name(context, "for_step");
    char *end_label = ir_new_label_name(context, "for_end");
    if (!condition_label || !step_label || !end_label) {
      free(condition_label);
      free(step_label);
      free(end_label);
      ir_set_error(context, "Out of memory while allocating for-loop labels");
      return 0;
    }

    int for_simd_mode = for_data->simd_mode != SIMD_ATTR_NONE
                            ? for_data->simd_mode
                            : context->current_function_simd_default;
    if (for_simd_mode == SIMD_ATTR_NONE && g_ir_lowering_explain) {
      for_simd_mode = SIMD_ATTR_REPORT;
    }
    int for_simd_id = -1;
    if (for_simd_mode != SIMD_ATTR_NONE) {
      for_simd_id = context->next_simd_request_id++;
      if (!ir_emit_simd_marker(context, function, 'B', for_simd_id,
                               for_simd_mode, statement->location)) {
        free(condition_label);
        free(step_label);
        free(end_label);
        return 0;
      }
    }

    if (!ir_lower_statement_or_expression(context, function,
                                          for_data->initializer)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (!ir_emit_label_instruction(context, function, condition_label,
                                   statement->location)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (for_data->condition) {
      if (!ir_emit_condition_false_branch(context, function,
                                          for_data->condition, end_label)) {
        free(condition_label);
        free(step_label);
        free(end_label);
        return 0;
      }
    }

    if (!ir_push_labeled_control_frame(context, end_label, step_label,
                                       for_data->label)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    int body_ok = ir_lower_statement_with_defers(context, function,
                                                 for_data->body, defers);
    ir_pop_control_frame(context);
    if (!body_ok) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (!ir_emit_label_instruction(context, function, step_label,
                                   statement->location)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (!ir_lower_statement_or_expression(context, function,
                                          for_data->increment)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (!ir_emit_jump_instruction(context, function, condition_label,
                                  statement->location) ||
        !ir_emit_label_instruction(context, function, end_label,
                                   statement->location)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (for_simd_id >= 0 &&
        !ir_emit_simd_marker(context, function, 'E', for_simd_id, 0,
                             statement->location)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    free(condition_label);
    free(step_label);
    free(end_label);
    return 1;
  }

  case AST_SWITCH_STATEMENT:
    return ir_lower_switch_statement(context, function, statement);

  case AST_MATCH_STATEMENT: {
    MatchStatement *m = (MatchStatement *)statement->data;
    if (m && m->is_expression) {
      // match used as an expression-statement: lower it and discard the value.
      IROperand discarded = ir_operand_none();
      int r = ir_lower_match_expression(context, function, statement,
                                        &discarded);
      ir_operand_destroy(&discarded);
      return r;
    }
    return ir_lower_match_statement(context, function, statement, defers);
  }

  case AST_BREAK_STATEMENT: {
    LoopControlStatement *ctrl = (LoopControlStatement *)statement->data;
    const char *user_label = ctrl ? ctrl->target_label : NULL;
    const char *target = user_label ? ir_find_labeled_break(context, user_label)
                                    : ir_current_break_label(context);
    if (!target) {
      if (user_label) {
        ir_set_error(context, "'break %s' has no matching labeled loop",
                     user_label);
      } else {
        ir_set_error(context, "'break' used outside loop/switch");
      }
      return 0;
    }
    return ir_emit_jump_instruction(context, function, target,
                                    statement->location);
  }

  case AST_CONTINUE_STATEMENT: {
    LoopControlStatement *ctrl = (LoopControlStatement *)statement->data;
    const char *user_label = ctrl ? ctrl->target_label : NULL;
    const char *target = user_label
                             ? ir_find_labeled_continue(context, user_label)
                             : ir_current_continue_label(context);
    if (!target) {
      if (user_label) {
        ir_set_error(context, "'continue %s' has no matching labeled loop",
                     user_label);
      } else {
        ir_set_error(context, "'continue' used outside loop");
      }
      return 0;
    }
    return ir_emit_jump_instruction(context, function, target,
                                    statement->location);
  }

  case AST_DEFER_STATEMENT: {
    if (!defers) {
      return 1;
    }
    // Snapshot argument values now so the deferred call captures them by value
    // rather than re-reading the variables at scope exit.
    char *cap_name = NULL;
    char **cap_temps = NULL;
    size_t cap_count = 0;
    int captured = ir_defer_capture_call(context, function, statement,
                                         &cap_name, &cap_temps, &cap_count);
    if (captured < 0) {
      return 0;
    }
    if (!ir_defer_stack_push(context, &defers->stack, statement, 0)) {
      for (size_t i = 0; i < cap_count; i++) {
        free(cap_temps[i]);
      }
      free(cap_temps);
      free(cap_name);
      ir_set_error(context, "Out of memory while recording defer statement");
      return 0;
    }
    if (captured > 0) {
      size_t idx = defers->stack.count - 1;
      defers->stack.entries[idx].capture_call_name = cap_name;
      defers->stack.entries[idx].capture_arg_temps = cap_temps;
      defers->stack.entries[idx].capture_arg_count = cap_count;
    }
    return 1;
  }

  case AST_ERRDEFER_STATEMENT: {
    if (!defers) {
      return 1;
    }
    if (!ir_defer_stack_push(context, &defers->stack, statement, 1)) {
      ir_set_error(context, "Out of memory while recording errdefer statement");
      return 0;
    }
    return 1;
  }

  default: {
    /* Any expression usable as a bare statement (result discarded), e.g. a
     * call for its side effects. The AST_IDENTIFIER..AST_NEW_EXPRESSION range
     * covers most expression kinds contiguously; a few were added later at
     * other enum positions and are listed explicitly, notably
     * AST_FUNC_PTR_CALL: a call through a function-pointer/closure struct
     * field or expression result, used as a statement (`obj.callback(args);`).
     */
    int is_statement_expression =
        (statement->type >= AST_IDENTIFIER &&
         statement->type <= AST_NEW_EXPRESSION) ||
        statement->type == AST_FUNC_PTR_CALL ||
        statement->type == AST_CAST_EXPRESSION ||
        statement->type == AST_LAMBDA_EXPRESSION ||
        statement->type == AST_CLOSURE_ADAPT_EXPRESSION;
    if (is_statement_expression) {
      IROperand ignored = ir_operand_none();
      int ok = ir_lower_expression(context, function, statement, &ignored);
      ir_operand_destroy(&ignored);
      return ok;
    }

    ir_set_error(context, "Unsupported statement type in pure IR lowering");
    return 0;
  }
  }
}
