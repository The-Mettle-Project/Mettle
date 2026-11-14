#include "ir_optimize_internal.h"
#include "ir_optimize_explain_internal.h"
#include "../ir_verify.h"
#include "../ir_explain_ownership.h"
#include "../ir_safety.h"

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
  const IROptScheduledPass *passes;
  size_t pass_count;
  int max_iterations;
} IROptFixpointStage;

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

#define IR_OPT_CANONICAL_MAX_ITERATIONS 6
#define IR_OPT_RECOGNIZER_MAX_ITERATIONS 2

#define IR_GATE_LOOP {IR_OPT_LABEL_JUMP, IR_OPT_REQUIRE_NONE}
#define IR_GATE_LOOP_LOAD                                                     \
  {IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}
#define IR_GATE_LOOP_BRANCH                                                   \
  {IR_OPT_LABEL_JUMP, IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_BRANCH_EQ}

static IRBuiltinLibraryRun g_ir_builtin_library = NULL;

void ir_builtin_library_set(IRBuiltinLibraryRun run) {
  g_ir_builtin_library = run;
}

static int ir_or_chain_library_pass(IRFunction *function, int *changed) {
  const char *label = NULL;
  int run = 0;
  if (!g_ir_builtin_library) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count && run < 3; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_BRANCH_EQ && ins->rhs.kind == IR_OPERAND_INT && ins->text &&
        (run == 0 || strcmp(ins->text, label) == 0)) {
      label = ins->text;
      run++;
    } else {
      run = 0;
    }
  }
  if (run < 3) {
    return 1;
  }
  return g_ir_builtin_library("or_chain_bitset", function, changed);
}

static const IROptNamedPass g_ir_pre_inline_canonical[] = {
    {"drop_dead_narrowing", ir_drop_dead_narrowing_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"hoist_global_bases", ir_hoist_global_bases_pass, IR_GATE_LOOP},
    {"hoist_row_pointers", ir_hoist_row_pointers_pass, IR_GATE_LOOP},
    {"if_convert_accumulate", ir_if_convert_accumulate_pass,
     IR_GATE_LOOP_BRANCH},
    {"scan_from_first", ir_normalize_scan_from_first_pass, IR_GATE_LOOP_LOAD},
};

static const IROptNamedPass g_ir_pre_inline_leaf_cleanup[] = {
    {"leaf_branch_simplify", ir_constant_and_branch_simplify_pass, {0, 0}},
    {"leaf_unreachable", ir_eliminate_unreachable_straightline_pass, {0, 0}},
    {"leaf_redundant_jumps", ir_remove_redundant_jumps_pass, {0, 0}},
    {"leaf_unused_labels", ir_remove_unused_labels_pass, {0, 0}},
    {"leaf_forward_stored_values", ir_forward_stored_values_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"leaf_copy_prop", ir_copy_and_constant_propagation_pass, {0, 0}},
    {"leaf_dead_temps", ir_eliminate_dead_temp_writes_pass, {0, 0}},
};

static int ir_function_makes_no_calls(const IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_CALL || op == IR_OP_CALL_INDIRECT ||
        op == IR_OP_INLINE_ASM) {
      return 0;
    }
  }
  return 1;
}

static const IROptNamedPass g_ir_pre_inline_recognizers[] = {
    {"user_rewrite", ir_user_rewrite_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"simd_minmax_i32", ir_simd_minmax_i32_pass, IR_GATE_LOOP_LOAD},
    {"prefix_sum_i32", ir_prefix_sum_i32_pass, IR_GATE_LOOP_LOAD},
    {"induction_pointer", ir_pointer_induction_pass, IR_GATE_LOOP},
    {"simd_dot_i32", ir_simd_dot_i32_pass, IR_GATE_LOOP_LOAD},
    {"simd_dot_i8", ir_simd_dot_i8_pass, IR_GATE_LOOP_LOAD},
    {"simd_insertion_sort_i32", ir_simd_insertion_sort_i32_pass,
     IR_GATE_LOOP_LOAD},
    {"lower_bound_i32", ir_lower_bound_i32_pass, IR_GATE_LOOP_LOAD},
};

static const IROptNamedPass g_ir_loop_canonical_passes[] = {
    {"drop_dead_narrowing", ir_drop_dead_narrowing_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"demote_scalar_addresses", ir_demote_scalar_addresses_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"unify_param_copy_spelling", ir_unify_param_copy_spelling_pass,
     IR_GATE_LOOP},
    {"hoist_body_locals", ir_hoist_body_locals_pass, IR_GATE_LOOP},
    {"hoist_global_bases", ir_hoist_global_bases_pass, IR_GATE_LOOP},
    {"hoist_row_pointers", ir_hoist_row_pointers_pass, IR_GATE_LOOP},
    {"forward_piece_copies", ir_forward_piece_copies_pass, IR_GATE_LOOP},
    {"hoist_descriptor_loads", ir_hoist_descriptor_loads_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"hoist_load_bases", ir_hoist_load_bases_pass, IR_GATE_LOOP},
    {"hoist_column_bases", ir_hoist_column_bases_pass, IR_GATE_LOOP},
    {"hoist_dead_temps", ir_eliminate_dead_temp_writes_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"hoist_invariant_assigns", ir_hoist_invariant_assigns_pass, IR_GATE_LOOP},
    {"if_convert_accumulate", ir_if_convert_accumulate_pass,
     IR_GATE_LOOP_BRANCH},
    {"scan_from_first", ir_normalize_scan_from_first_pass, IR_GATE_LOOP_LOAD},
};

static const IROptNamedPass g_ir_recognizer_passes[] = {
    {"simd_sum_i32", ir_simd_sum_i32_pass, IR_GATE_LOOP_LOAD},
    {"simd_sum_u8", ir_simd_sum_u8_pass, IR_GATE_LOOP_LOAD},
    {"simd_byte_map", ir_simd_byte_map_pass, IR_GATE_LOOP_LOAD},
    {"simd_dot_i32", ir_simd_dot_i32_pass, IR_GATE_LOOP_LOAD},
    {"simd_dot_i8", ir_simd_dot_i8_pass, IR_GATE_LOOP_LOAD},
    {"simd_minmax_reduce", ir_simd_minmax_reduce_pass, IR_GATE_LOOP_LOAD},
    {"induction_pointer", ir_pointer_induction_pass, IR_GATE_LOOP},
    {"simd_fill", ir_simd_fill_pass, IR_GATE_LOOP},
    {"simd_copy", ir_simd_copy_pass, IR_GATE_LOOP},
    {"prefix_sum_i32", ir_prefix_sum_i32_pass, IR_GATE_LOOP_LOAD},
    {"simd_minmax_i32", ir_simd_minmax_i32_pass, IR_GATE_LOOP_LOAD},
    {"simd_affine_map_float", ir_simd_affine_map_float_pass, IR_GATE_LOOP},
    {"simd_exp_f32", ir_simd_exp_f32_pass, IR_GATE_LOOP},
    {"simd_silu_f32", ir_simd_silu_f32_pass, IR_GATE_LOOP},
    {"simd_lcg", ir_simd_lcg_pass, IR_GATE_LOOP},
    {"simd_i2f_reduce", ir_simd_i2f_reduce_pass, IR_GATE_LOOP},
    {"simd_dot_float", ir_simd_dot_float_pass, IR_GATE_LOOP_LOAD},
    {"simd_sum_float", ir_simd_sum_float_pass, IR_GATE_LOOP_LOAD},
    {"auto_vectorize", ir_auto_vectorize_pass, IR_GATE_LOOP},
    {"auto_vectorize_int", ir_auto_vectorize_int_pass, IR_GATE_LOOP},
    {"auto_vectorize_find", ir_auto_vectorize_find_pass, IR_GATE_LOOP},
    {"outer_vectorize", ir_outer_vectorize_pass, IR_GATE_LOOP},
    {"simd_memory_map", ir_simd_memory_map_pass, IR_GATE_LOOP},
    {"lower_bound_i32", ir_lower_bound_i32_pass, IR_GATE_LOOP_LOAD},
    {"detect_shift_loops", ir_detect_shift_loops_pass, IR_GATE_LOOP},
    {"eliminate_congruent_ivs", ir_eliminate_congruent_ivs_pass,
     IR_GATE_LOOP},
    {"simd_slp_mac_i32", ir_simd_slp_mac_i32_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"simd_slp_mac_i8", ir_simd_slp_mac_i8_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"simd_lanes", ir_simd_lanes_pass, IR_GATE_LOOP},
};

static const IROptNamedPass g_ir_post_recognizer_tail[] = {
    {"if_convert", ir_if_convert_pass,
     {IR_OPT_FEATURE_LABEL,
      IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_BRANCH_EQ}},
    {"prefetch_indirect", ir_prefetch_indirect_pass, IR_GATE_LOOP_LOAD},
};

static const IROptNamedPass g_ir_ssa_enter[] = {
    {"promote_scalar_locals", ir_promote_scalar_locals_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
};

static const IROptNamedPass g_ir_ssa_optimize[] = {
    {"ssa_propagate", ir_ssa_propagate_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"ssa_sabotage", ir_ssa_sabotage_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"ssa_branch_simplify", ir_constant_and_branch_simplify_pass, {0, 0}},
    {"ssa_redundant_jumps", ir_remove_redundant_jumps_pass, {0, 0}},
    {"ssa_unused_labels", ir_remove_unused_labels_pass, {0, 0}},
};
static const IROptNamedPass g_ir_ssa_leave[] = {
    {"leave_ssa", ir_leave_ssa_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
};

static const IROptNamedPass g_ir_lowering_cleanup[] = {
    {"select_field_load", ir_select_adjacent_field_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_BRANCH_ZERO, IR_OPT_REQUIRE_NONE}},
    {"promote_loop_memory", ir_promote_loop_memory_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"hoist_invariant_loads", ir_hoist_invariant_loads_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"hoist_held_column_bases", ir_hoist_held_column_bases_pass,
     {IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"hoist_invariant_arith", ir_hoist_invariant_arith_pass,
     {IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"forward_stored_values", ir_forward_stored_values_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"widen_subword_cast", ir_widen_subword_load_cast_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"widen_byte_pack", ir_widen_byte_pack_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"ascii_casefold_range", ir_ascii_casefold_range_pass,
     {IR_OPT_FEATURE_BRANCH_ZERO, IR_OPT_REQUIRE_NONE}},
    {"fold_range_test", ir_fold_range_test_pass,
     {IR_OPT_FEATURE_BRANCH_ZERO, IR_OPT_REQUIRE_NONE}},
    {"or_chain_bitset", ir_or_chain_library_pass,
     {IR_OPT_FEATURE_BRANCH_EQ | IR_OPT_FEATURE_BRANCH_ZERO,
      IR_OPT_REQUIRE_NONE}},
    {"float_pow2_reciprocal", ir_float_divide_by_power_of_two_pass,
     {IR_OPT_FEATURE_DIV, IR_OPT_REQUIRE_NONE}},
    {"redundancy_elim", ir_redundancy_elimination_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"redundancy_copy_prop", ir_copy_and_constant_propagation_pass, {0, 0}},
    {"devirtualize_known_tables", ir_devirtualize_known_tables_pass, {0, 0}},
    {"merge_const_stores", ir_merge_adjacent_const_stores_pass, {0, 0}},
    {"merge_row_pointers", ir_merge_row_pointers_pass,
     {IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"redundancy_dead_temps", ir_eliminate_dead_temp_writes_pass,
     {IR_OPT_FEATURE_TEMP_WRITE, IR_OPT_REQUIRE_NONE}},
    {"auto_vectorize_class_find", ir_auto_vectorize_find_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_LABEL,
      IR_OPT_FEATURE_BRANCH_ZERO}},
    /* Last, because a cast is a shape the recognizers above match on: dropping
       one earlier makes a byte kernel widen the wrong way. */
    {"drop_redundant_int_casts", ir_drop_redundant_int_casts_pass, {0, 0}},
    {"cast_copy_prop", ir_copy_and_constant_propagation_pass, {0, 0}},
    {"cast_dead_temps", ir_eliminate_dead_temp_writes_pass,
     {IR_OPT_FEATURE_TEMP_WRITE, IR_OPT_REQUIRE_NONE}},
    {"cast_coalesce_temp_assign", ir_coalesce_single_use_temp_assign_pass,
     {IR_OPT_FEATURE_ASSIGN, IR_OPT_REQUIRE_NONE}},
    {"late_invariant_arith", ir_hoist_invariant_arith_pass,
     {IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"guard_and_hoist_load", ir_guard_loop_and_hoist_load_pass,
     {IR_OPT_FEATURE_LABEL | IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
};

static const IROptScheduledPass g_ir_fixpoint_passes[] = {
    IR_OPT_PASS_WHEN_ALL(REDUCTION_UNROLL, ir_reduction_unroll_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_ALWAYS(COPY_AND_CONSTANT_PROPAGATION,
                       ir_copy_and_constant_propagation_pass),
    IR_OPT_PASS_ALWAYS(USER_REWRITE, ir_user_rewrite_pass),
    IR_OPT_PASS_ALWAYS(FUSE_TENSOR_MMA_CHAINS,
                       ir_fuse_tensor_mma_chains_pass),
    IR_OPT_PASS_ALWAYS(FUSE_ROTATE_ADD, ir_fuse_rotate_add_pass),
    IR_OPT_PASS_WHEN_ALL(STRENGTH_REDUCE_ROTATE_LOOPS,
                         ir_strength_reduce_rotate_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_WHEN_ALL(UNROLL_SMALL_CONST_BOUND_LOOPS,
                         ir_unroll_small_const_bound_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_WHEN_ALL(UNROLL_ANNOTATED_LOOPS,
                         ir_unroll_annotated_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_WHEN_ALL(FOLD_POPCOUNT_BYTE_LOOP,
                         ir_fold_popcount_byte_loop_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY),
    IR_OPT_PASS_WHEN_ALL(FUSE_POPCOUNT_BUFFER_LOOP,
                         ir_fuse_popcount_buffer_loop_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(FOLD_KERNIGHAN_POPCOUNT,
                         ir_fold_kernighan_popcount_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY),
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
    IR_OPT_PASS_WHEN_ALL(EGRAPH_SIMPLIFY, ir_egraph_simplify_pass,
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
                         IR_OPT_FEATURE_LABEL | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_CALL),
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

static const IROptScheduledPass g_ir_portable_fixpoint_passes[] = {
    IR_OPT_PASS_ALWAYS(DROP_DEAD_NARROWING, ir_drop_dead_narrowing_pass),
    IR_OPT_PASS_WHEN_ALL(UNROLL_ANNOTATED_LOOPS,
                         ir_unroll_annotated_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_ALWAYS(COPY_AND_CONSTANT_PROPAGATION,
                       ir_copy_and_constant_propagation_pass),
    IR_OPT_PASS_ALWAYS(FUSE_TENSOR_MMA_CHAINS,
                       ir_fuse_tensor_mma_chains_pass),
    IR_OPT_PASS_WHEN_ALL(PROMOTE_GPU_ASYNC_STAGING,
                         ir_promote_gpu_async_staging_pass,
                         IR_OPT_FEATURE_LOAD),
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
    IR_OPT_PASS_WHEN_ALL(ELIMINATE_DEAD_TEMP_WRITES,
                         ir_eliminate_dead_temp_writes_pass,
                         IR_OPT_FEATURE_TEMP_WRITE),
    IR_OPT_PASS_WHEN_ALL_ANY(THREAD_JUMP_TARGETS,
                             ir_thread_jump_targets_pass,
                             IR_OPT_FEATURE_LABEL, IR_OPT_BRANCH_TESTS),
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
    IR_OPT_PASS_ALWAYS(ELIMINATE_LOAD_SYMBOL_COPY,
                       ir_eliminate_load_symbol_copy_pass),
};

static const IROptFixpointStage g_ir_portable_fixpoint_stage = {
    "target-neutral fixpoint",
    g_ir_portable_fixpoint_passes,
    IR_ARRAY_COUNT(g_ir_portable_fixpoint_passes),
    IR_OPT_FIXPOINT_MAX_ITERATIONS,
};

int ir_optimize_pre_inline_function(IRFunction *function) {
  mettle_compiler_ctx_set_pass_name("pre-inline canonicalization");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_pre_inline_canonical,
          IR_ARRAY_COUNT(g_ir_pre_inline_canonical),
          IR_OPT_CANONICAL_MAX_ITERATIONS, "pre-inline loop canonical form",
          "IR optimization pre-inline pass failed", 1)) {
    return 0;
  }
  mettle_compiler_ctx_set_pass_name("pre-inline idiom recognition");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_pre_inline_recognizers,
          IR_ARRAY_COUNT(g_ir_pre_inline_recognizers),
          IR_OPT_RECOGNIZER_MAX_ITERATIONS, "pre-inline idiom recognition",
          "IR optimization pre-inline pass failed", 0)) {
    return 0;
  }
  ir_library_passes_at(function, "pre_inline");
  return 1;
}

size_t ir_inline_cleaned_instruction_count(const IRFunction *function) {
  if (!function || function->has_volatile_access ||
      !ir_function_makes_no_calls(function)) {
    return SIZE_MAX;
  }
  IRFunction *clone = ir_function_create(function->name);
  if (!clone) {
    return SIZE_MAX;
  }
  int ok = ir_function_set_parameters(
      clone, (const char **)function->parameter_names,
      (const char **)function->parameter_types, function->parameter_count);
  for (size_t i = 0; ok && i < function->instruction_count; i++) {
    IRInstruction cloned = {0};
    ok = ir_clone_instruction_plain(&function->instructions[i], &cloned) &&
         ir_function_append_instruction(clone, &cloned);
    ir_instruction_destroy_storage(&cloned);
  }
  clone->entry_block = function->entry_block;
  size_t count = SIZE_MAX;
  if (ok && ir_run_named_stage_fixpoint(
                clone, g_ir_pre_inline_leaf_cleanup,
                IR_ARRAY_COUNT(g_ir_pre_inline_leaf_cleanup), 1,
                "inline size measurement",
                "IR optimization pre-inline pass failed", 0)) {
    count = 0;
    for (size_t i = 0; i < clone->instruction_count; i++) {
      if (clone->instructions[i].op != IR_OP_NOP) {
        count++;
      }
    }
  }
  ir_function_destroy(clone);
  return count;
}

#define IR_FP_MAX_LOOPS 64

typedef struct {
  char *label;
  unsigned long long fp;
} IRLoopFpEntry;

static int ir_loop_fp_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_LOOP_FINGERPRINT");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static int ir_loop_fp_snapshot(IRFunction *function, IRLoopFpEntry *entries) {
  int count = 0;
  int total = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL || !ir_label_is_while_header(ins->text)) {
      continue;
    }
    IRAffineLoop loop;
    if (!ir_affine_model_loop(function, i, &loop)) {
      continue;
    }
    total++;
    if (count >= IR_FP_MAX_LOOPS) {
      continue;
    }
    entries[count].label = mettle_strdup(ins->text);
    if (!entries[count].label) {
      continue;
    }
    entries[count].fp = ir_affine_loop_fingerprint(function, &loop);
    count++;
  }
  if (total > count) {
    fprintf(stderr,
            "[loop-fp] NOTE function=%s has %d modelled loops but the cap is "
            "%d; %d are unchecked\n",
            function->name ? function->name : "<anonymous>", total,
            IR_FP_MAX_LOOPS, total - count);
  }
  return count;
}

static void ir_loop_fp_report(const IRFunction *function,
                              const IRLoopFpEntry *entries, int count) {
  for (int e = 0; e < count; e++) {
    int survives = 0;
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ins->op == IR_OP_LABEL && ins->text &&
          strcmp(ins->text, entries[e].label) == 0) {
        survives = 1;
        break;
      }
    }
    fprintf(stderr, "[loop-fp] function=%s loop=%s fp=%016llx claimed=%d\n",
            function->name ? function->name : "<anonymous>", entries[e].label,
            entries[e].fp, survives ? 0 : 1);
  }
}

static void ir_loop_fp_destroy(IRLoopFpEntry *entries, int count) {
  for (int e = 0; e < count; e++) {
    free(entries[e].label);
  }
}

static int ir_run_post_fixpoint_stages(IRFunction *function) {
  mettle_compiler_ctx_set_pass_name("loop canonical form");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_loop_canonical_passes,
          IR_ARRAY_COUNT(g_ir_loop_canonical_passes),
          IR_OPT_CANONICAL_MAX_ITERATIONS, "loop canonical form",
          "IR optimization pass failed", 1)) {
    return 0;
  }
  {
    char detail[192];
    detail[0] = '\0';
    if (!ir_verify_loop_canonical_form(function, detail, sizeof(detail))) {
      int quarantined = 0;
      for (size_t i = 0; i < IR_ARRAY_COUNT(g_ir_loop_canonical_passes); i++) {
        if (ir_verify_pass_quarantined(function,
                                       g_ir_loop_canonical_passes[i].name)) {
          quarantined = 1;
          break;
        }
      }
      if (!quarantined) {
        fprintf(stderr,
                "mettle: internal error: loop canonical form does not hold in "
                "function '%s': %s\n",
                function->name ? function->name : "<anonymous>", detail);
        mettle_compiler_ice("IR loop canonical form violated");
      }
    }
  }

  ir_library_passes_at(function, "loop_canonical");

  mettle_compiler_ctx_set_pass_name("post-fixpoint idiom recognition");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_recognizer_passes,
          IR_ARRAY_COUNT(g_ir_recognizer_passes),
          IR_OPT_RECOGNIZER_MAX_ITERATIONS, "post-fixpoint idiom recognition",
          "IR optimization pass failed", 0)) {
    return 0;
  }
  ir_library_passes_at(function, "recognizers");
  mettle_compiler_ctx_set_pass_name("post-recognizer tail");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_post_recognizer_tail,
          IR_ARRAY_COUNT(g_ir_post_recognizer_tail), 1, "post-recognizer tail",
          "IR optimization pass failed", 0)) {
    return 0;
  }
  return 1;
}

static int ir_scheduled_pass_is_enabled(const IROptScheduledPass *pass,
                                        unsigned features) {
  if ((features & pass->gate.all) != pass->gate.all) {
    return 0;
  }
  return pass->gate.any == IR_OPT_REQUIRE_NONE ||
         (features & pass->gate.any) != 0;
}

static int ir_time_functions_enabled(void) {
  static int cached = -1;

  if (cached < 0) {
    cached = getenv("METTLE_TIME_FUNCTIONS") != NULL;
  }
  return cached;
}

static int ir_run_fixpoint_stage(IRFunction *function,
                                 const IROptFixpointStage *stage) {
  if (!stage || !stage->passes || stage->max_iterations <= 0) {
    return 0;
  }

  unsigned long long version = 1;
  int used = 1;
  unsigned long long clean_version[IR_OPT_PASS_COUNT];
  for (int i = 0; i < IR_OPT_PASS_COUNT; i++) {
    clean_version[i] = 0;
  }

  for (int iteration = 0; iteration < stage->max_iterations; iteration++) {
    int changed = 0;
    IROptFunctionFeatures features;

    if (ir_function_drop_dead_nops(function) > 0) {
      version++;
    }
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
    used = iteration + 2;
  }

  if (ir_time_functions_enabled() && function->instruction_count > 800) {
    fprintf(stderr, "   fixpoint %s: %d iterations over %zu instructions\n",
            function->name ? function->name : "?", used,
            function->instruction_count);
  }
  mettle_compiler_ctx_set_fixpoint_iteration(0);
  return 1;
}

int ir_optimize_function_pipeline(IRFunction *function) {
  if (!function) {
    return 0;
  }

  ir_explain_function_before(function);

  if (!ir_pass_name_is_skipped("fuse_rotate_add")) {
    int pre_changed = 0;
    if (!ir_fuse_rotate_add_pass(function, &pre_changed)) {
      return 0;
    }
  }

  IRLoopFpEntry fp_entries[IR_FP_MAX_LOOPS];
  int fp_count = 0;
  if (ir_loop_fp_enabled()) {
    fp_count = ir_loop_fp_snapshot(function, fp_entries);
  }

  if (!ir_run_fixpoint_stage(function, &g_ir_fixpoint_stage)) {
    ir_loop_fp_destroy(fp_entries, fp_count);
    return 0;
  }
  ir_library_passes_at(function, "fixpoint");

  if (!ir_run_post_fixpoint_stages(function)) {
    ir_loop_fp_destroy(fp_entries, fp_count);
    return 0;
  }

  if (fp_count > 0) {
    ir_loop_fp_report(function, fp_entries, fp_count);
  }
  ir_loop_fp_destroy(fp_entries, fp_count);

  double t0 = ir_pass_time_begin();
  if (!ir_verify_simd_contracts(function)) {
    return 0;
  }
  ir_pass_time_end("verify_simd_contracts [stage]", t0);

  ir_explain_function_after(function);

  mettle_compiler_ctx_set_pass_name("lowering cleanup");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_lowering_cleanup,
          IR_ARRAY_COUNT(g_ir_lowering_cleanup), 1, "lowering cleanup",
          "IR optimization pass failed", 0)) {
    return 0;
  }
  ir_library_passes_at(function, "cleanup");

  if (ir_ssa_enabled()) {
    mettle_compiler_ctx_set_pass_name("enter ssa");
    if (!ir_run_named_stage_fixpoint(function, g_ir_ssa_enter,
                                     IR_ARRAY_COUNT(g_ir_ssa_enter), 1,
                                     "enter ssa", "IR optimization pass failed",
                                     0)) {
      return 0;
    }
    mettle_compiler_ctx_set_pass_name("ssa optimize");
    if (!ir_run_named_stage_fixpoint(function, g_ir_ssa_optimize,
                                     IR_ARRAY_COUNT(g_ir_ssa_optimize), 4,
                                     "ssa optimize",
                                     "IR optimization pass failed", 0)) {
      return 0;
    }
    mettle_compiler_ctx_set_pass_name("leave ssa");
    if (!ir_run_named_stage_fixpoint(function, g_ir_ssa_leave,
                                     IR_ARRAY_COUNT(g_ir_ssa_leave), 1,
                                     "leave ssa", "IR optimization pass failed",
                                     0)) {
      return 0;
    }
    ir_library_passes_at(function, "ssa");
  }
  ir_library_passes_at(function, "end");

  t0 = ir_pass_time_begin();
  int ok = ir_function_rebuild_cfg(function);
  ir_pass_time_end("rebuild_cfg [stage]", t0);
  return ok;
}

int ir_optimize_function_revectorize(IRFunction *function) {
  if (!function) {
    return 0;
  }
  if (!ir_run_fixpoint_stage(function, &g_ir_fixpoint_stage)) {
    return 0;
  }
  return ir_run_post_fixpoint_stages(function);
}

static void ir_set_current_function_context(IRFunction *function) {
  if (function) {
    mettle_compiler_ctx_set_function_name(
        function->name ? function->name : "<anonymous>");
  }
}

static int ir_run_program_stage_for_each_function(
    IRProgram *program, int (*run)(IRFunction *function)) {
  int report = ir_time_functions_enabled();
  double slowest = 0.0;
  double total = 0.0;
  double tiny_ms = 0.0;
  size_t tiny_count = 0;
  const char *slowest_name = NULL;

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    double began = report ? mettle_now_ms() : 0.0;
    if (function && function->rewrite_role) {
      continue;
    }
    ir_set_current_function_context(function);
    if (!run(function)) {
      return 0;
    }
    if (report) {
      double took = mettle_now_ms() - began;
      total += took;
      if (function->instruction_count < 64) {
        tiny_ms += took;
        tiny_count++;
      }
      if (took > slowest) {
        slowest = took;
        slowest_name = function->name;
      }
    }
  }
  if (report) {
    fprintf(stderr,
            "-- stage over %zu functions: %.1f ms total, slowest %.1f ms (%s); "
            "%zu under 64 instructions cost %.1f ms --\n",
            program->function_count, total, slowest,
            slowest_name ? slowest_name : "?", tiny_count, tiny_ms);
  }
  return 1;
}

static void ir_explain_device_pipeline_end(const IRProgram *program,
                                           const IRGpuCallGraph *graph,
                                           int gpu_only, int ok) {
  if (!gpu_only) {
    ir_explain_set_device_functions(NULL, NULL);
    return;
  }
  for (size_t i = 0; ok && graph->reachable && i < program->function_count;
       i++) {
    if (!graph->reachable[i]) continue;
    ir_tensor_explain_declines(program->functions[i]);
    ir_gpu_staging_explain_declines(program->functions[i]);
  }
  ir_inline_explain_report_remaining((IRProgram *)program);
  ir_explain_set_device_functions(program, graph->reachable);
}

static int ir_optimize_portable_program_pipeline(
    IRProgram *program, const IROptimizeOptions *options) {
  IRGpuCallGraph graph = {0};
  char *graph_error = NULL;
  int gpu_only = options && options->gpu_device_only;
  int ok = 1;

  ir_optimize_reset_user_error();
  ir_optimize_set_simd_report(0);
  ir_optimize_set_explain(options && options->explain,
                          options ? options->explain_focus_file : NULL);
  ir_function_index_reset();
  ir_verify_begin_program(program);

  if (gpu_only &&
      !ir_program_build_gpu_call_graph(program, &graph, &graph_error)) {
    fprintf(stderr, "GPU optimization eligibility failed: %s\n",
            graph_error ? graph_error : "invalid device module");
    free(graph_error);
    ir_optimize_note_user_error();
    ir_verify_end_program();
    ir_function_index_reset();
    return 0;
  }

  ir_explain_set_program(program);

  if (gpu_only && (!options || !options->preserve_function_boundaries) &&
      !ir_pass_name_is_skipped("inline_small_functions")) {
    int inlining_changed = 0;
    mettle_compiler_ctx_set_pass_name("inline_small_functions");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_inline_small_functions_pass(program, &inlining_changed)) {
      mettle_compiler_ice("IR optimization inlining pass failed");
    }
    ir_pass_time_end("inline_small_functions [program]", t0);

    if (inlining_changed) {
      for (size_t i = 0; i < program->function_count; i++) {
        if (program->functions[i] &&
            !ir_function_rebuild_cfg(program->functions[i])) {
          ok = 0;
        }
      }
      ir_gpu_call_graph_destroy(&graph);
      memset(&graph, 0, sizeof(graph));
      graph_error = NULL;
      if (ok &&
          !ir_program_build_gpu_call_graph(program, &graph, &graph_error)) {
        fprintf(stderr, "GPU optimization eligibility failed: %s\n",
                graph_error ? graph_error : "invalid device module");
        free(graph_error);
        ir_optimize_note_user_error();
        ok = 0;
      }
    }
  }

  if (ok && gpu_only && !ir_pass_name_is_skipped("hoist_pure_calls")) {
    int pure_licm_changed = 0;
    mettle_compiler_ctx_set_pass_name("hoist_pure_calls");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_hoist_pure_calls_pass(program, &pure_licm_changed)) {
      mettle_compiler_ice("IR optimization pure-call hoisting pass failed");
    }
    ir_pass_time_end("hoist_pure_calls [program]", t0);
  }

  for (size_t i = 0; ok && i < program->function_count; i++) {
    if (gpu_only && (!graph.reachable || !graph.reachable[i])) continue;
    IRFunction *function = program->functions[i];
    ir_set_current_function_context(function);
    if (!ir_run_fixpoint_stage(function, &g_ir_portable_fixpoint_stage) ||
        !ir_function_rebuild_cfg(function)) {
      ok = 0;
      break;
    }
  }

  if (ok && gpu_only && (!options || !options->preserve_function_boundaries) &&
      !ir_inline_enforce_contracts(program)) {
    ir_optimize_note_user_error();
    ok = 0;
  }

  ir_explain_device_pipeline_end(program, &graph, gpu_only, ok);

  ir_explain_set_program(NULL);
  ir_gpu_call_graph_destroy(&graph);
  ir_explain_attribute_remarks(program);
  ir_explain_ownership_scan(program);
  ir_explain_flush();
  ir_pass_time_report();
  ir_verify_end_program();
  ir_function_index_reset();
  return ok;
}

static int ir_safety_analysis_function(IRFunction *function) {
  static const IROptNamedPass passes[] = {
      {"safety_narrowing", ir_drop_dead_narrowing_pass, {0, 0}},
      {"safety_copy_constants", ir_copy_and_constant_propagation_pass, {0, 0}},
      {"safety_coalesce", ir_coalesce_single_use_temp_assign_pass, {0, 0}},
      {"safety_branches", ir_constant_and_branch_simplify_pass, {0, 0}},
      {"safety_unreachable", ir_eliminate_unreachable_straightline_pass, {0, 0}},
      {"safety_redundant_jumps", ir_remove_redundant_jumps_pass, {0, 0}},
      {"safety_unused_labels", ir_remove_unused_labels_pass, {0, 0}},
      {"safety_dead_temps", ir_eliminate_dead_temp_writes_pass, {0, 0}},
  };
  if (function->is_rule) {
    return 1;
  }
  return ir_run_named_stage_fixpoint(function, passes, IR_ARRAY_COUNT(passes),
      IR_OPT_FIXPOINT_MAX_ITERATIONS, "safety scalar analysis",
      "Safety scalar analysis failed", 0);
}

int ir_optimize_safety_analysis(IRProgram *program, int preserve_boundaries) {
  if (!program) return 0;
  ir_optimize_set_program(program);
  ir_function_index_reset();
  ir_verify_begin_program(program);
  int ok = ir_run_program_stage_for_each_function(program,
                                                  ir_safety_analysis_function);
  if (ok && !preserve_boundaries &&
      !ir_pass_name_is_skipped("inline_small_functions")) {
    int changed = 0;
    ok = ir_inline_small_functions_pass(program, &changed);
  }
  if (ok) ok = ir_safety_elide_inlined_frames(program);
  if (ok) ok = ir_run_program_stage_for_each_function(program,
                                                      ir_safety_analysis_function);
  ir_verify_end_program();
  ir_function_index_reset();
  return ok;
}

typedef struct {
  const char *name;
  const char *timing_label;
  int (*run)(IRProgram *program, int *changed);
  const char *ice_message;
  int needs_boundaries_free;
  int needs_whole_program;
  int unskippable;
} IRProgramPass;

static const IRProgramPass kProgramPasses[] = {
    {"hoist_pure_calls", "hoist_pure_calls_pre_inline [program]",
     ir_hoist_pure_calls_pass,
     "IR optimization pure-call hoisting pass failed", 0, 0, 0},
    {"tail_recursion_elim", "tail_recursion_elim [program]",
     ir_tail_recursion_elimination_pass,
     "IR tail-recursion elimination pass failed", 1, 0, 0},
    {"inline_small_functions", "inline_small_functions [program]",
     ir_inline_small_functions_pass, "IR optimization inlining pass failed", 1,
     0, 0},
    {"inline_self_recursion", "inline_self_recursion [program]",
     ir_inline_self_recursion_pass,
     "IR optimization self-recursion inlining failed", 1, 0, 0},
    {"hoist_pure_calls", "hoist_pure_calls [program]",
     ir_hoist_pure_calls_pass,
     "IR optimization pure-call hoisting pass failed", 0, 0, 0},
};

static const IRProgramPass kParallelizePass = {
    "parallelize_marked_loops", "parallelize_marked_loops [program]",
    ir_parallelize_marked_loops_pass, "IR loop parallelization pass failed", 0,
    0, 1};

static void ir_run_program_pass(IRProgram *program,
                                const IROptimizeOptions *options,
                                const IRProgramPass *pass) {
  int changed = 0;
  double started;

  if (pass->needs_boundaries_free && options &&
      options->preserve_function_boundaries) {
    return;
  }
  if (pass->needs_whole_program && (!options || !options->whole_program)) {
    return;
  }
  if ((!pass->unskippable || ir_optimize_scalar_target()) &&
      ir_pass_name_is_skipped(pass->name)) {
    return;
  }
  mettle_compiler_ctx_set_pass_name(pass->name);
  mettle_compiler_ctx_set_fixpoint_iteration(0);
  started = ir_pass_time_begin();
  if (!pass->run(program, &changed)) {
    mettle_compiler_ice(pass->ice_message);
  }
  ir_pass_time_end(pass->timing_label, started);
}

static int ir_named_list_holds(const IROptNamedPass *passes, size_t count,
                               const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(passes[i].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_optimizer_point_known(const char *point) {
  static const char *const STAGES[] = {"pre_inline",  "fixpoint", "loop_canonical",
                                       "recognizers", "cleanup",  "ssa",
                                       "end"};
  for (size_t i = 0; i < IR_ARRAY_COUNT(STAGES); i++) {
    if (strcmp(point, STAGES[i]) == 0) {
      return 1;
    }
  }
  if (strncmp(point, "after_", 6) != 0) {
    return 0;
  }
  const char *name = point + 6;
  for (int i = 0; i < IR_OPT_PASS_COUNT; i++) {
    if (strcmp(ir_opt_pass_name((IROptPassId)i), name) == 0) {
      return 1;
    }
  }
#define IR_POINT_LIST(list)                                                    \
  ir_named_list_holds(list, IR_ARRAY_COUNT(list), name)
  return IR_POINT_LIST(g_ir_pre_inline_canonical) ||
         IR_POINT_LIST(g_ir_pre_inline_recognizers) ||
         IR_POINT_LIST(g_ir_loop_canonical_passes) ||
         IR_POINT_LIST(g_ir_recognizer_passes) ||
         IR_POINT_LIST(g_ir_post_recognizer_tail) ||
         IR_POINT_LIST(g_ir_ssa_enter) || IR_POINT_LIST(g_ir_ssa_optimize) ||
         IR_POINT_LIST(g_ir_ssa_leave) || IR_POINT_LIST(g_ir_lowering_cleanup);
#undef IR_POINT_LIST
}

int ir_optimize_program_pipeline(IRProgram *program,
                                 const IROptimizeOptions *options) {
  if (!program) {
    return 0;
  }
  ir_optimize_set_program(program);
  ir_devirtualize_set_program(program);
  ir_optimize_set_scalar_target(options && options->scalar_target);
  if (options && options->target_neutral_only) {
    return ir_optimize_portable_program_pipeline(program, options);
  }

  ir_optimize_reset_user_error();
  ir_optimize_set_simd_report(options && options->simd_report);
  ir_optimize_set_explain(options && options->explain,
                          options ? options->explain_focus_file : NULL);
  ir_function_index_reset();
  ir_verify_begin_program(program);
  ir_program_register_scalar_pointer_types(program);

  if (!ir_user_rewrite_begin(program)) {
    ir_verify_end_program();
    ir_function_index_reset();
    return 0;
  }

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
    if (getenv("METTLE_DUMP_PRE_INLINE")) {
      ir_program_dump(program, stderr);
    }
  }

  for (size_t i = 0; i < sizeof(kProgramPasses) / sizeof(kProgramPasses[0]);
       i++) {
    ir_run_program_pass(program, options, &kProgramPasses[i]);
  }

  {
    ir_run_program_pass(program, options, &kParallelizePass);
    if (ir_optimize_had_user_error()) {
      ir_explain_flush();
      ir_verify_end_program();
      ir_function_index_reset();
      return 0;
    }
  }

  if (!ir_pass_name_is_skipped("alias_facts")) {
    double t0 = ir_pass_time_begin();
    ir_alias_facts_build(program);
    ir_pass_time_end("alias_facts [program]", t0);
  }

  ir_explain_set_program(program);
  if (!ir_run_program_stage_for_each_function(
          program, ir_optimize_function_pipeline)) {
    ir_explain_set_program(NULL);
    ir_alias_facts_reset();
    if (!ir_optimize_had_user_error()) {
      mettle_compiler_ice_report("IR optimization failed", NULL);
    }
    ir_function_index_reset();
    return 0;
  }
  ir_explain_set_program(NULL);
  ir_alias_facts_reset();

  int contracts_ok = 1;
  if (!options || !options->preserve_function_boundaries) {
    contracts_ok &= ir_inline_enforce_contracts(program);
  }
  contracts_ok &= ir_enforce_noalloc_contracts(program);
  contracts_ok &= ir_user_rewrite_end(program);
  contracts_ok &= !ir_optimize_had_user_error();

  ir_inline_explain_report_remaining(program);
  ir_explain_attribute_remarks(program);
  ir_explain_ownership_scan(program);
  ir_explain_flush();
  if (!contracts_ok) {
    ir_explain_finalize(1);
  }
  ir_pass_time_report();
  ir_analysis_report_stats();
  ir_ssa_opt_report_stats();
  ir_verify_end_program();

  ir_function_index_reset();
  return contracts_ok;
}
