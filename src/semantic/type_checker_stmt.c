// Type checker: statement checking (if / for / switch / dispatch).
#include "type_checker_internal.h"

// Validation functions for semantic analysis

// Statement and expression validation functions

int type_checker_check_if_statement(TypeChecker *checker,
                                           ASTNode *statement) {
  IfStatement *if_stmt = (IfStatement *)statement->data;
  if (!if_stmt || !if_stmt->condition) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid if statement");
    return 0;
  }

  Type *condition_type = type_checker_infer_type(checker, if_stmt->condition);
  if (!condition_type) {
    return 0;
  }

  if (!type_checker_is_numeric_type(condition_type)) {
    type_checker_report_type_mismatch(checker, if_stmt->condition->location,
                                      "numeric type", condition_type->name);
    return 0;
  }

  size_t init_snapshot_count = 0;
  unsigned char *init_snapshot =
      type_checker_init_tracker_capture(checker, &init_snapshot_count);
  if (checker->tracked_var_count > 0 && !init_snapshot) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while analyzing variable initialization flow");
    return 0;
  }

  if (if_stmt->then_branch &&
      !type_checker_check_statement(checker, if_stmt->then_branch)) {
    free(init_snapshot);
    return 0;
  }
  type_checker_init_tracker_restore(checker, init_snapshot,
                                    init_snapshot_count);

  for (size_t i = 0; i < if_stmt->else_if_count; i++) {
    Type *elif_cond_type =
        type_checker_infer_type(checker, if_stmt->else_ifs[i].condition);
    if (!elif_cond_type) {
      free(init_snapshot);
      return 0;
    }
    if (!type_checker_is_numeric_type(elif_cond_type)) {
      type_checker_report_type_mismatch(
          checker, if_stmt->else_ifs[i].condition->location, "numeric type",
          elif_cond_type->name);
      free(init_snapshot);
      return 0;
    }
    if (if_stmt->else_ifs[i].body &&
        !type_checker_check_statement(checker, if_stmt->else_ifs[i].body)) {
      free(init_snapshot);
      return 0;
    }
    type_checker_init_tracker_restore(checker, init_snapshot,
                                      init_snapshot_count);
  }

  if (if_stmt->else_branch &&
      !type_checker_check_statement(checker, if_stmt->else_branch)) {
    free(init_snapshot);
    return 0;
  }
  type_checker_init_tracker_restore(checker, init_snapshot,
                                    init_snapshot_count);
  free(init_snapshot);

  return 1;
}

int type_checker_check_for_statement(TypeChecker *checker,
                                            ASTNode *statement) {
  ForStatement *for_stmt = (ForStatement *)statement->data;
  if (!for_stmt) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid for statement");
    return 0;
  }

  if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_BLOCK)) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while entering for-loop scope");
    return 0;
  }
  if (!type_checker_init_tracker_enter_scope(checker)) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while entering initialization analysis scope");
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }

  if (for_stmt->initializer) {
    int init_ok = 0;
    if (for_stmt->initializer->type == AST_VAR_DECLARATION ||
        for_stmt->initializer->type == AST_ASSIGNMENT ||
        for_stmt->initializer->type == AST_FUNCTION_CALL) {
      init_ok = type_checker_check_statement(checker, for_stmt->initializer);
    } else {
      init_ok = type_checker_check_expression(checker, for_stmt->initializer);
    }
    if (!init_ok) {
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
  }

  size_t post_init_snapshot_count = 0;
  unsigned char *post_init_snapshot =
      type_checker_init_tracker_capture(checker, &post_init_snapshot_count);
  if (checker->tracked_var_count > 0 && !post_init_snapshot) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while analyzing variable initialization flow");
    type_checker_init_tracker_exit_scope(checker);
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }

  if (for_stmt->condition) {
    Type *cond_type = type_checker_infer_type(checker, for_stmt->condition);
    if (!cond_type) {
      free(post_init_snapshot);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
    if (!type_checker_is_numeric_type(cond_type)) {
      type_checker_report_type_mismatch(checker,
                                        for_stmt->condition->location,
                                        "numeric type", cond_type->name);
      free(post_init_snapshot);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
  }

  if (for_stmt->increment &&
      !type_checker_check_expression(checker, for_stmt->increment)) {
    free(post_init_snapshot);
    type_checker_init_tracker_exit_scope(checker);
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }

  checker->loop_depth++;
  if (for_stmt->body &&
      !type_checker_check_statement(checker, for_stmt->body)) {
    checker->loop_depth--;
    free(post_init_snapshot);
    type_checker_init_tracker_exit_scope(checker);
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }
  checker->loop_depth--;

  type_checker_init_tracker_restore(checker, post_init_snapshot,
                                    post_init_snapshot_count);
  free(post_init_snapshot);
  type_checker_init_tracker_exit_scope(checker);
  symbol_table_exit_scope(checker->symbol_table);
  return 1;
}

int type_checker_check_switch_statement(TypeChecker *checker,
                                               ASTNode *statement) {
  SwitchStatement *switch_stmt = (SwitchStatement *)statement->data;
  if (!switch_stmt || !switch_stmt->expression) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid switch statement");
    return 0;
  }

  Type *switch_type =
      type_checker_infer_type(checker, switch_stmt->expression);
  if (!switch_type) {
    return 0;
  }
  if (!type_checker_is_integer_type(switch_type)) {
    type_checker_report_type_mismatch(checker,
                                      switch_stmt->expression->location,
                                      "integer type", switch_type->name);
    return 0;
  }

  size_t init_snapshot_count = 0;
  unsigned char *init_snapshot =
      type_checker_init_tracker_capture(checker, &init_snapshot_count);
  if (checker->tracked_var_count > 0 && !init_snapshot) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while analyzing variable initialization flow");
    return 0;
  }

  long long *case_values = NULL;
  size_t case_value_count = 0;
  int seen_default = 0;

  if (switch_stmt->case_count > 0) {
    case_values = malloc(switch_stmt->case_count * sizeof(long long));
    if (!case_values) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Memory allocation failed in switch validation");
      return 0;
    }
  }

  checker->switch_depth++;
  for (size_t i = 0; i < switch_stmt->case_count; i++) {
    ASTNode *case_node = switch_stmt->cases ? switch_stmt->cases[i] : NULL;
    if (!case_node || case_node->type != AST_CASE_CLAUSE) {
      type_checker_set_error_at_location(checker, statement->location,
                                         "Invalid case clause in switch");
      checker->switch_depth--;
      free(init_snapshot);
      free(case_values);
      return 0;
    }

    CaseClause *case_clause = (CaseClause *)case_node->data;
    if (!case_clause) {
      type_checker_set_error_at_location(checker, case_node->location,
                                         "Invalid case clause");
      checker->switch_depth--;
      free(init_snapshot);
      free(case_values);
      return 0;
    }

    if (case_clause->is_default) {
      if (seen_default) {
        type_checker_set_error_at_location(
            checker, case_node->location,
            "Switch may only contain one default clause");
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }
      seen_default = 1;
    } else {
      if (!case_clause->value) {
        type_checker_set_error_at_location(
            checker, case_node->location,
            "Case clause is missing a value expression");
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }

      Type *case_type = type_checker_infer_type(checker, case_clause->value);
      if (!case_type) {
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }
      if (!type_checker_is_integer_type(case_type)) {
        type_checker_report_type_mismatch(checker,
                                          case_clause->value->location,
                                          "integer type", case_type->name);
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }
      if (!type_checker_is_assignable(checker, switch_type, case_type)) {
        type_checker_report_type_mismatch(checker,
                                          case_clause->value->location,
                                          switch_type->name, case_type->name);
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }

      long long case_value = 0;
      int case_eval_ok =
          type_checker_eval_integer_constant(case_clause->value, &case_value);
      if (!case_eval_ok &&
          case_clause->value->type == AST_IDENTIFIER) {
        Identifier *cid = (Identifier *)case_clause->value->data;
        Symbol *csym =
            symbol_table_lookup(checker->symbol_table, cid->name);
        if (csym && csym->kind == SYMBOL_CONSTANT) {
          case_value = csym->data.constant.value;
          case_eval_ok = 1;
        }
      }
      /* Qualified plain-enum variant in a case: `case EnumName.Variant:`. */
      if (!case_eval_ok &&
          case_clause->value->type == AST_MEMBER_ACCESS) {
        MemberAccess *cma = (MemberAccess *)case_clause->value->data;
        if (cma && cma->object && cma->object->type == AST_IDENTIFIER &&
            cma->member) {
          Identifier *cma_obj = (Identifier *)cma->object->data;
          if (cma_obj && cma_obj->name) {
            Symbol *enum_sym =
                symbol_table_lookup(checker->symbol_table, cma_obj->name);
            if (enum_sym && enum_sym->kind == SYMBOL_ENUM) {
              Symbol *vsym =
                  symbol_table_lookup(checker->symbol_table, cma->member);
              if (vsym && vsym->kind == SYMBOL_CONSTANT) {
                case_value = vsym->data.constant.value;
                case_eval_ok = 1;
              }
            }
          }
        }
      }
      if (!case_eval_ok) {
        type_checker_set_error_at_location(
            checker, case_clause->value->location,
            "Case value must be a compile-time integer constant expression");
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }

      // Range case `lo..hi`: validate the upper bound the same way as the
      // lower bound, require it be a compile-time integer constant, and ensure
      // lo <= hi. First-match-wins dispatch makes overlapping ranges harmless,
      // so they are not tracked for duplicate detection.
      if (case_clause->value_high) {
        Type *high_type =
            type_checker_infer_type(checker, case_clause->value_high);
        if (!high_type) {
          checker->switch_depth--;
          free(init_snapshot);
          free(case_values);
          return 0;
        }
        if (!type_checker_is_integer_type(high_type) ||
            !type_checker_is_assignable(checker, switch_type, high_type)) {
          type_checker_report_type_mismatch(
              checker, case_clause->value_high->location, switch_type->name,
              high_type->name);
          checker->switch_depth--;
          free(init_snapshot);
          free(case_values);
          return 0;
        }

        long long case_high_value = 0;
        int high_eval_ok = type_checker_eval_integer_constant(
            case_clause->value_high, &case_high_value);
        if (!high_eval_ok &&
            case_clause->value_high->type == AST_IDENTIFIER) {
          Identifier *hid = (Identifier *)case_clause->value_high->data;
          Symbol *hsym =
              symbol_table_lookup(checker->symbol_table, hid->name);
          if (hsym && hsym->kind == SYMBOL_CONSTANT) {
            case_high_value = hsym->data.constant.value;
            high_eval_ok = 1;
          }
        }
        if (!high_eval_ok) {
          type_checker_set_error_at_location(
              checker, case_clause->value_high->location,
              "Range upper bound must be a compile-time integer constant "
              "expression");
          checker->switch_depth--;
          free(init_snapshot);
          free(case_values);
          return 0;
        }
        if (case_value > case_high_value) {
          type_checker_set_error_at_location(
              checker, case_clause->value->location,
              "Range lower bound '%lld' exceeds upper bound '%lld'",
              case_value, case_high_value);
          checker->switch_depth--;
          free(init_snapshot);
          free(case_values);
          return 0;
        }
      } else {
        for (size_t j = 0; j < case_value_count; j++) {
          if (case_values[j] == case_value) {
            type_checker_set_error_at_location(
                checker, case_clause->value->location,
                "Duplicate case value '%lld' in switch", case_value);
            checker->switch_depth--;
            free(init_snapshot);
            free(case_values);
            return 0;
          }
        }
        case_values[case_value_count++] = case_value;
      }
    }

    if (!case_clause->body) {
      type_checker_set_error_at_location(checker, case_node->location,
                                         "Case clause must have a body");
      checker->switch_depth--;
      free(init_snapshot);
      free(case_values);
      return 0;
    }

    if (!type_checker_check_statement(checker, case_clause->body)) {
      checker->switch_depth--;
      free(init_snapshot);
      free(case_values);
      return 0;
    }
    type_checker_init_tracker_restore(checker, init_snapshot,
                                      init_snapshot_count);
  }
  checker->switch_depth--;
  type_checker_init_tracker_restore(checker, init_snapshot,
                                    init_snapshot_count);
  free(init_snapshot);

  if (switch_type->kind == TYPE_ENUM && !seen_default) {
    Scope *global = checker->symbol_table->global_scope;
    for (size_t i = 0; i < global->symbol_count; i++) {
      Symbol *sym = global->symbols[i];
      if (!sym || sym->kind != SYMBOL_CONSTANT || sym->type != switch_type) {
        continue;
      }
      int covered = 0;
      for (size_t j = 0; j < case_value_count; j++) {
        if (case_values[j] == sym->data.constant.value) {
          covered = 1;
          break;
        }
      }
      if (!covered) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Non-exhaustive switch on '%s': variant '%s' not covered; "
            "add a 'case %s:' arm or a 'default:' arm",
            switch_type->name, sym->name, sym->name);
        free(case_values);
        return 0;
      }
    }
  }

  if (switch_type->kind == TYPE_BOOL && !seen_default) {
    int has_true = 0, has_false = 0;
    for (size_t i = 0; i < case_value_count; i++) {
      if (case_values[i] == 1) has_true = 1;
      if (case_values[i] == 0) has_false = 1;
    }
    if (!has_true || !has_false) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Non-exhaustive switch over 'bool': must cover both 'true' and "
          "'false', or add a 'default:' arm");
      free(case_values);
      return 0;
    }
  }

  free(case_values);
  return 1;
}

int type_checker_check_statement(TypeChecker *checker, ASTNode *statement) {
  if (!checker || !statement)
    return 0;

  switch (statement->type) {
  case AST_DEFER_STATEMENT: {
    if (!checker->current_function) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Defer statement outside of a function");
      return 0;
    }

    DeferStatement *defer_stmt = (DeferStatement *)statement->data;
    if (!defer_stmt || !defer_stmt->statement) {
      type_checker_set_error_at_location(checker, statement->location,
                                         "Invalid defer statement");
      return 0;
    }

    switch (defer_stmt->statement->type) {
    case AST_FUNCTION_CALL:
    case AST_ASSIGNMENT:
    case AST_PROGRAM:
      break;
    default:
      type_checker_set_error_at_location(
          checker, defer_stmt->statement->location,
          "Deferred statement must be a function call, assignment, or block");
      return 0;
    }

    return type_checker_check_statement(checker, defer_stmt->statement);
  }

  case AST_ERRDEFER_STATEMENT: {
    if (!checker->current_function) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Errdefer statement outside of a function");
      return 0;
    }

    DeferStatement *defer_stmt = (DeferStatement *)statement->data;
    if (!defer_stmt || !defer_stmt->statement) {
      type_checker_set_error_at_location(checker, statement->location,
                                         "Invalid errdefer statement");
      return 0;
    }

    switch (defer_stmt->statement->type) {
    case AST_FUNCTION_CALL:
    case AST_ASSIGNMENT:
    case AST_PROGRAM:
      break;
    default:
      type_checker_set_error_at_location(checker,
                                         defer_stmt->statement->location,
                                         "Errdeferred statement must be a "
                                         "function call, assignment, or block");
      return 0;
    }

    return type_checker_check_statement(checker, defer_stmt->statement);
  }
  case AST_VAR_DECLARATION:
  case AST_FUNCTION_DECLARATION:
  case AST_STRUCT_DECLARATION:
  case AST_ASSIGNMENT:
    // These are handled by process_declaration
    return type_checker_process_declaration(checker, statement);

  case AST_FUNCTION_CALL: {
    // Function call as statement (no return value used)
    Type *return_type = type_checker_infer_type(checker, statement);
    return return_type != NULL; // Error already reported if NULL
  }

  case AST_RETURN_STATEMENT: {
    ReturnStatement *ret_stmt = (ReturnStatement *)statement->data;
    if (ret_stmt && ret_stmt->value) {
      // Check if return value type matches function return type
      Type *value_type = type_checker_infer_type(checker, ret_stmt->value);
      if (!value_type) {
        // Error already reported by type_checker_infer_type if it failed
        // Only set generic error if no specific error was set
        if (!checker->has_error) {
          type_checker_set_error_at_location(
              checker, ret_stmt->value->location,
              "Cannot infer type of return value");
        }
        return 0;
      }

      if (checker->current_function) {
        Type *func_return_type =
            checker->current_function->data.function.return_type;
        if (!(func_return_type->kind == TYPE_POINTER &&
              type_checker_is_null_pointer_constant(ret_stmt->value)) &&
            !type_checker_is_assignable(checker, func_return_type,
                                        value_type)) {
          type_checker_report_type_mismatch(checker, ret_stmt->value->location,
                                            func_return_type->name,
                                            value_type->name);
          return 0;
        }

        if (checker->current_function_decl &&
            type_checker_ast_contains_node_type(checker->current_function_decl,
                                                AST_ERRDEFER_STATEMENT)) {
          long long constant_value = 0;
          if (type_checker_eval_integer_constant(ret_stmt->value,
                                                 &constant_value) &&
              constant_value != 0) {
            error_reporter_add_warning(
                checker->error_reporter, ERROR_SEMANTIC,
                ret_stmt->value->location,
                "Non-zero constant return in function with errdefer will "
                "trigger errdefer by convention");
          }
        }
      } else {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Return statement outside of a function");
        return 0;
      }
    }
    return 1;
  }

  case AST_IF_STATEMENT:
    return type_checker_check_if_statement(checker, statement);

  case AST_WHILE_STATEMENT: {
    WhileStatement *while_stmt = (WhileStatement *)statement->data;
    if (!while_stmt || !while_stmt->condition) {
      type_checker_set_error_at_location(checker, statement->location,
                                         "Invalid while statement");
      return 0;
    }

    // Check condition type
    Type *condition_type =
        type_checker_infer_type(checker, while_stmt->condition);
    if (!condition_type) {
      return 0; // Error already reported
    }

    // Condition should be a numeric type (treated as boolean)
    if (!type_checker_is_numeric_type(condition_type)) {
      type_checker_report_type_mismatch(checker,
                                        while_stmt->condition->location,
                                        "numeric type", condition_type->name);
      return 0;
    }

    size_t init_snapshot_count = 0;
    unsigned char *init_snapshot =
        type_checker_init_tracker_capture(checker, &init_snapshot_count);
    if (checker->tracked_var_count > 0 && !init_snapshot) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Out of memory while analyzing variable initialization flow");
      return 0;
    }

    checker->loop_depth++;
    if (while_stmt->body &&
        !type_checker_check_statement(checker, while_stmt->body)) {
      checker->loop_depth--;
      free(init_snapshot);
      return 0;
    }
    checker->loop_depth--;
    type_checker_init_tracker_restore(checker, init_snapshot,
                                      init_snapshot_count);
    free(init_snapshot);

    return 1;
  }

  case AST_FOR_STATEMENT:
    return type_checker_check_for_statement(checker, statement);

  case AST_SWITCH_STATEMENT:
    return type_checker_check_switch_statement(checker, statement);

  case AST_MATCH_STATEMENT: {
    MatchStatement *m = (MatchStatement *)statement->data;
    if (m && m->is_expression)
      return type_checker_check_match_expression(checker, statement) != NULL;
    return type_checker_check_match_statement(checker, statement);
  }

  case AST_BREAK_STATEMENT:
    if (checker->loop_depth <= 0 && checker->switch_depth <= 0) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "'break' can only be used inside a loop or switch");
      return 0;
    }
    return 1;

  case AST_CONTINUE_STATEMENT:
    if (checker->loop_depth <= 0) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "'continue' can only be used inside a loop");
      return 0;
    }
    return 1;

  case AST_INLINE_ASM:
    return 1;

  case AST_PROGRAM: {
    // A block of statements
    Program *block = (Program *)statement->data;
    if (block) {
      // Enter a new nested scope
      if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_BLOCK)) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Out of memory while entering block scope");
        return 0;
      }
      if (!type_checker_init_tracker_enter_scope(checker)) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Out of memory while entering initialization analysis scope");
        symbol_table_exit_scope(checker->symbol_table);
        return 0;
      }

      int reached_terminator = 0;
      int block_ok = 1;
      for (size_t i = 0; i < statement->child_count; i++) {
        ASTNode *child = statement->children[i];
        if (reached_terminator && checker->error_reporter && child) {
          error_reporter_add_warning(
              checker->error_reporter, ERROR_SEMANTIC, child->location,
              "Unreachable code: statement will never execute");
        }
        // A bad statement doesn't stop the walk: keep checking the block's
        // remaining statements so one compile reports every error.
        if (!type_checker_check_statement(checker, statement->children[i])) {
          block_ok = 0;
        }
        if (type_checker_statement_guarantees_termination(child)) {
          reached_terminator = 1;
        }
      }

      if (block_ok)
        type_checker_warn_unused_locals(checker);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      if (!block_ok)
        return 0;
    }
    return 1;
  }
  default:
    // Unknown statement type
    type_checker_set_error_at_location(checker, statement->location,
                                       "Unknown statement type");
    return 0;
  }
}
