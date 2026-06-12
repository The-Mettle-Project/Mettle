// Type checker: constant evaluation, buffer-extent / alignment safety analysis.
#include "type_checker_internal.h"

int type_checker_is_lvalue_expression(ASTNode *expression) {
  if (!expression) {
    return 0;
  }

  switch (expression->type) {
  case AST_IDENTIFIER:
  case AST_MEMBER_ACCESS:
  case AST_INDEX_EXPRESSION:
    return 1;
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    return unary && unary->operator && strcmp(unary->operator, "*") == 0;
  }
  default:
    return 0;
  }
}

int type_checker_eval_integer_constant_with_checker(TypeChecker *checker,
                                                           ASTNode *expression,
                                                           long long *out_value) {
  if (!expression || !out_value) {
    return 0;
  }

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal || literal->is_float) {
      return 0;
    }
    *out_value = literal->int_value;
    return 1;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      return 0;
    }

    Symbol *symbol = checker ? symbol_table_lookup(checker->symbol_table,
                                                   identifier->name)
                             : NULL;
    if (!symbol || symbol->kind != SYMBOL_CONSTANT) {
      return 0;
    }

    *out_value = symbol->data.constant.value;
    return 1;
  }

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    if (!call || !call->function_name ||
        strcmp(call->function_name, "sizeof") != 0 ||
        call->argument_count != 1 || !call->arguments[0] ||
        call->arguments[0]->type != AST_IDENTIFIER) {
      return 0;
    }

    Identifier *type_id = (Identifier *)call->arguments[0]->data;
    Type *type = (checker && type_id)
                     ? type_checker_get_type_by_name(checker, type_id->name)
                     : NULL;
    if (!type || type->size > (size_t)LLONG_MAX) {
      return 0;
    }

    *out_value = (long long)type->size;
    return 1;
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary_expr = (UnaryExpression *)expression->data;
    long long operand = 0;
    if (!unary_expr || !unary_expr->operator || !unary_expr->operand ||
        !type_checker_eval_integer_constant_with_checker(
            checker, unary_expr->operand, &operand)) {
      return 0;
    }

    if (strcmp(unary_expr->operator, "+") == 0) {
      *out_value = operand;
      return 1;
    }
    if (strcmp(unary_expr->operator, "-") == 0) {
      *out_value = -operand;
      return 1;
    }
    return 0;
  }

  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary_expr = (BinaryExpression *)expression->data;
    long long left = 0;
    long long right = 0;
    if (!binary_expr || !binary_expr->operator || !binary_expr->left ||
        !binary_expr->right ||
        !type_checker_eval_integer_constant_with_checker(
            checker, binary_expr->left, &left) ||
        !type_checker_eval_integer_constant_with_checker(
            checker, binary_expr->right, &right)) {
      return 0;
    }

    if (strcmp(binary_expr->operator, "+") == 0) {
      *out_value = left + right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "-") == 0) {
      *out_value = left - right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "*") == 0) {
      *out_value = left * right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "/") == 0) {
      if (right == 0) {
        return 0;
      }
      *out_value = left / right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "%") == 0) {
      if (right == 0) {
        return 0;
      }
      *out_value = left % right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "==") == 0) {
      *out_value = left == right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "!=") == 0) {
      *out_value = left != right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "<") == 0) {
      *out_value = left < right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "<=") == 0) {
      *out_value = left <= right;
      return 1;
    }
    if (strcmp(binary_expr->operator, ">") == 0) {
      *out_value = left > right;
      return 1;
    }
    if (strcmp(binary_expr->operator, ">=") == 0) {
      *out_value = left >= right;
      return 1;
    }
    if (strcmp(binary_expr->operator, "&&") == 0) {
      *out_value = (left != 0) && (right != 0);
      return 1;
    }
    if (strcmp(binary_expr->operator, "||") == 0) {
      *out_value = (left != 0) || (right != 0);
      return 1;
    }
    return 0;
  }

  default:
    return 0;
  }
}

int type_checker_eval_integer_constant(ASTNode *expression,
                                              long long *out_value) {
  return type_checker_eval_integer_constant_with_checker(NULL, expression,
                                                        out_value);
}

Type *type_checker_resolve_sizeof_argument(TypeChecker *checker,
                                                  CallExpression *call,
                                                  SourceLocation location) {
  if (!checker || !call) {
    return NULL;
  }

  if (call->argument_count != 1) {
    type_checker_set_error_at_location(
        checker, location, "sizeof expects exactly one type argument");
    return NULL;
  }

  ASTNode *arg = call->arguments ? call->arguments[0] : NULL;
  if (!arg || arg->type != AST_IDENTIFIER) {
    type_checker_set_error_at_location(
        checker, location, "sizeof expects a type name");
    return NULL;
  }

  Identifier *type_id = (Identifier *)arg->data;
  Type *type = type_id ? type_checker_get_type_by_name(checker, type_id->name)
                       : NULL;
  if (!type) {
    type_checker_set_error_at_location(
        checker, arg->location, "Unknown type '%s' in sizeof",
        type_id && type_id->name ? type_id->name : "<invalid>");
    return NULL;
  }

  return type;
}

int type_checker_validate_static_assert(TypeChecker *checker,
                                               CallExpression *call,
                                               SourceLocation location) {
  if (!checker || !call) {
    return 0;
  }

  if (call->argument_count != 1) {
    type_checker_set_error_at_location(
        checker, location, "static_assert expects exactly one condition");
    return 0;
  }

  long long value = 0;
  if (!type_checker_eval_integer_constant_with_checker(
          checker, call->arguments[0], &value)) {
    type_checker_set_error_at_location(
        checker, call->arguments[0] ? call->arguments[0]->location : location,
        "static_assert condition must be a compile-time integer expression");
    return 0;
  }

  if (value == 0) {
    type_checker_set_error_at_location(checker, location,
                                       "static_assert failed");
    return 0;
  }

  return 1;
}

void type_checker_buffer_extent_clear(TypeChecker *checker) {
  if (!checker) {
    return;
  }

  TrackedBufferExtent *node = checker->tracked_buffer_extents;
  while (node) {
    TrackedBufferExtent *next = node->next;
    free(node->name);
    free(node);
    node = next;
  }
  checker->tracked_buffer_extents = NULL;
}

void type_checker_buffer_extent_exit_scope(TypeChecker *checker,
                                                  int scope_depth) {
  if (!checker) {
    return;
  }

  TrackedBufferExtent **node_ptr = &checker->tracked_buffer_extents;
  while (*node_ptr) {
    TrackedBufferExtent *node = *node_ptr;
    if (node->scope_depth == scope_depth) {
      *node_ptr = node->next;
      free(node->name);
      free(node);
      continue;
    }
    node_ptr = &node->next;
  }
}

TrackedBufferExtent *
type_checker_buffer_extent_find(TypeChecker *checker, const char *name) {
  if (!checker || !name) {
    return NULL;
  }

  TrackedBufferExtent *node = checker->tracked_buffer_extents;
  while (node) {
    if (node->name && strcmp(node->name, name) == 0) {
      return node;
    }
    node = node->next;
  }
  return NULL;
}

int type_checker_buffer_extent_declare(TypeChecker *checker,
                                              const char *name,
                                              long long byte_count,
                                              long long known_alignment) {
  if (!checker || !name) {
    return 0;
  }

  TrackedBufferExtent *node = malloc(sizeof(TrackedBufferExtent));
  if (!node) {
    return 0;
  }

  node->name = strdup(name);
  if (!node->name) {
    free(node);
    return 0;
  }

  node->byte_count = byte_count;
  node->known_alignment = known_alignment;
  node->scope_depth = checker->tracked_scope_depth;
  node->next = checker->tracked_buffer_extents;
  checker->tracked_buffer_extents = node;
  return 1;
}

int type_checker_buffer_extent_set(TypeChecker *checker, const char *name,
                                          long long byte_count,
                                          long long known_alignment) {
  if (!checker || !name) {
    return 0;
  }

  TrackedBufferExtent *node = type_checker_buffer_extent_find(checker, name);
  if (!node) {
    return type_checker_buffer_extent_declare(checker, name, byte_count,
                                              known_alignment);
  }

  node->byte_count = byte_count;
  node->known_alignment = known_alignment;
  return 1;
}

long long type_checker_default_heap_alignment(void) {
  // Current backend target is 64-bit; model malloc/calloc as at least 8-byte
  // aligned so we can reason about common scalar casts.
  return 8;
}

long long
type_checker_extract_allocation_call_alignment(CallExpression *call) {
  if (!call || !call->function_name) {
    return -1;
  }
  if (strcmp(call->function_name, "malloc") == 0 ||
      strcmp(call->function_name, "calloc") == 0) {
    return type_checker_default_heap_alignment();
  }
  return -1;
}

long long type_checker_known_alignment_after_offset(long long base_align,
                                                           long long offset) {
  if (base_align <= 0) {
    return -1;
  }
  if (offset == 0) {
    return base_align;
  }
  if (offset == LLONG_MIN) {
    return 1;
  }

  long long magnitude = offset < 0 ? -offset : offset;
  long long result = base_align;
  while (result > 1 && (magnitude % result) != 0) {
    result /= 2;
  }
  return result > 0 ? result : 1;
}

const char *type_checker_extract_identifier_name(ASTNode *expression) {
  if (!expression) {
    return NULL;
  }

  if (expression->type == AST_CAST_EXPRESSION) {
    CastExpression *cast_expr = (CastExpression *)expression->data;
    if (!cast_expr) {
      return NULL;
    }
    return type_checker_extract_identifier_name(cast_expr->operand);
  }

  if (expression->type != AST_IDENTIFIER) {
    return NULL;
  }

  Identifier *id = (Identifier *)expression->data;
  if (!id || !id->name) {
    return NULL;
  }
  return id->name;
}

long long
type_checker_extract_allocation_call_extent(CallExpression *call) {
  if (!call || !call->function_name) {
    return -1;
  }

  if (strcmp(call->function_name, "malloc") == 0) {
    if (call->argument_count != 1) {
      return -1;
    }
    long long size = 0;
    if (!type_checker_eval_integer_constant(call->arguments[0], &size) ||
        size < 0) {
      return -1;
    }
    return size;
  }

  if (strcmp(call->function_name, "calloc") == 0) {
    if (call->argument_count != 2) {
      return -1;
    }
    long long count = 0;
    long long size = 0;
    if (!type_checker_eval_integer_constant(call->arguments[0], &count) ||
        !type_checker_eval_integer_constant(call->arguments[1], &size) ||
        count < 0 || size < 0) {
      return -1;
    }
    if (count > 0 && size > (LLONG_MAX / count)) {
      return -1;
    }
    return count * size;
  }

  return -1;
}

long long type_checker_extract_known_buffer_extent(TypeChecker *checker,
                                                          ASTNode *expression) {
  if (!expression) {
    return -1;
  }

  if (expression->type == AST_CAST_EXPRESSION) {
    CastExpression *cast_expr = (CastExpression *)expression->data;
    if (!cast_expr) {
      return -1;
    }
    return type_checker_extract_known_buffer_extent(checker, cast_expr->operand);
  }

  if (expression->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)expression->data;
    return type_checker_extract_allocation_call_extent(call);
  }

  if (expression->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary_expr = (BinaryExpression *)expression->data;
    if (!binary_expr || !binary_expr->operator || !binary_expr->left ||
        !binary_expr->right) {
      return -1;
    }
    if (strcmp(binary_expr->operator, "+") == 0) {
      long long offset = 0;
      long long base_extent = -1;
      if (type_checker_eval_integer_constant(binary_expr->right, &offset)) {
        base_extent =
            type_checker_extract_known_buffer_extent(checker, binary_expr->left);
      } else if (type_checker_eval_integer_constant(binary_expr->left, &offset)) {
        base_extent =
            type_checker_extract_known_buffer_extent(checker, binary_expr->right);
      } else {
        return -1;
      }
      if (base_extent < 0 || offset < 0) {
        return -1;
      }
      if (offset >= base_extent) {
        return 0;
      }
      return base_extent - offset;
    }
  }

  const char *name = type_checker_extract_identifier_name(expression);
  if (!name) {
    return -1;
  }

  TrackedBufferExtent *node = type_checker_buffer_extent_find(checker, name);
  if (!node) {
    return -1;
  }

  return node->byte_count;
}

long long
type_checker_extract_known_pointer_alignment(TypeChecker *checker,
                                             ASTNode *expression) {
  if (!expression) {
    return -1;
  }

  if (expression->type == AST_CAST_EXPRESSION) {
    CastExpression *cast_expr = (CastExpression *)expression->data;
    if (!cast_expr) {
      return -1;
    }
    return type_checker_extract_known_pointer_alignment(checker,
                                                        cast_expr->operand);
  }

  if (expression->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)expression->data;
    return type_checker_extract_allocation_call_alignment(call);
  }

  if (expression->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary_expr = (BinaryExpression *)expression->data;
    if (!binary_expr || !binary_expr->operator || !binary_expr->left ||
        !binary_expr->right) {
      return -1;
    }

    if (strcmp(binary_expr->operator, "+") == 0 ||
        strcmp(binary_expr->operator, "-") == 0) {
      long long offset = 0;
      long long base_align = -1;

      if (type_checker_eval_integer_constant(binary_expr->right, &offset)) {
        base_align = type_checker_extract_known_pointer_alignment(
            checker, binary_expr->left);
      } else if (strcmp(binary_expr->operator, "+") == 0 &&
                 type_checker_eval_integer_constant(binary_expr->left,
                                                   &offset)) {
        base_align = type_checker_extract_known_pointer_alignment(
            checker, binary_expr->right);
      } else {
        return -1;
      }

      if (base_align <= 0) {
        return -1;
      }
      return type_checker_known_alignment_after_offset(base_align, offset);
    }
  }

  const char *name = type_checker_extract_identifier_name(expression);
  if (!name) {
    return -1;
  }

  TrackedBufferExtent *node = type_checker_buffer_extent_find(checker, name);
  if (!node) {
    return -1;
  }

  return node->known_alignment;
}

void type_checker_warn_potential_misaligned_cast(TypeChecker *checker,
                                                        ASTNode *expression,
                                                        CastExpression *cast_expr,
                                                        Type *target_type) {
  if (!checker || !checker->error_reporter || !expression || !cast_expr ||
      !target_type || target_type->kind != TYPE_POINTER ||
      !target_type->base_type) {
    return;
  }

  size_t required_alignment = target_type->base_type->alignment;
  if (required_alignment <= 1) {
    return;
  }

  long long known_alignment =
      type_checker_extract_known_pointer_alignment(checker, cast_expr->operand);
  if (known_alignment <= 0) {
    return;
  }

  if (known_alignment < (long long)required_alignment) {
    char message[512];
    snprintf(
        message, sizeof(message),
        "Cast to %s may violate required %zu-byte alignment (known alignment %lld)",
        target_type->name ? target_type->name : "pointer", required_alignment,
        known_alignment);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               expression->location, message);
  }
}

void type_checker_warn_recv_buffer_bounds(TypeChecker *checker,
                                                 CallExpression *call) {
  if (!checker || !checker->error_reporter || !call || !call->function_name) {
    return;
  }
  if (strcmp(call->function_name, "recv") != 0 || call->argument_count < 3) {
    return;
  }

  const char *buffer_name = type_checker_extract_identifier_name(call->arguments[1]);
  if (!buffer_name) {
    return;
  }

  TrackedBufferExtent *fact =
      type_checker_buffer_extent_find(checker, buffer_name);
  if (!fact || fact->byte_count < 0) {
    return;
  }

  long long recv_len = 0;
  if (!type_checker_eval_integer_constant(call->arguments[2], &recv_len)) {
    return;
  }

  char message[512];
  if (recv_len > fact->byte_count) {
    snprintf(message, sizeof(message),
             "recv length %lld exceeds tracked allocation %lld bytes for '%s'",
             recv_len, fact->byte_count, buffer_name);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               call->arguments[2]->location, message);
  }
}

void type_checker_warn_memcpy_buffer_bounds(TypeChecker *checker,
                                                   CallExpression *call) {
  if (!checker || !checker->error_reporter || !call || !call->function_name) {
    return;
  }
  int is_memcpy = strcmp(call->function_name, "memcpy") == 0;
  int is_memmove = strcmp(call->function_name, "memmove") == 0;
  if ((!is_memcpy && !is_memmove) || call->argument_count < 3) {
    return;
  }

  long long copy_len = 0;
  if (!type_checker_eval_integer_constant(call->arguments[2], &copy_len) ||
      copy_len < 0) {
    return;
  }

  long long dst_extent =
      type_checker_extract_known_buffer_extent(checker, call->arguments[0]);
  long long src_extent =
      type_checker_extract_known_buffer_extent(checker, call->arguments[1]);
  const char *fn_name = call->function_name;

  char message[512];
  if (dst_extent >= 0 && copy_len > dst_extent) {
    snprintf(message, sizeof(message),
             "%s length %lld exceeds known destination extent %lld bytes",
             fn_name, copy_len, dst_extent);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               call->arguments[2]->location, message);
  }

  if (src_extent >= 0 && copy_len > src_extent) {
    snprintf(message, sizeof(message),
             "%s length %lld exceeds known source extent %lld bytes", fn_name,
             copy_len, src_extent);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               call->arguments[2]->location, message);
  }
}

int type_checker_ast_contains_node_type(ASTNode *node,
                                               ASTNodeType target_type) {
  if (!node) {
    return 0;
  }
  if (node->type == target_type) {
    return 1;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (type_checker_ast_contains_node_type(node->children[i], target_type)) {
      return 1;
    }
  }
  return 0;
}

int type_checker_is_null_pointer_constant(ASTNode *expression) {
  long long value = 0;
  return type_checker_eval_integer_constant(expression, &value) && value == 0;
}

int type_checker_statement_guarantees_termination(ASTNode *statement) {
  if (!statement) {
    return 0;
  }

  switch (statement->type) {
  case AST_RETURN_STATEMENT:
  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT:
    return 1;
  case AST_IF_STATEMENT: {
    IfStatement *if_stmt = (IfStatement *)statement->data;
    if (!if_stmt || !if_stmt->then_branch || !if_stmt->else_branch) {
      return 0;
    }
    if (!type_checker_statement_guarantees_termination(if_stmt->then_branch)) {
      return 0;
    }
    for (size_t i = 0; i < if_stmt->else_if_count; i++) {
      if (!if_stmt->else_ifs[i].body ||
          !type_checker_statement_guarantees_termination(
              if_stmt->else_ifs[i].body)) {
        return 0;
      }
    }
    return type_checker_statement_guarantees_termination(if_stmt->else_branch);
  }
  case AST_PROGRAM: {
    for (size_t i = 0; i < statement->child_count; i++) {
      if (type_checker_statement_guarantees_termination(
              statement->children[i])) {
        return 1;
      }
    }
    return 0;
  }
  default:
    return 0;
  }
}
