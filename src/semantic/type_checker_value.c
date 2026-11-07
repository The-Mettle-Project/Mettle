#include "type_checker_internal.h"

static int value_condition_is_always_true(const ASTNode *condition) {
  if (!condition) {
    return 1;
  }
  if (condition->type == AST_NUMBER_LITERAL && condition->data) {
    const NumberLiteral *literal = (const NumberLiteral *)condition->data;
    return !literal->is_float && literal->int_value != 0;
  }
  if (condition->type == AST_IDENTIFIER && condition->data) {
    const Identifier *id = (const Identifier *)condition->data;
    return id->name && strcmp(id->name, "true") == 0;
  }
  return 0;
}

static int value_has_break(const ASTNode *node, const char *label,
                           int match_plain) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION) {
    return 0;
  }
  if (node->type == AST_BREAK_STATEMENT && node->data) {
    const LoopControlStatement *ctrl = (const LoopControlStatement *)node->data;
    if (ctrl->target_label) {
      return label && strcmp(ctrl->target_label, label) == 0;
    }
    return match_plain;
  }
  if (node->type == AST_WHILE_STATEMENT || node->type == AST_FOR_STATEMENT ||
      node->type == AST_SWITCH_STATEMENT) {
    match_plain = 0;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (value_has_break(node->children[i], label, match_plain)) {
      return 1;
    }
  }
  return 0;
}

static int value_call_never_returns(TypeChecker *checker, const ASTNode *call) {
  const CallExpression *data =
      call->type == AST_FUNCTION_CALL ? (const CallExpression *)call->data : NULL;
  if (!data || !data->function_name || data->object) {
    return 0;
  }
  Symbol *symbol = symbol_table_lookup(checker->symbol_table, data->function_name);
  return symbol && symbol->kind == SYMBOL_FUNCTION && symbol->is_noreturn;
}

static int value_loop_completes(const ASTNode *condition, const char *label,
                                const ASTNode *body) {
  return !value_condition_is_always_true(condition) ||
         value_has_break(body, label, 1);
}

static int value_switch_completes(TypeChecker *checker, const ASTNode *node) {
  const SwitchStatement *branch = (const SwitchStatement *)node->data;
  int has_default = 0;
  if (!branch) {
    return 1;
  }
  for (size_t i = 0; i < branch->case_count; i++) {
    const CaseClause *clause = (const CaseClause *)branch->cases[i]->data;
    const Program *body =
        clause && clause->body && clause->body->type == AST_PROGRAM
            ? (const Program *)clause->body->data
            : NULL;
    if (!clause) {
      continue;
    }
    if (clause->is_default) {
      has_default = 1;
    }
    if (body && body->declaration_count == 0) {
      if (i + 1 == branch->case_count) {
        return 1;
      }
      continue;
    }
    if (type_checker_flow_completes(checker, clause->body) ||
        value_has_break(clause->body, NULL, 1)) {
      return 1;
    }
  }
  return !has_default;
}

int type_checker_flow_completes(TypeChecker *checker, const ASTNode *node) {
  if (!node) {
    return 1;
  }
  switch (node->type) {
  case AST_RETURN_STATEMENT:
  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT:
  case AST_YIELD_STATEMENT:
  case AST_FALLTHROUGH_STATEMENT:
    return 0;
  case AST_FUNCTION_CALL:
    return !value_call_never_returns(checker, node);
  case AST_PROGRAM: {
    const Program *block = (const Program *)node->data;
    for (size_t i = 0; block && i < block->declaration_count; i++) {
      if (!type_checker_flow_completes(checker, block->declarations[i])) {
        return 0;
      }
    }
    return 1;
  }
  case AST_IF_STATEMENT: {
    const IfStatement *branch = (const IfStatement *)node->data;
    if (!branch || !branch->else_branch ||
        type_checker_flow_completes(checker, branch->then_branch) ||
        type_checker_flow_completes(checker, branch->else_branch)) {
      return 1;
    }
    for (size_t i = 0; i < branch->else_if_count; i++) {
      if (type_checker_flow_completes(checker, branch->else_ifs[i].body)) {
        return 1;
      }
    }
    return 0;
  }
  case AST_MATCH_STATEMENT: {
    const MatchStatement *match = (const MatchStatement *)node->data;
    for (size_t i = 0; match && i < match->arm_count; i++) {
      if (type_checker_flow_completes(checker, match->arms[i].body)) {
        return 1;
      }
    }
    return !match || match->arm_count == 0;
  }
  case AST_SWITCH_STATEMENT:
    return value_switch_completes(checker, node);
  case AST_WHILE_STATEMENT: {
    const WhileStatement *loop = (const WhileStatement *)node->data;
    return !loop ||
           value_loop_completes(loop->condition, loop->label, loop->body);
  }
  case AST_FOR_STATEMENT: {
    const ForStatement *loop = (const ForStatement *)node->data;
    return !loop ||
           value_loop_completes(loop->condition, loop->label, loop->body);
  }
  default:
    return 1;
  }
}

static ASTNode *value_pick_tail(TypeChecker *checker, ASTNode *node,
                                int scope_id);

static ASTNode *value_pick_if(TypeChecker *checker, ASTNode *node,
                              int scope_id) {
  IfStatement *branch = (IfStatement *)node->data;
  long long truth = 0;
  if (!branch || !type_checker_eval_integer_constant_with_checker(
                     checker, branch->condition, &truth)) {
    return NULL;
  }
  if (truth) {
    return value_pick_tail(checker, branch->then_branch, scope_id);
  }
  for (size_t i = 0; i < branch->else_if_count; i++) {
    if (!type_checker_eval_integer_constant_with_checker(
            checker, branch->else_ifs[i].condition, &truth)) {
      return NULL;
    }
    if (truth) {
      return value_pick_tail(checker, branch->else_ifs[i].body, scope_id);
    }
  }
  return value_pick_tail(checker, branch->else_branch, scope_id);
}

static ASTNode *value_pick_switch(TypeChecker *checker, ASTNode *node,
                                  int scope_id) {
  SwitchStatement *branch = (SwitchStatement *)node->data;
  long long subject = 0;
  if (!branch || !type_checker_eval_integer_constant_with_checker(
                     checker, branch->expression, &subject)) {
    return NULL;
  }
  size_t chosen = branch->case_count;
  for (size_t i = 0; i < branch->case_count && chosen == branch->case_count;
       i++) {
    CaseClause *clause = (CaseClause *)branch->cases[i]->data;
    long long low = 0;
    long long high = 0;
    if (clause->is_default) {
      continue;
    }
    if (!type_checker_eval_integer_constant_with_checker(checker, clause->value,
                                                         &low)) {
      return NULL;
    }
    high = low;
    if (clause->value_high &&
        !type_checker_eval_integer_constant_with_checker(
            checker, clause->value_high, &high)) {
      return NULL;
    }
    if (subject >= low && subject <= high) {
      chosen = i;
    }
  }
  for (size_t i = 0; i < branch->case_count && chosen == branch->case_count;
       i++) {
    if (((CaseClause *)branch->cases[i]->data)->is_default) {
      chosen = i;
    }
  }
  for (size_t i = chosen; i < branch->case_count; i++) {
    CaseClause *clause = (CaseClause *)branch->cases[i]->data;
    const Program *body = clause->body && clause->body->type == AST_PROGRAM
                              ? (const Program *)clause->body->data
                              : NULL;
    if (!body || body->declaration_count > 0) {
      return value_pick_tail(checker, clause->body, scope_id);
    }
  }
  return NULL;
}

static ASTNode *value_pick_tail(TypeChecker *checker, ASTNode *node,
                                int scope_id) {
  if (!node) {
    return NULL;
  }
  switch (node->type) {
  case AST_PROGRAM: {
    Program *block = (Program *)node->data;
    return block && block->declaration_count == 1
               ? value_pick_tail(checker, block->declarations[0], scope_id)
               : NULL;
  }
  case AST_YIELD_STATEMENT: {
    YieldStatement *yield = (YieldStatement *)node->data;
    return yield && yield->scope_id == scope_id ? yield->value : NULL;
  }
  case AST_IF_STATEMENT:
    return value_pick_if(checker, node, scope_id);
  case AST_SWITCH_STATEMENT:
    return value_pick_switch(checker, node, scope_id);
  default:
    return NULL;
  }
}

ASTNode *type_checker_pick_constant_value(TypeChecker *checker,
                                          ASTNode *initializer) {
  while (initializer && initializer->type == AST_VALUE_SCOPE &&
         initializer->data) {
    ValueScope *scope = (ValueScope *)initializer->data;
    initializer = value_pick_tail(checker, scope->body, scope->scope_id);
  }
  return initializer;
}

static const char *value_scope_word(ValueScopeKind kind) {
  switch (kind) {
  case VALUE_SCOPE_IF:
    return "'if'";
  case VALUE_SCOPE_MATCH:
    return "'match'";
  case VALUE_SCOPE_SWITCH:
    return "'switch'";
  case VALUE_SCOPE_LOOP:
    return "loop";
  default:
    return "block";
  }
}

static int value_is_bare_literal(const ASTNode *value) {
  return value->type == AST_NUMBER_LITERAL ||
         type_checker_is_float_literal_tree(value);
}

static int value_takes_target(const ASTNode *value) {
  return value->type == AST_AGGREGATE_LITERAL ||
         value->type == AST_FUNCTION_CALL || value->type == AST_IDENTIFIER ||
         value->type == AST_VALUE_SCOPE;
}

static TypeCheckerValueFrame *value_find_frame(TypeChecker *checker,
                                               int scope_id) {
  for (size_t i = checker->value_frame_count; i > 0; i--) {
    if (checker->value_frames[i - 1].scope_id == scope_id) {
      return &checker->value_frames[i - 1];
    }
  }
  return NULL;
}

static int value_report_mismatch(TypeChecker *checker, const ASTNode *value,
                                 const TypeCheckerValueFrame *frame,
                                 const Type *found) {
  if (frame->kind == VALUE_SCOPE_MATCH) {
    type_checker_set_error_at_location(
        checker, value->location,
        "match arms have incompatible types: '%s' vs '%s'",
        frame->result && frame->result->name ? frame->result->name : "?",
        found->name ? found->name : "?");
    return 0;
  }
  type_checker_set_error_at_location(
      checker, value->location,
      "The branches of this %s give different types, '%s' and '%s'; cast one "
      "so they agree",
      value_scope_word(frame->kind),
      frame->result && frame->result->name ? frame->result->name : "?",
      found->name ? found->name : "?");
  return 0;
}

static int value_unify_without_target(TypeChecker *checker, ASTNode *value,
                                      TypeCheckerValueFrame *frame,
                                      Type *found) {
  if (!frame->result) {
    frame->result = found;
    frame->first_value = value;
    return 1;
  }
  if (type_checker_types_equal(frame->result, found)) {
    return 1;
  }
  if (value_is_bare_literal(frame->first_value) &&
      (type_checker_takes_null(found, frame->first_value) ||
       type_checker_is_assignable_from(checker, found, frame->result,
                                       frame->first_value))) {
    Type *first = frame->result;
    if (type_checker_adopt_float_literal(checker, frame->first_value, &first,
                                         found) < 0) {
      return 0;
    }
    frame->result = found;
    frame->first_value = value;
    return 1;
  }
  if (value_is_bare_literal(value) &&
      (type_checker_takes_null(frame->result, value) ||
       type_checker_is_assignable_from(checker, frame->result, found, value))) {
    return 1;
  }
  if (frame->kind == VALUE_SCOPE_MATCH &&
      type_checker_are_compatible(frame->result, found)) {
    return 1;
  }
  int both_numeric = type_checker_is_numeric_type(frame->result) &&
                     type_checker_is_numeric_type(found);
  if (!both_numeric && type_checker_is_assignable_from(checker, frame->result,
                                                       found, value)) {
    return 1;
  }
  return value_report_mismatch(checker, value, frame, found);
}

int type_checker_check_yield(TypeChecker *checker, ASTNode *statement) {
  YieldStatement *yield = (YieldStatement *)statement->data;
  TypeCheckerValueFrame *frame =
      yield ? value_find_frame(checker, yield->scope_id) : NULL;
  if (!frame) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "This value has no scope to go to");
    return 0;
  }
  if (!yield->value) {
    frame->saw_empty = 1;
    return 1;
  }

  ASTNode *value = yield->value;
  Type *target = frame->target ? frame->target : frame->result;
  checker->aggregate_target_type = value_takes_target(value) ? target : NULL;
  Type *found = type_checker_infer_type(checker, value);
  checker->aggregate_target_type = NULL;
  frame = value_find_frame(checker, yield->scope_id);
  if (!found) {
    return 0;
  }
  if (type_checker_reject_comptime_escape(checker, value->location, found)) {
    return 0;
  }
  if (value_call_never_returns(checker, value)) {
    return 1;
  }
  if (found->kind == TYPE_VOID) {
    frame->saw_empty = 1;
    return 1;
  }
  frame->saw_value = 1;
  if (type_checker_adopt_float_literal(
          checker, value, &found,
          frame->target ? frame->target : frame->result) < 0) {
    return 0;
  }
  if (!frame->target) {
    return value_unify_without_target(checker, value, frame, found);
  }
  if (type_checker_takes_null(frame->target, value) ||
      type_checker_is_assignable_from(checker, frame->target, found, value)) {
    return 1;
  }
  if (frame->kind == VALUE_SCOPE_MATCH &&
      type_checker_are_compatible(frame->target, found)) {
    return 1;
  }
  type_checker_report_assign_mismatch(checker, value, value->location,
                                      frame->target, found);
  return 0;
}

static int value_push_frame(TypeChecker *checker, const ValueScope *scope,
                            Type *target, SourceLocation location) {
  if (checker->value_frame_count == checker->value_frame_capacity) {
    size_t grown =
        checker->value_frame_capacity ? checker->value_frame_capacity * 2 : 8;
    TypeCheckerValueFrame *frames =
        realloc(checker->value_frames, grown * sizeof(TypeCheckerValueFrame));
    if (!frames) {
      type_checker_set_error_at_location(checker, location,
                                         "Out of memory checking a value");
      return 0;
    }
    checker->value_frames = frames;
    checker->value_frame_capacity = grown;
  }
  TypeCheckerValueFrame *frame =
      &checker->value_frames[checker->value_frame_count++];
  frame->scope_id = scope->scope_id;
  frame->kind = scope->kind;
  frame->target = target && target->kind != TYPE_VOID ? target : NULL;
  frame->result = NULL;
  frame->first_value = NULL;
  frame->saw_value = 0;
  frame->saw_empty = 0;
  return 1;
}

static void value_report_fell_off(TypeChecker *checker, const ASTNode *expression,
                                  const ValueScope *scope) {
  type_checker_set_error_at_location(
      checker, expression->location,
      "This %s can finish without giving a value; end each path with an "
      "expression and no ';', or leave it with return, break or continue",
      value_scope_word(scope->kind));
}

Type *type_checker_check_value_scope(TypeChecker *checker, ASTNode *expression) {
  ValueScope *scope = (ValueScope *)expression->data;
  Type *target = checker->aggregate_target_type;
  checker->aggregate_target_type = NULL;
  checker->aggregate_requires_constant = 0;
  if (!scope || !scope->body) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Invalid value expression");
    return NULL;
  }
  if (!value_push_frame(checker, scope, target, expression->location)) {
    return NULL;
  }
  int ok = type_checker_check_statement(checker, scope->body);
  TypeCheckerValueFrame frame =
      checker->value_frames[--checker->value_frame_count];
  if (!ok) {
    return NULL;
  }
  if (!frame.saw_value) {
    if (frame.target && !frame.saw_empty) {
      return frame.target;
    }
    if (frame.saw_empty || scope->kind == VALUE_SCOPE_LOOP ||
        scope->label) {
      return checker->builtin_void;
    }
    type_checker_set_error_at_location(
        checker, expression->location,
        "This %s never gives a value, so it has no type",
        value_scope_word(scope->kind));
    return NULL;
  }
  if (frame.saw_empty) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "One path through this %s gives a value and another gives none",
        value_scope_word(scope->kind));
    return NULL;
  }
  if (type_checker_flow_completes(checker, scope->body)) {
    value_report_fell_off(checker, expression, scope);
    return NULL;
  }
  return frame.target ? frame.target : frame.result;
}

int type_checker_check_value_scope_statement(TypeChecker *checker,
                                             ASTNode *statement) {
  Type *type = type_checker_check_value_scope(checker, statement);
  if (!type) {
    return 0;
  }
  statement->resolved_type = type;
  if (type->kind != TYPE_VOID) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "The value of this %s goes nowhere; use it, or give 'break' no value",
        value_scope_word(((ValueScope *)statement->data)->kind));
    return 0;
  }
  return 1;
}
