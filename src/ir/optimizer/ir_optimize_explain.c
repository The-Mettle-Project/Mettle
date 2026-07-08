#include "ir_optimize_internal.h"
#include "../ir_explain_memory.h"

#include <stdarg.h>
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
/* Output binary path (-o): a large report is diverted to a `.explain.txt`
 * sidecar next to it instead of flooding the terminal. */
static const char *g_explain_output_path = NULL;
/* Set while a fix hypothesis is being simulated on a scratch clone: the
 * re-run optimizer passes must not pollute the report with the clone's
 * remarks (the unroller, for one, records remarks from inside the stages). */
static int g_explain_hypothesis = 0;

void ir_explain_set_hypothesis(int active) { g_explain_hypothesis = active; }

void ir_explain_set_output_path(const char *path) {
  g_explain_output_path = path;
}

/* ---- machine-readable report (--explain-json) -------------------------------
 * A `<output-stem>.explain.json` sidecar with the same content as the prose
 * report, for tooling (the editor panel parses this instead of prose). The
 * fragments are accumulated here as sections flush, and finalize assembles
 * the document. */

static int g_explain_json = 0;
/* When set (by --annotate-asm), ir_explain_flush keeps the remark table alive
 * past optimization so the codegen annotator can join it onto emitted asm. */
static int g_explain_retain_remarks = 0;
static char *g_json_buf = NULL;
static size_t g_json_len = 0;
static size_t g_json_cap = 0;

void ir_explain_set_json(int enabled) { g_explain_json = enabled; }

static void ir_explain_json_raw(const char *fmt, ...) {
  va_list args;
  if (!g_explain_json) {
    return;
  }
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) {
    return;
  }
  if (g_json_len + (size_t)needed + 1 > g_json_cap) {
    size_t new_cap = g_json_cap ? g_json_cap * 2 : 4096;
    while (new_cap < g_json_len + (size_t)needed + 1) {
      new_cap *= 2;
    }
    char *grown = realloc(g_json_buf, new_cap);
    if (!grown) {
      return;
    }
    g_json_buf = grown;
    g_json_cap = new_cap;
  }
  va_start(args, fmt);
  vsnprintf(g_json_buf + g_json_len, g_json_cap - g_json_len, fmt, args);
  va_end(args);
  g_json_len += (size_t)needed;
}

/* Append a JSON string literal (quoted, escaped); NULL becomes null. */
static void ir_explain_json_str(const char *s) {
  if (!g_explain_json) {
    return;
  }
  if (!s) {
    ir_explain_json_raw("null");
    return;
  }
  ir_explain_json_raw("\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
    case '"': ir_explain_json_raw("\\\""); break;
    case '\\': ir_explain_json_raw("\\\\"); break;
    case '\n': ir_explain_json_raw("\\n"); break;
    case '\r': ir_explain_json_raw("\\r"); break;
    case '\t': ir_explain_json_raw("\\t"); break;
    default:
      if (c < 0x20) {
        ir_explain_json_raw("\\u%04x", c);
      } else {
        ir_explain_json_raw("%c", c);
      }
    }
  }
  ir_explain_json_raw("\"");
}

/* ---- report buffer ----------------------------------------------------------
 * Both report sections render here first (with color codes; they're stripped
 * if the report goes to a file). Routing happens once, at finalize time, when
 * the total size is known: small reports print to stderr as before, large
 * ones are written to the sidecar with a digest on stderr. */

static char *g_report_buf = NULL;
static size_t g_report_len = 0;
static size_t g_report_cap = 0;

static void ir_explain_emit(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) {
    return;
  }
  if (g_report_len + (size_t)needed + 1 > g_report_cap) {
    size_t new_cap = g_report_cap ? g_report_cap * 2 : 4096;
    while (new_cap < g_report_len + (size_t)needed + 1) {
      new_cap *= 2;
    }
    char *grown = realloc(g_report_buf, new_cap);
    if (!grown) {
      return;
    }
    g_report_buf = grown;
    g_report_cap = new_cap;
  }
  va_start(args, fmt);
  vsnprintf(g_report_buf + g_report_len, g_report_cap - g_report_len, fmt,
            args);
  va_end(args);
  g_report_len += (size_t)needed;
}

/* Digest stats collected while the sections render, for the one-paragraph
 * stderr summary that accompanies a file-diverted report. */
static struct {
  size_t loops_vectorized;
  size_t loops_scalar;
  size_t fixes_verified;
  size_t calls_inlined;
  size_t calls_refused;
  size_t backend_ok;
  size_t backend_total;
  size_t changes_improved;
  size_t changes_regressed;
  int had_baseline;
} g_digest;

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
  size_t depth;   /* loop nest depth (1 = top level); 0 = not a loop/unknown */
} IRExplainRemark;

static IRExplainRemark *g_remarks = NULL;
static size_t g_remark_count = 0;
static size_t g_remark_capacity = 0;

/* Backend (codegen-stage) entries: per function, did it get the
 * register-allocating MIR backend or fall back to baseline codegen? */
typedef struct {
  char *function_name;
  int ok;
  char *detail;        /* gate reason code when !ok */
  size_t instructions; /* non-nop IR size: where baseline codegen COSTS */
} IRExplainBackendEntry;

static IRExplainBackendEntry *g_backend = NULL;
static size_t g_backend_count = 0;
static size_t g_backend_capacity = 0;

/* ---- memory diagnostics (--explain surfacing, fed by the type checker) ---- */
typedef struct {
  int severity; /* 0 = warning, 1 = error */
  size_t line;
  char *headline;
  char *fix; /* may be NULL */
} IRExplainMemNote;

static IRExplainMemNote *g_mem = NULL;
static size_t g_mem_count = 0;
static size_t g_mem_capacity = 0;
static int g_mem_collect = 0;
static char *g_mem_focus = NULL; /* basename to filter by, or NULL */

void ir_optimize_set_explain(int enabled, const char *focus_file) {
  g_explain = enabled;
  g_explain_focus_file = focus_file;
}

int ir_explain_enabled(void) { return g_explain; }

void ir_explain_set_retain_remarks(int enabled) {
  g_explain_retain_remarks = enabled ? 1 : 0;
}

/* --annotate-asm reads the collected remarks to enrich its codegen listing with
 * the same verified vectorization/inlining narration. These accessors expose the
 * remark table read-only without leaking the struct definition. */
size_t ir_explain_remark_count(void) { return g_remark_count; }

int ir_explain_remark_at(size_t i, const char **function_name,
                         const char **entity, size_t *line, int *positive,
                         const char **headline, const char **reason,
                         const char **fix, const char **verified,
                         size_t *depth) {
  if (i >= g_remark_count) {
    return 0;
  }
  const IRExplainRemark *r = &g_remarks[i];
  if (function_name) *function_name = r->function_name;
  if (entity) *entity = r->entity;
  if (line) *line = r->line;
  if (positive) *positive = r->positive;
  if (headline) *headline = r->headline;
  if (reason) *reason = r->reason;
  if (fix) *fix = r->fix;
  if (verified) *verified = r->verified;
  if (depth) *depth = r->depth;
  return 1;
}

static const char *ir_explain_path_basename(const char *path) {
  const char *base = path;
  for (; *path; path++) {
    if (*path == '/' || *path == '\\') {
      base = path + 1;
    }
  }
  return base;
}

void ir_explain_memory_set_collect(int enabled, const char *focus_file) {
  g_mem_collect = enabled;
  free(g_mem_focus);
  g_mem_focus = NULL;
  if (enabled && focus_file) {
    const char *base = ir_explain_path_basename(focus_file);
    if (base && *base) {
      g_mem_focus = strdup(base);
    }
  }
}

void ir_explain_memory_note(const char *file, int severity, size_t line,
                            const char *headline, const char *fix) {
  if (!g_mem_collect || !headline) {
    return;
  }
  /* Scope to the focus file when one is known (mirrors optimizer remarks). A
   * note with an unknown file is kept rather than risk dropping a real one. */
  if (g_mem_focus && file &&
      strcmp(ir_explain_path_basename(file), g_mem_focus) != 0) {
    return;
  }
  if (g_mem_count == g_mem_capacity) {
    size_t new_cap = g_mem_capacity ? g_mem_capacity * 2 : 8;
    IRExplainMemNote *grown = realloc(g_mem, new_cap * sizeof(*grown));
    if (!grown) {
      return;
    }
    g_mem = grown;
    g_mem_capacity = new_cap;
  }
  IRExplainMemNote *n = &g_mem[g_mem_count++];
  n->severity = severity ? 1 : 0;
  n->line = line;
  n->headline = strdup(headline);
  n->fix = fix ? strdup(fix) : NULL;
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
static const char *glyph_arrow(void) {
  return ir_explain_use_unicode() ? "\xE2\x86\x92" : "->";
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
  r->depth = 0;
}

/* Stamp the nest depth on the most recent loop remark at `line` (the
 * contract walker computes containment after recording). 1 = top level. */
void ir_explain_remark_loop_depth(size_t line, size_t depth) {
  for (size_t i = g_remark_count; i > 0; i--) {
    IRExplainRemark *r = &g_remarks[i - 1];
    if (r->line == line && r->entity && strcmp(r->entity, "loop") == 0) {
      r->depth = depth;
      return;
    }
  }
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

/* ---- "since last build" diffing ---------------------------------------------
 * Each explain build writes a compact baseline of its loop/call outcomes to
 * `<output-stem>.explain.base`; the next build compares before rendering and
 * leads the report with what CHANGED -- newly vectorized loops, and (the part
 * benchmarks find too late) regressions. Entities are matched by (function,
 * ordinal within the function) so ordinary edits that shift line numbers do
 * not produce false alarms. */

typedef struct {
  char function_name[128];
  char callee[96]; /* calls only; empty for loops */
  size_t ordinal;
  size_t line;
  char status; /* 'V'/'S' for loops, 'I'/'R' for calls */
  char kind;   /* 'L' or 'C' */
  const char *reason; /* current-side only: points into g_remarks */
} IRExplainBaseKey;

/* Status for diffing, or 0 when the remark is not tracked. "vectorized
 * inner, scalar outer" is intentionally untracked: its own status lives on
 * the inner loop's remark. */
static char ir_explain_remark_status(const IRExplainRemark *r, char *kind) {
  if (!r->entity || !r->headline) {
    return 0;
  }
  if (strcmp(r->entity, "loop") == 0) {
    *kind = 'L';
    if (strncmp(r->headline, "vectorized inner", 16) == 0) {
      return 0;
    }
    if (strncmp(r->headline, "vectorized", 10) == 0) {
      return 'V';
    }
    if (strncmp(r->headline, "NOT vectorized", 14) == 0) {
      return 'S';
    }
    return 0;
  }
  if (strncmp(r->entity, "call to ", 8) == 0) {
    *kind = 'C';
    if (strcmp(r->headline, "inlined") == 0) {
      return 'I';
    }
    if (strcmp(r->headline, "NOT inlined") == 0) {
      return 'R';
    }
    return 0;
  }
  return 0;
}

/* Build the tracked-outcome list from the (already sorted) remarks.
 * Returns a malloc'd array; count in *count_out. */
static IRExplainBaseKey *ir_explain_build_keys(size_t *count_out) {
  IRExplainBaseKey *keys = calloc(g_remark_count ? g_remark_count : 1,
                                  sizeof(IRExplainBaseKey));
  size_t count = 0;
  if (!keys) {
    *count_out = 0;
    return NULL;
  }
  for (size_t i = 0; i < g_remark_count; i++) {
    const IRExplainRemark *r = &g_remarks[i];
    char kind = 0;
    char status = ir_explain_remark_status(r, &kind);
    if (!status) {
      continue;
    }
    IRExplainBaseKey *k = &keys[count];
    snprintf(k->function_name, sizeof(k->function_name), "%s",
             r->function_name ? r->function_name : "?");
    k->callee[0] = '\0';
    if (kind == 'C') {
      ir_explain_entity_callee(r->entity, k->callee, sizeof(k->callee));
    }
    k->kind = kind;
    k->status = status;
    k->line = r->line;
    k->reason = r->reason;
    /* ordinal: how many earlier tracked entries share (kind, fn, callee) */
    k->ordinal = 0;
    for (size_t j = 0; j < count; j++) {
      if (keys[j].kind == kind &&
          strcmp(keys[j].function_name, k->function_name) == 0 &&
          strcmp(keys[j].callee, k->callee) == 0) {
        k->ordinal++;
      }
    }
    count++;
  }
  *count_out = count;
  return keys;
}

/* `<dir>/<stem><suffix>` from the output path; caller frees. */
static char *ir_explain_derived_path(const char *suffix) {
  if (!g_explain_output_path) {
    return NULL;
  }
  size_t base_len = strlen(g_explain_output_path);
  const char *last_dot = NULL;
  for (const char *p = g_explain_output_path; *p; p++) {
    if (*p == '.') {
      last_dot = p;
    } else if (*p == '/' || *p == '\\') {
      last_dot = NULL;
    }
  }
  size_t stem_len = last_dot ? (size_t)(last_dot - g_explain_output_path)
                             : base_len;
  char *path = malloc(stem_len + strlen(suffix) + 1);
  if (!path) {
    return NULL;
  }
  memcpy(path, g_explain_output_path, stem_len);
  strcpy(path + stem_len, suffix);
  return path;
}

static IRExplainBaseKey *ir_explain_read_baseline(size_t *count_out) {
  *count_out = 0;
  char *path = ir_explain_derived_path(".explain.base");
  if (!path) {
    return NULL;
  }
  FILE *in = fopen(path, "rb");
  free(path);
  if (!in) {
    return NULL;
  }
  IRExplainBaseKey *keys = NULL;
  size_t count = 0, capacity = 0;
  char line[512];
  while (fgets(line, sizeof(line), in)) {
    char kind = line[0];
    if ((kind != 'L' && kind != 'C') || line[1] != '\t') {
      continue;
    }
    if (count == capacity) {
      size_t new_capacity = capacity ? capacity * 2 : 64;
      IRExplainBaseKey *grown =
          realloc(keys, new_capacity * sizeof(IRExplainBaseKey));
      if (!grown) {
        break;
      }
      keys = grown;
      capacity = new_capacity;
    }
    IRExplainBaseKey *k = &keys[count];
    memset(k, 0, sizeof(*k));
    k->kind = kind;
    /* L \t fn \t ordinal \t status \t line
     * C \t fn \t callee \t ordinal \t status \t line */
    char *cursor = line + 2;
    char *fields[5] = {0};
    int n_fields = 0;
    while (cursor && n_fields < 5) {
      fields[n_fields++] = cursor;
      cursor = strchr(cursor, '\t');
      if (cursor) {
        *cursor++ = '\0';
      }
    }
    int needed = kind == 'L' ? 4 : 5;
    if (n_fields < needed) {
      continue;
    }
    snprintf(k->function_name, sizeof(k->function_name), "%s", fields[0]);
    int field = 1;
    if (kind == 'C') {
      snprintf(k->callee, sizeof(k->callee), "%s", fields[field++]);
    }
    k->ordinal = (size_t)strtoul(fields[field++], NULL, 10);
    k->status = fields[field++][0];
    k->line = (size_t)strtoul(fields[field], NULL, 10);
    count++;
  }
  fclose(in);
  *count_out = count;
  return keys;
}

static void ir_explain_write_baseline(const IRExplainBaseKey *keys,
                                      size_t count) {
  char *path = ir_explain_derived_path(".explain.base");
  if (!path) {
    return;
  }
  FILE *out = fopen(path, "wb");
  free(path);
  if (!out) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    const IRExplainBaseKey *k = &keys[i];
    if (k->kind == 'L') {
      fprintf(out, "L\t%s\t%zu\t%c\t%zu\n", k->function_name, k->ordinal,
              k->status, k->line);
    } else {
      fprintf(out, "C\t%s\t%s\t%zu\t%c\t%zu\n", k->function_name, k->callee,
              k->ordinal, k->status, k->line);
    }
  }
  fclose(out);
}

static const IRExplainBaseKey *
ir_explain_find_key(const IRExplainBaseKey *keys, size_t count,
                    const IRExplainBaseKey *want) {
  for (size_t i = 0; i < count; i++) {
    if (keys[i].kind == want->kind && keys[i].ordinal == want->ordinal &&
        strcmp(keys[i].function_name, want->function_name) == 0 &&
        strcmp(keys[i].callee, want->callee) == 0) {
      return &keys[i];
    }
  }
  return NULL;
}

/* Compare against the previous build, render the "changes" section, emit the
 * JSON changes object, update the digest, and rewrite the baseline. Runs on
 * the SORTED remark list before the main listing renders. */
static void ir_explain_render_changes(void) {
  size_t current_count = 0, old_count = 0;
  IRExplainBaseKey *current = ir_explain_build_keys(&current_count);
  IRExplainBaseKey *old = ir_explain_read_baseline(&old_count);

  if (current) {
    if (old) {
      size_t improved = 0, regressed = 0;
      ir_explain_json_raw("\"changes\":{\"baseline\":true,\"entries\":[");
      size_t json_entries = 0;
      for (size_t i = 0; i < current_count; i++) {
        const IRExplainBaseKey *was =
            ir_explain_find_key(old, old_count, &current[i]);
        if (!was || was->status == current[i].status) {
          continue;
        }
        const char *what = current[i].kind == 'L' ? "loop" : "call";
        int now_better = current[i].status == 'V' || current[i].status == 'I';
        if ((improved + regressed) == 0) {
          ir_explain_emit("  %schanges since the last explain build:%s\n",
                          clr(EXPLAIN_BOLD), clr(EXPLAIN_RESET));
        }
        if (now_better) {
          improved++;
          ir_explain_emit(
              "    %s+ %s (%s @ line %zu): now %s%s\n", clr(EXPLAIN_GREEN),
              current[i].function_name, what, current[i].line,
              current[i].kind == 'L' ? "vectorized" : "inlined",
              clr(EXPLAIN_RESET));
        } else {
          regressed++;
          ir_explain_emit(
              "    %s%s- %s (%s @ line %zu): REGRESSED %s was %s%s\n",
              clr(EXPLAIN_BOLD), clr(EXPLAIN_RED), current[i].function_name,
              what, current[i].line, glyph_arrow(),
              current[i].kind == 'L' ? "vectorized, now scalar"
                                     : "inlined, now a real call",
              clr(EXPLAIN_RESET));
          if (current[i].reason) {
            ir_explain_emit("        %s%s reason: %s%s\n", clr(EXPLAIN_DIM),
                            glyph_elbow(), current[i].reason,
                            clr(EXPLAIN_RESET));
          }
        }
        ir_explain_json_raw("%s{\"kind\":\"%s\",\"fn\":",
                            json_entries++ ? "," : "", what);
        ir_explain_json_str(current[i].function_name);
        ir_explain_json_raw(",\"line\":%zu,\"direction\":\"%s\",\"reason\":",
                            current[i].line,
                            now_better ? "improved" : "regressed");
        ir_explain_json_str(now_better ? NULL : current[i].reason);
        ir_explain_json_raw("}");
      }
      if ((improved + regressed) == 0) {
        ir_explain_emit("  %sno optimization changes since the last explain "
                        "build%s\n",
                        clr(EXPLAIN_DIM), clr(EXPLAIN_RESET));
      }
      ir_explain_emit("\n");
      ir_explain_json_raw("]},");
      g_digest.changes_improved = improved;
      g_digest.changes_regressed = regressed;
      g_digest.had_baseline = 1;
    } else {
      ir_explain_json_raw("\"changes\":{\"baseline\":false,\"entries\":[]},");
    }
    ir_explain_write_baseline(current, current_count);
  }
  free(current);
  free(old);
}

static void ir_explain_print_header(const char *what) {
  const char *file = g_explain_focus_file
                         ? ir_explain_path_basename(g_explain_focus_file)
                         : "<input>";
  const char *rule = glyph_rule();
  ir_explain_emit("\n%s%s %s: %s %s%s%s%s%s%s\n", clr(EXPLAIN_BOLD), rule,
                  what, file, rule, rule, rule, rule, rule,
                  clr(EXPLAIN_RESET));
}

/* One remark as a JSON object in the "remarks" array. `kind` is explicit so
 * consumers never re-derive it from prose. */
static void ir_explain_json_remark(const IRExplainRemark *r, const char *kind,
                                   const char *callee, size_t count,
                                   size_t line_end, const char *calls,
                                   size_t *json_count) {
  if (!g_explain_json) {
    return;
  }
  ir_explain_json_raw("%s{\"kind\":\"%s\",\"fn\":", (*json_count)++ ? "," : "",
                      kind);
  ir_explain_json_str(r->function_name);
  ir_explain_json_raw(",\"entity\":");
  ir_explain_json_str(r->entity);
  ir_explain_json_raw(",\"line\":%zu,\"positive\":%s,\"headline\":", r->line,
                      r->positive ? "true" : "false");
  ir_explain_json_str(r->headline);
  ir_explain_json_raw(",\"reason\":");
  ir_explain_json_str(r->reason);
  ir_explain_json_raw(",\"fix\":");
  ir_explain_json_str(r->fix);
  ir_explain_json_raw(",\"verified\":");
  ir_explain_json_str(r->verified);
  ir_explain_json_raw(",\"callee\":");
  ir_explain_json_str(callee);
  if (count > 1) {
    ir_explain_json_raw(",\"count\":%zu,\"lineEnd\":%zu,\"calls\":", count,
                        line_end);
    ir_explain_json_str(calls);
  }
  if (r->depth > 0) {
    ir_explain_json_raw(",\"depth\":%zu", r->depth);
  }
  ir_explain_json_raw("}");
}

/* Render the memory diagnostics the type checker handed us: a JSON "memory"
 * array (always emitted so the document's comma chain stays valid) and a prose
 * "memory report" section. Called at the tail of ir_explain_flush, so it lands
 * after "remarks" and before "backend" in the JSON buffer. */
static void ir_explain_memory_flush(void) {
  if (g_explain_json) {
    ir_explain_json_raw("\"memory\":[");
    for (size_t i = 0; i < g_mem_count; i++) {
      const IRExplainMemNote *n = &g_mem[i];
      ir_explain_json_raw("%s{\"severity\":", i ? "," : "");
      ir_explain_json_str(n->severity ? "error" : "warning");
      ir_explain_json_raw(",\"line\":%zu,\"headline\":", n->line);
      ir_explain_json_str(n->headline);
      ir_explain_json_raw(",\"fix\":");
      ir_explain_json_str(n->fix);
      ir_explain_json_raw("}");
    }
    ir_explain_json_raw("],");
  }

  if (!g_explain) {
    return;
  }
  ir_explain_print_header("memory report");
  if (g_mem_count == 0) {
    ir_explain_emit("  %sno memory diagnostics in this file%s\n\n",
                    clr(EXPLAIN_DIM), clr(EXPLAIN_RESET));
    return;
  }
  size_t errors = 0, warnings = 0;
  for (size_t i = 0; i < g_mem_count; i++) {
    if (g_mem[i].severity) {
      errors++;
    } else {
      warnings++;
    }
  }
  ir_explain_emit("  %zu issue%s (%zu error%s, %zu warning%s):\n", g_mem_count,
                  g_mem_count == 1 ? "" : "s", errors, errors == 1 ? "" : "s",
                  warnings, warnings == 1 ? "" : "s");
  for (size_t i = 0; i < g_mem_count; i++) {
    const IRExplainMemNote *n = &g_mem[i];
    ir_explain_emit("  %s%s%s (line %zu): %s\n",
                    clr(n->severity ? EXPLAIN_RED : EXPLAIN_BOLD),
                    n->severity ? "error" : "warning", clr(EXPLAIN_RESET),
                    n->line, n->headline);
    if (n->fix) {
      ir_explain_emit("      %s%s fix: %s%s\n", clr(EXPLAIN_DIM), glyph_elbow(),
                      n->fix, clr(EXPLAIN_RESET));
    }
  }
  ir_explain_emit("\n");
}

void ir_explain_flush(void) {
  if (!g_explain) {
    return;
  }

  ir_explain_print_header("optimization report");

  if (g_remark_count > 0) {
    qsort(g_remarks, g_remark_count, sizeof(IRExplainRemark),
          ir_explain_remark_compare);
  }
  ir_explain_render_changes();

  size_t json_remark_count = 0;
  ir_explain_json_raw("\"remarks\":[");

  if (g_remark_count == 0) {
    ir_explain_emit("  (no loops or calls to report)\n\n");
  } else {
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
          ir_explain_emit("  %s%s%s (%zu calls, lines %zu-%zu): %s%s%s\n",
                          clr(EXPLAIN_BOLD), r->function_name,
                          clr(EXPLAIN_RESET), group_count, r->line, last_line,
                          clr(r->positive ? EXPLAIN_GREEN : EXPLAIN_RED),
                          r->headline, clr(EXPLAIN_RESET));
          ir_explain_emit("      %s%s reason: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), r->reason, clr(EXPLAIN_RESET));
          if (r->fix) {
            ir_explain_emit("      %s%s fix: %s%s\n", clr(EXPLAIN_DIM),
                            glyph_elbow(), r->fix, clr(EXPLAIN_RESET));
          }
          if (r->verified) {
            ir_explain_emit("      %s%s verified: %s%s%s\n", clr(EXPLAIN_DIM),
                            glyph_elbow(), clr(EXPLAIN_GREEN), r->verified,
                            clr(EXPLAIN_RESET));
          }
          ir_explain_emit("      %s%s calls: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), callees, clr(EXPLAIN_RESET));
          if (strcmp(r->headline, "NOT inlined") == 0) {
            g_digest.calls_refused += group_count;
          }
          ir_explain_json_remark(r, "calls-folded", NULL, group_count,
                                 last_line, callees, &json_remark_count);
          for (size_t j = i; j < g_remark_count; j++) {
            if (ir_explain_remark_foldable(&g_remarks[j]) &&
                ir_explain_remarks_groupable(r, &g_remarks[j])) {
              suppressed[j] = 1;
            }
          }
          continue;
        }
      }

      ir_explain_emit("  %s%s%s (%s @ line %zu): %s%s%s\n", clr(EXPLAIN_BOLD),
                      r->function_name, clr(EXPLAIN_RESET), r->entity, r->line,
                      clr(r->positive ? EXPLAIN_GREEN : EXPLAIN_RED),
                      r->headline, clr(EXPLAIN_RESET));
      if (r->reason) {
        ir_explain_emit("      %s%s reason: %s%s\n", clr(EXPLAIN_DIM),
                        glyph_elbow(), r->reason, clr(EXPLAIN_RESET));
      }
      if (r->fix) {
        ir_explain_emit("      %s%s fix: %s%s\n", clr(EXPLAIN_DIM),
                        glyph_elbow(), r->fix, clr(EXPLAIN_RESET));
      }
      if (r->verified) {
        ir_explain_emit("      %s%s verified: %s%s%s\n", clr(EXPLAIN_DIM),
                        glyph_elbow(), clr(EXPLAIN_GREEN), r->verified,
                        clr(EXPLAIN_RESET));
        g_digest.fixes_verified++;
      }
      /* Digest tallies (loop/call outcomes by entity + headline). */
      if (r->entity && strcmp(r->entity, "loop") == 0) {
        if (strncmp(r->headline, "vectorized", 10) == 0) {
          g_digest.loops_vectorized++;
        } else if (strncmp(r->headline, "NOT vectorized", 14) == 0) {
          g_digest.loops_scalar++;
        }
        ir_explain_json_remark(r, "loop", NULL, 1, 0, NULL,
                               &json_remark_count);
      } else if (r->entity && strncmp(r->entity, "call to ", 8) == 0) {
        if (strcmp(r->headline, "inlined") == 0) {
          g_digest.calls_inlined++;
        } else if (strcmp(r->headline, "NOT inlined") == 0) {
          g_digest.calls_refused++;
        }
        char callee[96];
        ir_explain_entity_callee(r->entity, callee, sizeof(callee));
        ir_explain_json_remark(r, "call", callee, 1, 0, NULL,
                               &json_remark_count);
      } else {
        ir_explain_json_remark(
            r, r->entity && strcmp(r->entity, "function") == 0 ? "function"
                                                               : "other",
            NULL, 1, 0, NULL, &json_remark_count);
      }
    }
    free(suppressed);
    ir_explain_emit("\n");
  }
  ir_explain_json_raw("],");

  /* --annotate-asm reads the remarks during codegen (which runs AFTER this
   * optimization-stage flush), so when retention is requested we keep them
   * alive; the one-shot compile leaks them at exit. */
  if (!g_explain_retain_remarks) {
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

  /* Memory diagnostics land after "remarks" and before "backend". */
  ir_explain_memory_flush();
}

/* ---- backend (codegen) section ------------------------------------------- */

void ir_explain_backend_function(const char *function_name,
                                 const char *filename, int ok,
                                 const char *detail, size_t instructions) {
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
  e->instructions = instructions;
}

/* ---- report routing ---------------------------------------------------------
 * Small reports print to stderr exactly as before. Past a line threshold (a
 * real application produces hundreds of remarks) the full report is written
 * to `<output-stem>.explain.txt` next to the output binary, and stderr gets a
 * one-paragraph digest with the path. */

#define IR_EXPLAIN_STDERR_MAX_LINES 200

static size_t ir_explain_report_lines(void) {
  size_t lines = 0;
  for (size_t i = 0; i < g_report_len; i++) {
    lines += (g_report_buf[i] == '\n') ? 1 : 0;
  }
  return lines;
}

/* `<dir>/<stem>.explain.txt` from the output path; caller frees. */
static char *ir_explain_sidecar_path(void) {
  return ir_explain_derived_path(".explain.txt");
}

/* Write the buffer with ANSI color sequences stripped (the report renders
 * with stderr in mind; a file must stay plain). */
static int ir_explain_write_plain(FILE *out) {
  for (size_t i = 0; i < g_report_len; i++) {
    if (g_report_buf[i] == '\x1b' && i + 1 < g_report_len &&
        g_report_buf[i + 1] == '[') {
      i += 2;
      while (i < g_report_len && g_report_buf[i] != 'm') {
        i++;
      }
      continue;
    }
    if (fputc(g_report_buf[i], out) == EOF) {
      return 0;
    }
  }
  return 1;
}

void ir_explain_finalize(int force_stderr) {
  if (!g_explain || !g_report_buf || g_report_len == 0) {
    return;
  }

  size_t threshold = IR_EXPLAIN_STDERR_MAX_LINES;
  const char *env = getenv("METTLE_EXPLAIN_REPORT_LINES");
  if (env && env[0]) {
    long v = atol(env);
    threshold = (v <= 0) ? (size_t)-1 : (size_t)v;
  }

  char *sidecar = NULL;
  int diverted = 0;
  if (!force_stderr && ir_explain_report_lines() > threshold &&
      (sidecar = ir_explain_sidecar_path()) != NULL) {
    FILE *out = fopen(sidecar, "wb");
    if (out) {
      diverted = ir_explain_write_plain(out);
      fclose(out);
    }
  }

  /* The machine-readable sidecar, independent of where the prose went. */
  if (g_explain_json && g_json_buf) {
    char *json_path = ir_explain_derived_path(".explain.json");
    if (json_path) {
      FILE *out = fopen(json_path, "wb");
      if (out) {
        const char *source = g_explain_focus_file
                                 ? ir_explain_path_basename(g_explain_focus_file)
                                 : "";
        fprintf(out, "{\"schema\":1,\"source\":\"%s\",", source);
        fwrite(g_json_buf, 1, g_json_len, out);
        fprintf(out,
                "\"stats\":{\"loopsVectorized\":%zu,\"loopsScalar\":%zu,"
                "\"fixesVerified\":%zu,\"callsInlined\":%zu,"
                "\"callsRefused\":%zu,\"changesImproved\":%zu,"
                "\"changesRegressed\":%zu,\"hadBaseline\":%s}}\n",
                g_digest.loops_vectorized, g_digest.loops_scalar,
                g_digest.fixes_verified, g_digest.calls_inlined,
                g_digest.calls_refused, g_digest.changes_improved,
                g_digest.changes_regressed,
                g_digest.had_baseline ? "true" : "false");
        fclose(out);
      }
      free(json_path);
    }
  }
  free(g_json_buf);
  g_json_buf = NULL;
  g_json_len = 0;
  g_json_cap = 0;

  for (size_t i = 0; i < g_mem_count; i++) {
    free(g_mem[i].headline);
    free(g_mem[i].fix);
  }
  free(g_mem);
  g_mem = NULL;
  g_mem_count = 0;
  g_mem_capacity = 0;
  free(g_mem_focus);
  g_mem_focus = NULL;

  if (!diverted) {
    fwrite(g_report_buf, 1, g_report_len, stderr);
  } else {
    /* The digest: the report's conclusions in five lines, plus the path.
     * Regressions lead -- they must never hide inside a sidecar. */
    fprintf(stderr, "\n%s%s optimization report %s%s%s%s%s%s%s\n",
            clr(EXPLAIN_BOLD), glyph_rule(), glyph_rule(), glyph_rule(),
            glyph_rule(), glyph_rule(), glyph_rule(), glyph_rule(),
            clr(EXPLAIN_RESET));
    if (g_digest.changes_regressed > 0) {
      fprintf(stderr,
              "  %s%s%zu optimization%s REGRESSED since the last build%s "
              "(see the changes section of the report)\n",
              clr(EXPLAIN_BOLD), clr(EXPLAIN_RED), g_digest.changes_regressed,
              g_digest.changes_regressed == 1 ? "" : "s", clr(EXPLAIN_RESET));
    } else if (g_digest.changes_improved > 0) {
      fprintf(stderr, "  %s%zu optimization%s improved since the last build%s\n",
              clr(EXPLAIN_GREEN), g_digest.changes_improved,
              g_digest.changes_improved == 1 ? "" : "s", clr(EXPLAIN_RESET));
    }
    fprintf(stderr,
            "  loops: %s%zu vectorized%s, %zu scalar; %s%zu fix suggestions "
            "verified by simulation%s\n",
            clr(EXPLAIN_GREEN), g_digest.loops_vectorized, clr(EXPLAIN_RESET),
            g_digest.loops_scalar, clr(EXPLAIN_GREEN), g_digest.fixes_verified,
            clr(EXPLAIN_RESET));
    fprintf(stderr, "  calls: %zu inlined, %zu kept as real calls\n",
            g_digest.calls_inlined, g_digest.calls_refused);
    if (g_digest.backend_total > 0) {
      fprintf(stderr,
              "  backend: %zu/%zu functions register-allocated\n",
              g_digest.backend_ok, g_digest.backend_total);
    }
    fprintf(stderr, "  full report (%zu lines): %s%s%s\n\n",
            ir_explain_report_lines(), clr(EXPLAIN_BOLD), sidecar,
            clr(EXPLAIN_RESET));
  }

  free(sidecar);
  free(g_report_buf);
  g_report_buf = NULL;
  g_report_len = 0;
  g_report_cap = 0;
  memset(&g_digest, 0, sizeof(g_digest));
}

/* Translate the MIR gate's terse reason codes ("op:37", "params>4", ...) into
 * a sentence, plus -- where the family is understood -- what falling back
 * actually COSTS and what (if anything) the user can do about it. "op:N"
 * carries an IROpcode; SIMD kernels get their own family because the common
 * misreading is "my vectorized function is slow now" (it isn't: the kernel
 * runs at full speed, only surrounding scalar code spills). */
static void ir_explain_backend_reason(const IRExplainBackendEntry *e, char *buf,
                                      size_t cap, const char **consequence,
                                      const char **fix) {
  *consequence = NULL;
  *fix = NULL;
  if (!e->detail) {
    snprintf(buf, cap, "declined by the eligibility gate");
    return;
  }
  if (strncmp(e->detail, "op:", 3) == 0) {
    int op = atoi(e->detail + 3);
    if (op >= (int)IR_OP_COUNT_WORD_STARTS &&
        op <= (int)IR_OP_SIMD_OUTER_LANE_F64) {
      snprintf(buf, cap,
               "contains the SIMD kernel `%s`, which the register allocator "
               "doesn't cover yet",
               ir_opcode_name((IROpcode)op));
      *consequence =
          "the kernel itself runs at full vector speed; only the scalar code "
          "around it keeps values on the stack";
      *fix = "nothing for small functions; if a LARGE function mixes a kernel "
             "with hot scalar code, move the kernel loop into its own small "
             "function so the scalar part keeps the register allocator";
      return;
    }
    snprintf(buf, cap,
             "contains `%s`, which the register allocator doesn't cover yet",
             ir_opcode_name((IROpcode)op));
    *consequence = "every value in the function is kept on the stack instead "
                   "of in registers";
    return;
  }
  if (strcmp(e->detail, "call_unsupported") == 0) {
    snprintf(buf, cap, "contains a call form the register allocator doesn't "
                       "support yet");
    *consequence = "every value in the function is kept on the stack instead "
                   "of in registers";
    return;
  }
  /* A SIMD kernel whose inline-passthrough subset doesn't yet cover this loop's
   * exact shape (e.g. a mode-2 fill, or an affine map with a runtime
   * coefficient). The kernel still vectorizes; only the scalar code around it
   * keeps values on the stack -- the same consequence as an unsupported op. */
  if (strncmp(e->detail, "simd_fill:", 10) == 0 ||
      strncmp(e->detail, "affine_map:", 11) == 0) {
    const char *kernel =
        e->detail[0] == 's' ? "simd_fill" : "simd_affine_map_f32";
    snprintf(buf, cap,
             "contains the SIMD kernel `%s` in a form the register allocator's "
             "inline passthrough doesn't cover yet",
             kernel);
    *consequence =
        "the kernel itself runs at full vector speed; only the scalar code "
        "around it keeps values on the stack";
    *fix = "nothing needed for a small function; a different shape of the same "
           "kernel (a simpler fill/map) inlines into register-allocated code";
    return;
  }
  snprintf(buf, cap, "declined by the eligibility gate (reason code: %s)",
           e->detail);
}

/* Sort helper: biggest functions first -- size is where baseline codegen
 * costs, so the list reads as a priority queue. */
static int ir_explain_backend_size_compare(const void *a, const void *b) {
  const IRExplainBackendEntry *ea = a, *eb = b;
  if (ea->instructions != eb->instructions) {
    return ea->instructions > eb->instructions ? -1 : 1;
  }
  return strcmp(ea->function_name ? ea->function_name : "",
                eb->function_name ? eb->function_name : "");
}

void ir_explain_backend_flush(void) {
  if (!g_explain) {
    return;
  }

  size_t ok_count = 0;
  size_t total_instructions = 0;
  size_t ok_instructions = 0;
  for (size_t i = 0; i < g_backend_count; i++) {
    ok_count += g_backend[i].ok ? 1 : 0;
    total_instructions += g_backend[i].instructions;
    ok_instructions += g_backend[i].ok ? g_backend[i].instructions : 0;
  }
  g_digest.backend_ok = ok_count;
  g_digest.backend_total = g_backend_count;

  ir_explain_json_raw(
      "\"backend\":{\"ok\":%zu,\"total\":%zu,\"instructions\":%zu,"
      "\"okInstructions\":%zu,\"groups\":[",
      ok_count, g_backend_count, total_instructions, ok_instructions);
  size_t json_group_count = 0;

  ir_explain_print_header("backend report");
  if (g_backend_count == 0) {
    ir_explain_emit("  (no functions reached native codegen)\n\n");
  } else {
    ir_explain_emit(
        "  %zu/%zu functions reaching codegen (after inlining) compiled with "
        "the register-allocating backend\n",
        ok_count, g_backend_count);
    if (total_instructions > 0) {
      ir_explain_emit(
          "  %.1f%% of the program's %zu optimized IR instructions are in "
          "register-allocated code\n",
          100.0 * (double)ok_instructions / (double)total_instructions,
          total_instructions);
    }

    if (ok_count < g_backend_count) {
      ir_explain_emit(
          "\n  %zu function%s use%s baseline (spill-everything) codegen, "
          "grouped by cause, largest first:\n",
          g_backend_count - ok_count,
          g_backend_count - ok_count == 1 ? "" : "s",
          g_backend_count - ok_count == 1 ? "s" : "");

      /* Group the bailed entries by their rendered reason sentence, ordered
       * by the group's total instruction count (where the cost actually
       * is). Entries were sorted by size already, so each group's function
       * list reads largest-first. */
      qsort(g_backend, g_backend_count, sizeof(IRExplainBackendEntry),
            ir_explain_backend_size_compare);
      char *grouped = calloc(g_backend_count, 1);
      for (;;) {
        /* Pick the ungrouped reason with the largest remaining total. */
        char best_reason[256];
        const char *best_consequence = NULL, *best_fix = NULL;
        size_t best_total = 0, best_first = (size_t)-1;
        for (size_t i = 0; i < g_backend_count; i++) {
          if (g_backend[i].ok || (grouped && grouped[i])) {
            continue;
          }
          char reason_i[256];
          const char *cons_i, *fix_i;
          ir_explain_backend_reason(&g_backend[i], reason_i, sizeof(reason_i),
                                    &cons_i, &fix_i);
          size_t total_i = 0;
          for (size_t j = i; j < g_backend_count; j++) {
            if (g_backend[j].ok || (grouped && grouped[j])) {
              continue;
            }
            char reason_j[256];
            const char *cj, *fj;
            ir_explain_backend_reason(&g_backend[j], reason_j,
                                      sizeof(reason_j), &cj, &fj);
            if (strcmp(reason_i, reason_j) == 0) {
              total_i += g_backend[j].instructions;
            }
          }
          if (best_first == (size_t)-1 || total_i > best_total) {
            snprintf(best_reason, sizeof(best_reason), "%s", reason_i);
            best_consequence = cons_i;
            best_fix = fix_i;
            best_total = total_i;
            best_first = i;
          }
        }
        if (best_first == (size_t)-1) {
          break;
        }

        /* Render the group: header, consequence/fix once, then the largest
         * members with sizes. */
        size_t members = 0;
        for (size_t j = best_first; j < g_backend_count; j++) {
          if (g_backend[j].ok || (grouped && grouped[j])) {
            continue;
          }
          char reason_j[256];
          const char *cj, *fj;
          ir_explain_backend_reason(&g_backend[j], reason_j, sizeof(reason_j),
                                    &cj, &fj);
          if (strcmp(best_reason, reason_j) == 0) {
            members++;
          }
        }
        ir_explain_emit("\n  %s%s%s (%zu function%s, %zu instructions):\n",
                        clr(EXPLAIN_BOLD), best_reason, clr(EXPLAIN_RESET),
                        members, members == 1 ? "" : "s", best_total);
        if (best_consequence) {
          ir_explain_emit("      %s%s consequence: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), best_consequence,
                          clr(EXPLAIN_RESET));
        }
        if (best_fix) {
          ir_explain_emit("      %s%s fix: %s%s\n", clr(EXPLAIN_DIM),
                          glyph_elbow(), best_fix, clr(EXPLAIN_RESET));
        }
        ir_explain_json_raw("%s{\"reason\":", json_group_count++ ? "," : "");
        ir_explain_json_str(best_reason);
        ir_explain_json_raw(",\"functions\":%zu,\"instructions\":%zu,"
                            "\"consequence\":",
                            members, best_total);
        ir_explain_json_str(best_consequence);
        ir_explain_json_raw(",\"fix\":");
        ir_explain_json_str(best_fix);
        ir_explain_json_raw(",\"members\":[");
        size_t shown = 0;
        char list[512];
        size_t written = 0;
        list[0] = '\0';
        for (size_t j = best_first; j < g_backend_count && shown < 6; j++) {
          if (g_backend[j].ok || (grouped && grouped[j])) {
            continue;
          }
          char reason_j[256];
          const char *cj, *fj;
          ir_explain_backend_reason(&g_backend[j], reason_j, sizeof(reason_j),
                                    &cj, &fj);
          if (strcmp(best_reason, reason_j) != 0) {
            continue;
          }
          int n = snprintf(list + written, sizeof(list) - written,
                           "%s%s (%zu)", shown ? ", " : "",
                           g_backend[j].function_name, g_backend[j].instructions);
          if (n < 0 || (size_t)n >= sizeof(list) - written) {
            break;
          }
          written += (size_t)n;
          ir_explain_json_raw("%s{\"fn\":", shown ? "," : "");
          ir_explain_json_str(g_backend[j].function_name);
          ir_explain_json_raw(",\"instructions\":%zu}",
                              g_backend[j].instructions);
          shown++;
        }
        ir_explain_json_raw("]}");
        ir_explain_emit("      %s%s %s%s%s%s\n", clr(EXPLAIN_DIM),
                        glyph_elbow(),
                        members > shown ? "largest: " : "", list,
                        members > shown ? " ..." : "", clr(EXPLAIN_RESET));
        if (members > shown) {
          ir_explain_emit("      %s%s   ... and %zu more%s\n",
                          clr(EXPLAIN_DIM), glyph_elbow(), members - shown,
                          clr(EXPLAIN_RESET));
        }
        for (size_t j = best_first; j < g_backend_count; j++) {
          if (g_backend[j].ok || (grouped && grouped[j])) {
            continue;
          }
          char reason_j[256];
          const char *cj, *fj;
          ir_explain_backend_reason(&g_backend[j], reason_j, sizeof(reason_j),
                                    &cj, &fj);
          if (grouped && strcmp(best_reason, reason_j) == 0) {
            grouped[j] = 1;
          }
        }
        if (!grouped) {
          break; /* allocation failed: rendered the largest group, stop */
        }
      }
      free(grouped);
    }
    ir_explain_emit("\n");
  }

  ir_explain_json_raw("]},");

  for (size_t i = 0; i < g_backend_count; i++) {
    free(g_backend[i].function_name);
    free(g_backend[i].detail);
  }
  free(g_backend);
  g_backend = NULL;
  g_backend_count = 0;
  g_backend_capacity = 0;

  ir_explain_finalize(0);
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
  case IR_OP_SIMD_FILL:
    snprintf(buf, cap, "16-byte splat stores (vectorized fill/memset)");
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
  case IR_OP_SIMD_SILU_F32:
    snprintf(buf, cap, "8-wide float32 SiLU/SwiGLU gate (AVX2 exp poly)");
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
  case IR_OP_SIMD_VLOOP_I32: {
    int reduce = ins->argument_count > 0 && ins->arguments[0].int_value == 1;
    snprintf(buf, cap, "8-wide int32 %s (AVX2 general vectorizer, bit-exact)",
             reduce ? "'+' reduction" : "element-wise map");
    return;
  }
  case IR_OP_SIMD_FIND: {
    int u8 = ins->argument_count > 1 && ins->arguments[1].int_value == 1;
    snprintf(buf, cap,
             "%s-wide %s search skip-ahead (AVX2 compare+movemask; the scalar "
             "loop replays only the hit iteration, exits exact)",
             u8 ? "32" : "8", u8 ? "byte" : "int32");
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

/* Render the --ml-opt report from the native pass's TSV (fn, gidx, kind, before,
 * after, saved, line, file), styled like the main report. */
static const char *glyph_ellipsis(void) {
  return ir_explain_use_unicode() ? "\xE2\x80\xA6" : "..";
}

static void ml_fit(const char *s, int width, char *out, size_t cap) {
  int len = (int)strlen(s);
  if (len <= width) {
    snprintf(out, cap, "%-*s", width, s);
    return;
  }
  int keep = width - 2;
  if (keep < 1) keep = 1;
  snprintf(out, cap, "%.*s%s", keep, s, glyph_ellipsis());
}

void ir_explain_ml_opt(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    return;
  }
  const char *rule = glyph_rule();
  fprintf(stderr, "\n%s%s ml-opt: model-driven IR optimizations %s%s%s%s%s\n\n",
          clr(EXPLAIN_BOLD), rule, rule, rule, rule, rule, clr(EXPLAIN_RESET));

  char ln[1024];
  char cur_fn[256] = "";
  int total = 0, funcs = 0, saved_total = 0;
  int n_validated = 0, n_proven = 0, n_rejected = 0;
  while (fgets(ln, sizeof ln, f)) {
    char *fn = strtok(ln, "\t");
    char *gi = fn ? strtok(NULL, "\t") : NULL;
    char *kind = gi ? strtok(NULL, "\t") : NULL;
    char *before = kind ? strtok(NULL, "\t") : NULL;
    char *after = before ? strtok(NULL, "\t") : NULL;
    char *saved = after ? strtok(NULL, "\t") : NULL;
    char *line = saved ? strtok(NULL, "\t") : NULL;
    char *file = line ? strtok(NULL, "\t\n") : NULL;
    char *verdict = file ? strtok(NULL, "\t\n") : NULL;
    if (!after) {
      continue;
    }
    int rejected = verdict && strcmp(verdict, "rejected") == 0;
    int skipped = verdict && strcmp(verdict, "skipped") == 0;
    int proven = verdict && strcmp(verdict, "proven") == 0;
    if (skipped) {
      continue; /* declined applier or unverifiable speculative: never stood */
    }
    int sv = saved ? atoi(saved) : 0;
    long src_line = line ? atol(line) : 0;
    if (strcmp(fn, cur_fn) != 0) {
      snprintf(cur_fn, sizeof cur_fn, "%s", fn);
      funcs++;
      if (file && file[0]) {
        fprintf(stderr, "  %sfunction %s%s%s  %s%s%s\n", clr(EXPLAIN_DIM),
                clr(EXPLAIN_RESET), clr(EXPLAIN_BOLD), fn, clr(EXPLAIN_DIM),
                file, clr(EXPLAIN_RESET));
      } else {
        fprintf(stderr, "  %sfunction %s%s%s%s\n", clr(EXPLAIN_DIM),
                clr(EXPLAIN_RESET), clr(EXPLAIN_BOLD), fn, clr(EXPLAIN_RESET));
      }
    }
    char kbuf[28], bbuf[34], loc[24];
    ml_fit(kind, 22, kbuf, sizeof kbuf);
    ml_fit(before, 30, bbuf, sizeof bbuf);
    if (src_line > 0) snprintf(loc, sizeof loc, "line %ld", src_line);
    else snprintf(loc, sizeof loc, "ir#%s", gi ? gi : "?");
    const char *aft = (after[0] == '@') ? after + 1 : after;   /* drop sigil */
    fprintf(stderr, "    %s%s %-9s%s %s  %s  %s %s%s%s",
            clr(EXPLAIN_DIM), glyph_elbow(), loc, clr(EXPLAIN_RESET), kbuf, bbuf,
            glyph_arrow(), rejected ? clr(EXPLAIN_RED) : clr(EXPLAIN_GREEN), aft,
            clr(EXPLAIN_RESET));
    if (rejected) {
      fprintf(stderr, "  %sREJECTED: counterexample found%s", clr(EXPLAIN_RED),
              clr(EXPLAIN_RESET));
      n_rejected++;
    } else {
      if (sv > 0) {
        fprintf(stderr, "  %s-%d op%s%s", clr(EXPLAIN_DIM), sv,
                sv == 1 ? "" : "s", clr(EXPLAIN_RESET));
      }
      if (proven) {
        fprintf(stderr, "  %s(proven)%s", clr(EXPLAIN_DIM), clr(EXPLAIN_RESET));
        n_proven++;
      } else {
        n_validated++;
      }
      total++;
      saved_total += sv;
    }
    fprintf(stderr, "\n");
  }
  fclose(f);
  if (total || n_rejected) {
    const char *dot = ir_explain_use_unicode() ? "\xC2\xB7" : "*";
    fprintf(stderr,
            "\n  %s%d rewrite%s in %d function%s %s %d IR op%s removed %s ",
            clr(EXPLAIN_DIM), total, total == 1 ? "" : "s", funcs,
            funcs == 1 ? "" : "s", dot, saved_total,
            saved_total == 1 ? "" : "s", dot);
    if (n_proven == 0) {
      fprintf(stderr, "all validated equivalent by the interpreter");
    } else {
      fprintf(stderr, "%d validated equivalent, %d proven by construction",
              n_validated, n_proven);
    }
    if (n_rejected > 0) {
      fprintf(stderr, "%s %s%d proposal%s rejected with a counterexample",
              clr(EXPLAIN_RESET), clr(EXPLAIN_RED), n_rejected,
              n_rejected == 1 ? "" : "s");
    }
    fprintf(stderr, "%s\n", clr(EXPLAIN_RESET));
  }
}
