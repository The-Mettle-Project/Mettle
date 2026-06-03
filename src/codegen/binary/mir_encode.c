#include "codegen/binary/mir.h"

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

#define SCRATCH_A BINARY_GP_RAX
#define SCRATCH_B BINARY_GP_RCX
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
    return binary_emit_mov_reg_mem(code, target, BINARY_GP_RBP, -spill_off(v));
  }
  case MIR_OPK_PHYS:
    if ((BinaryGpRegister)op->phys != target) {
      return binary_emit_mov_reg_reg(code, target, (BinaryGpRegister)op->phys);
    }
    return 1;
  case MIR_OPK_IMM:
    return binary_emit_mov_reg_imm64(code, target, (uint64_t)op->imm);
  case MIR_OPK_STACKHOME:
    return binary_emit_mov_reg_mem(code, target, BINARY_GP_RBP, -op->disp);
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
    *ok = binary_emit_mov_reg_mem(&fn->context->code, scratch, BINARY_GP_RBP,
                                  -spill_off(v));
    return scratch;
  }
  case MIR_OPK_PHYS:
    return (BinaryGpRegister)op->phys;
  case MIR_OPK_IMM:
    *ok = binary_emit_mov_reg_imm64(&fn->context->code, scratch,
                                    (uint64_t)op->imm);
    return scratch;
  case MIR_OPK_STACKHOME:
    *ok = binary_emit_mov_reg_mem(&fn->context->code, scratch, BINARY_GP_RBP,
                                  -op->disp);
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
    return binary_emit_mov_mem_reg(code, BINARY_GP_RBP, -spill_off(v), src_phys);
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

static int encode_alu(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  unsigned char opc;
  if (!alu_opcode(in->op, &opc)) {
    return enc_err(fn, "bad ALU opcode");
  }
  int is_sub = (in->op == MIR_SUB);
  BinaryGpRegister D;

  if (dst_is_reg(fn, &in->dst, &D)) {
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
  } else {
    /* Variable count must be in CL. Load the count into RCX FIRST (before the
     * value lands in `work`), so a count that happens to live in `work` is not
     * clobbered by staging the shifted value. RCX is never allocatable. */
    int ok;
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_A, &ok);
    if (!ok) {
      return 0;
    }
    if (breg != BINARY_GP_RCX &&
        !binary_emit_mov_reg_reg(code, BINARY_GP_RCX, breg)) {
      return enc_err(fn, "out of memory moving shift count");
    }
    if (!operand_in_phys(fn, &in->a, work) &&
        !materialize_into(fn, &in->a, work)) {
      return 0;
    }
    if (!binary_emit_shift_reg_cl(code, sub, work)) {
      return enc_err(fn, "out of memory in shift");
    }
  }
  return dst_reg ? 1 : store_from(fn, &in->dst, SCRATCH_A);
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
  if (in->b.kind == MIR_OPK_IMM &&
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
  return store_from(fn, &in->dst, SCRATCH_A);
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

  if (signed_ext && dst_is_reg(fn, &in->dst, &D)) {
    int ok;
    BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
    if (!ok) {
      return 0;
    }
    int done = 1;
    switch (in->width) {
    case 4: done = binary_emit_movsxd_reg_reg32(code, D, areg); break;
    case 2: done = binary_emit_movsx_reg_reg16(code, D, areg); break;
    case 1: done = binary_emit_movsx_reg_reg8(code, D, areg); break;
    default: return enc_err(fn, "bad extend width");
    }
    return done ? 1 : enc_err(fn, "out of memory in extend");
  }

  /* RAX path. */
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  int ok = 1;
  switch (in->width) {
  case 4:
    ok = signed_ext ? binary_emit_movsxd_rax_eax(code)
                    : binary_emit_mov_eax_eax(code);
    break;
  case 2:
    ok = signed_ext ? binary_emit_movsx_rax_ax(code)
                    : binary_emit_movzx_eax_ax(code);
    break;
  case 1:
    ok = signed_ext ? binary_emit_movsx_rax_al(code)
                    : binary_emit_movzx_eax_al(code);
    break;
  default:
    return enc_err(fn, "bad extend width");
  }
  if (!ok) {
    return enc_err(fn, "out of memory in extend");
  }
  return store_from(fn, &in->dst, SCRATCH_A);
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
                                         target, BINARY_GP_RBP,
                                         -v->spill_offset);
}

static int xmm_spill_store(MirFunction *fn, const MirVreg *v,
                           BinaryXmmRegister src) {
  unsigned char prefix = (v->width == 4) ? 0xF3 : 0xF2;
  return simd_emit_prefixed_xmm_mem_disp(&fn->context->code, prefix, 0x11, src,
                                         BINARY_GP_RBP, -v->spill_offset);
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

  /* LOAD: dst <- [base], width bytes. */
  if (in->a.kind == MIR_OPK_MEM) {
    int ok;
    MirOperand base = mir_op_vreg(in->a.mem.base);
    BinaryGpRegister addr = value_reg(fn, &base, SCRATCH_B, &ok);
    if (!ok) {
      return 0;
    }
    if (!code_generator_binary_emit_load_from_address(g, ctx, addr, in->width,
                                                      SCRATCH_A)) {
      return enc_err(fn, "out of memory in load");
    }
    /* is_unsigned==0 means a signed load needing sign-extension to 64 bits. */
    if (!in->is_unsigned && in->width < 8) {
      int ok2 = 1;
      if (in->width == 1) {
        ok2 = binary_emit_movsx_rax_al(&ctx->code);
      } else if (in->width == 2) {
        ok2 = binary_emit_movsx_rax_ax(&ctx->code);
      } else if (in->width == 4) {
        ok2 = binary_emit_movsxd_rax_eax(&ctx->code);
      }
      if (!ok2) {
        return enc_err(fn, "out of memory sign-extending load");
      }
    }
    return store_from(fn, &in->dst, SCRATCH_A);
  }

  /* STORE: [base] <- a, width bytes. */
  if (in->dst.kind == MIR_OPK_MEM) {
    int ok1, ok2;
    MirOperand base = mir_op_vreg(in->dst.mem.base);
    BinaryGpRegister addr = value_reg(fn, &base, SCRATCH_B, &ok1);
    BinaryGpRegister val = value_reg(fn, &in->a, SCRATCH_A, &ok2);
    if (!ok1 || !ok2) {
      return 0;
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
    if (fn->insns[i].op == MIR_CALL) {
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
    raw += 32; /* shadow space at the bottom; spills/saves never reach it */
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
      ok = p->is_signed ? binary_emit_movsxd_reg_reg32(code, D, arg)
                        : binary_emit_mov_reg_reg32(code, D, arg);
    } else if (p->width == 2 && p->is_signed) {
      ok = binary_emit_movsx_reg_reg16(code, D, arg);
    } else if (p->width == 1 && p->is_signed) {
      ok = binary_emit_movsx_reg_reg8(code, D, arg);
    } else {
      ok = binary_emit_mov_reg_reg(code, SCRATCH_A, arg) &&
           (p->width == 2 ? binary_emit_movzx_eax_ax(code)
                          : binary_emit_movzx_eax_al(code)) &&
           binary_emit_mov_reg_reg(code, D, SCRATCH_A);
    }
    return ok ? 1 : enc_err(fn, "out of memory extending parameter");
  }
  if (!binary_emit_mov_reg_reg(code, SCRATCH_A, arg)) {
    return enc_err(fn, "out of memory homing parameter");
  }
  int ok = 1;
  if (p->width == 4) {
    ok = p->is_signed ? binary_emit_movsxd_rax_eax(code)
                      : binary_emit_mov_eax_eax(code);
  } else if (p->width == 2) {
    ok = p->is_signed ? binary_emit_movsx_rax_ax(code)
                      : binary_emit_movzx_eax_ax(code);
  } else if (p->width == 1) {
    ok = p->is_signed ? binary_emit_movsx_rax_al(code)
                      : binary_emit_movzx_eax_al(code);
  }
  if (!ok || !store_from(fn, &dst, SCRATCH_A)) {
    return enc_err(fn, "out of memory extending parameter");
  }
  return 1;
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
                    binary_emit_mov_mem_reg32(code, BINARY_GP_RBP, -mv[i].dst,
                                              SCRATCH_A))
                 : (binary_emit_movq_reg_xmm(code, SCRATCH_A, mv[i].src) &&
                    binary_emit_mov_mem_reg(code, BINARY_GP_RBP, -mv[i].dst,
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
  if (pc == 0) {
    return 1;
  }
  const BinaryAbi *abi = code_generator_binary_active_abi();
  int is_float[MIR_MAX_PARAMS];
  BinaryArgLocation locs[MIR_MAX_PARAMS];
  for (size_t i = 0; i < pc; i++) {
    is_float[i] = fn->params[i].is_float;
  }
  if (!code_generator_binary_compute_arg_layout(abi, is_float, pc, locs, NULL)) {
    return enc_err(fn, "failed to compute parameter layout");
  }

  MirXmmMove xm[MIR_MAX_PARAMS];
  int nxm = 0;
  for (size_t i = 0; i < pc; i++) {
    const MirParam *p = &fn->params[i];
    if (!fn->vregs[p->vreg].assigned) {
      continue; /* unused parameter */
    }
    const BinaryArgLocation *loc = &locs[i];
    if (!p->is_float) {
      if (loc->kind != BINARY_ARG_IN_GP_REGISTER) {
        return enc_err(fn, "unsupported parameter location");
      }
      if (!mir_home_gp_param(fn, p, loc->gp_register)) {
        return 0;
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
  if (!binary_emit_push_reg(code, BINARY_GP_RBP) ||
      !binary_emit_mov_reg_reg(code, BINARY_GP_RBP, BINARY_GP_RSP)) {
    return enc_err(fn, "out of memory in prologue");
  }
  if (!binary_emit_frame_allocation(code, ctx->frame_size)) {
    return enc_err(fn, "out of memory allocating frame");
  }
  for (size_t i = 0; i < ctx->saved_register_count; i++) {
    if (!binary_emit_mov_mem_reg(code, BINARY_GP_RBP,
                                 -ctx->saved_register_offsets[i],
                                 ctx->saved_registers[i])) {
      return enc_err(fn, "out of memory saving callee registers");
    }
  }
  for (size_t i = 0; i < ctx->saved_xmm_count; i++) {
    if (!simd_movdqu_mem_xmm_disp(code, BINARY_GP_RBP,
                                  -ctx->saved_xmm_offsets[i],
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
  for (size_t i = ctx->saved_xmm_count; i > 0; i--) {
    size_t j = i - 1;
    if (!simd_movdqu_xmm_mem_disp(code, ctx->saved_xmm_registers[j],
                                  BINARY_GP_RBP, -ctx->saved_xmm_offsets[j])) {
      return enc_err(fn, "out of memory restoring callee xmm registers");
    }
  }
  for (size_t i = ctx->saved_register_count; i > 0; i--) {
    size_t j = i - 1;
    if (!binary_emit_mov_reg_mem(code, ctx->saved_registers[j], BINARY_GP_RBP,
                                 -ctx->saved_register_offsets[j])) {
      return enc_err(fn, "out of memory restoring callee registers");
    }
  }
  if (!binary_emit_mov_reg_reg(code, BINARY_GP_RSP, BINARY_GP_RBP) ||
      !binary_emit_pop_reg(code, BINARY_GP_RBP) || !binary_emit_ret(code)) {
    return enc_err(fn, "out of memory in epilogue");
  }
  return 1;
}

int mir_encode(MirFunction *fn) {
  if (!fn || !fn->context) {
    return 0;
  }
  BinaryFunctionContext *ctx = fn->context;

  if (!mir_layout_frame(fn) || !mir_emit_prologue(fn)) {
    return 0;
  }

  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    int ok = 1;
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
      ok = store_from(fn, &in->dst, SCRATCH_A);
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
    case MIR_CMPBR: {
      /* cmp a,b ; j<cc> label  (fused compare-and-branch). */
      int rok;
      BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok) {
        ok = 0;
        break;
      }
      if (in->b.kind == MIR_OPK_IMM &&
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
    if (!ok) {
      return 0;
    }
  }

  /* Resolve label/jump rel32 fixups against the defined labels. */
  if (!code_generator_binary_resolve_fixups(fn->generator, ctx,
                                            ctx->code.size)) {
    return 0;
  }
  return 1;
}
