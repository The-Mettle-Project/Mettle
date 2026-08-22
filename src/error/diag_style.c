#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "diag_style.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#ifndef isatty
#define isatty _isatty
#endif
#ifndef fileno
#define fileno _fileno
#endif
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define DIAG_STREAM stderr

#define DIAG_COLUMNS_MIN 60u
#define DIAG_COLUMNS_MAX 110u
#define DIAG_COLUMNS_FALLBACK 90u

#ifdef _WIN32
static void diag_enable_vt(void) {
  static int done = 0;
  if (done) {
    return;
  }
  done = 1;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD mode = 0;
  if (GetConsoleMode(h, &mode)) {
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
}
#endif

static int diag_stderr_is_tty(void) {
  int fd = fileno(DIAG_STREAM);
  return fd >= 0 && isatty(fd);
}

int diag_style_color(void) {
  static int cached = -1;
  if (cached >= 0) {
    return cached;
  }

  const char *force = getenv("CLICOLOR_FORCE");
  if (force && force[0] != '\0' && strcmp(force, "0") != 0) {
#ifdef _WIN32
    diag_enable_vt();
#endif
    cached = 1;
    return cached;
  }

  const char *no_color = getenv("NO_COLOR");
  if (no_color && no_color[0] != '\0') {
    cached = 0;
    return cached;
  }

  const char *term = getenv("TERM");
  if (term && strcmp(term, "dumb") == 0) {
    cached = 0;
    return cached;
  }

  const char *clicolor = getenv("CLICOLOR");
  if (clicolor && strcmp(clicolor, "0") == 0) {
    cached = 0;
    return cached;
  }

  if (!diag_stderr_is_tty()) {
    cached = 0;
    return cached;
  }

#ifdef _WIN32
  diag_enable_vt();
#endif
  cached = 1;
  return cached;
}

int diag_style_unicode(void) {
  static int cached = -1;
  if (cached >= 0) {
    return cached;
  }

  const char *forced = getenv("METTLE_DIAG_UNICODE");
  if (forced && forced[0]) {
    cached = (forced[0] != '0');
    return cached;
  }

  if (!diag_stderr_is_tty()) {
    cached = 0;
    return cached;
  }

#ifdef _WIN32
  cached = (GetConsoleOutputCP() == CP_UTF8) ? 1 : 0;
#else
  {
    const char *locale = getenv("LC_ALL");
    if (!locale || !locale[0]) {
      locale = getenv("LC_CTYPE");
    }
    if (!locale || !locale[0]) {
      locale = getenv("LANG");
    }
    cached = (locale && (strstr(locale, "UTF-8") || strstr(locale, "utf8") ||
                         strstr(locale, "UTF8")))
                 ? 1
                 : 0;
  }
#endif
  return cached;
}

int diag_style_wrap(void) {
  return diag_stderr_is_tty();
}

size_t diag_style_columns(void) {
  static size_t cached = 0;
  if (cached) {
    return cached;
  }

  const char *forced = getenv("METTLE_DIAG_COLUMNS");
  if (!forced || !forced[0]) {
    forced = getenv("METTLE_EXPLAIN_COLUMNS");
  }
  if (forced && forced[0]) {
    size_t value = 0;
    const char *p = forced;
    while (*p >= '0' && *p <= '9' && value < 100000) {
      value = value * 10 + (size_t)(*p - '0');
      p++;
    }
    if (value >= DIAG_COLUMNS_MIN) {
      cached = value;
      return cached;
    }
  }

  size_t detected = 0;
#ifdef _WIN32
  {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
      int width = info.srWindow.Right - info.srWindow.Left + 1;
      if (width > 0) {
        detected = (size_t)width;
      }
    }
  }
#else
  {
    struct winsize ws;
    if (ioctl(fileno(DIAG_STREAM), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
      detected = (size_t)ws.ws_col;
    }
  }
#endif

  if (!detected) {
    detected = DIAG_COLUMNS_FALLBACK;
  }

  if (detected > 1) {
    detected--;
  }
  if (detected < DIAG_COLUMNS_MIN) {
    detected = DIAG_COLUMNS_MIN;
  }
  if (detected > DIAG_COLUMNS_MAX) {
    detected = DIAG_COLUMNS_MAX;
  }
  cached = detected;
  return cached;
}

#define DIAG_SGR(name, seq)                                                    \
  const char *diag_sgr_##name(void) {                                          \
    return diag_style_color() ? seq : "";                                      \
  }

DIAG_SGR(reset, "\x1b[0m")
DIAG_SGR(bold, "\x1b[1m")
DIAG_SGR(dim, "\x1b[2m")
DIAG_SGR(red, "\x1b[31m")
DIAG_SGR(green, "\x1b[32m")
DIAG_SGR(yellow, "\x1b[33m")
DIAG_SGR(blue, "\x1b[34m")
DIAG_SGR(magenta, "\x1b[35m")
DIAG_SGR(cyan, "\x1b[36m")
DIAG_SGR(error, "\x1b[1;31m")
DIAG_SGR(warning, "\x1b[1;33m")
DIAG_SGR(note, "\x1b[1;36m")

#undef DIAG_SGR

static const DiagGlyphs g_unicode_glyphs = {
    "\xE2\x94\x80",
    "\xE2\x94\x82",
    "\xC2\xB7",
    "\xE2\x94\xAC",
    "\xE2\x94\xBC",
    "\xE2\x94\xB4",
    "\xE2\x94\x94\xE2\x94\x80",
    "\xE2\x86\x92",
    "\xC2\xB7",
};

static const DiagGlyphs g_ascii_glyphs = {
    "-", "|", ":", "+", "+", "+", "\\_", "->", "-",
};

const DiagGlyphs *diag_glyphs(void) {
  return diag_style_unicode() ? &g_unicode_glyphs : &g_ascii_glyphs;
}

size_t diag_visible_width(const char *s) {
  if (!s) {
    return 0;
  }
  size_t width = 0;
  while (*s) {
    if (*s == '\x1b') {
      s++;
      if (*s == '[') {
        s++;
        while (*s && *s != 'm') {
          s++;
        }
        if (*s) {
          s++;
        }
      }
      continue;
    }

    if (((unsigned char)*s & 0xC0) != 0x80) {
      width++;
    }
    s++;
  }
  return width;
}

void diag_repeat(FILE *out, const char *glyph, size_t n) {
  if (!out || !glyph) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    fputs(glyph, out);
  }
}

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} DiagBuf;

static void db_puts(DiagBuf *b, const char *s) {
  if (!s || !b->cap) {
    return;
  }
  size_t n = strlen(s);
  size_t room = (b->cap > b->len + 1) ? b->cap - b->len - 1 : 0;
  if (n > room) {
    n = room;
  }
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

static void db_write(DiagBuf *b, const char *s, size_t n) {
  if (!s || !b->cap) {
    return;
  }
  size_t room = (b->cap > b->len + 1) ? b->cap - b->len - 1 : 0;
  if (n > room) {
    n = room;
  }
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

static void db_repeat(DiagBuf *b, const char *s, size_t n) {
  while (n--) {
    db_puts(b, s);
  }
}

size_t diag_rule_into(char *buf, size_t cap, size_t indent, const char *label,
                      const char *label_sgr) {
  if (!buf || !cap) {
    return 0;
  }
  buf[0] = '\0';
  DiagBuf b = {buf, cap, 0};
  const DiagGlyphs *g = diag_glyphs();
  const char *dim = diag_sgr_dim();
  const char *reset = diag_sgr_reset();
  size_t columns = diag_style_columns();

  db_repeat(&b, " ", indent);
  size_t drawn = indent;

  db_puts(&b, dim);
  db_repeat(&b, g->h, 2);
  drawn += 2;
  if (!label || !label[0]) {
    if (columns > drawn) {
      db_repeat(&b, g->h, columns - drawn);
    }
    db_puts(&b, reset);
    return b.len;
  }

  {
    db_puts(&b, " ");
    drawn++;
    db_puts(&b, reset);
    db_puts(&b, label_sgr ? label_sgr : "");
    db_puts(&b, label);
    db_puts(&b, reset);
    drawn += diag_visible_width(label);

    if (columns > drawn + 1) {
      db_puts(&b, dim);
      db_puts(&b, " ");
      drawn++;
      if (columns > drawn) {
        db_repeat(&b, g->h, columns - drawn);
      }
      db_puts(&b, reset);
      return b.len;
    }
  }

  if (columns > drawn) {
    db_puts(&b, dim);
    db_repeat(&b, g->h, columns - drawn);
  }
  db_puts(&b, reset);
  return b.len;
}

size_t diag_rule_junction_into(char *buf, size_t cap, size_t indent,
                               size_t gutter, const char *junction) {
  if (!buf || !cap) {
    return 0;
  }
  buf[0] = '\0';
  DiagBuf b = {buf, cap, 0};
  const DiagGlyphs *g = diag_glyphs();
  size_t columns = diag_style_columns();

  db_repeat(&b, " ", indent);
  db_puts(&b, diag_sgr_dim());
  db_repeat(&b, g->h, gutter + 1);
  db_puts(&b, junction);

  size_t drawn = indent + gutter + 2;
  if (columns > drawn) {
    db_repeat(&b, g->h, columns - drawn);
  }
  db_puts(&b, diag_sgr_reset());
  return b.len;
}

void diag_rule(FILE *out, size_t indent, const char *label,
               const char *label_sgr) {
  if (!out) {
    return;
  }
  char line[2048];
  diag_rule_into(line, sizeof(line), indent, label, label_sgr);
  fputs(line, out);
  fputc('\n', out);
}

void diag_rule_junction(FILE *out, size_t indent, size_t gutter,
                        const char *junction) {
  if (!out) {
    return;
  }
  char line[2048];
  diag_rule_junction_into(line, sizeof(line), indent, gutter, junction);
  fputs(line, out);
  fputc('\n', out);
}

void diag_wrap(FILE *out, const char *first_prefix, size_t first_width,
               const char *text, const char *text_sgr) {
  if (!out || !text) {
    return;
  }
  const char *reset = diag_sgr_reset();
  const char *sgr = text_sgr ? text_sgr : "";

  if (!diag_style_wrap()) {
    fprintf(out, "%s%s%s%s" "%c", first_prefix ? first_prefix : "", sgr, text,
            reset, 10);
    return;
  }

  size_t columns = diag_style_columns();
  size_t body = (columns > first_width + 8) ? columns - first_width : 8;

  const char *p = text;
  int first = 1;
  while (*p) {

    const char *nl = strchr(p, '\n');
    const char *limit = nl ? nl : p + strlen(p);

    while (p < limit) {
      size_t remaining = (size_t)(limit - p);
      size_t take = remaining;
      if (take > body) {
        take = body;

        size_t cut = take;
        while (cut > 0 && p[cut] != ' ') {
          cut--;
        }
        if (cut > 0) {
          take = cut;
        }
      }

      if (first) {
        fputs(first_prefix ? first_prefix : "", out);
        first = 0;
      } else {
        for (size_t i = 0; i < first_width; i++) {
          fputc(' ', out);
        }
      }
      fprintf(out, "%s%.*s%s\n", sgr, (int)take, p, reset);

      p += take;
      while (p < limit && *p == ' ') {
        p++;
      }
    }

    if (nl) {
      p = nl + 1;
    } else {
      break;
    }
  }
}

static const char *const g_control_keywords[] = {
    "fn",      "if",       "else",     "for",      "while",    "return",
    "break",   "continue", "switch",   "case",     "default",  "match",
    "defer",   "errdefer", "import",   "import_str", "export",  "extern",
    "struct",  "enum",     "trait",    "impl",     "where",    "method",
    "var",     "const",    "new",      "this",     "private",  "asm",
    "kernel",  "dispatch", "barrier",  "workgroup", "comptime", "test",
    "true",    "false",    "null",     "sizeof",   "as",       NULL};

static const char *const g_type_keywords[] = {
    "int8",    "int16",   "int32",   "int64",  "uint8",  "uint16",
    "uint32",  "uint64",  "float32", "float64", "string", "char",
    "bool",    "void",    "rawptr",  "cstr",   NULL};

static int diag_ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static int diag_word_in(const char *const *table, const char *word,
                        size_t len) {
  for (size_t i = 0; table[i]; i++) {
    if (strlen(table[i]) == len && strncmp(table[i], word, len) == 0) {
      return 1;
    }
  }
  return 0;
}

size_t diag_source_into(char *buf, size_t cap, const char *text) {
  if (!buf || !cap) {
    return 0;
  }
  buf[0] = '\0';
  if (!text) {
    return 0;
  }
  DiagBuf b = {buf, cap, 0};
  if (!diag_style_color()) {
    db_puts(&b, text);
    return b.len;
  }

  const char *reset = diag_sgr_reset();
  const char *p = text;

  while (*p) {

    if (p[0] == '/' && (p[1] == '/' || p[1] == '*')) {
      db_puts(&b, diag_sgr_dim());
      db_puts(&b, p);
      db_puts(&b, reset);
      return b.len;
    }

    if (*p == '"' || *p == '\'') {
      char quote = *p;
      const char *start = p++;
      while (*p && *p != quote) {
        if (*p == '\\' && p[1]) {
          p++;
        }
        p++;
      }
      if (*p) {
        p++;
      }
      db_puts(&b, diag_sgr_green());
      db_write(&b, start, (size_t)(p - start));
      db_puts(&b, reset);
      continue;
    }

    if (*p == '@') {
      const char *start = p++;
      while (diag_ident_char(*p)) {
        p++;
      }
      if (*p == '!') {
        p++;
      }
      db_puts(&b, diag_sgr_blue());
      db_write(&b, start, (size_t)(p - start));
      db_puts(&b, reset);
      continue;
    }

    if (*p >= '0' && *p <= '9') {
      const char *start = p;
      while (diag_ident_char(*p) || (*p == '.' && p[1] >= '0' && p[1] <= '9')) {
        p++;
      }
      db_puts(&b, diag_sgr_yellow());
      db_write(&b, start, (size_t)(p - start));
      db_puts(&b, reset);
      continue;
    }

    if (diag_ident_char(*p)) {
      const char *start = p;
      while (diag_ident_char(*p)) {
        p++;
      }
      size_t len = (size_t)(p - start);
      const char *color = NULL;
      if (diag_word_in(g_control_keywords, start, len)) {
        color = diag_sgr_magenta();
      } else if (diag_word_in(g_type_keywords, start, len)) {
        color = diag_sgr_cyan();
      }
      if (color) {
        db_puts(&b, color);
        db_write(&b, start, len);
        db_puts(&b, reset);
      } else {
        db_write(&b, start, len);
      }
      continue;
    }

    db_write(&b, p, 1);
    p++;
  }
  return b.len;
}

void diag_write_source(FILE *out, const char *text) {
  if (!out || !text) {
    return;
  }
  if (!diag_style_color()) {
    fputs(text, out);
    return;
  }

  size_t cap = strlen(text) * 12 + 64;
  char *buf = malloc(cap);
  if (!buf) {
    fputs(text, out);
    return;
  }
  diag_source_into(buf, cap, text);
  fputs(buf, out);
  free(buf);
}

size_t diag_expand_tabs(const char *in, char *out, size_t cap,
                        size_t tab_width) {
  if (!out || cap == 0) {
    return 0;
  }
  if (!in) {
    out[0] = '\0';
    return 0;
  }
  if (tab_width == 0) {
    tab_width = 1;
  }

  size_t written = 0;
  for (const char *p = in; *p; p++) {
    if (*p == '\t') {
      size_t pad = tab_width - (written % tab_width);
      while (pad-- > 0 && written + 1 < cap) {
        out[written++] = ' ';
      }
      continue;
    }
    if (written + 1 >= cap) {
      break;
    }
    out[written++] = *p;
  }
  out[written] = '\0';
  return written;
}

size_t diag_expanded_column(const char *in, size_t column, size_t tab_width) {
  if (!in || column <= 1) {
    return column;
  }
  if (tab_width == 0) {
    tab_width = 1;
  }

  size_t display = 0;
  size_t index = 0;
  for (const char *p = in; *p && index + 1 < column; p++, index++) {
    if (*p == '\t') {
      display += tab_width - (display % tab_width);
    } else {
      display++;
    }
  }

  display += (index + 1 < column) ? (column - 1 - index) : 0;
  return display + 1;
}
