#include "ir_optimize_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define explain_isatty _isatty
#define explain_fileno _fileno
#else
#include <unistd.h>
#define explain_isatty isatty
#define explain_fileno fileno
#endif

/* --explain: the optimization report.
 *
 * Every pass that makes a user-visible decision (the loop verifier in
 * ir_optimize_simd_contract.c, the inliner in ir_optimize_inline.c, the MIR
 * eligibility gate in codegen) records a remark here instead of printing
 * directly. At the end of the optimizer pipeline the remarks are sorted into
 * source order and printed as one coherent, human-first report:
 *
 *   saxpy (loop @ line 12): vectorized -> vfmadd231ps, 8-wide float32
 *   process (loop @ line 40): NOT vectorized
 *       |_ reason: each iteration calls `scale`; ...
 *       |_ fix: mark `scale` @inline, or hoist the call out of the loop
 *
 * Remarks are limited to the main input file (the focus file) so imported
 * stdlib modules don't flood the report. */

static int g_explain = 0;
static const char *g_explain_focus_file = NULL;
/* Set while a fix hypothesis is being simulated on a scratch clone: the
 * re-run optimizer passes must not pollute the report with the clone's
 * remarks (the unroller, for one, records remarks from inside the stages). */
static int g_explain_hypothesis = 0;

void ir_explain_set_hypothesis(int active) { g_explain_hypothesis = active; }

/* One remark: an entity ("loop", "call to `f`") in a function, a colored
 * headline, and optional reason/fix detail lines. */
typedef struct {
  char *function_name;
  char *entity;
  size_t line;
  size_t column;
  int positive; /* 1 = the optimizer did something good (green), 0 = declined */
  char *headline;
  char *reason;   /* may be NULL */
  char *fix;      /* may be NULL */
  char *verified; /* may be NULL: the fix was SIMULATED and proven to work */
} IRExplainRemark;

static IRExplainRemark *g_remarks = NULL;
static size_t g_remark_count = 0;
static size_t g_remark_capacity = 0;

/* Backend (codegen-stage) entries: per function, did it get the
 * register-allocating MIR backend or fall back to baseline codegen? */
typedef struct {
  char *function_name;
  int ok;
  char *detail; /* gate reason code when !ok */
} IRExplainBackendEntry;

static IRExplainBackendEntry *g_backend = NULL;
static size_t g_backend_count = 0;
static size_t g_backend_capacity = 0;

void ir_optimize_set_explain(int enabled, const char *focus_file) {
  g_explain = enabled;
  g_explain_focus_file = focus_file;
}

int ir_explain_enabled(void) { return g_explain; }

static const char *ir_explain_path_basename(const char *path) {
  const char *base = path;
  for (; *path; path++) {
    if (*path == '/' || *path == '\\') {
      base = path + 1;
    }
  }
  return base;
}

int ir_explain_file_enabled(const char *filename) {
  if (!g_explain) {
    return 0;
  }
  if (!g_explain_focus_file || !filename) {
    return 1;
  }
  return strcmp(ir_explain_path_basename(filename),
                ir_explain_path_basename(g_explain_focus_file)) == 0;
}

int ir_explain_location_enabled(const SourceLocation *location) {
  if (!g_explain) {
    return 0;
  }
  if (!location || !location->filename) {
    return g_explain_focus_file == NULL;
  }
  return ir_explain_file_enabled(location->filename);
}

/* ---- color ---------------------------------------------------------------
 * Same policy as error_reporter.c (CLICOLOR_FORCE > NO_COLOR > TERM=dumb >
 * CLICOLOR=0 > stderr-is-a-tty), kept local because that helper is private. */

#ifdef _WIN32
static void ir_explain_enable_vt(void) {
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

static int ir_explain_use_color(void) {
  static int cached = -1;
  if (cached >= 0) {
    return cached;
  }
  const char *force = getenv("CLICOLOR_FORCE");
  if (force && force[0] != '\0' && strcmp(force, "0") != 0) {
#ifdef _WIN32
    ir_explain_enable_vt();
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
  int fd = explain_fileno(stderr);
  if (fd < 0 || !explain_isatty(fd)) {
    cached = 0;
    return cached;
  }
#ifdef _WIN32
  ir_explain_enable_vt();
#endif
  cached = 1;
  return cached;
}

#define EXPLAIN_GREEN "\x1b[32m"
#define EXPLAIN_RED "\x1b[31m"
#define EXPLAIN_DIM "\x1b[2m"
#define EXPLAIN_BOLD "\x1b[1m"
#define EXPLAIN_RESET "\x1b[0m"

static const char *clr(const char *code) {
  return ir_explain_use_color() ? code : "";
}

/* ---- UTF-8 vs ASCII -------------------------------------------------------
 * The report's glyphs (└ → ── —) are UTF-8. A Windows console on a legacy
 * codepage (the default outside `chcp 65001`) renders those bytes as
 * mojibake, and PowerShell 5.1 decodes redirected stderr with the console CP
 * too. So: glyphs only when the target provably renders UTF-8 -- a console
 * whose output CP is UTF-8 on Windows, a UTF-8 locale on POSIX -- and ASCII
 * art everywhere else (including all redirected output, where we cannot know
 * what will decode it). */

static int ir_explain_use_unicode(void) {
  static int cached = -1;
  if (cached >= 0) {
    return cached;
  }
#ifdef _WIN32
  int fd = explain_fileno(stderr);
  if (fd >= 0 && explain_isatty(fd)) {
    cached = (GetConsoleOutputCP() == CP_UTF8) ? 1 : 0;
  } else {
    cached = 0;
  }
#else
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
#endif
  return cached;
}

/* Tree-corner / arrow / rule glyphs, with ASCII fallbacks. */
static const char *glyph_elbow(void) {
  return ir_explain_use_unicode() ? "\xE2\x94\x94" : "\\_";
}
static const char *glyph_rule(void) {
  return ir_explain_use_unicode() ? "\xE2\x94\x80\xE2\x94\x80" : "--";
}

/* ---- remark store -------------------------------------------------------- */

static char *ir_explain_strdup(const char *s) {
  if (!s) {
    return NULL;
  }
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (copy) {
    memcpy(copy, s, n);
  }
  return copy;
}

/* Copy remark text, transliterating the report's known UTF-8 glyphs to ASCII
 * when the output target can't render UTF-8. Contributors (the loop verifier,
 * the inliner) embed → and — freely; this is the single choke point that keeps
 * them readable everywhere. Every replacement is no longer than the original
 * sequence, so the transliteration runs in place on the copy. */
static char *ir_explain_text_dup(const char *s) {
  char *copy = ir_explain_strdup(s);
  if (!copy || ir_explain_use_unicode()) {
    return copy;
  }
  const unsigned char *read = (const unsigned char *)copy;
  char *write = copy;
  while (*read) {
    if (read[0] == 0xE2 && read[1] == 0x86 && read[2] == 0x92) {
      *write++ = '-'; /* → */
      *write++ = '>';
      read += 3;
    } else if (read[0] == 0xE2 && read[1] == 0x80 && read[2] == 0x94) {
      *write++ = '-'; /* — */
      *write++ = '-';
      read += 3;
    } else if (read[0] == 0xE2 && read[1] == 0x94 && read[2] == 0x94) {
      *write++ = '\\'; /* └ */
      *write++ = '_';
      read += 3;
    } else if (read[0] == 0xE2 && read[1] == 0x94 && read[2] == 0x80) {
      *write++ = '-'; /* ─ */
      read += 3;
    } else if (read[0] >= 0x80) {
      *write++ = '?'; /* any other multibyte: never emit raw mojibake */
      read++;
      while (*read >= 0x80 && *read < 0xC0) {
        read++; /* skip the sequence's continuation bytes */
      }
    } else {
      *write++ = (char)*read++;
    }
  }
  *write = '\0';
  return copy;
}

void ir_explain_remark(const char *function_name, const char *entity,
                       SourceLocation location, int positive,
                       const char *headline, const char *reason,
                       const char *fix, const char *verified) {
  if (!g_explain || g_explain_hypothesis || !headline ||
      !ir_explain_location_enabled(&location)) {
    return;
  }

  /* Dedupe: an inlined callee's body can be cloned into several callers, each
   * clone carrying the callee's original source locations; report the
   * decision once. */
  for (size_t i = 0; i < g_remark_count; i++) {
    IRExplainRemark *r = &g_remarks[i];
    if (r->line == location.line && r->column == location.column &&
        r->entity && entity && strcmp(r->entity, entity) == 0 &&
        strcmp(r->headline, headline) == 0) {
      return;
    }
  }

  if (g_remark_count == g_remark_capacity) {
    size_t new_capacity = g_remark_capacity ? g_remark_capacity * 2 : 32;
    IRExplainRemark *grown =
        realloc(g_remarks, new_capacity * sizeof(IRExplainRemark));
    if (!grown) {
      return;
    }
    g_remarks = grown;
    g_remark_capacity = new_capacity;
  }

  IRExplainRemark *r = &g_remarks[g_remark_count++];
  r->function_name = ir_explain_strdup(function_name ? function_name : "?");
  r->entity = ir_explain_text_dup(entity ? entity : "loop");
  r->line = location.line;
  r->column = location.column;
  r->positive = positive;
  r->headline = ir_explain_text_dup(headline);
  r->reason = ir_explain_text_dup(reason);
  r->fix = ir_explain_text_dup(fix);
  r->verified = ir_explain_text_dup(verified);
}

int ir_explain_has_remark_at(size_t line, const char *entity) {
  for (size_t i = 0; i < g_remark_count; i++) {
    if (g_remarks[i].line == line && g_remarks[i].entity && entity &&
        strcmp(g_remarks[i].entity, entity) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_explain_remark_compare(const void *a, const void *b) {
  const IRExplainRemark *ra = a, *rb = b;
  if (ra->line != rb->line) {
    return ra->line < rb->line ? -1 : 1;
  }
  if (ra->column != rb->column) {
    return ra->column < rb->column ? -1 : 1;
  }
  return 0;
}

/* ---- repeated-refusal aggregation ------------------------------------------
 * Real-world functions (a setup-heavy main, an init routine) produce WALLS of
 * identical call refusals -- one fact ("main is over the caller budget")
 * repeated for every call site, drowning the remarks that matter. Identical
 * (caller, headline, reason, fix) call remarks are folded into one entry with
 * the line range and a deduplicated callee list. Remarks carrying a verified
 * line are never folded: each is a per-site proof. */

#define IR_EXPLAIN_GROUP_MIN 4
#define IR_EXPLAIN_GROUP_LIST_MAX 6

static int ir_explain_str_eq(const char *a, const char *b) {
  if (!a || !b) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

/* A call remark eligible for folding: "call to `f`" entity with a reason
 * (the repeated-refusal shape). */
static int ir_explain_remark_foldable(const IRExplainRemark *r) {
  return r->entity && strncmp(r->entity, "call to ", 8) == 0 && r->reason;
}

/* Verified text participates in the key: a generic per-group claim ("with
 * @inline this will inline") folds with its group, while per-site proofs
 * that differ in wording keep their own entries. */
static int ir_explain_remarks_groupable(const IRExplainRemark *a,
                                        const IRExplainRemark *b) {
  return ir_explain_str_eq(a->function_name, b->function_name) &&
         ir_explain_str_eq(a->headline, b->headline) &&
         ir_explain_str_eq(a->reason, b->reason) &&
         ir_explain_str_eq(a->fix, b->fix) &&
         ir_explain_str_eq(a->verified, b->verified);
}

/* The callee name inside a "call to `f`" entity; "?" when unparsable. */
static void ir_explain_entity_callee(const char *entity, char *buf,
                                     size_t cap) {
  const char *open = entity ? strchr(entity, '`') : NULL;
  const char *close = open ? strchr(open + 1, '`') : NULL;
  if (!open || !close || (size_t)(close - open) >= cap) {
    snprintf(buf, cap, "?");
    return;
  }
  size_t n = (size_t)(close - open - 1);
  memcpy(buf, open + 1, n);
  buf[n] = '\0';
}

/* Build the group's deduplicated callee list ("a, b (x9), c ... and N more")
 * into `out`. Membership is determined by the same predicate the flush loop
 * groups by, starting at the group leader `first`. */
static void ir_explain_group_callee_list(const IRExplainRemark *remarks,
                                         size_t first, char *out, size_t cap) {
  char names[64][96];
  size_t name_counts[64];
  size_t n_names = 0;

  for (size_t j = first; j < g_remark_count; j++) {
    if (!ir_explain_remark_foldable(&remarks[j]) ||
        !ir_explain_remarks_groupable(&remarks[first], &remarks[j])) {
      continue;
    }
    char callee[96];
    ir_explain_entity_callee(remarks[j].entity, callee, sizeof(callee));
    size_t k = 0;
    for (; k < n_names; k++) {
      if (strcmp(names[k], callee) == 0) {
        name_counts[k]++;
        break;
      }
    }
    if (k == n_names && n_names < 64) {
      snprintf(names[n_names], sizeof(names[0]), "%s", callee);
      name_counts[n_names] = 1;
      n_names++;
    }
  }

  size_t written = 0;
  out[0] = '\0';
  size_t shown = n_names < IR_EXPLAIN_GROUP_LIST_MAX
                     ? n_names
                     : IR_EXPLAIN_GROUP_LIST_MAX;
  for (size_t k = 0; k < shown; k++) {
    int n;
    if (name_counts[k] > 1) {
      n = snprintf(out + written, cap - written, "%s%s (x%zu)",
                   k ? ", " : "", names[k], name_counts[k]);
    } else {
      n = snprintf(out + written, cap - written, "%s%s", k ? ", " : "",
                   names[k]);
    }
    if (n < 0 || (size_t)n >= cap - written) {
      return;
    }
    written += (size_t)n;
  }
  if (n_names > shown) {
    snprintf(out + written, cap - written, " ... and %zu more",
             n_names - shown);
  }
}

static void ir_explain_print_header(const char *what) {
  const char *file = g_explain_focus_file
                         ? ir_explain_path_basename(g_explain_focus_file)
                         : "<input>";
  const char *rule = glyph_rule();
  fprintf(stderr, "\n%s%s %s: %s %s%s%s%s%s%s\n", clr(EXPLAIN_BOLD), rule,
          what, file, rule, rule, rule, rule, rule, clr(EXPLAIN_RESET));
}

void ir_explain_flush(void) {
  if (!g_explain) {
    return;
  }

  ir_explain_print_header("optimization report");

  if (g_remark_count == 0) {
    fprintf(stderr, "  (no loops or calls to report)\n\n");
  } else {
    qsort(g_remarks, g_remark_count, sizeof(IRExplainRemark),
          ir_explain_remark_compare);
    char *suppressed = calloc(g_remark_count, 1);
    for (size_t i = 0; i < g_remark_count; i++) {
      const IRExplainRemark *r = &g_remarks[i];
      if (suppressed && suppressed[i]) {
        continue;
      }

      /* Fold a run of identical call refusals into one entry. */
      if (suppressed && ir_explain_remark_foldable(r)) {
        size_t group_count = 0;
        size_t last_line = r->line;
        for (size_t j = i; j < g_remark_count; j++) {
          if (ir_explain_remark_foldable(&g_remarks[j]) &&
              ir_explain_remarks_groupable(r, &g_remarks[j])) {
            group_count++;
            last_line = g_remarks[j].line;
          }
        }
        if (group_count >= IR_EXPLAIN_GROUP_MIN) {
          char callees[512];
          ir_explain_group_callee_list(g_remarks, i, callees,
                                       sizeof(callees));
          fprintf(stderr, "  %s%s%s (%zu calls, lines %zu-%zu): %s%s%s\n",
                  clr(EXPLAIN_BOLD), r->function_name, clr(EXPLAIN_RESET),
                  group_count, r->line, last_line,
                  clr(r->positive ? EXPLAIN_GREEN : EXPLAIN_RED), r->headline,
                  clr(EXPLAIN_RESET));
          fprintf(stderr, "      %s%s reason: %s%s\n", clr(EXPLAIN_DIM),
                  glyph_elbow(), r->reason, clr(EXPLAIN_RESET));
          if (r->fix) {
            fprintf(stderr, "      %s%s fix: %s%s\n", clr(EXPLAIN_DIM),
                    glyph_elbow(), r->fix, clr(EXPLAIN_RESET));
          }
          if (r->verified) {
            fprintf(stderr, "      %s%s verified: %s%s%s\n", clr(EXPLAIN_DIM),
                    glyph_elbow(), clr(EXPLAIN_GREEN), r->verified,
                    clr(EXPLAIN_RESET));
          }
          fprintf(stderr, "      %s%s calls: %s%s\n", clr(EXPLAIN_DIM),
                  glyph_elbow(), callees, clr(EXPLAIN_RESET));
          for (size_t j = i; j < g_remark_count; j++) {
            if (ir_explain_remark_foldable(&g_remarks[j]) &&
                ir_explain_remarks_groupable(r, &g_remarks[j])) {
              suppressed[j] = 1;
            }
          }
          continue;
        }
      }

      fprintf(stderr, "  %s%s%s (%s @ line %zu): %s%s%s\n", clr(EXPLAIN_BOLD),
              r->function_name, clr(EXPLAIN_RESET), r->entity, r->line,
              clr(r->positive ? EXPLAIN_GREEN : EXPLAIN_RED), r->headline,
              clr(EXPLAIN_RESET));
      if (r->reason) {
        fprintf(stderr, "      %s%s reason: %s%s\n", clr(EXPLAIN_DIM),
                glyph_elbow(), r->reason, clr(EXPLAIN_RESET));
      }
      if (r->fix) {
        fprintf(stderr, "      %s%s fix: %s%s\n", clr(EXPLAIN_DIM),
                glyph_elbow(), r->fix, clr(EXPLAIN_RESET));
      }
      if (r->verified) {
        fprintf(stderr, "      %s%s verified: %s%s%s\n", clr(EXPLAIN_DIM),
                glyph_elbow(), clr(EXPLAIN_GREEN), r->verified,
                clr(EXPLAIN_RESET));
      }
    }
    free(suppressed);
    fprintf(stderr, "\n");
  }

  for (size_t i = 0; i < g_remark_count; i++) {
    free(g_remarks[i].function_name);
    free(g_remarks[i].entity);
    free(g_remarks[i].headline);
    free(g_remarks[i].reason);
    free(g_remarks[i].fix);
    free(g_remarks[i].verified);
  }
  free(g_remarks);
  g_remarks = NULL;
  g_remark_count = 0;
  g_remark_capacity = 0;
}

/* ---- backend (codegen) section ------------------------------------------- */

void ir_explain_backend_function(const char *function_name,
                                 const char *filename, int ok,
                                 const char *detail) {
  if (!g_explain || !function_name || !ir_explain_file_enabled(filename)) {
    return;
  }
  for (size_t i = 0; i < g_backend_count; i++) {
    if (strcmp(g_backend[i].function_name, function_name) == 0) {
      return; /* first decision wins; the gate can be probed more than once */
    }
  }
  if (g_backend_count == g_backend_capacity) {
    size_t new_capacity = g_backend_capacity ? g_backend_capacity * 2 : 16;
    IRExplainBackendEntry *grown =
        realloc(g_backend, new_capacity * sizeof(IRExplainBackendEntry));
    if (!grown) {
      return;
    }
    g_backend = grown;
    g_backend_capacity = new_capacity;
  }
  IRExplainBackendEntry *e = &g_backend[g_backend_count++];
  e->function_name = ir_explain_strdup(function_name);
  e->ok = ok;
  e->detail = ir_explain_strdup(detail);
}

/* Translate the MIR gate's terse reason codes ("op:37", "params>4", ...) into
 * a sentence. "op:N" carries an IROpcode -- name it. */
static void ir_explain_backend_reason(const IRExplainBackendEntry *e, char *buf,
                                      size_t cap) {
  if (!e->detail) {
    snprintf(buf, cap, "declined by the eligibility gate");
    return;
  }
  if (strncmp(e->detail, "op:", 3) == 0) {
    int op = atoi(e->detail + 3);
    snprintf(buf, cap,
             "contains `%s`, which the register allocator doesn't cover yet",
             ir_opcode_name((IROpcode)op));
    return;
  }
  if (strcmp(e->detail, "call_unsupported") == 0) {
    snprintf(buf, cap, "contains a call form the register allocator doesn't "
                       "support yet");
    return;
  }
  snprintf(buf, cap, "declined by the eligibility gate (reason code: %s)",
           e->detail);
}

void ir_explain_backend_flush(void) {
  if (!g_explain) {
    return;
  }

  size_t ok_count = 0;
  for (size_t i = 0; i < g_backend_count; i++) {
    ok_count += g_backend[i].ok ? 1 : 0;
  }

  ir_explain_print_header("backend report");
  if (g_backend_count == 0) {
    fprintf(stderr, "  (no functions reached native codegen)\n\n");
  } else {
    fprintf(stderr,
            "  %zu/%zu functions reaching codegen (after inlining) compiled "
            "with the register-allocating backend%s\n",
            ok_count, g_backend_count,
            ok_count == g_backend_count ? "" : "; the rest use baseline "
                                               "(spill-everything) codegen:");
    for (size_t i = 0; i < g_backend_count; i++) {
      if (g_backend[i].ok) {
        continue;
      }
      char reason[256];
      ir_explain_backend_reason(&g_backend[i], reason, sizeof(reason));
      fprintf(stderr, "      %s%s %s%s%s%s: %s%s\n", clr(EXPLAIN_DIM),
              glyph_elbow(), clr(EXPLAIN_RESET), clr(EXPLAIN_BOLD),
              g_backend[i].function_name, clr(EXPLAIN_RESET), reason,
              clr(EXPLAIN_RESET));
    }
    fprintf(stderr, "\n");
  }

  for (size_t i = 0; i < g_backend_count; i++) {
    free(g_backend[i].function_name);
    free(g_backend[i].detail);
  }
  free(g_backend);
  g_backend = NULL;
  g_backend_count = 0;
  g_backend_capacity = 0;
}

/* ---- hypothesis clone ------------------------------------------------------
 * A scratch deep copy of a function for simulating a suggested fix: the
 * caller mutates the clone, re-runs the vectorization stages on it, inspects
 * the result, and destroys it. Parameter names/types are copied because the
 * recognizers consult them (e.g. the uint8* gate on the byte-sum kernel). */

IRFunction *ir_explain_clone_function(const IRFunction *src) {
  if (!src) {
    return NULL;
  }
  IRFunction *clone = ir_function_create(src->name ? src->name : "?");
  if (!clone) {
    return NULL;
  }
  if (src->parameter_count > 0 &&
      !ir_function_set_parameters(clone,
                                  (const char **)src->parameter_names,
                                  (const char **)src->parameter_types,
                                  src->parameter_count)) {
    ir_function_destroy(clone);
    return NULL;
  }
  clone->is_inline = src->is_inline;
  clone->is_noinline = src->is_noinline;
  clone->is_pure = src->is_pure;
  for (size_t i = 0; i < src->instruction_count; i++) {
    if (!ir_function_append_instruction(clone, &src->instructions[i])) {
      ir_function_destroy(clone);
      return NULL;
    }
  }
  return clone;
}

/* ---- kernel descriptions --------------------------------------------------
 * What a vectorized loop actually became, in instruction-level terms a
 * performance programmer recognizes. */

void ir_explain_kernel_desc(const IRInstruction *ins, char *buf, size_t cap) {
  if (!ins) {
    snprintf(buf, cap, "a SIMD kernel");
    return;
  }
  switch (ins->op) {
  case IR_OP_COUNT_WORD_STARTS:
    snprintf(buf, cap, "SSE2 word-start scan, 16 bytes/iteration");
    return;
  case IR_OP_MEMCPY_INLINE:
    snprintf(buf, cap, "inline memcpy (constant size)");
    return;
  case IR_OP_SIMD_SUM_I32:
    snprintf(buf, cap, "vpaddd, 8-wide int32 sum (AVX2)");
    return;
  case IR_OP_SIMD_SUM_U8:
    snprintf(buf, cap, "vpsadbw, 32-wide byte sum (AVX2)");
    return;
  case IR_OP_SIMD_BYTE_MAP:
    snprintf(buf, cap, "32-wide byte map (AVX2)");
    return;
  case IR_OP_SIMD_DOT_I32:
    snprintf(buf, cap, "vpmulld + vpaddd, 8-wide int32 dot product (AVX2)");
    return;
  case IR_OP_SIMD_DOT_I8:
    snprintf(buf, cap, "vpmaddwd, 16-wide int8 dot product (AVX2)");
    return;
  case IR_OP_SIMD_SLP_MAC_I32:
    snprintf(buf, cap, "SLP multiply-accumulate, %lld int32 lanes (AVX2)",
             ins->argument_count > 0 ? ins->arguments[0].int_value : 4LL);
    return;
  case IR_OP_SIMD_SLP_MAC_I8:
    snprintf(buf, cap, "SLP int8 multiply-accumulate tile (AVX2)");
    return;
  case IR_OP_SIMD_SCALE_I32:
    snprintf(buf, cap, "8-wide int32 scale map (AVX2)");
    return;
  case IR_OP_SIMD_CLAMP_I32:
    snprintf(buf, cap, "vpminsd/vpmaxsd, 8-wide int32 clamp (AVX2)");
    return;
  case IR_OP_SIMD_REVERSE_COPY_I32:
    snprintf(buf, cap, "8-wide int32 reverse copy (AVX2)");
    return;
  case IR_OP_LOWER_BOUND_I32:
    snprintf(buf, cap, "branchless lower-bound search");
    return;
  case IR_OP_PREFIX_SUM_I32:
    snprintf(buf, cap, "vectorized int32 prefix sum");
    return;
  case IR_OP_SIMD_MINMAX_I32:
    snprintf(buf, cap, "vpminsd/vpmaxsd, 8-wide int32 min/max scan (AVX2)");
    return;
  case IR_OP_SIMD_SUM_F64:
    snprintf(buf, cap, "vaddpd, 4-wide float64 sum, 2 accumulators (AVX)");
    return;
  case IR_OP_SIMD_SUM_F32:
    snprintf(buf, cap, "vaddps, 8-wide float32 sum, 2 accumulators (AVX)");
    return;
  case IR_OP_SIMD_DOT_F64:
    snprintf(buf, cap, "vfmadd231pd, 4-wide float64 FMA dot product");
    return;
  case IR_OP_SIMD_DOT_F32:
    snprintf(buf, cap, "vfmadd231ps, 8-wide float32 FMA dot product");
    return;
  case IR_OP_SIMD_AFFINE_MAP_F64:
    snprintf(buf, cap, "vfmadd231pd, 4-wide float64 affine map");
    return;
  case IR_OP_SIMD_AFFINE_MAP_F32:
    snprintf(buf, cap, "vfmadd231ps, 8-wide float32 affine map");
    return;
  case IR_OP_SIMD_EXP_F32:
    snprintf(buf, cap, "8-wide float32 exp (Cephes polynomial, AVX2)");
    return;
  case IR_OP_SIMD_I2F_REDUCE_F64:
    snprintf(buf, cap, "4-wide float64 counter reduction (AVX2)");
    return;
  case IR_OP_SIMD_VLOOP_F64: {
    int f32 = ins->float_bits == 32;
    int reduce = ins->argument_count > 0 && ins->arguments[0].int_value == 1;
    snprintf(buf, cap, "%s-wide %s %s (AVX2 general vectorizer)",
             f32 ? "8" : "4", f32 ? "float32" : "float64",
             reduce ? "'+' reduction" : "element-wise map");
    return;
  }
  case IR_OP_SIMD_OUTER_LANE_F64:
    snprintf(buf, cap, "vdivpd, 4 outer iterations in 4-wide float64 lockstep "
                       "(hides the inner recurrence's latency)");
    return;
  case IR_OP_SIMD_MATMUL_N32:
    snprintf(buf, cap, "32x32 int32 matrix-multiply kernel");
    return;
  case IR_OP_SIMD_INSERTION_SORT_I32:
    snprintf(buf, cap, "accelerated int32 insertion sort");
    return;
  default:
    snprintf(buf, cap, "%s (SIMD kernel)", ir_opcode_name(ins->op));
    return;
  }
}
