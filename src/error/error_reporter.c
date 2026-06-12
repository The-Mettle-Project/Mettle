#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "error_reporter.h"
#include "../common.h"
#include "../parser/ast.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

#define INITIAL_ERROR_CAPACITY 16
#define MAX_ERRORS_DEFAULT 100
#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_RESET "\x1b[0m"
#define ANSI_BOLD "\x1b[1m"

/* Maximum source-line width in the snippet before truncation with "…" */
#define SNIPPET_MAX_COLS 120

/* All diagnostic output goes to stderr so it doesn't pollute stdout pipelines */
#define DIAG_STREAM stderr

#ifdef _WIN32
/* Enable VT sequences on Windows once per process */
static void error_reporter_enable_vt_windows(void) {
  static int done = 0;
  if (done)
    return;
  done = 1;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (h == INVALID_HANDLE_VALUE)
    return;
  DWORD mode = 0;
  if (!GetConsoleMode(h, &mode))
    return;
  SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#endif

static int error_reporter_should_use_color(void) {
  static int cached = -1;
  if (cached >= 0) return cached;

  /* CLICOLOR_FORCE=1 overrides everything (force color even when not a tty) */
  const char *force = getenv("CLICOLOR_FORCE");
  if (force && force[0] != '\0' && strcmp(force, "0") != 0) {
#ifdef _WIN32
    error_reporter_enable_vt_windows();
#endif
    cached = 1;
    return cached;
  }

  /* NO_COLOR disables color (https://no-color.org/) */
  const char *no_color = getenv("NO_COLOR");
  if (no_color && no_color[0] != '\0') {
    cached = 0;
    return cached;
  }

  /* TERM=dumb - no sequences */
  const char *term = getenv("TERM");
  if (term && strcmp(term, "dumb") == 0) {
    cached = 0;
    return cached;
  }

  /* CLICOLOR=0 disables even when tty */
  const char *clicolor = getenv("CLICOLOR");
  if (clicolor && strcmp(clicolor, "0") == 0) {
    cached = 0;
    return cached;
  }

  /* Only color when stderr is a tty */
  int fd = fileno(DIAG_STREAM);
  if (fd < 0) {
    cached = 0;
    return cached;
  }
  if (!isatty(fd)) {
    cached = 0;
    return cached;
  }

#ifdef _WIN32
  error_reporter_enable_vt_windows();
#endif
  cached = 1;
  return cached;
}

/* Return the short error-code string for an ErrorType */
static const char *error_type_code(ErrorType type) {
  switch (type) {
  case ERROR_LEXICAL:  return ERROR_CODE_LEXICAL;
  case ERROR_SYNTAX:   return ERROR_CODE_SYNTAX;
  case ERROR_SEMANTIC: return ERROR_CODE_SEMANTIC;
  case ERROR_TYPE:     return ERROR_CODE_TYPE;
  case ERROR_SCOPE:    return ERROR_CODE_SCOPE;
  case ERROR_IO:       return ERROR_CODE_IO;
  case ERROR_INTERNAL: return ERROR_CODE_INTERNAL;
  default:             return "E????";
  }
}

static size_t error_reporter_count_digits(size_t n) {
  size_t digits = 1;
  while (n >= 10) {
    n /= 10;
    digits++;
  }
  return digits;
}

ErrorReporter *error_reporter_create(const char *filename,
                                     const char *source_code) {
  ErrorReporter *reporter = malloc(sizeof(ErrorReporter));
  if (!reporter)
    return NULL;

  reporter->errors = malloc(sizeof(ErrorReport) * INITIAL_ERROR_CAPACITY);
  if (!reporter->errors) {
    free(reporter);
    return NULL;
  }

  reporter->count = 0;
  reporter->capacity = INITIAL_ERROR_CAPACITY;
  reporter->max_errors = MAX_ERRORS_DEFAULT;
  reporter->source_code = source_code;
  reporter->filename = filename ? mettle_strdup(filename) : NULL;
  reporter->sources = NULL;
  reporter->source_count = 0;
  reporter->source_capacity = 0;
  reporter->current_filename = reporter->filename;
  reporter->current_source_code = reporter->source_code;
  error_reporter_register_source(reporter, filename, source_code);

  return reporter;
}

SourceSpan source_span_create(size_t line, size_t column, size_t length) {
  SourceSpan span;
  span.line = line;
  span.column = column;
  span.length = length;
  span.filename = NULL;
  return span;
}

SourceSpan source_span_from_location(SourceLocation location, size_t length) {
  SourceSpan span = source_span_create(location.line, location.column, length);
  span.filename = location.filename;
  return span;
}

void error_reporter_destroy(ErrorReporter *reporter) {
  if (!reporter)
    return;

  for (size_t i = 0; i < reporter->count; i++) {
    free(reporter->errors[i].filename);
    free(reporter->errors[i].message);
    free(reporter->errors[i].suggestion);
    free(reporter->errors[i].code_snippet);
  }

  for (size_t i = 0; i < reporter->source_count; i++) {
    free(reporter->sources[i].filename);
    free(reporter->sources[i].source_code);
  }
  free(reporter->sources);
  free(reporter->errors);
  free((char *)reporter->filename);
  free(reporter);
}

static const ErrorReporterSource *
error_reporter_find_source(ErrorReporter *reporter, const char *filename) {
  if (!reporter || !filename) {
    return NULL;
  }

  for (size_t i = 0; i < reporter->source_count; i++) {
    if (reporter->sources[i].filename &&
        strcmp(reporter->sources[i].filename, filename) == 0) {
      return &reporter->sources[i];
    }
  }

  return NULL;
}

int error_reporter_register_source(ErrorReporter *reporter,
                                   const char *filename,
                                   const char *source_code) {
  if (!reporter || !filename || !source_code) {
    return 0;
  }

  ErrorReporterSource *existing =
      (ErrorReporterSource *)error_reporter_find_source(reporter, filename);
  if (existing) {
    char *source_copy = mettle_strdup(source_code);
    if (!source_copy) {
      return 0;
    }
    free(existing->source_code);
    existing->source_code = source_copy;
    return 1;
  }

  if (reporter->source_count >= reporter->source_capacity) {
    size_t new_capacity =
        reporter->source_capacity == 0 ? 8 : reporter->source_capacity * 2;
    ErrorReporterSource *grown =
        realloc(reporter->sources, new_capacity * sizeof(ErrorReporterSource));
    if (!grown) {
      return 0;
    }
    reporter->sources = grown;
    reporter->source_capacity = new_capacity;
  }

  ErrorReporterSource *entry = &reporter->sources[reporter->source_count];
  entry->filename = mettle_strdup(filename);
  entry->source_code = mettle_strdup(source_code);
  if (!entry->filename || !entry->source_code) {
    free(entry->filename);
    free(entry->source_code);
    entry->filename = NULL;
    entry->source_code = NULL;
    return 0;
  }

  reporter->source_count++;
  return 1;
}

int error_reporter_set_source_context(ErrorReporter *reporter,
                                      const char *filename,
                                      const char *source_code) {
  if (!reporter) {
    return 0;
  }

  if (filename && source_code &&
      !error_reporter_register_source(reporter, filename, source_code)) {
    return 0;
  }

  const ErrorReporterSource *entry =
      filename ? error_reporter_find_source(reporter, filename) : NULL;
  reporter->current_filename =
      entry ? entry->filename : (filename ? filename : reporter->filename);
  reporter->current_source_code =
      entry ? entry->source_code
            : (source_code ? source_code : reporter->source_code);
  return 1;
}

const char *error_reporter_current_filename(ErrorReporter *reporter) {
  return reporter ? reporter->current_filename : NULL;
}

const char *error_reporter_current_source_code(ErrorReporter *reporter) {
  return reporter ? reporter->current_source_code : NULL;
}

static int error_reporter_expand_capacity(ErrorReporter *reporter) {
  if (reporter->count >= reporter->capacity) {
    size_t new_capacity = reporter->capacity * 2;
    ErrorReport *new_errors =
        realloc(reporter->errors, sizeof(ErrorReport) * new_capacity);
    if (!new_errors)
      return 0;

    reporter->errors = new_errors;
    reporter->capacity = new_capacity;
  }
  return 1;
}

void error_reporter_add_error(ErrorReporter *reporter, ErrorType type,
                              SourceLocation location, const char *message) {
  error_reporter_add_error_with_suggestion(reporter, type, location, message,
                                           NULL);
}

void error_reporter_add_error_with_suggestion(ErrorReporter *reporter,
                                              ErrorType type,
                                              SourceLocation location,
                                              const char *message,
                                              const char *suggestion) {
  SourceSpan span = source_span_from_location(location, 1);
  error_reporter_add_error_with_span_and_suggestion(reporter, type, span,
                                                    message, suggestion);
}

static void error_reporter_add_report(ErrorReporter *reporter,
                                      ErrorSeverity severity, SourceSpan span,
                                      ErrorType type, const char *message,
                                      const char *suggestion) {
  if (!reporter || reporter->count >= reporter->max_errors)
    return;

  if (!error_reporter_expand_capacity(reporter))
    return;

  if (span.length == 0)
    span.length = 1;

  const char *filename =
      span.filename ? span.filename : reporter->current_filename;
  const ErrorReporterSource *source_entry =
      filename ? error_reporter_find_source(reporter, filename) : NULL;
  const char *source_code =
      source_entry ? source_entry->source_code : reporter->current_source_code;

  ErrorReport *error = &reporter->errors[reporter->count];
  error->type = type;
  error->severity = severity;
  error->location = source_location_create(span.line, span.column);
  error->location.filename = filename;
  error->span = span;
  error->span.filename = filename;
  error->filename = filename ? mettle_strdup(filename) : NULL;
  error->source_code = source_code;
  error->message = mettle_strdup(message);
  error->suggestion = suggestion ? mettle_strdup(suggestion) : NULL;
  error->code_snippet =
      error_reporter_get_line_from_source(source_code, span.line);

  reporter->count++;
}

void error_reporter_add_error_with_span(ErrorReporter *reporter, ErrorType type,
                                        SourceSpan span, const char *message) {
  error_reporter_add_error_with_span_and_suggestion(reporter, type, span,
                                                    message, NULL);
}

void error_reporter_add_error_with_span_and_suggestion(
    ErrorReporter *reporter, ErrorType type, SourceSpan span, const char *message,
    const char *suggestion) {
  error_reporter_add_report(reporter, DIAG_SEVERITY_ERROR, span, type, message,
                            suggestion);
}

void error_reporter_add_warning(ErrorReporter *reporter, ErrorType type,
                                SourceLocation location, const char *message) {
  SourceSpan span = source_span_from_location(location, 1);
  error_reporter_add_warning_with_span(reporter, type, span, message);
}

void error_reporter_add_warning_with_span(ErrorReporter *reporter, ErrorType type,
                                          SourceSpan span, const char *message) {
  error_reporter_add_report(reporter, DIAG_SEVERITY_WARNING, span, type,
                            message, NULL);
}

void error_reporter_add_warning_with_suggestion(ErrorReporter *reporter,
                                                ErrorType type,
                                                SourceLocation location,
                                                const char *message,
                                                const char *suggestion) {
  SourceSpan span = source_span_from_location(location, 1);
  error_reporter_add_report(reporter, DIAG_SEVERITY_WARNING, span, type,
                            message, suggestion);
}

void error_reporter_print_errors(ErrorReporter *reporter) {
  if (!reporter)
    return;

  for (size_t i = 0; i < reporter->count; i++) {
    const ErrorReport *e = &reporter->errors[i];
    /* NOTE_OF entries are printed by their parent — skip here */
    if (e->severity == DIAG_SEVERITY_NOTE_OF)
      continue;
    error_reporter_print_error(reporter, e);
    /* Print any immediately-following NOTE_OF entries */
    for (size_t j = i + 1;
         j < reporter->count &&
         reporter->errors[j].severity == DIAG_SEVERITY_NOTE_OF;
         j++) {
      error_reporter_print_error(reporter, &reporter->errors[j]);
    }
    if (i < reporter->count - 1) {
      fprintf(DIAG_STREAM, "\n");
    }
  }
}

/* Truncate a source line for display if it exceeds SNIPPET_MAX_COLS.
   Returns a heap-allocated string; caller must free. */
static char *snippet_truncate(const char *line) {
  if (!line)
    return NULL;
  size_t len = strlen(line);
  if (len <= SNIPPET_MAX_COLS) {
    char *copy = malloc(len + 1);
    if (!copy)
      return NULL;
    memcpy(copy, line, len + 1);
    return copy;
  }
  /* Keep first SNIPPET_MAX_COLS chars and append UTF-8 ellipsis */
  char *buf = malloc(SNIPPET_MAX_COLS + 4); /* 3 bytes for "..." + NUL */
  if (!buf)
    return NULL;
  memcpy(buf, line, SNIPPET_MAX_COLS);
  buf[SNIPPET_MAX_COLS]     = '.';
  buf[SNIPPET_MAX_COLS + 1] = '.';
  buf[SNIPPET_MAX_COLS + 2] = '.';
  buf[SNIPPET_MAX_COLS + 3] = '\0';
  return buf;
}

void error_reporter_print_error(ErrorReporter *reporter,
                                const ErrorReport *error) {
  if (!reporter || !error)
    return;

  const int use_color = error_reporter_should_use_color();
  const char *severity_color = "";
  const char *severity_text = "";
  const char *reset = "";
  const char *help_color = "";

  switch (error->severity) {
  case DIAG_SEVERITY_ERROR:
    severity_color = use_color ? ANSI_COLOR_RED ANSI_BOLD : "";
    severity_text = "error";
    break;
  case DIAG_SEVERITY_WARNING:
    severity_color = use_color ? ANSI_COLOR_YELLOW ANSI_BOLD : "";
    severity_text = "warning";
    break;
  case DIAG_SEVERITY_NOTE:
  case DIAG_SEVERITY_NOTE_OF:
    severity_color = use_color ? ANSI_COLOR_CYAN ANSI_BOLD : "";
    severity_text = "note";
    break;
  default:
    severity_text = "unknown";
    break;
  }

  reset     = use_color ? ANSI_COLOR_RESET : "";
  help_color = use_color ? ANSI_COLOR_CYAN : "";

  /* Header: "error[E0002]: message" */
  fprintf(DIAG_STREAM, "%s%s[%s]%s: %s\n",
          severity_color, severity_text,
          error_type_code(error->type),
          reset, error->message);

  /* Location */
  if (error->severity == DIAG_SEVERITY_NOTE_OF ||
      error->severity == DIAG_SEVERITY_NOTE) {
    /* Notes: indent slightly to show they belong to the parent */
    const char *filename = error->filename ? error->filename : reporter->filename;
    if (filename) {
      fprintf(DIAG_STREAM, "   = %snote%s at %s:%zu:%zu\n",
              help_color, reset,
              filename, error->location.line, error->location.column);
    }
  } else {
    const char *filename = error->filename ? error->filename : reporter->filename;
    if (filename) {
      fprintf(DIAG_STREAM, "  --> %s:%zu:%zu\n", filename,
              error->location.line, error->location.column);
    } else {
      fprintf(DIAG_STREAM, "  --> line %zu, column %zu\n",
              error->location.line, error->location.column);
    }
  }

  /* Code snippet with caret (+ context), skipped for bare notes without span */
  if (error->code_snippet &&
      error->severity != DIAG_SEVERITY_NOTE_OF &&
      error->severity != DIAG_SEVERITY_NOTE) {
    const char *source_code =
        error->source_code ? error->source_code : reporter->source_code;
    const size_t line = error->location.line;
    const size_t prev_line = (line > 1) ? (line - 1) : 0;
    const size_t next_line = line + 1;

    char *prev_raw = prev_line ? error_reporter_get_line_from_source(
                                     source_code, prev_line)
                               : NULL;
    char *next_raw = error_reporter_get_line_from_source(source_code,
                                                         next_line);

    char *prev = snippet_truncate(prev_raw);
    char *snippet = snippet_truncate(error->code_snippet);
    char *next = snippet_truncate(next_raw);
    free(prev_raw);
    free(next_raw);

    size_t max_line_num = next ? next_line : line;
    size_t gutter_width = error_reporter_count_digits(max_line_num);

    fprintf(DIAG_STREAM, "%*s |\n", (int)gutter_width, "");

    if (prev) {
      fprintf(DIAG_STREAM, "%*zu | %s\n", (int)gutter_width, prev_line, prev);
    }

    fprintf(DIAG_STREAM, "%*zu | %s\n", (int)gutter_width, line,
            snippet ? snippet : "");

    size_t caret_len = (error->span.length > 0) ? error->span.length : 1;
    /* Clamp caret so it doesn't extend past truncation point */
    if (error->location.column > 0 &&
        error->location.column - 1 + caret_len > SNIPPET_MAX_COLS) {
      size_t remaining = SNIPPET_MAX_COLS - (error->location.column - 1);
      caret_len = remaining > 0 ? remaining : 1;
    }
    char *caret_line =
        error_reporter_create_caret_line(error->location.column, caret_len);
    if (caret_line) {
      fprintf(DIAG_STREAM, "%*s | %s%s%s\n", (int)gutter_width, "",
              severity_color, caret_line, reset);
      free(caret_line);
    }

    if (next) {
      fprintf(DIAG_STREAM, "%*zu | %s\n", (int)gutter_width, next_line, next);
    }

    free(prev);
    free(snippet);
    free(next);
  }

  /* Suggestion / help line */
  if (error->suggestion) {
    fprintf(DIAG_STREAM, "   = %shelp%s: %s\n", help_color, reset,
            error->suggestion);
  }
}

int error_reporter_has_errors(ErrorReporter *reporter) {
  if (!reporter)
    return 0;

  for (size_t i = 0; i < reporter->count; i++) {
    if (reporter->errors[i].severity == DIAG_SEVERITY_ERROR) {
      return 1;
    }
  }
  return 0;
}

int error_reporter_get_error_count(ErrorReporter *reporter) {
  if (!reporter)
    return 0;

  int count = 0;
  for (size_t i = 0; i < reporter->count; i++) {
    if (reporter->errors[i].severity == DIAG_SEVERITY_ERROR) {
      count++;
    }
  }
  return count;
}

SourceLocation source_location_create(size_t line, size_t column) {
  SourceLocation location;
  location.line = line;
  location.column = column;
  location.filename = NULL;
  return location;
}

char *error_reporter_get_line_from_source(const char *source,
                                          size_t line_number) {
  if (!source || line_number == 0)
    return NULL;

  size_t current_line = 1;
  const char *line_start = source;
  const char *line_end = source;

  while (*line_end && current_line < line_number) {
    if (*line_end == '\n') {
      current_line++;
      line_start = line_end + 1;
    }
    line_end++;
  }

  if (current_line != line_number)
    return NULL;

  while (*line_end && *line_end != '\n') {
    line_end++;
  }

  size_t line_length = line_end - line_start;
  char *line = malloc(line_length + 1);
  if (!line)
    return NULL;

  strncpy(line, line_start, line_length);
  line[line_length] = '\0';

  return line;
}

char *error_reporter_create_caret_line(size_t column, size_t length) {
  if (column == 0)
    return NULL;

  size_t caret_length = (column - 1) + (length > 0 ? length : 1);
  char *caret_line = malloc(caret_length + 1);
  if (!caret_line)
    return NULL;

  for (size_t i = 0; i < column - 1; i++) {
    caret_line[i] = ' ';
  }

  for (size_t i = column - 1; i < column - 1 + (length > 0 ? length : 1); i++) {
    caret_line[i] = '^';
  }

  caret_line[caret_length] = '\0';
  return caret_line;
}

const char *error_reporter_suggest_for_token(const char *token) {
  if (!token)
    return NULL;

  if (strcmp(token, "fucntion") == 0)
    return "did you mean 'function'?";
  if (strcmp(token, "retrun") == 0)
    return "did you mean 'return'?";
  if (strcmp(token, "strcut") == 0)
    return "did you mean 'struct'?";
  if (strcmp(token, "methdo") == 0)
    return "did you mean 'method'?";
  if (strcmp(token, "whiel") == 0)
    return "did you mean 'while'?";
  if (strcmp(token, "fi") == 0)
    return "did you mean 'if'?";
  if (strcmp(token, "itn32") == 0)
    return "did you mean 'int32'?";
  if (strcmp(token, "int23") == 0)
    return "did you mean 'int32'?";
  if (strcmp(token, "stirng") == 0)
    return "did you mean 'string'?";
  if (strcmp(token, "flaot32") == 0)
    return "did you mean 'float32'?";
  if (strcmp(token, "flaot64") == 0)
    return "did you mean 'float64'?";

  return NULL;
}

static char er_lower(char c) {
  if (c >= 'A' && c <= 'Z')
    return (char)(c - 'A' + 'a');
  return c;
}

/* Standard iterative Levenshtein distance (insert/delete/substitute = 1),
   computed case-insensitively so "Print"/"print" count as distance 0.
   Returns SIZE_MAX on allocation failure so callers treat it as "no match". */
size_t error_reporter_edit_distance(const char *a, const char *b) {
  if (!a || !b)
    return (size_t)-1;

  size_t la = strlen(a);
  size_t lb = strlen(b);
  if (la == 0)
    return lb;
  if (lb == 0)
    return la;

  /* Single rolling row of size lb+1. */
  size_t *row = malloc((lb + 1) * sizeof(size_t));
  if (!row)
    return (size_t)-1;

  for (size_t j = 0; j <= lb; j++)
    row[j] = j;

  for (size_t i = 1; i <= la; i++) {
    size_t prev_diag = row[0];
    row[0] = i;
    for (size_t j = 1; j <= lb; j++) {
      size_t prev = row[j];
      size_t cost = (er_lower(a[i - 1]) == er_lower(b[j - 1])) ? 0 : 1;
      size_t del = row[j] + 1;
      size_t ins = row[j - 1] + 1;
      size_t sub = prev_diag + cost;
      size_t best = del < ins ? del : ins;
      if (sub < best)
        best = sub;
      row[j] = best;
      prev_diag = prev;
    }
  }

  size_t result = row[lb];
  free(row);
  return result;
}

char *error_reporter_closest_candidate(const char *name,
                                       const char *const *candidates,
                                       size_t count) {
  if (!name || !candidates || count == 0)
    return NULL;

  size_t name_len = strlen(name);
  if (name_len == 0)
    return NULL;

  /* Tolerate roughly one third of the name being wrong, with a floor of 1
     (catches single-character typos in short names like "i"/"j") and a cap
     so unrelated long names don't get spuriously matched. */
  size_t threshold = name_len / 3;
  if (threshold < 1)
    threshold = 1;
  if (threshold > 3)
    threshold = 3;

  const char *best = NULL;
  size_t best_distance = (size_t)-1;

  for (size_t i = 0; i < count; i++) {
    const char *cand = candidates[i];
    if (!cand || cand[0] == '\0')
      continue;
    if (strcmp(cand, name) == 0)
      continue; /* exact match isn't a useful "did you mean" */

    size_t d = error_reporter_edit_distance(name, cand);
    if (d <= threshold && d < best_distance) {
      best_distance = d;
      best = cand;
    }
  }

  return best ? mettle_strdup(best) : NULL;
}

/* Returns a heap-allocated suggestion string the caller must free, or NULL. */
char *error_reporter_suggest_for_type_mismatch(const char *expected,
                                               const char *actual) {
  if (!expected || !actual)
    return NULL;

  char buf[256];

  /* Suggest explicit casting between int32/int64 */
  if ((strcmp(expected, "int32") == 0 && strcmp(actual, "int64") == 0) ||
      (strcmp(expected, "int64") == 0 && strcmp(actual, "int32") == 0)) {
    snprintf(buf, sizeof(buf), "consider explicit casting: (%s)value", expected);
    return mettle_strdup(buf);
  }

  /* Suggest string literal for string type */
  if (strcmp(expected, "string") == 0 && strcmp(actual, "int32") == 0) {
    return mettle_strdup("use string literal with double quotes: \"value\"");
  }

  /* Suggest numeric literal for numeric types */
  if (strstr(expected, "int") && strcmp(actual, "string") == 0) {
    return mettle_strdup("use numeric literal without quotes: 42");
  }

  return NULL;
}
