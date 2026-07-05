#include "codegen/binary/mir.h"
#include "codegen/binary/mir_annotate.h"
#include "codegen/code_generator_internal.h"
#include "semantic/symbol_table.h"

#include <stdlib.h>
#include <string.h>

/* MIR (post-allocation) -> machine bytes in fn->context->code.
 *
 * Compute model: RAX is the primary scratch/accumulator and RCX the secondary;
 * RDX is reserved (future divide). Operand values come from their ALLOCATED
 * registers (or are materialized from a spill slot / immediate into a scratch),
 * never from per-temp stack homes — that is the whole point. Each MIR op
 * computes into RAX and writes the destination's register (or spill slot). The
 * extra reg-reg moves vs an optimal in-place scheme are cheap and removable
 * later; correctness first. */

/* Encoder scratch registers. R10/R11 are pure scratch — not allocatable, and
 * not ABI argument registers on EITHER Win64 or SysV — so RAX/RCX/RDX are freed
 * for the register allocator. Ops that need a HARDWARE register (divide's
 * RDX:RAX, variable shift's CL, setcc's byte target) name it explicitly. */
#define SCRATCH_A BINARY_GP_R10
#define SCRATCH_B BINARY_GP_R11
/* Float scratch (see MIR_XMM_POOL): XMM4 primary, XMM5 secondary. */
#define FSCRATCH_A BINARY_XMM4
#define FSCRATCH_B BINARY_XMM5

static int enc_err(MirFunction *fn, const char *msg) {
  if (fn->generator && !fn->generator->has_error) {
    code_generator_set_error(fn->generator, "%s in function '%s'", msg,
                             fn->context->function_name
                                 ? fn->context->function_name
                                 : "?");
  }
  fn->has_error = 1;
  return 0;
}

/* rbp-relative offset of a spilled vreg (mem = [rbp - offset]). */
static int spill_off(const MirVreg *v) { return v->spill_offset; }

/* Frame base register for stack slots: RSP when the frame pointer is omitted
 * (rbp is then free for allocation), otherwise RBP. */
static BinaryGpRegister frame_base(const MirFunction *fn) {
  return fn->context->omit_frame_pointer ? BINARY_GP_RSP : BINARY_GP_RBP;
}

/* Translate an rbp-relative displacement to the active frame base. With the
 * frame pointer omitted, rsp sits frame_size below where rbp would point, so
 * [rbp+d] == [rsp+frame_size+d]. */
static int frame_disp(const MirFunction *fn, int rbp_disp) {
  return fn->context->omit_frame_pointer ? rbp_disp + fn->context->frame_size
                                         : rbp_disp;
}

/* Emit: target <- value of `op`. */
static int materialize_into(MirFunction *fn, const MirOperand *op,
                            BinaryGpRegister target) {
  BinaryCodeBuffer *code = &fn->context->code;
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      if ((BinaryGpRegister)v->phys != target) {
        return binary_emit_mov_reg_reg(code, target, (BinaryGpRegister)v->phys);
      }
      return 1;
    }
    return binary_emit_mov_reg_mem(code, target, frame_base(fn),
                                   frame_disp(fn, -spill_off(v)));
  }
  case MIR_OPK_PHYS:
    if ((BinaryGpRegister)op->phys != target) {
      return binary_emit_mov_reg_reg(code, target, (BinaryGpRegister)op->phys);
    }
    return 1;
  case MIR_OPK_IMM:
    return binary_emit_mov_reg_imm64(code, target, (uint64_t)op->imm);
  case MIR_OPK_STACKHOME:
    return binary_emit_mov_reg_mem(code, target, frame_base(fn),
                                   frame_disp(fn, -op->disp));
  default:
    return enc_err(fn, "unsupported MIR operand in materialize");
  }
}

/* Return the physical register currently holding `op`'s value, materializing
 * into `scratch` when the operand is a spill/immediate/home. */
static BinaryGpRegister value_reg(MirFunction *fn, const MirOperand *op,
                                  BinaryGpRegister scratch, int *ok) {
  *ok = 1;
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      return (BinaryGpRegister)v->phys;
    }
    *ok = binary_emit_mov_reg_mem(&fn->context->code, scratch, frame_base(fn),
                                  frame_disp(fn, -spill_off(v)));
    return scratch;
  }
  case MIR_OPK_PHYS:
    return (BinaryGpRegister)op->phys;
  case MIR_OPK_IMM:
    *ok = binary_emit_mov_reg_imm64(&fn->context->code, scratch,
                                    (uint64_t)op->imm);
    return scratch;
  case MIR_OPK_STACKHOME:
    *ok = binary_emit_mov_reg_mem(&fn->context->code, scratch, frame_base(fn),
                                  frame_disp(fn, -op->disp));
    return scratch;
  default:
    *ok = enc_err(fn, "unsupported MIR operand as value");
    return scratch;
  }
}

/* Emit: dst <- value in src_phys. */
static int store_from(MirFunction *fn, const MirOperand *dst,
                      BinaryGpRegister src_phys) {
  BinaryCodeBuffer *code = &fn->context->code;
  switch (dst->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->in_register) {
      if ((BinaryGpRegister)v->phys != src_phys) {
        return binary_emit_mov_reg_reg(code, (BinaryGpRegister)v->phys,
                                       src_phys);
      }
      return 1;
    }
    return binary_emit_mov_mem_reg(code, frame_base(fn),
                                   frame_disp(fn, -spill_off(v)), src_phys);
  }
  case MIR_OPK_PHYS:
    if ((BinaryGpRegister)dst->phys != src_phys) {
      return binary_emit_mov_reg_reg(code, (BinaryGpRegister)dst->phys, src_phys);
    }
    return 1;
  default:
    return enc_err(fn, "unsupported MIR destination");
  }
}

/* ALU r/m,reg opcode bytes for the reg-reg ALU forms. */
static int alu_opcode(MirOpcode op, unsigned char *out) {
  switch (op) {
  case MIR_ADD: *out = 0x01; return 1;
  case MIR_SUB: *out = 0x29; return 1;
  case MIR_AND: *out = 0x21; return 1;
  case MIR_OR:  *out = 0x09; return 1;
  case MIR_XOR: *out = 0x31; return 1;
  default: return 0;
  }
}

static int alu_imm(MirFunction *fn, MirOpcode op, BinaryGpRegister reg,
                   long long imm) {
  BinaryCodeBuffer *code = &fn->context->code;
  uint32_t v = (uint32_t)imm;
  switch (op) {
  case MIR_ADD: return binary_emit_add_reg_imm32(code, reg, v);
  case MIR_SUB: return binary_emit_sub_reg_imm32(code, reg, v);
  case MIR_AND: return binary_emit_and_reg_imm32(code, reg, v);
  case MIR_OR:  return binary_emit_or_reg_imm32(code, reg, v);
  case MIR_XOR: return binary_emit_xor_reg_imm32(code, reg, v);
  default: return 0;
  }
}

/* Does `op` currently resolve to physical register D? (A register-resident
 * vreg or a fixed PHYS operand.) Immediates/spills/memory never alias D. */
static int operand_in_phys(MirFunction *fn, const MirOperand *op,
                           BinaryGpRegister D) {
  if (op->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[op->vreg];
    return v->in_register && (BinaryGpRegister)v->phys == D;
  }
  if (op->kind == MIR_OPK_PHYS) {
    return (BinaryGpRegister)op->phys == D;
  }
  return 0;
}

/* True (filling *reg) when `op` is already resident in a GP register, with no
 * spill reload or immediate materialization needed. */
static int operand_gp_reg(MirFunction *fn, const MirOperand *op,
                          BinaryGpRegister *reg) {
  if (op->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register && v->rclass == MIR_RC_GP) {
      *reg = (BinaryGpRegister)v->phys;
      return 1;
    }
    return 0;
  }
  if (op->kind == MIR_OPK_PHYS) {
    *reg = (BinaryGpRegister)op->phys;
    return 1;
  }
  return 0;
}

/* If `dst` is register-resident, write its physical register and return 1;
 * otherwise (spilled) return 0. */
static int dst_is_reg(MirFunction *fn, const MirOperand *dst,
                      BinaryGpRegister *D_out) {
  if (dst->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->in_register) {
      *D_out = (BinaryGpRegister)v->phys;
      return 1;
    }
    return 0;
  }
  if (dst->kind == MIR_OPK_PHYS) {
    *D_out = (BinaryGpRegister)dst->phys;
    return 1;
  }
  return 0;
}

/* Emit `target OP= x` for an integer ALU op. `x` must not alias `target` unless
 * the op is commutative (callers guarantee this). Uses the scratch register
 * that is not `target` to stage a spilled/wide-immediate `x`. */
static int emit_op_eq(MirFunction *fn, MirOpcode mop, unsigned char opc,
                      BinaryGpRegister target, const MirOperand *x) {
  BinaryCodeBuffer *code = &fn->context->code;
  if (x->kind == MIR_OPK_IMM &&
      code_generator_binary_immediate_fits_signed_32(x->imm)) {
    return alu_imm(fn, mop, target, x->imm) ? 1
                                            : enc_err(fn, "out of memory in ALU imm");
  }
  BinaryGpRegister scratch = (target == SCRATCH_A) ? SCRATCH_B : SCRATCH_A;
  int ok;
  BinaryGpRegister xr = value_reg(fn, x, scratch, &ok);
  if (!ok) {
    return 0;
  }
  return binary_emit_alu_reg_reg(code, opc, target, xr)
             ? 1
             : enc_err(fn, "out of memory in ALU");
}

/* dst = -a (MIR_NEG) or dst = ~a (MIR_NOT). One-source two-address: stage a in
 * the destination register (or RAX scratch for a spilled dst), then neg/not in
 * place. */
static int encode_neg_not(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister D;
  if (dst_is_reg(fn, &in->dst, &D)) {
    if (!operand_in_phys(fn, &in->a, D) && !materialize_into(fn, &in->a, D)) {
      return 0;
    }
    int ok = (in->op == MIR_NEG) ? binary_emit_neg_reg(code, D)
                                 : binary_emit_not_reg(code, D);
    return ok ? 1 : enc_err(fn, "out of memory in neg/not");
  }
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  int ok = (in->op == MIR_NEG) ? binary_emit_neg_reg(code, SCRATCH_A)
                               : binary_emit_not_reg(code, SCRATCH_A);
  if (!ok) {
    return enc_err(fn, "out of memory in neg/not");
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

static int encode_alu(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  unsigned char opc;
  if (!alu_opcode(in->op, &opc)) {
    return enc_err(fn, "bad ALU opcode");
  }
  int is_sub = (in->op == MIR_SUB);
  BinaryGpRegister D;

  if (dst_is_reg(fn, &in->dst, &D)) {
    /* `D = a + b` with neither operand already in D would otherwise be
     * `mov D, a; add D, b` (two instructions). When both operands are live in
     * GP registers, `lea D, [a + b]` does it in one and -- unlike register
     * coalescing -- changes nothing about allocation (D stays D), so it cannot
     * lengthen a live range or cause a spill. ADD only (LEA can't subtract);
     * the SIB index can't be RSP, so swap operands if needed (ADD commutes).
     * MIR consumes condition flags only through explicit CMP, so LEA not
     * setting flags is fine. */
    if (in->op == MIR_ADD && !operand_in_phys(fn, &in->a, D) &&
        !operand_in_phys(fn, &in->b, D)) {
      BinaryGpRegister ra, rb;
      if (operand_gp_reg(fn, &in->a, &ra) && operand_gp_reg(fn, &in->b, &rb)) {
        BinaryGpRegister base = ra, index = rb;
        if (index == BINARY_GP_RSP) {
          base = rb;
          index = ra;
        }
        if (index != BINARY_GP_RSP &&
            binary_emit_lea_reg_base_index_scale_disp(code, D, base, index, 1,
                                                      0)) {
          return 1;
        }
      }
    }
    if (operand_in_phys(fn, &in->b, D)) {
      /* b already occupies the destination register. */
      if (is_sub) {
        /* dst = a - b, b in D: stage a in RAX, subtract D, write back. */
        if (!materialize_into(fn, &in->a, SCRATCH_A) ||
            !binary_emit_alu_reg_reg(code, opc, SCRATCH_A, D)) {
          return enc_err(fn, "out of memory in sub");
        }
        return store_from(fn, &in->dst, SCRATCH_A);
      }
      /* commutative: D = D OP a == a OP b. */
      return emit_op_eq(fn, in->op, opc, D, &in->a);
    }
    /* b does not alias D: place a in D, then D OP= b. */
    if (!operand_in_phys(fn, &in->a, D) &&
        !materialize_into(fn, &in->a, D)) {
      return 0;
    }
    return emit_op_eq(fn, in->op, opc, D, &in->b);
  }

  /* Spilled destination: compute in RAX (no allocatable reg aliases it), store. */
  if (!materialize_into(fn, &in->a, SCRATCH_A) ||
      !emit_op_eq(fn, in->op, opc, SCRATCH_A, &in->b)) {
    return 0;
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

static int encode_imul(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int b_imm32 = in->b.kind == MIR_OPK_IMM &&
                code_generator_binary_immediate_fits_signed_32(in->b.imm);
  BinaryGpRegister D;

  if (dst_is_reg(fn, &in->dst, &D)) {
    int ok;
    if (b_imm32) {
      /* D = a * imm: three-operand imul reads a, writes D (a may equal D). */
      BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
      if (!ok ||
          !binary_emit_imul_reg_reg_imm32(code, D, areg, (uint32_t)in->b.imm)) {
        return enc_err(fn, "out of memory in imul imm");
      }
      return 1;
    }
    if (operand_in_phys(fn, &in->b, D)) {
      /* D holds b; D *= a (imul is commutative). */
      BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
      if (!ok || !binary_emit_imul_reg_reg(code, D, areg)) {
        return enc_err(fn, "out of memory in imul");
      }
      return 1;
    }
    if (!operand_in_phys(fn, &in->a, D) &&
        !materialize_into(fn, &in->a, D)) {
      return 0;
    }
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_A, &ok);
    if (!ok || !binary_emit_imul_reg_reg(code, D, breg)) {
      return enc_err(fn, "out of memory in imul");
    }
    return 1;
  }

  /* Spilled destination. */
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  if (b_imm32) {
    if (!binary_emit_imul_reg_reg_imm32(code, SCRATCH_A, SCRATCH_A,
                                        (uint32_t)in->b.imm)) {
      return enc_err(fn, "out of memory in imul imm");
    }
  } else {
    int ok;
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &ok);
    if (!ok || !binary_emit_imul_reg_reg(code, SCRATCH_A, breg)) {
      return enc_err(fn, "out of memory in imul");
    }
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

/* dst = a / b (quotient) or a % b (remainder when in->cc != 0). Signedness is
 * in->is_unsigned (the dividend's type): signed uses CQO + IDIV, unsigned uses
 * XOR(RDX) + DIV. Always 64-bit on the sign/zero-extended operands, which gives
 * the same result as a narrower divide. The dividend goes in RAX, RDX is the
 * high half, so the divisor must be staged out of RAX/RDX (now allocatable)
 * into a scratch register BEFORE the dividend is loaded into RAX. */
static int encode_div(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int rok;
  /* Resolve the divisor first (it might currently live in RAX or RDX, which the
   * dividend/high-half are about to overwrite); force it into SCRATCH_B then. */
  BinaryGpRegister divisor = value_reg(fn, &in->b, SCRATCH_B, &rok);
  if (!rok) {
    return 0;
  }
  if (divisor == BINARY_GP_RAX || divisor == BINARY_GP_RDX) {
    if (!binary_emit_mov_reg_reg(code, SCRATCH_B, divisor)) {
      return enc_err(fn, "out of memory staging divisor");
    }
    divisor = SCRATCH_B;
  }
  if (!materialize_into(fn, &in->a, BINARY_GP_RAX)) {
    return 0;
  }
  if (in->is_unsigned) {
    if (!binary_emit_xor_reg_reg32(code, BINARY_GP_RDX) ||
        !binary_emit_div_reg(code, divisor)) {
      return enc_err(fn, "out of memory in div");
    }
  } else {
    if (!binary_emit_cqo(code) || !binary_emit_idiv_reg(code, divisor)) {
      return enc_err(fn, "out of memory in idiv");
    }
  }
  BinaryGpRegister result = in->cc ? BINARY_GP_RDX : BINARY_GP_RAX;
  return store_from(fn, &in->dst, result);
}

/* dst = high 64 bits of (a * b). The multiplicand goes in RAX; the one-operand
 * mul/imul writes the full 128-bit product to RDX:RAX and we keep RDX. b is the
 * magic constant (IMM) or a register staged out of RAX/RDX (now allocatable)
 * into SCRATCH_B before RAX is loaded. is_unsigned selects mul. */
static int encode_mulhi(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister mreg;
  if (in->b.kind == MIR_OPK_IMM) {
    if (!binary_emit_mov_reg_imm64(code, SCRATCH_B, (uint64_t)in->b.imm)) {
      return enc_err(fn, "out of memory in mulhi imm");
    }
    mreg = SCRATCH_B;
  } else {
    int rok;
    mreg = value_reg(fn, &in->b, SCRATCH_B, &rok);
    if (!rok) {
      return 0;
    }
    if (mreg == BINARY_GP_RAX || mreg == BINARY_GP_RDX) {
      if (!binary_emit_mov_reg_reg(code, SCRATCH_B, mreg)) {
        return enc_err(fn, "out of memory staging multiplier");
      }
      mreg = SCRATCH_B;
    }
  }
  if (!materialize_into(fn, &in->a, BINARY_GP_RAX)) {
    return 0;
  }
  if (in->is_unsigned ? !binary_emit_mul_reg(code, mreg)
                      : !binary_emit_imul_reg(code, mreg)) {
    return enc_err(fn, "out of memory in mulhi");
  }
  return store_from(fn, &in->dst, BINARY_GP_RDX);
}

static int encode_shift(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  unsigned char sub = (in->op == MIR_SHL) ? 4 : (in->op == MIR_SHR) ? 5 : 7;
  BinaryGpRegister D;
  int dst_reg = dst_is_reg(fn, &in->dst, &D);
  BinaryGpRegister work = dst_reg ? D : SCRATCH_A;

  if (in->b.kind == MIR_OPK_IMM) {
    if ((dst_reg && !operand_in_phys(fn, &in->a, D) &&
         !materialize_into(fn, &in->a, work)) ||
        (!dst_reg && !materialize_into(fn, &in->a, work))) {
      return 0;
    }
    if (!binary_emit_shift_reg_imm8(code, sub, work,
                                    (unsigned char)(in->b.imm & 63))) {
      return enc_err(fn, "out of memory in shift imm");
    }
    return dst_reg ? 1 : store_from(fn, &in->dst, work);
  }
  /* Variable count: it must end up in CL (RCX). RCX is now allocatable, so the
   * value `a` may itself live in RCX, and the count may live anywhere. Stage the
   * value into SCRATCH_A first (reading it from wherever, RCX included), then
   * move the count into RCX (the value is already safe in SCRATCH_A), shift, and
   * store. The MIR layer marks a variable shift as an RCX clobber. */
  int ok;
  BinaryGpRegister cnt = value_reg(fn, &in->b, SCRATCH_B, &ok);
  if (!ok) {
    return 0;
  }
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  if (cnt != BINARY_GP_RCX &&
      !binary_emit_mov_reg_reg(code, BINARY_GP_RCX, cnt)) {
    return enc_err(fn, "out of memory moving shift count");
  }
  if (!binary_emit_shift_reg_cl(code, sub, SCRATCH_A)) {
    return enc_err(fn, "out of memory in shift");
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

static int encode_setcc(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  /* The compare reads a and b without modifying them, so use their own
   * registers directly. setcc requires an 8-bit-addressable low reg, so it
   * always targets AL and the result is zero-extended into RAX, then stored. */
  int ok;
  BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  /* A 4-byte (int32/uint32) compare must be 32-bit: MIR computes in 64-bit, so a
   * narrow operand can carry garbage in its high 32 bits; a 32-bit cmp ignores
   * them (the low 32 bits are the true value). An immediate is staged into a
   * register first since the 64-bit cmp-imm would sign-extend it. */
  if (in->width == 4) {
    /* A 32-bit immediate folds straight into the 32-bit cmp (no scratch reg);
     * the low 32 bits are the int32/uint32 constant being compared. */
    if (in->b.kind == MIR_OPK_IMM) {
      if (!binary_emit_cmp_reg_imm_w32(code, areg, (uint32_t)in->b.imm)) {
        return enc_err(fn, "out of memory in cmp32 imm");
      }
    } else {
      BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &ok);
      if (!ok || !binary_emit_cmp_reg_reg32(code, areg, breg)) {
        return enc_err(fn, "out of memory in cmp32");
      }
    }
  } else if (in->b.kind == MIR_OPK_IMM &&
             code_generator_binary_immediate_fits_signed_32(in->b.imm)) {
    if (!binary_emit_cmp_reg_imm32(code, areg, (uint32_t)in->b.imm)) {
      return enc_err(fn, "out of memory in cmp imm");
    }
  } else {
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &ok);
    if (!ok || !binary_emit_cmp_reg_reg(code, areg, breg)) {
      return enc_err(fn, "out of memory in cmp");
    }
  }
  if (!binary_emit_setcc_reg8(code, in->cc, BINARY_GP_RAX) ||
      !binary_emit_movzx_eax_al(code)) {
    return enc_err(fn, "out of memory in setcc");
  }
  /* setcc/movzx target AL/EAX specifically; the allocator marks SETCC as an RAX
   * clobber so no live value sits in RAX across it. */
  return store_from(fn, &in->dst, BINARY_GP_RAX);
}

/* dst <- extend(low `width` bytes of a) per signedness. Signed extensions emit
 * directly into the destination register (the reg-reg encoders always emit and
 * are correct in place). Unsigned narrowings and spilled destinations use the
 * RAX path with the dedicated AL/AX/EAX encoders (which always emit, unlike
 * mov_reg_reg32 which is a no-op when dst==src and would skip the zeroing). */
static int encode_extend(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int signed_ext = (in->op == MIR_MOVSX);
  BinaryGpRegister D;

  if (dst_is_reg(fn, &in->dst, &D)) {
    int ok;
    BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
    if (!ok) {
      return 0;
    }
    int done = 1;
    /* The width-4 unsigned form must be the ALWAYS-emitting 32-bit mov: the
     * canonicalizing `mov D32, D32` has dst == src, and the skip-when-equal
     * mov_reg_reg32 would silently drop the zero-extension. */
    switch (in->width) {
    case 4:
      done = signed_ext ? binary_emit_movsxd_reg_reg32(code, D, areg)
                        : binary_emit_movzx_reg_reg32(code, D, areg);
      break;
    case 2:
      done = signed_ext ? binary_emit_movsx_reg_reg16(code, D, areg)
                        : binary_emit_movzx_reg_reg16(code, D, areg);
      break;
    case 1:
      done = signed_ext ? binary_emit_movsx_reg_reg8(code, D, areg)
                        : binary_emit_movzx_reg_reg8(code, D, areg);
      break;
    default: return enc_err(fn, "bad extend width");
    }
    return done ? 1 : enc_err(fn, "out of memory in extend");
  }

  /* Scratch path (spilled destination): extend in SCRATCH_A using the general
   * reg-reg forms (no RAX dependency), then store. */
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  BinaryGpRegister S = SCRATCH_A;
  int ok = 1;
  switch (in->width) {
  case 4:
    ok = signed_ext ? binary_emit_movsxd_reg_reg32(code, S, S)
                    : binary_emit_movzx_reg_reg32(code, S, S);
    break;
  case 2:
    ok = signed_ext ? binary_emit_movsx_reg_reg16(code, S, S)
                    : binary_emit_movzx_reg_reg16(code, S, S);
    break;
  case 1:
    ok = signed_ext ? binary_emit_movsx_reg_reg8(code, S, S)
                    : binary_emit_movzx_reg_reg8(code, S, S);
    break;
  default:
    return enc_err(fn, "bad extend width");
  }
  if (!ok) {
    return enc_err(fn, "out of memory in extend");
  }
  return store_from(fn, &in->dst, S);
}

/* ---- float (XMM) operand plumbing -------------------------------------- */

static int dst_is_xmm_reg(MirFunction *fn, const MirOperand *dst,
                          BinaryXmmRegister *D_out) {
  if (dst->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->in_register) {
      *D_out = (BinaryXmmRegister)v->phys;
      return 1;
    }
    return 0;
  }
  if (dst->kind == MIR_OPK_PHYS) {
    *D_out = (BinaryXmmRegister)dst->phys;
    return 1;
  }
  return 0;
}

static int xmm_operand_in_phys(MirFunction *fn, const MirOperand *op,
                               BinaryXmmRegister D) {
  if (op->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[op->vreg];
    return v->in_register && (BinaryXmmRegister)v->phys == D;
  }
  if (op->kind == MIR_OPK_PHYS) {
    return (BinaryXmmRegister)op->phys == D;
  }
  return 0;
}

/* xmm dst <- xmm src, scalar (movss for width 4, movsd for width 8). */
static int xmm_mov(BinaryCodeBuffer *code, BinaryXmmRegister dst,
                   BinaryXmmRegister src, int width) {
  if (dst == src) {
    return 1;
  }
  /* movaps dst, src (0F 28 /r). A reg-reg movss/movsd MERGES into the
   * destination's upper lanes, creating a false dependency on its prior value
   * and defeating the rename-stage move-elimination; movaps copies the whole
   * register, so the copy is dependency-free and typically eliminated. We only
   * use the low lane, so copying all 128 bits is semantically irrelevant. */
  (void)width;
  return binary_emit_rex(code, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(code, 0x0F) &&
         binary_code_buffer_append_u8(code, 0x28) &&
         binary_code_buffer_append_u8(
             code, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* Load a float immediate's raw bits into an XMM register via a GP staging reg. */
static int xmm_load_fimm(MirFunction *fn, uint64_t bits,
                         BinaryXmmRegister target, int width) {
  BinaryCodeBuffer *code = &fn->context->code;
  if (width == 4) {
    return binary_emit_mov_reg_imm32_zero_extend(code, SCRATCH_A,
                                                 (uint32_t)bits) &&
           binary_emit_movd_xmm_reg(code, target, SCRATCH_A);
  }
  return binary_emit_mov_reg_imm64(code, SCRATCH_A, bits) &&
         binary_emit_movq_xmm_reg(code, target, SCRATCH_A);
}

/* Float spill slots are GP-width stack homes; reload/store via a GP reg so no
 * scalar-memory SSE encoders are needed. */
static int xmm_spill_load(MirFunction *fn, const MirVreg *v,
                          BinaryXmmRegister target) {
  unsigned char prefix = (v->width == 4) ? 0xF3 : 0xF2;
  return simd_emit_prefixed_xmm_mem_disp(&fn->context->code, prefix, 0x10,
                                         target, frame_base(fn),
                                         frame_disp(fn, -v->spill_offset));
}

static int xmm_spill_store(MirFunction *fn, const MirVreg *v,
                           BinaryXmmRegister src) {
  unsigned char prefix = (v->width == 4) ? 0xF3 : 0xF2;
  return simd_emit_prefixed_xmm_mem_disp(&fn->context->code, prefix, 0x11, src,
                                         frame_base(fn),
                                         frame_disp(fn, -v->spill_offset));
}

/* Resolve a float operand to the XMM register holding its value, materializing
 * a spill/immediate into `scratch`. */
static BinaryXmmRegister xmm_value(MirFunction *fn, const MirOperand *op,
                                   BinaryXmmRegister scratch, int width,
                                   int *ok) {
  *ok = 1;
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      return (BinaryXmmRegister)v->phys;
    }
    *ok = xmm_spill_load(fn, v, scratch);
    return scratch;
  }
  case MIR_OPK_PHYS:
    return (BinaryXmmRegister)op->phys;
  case MIR_OPK_FIMM:
    *ok = xmm_load_fimm(fn, (uint64_t)op->imm, scratch, width);
    return scratch;
  default:
    *ok = enc_err(fn, "unsupported float operand");
    return scratch;
  }
}

static int materialize_xmm_into(MirFunction *fn, const MirOperand *op,
                                BinaryXmmRegister target, int width) {
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      return xmm_mov(&fn->context->code, target, (BinaryXmmRegister)v->phys,
                     width);
    }
    return xmm_spill_load(fn, v, target);
  }
  case MIR_OPK_PHYS:
    return xmm_mov(&fn->context->code, target, (BinaryXmmRegister)op->phys,
                   width);
  case MIR_OPK_FIMM:
    return xmm_load_fimm(fn, (uint64_t)op->imm, target, width);
  default:
    return enc_err(fn, "unsupported float operand in materialize");
  }
}

static int xmm_store(MirFunction *fn, const MirOperand *dst,
                     BinaryXmmRegister src, int width) {
  switch (dst->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->in_register) {
      return xmm_mov(&fn->context->code, (BinaryXmmRegister)v->phys, src, width);
    }
    return xmm_spill_store(fn, v, src);
  }
  case MIR_OPK_PHYS:
    return xmm_mov(&fn->context->code, (BinaryXmmRegister)dst->phys, src, width);
  default:
    return enc_err(fn, "unsupported float destination");
  }
}

/* target OP= src for a scalar float op. */
static int sse_arith(MirFunction *fn, MirOpcode op, int width,
                     BinaryXmmRegister target, BinaryXmmRegister src) {
  BinaryCodeBuffer *code = &fn->context->code;
  if (width == 4) {
    switch (op) {
    case MIR_FADD: return binary_emit_addss_xmm_xmm(code, target, src);
    case MIR_FSUB: return binary_emit_subss_xmm_xmm(code, target, src);
    case MIR_FMUL: return binary_emit_mulss_xmm_xmm(code, target, src);
    case MIR_FDIV: return binary_emit_divss_xmm_xmm(code, target, src);
    default: return 0;
    }
  }
  switch (op) {
  case MIR_FADD: return binary_emit_addsd_xmm_xmm(code, target, src);
  case MIR_FSUB: return binary_emit_subsd_xmm_xmm(code, target, src);
  case MIR_FMUL: return binary_emit_mulsd_xmm_xmm(code, target, src);
  case MIR_FDIV: return binary_emit_divsd_xmm_xmm(code, target, src);
  default: return 0;
  }
}

static int encode_fbinop(MirFunction *fn, const MirInst *in) {
  int w = in->width;
  int commutative = (in->op == MIR_FADD || in->op == MIR_FMUL);
  BinaryXmmRegister D;
  int ok;

  if (dst_is_xmm_reg(fn, &in->dst, &D)) {
    if (xmm_operand_in_phys(fn, &in->b, D)) {
      if (!commutative) {
        /* D = a OP b, b in D: stage a in scratch, op b, move back to D. */
        if (!materialize_xmm_into(fn, &in->a, FSCRATCH_A, w) ||
            !sse_arith(fn, in->op, w, FSCRATCH_A, D) ||
            !xmm_mov(&fn->context->code, D, FSCRATCH_A, w)) {
          return enc_err(fn, "out of memory in float op");
        }
        return 1;
      }
      /* commutative: D = D OP a. */
      BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
      if (!ok || !sse_arith(fn, in->op, w, D, aval)) {
        return enc_err(fn, "out of memory in float op");
      }
      return 1;
    }
    if (!xmm_operand_in_phys(fn, &in->a, D) &&
        !materialize_xmm_into(fn, &in->a, D, w)) {
      return enc_err(fn, "out of memory in float op");
    }
    BinaryXmmRegister bval = xmm_value(fn, &in->b, FSCRATCH_A, w, &ok);
    if (!ok || !sse_arith(fn, in->op, w, D, bval)) {
      return enc_err(fn, "out of memory in float op");
    }
    return 1;
  }

  /* Spilled destination: compute in FSCRATCH_A (b may stage in FSCRATCH_B). */
  if (!materialize_xmm_into(fn, &in->a, FSCRATCH_A, w)) {
    return 0;
  }
  BinaryXmmRegister bval = xmm_value(fn, &in->b, FSCRATCH_B, w, &ok);
  if (!ok || !sse_arith(fn, in->op, w, FSCRATCH_A, bval)) {
    return enc_err(fn, "out of memory in float op");
  }
  return xmm_store(fn, &in->dst, FSCRATCH_A, w);
}

/* int -> float: dst(xmm) = cvtsi2sd/ss(a gp). in->width is the float width. */
static int encode_cvtsi2f(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_A;
  int done = (in->width == 4) ? binary_emit_cvtsi2ss_xmm_reg(code, target, areg)
                              : binary_emit_cvtsi2sd_xmm_reg(code, target, areg);
  if (!done) {
    return enc_err(fn, "out of memory in cvtsi2f");
  }
  return (target == FSCRATCH_A) ? xmm_store(fn, &in->dst, FSCRATCH_A, in->width)
                                : 1;
}

/* float -> int (truncating): dst(gp) = cvtt(a xmm). in->width is float width. */
static int encode_cvtf2si(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryXmmRegister xval = xmm_value(fn, &in->a, FSCRATCH_A, in->width, &ok);
  if (!ok) {
    return 0;
  }
  BinaryGpRegister D;
  BinaryGpRegister target = dst_is_reg(fn, &in->dst, &D) ? D : SCRATCH_A;
  int done = (in->width == 4) ? binary_emit_cvttss2si_reg_xmm(code, target, xval)
                              : binary_emit_cvttsd2si_reg_xmm(code, target, xval);
  if (!done) {
    return enc_err(fn, "out of memory in cvtf2si");
  }
  return (target == SCRATCH_A) ? store_from(fn, &in->dst, SCRATCH_A) : 1;
}

/* float -> float width change: in->width is the destination float width. */
static int encode_cvtf2f(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  int srcw = (in->width == 8) ? 4 : 8;
  BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, srcw, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_B;
  int done = (in->width == 8) ? binary_emit_cvtss2sd_xmm_xmm(code, target, aval)
                              : binary_emit_cvtsd2ss_xmm_xmm(code, target, aval);
  if (!done) {
    return enc_err(fn, "out of memory in cvtf2f");
  }
  return (target == FSCRATCH_B) ? xmm_store(fn, &in->dst, FSCRATCH_B, in->width)
                                : 1;
}

/* Load `size` bytes from [base (+ index*scale) + disp] straight into `target`,
 * sign/zero-extending to 64 bits in the SAME instruction (movsxd/movsx/movzx
 * from memory, or a plain mov for 8 bytes / unsigned 4). This is the general
 * shape win: every signed sub-word array read drops a separate movsx, and any
 * load whose destination already has a register skips the scratch bounce. */
static int emit_ext_load(BinaryCodeBuffer *code, BinaryGpRegister target,
                         BinaryGpRegister base, int has_index,
                         BinaryGpRegister index, int scale, int disp, int size,
                         int is_signed) {
  int rexw = 0, has2 = 0;
  unsigned char op1 = 0, op2 = 0;
  switch (size) {
  case 1:
    rexw = 1;
    has2 = 1;
    op1 = 0x0F;
    op2 = is_signed ? 0xBE : 0xB6; /* movsx/movzx r64, m8 */
    break;
  case 2:
    rexw = 1;
    has2 = 1;
    op1 = 0x0F;
    op2 = is_signed ? 0xBF : 0xB7; /* movsx/movzx r64, m16 */
    break;
  case 4:
    if (is_signed) {
      rexw = 1;
      op1 = 0x63; /* movsxd r64, m32 */
    } else {
      op1 = 0x8B; /* mov r32, m32 (zero-extends to 64) */
    }
    break;
  case 8:
    rexw = 1;
    op1 = 0x8B; /* mov r64, m64 */
    break;
  default:
    return 0;
  }
  if (has_index) {
    return binary_emit_memory_access_sib(code, 0, rexw, op1, has2, op2, target,
                                         base, index, scale, disp);
  }
  return binary_emit_memory_access_ex(code, 0, rexw, op1, has2, op2, target,
                                      base, disp);
}

static int encode_mov(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;

  /* Float moves: load/store via a GP staging reg (mov [mem]->RAX, movq/movd to
   * xmm and back), and reg-reg / float-immediate copies. */
  if (in->is_float) {
    int ok;
    int w = in->width;
    unsigned char prefix = (w == 4) ? 0xF3 : 0xF2; /* movss / movsd */
    if (in->a.kind == MIR_OPK_MEM) {
      /* float LOAD: movss/movsd dst <- [base], straight into dst's register. */
      MirOperand base = mir_op_vreg(in->a.mem.base);
      BinaryGpRegister addr = value_reg(fn, &base, SCRATCH_B, &ok);
      if (!ok) {
        return 0;
      }
      BinaryXmmRegister target;
      int direct = dst_is_xmm_reg(fn, &in->dst, &target);
      if (!direct) {
        target = FSCRATCH_A;
      }
      if (!simd_emit_prefixed_xmm_mem_disp(&ctx->code, prefix, 0x10, target,
                                           addr, 0)) {
        return enc_err(fn, "out of memory in float load");
      }
      return direct ? 1 : xmm_store(fn, &in->dst, FSCRATCH_A, w);
    }
    if (in->dst.kind == MIR_OPK_MEM) {
      /* float STORE: movss/movsd [base] <- a. */
      MirOperand base = mir_op_vreg(in->dst.mem.base);
      BinaryGpRegister addr = value_reg(fn, &base, SCRATCH_B, &ok);
      if (!ok) {
        return 0;
      }
      BinaryXmmRegister val = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
      if (!ok) {
        return 0;
      }
      if (!simd_emit_prefixed_xmm_mem_disp(&ctx->code, prefix, 0x11, val, addr,
                                           0)) {
        return enc_err(fn, "out of memory in float store");
      }
      return 1;
    }
    BinaryXmmRegister sval = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
    if (!ok) {
      return 0;
    }
    return xmm_store(fn, &in->dst, sval, w);
  }

  /* LOAD: dst <- [base (+ index*scale + disp)], width bytes. Load straight into
   * dst's register (extending in the same instruction); only bounce through
   * SCRATCH_A when dst is spilled. */
  if (in->a.kind == MIR_OPK_MEM) {
    int ok;
    int is_signed = !in->is_unsigned;
    BinaryGpRegister D;
    int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
    BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
    if (in->a.mem.index != MIR_VREG_NONE) {
      MirOperand bop = mir_op_vreg(in->a.mem.base);
      MirOperand iop = mir_op_vreg(in->a.mem.index);
      BinaryGpRegister base_reg = value_reg(fn, &bop, SCRATCH_B, &ok);
      if (!ok) {
        return 0;
      }
      /* Stage a spilled index in RDX, unless RDX is the load target. */
      BinaryGpRegister idx_scratch =
          (target == BINARY_GP_RDX) ? SCRATCH_B : BINARY_GP_RDX;
      BinaryGpRegister index_reg = value_reg(fn, &iop, idx_scratch, &ok);
      if (!ok) {
        return 0;
      }
      if (!emit_ext_load(&ctx->code, target, base_reg, 1, index_reg,
                         in->a.mem.scale, in->a.mem.disp, in->width,
                         is_signed)) {
        return enc_err(fn, "out of memory in scaled load");
      }
    } else {
      MirOperand base = mir_op_vreg(in->a.mem.base);
      BinaryGpRegister base_reg = value_reg(fn, &base, SCRATCH_B, &ok);
      if (!ok) {
        return 0;
      }
      if (!emit_ext_load(&ctx->code, target, base_reg, 0, BINARY_GP_RSP, 1,
                         in->a.mem.disp, in->width, is_signed)) {
        return enc_err(fn, "out of memory in load");
      }
    }
    if (!dst_in_reg) {
      return store_from(fn, &in->dst, SCRATCH_A);
    }
    return 1;
  }

  /* STORE: [base (+ index*scale + disp)] <- a, width bytes. */
  if (in->dst.kind == MIR_OPK_MEM) {
    int ok1, ok2;
    if (in->dst.mem.index != MIR_VREG_NONE) {
      /* Stage base in RCX and index in RDX, value in RAX. For 4/8-byte stores
       * emit one direct SIB `mov [base+idx*scale], val`; narrower widths lea
       * the address into RCX and store through it. RAX/RCX/RDX are all free
       * scratch inside a store encoding. */
      MirOperand bop = mir_op_vreg(in->dst.mem.base);
      MirOperand iop = mir_op_vreg(in->dst.mem.index);
      BinaryGpRegister base_reg = value_reg(fn, &bop, SCRATCH_B, &ok1);
      BinaryGpRegister index_reg = value_reg(fn, &iop, BINARY_GP_RDX, &ok2);
      if (!ok1 || !ok2) {
        return 0;
      }
      BinaryGpRegister val = value_reg(fn, &in->a, SCRATCH_A, &ok1);
      if (!ok1) {
        return 0;
      }
      if (in->width == 4 || in->width == 8) {
        if (!binary_emit_memory_access_sib(
                &ctx->code, 0, in->width == 8 ? 1 : 0, 0x89, 0, 0, val,
                base_reg, index_reg, in->dst.mem.scale, in->dst.mem.disp)) {
          return enc_err(fn, "out of memory in scaled store");
        }
        return 1;
      }
      if (!binary_emit_lea_reg_base_index_scale_disp(
              &ctx->code, SCRATCH_B, base_reg, index_reg, in->dst.mem.scale,
              in->dst.mem.disp)) {
        return enc_err(fn, "out of memory in scaled store address");
      }
      if (!code_generator_binary_emit_store_to_address(g, ctx, SCRATCH_B,
                                                       in->width, val)) {
        return enc_err(fn, "out of memory in store");
      }
      return 1;
    }
    MirOperand base = mir_op_vreg(in->dst.mem.base);
    BinaryGpRegister addr = value_reg(fn, &base, SCRATCH_B, &ok1);
    if (!ok1) {
      return 0;
    }
    BinaryGpRegister val = value_reg(fn, &in->a, SCRATCH_A, &ok2);
    if (!ok2) {
      return 0;
    }
    if (in->dst.mem.disp != 0) {
      /* Constant-index access: fold the byte displacement into the address.
       * lea into SCRATCH_B so a base held in a live vreg register is preserved
       * (value_reg returns that register directly when the base is not spilled). */
      if (!binary_emit_lea_reg_mem(&ctx->code, SCRATCH_B, addr,
                                   in->dst.mem.disp)) {
        return enc_err(fn, "out of memory in store address");
      }
      addr = SCRATCH_B;
    }
    if (!code_generator_binary_emit_store_to_address(g, ctx, addr, in->width,
                                                     val)) {
      return enc_err(fn, "out of memory in store");
    }
    return 1;
  }

  /* Plain register/immediate move. */
  int ok;
  BinaryGpRegister src = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  return store_from(fn, &in->dst, src);
}

/* ---- prologue / epilogue ------------------------------------------------ */

static int mir_has_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    /* MIR_TRAP also emits calls (puts/exit), so it needs outgoing shadow space
     * reserved at the bottom of the frame just like a MIR_CALL. */
    if (fn->insns[i].op == MIR_CALL ||
        fn->insns[i].op == MIR_CALL_INDIRECT ||
        fn->insns[i].op == MIR_TRAP) {
      return 1;
    }
  }
  return 0;
}

static int mir_layout_frame(MirFunction *fn) {
  /* Spill slots occupy [rbp-8 .. rbp-spill_bytes]; saved nonvolatiles sit
   * below them. If the function makes calls, 32 bytes of Win64 shadow space are
   * reserved at the very bottom of the frame (where rsp points), so an outgoing
   * call has shadow space and a 16-aligned rsp without adjusting rsp in-body.
   * frame_size is 16-aligned. */
  BinaryFunctionContext *ctx = fn->context;
  int spill = fn->spill_bytes;
  for (size_t i = 0; i < ctx->saved_register_count; i++) {
    ctx->saved_register_offsets[i] = spill + (int)((i + 1) * 8);
  }
  int after_gp = spill + (int)(ctx->saved_register_count * 8);
  /* Saved XMM nonvolatiles sit below the GP saves, 16 bytes (full movdqu) each. */
  for (size_t i = 0; i < ctx->saved_xmm_count; i++) {
    ctx->saved_xmm_offsets[i] = after_gp + (int)((i + 1) * 16);
  }
  int raw = after_gp + (int)(ctx->saved_xmm_count * 16);
  if (mir_has_calls(fn)) {
    /* Outgoing call region at the very bottom of the frame: the INDIRECT
     * struct-argument copy region (lowest, rsp-relative), then 32B Win64 shadow
     * space, then any outgoing stack-argument bytes (calls with more GP args
     * than argument registers). Spills/saves sit above and never reach it. */
    raw += fn->outgoing_indirect_bytes + 32 + fn->outgoing_stack_bytes;
  }
  if (!binary_align_up_int(raw, 16, &ctx->frame_size)) {
    return enc_err(fn, "stack frame too large");
  }
  ctx->raw_frame_size = raw;
  return 1;
}

/* Home one GP parameter from its incoming argument register into its vreg,
 * extending narrow signed/unsigned values to 64 bits. */
static int mir_home_gp_param(MirFunction *fn, const MirParam *p,
                             BinaryGpRegister arg) {
  BinaryCodeBuffer *code = &fn->context->code;
  MirOperand dst = mir_op_vreg(p->vreg);
  if (p->width == 8) {
    return store_from(fn, &dst, arg);
  }
  BinaryGpRegister D;
  if (dst_is_reg(fn, &dst, &D)) {
    int ok = 1;
    if (p->width == 4) {
      /* movzx_reg_reg32: must emit even when D == arg (the regalloc often
       * coalesces a param into its incoming register) — the skip-when-equal
       * mov would silently drop the uint32 canonicalization. */
      ok = p->is_signed ? binary_emit_movsxd_reg_reg32(code, D, arg)
                        : binary_emit_movzx_reg_reg32(code, D, arg);
    } else if (p->width == 2 && p->is_signed) {
      ok = binary_emit_movsx_reg_reg16(code, D, arg);
    } else if (p->width == 1 && p->is_signed) {
      ok = binary_emit_movsx_reg_reg8(code, D, arg);
    } else {
      ok = (p->width == 2) ? binary_emit_movzx_reg_reg16(code, D, arg)
                           : binary_emit_movzx_reg_reg8(code, D, arg);
    }
    return ok ? 1 : enc_err(fn, "out of memory extending parameter");
  }
  /* Spilled destination: extend arg into SCRATCH_A (general reg-reg forms), then
   * store. */
  BinaryGpRegister S = SCRATCH_A;
  int ok = 1;
  if (p->width == 4) {
    ok = p->is_signed ? binary_emit_movsxd_reg_reg32(code, S, arg)
                      : binary_emit_movzx_reg_reg32(code, S, arg);
  } else if (p->width == 2) {
    ok = p->is_signed ? binary_emit_movsx_reg_reg16(code, S, arg)
                      : binary_emit_movzx_reg_reg16(code, S, arg);
  } else if (p->width == 1) {
    ok = p->is_signed ? binary_emit_movsx_reg_reg8(code, S, arg)
                      : binary_emit_movzx_reg_reg8(code, S, arg);
  }
  if (!ok || !store_from(fn, &dst, S)) {
    return enc_err(fn, "out of memory extending parameter");
  }
  return 1;
}

/* Home one GP parameter passed on the caller's stack into its vreg. The slot is
 * a full 8-byte slot above saved-rbp+return-address (16) and the callee's shadow
 * space; the caller stored the (already-extended) value there, so an 8-byte load
 * matches the fallback emitter exactly. */
static int mir_home_gp_stack_param(MirFunction *fn, const MirParam *p,
                                   int rbp_offset) {
  BinaryCodeBuffer *code = &fn->context->code;
  MirOperand dst = mir_op_vreg(p->vreg);
  BinaryGpRegister D;
  if (dst_is_reg(fn, &dst, &D)) {
    return binary_emit_mov_reg_mem(code, D, frame_base(fn),
                                   frame_disp(fn, rbp_offset))
               ? 1
               : enc_err(fn, "out of memory homing stack parameter");
  }
  if (!binary_emit_mov_reg_mem(code, SCRATCH_A, frame_base(fn),
                               frame_disp(fn, rbp_offset))) {
    return enc_err(fn, "out of memory homing stack parameter");
  }
  return store_from(fn, &dst, SCRATCH_A);
}

/* A pending XMM->home move for float-parameter homing. */
typedef struct {
  BinaryXmmRegister src;
  int is_spill;
  int dst; /* xmm register (is_spill==0) or rbp-relative spill offset */
  int width;
  int done;
} MirXmmMove;

/* Home float parameters: incoming XMM arg registers -> param vregs. The arg
 * registers (XMM0..XMM3) are themselves allocatable, so this is a parallel
 * move: spill destinations are emitted first (they only read sources), then the
 * register->register permutation is resolved, breaking any cycle with the XMM
 * scratch register. All copies use movsd (low 64 bits) which preserves a scalar
 * float of either width. */
static int mir_home_float_params(MirFunction *fn, MirXmmMove *mv, int n) {
  BinaryCodeBuffer *code = &fn->context->code;
  /* Spill destinations first, while every source register is still intact. */
  for (int i = 0; i < n; i++) {
    if (!mv[i].is_spill) {
      continue;
    }
    int ok = (mv[i].width == 4)
                 ? (binary_emit_movd_reg_xmm(code, SCRATCH_A, mv[i].src) &&
                    binary_emit_mov_mem_reg32(code, frame_base(fn),
                                              frame_disp(fn, -mv[i].dst),
                                              SCRATCH_A))
                 : (binary_emit_movq_reg_xmm(code, SCRATCH_A, mv[i].src) &&
                    binary_emit_mov_mem_reg(code, frame_base(fn),
                                            frame_disp(fn, -mv[i].dst),
                                            SCRATCH_A));
    if (!ok) {
      return enc_err(fn, "out of memory homing float parameter");
    }
    mv[i].done = 1;
  }
  /* Register->register permutation. */
  int remaining = 0;
  for (int i = 0; i < n; i++) {
    if (!mv[i].done && (BinaryXmmRegister)mv[i].dst == mv[i].src) {
      mv[i].done = 1; /* already in place */
    }
    if (!mv[i].done) {
      remaining++;
    }
  }
  while (remaining > 0) {
    int progressed = 0;
    for (int i = 0; i < n; i++) {
      if (mv[i].done) {
        continue;
      }
      int dst_is_src = 0;
      for (int j = 0; j < n; j++) {
        if (!mv[j].done && j != i && mv[j].src == (BinaryXmmRegister)mv[i].dst) {
          dst_is_src = 1;
          break;
        }
      }
      if (!dst_is_src) {
        if (!binary_emit_sse_reg_reg(code, 0xF2, 0, 0x0F, 0x10,
                                     (BinaryXmmRegister)mv[i].dst, mv[i].src)) {
          return enc_err(fn, "out of memory homing float parameter");
        }
        mv[i].done = 1;
        remaining--;
        progressed = 1;
      }
    }
    if (progressed) {
      continue;
    }
    /* Pure cycle: save one destination's current value into the scratch XMM,
     * then redirect the move that consumes it to read the scratch. */
    int i;
    for (i = 0; i < n; i++) {
      if (!mv[i].done) {
        break;
      }
    }
    if (!binary_emit_sse_reg_reg(code, 0xF2, 0, 0x0F, 0x10, FSCRATCH_A,
                                 (BinaryXmmRegister)mv[i].dst)) {
      return enc_err(fn, "out of memory breaking float-param cycle");
    }
    for (int j = 0; j < n; j++) {
      if (!mv[j].done && mv[j].src == (BinaryXmmRegister)mv[i].dst) {
        mv[j].src = FSCRATCH_A;
      }
    }
    if (!binary_emit_sse_reg_reg(code, 0xF2, 0, 0x0F, 0x10,
                                 (BinaryXmmRegister)mv[i].dst, mv[i].src)) {
      return enc_err(fn, "out of memory homing float parameter");
    }
    mv[i].done = 1;
    remaining--;
  }
  return 1;
}

/* Home all parameters from their ABI incoming locations into their vregs. */
static int mir_home_parameters(MirFunction *fn) {
  size_t pc = fn->param_count;
  const BinaryAbi *abi = code_generator_binary_active_abi();
  /* An INDIRECT struct return prepends a hidden integer out-pointer argument
   * (Win64: RCX, SysV: RDI); home it into the reserved vreg and shift every
   * user parameter up one ABI slot in the layout. */
  size_t hidden = fn->returns_indirect ? 1 : 0;
  if (hidden) {
    if (fn->indirect_return_vreg != MIR_VREG_NONE &&
        fn->vregs[fn->indirect_return_vreg].assigned) {
      MirOperand dst = mir_op_vreg(fn->indirect_return_vreg);
      if (!store_from(fn, &dst, abi->indirect_return_register)) {
        return enc_err(fn, "out of memory homing indirect-return pointer");
      }
    }
  }
  if (pc == 0) {
    return 1;
  }
  int is_float[MIR_MAX_PARAMS + 1];
  BinaryArgLocation locs[MIR_MAX_PARAMS + 1];
  if (hidden) {
    is_float[0] = 0; /* hidden out-pointer is an integer arg */
  }
  for (size_t i = 0; i < pc; i++) {
    is_float[i + hidden] = fn->params[i].is_float;
  }
  if (!code_generator_binary_compute_arg_layout(abi, is_float, pc + hidden, locs,
                                                NULL)) {
    return enc_err(fn, "failed to compute parameter layout");
  }

  MirXmmMove xm[MIR_MAX_PARAMS];
  int nxm = 0;
  for (size_t i = 0; i < pc; i++) {
    const MirParam *p = &fn->params[i];
    if (!fn->vregs[p->vreg].assigned) {
      continue; /* unused parameter */
    }
    const BinaryArgLocation *loc = &locs[i + hidden];
    if (!p->is_float) {
      if (loc->kind == BINARY_ARG_IN_GP_REGISTER) {
        if (!mir_home_gp_param(fn, p, loc->gp_register)) {
          return 0;
        }
      } else if (loc->kind == BINARY_ARG_ON_STACK) {
        int rbp_offset = 16 + abi->shadow_space_size + loc->stack_offset;
        if (!mir_home_gp_stack_param(fn, p, rbp_offset)) {
          return 0;
        }
      } else {
        return enc_err(fn, "unsupported parameter location");
      }
    } else {
      if (loc->kind != BINARY_ARG_IN_XMM_REGISTER) {
        return enc_err(fn, "unsupported float parameter location");
      }
      MirVreg *vr = &fn->vregs[p->vreg];
      xm[nxm].src = loc->xmm_register;
      xm[nxm].width = p->width;
      xm[nxm].done = 0;
      if (vr->in_register) {
        xm[nxm].is_spill = 0;
        xm[nxm].dst = vr->phys;
      } else {
        xm[nxm].is_spill = 1;
        xm[nxm].dst = vr->spill_offset;
      }
      nxm++;
    }
  }
  return mir_home_float_params(fn, xm, nxm);
}

static int mir_emit_prologue(MirFunction *fn) {
  BinaryFunctionContext *ctx = fn->context;
  BinaryCodeBuffer *code = &ctx->code;
  if (ctx->omit_frame_pointer) {
    /* No rbp frame: fold the 8 bytes the saved-rbp slot used to occupy into the
     * allocation so rsp stays 16-aligned at calls (entry rsp == 8 mod 16, and
     * frame_size is 16-aligned, so +8 realigns to 0). rbp is now an ordinary
     * allocatable callee-saved register; if the allocator used it, the saved-
     * register loop below preserves it (caller's value is still intact here). */
    if (!binary_emit_frame_allocation(code, ctx->frame_size + 8)) {
      return enc_err(fn, "out of memory allocating frame");
    }
  } else {
    if (!binary_emit_push_reg(code, BINARY_GP_RBP) ||
        !binary_emit_mov_reg_reg(code, BINARY_GP_RBP, BINARY_GP_RSP)) {
      return enc_err(fn, "out of memory in prologue");
    }
    if (!binary_emit_frame_allocation(code, ctx->frame_size)) {
      return enc_err(fn, "out of memory allocating frame");
    }
  }
  for (size_t i = 0; i < ctx->saved_register_count; i++) {
    if (!binary_emit_mov_mem_reg(code, frame_base(fn),
                                 frame_disp(fn, -ctx->saved_register_offsets[i]),
                                 ctx->saved_registers[i])) {
      return enc_err(fn, "out of memory saving callee registers");
    }
  }
  for (size_t i = 0; i < ctx->saved_xmm_count; i++) {
    if (!simd_movdqu_mem_xmm_disp(code, frame_base(fn),
                                  frame_disp(fn, -ctx->saved_xmm_offsets[i]),
                                  ctx->saved_xmm_registers[i])) {
      return enc_err(fn, "out of memory saving callee xmm registers");
    }
  }
  if (!mir_home_parameters(fn)) {
    return 0;
  }
  return 1;
}

static int mir_emit_epilogue(MirFunction *fn) {
  BinaryFunctionContext *ctx = fn->context;
  BinaryCodeBuffer *code = &ctx->code;
  /* An inline vector kernel (e.g. MIR_SIMD_SLP_MAC) left the YMM upper halves
   * dirty; clear them once here so a caller running legacy SSE pays no AVX->SSE
   * transition penalty. Emitted per RET, but functions typically have one. */
  if (fn->used_inline_vector && !code_generator_binary_emit_vzeroupper(code)) {
    return enc_err(fn, "out of memory emitting epilogue vzeroupper");
  }
  for (size_t i = ctx->saved_xmm_count; i > 0; i--) {
    size_t j = i - 1;
    if (!simd_movdqu_xmm_mem_disp(code, ctx->saved_xmm_registers[j],
                                  frame_base(fn),
                                  frame_disp(fn, -ctx->saved_xmm_offsets[j]))) {
      return enc_err(fn, "out of memory restoring callee xmm registers");
    }
  }
  for (size_t i = ctx->saved_register_count; i > 0; i--) {
    size_t j = i - 1;
    if (!binary_emit_mov_reg_mem(code, ctx->saved_registers[j], frame_base(fn),
                                 frame_disp(fn,
                                            -ctx->saved_register_offsets[j]))) {
      return enc_err(fn, "out of memory restoring callee registers");
    }
  }
  if (ctx->omit_frame_pointer) {
    /* Slots are addressed off rsp, which is still at the frame bottom here, so
     * tear the frame down (the saved-rbp +8) and return. No pop rbp. */
    if (!binary_emit_add_rsp_imm32(code, (uint32_t)(ctx->frame_size + 8)) ||
        !binary_emit_ret(code)) {
      return enc_err(fn, "out of memory in epilogue");
    }
  } else if (!binary_emit_mov_reg_reg(code, BINARY_GP_RSP, BINARY_GP_RBP) ||
             !binary_emit_pop_reg(code, BINARY_GP_RBP) ||
             !binary_emit_ret(code)) {
    return enc_err(fn, "out of memory in epilogue");
  }
  return 1;
}

/* MIR_LOAD_GLOBAL: dst <- value of the read-only global named by in->a (SYMBOL).
 * Uses the const-table immediate when the global folds to a constant, otherwise
 * a RIP-relative load (which sign/zero-extends to the dst register width). */
static int encode_load_global(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;
  const char *name = in->a.sym;
  if (!name) {
    return enc_err(fn, "MIR_LOAD_GLOBAL without a symbol");
  }

  /* A float global is cached in an XMM vreg: load its raw bits into a GP scratch
   * (the RIP-relative load helper is GP-only) then movd/movq them into the XMM
   * lane. Float globals are never const-folded (see globals.c), so no immediate
   * branch is needed here. */
  if (in->dst.kind == MIR_OPK_VREG &&
      fn->vregs[in->dst.vreg].rclass == MIR_RC_XMM) {
    int width = fn->vregs[in->dst.vreg].width;
    const char *link = code_generator_get_link_symbol_name(g, name);
    Symbol *s =
        (g && g->symbol_table) ? symbol_table_lookup(g->symbol_table, name)
                               : NULL;
    if (!link || !link[0] || !s) {
      return enc_err(fn, "unresolved global in MIR_LOAD_GLOBAL");
    }
    if (!code_generator_binary_emit_global_symbol_load(g, ctx, link, s->type,
                                                       s->is_extern, SCRATCH_A)) {
      return enc_err(fn, "out of memory loading float global");
    }
    int moved = (width == 4)
                    ? binary_emit_movd_xmm_reg(&ctx->code, FSCRATCH_A, SCRATCH_A)
                    : binary_emit_movq_xmm_reg(&ctx->code, FSCRATCH_A, SCRATCH_A);
    if (!moved) {
      return enc_err(fn, "out of memory staging float global to xmm");
    }
    return xmm_store(fn, &in->dst, FSCRATCH_A, width);
  }

  BinaryGpRegister D;
  int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
  BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;

  uint64_t cval = 0;
  if (binary_global_const_table_get(name, &cval)) {
    if (!binary_emit_mov_reg_imm64(&ctx->code, target, cval)) {
      return enc_err(fn, "out of memory loading global constant");
    }
  } else {
    const char *link = code_generator_get_link_symbol_name(g, name);
    Symbol *s =
        (g && g->symbol_table) ? symbol_table_lookup(g->symbol_table, name)
                               : NULL;
    if (!link || !link[0] || !s) {
      return enc_err(fn, "unresolved global in MIR_LOAD_GLOBAL");
    }
    if (!code_generator_binary_emit_global_symbol_load(g, ctx, link, s->type,
                                                       s->is_extern, target)) {
      return enc_err(fn, "out of memory loading global");
    }
  }
  if (!dst_in_reg) {
    return store_from(fn, &in->dst, target);
  }
  return 1;
}

/* MIR_STORE_GLOBAL: global named by in->a (SYMBOL) <- value in in->b (vreg).
 * Writes a register-promoted global back to memory via a RIP-relative store of
 * the low `width` bytes. Symmetric to encode_load_global. */
static int encode_store_global(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;
  const char *name = in->a.sym;
  if (!name) {
    return enc_err(fn, "MIR_STORE_GLOBAL without a symbol");
  }
  BinaryGpRegister src;
  /* A float global is cached in an XMM vreg: pull its bits out of the XMM lane
   * into a GP scratch, then the RIP-relative store writes the low `size` bytes
   * (the GP store helper is GP-only). */
  if (in->b.kind == MIR_OPK_VREG &&
      fn->vregs[in->b.vreg].rclass == MIR_RC_XMM) {
    int width = fn->vregs[in->b.vreg].width;
    int xok = 1;
    BinaryXmmRegister xsrc = xmm_value(fn, &in->b, FSCRATCH_A, width, &xok);
    if (!xok) {
      return 0;
    }
    int moved = (width == 4)
                    ? binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A, xsrc)
                    : binary_emit_movq_reg_xmm(&ctx->code, SCRATCH_A, xsrc);
    if (!moved) {
      return enc_err(fn, "out of memory staging float global from xmm");
    }
    src = SCRATCH_A;
  } else {
    int rok = 1;
    src = value_reg(fn, &in->b, SCRATCH_A, &rok);
    if (!rok) {
      return 0;
    }
  }
  const char *link = code_generator_get_link_symbol_name(g, name);
  Symbol *s = (g && g->symbol_table) ? symbol_table_lookup(g->symbol_table, name)
                                     : NULL;
  if (!link || !link[0] || !s) {
    return enc_err(fn, "unresolved global in MIR_STORE_GLOBAL");
  }
  if (!code_generator_binary_emit_global_symbol_store(g, ctx, link, s->type,
                                                      s->is_extern, src)) {
    return enc_err(fn, "out of memory storing global");
  }
  return 1;
}

/* MIR index of the MIR_LABEL defining `name`, or -1. */
static int mir_encode_label_index(const MirFunction *fn, const char *name) {
  if (!name) {
    return -1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
        strcmp(in->dst.sym, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

int mir_encode(MirFunction *fn) {
  if (!fn || !fn->context) {
    return 0;
  }
  BinaryFunctionContext *ctx = fn->context;

  /* --annotate-asm: byte offsets are reported relative to this function's start
   * (the context's code buffer may already hold earlier functions). */
  size_t annot_base = ctx->code.size;
  int annot = mir_annotate_enabled();

  if (!mir_layout_frame(fn) || !mir_emit_prologue(fn)) {
    return 0;
  }
  if (annot && ctx->code.size > annot_base) {
    mir_annotate_record_synthetic("prologue", "frame", 0,
                                  ctx->code.size - annot_base,
                                  ctx->code.data + annot_base);
  }

  /* Loop-header alignment: a label that is the target of a BACKWARD branch is a
   * loop top; pad it to a 16-byte boundary (like gcc -falign-loops) so the hot
   * loop's instruction fetch does not depend on where the function happened to
   * land. The pad NOPs sit BEFORE the label, so a back-edge (which jumps to the
   * label, past the pad) never executes them; only a fall-through into the loop
   * pays them, once. Pure performance -- it cannot change behaviour. */
  char *align_label = (char *)calloc(fn->insn_count ? fn->insn_count : 1, 1);
  if (align_label) {
    for (size_t b = 0; b < fn->insn_count; b++) {
      const MirInst *in = &fn->insns[b];
      if (in->op != MIR_JMP && in->op != MIR_JCC && in->op != MIR_CMPBR &&
          in->op != MIR_FCMPBR) {
        continue;
      }
      if (in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
        continue;
      }
      int d = mir_encode_label_index(fn, in->dst.sym);
      if (d >= 0 && (size_t)d < b) {
        align_label[d] = 1;
      }
    }
  }

  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    int ok = 1;
    size_t annot_off = ctx->code.size;
    if (in->op == MIR_LABEL && align_label && align_label[i]) {
      while (ctx->code.size % 16 != 0) {
        if (!binary_code_buffer_append_u8(&ctx->code, 0x90)) {
          ok = 0;
          break;
        }
      }
    }
    if (!ok) {
      free(align_label);
      return 0;
    }
    switch (in->op) {
    case MIR_NOP:
      break;
    case MIR_MOV:
      ok = encode_mov(fn, in);
      break;
    case MIR_ADD:
    case MIR_SUB:
    case MIR_AND:
    case MIR_OR:
    case MIR_XOR:
      ok = encode_alu(fn, in);
      break;
    case MIR_IMUL:
      ok = encode_imul(fn, in);
      break;
    case MIR_NEG:
    case MIR_NOT:
      ok = encode_neg_not(fn, in);
      break;
    case MIR_IDIV:
      ok = encode_div(fn, in);
      break;
    case MIR_MULHI:
      ok = encode_mulhi(fn, in);
      break;
    case MIR_SHL:
    case MIR_SHR:
    case MIR_SAR:
      ok = encode_shift(fn, in);
      break;
    case MIR_SETCC:
      ok = encode_setcc(fn, in);
      break;
    case MIR_MOVZX:
    case MIR_MOVSX:
      ok = encode_extend(fn, in);
      break;
    case MIR_LOAD_GLOBAL:
      ok = encode_load_global(fn, in);
      break;
    case MIR_STORE_GLOBAL:
      ok = encode_store_global(fn, in);
      break;
    case MIR_FADD:
    case MIR_FSUB:
    case MIR_FMUL:
    case MIR_FDIV:
      ok = encode_fbinop(fn, in);
      break;
    case MIR_CVTSI2F:
      ok = encode_cvtsi2f(fn, in);
      break;
    case MIR_CVTF2SI:
      ok = encode_cvtf2si(fn, in);
      break;
    case MIR_CVTF2F:
      ok = encode_cvtf2f(fn, in);
      break;
    case MIR_FSETCC: {
      int rok;
      BinaryXmmRegister av = xmm_value(fn, &in->a, FSCRATCH_A, in->width, &rok);
      if (!rok) { ok = 0; break; }
      BinaryXmmRegister bv = xmm_value(fn, &in->b, FSCRATCH_B, in->width, &rok);
      if (!rok) { ok = 0; break; }
      int cmp = (in->width == 4)
                    ? binary_emit_ucomiss_xmm_xmm(&ctx->code, av, bv)
                    : binary_emit_ucomisd_xmm_xmm(&ctx->code, av, bv);
      if (!cmp || !binary_emit_setcc_reg8(&ctx->code, in->cc, BINARY_GP_RAX) ||
          !binary_emit_movzx_eax_al(&ctx->code)) {
        ok = enc_err(fn, "out of memory in fsetcc");
        break;
      }
      ok = store_from(fn, &in->dst, BINARY_GP_RAX); /* result in RAX (movzx) */
      break;
    }
    case MIR_FCMPBR: {
      int rok;
      BinaryXmmRegister av = xmm_value(fn, &in->a, FSCRATCH_A, in->width, &rok);
      if (!rok) { ok = 0; break; }
      BinaryXmmRegister bv = xmm_value(fn, &in->b, FSCRATCH_B, in->width, &rok);
      if (!rok) { ok = 0; break; }
      int cmp = (in->width == 4)
                    ? binary_emit_ucomiss_xmm_xmm(&ctx->code, av, bv)
                    : binary_emit_ucomisd_xmm_xmm(&ctx->code, av, bv);
      size_t off = 0;
      if (!cmp || !binary_emit_jcc_placeholder(&ctx->code, in->cc, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in fcmpbr");
      }
      break;
    }
    case MIR_LABEL:
      if (!binary_label_table_define(&ctx->labels, in->dst.sym,
                                     ctx->code.size)) {
        ok = enc_err(fn, "duplicate label");
      }
      break;
    case MIR_JMP: {
      size_t off = 0;
      if (!binary_emit_jmp_placeholder(&ctx->code, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in jmp");
      }
      break;
    }
    case MIR_JCC: {
      /* test cond; je/jcc label. The test only reads the condition, so use its
       * own register directly (staging into RAX only when spilled/immediate). */
      int rok;
      BinaryGpRegister creg = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok || !binary_emit_test_reg_reg(&ctx->code, creg)) {
        ok = enc_err(fn, "out of memory in branch test");
        break;
      }
      size_t off = 0;
      if (!binary_emit_jcc_placeholder(&ctx->code, in->cc, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in branch");
      }
      break;
    }
    case MIR_CALL: {
      /* rsp already points at the reserved shadow space (set by the prologue),
       * so just emit the relocated call. Arguments were moved into ABI
       * registers by preceding MIR_MOVs; the return value is consumed by the
       * following MIR_MOV from RAX/XMM0. */
      const char *link =
          code_generator_get_link_symbol_name(fn->generator, in->dst.sym);
      size_t off = 0;
      if (!link || !binary_emit_call_placeholder(&ctx->code, &off) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, link, off)) {
        ok = enc_err(fn, "out of memory emitting call");
      }
      break;
    }
    case MIR_CALL_INDIRECT: {
      /* Same frame contract as MIR_CALL: the prologue reserved shadow/stack
       * argument space, and regalloc kept the target out of any argument
       * register clobbered by the preceding marshalling moves. */
      int rok;
      BinaryGpRegister target = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok || !binary_emit_call_reg(&ctx->code, target)) {
        ok = enc_err(fn, "out of memory emitting indirect call");
      }
      break;
    }
    case MIR_SIMD_SLP_MAC: {
      /* Inline SLP MAC kernel. The preceding MIR_MOVs marshalled a/b/out element
       * pointers into RCX/RDX/R8, the k count into R9, and the byte row stride
       * into RAX; emit the pure inner loop (no operand loads, so it needs no
       * coherent fallback stack homes). dst.imm = K (4/8); width = b's element
       * size (1 = int8-widening kernel, 4 = int32 kernel). */
      if (in->width == 1
              ? !code_generator_binary_emit_simd_slp_mac_i8_loop(&ctx->code,
                                                                 in->dst.imm)
              : !code_generator_binary_emit_simd_slp_mac_i32_loop(&ctx->code,
                                                                  in->dst.imm)) {
        ok = enc_err(fn, "out of memory emitting inline SLP MAC kernel");
      }
      fn->used_inline_vector = 1;
      break;
    }
    case MIR_SIMD_FILL: {
      /* Inline fill kernel. The preceding MIR_MOVs marshalled the base pointer
       * into RCX, the element count (mode 0) or end pointer (mode 1) into R8, and
       * the fill value into RAX; emit the splat-build + 16-byte-store loop +
       * scalar tail (no operand loads, no live-iv write-back). dst.imm = element
       * size (1/2/4/8); a.imm = mode (0 element-counted, 1 byte-walk). The kernel
       * uses VEX.128 stores (upper YMM lanes zeroed), so no vzeroupper is needed
       * and used_inline_vector stays unset. */
      int fok = code_generator_binary_emit_simd_fill_splat(&ctx->code,
                                                           in->dst.imm);
      if (fok) {
        fok = (in->a.imm == 0)
                  ? code_generator_binary_emit_simd_fill_loop_mode0(&ctx->code,
                                                                    in->dst.imm)
                  : code_generator_binary_emit_simd_fill_loop_bytewalk(
                        &ctx->code, in->dst.imm, 1);
      }
      if (!fok) {
        ok = enc_err(fn, "out of memory emitting inline fill kernel");
      }
      break;
    }
    case MIR_SIMD_AFFINE_MAP_F32: {
      /* Inline float32 affine map. The preceding MIR_MOVs marshalled src->RCX,
       * dst->RDX, count->R8; dst.imm/a.imm/b.imm hold the a/b/c coefficient float
       * bits and cc holds b_is_one|b_is_zero<<1|c_is_zero<<2. The kernel emits
       * its own closing vzeroupper, so used_inline_vector stays unset. */
      if (!code_generator_binary_emit_simd_affine_map_f32_inline(
              &ctx->code, (unsigned)in->dst.imm, (unsigned)in->a.imm,
              (unsigned)in->b.imm, (in->cc & 1) != 0, (in->cc & 2) != 0,
              (in->cc & 4) != 0)) {
        ok = enc_err(fn, "out of memory emitting inline affine-map kernel");
      }
      break;
    }
    case MIR_SIMD_AFFINE_MAP_F64: {
      /* Inline float64 affine map. The preceding MIR_MOVs marshalled src->RCX,
       * dst->RDX, count->R8; dst.imm/a.imm/b.imm hold the a/b/c coefficient
       * double bits and cc holds b_is_one|b_is_zero<<1|c_is_zero<<2. The kernel
       * emits its own closing vzeroupper, so used_inline_vector stays unset. */
      if (!code_generator_binary_emit_simd_affine_map_f64_inline(
              &ctx->code, (unsigned long long)in->dst.imm,
              (unsigned long long)in->a.imm, (unsigned long long)in->b.imm,
              (in->cc & 1) != 0, (in->cc & 2) != 0, (in->cc & 4) != 0,
              (in->cc & 8) != 0)) {
        ok = enc_err(fn, "out of memory emitting inline f64 affine-map kernel");
      }
      break;
    }
    case MIR_SIMD_VLOOP: {
      /* Inline general vloop (float64 map). The preceding MIR_MOVs marshalled the
       * base pointers + count into the ABI arg registers; the DAG comes from the
       * borrowed IRInstruction in `aux`. operands_marshaled=1 makes the kernel
       * read them from registers instead of the operands' stack homes. */
      const IRInstruction *vir = (const IRInstruction *)in->aux;
      if (!vir || !code_generator_binary_emit_simd_vloop_f64(
                      fn->generator, fn->context, vir, 1)) {
        ok = enc_err(fn, "out of memory emitting inline vloop kernel");
      }
      break;
    }
    case MIR_SIMD_SILU_F32: {
      /* Inline SiLU/SwiGLU gate. g/out->RCX, u->RDX, count->R8 marshalled by the
       * preceding MIR_MOVs; dst.imm = has_mul. The kernel emits its own closing
       * vzeroupper, so used_inline_vector stays unset. */
      if (!code_generator_binary_emit_simd_silu_f32_inline(&ctx->code,
                                                           (int)in->dst.imm)) {
        ok = enc_err(fn, "out of memory emitting inline SiLU kernel");
      }
      break;
    }
    case MIR_STORE_OUTARG: {
      /* Store an outgoing stack call argument to [rsp + b.imm]. rsp is fixed
       * after the prologue and the outgoing region is reserved there, so this
       * is a plain rsp-relative store. */
      int ok;
      BinaryGpRegister r = value_reg(fn, &in->a, SCRATCH_A, &ok);
      if (!ok) {
        break;
      }
      if (!binary_emit_mov_mem_reg(&ctx->code, BINARY_GP_RSP, (int)in->b.imm,
                                   r)) {
        ok = enc_err(fn, "out of memory storing outgoing call argument");
      }
      break;
    }
    case MIR_CMOV: {
      /* dst = (a != 0) ? b : dst. dst was pre-loaded with the else value by a
       * preceding MIR_MOV, so `test a; cmovnz dst, b` completes the select.
       * A spilled dst is staged through SCRATCH_A; cond stages through
       * SCRATCH_B, and `then` reuses SCRATCH_B (cond is dead after the test). */
      int rok;
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = D;
      if (!dst_in_reg) {
        const MirVreg *v = &fn->vregs[in->dst.vreg];
        target = SCRATCH_A;
        if (!binary_emit_mov_reg_mem(&ctx->code, SCRATCH_A, frame_base(fn),
                                     frame_disp(fn, -spill_off(v)))) {
          ok = enc_err(fn, "out of memory loading cmov dst");
          break;
        }
      }
      BinaryGpRegister creg = value_reg(fn, &in->a, SCRATCH_B, &rok);
      if (!rok || !binary_emit_test_reg_reg(&ctx->code, creg)) {
        ok = enc_err(fn, "out of memory in cmov test");
        break;
      }
      BinaryGpRegister treg = value_reg(fn, &in->b, SCRATCH_B, &rok);
      if (!rok ||
          !binary_emit_cmovcc_reg_reg(&ctx->code, 0x45, target, treg)) {
        ok = enc_err(fn, "out of memory in cmov");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, target);
      }
      break;
    }
    case MIR_PREFETCH: {
      /* prefetcht0 [base + disp]: advisory, no destination. The address vreg
       * is a plain read; a spilled address stages through SCRATCH_A. */
      if (in->a.kind != MIR_OPK_MEM || in->a.mem.index != MIR_VREG_NONE) {
        ok = enc_err(fn, "MIR_PREFETCH expects a base-only memory operand");
        break;
      }
      int prok;
      MirOperand pbop = mir_op_vreg(in->a.mem.base);
      BinaryGpRegister pbase = value_reg(fn, &pbop, SCRATCH_A, &prok);
      if (!prok) {
        break;
      }
      if (!binary_emit_prefetcht0_mem(&ctx->code, pbase, in->a.mem.disp)) {
        ok = enc_err(fn, "out of memory in prefetch");
      }
      break;
    }
    case MIR_LEA: {
      /* dst <- address of [base + index*scale + disp]. base/index are vregs
       * (index optional). Mirrors the scaled-LOAD address staging but
       * materializes the address instead of dereferencing it. Emitted by the
       * SLP-kernel lowering to form effective element pointers. */
      if (in->a.kind != MIR_OPK_MEM) {
        ok = enc_err(fn, "MIR_LEA expects a memory operand");
        break;
      }
      int rok;
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      MirOperand bop = mir_op_vreg(in->a.mem.base);
      BinaryGpRegister base_reg = value_reg(fn, &bop, SCRATCH_B, &rok);
      if (!rok) {
        break;
      }
      if (in->a.mem.index != MIR_VREG_NONE) {
        MirOperand iop = mir_op_vreg(in->a.mem.index);
        BinaryGpRegister idx_scratch =
            (target == BINARY_GP_RDX) ? SCRATCH_B : BINARY_GP_RDX;
        BinaryGpRegister index_reg = value_reg(fn, &iop, idx_scratch, &rok);
        if (!rok) {
          break;
        }
        if (!binary_emit_lea_reg_base_index_scale_disp(
                &ctx->code, target, base_reg, index_reg, in->a.mem.scale,
                in->a.mem.disp)) {
          ok = enc_err(fn, "out of memory in scaled lea");
          break;
        }
      } else if (!binary_emit_lea_reg_mem(&ctx->code, target, base_reg,
                                          in->a.mem.disp)) {
        ok = enc_err(fn, "out of memory in lea");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_OUTARG: {
      /* dst <- lea &slot in the INDIRECT struct-arg copy region. That region
       * sits ABOVE the Win64 shadow space and the outgoing stack args (so a
       * callee writing its shadow at [rsp..rsp+32] cannot clobber the copies),
       * hence the absolute rsp offset is shadow + outgoing_stack_bytes + the
       * per-arg slot offset (in->a.imm). rsp is fixed after the prologue. */
      const BinaryAbi *oa = code_generator_binary_active_abi();
      int off = oa->shadow_space_size + fn->outgoing_stack_bytes + (int)in->a.imm;
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!binary_emit_lea_reg_mem(&ctx->code, target, BINARY_GP_RSP, off)) {
        ok = enc_err(fn, "out of memory in lea outarg");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_GLOBAL: {
      /* dst <- RIP-relative address of global symbol a.sym. is_unsigned carries
       * the declare-external flag (set by lowering from the symbol). */
      const char *name = in->a.sym ? in->a.sym : "";
      const char *link = code_generator_get_link_symbol_name(fn->generator, name);
      if (!link || link[0] == '\0') {
        ok = enc_err(fn, "invalid global symbol in address-of");
        break;
      }
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!code_generator_binary_emit_symbol_address(fn->generator, ctx, link,
                                                     in->is_unsigned, target)) {
        ok = enc_err(fn, "out of memory emitting global address");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_FUNC: {
      /* dst <- RIP-relative address of function symbol a.sym. This shares the
       * same relocation path as global addresses; the linker resolves the code
       * symbol and the function pointer receives that address. */
      const char *name = in->a.sym ? in->a.sym : "";
      const char *link = code_generator_get_link_symbol_name(fn->generator, name);
      if (!link || link[0] == '\0') {
        ok = enc_err(fn, "invalid function symbol in address-of");
        break;
      }
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!code_generator_binary_emit_symbol_address(fn->generator, ctx, link,
                                                     in->is_unsigned, target)) {
        ok = enc_err(fn, "out of memory emitting function address");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_LOCAL: {
      /* dst <- address of local vreg a's stack home. The allocator forces an
       * address-taken value to spill, so a is always memory-resident. */
      const MirVreg *lv = &fn->vregs[in->a.vreg];
      if (lv->in_register) {
        ok = enc_err(fn, "address-taken value was not spilled");
        break;
      }
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!binary_emit_lea_reg_mem(&ctx->code, target, frame_base(fn),
                                   frame_disp(fn, -lv->spill_offset))) {
        ok = enc_err(fn, "out of memory emitting local address");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_CSTR: {
      /* dst <- address of the string literal a.sym (RIP-relative lea into a
       * .rdata cstring). dst is typically an ABI argument register. */
      const char *s = in->a.sym ? in->a.sym : "";
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!code_generator_binary_emit_cstring_literal_address(fn->generator, ctx,
                                                              s, target)) {
        ok = enc_err(fn, "out of memory emitting cstring argument");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_TRAP: {
      /* Terminal abort for a failed safety check. MIR only runs without
       * stack-trace support, so this is the degraded path: puts(message) +
       * exit(1) (matching code_generator_binary_emit_runtime_trap_call). rsp
       * already sits on the reserved shadow space (mir_has_calls counts
       * MIR_TRAP), so the calls need no rsp adjustment. The sequence never
       * returns; it is reached only on the cold guard-fail branch. */
      const BinaryAbi *abi = code_generator_binary_active_abi();
      BinaryGpRegister arg0 = abi->int_param_registers[0];
      const char *msg = in->a.sym ? in->a.sym : "";
      size_t off = 0;
      if (!code_generator_binary_declare_external_symbol(fn->generator, "puts") ||
          !code_generator_binary_declare_external_symbol(fn->generator, "exit")) {
        ok = enc_err(fn, "out of memory declaring trap externals");
        break;
      }
      if (!code_generator_binary_emit_cstring_literal_address(fn->generator, ctx,
                                                              msg, arg0)) {
        ok = enc_err(fn, "out of memory emitting trap message");
        break;
      }
      if (!binary_emit_call_placeholder(&ctx->code, &off) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, "puts",
                                            off)) {
        ok = enc_err(fn, "out of memory emitting trap puts");
        break;
      }
      if (!binary_emit_mov_reg_imm64(&ctx->code, arg0, 1)) {
        ok = enc_err(fn, "out of memory emitting trap exit arg");
        break;
      }
      off = 0;
      if (!binary_emit_call_placeholder(&ctx->code, &off) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, "exit",
                                            off)) {
        ok = enc_err(fn, "out of memory emitting trap exit");
        break;
      }
      break;
    }
    case MIR_CMPBR: {
      /* cmp a,b ; j<cc> label  (fused compare-and-branch). */
      int rok;
      BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok) {
        ok = 0;
        break;
      }
      if (in->width == 4) {
        /* 4-byte (int32/uint32) compare: 32-bit cmp ignores garbage high bits a
         * 64-bit MIR value may carry (see encode_setcc). An immediate folds into
         * the 32-bit cmp directly (its low 32 bits are the constant); only a
         * register operand needs the reg-reg form. */
        if (in->b.kind == MIR_OPK_IMM) {
          if (!binary_emit_cmp_reg_imm_w32(&ctx->code, areg,
                                           (uint32_t)in->b.imm)) {
            ok = enc_err(fn, "out of memory in cmpbr32 imm");
            break;
          }
        } else {
          BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &rok);
          if (!rok || !binary_emit_cmp_reg_reg32(&ctx->code, areg, breg)) {
            ok = enc_err(fn, "out of memory in cmpbr32");
            break;
          }
        }
      } else if (in->b.kind == MIR_OPK_IMM &&
                 code_generator_binary_immediate_fits_signed_32(in->b.imm)) {
        if (!binary_emit_cmp_reg_imm32(&ctx->code, areg, (uint32_t)in->b.imm)) {
          ok = enc_err(fn, "out of memory in cmpbr");
          break;
        }
      } else {
        BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &rok);
        if (!rok || !binary_emit_cmp_reg_reg(&ctx->code, areg, breg)) {
          ok = enc_err(fn, "out of memory in cmpbr");
          break;
        }
      }
      size_t off = 0;
      if (!binary_emit_jcc_placeholder(&ctx->code, in->cc, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in cmpbr");
      }
      break;
    }
    case MIR_RET:
      ok = mir_emit_epilogue(fn);
      break;
    default:
      ok = enc_err(fn, "unsupported MIR opcode in encoder");
      break;
    }
    if (annot && ok && ctx->code.size > annot_off) {
      mir_annotate_record(fn, in, (int)i, annot_off - annot_base,
                          ctx->code.size - annot_off,
                          ctx->code.data + annot_off);
    }
    if (!ok) {
      free(align_label);
      return 0;
    }
  }
  free(align_label);

  /* Resolve label/jump rel32 fixups against the defined labels. */
  if (!code_generator_binary_resolve_fixups(fn->generator, ctx,
                                            ctx->code.size)) {
    return 0;
  }
  return 1;
}
