#ifndef IR_OPTIMIZE_INTERNAL_H
#define IR_OPTIMIZE_INTERNAL_H

#include "../ir_optimize.h"
#include "../ir_profile.h"
#include "../../common.h"
#include "../../compiler/compiler_context.h"
#include "../../compiler/compiler_crash.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared optimizer-private limits, maps, vectors, and pass metadata. */
#define IR_INLINE_MAX_NON_NOP_INSTRUCTIONS 128
#define IR_INLINE_MAX_PARAMETERS 16
#define IR_INLINE_MAX_ROUNDS 4
#define IR_INLINE_MAX_CALLER_NON_NOP_INSTRUCTIONS 512
/* Callees this small (and call-free) stay inlinable into callers that are
 * over the caller-size budget: the budget exists to stop runaway growth, and
 * a tiny leaf's body costs about as much caller growth as the call sequence
 * it replaces. */
#define IR_INLINE_TINY_LEAF_NON_NOP_INSTRUCTIONS 16
/* Self-recursion inlining: expand direct self-call sites with the current
 * body up to MAX_DEPTH rounds, stopping early once the body outgrows the
 * instruction cap (so the depth is effectively size-bounded). */
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

/* Name -> value map with a lazily maintained open-addressing hash index over
 * `items`. The array stays the source of truth (passes iterate it directly);
 * the index makes find/set/remove O(1) so copy-propagation stays linear on
 * the multi-thousand-instruction functions inlining produces. Code that
 * compacts `items` in place must call ir_temp_value_map_reindex afterwards. */
typedef struct IRTempValueMap {
  IRTempValueEntry *items;
  size_t count;
  size_t capacity;
  unsigned int *ix;     /* buckets: 0 = empty, UINT_MAX = tombstone, else slot+1 */
  size_t ix_capacity;   /* power of two, 0 until first index build */
  size_t ix_tombstones;
  /* Lazily built reverse count: value-symbol name -> how many entries map to
   * it (int values; itself never carries a reverse count). Lets the
   * per-symbol-write invalidation in copy-propagation answer "no entry
   * values this symbol" in O(1) instead of scanning every entry. NULL until
   * ir_temp_value_map_remove_symbol_values first needs it. */
  struct IRTempValueMap *vsym_counts;
} IRTempValueMap;

/* Block-local known values for stack locals (@symbol operands). */
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
  /* label -> slot index (int values); copy-propagation consults this map per
   * label/jump/branch, and inlined functions carry thousands of labels. */
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
  /* Open-addressing hash of name -> (index+1) into items, so find() is O(1)
   * instead of a linear scan. 0 means empty slot. Sized to a power of two with
   * load factor < 0.5; rebuilt when items grows. */
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
  /* Open-addressing index over items (slot+1; 0 = empty). The inliner hits
   * lookup per operand of every inlined instruction. */
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
} IRExpressionEntry;

typedef struct {
  IRExpressionEntry *items;
  size_t count;
  size_t capacity;
} IRExpressionMap;

typedef struct {
  const char *name; /* borrowed from the IRFunction; not owned */
  IRFunction *function;
} IRFunctionIndexSlot;

typedef struct {
  IRFunctionIndexSlot *slots;
  size_t slot_count; /* power of two */
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

#define IR_PTR_BIND_MAX 4

typedef struct {
  const char *base;
  char *ptr_p;
  char *addr_temps[8];
  size_t addr_temp_count;
} IRPtrBaseBinding;

typedef struct {
  size_t header_index;   /* LOOP label */
  size_t end_index;      /* END label */
  const char *counter;   /* COUNTER symbol (stepped by -1) */
  const char *dst;       /* DST pointer symbol */
  const char *src;       /* SRC pointer symbol (= dst - stride) */
  const char *key;       /* KEY value compared against *src */
  const char *cmp_op;    /* continue-comparison operator (e.g. "<=") */
  long long stride;      /* pointer step in bytes (positive) */
  int elem_size;         /* element byte size of the load/store */
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
  char *name; /* synthesized scalar local name, owned */
} IRSroaSlot;

/* A temp that holds &S (offset 0) or &S + CONST. */
typedef struct {
  const char *temp; /* borrowed temp name */
  long long offset; /* byte offset into S */
  int valid;        /* cleared if the temp is used in a disqualifying way */
} IRSroaAddr;

/* A scalarizable aggregate local and its decl index. */
typedef struct {
  const char *name; /* borrowed symbol name */
  size_t decl_index;
} IRSroaMember;

/* Per-member transform context: the member name + its addr temps. */
typedef struct {
  const char *name;
  size_t decl_index;
  IRSroaAddr addrs[IR_SROA_MAX_SLOTS * 2];
  size_t addr_count;
} IRSroaMemberCtx;

#define IR_ARRAY_COUNT(items) (sizeof(items) / sizeof((items)[0]))

#define IR_OPT_PASS_LIST(X)                                                   \
  X(REDUCTION_UNROLL, "reduction_unroll")                                    \
  X(COPY_AND_CONSTANT_PROPAGATION, "copy_and_constant_propagation")           \
  X(FUSE_ROTATE_ADD, "fuse_rotate_add")                                      \
  X(STRENGTH_REDUCE_ROTATE_LOOPS, "strength_reduce_rotate_loops")            \
  X(UNROLL_SMALL_CONST_BOUND_LOOPS, "unroll_small_const_bound_loops")         \
  X(POSITIVE_LOOP_DIV2_TO_SHIFT, "positive_loop_div2_to_shift")              \
  X(FOLD_POPCOUNT_BYTE_LOOP, "fold_popcount_byte_loop")                      \
  X(FUSE_POPCOUNT_BUFFER_LOOP, "fuse_popcount_buffer_loop")                  \
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
  X(SROA, "sroa")

typedef enum {
#define IR_OPT_PASS_ENUM(id, name) IR_OPT_PASS_##id,
  IR_OPT_PASS_LIST(IR_OPT_PASS_ENUM)
#undef IR_OPT_PASS_ENUM
  IR_OPT_PASS_COUNT
} IROptPassId;

typedef int (*IROptFunctionPass)(IRFunction *function, int *changed);

typedef struct {
  const char *name;
  IROptFunctionPass run;
} IROptNamedPass;

extern const char *g_ir_pass_names[IR_OPT_PASS_COUNT];
const char *ir_opt_pass_name(IROptPassId pass_id);

/* Optimizer-private function API. */
int ir_binary_is_unit_increment_of_iv(const IRInstruction *instruction,
                                             const char *iv_symbol);
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
int ir_find_label_index(const IRFunction *function, const char *label,
                               size_t *out_index);
int ir_find_last_writer_before(const IRFunction *function, size_t before_index,
                                      IROperandKind kind, const char *name,
                                      size_t *writer_index);
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
int ir_find_while_loop_bounds(IRFunction *function, size_t header_index,
                                     IRWhileLoopBounds *out);
int ir_fold_popcount_byte_loop_pass(IRFunction *function, int *changed);
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
int ir_fuse_rotate_add_pass(IRFunction *function, int *changed);
int ir_fuse_while_loop_to_insn(IRFunction *function, size_t header_index,
                                      size_t jump_index, IRInstruction *fused,
                                      int *changed);
int ir_index_vector_append(IRIndexVector *vector, size_t value);
void ir_index_vector_destroy(IRIndexVector *vector);
int ir_inline_small_functions_pass(IRProgram *program, int *changed);
int ir_inline_self_recursion_pass(IRProgram *program, int *changed);
int ir_tail_recursion_elimination_pass(IRProgram *program, int *changed);
/* Allocation-site layout factorization (ir_optimize_layout.c): re-map the
 * interior layout of provably-private malloc pools (compact padded strides /
 * factor into per-field arrays). Whole-program; run after inlining. */
int ir_layout_factor_pass(IRProgram *program, int *changed);
struct IRGlobalIntConst;
int ir_fold_readonly_globals_pass(IRProgram *program,
                                  const struct IRGlobalIntConst *consts,
                                  size_t count, int *changed);
/* Resolve a function by name within the program (hashed lookup with a linear
 * fallback). Defined in ir_optimize_inline.c. */
IRFunction *ir_program_find_function(IRProgram *program, const char *name);
/* `@pure` loop-invariant call hoisting (program-level; runs after inlining). */
int ir_hoist_pure_calls_pass(IRProgram *program, int *changed);
void ir_instruction_clear_arguments(IRInstruction *instruction);
void ir_instruction_destroy_storage(IRInstruction *instruction);
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
void ir_label_value_map_destroy(IRLabelValueMap *map);
int ir_label_value_map_init(IRLabelValueMap *map);

const IRLabelValueEntry *ir_label_value_map_lookup(
    const IRLabelValueMap *map, const char *label);
int ir_label_value_map_merge_incoming(IRLabelValueMap *map,
                                             const char *label,
                                             const IRTempValueMap *incoming,
                                             int *changed);
int ir_loop_body_has_nested_while(IRFunction *function, size_t start,
                                         size_t end);
int ir_loop_body_opcode_is_unroll_safe(IROpcode op);
int ir_lower_bound_i32_pass(IRFunction *function, int *changed);

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
/* Profile-aware optimization policy (ir_optimize_hotness.c). When no PGO data
 * is available these return the historical static thresholds. With zero-run
 * PGO, function body steps and source-keyed site counts act like lightweight
 * block hotness for thresholds that trade code size against speed. */
int ir_opt_function_is_hot(const IRFunction *function);
int ir_opt_function_is_cold(const IRFunction *function);
int ir_opt_site_is_hot(const IRFunction *function, SourceLocation location);
int ir_opt_site_is_cold(const IRFunction *function, SourceLocation location);
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
/* Enforce `@simd` / `@simd!` loop attributes after vectorization has run.
 * Returns 1 if every contract was honored (and clears all `@simd` markers),
 * 0 if a `@simd!` contract was violated (after printing a diagnostic). */
int ir_verify_simd_contracts(IRFunction *function);
/* Resets/queries the "a @simd! contract was violated" flag so the driver can
 * distinguish a user error from an internal compiler error.
 * (ir_optimize_had_user_error is also declared in the public ir_optimize.h.) */
void ir_optimize_reset_user_error(void);
int ir_optimize_had_user_error(void);
/* Lets the `@inline!` / `@noalloc` contract checkers report through the same
 * "user error, not ICE" channel `@simd!` uses. */
void ir_optimize_note_user_error(void);
/* `@inline!`: error for every surviving call to a contract function, with the
 * inliner's reason. Returns 1 when all contracts held. */
int ir_inline_enforce_contracts(IRProgram *program);
/* `@noalloc`: transitive allocation-freedom proof per contract function
 * (ir_optimize_contracts.c). Returns 1 when all contracts held. */
int ir_enforce_noalloc_contracts(IRProgram *program);
/* --simd-report: emit a note for every `@simd` loop (vectorized or not). */
void ir_optimize_set_simd_report(int enabled);
/* --explain: optimization-decision remarks (state and report rendering live in
 * ir_optimize_explain.c). The loop verifier, the inliner, and the codegen MIR
 * gate record remarks; the pipeline flushes them as one sorted, tree-formatted
 * report. `focus_file` (NULL = no filter) limits remarks to that file so
 * imported modules don't flood the report. */
void ir_optimize_set_explain(int enabled, const char *focus_file);
int ir_explain_enabled(void);
int ir_explain_location_enabled(const SourceLocation *location);
int ir_explain_file_enabled(const char *filename);
/* Record one remark: `entity` is "loop" / "call to `f`"; `positive` colors the
 * headline (1 = the optimizer did something good); reason/fix/verified may be
 * NULL. `verified` is reserved for claims PROVEN by simulating the fix on a
 * clone -- never for guesses. */
void ir_explain_remark(const char *function_name, const char *entity,
                       SourceLocation location, int positive,
                       const char *headline, const char *reason,
                       const char *fix, const char *verified);
/* Stamp the nest depth (1 = top level) on the most recent loop remark at
 * `line`; rendered into the JSON sidecar so tooling can rank scalar loops by
 * how deeply they nest (a static hotness proxy). */
void ir_explain_remark_loop_depth(size_t line, size_t depth);
/* Hypothesis testing: deep-copy a function for fix simulation, suppress
 * remark recording while the cloned stages run, and re-run the optimization
 * stages on the clone (ir_optimize_pipeline.c). */
IRFunction *ir_explain_clone_function(const IRFunction *src);
void ir_explain_set_hypothesis(int active);
int ir_optimize_function_revectorize(IRFunction *function);
/* Program access for program-level fix simulations (the call-in-body
 * hypothesis re-runs the INLINER on a caller clone, which needs callee
 * lookup). Set by the program pipeline around the per-function stage; NULL
 * outside it. */
void ir_explain_set_program(IRProgram *program);
/* --explain hypothesis (ir_optimize_inline.c): pretend `callee_name` is
 * inline-eligible (@noinline removed, @inline added; *was_noinline_out says
 * which pretend the verified message should describe) and run inliner rounds
 * over `caller` (a scratch clone). Returns 1 when at least one call site
 * actually expanded. The pretend flags are restored; the inliner's structural
 * guards still apply, so this cannot "verify" advice that the decorator
 * change would not in fact deliver. When the inliner refuses the callee even
 * with the pretend flags set (a STRUCTURAL refusal -- @inline cannot fix it),
 * *decline_reason_out (may be NULL) receives the inliner's own static reason
 * string; it stays NULL on success and on couldn't-tell outcomes. */
int ir_inline_explain_simulate_force_inline(IRProgram *program,
                                            IRFunction *caller,
                                            const char *callee_name,
                                            int *was_noinline_out,
                                            const char **decline_reason_out);

/* Machine-readable ids for every vectorization-bail diagnosis the --explain
 * analyzer can make (ir_optimize_simd_contract.c). This list is the schema:
 * fix-hypothesis transforms key off these ids, and a future --explain=json
 * emits them as stable names (ir_simd_bail_id_name). One id per prose branch
 * in ir_simd_explain_bail -- a new diagnosis MUST add an id, not hide under
 * UNRECOGNIZED_SHAPE. */
typedef enum {
  IR_SIMD_BAIL_NONE = 0,           /* no diagnosis ran / loop vectorized */
  IR_SIMD_BAIL_CALL_IN_BODY,       /* calls a program-defined fn every iteration */
  IR_SIMD_BAIL_EXTERN_CALL_IN_BODY,/* calls an extern: inlining is impossible */
  IR_SIMD_BAIL_INDIRECT_CALL,      /* calls through a function pointer */
  IR_SIMD_BAIL_ALLOC_IN_BODY,      /* allocates (`new`) every iteration */
  IR_SIMD_BAIL_INLINE_ASM,         /* body contains inline assembly */
  IR_SIMD_BAIL_CONTROL_FLOW,       /* data-dependent branching inside the body */
  IR_SIMD_BAIL_EARLY_EXIT,         /* the loop can leave before the trip count */
  IR_SIMD_BAIL_INT16_ELEMENTS,     /* 16-bit integer memory, no kernel */
  IR_SIMD_BAIL_INT64_ELEMENTS,     /* 64-bit integer memory, no kernel */
  IR_SIMD_BAIL_SERIAL_RECURRENCE,  /* non-reassociable loop-carried recurrence:
                                      a scalar computed from its own prior value
                                      through *, /, shift, bitwise, or xor (int
                                      or float) */
  IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS, /* f32 and f64 elements in one loop */
  IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC,/* byte sum into an int32 accumulator */
  IR_SIMD_BAIL_I32_SUM_NARROW_ACC, /* int32 sum into a non-int64 accumulator */
  IR_SIMD_BAIL_INLINED_PARAM_LOCAL,/* leftover __inl_* param copy in body */
  IR_SIMD_BAIL_BODY_LOCAL,         /* user local declared inside the body */
  IR_SIMD_BAIL_DOT_SHAPE_ADDRESS,  /* float MAC, address pattern unmatched */
  IR_SIMD_BAIL_STORE_ONLY_FILL,    /* writes-only fill/init pattern */
  IR_SIMD_BAIL_UNRECOGNIZED_SHAPE  /* honest fallback: no cause identified */
} IRSimdBailId;
/* Stable lowercase-kebab name for an id (e.g. "byte-sum-narrow-acc"). */
const char *ir_simd_bail_id_name(int id);
/* Render the sorted optimization report (loops + calls) into the report
 * buffer and clear the store. Routing to stderr or the sidecar file happens
 * in ir_explain_finalize -- called by the backend flush on the normal path,
 * or with force_stderr=1 when compilation aborts before codegen (a contract
 * violation must not eat the report). */
void ir_explain_flush(void);
void ir_explain_finalize(int force_stderr);
/* True when a remark for this (line, entity) is already recorded -- lets a
 * later pass skip a weaker guess when a definitive remark exists (e.g. the
 * unroller's "fully unrolled" beats the verifier's "no loop remains"). */
int ir_explain_has_remark_at(size_t line, const char *entity);
/* Instruction-level description of a vectorized kernel op for headlines. */
void ir_explain_kernel_desc(const IRInstruction *ins, char *buf, size_t cap);
/* --explain: after all inlining rounds, record a remark for every surviving
 * call to a program-defined function with the reason it was not inlined. */
void ir_inline_explain_report_remaining(IRProgram *program);
int ir_optimize_pre_inline_function(IRFunction *function);
int ir_pass_is_skipped(IROptPassId pass_id);
int ir_pass_name_is_skipped(const char *pass_name);
/* METTLE_TIME_IR_PASSES=1: cumulative per-pass wall-time table, dumped at the
 * end of optimization. begin/end bracket program-level passes by name. */
double ir_pass_time_begin(void);
void ir_pass_time_end(const char *name, double begin_ms);
void ir_pass_time_report(void);
int ir_pointer_induction_pass(IRFunction *function, int *changed);
int ir_positive_loop_div2_to_shift_pass(IRFunction *function,
                                               int *changed);
int ir_prefix_sum_i32_pass(IRFunction *function, int *changed);
/* Software prefetch insertion for indirect (gather) loads in counted loops
 * (ir_optimize_prefetch.c). Runs last in the post-fixpoint stage. */
int ir_prefetch_indirect_pass(IRFunction *function, int *changed);
/* If-conversion of register-only if/else diamonds to branchless IR_OP_SELECT
 * (cmov), for data-dependent branches (ir_optimize_if_convert.c). */
int ir_if_convert_pass(IRFunction *function, int *changed);
int ir_ptr_induction_iv_start_value(const IRFunction *function,
                                           size_t header_index,
                                           const char *iv_symbol,
                                           long long *out_start);
int ir_reduction_unroll_pass(IRFunction *function, int *changed);
/* Declarative algebraic rewrite engine (ir_optimize_rewrite.c): the integer
 * identity table lives there, so adding an identity is adding one table row. */
int ir_rewrite_apply_binary_identities(IRInstruction *instruction,
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
/* Read-only probe: 1 if the int auto-vectorizer would claim the counted loop
 * whose header label is at header_index (pointer-induction declines those --
 * the vectorizer needs the indexed form). */
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
int ir_simd_sum_float_pass(IRFunction *function, int *changed);
int ir_simd_sum_i32_pass(IRFunction *function, int *changed);
int ir_simd_sum_u8_pass(IRFunction *function, int *changed);
int ir_simd_fill_pass(IRFunction *function, int *changed);
int ir_iv_zero_at_header(const IRFunction *function, size_t header_index,
                         const char *iv);
int ir_simd_byte_map_pass(IRFunction *function, int *changed);
int ir_simd_lcg_pass(IRFunction *function, int *changed);
int ir_sroa_pass(IRFunction *function, int *changed);
int ir_strength_reduce_rotate_loops_pass(IRFunction *function, int *changed);
int ir_symbol_address_taken(const IRFunction *function,
                                   const char *symbol_name);
int ir_symbol_contains(const char *symbol, const char *needle);
int ir_symbol_is_i32_ptr_param(IRFunction *function,
                                      const char *symbol_name);
int ir_symbol_is_sum_array_base(const IRFunction *function,
                                       const char *symbol_name);
int ir_symbol_is_sum_loop_bound(const IRFunction *function,
                                       const char *symbol_name);
int ir_symbol_read_after(const IRFunction *function, size_t start_index,
                                const char *symbol_name);
/* Loop-exit iv liveness for the SIMD recognizers: like read_after, but a
 * straight-line full redefinition (iv reuse by the next loop) kills the
 * value instead of blocking vectorization. */
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
/* `addr_taken` is the function's address-taken symbol set, precomputed once
 * per pass with ir_addr_taken_set_build (scanning the function per entry per
 * store was a cubic term on large functions). */
void ir_temp_value_map_invalidate_after_store(IRTempValueMap *map,
                                              const IRTempValueMap *addr_taken);
int ir_addr_taken_set_build(const IRFunction *function, IRTempValueMap *set);
int ir_temp_value_map_set(IRTempValueMap *map, const char *name,
                                 const IROperand *value);
/* Rebuild the hash index after compacting `items` in place. */
int ir_temp_value_map_reindex(IRTempValueMap *map);
/* True when some entry's VALUE is `symbol_name` (lazily builds the reverse
 * count); a 0 lets per-symbol-write invalidation skip its scan. Compactors
 * that remove entries in place must report each removed value via
 * ir_temp_value_map_note_value_removed to keep the counts true. */
int ir_temp_value_map_any_value_symbol(IRTempValueMap *map,
                                       const char *symbol_name);
void ir_temp_value_map_note_value_removed(IRTempValueMap *map,
                                          const IROperand *value);
int ir_thread_jump_targets_pass(IRFunction *function, int *changed);
int ir_try_parse_direct_unit_increment(const IRInstruction *instruction,
                                              const char *iv_symbol);
int ir_unroll_small_const_bound_loops_pass(IRFunction *function,
                                                  int *changed);

#endif /* IR_OPTIMIZE_INTERNAL_H */
