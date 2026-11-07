#ifndef IR_OPTIMIZE_INTERNAL_H
#define IR_OPTIMIZE_INTERNAL_H

#include "../ir_optimize.h"
#include "../ir_safety.h"
#include "../ir_profile.h"
#include "../../common.h"
#include "../../compiler/compiler_context.h"
#include "../../compiler/compiler_crash.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_INLINE_MAX_NON_NOP_INSTRUCTIONS 128
#define IR_INLINE_MAX_PARAMETERS 16
#define IR_INLINE_MAX_ROUNDS 4
#define IR_INLINE_MAX_CALLER_NON_NOP_INSTRUCTIONS 512
#define IR_INLINE_TINY_LEAF_NON_NOP_INSTRUCTIONS 16
#define IR_INLINE_LOOP_BODY_INSTRUCTIONS 80
#define IR_SELF_INLINE_MAX_DEPTH 3
#define IR_SELF_INLINE_MAX_SELF_CALLS 4
#define IR_SELF_INLINE_MAX_BODY_INSTRUCTIONS 320
#define IR_UNROLL_MAX_TRIP_COUNT 64
#define IR_UNROLL_COLD_MAX_TRIP_COUNT 16
#define IR_UNROLL_HOT_MAX_TRIP_COUNT 128

typedef struct {
  char *name;
  IROperand value;
} IRTempValueEntry;

typedef struct IRTempValueMap {
  IRTempValueEntry *items;
  size_t count;
  size_t capacity;
  unsigned int *ix;
  size_t ix_capacity;
  size_t ix_tombstones;
  struct IRTempValueMap *vsym_counts;
} IRTempValueMap;

typedef IRTempValueMap IRSymbolValueMap;

typedef struct {
  char *label;
  IRTempValueMap in_map;
  int initialized;
} IRLabelValueEntry;

typedef struct {
  IRLabelValueEntry *items;
  size_t count;
  size_t capacity;
  IRTempValueMap index;
} IRLabelValueMap;

typedef struct {
  char *name;
  size_t use_count;
} IRTempUseEntry;

typedef struct {
  IRTempUseEntry *items;
  size_t count;
  size_t capacity;
  size_t *hash;
  size_t hash_count;
} IRTempUseMap;

typedef struct {
  char *from;
  char *to;
} IRNameMapEntry;

typedef struct {
  IRNameMapEntry *items;
  size_t count;
  size_t capacity;
  size_t *buckets;
  size_t bucket_count;
} IRNameMap;

typedef struct {
  IRInstruction *items;
  size_t count;
  size_t capacity;
} IRInstructionVector;

typedef struct {
  size_t *items;
  size_t count;
  size_t capacity;
} IRIndexVector;

typedef enum {
  IR_EXPR_BINARY,
  IR_EXPR_UNARY,
  IR_EXPR_CAST,
  IR_EXPR_ADDRESS_OF
} IRExpressionKind;

typedef struct {
  IRExpressionKind kind;
  char *op_text;
  IROperand lhs;
  IROperand rhs;
  int is_float;
  IROperand value;
  int alive;
} IRExpressionEntry;

typedef struct {
  size_t name_hash;
  size_t entry_index;
  size_t next;
} IRExprNameOcc;

typedef struct {
  IRExpressionEntry *items;
  size_t count;
  size_t capacity;
  size_t *name_hashes;
  size_t name_hash_capacity;
  size_t name_hash_count;
  size_t *expr_slots;
  size_t expr_capacity;
  int expr_valid;
  size_t dead_count;
  IRExprNameOcc *occ;
  size_t occ_count;
  size_t occ_capacity;
  size_t *occ_heads;
  size_t occ_head_capacity;
  size_t *sym_entries;
  size_t sym_count;
  size_t sym_capacity;
} IRExpressionMap;

typedef struct {
  const char *name;
  IRFunction *function;
} IRFunctionIndexSlot;

typedef struct {
  IRFunctionIndexSlot *slots;
  size_t slot_count;
  const IRProgram *program;
  size_t function_count;
} IRFunctionIndex;

typedef struct {
  int has_label;
  int has_while_label;
  int has_jump;
  int has_branch_zero;
  int has_branch_eq;
  int has_call;
  int has_load;
  int has_assign;
  int has_temp_write;
  int has_binary;
  int has_div;
} IROptFunctionFeatures;

typedef struct {
  size_t compare_index;
  size_t branch_index;
  size_t jump_index;
  const char *loop_label;
  const char *exit_label;
} IRWhileLoopBounds;

typedef struct {
  IRWhileLoopBounds bounds;
  const IRFunction *function;
  size_t header_index;
  const char *iv;
  IROperand bound;
  size_t body_start;
  size_t body_end;
  long long step;
  size_t step_index;
  signed char c_unit_step;
  signed char c_starts_at_zero;
  signed char c_straight_line;
  signed char c_bound_invariant;
  signed char c_unclaimable;
} IRAffineLoop;

int ir_affine_model_loop(const IRFunction *function, size_t header_index,
                         IRAffineLoop *out);
int ir_affine_unit_step(IRAffineLoop *loop);
int ir_affine_starts_at_zero(IRAffineLoop *loop);
int ir_affine_straight_line_body(IRAffineLoop *loop);
int ir_affine_bound_invariant(IRAffineLoop *loop);
int ir_affine_body_unclaimable(IRAffineLoop *loop);
int ir_affine_index_decompose(const IRFunction *function, size_t before,
                              const IROperand *index, const char **name_out,
                              long long *coeff_out, long long *addend_out);
int ir_affine_symbol_written_in(const IRFunction *function, size_t start,
                                size_t end, const char *symbol);
int ir_verify_loop_canonical_form(const IRFunction *function, char *detail,
                                  size_t detail_size);

#define IR_PTR_BIND_MAX 4

typedef struct {
  const char *base;
  char *ptr_p;
  char *addr_temps[8];
  size_t addr_temp_count;
} IRPtrBaseBinding;

typedef struct {
  size_t header_index;
  size_t end_index;
  const char *counter;
  const char *dst;
  const char *src;
  const char *key;
  const char *val;
  const char *cmp_op;
  long long stride;
  int elem_size;
} IRShiftLoopMatch;

typedef struct {
  const char *src_base;
  const char *dst_base;
  IROperand src_scale;
  IROperand dst_scale;
  IROperand bias;
  int has_src_scale;
  int has_dst_scale;
  int has_bias;
  int width_bits;
} IRAffineMapTerms;

#define IR_SROA_MAX_SLOTS 16
#define IR_SROA_MAX_GROUP 8

typedef struct {
  long long offset;
  int size;
  int is_float;
  int float_bits;
  int is_unsigned;
  int alias_class;
  char *name;
} IRSroaSlot;

typedef struct {
  const char *temp;
  long long offset;
  int valid;
} IRSroaAddr;

typedef struct {
  const char *name;
  size_t decl_index;
} IRSroaMember;

typedef struct {
  const char *name;
  size_t decl_index;
  IRSroaAddr addrs[IR_SROA_MAX_SLOTS * 2];
  size_t addr_count;
} IRSroaMemberCtx;

typedef struct {
  const char **names;
  size_t *values;
  size_t capacity;
  size_t count;
} IRNameIndex;

typedef struct {
  long long lo;
  long long hi;
} IRIntRange;

typedef struct {
  const IRFunction *function;
  IRTempValueMap decl_types;
  IRTempValueMap addr_taken;
  IRTempValueMap monotone;
  IRTempValueMap counter_lo;
  IRTempValueMap counter_hi;
  IRTempValueMap label_guard;
  IRTempValueMap unique_def;
  IRTempValueMap label_refs;
  IRNameIndex symbol_groups;
  char **symbol_names;
  size_t symbol_name_count;
  size_t *symbol_sites;
  size_t *symbol_group_start;
  size_t symbol_sites_insns;
  int symbol_sites_built;
  int built;
  int ok;
} IRValueRangeCtx;

#define IR_ARRAY_COUNT(items) (sizeof(items) / sizeof((items)[0]))

#define IR_OPT_PASS_LIST(X)                                                   \
  X(REDUCTION_UNROLL, "reduction_unroll")                                    \
  X(DROP_DEAD_NARROWING, "drop_dead_narrowing")                               \
  X(COPY_AND_CONSTANT_PROPAGATION, "copy_and_constant_propagation")           \
  X(FUSE_TENSOR_MMA_CHAINS, "fuse_tensor_mma_chains")                        \
  X(PROMOTE_GPU_ASYNC_STAGING, "promote_gpu_async_staging")                  \
  X(FUSE_ROTATE_ADD, "fuse_rotate_add")                                      \
  X(STRENGTH_REDUCE_ROTATE_LOOPS, "strength_reduce_rotate_loops")            \
  X(UNROLL_SMALL_CONST_BOUND_LOOPS, "unroll_small_const_bound_loops")         \
  X(UNROLL_ANNOTATED_LOOPS, "unroll_annotated_loops")                         \
  X(FOLD_POPCOUNT_BYTE_LOOP, "fold_popcount_byte_loop")                      \
  X(FUSE_POPCOUNT_BUFFER_LOOP, "fuse_popcount_buffer_loop")                  \
  X(FOLD_KERNIGHAN_POPCOUNT, "fold_kernighan_popcount")                      \
  X(COLLATZ_ODD_STEP_FOLD, "collatz_odd_step_fold")                          \
  X(COALESCE_SINGLE_USE_TEMP_ASSIGN, "coalesce_single_use_temp_assign")      \
  X(ELIMINATE_SINGLE_USE_FLOAT_SYMBOL_COPIES,                                 \
    "eliminate_single_use_float_symbol_copies")                              \
  X(COMMON_SUBEXPRESSION_ELIMINATION, "common_subexpression_elimination")    \
  X(CONSTANT_AND_BRANCH_SIMPLIFY, "constant_and_branch_simplify")            \
  X(REASSOCIATE_CONSTANTS, "reassociate_constants")                          \
  X(COUNT_WORD_STARTS, "count_word_starts")                                  \
  X(ELIMINATE_DEAD_TEMP_WRITES, "eliminate_dead_temp_writes")                \
  X(THREAD_JUMP_TARGETS, "thread_jump_targets")                              \
  X(NULL_CHECK_LICM, "null_check_licm")                                      \
  X(REMOVE_EMPTY_CONDITIONAL_DIAMONDS, "remove_empty_conditional_diamonds")  \
  X(REMOVE_REDUNDANT_FALLTHROUGH_BRANCHES,                                    \
    "remove_redundant_fallthrough_branches")                                 \
  X(REMOVE_REDUNDANT_JUMPS, "remove_redundant_jumps")                        \
  X(ELIMINATE_UNREACHABLE_STRAIGHTLINE,                                       \
    "eliminate_unreachable_straightline")                                    \
  X(ELIMINATE_UNREACHABLE_BLOCKS, "eliminate_unreachable_blocks")            \
  X(REMOVE_UNUSED_LABELS, "remove_unused_labels")                            \
  X(MEMCPY_INLINE, "memcpy_inline")                                          \
  X(MEMCMP_BYTE_LOOP, "memcmp_byte_loop")                                    \
  X(ELIMINATE_LOAD_SYMBOL_COPY, "eliminate_load_symbol_copy")                \
  X(SIMD_SUM_I32, "simd_sum_i32")                                            \
  X(SIMD_SUM_U8, "simd_sum_u8")                                              \
  X(SIMD_BYTE_MAP, "simd_byte_map")                                          \
  X(SIMD_DOT_I32, "simd_dot_i32")                                            \
  X(SIMD_DOT_I8, "simd_dot_i8")                                              \
  X(SIMD_SLP_MAC_I32, "simd_slp_mac_i32")                                    \
  X(SIMD_SLP_MAC_I8, "simd_slp_mac_i8")                                      \
  X(SIMD_INSERTION_SORT_I32, "simd_insertion_sort_i32")                      \
  X(SROA, "sroa")                                                             \
  X(EGRAPH_SIMPLIFY, "egraph_simplify")                                       \
  X(USER_REWRITE, "user_rewrite")                                             \
  X(PROMOTE_SCALAR_LOCALS, "promote_scalar_locals")                           \
  X(LEAVE_SSA, "leave_ssa")

typedef enum {
#define IR_OPT_PASS_ENUM(id, name) IR_OPT_PASS_##id,
  IR_OPT_PASS_LIST(IR_OPT_PASS_ENUM)
#undef IR_OPT_PASS_ENUM
  IR_OPT_PASS_COUNT
} IROptPassId;

typedef int (*IROptFunctionPass)(IRFunction *function, int *changed);

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

#define IR_OPT_FEATURE_ALL                                                    \
  (IR_OPT_FEATURE_LABEL | IR_OPT_FEATURE_WHILE_LABEL | IR_OPT_FEATURE_JUMP |  \
   IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_BRANCH_EQ |                    \
   IR_OPT_FEATURE_CALL | IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_ASSIGN |        \
   IR_OPT_FEATURE_TEMP_WRITE | IR_OPT_FEATURE_BINARY | IR_OPT_FEATURE_DIV)

typedef struct {
  unsigned all;
  unsigned any;
} IROptPassGate;

typedef struct {
  const char *name;
  IROptFunctionPass run;
  IROptPassGate gate;
} IROptNamedPass;

extern const char *g_ir_pass_names[IR_OPT_PASS_COUNT];
const char *ir_opt_pass_name(IROptPassId pass_id);

int ir_binary_is_unit_increment_of_iv(const IRInstruction *instruction,
                                             const char *iv_symbol);
int ir_symbol_int_map_step(const IRInstruction *instruction,
                           IRSymbolValueMap *symbol_map);
int ir_build_symbol_int_map_before(const IRFunction *function,
                                          size_t before_index,
                                          IRSymbolValueMap *symbol_map);
int ir_clone_instruction_plain(const IRInstruction *source,
                                      IRInstruction *out);
int ir_coalesce_single_use_temp_assign_pass(IRFunction *function,
                                                   int *changed);
int ir_collatz_odd_step_fold_pass(IRFunction *function, int *changed);
void ir_collect_function_features(const IRFunction *function,
                                         IROptFunctionFeatures *features);
int ir_collect_instruction_temp_uses(IRTempUseMap *uses,
                                            const IRInstruction *instruction);
int ir_common_subexpression_elimination_pass(IRFunction *function,
                                                    int *changed);
int ir_constant_and_branch_simplify_pass(IRFunction *function,
                                                int *changed);
int ir_copy_and_constant_propagation_pass(IRFunction *function,
                                                 int *changed);
int ir_count_word_starts_pass(IRFunction *function, int *changed);
int ir_detect_shift_loops_pass(IRFunction *function, int *changed);
int ir_eliminate_congruent_ivs_pass(IRFunction *function, int *changed);
int ir_eliminate_dead_temp_writes_pass(IRFunction *function,
                                              int *changed);
int ir_eliminate_load_symbol_copy_pass(IRFunction *function,
                                              int *changed);
int ir_eliminate_single_use_float_symbol_copies_pass(IRFunction *function,
                                                            int *changed);
int ir_eliminate_unreachable_blocks_pass(IRFunction *function,
                                                int *changed);
int ir_eliminate_unreachable_straightline_pass(IRFunction *function,
                                                       int *changed);
int ir_hoist_body_locals_pass(IRFunction *function, int *changed);
int ir_drop_dead_narrowing_pass(IRFunction *function, int *changed);
int ir_hoist_global_bases_pass(IRFunction *function, int *changed);
int ir_hoist_load_bases_pass(IRFunction *function, int *changed);
int ir_hoist_column_bases_pass(IRFunction *function, int *changed);
int ir_devirtualize_known_tables_pass(IRFunction *function, int *changed);
void ir_devirtualize_set_program(IRProgram *program);
int ir_hoist_descriptor_loads_pass(IRFunction *function, int *changed);
int ir_hoist_invariant_assigns_pass(IRFunction *function, int *changed);
int ir_hoist_row_pointers_pass(IRFunction *function, int *changed);
int ir_merge_row_pointers_pass(IRFunction *function, int *changed);
int ir_hoist_invariant_arith_pass(IRFunction *function, int *changed);
int ir_if_convert_accumulate_pass(IRFunction *function, int *changed);
int ir_normalize_scan_from_first_pass(IRFunction *function, int *changed);
int ir_redundancy_elimination_pass(IRFunction *function, int *changed);
int ir_promote_scalar_locals_pass(IRFunction *function, int *changed);
int ir_leave_ssa_pass(IRFunction *function, int *changed);
int ir_ssa_enabled(void);
int ir_ssa_propagate_pass(IRFunction *function, int *changed);
void ir_ssa_opt_report_stats(void);
int ir_select_adjacent_field_pass(IRFunction *function, int *changed);

void ir_library_passes_at(IRFunction *function, const char *point);
int ir_ascii_casefold_range_pass(IRFunction *function, int *changed);
int ir_fold_range_test_pass(IRFunction *function, int *changed);
int ir_float_divide_by_power_of_two_pass(IRFunction *function, int *changed);
int ir_widen_byte_pack_pass(IRFunction *function, int *changed);
int ir_widen_subword_load_cast_pass(IRFunction *function, int *changed);
int ir_hoist_invariant_loads_pass(IRFunction *function, int *changed);
int ir_hoist_held_column_bases_pass(IRFunction *function, int *changed);
void ir_rename_temp_reads(IRFunction *function, const char *temp,
                          const char *other);
int ir_promote_loop_memory_pass(IRFunction *function, int *changed);
int ir_forward_stored_values_pass(IRFunction *function, int *changed);
size_t ir_inline_cleaned_instruction_count(const IRFunction *function);
int ir_demote_scalar_addresses_pass(IRFunction *function, int *changed);
int ir_merge_adjacent_const_stores_pass(IRFunction *function, int *changed);
int ir_unify_param_copy_spelling_pass(IRFunction *function, int *changed);
int ir_find_label_index(const IRFunction *function, const char *label,
                               size_t *out_index);
int ir_find_last_writer_before(const IRFunction *function, size_t before_index,
                                      IROperandKind kind, const char *name,
                                      size_t *writer_index);

typedef struct {
  IRNameIndex symbol_defs;
  IRNameIndex symbol_first_def;
  IRNameIndex temp_defs;
  IRNameIndex temp_first_def;
  size_t instruction_count;
  int valid;
} IRFacts;

int ir_facts_build(IRFacts *facts, const IRFunction *function);
void ir_facts_destroy(IRFacts *facts);
size_t ir_facts_symbol_def_count(const IRFacts *facts, const char *name);
size_t ir_facts_temp_def_count(const IRFacts *facts, const char *name);
int ir_facts_symbol_single_def(const IRFacts *facts, const char *name,
                               size_t *at);
int ir_facts_temp_single_def(const IRFacts *facts, const char *name,
                             size_t *at);
int ir_facts_matches(const IRFacts *facts, const IRFunction *function);
const IRFacts *ir_facts_of(const IRFunction *function);
void ir_facts_invalidate(void);
void ir_facts_release(void);

int ir_name_index_init(IRNameIndex *index, size_t expected);
void ir_name_index_insert(IRNameIndex *index, const char *name, size_t value);
size_t ir_pass_loop_latch(const IRFunction *function, size_t after,
                          const char *label);
int ir_name_index_find(const IRNameIndex *index, const char *name,
                       size_t *out_value);
void ir_name_index_add(IRNameIndex *index, const char *name, size_t delta);
void ir_name_index_sub(IRNameIndex *index, const char *name, size_t delta);
void ir_name_index_destroy(IRNameIndex *index);

int ir_find_next_non_nop(const IRFunction *function, size_t start_index,
                                size_t *out_index);
int ir_find_next_non_nop_in_block(const IRFunction *function,
                                         size_t start_index, size_t *out_index);

const char *ir_find_ptr_init_base(const IRFunction *function, size_t before,
                                         const char *ptr_symbol);
int ir_find_ptr_loop_len_operand(const IRFunction *function,
                                        size_t header_index,
                                        const char *end_ptr, const char *base,
                                        IROperand *out_len);

const char *ir_find_ptr_step_with_suffix(const IRFunction *function,
                                                size_t start, size_t end,
                                                long long step,
                                                const char *suffix);

const IRInstruction *ir_find_temp_producer_before(const IRFunction *function,
                                                  size_t before_index,
                                                  const char *temp_name);
int ir_find_while_loop_bounds(const IRFunction *function, size_t header_index,
                                     IRWhileLoopBounds *out);
int ir_fold_popcount_byte_loop_pass(IRFunction *function, int *changed);
int ir_fold_kernighan_popcount_pass(IRFunction *function, int *changed);
int ir_drop_redundant_int_casts_pass(IRFunction *function, int *changed);
int ir_guard_loop_and_hoist_load_pass(IRFunction *function, int *changed);
void ir_function_index_reset(void);

const char *ir_function_local_declared_type(const IRFunction *function,
                                                   const char *symbol_name);
int ir_function_replace_instructions(IRFunction *function,
                                            IRInstructionVector *vector);
int ir_function_symbol_is_inlined_param(const IRFunction *function,
                                               const char *symbol_name,
                                               const char *expected_type,
                                               const char *param_tag);
int ir_function_symbol_is_parameter(const IRFunction *function,
                                           const char *symbol_name);
int ir_fuse_popcount_buffer_loop_pass(IRFunction *function, int *changed);
int ir_fuse_tensor_mma_chains_pass(IRFunction *function, int *changed);
int ir_promote_gpu_async_staging_pass(IRFunction *function, int *changed);
int ir_fuse_rotate_add_pass(IRFunction *function, int *changed);
int ir_fuse_while_loop_to_insn(IRFunction *function, size_t header_index,
                                      size_t jump_index, IRInstruction *fused,
                                      int *changed);
int ir_index_vector_append(IRIndexVector *vector, size_t value);
void ir_index_vector_destroy(IRIndexVector *vector);
int ir_inline_small_functions_pass(IRProgram *program, int *changed);
int ir_inline_self_recursion_pass(IRProgram *program, int *changed);
int ir_tail_recursion_elimination_pass(IRProgram *program, int *changed);
struct IRGlobalIntConst;
int ir_fold_readonly_globals_pass(IRProgram *program,
                                  const struct IRGlobalIntConst *consts,
                                  size_t count, int *changed);
IRFunction *ir_program_find_function(IRProgram *program, const char *name);
int ir_hoist_pure_calls_pass(IRProgram *program, int *changed);

void ir_optimize_set_program(IRProgram *program);
IRProgram *ir_optimize_get_program(void);
void ir_alias_facts_build(IRProgram *program);
void ir_alias_facts_reset(void);
int ir_alias_bases_distinct(const IRFunction *function, const char *base_a,
                            const char *base_b);
int ir_alias_classes_distinct(unsigned a, unsigned b);
void ir_instruction_clear_arguments(IRInstruction *instruction);
void ir_instruction_destroy_storage(IRInstruction *instruction);
int ir_parallelize_marked_loops_pass(IRProgram *program, int *changed);
int ir_instruction_has_side_effect(const IRInstruction *instruction);
int ir_instruction_insert_move(IRFunction *function, size_t index,
                                      IRInstruction *instruction);
int ir_instruction_is_trivially_dead_if_dest_unused(
    const IRInstruction *instruction);
void ir_instruction_make_jump(IRInstruction *instruction);
void ir_instruction_make_nop(IRInstruction *instruction);
int ir_instruction_vector_reserve(IRInstructionVector *vector,
                                  size_t capacity);
int ir_instruction_vector_append_move(IRInstructionVector *vector,
                                             IRInstruction *instruction);
void ir_instruction_vector_destroy(IRInstructionVector *vector);
int ir_instruction_writes_destination(const IRInstruction *instruction);
int ir_instruction_writes_symbol(const IRInstruction *instruction);
int ir_instruction_writes_temp(const IRInstruction *instruction);
int ir_label_is_while_header(const char *label);
int ir_fused_loop_exit_is_adjacent(const IRFunction *function,
                                   size_t jump_index, const char *exit_label);
void ir_label_value_map_destroy(IRLabelValueMap *map);
int ir_label_value_map_init(IRLabelValueMap *map);

const IRLabelValueEntry *ir_label_value_map_lookup(
    const IRLabelValueMap *map, const char *label);
int ir_label_value_map_merge_incoming(IRLabelValueMap *map,
                                             const char *label,
                                             const IRTempValueMap *incoming,
                                             int *changed);
int ir_fused_loop_claims_body(const IRFunction *function, size_t header_index,
                              size_t jump_index, const IRInstruction *fused);
int ir_fused_loop_claims_body_with(const IRFunction *function,
                                   size_t header_index, size_t jump_index,
                                   const IRInstruction *fused,
                                   const char *also);
int ir_fused_loop_claims_symbols(const IRFunction *function,
                                 size_t header_index, size_t jump_index,
                                 IROpcode op, const char *const *names,
                                 size_t name_count);
int ir_loop_body_is_unclaimable(const IRFunction *function, size_t start,
                                       size_t end);

int ir_range_has_safety_call(const IRFunction *function, size_t start,
                                    size_t end);
int ir_loop_body_opcode_is_unroll_safe(IROpcode op);
int ir_lower_bound_i32_pass(IRFunction *function, int *changed);
int ir_unroll_annotated_loops_pass(IRFunction *function, int *changed);

char *ir_make_inline_name(const char *prefix, const char *kind,
                                 const char *base);

char *ir_make_inline_prefix(const char *callee_name, size_t inline_id);
int ir_make_simd_with_len(IRInstruction *out, SourceLocation location,
                                 IROpcode op, const IROperand *dest,
                                 const char *lhs_symbol, const char *rhs_symbol,
                                 const IROperand *len_operand);
int ir_match_forward_i32_index(const IRInstruction *index_prod,
                                      const char *iv);
int ir_match_null_trap_diamond(const IRFunction *function,
                                      size_t start_index, size_t *end_index_out,
                                      const char **symbol_name_out);
int ir_memcmp_byte_loop_pass(IRFunction *function, int *changed);
int ir_memcpy_inline_pass(IRFunction *function, int *changed);
int ir_name_map_add(IRNameMap *map, const char *from, const char *to);
void ir_name_map_destroy(IRNameMap *map);

const char *ir_name_map_get_or_create(IRNameMap *map, const char *from,
                                             const char *prefix,
                                             const char *kind);

const char *ir_name_map_lookup(const IRNameMap *map, const char *from);
int ir_null_check_licm_pass(IRFunction *function, int *changed);
int ir_operand_clone(const IROperand *source, IROperand *out);
int ir_operand_equals(const IROperand *lhs, const IROperand *rhs);
int ir_operand_is_int_value(const IROperand *operand,
                                   long long value);
int ir_operand_is_propagatable_value(const IROperand *operand);
int ir_operand_is_symbol_named(const IROperand *operand,
                                      const char *name);
int ir_operand_is_temp_named(const IROperand *operand,
                                    const char *name);
int ir_operand_resolve_symbol_int(const IRSymbolValueMap *symbol_map,
                                         const IROperand *operand,
                                         long long *out_value);
int ir_optimize_function_pipeline(IRFunction *function);
int ir_optimize_program_pipeline(IRProgram *program,
                                 const IROptimizeOptions *options);
int ir_opt_function_is_hot(const IRFunction *function);
int ir_opt_function_is_cold(const IRFunction *function);
int ir_opt_site_is_hot(const IRFunction *function, SourceLocation location);
int ir_opt_site_is_cold(const IRFunction *function, SourceLocation location);
size_t ir_opt_inline_scale(void);
size_t ir_opt_inline_body_budget(const IRFunction *callee);
size_t ir_opt_inline_nested_call_budget(const IRFunction *callee);
size_t ir_opt_inline_caller_budget(const IRFunction *caller);
int ir_opt_self_inline_max_depth(const IRFunction *function);
size_t ir_opt_self_inline_body_budget(const IRFunction *function);
long long ir_opt_unroll_max_trip_count(const IRFunction *function,
                                       SourceLocation location);
long long ir_opt_prefetch_distance_for_site(const IRFunction *function,
                                            SourceLocation location,
                                            long long default_distance);
int ir_opt_should_prefetch_site(const IRFunction *function,
                                SourceLocation location);
int ir_verify_simd_contracts(IRFunction *function);
void ir_optimize_reset_user_error(void);
int ir_optimize_had_user_error(void);
void ir_optimize_note_user_error(void);
int ir_inline_enforce_contracts(IRProgram *program);
int ir_enforce_noalloc_contracts(IRProgram *program);
int ir_user_rewrite_begin(IRProgram *program);
int ir_user_rewrite_pass(IRFunction *function, int *changed);
int ir_user_rewrite_end(IRProgram *program);
void ir_optimize_set_simd_report(int enabled);
void ir_optimize_set_explain(int enabled, const char *focus_file);
void ir_explain_set_quiet(int quiet);
int ir_explain_enabled(void);
int ir_explain_location_enabled(const SourceLocation *location);
int ir_explain_file_enabled(const char *filename);
void ir_explain_remark(const char *function_name, const char *entity,
                       SourceLocation location, int positive,
                       const char *headline, const char *reason,
                       const char *fix, const char *verified);
void ir_explain_remark_loop_depth(size_t line, size_t depth);
void ir_explain_remark_code(const char *code);
void ir_explain_remark_extent(size_t end_line);
void ir_explain_remark_trivial(void);
void ir_explain_remark_advisory(void);
void ir_explain_remark_declined(void);
void ir_explain_loop_trips(const char *function_name, SourceLocation location,
                           long long trips);
void ir_explain_remark_partial(const char *what_still_blocks);
void ir_explain_remark_quantity(const char *name, long value);

#define IR_EXPLAIN_TRIVIAL_CALLEE_INSTRUCTIONS 12

size_t ir_explain_instruction_weight(const IRFunction *function);
void ir_explain_pass_begin(const IRFunction *function);
void ir_explain_pass_end(const IRFunction *function, const char *name,
                         int changed);
void ir_explain_function_before(const IRFunction *function);
void ir_explain_function_after(const IRFunction *function);
IRFunction *ir_explain_clone_function(const IRFunction *src);
void ir_explain_set_hypothesis(int active);
int ir_optimize_function_revectorize(IRFunction *function);
void ir_explain_set_program(IRProgram *program);
const IRModuleSymbol *ir_optimize_module_symbol(const char *name);
int ir_inline_explain_simulate_force_inline(IRProgram *program,
                                            IRFunction *caller,
                                            const char *callee_name,
                                            int *was_noinline_out,
                                            const char **decline_reason_out);

typedef enum {
  IR_SIMD_BAIL_NONE = 0,
  IR_SIMD_BAIL_CALL_IN_BODY,
  IR_SIMD_BAIL_EXTERN_CALL_IN_BODY,
  IR_SIMD_BAIL_SAFETY_IN_BODY,
  IR_SIMD_BAIL_INDIRECT_CALL,
  IR_SIMD_BAIL_ALLOC_IN_BODY,
  IR_SIMD_BAIL_INLINE_ASM,
  IR_SIMD_BAIL_CONTROL_FLOW,
  IR_SIMD_BAIL_EARLY_EXIT,
  IR_SIMD_BAIL_INT16_ELEMENTS,
  IR_SIMD_BAIL_INT64_ELEMENTS,
  IR_SIMD_BAIL_SERIAL_RECURRENCE,
  IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS,
  IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC,
  IR_SIMD_BAIL_I32_SUM_NARROW_ACC,
  IR_SIMD_BAIL_INLINED_PARAM_LOCAL,
  IR_SIMD_BAIL_BODY_LOCAL,
  IR_SIMD_BAIL_DOT_SHAPE_ADDRESS,
  IR_SIMD_BAIL_STORE_ONLY_FILL,
  IR_SIMD_BAIL_EXTREMUM_SHAPE,
  IR_SIMD_BAIL_PREDICATED_COUNT,
  IR_SIMD_BAIL_CLAMP_STORE,
  IR_SIMD_BAIL_STRIDED_ACCESS,
  IR_SIMD_BAIL_UNBOUNDED_SHIFT,
  IR_SIMD_BAIL_VARIABLE_SHIFT,
  IR_SIMD_BAIL_RELOADED_BASE,
  IR_SIMD_BAIL_COUNTER_STORE,
  IR_SIMD_BAIL_SCATTER_STORE,
  IR_SIMD_BAIL_CHOSEN_INDEX,
  IR_SIMD_BAIL_UNRECOGNIZED_SHAPE
} IRSimdBailId;
const char *ir_simd_bail_id_name(int id);
void ir_explain_flush(void);
void ir_explain_finalize(int force_stderr);
void ir_explain_set_filter(const char *selector);
const char *ir_explain_filter(void);
int ir_explain_has_remark_at(size_t line, const char *entity);
size_t ir_explain_inlined_calls_in_range(const char *function_name,
                                         size_t first_line, size_t last_line,
                                         size_t *callee_line, char *callee_out,
                                         size_t callee_cap);
void ir_explain_kernel_desc(const IRInstruction *ins, char *buf, size_t cap);
void ir_inline_explain_report_remaining(IRProgram *program);
int ir_optimize_pre_inline_function(IRFunction *function);
int ir_pass_is_skipped(IROptPassId pass_id);
int ir_pass_name_is_skipped(const char *pass_name);
double ir_pass_time_begin(void);
void ir_pass_time_end(const char *name, double begin_ms);
void ir_pass_time_report(void);
int ir_pointer_induction_pass(IRFunction *function, int *changed);
int ir_prefix_sum_i32_pass(IRFunction *function, int *changed);
int ir_prefetch_indirect_pass(IRFunction *function, int *changed);
int ir_if_convert_pass(IRFunction *function, int *changed);
int ir_ptr_induction_iv_start_value(const IRFunction *function,
                                           size_t header_index,
                                           const char *iv_symbol,
                                           long long *out_start);
int ir_reduction_unroll_pass(IRFunction *function, int *changed);
int ir_rewrite_apply_binary_identities(IRInstruction *instruction,
                                       IRValueRangeCtx *ranges, size_t at,
                                       int *changed);
int ir_reassociate_constants_pass(IRFunction *function, int *changed);
int ir_remove_empty_conditional_diamonds_pass(IRFunction *function,
                                                     int *changed);
int ir_remove_redundant_fallthrough_branches_pass(IRFunction *function,
                                                         int *changed);
int ir_remove_redundant_jumps_pass(IRFunction *function, int *changed);
int ir_remove_unused_labels_pass(IRFunction *function, int *changed);
int ir_resolve_indexed_address_temp(const IRFunction *function,
                                            size_t before_index, const char *iv,
                                            const char *bound,
                                            const char *addr_temp,
                                            const char **base_out,
                                            int *elem_size_out, int *step_out);
int ir_rewrite_to_assign_int(IRInstruction *instruction, long long value,
                                    int *changed);
int ir_rewrite_to_assign_operand(IRInstruction *instruction,
                                        const IROperand *value, int *changed);
int ir_run_named_pass_sequence(IRFunction *function,
                                      const IROptNamedPass *passes,
                                      size_t pass_count,
                                      const char *failure_message);
unsigned ir_opt_feature_flags(const IROptFunctionFeatures *features);
int ir_egraph_simplify_pass(IRFunction *function, int *changed);
unsigned long long ir_affine_loop_fingerprint(const IRFunction *function,
                                              const IRAffineLoop *loop);
int ir_run_named_stage_fixpoint(IRFunction *function,
                                const IROptNamedPass *passes,
                                size_t pass_count, int max_iterations,
                                const char *stage_name,
                                const char *failure_message,
                                int require_convergence);
int ir_run_fixpoint_pass(IRFunction *function, IROptPassId pass_id,
                         IROptFunctionPass pass, int enabled,
                         unsigned long long *version,
                         unsigned long long *clean_version, int *changed);
int ir_simd_affine_map_float_pass(IRFunction *function, int *changed);
int ir_simd_exp_f32_pass(IRFunction *function, int *changed);
int ir_simd_silu_f32_pass(IRFunction *function, int *changed);
int ir_simd_i2f_reduce_pass(IRFunction *function, int *changed);
int ir_auto_vectorize_pass(IRFunction *function, int *changed);
int ir_auto_vectorize_int_pass(IRFunction *function, int *changed);
int ir_auto_vectorize_find_pass(IRFunction *function, int *changed);
int ir_auto_vectorize_find_claimable(IRFunction *function, size_t header_index);
int ir_auto_vectorize_int_claimable(IRFunction *function, size_t header_index);
int ir_outer_vectorize_pass(IRFunction *function, int *changed);
int ir_simd_dot_float_pass(IRFunction *function, int *changed);
int ir_simd_dot_i32_pass(IRFunction *function, int *changed);
int ir_simd_dot_i8_pass(IRFunction *function, int *changed);
int ir_simd_slp_mac_i32_pass(IRFunction *function, int *changed);
int ir_simd_slp_mac_i8_pass(IRFunction *function, int *changed);
int ir_simd_insertion_sort_i32_pass(IRFunction *function, int *changed);
int ir_simd_memory_map_pass(IRFunction *function, int *changed);
int ir_simd_minmax_i32_pass(IRFunction *function, int *changed);
int ir_simd_minmax_reduce_pass(IRFunction *function, int *changed);
int ir_simd_sum_float_pass(IRFunction *function, int *changed);
int ir_simd_sum_i32_pass(IRFunction *function, int *changed);
int ir_simd_sum_u8_pass(IRFunction *function, int *changed);
int ir_simd_copy_pass(IRFunction *function, int *changed);
int ir_simd_fill_pass(IRFunction *function, int *changed);
#define IR_SAFETY_TEMP_PREFIX ".safe"

int ir_instruction_is_safety_scaffolding(const IRInstruction *instruction);
int ir_instruction_is_ptr_count_guard(const IRInstruction *instruction);

int ir_iv_zero_at_header(const IRFunction *function, size_t header_index,
                         const char *iv);
int ir_simd_byte_map_pass(IRFunction *function, int *changed);
int ir_simd_lcg_pass(IRFunction *function, int *changed);
int ir_sroa_pass(IRFunction *function, int *changed);
int ir_forward_piece_copies_pass(IRFunction *function, int *changed);
int ir_strength_reduce_rotate_loops_pass(IRFunction *function, int *changed);
int ir_symbol_address_taken(const IRFunction *function,
                                   const char *symbol_name);
int ir_symbol_contains(const char *symbol, const char *needle);
int ir_symbol_is_i32_ptr_param(const IRFunction *function,
                                      const char *symbol_name);
int ir_symbol_is_sum_array_base(const IRFunction *function,
                                       const char *symbol_name);
int ir_symbol_is_settled_local(const IRFunction *function,
                               const char *symbol_name,
                               const char *expected_type);
int ir_symbol_is_loop_bound(const IRFunction *function,
                            const char *symbol_name, size_t header_index,
                            size_t jump_index);
int ir_symbol_is_sum_loop_bound(const IRFunction *function,
                                       const char *symbol_name);
int ir_symbol_read_after(const IRFunction *function, size_t start_index,
                                const char *symbol_name);
int ir_symbol_live_after_loop(const IRFunction *function, size_t exit_index,
                              const char *symbol_name);
void ir_temp_use_map_destroy(IRTempUseMap *map);
size_t ir_temp_use_map_get(const IRTempUseMap *map, const char *name);
int ir_temp_use_map_init(IRTempUseMap *map);
void ir_temp_value_map_clear(IRTempValueMap *map);
int ir_temp_value_map_clone(IRTempValueMap *dest,
                                   const IRTempValueMap *src);
void ir_temp_value_map_destroy(IRTempValueMap *map);
int ir_temp_value_map_init(IRTempValueMap *map);

const IROperand *ir_temp_value_map_lookup(const IRTempValueMap *map,
                                                 const char *name);
void ir_temp_value_map_remove(IRTempValueMap *map, const char *name);
void ir_temp_value_map_remove_symbol_values(IRTempValueMap *map,
                                                   const char *symbol_name);
void ir_temp_value_map_invalidate_after_store(IRTempValueMap *map,
                                              const IRTempValueMap *addr_taken);
int ir_addr_taken_set_build(const IRFunction *function, IRTempValueMap *set);
int ir_temp_value_map_set(IRTempValueMap *map, const char *name,
                                 const IROperand *value);
int ir_temp_value_map_reindex(IRTempValueMap *map);
int ir_temp_value_map_any_value_symbol(IRTempValueMap *map,
                                       const char *symbol_name);
void ir_temp_value_map_note_value_removed(IRTempValueMap *map,
                                          const IROperand *value);
int ir_thread_jump_targets_pass(IRFunction *function, int *changed);
void ir_value_range_ctx_init(IRValueRangeCtx *ctx, const IRFunction *function);
void ir_value_range_ctx_destroy(IRValueRangeCtx *ctx);
void ir_value_range_of(IRValueRangeCtx *ctx, size_t at,
                       const IROperand *operand, IRIntRange *out);
int ir_value_is_nonnegative(IRValueRangeCtx *ctx, size_t at,
                            const IROperand *operand);
int ir_value_range_simplify(IRValueRangeCtx *ctx, size_t at,
                            IRInstruction *instruction, int *changed);
int ir_remainder_zero_test_to_mask_pass(IRFunction *function, int *changed);
int ir_int_type_name_info(const char *name, int *bits_out, int *is_unsigned_out);
int ir_storage_symbol_set_build(const IRFunction *function,
                                IRTempValueMap *set);
int ir_try_parse_direct_unit_increment(const IRInstruction *instruction,
                                              const char *iv_symbol);
int ir_unroll_small_const_bound_loops_pass(IRFunction *function,
                                                  int *changed);

#endif
