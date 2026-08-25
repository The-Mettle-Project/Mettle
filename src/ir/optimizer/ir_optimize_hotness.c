#include "ir_optimize_internal.h"
#include "../ir_pgo.h"

static long long ir_opt_hot_threshold(void) {
  return ir_pgo_hot_threshold();
}

static long long ir_opt_function_calls(const IRFunction *function) {
  if (!function || !function->name) {
    return -1;
  }
  return ir_pgo_callee_calls(function->name);
}

static long long ir_opt_function_steps(const IRFunction *function) {
  if (!function || !function->name) {
    return -1;
  }
  return ir_pgo_function_body_steps(function->name);
}

int ir_opt_function_is_hot(const IRFunction *function) {
  if (!ir_pgo_enabled() || !function || !function->name) {
    return 0;
  }
  long long hot = ir_opt_hot_threshold();
  long long calls = ir_opt_function_calls(function);
  long long steps = ir_opt_function_steps(function);
  return (calls >= hot) || (steps >= hot);
}

int ir_opt_function_is_cold(const IRFunction *function) {
  if (!ir_pgo_enabled() || !function || !function->name) {
    return 0;
  }
  long long calls = ir_opt_function_calls(function);
  long long steps = ir_opt_function_steps(function);
  return calls == 0 && steps == 0;
}

static long long ir_opt_site_count(const IRFunction *function,
                                   SourceLocation location) {
  if (!ir_pgo_enabled() || !function || !function->name ||
      location.line == 0) {
    return -1;
  }
  return ir_pgo_site_count(function->name, location);
}

int ir_opt_site_is_hot(const IRFunction *function, SourceLocation location) {
  long long count = ir_opt_site_count(function, location);
  if (count >= 0) {
    return count >= ir_opt_hot_threshold();
  }
  return ir_opt_function_is_hot(function);
}

int ir_opt_site_is_cold(const IRFunction *function, SourceLocation location) {
  long long count = ir_opt_site_count(function, location);
  if (count >= 0) {
    return count == 0;
  }
  return ir_opt_function_is_cold(function);
}

size_t ir_opt_inline_body_budget(const IRFunction *callee) {
  if (ir_opt_function_is_hot(callee)) {
    return 4u * IR_INLINE_MAX_NON_NOP_INSTRUCTIONS;
  }
  if (ir_opt_function_is_cold(callee)) {
    return IR_INLINE_MAX_NON_NOP_INSTRUCTIONS / 2u;
  }
  return IR_INLINE_MAX_NON_NOP_INSTRUCTIONS;
}

size_t ir_opt_inline_nested_call_budget(const IRFunction *callee) {
  if (ir_opt_function_is_hot(callee)) {
    return 6u;
  }
  if (ir_opt_function_is_cold(callee)) {
    return 1u;
  }
  return 2u;
}

size_t ir_opt_inline_caller_budget(const IRFunction *caller) {
  if (ir_opt_function_is_hot(caller)) {
    return 2u * IR_INLINE_MAX_CALLER_NON_NOP_INSTRUCTIONS;
  }
  if (ir_opt_function_is_cold(caller)) {
    return IR_INLINE_MAX_CALLER_NON_NOP_INSTRUCTIONS / 2u;
  }
  return IR_INLINE_MAX_CALLER_NON_NOP_INSTRUCTIONS;
}

int ir_opt_self_inline_max_depth(const IRFunction *function) {
  if (ir_opt_function_is_hot(function)) {
    return IR_SELF_INLINE_MAX_DEPTH + 1;
  }
  if (ir_opt_function_is_cold(function)) {
    return 1;
  }
  return IR_SELF_INLINE_MAX_DEPTH;
}

size_t ir_opt_self_inline_body_budget(const IRFunction *function) {
  if (ir_opt_function_is_hot(function)) {
    return IR_SELF_INLINE_MAX_BODY_INSTRUCTIONS +
           IR_SELF_INLINE_MAX_BODY_INSTRUCTIONS / 2u;
  }
  if (ir_opt_function_is_cold(function)) {
    return IR_SELF_INLINE_MAX_BODY_INSTRUCTIONS / 2u;
  }
  return IR_SELF_INLINE_MAX_BODY_INSTRUCTIONS;
}

long long ir_opt_unroll_max_trip_count(const IRFunction *function,
                                       SourceLocation location) {
  if (ir_opt_site_is_hot(function, location)) {
    return IR_UNROLL_HOT_MAX_TRIP_COUNT;
  }
  if (ir_opt_site_is_cold(function, location)) {
    return IR_UNROLL_COLD_MAX_TRIP_COUNT;
  }
  return IR_UNROLL_MAX_TRIP_COUNT;
}

long long ir_opt_prefetch_distance_for_site(const IRFunction *function,
                                            SourceLocation location,
                                            long long default_distance) {
  if (default_distance < 1) {
    return default_distance;
  }
  if (ir_opt_site_is_hot(function, location)) {
    long long widened = default_distance * 2;
    return widened > 4096 ? 4096 : widened;
  }
  if (ir_opt_site_is_cold(function, location)) {
    long long narrowed = default_distance / 4;
    return narrowed < 8 ? 8 : narrowed;
  }
  return default_distance;
}

int ir_opt_should_prefetch_site(const IRFunction *function,
                                SourceLocation location) {
  return !ir_opt_site_is_cold(function, location);
}
