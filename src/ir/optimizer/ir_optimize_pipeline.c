#include "ir_optimize_internal.h"
#include "../ir_verify.h"

typedef struct {
  IROptPassId id;
  IROptFunctionPass run;
  struct {
    unsigned all;
    unsigned any;
  } gate;
} IROptScheduledPass;

typedef struct {
  const char *name;
  const IROptNamedPass *passes;
  size_t pass_count;
  const char *failure_message;
} IROptNamedStage;

typedef struct {
  const char *name;
  const IROptScheduledPass *passes;
  size_t pass_count;
  int max_iterations;
} IROptFixpointStage;

typedef enum {
  IR_OPT_FEATURE_LABEL = 1u << 0,
  IR_OPT_FEATURE_WHILE_LABEL = 1u << 1,
  IR_OPT_FEATURE_JUMP = 1u << 2,
  IR_OPT_FEATURE_BRANCH_ZERO = 1u << 3,
  IR_OPT_FEATURE_BRANCH_EQ = 1u << 4,
  IR_OPT_FEATURE_CALL = 1u << 5,
  IR_OPT_FEATURE_LOAD = 1u << 6,
  IR_OPT_FEATURE_ASSIGN = 1u << 7,
  IR_OPT_FEATURE_TEMP_WRITE = 1u << 8,
  IR_OPT_FEATURE_BINARY = 1u << 9,
  IR_OPT_FEATURE_DIV = 1u << 10
} IROptFeatureFlag;

#define IR_OPT_REQUIRE_NONE 0u
#define IR_OPT_FIXPOINT_MAX_ITERATIONS 8
#define IR_OPT_LABEL_JUMP (IR_OPT_FEATURE_LABEL | IR_OPT_FEATURE_JUMP)
#define IR_OPT_BRANCH_TESTS                                                   \
  (IR_OPT_FEATURE_JUMP | IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_BRANCH_EQ)
#define IR_OPT_PASS_ALWAYS(id, fn)                                            \
  { IR_OPT_PASS_##id, fn, {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE} }
#define IR_OPT_PASS_WHEN_ALL(id, fn, all_features)                            \
  { IR_OPT_PASS_##id, fn, {all_features, IR_OPT_REQUIRE_NONE} }
#define IR_OPT_PASS_WHEN_ALL_ANY(id, fn, all_features, any_features)           \
  { IR_OPT_PASS_##id, fn, {all_features, any_features} }

static const IROptNamedPass g_ir_pre_inline_passes[] = {
    {"simd_minmax_i32", ir_simd_minmax_i32_pass},
    {"prefix_sum_i32", ir_prefix_sum_i32_pass},
    {"induction_pointer", ir_pointer_induction_pass},
    {"simd_dot_i32", ir_simd_dot_i32_pass},
    {"simd_dot_i8", ir_simd_dot_i8_pass},
    {"simd_insertion_sort_i32", ir_simd_insertion_sort_i32_pass},
    {"simd_minmax_i32", ir_simd_minmax_i32_pass},
    {"lower_bound_i32", ir_lower_bound_i32_pass},
    {"prefix_sum_i32", ir_prefix_sum_i32_pass},
};

static const IROptNamedPass g_ir_post_fixpoint_passes[] = {
    {"induction_pointer", ir_pointer_induction_pass},
    /* After pointer induction so range-for fills (already converted to the
     * pointer-walk form) and while-loop fills (still indexed) both match. */
    {"simd_fill", ir_simd_fill_pass},
    {"prefix_sum_i32", ir_prefix_sum_i32_pass},
    {"simd_minmax_i32", ir_simd_minmax_i32_pass},
    {"simd_affine_map_float", ir_simd_affine_map_float_pass},
    {"simd_exp_f32", ir_simd_exp_f32_pass},
    {"simd_silu_f32", ir_simd_silu_f32_pass},
    {"simd_lcg", ir_simd_lcg_pass},
    {"simd_i2f_reduce", ir_simd_i2f_reduce_pass},
    {"simd_dot_float", ir_simd_dot_float_pass},
    {"simd_sum_float", ir_simd_sum_float_pass},
    {"auto_vectorize", ir_auto_vectorize_pass},
    {"auto_vectorize_int", ir_auto_vectorize_int_pass},
    {"auto_vectorize_find", ir_auto_vectorize_find_pass},
    {"outer_vectorize", ir_outer_vectorize_pass},
    {"simd_memory_map", ir_simd_memory_map_pass},
    {"lower_bound_i32", ir_lower_bound_i32_pass},
    {"detect_shift_loops", ir_detect_shift_loops_pass},
    {"eliminate_congruent_ivs", ir_eliminate_congruent_ivs_pass},
    /* After congruent-IV merge so parallel lane indices appear as base+J. */
    {"simd_slp_mac_i32", ir_simd_slp_mac_i32_pass},
    {"simd_slp_mac_i8", ir_simd_slp_mac_i8_pass},
    /* After every recognizer: collapses register-only data-dependent
     * diamonds to branchless selects. Runs before prefetch (which only
     * touches loop headers) and after vectorizers (whose loop bodies are
     * straight-line and thus unaffected). */
    {"if_convert", ir_if_convert_pass},
    /* LAST: inserts control flow into loop bodies, which would defeat every
     * recognizer above. Only fires on loops with indirect (load-fed) accesses
     * -- shapes no vectorizer can claim. */
    {"prefetch_indirect", ir_prefetch_indirect_pass},
};

static const IROptNamedStage g_ir_pre_inline_stage = {
    "pre-inline canonicalization",
    g_ir_pre_inline_passes,
    IR_ARRAY_COUNT(g_ir_pre_inline_passes),
    "IR optimization pre-inline pass failed",
};

/* SROA runs after copy/coalesce fold inlined struct copies into clean
 * symbol-to-symbol form, and before CSE/dead-temp cleanup. */
static const IROptScheduledPass g_ir_fixpoint_passes[] = {
    IR_OPT_PASS_WHEN_ALL(REDUCTION_UNROLL, ir_reduction_unroll_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_ALWAYS(COPY_AND_CONSTANT_PROPAGATION,
                       ir_copy_and_constant_propagation_pass),
    IR_OPT_PASS_ALWAYS(FUSE_ROTATE_ADD, ir_fuse_rotate_add_pass),
    IR_OPT_PASS_WHEN_ALL(STRENGTH_REDUCE_ROTATE_LOOPS,
                         ir_strength_reduce_rotate_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_WHEN_ALL(UNROLL_SMALL_CONST_BOUND_LOOPS,
                         ir_unroll_small_const_bound_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_WHEN_ALL(POSITIVE_LOOP_DIV2_TO_SHIFT,
                         ir_positive_loop_div2_to_shift_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_DIV),
    IR_OPT_PASS_WHEN_ALL(FOLD_POPCOUNT_BYTE_LOOP,
                         ir_fold_popcount_byte_loop_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY),
    IR_OPT_PASS_WHEN_ALL(FUSE_POPCOUNT_BUFFER_LOOP,
                         ir_fuse_popcount_buffer_loop_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(COLLATZ_ODD_STEP_FOLD,
                         ir_collatz_odd_step_fold_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY),
    IR_OPT_PASS_WHEN_ALL(COALESCE_SINGLE_USE_TEMP_ASSIGN,
                         ir_coalesce_single_use_temp_assign_pass,
                         IR_OPT_FEATURE_ASSIGN),
    IR_OPT_PASS_WHEN_ALL(ELIMINATE_SINGLE_USE_FLOAT_SYMBOL_COPIES,
                         ir_eliminate_single_use_float_symbol_copies_pass,
                         IR_OPT_FEATURE_ASSIGN),
    IR_OPT_PASS_ALWAYS(SROA, ir_sroa_pass),
    IR_OPT_PASS_ALWAYS(COMMON_SUBEXPRESSION_ELIMINATION,
                       ir_common_subexpression_elimination_pass),
    IR_OPT_PASS_ALWAYS(CONSTANT_AND_BRANCH_SIMPLIFY,
                       ir_constant_and_branch_simplify_pass),
    IR_OPT_PASS_WHEN_ALL(REASSOCIATE_CONSTANTS, ir_reassociate_constants_pass,
                         IR_OPT_FEATURE_BINARY),
    IR_OPT_PASS_WHEN_ALL(COUNT_WORD_STARTS, ir_count_word_starts_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(ELIMINATE_DEAD_TEMP_WRITES,
                         ir_eliminate_dead_temp_writes_pass,
                         IR_OPT_FEATURE_TEMP_WRITE),
    IR_OPT_PASS_WHEN_ALL_ANY(THREAD_JUMP_TARGETS,
                             ir_thread_jump_targets_pass,
                             IR_OPT_FEATURE_LABEL, IR_OPT_BRANCH_TESTS),
    IR_OPT_PASS_WHEN_ALL(NULL_CHECK_LICM, ir_null_check_licm_pass,
                         IR_OPT_FEATURE_WHILE_LABEL |
                             IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_CALL),
    IR_OPT_PASS_WHEN_ALL_ANY(REMOVE_EMPTY_CONDITIONAL_DIAMONDS,
                             ir_remove_empty_conditional_diamonds_pass,
                             IR_OPT_LABEL_JUMP,
                             IR_OPT_FEATURE_BRANCH_ZERO |
                                 IR_OPT_FEATURE_BRANCH_EQ),
    IR_OPT_PASS_WHEN_ALL_ANY(REMOVE_REDUNDANT_FALLTHROUGH_BRANCHES,
                             ir_remove_redundant_fallthrough_branches_pass,
                             IR_OPT_FEATURE_LABEL,
                             IR_OPT_FEATURE_BRANCH_ZERO |
                                 IR_OPT_FEATURE_BRANCH_EQ),
    IR_OPT_PASS_WHEN_ALL(REMOVE_REDUNDANT_JUMPS,
                         ir_remove_redundant_jumps_pass, IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_ALWAYS(ELIMINATE_UNREACHABLE_STRAIGHTLINE,
                       ir_eliminate_unreachable_straightline_pass),
    IR_OPT_PASS_WHEN_ALL_ANY(ELIMINATE_UNREACHABLE_BLOCKS,
                             ir_eliminate_unreachable_blocks_pass,
                             IR_OPT_FEATURE_LABEL, IR_OPT_BRANCH_TESTS),
    IR_OPT_PASS_WHEN_ALL(REMOVE_UNUSED_LABELS, ir_remove_unused_labels_pass,
                         IR_OPT_FEATURE_LABEL),
    IR_OPT_PASS_ALWAYS(MEMCPY_INLINE, ir_memcpy_inline_pass),
    IR_OPT_PASS_WHEN_ALL(MEMCMP_BYTE_LOOP, ir_memcmp_byte_loop_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_ALWAYS(ELIMINATE_LOAD_SYMBOL_COPY,
                       ir_eliminate_load_symbol_copy_pass),
    IR_OPT_PASS_WHEN_ALL(SIMD_SUM_I32, ir_simd_sum_i32_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_SUM_U8, ir_simd_sum_u8_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_BYTE_MAP, ir_simd_byte_map_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_DOT_I32, ir_simd_dot_i32_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_DOT_I8, ir_simd_dot_i8_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_INSERTION_SORT_I32,
                         ir_simd_insertion_sort_i32_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
};

static const IROptFixpointStage g_ir_fixpoint_stage = {
    "main fixpoint",
    g_ir_fixpoint_passes,
    IR_ARRAY_COUNT(g_ir_fixpoint_passes),
    IR_OPT_FIXPOINT_MAX_ITERATIONS,
};

static const IROptNamedStage g_ir_post_fixpoint_stage = {
    "post-fixpoint idiom recognition",
    g_ir_post_fixpoint_passes,
    IR_ARRAY_COUNT(g_ir_post_fixpoint_passes),
    "IR optimization pass failed",
};

static int ir_run_named_stage(IRFunction *function,
                              const IROptNamedStage *stage) {
  if (!stage || !stage->passes) {
    return 0;
  }

  mettle_compiler_ctx_set_pass_name(stage->name);
  mettle_compiler_ctx_set_fixpoint_iteration(0);
  return ir_run_named_pass_sequence(
      function, stage->passes, stage->pass_count, stage->failure_message);
}

int ir_optimize_pre_inline_function(IRFunction *function) {
  return ir_run_named_stage(function, &g_ir_pre_inline_stage);
}

static unsigned ir_opt_feature_flags(const IROptFunctionFeatures *features) {
  unsigned flags = 0;
  if (features->has_label) {
    flags |= IR_OPT_FEATURE_LABEL;
  }
  if (features->has_while_label) {
    flags |= IR_OPT_FEATURE_WHILE_LABEL;
  }
  if (features->has_jump) {
    flags |= IR_OPT_FEATURE_JUMP;
  }
  if (features->has_branch_zero) {
    flags |= IR_OPT_FEATURE_BRANCH_ZERO;
  }
  if (features->has_branch_eq) {
    flags |= IR_OPT_FEATURE_BRANCH_EQ;
  }
  if (features->has_call) {
    flags |= IR_OPT_FEATURE_CALL;
  }
  if (features->has_load) {
    flags |= IR_OPT_FEATURE_LOAD;
  }
  if (features->has_assign) {
    flags |= IR_OPT_FEATURE_ASSIGN;
  }
  if (features->has_temp_write) {
    flags |= IR_OPT_FEATURE_TEMP_WRITE;
  }
  if (features->has_binary) {
    flags |= IR_OPT_FEATURE_BINARY;
  }
  if (features->has_div) {
    flags |= IR_OPT_FEATURE_DIV;
  }
  return flags;
}

static int ir_scheduled_pass_is_enabled(const IROptScheduledPass *pass,
                                        unsigned features) {
  if ((features & pass->gate.all) != pass->gate.all) {
    return 0;
  }
  return pass->gate.any == IR_OPT_REQUIRE_NONE ||
         (features & pass->gate.any) != 0;
}

static int ir_run_fixpoint_stage(IRFunction *function,
                                 const IROptFixpointStage *stage) {
  if (!stage || !stage->passes || stage->max_iterations <= 0) {
    return 0;
  }

  unsigned long long version = 1;
  unsigned long long clean_version[IR_OPT_PASS_COUNT];
  for (int i = 0; i < IR_OPT_PASS_COUNT; i++) {
    clean_version[i] = 0;
  }

  for (int iteration = 0; iteration < stage->max_iterations; iteration++) {
    int changed = 0;
    IROptFunctionFeatures features;

    mettle_compiler_ctx_set_fixpoint_iteration(iteration + 1);
    ir_collect_function_features(function, &features);
    unsigned feature_flags = ir_opt_feature_flags(&features);

    for (size_t pass_index = 0; pass_index < stage->pass_count; pass_index++) {
      const IROptScheduledPass *pass = &stage->passes[pass_index];
      int enabled = ir_scheduled_pass_is_enabled(pass, feature_flags);
      if (!ir_run_fixpoint_pass(function, pass->id, pass->run, enabled, &version,
                                clean_version, &changed)) {
        return 0;
      }
    }

    if (!changed) {
      break;
    }
  }

  mettle_compiler_ctx_set_fixpoint_iteration(0);
  return 1;
}

int ir_optimize_function_pipeline(IRFunction *function) {
  if (!function) {
    return 0;
  }

  {
    int pre_changed = 0;
    if (!ir_fuse_rotate_add_pass(function, &pre_changed)) {
      return 0;
    }
  }

  if (!ir_run_fixpoint_stage(function, &g_ir_fixpoint_stage)) {
    return 0;
  }

  if (!ir_run_named_stage(function, &g_ir_post_fixpoint_stage)) {
    return 0;
  }

  /* Enforce `@simd` contracts now that every vectorizer has had its chance,
   * then strip the markers before CFG rebuild / codegen. */
  double t0 = ir_pass_time_begin();
  if (!ir_verify_simd_contracts(function)) {
    return 0;
  }
  ir_pass_time_end("verify_simd_contracts [stage]", t0);

  t0 = ir_pass_time_begin();
  int ok = ir_function_rebuild_cfg(function);
  ir_pass_time_end("rebuild_cfg [stage]", t0);
  return ok;
}

/* --explain hypothesis testing: re-run the optimization stages (including
 * every vectorizer) on a scratch clone that carries a simulated fix. No
 * contract verification, no CFG rebuild -- the caller inspects the clone's
 * marker regions itself and then throws it away. */
int ir_optimize_function_revectorize(IRFunction *function) {
  if (!function) {
    return 0;
  }
  if (!ir_run_fixpoint_stage(function, &g_ir_fixpoint_stage)) {
    return 0;
  }
  return ir_run_named_stage(function, &g_ir_post_fixpoint_stage);
}

static void ir_set_current_function_context(IRFunction *function) {
  if (function) {
    mettle_compiler_ctx_set_function_name(
        function->name ? function->name : "<anonymous>");
  }
}

static int ir_run_program_stage_for_each_function(
    IRProgram *program, int (*run)(IRFunction *function)) {
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    ir_set_current_function_context(function);
    if (!run(function)) {
      return 0;
    }
  }
  return 1;
}

int ir_optimize_program_pipeline(IRProgram *program,
                                 const IROptimizeOptions *options) {
  if (!program) {
    return 0;
  }

  ir_optimize_reset_user_error();
  ir_optimize_set_simd_report(options && options->simd_report);
  ir_optimize_set_explain(options && options->explain,
                          options ? options->explain_focus_file : NULL);
  ir_function_index_reset();
  ir_verify_begin_program(program);

  /* Fold never-written global integer vars to their initializer constants
   * first, so every later pass (strength reduction, vectorizers, TRE) sees
   * plain constants instead of opaque global reads. */
  if (options && options->global_int_consts &&
      !ir_pass_name_is_skipped("fold_readonly_globals")) {
    int fold_changed = 0;
    mettle_compiler_ctx_set_pass_name("fold_readonly_globals");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_fold_readonly_globals_pass(program, options->global_int_consts,
                                       options->global_int_const_count,
                                       &fold_changed)) {
      mettle_compiler_ice("IR read-only global fold pass failed");
    }
    ir_pass_time_end("fold_readonly_globals [program]", t0);
  }

  {
    double t0 = ir_pass_time_begin();
    if (!ir_run_program_stage_for_each_function(
            program, ir_optimize_pre_inline_function)) {
      ir_function_index_reset();
      return 0;
    }
    ir_pass_time_end("pre_inline [stage]", t0);
  }

  /* Tail-recursion elimination before any inlining: converting the tail
   * self call into a loop first means the regular inliner sees a loop-shaped
   * callee and the bounded self-recursion expander only has the remaining
   * non-tail calls to amortize. */
  if ((!options || !options->preserve_function_boundaries) &&
      !ir_pass_name_is_skipped("tail_recursion_elim")) {
    int tre_changed = 0;
    mettle_compiler_ctx_set_pass_name("tail_recursion_elim");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_tail_recursion_elimination_pass(program, &tre_changed)) {
      mettle_compiler_ice("IR tail-recursion elimination pass failed");
    }
    ir_pass_time_end("tail_recursion_elim [program]", t0);
  }

  if ((!options || !options->preserve_function_boundaries) &&
      !ir_pass_name_is_skipped("inline_small_functions")) {
    int inlining_changed = 0;
    mettle_compiler_ctx_set_pass_name("inline_small_functions");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_inline_small_functions_pass(program, &inlining_changed)) {
      mettle_compiler_ice("IR optimization inlining pass failed");
    }
    ir_pass_time_end("inline_small_functions [program]", t0);
  }

  /* Bounded recursive inlining: expand a recursive function's direct
   * self-call sites into copies of its own body (depth- and size-capped), so
   * each remaining real call amortizes prologue/epilogue and argument-passing
   * overhead across a subtree of the recursion. Runs after the regular
   * inliner so a self-recursive helper is first inlined into callers where
   * possible, then expanded in place. */
  if ((!options || !options->preserve_function_boundaries) &&
      !ir_pass_name_is_skipped("inline_self_recursion")) {
    int self_inline_changed = 0;
    mettle_compiler_ctx_set_pass_name("inline_self_recursion");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_inline_self_recursion_pass(program, &self_inline_changed)) {
      mettle_compiler_ice("IR optimization self-recursion inlining failed");
    }
    ir_pass_time_end("inline_self_recursion [program]", t0);
  }

  /* `@pure` loop-invariant call hoisting. Program-level (resolves callees by
   * name) and run after inlining so an inlined pure body is hoisted as ordinary
   * loop-invariant code; the per-function fixpoint below then cleans up. */
  if (!ir_pass_name_is_skipped("hoist_pure_calls")) {
    int pure_licm_changed = 0;
    mettle_compiler_ctx_set_pass_name("hoist_pure_calls");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_hoist_pure_calls_pass(program, &pure_licm_changed)) {
      mettle_compiler_ice("IR optimization pure-call hoisting pass failed");
    }
    ir_pass_time_end("hoist_pure_calls [program]", t0);
  }

  /* Allocation-site layout factorization: re-map provably-private malloc
   * pools (compact padded strides / factor into per-field SoA arrays).
   * Whole-program only: rewriting a callee body to a new pool layout is
   * sound only when every call site is visible. Runs after inlining so
   * field-accessor helpers are already folded into their callers, and
   * before the per-function stage so the vectorizers see the rewritten
   * unit-stride form. NOT under per-function --verify: the transform
   * preserves program behavior but changes the buffer's byte image, which
   * the per-function validator counts as an observation (and a coordinated
   * multi-function rewrite must never be quarantined one function at a
   * time). METTLE_SKIP_PASS=layout_factor disables it. */
  if (options && options->whole_program &&
      !options->preserve_function_boundaries &&
      !ir_pass_name_is_skipped("layout_factor")) {
    int layout_changed = 0;
    mettle_compiler_ctx_set_pass_name("layout_factor");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_layout_factor_pass(program, &layout_changed)) {
      mettle_compiler_ice("IR layout factorization pass failed");
    }
    ir_pass_time_end("layout_factor [program]", t0);
  }

  /* Give the per-function contract verifier program access for the duration
   * of the stage: the call-in-body fix simulation re-runs the inliner on a
   * caller clone, which needs callee lookup. */
  ir_explain_set_program(program);
  if (!ir_run_program_stage_for_each_function(
          program, ir_optimize_function_pipeline)) {
    ir_explain_set_program(NULL);
    /* A violated `@simd!` contract already printed a user diagnostic; don't
     * dress it up as an internal compiler error. */
    if (!ir_optimize_had_user_error()) {
      mettle_compiler_ice_report("IR optimization failed", NULL);
    }
    ir_function_index_reset();
    return 0;
  }
  ir_explain_set_program(NULL);

  /* Function-level contracts, now that every optimization that could satisfy
   * them has run. `@inline!` is skipped when function boundaries are pinned
   * (--profile-runtime disables inlining entirely; failing every contract
   * there would be noise, not information). Check both before deciding the
   * outcome so a build with several violations reports them all. */
  int contracts_ok = 1;
  if (!options || !options->preserve_function_boundaries) {
    contracts_ok &= ir_inline_enforce_contracts(program);
  }
  contracts_ok &= ir_enforce_noalloc_contracts(program);

  /* --explain: every inline that happened was recorded as it happened; record
   * each surviving call with the reason it was refused, then print the whole
   * sorted report. (No-ops unless explain is enabled.) */
  ir_inline_explain_report_remaining(program);
  ir_explain_flush();
  if (!contracts_ok) {
    /* Compilation stops before codegen, so the backend flush (the normal
     * report-routing point) never runs: print the buffered report now. */
    ir_explain_finalize(1);
  }
  ir_pass_time_report();
  ir_verify_end_program();

  ir_function_index_reset();
  return contracts_ok;
}
