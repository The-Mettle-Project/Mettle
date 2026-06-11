// Type checker: match statement and match expression checking.
#include "type_checker_internal.h"

// ---------------------------------------------------------------------------
// Type-check a match statement.
// ---------------------------------------------------------------------------
int type_checker_check_match_statement(TypeChecker *checker,
                                               ASTNode *statement) {
  MatchStatement *match = (MatchStatement *)statement->data;
  if (!match || !match->expression) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid match statement");
    return 0;
  }

  Type *subject_type = type_checker_infer_type(checker, match->expression);
  if (!subject_type)
    return 0;

  if (subject_type->kind != TYPE_TAGGED_ENUM) {
    type_checker_set_error_at_location(
        checker, match->expression->location,
        "match expression must be a tagged enum type, got '%s'",
        subject_type->name);
    return 0;
  }

  int seen_default = 0;
  // Track which variant tags have been covered
  int *covered = calloc(subject_type->tagged_variant_count, sizeof(int));
  if (!covered) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Out of memory in match");
    return 0;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];

    if (arm->is_default) {
      seen_default = 1;
    } else {
      // Find the variant index
      int variant_idx = -1;
      for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
        if (subject_type->tagged_variant_names[v] &&
            strcmp(subject_type->tagged_variant_names[v],
                   arm->variant_name) == 0) {
          variant_idx = (int)v;
          break;
        }
      }
      if (variant_idx < 0) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Unknown variant '%s' for type '%s' in match",
            arm->variant_name, subject_type->name);
        free(covered);
        return 0;
      }
      if (covered[variant_idx]) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Duplicate match arm for variant '%s'", arm->variant_name);
        free(covered);
        return 0;
      }
      covered[variant_idx] = 1;

      // If the arm has a binding, introduce it as a local variable
      if (arm->binding_name) {
        Type *payload =
            subject_type->tagged_variant_payloads[variant_idx];
        if (!payload) {
          type_checker_set_error_at_location(
              checker, statement->location,
              "Variant '%s' carries no payload but binding '%s' was given",
              arm->variant_name, arm->binding_name);
          free(covered);
          return 0;
        }
        if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_BLOCK)) {
          type_checker_set_error_at_location(
              checker, statement->location,
              "Out of memory while entering match binding scope");
          free(covered);
          return 0;
        }
        Symbol *binding =
            symbol_create(arm->binding_name, SYMBOL_VARIABLE, payload);
        if (binding) {
          binding->is_initialized = 1;
          symbol_table_declare(checker->symbol_table, binding);
        }
        int ok = arm->body ? type_checker_check_statement(checker, arm->body)
                           : 1;
        symbol_table_exit_scope(checker->symbol_table);
        if (!ok) {
          free(covered);
          return 0;
        }
        continue;
      }
    }

    // No binding: just check the body
    if (arm->body && !type_checker_check_statement(checker, arm->body)) {
      free(covered);
      return 0;
    }
  }

  // Exhaustiveness check
  if (!seen_default) {
    for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
      if (!covered[v]) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Non-exhaustive match on '%s': variant '%s' not covered; "
            "add a 'case %s:' arm or a 'default:' arm",
            subject_type->name,
            subject_type->tagged_variant_names[v]
                ? subject_type->tagged_variant_names[v]
                : "?",
            subject_type->tagged_variant_names[v]
                ? subject_type->tagged_variant_names[v]
                : "?");
        free(covered);
        return 0;
      }
    }
  }

  free(covered);
  return 1;
}

// Type-check a match used in expression position. Every arm body is a
// value-yielding expression; all arm types must unify, and the match must be
// exhaustive (no implicit fallthrough is allowed when a value is required).
// Returns the unified result Type*, or NULL on error.
Type *type_checker_check_match_expression(TypeChecker *checker,
                                                 ASTNode *expression) {
  MatchStatement *match = (MatchStatement *)expression->data;
  if (!match || !match->expression) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Invalid match expression");
    return NULL;
  }

  Type *subject_type = type_checker_infer_type(checker, match->expression);
  if (!subject_type)
    return NULL;

  if (subject_type->kind != TYPE_TAGGED_ENUM) {
    type_checker_set_error_at_location(
        checker, match->expression->location,
        "match expression must be a tagged enum type, got '%s'",
        subject_type->name);
    return NULL;
  }

  int seen_default = 0;
  Type *result_type = NULL;
  int *covered = calloc(subject_type->tagged_variant_count, sizeof(int));
  if (!covered) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Out of memory in match");
    return NULL;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;
    Type *payload = NULL;

    if (arm->is_default) {
      seen_default = 1;
    } else {
      for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
        if (subject_type->tagged_variant_names[v] &&
            strcmp(subject_type->tagged_variant_names[v],
                   arm->variant_name) == 0) {
          variant_idx = (int)v;
          break;
        }
      }
      if (variant_idx < 0) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Unknown variant '%s' for type '%s' in match",
            arm->variant_name, subject_type->name);
        free(covered);
        return NULL;
      }
      if (covered[variant_idx]) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Duplicate match arm for variant '%s'", arm->variant_name);
        free(covered);
        return NULL;
      }
      covered[variant_idx] = 1;

      if (arm->binding_name) {
        payload = subject_type->tagged_variant_payloads[variant_idx];
        if (!payload) {
          type_checker_set_error_at_location(
              checker, expression->location,
              "Variant '%s' carries no payload but binding '%s' was given",
              arm->variant_name, arm->binding_name);
          free(covered);
          return NULL;
        }
      }
    }

    if (!arm->body) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "match arm must yield a value in expression position");
      free(covered);
      return NULL;
    }

    int has_binding_scope = (payload && arm->binding_name);
    if (has_binding_scope) {
      if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_BLOCK)) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Out of memory while entering match binding scope");
        return NULL;
      }
      Symbol *binding =
          symbol_create(arm->binding_name, SYMBOL_VARIABLE, payload);
      if (binding) {
        binding->is_initialized = 1;
        symbol_table_declare(checker->symbol_table, binding);
      }
    }

    Type *arm_type = type_checker_infer_type(checker, arm->body);

    if (has_binding_scope)
      symbol_table_exit_scope(checker->symbol_table);

    if (!arm_type) {
      free(covered);
      return NULL;
    }

    if (!result_type) {
      result_type = arm_type;
    } else if (!type_checker_are_compatible(result_type, arm_type)) {
      type_checker_set_error_at_location(
          checker, arm->body->location,
          "match arms have incompatible types: '%s' vs '%s'",
          result_type->name ? result_type->name : "?",
          arm_type->name ? arm_type->name : "?");
      free(covered);
      return NULL;
    }
  }

  if (!seen_default) {
    for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
      if (!covered[v]) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Non-exhaustive match expression on '%s': variant '%s' not "
            "covered; add a 'case %s:' arm or a 'default:' arm",
            subject_type->name,
            subject_type->tagged_variant_names[v]
                ? subject_type->tagged_variant_names[v]
                : "?",
            subject_type->tagged_variant_names[v]
                ? subject_type->tagged_variant_names[v]
                : "?");
        free(covered);
        return NULL;
      }
    }
  }

  free(covered);

  if (!result_type) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "match expression has no arms to determine a result type");
    return NULL;
  }
  return result_type;
}
