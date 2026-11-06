#ifndef METTLE_EXPLAIN_TERM_H
#define METTLE_EXPLAIN_TERM_H

#include <stdio.h>

enum {
  EXA_BOLD = 1,
  EXA_DIM = 2,
  EXA_REVERSE = 4,
  EXA_UNDERLINE = 8,
  EXA_ACCENT = 16,
  EXA_QUIET = 32
};

typedef struct {
  unsigned ch;
  unsigned char attr;
} ExCell;

typedef struct {
  int columns;
  int rows;
  ExCell *cells;
} ExScreen;

typedef enum {
  EXK_NONE,
  EXK_CHAR,
  EXK_UP,
  EXK_DOWN,
  EXK_LEFT,
  EXK_RIGHT,
  EXK_PAGE_UP,
  EXK_PAGE_DOWN,
  EXK_HOME,
  EXK_END,
  EXK_ENTER,
  EXK_ESCAPE,
  EXK_TAB,
  EXK_BACKTAB,
  EXK_BACKSPACE,
  EXK_RESIZE,
  EXK_WHEEL_UP,
  EXK_WHEEL_DOWN,
  EXK_CLICK
} ExKeyKind;

typedef struct {
  ExKeyKind kind;
  unsigned code;
  int x;
  int y;
} ExKey;

typedef struct ExTerm ExTerm;

int ex_term_interactive(void);
ExTerm *ex_term_open(void);
ExTerm *ex_term_virtual(int columns, int rows, const char *keys);
void ex_term_size(ExTerm *term, int *columns, int *rows);
int ex_term_read(ExTerm *term, ExKey *key, int timeout_ms);
void ex_term_present(ExTerm *term, const ExScreen *screen);
void ex_term_close(ExTerm *term);
void ex_term_dump(ExTerm *term, FILE *out);
int ex_term_unicode(ExTerm *term);

int ex_screen_init(ExScreen *screen, int columns, int rows);
void ex_screen_free(ExScreen *screen);
void ex_screen_clear(ExScreen *screen);
int ex_screen_put(ExScreen *screen, int x, int y, int limit, const char *text,
                  unsigned char attr);
void ex_screen_fill(ExScreen *screen, int x, int y, int width, unsigned ch,
                    unsigned char attr);
void ex_screen_style(ExScreen *screen, int x, int y, int width,
                     unsigned char attr);

unsigned ex_utf8_next(const char **text);
int ex_char_width(unsigned ch);
int ex_text_width(const char *text);

#endif
