#include "ir_optimize_internal.h"

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

static int ir_skip_delimiter(char c) {
  return c == ',' || c == ' ' || c == '\t';
}

static int ir_skip_token_equals(const char *token, size_t token_len,
                                const char *value) {
  return value && strlen(value) == token_len &&
         strncmp(token, value, token_len) == 0;
}

static int ir_pass_trace_enabled(void) {
  const char *spec = getenv("METTLE_TRACE_IR_PASSES");
  return spec && spec[0] != '\0' && strcmp(spec, "0") != 0;
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
}

/* Diagnostic: METTLE_SKIP_PASS="sroa,16" disables the listed pass names or
 * numeric pass IDs so a miscompile can be bisected to a single pass. */
int ir_pass_is_skipped(IROptPassId pass_id) {
  const char *spec = getenv("METTLE_SKIP_PASS");
  if (!spec || !*spec) {
    return 0;
  }

  if (pass_id < 0 || pass_id >= IR_OPT_PASS_COUNT) {
    return 0;
  }

  char id_text[16];
  int id_len = snprintf(id_text, sizeof(id_text), "%d", (int)pass_id);
  if (id_len <= 0) {
    return 0;
  }

  const char *pass_name = ir_opt_pass_name(pass_id);
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

static int ir_run_named_pass(IRFunction *function, const IROptNamedPass *pass,
                             const char *failure_message) {
  int changed = 0;

  if (!pass || !pass->name || !pass->run) {
    return 0;
  }

  mettle_compiler_ctx_set_pass_name(pass->name);
  if (!pass->run(function, &changed)) {
    ir_trace_pass_event(pass->name, "failed", NULL, -1);
    mettle_compiler_ice(failure_message);
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

  int pass_changed = 0;
  mettle_compiler_ctx_set_pass_name(pass_name);
  if (!pass || !pass(function, &pass_changed)) {
    ir_trace_pass_event(pass_name, "failed", version, -1);
    return 0;
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
