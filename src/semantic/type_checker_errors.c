// Type checker: diagnostic emission helpers.
#include "type_checker_internal.h"

void type_checker_set_error(TypeChecker *checker, const char *format, ...) {
  if (!checker || !format)
    return;

  // Free previous error message
  free(checker->error_message);

  // Calculate required buffer size
  va_list args1, args2;
  va_start(args1, format);
  va_copy(args2, args1);

  int size = vsnprintf(NULL, 0, format, args1);
  va_end(args1);

  if (size < 0) {
    checker->error_message = NULL;
    checker->has_error = 1;
    va_end(args2);
    return;
  }

  // Allocate and format the message
  checker->error_message = malloc(size + 1);
  if (checker->error_message) {
    vsnprintf(checker->error_message, size + 1, format, args2);
  }

  va_end(args2);
  checker->has_error = 1;
}

// Enhanced error reporting functions

void type_checker_set_error_at_location(TypeChecker *checker,
                                        SourceLocation location,
                                        const char *format, ...) {
  if (!checker || !format)
    return;

  checker->has_error = 1;
  free(checker->error_message);

  va_list args;
  va_start(args, format);

  // Calculate required buffer size
  va_list args_copy;
  va_copy(args_copy, args);
  int size = vsnprintf(NULL, 0, format, args_copy);
  va_end(args_copy);

  if (size > 0) {
    checker->error_message = malloc(size + 1);
    if (checker->error_message) {
      vsnprintf(checker->error_message, size + 1, format, args);
    }
  }

  // If we have an error reporter, add the error to it
  if (checker->error_reporter) {
    char *message = checker->error_message;
    SourceSpan span = source_span_from_location(location, 1);
    error_reporter_add_error_with_span(checker->error_reporter, ERROR_SEMANTIC,
                                       span, message);
  }

  va_end(args);
}

/* Width of the source text a node occupies, for full-token caret underlines.
   Conservative: falls back to 1 when the width isn't recoverable. */
size_t type_checker_node_span_length(const ASTNode *node) {
  if (!node || !node->data)
    return 1;
  switch (node->type) {
  case AST_IDENTIFIER: {
    const Identifier *id = (const Identifier *)node->data;
    return id->name ? strlen(id->name) : 1;
  }
  case AST_STRING_LITERAL: {
    const StringLiteral *lit = (const StringLiteral *)node->data;
    return lit->value ? strlen(lit->value) + 2 : 1; /* include quotes */
  }
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *num = (const NumberLiteral *)node->data;
    char buf[64];
    int n;
    if (num->is_float)
      n = snprintf(buf, sizeof(buf), "%g", num->float_value);
    else
      n = snprintf(buf, sizeof(buf), "%lld", num->int_value);
    return n > 0 ? (size_t)n : 1;
  }
  case AST_FUNCTION_CALL: {
    const CallExpression *call = (const CallExpression *)node->data;
    return call->function_name ? strlen(call->function_name) : 1;
  }
  default:
    return 1;
  }
}

static void type_checker_report_type_mismatch_span(TypeChecker *checker,
                                                   SourceLocation location,
                                                   size_t span_length,
                                                   const char *expected,
                                                   const char *actual) {
  if (!checker || !expected || !actual)
    return;

  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg),
           "Type mismatch: expected '%s', found '%s'", expected, actual);

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    char *suggestion =
        error_reporter_suggest_for_type_mismatch(expected, actual);
    SourceSpan span = source_span_from_location(location, span_length);
    if (suggestion) {
      error_reporter_add_error_with_span_and_suggestion(
          checker->error_reporter, ERROR_TYPE, span, error_msg, suggestion);
      free(suggestion);
    } else {
      error_reporter_add_error_with_span(checker->error_reporter, ERROR_TYPE,
                                         span, error_msg);
    }
    char label[192];
    snprintf(label, sizeof(label), "expected '%s', found '%s'", expected,
             actual);
    error_reporter_set_last_label(checker->error_reporter, label);
  }
}

void type_checker_report_type_mismatch(TypeChecker *checker,
                                       SourceLocation location,
                                       const char *expected,
                                       const char *actual) {
  type_checker_report_type_mismatch_span(checker, location, 1, expected,
                                         actual);
}

void type_checker_report_type_mismatch_node(TypeChecker *checker,
                                            const ASTNode *node,
                                            const char *expected,
                                            const char *actual) {
  if (!node) {
    return;
  }
  type_checker_report_type_mismatch_span(checker, node->location,
                                         type_checker_node_span_length(node),
                                         expected, actual);
}

/* Warn about locals declared in the current (about-to-close) scope that were
   never read. `_`-prefixed names opt out; only the main compile unit is
   checked so imported/stdlib code stays quiet. */
void type_checker_warn_unused_locals(TypeChecker *checker) {
  if (!checker || !checker->error_reporter)
    return;
  Scope *scope = symbol_table_get_current_scope(checker->symbol_table);
  if (!scope || scope->type == SCOPE_GLOBAL)
    return;
  const char *main_file = checker->error_reporter->filename;
  for (size_t i = 0; i < scope->symbol_count; i++) {
    Symbol *s = scope->symbols[i];
    if (!s || s->kind != SYMBOL_VARIABLE || s->is_used)
      continue;
    if (!s->name || s->name[0] == '_' || s->name[0] == '.' || !s->decl_line)
      continue;
    if (s->decl_file && main_file && strcmp(s->decl_file, main_file) != 0)
      continue;
    char msg[256];
    snprintf(msg, sizeof(msg), "unused %s '%s'",
             s->is_immutable ? "constant" : "variable", s->name);
    char suggestion[256];
    snprintf(suggestion, sizeof(suggestion),
             "remove it, or rename it to '_%s' to keep it intentionally",
             s->name);
    SourceSpan span =
        source_span_create(s->decl_line, s->decl_column, strlen(s->name));
    span.filename = s->decl_file;
    span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                             s->name);
    error_reporter_add_warning_span_suggestion(
        checker->error_reporter, ERROR_SEMANTIC, span, msg, suggestion);
  }
}

void type_checker_note_declared_here(TypeChecker *checker,
                                     const Symbol *symbol, const char *what) {
  if (!checker || !checker->error_reporter || !symbol || !symbol->decl_line)
    return;
  char note[256];
  snprintf(note, sizeof(note), "%s '%s' %s here", what, symbol->name,
           symbol->kind == SYMBOL_FUNCTION ? "defined" : "declared");
  SourceSpan span = source_span_create(symbol->decl_line, symbol->decl_column,
                                       strlen(symbol->name));
  span.filename = symbol->decl_file;
  span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                           symbol->name);
  error_reporter_add_note_of_span(checker->error_reporter, span, note);
}

void type_checker_report_undefined_symbol(TypeChecker *checker,
                                          SourceLocation location,
                                          const char *symbol_name,
                                          const char *symbol_type) {
  if (!checker || !symbol_name || !symbol_type)
    return;

  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg), "Undefined %s '%s'", symbol_type,
           symbol_name);

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    char suggestion[256];
    char *closest = symbol_table_suggest_similar(checker->symbol_table,
                                                 symbol_name, NULL, 0);
    if (closest) {
      snprintf(suggestion, sizeof(suggestion),
               "did you mean '%s'? (or declare '%s' before using it)", closest,
               symbol_name);
      free(closest);
    } else {
      snprintf(suggestion, sizeof(suggestion), "declare '%s' before using it",
               symbol_name);
    }
    SourceSpan span = source_span_from_location(location, strlen(symbol_name));
    span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                             symbol_name);
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_SEMANTIC, span, error_msg, suggestion);
  }
}

void type_checker_report_duplicate_declaration_prev(TypeChecker *checker,
                                                    SourceLocation location,
                                                    const char *symbol_name,
                                                    const Symbol *previous) {
  type_checker_report_duplicate_declaration(checker, location, symbol_name);
  if (previous && previous->decl_line) {
    char note[256];
    snprintf(note, sizeof(note), "previous declaration of '%s' is here",
             symbol_name);
    SourceSpan span = source_span_create(
        previous->decl_line, previous->decl_column, strlen(symbol_name));
    span.filename = previous->decl_file;
    span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                             symbol_name);
    error_reporter_add_note_of_span(checker->error_reporter, span, note);
  }
}

void type_checker_report_duplicate_declaration(TypeChecker *checker,
                                               SourceLocation location,
                                               const char *symbol_name) {
  if (!checker || !symbol_name)
    return;

  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg), "Duplicate declaration of '%s'",
           symbol_name);

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    char suggestion[256];
    snprintf(suggestion, sizeof(suggestion),
             "use a different name or remove the duplicate declaration");
    SourceSpan span = source_span_from_location(location, strlen(symbol_name));
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_SEMANTIC, span, error_msg, suggestion);
  }
}
