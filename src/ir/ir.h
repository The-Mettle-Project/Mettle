#ifndef IR_H
#define IR_H

#include "../simd_attr.h"
#include "../source_location.h"
#include "ir_analysis.h"
#include "ir_values.h"
#include "ir_verify_structure.h"
#include "mtlc/intrinsic.h"
#include "mtlc/tensor.h"
#include "mtlc/type.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define IR_PROFILE_ID_NONE UINT32_MAX

#define IR_SIMD_MARKER_PREFIX "@@simd:"
#define IR_UNROLL_MARKER_PREFIX "@@unroll:"
#define IR_PARALLEL_MARKER_PREFIX "@@parallel:"

#define IR_SYSCALL_CALL_NAME "__mtl_syscall"

enum {
  IR_REWRITE_ROLE_NONE = 0,
  IR_REWRITE_ROLE_FROM = 1,
  IR_REWRITE_ROLE_TO = 2,
  IR_REWRITE_ROLE_WHERE = 3
};
#define IR_REWRITE_FROM_PREFIX "__rewrite_from__"
#define IR_REWRITE_TO_PREFIX "__rewrite_to__"
#define IR_REWRITE_WHERE_PREFIX "__rewrite_where__"

typedef enum {
  IR_OPERAND_NONE,
  IR_OPERAND_TEMP,
  IR_OPERAND_SYMBOL,
  IR_OPERAND_INT,
  IR_OPERAND_FLOAT,
  IR_OPERAND_STRING,
  IR_OPERAND_LABEL
} IROperandKind;

typedef struct {
  IROperandKind kind;
  char *name;
  long long int_value;
  double float_value;
  int float_bits;
  uint32_t value_id;
} IROperand;

typedef enum {
  IR_OP_NOP,
  IR_OP_LABEL,
  IR_OP_JUMP,
  IR_OP_BRANCH_ZERO,
  IR_OP_BRANCH_EQ,
  IR_OP_DECLARE_LOCAL,
  IR_OP_ADDRESS_SPACE_ALLOC,
  IR_OP_BARRIER,
  IR_OP_ASYNC_COPY,
  IR_OP_ASYNC_COMMIT,
  IR_OP_ASYNC_WAIT,
  IR_OP_TENSOR_TRANSFER,
  IR_OP_TENSOR_MMA,
  IR_OP_TENSOR_MATMUL,
  IR_OP_TENSOR_EPILOGUE,
  IR_OP_TENSOR_COMMIT,
  IR_OP_ASSIGN,
  IR_OP_ADDRESS_OF,
  IR_OP_LOAD,
  IR_OP_STORE,
  IR_OP_BINARY,
  IR_OP_UNARY,
  IR_OP_ROTATE_ADD,
  IR_OP_CALL,
  IR_OP_CALL_INDIRECT,
  IR_OP_GPU_LAUNCH,
  IR_OP_NEW,
  IR_OP_RETURN,
  IR_OP_INLINE_ASM,
  IR_OP_CAST,
  IR_OP_COUNT_WORD_STARTS,
  IR_OP_MEMCPY_INLINE,
  IR_OP_SIMD_SUM_I32,
  IR_OP_SIMD_SUM_U8,
  IR_OP_SIMD_BYTE_MAP,
  IR_OP_SIMD_FILL,
  IR_OP_SIMD_COPY,
  IR_OP_SIMD_MATMUL_N32,
  IR_OP_SIMD_INSERTION_SORT_I32,
  IR_OP_SIMD_DOT_I32,
  IR_OP_SIMD_DOT_I8,
  IR_OP_SIMD_SLP_MAC_I32,
  IR_OP_SIMD_SLP_MAC_I8,
  IR_OP_SIMD_SCALE_I32,
  IR_OP_SIMD_CLAMP_I32,
  IR_OP_SIMD_REVERSE_COPY_I32,
  IR_OP_LOWER_BOUND_I32,
  IR_OP_PREFIX_SUM_I32,
  IR_OP_SIMD_MINMAX_I32,
  IR_OP_SIMD_SUM_F64,
  IR_OP_SIMD_SUM_F32,
  IR_OP_SIMD_DOT_F64,
  IR_OP_SIMD_DOT_F32,
  IR_OP_SIMD_AFFINE_MAP_F64,
  IR_OP_SIMD_AFFINE_MAP_F32,
  IR_OP_SIMD_EXP_F32,
  IR_OP_SIMD_SILU_F32,
  IR_OP_SIMD_I2F_REDUCE_F64,
  IR_OP_SIMD_VLOOP_F64,
  IR_OP_SIMD_VLOOP_I32,
  IR_OP_SIMD_FIND,
  IR_OP_SIMD_OUTER_LANE_F64,
  IR_OP_SIMD_LCG_U32,
  IR_OP_PREFETCH,
  IR_OP_SELECT,
  IR_OP_SAFETY_CHECK,
  IR_OP_PHI,
  IR_OP_ASM_RESULT,
  IR_OP_TILE,
  IR_OP_TENSOR_SCALE_ROWS,
  IR_OP_SIMD_MULTIDOT_F32,
  IR_OP_SIMD_LANES,
  IR_OP_KIND_COUNT
} IROpcode;

#define IR_SAFETY_EXTENT_UNKNOWN (-1)

#define IR_SAFETY_ACCESS_READ 0
#define IR_SAFETY_ACCESS_WRITE 1

#define IR_SAFETY_ARG_BASE 0u
#define IR_SAFETY_ARG_OFFSET 1u
#define IR_SAFETY_ARG_SIZE 2u
#define IR_SAFETY_ARG_EXTENT 3u
#define IR_SAFETY_ARG_ACCESS 4u
#define IR_SAFETY_ARG_COUNT 5u
#define IR_SAFETY_ARG_IDENTITY 5u
#define IR_SAFETY_TRACKED_ARG_COUNT 6u
#define IR_SAFETY_ARG_DIAGNOSTIC_SIZE 6u
#define IR_SAFETY_ANALYZED_ARG_COUNT 7u

#define IR_GPU_LAUNCH_CONTROL_ARGS 8u

typedef enum {
  IR_BYTE_MAP_ADD = 0,
  IR_BYTE_MAP_SUB = 1,
  IR_BYTE_MAP_MUL = 2,
  IR_BYTE_MAP_XOR = 3,
  IR_BYTE_MAP_AND = 4,
  IR_BYTE_MAP_OR = 5
} IRByteMapOp;

typedef enum {
  IR_TENSOR_RESIDENCY_NONE = 0,
  IR_TENSOR_RESIDENCY_START = 1,
  IR_TENSOR_RESIDENCY_UPDATE = 2,
  IR_TENSOR_RESIDENCY_COMMIT = 3
} IRTensorResidencyRole;

typedef enum {
  IR_TENSOR_RESIDENCY_SCOPE_NONE = 0,
  IR_TENSOR_RESIDENCY_SCOPE_LOOP = 1,
  IR_TENSOR_RESIDENCY_SCOPE_PIPELINE = 2,
  IR_TENSOR_RESIDENCY_SCOPE_REGION = 3
} IRTensorResidencyScope;

typedef struct IRTensorAux {
  int heap_owned;
#ifdef METTLE_IR_TENSOR_DEBUG
  unsigned magic;
#endif
  MtlcTensorTransferDesc transfer;
  MtlcTensorMmaDesc mma;
  MtlcTensorEpilogueDesc epilogue;
} IRTensorAux;

typedef struct {
  IROpcode op;
  MtlcIntrinsic intrinsic;
  MtlcAddressSpace address_space;
  MtlcMemoryOrder memory_order;
  MtlcMemoryOrder failure_memory_order;
  MtlcMemoryScope memory_scope;
  unsigned memory_regions;
  unsigned char uniform_branch;
  unsigned char uniform_value;
  unsigned char divergent_call;
  uint32_t async_copy_element_count;
  uint32_t async_copy_transaction_bytes;
  uint32_t async_copy_pending_groups;
  MtlcAsyncCache async_copy_cache;
  int async_copy_generated;
  IRTensorAux *tensor;
  int tensor_transfer_has_prepared_view;
  uint32_t tensor_mma_count;
  uint32_t tensor_residency_id;
  IRTensorResidencyRole tensor_residency_role;
  IRTensorResidencyScope tensor_residency_scope;
  SourceLocation location;
  IROperand dest;
  IROperand lhs;
  IROperand rhs;
  char *text;
  IROperand *arguments;
  MtlcType **argument_types;
  size_t argument_count;
  int is_float;
  int float_bits;
  int is_unsigned;
  int is_volatile;
  int allocates;
  unsigned char asm_operands;
  unsigned char uniform_condition;
  unsigned char gpu_launch_indirect;
  void *ast_ref;
  MtlcType *value_type;
  unsigned char alias_class;
  unsigned char column_base;
  const char *expansion_note;
  const char *effect_signature;
} IRInstruction;

enum {
  IR_DIVERGENT_NONE = 0,
  IR_DIVERGENT_FULL = 1,
  IR_DIVERGENT_BLOCK = 2
};

MtlcIntrinsic ir_intrinsic_from_name(const char *name);
const char *ir_gpu_only_construct_name(IROpcode op);
const char *ir_intrinsic_name(MtlcIntrinsic intrinsic);
int ir_intrinsic_arity(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_atomic(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_compare_exchange(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_atomic_load(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_atomic_store(MtlcIntrinsic intrinsic);
MtlcTypeKind ir_intrinsic_atomic_value_kind(MtlcIntrinsic intrinsic);
MtlcTypeKind ir_intrinsic_atomic_result_kind(MtlcIntrinsic intrinsic);
int ir_intrinsic_is_subgroup(MtlcIntrinsic intrinsic);
MtlcTypeKind ir_intrinsic_subgroup_result_kind(MtlcIntrinsic intrinsic);
int ir_tile_operand_is_tile(const MtlcType *type);
int ir_tensor_c_is_zero(const IROperand *operand);
typedef enum {
  IR_BLOCK_SCALE_PRODUCT = 0,
  IR_BLOCK_SCALE_B_FIRST,
  IR_BLOCK_SCALE_A_FIRST
} IRBlockScaleOrder;
IRBlockScaleOrder ir_tensor_block_scale_order(const MtlcTensorMmaDesc *desc);
int ir_tensor_mma_desc_valid(const MtlcTensorMmaDesc *desc);
int ir_tensor_epilogue_desc_valid(const MtlcTensorEpilogueDesc *desc);
int ir_tensor_transfer_desc_valid(const MtlcTensorTransferDesc *desc);
size_t ir_tensor_transfer_element_bytes(MtlcTensorElement element);
size_t ir_tensor_transfer_tile_elements(const MtlcTensorTransferDesc *desc);
size_t ir_tensor_transfer_operand_count(const MtlcTensorTransferDesc *desc,
                                        int has_prepared_view);
size_t ir_tensor_mma_operand_count(const MtlcTensorMmaDesc *desc);
size_t ir_tensor_matmul_operand_count(const MtlcTensorMmaDesc *desc);
size_t ir_tensor_epilogue_operand_count(
    const MtlcTensorEpilogueDesc *desc);
unsigned ir_tensor_mma_runtime_stride_mask(const MtlcTensorMmaDesc *desc);
size_t ir_tensor_mma_instruction_count(const IRInstruction *instruction);
size_t ir_tensor_scale_rows_operand_count(const MtlcTensorMmaDesc *desc);
int ir_tensor_mma_desc_equal(const MtlcTensorMmaDesc *lhs,
                             const MtlcTensorMmaDesc *rhs);
int ir_operand_same(const IROperand *lhs, const IROperand *rhs);
MtlcTypeKind ir_tensor_element_storage_kind(MtlcTensorElement element);

typedef struct {
  const char *label;
  IRInstruction *instructions;
  size_t instruction_count;
  size_t first_instruction;
  size_t *successors;
  size_t successor_count;
  size_t *predecessors;
  size_t predecessor_count;
} IRBasicBlock;

typedef enum {
  IR_ALIAS_CLASS_NONE = 0,
  IR_ALIAS_CLASS_POINTER,
  IR_ALIAS_CLASS_I8,
  IR_ALIAS_CLASS_I16,
  IR_ALIAS_CLASS_I32,
  IR_ALIAS_CLASS_I64,
  IR_ALIAS_CLASS_F32,
  IR_ALIAS_CLASS_F64,
  IR_ALIAS_CLASS_F16,
  IR_ALIAS_CLASS_BF16,
  IR_ALIAS_CLASS_COUNT
} IRAliasClassId;

typedef struct {
  char *name;
  uint32_t profile_id;
  char **parameter_names;
  char **parameter_types;
  size_t parameter_count;
  char *return_type_name;
  SourceLocation location;
  IRInstruction *instructions;
  size_t instruction_count;
  size_t instruction_capacity;
  IRBasicBlock *blocks;
  size_t block_count;
  size_t entry_block;
  int cfg_valid;
  IRValueTable values;
  uint64_t generation;
  uint64_t structure_generation;
  uint64_t numbered_generation;
  size_t numbered_instruction_count;
  void *analysis;
  int is_inline;
  int is_inline_contract;
  int is_noinline;
  int is_pure;
  int is_readonly_inferred;
  int is_speculatable_inferred;
  const char *reference_twin;
  long long deadline_cycles;
  int has_deadline;
  int deadline_inclusive;
  const char *explain_code;
  const char *explain_text;
  int is_noalloc;
  int is_test;
  const char *numerics_contract;
  int is_host_twin;
  int is_swappable;
  int is_naked;
  int is_interrupt;
  int is_rule;
  const char **effects_with;
  size_t effects_with_count;
  const char **effects_forbids;
  size_t effects_forbids_count;
  const char **effects_requires;
  size_t effects_requires_count;
  const char **effects_provides;
  size_t effects_provides_count;
  int has_volatile_access;
  int is_kernel;
  int rewrite_role;
  int is_exported;
  int kernel_block[3];
  int kernel_threads_per_item;
} IRFunction;

typedef struct {
  char *name;
  char *filename;
  uint64_t line;
} IRProfileEntry;

typedef struct {
  char *name;
  char *type_name;
} IRDebugLocalEntry;

typedef struct {
  char *name;
  MtlcType *type;
} IRTypeEntry;

typedef enum {
  IR_MODSYM_FUNCTION,
  IR_MODSYM_VARIABLE,
  IR_MODSYM_CONSTANT
} IRModuleSymbolKind;

typedef struct {
  size_t offset;
  char *symbol;
  char *string;
  size_t string_length;
  int string_wants_record;
} IRInitReloc;

typedef struct {
  char *name;
  MtlcType *type;
  IRModuleSymbolKind kind;
  int is_extern;
  int has_body;
  int is_kernel;
  char *link_name;
  char *effect_clause;
  long long const_value;
  int has_initializer;
  int init_is_float;
  long long init_bits;
  char *init_string;
  size_t init_string_length;
  char *init_symbol_ref;
  unsigned char *init_bytes;
  size_t init_bytes_size;
  IRInitReloc *init_relocs;
  size_t init_reloc_count;
  int has_unfoldable_initializer;
  int is_immutable;
  int is_exported;
  int is_volatile;
  MtlcType *return_type;
  MtlcType **param_types;
  size_t param_count;
  void *codegen_view;
} IRModuleSymbol;

int ir_inline_asm_binds_symbol(const char *assembly_text, const char *name);

typedef struct {
  char name[128];
  unsigned char reads;
  unsigned char writes;
} IRAsmBinding;

int ir_inline_asm_bindings(const char *text, IRAsmBinding **out,
                           size_t *count);
int ir_inline_asm_defines_label(const char *text);

typedef struct {
  IRFunction **functions;
  size_t function_count;
  size_t function_capacity;
  IRProfileEntry *profile_entries;
  size_t profile_entry_count;
  size_t profile_entry_capacity;
  IRDebugLocalEntry *debug_local_entries;
  size_t debug_local_entry_count;
  size_t debug_local_entry_capacity;
  IRTypeEntry *type_registry;
  size_t type_registry_count;
  size_t type_registry_capacity;
  MtlcType **owned_types;
  size_t owned_type_count;
  size_t owned_type_capacity;
  IRModuleSymbol *module_symbols;
  size_t module_symbol_count;
  size_t module_symbol_capacity;
  int main_wants_argc_argv;
  const char **alias_globals;
  size_t alias_global_count;
  int alias_globals_computed;
  int dead_functions_eliminated;
  size_t next_inline_id;
} IRProgram;

typedef struct {
  size_t *order;
  size_t count;
  unsigned char *reachable;
  size_t function_count;
} IRGpuCallGraph;

IROperand ir_operand_none(void);
IROperand ir_operand_temp(const char *name);
IROperand ir_operand_symbol(const char *name);
IROperand ir_operand_int(long long value);
IROperand ir_operand_float(double value);
IROperand ir_operand_float_sized(double value, int float_bits);
IROperand ir_operand_string(const char *value);
IROperand ir_operand_string_n(const char *value, size_t length);
char *ir_copy_literal_bytes(const char *value, size_t length);
size_t ir_operand_string_length(const IROperand *operand);
IROperand ir_operand_label(const char *name);
IROperand ir_operand_copy(const IROperand *operand);
void ir_operand_destroy(IROperand *operand);
void ir_operand_destroy_detached(IROperand *operand);

void ir_declare_float_bound(const char *type_name, double lo, double hi);
void ir_declare_nonzero_type(const char *type_name);
int ir_type_is_nonzero(const char *type_name);
void ir_display_name(const char *name, char *out, size_t cap);
size_t ir_display_code(const char *raw, size_t len, char *out, size_t cap);
int ir_lookup_float_bound(const char *type_name, double *lo, double *hi);
int ir_has_float_bounds(void);

IRFunction *ir_function_create(const char *name);
const char *ir_backend_type_name(const char *source_name);

int ir_function_set_parameters(IRFunction *function, const char **parameter_names,
                               const char **parameter_types,
                               size_t parameter_count);
void ir_function_destroy(IRFunction *function);
int ir_function_append_instruction(IRFunction *function,
                                   const IRInstruction *instruction);
int ir_function_insert_instruction(IRFunction *function, size_t index,
                                   const IRInstruction *instruction);
enum {
  IR_EFFECT_CLAUSE_WITH = 0,
  IR_EFFECT_CLAUSE_FORBIDS = 1,
  IR_EFFECT_CLAUSE_REQUIRES = 2,
  IR_EFFECT_CLAUSE_PROVIDES = 3
};
int ir_function_set_effects(IRFunction *function, int clause,
                            const char *const *names, size_t count);
int ir_program_register_scalar_pointer_types(IRProgram *program);
MtlcType *ir_program_int64_array_type(IRProgram *program, size_t count);
void *ir_value_range_oracle_create(const IRFunction *function);
void ir_value_range_oracle_destroy(void *oracle);
int ir_value_range_result_is_narrow(void *oracle, size_t at, int bits,
                                    int is_unsigned);
void ir_function_clear_cfg(IRFunction *function);
int ir_function_rebuild_cfg(IRFunction *function);
const IRBasicBlock *ir_function_blocks(IRFunction *function,
                                       size_t *block_count);

extern uint64_t g_ir_operand_writes;
void ir_instruction_destroy(IRInstruction *instruction);
int ir_promote_scalar_locals_pass(IRFunction *function, int *changed);
int ir_leave_ssa_pass(IRFunction *function, int *changed);
int ir_ssa_propagate_pass(IRFunction *function, int *changed);
int ir_repair_phis(IRFunction *function, int *changed);
int ir_ssa_sabotage_pass(IRFunction *function, int *changed);
int ir_ssa_enabled(void);
void ir_ssa_opt_report_stats(void);
int ir_instruction_writes_destination(const IRInstruction *instruction);
uint64_t ir_function_fingerprint(const IRFunction *function);
uint64_t ir_function_structure_fingerprint(const IRFunction *function);
void ir_check_silent_structure(const IRFunction *function,
                               const char *pass_name, uint64_t before,
                               uint64_t structure_before);
size_t ir_silent_structure_count(void);
size_t ir_cfg_repair_count(void);
void ir_check_silent_mutation(const IRFunction *function,
                              const char *pass_name, uint64_t before,
                              uint64_t generation_before);
size_t ir_silent_mutation_count(void);
int ir_operand_names_match(const IROperand *a, const IROperand *b);
int ir_operand_is_temp(const IROperand *operand);
int ir_operand_is_symbol(const IROperand *operand);
int ir_operand_is_value(const IROperand *operand);
uint32_t ir_function_value_id(IRFunction *function, const IROperand *operand);
const char *ir_function_value_name(const IRFunction *function, uint32_t id);
char *ir_name_share(const char *name);
int ir_function_number_values(IRFunction *function);
int ir_function_number_values_if_stale(IRFunction *function);
int ir_function_values_agree(const IRFunction *function, char *why,
                             size_t why_capacity);
int ir_function_values_audit(const IRFunction *function, char *why,
                             size_t why_capacity, size_t *unnumbered_out);
void ir_function_touch(IRFunction *function);
void ir_function_touch_structure(IRFunction *function);
void ir_value_check_after_pass(const IRFunction *function,
                               const char *pass_name);
void ir_value_maybe_sabotage(IRFunction *function, const char *pass_name);

int ir_function_check_structure(const IRFunction *function,
                                IRStructureReport *report, char *why,
                                size_t why_capacity);
size_t ir_structure_dominance_violations(IRFunction *function, char *why,
                                         size_t why_capacity);
size_t ir_function_check_phis(IRFunction *function, char *why,
                              size_t why_capacity);
void ir_phi_check_after_pass(IRFunction *function, const char *pass_name);
size_t ir_phi_violation_count(void);
size_t ir_structure_snapshot(const IRFunction *function);
void ir_structure_check_after_pass(const IRFunction *function,
                                   const char *pass_name, size_t before);
void ir_structure_maybe_sabotage(IRFunction *function, const char *pass_name);

const IRAnalysis *ir_function_analysis(IRFunction *function);
void ir_function_release_analysis(IRFunction *function);
size_t ir_function_instruction_block(IRFunction *function, size_t index);
int ir_block_dominates(const IRAnalysis *analysis, size_t a, size_t b);
int ir_function_block_dominates(IRFunction *function, size_t a, size_t b);
int ir_function_instruction_dominates(IRFunction *function, size_t a, size_t b);
uint32_t ir_function_value_single_def(IRFunction *function, uint32_t id);
size_t ir_function_value_def_count(IRFunction *function, uint32_t id);
const IRValueUse *ir_function_value_uses(IRFunction *function, uint32_t id,
                                         size_t *count_out);
const size_t *ir_function_dominance_frontier(IRFunction *function, size_t block,
                                             size_t *count_out);
const IRInstruction *ir_function_temp_producer_before(
    const IRFunction *function, size_t before_index, const char *temp_name,
    int *usable);
size_t ir_function_first_jump_to(const IRFunction *function, size_t after,
                                 const char *label);
size_t ir_function_last_jump_to(const IRFunction *function, size_t after,
                                const char *label);
void ir_analysis_report_stats(void);
size_t ir_analysis_self_check(IRFunction *function);
size_t ir_value_stale_report_count(void);
size_t ir_value_unnumbered_count(void);

int ir_program_global_address_taken(IRProgram *program, const char *name);
IRProgram *ir_program_create(void);
void ir_program_destroy(IRProgram *program);
int ir_program_add_function(IRProgram *program, IRFunction *function);

int ir_program_register_type(IRProgram *program, const char *name,
                             MtlcType *type);
MtlcType *ir_program_lookup_type(const IRProgram *program, const char *name);
int ir_program_drop_rules(IRProgram *program);
int ir_program_drop_numerics_harnesses(IRProgram *program);
int ir_program_drop_rules_except(IRProgram *program,
                                 int (*keep)(const IRFunction *));

IRModuleSymbol *ir_program_add_symbol(IRProgram *program,
                                      const IRModuleSymbol *proto);
const IRModuleSymbol *ir_program_lookup_symbol(const IRProgram *program,
                                               const char *name);

int ir_program_dump(IRProgram *program, FILE *output);
const char *ir_opcode_name(IROpcode op);
const IRTensorAux *ir_instruction_tensor_ro(const IRInstruction *instruction);

#define IR_TENSOR_TRANSFER(in) (ir_instruction_tensor_ro(in)->transfer)
#define IR_TENSOR_MMA(in) (ir_instruction_tensor_ro(in)->mma)
#define IR_TENSOR_EPILOGUE(in) (ir_instruction_tensor_ro(in)->epilogue)

int ir_instruction_tensor_copy(IRInstruction *dst, const IRInstruction *src);

void ir_instruction_tensor_clear(IRInstruction *instruction);

void ir_instruction_tensor_borrow(IRInstruction *dst, IRTensorAux *block,
                                  const IRInstruction *src);

#define ir_instruction_tensor_attach(dst, block) \
  ir_instruction_tensor_borrow((dst), (block), NULL)

int ir_instruction_dump(const IRInstruction *instruction,
                        char *buffer, size_t capacity);

int ir_program_route_to_native_heap(IRProgram *program);

int ir_program_lower_gpu_launches(IRProgram *program);
int ir_gpu_launch_is_indirect(const IRInstruction *instruction);
int ir_program_build_gpu_call_graph(const IRProgram *program,
                                    IRGpuCallGraph *graph, char **error);
void ir_gpu_call_graph_destroy(IRGpuCallGraph *graph);
int ir_program_bind_device_asm(IRProgram *program, char **error);

size_t ir_function_drop_dead_nops(IRFunction *function);
void ir_function_freeze_declarations(const IRFunction *function);
const IRInstruction *ir_function_find_declaration_frozen(
    const IRFunction *function, const char *symbol_name);
const IRInstruction *ir_function_find_declaration(const IRFunction *function,
                                                 const char *symbol_name,
                                                 int symbols_only);
int ir_program_eliminate_dead_functions(IRProgram *program, int keep_exports);
#define IR_DEVICE_IMAGE_SYMBOL "__mettle_device_image"
int ir_program_defines_kernels(const IRProgram *program);
int ir_program_can_register_device_image(IRProgram *program,
                                         const char *register_name);
int ir_program_embed_device_image(IRProgram *program, const char *image,
                                  size_t size, const MtlcType *image_type,
                                  const char *register_name);
int ir_program_drop_rewrite_rules(IRProgram *program);

int ir_init_image_is_all_zero(const IRModuleSymbol *symbol);

int ir_tensor_region_instruction_allowed(const IRProgram *program,
                                         const IRFunction *function,
                                         const IRInstruction *instruction,
                                         const IROperand *output);
int ir_tensor_scale_rows_joins(const IRProgram *program,
                               const IRFunction *function,
                               const IRInstruction *start,
                               const IRInstruction *scale);
int ir_tensor_region_update_operands_clean(const IRInstruction *update,
                                           const IROperand *output);

#endif
