#include "codegen/binary/arm64_mir.h"

#include <string.h>

Arm64Cond arm64_cond_from_x86_cc(unsigned char x86_cc) {
  switch (x86_cc & 0x0F) {
  case 0x0: return ARM64_VS; /* O  */
  case 0x1: return ARM64_VC; /* NO */
  case 0x2: return ARM64_CC; /* B/C   unsigned <  */
  case 0x3: return ARM64_CS; /* AE/NC unsigned >= */
  case 0x4: return ARM64_EQ; /* E/Z  */
  case 0x5: return ARM64_NE; /* NE   */
  case 0x6: return ARM64_LS; /* BE   unsigned <= */
  case 0x7: return ARM64_HI; /* A    unsigned >  */
  case 0x8: return ARM64_MI; /* S    */
  case 0x9: return ARM64_PL; /* NS   */
  case 0xC: return ARM64_LT; /* L    signed <  */
  case 0xD: return ARM64_GE; /* GE   signed >= */
  case 0xE: return ARM64_LE; /* LE   signed <= */
  case 0xF: return ARM64_GT; /* G    signed >  */
  default: return ARM64_AL;  /* P/NP have no AArch64 equivalent */
  }
}

/* The two IP scratch registers (x16/x17) are reserved for materializing
 * immediates that do not fit an instruction's inline field. */
#define SCRATCH0 ARM64_X16
#define SCRATCH1 ARM64_X17

/* Forward-referenceable label map: MIR labels are strings; the emit layer uses
 * integer ids, so intern each name to an id (creating on first sight so a
 * forward branch can reference a not-yet-defined label). */
typedef struct {
  const char *names[256];
  int ids[256];
  int count;
} LabelMap;

static int label_id(Arm64Emit *e, LabelMap *m, const char *name) {
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || (name && m->names[i] && strcmp(m->names[i], name) == 0)) {
      return m->ids[i];
    }
  }
  if (m->count >= 256) {
    e->error = 1;
    return 0;
  }
  int id = arm64_new_label(e);
  m->names[m->count] = name;
  m->ids[m->count] = id;
  m->count++;
  return id;
}

static void emit_mov_imm(Arm64Emit *e, Arm64Reg rd, uint64_t v) {
  arm64_emit_word(e, arm64_movz(1, rd, (uint16_t)(v & 0xFFFF), 0));
  if ((v >> 16) & 0xFFFF) {
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 16) & 0xFFFF), 1));
  }
  if ((v >> 32) & 0xFFFF) {
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 32) & 0xFFFF), 2));
  }
  if ((v >> 48) & 0xFFFF) {
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 48) & 0xFFFF), 3));
  }
}

/* Resolve an operand to a register, materializing an immediate into `scratch`. */
static Arm64Reg op_reg(Arm64Emit *e, const MirOperand *op, Arm64Reg scratch) {
  if (op->kind == MIR_OPK_PHYS || op->kind == MIR_OPK_VREG) {
    return (Arm64Reg)(op->kind == MIR_OPK_PHYS ? op->phys : op->vreg);
  }
  if (op->kind == MIR_OPK_IMM) {
    emit_mov_imm(e, scratch, (uint64_t)op->imm);
    return scratch;
  }
  e->error = 1;
  return scratch;
}

static int imm_fits_u12(const MirOperand *op) {
  return op->kind == MIR_OPK_IMM && op->imm >= 0 && op->imm <= 4095;
}

/* dst = a OP b for the add/sub family, using the immediate form when b is a
 * small unsigned immediate, otherwise the register form. */
static void alu_addsub(Arm64Emit *e, int is_sub, Arm64Reg dst, Arm64Reg a,
                       const MirOperand *b) {
  if (imm_fits_u12(b)) {
    arm64_emit_word(e, is_sub ? arm64_sub_imm(1, dst, a, (uint32_t)b->imm, 0)
                              : arm64_add_imm(1, dst, a, (uint32_t)b->imm, 0));
  } else {
    Arm64Reg br = op_reg(e, b, SCRATCH0);
    arm64_emit_word(e, is_sub ? arm64_sub_reg(1, dst, a, br)
                              : arm64_add_reg(1, dst, a, br));
  }
}

int arm64_mir_encode_seq(Arm64Emit *e, const MirInst *insns, size_t count) {
  LabelMap m;
  m.count = 0;

  if (!arm64_emit_prologue(e, 0, NULL, 0)) {
    return 0;
  }

  for (size_t i = 0; i < count && !e->error; i++) {
    const MirInst *in = &insns[i];
    Arm64Reg dst = (Arm64Reg)in->dst.phys;
    Arm64Reg a = (Arm64Reg)in->a.phys;

    switch (in->op) {
    case MIR_NOP:
      break;

    case MIR_MOV:
      if (in->a.kind == MIR_OPK_IMM) {
        emit_mov_imm(e, dst, (uint64_t)in->a.imm);
      } else {
        arm64_emit_mov(e, 1, dst, op_reg(e, &in->a, SCRATCH0));
      }
      break;

    case MIR_ADD:
      alu_addsub(e, 0, dst, a, &in->b);
      break;
    case MIR_SUB:
      alu_addsub(e, 1, dst, a, &in->b);
      break;
    case MIR_AND:
      arm64_emit_word(e, arm64_and_reg(1, dst, a, op_reg(e, &in->b, SCRATCH0)));
      break;
    case MIR_OR:
      arm64_emit_word(e, arm64_orr_reg(1, dst, a, op_reg(e, &in->b, SCRATCH0)));
      break;
    case MIR_XOR:
      arm64_emit_word(e, arm64_eor_reg(1, dst, a, op_reg(e, &in->b, SCRATCH0)));
      break;
    case MIR_IMUL:
      arm64_emit_word(e, arm64_mul(1, dst, a, op_reg(e, &in->b, SCRATCH0)));
      break;
    case MIR_NEG:
      arm64_emit_word(e, arm64_neg(1, dst, a));
      break;
    case MIR_NOT:
      arm64_emit_word(e, arm64_mvn(1, dst, a));
      break;

    case MIR_SHL:
    case MIR_SHR:
    case MIR_SAR:
      if (in->b.kind == MIR_OPK_IMM) {
        int s = (int)in->b.imm;
        arm64_emit_word(e, in->op == MIR_SHL  ? arm64_lsl_imm(1, dst, a, s)
                           : in->op == MIR_SHR ? arm64_lsr_imm(1, dst, a, s)
                                               : arm64_asr_imm(1, dst, a, s));
      } else {
        Arm64Reg br = op_reg(e, &in->b, SCRATCH0);
        arm64_emit_word(e, in->op == MIR_SHL  ? arm64_lslv(1, dst, a, br)
                           : in->op == MIR_SHR ? arm64_lsrv(1, dst, a, br)
                                               : arm64_asrv(1, dst, a, br));
      }
      break;

    case MIR_CMP:
      if (imm_fits_u12(&in->b)) {
        arm64_emit_word(e, arm64_cmp_imm(1, a, (uint32_t)in->b.imm, 0));
      } else {
        arm64_emit_word(e, arm64_cmp_reg(1, a, op_reg(e, &in->b, SCRATCH0)));
      }
      break;

    case MIR_SETCC:
      arm64_emit_word(e, arm64_cset(1, dst, arm64_cond_from_x86_cc(in->cc)));
      break;

    case MIR_JMP:
      arm64_emit_b(e, label_id(e, &m, in->dst.sym));
      break;

    case MIR_JCC: /* test a; cc -> label  ==  cmp a,#0 ; b.cond label */
      arm64_emit_word(e, arm64_cmp_imm(1, a, 0, 0));
      arm64_emit_bcond(e, arm64_cond_from_x86_cc(in->cc),
                       label_id(e, &m, in->dst.sym));
      break;

    case MIR_CMPBR: /* cmp a,b ; cc -> label */
      if (imm_fits_u12(&in->b)) {
        arm64_emit_word(e, arm64_cmp_imm(1, a, (uint32_t)in->b.imm, 0));
      } else {
        arm64_emit_word(e, arm64_cmp_reg(1, a, op_reg(e, &in->b, SCRATCH1)));
      }
      arm64_emit_bcond(e, arm64_cond_from_x86_cc(in->cc),
                       label_id(e, &m, in->dst.sym));
      break;

    case MIR_TEST:
      arm64_emit_word(e, arm64_tst(1, a, op_reg(e, &in->b, SCRATCH0)));
      break;

    case MIR_CMOVCC: /* dst <- a if cc, else dst keeps its value */
      arm64_emit_word(e, arm64_csel(1, dst, op_reg(e, &in->a, SCRATCH0), dst,
                                    arm64_cond_from_x86_cc(in->cc)));
      break;

    case MIR_MOVZX: {
      Arm64Reg ar = op_reg(e, &in->a, SCRATCH0);
      arm64_emit_word(e, in->width == 1   ? arm64_uxtb(dst, ar)
                         : in->width == 2 ? arm64_uxth(dst, ar)
                                          : arm64_mov_reg(0, dst, ar));
      break;
    }
    case MIR_MOVSX: {
      Arm64Reg ar = op_reg(e, &in->a, SCRATCH0);
      arm64_emit_word(e, in->width == 1   ? arm64_sxtb(dst, ar)
                         : in->width == 2 ? arm64_sxth(dst, ar)
                                          : arm64_sxtw(dst, ar));
      break;
    }

    case MIR_IDIV:
    case MIR_DIV: {
      /* dst = a / b, or a % b when in->cc is set (the x86 RDX-result flag). */
      int uns = (in->op == MIR_DIV) || in->is_unsigned;
      Arm64Reg ar = op_reg(e, &in->a, SCRATCH0);
      Arm64Reg br = op_reg(e, &in->b, SCRATCH1);
      if (in->cc) {
        arm64_emit_word(e, uns ? arm64_udiv(1, SCRATCH0, ar, br)
                               : arm64_sdiv(1, SCRATCH0, ar, br));
        arm64_emit_word(e, arm64_msub(1, dst, SCRATCH0, br, ar));
      } else {
        arm64_emit_word(e, uns ? arm64_udiv(1, dst, ar, br)
                               : arm64_sdiv(1, dst, ar, br));
      }
      break;
    }

    case MIR_LABEL:
      arm64_bind_label(e, label_id(e, &m, in->dst.sym));
      break;

    case MIR_RET:
      if (!arm64_emit_epilogue(e, 0, NULL, 0)) {
        return 0;
      }
      break;

    default:
      e->error = 1;
      break;
    }
  }

  return e->error ? 0 : arm64_emit_finalize(e);
}

/* ---- vreg path: stack-home every value, consume mir_lower output --------- */

/* Scratch registers for the load-op-store model (volatile temps, distinct from
 * the x16/x17 immediate-materialization scratch). */
#define VREG_A ARM64_X9
#define VREG_B ARM64_X10
#define VREG_D ARM64_X11

/* Load an operand into `scratch`: a vreg from its frame slot, an immediate via
 * movz/movk, or a physical register passed through. */
static Arm64Reg vload(Arm64Emit *e, const MirOperand *op, Arm64Reg scratch) {
  if (op->kind == MIR_OPK_VREG) {
    arm64_emit_word(e, arm64_ldr_imm(1, scratch, ARM64_SP, 8 * op->vreg));
    return scratch;
  }
  if (op->kind == MIR_OPK_IMM) {
    emit_mov_imm(e, scratch, (uint64_t)op->imm);
    return scratch;
  }
  if (op->kind == MIR_OPK_PHYS) {
    return (Arm64Reg)op->phys;
  }
  e->error = 1;
  return scratch;
}

static void vstore(Arm64Emit *e, const MirOperand *dst, Arm64Reg src) {
  if (dst->kind == MIR_OPK_VREG) {
    arm64_emit_word(e, arm64_str_imm(1, src, ARM64_SP, 8 * dst->vreg));
  } else if (dst->kind == MIR_OPK_PHYS) {
    arm64_emit_mov(e, 1, (Arm64Reg)dst->phys, src);
  }
}

int arm64_mir_encode_vregs(Arm64Emit *e, const MirInst *insns, size_t count,
                           int nvregs, int nparams) {
  LabelMap m;
  m.count = 0;

  int frame = (nvregs * 8 + 15) & ~15;
  if (!arm64_emit_prologue(e, frame, NULL, 0)) {
    return 0;
  }
  /* Home incoming parameters x0.. into their vreg slots (vregs 0..nparams-1). */
  for (int i = 0; i < nparams && i < 8; i++) {
    arm64_emit_word(e, arm64_str_imm(1, (Arm64Reg)(ARM64_X0 + i), ARM64_SP,
                                     8 * i));
  }

  for (size_t i = 0; i < count && !e->error; i++) {
    const MirInst *in = &insns[i];

    switch (in->op) {
    case MIR_NOP:
      break;

    case MIR_MOV:
      vstore(e, &in->dst, vload(e, &in->a, VREG_A));
      break;

    case MIR_ADD:
    case MIR_SUB:
    case MIR_AND:
    case MIR_OR:
    case MIR_XOR:
    case MIR_IMUL:
    case MIR_SHL:
    case MIR_SHR:
    case MIR_SAR: {
      Arm64Reg ar = vload(e, &in->a, VREG_A);
      Arm64Reg br = vload(e, &in->b, VREG_B);
      uint32_t w = 0;
      switch (in->op) {
      case MIR_ADD: w = arm64_add_reg(1, VREG_D, ar, br); break;
      case MIR_SUB: w = arm64_sub_reg(1, VREG_D, ar, br); break;
      case MIR_AND: w = arm64_and_reg(1, VREG_D, ar, br); break;
      case MIR_OR:  w = arm64_orr_reg(1, VREG_D, ar, br); break;
      case MIR_XOR: w = arm64_eor_reg(1, VREG_D, ar, br); break;
      case MIR_IMUL: w = arm64_mul(1, VREG_D, ar, br); break;
      case MIR_SHL: w = arm64_lslv(1, VREG_D, ar, br); break;
      case MIR_SHR: w = arm64_lsrv(1, VREG_D, ar, br); break;
      default:      w = arm64_asrv(1, VREG_D, ar, br); break;
      }
      arm64_emit_word(e, w);
      vstore(e, &in->dst, VREG_D);
      break;
    }

    case MIR_NEG:
      arm64_emit_word(e, arm64_neg(1, VREG_D, vload(e, &in->a, VREG_A)));
      vstore(e, &in->dst, VREG_D);
      break;
    case MIR_NOT:
      arm64_emit_word(e, arm64_mvn(1, VREG_D, vload(e, &in->a, VREG_A)));
      vstore(e, &in->dst, VREG_D);
      break;

    case MIR_CMP: {
      Arm64Reg ar = vload(e, &in->a, VREG_A);
      arm64_emit_word(e, arm64_cmp_reg(1, ar, vload(e, &in->b, VREG_B)));
      break;
    }
    case MIR_TEST: {
      Arm64Reg ar = vload(e, &in->a, VREG_A);
      arm64_emit_word(e, arm64_tst(1, ar, vload(e, &in->b, VREG_B)));
      break;
    }
    case MIR_SETCC:
      arm64_emit_word(e, arm64_cset(1, VREG_D, arm64_cond_from_x86_cc(in->cc)));
      vstore(e, &in->dst, VREG_D);
      break;

    case MIR_JMP:
      arm64_emit_b(e, label_id(e, &m, in->dst.sym));
      break;
    case MIR_JCC:
      arm64_emit_word(e, arm64_cmp_imm(1, vload(e, &in->a, VREG_A), 0, 0));
      arm64_emit_bcond(e, arm64_cond_from_x86_cc(in->cc),
                       label_id(e, &m, in->dst.sym));
      break;
    case MIR_CMPBR: {
      Arm64Reg ar = vload(e, &in->a, VREG_A);
      arm64_emit_word(e, arm64_cmp_reg(1, ar, vload(e, &in->b, VREG_B)));
      arm64_emit_bcond(e, arm64_cond_from_x86_cc(in->cc),
                       label_id(e, &m, in->dst.sym));
      break;
    }
    case MIR_LABEL:
      arm64_bind_label(e, label_id(e, &m, in->dst.sym));
      break;

    case MIR_RET:
      if (in->a.kind != MIR_OPK_NONE) {
        arm64_emit_mov(e, 1, ARM64_X0, vload(e, &in->a, VREG_A));
      }
      if (!arm64_emit_epilogue(e, frame, NULL, 0)) {
        return 0;
      }
      break;

    default:
      e->error = 1;
      break;
    }
  }

  return e->error ? 0 : arm64_emit_finalize(e);
}
