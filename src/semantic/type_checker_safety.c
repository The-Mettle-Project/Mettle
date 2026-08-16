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

typedef struct {
  int is_float;
  long long int_value;
  double float_value;
} TypeCheckerConstant;

static void type_checker_constant_from_int(TypeCheckerConstant *value,
                                           long long int_value) {
  value->is_float = 0;
  value->int_value = int_value;
  value->float_value = (double)int_value;
}

static void type_checker_constant_from_float(TypeCheckerConstant *value,
                                             double float_value) {
  value->is_float = 1;
  value->int_value = (long long)float_value;
  value->float_value = float_value;
}

static int type_checker_eval_numeric_constant(TypeChecker *checker,
                                              ASTNode *expression,
                                              TypeCheckerConstant *out_value) {
  if (!expression || !out_value) {
    return 0;
  }

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal || literal->is_float) {
      if (!literal) {
        return 0;
      }
      type_checker_constant_from_float(out_value, literal->float_value);
      return 1;
    }
    type_checker_constant_from_int(out_value, literal->int_value);
    return 1;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      return 0;
    }

    Symbol *symbol = checker
                         ? type_checker_resolve_identifier(checker, identifier)
                         : NULL;
    if (!symbol || (symbol->kind != SYMBOL_CONSTANT &&
                    !symbol->has_constant_value)) {
      return 0;
    }

    if (symbol->has_constant_value && symbol->constant_is_float) {
      type_checker_constant_from_float(out_value,
                                       symbol->constant_float_value);
    } else {
      long long value = symbol->has_constant_value
                            ? symbol->constant_integer_value
                            : symbol->data.constant.value;
      type_checker_constant_from_int(out_value, value);
    }
    return 1;
  }

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    if (!call || !call->function_name) {
      return 0;
    }
    if (strcmp(call->function_name, "offsetof") == 0) {
      long long offset = 0;
      if (!type_checker_eval_offsetof(checker, call, expression->location,
                                      &offset)) {
        return 0;
      }
      type_checker_constant_from_int(out_value, offset);
      return 1;
    }
    if (strcmp(call->function_name, "sizeof") != 0 ||
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

    type_checker_constant_from_int(out_value, (long long)type->size);
    return 1;
  }

  /* `f.offset` and friends: a Field member is a compile-time integer, so it
   * belongs in every constant context sizeof and offsetof already reach. */
  case AST_MEMBER_ACCESS: {
    ComptimeValue folded = comptime_none();
    if (!checker ||
        !type_checker_eval_comptime(checker, expression, &folded)) {
      return 0;
    }
    if (folded.kind == COMPTIME_INT) {
      type_checker_constant_from_int(out_value, folded.as.int_value);
      return 1;
    }
    if (folded.kind == COMPTIME_FLOAT) {
      type_checker_constant_from_float(out_value, folded.as.float_value);
      return 1;
    }
    return 0;
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary_expr = (UnaryExpression *)expression->data;
    TypeCheckerConstant operand = {0};
    if (!unary_expr || !unary_expr->operator || !unary_expr->operand ||
        !type_checker_eval_numeric_constant(
            checker, unary_expr->operand, &operand)) {
      return 0;
    }

    if (strcmp(unary_expr->operator, "+") == 0) {
      *out_value = operand;
      return 1;
    }
    if (strcmp(unary_expr->operator, "-") == 0) {
      if (operand.is_float) {
        type_checker_constant_from_float(out_value, -operand.float_value);
      } else {
        type_checker_constant_from_int(out_value, -operand.int_value);
      }
      return 1;
    }
    if (strcmp(unary_expr->operator, "!") == 0) {
      int is_zero = operand.is_float ? operand.float_value == 0.0
                                     : operand.int_value == 0;
      type_checker_constant_from_int(out_value, is_zero);
      return 1;
    }
    if (strcmp(unary_expr->operator, "~") == 0 && !operand.is_float) {
      type_checker_constant_from_int(out_value, ~operand.int_value);
      return 1;
    }
    return 0;
  }

  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary_expr = (BinaryExpression *)expression->data;
    TypeCheckerConstant left = {0};
    TypeCheckerConstant right = {0};
    if (!binary_expr || !binary_expr->operator || !binary_expr->left ||
        !binary_expr->right ||
        !type_checker_eval_numeric_constant(
            checker, binary_expr->left, &left) ||
        !type_checker_eval_numeric_constant(
            checker, binary_expr->right, &right)) {
      return 0;
    }

    if (left.is_float || right.is_float) {
      double left_value = left.is_float ? left.float_value
                                        : (double)left.int_value;
      double right_value = right.is_float ? right.float_value
                                          : (double)right.int_value;
      const char *operator = binary_expr->operator;
      if (strcmp(operator, "+") == 0) {
        type_checker_constant_from_float(out_value,
                                         left_value + right_value);
        return 1;
      }
      if (strcmp(operator, "-") == 0) {
        type_checker_constant_from_float(out_value,
                                         left_value - right_value);
        return 1;
      }
      if (strcmp(operator, "*") == 0) {
        type_checker_constant_from_float(out_value,
                                         left_value * right_value);
        return 1;
      }
      if (strcmp(operator, "/") == 0) {
        if (right_value == 0.0) {
          return 0;
        }
        type_checker_constant_from_float(out_value,
                                         left_value / right_value);
        return 1;
      }
      if (strcmp(operator, "==") == 0) {
        type_checker_constant_from_int(out_value, left_value == right_value);
        return 1;
      }
      if (strcmp(operator, "!=") == 0) {
        type_checker_constant_from_int(out_value, left_value != right_value);
        return 1;
      }
      if (strcmp(operator, "<") == 0) {
        type_checker_constant_from_int(out_value, left_value < right_value);
        return 1;
      }
      if (strcmp(operator, "<=") == 0) {
        type_checker_constant_from_int(out_value, left_value <= right_value);
        return 1;
      }
      if (strcmp(operator, ">") == 0) {
        type_checker_constant_from_int(out_value, left_value > right_value);
        return 1;
      }
      if (strcmp(operator, ">=") == 0) {
        type_checker_constant_from_int(out_value, left_value >= right_value);
        return 1;
      }
      if (strcmp(operator, "&&") == 0) {
        type_checker_constant_from_int(
            out_value, (left_value != 0.0) && (right_value != 0.0));
        return 1;
      }
      if (strcmp(operator, "||") == 0) {
        type_checker_constant_from_int(
            out_value, (left_value != 0.0) || (right_value != 0.0));
        return 1;
      }
      return 0;
    }

    long long left_value = left.int_value;
    long long right_value = right.int_value;
    if (strcmp(binary_expr->operator, "+") == 0) {
      type_checker_constant_from_int(out_value, left_value + right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "-") == 0) {
      type_checker_constant_from_int(out_value, left_value - right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "*") == 0) {
      type_checker_constant_from_int(out_value, left_value * right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "/") == 0) {
      if (right_value == 0) {
        return 0;
      }
      type_checker_constant_from_int(out_value, left_value / right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "%") == 0) {
      if (right_value == 0) {
        return 0;
      }
      type_checker_constant_from_int(out_value, left_value % right_value);
      return 1;
    }
    /* Bitwise and shift folding. These are how a byte constant is usually
     * written -- `1 << 7`, `0xF0 | 0x0F` -- so leaving them unfolded would
     * make the range check refuse constants that plainly fit. A shift count
     * at or past the width has no defined value to fold, so it is declined
     * (the caller then treats the expression as non-constant). */
    if (strcmp(binary_expr->operator, "&") == 0) {
      type_checker_constant_from_int(out_value, left_value & right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "|") == 0) {
      type_checker_constant_from_int(out_value, left_value | right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "^") == 0) {
      type_checker_constant_from_int(out_value, left_value ^ right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "<<") == 0) {
      if (right_value < 0 || right_value > 62 || left_value < 0) {
        return 0;
      }
      if (left_value > (LLONG_MAX >> right_value)) {
        return 0;
      }
      type_checker_constant_from_int(out_value, left_value << right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, ">>") == 0) {
      if (right_value < 0 || right_value > 63) {
        return 0;
      }
      type_checker_constant_from_int(out_value, left_value >> right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "==") == 0) {
      type_checker_constant_from_int(out_value, left_value == right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "!=") == 0) {
      type_checker_constant_from_int(out_value, left_value != right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "<") == 0) {
      type_checker_constant_from_int(out_value, left_value < right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "<=") == 0) {
      type_checker_constant_from_int(out_value, left_value <= right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, ">") == 0) {
      type_checker_constant_from_int(out_value, left_value > right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, ">=") == 0) {
      type_checker_constant_from_int(out_value, left_value >= right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "&&") == 0) {
      type_checker_constant_from_int(out_value,
                                     (left_value != 0) && (right_value != 0));
      return 1;
    }
    if (strcmp(binary_expr->operator, "||") == 0) {
      type_checker_constant_from_int(out_value,
                                     (left_value != 0) || (right_value != 0));
      return 1;
    }
    return 0;
  }

  default:
    return 0;
  }
}

int type_checker_eval_integer_constant_with_checker(TypeChecker *checker,
                                                    ASTNode *expression,
                                                    long long *out_value) {
  TypeCheckerConstant value = {0};
  if (!out_value || !type_checker_eval_numeric_constant(checker, expression,
                                                         &value) ||
      value.is_float) {
    return 0;
  }
  *out_value = value.int_value;
  return 1;
}

int type_checker_eval_float_constant_with_checker(TypeChecker *checker,
                                                 ASTNode *expression,
                                                 double *out_value) {
  TypeCheckerConstant value = {0};
  if (!out_value || !type_checker_eval_numeric_constant(checker, expression,
                                                         &value)) {
    return 0;
  }
  *out_value = value.is_float ? value.float_value : (double)value.int_value;
  return 1;
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

  if (type_contains_comptime_only(type)) {
    type_checker_reject_no_runtime_repr(checker, arg->location, type);
    return NULL;
  }

  return type;
}

int type_checker_eval_offsetof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location, long long *out_offset) {
  if (!checker || !call || !out_offset) {
    return 0;
  }
  if (call->argument_count != 1 || !call->arguments || !call->arguments[0]) {
    type_checker_set_error_at_location(
        checker, location, "offsetof expects exactly one field argument");
    return 0;
  }

  ASTNode *arg = call->arguments[0];
  Type *arg_type = type_checker_infer_type(checker, arg);
  if (!arg_type) {
    return 0;
  }
  if (arg_type->kind != TYPE_FIELD) {
    type_checker_set_error_at_location(
        checker, arg->location,
        "offsetof expects a compile-time Field (for example Point.x)");
    return 0;
  }

  ComptimeValue value = comptime_none();
  if (!type_checker_eval_comptime(checker, arg, &value) ||
      value.kind != COMPTIME_FIELD_REF) {
    type_checker_set_error_at_location(
        checker, arg->location,
        "offsetof argument must be a compile-time Field value");
    return 0;
  }

  Type *owner =
      type_checker_type_from_index(checker, value.as.field_ref.type_index);
  TypeField field;
  if (!owner ||
      !type_field_at(owner, value.as.field_ref.field_index, &field)) {
    type_checker_set_error_at_location(
        checker, arg->location,
        "offsetof could not read that field from the type table");
    return 0;
  }
  if (field.byte_offset > (size_t)LLONG_MAX) {
    type_checker_set_error_at_location(checker, arg->location,
                                       "field offset does not fit in int64");
    return 0;
  }
  *out_offset = (long long)field.byte_offset;
  return 1;
}

Type *type_checker_resolve_typeof_argument(TypeChecker *checker,
                                           CallExpression *call,
                                           SourceLocation location) {
  if (!checker || !call) {
    return NULL;
  }

  if (call->argument_count != 1 || !call->arguments || !call->arguments[0]) {
    type_checker_set_error_at_location(
        checker, location, "typeof expects exactly one argument");
    return NULL;
  }

  ASTNode *arg = call->arguments[0];
  if (arg->type == AST_IDENTIFIER && arg->data) {
    Identifier *id = (Identifier *)arg->data;
    if (id && id->name) {
      Type *named = type_checker_get_type_by_name(checker, id->name);
      Symbol *symbol = type_checker_resolve_identifier(checker, id);
      if (named && (!symbol || symbol->kind == SYMBOL_STRUCT ||
                    symbol->kind == SYMBOL_ENUM)) {
        return named;
      }
    }
  }

  Type *inferred = type_checker_infer_type(checker, arg);
  if (!inferred) {
    return NULL;
  }
  return inferred;
}

static int eval_comptime_from_symbol(TypeChecker *checker, Symbol *symbol,
                                     ComptimeValue *out_value) {
  if (!symbol || !out_value) {
    return 0;
  }
  if (!comptime_is_none(symbol->comptime_value)) {
    *out_value = symbol->comptime_value;
    return 1;
  }
  if (symbol->kind == SYMBOL_STRUCT || symbol->kind == SYMBOL_ENUM) {
    if (!symbol->type) {
      return 0;
    }
    uint32_t index = type_checker_intern_type(checker, symbol->type);
    if (index == UINT32_MAX) {
      return 0;
    }
    *out_value = comptime_type_ref(index);
    return 1;
  }
  if (symbol->has_constant_value) {
    if (symbol->constant_is_float) {
      *out_value = comptime_float(symbol->constant_float_value);
    } else {
      *out_value = comptime_int(symbol->constant_integer_value);
    }
    return 1;
  }
  if (symbol->kind == SYMBOL_CONSTANT) {
    *out_value = comptime_int(symbol->data.constant.value);
    return 1;
  }
  return 0;
}

int type_checker_eval_comptime(TypeChecker *checker, ASTNode *expression,
                               ComptimeValue *out_value) {
  if (!checker || !expression || !out_value) {
    return 0;
  }
  *out_value = comptime_none();

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal) {
      return 0;
    }
    if (literal->is_float) {
      *out_value = comptime_float(literal->float_value);
    } else {
      *out_value = comptime_int(literal->int_value);
    }
    return 1;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      return 0;
    }
    Symbol *symbol = type_checker_resolve_identifier(checker, identifier);
    if (eval_comptime_from_symbol(checker, symbol, out_value)) {
      return 1;
    }
    Type *named = type_checker_get_type_by_name(checker, identifier->name);
    if (named) {
      uint32_t index = type_checker_intern_type(checker, named);
      if (index == UINT32_MAX) {
        return 0;
      }
      *out_value = comptime_type_ref(index);
      return 1;
    }
    return 0;
  }

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    if (!call || !call->function_name) {
      return 0;
    }
    if (strcmp(call->function_name, "offsetof") == 0) {
      long long offset = 0;
      if (!type_checker_eval_offsetof(checker, call, expression->location,
                                      &offset)) {
        return 0;
      }
      *out_value = comptime_int(offset);
      return 1;
    }
    if (strcmp(call->function_name, "typeof") == 0) {
      Type *referred = type_checker_resolve_typeof_argument(
          checker, call, expression->location);
      if (!referred) {
        return 0;
      }
      uint32_t index = type_checker_intern_type(checker, referred);
      if (index == UINT32_MAX) {
        return 0;
      }
      *out_value = comptime_type_ref(index);
      return 1;
    }
    if (strcmp(call->function_name, "sizeof") == 0) {
      Type *sized = type_checker_resolve_sizeof_argument(
          checker, call, expression->location);
      if (!sized) {
        return 0;
      }
      *out_value = comptime_int((long long)sized->size);
      return 1;
    }
    return 0;
  }

  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expression->data;
    if (!member || !member->object || !member->member) {
      return 0;
    }
    ComptimeValue owner = comptime_none();
    if (!type_checker_eval_comptime(checker, member->object, &owner)) {
      return 0;
    }
    if (owner.kind == COMPTIME_FIELD_REF) {
      return type_checker_eval_field_member(checker, owner, member->member,
                                            out_value);
    }
    if (owner.kind == COMPTIME_SEQUENCE) {
      return type_checker_eval_sequence_member(checker, owner, member->member,
                                               out_value);
    }
    if (owner.kind != COMPTIME_TYPE_REF) {
      return 0;
    }
    Type *referred =
        type_checker_type_from_index(checker, owner.as.type_ref.type_index);
    if (!referred) {
      return 0;
    }
    /* `Color.Red` on a plain enum reads the member off the type table rather
     * than the variant's bare global, so a compiler-registered enum that
     * deliberately declares no bare globals still folds. */
    if (referred->kind == TYPE_ENUM) {
      for (size_t i = 0; i < referred->enum_member_count; i++) {
        if (referred->enum_member_names[i] &&
            strcmp(referred->enum_member_names[i], member->member) == 0) {
          *out_value = comptime_int(referred->enum_member_values[i]);
          return 1;
        }
      }
      /* Not a variant, so fall through: an enum type answers the same shape
       * queries every other type does (`typeof(Color).kind`). */
    }
    /* A struct field named the same as a query wins: the program's own
     * declaration is never shadowed by the reflection surface. */
    int field_index = type_get_field_index(referred, member->member);
    if (field_index >= 0) {
      *out_value = comptime_field_ref(owner.as.type_ref.type_index,
                                      (uint32_t)field_index);
      return 1;
    }
    return type_checker_eval_type_member(checker, owner, member->member,
                                         out_value);
  }

  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index_expr =
        (ArrayIndexExpression *)expression->data;
    if (!index_expr || !index_expr->array || !index_expr->index) {
      return 0;
    }
    ComptimeValue sequence = comptime_none();
    ComptimeValue subscript = comptime_none();
    if (!type_checker_eval_comptime(checker, index_expr->array, &sequence) ||
        sequence.kind != COMPTIME_SEQUENCE ||
        !type_checker_eval_comptime(checker, index_expr->index, &subscript) ||
        subscript.kind != COMPTIME_INT) {
      return 0;
    }
    return type_checker_eval_sequence_index(sequence, subscript.as.int_value,
                                            out_value);
  }

  default:
    return 0;
  }
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
    /* Folding failed, which says the condition is not constant but not why.
     * Type checking the condition first surfaces the real reason -- an unknown
     * query, a sequence index out of range -- and only when that comes back
     * clean is "not a constant" actually the whole story. */
    if (call->arguments[0] &&
        !type_checker_infer_type(checker, call->arguments[0])) {
      return 0;
    }
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

int type_checker_type_accepts_null_pointer(const Type *type) {
  if (!type) {
    return 0;
  }
  return type->kind == TYPE_POINTER || type->kind == TYPE_FUNCTION_POINTER;
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
