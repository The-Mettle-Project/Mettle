#include "ir_optimize_internal.h"
#include "../ir_verify.h"
#include "../../common.h"

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

static int ir_pass_time_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_TIME_IR_PASSES");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static int ir_pass_time_covers(const IRFunction *function) {
  const char *only = getenv("METTLE_TIME_ONE_FUNCTION");

  if (!only || !only[0]) {
    return 1;
  }
  return function && function->name && strcmp(function->name, only) == 0;
}

static size_t g_cfg_repairs = 0;

static int ir_cfg_repair_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *setting = getenv("METTLE_CFG_REPAIR");
    cached = (setting && strcmp(setting, "0") == 0) ? 0 : 1;
  }
  return cached;
}

size_t ir_cfg_repair_count(void) { return g_cfg_repairs; }

static double g_ir_pass_ms[IR_OPT_PASS_COUNT];
static unsigned long long g_ir_pass_runs[IR_OPT_PASS_COUNT];
#define IR_PASS_TIME_NAMED_MAX 96
static struct {
  const char *name;
  double ms;
  unsigned long long runs;
} g_ir_named_ms[IR_PASS_TIME_NAMED_MAX];
static size_t g_ir_named_count = 0;

static double ir_pass_now_ticks(void) {
  return mettle_now_ms();
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

double ir_pass_time_begin(void) {
  return ir_pass_time_enabled() ? ir_pass_now_ticks() : 0.0;
}

void ir_pass_time_end(const char *name, double begin_ms) {
  if (!ir_pass_time_enabled() || begin_ms == 0.0) {
    return;
  }
  ir_pass_time_add_named(name, ir_pass_now_ticks() - begin_ms);
}

void ir_pass_time_report(void) {
  if (!ir_pass_time_enabled()) {
    return;
  }
  fprintf(stderr, "-- IR pass times (cumulative ms) --\n");
  for (int dumped = 0; dumped < 40; dumped++) {
    double best = 0.5;
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
      fprintf(stderr, "  %-32s %12.0f  (%llu runs)\n",
              ir_opt_pass_name((IROptPassId)best_fix), g_ir_pass_ms[best_fix],
              g_ir_pass_runs[best_fix]);
      g_ir_pass_ms[best_fix] = 0.0;
    } else if (best_named != (size_t)-1) {
      fprintf(stderr, "  %-32s %12.0f  (%llu runs)\n",
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
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_TRACE_IR_PASSES");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static const char *ir_dump_passes_function(void) {
  static int fetched = 0;
  static const char *spec = NULL;
  if (!fetched) {
    const char *raw = getenv("METTLE_DUMP_IR_PASSES");
    spec = (raw && raw[0] != '\0') ? raw : NULL;
    fetched = 1;
  }
  return spec;
}

static void ir_dump_pass_result(const IRFunction *function,
                                const char *pass_name) {
  const char *spec = ir_dump_passes_function();
  if (!spec || !function || !function->name) {
    return;
  }
  if (strcmp(spec, "*") != 0 && strcmp(spec, function->name) != 0) {
    return;
  }
  fprintf(stderr, "[ir-dump] function=%s after pass=%s\n", function->name,
          pass_name ? pass_name : "<unnamed>");
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    char buffer[1024];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    ir_instruction_dump(instruction, buffer, sizeof(buffer));
    fprintf(stderr, "  %4zu: %s\n", i, buffer);
  }
  fflush(stderr);
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
  fflush(stderr);
}

static int ir_skip_spec_matches(const char *id_text, const char *pass_name) {
  static const char *spec = NULL;
  static int fetched = 0;
  if (!fetched) {
    const char *raw = getenv("METTLE_SKIP_PASS");
    spec = raw ? mettle_strdup(raw) : NULL;
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

static int ir_no_simd_enabled(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("METTLE_NO_SIMD");
    v = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
  }
  return v;
}

static int ir_pass_is_vectorizer(const char *name) {
  return name && (strncmp(name, "simd_", 5) == 0 ||
                  strncmp(name, "auto_vectorize", 14) == 0 ||
                  strncmp(name, "outer_vectorize", 15) == 0);
}

static int ir_pass_rewrites_whole_loop(const char *name) {
  return ir_pass_is_vectorizer(name) ||
         (name && (strcmp(name, "prefix_sum_i32") == 0 ||
                   strcmp(name, "lower_bound_i32") == 0 ||
                   strcmp(name, "detect_shift_loops") == 0 ||
                   strcmp(name, "induction_pointer") == 0));
}

static int ir_no_slp_enabled(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("NO_SLP");
    v = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
  }
  return v;
}

static int ir_pass_is_slp(const char *name) {
  return name && strncmp(name, "simd_slp_", 9) == 0;
}

static unsigned long long ir_volatile_signature(const IRFunction *function) {
  unsigned long long signature = 1469598103934665603ULL;
  size_t i;
  if (!function) {
    return signature;
  }
  for (i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    unsigned long long token;
    if (!instruction->is_volatile) {
      continue;
    }
    token = (unsigned long long)instruction->op * 131ULL +
            (unsigned long long)instruction->alias_class * 17ULL +
            (instruction->rhs.kind == IR_OPERAND_INT
                 ? (unsigned long long)instruction->rhs.int_value
                 : 0ULL);
    signature = (signature ^ token) * 1099511628211ULL;
  }
  return signature;
}

static void ir_library_passes_after(IRFunction *function, const char *pass_name);

static int ir_run_named_pass(IRFunction *function, const IROptNamedPass *pass,
                             const char *failure_message, int *changed_out) {
  int changed = 0;
  int audit_volatile = function && function->has_volatile_access;
  unsigned long long volatile_before =
      audit_volatile ? ir_volatile_signature(function) : 0;

  if (changed_out) {
    *changed_out = 0;
  }

  if (!pass || !pass->name || !pass->run) {
    return 0;
  }

  if (ir_no_simd_enabled() && ir_pass_is_vectorizer(pass->name)) {
    ir_trace_pass_event(pass->name, "skipped", NULL, -1);
    return 1;
  }

  if (audit_volatile && ir_pass_rewrites_whole_loop(pass->name)) {
    ir_trace_pass_event(pass->name, "skipped", NULL, -1);
    return 1;
  }

  if (ir_no_slp_enabled() && ir_pass_is_slp(pass->name)) {
    ir_trace_pass_event(pass->name, "skipped", NULL, -1);
    return 1;
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

  ir_function_number_values(function);
  const size_t structure_before = ir_structure_snapshot(function);
  const uint64_t fingerprint_before =
      getenv("METTLE_CACHE_AUDIT") ? ir_function_fingerprint(function) : 0;
  const uint64_t generation_before = function->generation;
  const uint64_t cfg_fingerprint_before =
      getenv("METTLE_CFG_AUDIT") ? ir_function_structure_fingerprint(function)
                                 : 0;
  const uint64_t structure_generation_before = function->structure_generation;

  mettle_compiler_ctx_set_pass_name(pass->name);
  ir_explain_pass_begin(function);
  double t0 = ir_pass_time_begin();
  if (!pass->run(function, &changed)) {
    ir_trace_pass_event(pass->name, "failed", NULL, -1);
    mettle_compiler_ice(failure_message);
  }
  ir_pass_time_end(pass->name, t0);
  ir_explain_pass_end(function, pass->name, changed);

  if (changed) {
    ir_function_touch(function);
  }
  ir_check_silent_mutation(function, pass->name, fingerprint_before,
                           generation_before);
  ir_check_silent_structure(function, pass->name, cfg_fingerprint_before,
                            structure_generation_before);
  if (ir_cfg_repair_enabled() && changed &&
      function->structure_generation == structure_generation_before) {
    ir_function_clear_cfg(function);
    g_cfg_repairs++;
  }
  ir_value_maybe_sabotage(function, pass->name);
  ir_value_check_after_pass(function, pass->name);
  ir_function_number_values(function);
  ir_structure_maybe_sabotage(function, pass->name);
  ir_structure_check_after_pass(function, pass->name, structure_before);
  {
    int repaired = 0;
    ir_repair_phis(function, &repaired);
    ir_function_number_values(function);
  }
  ir_phi_check_after_pass(function, pass->name);
  ir_analysis_self_check(function);

  if (audit_volatile && ir_volatile_signature(function) != volatile_before) {
    char message[256];
    snprintf(message, sizeof(message),
             "optimization pass '%s' changed the volatile accesses in '%s'; a "
             "volatile load or store may not be removed, duplicated or "
             "reordered against another",
             pass->name, function->name ? function->name : "<unnamed>");
    mettle_compiler_ice(message);
  }

  if (verify_snapshot) {
    ir_verify_maybe_sabotage(function, pass->name, &changed);
    ir_verify_check_pass(function, verify_snapshot, pass->name, &changed);
    ir_verify_snapshot_free(verify_snapshot);
  }

  ir_trace_pass_event(pass->name, changed ? "changed" : "clean", NULL,
                      changed);
  if (changed) {
    ir_dump_pass_result(function, pass->name);
  }
  if (changed_out) {
    *changed_out = changed;
  }
  ir_library_passes_after(function, pass->name);
  return 1;
}

static const IRLibraryPass *g_library_passes = NULL;
static size_t g_library_pass_count = 0;
static const IRLibraryPass *g_library_current = NULL;
static int g_library_running = 0;

void ir_library_passes_set(const IRLibraryPass *passes, size_t count) {
  g_library_passes = passes;
  g_library_pass_count = count;
}

static int ir_library_trampoline(IRFunction *function, int *changed) {
  return g_library_current->run(function, g_library_current->context, changed);
}

void ir_library_passes_at(IRFunction *function, const char *point) {
  if (g_library_pass_count == 0 || g_library_running) {
    return;
  }
  for (size_t i = 0; i < g_library_pass_count; i++) {
    const IRLibraryPass *pass = &g_library_passes[i];
    if (strcmp(pass->point, point) != 0) {
      continue;
    }
    IROptNamedPass named = {pass->name, ir_library_trampoline, {0, 0}};
    g_library_running = 1;
    g_library_current = pass;
    ir_run_named_pass(function, &named, "library pass failed", NULL);
    g_library_running = 0;
  }
}

static void ir_library_passes_after(IRFunction *function, const char *pass_name) {
  if (g_library_pass_count == 0 || g_library_running) {
    return;
  }
  for (size_t i = 0; i < g_library_pass_count; i++) {
    const IRLibraryPass *pass = &g_library_passes[i];
    if (strncmp(pass->point, "after_", 6) != 0 ||
        strcmp(pass->point + 6, pass_name) != 0) {
      continue;
    }
    IROptNamedPass named = {pass->name, ir_library_trampoline, {0, 0}};
    g_library_running = 1;
    g_library_current = pass;
    ir_run_named_pass(function, &named, "library pass failed", NULL);
    g_library_running = 0;
  }
}

int ir_run_named_pass_sequence(IRFunction *function,
                               const IROptNamedPass *passes,
                               size_t pass_count,
                               const char *failure_message) {
  for (size_t i = 0; i < pass_count; i++) {
    if (!ir_run_named_pass(function, &passes[i], failure_message, NULL)) {
      return 0;
    }
  }

  return 1;
}

#define IR_NAMED_STAGE_MAX_PASSES 64

int ir_run_named_stage_fixpoint(IRFunction *function,
                                const IROptNamedPass *passes,
                                size_t pass_count, int max_iterations,
                                const char *stage_name,
                                const char *failure_message,
                                int require_convergence) {
  if (!function || !passes || pass_count == 0 || max_iterations <= 0 ||
      pass_count > IR_NAMED_STAGE_MAX_PASSES) {
    return 0;
  }

  size_t slot[IR_NAMED_STAGE_MAX_PASSES];
  unsigned long long clean_version[IR_NAMED_STAGE_MAX_PASSES];
  for (size_t i = 0; i < pass_count; i++) {
    slot[i] = i;
    clean_version[i] = 0;
    for (size_t j = 0; j < i; j++) {
      if (passes[j].run == passes[i].run) {
        slot[i] = slot[j];
        break;
      }
    }
  }

  unsigned long long version = 1;
  int converged = 0;

  for (int iteration = 0; iteration < max_iterations && !converged;
       iteration++) {
    int iteration_changed = 0;
    IROptFunctionFeatures features;

    mettle_compiler_ctx_set_fixpoint_iteration(iteration + 1);
    ir_collect_function_features(function, &features);
    unsigned feature_flags = ir_opt_feature_flags(&features);

    for (size_t i = 0; i < pass_count; i++) {
      const IROptNamedPass *pass = &passes[i];
      unsigned all = pass->gate.all;
      unsigned any = pass->gate.any;

      if ((feature_flags & all) != all ||
          (any != 0 && (feature_flags & any) == 0)) {
        ir_trace_pass_event(pass->name, "disabled", &version, -1);
        clean_version[slot[i]] = version;
        ir_library_passes_after(function, pass->name);
        continue;
      }
      if (clean_version[slot[i]] == version) {
        ir_trace_pass_event(pass->name, "already_clean", &version, -1);
        ir_library_passes_after(function, pass->name);
        continue;
      }

      int changed = 0;
      if (!ir_run_named_pass(function, pass, failure_message, &changed)) {
        return 0;
      }
      if (changed) {
        version++;
        iteration_changed = 1;
        if (feature_flags != IR_OPT_FEATURE_ALL) {
          ir_collect_function_features(function, &features);
          feature_flags = ir_opt_feature_flags(&features);
        }
      } else {
        clean_version[slot[i]] = version;
      }
    }

    if (!iteration_changed) {
      converged = 1;
    }
  }

  mettle_compiler_ctx_set_fixpoint_iteration(0);

  if (require_convergence && !converged) {
    fprintf(stderr,
            "mettle: internal error: stage '%s' did not converge on function "
            "'%s' after %d iterations\n",
            stage_name ? stage_name : "<unnamed>",
            function->name ? function->name : "<anonymous>", max_iterations);
    mettle_compiler_ice("IR normal-form stage failed to converge");
  }

  return 1;
}

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
    ir_library_passes_after(function, pass_name);
    return 1;
  }

  if (ir_pass_is_skipped(pass_id)) {
    ir_trace_pass_event(pass_name, "skipped", version, -1);
    clean_version[pass_id] = *version;
    ir_library_passes_after(function, pass_name);
    return 1;
  }

  if (clean_version[pass_id] == *version) {
    ir_trace_pass_event(pass_name, "already_clean", version, -1);
    ir_library_passes_after(function, pass_name);
    return 1;
  }

  if (ir_verify_pass_quarantined(function, pass_name)) {
    ir_trace_pass_event(pass_name, "quarantined", version, -1);
    clean_version[pass_id] = *version;
    return 1;
  }

  IRVerifySnapshot *verify_snapshot = ir_verify_snapshot_take(function);

  int pass_changed = 0;
  ir_facts_invalidate();
  mettle_compiler_ctx_set_pass_name(pass_name);
  ir_explain_pass_begin(function);
  double t0 = ir_pass_time_begin();
  if (!pass || !pass(function, &pass_changed)) {
    ir_verify_snapshot_free(verify_snapshot);
    ir_trace_pass_event(pass_name, "failed", version, -1);
    return 0;
  }
  if (ir_pass_time_enabled() && ir_pass_time_covers(function)) {
    g_ir_pass_ms[pass_id] += ir_pass_now_ticks() - t0;
    g_ir_pass_runs[pass_id]++;
  }
  ir_explain_pass_end(function, pass_name, pass_changed);

  if (verify_snapshot) {
    ir_verify_maybe_sabotage(function, pass_name, &pass_changed);
    if (!ir_verify_check_pass(function, verify_snapshot, pass_name,
                              &pass_changed)) {
      clean_version[pass_id] = *version;
    }
    ir_verify_snapshot_free(verify_snapshot);
  }

  ir_facts_invalidate();
  if (pass_changed) {
    *changed = 1;
    (*version)++;
  } else {
    clean_version[pass_id] = *version;
  }

  ir_trace_pass_event(pass_name, pass_changed ? "changed" : "clean", version,
                      pass_changed);
  if (pass_changed) {
    ir_dump_pass_result(function, pass_name);
  }
  ir_library_passes_after(function, pass_name);
  return 1;
}
