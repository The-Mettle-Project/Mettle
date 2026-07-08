/* --verify: per-pass translation validation over the reference interpreter.
 * See ir_verify.h for the contract. */
#include "ir_verify.h"
#include "ir_interp.h"
#include "../common.h"
#include "optimizer/ir_optimize_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#define IRV_ISATTY _isatty
#define IRV_FILENO _fileno
#else
#include <unistd.h>
#define IRV_ISATTY isatty
#define IRV_FILENO fileno
#endif

#define IRV_INPUT_RUNS 6
#define IRV_FUEL_PER_RUN 400000LL
#define IRV_MAX_PARAMS 12
#define IRV_MAX_FN_INSNS 20000
#define IRV_MAX_QUARANTINE 256
#define IRV_MAX_SKIP_NOTES 12

/* ---------------- state ---------------- */

typedef struct {
  char *function_name;
  char *pass_name;
} IRVQuarantine;

typedef struct {
  char *function_name;
  char *reason;
} IRVSkipNote;

static int g_enabled = 0;
static IRProgram *g_program = NULL;
static int g_active = 0;

/* METTLE_VERIFY_STATS=1: accumulate where validation time goes and print a
 * breakdown with the summary. The cheap way to see whether snapshot copies,
 * before-runs, or after-runs dominate a slow --verify build. */
static int irv_stats_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_VERIFY_STATS");
    cached = (spec && spec[0] && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}
static double g_ms_snapshot, g_ms_before, g_ms_after, g_ms_setup;
static long long g_n_snapshot, g_n_before, g_n_after;
static long long g_n_snap_hits;
static double irv_now_ms(void) {
  return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void irv_take_cache_clear(void);

static long long g_apps_checked = 0;
static long long g_apps_validated = 0;
static long long g_apps_unverifiable = 0;
static long long g_apps_no_input = 0;
static long long g_divergences = 0;
static IRVQuarantine g_quarantine[IRV_MAX_QUARANTINE];
static size_t g_quarantine_count = 0;
static IRVSkipNote g_skip_notes[IRV_MAX_SKIP_NOTES];
static size_t g_skip_note_count = 0;
static int g_sabotage_fired = 0;

void ir_verify_set_enabled(int enabled) { g_enabled = enabled; }
int ir_verify_enabled(void) { return g_enabled; }
int ir_verify_divergence_count(void) { return (int)g_divergences; }

static int irv_color(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *no_color = getenv("NO_COLOR");
    cached = (!no_color || !no_color[0]) && IRV_ISATTY(IRV_FILENO(stderr));
  }
  return cached;
}

void ir_verify_begin_program(IRProgram *program) {
  if (!g_enabled) {
    return;
  }
  irv_take_cache_clear();
  g_program = program;
  g_active = 1;
  g_apps_checked = 0;
  g_apps_validated = 0;
  g_apps_unverifiable = 0;
  g_apps_no_input = 0;
  g_divergences = 0;
  g_sabotage_fired = 0;
  for (size_t i = 0; i < g_quarantine_count; i++) {
    free(g_quarantine[i].function_name);
    free(g_quarantine[i].pass_name);
  }
  g_quarantine_count = 0;
  for (size_t i = 0; i < g_skip_note_count; i++) {
    free(g_skip_notes[i].function_name);
    free(g_skip_notes[i].reason);
  }
  g_skip_note_count = 0;
}

static void irv_note_skip(const IRFunction *function, const char *reason) {
  if (!function || !function->name || !reason) {
    return;
  }
  for (size_t i = 0; i < g_skip_note_count; i++) {
    if (strcmp(g_skip_notes[i].function_name, function->name) == 0) {
      return; /* one note per function */
    }
  }
  if (g_skip_note_count >= IRV_MAX_SKIP_NOTES) {
    return;
  }
  g_skip_notes[g_skip_note_count].function_name = mettle_strdup(function->name);
  g_skip_notes[g_skip_note_count].reason = mettle_strdup(reason);
  if (g_skip_notes[g_skip_note_count].function_name &&
      g_skip_notes[g_skip_note_count].reason) {
    g_skip_note_count++;
  } else {
    free(g_skip_notes[g_skip_note_count].function_name);
    free(g_skip_notes[g_skip_note_count].reason);
  }
}

int ir_verify_pass_quarantined(const IRFunction *function,
                               const char *pass_name) {
  if (!g_active || !function || !function->name || !pass_name) {
    return 0;
  }
  for (size_t i = 0; i < g_quarantine_count; i++) {
    if (strcmp(g_quarantine[i].function_name, function->name) == 0 &&
        strcmp(g_quarantine[i].pass_name, pass_name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void irv_quarantine_add(const IRFunction *function,
                               const char *pass_name) {
  if (g_quarantine_count >= IRV_MAX_QUARANTINE || !function->name) {
    return;
  }
  g_quarantine[g_quarantine_count].function_name =
      mettle_strdup(function->name);
  g_quarantine[g_quarantine_count].pass_name = mettle_strdup(pass_name);
  if (g_quarantine[g_quarantine_count].function_name &&
      g_quarantine[g_quarantine_count].pass_name) {
    g_quarantine_count++;
  } else {
    free(g_quarantine[g_quarantine_count].function_name);
    free(g_quarantine[g_quarantine_count].pass_name);
  }
}

/* ---------------- snapshot / restore ---------------- */

struct IRVerifySnapshot {
  IRInstruction *instructions;
  size_t instruction_count;
  /* Owned by the take-cache below, not the caller: snapshot_free is a no-op
   * for these; the cache frees them on replacement or program end. */
  int cache_owned;
};

static int irv_instruction_deep_copy(IRInstruction *dst,
                                     const IRInstruction *src) {
  *dst = *src;
  dst->dest = ir_operand_copy(&src->dest);
  dst->lhs = ir_operand_copy(&src->lhs);
  dst->rhs = ir_operand_copy(&src->rhs);
  dst->text = src->text ? mettle_strdup(src->text) : NULL;
  dst->arguments = NULL;
  dst->argument_count = 0;
  if (src->text && !dst->text) {
    return 0;
  }
  if (src->argument_count > 0 && src->arguments) {
    dst->arguments =
        (IROperand *)calloc(src->argument_count, sizeof(IROperand));
    if (!dst->arguments) {
      return 0;
    }
    for (size_t i = 0; i < src->argument_count; i++) {
      dst->arguments[i] = ir_operand_copy(&src->arguments[i]);
    }
    dst->argument_count = src->argument_count;
  }
  return 1;
}

static void irv_instructions_free(IRInstruction *instructions, size_t count) {
  if (!instructions) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    ir_instruction_destroy_storage(&instructions[i]);
  }
  free(instructions);
}

static IRVerifySnapshot *irv_snapshot_capture_inner(IRFunction *function);

/* Take-cache: the pass driver snapshots a function before EVERY pass it
 * runs, but the vast majority of pass applications change nothing, so the
 * function's IR still matches the last snapshot exactly. Reusing it saves
 * the deep copy (thousands of small mallocs) and the matching frees. The
 * validity check is a full content comparison against the live IR - no
 * invalidation protocol to get wrong, and mutations that bypass the driver
 * (the inliner, ml-opt) are caught by the comparison itself. */
static struct {
  const IRFunction *fn;
  IRVerifySnapshot *snap;
} g_take_cache;

static int irv_operand_matches(const IROperand *a, const IROperand *b) {
  if (a->kind != b->kind || a->int_value != b->int_value ||
      a->float_bits != b->float_bits) {
    return 0;
  }
  if (a->kind == IR_OPERAND_FLOAT &&
      memcmp(&a->float_value, &b->float_value, sizeof(double)) != 0) {
    return 0;
  }
  if ((a->name == NULL) != (b->name == NULL)) {
    return 0;
  }
  return !a->name || strcmp(a->name, b->name) == 0;
}

static int irv_snapshot_matches(const IRFunction *function,
                                const IRVerifySnapshot *snapshot) {
  if (!snapshot || snapshot->instruction_count != function->instruction_count) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *a = &function->instructions[i];
    const IRInstruction *b = &snapshot->instructions[i];
    if (a->op != b->op || a->is_float != b->is_float ||
        a->float_bits != b->float_bits || a->is_unsigned != b->is_unsigned ||
        a->argument_count != b->argument_count) {
      return 0;
    }
    if ((a->text == NULL) != (b->text == NULL) ||
        (a->text && strcmp(a->text, b->text) != 0)) {
      return 0;
    }
    if (!irv_operand_matches(&a->dest, &b->dest) ||
        !irv_operand_matches(&a->lhs, &b->lhs) ||
        !irv_operand_matches(&a->rhs, &b->rhs)) {
      return 0;
    }
    for (size_t j = 0; j < a->argument_count; j++) {
      if (!irv_operand_matches(&a->arguments[j], &b->arguments[j])) {
        return 0;
      }
    }
  }
  return 1;
}

static void irv_take_cache_clear(void) {
  if (g_take_cache.snap) {
    g_take_cache.snap->cache_owned = 0;
    ir_verify_snapshot_free(g_take_cache.snap);
  }
  g_take_cache.fn = NULL;
  g_take_cache.snap = NULL;
}

IRVerifySnapshot *ir_verify_snapshot_take(IRFunction *function) {
  if (!g_active) {
    return NULL;
  }
  if (g_take_cache.fn == function) {
    double t0 = irv_stats_enabled() ? irv_now_ms() : 0.0;
    int hit = irv_snapshot_matches(function, g_take_cache.snap);
    if (irv_stats_enabled()) {
      g_ms_snapshot += irv_now_ms() - t0;
    }
    if (hit) {
      g_n_snap_hits++;
      return g_take_cache.snap;
    }
  }
  irv_take_cache_clear();
  IRVerifySnapshot *snap = ir_verify_snapshot_capture(function);
  if (snap) {
    snap->cache_owned = 1;
    g_take_cache.fn = function;
    g_take_cache.snap = snap;
  }
  return snap;
}

IRVerifySnapshot *ir_verify_snapshot_capture(IRFunction *function) {
  if (!function || function->instruction_count == 0 ||
      function->instruction_count > IRV_MAX_FN_INSNS) {
    return NULL;
  }
  double t0 = irv_stats_enabled() ? irv_now_ms() : 0.0;
  IRVerifySnapshot *result = irv_snapshot_capture_inner(function);
  if (irv_stats_enabled()) {
    g_ms_snapshot += irv_now_ms() - t0;
    g_n_snapshot++;
  }
  return result;
}

static IRVerifySnapshot *irv_snapshot_capture_inner(IRFunction *function) {
  IRVerifySnapshot *snapshot =
      (IRVerifySnapshot *)calloc(1, sizeof(*snapshot));
  if (!snapshot) {
    return NULL;
  }
  snapshot->instructions = (IRInstruction *)calloc(
      function->instruction_count, sizeof(IRInstruction));
  if (!snapshot->instructions) {
    free(snapshot);
    return NULL;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!irv_instruction_deep_copy(&snapshot->instructions[i],
                                   &function->instructions[i])) {
      irv_instructions_free(snapshot->instructions, i + 1);
      free(snapshot);
      return NULL;
    }
    snapshot->instruction_count = i + 1;
  }
  return snapshot;
}

void ir_verify_snapshot_free(IRVerifySnapshot *snapshot) {
  if (!snapshot || snapshot->cache_owned) {
    return; /* the take-cache owns it; freed on replacement / program end */
  }
  irv_instructions_free(snapshot->instructions, snapshot->instruction_count);
  free(snapshot);
}

/* Replace the function's instruction stream with a deep copy of the
 * snapshot's. */
static int irv_restore(IRFunction *function, const IRVerifySnapshot *snapshot) {
  IRInstruction *copy = (IRInstruction *)calloc(snapshot->instruction_count,
                                                sizeof(IRInstruction));
  if (!copy) {
    return 0;
  }
  for (size_t i = 0; i < snapshot->instruction_count; i++) {
    if (!irv_instruction_deep_copy(&copy[i], &snapshot->instructions[i])) {
      irv_instructions_free(copy, i + 1);
      return 0;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);
  function->instructions = copy;
  function->instruction_count = snapshot->instruction_count;
  function->instruction_capacity = snapshot->instruction_count;
  ir_function_clear_cfg(function);
  return 1;
}

int ir_verify_snapshot_restore(IRFunction *function,
                               const IRVerifySnapshot *snapshot) {
  if (!function || !snapshot) {
    return 0;
  }
  return irv_restore(function, snapshot);
}

/* ---------------- input generation ---------------- */

typedef enum {
  IRV_PARAM_INT,
  IRV_PARAM_FLOAT,
  IRV_PARAM_BUFFER,
  IRV_PARAM_CSTRING,
  IRV_PARAM_UNSUPPORTED
} IRVParamKind;

typedef struct {
  IRVParamKind kind;
  int elem_size;   /* buffers: pointee element size */
  int elem_float;  /* buffers: pointee is float */
} IRVParamInfo;

static IRVParamInfo irv_classify_param(const char *type) {
  IRVParamInfo info;
  info.kind = IRV_PARAM_UNSUPPORTED;
  info.elem_size = 1;
  info.elem_float = 0;
  if (!type) {
    return info;
  }
  size_t len = strlen(type);
  if (len > 1 && type[len - 1] == '*') {
    char base[24];
    if (len - 1 >= sizeof(base)) {
      return info;
    }
    memcpy(base, type, len - 1);
    base[len - 1] = '\0';
    static const struct { const char *name; int size; int is_float; } ELEMS[] = {
        {"int8", 1, 0},  {"int16", 2, 0},  {"int32", 4, 0},  {"int64", 8, 0},
        {"uint8", 1, 0}, {"uint16", 2, 0}, {"uint32", 4, 0}, {"uint64", 8, 0},
        {"bool", 1, 0},  {"float32", 4, 1}, {"float64", 8, 1},
    };
    for (size_t i = 0; i < sizeof(ELEMS) / sizeof(ELEMS[0]); i++) {
      if (strcmp(base, ELEMS[i].name) == 0) {
        info.kind = IRV_PARAM_BUFFER;
        info.elem_size = ELEMS[i].size;
        info.elem_float = ELEMS[i].is_float;
        return info;
      }
    }
    return info; /* pointer to struct/unknown: unsupported */
  }
  if (strcmp(type, "float32") == 0 || strcmp(type, "float64") == 0) {
    info.kind = IRV_PARAM_FLOAT;
    return info;
  }
  if (strcmp(type, "cstring") == 0) {
    info.kind = IRV_PARAM_CSTRING;
    info.elem_size = 1;
    return info;
  }
  static const char *INTS[] = {"int8",  "int16",  "int32",  "int64", "uint8",
                               "uint16", "uint32", "uint64", "bool"};
  for (size_t i = 0; i < sizeof(INTS) / sizeof(INTS[0]); i++) {
    if (strcmp(type, INTS[i]) == 0) {
      info.kind = IRV_PARAM_INT;
      return info;
    }
  }
  return info;
}

static unsigned int irv_lcg_next(unsigned int *state) {
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

/* Per-run buffer element counts and integer argument tables. Small ints keep
 * length-like parameters within buffer bounds; run 2 probes negatives; runs
 * 3-5 probe index-pair relationships ((0, N-1) spans, mid-range pairs, large
 * magnitudes) so loops whose trip count depends on how two arguments relate
 * (sift_down(start, end)-shaped code) actually execute their bodies. Run 3
 * caught a real escape the first three runs validated. */
static const long long IRV_BUFFER_ELEMS[IRV_INPUT_RUNS] = {33, 7, 16,
                                                           33, 32, 24};
static long long irv_int_arg(int run, size_t param_index) {
  switch (run) {
  case 0: return 5 + (long long)param_index * 3;
  case 1: return (long long)param_index;
  case 2: return param_index == 0 ? 7 : -2 + (long long)param_index;
  case 3: return param_index <= 1 ? 0 : 33 - (long long)param_index;
  case 4: return 2 + (long long)param_index * 7;
  default: return param_index == 0 ? 1 : 1000 + (long long)param_index * 37;
  }
}
static double irv_float_arg(int run, size_t param_index) {
  switch (run) {
  case 0: return 1.5 + (double)param_index;
  case 1: return 0.0 - (double)param_index * 0.5;
  case 2: return -2.25 + (double)param_index * 1.75;
  case 3: return (double)param_index * 0.125;
  case 4: return 100.5 - (double)param_index * 33.25;
  default: return 0.0001 + (double)param_index * 1e6;
  }
}

/* Build one machine for the run, registering identical buffers and argument
 * values. Returns 0 on setup failure. */
static int irv_setup_machine(IRInterpMachine *machine, IRFunction *shape,
                             const IRVParamInfo *params, size_t param_count,
                             int run, IRInterpValue *args) {
  (void)shape;
  for (size_t p = 0; p < param_count; p++) {
    switch (params[p].kind) {
    case IRV_PARAM_INT:
      args[p].i = irv_int_arg(run, p);
      args[p].f = 0;
      args[p].is_float = 0;
      break;
    case IRV_PARAM_FLOAT:
      args[p].i = 0;
      args[p].f = irv_float_arg(run, p);
      args[p].is_float = 1;
      break;
    case IRV_PARAM_CSTRING: {
      /* Deterministic printable text with a hard NUL terminator. */
      long long len = 9 + (long long)run * 4 + (long long)p;
      unsigned char *text = (unsigned char *)malloc((size_t)len + 1);
      if (!text) {
        return 0;
      }
      unsigned int seed = 0xC57131u ^ (unsigned int)(p * 977u + (size_t)run);
      for (long long c = 0; c < len; c++) {
        text[c] = (unsigned char)('a' + irv_lcg_next(&seed) % 26);
      }
      text[len] = '\0';
      unsigned long long addr = ir_interp_add_buffer(machine, text, len + 1);
      free(text);
      if (!addr) {
        return 0;
      }
      args[p].i = (long long)addr;
      args[p].f = 0;
      args[p].is_float = 0;
      break;
    }
    case IRV_PARAM_BUFFER: {
      long long elems = IRV_BUFFER_ELEMS[run];
      long long bytes = elems * params[p].elem_size;
      unsigned char *init = (unsigned char *)malloc((size_t)bytes);
      if (!init) {
        return 0;
      }
      unsigned int seed = 0x9E3779B9u ^ (unsigned int)(p * 2654435761u) ^
                          (unsigned int)(run * 40503u);
      if (params[p].elem_float) {
        for (long long e = 0; e < elems; e++) {
          double v = (double)(int)(irv_lcg_next(&seed) % 61) - 30.0 +
                     (double)(irv_lcg_next(&seed) % 4) * 0.25;
          if (params[p].elem_size == 4) {
            float f = (float)v;
            memcpy(init + e * 4, &f, 4);
          } else {
            memcpy(init + e * 8, &v, 8);
          }
        }
      } else {
        for (long long b = 0; b < bytes; b++) {
          init[b] = (unsigned char)(irv_lcg_next(&seed) >> 13);
        }
      }
      unsigned long long addr = ir_interp_add_buffer(machine, init, bytes);
      free(init);
      if (!addr) {
        return 0;
      }
      args[p].i = (long long)addr;
      args[p].f = 0;
      args[p].is_float = 0;
      break;
    }
    default:
      return 0;
    }
  }
  return 1;
}

/* ---------------- observation comparison ---------------- */

static int irv_float_close(double a, double b) {
  if (a == b) {
    return 1;
  }
  if (isnan(a) && isnan(b)) {
    return 1;
  }
  double mag = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
  return fabs(a - b) <= 1e-9 + 1e-6 * mag;
}

static int irv_value_equal(const IRInterpValue *a, const IRInterpValue *b) {
  if (a->is_float || b->is_float) {
    double x = a->is_float ? a->f : (double)a->i;
    double y = b->is_float ? b->f : (double)b->i;
    return irv_float_close(x, y);
  }
  return a->i == b->i;
}

static void irv_format_value(const IRInterpValue *value, char *buffer,
                             size_t capacity) {
  if (value->is_float) {
    snprintf(buffer, capacity, "%g", value->f);
  } else {
    snprintf(buffer, capacity, "%lld", value->i);
  }
}

/* Compare all observations of two completed runs. On mismatch, writes a
 * one-line description into `why` and returns 0. */
static int irv_compare_observations(IRInterpMachine *before,
                                    IRInterpMachine *after,
                                    const IRInterpValue *ret_before,
                                    const IRInterpValue *ret_after,
                                    size_t input_buffer_count, char *why,
                                    size_t why_capacity) {
  if (!irv_value_equal(ret_before, ret_after)) {
    char a[48], b[48];
    irv_format_value(ret_before, a, sizeof(a));
    irv_format_value(ret_after, b, sizeof(b));
    snprintf(why, why_capacity, "return value was %s, is now %s", a, b);
    return 0;
  }

  /* Input buffers exist in both machines by construction. */
  for (size_t i = 0; i < input_buffer_count; i++) {
    long long size_a = 0, size_b = 0;
    const unsigned char *a = ir_interp_buffer_data(before, i, &size_a);
    const unsigned char *b = ir_interp_buffer_data(after, i, &size_b);
    if (!a || !b || size_a != size_b) {
      snprintf(why, why_capacity, "input buffer %zu shape changed", i);
      return 0;
    }
    if (memcmp(a, b, (size_t)size_a) != 0) {
      long long at = 0;
      while (at < size_a && a[at] == b[at]) {
        at++;
      }
      snprintf(why, why_capacity,
               "buffer arg %zu differs at byte %lld (0x%02X -> 0x%02X)", i, at,
               a[at], b[at]);
      return 0;
    }
  }

  /* Runtime allocations: only comparable when allocation sequences match. */
  size_t total_a = ir_interp_buffer_count(before);
  size_t total_b = ir_interp_buffer_count(after);
  if (total_a == total_b) {
    for (size_t i = input_buffer_count; i < total_a; i++) {
      long long size_a = 0, size_b = 0;
      const unsigned char *a = ir_interp_buffer_data(before, i, &size_a);
      const unsigned char *b = ir_interp_buffer_data(after, i, &size_b);
      if (a && b && size_a == size_b &&
          memcmp(a, b, (size_t)size_a) != 0) {
        long long at = 0;
        while (at < size_a && a[at] == b[at]) {
          at++;
        }
        snprintf(why, why_capacity,
                 "heap allocation %zu differs at byte %lld (0x%02X -> 0x%02X)",
                 i, at, a[at], b[at]);
        return 0;
      }
    }
  }

  /* Extern-call trace: deletion, duplication, or reordering is a divergence. */
  size_t trace_a = ir_interp_extern_trace_count(before);
  size_t trace_b = ir_interp_extern_trace_count(after);
  if (trace_a != trace_b) {
    /* Name the first call present in one trace but not the other. */
    size_t common = trace_a < trace_b ? trace_a : trace_b;
    size_t at = 0;
    while (at < common &&
           strcmp(ir_interp_extern_trace(before, at)->name,
                  ir_interp_extern_trace(after, at)->name) == 0) {
      at++;
    }
    const IRInterpExternCall *odd =
        trace_a > trace_b ? ir_interp_extern_trace(before, at)
                          : ir_interp_extern_trace(after, at);
    snprintf(why, why_capacity,
             "extern call count was %zu, is now %zu (%s call #%zu: '%s')",
             trace_a, trace_b, trace_a > trace_b ? "removed" : "added", at,
             odd ? odd->name : "?");
    return 0;
  }
  for (size_t i = 0; i < trace_a; i++) {
    const IRInterpExternCall *a = ir_interp_extern_trace(before, i);
    const IRInterpExternCall *b = ir_interp_extern_trace(after, i);
    if (strcmp(a->name, b->name) != 0) {
      snprintf(why, why_capacity, "extern call %zu was %s, is now %s", i,
               a->name, b->name);
      return 0;
    }
    if (a->arg_count != b->arg_count) {
      snprintf(why, why_capacity, "extern call %zu (%s) arity changed", i,
               a->name);
      return 0;
    }
    for (size_t j = 0; j < a->arg_count; j++) {
      if (!irv_value_equal(&a->args[j], &b->args[j])) {
        snprintf(why, why_capacity, "extern call %zu (%s) argument %zu differs",
                 i, a->name, j);
        return 0;
      }
      /* Pointer arguments: the extern reads memory, so the bytes the pointer
       * addressed at call time are part of the observation. */
      if (a->arg_mem_len[j] != b->arg_mem_len[j] ||
          (a->arg_mem_len[j] > 0 &&
           memcmp(a->arg_mem[j], b->arg_mem[j], a->arg_mem_len[j]) != 0)) {
        snprintf(why, why_capacity,
                 "extern call %zu (%s) argument %zu points to differing bytes",
                 i, a->name, j);
        return 0;
      }
    }
  }

  /* Globals: union of names; a missing entry reads as untouched zero. */
  size_t global_capacity = ir_interp_global_count(before);
  for (size_t i = 0; i < global_capacity; i++) {
    const char *name = ir_interp_global_name(before, i);
    if (!name) {
      continue;
    }
    IRInterpValue va = ir_interp_global_value(before, i);
    IRInterpValue vb = {0, 0, 0};
    size_t cap_b = ir_interp_global_count(after);
    for (size_t j = 0; j < cap_b; j++) {
      const char *nb = ir_interp_global_name(after, j);
      if (nb && strcmp(nb, name) == 0) {
        vb = ir_interp_global_value(after, j);
        break;
      }
    }
    if (!irv_value_equal(&va, &vb)) {
      char a[48], b[48];
      irv_format_value(&va, a, sizeof(a));
      irv_format_value(&vb, b, sizeof(b));
      snprintf(why, why_capacity, "global '%s' was %s, is now %s", name, a, b);
      return 0;
    }
  }
  return 1;
}

/* ---------------- sabotage self-test ---------------- */

void ir_verify_maybe_sabotage(IRFunction *function, const char *pass_name,
                              int *changed) {
  if (!g_active || g_sabotage_fired || !function || !pass_name) {
    return;
  }
  const char *spec = getenv("METTLE_VERIFY_BREAK");
  if (!spec || !spec[0]) {
    return;
  }
  char pass_part[96];
  const char *colon = strchr(spec, ':');
  if (colon) {
    size_t n = (size_t)(colon - spec);
    if (n >= sizeof(pass_part)) {
      return;
    }
    memcpy(pass_part, spec, n);
    pass_part[n] = '\0';
    if (!function->name || strcmp(colon + 1, function->name) != 0) {
      return;
    }
  } else {
    snprintf(pass_part, sizeof(pass_part), "%s", spec);
  }
  if (strcmp(pass_part, pass_name) != 0) {
    return;
  }
  /* Corrupt the first additive BINARY constant we can find: `x = a + 7`
   * becomes `x = a + 8` - exactly the shape of a real constant-folding bug. */
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_BINARY && insn->text && !insn->is_float &&
        (strcmp(insn->text, "+") == 0 || strcmp(insn->text, "-") == 0 ||
         strcmp(insn->text, "*") == 0) &&
        insn->rhs.kind == IR_OPERAND_INT) {
      insn->rhs.int_value += 1;
      g_sabotage_fired = 1;
      if (changed) {
        *changed = 1;
      }
      return;
    }
  }
}

/* ---------------- the check ---------------- */

typedef enum {
  IRV_RUN_OK,
  IRV_RUN_GUARD_TRAP,  /* clean runtime-guard abort (mettle_crash_trap*) */
  IRV_RUN_SKIP,        /* trap/fuel on this input: try another */
  IRV_RUN_UNVERIFIABLE /* unsupported construct: give up on the function */
} IRVRunOutcome;

static IRVRunOutcome irv_run_one(IRInterpMachine *machine, IRFunction *fn,
                                 const IRInterpValue *args, size_t arg_count,
                                 IRInterpValue *ret, char *detail,
                                 size_t detail_capacity) {
  IRInterpStatus status =
      ir_interp_run(machine, fn, args, arg_count, ret, IRV_FUEL_PER_RUN);
  switch (status) {
  case IR_INTERP_OK:
    return IRV_RUN_OK;
  case IR_INTERP_GUARD_TRAP:
    snprintf(detail, detail_capacity, "%s", ir_interp_status_detail(machine));
    return IRV_RUN_GUARD_TRAP;
  case IR_INTERP_UNSUPPORTED:
    snprintf(detail, detail_capacity, "%s", ir_interp_status_detail(machine));
    return IRV_RUN_UNVERIFIABLE;
  default:
    snprintf(detail, detail_capacity, "%s", ir_interp_status_detail(machine));
    return IRV_RUN_SKIP;
  }
}

/* Format the diverging call as `fn(5, 8, <buf:33 elems>)`. */
static void irv_format_call(const IRFunction *function,
                            const IRVParamInfo *params,
                            const IRInterpValue *args, size_t arg_count,
                            int run, char *out, size_t capacity) {
  size_t off = (size_t)snprintf(out, capacity, "%s(",
                                function->name ? function->name : "?");
  for (size_t i = 0; i < arg_count && off + 1 < capacity; i++) {
    char value[48];
    if (params[i].kind == IRV_PARAM_BUFFER) {
      snprintf(value, sizeof(value), "<buf:%lld elems>",
               IRV_BUFFER_ELEMS[run]);
    } else if (params[i].kind == IRV_PARAM_CSTRING) {
      snprintf(value, sizeof(value), "<cstring>");
    } else {
      irv_format_value(&args[i], value, sizeof(value));
    }
    off += (size_t)snprintf(out + off, capacity - off, "%s%s",
                            i == 0 ? "" : ", ", value);
    if (off >= capacity) {
      return;
    }
  }
  if (off + 1 < capacity) {
    snprintf(out + off, capacity - off, ")");
  }
}

typedef enum {
  IRV_CHECK_VALIDATED,
  IRV_CHECK_DIVERGED,
  IRV_CHECK_UNVERIFIABLE,
  IRV_CHECK_NO_INPUT
} IRVCheckOutcome;

typedef struct {
  char why[192];         /* DIVERGED: divergence description */
  char cex[288];         /* DIVERGED: formatted counterexample call */
  char skip_reason[160]; /* UNVERIFIABLE / NO_INPUT: what blocked the check */
  int run;               /* DIVERGED: diverging input set */
} IRVCheckResult;

/* The policy-free differential check: run `function` (after) against the
 * snapshot's instructions (before) on generated inputs and compare every
 * observation. No counters, no quarantine, no restore, no printing - both
 * the --verify pass driver and the --ml-opt rewrite gate wrap this. */
static IRVCheckOutcome irv_check_function(IRProgram *program,
                                          IRFunction *function,
                                          const IRVerifySnapshot *snapshot,
                                          IRVCheckResult *result) {
  result->why[0] = '\0';
  result->cex[0] = '\0';
  result->skip_reason[0] = '\0';
  result->run = -1;

  /* Classify parameters; bail early on unverifiable signatures. */
  size_t param_count = function->parameter_count;
  IRVParamInfo params[IRV_MAX_PARAMS];
  if (param_count > IRV_MAX_PARAMS) {
    snprintf(result->skip_reason, sizeof(result->skip_reason),
             "more than 12 parameters");
    return IRV_CHECK_UNVERIFIABLE;
  }
  for (size_t i = 0; i < param_count; i++) {
    params[i] = irv_classify_param(function->parameter_types
                                       ? function->parameter_types[i]
                                       : NULL);
    if (params[i].kind == IRV_PARAM_UNSUPPORTED) {
      snprintf(result->skip_reason, sizeof(result->skip_reason),
               "parameter type '%s'",
               function->parameter_types && function->parameter_types[i]
                   ? function->parameter_types[i]
                   : "?");
      return IRV_CHECK_UNVERIFIABLE;
    }
  }

  /* Rebuild a callable BEFORE function around the snapshot's instructions. */
  IRFunction before_fn;
  memset(&before_fn, 0, sizeof(before_fn));
  before_fn.name = function->name;
  before_fn.parameter_names = function->parameter_names;
  before_fn.parameter_types = function->parameter_types;
  before_fn.parameter_count = function->parameter_count;
  before_fn.instructions = snapshot->instructions;
  before_fn.instruction_count = snapshot->instruction_count;

  int usable_inputs = 0;
  int stats = irv_stats_enabled();
  for (int run = 0; run < IRV_INPUT_RUNS; run++) {
    double t0 = stats ? irv_now_ms() : 0.0;
    IRInterpMachine *machine_before = ir_interp_create(program);
    IRInterpMachine *machine_after = ir_interp_create(program);
    if (!machine_before || !machine_after) {
      ir_interp_destroy(machine_before);
      ir_interp_destroy(machine_after);
      break;
    }
    ir_interp_set_override(machine_before, function->name, &before_fn);

    IRInterpValue args_before[IRV_MAX_PARAMS];
    IRInterpValue args_after[IRV_MAX_PARAMS];
    if (!irv_setup_machine(machine_before, function, params, param_count, run,
                           args_before) ||
        !irv_setup_machine(machine_after, function, params, param_count, run,
                           args_after)) {
      ir_interp_destroy(machine_before);
      ir_interp_destroy(machine_after);
      continue;
    }
    size_t input_buffer_count = ir_interp_buffer_count(machine_before);

    IRInterpValue ret_before = {0, 0, 0}, ret_after = {0, 0, 0};
    char detail_before[128] = "", detail_after[128] = "";
    double t1 = stats ? irv_now_ms() : 0.0;
    IRVRunOutcome outcome_before =
        irv_run_one(machine_before, &before_fn, args_before, param_count,
                    &ret_before, detail_before, sizeof(detail_before));
    double t2 = stats ? irv_now_ms() : 0.0;
    IRVRunOutcome outcome_after =
        irv_run_one(machine_after, function, args_after, param_count,
                    &ret_after, detail_after, sizeof(detail_after));
    if (stats) {
      double t3 = irv_now_ms();
      g_ms_setup += t1 - t0;
      g_ms_before += t2 - t1;
      g_ms_after += t3 - t2;
      g_n_before++;
      g_n_after++;
    }

    if (outcome_before == IRV_RUN_UNVERIFIABLE ||
        outcome_after == IRV_RUN_UNVERIFIABLE) {
      snprintf(result->skip_reason, sizeof(result->skip_reason),
               "unsupported construct: %s",
               outcome_after == IRV_RUN_UNVERIFIABLE ? detail_after
                                                     : detail_before);
      ir_interp_destroy(machine_before);
      ir_interp_destroy(machine_after);
      return IRV_CHECK_UNVERIFIABLE;
    }

    /* A program that cleanly guard-traps on both sides is equivalent: the
     * exact crash point of a runtime check may shift under optimization,
     * like any debug-checks build. */
    if (outcome_before == IRV_RUN_GUARD_TRAP &&
        outcome_after == IRV_RUN_GUARD_TRAP) {
      usable_inputs++;
      ir_interp_destroy(machine_before);
      ir_interp_destroy(machine_after);
      continue;
    }
    /* Exactly one side guard-trapped while the other finished: the pass
     * made a working program crash (or a crashing program succeed). */
    if ((outcome_before == IRV_RUN_GUARD_TRAP && outcome_after == IRV_RUN_OK) ||
        (outcome_before == IRV_RUN_OK && outcome_after == IRV_RUN_GUARD_TRAP)) {
      snprintf(result->why, sizeof(result->why), "%s (%s)",
               outcome_after == IRV_RUN_GUARD_TRAP
                   ? "pass made a completing program hit a runtime guard trap"
                   : "pass removed a runtime guard trap the program hit",
               outcome_after == IRV_RUN_GUARD_TRAP ? detail_after
                                                   : detail_before);
      goto divergence;
    }

    if (outcome_before != IRV_RUN_OK || outcome_after != IRV_RUN_OK) {
      /* One side trapped or ran out of fuel. Same fate on both sides means
       * the input is unusable; different fates on a trap is itself a
       * divergence (a pass must not add or remove traps). Fuel asymmetry is
       * inconclusive (vector kernels charge differently), so skip those. */
      int trap_before = outcome_before == IRV_RUN_SKIP &&
                        strstr(detail_before, "fuel") == NULL;
      int trap_after = outcome_after == IRV_RUN_SKIP &&
                       strstr(detail_after, "fuel") == NULL;
      if (trap_before != trap_after &&
          (outcome_before == IRV_RUN_OK || outcome_after == IRV_RUN_OK)) {
        snprintf(result->why, sizeof(result->why), "%s: %s",
                 trap_after ? "pass introduced a trap"
                            : "pass removed a trap",
                 trap_after ? detail_after : detail_before);
        goto divergence;
      }
      ir_interp_destroy(machine_before);
      ir_interp_destroy(machine_after);
      continue;
    }

    if (!irv_compare_observations(machine_before, machine_after, &ret_before,
                                  &ret_after, input_buffer_count, result->why,
                                  sizeof(result->why))) {
      goto divergence;
    }

    usable_inputs++;
    ir_interp_destroy(machine_before);
    ir_interp_destroy(machine_after);
    continue;

  divergence:
    result->run = run;
    irv_format_call(function, params, args_before, param_count, run,
                    result->cex, sizeof(result->cex));
    ir_interp_destroy(machine_before);
    ir_interp_destroy(machine_after);
    return IRV_CHECK_DIVERGED;
  }

  if (usable_inputs == 0) {
    snprintf(result->skip_reason, sizeof(result->skip_reason),
             "no executable inputs (traps/fuel on all sets)");
    return IRV_CHECK_NO_INPUT;
  }
  return IRV_CHECK_VALIDATED;
}

static void irv_report_divergence(IRFunction *function, const char *pass_name,
                                  const IRVCheckResult *result) {
  const char *red = irv_color() ? "\x1b[31m\x1b[1m" : "";
  const char *cyan = irv_color() ? "\x1b[36m" : "";
  const char *reset = irv_color() ? "\x1b[0m" : "";

  fprintf(stderr,
          "\n%sverify: MISCOMPILE CAUGHT%s: pass '%s' changed the observable "
          "behavior of function '%s'\n",
          red, reset, pass_name, function->name ? function->name : "?");
  fprintf(stderr, "  %scounterexample%s (input set %d): %s\n", cyan, reset,
          result->run, result->cex);
  fprintf(stderr, "  %sdivergence%s: %s\n", cyan, reset, result->why);
  fprintf(stderr,
          "  %saction%s: pre-pass IR restored; '%s' quarantined for '%s'; "
          "compilation continues from validated IR\n",
          cyan, reset, pass_name, function->name ? function->name : "?");
}

int ir_verify_check_pass(IRFunction *function, IRVerifySnapshot *snapshot,
                         const char *pass_name, int *changed) {
  if (!g_active || !function || !snapshot || !pass_name) {
    return 1;
  }
  if (!changed || !*changed) {
    return 1; /* nothing to validate */
  }

  g_apps_checked++;

  IRVCheckResult result;
  switch (irv_check_function(g_program, function, snapshot, &result)) {
  case IRV_CHECK_UNVERIFIABLE:
    irv_note_skip(function, result.skip_reason);
    g_apps_unverifiable++;
    return 1;
  case IRV_CHECK_NO_INPUT:
    irv_note_skip(function, result.skip_reason);
    g_apps_no_input++;
    return 1;
  case IRV_CHECK_DIVERGED:
    irv_report_divergence(function, pass_name, &result);
    g_divergences++;
    irv_quarantine_add(function, pass_name);
    if (irv_restore(function, snapshot)) {
      *changed = 0;
    }
    return 0;
  case IRV_CHECK_VALIDATED:
  default:
    g_apps_validated++;
    return 1;
  }
}

IRVerifyRewriteVerdict ir_verify_check_rewrite(
    IRProgram *program, IRFunction *function, const IRVerifySnapshot *snapshot,
    char *why, size_t why_capacity, char *counterexample, size_t cex_capacity,
    char *skip_reason, size_t skip_capacity) {
  if (why && why_capacity) {
    why[0] = '\0';
  }
  if (counterexample && cex_capacity) {
    counterexample[0] = '\0';
  }
  if (skip_reason && skip_capacity) {
    skip_reason[0] = '\0';
  }
  if (!program || !function || !snapshot) {
    if (skip_reason && skip_capacity) {
      snprintf(skip_reason, skip_capacity, "nothing to check");
    }
    return IR_VERIFY_REWRITE_UNVERIFIABLE;
  }

  IRVCheckResult result;
  switch (irv_check_function(program, function, snapshot, &result)) {
  case IRV_CHECK_DIVERGED:
    if (why && why_capacity) {
      snprintf(why, why_capacity, "%s", result.why);
    }
    if (counterexample && cex_capacity) {
      snprintf(counterexample, cex_capacity, "(input set %d) %s", result.run,
               result.cex);
    }
    return IR_VERIFY_REWRITE_DIVERGED;
  case IRV_CHECK_UNVERIFIABLE:
  case IRV_CHECK_NO_INPUT:
    if (skip_reason && skip_capacity) {
      snprintf(skip_reason, skip_capacity, "%s", result.skip_reason);
    }
    return IR_VERIFY_REWRITE_UNVERIFIABLE;
  case IRV_CHECK_VALIDATED:
  default:
    return IR_VERIFY_REWRITE_VALIDATED;
  }
}

void ir_verify_end_program(void) {
  if (!g_active) {
    return;
  }
  const char *bold = irv_color() ? "\x1b[1m" : "";
  const char *green = irv_color() ? "\x1b[32m\x1b[1m" : "";
  const char *red = irv_color() ? "\x1b[31m\x1b[1m" : "";
  const char *dim = irv_color() ? "\x1b[36m" : "";
  const char *reset = irv_color() ? "\x1b[0m" : "";

  fprintf(stderr, "\n%stranslation validation%s: ", bold, reset);
  if (g_divergences == 0) {
    fprintf(stderr,
            "%sOK%s - %lld pass applications validated on %d input sets each",
            green, reset, g_apps_validated, IRV_INPUT_RUNS);
  } else {
    fprintf(stderr, "%s%lld MISCOMPILE%s CAUGHT & QUARANTINED%s (%lld validated)",
            red, g_divergences, g_divergences == 1 ? "" : "S", reset,
            g_apps_validated);
  }
  if (g_apps_unverifiable > 0 || g_apps_no_input > 0) {
    fprintf(stderr, "; %lld skipped", g_apps_unverifiable + g_apps_no_input);
  }
  fprintf(stderr, "\n");
  for (size_t i = 0; i < g_skip_note_count; i++) {
    fprintf(stderr, "  %snot validated%s: %s (%s)\n", dim, reset,
            g_skip_notes[i].function_name, g_skip_notes[i].reason);
  }
  if (irv_stats_enabled()) {
    fprintf(stderr,
            "  %sverify stats%s: snapshots %.0f ms (%lld copies, %lld cache "
            "hits), machine setup %.0f ms, before-runs %.0f ms (%lld), "
            "after-runs %.0f ms (%lld)\n",
            dim, reset, g_ms_snapshot, g_n_snapshot, g_n_snap_hits, g_ms_setup,
            g_ms_before, g_n_before, g_ms_after, g_n_after);
    g_ms_snapshot = g_ms_before = g_ms_after = g_ms_setup = 0;
    g_n_snapshot = g_n_before = g_n_after = g_n_snap_hits = 0;
  }
  irv_take_cache_clear();
  g_active = 0;
  g_program = NULL;
}
