#ifndef CODEGEN_BINARY_MIR_H
#define CODEGEN_BINARY_MIR_H

#include "codegen/binary/internal.h"

#include <stddef.h>
#include <stdint.h>

typedef int MirVregId;
#define MIR_VREG_NONE (-1)

typedef enum {
  MIR_RC_GP = 0,
  MIR_RC_XMM = 1,
  MIR_RC_VEC = 2
} MirRegClass;

typedef struct {
  MirRegClass rclass;
  int width;
  int lanes;
  int assigned;
  int in_register;
  int phys;
  int spill_offset;
  int live_start;
  int live_end;
  int crosses_call;
  int loop_carried;
  int coalesce_hint;
  int address_taken;
  int home_bytes;
  int home_width;
  int home_signed;
  int home_granule;
  int entry_live;
  int crosses_preserving_only;
  int crosses_xmm_preserving_only;
  int coalesced_into;
  int may_read_undefined;
} MirVreg;
#define MIR_LIVE_NONE (-1)

typedef enum {
  MIR_OPK_NONE = 0,
  MIR_OPK_VREG,
  MIR_OPK_PHYS,
  MIR_OPK_IMM,
  MIR_OPK_FIMM,
  MIR_OPK_MEM,
  MIR_OPK_LABEL,
  MIR_OPK_SYMBOL,
  MIR_OPK_STACKHOME
} MirOperandKind;

typedef struct {
  MirVregId base;
  MirVregId index;
  int scale;
  int disp;
  int phys_base_valid;
  int phys_base;
  int frame_home_valid;
  MirVregId frame_home;
} MirMem;

typedef struct {
  MirOperandKind kind;
  MirVregId vreg;
  int phys;
  MirRegClass rclass;
  long long imm;
  MirMem mem;
  const char *sym;
  int disp;
} MirOperand;

typedef enum {
  MIR_NOP = 0,

  MIR_MOV,
  MIR_LEA,
  MIR_LEA_LOCAL,
  MIR_LEA_GLOBAL,
  MIR_LEA_FUNC,
  MIR_LEA_CSTR,
  MIR_LEA_STRLIT,
  MIR_POPCNT,
  MIR_HEAP_NEW,
  MIR_MOVZX,
  MIR_MOVSX,
  MIR_LOAD_GLOBAL,
  MIR_STORE_GLOBAL,

  MIR_ADD,
  MIR_SUB,
  MIR_AND,
  MIR_OR,
  MIR_XOR,
  MIR_IMUL,
  MIR_NEG,
  MIR_NOT,
  MIR_SHL,
  MIR_SHR,
  MIR_SAR,

  MIR_CQO,
  MIR_XOR_RDX,
  MIR_IDIV,
  MIR_DIV,
  MIR_MULHI,

  MIR_CMP,
  MIR_TEST,
  MIR_BT,
  MIR_SETCC,
  MIR_CMOVCC,

  MIR_JMP,
  MIR_JCC,
  MIR_CMPBR,
  MIR_JMP_TABLE,
  MIR_LABEL,
  MIR_PREFETCH,
  MIR_CMOV,

  MIR_CALL,
  MIR_CALL_INDIRECT,
  MIR_REP_MOVSB,
  MIR_REP_STOSB,
  MIR_SYSCALL,
  MIR_STORE_OUTARG,
  MIR_LEA_OUTARG,
  MIR_TRAP,
  MIR_INLINE_ASM,
  MIR_RET,

  MIR_FADD,
  MIR_FSUB,
  MIR_FMUL,
  MIR_FDIV,
  MIR_FXOR,
  MIR_FSQRT,
  MIR_FDUP,
  MIR_FEXTHI,
  MIR_CVTSI2F,
  MIR_CVTF2SI,
  MIR_CVTF2F,
  MIR_UCOMIS,
  MIR_FSETCC,
  MIR_FCMPBR,
  MIR_MOVD_TO_XMM,
  MIR_MOVD_TO_GP,
  MIR_CVTPH2PS,
  MIR_CVTPS2PH,

  MIR_VADD,
  MIR_VSUB,
  MIR_VMUL,
  MIR_VDIV,
  MIR_VCVTSI2F,
  MIR_VCVTF2SI,
  MIR_VLOAD,
  MIR_VSTORE,
  MIR_VBROADCAST,
  MIR_VIOTA,
  MIR_VHREDUCE,

  MIR_SIMD_SLP_MAC,

  MIR_SIMD_FILL,

  MIR_SIMD_AFFINE_MAP_F32,

  MIR_SIMD_AFFINE_MAP_F64,

  MIR_SIMD_SILU_F32,

  MIR_SIMD_VLOOP,

  MIR_IR_KERNEL,

  MIR_OPCODE_COUNT
} MirOpcode;

int mir_op_is_inline_kernel(MirOpcode op);

#define MIR_XMM_POOL_COUNT 4
extern const BinaryXmmRegister MIR_XMM_POOL[MIR_XMM_POOL_COUNT];

typedef struct {
  MirOpcode op;
  MirOperand dst;
  MirOperand a;
  MirOperand b;
  int width;
  int is_float;
  int is_unsigned;
  unsigned char cc;
  int ir_index;
  const void *aux;
  int preserves_rax;
  int preserves_xmm;
} MirInst;

#define MIR_KERNEL_MAX_SLOTS 16

typedef struct {
  const IRInstruction *ir;
  int kernel_index;
  int slot_count;
  MirVregId slot_vreg[MIR_KERNEL_MAX_SLOTS];
  int operand_count;
  const IROperand *operand[MIR_KERNEL_MAX_SLOTS];
  int operand_slot[MIR_KERNEL_MAX_SLOTS];
} MirKernelAux;

typedef struct {
  uint64_t bits;
  int width;
  MirVregId vreg;
} MirFConst;

typedef struct {
  int64_t value;
  MirVregId vreg;
} MirIConst;

typedef struct {
  MirVregId vreg;
  int arg_index;
  int width;
  int is_signed;
  int is_float;
  int sysv_eightbytes;
  int sysv_in_memory;
  int sysv_sse[2];
  int sysv_size;
  int sysv_direct_sse;
  MirVregId sysv_storage;
} MirParam;

#define MIR_MAX_PARAMS 128

typedef struct {
  MirVreg *vregs;
  size_t vreg_count;
  size_t vreg_capacity;

  MirInst *insns;
  size_t insn_count;
  size_t insn_capacity;

  char **owned_syms;
  size_t owned_sym_count;
  size_t owned_sym_capacity;

  void **owned_aux;
  size_t owned_aux_count;
  size_t owned_aux_capacity;

  BinaryFunctionContext *context;
  CodeGenerator *generator;

  MirParam params[MIR_MAX_PARAMS];
  size_t param_count;

  int returns_indirect;
  int indirect_return_size;
  MirVregId indirect_return_vreg;

  int scalar_return_width;
  int scalar_return_signed;

  int float_return_bits;

  struct {
    const void *sibling;
    MirVregId vreg;
  } divmod_precomp[16];
  size_t divmod_precomp_count;

  MirFConst *fconsts;
  size_t fconst_count;
  size_t fconst_capacity;

  MirIConst *iconsts;
  size_t iconst_count;
  size_t iconst_capacity;

  int spill_bytes;

  size_t merged_copies;

  int preserve_slot;
  int preserve_xmm_slot;

  int outgoing_stack_bytes;

  int outgoing_indirect_bytes;

  int used_inline_vector;

  int has_xmm_arg_call;

  int cur_ir_index;

  const IRFunction *ir_function;

  const struct MirTempUseIndex *temp_uses;

  int reserve_rbx;

  size_t incoming_arg_slots;

  int returns_sysv_registers;
  int sysv_return_eightbytes;
  int sysv_return_sse[2];
  int sysv_return_size;

  int has_error;

  size_t *label_slots;
  size_t label_slot_capacity;
  size_t label_slot_insns;

  int code_moved_after_allocation;
} MirFunction;

#define MIR_PARAM_SLOTS (2 * MIR_MAX_PARAMS + 1)

int mir_param_layout(const MirFunction *fn, const BinaryAbi *abi,
                     BinaryArgLocation *locs, size_t *first_slot,
                     size_t *count_out);

void mir_function_init(MirFunction *fn, BinaryFunctionContext *context);
void mir_function_destroy(MirFunction *fn);

MirVregId mir_new_vreg(MirFunction *fn, MirRegClass rclass, int width);

int mir_emit(MirFunction *fn, const MirInst *inst);

void *mir_function_own_aux(MirFunction *fn, void *block);

#define MIR_MAX_JUMP_TABLES 64

typedef struct {
  char **labels;
  size_t count;
} MirJumpTable;

typedef struct {
  IROpcode ir_op;
  const char *name;
  int (*emit)(CodeGenerator *generator, BinaryFunctionContext *context,
              const IRInstruction *instruction);
  unsigned gp_clobbers;
} MirIrKernel;

#define MIR_ASM_MAX_BINDS 16

typedef struct {
  const IRInstruction *ir;
  int count;
  const char *names[MIR_ASM_MAX_BINDS];
  MirVregId vregs[MIR_ASM_MAX_BINDS];
} MirAsmAux;

const MirIrKernel *mir_ir_kernel_for_op(IROpcode op);

const MirIrKernel *mir_ir_kernel_at(int index);

int mir_ir_kernel_index_for_op(IROpcode op);

MirOperand mir_op_none(void);
MirOperand mir_op_vreg(MirVregId v);
MirOperand mir_op_phys(int phys, MirRegClass rclass);
MirOperand mir_op_imm(long long value);
MirOperand mir_op_fimm(uint64_t ieee_bits);
MirOperand mir_op_label(const char *name);
MirOperand mir_op_symbol(const char *name);
MirOperand mir_op_mem_vreg(MirVregId base, MirVregId index, int scale, int disp);

void mir_function_dump(const MirFunction *fn, FILE *out);

const char *mir_opcode_name(MirOpcode op);

int mir_regalloc(MirFunction *fn);
int mir_regalloc_verify(MirFunction *fn);
int mir_verify_enabled(void);
int mir_verify_sabotage_enabled(void);
void mir_verify_sabotage(MirFunction *fn);
int mir_verify_structure(MirFunction *fn, const char *stage, int allocated);
int mir_regalloc_verify_enabled(void);
int mir_regalloc_verify_sabotage_enabled(void);
void mir_regalloc_verify_sabotage(MirFunction *fn);

static inline BinaryXmmRegister mir_xmm_scratch_a(void) {
  return code_generator_binary_active_abi()->counts_classes_separately
             ? BINARY_XMM8
             : BINARY_XMM4;
}

static inline BinaryXmmRegister mir_xmm_scratch_b(void) {
  return code_generator_binary_active_abi()->counts_classes_separately
             ? BINARY_XMM9
             : BINARY_XMM5;
}

static inline int mir_xmm_is_encoder_scratch(BinaryXmmRegister reg) {
  return reg == mir_xmm_scratch_a() || reg == mir_xmm_scratch_b();
}

static inline int mir_fsetcc_unordered_cc(unsigned char cc) {
  if (cc == 0x94) {
    return 0x9B;
  }
  if (cc == 0x95) {
    return 0x9A;
  }
  return -1;
}

int mir_encode(MirFunction *fn);

int mir_materialize_constant_callees(IRFunction *ir_function);
int mir_route_global_call_results(CodeGenerator *generator,
                                  IRFunction *ir_function);

void mir_fold_shared_address(MirFunction *fn);
void mir_sink_partial_loads(MirFunction *fn);
void mir_guard_counter_wrap(MirFunction *fn);
void mir_thread_known_tests(MirFunction *fn);
void mir_thread_dispatch(MirFunction *fn);
void mir_place_blocks(MirFunction *fn);
int mir_cfg_refresh_live_positions(MirFunction *fn);

int code_generator_binary_emit_function_via_mir(
    CodeGenerator *generator,
    IRFunction *ir_function, BinaryFunctionContext *context);

#endif
