#include "explain_model.h"

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
    ExEntry *entry = ex_add(tab, fn ? fn : "?");
    if (!entry) {
      return;
    }
    if (kind && strcmp(kind, "calls-folded") == 0) {
      long long last = ex_json_int(r, "lineEnd", line);
      entry->cells[0] = ex_fmt("%lld-%lld", line, last);
      entry->cells[1] = ex_fmt("%lld calls", count);
      entry->end_line = (int)last;
      entry->title = ex_fmt("%s, %lld calls on lines %lld to %lld",
                            fn ? fn : "?", count, line, last);
    } else {
      entry->cells[0] = ex_fmt("%lld", line);
      entry->cells[1] = ex_strdup(entity ? entity : kind);
      entry->end_line = (int)end_line;
      entry->title = ex_fmt("%s, %s at line %lld", fn ? fn : "?",
                            entity ? entity : "?", line);
    }
    entry->cells[2] = ex_strdup(headline);
    entry->line = (int)line;
    entry->code = ex_strdup(ex_json_text(r, "code"));
    if (positive || ex_json_flag(r, "trivial")) {
      entry->tone = EX_TONE_DONE;
    } else {
      entry->tone = verified ? EX_TONE_FIXABLE : EX_TONE_MISSED;
      entry->flags |= EXF_MISSED;
    }
    if (!positive && fix && !advisory) {
      entry->flags |= EXF_FIXABLE;
    }
    ex_field(entry, "Verdict", headline,
             positive ? EX_TONE_DONE : EX_TONE_MISSED);
    ex_field(entry, "Reason", ex_json_text(r, "reason"), EX_TONE_PLAIN);
    ex_field(entry, advisory ? "Note" : "Fix", fix, EX_TONE_PLAIN);
    ex_field(entry, "Verified", verified, EX_TONE_FIXABLE);
    ex_field(entry, "Still blocked", ex_json_text(r, "stillBlocked"),
             EX_TONE_MISSED);
    ex_field(entry, "Calls", ex_json_text(r, "calls"), EX_TONE_PLAIN);
    {
      const ExJson *quantities = ex_json_get(r, "quantities");
      for (size_t q = 0; quantities && quantities->kind == EXJ_OBJECT &&
                         q < quantities->count;
           q++) {
        char *label = ex_spaced(quantities->keys[q]);
        char *value = ex_fmt("%lld", (long long)quantities->items[q]->number);
        if (label && label[0]) {
          label[0] = (char)toupper((unsigned char)label[0]);
        }
        ex_field(entry, label, value, EX_TONE_PLAIN);
        free(label);
        free(value);
      }
    }
    ex_finish(entry);
  }
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
    entry->cells[0] = ex_fmt("%lld", line);
    entry->cells[1] = label;
    entry->cells[2] = subject ? ex_fmt("`%s`", subject) : NULL;
    entry->title = subject ? ex_fmt("%s `%s` at line %lld", label, subject,
                                    line)
                           : ex_fmt("%s at line %lld", label, line);
    entry->line = (int)line;
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
  for (size_t i = 0; i < ex_json_count(survivors); i++) {
    const ExJson *s = ex_json_at(survivors, i);
    const char *kind = ex_json_text(s, "kind");
    const char *fn = ex_json_text(s, "function");
    long long line = ex_json_int(s, "line", 0);
    ExEntry *entry = ex_add(tab, "Checked at run time");
    const char *what = kind && strcmp(kind, "runtime") == 0
                           ? "asks the runtime for the allocation"
                       : kind && strcmp(kind, "span") == 0
                           ? "checked once per loop"
                           : "compared against the extent";
    if (!entry) {
      return;
    }
    entry->cells[0] = ex_fmt("%lld", line);
    entry->cells[1] = ex_strdup(what);
    entry->title = ex_fmt("bounds check at line %lld%s%s", line,
                          fn ? " in " : "", fn ? fn : "");
    entry->line = (int)line;
    entry->tone = EX_TONE_PLAIN;
    ex_field(entry, "Why", ex_json_text(s, "why"), EX_TONE_PLAIN);
    ex_field(entry, "Function", fn, EX_TONE_PLAIN);
    ex_finish(entry);
  }
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
    for (size_t l = 0; l < ex_json_count(loops); l++) {
      const ExJson *loop = ex_json_at(loops, l);
      const char *bound = ex_json_text(loop, "bottleneck");
      const char *owner = ex_json_text(loop, "fn");
      if (!owner || !fn || strcmp(owner, fn) != 0) {
        continue;
      }
      ex_field_owned(entry, "Loop",
                     ex_fmt("line %lld: %lld cycles per iteration%s%s%s%s",
                            ex_json_int(loop, "line", 0),
                            ex_json_int(loop, "cyclesPerIter", 0),
                            bound ? ", bound by " : "", bound ? bound : "",
                            ex_json_flag(loop, "hasKernel")
                                ? ", runs a kernel"
                                : "",
                            ex_json_flag(loop, "estimated") ? ", estimated"
                                                            : ""),
                     EX_TONE_PLAIN);
    }
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
      ExEntry *e = &tab->entries[i];
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
  free(report->source_name);
  free(report->source_path);
  free(report);
}
