#include "semantic/ownership_internal.h"
#include "parser/ast_print.h"
#include <stdlib.h>
#include <string.h>

static const char *own_kind_word(OwnRecordKind kind) {
  switch (kind) {
  case OWN_RECORD_SCOPE_DROP:
    return "scope-drop";
  case OWN_RECORD_PARAM_DROP:
    return "parameter-drop";
  case OWN_RECORD_ARM_DROP:
    return "arm-drop";
  case OWN_RECORD_ASSIGN_DROP:
    return "assign-drop";
  case OWN_RECORD_DISCARD_DROP:
    return "discard-drop";
  case OWN_RECORD_TEMPORARY:
    return "temporary";
  case OWN_RECORD_GLUE:
    return "glue";
  case OWN_RECORD_MOVE:
    return "move";
  case OWN_RECORD_LIVE_BIT:
    return "live-bit";
  case OWN_RECORD_DROP_RUN:
    return "drop-runs";
  default:
    return "?";
  }
}

static const char *own_base_name(const char *path) {
  const char *slash = NULL;
  if (!path) {
    return "?";
  }
  slash = strrchr(path, '/');
  if (!slash) {
    slash = strrchr(path, '\\');
  }
  return slash ? slash + 1 : path;
}

static int own_record_in_focus(const OwnRecord *record, const char *focus) {
  if (!focus) {
    return 1;
  }
  return record->at.filename &&
         strcmp(own_base_name(record->at.filename), own_base_name(focus)) == 0;
}

static const char *g_report_focus = NULL;

void ownership_set_report_focus(const char *focus) { g_report_focus = focus; }

void ownership_print_report(FILE *out) {
  size_t shown = 0;
  const char *function = NULL;
  for (size_t i = 0; i < ownership_record_count(); i++) {
    const OwnRecord *r = ownership_record_at(i);
    if (!own_record_in_focus(r, g_report_focus) ||
        r->kind == OWN_RECORD_DROP_RUN) {
      continue;
    }
    if (shown == 0) {
      fprintf(out, "\nownership report (drops and moves the compiler "
                   "inserted or followed)\n");
    }
    if (!function || !r->function || strcmp(function, r->function) != 0) {
      function = r->function;
      fprintf(out, "  in %s\n", function ? function : "?");
    }
    fprintf(out, "    line %zu: %s%s%s%s: %s\n", r->at.line,
            own_kind_word(r->kind), r->subject ? " `" : "",
            r->subject ? r->subject : "", r->subject ? "`" : "",
            r->reason ? r->reason : "");
    if (r->text) {
      fprintf(out, "      written out: %s\n", r->text);
    }
    if (r->compiled_to) {
      fprintf(out, "      compiled to: %s\n", r->compiled_to);
    }
    shown++;
  }
}

static void own_json_string(FILE *out, const char *text) {
  fputc('"', out);
  for (const char *p = text ? text : ""; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') {
      fputc('\\', out);
      fputc((int)c, out);
    } else if (c == '\n') {
      fputs("\\n", out);
    } else if (c < 0x20) {
      fprintf(out, "\\u%04x", c);
    } else {
      fputc((int)c, out);
    }
  }
  fputc('"', out);
}

static void own_json_record(FILE *out, const OwnRecord *r) {
  fputs("{\"kind\":", out);
  own_json_string(out, own_kind_word(r->kind));
  fputs(",\"file\":", out);
  own_json_string(out, r->at.filename);
  fprintf(out, ",\"line\":%zu,\"column\":%zu,\"origin_line\":%zu",
          r->at.line, r->at.column, r->origin.line);
  fputs(",\"function\":", out);
  own_json_string(out, r->function);
  fputs(",\"subject\":", out);
  own_json_string(out, r->subject);
  fputs(",\"type\":", out);
  own_json_string(out, r->type_name);
  fputs(",\"text\":", out);
  own_json_string(out, r->text);
  fputs(",\"reason\":", out);
  own_json_string(out, r->reason);
  fputs(",\"compiled_to\":", out);
  own_json_string(out, r->compiled_to);
  fprintf(out, ",\"inserted\":%s}", r->inserted ? "true" : "false");
}

void ownership_print_json(FILE *out, int leading_comma) {
  size_t shown = 0;
  fputs(leading_comma ? ",\"ownership\":[" : "\"ownership\":[", out);
  for (size_t i = 0; i < ownership_record_count(); i++) {
    const OwnRecord *r = ownership_record_at(i);
    if (!own_record_in_focus(r, g_report_focus)) {
      continue;
    }
    if (shown++ > 0) {
      fputc(',', out);
    }
    own_json_record(out, r);
  }
  fputc(']', out);
}

typedef struct {
  size_t start;
  size_t end;
  char *text;
  size_t line;
} OwnEdit;

static char *own_read_file(const char *path, size_t *size) {
  FILE *in = fopen(path, "rb");
  char *text = NULL;
  long length = 0;
  if (!in) {
    return NULL;
  }
  fseek(in, 0, SEEK_END);
  length = ftell(in);
  fseek(in, 0, SEEK_SET);
  text = malloc((size_t)(length > 0 ? length : 0) + 1);
  if (text && length > 0 &&
      fread(text, 1, (size_t)length, in) != (size_t)length) {
    free(text);
    text = NULL;
  }
  fclose(in);
  if (text) {
    text[length > 0 ? length : 0] = '\0';
    *size = (size_t)(length > 0 ? length : 0);
  }
  return text;
}

static size_t own_offset_of(const char *text, size_t size, size_t line,
                            size_t column) {
  size_t at = 0;
  size_t current = 1;
  while (at < size && current < line) {
    if (text[at++] == '\n') {
      current++;
    }
  }
  return at + (column > 0 ? column - 1 : 0) <= size
             ? at + (column > 0 ? column - 1 : 0)
             : size;
}

static size_t own_line_start(const char *text, size_t at) {
  while (at > 0 && text[at - 1] != '\n') {
    at--;
  }
  return at;
}

static size_t own_skip_quoted(const char *text, size_t size, size_t at) {
  char quote = text[at++];
  while (at < size && text[at] != quote) {
    at += text[at] == '\\' ? 2 : 1;
  }
  return at;
}

static size_t own_statement_end(const char *text, size_t size, size_t at) {
  int depth = 0;
  for (; at < size; at++) {
    char c = text[at];
    if (c == '"' || c == '\'') {
      at = own_skip_quoted(text, size, at);
    } else if (c == '/' && at + 1 < size && text[at + 1] == '/') {
      while (at < size && text[at] != '\n') {
        at++;
      }
    } else if (c == '(' || c == '[' || c == '{') {
      depth++;
    } else if (c == ')' || c == ']' || c == '}') {
      depth--;
    } else if (c == ';' && depth <= 0) {
      return at;
    }
  }
  return size;
}

static char *own_indent_of(const char *text, size_t line_start) {
  size_t end = line_start;
  while (text[end] == ' ' || text[end] == '\t') {
    end++;
  }
  char *indent = malloc(end - line_start + 1);
  if (indent) {
    memcpy(indent, text + line_start, end - line_start);
    indent[end - line_start] = '\0';
  }
  return indent;
}

static char *own_join3(const char *a, const char *b, const char *c) {
  size_t length = strlen(a) + strlen(b) + strlen(c) + 1;
  char *joined = malloc(length);
  if (joined) {
    snprintf(joined, length, "%s%s%s", a, b, c);
  }
  return joined;
}

static size_t own_skip_space(const char *text, size_t size, size_t at) {
  while (at < size && (text[at] == ' ' || text[at] == '\t')) {
    at++;
  }
  return at;
}

static size_t own_trim_end(const char *text, size_t start, size_t end) {
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\n' || text[end - 1] == '\r')) {
    end--;
  }
  return end;
}

static size_t own_assign_equals(const char *text, size_t start, size_t end) {
  int depth = 0;
  for (size_t at = start; at < end; at++) {
    char c = text[at];
    char next = at + 1 < end ? text[at + 1] : '\0';
    char prev = at > start ? text[at - 1] : '\0';
    if (c == '"' || c == '\'') {
      at = own_skip_quoted(text, end, at);
    } else if (c == '(' || c == '[' || c == '{') {
      depth++;
    } else if (c == ')' || c == ']' || c == '}') {
      depth--;
    } else if (c == '=' && depth == 0 && next != '=' && prev != '=' &&
               prev != '!' && prev != '<' && prev != '>') {
      return at;
    }
  }
  return end;
}

static char *own_slice(const char *text, size_t start, size_t end) {
  char *out = malloc(end - start + 1);
  if (out) {
    memcpy(out, text + start, end - start);
    out[end - start] = '\0';
  }
  return out;
}

static int own_edit_hold(const char *text, size_t size, const OwnFix *fix,
                         const char *indent, size_t line_start,
                         OwnEdit *edits, size_t *count) {
  size_t start = own_skip_space(text, size, line_start);
  size_t end = own_statement_end(text, size, start);
  size_t equals = own_assign_equals(text, start, end);
  char *target = NULL;
  char *value = NULL;
  char *replacement = NULL;
  size_t length = 0;
  if (equals >= end) {
    return 0;
  }
  target = own_slice(text, start, own_trim_end(text, start, equals));
  value = own_slice(text, own_skip_space(text, size, equals + 1),
                    own_trim_end(text, equals + 1, end));
  length = strlen(target) * 2 + strlen(value) + strlen(indent) * 2 +
           strlen(fix->text) * 2 + 64;
  replacement = malloc(length);
  if (target && value && replacement) {
    snprintf(replacement, length, "var %s = %s;\n%sdrop(%s);\n%s%s = %s",
             fix->text, value, indent, target, indent, target, fix->text);
    edits[(*count)++] = (OwnEdit){start, end, replacement, fix->line};
    replacement = NULL;
  }
  free(target);
  free(value);
  free(replacement);
  return 1;
}

static int own_is_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static size_t own_call_start(const char *text, size_t at) {
  if (text[at] != '(') {
    return at;
  }
  while (at > 0 && own_is_name_char(text[at - 1])) {
    at--;
  }
  return at;
}

static size_t own_call_end(const char *text, size_t size, size_t at) {
  int depth = 0;
  while (at < size && ((text[at] >= 'a' && text[at] <= 'z') ||
                       (text[at] >= 'A' && text[at] <= 'Z') ||
                       (text[at] >= '0' && text[at] <= '9') ||
                       text[at] == '_')) {
    at++;
  }
  if (at >= size || text[at] != '(') {
    return 0;
  }
  for (; at < size; at++) {
    if (text[at] == '"' || text[at] == '\'') {
      at = own_skip_quoted(text, size, at);
    } else if (text[at] == '(') {
      depth++;
    } else if (text[at] == ')' && --depth == 0) {
      return at + 1;
    }
  }
  return 0;
}

static int own_edit_temporary(const char *text, size_t size,
                              const OwnFix *fix, OwnEdit *edits,
                              size_t *count) {
  size_t call =
      own_call_start(text, own_offset_of(text, size, fix->line, fix->column));
  size_t call_end = own_call_end(text, size, call);
  size_t statement = own_offset_of(text, size, fix->statement_line,
                                   fix->statement_column);
  size_t line_start = own_line_start(text, statement);
  char *indent = own_indent_of(text, line_start);
  char *held = NULL;
  char *declare = NULL;
  size_t length = 0;
  if (!indent || call_end == 0) {
    free(indent);
    return 0;
  }
  held = own_slice(text, call, call_end);
  length = strlen(indent) * 2 + strlen(fix->text) * 3 + strlen(held) + 64;
  declare = malloc(length);
  if (held && declare) {
    if (fix->statement_returns) {
      snprintf(declare, length, "%svar %s = %s;\n%sdefer drop(%s);\n", indent,
               fix->text, held, indent, fix->text);
    } else {
      size_t end = own_statement_end(text, size, statement);
      char *after = malloc(length);
      snprintf(declare, length, "%svar %s = %s;\n", indent, fix->text, held);
      if (after) {
        snprintf(after, length, "\n%sdrop(%s);", indent, fix->text);
        edits[(*count)++] = (OwnEdit){end + 1, end + 1, after, fix->line};
      }
    }
    edits[(*count)++] = (OwnEdit){line_start, line_start, declare, fix->line};
    declare = NULL;
    edits[(*count)++] = (OwnEdit){call, call_end,
                                  own_join3(fix->text, "", ""), fix->line};
  }
  free(held);
  free(declare);
  free(indent);
  return 1;
}

static int own_edit_for(const char *text, size_t size, const OwnFix *fix,
                        OwnEdit *edits, size_t *count) {
  if (fix->kind == OWN_FIX_TEMPORARY) {
    return own_edit_temporary(text, size, fix, edits, count);
  }
  size_t at = own_offset_of(text, size, fix->line, fix->column);
  size_t line_start = own_line_start(text, at);
  char *indent = own_indent_of(text, line_start);
  if (!indent) {
    return 0;
  }
  if (fix->kind == OWN_FIX_INSERT_AFTER) {
    size_t end = own_statement_end(text, size, at);
    char *prefix = own_join3("\n", indent, "");
    edits[*count] = (OwnEdit){end + 1, end + 1,
                              own_join3(prefix, fix->text, ""), fix->line};
    free(prefix);
    (*count)++;
  } else if (fix->kind == OWN_FIX_INSERT_BEFORE) {
    edits[*count] = (OwnEdit){line_start, line_start,
                              own_join3(indent, fix->text, "\n"), fix->line};
    (*count)++;
  } else if (fix->kind == OWN_FIX_WRAP_DROP) {
    size_t end = own_statement_end(text, size, at);
    at = own_call_start(text, at);
    edits[(*count)++] = (OwnEdit){end, end, own_join3(")", "", ""),
                                  fix->line};
    edits[(*count)++] = (OwnEdit){at, at, own_join3("drop(", "", ""),
                                  fix->line};
  } else if (fix->kind == OWN_FIX_ASSIGN_HOLD) {
    own_edit_hold(text, size, fix, indent, line_start, edits, count);
  }
  free(indent);
  return 1;
}

static void own_report_fix(FILE *report, const OwnFix *fix) {
  switch (fix->kind) {
  case OWN_FIX_INSERT_AFTER:
    fprintf(report, "fix: %s:%zu: added `%s` after this line\n", fix->file,
            fix->line, fix->text);
    return;
  case OWN_FIX_INSERT_BEFORE:
    fprintf(report, "fix: %s:%zu: added `%s` before this line\n", fix->file,
            fix->line, fix->text);
    return;
  case OWN_FIX_WRAP_DROP:
    fprintf(report, "fix: %s:%zu: wrapped the discarded value in drop(...)\n",
            fix->file, fix->line);
    return;
  case OWN_FIX_ASSIGN_HOLD:
    fprintf(report,
            "fix: %s:%zu: the new value is computed into `%s`, the old one "
            "is dropped, then the new one is stored\n",
            fix->file, fix->line, fix->text);
    return;
  case OWN_FIX_TEMPORARY:
    fprintf(report,
            "fix: %s:%zu: the temporary is held in `%s` and dropped when the "
            "statement ends\n",
            fix->file, fix->line, fix->text);
    return;
  default:
    return;
  }
}

static int own_edit_compare(const void *a, const void *b) {
  const OwnEdit *x = (const OwnEdit *)a;
  const OwnEdit *y = (const OwnEdit *)b;
  if (x->start != y->start) {
    return x->start < y->start ? 1 : -1;
  }
  return 0;
}

static int own_same_file(const char *a, const char *b) {
  if (!a || !b) {
    return 0;
  }
  for (; *a && *b; a++, b++) {
    char x = *a == '\\' ? '/' : *a;
    char y = *b == '\\' ? '/' : *b;
    if (x != y) {
      return 0;
    }
  }
  return *a == *b;
}

static void own_write_edited(FILE *out, const char *text, size_t size,
                             OwnEdit *edits, size_t count) {
  size_t cursor = 0;
  for (size_t i = count; i > 0; i--) {
    OwnEdit *edit = &edits[i - 1];
    if (edit->start < cursor) {
      continue;
    }
    fwrite(text + cursor, 1, edit->start - cursor, out);
    fputs(edit->text ? edit->text : "", out);
    cursor = edit->end;
  }
  fwrite(text + cursor, 1, size - cursor, out);
}

static size_t own_collect_edits(const char *path, const char *text,
                                size_t size, OwnEdit **out_edits) {
  OwnEdit *edits = calloc(own_fix_count() * 3 + 1, sizeof(*edits));
  size_t count = 0;
  for (size_t i = 0; edits && i < own_fix_count(); i++) {
    OwnFix *fix = own_fix_at(i);
    if (own_same_file(fix->file, path) && fix->text) {
      own_edit_for(text, size, fix, edits, &count);
    }
  }
  qsort(edits, count, sizeof(*edits), own_edit_compare);
  *out_edits = edits;
  return count;
}

static void own_free_edits(OwnEdit *edits, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(edits[i].text);
  }
  free(edits);
}

static int own_apply_file(const char *path, FILE *report) {
  size_t size = 0;
  char *text = own_read_file(path, &size);
  OwnEdit *edits = NULL;
  size_t count = 0;
  FILE *out = NULL;
  if (!text) {
    return 0;
  }
  count = own_collect_edits(path, text, size, &edits);
  out = count > 0 ? fopen(path, "wb") : NULL;
  if (out) {
    own_write_edited(out, text, size, edits, count);
    fclose(out);
    for (size_t i = 0; report && i < own_fix_count(); i++) {
      OwnFix *fix = own_fix_at(i);
      if (own_same_file(fix->file, path) && fix->text) {
        own_report_fix(report, fix);
      }
    }
  }
  own_free_edits(edits, count);
  free(text);
  return out ? (int)count : 0;
}

int ownership_print_fixed_source(FILE *out, const char *path) {
  size_t size = 0;
  char *text = own_read_file(path, &size);
  OwnEdit *edits = NULL;
  size_t count = 0;
  if (!text) {
    return 0;
  }
  count = own_collect_edits(path, text, size, &edits);
  own_write_edited(out, text, size, edits, count);
  own_free_edits(edits, count);
  free(text);
  return 1;
}

static const char *own_fix_kind_word(OwnFixKind kind) {
  switch (kind) {
  case OWN_FIX_INSERT_AFTER:
    return "insert-after";
  case OWN_FIX_INSERT_BEFORE:
    return "insert-before";
  case OWN_FIX_WRAP_DROP:
    return "wrap-drop";
  case OWN_FIX_ASSIGN_HOLD:
    return "assign-hold";
  case OWN_FIX_TEMPORARY:
    return "temporary";
  default:
    return "replace";
  }
}

void ownership_print_editor_json(FILE *out) {
  size_t shown = 0;
  fputc('{', out);
  ownership_print_json(out, 0);
  fputs(",\"fixes\":[", out);
  for (size_t i = 0; i < own_fix_count(); i++) {
    const OwnFix *fix = own_fix_at(i);
    if (!fix->text) {
      continue;
    }
    if (shown++ > 0) {
      fputc(',', out);
    }
    fputs("{\"kind\":", out);
    own_json_string(out, own_fix_kind_word(fix->kind));
    fputs(",\"file\":", out);
    own_json_string(out, fix->file);
    fprintf(out, ",\"line\":%zu,\"column\":%zu,\"text\":", fix->line,
            fix->column);
    own_json_string(out, fix->text);
    fputc('}', out);
  }
  fputs("]}\n", out);
}

int ownership_write_fixes(FILE *report) {
  int applied = 0;
  for (size_t i = 0; i < own_fix_count(); i++) {
    OwnFix *fix = own_fix_at(i);
    int seen = 0;
    for (size_t j = 0; j < i; j++) {
      if (own_same_file(own_fix_at(j)->file, fix->file)) {
        seen = 1;
        break;
      }
    }
    if (!seen) {
      applied += own_apply_file(fix->file, report);
    }
  }
  return applied;
}
