#include "ir_optimize_internal.h"
#include "../ir_verify.h"

#include <time.h>

const char *g_ir_pass_names[IR_OPT_PASS_COUNT] = {
#define IR_OPT_PASS_NAME(id, name) [IR_OPT_PASS_##id] = name,
    IR_OPT_PASS_LIST(IR_OPT_PASS_NAME)
#undef IR_OPT_PASS_NAME
};

const char *ir_opt_pass_name(IROptPassId pass_id) {
  if (pass_id < 0 || pass_id >= IR_OPT_PASS_COUNT ||
      !g_ir_pass_names[pass_id]) {
    return "<unnamed_ir_pass>";
  }
  return g_ir_pass_names[pass_id];
}

/* METTLE_TIME_IR_PASSES=1: accumulate wall time per pass across the whole
 * compile and dump a sorted table at the end of optimization. The cheap way
 * to answer "which pass is eating the build" without a sampling profiler. */
static int ir_pass_time_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_TIME_IR_PASSES");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static double g_ir_pass_ms[IR_OPT_PASS_COUNT];
static unsigned long long g_ir_pass_runs[IR_OPT_PASS_COUNT];
/* Named-sequence passes (vectorizers etc.) keyed by name, small fixed table. */
#define IR_PASS_TIME_NAMED_MAX 96
static struct {
  const char *name;
  double ms;
  unsigned long long runs;
} g_ir_named_ms[IR_PASS_TIME_NAMED_MAX];
static size_t g_ir_named_count = 0;

static double ir_pass_now_ms(void) {
  return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void ir_pass_time_add_named(const char *name, double ms) {
  for (size_t i = 0; i < g_ir_named_count; i++) {
    if (g_ir_named_ms[i].name == name ||
        strcmp(g_ir_named_ms[i].name, name) == 0) {
      g_ir_named_ms[i].ms += ms;
      g_ir_named_ms[i].runs++;
      return;
    }
  }
  if (g_ir_named_count < IR_PASS_TIME_NAMED_MAX) {
    g_ir_named_ms[g_ir_named_count].name = name;
    g_ir_named_ms[g_ir_named_count].ms = ms;
    g_ir_named_ms[g_ir_named_count].runs = 1;
    g_ir_named_count++;
  }
}

/* Timing hooks for program-level passes (the inliner, pure-call LICM) that
 * don't go through the per-function drivers. begin returns 0 when disabled. */
double ir_pass_time_begin(void) {
  return ir_pass_time_enabled() ? ir_pass_now_ms() : 0.0;
}

void ir_pass_time_end(const char *name, double begin_ms) {
  if (!ir_pass_time_enabled() || begin_ms == 0.0) {
    return;
  }
  ir_pass_time_add_named(name, ir_pass_now_ms() - begin_ms);
}

void ir_pass_time_report(void) {
  if (!ir_pass_time_enabled()) {
    return;
  }
  fprintf(stderr, "-- IR pass times (cumulative) --\n");
  for (int dumped = 0; dumped < 40; dumped++) {
    double best = 0.5; /* drop sub-half-millisecond noise */
    int best_fix = -1;
    size_t best_named = (size_t)-1;
    for (int i = 0; i < IR_OPT_PASS_COUNT; i++) {
      if (g_ir_pass_ms[i] > best) {
        best = g_ir_pass_ms[i];
        best_fix = i;
        best_named = (size_t)-1;
      }
    }
    for (size_t i = 0; i < g_ir_named_count; i++) {
      if (g_ir_named_ms[i].ms > best) {
        best = g_ir_named_ms[i].ms;
        best_named = i;
        best_fix = -1;
      }
    }
    if (best_fix >= 0) {
      fprintf(stderr, "  %-32s %10.1f ms  (%llu runs)\n",
              ir_opt_pass_name((IROptPassId)best_fix), g_ir_pass_ms[best_fix],
              g_ir_pass_runs[best_fix]);
      g_ir_pass_ms[best_fix] = 0.0;
    } else if (best_named != (size_t)-1) {
      fprintf(stderr, "  %-32s %10.1f ms  (%llu runs)\n",
              g_ir_named_ms[best_named].name, g_ir_named_ms[best_named].ms,
              g_ir_named_ms[best_named].runs);
      g_ir_named_ms[best_named].ms = 0.0;
    } else {
      break;
    }
  }
}

static int ir_skip_delimiter(char c) {
  return c == ',' || c == ' ' || c == '\t';
}

static int ir_skip_token_equals(const char *token, size_t token_len,
                                const char *value) {
  return value && strlen(value) == token_len &&
         strncmp(token, value, token_len) == 0;
}

static int ir_pass_trace_enabled(void) {
  /* Cached: this is consulted per pass EVENT (hundreds of thousands of times
   * on big programs) and getenv is not cheap on Windows. */
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_TRACE_IR_PASSES");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static void ir_trace_pass_event(const char *pass_name, const char *event,
                                const unsigned long long *version,
                                int changed) {
  if (!ir_pass_trace_enabled()) {
    return;
  }

  MettleCompilerContext *ctx = mettle_compiler_ctx();
  fprintf(stderr, "[ir-opt] function=%s",
          ctx->function_name ? ctx->function_name : "<anonymous>");
  if (ctx->fixpoint_iteration > 0) {
    fprintf(stderr, " iteration=%d", ctx->fixpoint_iteration);
  }
  if (version) {
    fprintf(stderr, " version=%llu", *version);
  }
  fprintf(stderr, " pass=%s event=%s", pass_name, event);
  if (changed >= 0) {
    fprintf(stderr, " changed=%d", changed);
  }
  fputc('\n', stderr);
  fflush(stderr); /* the trace exists to locate hangs; keep it ordered */
}

/* Diagnostic: METTLE_SKIP_PASS="sroa,16" disables the listed pass names or
 * numeric pass IDs so a miscompile can be bisected to a single pass. Names
 * cover both fixpoint passes and named-sequence passes (the pre-inline and
 * post-fixpoint stages: vectorizers, SLP, induction-pointer, ...). */
static int ir_skip_spec_matches(const char *id_text, const char *pass_name) {
  /* Snapshot once: consulted per pass run, and getenv per call was real
   * compile time on big programs. The env cannot change mid-process for a
   * diagnostic knob. */
  static const char *spec = NULL;
  static int fetched = 0;
  if (!fetched) {
    spec = getenv("METTLE_SKIP_PASS");
    fetched = 1;
  }
  if (!spec || !*spec) {
    return 0;
  }

  const char *p = spec;
  while (*p) {
    while (ir_skip_delimiter(*p)) {
      p++;
    }
    const char *token = p;
    while (*p && !ir_skip_delimiter(*p)) {
      p++;
    }
    size_t token_len = (size_t)(p - token);
    if (token_len == 0) {
      continue;
    }
    if (ir_skip_token_equals(token, token_len, id_text) ||
        ir_skip_token_equals(token, token_len, pass_name)) {
      return 1;
    }
  }
  return 0;
}

int ir_pass_name_is_skipped(const char *pass_name) {
  return ir_skip_spec_matches(NULL, pass_name);
}

int ir_pass_is_skipped(IROptPassId pass_id) {
  if (pass_id < 0 || pass_id >= IR_OPT_PASS_COUNT) {
    return 0;
  }

  char id_text[16];
  int id_len = snprintf(id_text, sizeof(id_text), "%d", (int)pass_id);
  if (id_len <= 0) {
    return 0;
  }

  return ir_skip_spec_matches(id_text, ir_opt_pass_name(pass_id));
}

/* METTLE_NO_SIMD: build a baseline (SSE2-only) binary by skipping every
 * vectorizer / SLP / SIMD named pass. Those are the only passes that emit
 * AVX/AVX2/FMA instructions, so a binary built with this set runs on any
 * x86-64 CPU (SSE2 is mandatory in the x86-64 baseline). Scalar float codegen
 * already uses legacy SSE2 encodings, so nothing else needs AVX. Use for
 * distributable builds that must run on older machines. */
static int ir_no_simd_enabled(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("METTLE_NO_SIMD");
    v = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
  }
  return v;
}

static int ir_run_named_pass(IRFunction *function, const IROptNamedPass *pass,
                             const char *failure_message) {
  int changed = 0;

  if (!pass || !pass->name || !pass->run) {
    return 0;
  }

  if (ir_no_simd_enabled()) {
    return 1;                 /* baseline build: no vectorization, no AVX */
  }

  if (ir_pass_name_is_skipped(pass->name)) {
    ir_trace_pass_event(pass->name, "skipped", NULL, -1);
    return 1;
  }

  if (ir_verify_pass_quarantined(function, pass->name)) {
    ir_trace_pass_event(pass->name, "quarantined", NULL, -1);
    return 1;
  }

  IRVerifySnapshot *verify_snapshot = ir_verify_snapshot_take(function);

  mettle_compiler_ctx_set_pass_name(pass->name);
  double t0 = ir_pass_time_begin();
  if (!pass->run(function, &changed)) {
    ir_trace_pass_event(pass->name, "failed", NULL, -1);
    mettle_compiler_ice(failure_message);
  }
  ir_pass_time_end(pass->name, t0);

  if (verify_snapshot) {
    ir_verify_maybe_sabotage(function, pass->name, &changed);
    ir_verify_check_pass(function, verify_snapshot, pass->name, &changed);
    ir_verify_snapshot_free(verify_snapshot);
  }

  ir_trace_pass_event(pass->name, changed ? "changed" : "clean", NULL,
                      changed);
  return 1;
}

int ir_run_named_pass_sequence(IRFunction *function,
                               const IROptNamedPass *passes,
                               size_t pass_count,
                               const char *failure_message) {
  for (size_t i = 0; i < pass_count; i++) {
    if (!ir_run_named_pass(function, &passes[i], failure_message)) {
      return 0;
    }
  }

  return 1;
}

/* Fixpoint pass driver with redundant-run skipping.
 *
 * The IR has a monotonically increasing version that bumps whenever any pass
 * changes it. Each pass records the version at which it last reported no
 * change. If that version is still current, the instruction array is identical
 * to what the pass already inspected, so the pass cannot change anything.
 */
int ir_run_fixpoint_pass(IRFunction *function, IROptPassId pass_id,
                         IROptFunctionPass pass, int enabled,
                         unsigned long long *version,
                         unsigned long long *clean_version, int *changed) {
  if (!version || !clean_version || !changed || pass_id < 0 ||
      pass_id >= IR_OPT_PASS_COUNT) {
    return 0;
  }

  const char *pass_name = ir_opt_pass_name(pass_id);
  if (!enabled) {
    ir_trace_pass_event(pass_name, "disabled", version, -1);
    clean_version[pass_id] = *version;
    return 1;
  }

  if (ir_pass_is_skipped(pass_id)) {
    ir_trace_pass_event(pass_name, "skipped", version, -1);
    clean_version[pass_id] = *version;
    return 1;
  }

  if (clean_version[pass_id] == *version) {
    ir_trace_pass_event(pass_name, "already_clean", version, -1);
    return 1;
  }

  if (ir_verify_pass_quarantined(function, pass_name)) {
    ir_trace_pass_event(pass_name, "quarantined", version, -1);
    clean_version[pass_id] = *version;
    return 1;
  }

  IRVerifySnapshot *verify_snapshot = ir_verify_snapshot_take(function);

  int pass_changed = 0;
  mettle_compiler_ctx_set_pass_name(pass_name);
  double t0 = ir_pass_time_begin();
  if (!pass || !pass(function, &pass_changed)) {
    ir_verify_snapshot_free(verify_snapshot);
    ir_trace_pass_event(pass_name, "failed", version, -1);
    return 0;
  }
  if (ir_pass_time_enabled()) {
    g_ir_pass_ms[pass_id] += ir_pass_now_ms() - t0;
    g_ir_pass_runs[pass_id]++;
  }

  if (verify_snapshot) {
    ir_verify_maybe_sabotage(function, pass_name, &pass_changed);
    if (!ir_verify_check_pass(function, verify_snapshot, pass_name,
                              &pass_changed)) {
      /* Divergence: IR restored; the pass is quarantined for this function,
       * so mark it clean at this version rather than re-running it. */
      clean_version[pass_id] = *version;
    }
    ir_verify_snapshot_free(verify_snapshot);
  }

  if (pass_changed) {
    *changed = 1;
    (*version)++;
  } else {
    clean_version[pass_id] = *version;
  }

  ir_trace_pass_event(pass_name, pass_changed ? "changed" : "clean", version,
                      pass_changed);
  return 1;
}
