#include "semantic/ownership_internal.h"
#include "semantic/type_checker_internal.h"
#include <string.h>

static struct Type *own_string_type_of(TypeChecker *checker, ASTNode *node) {
  if (!node) {
    return NULL;
  }
  return node->resolved_type ? node->resolved_type
                             : type_checker_infer_type(checker, node);
}

int own_is_string_type(struct Type *type) {
  while (type && type->refined_base) {
    type = type->refined_base;
  }
  return type && type->kind == TYPE_STRING;
}

static int own_interp_owned(TypeChecker *checker, ASTNode *argument) {
  struct Type *type = own_string_type_of(checker, argument);
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case TYPE_CHAR:
  case TYPE_INT8:
  case TYPE_INT16:
  case TYPE_INT32:
  case TYPE_INT64:
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_UINT64:
  case TYPE_FLOAT32:
  case TYPE_FLOAT64:
  case TYPE_FLOAT16:
  case TYPE_BFLOAT16:
    return 1;
  default:
    return 0;
  }
}

int own_is_interp_call(const ASTNode *node) {
  const CallExpression *call = node && node->type == AST_FUNCTION_CALL
                                   ? (const CallExpression *)node->data
                                   : NULL;
  return call && call->function_name && !call->object &&
         strcmp(call->function_name, "__mtl_interp") == 0;
}

static int own_call_string_owned(TypeChecker *checker, CallExpression *call) {
  if (!call || !call->function_name || call->object ||
      call->is_indirect_call) {
    return 0;
  }
  if (strcmp(call->function_name, "__mtl_interp") == 0) {
    return call->argument_count == 1 && call->arguments &&
           own_interp_owned(checker, call->arguments[0]);
  }
  if (type_checker_callee_returns_fresh(checker, call->function_name)) {
    return 1;
  }
  for (size_t i = 0; call->arguments && i < call->argument_count; i++) {
    if (type_checker_callee_hands_back(checker, call->function_name, i)) {
      return own_string_owned(checker, call->arguments[i]);
    }
  }
  return 0;
}

int own_string_owned(TypeChecker *checker, ASTNode *node) {
  if (!checker || !node ||
      (node->type != AST_BINARY_EXPRESSION &&
       node->type != AST_FUNCTION_CALL) ||
      (node->resolved_type && !own_is_string_type(node->resolved_type))) {
    return 0;
  }
  if (node->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary = (BinaryExpression *)node->data;
    return binary && binary->operator && strcmp(binary->operator, "+") == 0 &&
           own_is_string_type(own_string_type_of(checker, node));
  }
  return own_is_string_type(own_string_type_of(checker, node)) &&
         own_call_string_owned(checker, (CallExpression *)node->data);
}

int own_callee_borrows(TypeChecker *checker, CallExpression *call,
                       size_t index) {
  return call && call->function_name && !call->object &&
         !call->is_indirect_call &&
         type_checker_callee_borrows(checker, call->function_name, index);
}

typedef struct {
  TypeChecker *checker;
  const char *name;
  int strict;
  int assigned;
  int loops;
  int deferred;
} OwnKeep;

static int own_names(const ASTNode *node, const char *name) {
  if (own_is_interp_call(node)) {
    const CallExpression *call = (const CallExpression *)node->data;
    node = call->argument_count == 1 ? call->arguments[0] : NULL;
  }
  return node && node->type == AST_IDENTIFIER && node->data &&
         strcmp(((const Identifier *)node->data)->name, name) == 0;
}

static int own_names_exactly(const ASTNode *node, const char *name) {
  return node && node->type == AST_IDENTIFIER && node->data &&
         strcmp(((const Identifier *)node->data)->name, name) == 0;
}

static int own_is_drop_of(const ASTNode *node, const char *name) {
  const CallExpression *call = node && node->type == AST_FUNCTION_CALL
                                   ? (const CallExpression *)node->data
                                   : NULL;
  return call && call->function_name && !call->object &&
         (strcmp(call->function_name, "drop") == 0 ||
          strcmp(call->function_name, "forget") == 0) &&
         call->argument_count == 1 &&
         own_names_exactly(call->arguments[0], name);
}

static int own_expression_keeps(OwnKeep *k, const ASTNode *node);
static int own_statement_keeps(OwnKeep *k, const ASTNode *node);

static int own_operand_keeps(OwnKeep *k, const ASTNode *node) {
  if (own_names(node, k->name)) {
    return !k->strict;
  }
  return own_expression_keeps(k, node);
}

static int own_call_keeps(OwnKeep *k, const ASTNode *node) {
  const CallExpression *call = (const CallExpression *)node->data;
  if (!k->strict && call->function_name && !call->object &&
      (strcmp(call->function_name, "drop") == 0 ||
       strcmp(call->function_name, "forget") == 0)) {
    return 1;
  }
  if (call->object && !own_expression_keeps(k, call->object)) {
    return 0;
  }
  for (size_t i = 0; i < call->argument_count; i++) {
    if (own_names(call->arguments[i], k->name)) {
      if (k->strict || call->is_indirect_call || call->object ||
          !call->function_name ||
          !type_checker_callee_borrows(k->checker, call->function_name, i)) {
        return 0;
      }
    } else if (!own_expression_keeps(k, call->arguments[i])) {
      return 0;
    }
  }
  return 1;
}

static int own_list_keeps(OwnKeep *k, ASTNode *const *items, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (!own_expression_keeps(k, items[i])) {
      return 0;
    }
  }
  return 1;
}

static int own_lambda_keeps(OwnKeep *k, const ASTNode *node) {
  const FunctionDeclaration *lambda = (const FunctionDeclaration *)node->data;
  for (size_t i = 0; lambda && i < lambda->captured_count; i++) {
    if (lambda->captured_names && lambda->captured_names[i] &&
        strcmp(lambda->captured_names[i], k->name) == 0) {
      return 0;
    }
  }
  return lambda != NULL;
}

static int own_expression_keeps(OwnKeep *k, const ASTNode *node) {
  if (!node) {
    return 1;
  }
  switch (node->type) {
  case AST_NUMBER_LITERAL:
  case AST_STRING_LITERAL:
    return 1;
  case AST_IDENTIFIER:
    return !own_names(node, k->name);
  case AST_BINARY_EXPRESSION: {
    const BinaryExpression *binary = (const BinaryExpression *)node->data;
    return own_operand_keeps(k, binary->left) &&
           own_operand_keeps(k, binary->right);
  }
  case AST_UNARY_EXPRESSION:
    return own_expression_keeps(
        k, ((const UnaryExpression *)node->data)->operand);
  case AST_CAST_EXPRESSION:
    return own_expression_keeps(
        k, ((const CastExpression *)node->data)->operand);
  case AST_MEMBER_ACCESS: {
    const MemberAccess *access = (const MemberAccess *)node->data;
    if (own_names(access->object, k->name)) {
      return !k->strict && access->member &&
             strcmp(access->member, "length") == 0;
    }
    return own_expression_keeps(k, access->object);
  }
  case AST_INDEX_EXPRESSION: {
    const ArrayIndexExpression *index =
        (const ArrayIndexExpression *)node->data;
    return own_operand_keeps(k, index->array) &&
           own_expression_keeps(k, index->index);
  }
  case AST_FUNCTION_CALL:
    return own_call_keeps(k, node);
  case AST_FUNC_PTR_CALL: {
    const FuncPtrCall *call = (const FuncPtrCall *)node->data;
    return own_expression_keeps(k, call->function) &&
           own_list_keeps(k, call->arguments, call->argument_count);
  }
  case AST_AGGREGATE_LITERAL: {
    const AggregateLiteral *literal = (const AggregateLiteral *)node->data;
    return own_list_keeps(k, literal->elements, literal->element_count) &&
           own_expression_keeps(k, literal->repeat_count);
  }
  case AST_NEW_EXPRESSION: {
    const NewExpression *created = (const NewExpression *)node->data;
    return own_list_keeps(k, created->extents, created->extent_count) &&
           own_expression_keeps(k, created->count);
  }
  case AST_LAMBDA_EXPRESSION:
    return own_lambda_keeps(k, node);
  case AST_VALUE_SCOPE:
    return own_statement_keeps(k, ((const ValueScope *)node->data)->body);
  default:
    return 0;
  }
}

static int own_branch_keeps(OwnKeep *k, const ASTNode *body, int start,
                            int *after) {
  k->strict = start;
  if (!own_statement_keeps(k, body)) {
    return 0;
  }
  *after = *after || k->strict;
  return 1;
}

static int own_if_keeps(OwnKeep *k, const ASTNode *node) {
  const IfStatement *branch = (const IfStatement *)node->data;
  int start = k->strict;
  int after = start;
  if (!own_expression_keeps(k, branch->condition) ||
      !own_branch_keeps(k, branch->then_branch, start, &after)) {
    return 0;
  }
  for (size_t i = 0; i < branch->else_if_count; i++) {
    k->strict = start;
    if (!own_expression_keeps(k, branch->else_ifs[i].condition) ||
        !own_branch_keeps(k, branch->else_ifs[i].body, start, &after)) {
      return 0;
    }
  }
  if (!own_branch_keeps(k, branch->else_branch, start, &after)) {
    return 0;
  }
  k->strict = after;
  return 1;
}

static int own_match_keeps(OwnKeep *k, const ASTNode *node) {
  const MatchStatement *branch = (const MatchStatement *)node->data;
  int start = k->strict;
  int after = start;
  if (!own_expression_keeps(k, branch->expression)) {
    return 0;
  }
  for (size_t i = 0; i < branch->arm_count; i++) {
    const MatchArm *arm = &branch->arms[i];
    if (arm->binding_name && strcmp(arm->binding_name, k->name) == 0) {
      continue;
    }
    if (!own_branch_keeps(k, arm->body, start, &after)) {
      return 0;
    }
  }
  k->strict = after;
  return 1;
}

static int own_case_keeps(OwnKeep *k, const ASTNode *node) {
  const CaseClause *clause = node && node->type == AST_CASE_CLAUSE
                                 ? (const CaseClause *)node->data
                                 : NULL;
  if (!clause) {
    return node == NULL;
  }
  return own_expression_keeps(k, clause->value) &&
         own_expression_keeps(k, clause->value_high) &&
         own_statement_keeps(k, clause->body);
}

static int own_switch_keeps(OwnKeep *k, const ASTNode *node) {
  const SwitchStatement *branch = (const SwitchStatement *)node->data;
  if (!own_expression_keeps(k, branch->expression)) {
    return 0;
  }
  for (size_t i = 0; i < branch->case_count; i++) {
    if (!own_case_keeps(k, branch->cases[i])) {
      return 0;
    }
  }
  return 1;
}

static int own_assignment_keeps(OwnKeep *k, const ASTNode *node) {
  const Assignment *assignment = (const Assignment *)node->data;
  if (assignment->target_count == 0 && !assignment->target &&
      assignment->variable_name &&
      strcmp(assignment->variable_name, k->name) == 0) {
    k->assigned = 1;
    return !k->strict && own_expression_keeps(k, assignment->value);
  }
  if (assignment->target_count > 0) {
    return 0;
  }
  return own_expression_keeps(k, assignment->target) &&
         own_expression_keeps(k, assignment->value);
}

static int own_block_keeps(OwnKeep *k, const ASTNode *node) {
  const Program *block = (const Program *)node->data;
  for (size_t i = 0; block && i < block->declaration_count; i++) {
    const ASTNode *statement = block->declarations[i];
    const VarDeclaration *decl =
        statement && statement->type == AST_VAR_DECLARATION
            ? (const VarDeclaration *)statement->data
            : NULL;
    if (decl && decl->name && strcmp(decl->name, k->name) == 0) {
      return own_expression_keeps(k, decl->initializer);
    }
    if (!own_statement_keeps(k, statement)) {
      return 0;
    }
  }
  return 1;
}

static int own_loop_keeps(OwnKeep *k, const ASTNode *node) {
  int kept = 0;
  k->loops++;
  if (node->type == AST_WHILE_STATEMENT) {
    const WhileStatement *loop = (const WhileStatement *)node->data;
    kept = own_expression_keeps(k, loop->condition) &&
           own_statement_keeps(k, loop->body);
  } else {
    const ForStatement *loop = (const ForStatement *)node->data;
    kept = own_statement_keeps(k, loop->initializer) &&
           own_expression_keeps(k, loop->condition) &&
           own_statement_keeps(k, loop->increment) &&
           own_statement_keeps(k, loop->body);
  }
  k->loops--;
  return kept;
}

static int own_defer_keeps(OwnKeep *k, const ASTNode *node) {
  const ASTNode *deferred = ((const DeferStatement *)node->data)->statement;
  int kept = 0;
  if (!k->strict && !own_is_drop_of(deferred, k->name)) {
    OwnKeep probe = *k;
    probe.strict = 1;
    if (!own_statement_keeps(&probe, deferred)) {
      k->deferred = 1;
    }
  }
  k->loops++;
  kept = own_statement_keeps(k, deferred);
  k->loops--;
  return kept;
}

static int own_hands_over(TypeChecker *checker, const ASTNode *node,
                          const char *name) {
  if (!node || !node->data) {
    return 0;
  }
  if (node->type == AST_ASSIGNMENT) {
    const Assignment *assignment = (const Assignment *)node->data;
    return assignment->target_count == 0 && !assignment->target &&
           assignment->variable_name &&
           strcmp(assignment->variable_name, name) != 0 &&
           own_names_exactly(assignment->value, name);
  }
  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *decl = (VarDeclaration *)node->data;
    return decl->name && strcmp(decl->name, name) != 0 && !decl->is_const &&
           !decl->is_extern &&
           own_is_string_type(own_declared_type(checker, decl)) &&
           own_names_exactly(decl->initializer, name);
  }
  return 0;
}

static int own_statement_keeps(OwnKeep *k, const ASTNode *node) {
  if (!node) {
    return 1;
  }
  if (!k->strict && !k->loops && !k->deferred &&
      own_hands_over(k->checker, node, k->name)) {
    k->strict = 1;
    return 1;
  }
  switch (node->type) {
  case AST_PROGRAM:
    return own_block_keeps(k, node);
  case AST_VAR_DECLARATION: {
    const VarDeclaration *decl = (const VarDeclaration *)node->data;
    return decl->name && strcmp(decl->name, k->name) != 0 &&
           own_expression_keeps(k, decl->initializer);
  }
  case AST_ASSIGNMENT:
    return own_assignment_keeps(k, node);
  case AST_IF_STATEMENT:
    return own_if_keeps(k, node);
  case AST_WHILE_STATEMENT:
  case AST_FOR_STATEMENT:
    return own_loop_keeps(k, node);
  case AST_RETURN_STATEMENT: {
    const ReturnStatement *ret = (const ReturnStatement *)node->data;
    return !ret || (ret->value_count <= 1 &&
                    own_expression_keeps(k, ret->value));
  }
  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT:
    return !node->data ||
           ((const LoopControlStatement *)node->data)->value == NULL;
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT:
    return own_defer_keeps(k, node);
  case AST_YIELD_STATEMENT:
    return own_expression_keeps(
        k, ((const YieldStatement *)node->data)->value);
  case AST_SWITCH_STATEMENT:
    return own_switch_keeps(k, node);
  case AST_CASE_CLAUSE:
    return own_case_keeps(k, node);
  case AST_MATCH_STATEMENT:
    return own_match_keeps(k, node);
  case AST_FALLTHROUGH_STATEMENT:
  case AST_BARRIER_STATEMENT:
  case AST_QUIESCE_STATEMENT:
    return 1;
  default:
    return own_expression_keeps(k, node);
  }
}

int own_string_local_is_tracked(TypeChecker *checker, ASTNode *block,
                                size_t index, VarDeclaration *decl,
                                int handed) {
  Program *program = block ? (Program *)block->data : NULL;
  OwnKeep keep = {checker, NULL, 0, 0, 0, 0};
  int owned = 0;
  if (!program || !decl || !decl->name || !decl->initializer ||
      decl->is_const || decl->is_extern) {
    return 0;
  }
  keep.name = decl->name;
  owned = handed || own_string_owned(checker, decl->initializer);
  if (!owned && decl->initializer->type != AST_STRING_LITERAL) {
    return 0;
  }
  for (size_t i = index + 1; i < program->declaration_count; i++) {
    if (!own_statement_keeps(&keep, program->declarations[i])) {
      return 0;
    }
  }
  return owned || keep.assigned;
}
