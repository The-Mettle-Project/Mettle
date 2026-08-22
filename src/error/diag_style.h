#ifndef DIAG_STYLE_H
#define DIAG_STYLE_H

#include <stddef.h>
#include <stdio.h>

int diag_style_color(void);
int diag_style_unicode(void);

int diag_style_wrap(void);
size_t diag_style_columns(void);

const char *diag_sgr_reset(void);
const char *diag_sgr_bold(void);
const char *diag_sgr_dim(void);
const char *diag_sgr_red(void);
const char *diag_sgr_green(void);
const char *diag_sgr_yellow(void);
const char *diag_sgr_blue(void);
const char *diag_sgr_magenta(void);
const char *diag_sgr_cyan(void);

const char *diag_sgr_error(void);
const char *diag_sgr_warning(void);
const char *diag_sgr_note(void);

typedef struct {
  const char *h;
  const char *v;
  const char *dotted;
  const char *tee_down;
  const char *tee_cross;
  const char *tee_up;
  const char *elbow;
  const char *arrow;
  const char *bullet;
} DiagGlyphs;

const DiagGlyphs *diag_glyphs(void);

size_t diag_visible_width(const char *s);

void diag_repeat(FILE *out, const char *glyph, size_t n);

void diag_rule(FILE *out, size_t indent, const char *label,
               const char *label_sgr);

void diag_rule_junction(FILE *out, size_t indent, size_t gutter,
                        const char *junction);

size_t diag_rule_into(char *buf, size_t cap, size_t indent, const char *label,
                      const char *label_sgr);
size_t diag_rule_junction_into(char *buf, size_t cap, size_t indent,
                               size_t gutter, const char *junction);
size_t diag_source_into(char *buf, size_t cap, const char *text);

void diag_wrap(FILE *out, const char *first_prefix, size_t first_width,
               const char *text, const char *text_sgr);

void diag_write_source(FILE *out, const char *text);

size_t diag_expand_tabs(const char *in, char *out, size_t cap,
                        size_t tab_width);

size_t diag_expanded_column(const char *in, size_t column, size_t tab_width);

#endif
