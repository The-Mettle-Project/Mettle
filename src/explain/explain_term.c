#include "explain_term.h"
#include "../error/diag_style.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#if defined(__linux__)
typedef struct {
  unsigned c_iflag;
  unsigned c_oflag;
  unsigned c_cflag;
  unsigned c_lflag;
  unsigned char c_line;
  unsigned char c_cc[19];
} ExTermios;
#define EX_TCGETS 0x5401UL
#define EX_TCSETS 0x5402UL
#define EX_TIOCGWINSZ 0x5413UL
#define EX_VTIME 5
#define EX_VMIN 6
#define EX_ISIG 0x1u
#define EX_ICANON 0x2u
#define EX_ECHO 0x8u
#define EX_IEXTEN 0x8000u
#define EX_IXON 0x400u
#define EX_ICRNL 0x100u
#define EX_BRKINT 0x2u
#define EX_POSIX_TERM 1
#elif defined(__APPLE__)
typedef struct {
  unsigned long c_iflag;
  unsigned long c_oflag;
  unsigned long c_cflag;
  unsigned long c_lflag;
  unsigned char c_cc[20];
  unsigned long c_ispeed;
  unsigned long c_ospeed;
} ExTermios;
#define EX_TCGETS 0x40487413UL
#define EX_TCSETS 0x80487414UL
#define EX_TIOCGWINSZ 0x40087468UL
#define EX_VMIN 16
#define EX_VTIME 17
#define EX_ISIG 0x80u
#define EX_ICANON 0x100u
#define EX_ECHO 0x8u
#define EX_IEXTEN 0x400u
#define EX_IXON 0x200u
#define EX_ICRNL 0x100u
#define EX_BRKINT 0x2u
#define EX_POSIX_TERM 1
#endif

typedef struct {
  unsigned short rows;
  unsigned short columns;
  unsigned short x_pixels;
  unsigned short y_pixels;
} ExWinSize;

struct ExTerm {
  int is_virtual;
  int columns;
  int rows;
  int unicode;
  int color;
  ExScreen last;
  int have_last;
  char *out;
  size_t out_len;
  size_t out_cap;
  ExKey *script;
  size_t script_count;
  size_t script_at;
#ifdef _WIN32
  HANDLE in;
  HANDLE out_handle;
  DWORD in_mode;
  DWORD out_mode;
  UINT out_cp;
  unsigned high_surrogate;
#elif defined(EX_POSIX_TERM)
  ExTermios saved;
  unsigned char pending[128];
  size_t pending_len;
#endif
};

unsigned ex_utf8_next(const char **text) {
  const unsigned char *s = (const unsigned char *)*text;
  unsigned c = s[0];
  unsigned code;
  int extra;
  if (c == 0) {
    return 0;
  }
  if (c < 0x80) {
    *text += 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0) {
    code = c & 0x1F;
    extra = 1;
  } else if ((c & 0xF0) == 0xE0) {
    code = c & 0x0F;
    extra = 2;
  } else if ((c & 0xF8) == 0xF0) {
    code = c & 0x07;
    extra = 3;
  } else {
    *text += 1;
    return 0xFFFD;
  }
  for (int i = 1; i <= extra; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *text += i;
      return 0xFFFD;
    }
    code = (code << 6) | (s[i] & 0x3F);
  }
  *text += extra + 1;
  return code;
}

int ex_char_width(unsigned c) {
  if (c < 0x20 || (c >= 0x7F && c < 0xA0)) {
    return 0;
  }
  if ((c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
      (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
      (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200B || c == 0x200D) {
    return 0;
  }
  if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF &&
                                       c != 0x303F) ||
      (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
      (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
      (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
      (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD)) {
    return 2;
  }
  return 1;
}

int ex_text_width(const char *text) {
  int width = 0;
  while (text && *text) {
    unsigned c = ex_utf8_next(&text);
    width += c == '\t' ? 4 : ex_char_width(c);
  }
  return width;
}

int ex_screen_init(ExScreen *screen, int columns, int rows) {
  screen->columns = columns > 0 ? columns : 1;
  screen->rows = rows > 0 ? rows : 1;
  screen->cells =
      calloc((size_t)screen->columns * (size_t)screen->rows, sizeof(ExCell));
  if (!screen->cells) {
    return 0;
  }
  ex_screen_clear(screen);
  return 1;
}

void ex_screen_free(ExScreen *screen) {
  free(screen->cells);
  screen->cells = NULL;
  screen->columns = 0;
  screen->rows = 0;
}

void ex_screen_clear(ExScreen *screen) {
  size_t n = (size_t)screen->columns * (size_t)screen->rows;
  for (size_t i = 0; i < n; i++) {
    screen->cells[i].ch = ' ';
    screen->cells[i].attr = 0;
  }
}

static ExCell *ex_cell(ExScreen *screen, int x, int y) {
  if (x < 0 || y < 0 || x >= screen->columns || y >= screen->rows) {
    return NULL;
  }
  return &screen->cells[(size_t)y * (size_t)screen->columns + (size_t)x];
}

int ex_screen_put(ExScreen *screen, int x, int y, int limit, const char *text,
                  unsigned char attr) {
  int start = x;
  int end = x + limit;
  if (end > screen->columns) {
    end = screen->columns;
  }
  while (text && *text && x < end) {
    unsigned c = ex_utf8_next(&text);
    int width;
    if (c == '\t') {
      int stop = start + ((x - start) / 4 + 1) * 4;
      while (x < stop && x < end) {
        ExCell *cell = ex_cell(screen, x++, y);
        if (cell) {
          cell->ch = ' ';
          cell->attr = attr;
        }
      }
      continue;
    }
    width = ex_char_width(c);
    if (width == 0) {
      continue;
    }
    if (x + width > end) {
      ExCell *cell = ex_cell(screen, x++, y);
      if (cell) {
        cell->ch = ' ';
        cell->attr = attr;
      }
      break;
    }
    ExCell *cell = ex_cell(screen, x, y);
    if (cell) {
      cell->ch = c;
      cell->attr = attr;
    }
    if (width == 2) {
      ExCell *rest = ex_cell(screen, x + 1, y);
      if (rest) {
        rest->ch = 0;
        rest->attr = attr;
      }
    }
    x += width;
  }
  return x - start;
}

void ex_screen_fill(ExScreen *screen, int x, int y, int width, unsigned ch,
                    unsigned char attr) {
  for (int i = 0; i < width; i++) {
    ExCell *cell = ex_cell(screen, x + i, y);
    if (cell) {
      cell->ch = ch;
      cell->attr = attr;
    }
  }
}

void ex_screen_style(ExScreen *screen, int x, int y, int width,
                     unsigned char attr) {
  for (int i = 0; i < width; i++) {
    ExCell *cell = ex_cell(screen, x + i, y);
    if (cell) {
      cell->attr = attr;
    }
  }
}

static void ex_out(ExTerm *term, const char *bytes, size_t n) {
  if (term->out_len + n > term->out_cap) {
    size_t cap = term->out_cap ? term->out_cap : 8192;
    char *grown;
    while (term->out_len + n > cap) {
      cap *= 2;
    }
    grown = realloc(term->out, cap);
    if (!grown) {
      return;
    }
    term->out = grown;
    term->out_cap = cap;
  }
  memcpy(term->out + term->out_len, bytes, n);
  term->out_len += n;
}

static void ex_outs(ExTerm *term, const char *text) {
  ex_out(term, text, strlen(text));
}

static void ex_flush(ExTerm *term) {
  size_t done = 0;
  while (done < term->out_len) {
#ifdef _WIN32
    DWORD wrote = 0;
    if (!WriteFile(term->out_handle, term->out + done,
                   (DWORD)(term->out_len - done), &wrote, NULL) ||
        wrote == 0) {
      break;
    }
    done += wrote;
#elif defined(EX_POSIX_TERM)
    long wrote = (long)write(1, term->out + done, term->out_len - done);
    if (wrote <= 0) {
      break;
    }
    done += (size_t)wrote;
#else
    done = term->out_len;
#endif
  }
  term->out_len = 0;
}

static void ex_utf8_out(ExTerm *term, unsigned c) {
  char b[4];
  size_t n;
  if (c < 0x80) {
    b[0] = (char)c;
    n = 1;
  } else if (c < 0x800) {
    b[0] = (char)(0xC0 | (c >> 6));
    b[1] = (char)(0x80 | (c & 0x3F));
    n = 2;
  } else if (c < 0x10000) {
    b[0] = (char)(0xE0 | (c >> 12));
    b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    b[2] = (char)(0x80 | (c & 0x3F));
    n = 3;
  } else {
    b[0] = (char)(0xF0 | (c >> 18));
    b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    b[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    b[3] = (char)(0x80 | (c & 0x3F));
    n = 4;
  }
  ex_out(term, b, n);
}

static void ex_sgr(ExTerm *term, unsigned char attr) {
  char sgr[64];
  size_t n = 0;
  n += (size_t)snprintf(sgr + n, sizeof(sgr) - n, "\x1b[0");
  if (attr & EXA_BOLD) {
    n += (size_t)snprintf(sgr + n, sizeof(sgr) - n, ";1");
  }
  if (attr & EXA_UNDERLINE) {
    n += (size_t)snprintf(sgr + n, sizeof(sgr) - n, ";4");
  }
  if (attr & EXA_REVERSE) {
    n += (size_t)snprintf(sgr + n, sizeof(sgr) - n, ";7");
  }
  if (term->color) {
    if (attr & EXA_ACCENT) {
      n += (size_t)snprintf(sgr + n, sizeof(sgr) - n, ";38;5;179");
    } else if (attr & EXA_QUIET) {
      n += (size_t)snprintf(sgr + n, sizeof(sgr) - n, ";38;5;240");
    } else if (attr & EXA_DIM) {
      n += (size_t)snprintf(sgr + n, sizeof(sgr) - n, ";38;5;245");
    }
  } else if (attr & EXA_DIM) {
    n += (size_t)snprintf(sgr + n, sizeof(sgr) - n, ";2");
  }
  snprintf(sgr + n, sizeof(sgr) - n, "m");
  ex_outs(term, sgr);
}

static int ex_row_equal(const ExScreen *a, const ExScreen *b, int y) {
  return memcmp(&a->cells[(size_t)y * (size_t)a->columns],
                &b->cells[(size_t)y * (size_t)b->columns],
                (size_t)a->columns * sizeof(ExCell)) == 0;
}

void ex_term_present(ExTerm *term, const ExScreen *screen) {
  int full = !term->have_last || term->last.columns != screen->columns ||
             term->last.rows != screen->rows;
  if (full) {
    ex_screen_free(&term->last);
    if (!ex_screen_init(&term->last, screen->columns, screen->rows)) {
      return;
    }
    term->have_last = 1;
  }
  if (!term->is_virtual) {
    int attr = -1;
    if (full) {
      ex_outs(term, "\x1b[0m\x1b[2J");
    }
    for (int y = 0; y < screen->rows; y++) {
      char move[32];
      if (!full && ex_row_equal(screen, &term->last, y)) {
        continue;
      }
      snprintf(move, sizeof(move), "\x1b[%d;1H", y + 1);
      ex_outs(term, move);
      for (int x = 0; x < screen->columns; x++) {
        const ExCell *cell =
            &screen->cells[(size_t)y * (size_t)screen->columns + (size_t)x];
        if (cell->ch == 0) {
          continue;
        }
        if (cell->attr != attr) {
          ex_sgr(term, cell->attr);
          attr = cell->attr;
        }
        ex_utf8_out(term, cell->ch);
      }
    }
    ex_outs(term, "\x1b[0m");
    ex_flush(term);
  }
  memcpy(term->last.cells, screen->cells,
         (size_t)screen->columns * (size_t)screen->rows * sizeof(ExCell));
}

void ex_term_dump(ExTerm *term, FILE *out) {
  if (!term || !term->have_last) {
    return;
  }
  for (int y = 0; y < term->last.rows; y++) {
    int end = term->last.columns;
    while (end > 0 &&
           term->last.cells[(size_t)y * (size_t)term->last.columns +
                            (size_t)(end - 1)]
                   .ch == ' ') {
      end--;
    }
    for (int x = 0; x < end; x++) {
      unsigned c =
          term->last.cells[(size_t)y * (size_t)term->last.columns + (size_t)x]
              .ch;
      if (c == 0) {
        continue;
      }
      if (c < 0x80) {
        fputc((int)c, out);
      } else if (c < 0x800) {
        fputc((int)(0xC0 | (c >> 6)), out);
        fputc((int)(0x80 | (c & 0x3F)), out);
      } else if (c < 0x10000) {
        fputc((int)(0xE0 | (c >> 12)), out);
        fputc((int)(0x80 | ((c >> 6) & 0x3F)), out);
        fputc((int)(0x80 | (c & 0x3F)), out);
      } else {
        fputc((int)(0xF0 | (c >> 18)), out);
        fputc((int)(0x80 | ((c >> 12) & 0x3F)), out);
        fputc((int)(0x80 | ((c >> 6) & 0x3F)), out);
        fputc((int)(0x80 | (c & 0x3F)), out);
      }
    }
    fputc('\n', out);
  }
}

int ex_term_unicode(ExTerm *term) { return term ? term->unicode : 0; }

static int ex_script_key(const char *token, size_t n, ExKey *key) {
  static const struct {
    const char *name;
    ExKeyKind kind;
  } names[] = {
      {"up", EXK_UP},           {"down", EXK_DOWN},
      {"left", EXK_LEFT},       {"right", EXK_RIGHT},
      {"pgup", EXK_PAGE_UP},    {"pgdn", EXK_PAGE_DOWN},
      {"home", EXK_HOME},       {"end", EXK_END},
      {"enter", EXK_ENTER},     {"esc", EXK_ESCAPE},
      {"tab", EXK_TAB},         {"backtab", EXK_BACKTAB},
      {"bksp", EXK_BACKSPACE},  {"wheelup", EXK_WHEEL_UP},
      {"wheeldown", EXK_WHEEL_DOWN},
  };
  memset(key, 0, sizeof(*key));
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strlen(names[i].name) == n && strncmp(token, names[i].name, n) == 0) {
      key->kind = names[i].kind;
      return 1;
    }
  }
  if (n == 5 && strncmp(token, "space", 5) == 0) {
    key->kind = EXK_CHAR;
    key->code = ' ';
    return 1;
  }
  if (n > 6 && strncmp(token, "click:", 6) == 0) {
    key->kind = EXK_CLICK;
    if (sscanf(token + 6, "%d,%d", &key->x, &key->y) == 2) {
      return 1;
    }
    return 0;
  }
  if (n > 7 && strncmp(token, "resize:", 7) == 0) {
    key->kind = EXK_RESIZE;
    if (sscanf(token + 7, "%dx%d", &key->x, &key->y) == 2) {
      return 1;
    }
    return 0;
  }
  {
    const char *p = token;
    unsigned c = ex_utf8_next(&p);
    if ((size_t)(p - token) == n) {
      key->kind = EXK_CHAR;
      key->code = c;
      return 1;
    }
  }
  return 0;
}

static int ex_script_add(ExTerm *term, const ExKey *key) {
  ExKey *grown =
      realloc(term->script, (term->script_count + 1) * sizeof(ExKey));
  if (!grown) {
    return 0;
  }
  term->script = grown;
  term->script[term->script_count++] = *key;
  return 1;
}

ExTerm *ex_term_virtual(int columns, int rows, const char *keys) {
  ExTerm *term = calloc(1, sizeof(ExTerm));
  const char *p = keys ? keys : "";
  if (!term) {
    return NULL;
  }
  term->is_virtual = 1;
  term->columns = columns;
  term->rows = rows;
  term->unicode = 1;
  while (*p) {
    const char *start;
    size_t n;
    while (*p == ' ') {
      p++;
    }
    start = p;
    while (*p && *p != ' ') {
      p++;
    }
    n = (size_t)(p - start);
    if (n == 0) {
      continue;
    }
    if (n > 5 && strncmp(start, "text:", 5) == 0) {
      const char *q = start + 5;
      while (q < p) {
        ExKey key = {EXK_CHAR, ex_utf8_next(&q), 0, 0};
        ex_script_add(term, &key);
      }
      continue;
    }
    {
      ExKey key;
      if (!ex_script_key(start, n, &key)) {
        fprintf(stderr, "Error: --keys does not know '%.*s'\n", (int)n,
                start);
        ex_term_close(term);
        return NULL;
      }
      ex_script_add(term, &key);
    }
  }
  return term;
}

#ifdef _WIN32
static void ex_win_size(ExTerm *term, int *columns, int *rows) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(term->out_handle, &info)) {
    *columns = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
}
#endif

int ex_term_interactive(void) {
#ifdef _WIN32
  DWORD mode = 0;
  return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) &&
         GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
#elif defined(EX_POSIX_TERM)
  return isatty(0) && isatty(1);
#else
  return 0;
#endif
}

ExTerm *ex_term_open(void) {
  ExTerm *term;
  const char *no_color = getenv("NO_COLOR");
  if (!ex_term_interactive()) {
    return NULL;
  }
  term = calloc(1, sizeof(ExTerm));
  if (!term) {
    return NULL;
  }
  term->unicode = diag_style_unicode();
  term->color = !(no_color && no_color[0]);
#ifdef _WIN32
  term->in = GetStdHandle(STD_INPUT_HANDLE);
  term->out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  GetConsoleMode(term->in, &term->in_mode);
  GetConsoleMode(term->out_handle, &term->out_mode);
  term->out_cp = GetConsoleOutputCP();
  if (!SetConsoleMode(term->out_handle,
                      term->out_mode | ENABLE_PROCESSED_OUTPUT |
                          ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    free(term);
    return NULL;
  }
  SetConsoleMode(term->in, ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT |
                               ENABLE_EXTENDED_FLAGS);
  SetConsoleOutputCP(CP_UTF8);
  term->unicode = 1;
  ex_win_size(term, &term->columns, &term->rows);
  ex_outs(term, "\x1b[?1049h\x1b[?25l");
#elif defined(EX_POSIX_TERM)
  {
    ExTermios raw;
    ExWinSize size;
    if (ioctl(0, EX_TCGETS, &term->saved) != 0) {
      free(term);
      return NULL;
    }
    raw = term->saved;
    raw.c_lflag &= ~(EX_ECHO | EX_ICANON | EX_ISIG | EX_IEXTEN);
    raw.c_iflag &= ~(EX_IXON | EX_ICRNL | EX_BRKINT);
    raw.c_cc[EX_VMIN] = 0;
    raw.c_cc[EX_VTIME] = 1;
    if (ioctl(0, EX_TCSETS, &raw) != 0) {
      free(term);
      return NULL;
    }
    memset(&size, 0, sizeof(size));
    if (ioctl(1, EX_TIOCGWINSZ, &size) == 0 && size.columns > 0) {
      term->columns = size.columns;
      term->rows = size.rows;
    }
    ex_outs(term, "\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h");
  }
#endif
  if (term->columns <= 0 || term->rows <= 0) {
    term->columns = 80;
    term->rows = 24;
  }
  ex_flush(term);
  return term;
}

void ex_term_size(ExTerm *term, int *columns, int *rows) {
  *columns = term->columns;
  *rows = term->rows;
}

static int ex_poll_size(ExTerm *term) {
  int columns = term->columns;
  int rows = term->rows;
  if (term->is_virtual) {
    return 0;
  }
#ifdef _WIN32
  ex_win_size(term, &columns, &rows);
#elif defined(EX_POSIX_TERM)
  {
    ExWinSize size;
    memset(&size, 0, sizeof(size));
    if (ioctl(1, EX_TIOCGWINSZ, &size) == 0 && size.columns > 0) {
      columns = size.columns;
      rows = size.rows;
    }
  }
#endif
  if (columns > 0 && rows > 0 &&
      (columns != term->columns || rows != term->rows)) {
    term->columns = columns;
    term->rows = rows;
    term->have_last = 0;
    return 1;
  }
  return 0;
}

#ifdef _WIN32
static int ex_win_read(ExTerm *term, ExKey *key, int timeout_ms) {
  INPUT_RECORD record;
  DWORD got = 0;
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (WaitForSingleObject(term->in, (DWORD)timeout_ms) != WAIT_OBJECT_0) {
    return 0;
  }
  if (!ReadConsoleInputW(term->in, &record, 1, &got) || got == 0) {
    return 0;
  }
  if (record.EventType == KEY_EVENT) {
    const KEY_EVENT_RECORD *k = &record.Event.KeyEvent;
    unsigned ch = k->uChar.UnicodeChar;
    if (!k->bKeyDown) {
      return 0;
    }
    switch (k->wVirtualKeyCode) {
    case VK_UP: key->kind = EXK_UP; return 1;
    case VK_DOWN: key->kind = EXK_DOWN; return 1;
    case VK_LEFT: key->kind = EXK_LEFT; return 1;
    case VK_RIGHT: key->kind = EXK_RIGHT; return 1;
    case VK_PRIOR: key->kind = EXK_PAGE_UP; return 1;
    case VK_NEXT: key->kind = EXK_PAGE_DOWN; return 1;
    case VK_HOME: key->kind = EXK_HOME; return 1;
    case VK_END: key->kind = EXK_END; return 1;
    case VK_RETURN: key->kind = EXK_ENTER; return 1;
    case VK_ESCAPE: key->kind = EXK_ESCAPE; return 1;
    case VK_BACK: key->kind = EXK_BACKSPACE; return 1;
    case VK_TAB:
      key->kind = (k->dwControlKeyState & SHIFT_PRESSED) ? EXK_BACKTAB
                                                         : EXK_TAB;
      return 1;
    default:
      break;
    }
    if (ch >= 0xD800 && ch <= 0xDBFF) {
      term->high_surrogate = ch;
      return 0;
    }
    if (ch >= 0xDC00 && ch <= 0xDFFF && term->high_surrogate) {
      ch = 0x10000 + ((term->high_surrogate - 0xD800) << 10) + (ch - 0xDC00);
      term->high_surrogate = 0;
    }
    if (ch == 0) {
      return 0;
    }
    key->kind = EXK_CHAR;
    key->code = ch;
    return 1;
  }
  if (record.EventType == MOUSE_EVENT) {
    const MOUSE_EVENT_RECORD *m = &record.Event.MouseEvent;
    int top = GetConsoleScreenBufferInfo(term->out_handle, &info)
                  ? info.srWindow.Top
                  : 0;
    if (m->dwEventFlags & MOUSE_WHEELED) {
      key->kind = ((short)HIWORD(m->dwButtonState)) > 0 ? EXK_WHEEL_UP
                                                        : EXK_WHEEL_DOWN;
      key->x = m->dwMousePosition.X;
      key->y = m->dwMousePosition.Y - top;
      return 1;
    }
    if (m->dwEventFlags == 0 &&
        (m->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED)) {
      key->kind = EXK_CLICK;
      key->x = m->dwMousePosition.X;
      key->y = m->dwMousePosition.Y - top;
      return 1;
    }
    return 0;
  }
  return 0;
}
#elif defined(EX_POSIX_TERM)
static void ex_fill(ExTerm *term) {
  if (term->pending_len < sizeof(term->pending)) {
    long n = (long)read(0, term->pending + term->pending_len,
                        sizeof(term->pending) - term->pending_len);
    if (n > 0) {
      term->pending_len += (size_t)n;
    }
  }
}

static void ex_consume(ExTerm *term, size_t n) {
  if (n > term->pending_len) {
    n = term->pending_len;
  }
  memmove(term->pending, term->pending + n, term->pending_len - n);
  term->pending_len -= n;
}

static int ex_csi(ExTerm *term, ExKey *key) {
  size_t i = 2;
  int params[4] = {0, 0, 0, 0};
  int count = 0;
  int mouse = 0;
  unsigned char final;
  if (i < term->pending_len && term->pending[i] == '<') {
    mouse = 1;
    i++;
  }
  while (i < term->pending_len) {
    unsigned char c = term->pending[i];
    if (c >= '0' && c <= '9') {
      if (count == 0) {
        count = 1;
      }
      params[count - 1] = params[count - 1] * 10 + (c - '0');
      i++;
    } else if (c == ';') {
      if (count == 0) {
        count = 1;
      }
      if (count < 4) {
        count++;
      }
      i++;
    } else {
      break;
    }
  }
  if (i >= term->pending_len) {
    return -1;
  }
  final = term->pending[i];
  ex_consume(term, i + 1);
  if (mouse) {
    if (final != 'M') {
      return 0;
    }
    key->x = params[1] - 1;
    key->y = params[2] - 1;
    if (params[0] == 64) {
      key->kind = EXK_WHEEL_UP;
      return 1;
    }
    if (params[0] == 65) {
      key->kind = EXK_WHEEL_DOWN;
      return 1;
    }
    if (params[0] == 0) {
      key->kind = EXK_CLICK;
      return 1;
    }
    return 0;
  }
  switch (final) {
  case 'A': key->kind = EXK_UP; return 1;
  case 'B': key->kind = EXK_DOWN; return 1;
  case 'C': key->kind = EXK_RIGHT; return 1;
  case 'D': key->kind = EXK_LEFT; return 1;
  case 'H': key->kind = EXK_HOME; return 1;
  case 'F': key->kind = EXK_END; return 1;
  case 'Z': key->kind = EXK_BACKTAB; return 1;
  case '~':
    switch (params[0]) {
    case 1:
    case 7: key->kind = EXK_HOME; return 1;
    case 4:
    case 8: key->kind = EXK_END; return 1;
    case 5: key->kind = EXK_PAGE_UP; return 1;
    case 6: key->kind = EXK_PAGE_DOWN; return 1;
    default: return 0;
    }
  default:
    return 0;
  }
}

static int ex_posix_read(ExTerm *term, ExKey *key) {
  unsigned char c;
  if (term->pending_len == 0) {
    ex_fill(term);
  }
  if (term->pending_len == 0) {
    return 0;
  }
  c = term->pending[0];
  if (c == 0x1b) {
    if (term->pending_len == 1) {
      ex_fill(term);
    }
    if (term->pending_len == 1) {
      ex_consume(term, 1);
      key->kind = EXK_ESCAPE;
      return 1;
    }
    if (term->pending[1] == '[') {
      int got = ex_csi(term, key);
      if (got < 0) {
        ex_fill(term);
        got = ex_csi(term, key);
      }
      if (got < 0) {
        term->pending_len = 0;
        return 0;
      }
      return got;
    }
    if (term->pending[1] == 'O' && term->pending_len >= 3) {
      unsigned char f = term->pending[2];
      ex_consume(term, 3);
      switch (f) {
      case 'A': key->kind = EXK_UP; return 1;
      case 'B': key->kind = EXK_DOWN; return 1;
      case 'C': key->kind = EXK_RIGHT; return 1;
      case 'D': key->kind = EXK_LEFT; return 1;
      case 'H': key->kind = EXK_HOME; return 1;
      case 'F': key->kind = EXK_END; return 1;
      default: return 0;
      }
    }
    ex_consume(term, 1);
    key->kind = EXK_ESCAPE;
    return 1;
  }
  if (c == '\r' || c == '\n') {
    ex_consume(term, 1);
    key->kind = EXK_ENTER;
    return 1;
  }
  if (c == '\t') {
    ex_consume(term, 1);
    key->kind = EXK_TAB;
    return 1;
  }
  if (c == 0x7F || c == 0x08) {
    ex_consume(term, 1);
    key->kind = EXK_BACKSPACE;
    return 1;
  }
  {
    size_t need = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2
                               : (c & 0xF0) == 0xE0 ? 3
                                                    : 4;
    char buffer[5];
    const char *p = buffer;
    if (term->pending_len < need) {
      ex_fill(term);
    }
    if (term->pending_len < need) {
      need = term->pending_len;
    }
    memcpy(buffer, term->pending, need);
    buffer[need] = '\0';
    ex_consume(term, need);
    key->kind = EXK_CHAR;
    key->code = ex_utf8_next(&p);
    return 1;
  }
}
#endif

int ex_term_read(ExTerm *term, ExKey *key, int timeout_ms) {
  memset(key, 0, sizeof(*key));
  if (term->is_virtual) {
    if (term->script_at >= term->script_count) {
      return -1;
    }
    *key = term->script[term->script_at++];
    if (key->kind == EXK_RESIZE) {
      term->columns = key->x;
      term->rows = key->y;
    }
    return 1;
  }
  if (ex_poll_size(term)) {
    key->kind = EXK_RESIZE;
    return 1;
  }
#ifdef _WIN32
  return ex_win_read(term, key, timeout_ms);
#elif defined(EX_POSIX_TERM)
  (void)timeout_ms;
  return ex_posix_read(term, key);
#else
  (void)timeout_ms;
  return -1;
#endif
}

void ex_term_close(ExTerm *term) {
  if (!term) {
    return;
  }
  if (!term->is_virtual) {
#ifdef _WIN32
    ex_outs(term, "\x1b[0m\x1b[?25h\x1b[?1049l");
    ex_flush(term);
    SetConsoleMode(term->in, term->in_mode);
    SetConsoleMode(term->out_handle, term->out_mode);
    SetConsoleOutputCP(term->out_cp);
#elif defined(EX_POSIX_TERM)
    ex_outs(term, "\x1b[?1000l\x1b[?1006l\x1b[0m\x1b[?25h\x1b[?1049l");
    ex_flush(term);
    ioctl(0, EX_TCSETS, &term->saved);
#endif
  }
  ex_screen_free(&term->last);
  free(term->out);
  free(term->script);
  free(term);
}
