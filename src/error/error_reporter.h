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
  DIAG_SEVERITY_NOTE,
  DIAG_SEVERITY_NOTE_OF
} ErrorSeverity;

#include "../source_location.h"

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
  char *span_label;
  char *code_override;
} ErrorReport;

typedef struct {
  char *filename;
  char *source_code;
} ErrorReporterSource;

typedef struct {
  SourceSpan span;
  char *message;
} ErrorNoteFrame;

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
  int last_add_suppressed;
  ErrorNoteFrame *note_frames;
  size_t note_frame_count;
  size_t note_frame_capacity;
  int emitting_note_frames;
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
const char *error_reporter_source_for(ErrorReporter *reporter,
                                      const char *filename);

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

void error_reporter_refine_last(ErrorReporter *reporter, const char *message);
SourceSpan error_reporter_span_snap_to_token(ErrorReporter *reporter,
                                             SourceSpan span,
                                             const char *token);
void error_reporter_set_last_label(ErrorReporter *reporter, const char *label);
void error_reporter_set_last_code(ErrorReporter *reporter, const char *code);
void error_reporter_add_note_of_span(ErrorReporter *reporter, SourceSpan span,
                                     const char *message);

int error_reporter_push_note_frame(ErrorReporter *reporter, SourceSpan span,
                                   const char *message);
void error_reporter_pop_note_frame(ErrorReporter *reporter);
size_t error_reporter_note_frame_depth(const ErrorReporter *reporter);
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

const char *error_reporter_suggest_for_token(const char *token);
char *error_reporter_suggest_for_type_mismatch(const char *expected,
                                               const char *actual);

size_t error_reporter_edit_distance(const char *a, const char *b);

char *error_reporter_closest_candidate(const char *name,
                                       const char *const *candidates,
                                       size_t count);

#endif
