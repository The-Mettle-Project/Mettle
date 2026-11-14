#include "explain_tui.h"
#include "../error/error_explain.h"
#include "../ir/ir.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *rule;
  const char *vertical;
  const char *ellipsis;
  const char *marker;
  const char *move;
  const char *tabs;
  const char *cursor;
  const char *range;
  const char *corner[4];
} ExGlyphs;

static const ExGlyphs EX_UNICODE = {
    "\xe2\x94\x80",
    "\xe2\x94\x82",
    "\xe2\x80\xa6",
    "\xe2\x80\xba",
    "\xe2\x86\x91\xe2\x86\x93",
    "\xe2\x86\x90\xe2\x86\x92",
    "\xe2\x96\x8f",
    "\xe2\x96\x8c",
    {"\xe2\x94\x8c", "\xe2\x94\x90", "\xe2\x94\x94", "\xe2\x94\x98"}};
static const ExGlyphs EX_ASCII = {"-",          "|", "...", ">", "up/down",
                                  "left/right", "_", "|",   {"+", "+", "+", "+"}};

enum {
  EX_ROW_SUMMARY,
  EX_ROW_BLANK,
  EX_ROW_HEADER,
  EX_ROW_GROUP,
  EX_ROW_ENTRY
};

typedef struct {
  int kind;
  int index;
} ExRow;

typedef struct {
  int x;
  char *text;
  unsigned char attr;
} ExSeg;

typedef struct {
  ExSeg seg[3];
  int count;
} ExLine;

typedef struct {
  ExLine *lines;
  size_t count;
  size_t capacity;
} ExLines;

typedef struct {
  ExReport *report;
  ExTerm *term;
  const ExGlyphs *g;
  int columns;
  int rows;
  int tab;
  int focus;
  int source_view;
  int anchor;
  int anchor_pending;
  int selected[EX_TAB_COUNT];
  int list_top[EX_TAB_COUNT];
  int detail_top;
  int quick[EX_TAB_COUNT];
  char filter[96];
  int typing;
  int help;
  int quit;
  ExRow *row_buf;
  size_t row_count;
  size_t row_capacity;
  int *visible;
  size_t visible_count;
  size_t visible_capacity;
  int stacked;
  int list_x;
  int list_y;
  int list_w;
  int list_h;
  int detail_x;
  int detail_y;
  int detail_w;
  int detail_h;
  int tab_x[EX_TAB_COUNT];
  int tab_w[EX_TAB_COUNT];
  ExLines detail;
  ExLines summary;
} ExUi;

static const char *ex_quick_names[3] = {NULL, "missed only",
                                        "with a fix only"};

static char *ex_copy(const char *text, size_t n) {
  char *out = malloc(n + 1);
  if (out) {
    memcpy(out, text, n);
    out[n] = '\0';
  }
  return out;
}

static void ex_lines_clear(ExLines *lines) {
  for (size_t i = 0; i < lines->count; i++) {
    for (int s = 0; s < lines->lines[i].count; s++) {
      free(lines->lines[i].seg[s].text);
    }
  }
  lines->count = 0;
}

static ExLine *ex_line_new(ExLines *lines) {
  ExLine *line;
  if (lines->count == lines->capacity) {
    size_t capacity = lines->capacity ? lines->capacity * 2 : 64;
    ExLine *grown = realloc(lines->lines, capacity * sizeof(ExLine));
    if (!grown) {
      return NULL;
    }
    lines->lines = grown;
    lines->capacity = capacity;
  }
  line = &lines->lines[lines->count++];
  memset(line, 0, sizeof(*line));
  return line;
}

static void ex_line_seg(ExLine *line, int x, const char *text, size_t n,
                        unsigned char attr) {
  if (!line || line->count >= 3) {
    return;
  }
  line->seg[line->count].x = x;
  line->seg[line->count].text = ex_copy(text, n);
  line->seg[line->count].attr = attr;
  line->count++;
}

static size_t ex_take(const char *text, int width) {
  const char *p = text;
  const char *last_space = NULL;
  int used = 0;
  while (*p && *p != '\n') {
    const char *before = p;
    unsigned c = ex_utf8_next(&p);
    int w = c == '\t' ? 4 : ex_char_width(c);
    if (used + w > width) {
      if (last_space && last_space > text) {
        return (size_t)(last_space - text);
      }
      return before > text ? (size_t)(before - text) : (size_t)(p - text);
    }
    if (c == ' ') {
      last_space = before;
    }
    used += w;
  }
  return (size_t)(p - text);
}

static void ex_wrap(ExLines *lines, int indent, int width, const char *text,
                    unsigned char attr, const char *label,
                    unsigned char label_attr) {
  const char *p = text ? text : "";
  int first = 1;
  if (width < 8) {
    width = 8;
  }
  do {
    size_t n = ex_take(p, width);
    ExLine *line = ex_line_new(lines);
    if (!line) {
      return;
    }
    if (first && label) {
      ex_line_seg(line, 0, label, strlen(label), label_attr);
    }
    ex_line_seg(line, indent, p, n, attr);
    p += n;
    while (*p == ' ') {
      p++;
    }
    if (*p == '\n') {
      p++;
    }
    first = 0;
  } while (*p);
}

static char *ex_fit(const ExUi *ui, const char *text, int width) {
  const char *p = text ? text : "";
  int used = 0;
  int ell = ex_text_width(ui->g->ellipsis);
  if (width <= 0) {
    return ex_copy("", 0);
  }
  if (ex_text_width(p) <= width) {
    return ex_copy(p, strlen(p));
  }
  while (*p) {
    const char *before = p;
    unsigned c = ex_utf8_next(&p);
    int w = c == '\t' ? 4 : ex_char_width(c);
    if (used + w > width - ell) {
      size_t n = (size_t)(before - (text ? text : ""));
      char *out = malloc(n + strlen(ui->g->ellipsis) + 1);
      if (out) {
        memcpy(out, text, n);
        strcpy(out + n, ui->g->ellipsis);
      }
      return out;
    }
    used += w;
  }
  return ex_copy(text, strlen(text));
}

static int ex_matches(const ExUi *ui, const ExEntry *e, int tab) {
  if (ui->quick[tab] == 1 && !(e->flags & EXF_MISSED)) {
    return 0;
  }
  if (ui->quick[tab] == 2 && !(e->flags & EXF_FIXABLE)) {
    return 0;
  }
  if (ui->filter[0]) {
    char needle[96];
    size_t n = strlen(ui->filter);
    for (size_t i = 0; i <= n; i++) {
      needle[i] = (char)tolower((unsigned char)ui->filter[i]);
    }
    return e->search && strstr(e->search, needle) != NULL;
  }
  return 1;
}

static void ex_compute_visible(ExUi *ui) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  ui->visible_count = 0;
  if (tab->count > ui->visible_capacity) {
    int *grown = realloc(ui->visible, tab->count * sizeof(int));
    if (!grown) {
      return;
    }
    ui->visible = grown;
    ui->visible_capacity = tab->count;
  }
  for (size_t i = 0; i < tab->count; i++) {
    if (ex_matches(ui, &tab->entries[i], ui->tab)) {
      ui->visible[ui->visible_count++] = (int)i;
    }
  }
}

static int ex_position(const ExUi *ui) {
  int selected = ui->selected[ui->tab];
  for (size_t i = 0; i < ui->visible_count; i++) {
    if (ui->visible[i] == selected) {
      return (int)i;
    }
  }
  return -1;
}

static void ex_settle_selection(ExUi *ui) {
  if (ui->visible_count == 0) {
    ui->selected[ui->tab] = -1;
    return;
  }
  if (ex_position(ui) < 0) {
    int selected = ui->selected[ui->tab];
    size_t best = 0;
    for (size_t i = 0; i < ui->visible_count; i++) {
      if (ui->visible[i] <= selected) {
        best = i;
      }
    }
    ui->selected[ui->tab] = ui->visible[best];
  }
}

static void ex_row_add(ExUi *ui, int kind, int index) {
  if (ui->row_count == ui->row_capacity) {
    size_t capacity = ui->row_capacity ? ui->row_capacity * 2 : 128;
    ExRow *grown = realloc(ui->row_buf, capacity * sizeof(ExRow));
    if (!grown) {
      return;
    }
    ui->row_buf = grown;
    ui->row_capacity = capacity;
  }
  ui->row_buf[ui->row_count].kind = kind;
  ui->row_buf[ui->row_count].index = index;
  ui->row_count++;
}

static int ex_same(const char *a, const char *b) {
  if (!a || !b) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

static void ex_build_summary(ExUi *ui) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  int width = ui->list_w - 4;
  ex_lines_clear(&ui->summary);
  for (size_t i = 0; i < tab->summary_count; i++) {
    const char *line = tab->summary[i];
    const char *gap = strstr(line, "  ");
    if (gap && gap - line < 11) {
      char *label = ex_copy(line, (size_t)(gap - line));
      while (*gap == ' ') {
        gap++;
      }
      ex_wrap(&ui->summary, 11, width - 11, gap, 0, label, EXA_DIM);
      free(label);
    } else {
      ex_wrap(&ui->summary, 0, width, line, 0, NULL, 0);
    }
  }
}

static void ex_build_rows(ExUi *ui) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  const char *group = NULL;
  int grouped = 0;
  ui->row_count = 0;
  ex_build_summary(ui);
  for (size_t i = 0; i < ui->summary.count; i++) {
    ex_row_add(ui, EX_ROW_SUMMARY, (int)i);
  }
  if (tab->summary_count > 0 && ui->visible_count > 0) {
    ex_row_add(ui, EX_ROW_BLANK, 0);
    ex_row_add(ui, EX_ROW_HEADER, 0);
  }
  for (size_t i = 0; i < ui->visible_count; i++) {
    const ExEntry *e = &tab->entries[ui->visible[i]];
    if (e->group && (!grouped || !ex_same(group, e->group))) {
      if (grouped) {
        ex_row_add(ui, EX_ROW_BLANK, 0);
      }
      ex_row_add(ui, EX_ROW_GROUP, ui->visible[i]);
      group = e->group;
      grouped = 1;
    }
    ex_row_add(ui, EX_ROW_ENTRY, ui->visible[i]);
  }
}

static int ex_row_of_selected(const ExUi *ui) {
  for (size_t r = 0; r < ui->row_count; r++) {
    if (ui->row_buf[r].kind == EX_ROW_ENTRY &&
        ui->row_buf[r].index == ui->selected[ui->tab]) {
      return (int)r;
    }
  }
  return -1;
}

static void ex_layout(ExUi *ui) {
  int body_y = 3;
  int body_h = ui->rows - 4;
  ui->stacked = ui->columns < 110;
  if (ui->stacked) {
    int top = body_h * 9 / 20;
    if (top < 4) {
      top = 4;
    }
    ui->list_x = 0;
    ui->list_y = body_y;
    ui->list_w = ui->columns;
    ui->list_h = top;
    ui->detail_x = 2;
    ui->detail_y = body_y + top + 1;
    ui->detail_w = ui->columns - 4;
    ui->detail_h = body_h - top - 1;
  } else {
    int left = ui->columns * 9 / 20;
    if (left < 52) {
      left = 52;
    }
    ui->list_x = 0;
    ui->list_y = body_y;
    ui->list_w = left;
    ui->list_h = body_h;
    ui->detail_x = left + 3;
    ui->detail_y = body_y;
    ui->detail_w = ui->columns - left - 5;
    ui->detail_h = body_h;
  }
}

static const ExEntry *ex_current(const ExUi *ui) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  int selected = ui->selected[ui->tab];
  if (selected < 0 || (size_t)selected >= tab->count) {
    return NULL;
  }
  return &tab->entries[selected];
}

static const ExEntry *ex_shown(const ExUi *ui, const ExEntry *entry) {
  if (entry && entry->link_tab >= 0 && entry->link_entry >= 0) {
    const ExTab *target = &ui->report->tabs[entry->link_tab];
    if ((size_t)entry->link_entry < target->count) {
      return &target->entries[entry->link_entry];
    }
  }
  return entry;
}

static void ex_build_source_view(ExUi *ui, const ExEntry *entry) {
  int from = entry ? entry->line : 0;
  int to = entry && entry->end_line > from ? entry->end_line : from;
  char number[16];
  for (size_t i = 0; i < ui->report->line_count; i++) {
    int line_no = (int)i + 1;
    int inside = line_no >= from && line_no <= to;
    ExLine *line = ex_line_new(&ui->detail);
    snprintf(number, sizeof(number), "%5d", line_no);
    ex_line_seg(line, 0, number, strlen(number),
                inside ? EXA_BOLD | EXA_ACCENT : EXA_QUIET);
    if (inside) {
      ex_line_seg(line, 6, ui->g->range, strlen(ui->g->range), EXA_ACCENT);
    }
    ex_line_seg(line, 8, ui->report->lines[i], strlen(ui->report->lines[i]),
                inside ? 0 : EXA_DIM);
  }
}

static void ex_entry_range(const ExEntry *entry, int *from, int *to) {
  *from = entry->line;
  *to = entry->end_line > entry->line ? entry->end_line : entry->line;
}

static int ex_code_rows(const ExMachineFn *code, int from, int to) {
  int count = 0;
  for (size_t i = 0; i < code->count; i++) {
    count += !code->insns[i].label && code->insns[i].line >= from &&
             code->insns[i].line <= to;
  }
  return count;
}

static const ExMachineFn *ex_entry_code(const ExUi *ui, const ExEntry *entry) {
  const ExMachineFn *code = ex_report_machine(ui->report, entry->function);
  const ExMachineFn *best = NULL;
  int best_rows = 0;
  int from;
  int to;
  if (code || entry->line <= 0) {
    return code;
  }
  ex_entry_range(entry, &from, &to);
  for (size_t f = 0; f < ui->report->machine_count; f++) {
    int rows = ex_code_rows(&ui->report->machine[f], from, to);
    if (rows > best_rows) {
      best_rows = rows;
      best = &ui->report->machine[f];
    }
  }
  return best;
}

static void ex_insn_row(ExUi *ui, const ExInsn *insn, int marked) {
  char where[32];
  char cost[32];
  ExLine *line = ex_line_new(&ui->detail);
  if (insn->label) {
    char text[200];
    snprintf(text, sizeof(text), "%s:", insn->text ? insn->text : "?");
    ex_line_seg(line, 8, text, strlen(text), EXA_QUIET);
    return;
  }
  snprintf(where, sizeof(where), insn->device ? "%s%5llu" : "%s%5llx",
           marked ? ui->g->range : " ", (unsigned long long)insn->offset);
  if (insn->cost > 0) {
    char cycles[16];
    if (insn->cost % 100 == 0) {
      snprintf(cycles, sizeof(cycles), "%d", insn->cost / 100);
    } else {
      snprintf(cycles, sizeof(cycles), "%d.%02d", insn->cost / 100,
               insn->cost % 100);
    }
    snprintf(cost, sizeof(cost), "%5s %s", cycles,
             insn->ports ? insn->ports : "");
  } else {
    cost[0] = '\0';
  }
  ex_line_seg(line, 0, where, strlen(where), marked ? EXA_ACCENT : EXA_QUIET);
  ex_line_seg(line, 8, cost, strlen(cost), EXA_QUIET);
  ex_line_seg(line, insn->device ? 8 : 22, insn->text ? insn->text : "",
              insn->text ? strlen(insn->text) : 0,
              insn->kernel ? EXA_ACCENT : marked ? 0 : EXA_DIM);
}

static void ex_machine_counts(const ExMachineFn *code, int from, int to,
                              int *rows, int *vector, int *kernel) {
  *rows = 0;
  *vector = 0;
  *kernel = 0;
  for (size_t i = 0; i < code->count; i++) {
    const ExInsn *insn = &code->insns[i];
    if (insn->label || insn->line < from || insn->line > to) {
      continue;
    }
    (*rows)++;
    *vector += insn->text && insn->text[0] == 'v' &&
               (strstr(insn->text, "ymm") || strstr(insn->text, "xmm"));
    *kernel += insn->kernel;
  }
}

static int ex_insn_vector(const ExInsn *insn) {
  return insn->text && insn->text[0] == 'v' &&
         (strstr(insn->text, "ymm") || strstr(insn->text, "xmm"));
}

static int ex_find_loop(const ExMachineFn *code, int from, int to,
                        size_t *first, size_t *last) {
  int best_vector = -1;
  int best_inside = -1;
  size_t best_span = 0;
  for (size_t j = 0; j < code->count; j++) {
    const ExInsn *jump = &code->insns[j];
    const char *target;
    const char *jcc;
    if (jump->label || !jump->text || jump->line < from || jump->line > to) {
      continue;
    }
    jcc = jump->text[0] == 'j' ? jump->text : strstr(jump->text, "; j");
    if (!jcc) {
      continue;
    }
    target = strrchr(jcc, ' ');
    if (!target) {
      continue;
    }
    target++;
    for (size_t k = 0; k < j; k++) {
      const ExInsn *head = &code->insns[k];
      int hit;
      if (strncmp(target, "0x", 2) == 0) {
        hit = !head->label && head->offset == strtoll(target, NULL, 16);
      } else {
        hit = head->label && head->text && strcmp(head->text, target) == 0;
      }
      if (hit) {
        int vector = 0;
        int rows = 0;
        int inside = 0;
        for (size_t m = k; m <= j; m++) {
          const ExInsn *insn = &code->insns[m];
          vector += ex_insn_vector(insn);
          if (!insn->label) {
            rows++;
            inside += insn->line >= from && insn->line <= to;
          }
        }
        inside = rows ? inside * 100 / rows : 0;
        if (vector > best_vector ||
            (vector == best_vector &&
             (inside > best_inside ||
              (inside == best_inside && j - k > best_span)))) {
          best_vector = vector;
          best_inside = inside;
          best_span = j - k;
          *first = k;
          *last = j;
        }
        break;
      }
    }
  }
  return best_vector >= 0;
}

static void ex_machine_section(ExUi *ui, const ExEntry *entry) {
  const ExMachineFn *code = ex_entry_code(ui, entry);
  int width = ui->detail_w;
  int from;
  int to;
  int rows;
  int vector;
  int kernel;
  int shown = 0;
  char head[256];
  if (!code || code->count == 0) {
    return;
  }
  if (entry->line <= 0 || ui->tab == EX_TAB_FUNCTIONS) {
    from = 0;
    to = 1 << 30;
  } else {
    ex_entry_range(entry, &from, &to);
  }
  ex_machine_counts(code, from, to, &rows, &vector, &kernel);
  if (rows == 0) {
    return;
  }
  ex_line_new(&ui->detail);
  {
    const char *what = code->device ? "Device code" : "Machine code";
    const char *unit = code->device ? "lines" : "instructions";
    if (from == 0) {
      snprintf(head, sizeof(head), "%s  %s: %d %s", what, code->name, rows,
               unit);
    } else if (from == to) {
      snprintf(head, sizeof(head), "%s  %s, line %d: %d %s", what, code->name,
               from, rows, unit);
    } else {
      snprintf(head, sizeof(head), "%s  %s, lines %d to %d: %d %s", what,
               code->name, from, to, rows, unit);
    }
  }
  if (kernel) {
    size_t n = strlen(head);
    snprintf(head + n, sizeof(head) - n, ", %d vector, %d in a kernel", vector,
             kernel);
  } else if (vector) {
    size_t n = strlen(head);
    snprintf(head + n, sizeof(head) - n, ", %d vector", vector);
  }
  ex_wrap(&ui->detail, 0, width, head, EXA_BOLD, NULL, 0);
  if (from == 0) {
    ex_wrap(&ui->detail, 0, width, "d shows every instruction beside the line "
                                   "it came from",
            EXA_DIM, NULL, 0);
    return;
  }
  {
    size_t first = 0;
    size_t last = 0;
    if (ex_find_loop(code, from, to, &first, &last) && last - first < 40) {
      char note[96];
      int body = 0;
      for (size_t i = first; i <= last; i++) {
        body += !code->insns[i].label;
      }
      snprintf(note, sizeof(note), "the machine loop, %d instruction%s:",
               body, body == 1 ? "" : "s");
      ex_wrap(&ui->detail, 8, width - 8, note, EXA_DIM, NULL, 0);
      for (size_t i = first; i <= last; i++) {
        ex_insn_row(ui, &code->insns[i], 1);
        shown += !code->insns[i].label;
      }
    } else {
      for (size_t i = 0; i < code->count && shown < 18; i++) {
        const ExInsn *insn = &code->insns[i];
        if (insn->line < from || insn->line > to) {
          continue;
        }
        ex_insn_row(ui, insn, 0);
        shown += !insn->label;
      }
    }
  }
  if (rows > shown) {
    char more[96];
    snprintf(more, sizeof(more), "%d more; d shows the whole function",
             rows - shown);
    ex_wrap(&ui->detail, 8, width - 8, more, EXA_DIM, NULL, 0);
  }
}

static void ex_build_machine_view(ExUi *ui, const ExEntry *entry) {
  const ExMachineFn *code = ex_entry_code(ui, entry);
  int from;
  int to;
  int last_line = -1;
  const char *last_origin = NULL;
  char head[160];
  ui->anchor = 0;
  if (!code || code->count == 0) {
    ex_wrap(&ui->detail, 0, ui->detail_w,
            "No machine code for this entry: its function was inlined into "
            "every caller, or this target emits none.",
            EXA_DIM, NULL, 0);
    return;
  }
  ex_entry_range(entry, &from, &to);
  if (ui->tab == EX_TAB_FUNCTIONS) {
    from = to = -1;
  }
  snprintf(head, sizeof(head), "%s  %zu rows", code->name, code->count);
  ex_wrap(&ui->detail, 0, ui->detail_w, head, EXA_BOLD, NULL, 0);
  for (size_t i = 0; i < code->count; i++) {
    const ExInsn *insn = &code->insns[i];
    int marked = insn->line >= from && insn->line <= to && from > 0;
    if (insn->line > 0 && insn->line != last_line) {
      const char *text = ex_report_line(ui->report, insn->line);
      char number[16];
      ExLine *line = ex_line_new(&ui->detail);
      snprintf(number, sizeof(number), "%5d", insn->line);
      ex_line_seg(line, 0, number, strlen(number),
                  marked ? EXA_BOLD | EXA_ACCENT : EXA_QUIET);
      ex_line_seg(line, 8, text ? text : "", text ? strlen(text) : 0,
                  marked ? EXA_BOLD : EXA_DIM);
      last_line = insn->line;
      last_origin = NULL;
      if (marked && ui->anchor == 0) {
        ui->anchor = (int)ui->detail.count - 1;
      }
    } else if (insn->line == 0 && insn->origin &&
               (!last_origin || strcmp(last_origin, insn->origin) != 0)) {
      char note[160];
      snprintf(note, sizeof(note), "inlined from %s", insn->origin);
      ex_wrap(&ui->detail, 8, ui->detail_w - 8, note, EXA_QUIET, NULL, 0);
      last_origin = insn->origin;
      last_line = 0;
    }
    ex_insn_row(ui, insn, marked);
  }
}

static void ex_expand_row(ExUi *ui, const ExExpandLine *l, int number_shown,
                          int marked) {
  int indent = 8;
  const char *text = l->text ? l->text : "";
  while (*text == ' ') {
    text++;
    indent++;
  }
  if (l->note) {
    ex_wrap(&ui->detail, indent, ui->detail_w - indent, text, EXA_QUIET, NULL,
            0);
    return;
  }
  {
    ExLine *line = ex_line_new(&ui->detail);
    if (number_shown) {
      char number[16];
      snprintf(number, sizeof(number), "%5d", l->line);
      ex_line_seg(line, 0, number, strlen(number),
                  marked ? EXA_BOLD | EXA_ACCENT : EXA_QUIET);
    }
    if (l->inserted) {
      ex_line_seg(line, 6, "+", 1, EXA_ACCENT);
    }
    ex_line_seg(line, indent, text, strlen(text),
                l->inserted ? EXA_ACCENT : marked ? 0 : EXA_DIM);
  }
}

static int ex_expand_rows(ExUi *ui, const ExExpandFn *fn, int from, int to,
                          int all) {
  int last = -1;
  int first_marked = -1;
  for (size_t i = 0; i < fn->count; i++) {
    const ExExpandLine *l = &fn->lines[i];
    int marked = l->line >= from && l->line <= to && from > 0;
    if (!all && !marked) {
      continue;
    }
    if (marked && first_marked < 0) {
      first_marked = (int)ui->detail.count;
    }
    ex_expand_row(ui, l, l->line > 0 && l->line != last && !l->note, marked);
    if (!l->note && l->line > 0) {
      last = l->line;
    }
  }
  return first_marked;
}

static int ex_expanded_section(ExUi *ui, const ExEntry *entry) {
  const ExExpandFn *fn = ex_report_expanded(ui->report, entry->function);
  char head[200];
  char shown[128];
  int from;
  int to;
  if (!fn || fn->count == 0 || entry->line <= 0) {
    return 0;
  }
  ex_entry_range(entry, &from, &to);
  ir_display_name(fn->name, shown, sizeof(shown));
  snprintf(head, sizeof(head),
           "As the compiler rewrote it  %s, lines %d to %d; e shows all of it",
           shown, from, to);
  ex_line_new(&ui->detail);
  ex_wrap(&ui->detail, 0, ui->detail_w, head, EXA_BOLD, NULL, 0);
  ex_expand_rows(ui, fn, from, to, 0);
  return 1;
}

static void ex_build_expanded_view(ExUi *ui, const ExEntry *entry) {
  const ExExpandFn *fn = ex_report_expanded(ui->report, entry->function);
  char head[200];
  char shown[128];
  int from;
  int to;
  ui->anchor = 0;
  if (!fn) {
    ex_wrap(&ui->detail, 0, ui->detail_w,
            "No rewritten source for this entry: nothing in its function "
            "owns a resource, or it lives in another file.",
            EXA_DIM, NULL, 0);
    return;
  }
  ex_entry_range(entry, &from, &to);
  ir_display_name(fn->name, shown, sizeof(shown));
  snprintf(head, sizeof(head),
           "%s as the compiler rewrote it; + marks a line it inserted", shown);
  ex_wrap(&ui->detail, 0, ui->detail_w, head, EXA_BOLD, NULL, 0);
  {
    int first = ex_expand_rows(ui, fn, from, to, 1);
    ui->anchor = first > 0 ? first : 0;
  }
}

static void ex_build_detail(ExUi *ui) {
  const ExEntry *entry = ex_current(ui);
  const ExEntry *shown = ex_shown(ui, entry);
  int label_w = 0;
  int width = ui->detail_w;
  const char *title = NULL;
  const char *body = NULL;
  ex_lines_clear(&ui->detail);
  if (!entry) {
    return;
  }
  if (ui->source_view >= 2) {
    if (ui->source_view == 3) {
      ex_build_expanded_view(ui, shown);
    } else {
      ex_build_machine_view(ui, shown);
    }
    if (ui->anchor_pending) {
      ui->detail_top = ui->anchor > ui->detail_h / 3
                           ? ui->anchor - ui->detail_h / 3
                           : 0;
      ui->anchor_pending = 0;
    }
    return;
  }
  if (ui->source_view) {
    ex_build_source_view(ui, shown);
    return;
  }
  if (shown != entry) {
    char hint[96];
    snprintf(hint, sizeof(hint), "Enter opens this in %s",
             ui->report->tabs[entry->link_tab].name);
    ex_wrap(&ui->detail, 0, width, hint, EXA_DIM, NULL, 0);
    ex_line_new(&ui->detail);
  }
  ex_wrap(&ui->detail, 0, width, shown->title ? shown->title : "", EXA_BOLD,
          NULL, 0);
  ex_line_new(&ui->detail);
  for (size_t i = 0; i < shown->field_count; i++) {
    int w = ex_text_width(shown->fields[i].label);
    if (w > label_w) {
      label_w = w;
    }
  }
  if (label_w > 14) {
    label_w = 14;
  }
  for (size_t i = 0; i < shown->field_count; i++) {
    const ExField *f = &shown->fields[i];
    unsigned char attr = f->tone == EX_TONE_FIXABLE  ? EXA_ACCENT
                         : f->tone == EX_TONE_DONE ? EXA_DIM
                                                   : 0;
    const char *label =
        i > 0 && ex_same(shown->fields[i - 1].label, f->label) ? NULL
                                                                : f->label;
    if (label && ex_text_width(label) > label_w) {
      ex_wrap(&ui->detail, 0, width, label, EXA_DIM, NULL, 0);
      label = NULL;
    }
    ex_wrap(&ui->detail, label_w + 2, width - label_w - 2, f->text, attr,
            label, EXA_DIM);
  }
  if (ui->tab == EX_TAB_OWNERSHIP && ex_expanded_section(ui, shown)) {
  } else if (shown->line > 0 && ui->report->line_count > 0) {
    int from = shown->line - 2;
    int last = shown->end_line > shown->line ? shown->end_line : shown->line;
    int to = last + 2;
    if (last - shown->line > 12) {
      to = shown->line + 12;
    }
    if (from < 1) {
      from = 1;
    }
    ex_line_new(&ui->detail);
    for (int n = from; n <= to && (size_t)n <= ui->report->line_count; n++) {
      int inside = n >= shown->line && n <= last;
      char number[16];
      ExLine *line = ex_line_new(&ui->detail);
      const char *text = ex_report_line(ui->report, n);
      snprintf(number, sizeof(number), "%5d", n);
      ex_line_seg(line, 0, number, strlen(number),
                  inside ? EXA_BOLD : EXA_QUIET);
      if (inside) {
        ex_line_seg(line, 6, ui->g->range, strlen(ui->g->range), EXA_ACCENT);
      }
      ex_line_seg(line, 8, text, strlen(text), inside ? 0 : EXA_DIM);
    }
  }
  ex_machine_section(ui, shown);
  if (shown->code && mettle_explain_lookup(shown->code, &title, &body)) {
    char head[256];
    ex_line_new(&ui->detail);
    snprintf(head, sizeof(head), "%s  %s", shown->code, title);
    ex_wrap(&ui->detail, 0, width, head, EXA_BOLD, NULL, 0);
    ex_wrap(&ui->detail, 0, width, body, EXA_DIM, NULL, 0);
  }
}

static void ex_put(ExScreen *s, int x, int y, int w, const char *text,
                   unsigned char attr) {
  if (w > 0) {
    ex_screen_put(s, x, y, w, text, attr);
  }
}

static void ex_draw_header(ExUi *ui, ExScreen *s) {
  const char *path = ui->report->source_path ? ui->report->source_path
                     : ui->report->source_name ? ui->report->source_name
                                               : "";
  int x = 1;
  int hint_w = ex_text_width("? help");
  x += ex_screen_put(s, x, 0, ui->columns - x, "explain", EXA_BOLD);
  x += 2;
  {
    char *fitted = ex_fit(ui, path, ui->columns - x - hint_w - 3);
    ex_put(s, x, 0, ui->columns - x, fitted, EXA_DIM);
    free(fitted);
  }
  ex_put(s, ui->columns - hint_w - 1, 0, hint_w, "? help", EXA_QUIET);
}

static int ex_tab_width(const ExUi *ui, int t, int mode) {
  const ExTab *tab = &ui->report->tabs[t];
  char count[24];
  int w = 2;
  if (mode == 2 && t != ui->tab) {
    return 1;
  }
  w += ex_text_width(tab->name);
  if (mode == 0 && tab->count > 0) {
    snprintf(count, sizeof(count), " %zu", tab->count);
    w += ex_text_width(count);
  }
  return w;
}

static void ex_draw_tabs(ExUi *ui, ExScreen *s) {
  int x = 1;
  int mode = 0;
  for (; mode < 2; mode++) {
    int total = 0;
    for (int t = 0; t < EX_TAB_COUNT; t++) {
      total += ex_tab_width(ui, t, mode) + 3;
    }
    if (total <= ui->columns) {
      break;
    }
  }
  for (int t = 0; t < EX_TAB_COUNT; t++) {
    const ExTab *tab = &ui->report->tabs[t];
    char number[8];
    char count[24];
    int active = t == ui->tab;
    int start = x;
    snprintf(number, sizeof(number), mode == 2 && !active ? "%d" : "%d ",
             t + 1);
    snprintf(count, sizeof(count), " %zu", tab->count);
    x += ex_screen_put(s, x, 1, ui->columns - x, number,
                       active ? EXA_ACCENT : EXA_QUIET);
    if (mode < 2 || active) {
      x += ex_screen_put(
          s, x, 1, ui->columns - x, tab->name,
          active ? EXA_BOLD | EXA_UNDERLINE : tab->count ? 0 : EXA_DIM);
    }
    if (mode == 0 && tab->count > 0) {
      x += ex_screen_put(s, x, 1, ui->columns - x, count, EXA_DIM);
    }
    ui->tab_x[t] = start;
    ui->tab_w[t] = x - start;
    x += mode == 2 ? 2 : 3;
  }
  for (int i = 0; i < ui->columns; i++) {
    ex_screen_put(s, i, 2, 1, ui->g->rule, EXA_QUIET);
  }
}

static void ex_column_widths(const ExUi *ui, int *widths, int available) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  int n = tab->column_count;
  int used = 0;
  for (int c = 0; c < n; c++) {
    widths[c] = ex_text_width(tab->columns[c]);
  }
  for (size_t i = 0; i < ui->visible_count; i++) {
    const ExEntry *e = &tab->entries[ui->visible[i]];
    for (int c = 0; c < n - 1; c++) {
      int w = ex_text_width(e->cells[c]);
      if (w > widths[c]) {
        widths[c] = w;
      }
    }
  }
  for (int c = 0; c < n - 1; c++) {
    int cap = c == 0 && (ui->tab == EX_TAB_FUNCTIONS || ui->tab == EX_TAB_PASSES)
                  ? 34
              : c == 1 && ui->tab == EX_TAB_OWNERSHIP ? 16
                                                      : 24;
    if (widths[c] > cap) {
      widths[c] = cap;
    }
    used += widths[c] + 2;
  }
  widths[n - 1] = available - used;
  if (widths[n - 1] < 8) {
    widths[n - 1] = 8;
  }
}

static void ex_draw_entry_row(ExUi *ui, ExScreen *s, int y, const ExEntry *e,
                              const int *widths, int selected) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  int x = ui->list_x + 2;
  int end = ui->list_x + ui->list_w;
  unsigned char base = e->tone == EX_TONE_DONE ? EXA_DIM : 0;
  int reversed = selected && ui->focus == 0;
  if (selected && !reversed) {
    ex_put(s, ui->list_x, y, 1, ui->g->marker, EXA_ACCENT | EXA_BOLD);
  }
  for (int c = 0; c < tab->column_count && x < end; c++) {
    unsigned char attr = base;
    char *cell = ex_fit(ui, e->cells[c], widths[c]);
    if (c == tab->column_count - 1 && e->tone == EX_TONE_FIXABLE) {
      attr = EXA_ACCENT;
    }
    if (selected && !reversed) {
      attr |= EXA_BOLD;
    }
    if (reversed) {
      attr = EXA_REVERSE;
    }
    ex_put(s, x, y, end - x, cell, attr);
    free(cell);
    x += widths[c] + 2;
  }
  if (reversed) {
    ex_screen_style(s, ui->list_x + 1, y, ui->list_w - 1, EXA_REVERSE);
  }
}

static int ex_list_first(const ExUi *ui) {
  return ui->list_y +
         (ui->report->tabs[ui->tab].summary_count == 0 ? 1 : 0);
}

static void ex_draw_columns(ExUi *ui, ExScreen *s, int y, const int *widths) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  int x = ui->list_x + 2;
  for (int c = 0; c < tab->column_count; c++) {
    ex_put(s, x, y, widths[c], tab->columns[c], EXA_QUIET);
    x += widths[c] + 2;
  }
}

static void ex_draw_summary(ExUi *ui, ExScreen *s, int y, size_t index) {
  const ExLine *line = &ui->summary.lines[index];
  for (int k = 0; k < line->count; k++) {
    int x = ui->list_x + 2 + line->seg[k].x;
    ex_put(s, x, y, ui->list_x + ui->list_w - 1 - x, line->seg[k].text,
           line->seg[k].attr);
  }
}

static void ex_draw_empty(ExUi *ui, ExScreen *s) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  ExLines lines = {0};
  int y = ui->list_y;
  int end = ui->list_y + ui->list_h;
  for (size_t i = 0; i < ui->summary.count && y < end; i++) {
    ex_draw_summary(ui, s, y++, i);
  }
  if (tab->summary_count > 0) {
    y++;
  }
  ex_wrap(&lines, 0, ui->list_w - 4, tab->empty, EXA_DIM, NULL, 0);
  for (size_t i = 0; i < lines.count && y < end; i++, y++) {
    for (int k = 0; k < lines.lines[i].count; k++) {
      ex_put(s, ui->list_x + 2, y, ui->list_w - 4, lines.lines[i].seg[k].text,
             lines.lines[i].seg[k].attr);
    }
  }
  ex_lines_clear(&lines);
  free(lines.lines);
}

static void ex_draw_list(ExUi *ui, ExScreen *s) {
  const ExTab *tab = &ui->report->tabs[ui->tab];
  int widths[EX_CELLS];
  int first = ex_list_first(ui);
  int height = ui->list_h - (first - ui->list_y);
  int selected_row;
  int *top = &ui->list_top[ui->tab];
  if (tab->count == 0) {
    ex_draw_empty(ui, s);
    return;
  }
  ex_column_widths(ui, widths, ui->list_w - 3);
  if (first > ui->list_y) {
    ex_draw_columns(ui, s, ui->list_y, widths);
  }
  selected_row = ex_row_of_selected(ui);
  if (selected_row >= 0) {
    int want_top = selected_row;
    if (want_top > 0 && ui->row_buf[want_top - 1].kind == EX_ROW_GROUP) {
      want_top--;
    }
    if (*top > want_top) {
      *top = want_top;
    }
    if (selected_row >= *top + height) {
      *top = selected_row - height + 1;
    }
  }
  if (*top > (int)ui->row_count - height) {
    *top = (int)ui->row_count - height;
  }
  if (*top < 0) {
    *top = 0;
  }
  for (int r = 0; r < height && *top + r < (int)ui->row_count; r++) {
    const ExRow *row = &ui->row_buf[*top + r];
    int y = first + r;
    if (row->kind == EX_ROW_SUMMARY) {
      ex_draw_summary(ui, s, y, (size_t)row->index);
    } else if (row->kind == EX_ROW_HEADER) {
      ex_draw_columns(ui, s, y, widths);
    } else if (row->kind == EX_ROW_GROUP) {
      const ExEntry *e = &tab->entries[row->index];
      char shown[160];
      ir_display_name(e->group, shown, sizeof(shown));
      ex_put(s, ui->list_x + 2, y, ui->list_w - 3, shown, EXA_BOLD);
    } else if (row->kind == EX_ROW_ENTRY) {
      ex_draw_entry_row(ui, s, y, &tab->entries[row->index], widths,
                        row->index == ui->selected[ui->tab]);
    }
  }
}

static int ex_detail_rows(const ExUi *ui) {
  return (int)ui->detail.count > ui->detail_h ? ui->detail_h - 1
                                              : ui->detail_h;
}

static void ex_draw_detail(ExUi *ui, ExScreen *s) {
  int shown = ex_detail_rows(ui);
  int max_top = (int)ui->detail.count - shown;
  if (ui->detail_top > max_top) {
    ui->detail_top = max_top;
  }
  if (ui->detail_top < 0) {
    ui->detail_top = 0;
  }
  for (int r = 0; r < shown; r++) {
    size_t index = (size_t)(ui->detail_top + r);
    const ExLine *line;
    if (index >= ui->detail.count) {
      break;
    }
    line = &ui->detail.lines[index];
    for (int k = 0; k < line->count; k++) {
      int x = ui->detail_x + line->seg[k].x;
      ex_put(s, x, ui->detail_y + r, ui->detail_x + ui->detail_w - x,
             line->seg[k].text, line->seg[k].attr);
    }
  }
  if (shown < ui->detail_h) {
    char more[48];
    int below = (int)ui->detail.count - ui->detail_top - shown;
    if (below > 0) {
      snprintf(more, sizeof(more), "%d more line%s", below,
               below == 1 ? "" : "s");
    } else {
      snprintf(more, sizeof(more), "end");
    }
    ex_put(s, ui->detail_x + ui->detail_w - ex_text_width(more),
           ui->detail_y + shown, ex_text_width(more), more, EXA_QUIET);
  }
}

static void ex_draw_frame(ExUi *ui, ExScreen *s) {
  if (ui->stacked) {
    int y = ui->detail_y - 1;
    for (int i = 0; i < ui->columns; i++) {
      ex_screen_put(s, i, y, 1, ui->g->rule, EXA_QUIET);
    }
  } else {
    int x = ui->list_x + ui->list_w + 1;
    for (int y = ui->list_y; y < ui->list_y + ui->list_h; y++) {
      ex_screen_put(s, x, y, 1, ui->g->vertical, EXA_QUIET);
    }
  }
}

static void ex_draw_footer(ExUi *ui, ExScreen *s) {
  int y = ui->rows - 1;
  char hints[256];
  char status[160];
  const ExTab *tab = &ui->report->tabs[ui->tab];
  status[0] = '\0';
  if (ui->typing) {
    snprintf(hints, sizeof(hints), "filter: %s%s", ui->filter, ui->g->cursor);
    ex_put(s, 1, y, ui->columns - 2, hints, EXA_BOLD);
    snprintf(status, sizeof(status), "enter keeps it  esc clears it");
    ex_put(s, ui->columns - ex_text_width(status) - 1, y,
           ex_text_width(status), status, EXA_QUIET);
    return;
  }
  {
    const char *full = ui->source_view == 2
                           ? "%s scroll  d back to the detail  s whole file  "
                             "esc back  ? help  q quit"
                       : ui->source_view ? "%s scroll  s back to the detail  "
                                           "d machine code  esc back  ? help  "
                                           "q quit"
                       : ui->focus == 1 ? "%s scroll  s whole file  d machine "
                                          "code  esc back  ? help  q quit"
                                        : "%s move  %s tabs  enter open  / "
                                          "filter  m missed  f fix  a all  "
                                          "q quit";
    int room = ui->columns - 2;
    if (ui->filter[0] || ui->quick[ui->tab]) {
      room -= 40;
    }
    snprintf(hints, sizeof(hints), full, ui->g->move, ui->g->tabs);
    if (ex_text_width(hints) > room) {
      snprintf(hints, sizeof(hints), "%s move  enter open  ? help  q quit",
               ui->g->move);
    }
    if (ex_text_width(hints) > room) {
      snprintf(hints, sizeof(hints), "? help  q quit");
    }
  }
  ex_put(s, 1, y, ui->columns - 2, hints, EXA_QUIET);
  if (ui->filter[0] || ui->quick[ui->tab]) {
    size_t n = 0;
    if (ui->filter[0]) {
      n += (size_t)snprintf(status + n, sizeof(status) - n, "\"%s\"  ",
                            ui->filter);
    }
    if (ui->quick[ui->tab] && n < sizeof(status)) {
      n += (size_t)snprintf(status + n, sizeof(status) - n, "%s  ",
                            ex_quick_names[ui->quick[ui->tab]]);
    }
    if (n < sizeof(status)) {
      snprintf(status + n, sizeof(status) - n, "%zu of %zu",
               ui->visible_count, tab->count);
    }
    ex_put(s, ui->columns - ex_text_width(status) - 1, y,
           ex_text_width(status), status, EXA_ACCENT);
  }
}

static void ex_draw_help(ExUi *ui, ExScreen *s) {
  static const char *const rows[][2] = {
      {"left right  tab", "switch tabs"},
      {"1 to 8", "jump to a tab"},
      {"up down  j k", "move, or scroll the detail"},
      {"pgup pgdn  home end", "page, first, last"},
      {"enter", "open the detail, or follow a link"},
      {"s", "the whole source file, at this line"},
      {"d", "the machine code of this function, at this line"},
      {"e", "this function as the compiler rewrote it, every drop shown"},
      {"esc  backspace", "back"},
      {"/", "filter by any text in the entry"},
      {"m  f  a", "missed only, with a fix only, all"},
      {"mouse", "wheel scrolls, click selects"},
      {"q", "quit"},
  };
  int count = (int)(sizeof(rows) / sizeof(rows[0]));
  int key_w = 0;
  int text_w = 0;
  int w;
  int h = count + 4;
  int x0;
  int y0;
  for (int i = 0; i < count; i++) {
    int k = ex_text_width(rows[i][0]);
    int t = ex_text_width(rows[i][1]);
    key_w = k > key_w ? k : key_w;
    text_w = t > text_w ? t : text_w;
  }
  w = key_w + text_w + 9;
  if (w > ui->columns) {
    w = ui->columns;
  }
  x0 = (ui->columns - w) / 2;
  y0 = (ui->rows - h) / 2;
  if (y0 < 0) {
    y0 = 0;
  }
  for (int y = 0; y < h; y++) {
    ex_screen_fill(s, x0, y0 + y, w, ' ', 0);
    ex_screen_put(s, x0, y0 + y, 1, ui->g->vertical, EXA_QUIET);
    ex_screen_put(s, x0 + w - 1, y0 + y, 1, ui->g->vertical, EXA_QUIET);
  }
  for (int i = 0; i < w; i++) {
    ex_screen_put(s, x0 + i, y0, 1, ui->g->rule, EXA_QUIET);
    ex_screen_put(s, x0 + i, y0 + h - 1, 1, ui->g->rule, EXA_QUIET);
  }
  ex_screen_put(s, x0, y0, 1, ui->g->corner[0], EXA_QUIET);
  ex_screen_put(s, x0 + w - 1, y0, 1, ui->g->corner[1], EXA_QUIET);
  ex_screen_put(s, x0, y0 + h - 1, 1, ui->g->corner[2], EXA_QUIET);
  ex_screen_put(s, x0 + w - 1, y0 + h - 1, 1, ui->g->corner[3], EXA_QUIET);
  ex_put(s, x0 + 3, y0 + 1, w - 6, "Keys", EXA_BOLD);
  for (int i = 0; i < count; i++) {
    ex_put(s, x0 + 3, y0 + 2 + i, key_w, rows[i][0], EXA_ACCENT);
    ex_put(s, x0 + 6 + key_w, y0 + 2 + i, w - key_w - 9, rows[i][1], 0);
  }
}

static void ex_draw(ExUi *ui, ExScreen *s) {
  ex_screen_clear(s);
  if (ui->columns < 40 || ui->rows < 12) {
    const char *msg = "needs 40 by 12";
    int w = ex_text_width(msg);
    ex_put(s, ui->columns > w ? (ui->columns - w) / 2 : 0, ui->rows / 2,
           ui->columns, msg, EXA_DIM);
    return;
  }
  ex_layout(ui);
  ex_compute_visible(ui);
  ex_settle_selection(ui);
  ex_build_rows(ui);
  ex_build_detail(ui);
  ex_draw_header(ui, s);
  ex_draw_tabs(ui, s);
  ex_draw_list(ui, s);
  ex_draw_frame(ui, s);
  ex_draw_detail(ui, s);
  ex_draw_footer(ui, s);
  if (ui->help) {
    ex_draw_help(ui, s);
  }
}

static void ex_switch_tab(ExUi *ui, int tab) {
  if (tab < 0) {
    tab = EX_TAB_COUNT - 1;
  }
  if (tab >= EX_TAB_COUNT) {
    tab = 0;
  }
  ui->tab = tab;
  ui->focus = 0;
  ui->source_view = 0;
  ui->detail_top = 0;
}

static void ex_move(ExUi *ui, int delta) {
  int pos;
  ex_compute_visible(ui);
  ex_settle_selection(ui);
  if (ui->visible_count == 0) {
    return;
  }
  pos = ex_position(ui);
  if (pos < 0) {
    pos = 0;
  }
  pos += delta;
  if (pos < 0) {
    pos = 0;
  }
  if (pos >= (int)ui->visible_count) {
    pos = (int)ui->visible_count - 1;
  }
  if (ui->selected[ui->tab] != ui->visible[pos]) {
    ui->selected[ui->tab] = ui->visible[pos];
    ui->detail_top = 0;
    ui->source_view = 0;
  }
}

static void ex_scroll(ExUi *ui, int delta) {
  long long top = (long long)ui->detail_top + delta;
  ui->detail_top = top < 0 ? 0 : top > 1000000 ? 1000000 : (int)top;
}

static int ex_page(const ExUi *ui) {
  int page = (ui->focus ? ui->detail_h : ui->list_h) - 2;
  return page > 1 ? page : 1;
}

static void ex_step(ExUi *ui, int delta) {
  if (ui->focus) {
    ex_scroll(ui, delta);
  } else {
    ex_move(ui, delta);
  }
}

static void ex_open(ExUi *ui) {
  const ExEntry *entry = ex_current(ui);
  if (!entry) {
    return;
  }
  if (entry->link_tab >= 0 && entry->link_entry >= 0) {
    int target = entry->link_tab;
    int index = entry->link_entry;
    ex_switch_tab(ui, target);
    ui->selected[target] = index;
    if (!ex_matches(ui, &ui->report->tabs[target].entries[index], target)) {
      ui->quick[target] = 0;
      ui->filter[0] = '\0';
    }
    return;
  }
  ui->focus = 1;
}

static void ex_toggle_source(ExUi *ui) {
  const ExEntry *shown = ex_shown(ui, ex_current(ui));
  if (!shown || ui->report->line_count == 0) {
    return;
  }
  ui->source_view = ui->source_view == 1 ? 0 : 1;
  ui->focus = 1;
  ui->detail_top = ui->source_view
                       ? (shown->line > 0 ? shown->line - 1 - ui->detail_h / 3
                                          : 0)
                       : 0;
  if (ui->detail_top < 0) {
    ui->detail_top = 0;
  }
}

static void ex_toggle_machine(ExUi *ui) {
  const ExEntry *shown = ex_shown(ui, ex_current(ui));
  if (!shown || ui->report->machine_count == 0) {
    return;
  }
  ui->source_view = ui->source_view == 2 ? 0 : 2;
  ui->focus = 1;
  ui->detail_top = 0;
  ui->anchor_pending = ui->source_view == 2;
}

static void ex_toggle_expanded(ExUi *ui) {
  const ExEntry *shown = ex_shown(ui, ex_current(ui));
  if (!shown || ui->report->expanded_count == 0) {
    return;
  }
  ui->source_view = ui->source_view == 3 ? 0 : 3;
  ui->focus = 1;
  ui->detail_top = 0;
  ui->anchor_pending = ui->source_view == 3;
}

static void ex_back(ExUi *ui) {
  if (ui->source_view) {
    ui->source_view = 0;
    ui->detail_top = 0;
  } else if (ui->focus == 1) {
    ui->focus = 0;
  } else if (ui->filter[0]) {
    ui->filter[0] = '\0';
  } else if (ui->quick[ui->tab]) {
    ui->quick[ui->tab] = 0;
  }
}

static int ex_row_at(const ExUi *ui, int y) {
  int r = y - ex_list_first(ui) + ui->list_top[ui->tab];
  if (y < ex_list_first(ui) || y >= ui->list_y + ui->list_h || r < 0 ||
      r >= (int)ui->row_count) {
    return -1;
  }
  return r;
}

static int ex_in_detail(const ExUi *ui, int x, int y) {
  return y >= ui->detail_y && y < ui->detail_y + ui->detail_h &&
         x >= ui->detail_x - 1 && (ui->stacked || x > ui->list_w);
}

static void ex_mouse(ExUi *ui, const ExKey *key) {
  if (key->kind == EXK_WHEEL_UP || key->kind == EXK_WHEEL_DOWN) {
    int delta = key->kind == EXK_WHEEL_UP ? -3 : 3;
    if (ex_in_detail(ui, key->x, key->y)) {
      ex_scroll(ui, delta);
    } else {
      ex_move(ui, delta);
    }
    return;
  }
  if (key->y == 1) {
    for (int t = 0; t < EX_TAB_COUNT; t++) {
      if (key->x >= ui->tab_x[t] && key->x < ui->tab_x[t] + ui->tab_w[t]) {
        ex_switch_tab(ui, t);
        return;
      }
    }
    return;
  }
  if (ex_in_detail(ui, key->x, key->y)) {
    ui->focus = 1;
    return;
  }
  {
    int r = ex_row_at(ui, key->y);
    if (r >= 0 && ui->row_buf[r].kind == EX_ROW_ENTRY) {
      if (ui->selected[ui->tab] == ui->row_buf[r].index && ui->focus == 0) {
        ex_open(ui);
      } else {
        ui->selected[ui->tab] = ui->row_buf[r].index;
        ui->focus = 0;
        ui->source_view = 0;
        ui->detail_top = 0;
      }
    }
  }
}

static void ex_typing(ExUi *ui, const ExKey *key) {
  size_t n = strlen(ui->filter);
  switch (key->kind) {
  case EXK_ENTER:
    ui->typing = 0;
    return;
  case EXK_ESCAPE:
    ui->typing = 0;
    ui->filter[0] = '\0';
    return;
  case EXK_BACKSPACE:
    while (n > 0 && ((unsigned char)ui->filter[n - 1] & 0xC0) == 0x80) {
      n--;
    }
    if (n > 0) {
      n--;
    }
    ui->filter[n] = '\0';
    return;
  case EXK_CHAR:
    if (key->code >= 0x20 && key->code < 0x80 && n + 1 < sizeof(ui->filter)) {
      ui->filter[n] = (char)key->code;
      ui->filter[n + 1] = '\0';
    }
    return;
  default:
    return;
  }
}

static void ex_key_char(ExUi *ui, unsigned code) {
  switch (code) {
  case 'q':
  case 3:
    ui->quit = 1;
    return;
  case '?':
    ui->help = 1;
    return;
  case '/':
    ui->typing = 1;
    ui->filter[0] = '\0';
    ui->focus = 0;
    return;
  case 'j':
  case 'k':
    ex_step(ui, code == 'j' ? 1 : -1);
    return;
  case 'g':
  case 'G':
    ex_step(ui, code == 'G' ? 1000000 : -1000000);
    return;
  case 'h':
  case 'l':
    ex_switch_tab(ui, ui->tab + (code == 'l' ? 1 : -1));
    return;
  case 'm':
  case 'f': {
    int mode = code == 'm' ? 1 : 2;
    ui->quick[ui->tab] = ui->quick[ui->tab] == mode ? 0 : mode;
    ui->focus = 0;
    return;
  }
  case 'a':
    ui->quick[ui->tab] = 0;
    ui->filter[0] = '\0';
    return;
  case 's':
    ex_toggle_source(ui);
    return;
  case 'd':
    ex_toggle_machine(ui);
    return;
  case 'e':
    ex_toggle_expanded(ui);
    return;
  case ' ':
    ex_step(ui, ex_page(ui));
    return;
  default:
    if (code >= '1' && code < '1' + EX_TAB_COUNT) {
      ex_switch_tab(ui, (int)(code - '1'));
    }
    return;
  }
}

static void ex_key(ExUi *ui, const ExKey *key) {
  if (ui->typing) {
    ex_typing(ui, key);
    return;
  }
  if (ui->help) {
    if (key->kind == EXK_CHAR && (key->code == 'q' || key->code == 3)) {
      ui->quit = 1;
    }
    if (key->kind != EXK_RESIZE) {
      ui->help = 0;
    }
    return;
  }
  switch (key->kind) {
  case EXK_CHAR:
    ex_key_char(ui, key->code);
    return;
  case EXK_UP:
    ex_step(ui, -1);
    return;
  case EXK_DOWN:
    ex_step(ui, 1);
    return;
  case EXK_PAGE_UP:
    ex_step(ui, -ex_page(ui));
    return;
  case EXK_PAGE_DOWN:
    ex_step(ui, ex_page(ui));
    return;
  case EXK_HOME:
    ex_step(ui, -1000000);
    return;
  case EXK_END:
    ex_step(ui, 1000000);
    return;
  case EXK_LEFT:
  case EXK_BACKTAB:
    ex_switch_tab(ui, ui->tab - 1);
    return;
  case EXK_RIGHT:
  case EXK_TAB:
    ex_switch_tab(ui, ui->tab + 1);
    return;
  case EXK_ENTER:
    if (ui->focus == 0) {
      ex_open(ui);
    }
    return;
  case EXK_ESCAPE:
  case EXK_BACKSPACE:
    ex_back(ui);
    return;
  case EXK_WHEEL_UP:
  case EXK_WHEEL_DOWN:
  case EXK_CLICK:
    ex_mouse(ui, key);
    return;
  default:
    return;
  }
}

static int ex_first_tab(const ExReport *report) {
  if (report->tabs[EX_TAB_OVERVIEW].count > 0) {
    return EX_TAB_OVERVIEW;
  }
  return report->tabs[EX_TAB_OPTIMIZER].count > 0 ? EX_TAB_OPTIMIZER
                                                  : EX_TAB_OVERVIEW;
}

int ex_tui_run(ExReport *report, ExTerm *term) {
  ExUi ui;
  ExScreen screen = {0, 0, NULL};
  memset(&ui, 0, sizeof(ui));
  ui.report = report;
  ui.term = term;
  ui.g = ex_term_unicode(term) ? &EX_UNICODE : &EX_ASCII;
  for (int t = 0; t < EX_TAB_COUNT; t++) {
    ui.selected[t] = report->tabs[t].count ? 0 : -1;
  }
  ui.tab = ex_first_tab(report);
  while (!ui.quit) {
    ExKey key;
    int got;
    ex_term_size(term, &ui.columns, &ui.rows);
    if (screen.columns != ui.columns || screen.rows != ui.rows) {
      ex_screen_free(&screen);
      if (!ex_screen_init(&screen, ui.columns, ui.rows)) {
        break;
      }
    }
    ex_draw(&ui, &screen);
    ex_term_present(term, &screen);
    do {
      got = ex_term_read(term, &key, 100);
    } while (got == 0);
    if (got < 0) {
      break;
    }
    ex_key(&ui, &key);
  }
  ex_screen_free(&screen);
  ex_lines_clear(&ui.detail);
  free(ui.detail.lines);
  ex_lines_clear(&ui.summary);
  free(ui.summary.lines);
  free(ui.row_buf);
  free(ui.visible);
  return 0;
}

void ex_report_print(const ExReport *report, FILE *out) {
  fprintf(out, "explain  %s\n",
          report->source_path ? report->source_path
          : report->source_name ? report->source_name
                                : "");
  for (int t = 0; t < EX_TAB_COUNT; t++) {
    const ExTab *tab = &report->tabs[t];
    const char *group = NULL;
    int grouped = 0;
    fprintf(out, "\n%s\n", tab->name);
    for (size_t i = 0; i < tab->summary_count; i++) {
      fprintf(out, "  %s\n", tab->summary[i]);
    }
    if (tab->count == 0) {
      if (tab->summary_count == 0) {
        fprintf(out, "  %s\n", tab->empty);
      }
      continue;
    }
    for (size_t i = 0; i < tab->count; i++) {
      const ExEntry *e = &tab->entries[i];
      if (e->group && (!grouped || !ex_same(group, e->group))) {
        char shown[160];
        ir_display_name(e->group, shown, sizeof(shown));
        fprintf(out, "  %s\n", shown);
        group = e->group;
        grouped = 1;
      }
      fprintf(out, "    ");
      for (int c = 0; c < tab->column_count; c++) {
        if (e->cells[c] && e->cells[c][0]) {
          fprintf(out, "%s%s", c ? "  " : "", e->cells[c]);
        }
      }
      fputc('\n', out);
      for (size_t f = 0; f < e->field_count; f++) {
        int repeated = 0;
        for (int c = 0; c < tab->column_count; c++) {
          repeated |= ex_same(e->cells[c], e->fields[f].text);
        }
        if (!repeated) {
          fprintf(out, "      %s: %s\n",
                  e->fields[f].label ? e->fields[f].label : "",
                  e->fields[f].text);
        }
      }
    }
  }
}
