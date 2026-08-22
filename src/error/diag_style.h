#ifndef DIAG_STYLE_H
#define DIAG_STYLE_H

#include <stddef.h>
#include <stdio.h>

/* The presentation layer shared by the two things Mettle prints for a human to
   read: the diagnostics reporter and the --explain optimization report. Both
   used to carry their own copy of "is this a colour terminal, how wide is it,
   can it render box glyphs", and the two answers drifted. One module answers
   now, so the two surfaces look like one compiler.

   Everything here targets stderr, which is where both surfaces write.

   Three independent capabilities are probed:
     colour   - SGR sequences (honours NO_COLOR / CLICOLOR / CLICOLOR_FORCE)
     unicode  - box-drawing glyphs, only when the target provably decodes UTF-8
     columns  - the width to draw rules to

   Redirected output gets neither colour nor glyphs, so a captured build log
   stays plain ASCII. */

int diag_style_color(void);
int diag_style_unicode(void);
/* Whether to break long text into a hanging-indented block. Only a terminal
   gets that: a redirected diagnostic is read by test harnesses, editors and
   build logs that match against whole messages, and where the line breaks
   would fall is not something a message's content should depend on. */
int diag_style_wrap(void);
size_t diag_style_columns(void);

/* SGR sequences. Each returns "" when colour is off, so call sites can splice
   them into a format string unconditionally. */
const char *diag_sgr_reset(void);
const char *diag_sgr_bold(void);
const char *diag_sgr_dim(void);
const char *diag_sgr_red(void);
const char *diag_sgr_green(void);
const char *diag_sgr_yellow(void);
const char *diag_sgr_blue(void);
const char *diag_sgr_magenta(void);
const char *diag_sgr_cyan(void);
/* Bold red / bold yellow / bold cyan, the three severity headers. */
const char *diag_sgr_error(void);
const char *diag_sgr_warning(void);
const char *diag_sgr_note(void);

/* Frame glyphs, with ASCII stand-ins when the target cannot render UTF-8.
   Each is one display column wide (`h` is one column, repeat it for rules). */
typedef struct {
  const char *h;         /* horizontal rule segment          */
  const char *v;         /* vertical gutter bar              */
  const char *dotted;    /* gutter bar on an annotation row  */
  const char *tee_down;  /* rule meets the gutter, opening   */
  const char *tee_cross; /* rule meets the gutter, mid-frame */
  const char *tee_up;    /* rule meets the gutter, closing   */
  const char *elbow;     /* tree corner for a nested note    */
  const char *arrow;     /* "becomes"                        */
  const char *bullet;    /* separator inside a line          */
} DiagGlyphs;

const DiagGlyphs *diag_glyphs(void);

/* Number of display columns `s` occupies, skipping SGR escape sequences and
   counting a UTF-8 sequence as one column. */
size_t diag_visible_width(const char *s);

/* Write `glyph` `n` times. */
void diag_repeat(FILE *out, const char *glyph, size_t n);

/* A full-width horizontal rule with `label` set into it, indented by `indent`:

     -- error[E0004] --------------------------------------------

   `label_sgr` styles the label; the rule itself is always dim. A NULL label
   draws a plain rule. */
void diag_rule(FILE *out, size_t indent, const char *label,
               const char *label_sgr);

/* A rule that crosses a gutter bar at column `indent + gutter + 1`, drawn with
   `junction` (one of the tee glyphs). This is what frames the source table. */
void diag_rule_junction(FILE *out, size_t indent, size_t gutter,
                        const char *junction);

/* The same three, rendered into a caller's buffer instead of a stream, for the
   --explain report: it assembles the whole report in memory first so it can
   re-wrap it to the terminal, or divert it to a sidecar file. Each returns the
   length written and NUL-terminates. No trailing newline. */
size_t diag_rule_into(char *buf, size_t cap, size_t indent, const char *label,
                      const char *label_sgr);
size_t diag_rule_junction_into(char *buf, size_t cap, size_t indent,
                               size_t gutter, const char *junction);
size_t diag_source_into(char *buf, size_t cap, const char *text);

/* Write `text` word-wrapped to the terminal width. The first line is prefixed
   with `first_prefix` (already styled, and `first_width` display columns wide);
   continuation lines are indented to match. */
void diag_wrap(FILE *out, const char *first_prefix, size_t first_width,
               const char *text, const char *text_sgr);

/* Write one line of Mettle source with syntax colouring. Emits no newline and
   no SGR at all when colour is off, so the byte count on a redirected build
   log is unchanged. */
void diag_write_source(FILE *out, const char *text);

/* Expand tabs to `tab_width` columns so carets line up under the character
   they point at. Returns the expanded length; `out` is NUL-terminated and
   truncated to `cap`. */
size_t diag_expand_tabs(const char *in, char *out, size_t cap,
                        size_t tab_width);

/* The display column a 1-based source column lands on after tab expansion. */
size_t diag_expanded_column(const char *in, size_t column, size_t tab_width);

#endif /* DIAG_STYLE_H */
