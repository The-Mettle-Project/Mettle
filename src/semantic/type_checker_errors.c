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

void type_checker_report_type_mismatch(TypeChecker *checker,
                                       SourceLocation location,
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
    SourceSpan span = source_span_from_location(location, 1);
    if (suggestion) {
      error_reporter_add_error_with_span_and_suggestion(
          checker->error_reporter, ERROR_TYPE, span, error_msg, suggestion);
      free(suggestion);
    } else {
      error_reporter_add_error_with_span(checker->error_reporter, ERROR_TYPE,
                                         span, error_msg);
    }
  }
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
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_SEMANTIC, span, error_msg, suggestion);
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
