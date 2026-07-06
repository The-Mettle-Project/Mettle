#ifndef ERROR_REPORTER_H
#define ERROR_REPORTER_H

#include <stddef.h>

typedef enum {
  ERROR_LEXICAL,
  ERROR_SYNTAX,
  ERROR_SEMANTIC,
  ERROR_TYPE,
  ERROR_SCOPE,
  ERROR_IO,
  ERROR_INTERNAL
} ErrorType;

/* Short labels printed in diagnostic header, e.g. [E0002].
   Stable across compiler versions — useful for test grep and docs. */
#define ERROR_CODE_LEXICAL   "E0001"
#define ERROR_CODE_SYNTAX    "E0002"
#define ERROR_CODE_SEMANTIC  "E0003"
#define ERROR_CODE_TYPE      "E0004"
#define ERROR_CODE_SCOPE     "E0005"
#define ERROR_CODE_IO        "E0006"
#define ERROR_CODE_INTERNAL  "E0007"

typedef enum {
  DIAG_SEVERITY_ERROR,
  DIAG_SEVERITY_WARNING,
  /* Standalone informational message */
  DIAG_SEVERITY_NOTE,
  /* Note attached to the preceding diagnostic (printed inline) */
  DIAG_SEVERITY_NOTE_OF
} ErrorSeverity;

#include "../parser/ast.h"

typedef struct {
  size_t line;
  size_t column;
  size_t length;
  const char *filename;
} SourceSpan;

typedef struct {
  ErrorType type;
  ErrorSeverity severity;
  SourceLocation location;
  SourceSpan span;
  char *filename;
  const char *source_code;
  char *message;
  char *suggestion;
  char *code_snippet;
  /* Short text rendered inline after the caret underline, e.g.
     "expected 'int64', found 'string'". Optional. */
  char *span_label;
} ErrorReport;

typedef struct {
  char *filename;
  char *source_code;
} ErrorReporterSource;

typedef struct {
  ErrorReport *errors;
  size_t count;
  size_t capacity;
  size_t max_errors;
  const char *source_code;
  const char *filename;
  ErrorReporterSource *sources;
  size_t source_count;
  size_t source_capacity;
  const char *current_filename;
  const char *current_source_code;
  /* Set when the most recent add was suppressed as a duplicate/cascade, so
     follow-up label/note attachments know to skip themselves too. */
  int last_add_suppressed;
} ErrorReporter;

ErrorReporter *error_reporter_create(const char *filename,
                                     const char *source_code);
void error_reporter_destroy(ErrorReporter *reporter);
int error_reporter_register_source(ErrorReporter *reporter,
                                   const char *filename,
                                   const char *source_code);
int error_reporter_set_source_context(ErrorReporter *reporter,
                                      const char *filename,
                                      const char *source_code);
const char *error_reporter_current_filename(ErrorReporter *reporter);
const char *error_reporter_current_source_code(ErrorReporter *reporter);

void error_reporter_add_error(ErrorReporter *reporter, ErrorType type,
                              SourceLocation location, const char *message);
void error_reporter_add_error_with_suggestion(ErrorReporter *reporter,
                                              ErrorType type,
                                              SourceLocation location,
                                              const char *message,
                                              const char *suggestion);
void error_reporter_add_error_with_span(ErrorReporter *reporter, ErrorType type,
                                        SourceSpan span, const char *message);
void error_reporter_add_error_with_span_and_suggestion(
    ErrorReporter *reporter, ErrorType type, SourceSpan span, const char *message,
    const char *suggestion);
void error_reporter_add_warning(ErrorReporter *reporter, ErrorType type,
                                SourceLocation location, const char *message);
void error_reporter_add_warning_with_span(ErrorReporter *reporter, ErrorType type,
                                          SourceSpan span, const char *message);
void error_reporter_add_warning_with_suggestion(ErrorReporter *reporter,
                                                ErrorType type,
                                                SourceLocation location,
                                                const char *message,
                                                const char *suggestion);
void error_reporter_add_warning_span_suggestion(ErrorReporter *reporter,
                                                ErrorType type, SourceSpan span,
                                                const char *message,
                                                const char *suggestion);

/* Replace the message of the most recently added error with a more precise
   contextual one (e.g. "Expected '(', found identifier" -> "Expected '('
   after 'if'"). No-op if the last add was suppressed as a cascade. */
void error_reporter_refine_last(ErrorReporter *reporter, const char *message);
/* Move a span's caret onto the first whole-word occurrence of `token` on its
   line (searching from the span's column), so diagnostics underline the name
   rather than a leading keyword. Returns the span unchanged if not found. */
SourceSpan error_reporter_span_snap_to_token(ErrorReporter *reporter,
                                             SourceSpan span,
                                             const char *token);
/* Attach an inline caret label to the most recently added diagnostic. */
void error_reporter_set_last_label(ErrorReporter *reporter, const char *label);
/* Attach a note (with its own source snippet) to the most recently added
   diagnostic, e.g. "function 'add' defined here". */
void error_reporter_add_note_of_span(ErrorReporter *reporter, SourceSpan span,
                                     const char *message);
/* Switch diagnostic printing to newline-delimited JSON (for tooling). */
void error_reporter_set_format_json(int enabled);
int error_reporter_format_json(void);
int error_reporter_get_warning_count(ErrorReporter *reporter);

void error_reporter_print_errors(ErrorReporter *reporter);
void error_reporter_print_error(ErrorReporter *reporter,
                                const ErrorReport *error);
int error_reporter_has_errors(ErrorReporter *reporter);
int error_reporter_get_error_count(ErrorReporter *reporter);

SourceLocation source_location_create(size_t line, size_t column);
SourceSpan source_span_create(size_t line, size_t column, size_t length);
SourceSpan source_span_from_location(SourceLocation location, size_t length);
char *error_reporter_get_line_from_source(const char *source,
                                          size_t line_number);
char *error_reporter_create_caret_line(size_t column, size_t length);

// Common error suggestions
const char *error_reporter_suggest_for_token(const char *token);
// Returns a heap-allocated suggestion string (caller must free), or NULL.
char *error_reporter_suggest_for_type_mismatch(const char *expected,
                                               const char *actual);

// Case-insensitive-tolerant Levenshtein edit distance between two strings.
size_t error_reporter_edit_distance(const char *a, const char *b);

// Given a misspelled `name` and a list of `candidates` (count entries),
// returns the heap-allocated closest candidate within a sensible edit-distance
// threshold (scaled to the name length), or NULL if none is close enough.
// Caller must free the returned string.
char *error_reporter_closest_candidate(const char *name,
                                       const char *const *candidates,
                                       size_t count);

#endif // ERROR_REPORTER_H
