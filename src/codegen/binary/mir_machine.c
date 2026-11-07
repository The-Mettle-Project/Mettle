#include "codegen/binary/mir_machine.h"
#include "internal.h"

#define MIR_GP_BIT(r) (1u << (unsigned)(r))

#define MIR_RAX MIR_GP_BIT(BINARY_GP_RAX)
#define MIR_RCX MIR_GP_BIT(BINARY_GP_RCX)
#define MIR_RDX MIR_GP_BIT(BINARY_GP_RDX)

#define MIR_DEF MIR_OPF_PURE_DEF
#define MIR_COAL MIR_OPF_COALESCE_CANDIDATE
#define MIR_COMM (MIR_OPF_COMMUTATIVE | MIR_OPF_COALESCE_CANDIDATE)
#define MIR_BARRIER MIR_OPF_CALL_BARRIER
#define MIR_REALCALL (MIR_OPF_CALL_BARRIER | MIR_OPF_REAL_CALL)
#define MIR_KERNEL (MIR_OPF_CALL_BARRIER | MIR_OPF_INLINE_KERNEL)
#define MIR_CONDBR (MIR_OPF_LABEL_BRANCH | MIR_OPF_CONDITIONAL_BRANCH)

static const MirOpInfo MIR_OP_INFO[MIR_OPCODE_COUNT] = {
    [MIR_MOV] = {MIR_DEF | MIR_COAL, 0u},
    [MIR_LEA] = {MIR_DEF, 0u},
    [MIR_LEA_LOCAL] = {MIR_DEF, 0u},
    [MIR_LEA_GLOBAL] = {MIR_DEF, 0u},
    [MIR_LEA_FUNC] = {MIR_DEF, 0u},
    [MIR_LEA_CSTR] = {MIR_DEF, 0u},
    [MIR_LEA_STRLIT] = {MIR_DEF, 0u},
    [MIR_POPCNT] = {MIR_DEF, 0u},
    [MIR_MOVZX] = {MIR_DEF, 0u},
    [MIR_MOVSX] = {MIR_DEF, 0u},

    [MIR_ADD] = {MIR_DEF | MIR_COMM, 0u},
    [MIR_SUB] = {MIR_DEF | MIR_COAL, 0u},
    [MIR_AND] = {MIR_DEF | MIR_COMM, 0u},
    [MIR_OR] = {MIR_DEF | MIR_COMM, 0u},
    [MIR_XOR] = {MIR_DEF | MIR_COMM, 0u},
    [MIR_IMUL] = {MIR_DEF | MIR_COMM, 0u},
    [MIR_NEG] = {MIR_DEF | MIR_COAL, 0u},
    [MIR_NOT] = {MIR_DEF | MIR_COAL, 0u},

    [MIR_SHL] = {MIR_DEF | MIR_COAL | MIR_OPF_PINS_RCX_WHEN_SHIFT_IS_VARIABLE,
                 0u},
    [MIR_SHR] = {MIR_DEF | MIR_COAL | MIR_OPF_PINS_RCX_WHEN_SHIFT_IS_VARIABLE,
                 0u},
    [MIR_SAR] = {MIR_DEF | MIR_COAL | MIR_OPF_PINS_RCX_WHEN_SHIFT_IS_VARIABLE,
                 0u},

    [MIR_CQO] = {0u, MIR_RDX},
    [MIR_XOR_RDX] = {0u, MIR_RDX},
    [MIR_IDIV] = {0u, MIR_RAX | MIR_RDX},
    [MIR_DIV] = {0u, MIR_RAX | MIR_RDX},
    [MIR_MULHI] = {0u, MIR_RAX | MIR_RDX},

    [MIR_JMP] = {MIR_OPF_LABEL_BRANCH, 0u},
    [MIR_JCC] = {MIR_CONDBR, 0u},
    [MIR_CMPBR] = {MIR_CONDBR, 0u},
    [MIR_FCMPBR] = {MIR_CONDBR, 0u},
    [MIR_BT] = {MIR_CONDBR, 0u},

    [MIR_SETCC] = {MIR_DEF, MIR_RAX},
    [MIR_CMOVCC] = {MIR_DEF, 0u},
    [MIR_FSETCC] = {MIR_OPF_PINS_RCX_WHEN_COMPARE_IS_UNORDERED, MIR_RAX},

    [MIR_FADD] = {MIR_DEF | MIR_COMM, 0u},
    [MIR_FSUB] = {MIR_DEF | MIR_COAL, 0u},
    [MIR_FMUL] = {MIR_DEF | MIR_COMM, 0u},
    [MIR_FDIV] = {MIR_DEF | MIR_COAL, 0u},
    [MIR_FXOR] = {MIR_DEF | MIR_COMM, 0u},
    [MIR_FSQRT] = {MIR_DEF, 0u},
    [MIR_FDUP] = {MIR_DEF, 0u},
    [MIR_FEXTHI] = {MIR_DEF, 0u},
    [MIR_CVTSI2F] = {MIR_DEF, 0u},
    [MIR_CVTF2SI] = {MIR_DEF, 0u},
    [MIR_CVTF2F] = {MIR_DEF, 0u},
    [MIR_MOVD_TO_XMM] = {MIR_DEF, 0u},
    [MIR_MOVD_TO_GP] = {MIR_DEF, 0u},
    [MIR_CVTPH2PS] = {MIR_DEF, 0u},
    [MIR_CVTPS2PH] = {MIR_DEF, 0u},

    [MIR_CALL] = {MIR_REALCALL, 0u},
    [MIR_CALL_INDIRECT] = {MIR_REALCALL, 0u},
    [MIR_HEAP_NEW] = {MIR_REALCALL, 0u},
    [MIR_REP_MOVSB] = {MIR_REALCALL, 0u},
    [MIR_REP_STOSB] = {MIR_REALCALL, 0u},
    [MIR_SYSCALL] = {MIR_REALCALL, 0u},
    [MIR_INLINE_ASM] = {MIR_BARRIER | MIR_OPF_CLOBBERS_EVERY_GP, 0u},

    [MIR_SIMD_SLP_MAC] = {MIR_KERNEL, 0u},
    [MIR_SIMD_FILL] = {MIR_KERNEL, 0u},
    [MIR_SIMD_AFFINE_MAP_F32] = {MIR_KERNEL, 0u},
    [MIR_SIMD_AFFINE_MAP_F64] = {MIR_KERNEL, 0u},
    [MIR_SIMD_SILU_F32] = {MIR_KERNEL, 0u},
    [MIR_SIMD_VLOOP] = {MIR_KERNEL, 0u},
    [MIR_IR_KERNEL] = {MIR_KERNEL | MIR_OPF_CLOBBERS_LISTED_BY_KERNEL, 0u},
};

static const MirOpInfo MIR_OP_INFO_NONE = {0u, 0u};

const MirOpInfo *mir_op_info(MirOpcode op) {
  if ((int)op < 0 || (int)op >= MIR_OPCODE_COUNT) {
    return &MIR_OP_INFO_NONE;
  }
  return &MIR_OP_INFO[op];
}

int mir_op_has(MirOpcode op, unsigned flags) {
  return (mir_op_info(op)->flags & flags) != 0u;
}

unsigned mir_op_fixed_gp(MirOpcode op) { return mir_op_info(op)->fixed_gp; }

unsigned mir_inst_fixed_gp(const MirInst *in) {
  const MirOpInfo *info;
  unsigned mask;
  if (!in) {
    return 0u;
  }
  info = mir_op_info(in->op);
  mask = info->fixed_gp;
  if ((info->flags & MIR_OPF_PINS_RCX_WHEN_SHIFT_IS_VARIABLE) &&
      in->b.kind != MIR_OPK_IMM) {
    mask |= MIR_RCX;
  }
  if ((info->flags & MIR_OPF_PINS_RCX_WHEN_COMPARE_IS_UNORDERED) &&
      mir_fsetcc_unordered_cc(in->cc) >= 0) {
    mask |= MIR_RCX;
  }
  return mask;
}

int mir_inst_pins_gp(const MirInst *in, int reg) {
  if (reg < 0 || reg >= 64) {
    return 0;
  }
  return (mir_inst_fixed_gp(in) & MIR_GP_BIT(reg)) != 0u;
}

static const int MIR_X86_GP_ORDER[] = {
    BINARY_GP_RBX, BINARY_GP_R12, BINARY_GP_R13, BINARY_GP_R14,
    BINARY_GP_R15, BINARY_GP_RAX, BINARY_GP_RCX, BINARY_GP_RDX,
    BINARY_GP_RSI, BINARY_GP_RDI, BINARY_GP_R8,  BINARY_GP_R9};

static const int MIR_X86_XMM_ORDER[] = {BINARY_XMM0, BINARY_XMM1, BINARY_XMM2,
                                        BINARY_XMM3};

static const MirRegBank MIR_X86_BANKS[] = {
    {MIR_RC_GP, MIR_X86_GP_ORDER,
     sizeof(MIR_X86_GP_ORDER) / sizeof(MIR_X86_GP_ORDER[0])},
    {MIR_RC_XMM, MIR_X86_XMM_ORDER,
     sizeof(MIR_X86_XMM_ORDER) / sizeof(MIR_X86_XMM_ORDER[0])},
};

static const MirMachine MIR_X86_MACHINE = {
    "x86-64", MIR_X86_BANKS,
    sizeof(MIR_X86_BANKS) / sizeof(MIR_X86_BANKS[0]),
    MIR_RAX | MIR_RCX | MIR_RDX, 16};

const MirMachine *mir_machine(void) { return &MIR_X86_MACHINE; }

int mir_machine_gp_is_pinned(int reg) {
  if (reg < 0 || reg >= 64) {
    return 0;
  }
  return (mir_machine()->pinned_gp & MIR_GP_BIT(reg)) != 0u;
}
