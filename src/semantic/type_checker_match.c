#include "type_checker_internal.h"

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
  if (type_checker_reject_comptime_escape(checker, match->expression->location,
                                          subject_type)) {
    return 0;
  }

  if (subject_type->kind != TYPE_TAGGED_ENUM) {
    type_checker_set_error_at_location(
        checker, match->expression->location,
        "match expression must be a tagged enum type, got '%s'",
        subject_type->name);
    return 0;
  }

  int seen_default = 0;
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

    if (arm->body && !type_checker_check_statement(checker, arm->body)) {
      free(covered);
      return 0;
    }
  }

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
