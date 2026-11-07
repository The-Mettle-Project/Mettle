#include "explain_model.h"
#include "../error/error_explain.h"
#include "../ir/ir.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *ex_strdup(const char *text) {
  size_t n;
  char *copy;
  if (!text) {
    return NULL;
  }
  n = strlen(text);
  copy = malloc(n + 1);
  if (copy) {
    memcpy(copy, text, n + 1);
  }
  return copy;
}

static char *ex_fmt(const char *format, ...) {
  va_list args;
  int needed;
  char *out;
  va_start(args, format);
  needed = vsnprintf(NULL, 0, format, args);
  va_end(args);
  if (needed < 0) {
    return NULL;
  }
  out = malloc((size_t)needed + 1);
  if (!out) {
    return NULL;
  }
  va_start(args, format);
  vsnprintf(out, (size_t)needed + 1, format, args);
  va_end(args);
  return out;
}

static const char *ex_shown(const char *name, char *buf, size_t cap) {
  ir_display_name(name ? name : "?", buf, cap);
  return buf;
}

static char *ex_shown_entity(const char *entity) {
  char inner[160];
  char shown[192];
  size_t len = entity ? strlen(entity) : 0;
  if (len < 11 || strncmp(entity, "call to `", 9) != 0 ||
      entity[len - 1] != '`' || len - 10 >= sizeof(inner)) {
    return ex_strdup(entity);
  }
  memcpy(inner, entity + 9, len - 10);
  inner[len - 10] = '\0';
  return ex_fmt("call to `%s`", ex_shown(inner, shown, sizeof(shown)));
}

static char *ex_spaced(const char *text) {
  char *copy = ex_strdup(text);
  for (char *p = copy; p && *p; p++) {
    if (*p == '-' || *p == '_') {
      *p = ' ';
    }
  }
  return copy;
}

static ExEntry *ex_add(ExTab *tab, const char *group) {
  ExEntry *entry;
  if (tab->count == tab->capacity) {
    size_t capacity = tab->capacity ? tab->capacity * 2 : 32;
    ExEntry *grown = realloc(tab->entries, capacity * sizeof(ExEntry));
    if (!grown) {
      return NULL;
    }
    tab->entries = grown;
    tab->capacity = capacity;
  }
  entry = &tab->entries[tab->count++];
  memset(entry, 0, sizeof(*entry));
  entry->group = ex_strdup(group);
  entry->link_tab = -1;
  entry->link_entry = -1;
  return entry;
}

static void ex_field(ExEntry *entry, const char *label, const char *text,
                     int tone) {
  ExField *grown;
  if (!entry || !text || !text[0]) {
    return;
  }
  grown = realloc(entry->fields, (entry->field_count + 1) * sizeof(ExField));
  if (!grown) {
    return;
  }
  entry->fields = grown;
  entry->fields[entry->field_count].label = ex_strdup(label);
  entry->fields[entry->field_count].text = ex_strdup(text);
  entry->fields[entry->field_count].tone = tone;
  entry->field_count++;
}

static void ex_field_owned(ExEntry *entry, const char *label, char *text,
                           int tone) {
  ex_field(entry, label, text, tone);
  free(text);
}

static void ex_summary(ExTab *tab, char *line) {
  char **grown;
  if (!line) {
    return;
  }
  grown = realloc(tab->summary, (tab->summary_count + 1) * sizeof(char *));
  if (!grown) {
    free(line);
    return;
  }
  tab->summary = grown;
  tab->summary[tab->summary_count++] = line;
}

static void ex_append_lower(char **out, size_t *len, size_t *cap,
                            const char *text) {
  size_t n;
  if (!text) {
    return;
  }
  n = strlen(text);
  if (*len + n + 2 > *cap) {
    size_t capacity = *cap ? *cap : 128;
    char *grown;
    while (*len + n + 2 > capacity) {
      capacity *= 2;
    }
    grown = realloc(*out, capacity);
    if (!grown) {
      return;
    }
    *out = grown;
    *cap = capacity;
  }
  for (size_t i = 0; i < n; i++) {
    (*out)[(*len)++] = (char)tolower((unsigned char)text[i]);
  }
  (*out)[(*len)++] = '\n';
  (*out)[*len] = '\0';
}

static void ex_finish(ExEntry *entry) {
  char *search = NULL;
  size_t len = 0;
  size_t cap = 0;
  if (!entry) {
    return;
  }
  ex_append_lower(&search, &len, &cap, entry->group);
  for (int i = 0; i < EX_CELLS; i++) {
    ex_append_lower(&search, &len, &cap, entry->cells[i]);
  }
  ex_append_lower(&search, &len, &cap, entry->title);
  ex_append_lower(&search, &len, &cap, entry->code);
  for (size_t i = 0; i < entry->field_count; i++) {
    ex_append_lower(&search, &len, &cap, entry->fields[i].text);
  }
  entry->search = search;
}

static void ex_entry_free(ExEntry *e) {
  free(e->group);
  for (int c = 0; c < EX_CELLS; c++) {
    free(e->cells[c]);
  }
  free(e->title);
  for (size_t f = 0; f < e->field_count; f++) {
    free(e->fields[f].label);
    free(e->fields[f].text);
  }
  free(e->fields);
  free(e->code);
  free(e->search);
  free(e->function);
}

static void ex_cycles(char *out, size_t cap, long long centi) {
  if (centi % 100 == 0) {
    snprintf(out, cap, "%lld", centi / 100);
  } else {
    snprintf(out, cap, "%lld.%02lld", centi / 100, centi % 100);
  }
}

static char *ex_words(const char *key) {
  size_t n = key ? strlen(key) : 0;
  char *out = malloc(n * 2 + 1);
  size_t o = 0;
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    char c = key[i];
    if (c == '-' || c == '_') {
      out[o++] = ' ';
    } else if (isupper((unsigned char)c) && o > 0) {
      out[o++] = ' ';
      out[o++] = (char)tolower((unsigned char)c);
    } else if (o == 0) {
      out[o++] = (char)toupper((unsigned char)c);
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
  return out;
}

static int ex_same_text(const char *a, const char *b) {
  return a == b || (a && b && strcmp(a, b) == 0);
}

static long long ex_function_line(const ExJson *root, const char *fn) {
  const ExJson *functions = ex_json_get(root, "functions");
  if (!fn) {
    return -1;
  }
  for (size_t i = 0; i < ex_json_count(functions); i++) {
    const ExJson *f = ex_json_at(functions, i);
    if (ex_same_text(ex_json_text(f, "fn"), fn)) {
      return ex_json_int(f, "line", -1);
    }
  }
  return -1;
}

typedef struct {
  long long rank;
  const char *group;
  int line;
  int order;
  size_t index;
} ExSortKey;

static int ex_key_order(const void *a, const void *b) {
  const ExSortKey *x = (const ExSortKey *)a;
  const ExSortKey *y = (const ExSortKey *)b;
  int named;
  if (x->rank != y->rank) {
    return x->rank < y->rank ? -1 : 1;
  }
  named = strcmp(x->group ? x->group : "", y->group ? y->group : "");
  if (named != 0) {
    return named;
  }
  if (x->line != y->line) {
    return x->line < y->line ? -1 : 1;
  }
  if (x->order != y->order) {
    return x->order < y->order ? -1 : 1;
  }
  return x->index < y->index ? -1 : x->index > y->index;
}

static void ex_order_groups(ExTab *tab, const ExJson *root,
                            const char *lead) {
  ExSortKey *keys;
  ExEntry *sorted;
  if (tab->count < 2) {
    return;
  }
  keys = malloc(tab->count * sizeof(*keys));
  sorted = malloc(tab->capacity * sizeof(*sorted));
  if (!keys || !sorted) {
    free(keys);
    free(sorted);
    return;
  }
  for (size_t i = 0; i < tab->count; i++) {
    const ExEntry *e = &tab->entries[i];
    long long rank = -1;
    if (!lead || !ex_same_text(e->group, lead)) {
      rank = ex_function_line(root, e->group);
      if (rank < 0) {
        rank = e->line;
        for (size_t j = 0; j < tab->count; j++) {
          if (ex_same_text(tab->entries[j].group, e->group) &&
              tab->entries[j].line < rank) {
            rank = tab->entries[j].line;
          }
        }
      }
    }
    keys[i].rank = rank;
    keys[i].group = e->group;
    keys[i].line = e->line;
    keys[i].order = e->order;
    keys[i].index = i;
  }
  qsort(keys, tab->count, sizeof(*keys), ex_key_order);
  for (size_t i = 0; i < tab->count; i++) {
    sorted[i] = tab->entries[keys[i].index];
  }
  free(tab->entries);
  tab->entries = sorted;
  free(keys);
}

static int ex_same_entry(const ExEntry *a, const ExEntry *b) {
  if (a->line != b->line || a->end_line != b->end_line ||
      a->tone != b->tone || a->flags != b->flags ||
      a->field_count != b->field_count || !ex_same_text(a->group, b->group) ||
      !ex_same_text(a->title, b->title) || !ex_same_text(a->code, b->code)) {
    return 0;
  }
  for (int c = 0; c < EX_CELLS; c++) {
    if (!ex_same_text(a->cells[c], b->cells[c])) {
      return 0;
    }
  }
  for (size_t f = 0; f < a->field_count; f++) {
    if (!ex_same_text(a->fields[f].label, b->fields[f].label) ||
        !ex_same_text(a->fields[f].text, b->fields[f].text)) {
      return 0;
    }
  }
  return 1;
}

static void ex_merge_repeats(ExTab *tab, const char *label) {
  size_t kept = 0;
  for (size_t i = 0; i < tab->count; i++) {
    size_t j = 0;
    while (j < kept && !ex_same_entry(&tab->entries[j], &tab->entries[i])) {
      j++;
    }
    if (j < kept) {
      tab->entries[j].repeat++;
      ex_entry_free(&tab->entries[i]);
      continue;
    }
    if (kept != i) {
      tab->entries[kept] = tab->entries[i];
    }
    tab->entries[kept].repeat = 1;
    kept++;
  }
  tab->count = kept;
  for (size_t i = 0; i < tab->count; i++) {
    if (tab->entries[i].repeat > 1) {
      ex_field_owned(&tab->entries[i], label,
                     ex_fmt("%d", tab->entries[i].repeat), EX_TONE_PLAIN);
    }
  }
}

static const char *ex_bound_words(const char *bound, char *out, size_t cap) {
  if (!bound) {
    return NULL;
  }
  if (bound[0] == 'p' && bound[1] >= '0' && bound[1] <= '9') {
    snprintf(out, cap, "port %s", bound + 1);
  } else if (strcmp(bound, "load") == 0) {
    snprintf(out, cap, "the load ports");
  } else if (strcmp(bound, "store") == 0) {
    snprintf(out, cap, "the store port");
  } else {
    snprintf(out, cap, "%s", bound);
  }
  return out;
}

static int ex_insn_is_vector(const ExInsn *insn) {
  return insn->text && insn->text[0] == 'v' &&
         (strstr(insn->text, "ymm") || strstr(insn->text, "xmm"));
}

static long long ex_branch_target(const ExInsn *insn) {
  const char *at;
  if (!insn->text || insn->text[0] != 'j') {
    return -1;
  }
  at = strstr(insn->text, " 0x");
  return at ? strtoll(at + 3, NULL, 16) : -1;
}

typedef struct {
  int uops;
  int loads;
  int stores;
  int fp;
  int vint;
  int shuffle;
  int mul;
} ExPressure;

static int ex_starts(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

static const char *const EX_FUSING[] = {"cmp ", "test ", "add ", "sub ",
                                         "and ", "inc ",  "dec "};
static const char *const EX_PURE_LOADS[] = {"vmov", "mov", "vbroadcast",
                                            "vpbroadcast"};
static const char *const EX_FP_PORTS[] = {
    "vfm",  "vfnm",  "vadd",  "vsub",     "vmul",    "vmin",     "vmax", "vcvt",
    "vpsra", "vpsrl", "vpsll", "vpmaddwd", "vpmuldq", "vpmuludq", "vround",
    "vdiv"};
static const char *const EX_INT_PORTS[] = {
    "vpadd", "vpsub", "vpand", "vpor",  "vpxor", "vpmin",   "vpmax",
    "vpcmp", "vpabs", "vxorp", "vandp", "vorp",  "vpblend", "vblend"};

static int ex_starts_any(const char *text, const char *const *list,
                         size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (ex_starts(text, list[i])) {
      return 1;
    }
  }
  return 0;
}

#define EX_STARTS_ANY(text, list) \
  ex_starts_any(text, list, sizeof(list) / sizeof(list[0]))

static void ex_price_vector(const char *text, ExPressure *p) {
  if (EX_STARTS_ANY(text, EX_FP_PORTS)) {
    p->fp++;
  } else if (ex_starts(text, "vpmulld")) {
    p->mul += 2;
  } else if (EX_STARTS_ANY(text, EX_INT_PORTS)) {
    p->vint++;
  } else if (!ex_starts(text, "vmov") && !ex_starts(text, "vzero")) {
    p->shuffle++;
  }
}

static void ex_price(const char *text, ExPressure *p, int *fused) {
  const char *comma = strchr(text, ',');
  const char *bracket = strchr(text, '[');
  int store = bracket && comma && bracket < comma;
  if (*fused && text[0] == 'j') {
    *fused = 0;
    return;
  }
  *fused = EX_STARTS_ANY(text, EX_FUSING);
  p->uops++;
  if (store) {
    p->stores++;
    if (!ex_starts(text, "vmov") && !ex_starts(text, "mov")) {
      p->loads++;
      p->uops++;
    }
    return;
  }
  if (bracket) {
    p->loads++;
    if (EX_STARTS_ANY(text, EX_PURE_LOADS)) {
      return;
    }
  }
  if (text[0] == 'v') {
    ex_price_vector(text, p);
  }
}

static const char *ex_pressure_bound(const ExPressure *p, int *centi) {
  struct {
    const char *name;
    int centi;
  } limits[6] = {
      {"the front end, four instructions a cycle", p->uops * 25},
      {"the load ports", p->loads * 50},
      {"the store port", p->stores * 100},
      {"ports 0 and 1", (p->fp + p->mul) * 50},
      {"ports 0, 1 and 5", (p->fp + p->mul + p->vint + p->shuffle) * 100 / 3},
      {"port 5", p->shuffle * 100},
  };
  int best = 0;
  for (int i = 1; i < 6; i++) {
    if (limits[i].centi > limits[best].centi) {
      best = i;
    }
  }
  *centi = limits[best].centi;
  return limits[best].name;
}

static int ex_headline_lanes(const char *headline, int *element_bytes) {
  const char *wide = headline ? strstr(headline, "-wide ") : NULL;
  const char *start;
  const char *type;
  int lanes;
  if (!wide) {
    return 0;
  }
  start = wide;
  while (start > headline && start[-1] >= '0' && start[-1] <= '9') {
    start--;
  }
  lanes = atoi(start);
  type = wide + 6;
  *element_bytes = ex_starts(type, "float64") || ex_starts(type, "int64") ? 8
                   : ex_starts(type, "float32") || ex_starts(type, "int32") ||
                           ex_starts(type, "uint32")
                       ? 4
                   : ex_starts(type, "int16") || ex_starts(type, "uint16") ? 2
                   : ex_starts(type, "byte") || ex_starts(type, "int8") ||
                           ex_starts(type, "uint8")
                       ? 1
                       : 0;
  return lanes;
}

static void ex_vector_cost(ExEntry *entry, const ExMachineFn *code,
                           long long from, long long to,
                           const char *headline) {
  ExPressure p;
  int fused = 0;
  int centi = 0;
  int element_bytes = 0;
  int lanes = ex_headline_lanes(headline, &element_bytes);
  long long advance = 0;
  const char *bound;
  char cycles[24];
  memset(&p, 0, sizeof(p));
  for (size_t k = 0; k < code->count; k++) {
    const ExInsn *insn = &code->insns[k];
    if (!insn->kernel || insn->label || insn->offset < from ||
        insn->offset > to || !insn->text) {
      continue;
    }
    ex_price(insn->text, &p, &fused);
    if (ex_starts(insn->text, "add r") && strchr(insn->text, ',')) {
      long long step = atoll(strchr(insn->text, ',') + 1);
      if (step > advance) {
        advance = step;
      }
    }
  }
  bound = ex_pressure_bound(&p, &centi);
  if (centi <= 0) {
    return;
  }
  ex_cycles(cycles, sizeof(cycles), centi);
  if (lanes > 0 && element_bytes > 0 && advance == (long long)lanes *
                                                       element_bytes) {
    char each[24];
    ex_cycles(each, sizeof(each), (centi + lanes / 2) / lanes);
    ex_field_owned(entry, "Vector cost",
                   ex_fmt("about %s cycles per pass, bound by %s; a pass "
                          "covers %d elements, about %s cycles each",
                          cycles, bound, lanes, each),
                   EX_TONE_DONE);
  } else {
    ex_field_owned(entry, "Vector cost",
                   ex_fmt("about %s cycles per pass, bound by %s", cycles,
                          bound),
                   EX_TONE_DONE);
  }
}

static void ex_vector_body(ExEntry *entry, const ExReport *report,
                           const char *fn, int line, int end_line,
                           const char *headline) {
  const ExMachineFn *code = ex_report_machine(report, fn);
  long long best_from = -1;
  long long best_to = -1;
  int best_vector = 0;
  if (!code) {
    return;
  }
  for (size_t i = 0; i < code->count; i++) {
    const ExInsn *jump = &code->insns[i];
    long long target = ex_branch_target(jump);
    int vector = 0;
    if (!jump->kernel || jump->line < line || jump->line > end_line ||
        target < 0 || target >= jump->offset) {
      continue;
    }
    for (size_t k = 0; k < code->count; k++) {
      const ExInsn *insn = &code->insns[k];
      if (insn->kernel && insn->offset >= target &&
          insn->offset <= jump->offset && ex_insn_is_vector(insn)) {
        vector += strstr(insn->text, "ymm") ? 2 : 1;
      }
    }
    if (vector > best_vector) {
      best_vector = vector;
      best_from = target;
      best_to = jump->offset;
    }
  }
  if (best_vector == 0) {
    return;
  }
  {
    char list[512];
    char names[32][24];
    int uses[32];
    int kinds = 0;
    size_t n = 0;
    int count = 0;
    int vector = 0;
    list[0] = '\0';
    for (size_t k = 0; k < code->count; k++) {
      const ExInsn *insn = &code->insns[k];
      size_t word = 0;
      int known = 0;
      if (!insn->kernel || insn->offset < best_from ||
          insn->offset > best_to || !insn->text) {
        continue;
      }
      count++;
      vector += ex_insn_is_vector(insn);
      while (insn->text[word] && insn->text[word] != ' ' && word < 23) {
        word++;
      }
      while (known < kinds && (strlen(names[known]) != word ||
                               strncmp(names[known], insn->text, word) != 0)) {
        known++;
      }
      if (known == kinds && kinds < 32) {
        memcpy(names[kinds], insn->text, word);
        names[kinds][word] = '\0';
        uses[kinds++] = 0;
      }
      if (known < kinds) {
        uses[known]++;
      }
      if (n + word + 3 < sizeof(list)) {
        n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%.*s",
                              n ? ", " : "", (int)word, insn->text);
      }
    }
    if (count > 12) {
      n = 0;
      for (int k = 0; k < kinds && n + 32 < sizeof(list); k++) {
        if (uses[k] > 1) {
          n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%d %s",
                                n ? ", " : "", uses[k], names[k]);
        } else {
          n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%s",
                                n ? ", " : "", names[k]);
        }
      }
    }
    ex_field_owned(entry, "Vector body",
                   ex_fmt("%d instructions per pass, %d of them vector: %s",
                          count, vector, list),
                   EX_TONE_DONE);
    ex_vector_cost(entry, code, best_from, best_to, headline);
  }
}

static void ex_loop_cost_field(ExEntry *entry, const ExJson *root,
                               const char *fn, long long line) {
  const ExJson *loops = ex_json_get(root, "loops");
  for (size_t i = 0; i < ex_json_count(loops); i++) {
    const ExJson *loop = ex_json_at(loops, i);
    char cycles[24];
    char bound[48];
    if (!ex_same_text(ex_json_text(loop, "fn"), fn) ||
        ex_json_int(loop, "line", 0) != line) {
      continue;
    }
    if (ex_json_flag(loop, "hasKernel")) {
      return;
    }
    ex_cycles(cycles, sizeof(cycles), ex_json_int(loop, "cyclesPerIter", 0));
    ex_field_owned(
        entry, "Machine loop",
        ex_fmt("%s cycles per iteration%s%s%s", cycles,
               ex_json_text(loop, "bottleneck") ? ", bound by " : "",
               ex_json_text(loop, "bottleneck")
                   ? ex_bound_words(ex_json_text(loop, "bottleneck"), bound,
                                    sizeof(bound))
                   : "",
               ex_json_flag(loop, "estimated") ? ", estimated" : ""),
        EX_TONE_PLAIN);
    return;
  }
}

static void ex_build_optimizer(ExReport *report, const ExJson *root) {
  ExTab *tab = &report->tabs[EX_TAB_OPTIMIZER];
  const ExJson *remarks = ex_json_get(root, "remarks");
  tab->name = "Optimizer";
  tab->columns[0] = "Line";
  tab->columns[1] = "What";
  tab->columns[2] = "Verdict";
  tab->column_count = 3;
  tab->empty = "No loops or calls in this file to report.";
  for (size_t i = 0; i < ex_json_count(remarks); i++) {
    const ExJson *r = ex_json_at(remarks, i);
    const char *fn = ex_json_text(r, "fn");
    const char *kind = ex_json_text(r, "kind");
    const char *entity = ex_json_text(r, "entity");
    const char *headline = ex_json_text(r, "headline");
    const char *fix = ex_json_text(r, "fix");
    const char *verified = ex_json_text(r, "verified");
    long long line = ex_json_int(r, "line", 0);
    long long end_line = ex_json_int(r, "endLine", 0);
    long long count = ex_json_int(r, "count", 1);
    int positive = ex_json_flag(r, "positive");
    int advisory = ex_json_flag(r, "advisory");
    int declined = ex_json_flag(r, "declined");
    const char *host = ex_json_text(r, "inlinedInto");
    char fn_shown[160];
    char host_shown[160];
    ExEntry *entry = ex_add(tab, fn ? fn : "?");
    if (!entry) {
      return;
    }
    ex_shown(fn, fn_shown, sizeof(fn_shown));
    entry->function = ex_strdup(fn);
    if (kind && strcmp(kind, "calls-folded") == 0) {
      long long last = ex_json_int(r, "lineEnd", line);
      entry->cells[0] = ex_fmt("%lld-%lld", line, last);
      entry->cells[1] = ex_fmt("%lld calls", count);
      entry->end_line = (int)last;
      entry->title = ex_fmt("%s, %lld calls on lines %lld to %lld", fn_shown,
                            count, line, last);
    } else {
      char *shown_entity = ex_shown_entity(entity ? entity : kind);
      entry->cells[0] = ex_fmt("%lld", line);
      entry->cells[1] = shown_entity;
      entry->end_line = (int)end_line;
      entry->title = ex_fmt("%s, %s at line %lld", fn_shown,
                            shown_entity ? shown_entity : "?", line);
    }
    entry->cells[2] = ex_strdup(headline);
    entry->line = (int)line;
    entry->code = ex_strdup(ex_json_text(r, "code"));
    if (positive || ex_json_flag(r, "trivial")) {
      entry->tone = EX_TONE_DONE;
    } else if (declined) {
      entry->tone = EX_TONE_PLAIN;
    } else {
      entry->tone = verified ? EX_TONE_FIXABLE : EX_TONE_MISSED;
      entry->flags |= EXF_MISSED;
    }
    if (!positive && fix && !advisory) {
      entry->flags |= EXF_FIXABLE;
    }
    ex_field(entry, "Verdict", headline,
             positive   ? EX_TONE_DONE
             : declined ? EX_TONE_PLAIN
                        : EX_TONE_MISSED);
    if (host) {
      ex_field(entry, "Inlined into", ex_shown(host, host_shown,
                                               sizeof(host_shown)),
               EX_TONE_PLAIN);
    }
    ex_field(entry, "Reason", ex_json_text(r, "reason"), EX_TONE_PLAIN);
    ex_field(entry, advisory ? "Note" : "Fix", fix, EX_TONE_PLAIN);
    ex_field(entry, "Verified", verified, EX_TONE_FIXABLE);
    ex_field(entry, "Still blocked", ex_json_text(r, "stillBlocked"),
             EX_TONE_MISSED);
    ex_field(entry, "Calls", ex_json_text(r, "calls"), EX_TONE_PLAIN);
    if (kind && strcmp(kind, "loop") == 0) {
      ex_loop_cost_field(entry, root, fn, line);
      if (positive) {
        ex_vector_body(entry, report, fn, (int)line,
                       end_line > line ? (int)end_line : (int)line, headline);
      }
    }
    {
      const ExJson *quantities = ex_json_get(r, "quantities");
      for (size_t q = 0; quantities && quantities->kind == EXJ_OBJECT &&
                         q < quantities->count;
           q++) {
        char *label = ex_words(quantities->keys[q]);
        char *value = ex_fmt("%lld", (long long)quantities->items[q]->number);
        ex_field(entry, label, value, EX_TONE_PLAIN);
        free(label);
        free(value);
      }
    }
    ex_finish(entry);
  }
  ex_order_groups(tab, root, NULL);
}

static int ex_find_remark(const ExTab *tab, const char *fn, long long line,
                          const char *code) {
  int fallback = -1;
  for (size_t i = 0; i < tab->count; i++) {
    const ExEntry *e = &tab->entries[i];
    if (e->line != line || !e->group || !fn || strcmp(e->group, fn) != 0) {
      continue;
    }
    if (!code || (e->code && strcmp(e->code, code) == 0)) {
      return (int)i;
    }
    if (fallback < 0) {
      fallback = (int)i;
    }
  }
  return fallback;
}

static void ex_count_ownership(const ExJson *root, long long *inserted,
                               long long *moves, long long *bits) {
  const ExJson *records = ex_json_get(root, "ownership");
  *inserted = 0;
  *moves = 0;
  *bits = 0;
  for (size_t i = 0; i < ex_json_count(records); i++) {
    const ExJson *r = ex_json_at(records, i);
    const char *kind = ex_json_text(r, "kind");
    if (!kind) {
      continue;
    }
    if (strcmp(kind, "move") == 0) {
      (*moves)++;
    } else if (strcmp(kind, "live-bit") == 0) {
      (*bits)++;
    } else if (ex_json_flag(r, "inserted") && strcmp(kind, "drop-runs") != 0) {
      (*inserted)++;
    }
  }
}

static int ex_find_loop_remark(const ExTab *tab, const char *fn, long long line) {
  int best = -1;
  long long best_score = 0;
  for (size_t i = 0; i < tab->count; i++) {
    const ExEntry *e = &tab->entries[i];
    int last = e->end_line > e->line ? e->end_line : e->line;
    long long score;
    if (!e->cells[1] || strcmp(e->cells[1], "loop") != 0 || line < e->line ||
        line > last) {
      continue;
    }
    score = (e->line == line ? 0 : 1000000) + (last - e->line) +
            (ex_same_text(e->function, fn) ? 0 : 100000);
    if (best < 0 || score < best_score) {
      best = (int)i;
      best_score = score;
    }
  }
  return best;
}

typedef struct {
  const char *fn;
  long long line;
  long long cost;
  long long cycles;
  const char *bound;
  int kernel;
  int copies;
} ExHotLoop;

static int ex_hot_order(const void *a, const void *b) {
  const ExHotLoop *x = (const ExHotLoop *)a;
  const ExHotLoop *y = (const ExHotLoop *)b;
  return x->cost > y->cost ? -1 : x->cost < y->cost;
}

static void ex_hot_loops(ExTab *tab, const ExTab *optimizer,
                         const ExJson *root) {
  const ExJson *loops = ex_json_get(root, "loops");
  size_t total = ex_json_count(loops);
  size_t count = 0;
  long long sum = 0;
  ExHotLoop *hot = total ? calloc(total, sizeof(*hot)) : NULL;
  if (!hot) {
    return;
  }
  for (size_t i = 0; i < total; i++) {
    const ExJson *loop = ex_json_at(loops, i);
    const char *fn = ex_json_text(loop, "fn");
    long long line = ex_json_int(loop, "line", 0);
    long long cycles = ex_json_int(loop, "cyclesPerIter", 0);
    long long depth = ex_json_int(loop, "depth", 0);
    long long trips = ex_json_int(loop, "trips", 0);
    long long cost = cycles;
    size_t k = 0;
    for (long long d = 0; d < depth && d < 3; d++) {
      cost *= 10;
    }
    if (trips > 0) {
      cost = cost * trips / 10;
    }
    sum += cost;
    while (k < count && !(hot[k].line == line && ex_same_text(hot[k].fn, fn))) {
      k++;
    }
    if (k == count) {
      hot[count].fn = fn;
      hot[count].line = line;
      hot[count].cycles = cycles;
      hot[count].bound = ex_json_text(loop, "bottleneck");
      hot[count].kernel = ex_json_flag(loop, "hasKernel");
      count++;
    }
    hot[k].cost += cost;
    hot[k].copies++;
  }
  if (count > 1) {
    qsort(hot, count, sizeof(*hot), ex_hot_order);
  }
  for (size_t i = 0; i < count && i < 6 && sum > 0; i++) {
    const ExHotLoop *h = &hot[i];
    long long share = h->cost * 100 / sum;
    char cycles[24];
    char bound[48];
    int remark = ex_find_loop_remark(optimizer, h->fn, h->line);
    const ExEntry *linked = remark >= 0 ? &optimizer->entries[remark] : NULL;
    ExEntry *entry;
    if (h->cost == 0 || (share == 0 && i >= 3)) {
      break;
    }
    entry = ex_add(tab, "Hottest loops");
    if (!entry) {
      break;
    }
    ex_cycles(cycles, sizeof(cycles), h->cycles);
    entry->function = ex_strdup(h->fn);
    entry->cells[0] = ex_fmt("%lld%%", share);
    entry->cells[1] = ex_fmt("%s:%lld", h->fn ? h->fn : "?", h->line);
    {
      const char *title = NULL;
      const char *body = NULL;
      if (linked && linked->tone != EX_TONE_DONE && linked->code &&
          mettle_explain_lookup(linked->code, &title, &body)) {
        entry->cells[2] = ex_strdup(title);
      } else if (linked && linked->cells[2]) {
        entry->cells[2] = ex_strdup(linked->cells[2]);
      } else {
        entry->cells[2] =
            ex_strdup(h->kernel ? "runs a vector kernel" : "scalar");
      }
    }
    entry->title = ex_fmt("%s, loop at line %lld", h->fn ? h->fn : "?",
                          h->line);
    entry->line = (int)h->line;
    entry->tone = linked ? linked->tone : EX_TONE_PLAIN;
    entry->flags = linked ? linked->flags : 0;
    ex_field_owned(entry, "Share",
                   ex_fmt("%lld%% of the static loop cost: cycles per "
                          "iteration, times ten for each level of nesting",
                          share),
                   EX_TONE_PLAIN);
    if (!h->kernel) {
      ex_field_owned(entry, "Machine loop",
                     ex_fmt("%s cycles per iteration%s%s", cycles,
                            h->bound ? ", bound by " : "",
                            h->bound ? ex_bound_words(h->bound, bound,
                                                      sizeof(bound))
                                     : ""),
                     EX_TONE_PLAIN);
    }
    if (h->copies > 1) {
      ex_field_owned(entry, "Copies",
                     ex_fmt("%d, one for each place it was inlined",
                            h->copies),
                     EX_TONE_PLAIN);
    }
    if (linked) {
      entry->link_tab = EX_TAB_OPTIMIZER;
      entry->link_entry = remark;
    }
    ex_finish(entry);
  }
  free(hot);
}

static void ex_build_overview(ExReport *report, const ExJson *root) {
  ExTab *tab = &report->tabs[EX_TAB_OVERVIEW];
  const ExTab *optimizer = &report->tabs[EX_TAB_OPTIMIZER];
  const ExJson *stats = ex_json_get(root, "stats");
  const ExJson *backend = ex_json_get(root, "backend");
  const ExJson *safety = ex_json_get(root, "safety");
  const ExJson *start = ex_json_get(root, "startHere");
  const ExJson *changes = ex_json_get(root, "changes");
  const ExJson *entries = ex_json_get(changes, "entries");
  long long inserted = 0;
  long long moves = 0;
  long long bits = 0;
  const char *target = ex_json_text(backend, "target");
  tab->name = "Overview";
  tab->columns[0] = "";
  tab->columns[1] = "Where";
  tab->columns[2] = "What to do";
  tab->column_count = 3;
  tab->empty = "Nothing to start with: every loop and call this file has "
               "either optimized or has no fix to offer.";
  {
    long long fixes = ex_json_int(stats, "fixesVerified", 0);
    ex_summary(tab, ex_fmt("Loops      %lld vectorized, %lld scalar, %lld "
                           "%s proven",
                           ex_json_int(stats, "loopsVectorized", 0),
                           ex_json_int(stats, "loopsScalar", 0), fixes,
                           fixes == 1 ? "fix" : "fixes"));
  }
  ex_summary(tab, ex_fmt("Calls      %lld inlined, %lld kept",
                         ex_json_int(stats, "callsInlined", 0),
                         ex_json_int(stats, "callsRefused", 0)));
  if (target) {
    ex_summary(tab, ex_fmt("Backend    emitted through the %s backend",
                           target));
  } else {
    ex_summary(tab, ex_fmt("Backend    %lld of %lld functions "
                           "register-allocated",
                           ex_json_int(backend, "ok", 0),
                           ex_json_int(backend, "total", 0)));
  }
  ex_count_ownership(root, &inserted, &moves, &bits);
  if (ex_json_count(ex_json_get(root, "ownership")) == 0) {
    ex_summary(tab, ex_strdup("Ownership  nothing here owns a resource"));
  } else {
    ex_summary(tab, ex_fmt("Ownership  %lld inserted, %lld %s, %lld live "
                           "%s",
                           inserted, moves, moves == 1 ? "move" : "moves",
                           bits, bits == 1 ? "bit" : "bits"));
  }
  if (ex_json_flag(safety, "enabled")) {
    ex_summary(tab, ex_fmt("Safety     %lld of %lld accesses settled at "
                           "compile time",
                           ex_json_int(safety, "proved", 0) +
                               ex_json_int(safety, "hoisted", 0),
                           ex_json_int(safety, "accesses", 0)));
  }
  {
    long long beliefs = ex_json_int(root, "beliefsTotal", 0);
    ex_summary(tab, beliefs == 0
                        ? ex_strdup("Trust      nothing taken on trust")
                        : ex_fmt("Trust      %lld %s not checked", beliefs,
                                 beliefs == 1 ? "belief" : "beliefs"));
  }
  if (!ex_json_flag(changes, "baseline")) {
    ex_summary(tab, ex_strdup("Changes    first run, nothing to compare"));
  } else {
    ex_summary(tab, ex_fmt("Changes    %lld better, %lld worse since the "
                           "last run",
                           ex_json_int(stats, "changesImproved", 0),
                           ex_json_int(stats, "changesRegressed", 0)));
  }
  for (size_t i = 0; i < ex_json_count(start); i++) {
    const ExJson *s = ex_json_at(start, i);
    const char *fn = ex_json_text(s, "fn");
    long long line = ex_json_int(s, "line", 0);
    int proven = ex_json_flag(s, "proven");
    long long sites = ex_json_int(s, "sites", 1);
    ExEntry *entry = ex_add(tab, "Start here");
    if (!entry) {
      return;
    }
    entry->function = ex_strdup(fn);
    entry->cells[0] = ex_fmt("%zu", i + 1);
    entry->cells[1] = ex_fmt("%s:%lld", fn ? fn : "?", line);
    entry->cells[2] = ex_strdup(ex_json_text(s, "fix"));
    entry->title = ex_fmt("%s, line %lld", fn ? fn : "?", line);
    entry->line = (int)line;
    entry->code = ex_strdup(ex_json_text(s, "code"));
    entry->tone = proven ? EX_TONE_FIXABLE : EX_TONE_MISSED;
    entry->flags = EXF_MISSED | EXF_FIXABLE;
    ex_field(entry, "Fix", ex_json_text(s, "fix"), EX_TONE_PLAIN);
    ex_field(entry, "Proven",
             proven ? "applied to a copy of the function and re-checked: "
                      "the optimization then happens"
                    : NULL,
             EX_TONE_FIXABLE);
    ex_field(entry, "Still blocked",
             ex_json_flag(s, "stillBlocked")
                 ? "the fix helps, and something else still stands in the "
                   "way; the Optimizer tab says what"
                 : NULL,
             EX_TONE_MISSED);
    if (sites > 1) {
      ex_field_owned(entry, "Places", ex_fmt("%lld", sites), EX_TONE_PLAIN);
    }
    entry->link_tab = EX_TAB_OPTIMIZER;
    entry->link_entry =
        ex_find_remark(optimizer, fn, line, ex_json_text(s, "code"));
    ex_finish(entry);
  }
  ex_hot_loops(tab, optimizer, root);
  for (size_t i = 0; i < ex_json_count(entries); i++) {
    const ExJson *c = ex_json_at(entries, i);
    const char *fn = ex_json_text(c, "fn");
    const char *direction = ex_json_text(c, "direction");
    const char *kind = ex_json_text(c, "kind");
    long long line = ex_json_int(c, "line", 0);
    int regressed = direction && strcmp(direction, "regressed") == 0;
    ExEntry *entry = ex_add(tab, "Changed since the last run");
    if (!entry) {
      return;
    }
    entry->function = ex_strdup(fn);
    entry->cells[0] = ex_strdup(regressed ? "worse" : "better");
    entry->cells[1] = ex_fmt("%s:%lld", fn ? fn : "?", line);
    entry->cells[2] =
        ex_fmt("%s %s", kind ? kind : "?",
               regressed ? (kind && strcmp(kind, "loop") == 0
                                ? "was vectorized, now scalar"
                                : "was inlined, now a real call")
                         : (kind && strcmp(kind, "loop") == 0
                                ? "now vectorized"
                                : "now inlined"));
    entry->title = ex_fmt("%s, %s at line %lld", fn ? fn : "?",
                          kind ? kind : "?", line);
    entry->line = (int)line;
    entry->tone = regressed ? EX_TONE_MISSED : EX_TONE_DONE;
    ex_field(entry, "Change", entry->cells[2],
             regressed ? EX_TONE_MISSED : EX_TONE_DONE);
    ex_field(entry, "Reason", ex_json_text(c, "reason"), EX_TONE_PLAIN);
    entry->link_tab = EX_TAB_OPTIMIZER;
    entry->link_entry = ex_find_remark(optimizer, fn, line, NULL);
    ex_finish(entry);
  }
}

static void ex_build_ownership(ExReport *report, const ExJson *root) {
  ExTab *tab = &report->tabs[EX_TAB_OWNERSHIP];
  const ExJson *records = ex_json_get(root, "ownership");
  tab->name = "Ownership";
  tab->columns[0] = "Line";
  tab->columns[1] = "Record";
  tab->columns[2] = "Value";
  tab->column_count = 3;
  tab->empty = "Nothing in this file owns a resource, so the compiler "
               "inserted no drops and followed no moves.";
  for (size_t i = 0; i < ex_json_count(records); i++) {
    const ExJson *r = ex_json_at(records, i);
    const char *kind = ex_json_text(r, "kind");
    const char *subject = ex_json_text(r, "subject");
    long long line = ex_json_int(r, "line", 0);
    int inserted = ex_json_flag(r, "inserted");
    ExEntry *entry = ex_add(tab, ex_json_text(r, "function"));
    char *label = ex_spaced(kind ? kind : "?");
    if (!entry) {
      free(label);
      return;
    }
    entry->function = ex_strdup(ex_json_text(r, "function"));
    entry->cells[0] = ex_fmt("%lld", line);
    entry->cells[1] = label;
    entry->cells[2] = subject ? ex_fmt("`%s`", subject) : NULL;
    entry->title = subject ? ex_fmt("%s `%s` at line %lld", label, subject,
                                    line)
                           : ex_fmt("%s at line %lld", label, line);
    entry->line = (int)line;
    entry->order = !kind                              ? 1
                   : strcmp(kind, "temporary") == 0 ? 0
                   : strcmp(kind, "drop-runs") == 0 ? 2
                                                     : 1;
    entry->tone = inserted ? EX_TONE_PLAIN : EX_TONE_DONE;
    ex_field(entry, "Written out", ex_json_text(r, "text"), EX_TONE_PLAIN);
    ex_field(entry, "Reason", ex_json_text(r, "reason"), EX_TONE_PLAIN);
    ex_field(entry, "Lowered to", ex_json_text(r, "lowered"), EX_TONE_PLAIN);
    ex_field(entry, "Optimized", ex_json_text(r, "optimized"), EX_TONE_PLAIN);
    ex_field(entry, "Type", ex_json_text(r, "type"), EX_TONE_PLAIN);
    ex_field(entry, "Inserted",
             inserted ? "by the compiler; mettle expand prints it" : NULL,
             EX_TONE_PLAIN);
    ex_finish(entry);
  }
  ex_order_groups(tab, root, NULL);
}

static void ex_build_memory(ExReport *report, const ExJson *root) {
  ExTab *tab = &report->tabs[EX_TAB_MEMORY];
  const ExJson *safety = ex_json_get(root, "safety");
  const ExJson *survivors = ex_json_get(safety, "survivors");
  const ExJson *notes = ex_json_get(root, "memory");
  tab->name = "Memory";
  tab->columns[0] = "Line";
  tab->columns[1] = "Finding";
  tab->column_count = 2;
  tab->empty = "No memory findings for this file.";
  if (ex_json_flag(safety, "enabled")) {
    long long accesses = ex_json_int(safety, "accesses", 0);
    long long proved = ex_json_int(safety, "proved", 0);
    long long hoisted = ex_json_int(safety, "hoisted", 0);
    long long settled = proved + hoisted;
    ex_summary(tab, ex_fmt("%lld accesses, %lld settled at compile time "
                           "(%lld%%), %lld checked at run time",
                           accesses, settled,
                           accesses ? settled * 100 / accesses : 0,
                           ex_json_int(safety, "survivorsTotal", 0)));
    ex_summary(tab, ex_fmt("%lld proved in place, %lld folded into one check "
                           "covering a whole loop",
                           proved, hoisted));
    if (ex_json_int(safety, "exempt", 0) > 0) {
      ex_summary(tab, ex_fmt("%lld inside the allocator, which is not "
                             "checked",
                             ex_json_int(safety, "exempt", 0)));
    }
  } else {
    ex_summary(tab, ex_strdup("Bounds checks are off in this build; --safe "
                              "turns them on and this tab says which "
                              "survive"));
  }
  for (size_t i = 0; i < ex_json_count(notes); i++) {
    const ExJson *n = ex_json_at(notes, i);
    const char *severity = ex_json_text(n, "severity");
    const char *headline = ex_json_text(n, "headline");
    long long line = ex_json_int(n, "line", 0);
    ExEntry *entry = ex_add(tab, "Memory report");
    if (!entry) {
      return;
    }
    entry->cells[0] = ex_fmt("%lld", line);
    entry->cells[1] = ex_strdup(headline);
    entry->title = ex_fmt("%s at line %lld",
                          severity ? severity : "finding", line);
    entry->line = (int)line;
    entry->code = ex_strdup(ex_json_text(n, "code"));
    entry->tone = severity && strcmp(severity, "error") == 0 ? EX_TONE_MISSED
                                                             : EX_TONE_PLAIN;
    entry->flags = EXF_MISSED;
    ex_field(entry, "Finding", headline, entry->tone);
    ex_field(entry, "Fix", ex_json_text(n, "fix"), EX_TONE_PLAIN);
    ex_finish(entry);
  }
  {
    size_t first = tab->count;
    size_t listed = ex_json_count(survivors);
    long long total = ex_json_int(safety, "survivorsTotal", (long long)listed);
    for (size_t i = 0; i < listed; i++) {
      const ExJson *s = ex_json_at(survivors, i);
      const char *kind = ex_json_text(s, "kind");
      const char *fn = ex_json_text(s, "function");
      long long line = ex_json_int(s, "line", 0);
      const char *what = kind && strcmp(kind, "runtime") == 0
                             ? "asks the runtime for the allocation"
                         : kind && strcmp(kind, "span") == 0
                             ? "checked once per loop"
                             : "compared against the extent";
      ExEntry *entry = NULL;
      for (size_t j = first; j < tab->count && !entry; j++) {
        ExEntry *e = &tab->entries[j];
        if (e->line == line && ex_same_text(e->group, fn ? fn : "?") &&
            ex_same_text(e->cells[1], what)) {
          entry = e;
        }
      }
      if (entry) {
        entry->repeat++;
        continue;
      }
      entry = ex_add(tab, fn ? fn : "?");
      if (!entry) {
        return;
      }
      entry->function = ex_strdup(fn);
      entry->cells[0] = ex_fmt("%lld", line);
      entry->cells[1] = ex_strdup(what);
      entry->line = (int)line;
      entry->repeat = 1;
      entry->tone = EX_TONE_PLAIN;
      ex_field(entry, "Why", ex_json_text(s, "why"), EX_TONE_PLAIN);
      ex_field(entry, "Function", fn, EX_TONE_PLAIN);
    }
    for (size_t j = first; j < tab->count; j++) {
      ExEntry *e = &tab->entries[j];
      if (e->repeat > 1) {
        char *row = ex_fmt("%s, %d accesses", e->cells[1], e->repeat);
        free(e->cells[1]);
        e->cells[1] = row;
        e->title = ex_fmt("%d bounds checks at line %d in %s", e->repeat,
                          e->line, e->group);
        ex_field_owned(e, "Accesses", ex_fmt("%d", e->repeat),
                       EX_TONE_PLAIN);
      } else {
        e->title = ex_fmt("bounds check at line %d in %s", e->line, e->group);
      }
      ex_finish(e);
    }
    if (total > (long long)listed) {
      ex_summary(tab, ex_fmt("%zu of the %lld run-time checks are listed",
                             listed, total));
    }
  }
  ex_order_groups(tab, root, "Memory report");
}

static int ex_function_order(const void *a, const void *b) {
  const ExJson *x = *(const ExJson *const *)a;
  const ExJson *y = *(const ExJson *const *)b;
  long long cx = ex_json_int(x, "hotCost", 0);
  long long cy = ex_json_int(y, "hotCost", 0);
  if (cx != cy) {
    return cx > cy ? -1 : 1;
  }
  return (int)(ex_json_int(x, "line", 0) - ex_json_int(y, "line", 0));
}

typedef struct {
  long long line;
  const char *bound;
  int kernel;
  int estimated;
  long long low;
  long long high;
  int copies;
} ExLoopRow;

static int ex_loop_row_order(const void *a, const void *b) {
  const ExLoopRow *x = (const ExLoopRow *)a;
  const ExLoopRow *y = (const ExLoopRow *)b;
  if (x->line != y->line) {
    return x->line < y->line ? -1 : 1;
  }
  return x->low < y->low ? -1 : x->low > y->low;
}

static void ex_loop_fields(ExEntry *entry, const ExJson *loops,
                           const char *fn) {
  size_t total = ex_json_count(loops);
  size_t count = 0;
  ExLoopRow *rows = total ? calloc(total, sizeof(*rows)) : NULL;
  if (!rows || !fn) {
    free(rows);
    return;
  }
  for (size_t l = 0; l < total; l++) {
    const ExJson *loop = ex_json_at(loops, l);
    ExLoopRow row;
    size_t r = 0;
    if (!ex_same_text(ex_json_text(loop, "fn"), fn)) {
      continue;
    }
    row.line = ex_json_int(loop, "line", 0);
    row.bound = ex_json_text(loop, "bottleneck");
    row.kernel = ex_json_flag(loop, "hasKernel");
    row.estimated = ex_json_flag(loop, "estimated");
    row.low = row.high = ex_json_int(loop, "cyclesPerIter", 0);
    row.copies = 1;
    while (r < count &&
           !(rows[r].line == row.line && rows[r].kernel == row.kernel &&
             rows[r].estimated == row.estimated &&
             ex_same_text(rows[r].bound, row.bound))) {
      r++;
    }
    if (r < count) {
      rows[r].copies++;
      if (row.low < rows[r].low) {
        rows[r].low = row.low;
      }
      if (row.high > rows[r].high) {
        rows[r].high = row.high;
      }
      continue;
    }
    rows[count++] = row;
  }
  if (count > 1) {
    qsort(rows, count, sizeof(*rows), ex_loop_row_order);
  }
  for (size_t r = 0; r < count; r++) {
    const ExLoopRow *row = &rows[r];
    char cycles[64];
    char low[24];
    char high[24];
    char copies[32] = "";
    ex_cycles(low, sizeof(low), row->low);
    ex_cycles(high, sizeof(high), row->high);
    if (row->low == row->high) {
      snprintf(cycles, sizeof(cycles), "%s", low);
    } else {
      snprintf(cycles, sizeof(cycles), "%s to %s", low, high);
    }
    if (row->copies > 1) {
      snprintf(copies, sizeof(copies), ", %d copies", row->copies);
    }
    ex_field_owned(entry, "Loop",
                   ex_fmt("line %lld: %s cycles per iteration%s%s%s%s%s",
                          row->line, cycles, row->bound ? ", bound by " : "",
                          row->bound ? row->bound : "",
                          row->kernel ? ", runs a kernel" : "",
                          row->estimated ? ", estimated" : "", copies),
                   EX_TONE_PLAIN);
  }
  free(rows);
}

static void ex_build_functions(ExReport *report, const ExJson *root) {
  ExTab *tab = &report->tabs[EX_TAB_FUNCTIONS];
  const ExJson *functions = ex_json_get(root, "functions");
  const ExJson *loops = ex_json_get(root, "loops");
  const ExJson *graph = ex_json_get(root, "callGraph");
  const ExJson *hot = ex_json_get(root, "hotspots");
  size_t count = ex_json_count(functions);
  const ExJson **order = count ? malloc(count * sizeof(*order)) : NULL;
  tab->name = "Functions";
  tab->columns[0] = "Function";
  tab->columns[1] = "Instructions";
  tab->columns[2] = "Loops";
  tab->columns[3] = "Cost";
  tab->column_count = 4;
  tab->empty = "No functions reached the optimizer.";
  if (count && !order) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    order[i] = ex_json_at(functions, i);
  }
  if (count > 1) {
    qsort(order, count, sizeof(*order), ex_function_order);
  }
  for (size_t i = 0; i < count; i++) {
    const ExJson *f = order[i];
    const char *fn = ex_json_text(f, "fn");
    long long loops_total = ex_json_int(f, "loops", 0);
    long long loops_vec = ex_json_int(f, "loopsVectorized", 0);
    int ok = ex_json_flag(f, "backendOk");
    ExEntry *entry = ex_add(tab, NULL);
    if (!entry) {
      break;
    }
    entry->function = ex_strdup(fn);
    entry->cells[0] = ex_strdup(fn);
    entry->cells[1] = ex_fmt("%lld", ex_json_int(f, "instructionsAfter", 0));
    entry->cells[2] = loops_total ? ex_fmt("%lld/%lld", loops_vec, loops_total)
                                  : ex_strdup("");
    entry->cells[3] = ex_fmt("%lld", ex_json_int(f, "hotCost", 0));
    entry->title = ex_fmt("%s, line %lld", fn ? fn : "?",
                          ex_json_int(f, "line", 0));
    entry->line = (int)ex_json_int(f, "line", 0);
    entry->tone = ok ? EX_TONE_PLAIN : EX_TONE_MISSED;
    ex_field_owned(entry, "Instructions",
                   ex_fmt("%lld before optimization, %lld after",
                          ex_json_int(f, "instructionsBefore", 0),
                          ex_json_int(f, "instructionsAfter", 0)),
                   EX_TONE_PLAIN);
    if (loops_total) {
      ex_field_owned(entry, "Loops",
                     ex_fmt("%lld, %lld vectorized", loops_total, loops_vec),
                     EX_TONE_PLAIN);
    }
    ex_field_owned(entry, "Calls",
                   ex_fmt("%lld inlined, %lld kept as calls",
                          ex_json_int(f, "callsInlined", 0),
                          ex_json_int(f, "callsRefused", 0)),
                   EX_TONE_PLAIN);
    if (ok) {
      ex_field_owned(entry, "Backend",
                     ex_fmt("register-allocated, %lld instructions",
                            ex_json_int(f, "backendInstructions", 0)),
                     EX_TONE_PLAIN);
    } else {
      const char *why = ex_json_text(f, "backendReason");
      ex_field_owned(entry, "Backend",
                     ex_fmt("not register-allocated%s%s", why ? ": " : "",
                            why ? why : ""),
                     EX_TONE_MISSED);
    }
    ex_field_owned(entry, "Registers",
                   ex_fmt("%lld used, %lld spilled",
                          ex_json_int(f, "regsUsed", 0),
                          ex_json_int(f, "spills", 0)),
                   EX_TONE_PLAIN);
    ex_field_owned(entry, "Cost",
                   ex_fmt("throughput %lld, hot cost %lld, %lld vector "
                          "operations",
                          ex_json_int(f, "throughput", 0),
                          ex_json_int(f, "hotCost", 0),
                          ex_json_int(f, "vectorOps", 0)),
                   EX_TONE_PLAIN);
    ex_loop_fields(entry, loops, fn);
    for (size_t c = 0; c < ex_json_count(graph); c++) {
      const ExJson *edge = ex_json_at(graph, c);
      const char *caller = ex_json_text(edge, "caller");
      const char *callee = ex_json_text(edge, "callee");
      if (caller && fn && strcmp(caller, fn) == 0) {
        ex_field_owned(entry, "Calls out",
                       ex_fmt("%s: %lld inlined, %lld kept",
                              callee ? callee : "?",
                              ex_json_int(edge, "inlined", 0),
                              ex_json_int(edge, "refused", 0)),
                       EX_TONE_PLAIN);
      }
    }
    for (size_t c = 0; c < ex_json_count(graph); c++) {
      const ExJson *edge = ex_json_at(graph, c);
      const char *caller = ex_json_text(edge, "caller");
      const char *callee = ex_json_text(edge, "callee");
      if (callee && fn && strcmp(callee, fn) == 0) {
        ex_field_owned(entry, "Called by",
                       ex_fmt("%s: %lld inlined, %lld kept",
                              caller ? caller : "?",
                              ex_json_int(edge, "inlined", 0),
                              ex_json_int(edge, "refused", 0)),
                       EX_TONE_PLAIN);
      }
    }
    for (size_t h = 0; h < ex_json_count(hot); h++) {
      const ExJson *spot = ex_json_at(hot, h);
      const char *owner = ex_json_text(spot, "fn");
      if (!owner || !fn || strcmp(owner, fn) != 0) {
        continue;
      }
      ex_field_owned(entry, "Hot",
                     ex_fmt("%s at line %lld, cost %lld",
                            ex_json_text(spot, "kind")
                                ? ex_json_text(spot, "kind")
                                : "?",
                            ex_json_int(spot, "line", 0),
                            ex_json_int(spot, "cost", 0)),
                     EX_TONE_PLAIN);
    }
    ex_finish(entry);
  }
  free(order);
}

static void ex_more(ExTab *tab, const ExJson *root, const char *key,
                    const char *what, size_t shown) {
  long long total = ex_json_int(root, key, (long long)shown);
  if (total > (long long)shown) {
    ex_summary(tab, ex_fmt("%s: %zu of %lld listed", what, shown, total));
  }
}

static void ex_build_proofs(ExReport *report, const ExJson *root) {
  ExTab *tab = &report->tabs[EX_TAB_PROOFS];
  const ExJson *typed = ex_json_get(root, "proven_by_type");
  const ExJson *payoffs = ex_json_get(root, "payoffs");
  const ExJson *proofs = ex_json_get(root, "proofs");
  const ExJson *effects = ex_json_get(root, "effects");
  const ExJson *rules = ex_json_get(root, "rules");
  const ExJson *beliefs = ex_json_get(root, "beliefs");
  const ExJson *layout = ex_json_get(root, "layout");
  tab->name = "Proofs";
  tab->columns[0] = "Where";
  tab->columns[1] = "What";
  tab->column_count = 2;
  tab->empty = "Nothing in this file was proven from a declared type, and "
               "the build took nothing on trust.";
  for (size_t i = 0; i < ex_json_count(layout); i++) {
    const ExJson *l = ex_json_at(layout, i);
    long long line = ex_json_int(l, "line", 0);
    ExEntry *entry = ex_add(tab, "Placement and layout the types state");
    if (!entry) {
      return;
    }
    entry->function = ex_strdup(ex_json_text(l, "fn"));
    entry->cells[0] = ex_fmt("line %lld", line);
    entry->cells[1] = ex_strdup(ex_json_text(l, "detail"));
    entry->title = ex_fmt("line %lld", line);
    entry->line = (int)line;
    ex_field(entry, "Proof", ex_json_text(l, "detail"), EX_TONE_PLAIN);
    ex_field(entry, "Function", ex_json_text(l, "fn"), EX_TONE_PLAIN);
    ex_finish(entry);
  }
  ex_more(tab, root, "layoutTotal", "Layout facts", ex_json_count(layout));
  for (size_t i = 0; i < ex_json_count(typed); i++) {
    const ExJson *t = ex_json_at(typed, i);
    const char *type = ex_json_text(t, "type");
    long long line = ex_json_int(t, "line", 0);
    ExEntry *entry = ex_add(tab, "Bounds checks a declared type removed");
    if (!entry) {
      return;
    }
    entry->function = ex_strdup(ex_json_text(t, "fn"));
    entry->cells[0] = ex_fmt("line %lld", line);
    entry->cells[1] = ex_fmt("'%s' holds %lld..%lld", type ? type : "?",
                             ex_json_int(t, "min", 0),
                             ex_json_int(t, "max", 0));
    entry->title = ex_fmt("bounds check removed at line %lld", line);
    entry->line = (int)line;
    entry->code = ex_strdup("P0002");
    ex_field_owned(entry, "Proof",
                   ex_fmt("'%s' holds %lld..%lld, inside an array of %lld",
                          type ? type : "?", ex_json_int(t, "min", 0),
                          ex_json_int(t, "max", 0),
                          ex_json_int(t, "length", 0)),
                   EX_TONE_PLAIN);
    ex_field(entry, "Function", ex_json_text(t, "fn"), EX_TONE_PLAIN);
    ex_finish(entry);
  }
  ex_more(tab, root, "provenByTypeTotal", "Bounds checks removed",
          ex_json_count(typed));
  for (size_t i = 0; i < ex_json_count(payoffs); i++) {
    const ExJson *p = ex_json_at(payoffs, i);
    long long line = ex_json_int(p, "line", 0);
    ExEntry *entry = ex_add(tab, "Optimizations a declared type earned");
    if (!entry) {
      return;
    }
    entry->function = ex_strdup(ex_json_text(p, "fn"));
    entry->cells[0] = ex_fmt("line %lld", line);
    entry->cells[1] = ex_strdup(ex_json_text(p, "what"));
    entry->title = ex_fmt("line %lld", line);
    entry->line = (int)line;
    ex_field(entry, "Optimization", ex_json_text(p, "what"), EX_TONE_PLAIN);
    ex_field_owned(entry, "Because",
                   ex_fmt("'%s' %s",
                          ex_json_text(p, "type") ? ex_json_text(p, "type")
                                                  : "?",
                          ex_json_text(p, "detail")
                              ? ex_json_text(p, "detail")
                              : ""),
                   EX_TONE_PLAIN);
    ex_field(entry, "Function", ex_json_text(p, "fn"), EX_TONE_PLAIN);
    ex_finish(entry);
  }
  ex_more(tab, root, "payoffsTotal", "Optimizations a type earned",
          ex_json_count(payoffs));
  for (size_t i = 0; i < ex_json_count(proofs); i++) {
    const ExJson *p = ex_json_at(proofs, i);
    long long line = ex_json_int(p, "line", 0);
    const char *expression = ex_json_text(p, "expression");
    const char *type = ex_json_text(p, "type");
    ExEntry *entry = ex_add(tab, "Conversions proven");
    if (!entry) {
      return;
    }
    entry->cells[0] = ex_fmt("line %lld", line);
    entry->cells[1] = ex_fmt("`%s` becomes '%s'", expression ? expression : "?",
                             type ? type : "?");
    entry->title = ex_fmt("conversion at line %lld", line);
    entry->line = (int)line;
    ex_field(entry, "Because", ex_json_text(p, "route"), EX_TONE_PLAIN);
    ex_field(entry, "Used by", ex_json_text(p, "consumer"), EX_TONE_PLAIN);
    ex_finish(entry);
  }
  ex_more(tab, root, "proofsTotal", "Conversions proven",
          ex_json_count(proofs));
  for (size_t i = 0; i < ex_json_count(effects); i++) {
    const ExJson *e = ex_json_at(effects, i);
    const char *fn = ex_json_text(e, "fn");
    ExEntry *entry = ex_add(tab, "Effects inferred and checked");
    if (!entry) {
      return;
    }
    entry->function = ex_strdup(fn);
    entry->cells[0] = ex_strdup(fn);
    entry->cells[1] = ex_fmt("performs %s",
                             ex_json_text(e, "performs")
                                 ? ex_json_text(e, "performs")
                                 : "nothing");
    entry->title = ex_fmt("effects of %s", fn ? fn : "?");
    ex_field(entry, "Performs",
             ex_json_text(e, "performs") ? ex_json_text(e, "performs")
                                         : "nothing",
             EX_TONE_PLAIN);
    ex_field(entry, "Needs",
             ex_json_text(e, "needs") ? ex_json_text(e, "needs") : "nothing",
             EX_TONE_PLAIN);
    ex_finish(entry);
  }
  ex_more(tab, root, "effectsTotal", "Effects", ex_json_count(effects));
  for (size_t i = 0; i < ex_json_count(rules); i++) {
    const ExJson *r = ex_json_at(rules, i);
    const char *rule = ex_json_text(r, "rule");
    const char *verdict = ex_json_text(r, "verdict");
    ExEntry *entry = ex_add(tab, "Rules run");
    if (!entry) {
      return;
    }
    entry->cells[0] = ex_strdup(rule);
    entry->cells[1] = ex_strdup(verdict);
    entry->title = ex_fmt("@rule %s", rule ? rule : "?");
    entry->tone = verdict && strstr(verdict, "fail") ? EX_TONE_MISSED
                                                     : EX_TONE_PLAIN;
    ex_field(entry, "Verdict", verdict, entry->tone);
    ex_field_owned(entry, "Steps", ex_fmt("%lld", ex_json_int(r, "steps", 0)),
                   EX_TONE_PLAIN);
    ex_finish(entry);
  }
  ex_more(tab, root, "rulesTotal", "Rules", ex_json_count(rules));
  for (size_t i = 0; i < ex_json_count(beliefs); i++) {
    const ExJson *b = ex_json_at(beliefs, i);
    const char *what = ex_json_text(b, "what");
    ExEntry *entry = ex_add(tab, "Taken on trust");
    if (!entry) {
      return;
    }
    entry->cells[0] = ex_strdup(what);
    entry->cells[1] = ex_strdup(ex_json_text(b, "why"));
    entry->title = ex_strdup(what);
    entry->tone = EX_TONE_MISSED;
    ex_field(entry, "Why", ex_json_text(b, "why"), EX_TONE_PLAIN);
    ex_field(entry, "Checked",
             "at run time under mettle test, --check-effects or "
             "--check-proofs, and nowhere else",
             EX_TONE_PLAIN);
    ex_finish(entry);
  }
  ex_more(tab, root, "beliefsTotal", "Beliefs", ex_json_count(beliefs));
  ex_merge_repeats(tab, "Places");
}

static int ex_pass_order(const void *a, const void *b) {
  const ExJson *x = *(const ExJson *const *)a;
  const ExJson *y = *(const ExJson *const *)b;
  long long rx = ex_json_int(x, "instructionsRemoved", 0);
  long long ry = ex_json_int(y, "instructionsRemoved", 0);
  if (rx != ry) {
    return rx > ry ? -1 : 1;
  }
  return (int)(ex_json_int(y, "changedRuns", 0) -
               ex_json_int(x, "changedRuns", 0));
}

static void ex_build_passes(ExReport *report, const ExJson *root) {
  ExTab *tab = &report->tabs[EX_TAB_PASSES];
  const ExJson *passes = ex_json_get(root, "passes");
  size_t count = ex_json_count(passes);
  const ExJson **order = count ? malloc(count * sizeof(*order)) : NULL;
  tab->name = "Passes";
  tab->columns[0] = "Pass";
  tab->columns[1] = "Runs";
  tab->columns[2] = "Changed";
  tab->columns[3] = "Removed";
  tab->column_count = 4;
  tab->empty = "No optimizer pass ran on this file.";
  if (count && !order) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    order[i] = ex_json_at(passes, i);
  }
  if (count > 1) {
    qsort(order, count, sizeof(*order), ex_pass_order);
  }
  for (size_t i = 0; i < count; i++) {
    const ExJson *p = order[i];
    const ExJson *effects = ex_json_get(p, "effects");
    const ExJson *sites = ex_json_get(p, "sites");
    const char *name = ex_json_text(p, "pass");
    long long changed = ex_json_int(p, "changedRuns", 0);
    ExEntry *entry = ex_add(tab, NULL);
    if (!entry) {
      break;
    }
    entry->cells[0] = ex_strdup(name);
    entry->cells[1] = ex_fmt("%lld", ex_json_int(p, "runs", 0));
    entry->cells[2] = ex_fmt("%lld", changed);
    entry->cells[3] = ex_fmt("%lld", ex_json_int(p, "instructionsRemoved", 0));
    entry->title = ex_strdup(name);
    entry->tone = changed ? EX_TONE_PLAIN : EX_TONE_DONE;
    ex_field_owned(entry, "Runs",
                   ex_fmt("%lld, %lld of them changed something",
                          ex_json_int(p, "runs", 0), changed),
                   EX_TONE_PLAIN);
    ex_field_owned(entry, "Removed",
                   ex_fmt("%lld instructions",
                          ex_json_int(p, "instructionsRemoved", 0)),
                   EX_TONE_PLAIN);
    for (size_t e = 0; effects && effects->kind == EXJ_OBJECT &&
                       e < effects->count;
         e++) {
      char *label = ex_spaced(effects->keys[e]);
      ex_field_owned(entry, "Effect",
                     ex_fmt("%s %lld", label ? label : "?",
                            (long long)effects->items[e]->number),
                     EX_TONE_PLAIN);
      free(label);
    }
    for (size_t s = 0; s < ex_json_count(sites); s++) {
      const ExJson *site = ex_json_at(sites, s);
      ex_field_owned(entry, "Site",
                     ex_fmt("%s, line %lld: %lld fewer instructions",
                            ex_json_text(site, "fn")
                                ? ex_json_text(site, "fn")
                                : "?",
                            ex_json_int(site, "line", 0),
                            ex_json_int(site, "delta", 0)),
                     EX_TONE_PLAIN);
    }
    ex_finish(entry);
  }
  free(order);
}

static void ex_load_source(ExReport *report, const char *path) {
  FILE *in;
  long size;
  char *text;
  if (!path) {
    return;
  }
  in = fopen(path, "rb");
  if (!in) {
    return;
  }
  if (fseek(in, 0, SEEK_END) != 0 || (size = ftell(in)) < 0 ||
      fseek(in, 0, SEEK_SET) != 0) {
    fclose(in);
    return;
  }
  text = malloc((size_t)size + 1);
  if (!text) {
    fclose(in);
    return;
  }
  size = (long)fread(text, 1, (size_t)size, in);
  fclose(in);
  text[size] = '\0';
  for (char *line = text; line && *line;) {
    char *end = strchr(line, '\n');
    char **grown;
    size_t n = end ? (size_t)(end - line) : strlen(line);
    if (n > 0 && line[n - 1] == '\r') {
      n--;
    }
    grown = realloc(report->lines, (report->line_count + 1) * sizeof(char *));
    if (!grown) {
      break;
    }
    report->lines = grown;
    report->lines[report->line_count] = malloc(n + 1);
    if (report->lines[report->line_count]) {
      memcpy(report->lines[report->line_count], line, n);
      report->lines[report->line_count][n] = '\0';
      report->line_count++;
    }
    line = end ? end + 1 : NULL;
  }
  free(text);
}

static long long ex_item_int(const ExJson *array, size_t index) {
  const ExJson *item = ex_json_at(array, index);
  return item && item->kind == EXJ_NUMBER ? (long long)item->number : 0;
}

static char *ex_item_text(const ExJson *array, size_t index) {
  const ExJson *item = ex_json_at(array, index);
  return item && item->kind == EXJ_STRING ? ex_strdup(item->string) : NULL;
}

static void ex_build_machine(ExReport *report, const ExJson *root) {
  const ExJson *machine = ex_json_get(root, "machine");
  size_t count = ex_json_count(machine);
  report->machine_total = ex_json_int(root, "machineTotal", 0);
  if (count == 0) {
    return;
  }
  report->machine = calloc(count, sizeof(ExMachineFn));
  if (!report->machine) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    const ExJson *f = ex_json_at(machine, i);
    const ExJson *insns = ex_json_get(f, "insns");
    size_t n = ex_json_count(insns);
    ExMachineFn *fn = &report->machine[report->machine_count++];
    fn->name = ex_strdup(ex_json_text(f, "fn"));
    fn->insns = n ? calloc(n, sizeof(ExInsn)) : NULL;
    if (!fn->insns) {
      continue;
    }
    for (size_t k = 0; k < n; k++) {
      const ExJson *row = ex_json_at(insns, k);
      ExInsn *insn = &fn->insns[fn->count++];
      insn->line = (int)ex_item_int(row, 0);
      insn->text = ex_item_text(row, 1);
      insn->ports = ex_item_text(row, 2);
      insn->cost = (int)ex_item_int(row, 3);
      insn->latency = (int)ex_item_int(row, 4);
      insn->kernel = (int)ex_item_int(row, 5);
      insn->depth = (int)ex_item_int(row, 6);
      insn->label = (int)ex_item_int(row, 7);
      insn->origin = ex_item_text(row, 8);
      insn->offset = ex_item_int(row, 9);
    }
  }
}

const ExMachineFn *ex_report_machine(const ExReport *report,
                                     const char *function) {
  if (!report || !function) {
    return NULL;
  }
  for (size_t i = 0; i < report->machine_count; i++) {
    if (ex_same_text(report->machine[i].name, function)) {
      return &report->machine[i];
    }
  }
  return NULL;
}

const char *ex_report_line(const ExReport *report, int line) {
  if (!report || line <= 0 || (size_t)line > report->line_count) {
    return NULL;
  }
  return report->lines[line - 1];
}

ExReport *ex_report_build(const ExJson *root, const char *source_path) {
  ExReport *report = calloc(1, sizeof(ExReport));
  const char *path = source_path ? source_path : ex_json_text(root, "path");
  if (!report) {
    return NULL;
  }
  report->source_name = ex_strdup(ex_json_text(root, "source"));
  report->source_path = ex_strdup(path);
  ex_load_source(report, path);
  ex_build_machine(report, root);
  ex_build_optimizer(report, root);
  ex_build_overview(report, root);
  ex_build_ownership(report, root);
  ex_build_memory(report, root);
  ex_build_functions(report, root);
  ex_build_proofs(report, root);
  ex_build_passes(report, root);
  return report;
}

void ex_report_free(ExReport *report) {
  if (!report) {
    return;
  }
  for (int t = 0; t < EX_TAB_COUNT; t++) {
    ExTab *tab = &report->tabs[t];
    for (size_t i = 0; i < tab->count; i++) {
      ex_entry_free(&tab->entries[i]);
    }
    free(tab->entries);
    for (size_t s = 0; s < tab->summary_count; s++) {
      free(tab->summary[s]);
    }
    free(tab->summary);
  }
  for (size_t i = 0; i < report->line_count; i++) {
    free(report->lines[i]);
  }
  free(report->lines);
  for (size_t i = 0; i < report->machine_count; i++) {
    ExMachineFn *fn = &report->machine[i];
    for (size_t k = 0; k < fn->count; k++) {
      free(fn->insns[k].text);
      free(fn->insns[k].ports);
      free(fn->insns[k].origin);
    }
    free(fn->insns);
    free(fn->name);
  }
  free(report->machine);
  free(report->source_name);
  free(report->source_path);
  free(report);
}
