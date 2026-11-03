#include "codegen/binary/mir.h"
#include "ir/ir_machine.h"

long long mir_encode_last_spills = 0;
#include "codegen/binary/mir_annotate.h"
#include "codegen/binary/simd_internal.h"
#include "codegen/code_generator_internal.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

#define SCRATCH_A BINARY_GP_R10
#define SCRATCH_B BINARY_GP_R11
#define FSCRATCH_A mir_xmm_scratch_a()
#define FSCRATCH_B mir_xmm_scratch_b()

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

static int spill_off(const MirVreg *v) { return v->spill_offset; }

typedef struct {
  int valid;
  int disp;
  BinaryGpRegister reg;
  size_t code_size;
} MirHomeForward;

static MirHomeForward g_home_fwd;

static void home_fwd_clear(void) { g_home_fwd.valid = 0; }

static void home_fwd_note_boundary(MirOpcode op) {
  if (op == MIR_LABEL || op == MIR_JMP || op == MIR_JCC || op == MIR_CMPBR ||
      op == MIR_BT ||
      op == MIR_FCMPBR || op == MIR_CALL || op == MIR_RET ||
      op == MIR_INLINE_ASM) {
    g_home_fwd.valid = 0;
  }
}

static void home_fwd_record(const BinaryCodeBuffer *code, int disp,
                            BinaryGpRegister reg) {
  g_home_fwd.valid = 1;
  g_home_fwd.disp = disp;
  g_home_fwd.reg = reg;
  g_home_fwd.code_size = code->size;
}

static int home_fwd_has(const BinaryCodeBuffer *code, int disp,
                        BinaryGpRegister reg) {
  return g_home_fwd.valid && g_home_fwd.disp == disp &&
         g_home_fwd.reg == reg && g_home_fwd.code_size == code->size;
}

static BinaryGpRegister frame_base(const MirFunction *fn) {
  return fn->context->omit_frame_pointer ? BINARY_GP_RSP : BINARY_GP_RBP;
}

static int frame_disp(const MirFunction *fn, int rbp_disp) {
  return fn->context->omit_frame_pointer ? rbp_disp + fn->context->frame_size
                                         : rbp_disp;
}

static int gp_home_load(MirFunction *fn, const MirVreg *v,
                        BinaryGpRegister dst) {
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister base = frame_base(fn);
  int disp = frame_disp(fn, -spill_off(v));
  if (!v->address_taken && home_fwd_has(code, disp, dst)) {
    return 1;
  }
  if (v->address_taken) {
    switch (v->home_width) {
    case 4:
      return v->home_signed
                 ? binary_emit_movsxd_reg_mem(code, dst, base, disp)
                 : binary_emit_mov_reg_mem32(code, dst, base, disp);
    case 2:
      return v->home_signed
                 ? binary_emit_movsx_reg_mem16(code, dst, base, disp)
                 : binary_emit_movzx_reg_mem16(code, dst, base, disp);
    case 1:
      return v->home_signed
                 ? binary_emit_movsx_reg_mem8(code, dst, base, disp)
                 : binary_emit_movzx_reg_mem8(code, dst, base, disp);
    default:
      break;
    }
  }
  return binary_emit_mov_reg_mem(code, dst, base, disp);
}

static int gp_home_mem(MirFunction *fn, const MirOperand *op,
                       BinaryGpRegister *base, int *disp) {
  if (op->kind == MIR_OPK_STACKHOME) {
    *base = frame_base(fn);
    *disp = frame_disp(fn, -op->disp);
    return 1;
  }
  if (op->kind != MIR_OPK_VREG) {
    return 0;
  }
  {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register || v->rclass != MIR_RC_GP) {
      return 0;
    }
    if (v->address_taken &&
        (v->home_width == 1 || v->home_width == 2 || v->home_width == 4)) {
      return 0;
    }
    *base = frame_base(fn);
    *disp = frame_disp(fn, -spill_off(v));
  }
  return 1;
}

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
    return gp_home_load(fn, v, target);
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

static int mir_reg_in(BinaryGpRegister r, const BinaryGpRegister *set, int n) {
  for (int i = 0; i < n; i++) {
    if (set[i] == r) {
      return 1;
    }
  }
  return 0;
}

static int mir_operand_fixed_reg(const MirFunction *fn, const MirOperand *op,
                                 BinaryGpRegister *out) {
  if (op->kind == MIR_OPK_VREG && op->vreg >= 0 &&
      (size_t)op->vreg < fn->vreg_count && fn->vregs[op->vreg].in_register) {
    *out = (BinaryGpRegister)fn->vregs[op->vreg].phys;
    return 1;
  }
  if (op->kind == MIR_OPK_PHYS) {
    *out = (BinaryGpRegister)op->phys;
    return 1;
  }
  return 0;
}

static void mir_note_fixed_reg(const MirFunction *fn, const MirOperand *op,
                               BinaryGpRegister *set, int *n) {
  BinaryGpRegister r;
  if (mir_operand_fixed_reg(fn, op, &r)) {
    set[(*n)++] = r;
  }
}

static int mir_phys_live_at(const MirFunction *fn, BinaryGpRegister phys,
                            size_t idx) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (!vr->assigned || !vr->in_register || vr->rclass == MIR_RC_XMM ||
        (BinaryGpRegister)vr->phys != phys) {
      continue;
    }
    if (vr->live_start <= (int)idx && vr->live_end >= (int)idx) {
      return 1;
    }
  }
  return 0;
}

static int mir_pick_scratch(const MirFunction *fn, size_t idx,
                            BinaryGpRegister preferred,
                            const BinaryGpRegister *avoid, int avoid_n,
                            const BinaryGpRegister *extra, int extra_n,
                            BinaryGpRegister *out) {
  BinaryGpRegister pool[8];
  int vouched[8];
  int n = 0;
  pool[n] = preferred;
  vouched[n++] = 0;
  pool[n] = SCRATCH_B;
  vouched[n++] = 1;
  pool[n] = SCRATCH_A;
  vouched[n++] = 1;
  for (int i = 0; i < extra_n && n < 7; i++) {
    pool[n] = extra[i];
    vouched[n++] = 1;
  }
  pool[n] = BINARY_GP_RDX;
  vouched[n++] = 0;

  for (int i = 0; i < n; i++) {
    if (mir_reg_in(pool[i], avoid, avoid_n)) {
      continue;
    }
    if (!vouched[i] && pool[i] != SCRATCH_A && pool[i] != SCRATCH_B &&
        mir_phys_live_at(fn, pool[i], idx)) {
      continue;
    }
    *out = pool[i];
    return 1;
  }
  return 0;
}

static int mir_env_addr_store(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_MIR_ADDR_STORE") ? 1 : 0;
  }
  return cached;
}

static BinaryGpRegister value_reg(MirFunction *fn, const MirOperand *op,
                                  BinaryGpRegister scratch, int *ok) {
  *ok = 1;
  switch (op->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[op->vreg];
    if (v->in_register) {
      if (v->rclass == MIR_RC_XMM) {
        *ok = binary_emit_movq_reg_xmm(&fn->context->code, scratch,
                                       (BinaryXmmRegister)v->phys);
        if (!*ok) {
          *ok = enc_err(fn, "out of memory moving a float to a GP register");
        }
        return scratch;
      }
      if (v->rclass != MIR_RC_GP) {
        *ok = enc_err(fn, "a packed vector has no GP value form");
        return scratch;
      }
      return (BinaryGpRegister)v->phys;
    }
    *ok = gp_home_load(fn, v, scratch);
    return scratch;
  }
  case MIR_OPK_PHYS:
    return (BinaryGpRegister)op->phys;
  case MIR_OPK_IMM:
  case MIR_OPK_FIMM:
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

static int mir_mem_is_rooted(const MirMem *m) {
  return m->frame_home_valid || m->phys_base_valid;
}

static void mir_note_mem_base(MirFunction *fn, const MirMem *m,
                              BinaryGpRegister *taken, int *tn) {
  MirOperand bop;
  if (mir_mem_is_rooted(m)) {
    return;
  }
  bop = mir_op_vreg(m->base);
  mir_note_fixed_reg(fn, &bop, taken, tn);
}

static BinaryGpRegister mir_mem_base_reg(MirFunction *fn, const MirMem *m,
                                         BinaryGpRegister scratch, int *disp,
                                         int *ok) {
  MirOperand base;
  if (m->frame_home_valid) {
    *ok = 1;
    *disp = frame_disp(fn, -spill_off(&fn->vregs[m->frame_home])) + m->disp;
    return frame_base(fn);
  }
  if (m->phys_base_valid) {
    *ok = 1;
    *disp = m->disp;
    return (BinaryGpRegister)m->phys_base;
  }
  base = mir_op_vreg(m->base);
  *disp = m->disp;
  return value_reg(fn, &base, scratch, ok);
}

static int store_from(MirFunction *fn, const MirOperand *dst,
                      BinaryGpRegister src_phys) {
  BinaryCodeBuffer *code = &fn->context->code;
  switch (dst->kind) {
  case MIR_OPK_VREG: {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->in_register) {
      if (v->rclass == MIR_RC_XMM) {
        return binary_emit_movq_xmm_reg(code, (BinaryXmmRegister)v->phys,
                                        src_phys);
      }
      if (v->rclass != MIR_RC_GP) {
        return enc_err(fn, "a packed vector has no GP value form");
      }
      if ((BinaryGpRegister)v->phys != src_phys) {
        return binary_emit_mov_reg_reg(code, (BinaryGpRegister)v->phys,
                                       src_phys);
      }
      return 1;
    }
    {
      int disp = frame_disp(fn, -spill_off(v));
      if (!v->address_taken && home_fwd_has(code, disp, src_phys)) {
        return 1;
      }
      if (!binary_emit_mov_mem_reg(code, frame_base(fn), disp, src_phys)) {
        return 0;
      }
      if (!v->address_taken) {
        home_fwd_record(code, disp, src_phys);
      }
      return 1;
    }
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

static int alu_imm_subopcode(MirOpcode op, unsigned char *out) {
  switch (op) {
  case MIR_ADD: *out = 0; return 1;
  case MIR_OR:  *out = 1; return 1;
  case MIR_AND: *out = 4; return 1;
  case MIR_SUB: *out = 5; return 1;
  case MIR_XOR: *out = 6; return 1;
  default: return 0;
  }
}

static int alu_imm(MirFunction *fn, MirOpcode op, BinaryGpRegister reg,
                   long long imm, int width) {
  BinaryCodeBuffer *code = &fn->context->code;
  uint32_t v = (uint32_t)imm;
  if (width == 4) {
    unsigned char sub;
    if (!alu_imm_subopcode(op, &sub)) {
      return 0;
    }
    return binary_emit_alu_reg_imm_w32(code, sub, reg, v);
  }
  switch (op) {
  case MIR_ADD: return binary_emit_add_reg_imm32(code, reg, v);
  case MIR_SUB: return binary_emit_sub_reg_imm32(code, reg, v);
  case MIR_AND: return binary_emit_and_reg_imm32(code, reg, v);
  case MIR_OR:  return binary_emit_or_reg_imm32(code, reg, v);
  case MIR_XOR: return binary_emit_xor_reg_imm32(code, reg, v);
  default: return 0;
  }
}

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

static int dst_is_reg(MirFunction *fn, const MirOperand *dst,
                      BinaryGpRegister *D_out) {
  if (dst->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->rclass != MIR_RC_GP) {
      return 0;
    }
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

static int emit_op_eq(MirFunction *fn, MirOpcode mop, unsigned char opc,
                      BinaryGpRegister target, const MirOperand *x, int width) {
  BinaryCodeBuffer *code = &fn->context->code;
  if (x->kind == MIR_OPK_IMM &&
      (code_generator_binary_immediate_fits_signed_32(x->imm) ||
       (width == 4 && (unsigned long long)x->imm <= 0xFFFFFFFFULL))) {
    return alu_imm(fn, mop, target, x->imm, width)
               ? 1
               : enc_err(fn, "out of memory in ALU imm");
  }
  if (x->kind == MIR_OPK_IMM && mop == MIR_AND && width == 8 &&
      (unsigned long long)x->imm <= 0xFFFFFFFFULL) {
    return alu_imm(fn, mop, target, x->imm, 4)
               ? 1
               : enc_err(fn, "out of memory in ALU imm");
  }
  {
    BinaryGpRegister mbase;
    int mdisp;
    if (gp_home_mem(fn, x, &mbase, &mdisp)) {
      return binary_emit_alu_reg_mem(code, opc, target, mbase, mdisp,
                                     width == 4 ? 4 : 8)
                 ? 1
                 : enc_err(fn, "out of memory in ALU mem");
    }
  }
  BinaryGpRegister scratch = (target == SCRATCH_A) ? SCRATCH_B : SCRATCH_A;
  int ok;
  BinaryGpRegister xr = value_reg(fn, x, scratch, &ok);
  if (!ok) {
    return 0;
  }
  int emitted = (width == 4)
                    ? binary_emit_alu_reg_reg32(code, opc, target, xr)
                    : binary_emit_alu_reg_reg(code, opc, target, xr);
  return emitted ? 1 : enc_err(fn, "out of memory in ALU");
}

static int encode_neg_not(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int w32 = (in->width == 4);
  BinaryGpRegister D;
  if (dst_is_reg(fn, &in->dst, &D)) {
    if (!operand_in_phys(fn, &in->a, D) && !materialize_into(fn, &in->a, D)) {
      return 0;
    }
    int ok = (in->op == MIR_NEG)
                 ? (w32 ? binary_emit_neg_reg32(code, D)
                        : binary_emit_neg_reg(code, D))
                 : (w32 ? binary_emit_not_reg32(code, D)
                        : binary_emit_not_reg(code, D));
    return ok ? 1 : enc_err(fn, "out of memory in neg/not");
  }
  if (!materialize_into(fn, &in->a, SCRATCH_A)) {
    return 0;
  }
  int ok = (in->op == MIR_NEG)
               ? (w32 ? binary_emit_neg_reg32(code, SCRATCH_A)
                      : binary_emit_neg_reg(code, SCRATCH_A))
               : (w32 ? binary_emit_not_reg32(code, SCRATCH_A)
                      : binary_emit_not_reg(code, SCRATCH_A));
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
  int w32 = (in->width == 4);
  BinaryGpRegister D;

  if (dst_is_reg(fn, &in->dst, &D)) {
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
            (w32 ? binary_emit_lea32_reg_base_index_scale_disp(code, D, base,
                                                               index, 1, 0)
                 : binary_emit_lea_reg_base_index_scale_disp(code, D, base,
                                                             index, 1, 0))) {
          return 1;
        }
      }
    }
    if ((in->op == MIR_ADD || is_sub) &&
        in->b.kind == MIR_OPK_IMM && in->b.imm >= -2147483647LL &&
        in->b.imm <= 2147483647LL && !operand_in_phys(fn, &in->a, D)) {
      BinaryGpRegister ra;
      long long disp = is_sub ? -in->b.imm : in->b.imm;
      if (operand_gp_reg(fn, &in->a, &ra) && ra != BINARY_GP_RSP &&
          (w32 ? binary_emit_lea32_reg_mem(code, D, ra, (int)disp)
               : binary_emit_lea_reg_mem(code, D, ra, (int)disp))) {
        return 1;
      }
    }
    if (operand_in_phys(fn, &in->b, D)) {
      if (is_sub) {
        if (!materialize_into(fn, &in->a, SCRATCH_A)) {
          return 0;
        }
        if (!(w32 ? binary_emit_alu_reg_reg32(code, opc, SCRATCH_A, D)
                  : binary_emit_alu_reg_reg(code, opc, SCRATCH_A, D))) {
          return enc_err(fn, "out of memory in sub");
        }
        return store_from(fn, &in->dst, SCRATCH_A);
      }
      return emit_op_eq(fn, in->op, opc, D, &in->a, in->width);
    }
    if (!operand_in_phys(fn, &in->a, D) &&
        !materialize_into(fn, &in->a, D)) {
      return 0;
    }
    return emit_op_eq(fn, in->op, opc, D, &in->b, in->width);
  }

  if (!materialize_into(fn, &in->a, SCRATCH_A) ||
      !emit_op_eq(fn, in->op, opc, SCRATCH_A, &in->b, in->width)) {
    return 0;
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

static int encode_imul(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int b_imm32 = in->b.kind == MIR_OPK_IMM &&
                code_generator_binary_immediate_fits_signed_32(in->b.imm);
  BinaryGpRegister D;

  if (in->width == 4) {
    int ok;
    int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
    BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
    BinaryGpRegister stage = (target == SCRATCH_A) ? SCRATCH_B : SCRATCH_A;
    if (b_imm32) {
      BinaryGpRegister areg = value_reg(fn, &in->a, stage, &ok);
      BinaryGpRegister mul_scratch =
          (areg == stage) ? ((stage == SCRATCH_A) ? SCRATCH_B : SCRATCH_A)
                          : stage;
      if (!ok || !binary_emit_imul_reg_reg_imm32_scratch_w32(
                     code, target, areg, (uint32_t)in->b.imm,
                     mul_scratch != target, mul_scratch)) {
        return enc_err(fn, "out of memory in imul32 imm");
      }
    } else if (dst_in_reg && operand_in_phys(fn, &in->b, target)) {
      BinaryGpRegister areg = value_reg(fn, &in->a, stage, &ok);
      if (!ok || !binary_emit_imul_reg_reg32(code, target, areg)) {
        return enc_err(fn, "out of memory in imul32");
      }
    } else {
      BinaryGpRegister breg;
      if (!operand_in_phys(fn, &in->a, target) &&
          !materialize_into(fn, &in->a, target)) {
        return 0;
      }
      breg = value_reg(fn, &in->b, stage, &ok);
      if (!ok || !binary_emit_imul_reg_reg32(code, target, breg)) {
        return enc_err(fn, "out of memory in imul32");
      }
    }
    if (!dst_in_reg) {
      return store_from(fn, &in->dst, SCRATCH_A);
    }
    return 1;
  }

  if (dst_is_reg(fn, &in->dst, &D)) {
    int ok;
    if (b_imm32) {
      BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
      BinaryGpRegister scratch = (areg == SCRATCH_A) ? SCRATCH_B : SCRATCH_A;
      if (!ok || !binary_emit_imul_reg_reg_imm32_scratch(
                     code, D, areg, (uint32_t)in->b.imm, 1, scratch)) {
        return enc_err(fn, "out of memory in imul imm");
      }
      return 1;
    }
    if (operand_in_phys(fn, &in->b, D)) {
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

static int encode_div(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int rok;
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
  size_t to_wide = 0;
  size_t to_done = 0;
  if (!binary_emit_mov_reg_reg(code, SCRATCH_A, BINARY_GP_RAX) ||
      !binary_emit_alu_reg_reg(code, 0x09, SCRATCH_A, divisor) ||
      !binary_emit_shift_reg_imm8(code, 5, SCRATCH_A, 32) ||
      !wcs_jcc(code, 0x85, &to_wide) ||
      !binary_emit_xor_reg_reg32(code, BINARY_GP_RDX) ||
      !binary_emit_unary_reg32(code, 6, divisor) ||
      !wcs_jcc(code, 0, &to_done) || !wcs_patch_here(code, to_wide)) {
    return enc_err(fn, "out of memory in narrow divide");
  }
  if (in->is_unsigned) {
    if (!binary_emit_xor_reg_reg32(code, BINARY_GP_RDX) ||
        !binary_emit_div_reg(code, divisor)) {
      return enc_err(fn, "out of memory in div");
    }
  } else {
    int needs_guard = !(in->b.kind == MIR_OPK_IMM && in->b.imm != -1);
    if (needs_guard) {
      if (!binary_emit_idiv_wrapping(code, divisor)) {
        return enc_err(fn, "out of memory in idiv");
      }
    } else if (!binary_emit_cqo(code) || !binary_emit_idiv_reg(code, divisor)) {
      return enc_err(fn, "out of memory in idiv");
    }
  }
  if (!wcs_patch_here(code, to_done)) {
    return enc_err(fn, "out of memory in narrow divide");
  }
  BinaryGpRegister result = in->cc ? BINARY_GP_RDX : BINARY_GP_RAX;
  return store_from(fn, &in->dst, result);
}

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
  int w32 = (in->width == 4);
  BinaryGpRegister D;
  int dst_reg = dst_is_reg(fn, &in->dst, &D);
  BinaryGpRegister work = dst_reg ? D : SCRATCH_A;

  if (in->b.kind == MIR_OPK_IMM) {
    unsigned char count = (unsigned char)(in->b.imm & (w32 ? 31 : 63));
    if ((dst_reg && !operand_in_phys(fn, &in->a, D) &&
         !materialize_into(fn, &in->a, work)) ||
        (!dst_reg && !materialize_into(fn, &in->a, work))) {
      return 0;
    }
    if (!(w32 ? binary_emit_shift_reg_imm8_32(code, sub, work, count)
              : binary_emit_shift_reg_imm8(code, sub, work, count))) {
      return enc_err(fn, "out of memory in shift imm");
    }
    return dst_reg ? 1 : store_from(fn, &in->dst, work);
  }
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
  if (!(w32 ? binary_emit_shift_reg_cl_32(code, sub, SCRATCH_A)
            : binary_emit_shift_reg_cl(code, sub, SCRATCH_A))) {
    return enc_err(fn, "out of memory in shift");
  }
  return store_from(fn, &in->dst, SCRATCH_A);
}

static int encode_setcc(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryGpRegister cbase;
  int cdisp;
  BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  if (in->width == 4) {
    if (in->b.kind == MIR_OPK_IMM) {
      if (!binary_emit_cmp_reg_imm_w32(code, areg, (uint32_t)in->b.imm)) {
        return enc_err(fn, "out of memory in cmp32 imm");
      }
    } else if (gp_home_mem(fn, &in->b, &cbase, &cdisp)) {
      if (!binary_emit_alu_reg_mem(code, 0x39, areg, cbase, cdisp, 4)) {
        return enc_err(fn, "out of memory in cmp32 mem");
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
  } else if (gp_home_mem(fn, &in->b, &cbase, &cdisp)) {
    if (!binary_emit_alu_reg_mem(code, 0x39, areg, cbase, cdisp, 8)) {
      return enc_err(fn, "out of memory in cmp mem");
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
  return store_from(fn, &in->dst, BINARY_GP_RAX);
}

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

static int dst_is_xmm_reg(MirFunction *fn, const MirOperand *dst,
                          BinaryXmmRegister *D_out) {
  if (dst->kind == MIR_OPK_VREG) {
    const MirVreg *v = &fn->vregs[dst->vreg];
    if (v->rclass == MIR_RC_GP) {
      return 0;
    }
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

static int xmm_mov(BinaryCodeBuffer *code, BinaryXmmRegister dst,
                   BinaryXmmRegister src, int width) {
  if (dst == src) {
    return 1;
  }
  (void)width;
  return binary_emit_rex(code, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(code, 0x0F) &&
         binary_code_buffer_append_u8(code, 0x28) &&
         binary_code_buffer_append_u8(
             code, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

static int xmm_load_fimm(MirFunction *fn, uint64_t bits,
                         BinaryXmmRegister target, int width) {
  BinaryCodeBuffer *code = &fn->context->code;
  if (bits == 0) {
    return binary_emit_pxor_xmm_xmm(code, target, target);
  }
  if (width == 4) {
    return binary_emit_mov_reg_imm32_zero_extend(code, SCRATCH_A,
                                                 (uint32_t)bits) &&
           binary_emit_movd_xmm_reg(code, target, SCRATCH_A);
  }
  return binary_emit_mov_reg_imm64(code, SCRATCH_A, bits) &&
         binary_emit_movq_xmm_reg(code, target, SCRATCH_A);
}

static int mir_emit_preserve_volatiles(MirFunction *fn, const MirInst *in,
                                       int save) {
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister base = frame_base(fn);
  if (in->preserves_rax && fn->preserve_slot > 0) {
    if (save) {
      if (!binary_emit_mov_mem_reg(code, base,
                                   frame_disp(fn, -fn->preserve_slot),
                                   BINARY_GP_RAX)) {
        return 0;
      }
    } else if (!binary_emit_mov_reg_mem(code, BINARY_GP_RAX, base,
                                        frame_disp(fn, -fn->preserve_slot))) {
      return 0;
    }
  }
  if (!in->preserves_xmm || fn->preserve_xmm_slot <= 0) {
    return 1;
  }
  for (size_t i = 0; i < MIR_XMM_POOL_COUNT; i++) {
    int disp = frame_disp(fn, -fn->preserve_xmm_slot + (int)i * 8);
    if (!simd_emit_prefixed_xmm_mem_disp(code, 0xF2, save ? 0x11 : 0x10,
                                         MIR_XMM_POOL[i], base, disp)) {
      return 0;
    }
  }
  return 1;
}

static int xmm_spill_load(MirFunction *fn, const MirVreg *v,
                          BinaryXmmRegister target) {
  unsigned char prefix =
      (v->width == 16) ? 0x66 : ((v->width == 4) ? 0xF3 : 0xF2);
  return simd_emit_prefixed_xmm_mem_disp(&fn->context->code, prefix, 0x10,
                                         target, frame_base(fn),
                                         frame_disp(fn, -v->spill_offset));
}

static int xmm_spill_store(MirFunction *fn, const MirVreg *v,
                           BinaryXmmRegister src) {
  unsigned char prefix =
      (v->width == 16) ? 0x66 : ((v->width == 4) ? 0xF3 : 0xF2);
  return simd_emit_prefixed_xmm_mem_disp(&fn->context->code, prefix, 0x11, src,
                                         frame_base(fn),
                                         frame_disp(fn, -v->spill_offset));
}

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

static int vex_xmm_3op(MirFunction *fn, int pp, unsigned char opcode,
                       BinaryXmmRegister dst, BinaryXmmRegister a,
                       BinaryXmmRegister b) {
  BinaryCodeBuffer *code = &fn->context->code;
  return wcs_vex3(code, 1, pp, 0, 0, (int)dst, (int)b, (int)a) &&
         binary_code_buffer_append_u8(code, opcode) &&
         binary_code_buffer_append_u8(
             code, (unsigned char)(0xC0 | ((dst & 7) << 3) | (b & 7)));
}

static int vex_scalar_arith(MirFunction *fn, MirOpcode op, int width,
                            BinaryXmmRegister dst, BinaryXmmRegister a,
                            BinaryXmmRegister b) {
  unsigned char opcode;
  switch (op) {
  case MIR_FADD: opcode = 0x58; break;
  case MIR_FSUB: opcode = 0x5C; break;
  case MIR_FMUL: opcode = 0x59; break;
  case MIR_FDIV: opcode = 0x5E; break;
  case MIR_FXOR:
    return vex_xmm_3op(fn, 1, 0x57, dst, a, b);
  default: return 0;
  }
  return vex_xmm_3op(fn, width == 16 ? 1 : (width == 4 ? 2 : 3), opcode, dst,
                     a, b);
}

static int encode_fbinop(MirFunction *fn, const MirInst *in) {
  int w = in->width;
  BinaryXmmRegister D;
  int ok;

  int dst_in_reg = dst_is_xmm_reg(fn, &in->dst, &D);
  if (!dst_in_reg) {
    D = FSCRATCH_A;
  }
  BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
  if (!ok) {
    return enc_err(fn, "out of memory in float op");
  }
  BinaryXmmRegister bval = xmm_value(fn, &in->b, FSCRATCH_B, w, &ok);
  if (!ok || !vex_scalar_arith(fn, in->op, w, D, aval, bval)) {
    return enc_err(fn, "out of memory in float op");
  }
  return dst_in_reg ? 1 : xmm_store(fn, &in->dst, FSCRATCH_A, w);
}

static int encode_cvtsi2f(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_A;
  int done;
  if (in->is_unsigned) {
    done = code_generator_binary_emit_unsigned_int_to_float(
        fn->context, in->width == 4 ? 32 : 64, target, areg, SCRATCH_A,
        SCRATCH_B);
  } else {
    done = (in->width == 4) ? binary_emit_cvtsi2ss_xmm_reg(code, target, areg)
                            : binary_emit_cvtsi2sd_xmm_reg(code, target, areg);
  }
  if (!done) {
    return enc_err(fn, "out of memory in cvtsi2f");
  }
  return (target == FSCRATCH_A) ? xmm_store(fn, &in->dst, FSCRATCH_A, in->width)
                                : 1;
}

static int encode_cvtf2si(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryXmmRegister xval = xmm_value(fn, &in->a, FSCRATCH_A, in->width, &ok);
  if (!ok) {
    return 0;
  }
  BinaryGpRegister D;
  BinaryGpRegister target = dst_is_reg(fn, &in->dst, &D) ? D : SCRATCH_A;
  int done;
  if (in->is_unsigned) {
    done = code_generator_binary_emit_float_to_unsigned_int(
        fn->context, in->width == 4 ? 32 : 64, target, xval, SCRATCH_B,
        FSCRATCH_B);
  } else {
    done = (in->width == 4)
               ? binary_emit_cvttss2si_reg_xmm(code, target, xval)
               : binary_emit_cvttsd2si_reg_xmm(code, target, xval);
  }
  if (!done) {
    return enc_err(fn, "out of memory in cvtf2si");
  }
  return (target == SCRATCH_A) ? store_from(fn, &in->dst, SCRATCH_A) : 1;
}

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

static int encode_cvtph2ps(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, 4, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_B;
  int done = wcs_avx_vcvtph2ps_xmm(code, (int)target, (int)aval);
  if (!done) {
    return enc_err(fn, "out of memory in cvtph2ps");
  }
  return (target == FSCRATCH_B) ? xmm_store(fn, &in->dst, FSCRATCH_B, 4)
                                : 1;
}

static int encode_cvtps2ph(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  int ok;
  BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, 4, &ok);
  if (!ok) {
    return 0;
  }
  BinaryXmmRegister D;
  BinaryXmmRegister target = dst_is_xmm_reg(fn, &in->dst, &D) ? D : FSCRATCH_B;
  int done = wcs_avx_vcvtps2ph_xmm(code, (int)target, (int)aval, 0);
  if (!done) {
    return enc_err(fn, "out of memory in cvtps2ph");
  }
  return (target == FSCRATCH_B) ? xmm_store(fn, &in->dst, FSCRATCH_B, 4)
                                : 1;
}

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
    op2 = is_signed ? 0xBE : 0xB6;
    break;
  case 2:
    rexw = 1;
    has2 = 1;
    op1 = 0x0F;
    op2 = is_signed ? 0xBF : 0xB7;
    break;
  case 4:
    if (is_signed) {
      rexw = 1;
      op1 = 0x63;
    } else {
      op1 = 0x8B;
    }
    break;
  case 8:
    rexw = 1;
    op1 = 0x8B;
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

typedef struct {
  int prefix66;
  int rexw;
  unsigned char op;
} MirStoreForm;

static int mir_width_is_scalar(int width) {
  return width == 1 || width == 2 || width == 4 || width == 8;
}

static MirStoreForm mir_store_form(int width) {
  MirStoreForm form;

  form.prefix66 = width == 2;
  form.rexw = width == 8;
  form.op = width == 1 ? 0x88 : 0x89;
  return form;
}

static int mir_store_needs_forced_rex(int width, BinaryGpRegister val) {
  return width == 1 && val >= BINARY_GP_RSP && val <= BINARY_GP_RDI;
}

static int mir_emit_store_at(BinaryCodeBuffer *code, int width,
                             BinaryGpRegister val, BinaryGpRegister addr,
                             int displacement) {
  MirStoreForm form = mir_store_form(width);

  if (mir_store_needs_forced_rex(width, val)) {
    return binary_emit_memory_access_ex_forced(code, form.prefix66, form.rexw,
                                               form.op, 0, 0, val, addr,
                                               displacement);
  }
  return binary_emit_memory_access_ex(code, form.prefix66, form.rexw, form.op,
                                      0, 0, val, addr, displacement);
}

static int mir_emit_store_sib(BinaryCodeBuffer *code, int width,
                              BinaryGpRegister val, BinaryGpRegister base,
                              BinaryGpRegister index, int scale,
                              int displacement) {
  MirStoreForm form = mir_store_form(width);

  if (mir_store_needs_forced_rex(width, val)) {
    return binary_emit_memory_access_sib_forced(
        code, form.prefix66, form.rexw, form.op, 0, 0, val, base, index, scale,
        displacement);
  }
  return binary_emit_memory_access_sib(code, form.prefix66, form.rexw, form.op,
                                       0, 0, val, base, index, scale,
                                       displacement);
}


static int encode_mov_float(MirFunction *fn, const MirInst *in) {
  BinaryFunctionContext *ctx = fn->context;

  int ok;
  int w = in->width;
  unsigned char prefix = (w == 16) ? 0x66 : ((w == 4) ? 0xF3 : 0xF2);
  if (in->a.kind == MIR_OPK_MEM) {
    if (in->a.mem.index != MIR_VREG_NONE) {
      return enc_err(fn, "scaled index in a float load");
    }
    int base_disp;
    BinaryGpRegister addr =
        mir_mem_base_reg(fn, &in->a.mem, SCRATCH_B, &base_disp, &ok);
    if (!ok) {
      return 0;
    }
    BinaryXmmRegister target;
    int direct = dst_is_xmm_reg(fn, &in->dst, &target);
    if (!direct) {
      target = FSCRATCH_A;
    }
    if (!simd_emit_prefixed_xmm_mem_disp(&ctx->code, prefix, 0x10, target,
                                         addr, base_disp)) {
      return enc_err(fn, "out of memory in float load");
    }
    return direct ? 1 : xmm_store(fn, &in->dst, FSCRATCH_A, w);
  }
  if (in->dst.kind == MIR_OPK_MEM) {
    if (in->dst.mem.index != MIR_VREG_NONE) {
      return enc_err(fn, "scaled index in a float store");
    }
    int base_disp;
    BinaryGpRegister addr =
        mir_mem_base_reg(fn, &in->dst.mem, SCRATCH_B, &base_disp, &ok);
    if (!ok) {
      return 0;
    }
    BinaryXmmRegister val = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
    if (!ok) {
      return 0;
    }
    if (!simd_emit_prefixed_xmm_mem_disp(&ctx->code, prefix, 0x11, val, addr,
                                         base_disp)) {
      return enc_err(fn, "out of memory in float store");
    }
    return 1;
  }
  if (in->a.kind == MIR_OPK_FIMM) {
    BinaryXmmRegister target;
    if (dst_is_xmm_reg(fn, &in->dst, &target)) {
      return xmm_load_fimm(fn, (uint64_t)in->a.imm, target, w);
    }
  }
  BinaryXmmRegister sval = xmm_value(fn, &in->a, FSCRATCH_A, w, &ok);
  if (!ok) {
    return 0;
  }
  return xmm_store(fn, &in->dst, sval, w);
}

static int encode_mov_load(MirFunction *fn, const MirInst *in) {
  BinaryFunctionContext *ctx = fn->context;

  int ok;
  int is_signed = !in->is_unsigned;
  BinaryGpRegister D;
  int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
  BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
  if (in->a.mem.index != MIR_VREG_NONE) {
    MirOperand iop = mir_op_vreg(in->a.mem.index);
    BinaryGpRegister taken[4];
    int base_disp;
    int tn = 0;
    mir_note_mem_base(fn, &in->a.mem, taken, &tn);
    mir_note_fixed_reg(fn, &iop, taken, &tn);
    BinaryGpRegister vouch[1];
    int vn = mir_reg_in(target, taken, tn) ? 0 : 1;
    vouch[0] = target;
    size_t idx = (size_t)(in - fn->insns);
    BinaryGpRegister base_scratch, index_scratch;
    if (!mir_pick_scratch(fn, idx, SCRATCH_B, taken, tn, vouch, vn,
                          &base_scratch)) {
      return enc_err(fn, "no free scratch register for a scaled load base");
    }
    BinaryGpRegister base_reg =
        mir_mem_base_reg(fn, &in->a.mem, base_scratch, &base_disp, &ok);
    if (!ok) {
      return 0;
    }
    taken[tn++] = base_reg;
    if (!mir_pick_scratch(fn, idx, BINARY_GP_RDX, taken, tn, vouch, vn,
                          &index_scratch)) {
      return enc_err(fn, "no free scratch register for a scaled load index");
    }
    BinaryGpRegister index_reg = value_reg(fn, &iop, index_scratch, &ok);
    if (!ok) {
      return 0;
    }
    if (!emit_ext_load(&ctx->code, target, base_reg, 1, index_reg,
                       in->a.mem.scale, base_disp, in->width,
                       is_signed)) {
      return enc_err(fn, "out of memory in scaled load");
    }
  } else {
    int base_disp;
    BinaryGpRegister base_reg =
        mir_mem_base_reg(fn, &in->a.mem, SCRATCH_B, &base_disp, &ok);
    if (!ok) {
      return 0;
    }
    if (!emit_ext_load(&ctx->code, target, base_reg, 0, BINARY_GP_RSP, 1,
                       base_disp, in->width, is_signed)) {
      return enc_err(fn, "out of memory in load");
    }
  }
  if (!dst_in_reg) {
    return store_from(fn, &in->dst, SCRATCH_A);
  }
  return 1;
}

static int encode_mov_store(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;

  int ok1, ok2;
  int scalar_w = mir_width_is_scalar(in->width);
  if (in->a.kind == MIR_OPK_IMM && scalar_w) {
    int has_index = in->dst.mem.index != MIR_VREG_NONE;
    int base_disp;
    BinaryGpRegister base_reg =
        mir_mem_base_reg(fn, &in->dst.mem, SCRATCH_B, &base_disp, &ok1);
    BinaryGpRegister index_reg = BINARY_GP_RAX;
    if (!ok1) {
      return 0;
    }
    if (has_index) {
      MirOperand iop = mir_op_vreg(in->dst.mem.index);
      index_reg = value_reg(fn, &iop, SCRATCH_A, &ok2);
      if (!ok2) {
        return 0;
      }
    }
    if (binary_emit_mov_mem_imm_width(&ctx->code, base_reg, has_index,
                                      index_reg, in->dst.mem.scale,
                                      base_disp, in->a.imm,
                                      in->width)) {
      return 1;
    }
  }
  if (in->dst.mem.index != MIR_VREG_NONE) {
    MirOperand iop = mir_op_vreg(in->dst.mem.index);
    BinaryGpRegister taken[6];
    int base_disp;
    int tn = 0;
    mir_note_mem_base(fn, &in->dst.mem, taken, &tn);
    mir_note_fixed_reg(fn, &iop, taken, &tn);
    mir_note_fixed_reg(fn, &in->a, taken, &tn);

    size_t idx = (size_t)(in - fn->insns);
    BinaryGpRegister base_scratch, index_scratch, val_scratch;
    if (!mir_pick_scratch(fn, idx, SCRATCH_B, taken, tn, NULL, 0, &base_scratch)) {
      return enc_err(fn, "no free scratch register for a scaled store base");
    }
    BinaryGpRegister base_reg =
        mir_mem_base_reg(fn, &in->dst.mem, base_scratch, &base_disp, &ok1);
    if (!ok1) {
      return 0;
    }
    taken[tn++] = base_reg;
    if (!mir_pick_scratch(fn, idx, BINARY_GP_RDX, taken, tn, NULL, 0, &index_scratch)) {
      return enc_err(fn, "no free scratch register for a scaled store index");
    }
    BinaryGpRegister index_reg = value_reg(fn, &iop, index_scratch, &ok2);
    if (!ok2) {
      return 0;
    }
    taken[tn++] = index_reg;
    int scalar_width = mir_width_is_scalar(in->width);
    if (!scalar_width) {
      taken[tn++] = SCRATCH_B;
    }
    BinaryGpRegister val;
    if (mir_env_addr_store() ||
        !mir_pick_scratch(fn, idx, SCRATCH_A, taken, tn, NULL, 0,
                          &val_scratch)) {
      BinaryGpRegister addr = SCRATCH_B;
      BinaryGpRegister val_stage = SCRATCH_A;
      BinaryGpRegister val_fixed;
      if (mir_operand_fixed_reg(fn, &in->a, &val_fixed) &&
          val_fixed == addr) {
        addr = SCRATCH_A;
        val_stage = SCRATCH_B;
      }
      if (!binary_emit_lea_reg_base_index_scale_disp(
              &ctx->code, addr, base_reg, index_reg, in->dst.mem.scale,
              base_disp)) {
        return enc_err(fn, "out of memory in scaled store address");
      }
      val = value_reg(fn, &in->a, val_stage, &ok1);
      if (!ok1) {
        return 0;
      }
      if (!scalar_width) {
        if (!code_generator_binary_emit_store_to_address(g, ctx, addr,
                                                         in->width, val)) {
          return enc_err(fn, "out of memory in store");
        }
        return 1;
      }
      int done = mir_emit_store_at(&ctx->code, in->width, val, addr, 0);
      if (!done) {
        return enc_err(fn, "out of memory in scaled store");
      }
      return 1;
    }
    val = value_reg(fn, &in->a, val_scratch, &ok1);
    if (!ok1) {
      return 0;
    }
    if (scalar_width) {
      int done = mir_emit_store_sib(&ctx->code, in->width, val, base_reg,
                                    index_reg, in->dst.mem.scale, base_disp);
      if (!done) {
        return enc_err(fn, "out of memory in scaled store");
      }
      return 1;
    }
    if (!binary_emit_lea_reg_base_index_scale_disp(
            &ctx->code, SCRATCH_B, base_reg, index_reg, in->dst.mem.scale,
            base_disp)) {
      return enc_err(fn, "out of memory in scaled store address");
    }
    if (!code_generator_binary_emit_store_to_address(g, ctx, SCRATCH_B,
                                                     in->width, val)) {
      return enc_err(fn, "out of memory in store");
    }
    return 1;
  }
  int base_disp;
  BinaryGpRegister addr =
      mir_mem_base_reg(fn, &in->dst.mem, SCRATCH_B, &base_disp, &ok1);
  if (!ok1) {
    return 0;
  }
  BinaryGpRegister val = value_reg(fn, &in->a, SCRATCH_A, &ok2);
  if (!ok2) {
    return 0;
  }
  if (mir_width_is_scalar(in->width)) {
    int done = mir_emit_store_at(&ctx->code, in->width, val, addr, base_disp);
    if (!done) {
      return enc_err(fn, "out of memory in store");
    }
    return 1;
  }
  if (base_disp != 0) {
    if (!binary_emit_lea_reg_mem(&ctx->code, SCRATCH_B, addr,
                                 base_disp)) {
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

static int encode_mov(MirFunction *fn, const MirInst *in) {
  BinaryFunctionContext *ctx = fn->context;

  if (in->is_float) {
    return encode_mov_float(fn, in);
  }
  if (in->a.kind == MIR_OPK_MEM) {
    return encode_mov_load(fn, in);
  }
  if (in->dst.kind == MIR_OPK_MEM) {
    return encode_mov_store(fn, in);
  }

  if (in->a.kind == MIR_OPK_IMM) {
    BinaryGpRegister D;
    if (dst_is_reg(fn, &in->dst, &D)) {
      if (!binary_emit_mov_reg_imm64(&ctx->code, D, (uint64_t)in->a.imm)) {
        return enc_err(fn, "out of memory in immediate move");
      }
      return 1;
    }
    if (in->dst.kind == MIR_OPK_VREG) {
      const MirVreg *v = &fn->vregs[in->dst.vreg];
      if (in->a.imm >= INT32_MIN && in->a.imm <= INT32_MAX) {
        if (!binary_emit_mov_mem_imm32(&ctx->code, frame_base(fn),
                                       frame_disp(fn, -spill_off(v)),
                                       (int32_t)in->a.imm)) {
          return enc_err(fn, "out of memory in immediate spill store");
        }
        return 1;
      }
    }
  }

  int ok;
  BinaryGpRegister src = value_reg(fn, &in->a, SCRATCH_A, &ok);
  if (!ok) {
    return 0;
  }
  return store_from(fn, &in->dst, src);
}

static int mir_has_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op == MIR_CALL ||
        fn->insns[i].op == MIR_CALL_INDIRECT ||
        fn->insns[i].op == MIR_SYSCALL ||
        fn->insns[i].op == MIR_TRAP ||
        fn->insns[i].op == MIR_INLINE_ASM) {
      return 1;
    }
  }
  return 0;
}

static int mir_layout_frame(MirFunction *fn) {
  BinaryFunctionContext *ctx = fn->context;
  int spill = fn->spill_bytes;
  for (size_t i = 0; i < ctx->saved_register_count; i++) {
    ctx->saved_register_offsets[i] = spill + (int)((i + 1) * 8);
  }
  int after_gp = spill + (int)(ctx->saved_register_count * 8);
  for (size_t i = 0; i < ctx->saved_xmm_count; i++) {
    ctx->saved_xmm_offsets[i] = after_gp + (int)((i + 1) * 16);
  }
  int raw = after_gp + (int)(ctx->saved_xmm_count * 16);
  if (mir_has_calls(fn)) {
    raw += fn->outgoing_indirect_bytes + 32 + fn->outgoing_stack_bytes;
  }
  if (!binary_align_up_int(raw, 16, &ctx->frame_size)) {
    return enc_err(fn, "stack frame too large");
  }
  ctx->raw_frame_size = raw;
  if (ir_machine_collecting()) {
    long long spilled = 0;
    for (size_t v = 0; v < fn->vreg_count; v++) {
      if (fn->vregs[v].spill_offset != 0) {
        spilled++;
      }
    }
    mir_encode_last_spills = spilled;
  }
  return 1;
}

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

static int mir_emit_gp_widen(MirFunction *fn, BinaryGpRegister dst,
                             BinaryGpRegister src, int width, int is_signed);

static int mir_home_gp_stack_param(MirFunction *fn, const MirParam *p,
                                   int rbp_offset) {
  BinaryCodeBuffer *code = &fn->context->code;
  MirOperand dst = mir_op_vreg(p->vreg);
  BinaryGpRegister D;
  if (dst_is_reg(fn, &dst, &D)) {
    return binary_emit_mov_reg_mem(code, D, frame_base(fn),
                                   frame_disp(fn, rbp_offset)) &&
                   mir_emit_gp_widen(fn, D, D, p->width, p->is_signed)
               ? 1
               : enc_err(fn, "out of memory homing stack parameter");
  }
  if (!binary_emit_mov_reg_mem(code, SCRATCH_A, frame_base(fn),
                               frame_disp(fn, rbp_offset)) ||
      !mir_emit_gp_widen(fn, SCRATCH_A, SCRATCH_A, p->width, p->is_signed)) {
    return enc_err(fn, "out of memory homing stack parameter");
  }
  return store_from(fn, &dst, SCRATCH_A);
}

typedef struct {
  BinaryXmmRegister src;
  int is_spill;
  int dst;
  int width;
  int done;
} MirXmmMove;

static int mir_home_float_params(MirFunction *fn, MirXmmMove *mv, int n) {
  BinaryCodeBuffer *code = &fn->context->code;
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
  int remaining = 0;
  for (int i = 0; i < n; i++) {
    if (!mv[i].done && (BinaryXmmRegister)mv[i].dst == mv[i].src) {
      mv[i].done = 1;
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

static int mir_home_indirect_return(MirFunction *fn, const BinaryAbi *abi) {
  MirOperand dst;

  if (!fn->returns_indirect || fn->indirect_return_vreg == MIR_VREG_NONE ||
      !fn->vregs[fn->indirect_return_vreg].assigned) {
    return 1;
  }
  dst = mir_op_vreg(fn->indirect_return_vreg);
  if (!store_from(fn, &dst, abi->indirect_return_register)) {
    return enc_err(fn, "out of memory homing indirect-return pointer");
  }
  return 1;
}

static int mir_incoming_stack_offset(const BinaryAbi *abi,
                                     const BinaryArgLocation *loc) {
  return 16 + abi->shadow_space_size + loc->stack_offset;
}

static int mir_home_memory_param(MirFunction *fn, const MirParam *p,
                                 const BinaryArgLocation *loc,
                                 const BinaryAbi *abi) {
  MirOperand dst = mir_op_vreg(p->vreg);

  if (loc->kind != BINARY_ARG_ON_STACK ||
      !binary_emit_lea_reg_mem(&fn->context->code, SCRATCH_A, frame_base(fn),
                               frame_disp(fn,
                                          mir_incoming_stack_offset(abi,
                                                                    loc))) ||
      !store_from(fn, &dst, SCRATCH_A)) {
    return enc_err(fn, "out of memory homing a memory-class parameter");
  }
  return 1;
}

static int mir_home_eightbyte_address(MirFunction *fn, const MirParam *p) {
  BinaryCodeBuffer *code = &fn->context->code;
  MirOperand dst = mir_op_vreg(p->vreg);
  int storage = spill_off(&fn->vregs[p->sysv_storage]);

  if (!binary_emit_lea_reg_mem(code, SCRATCH_A, frame_base(fn),
                               frame_disp(fn, -storage)) ||
      !store_from(fn, &dst, SCRATCH_A)) {
    return enc_err(fn, "out of memory homing an aggregate parameter");
  }
  return 1;
}

static int mir_home_eightbyte_stores(MirFunction *fn, const MirParam *p,
                                     const BinaryArgLocation *locs,
                                     const BinaryAbi *abi) {
  BinaryCodeBuffer *code = &fn->context->code;
  int storage = spill_off(&fn->vregs[p->sysv_storage]);

  for (int e = 0; e < p->sysv_eightbytes; e++) {
    const BinaryArgLocation *loc = &locs[e];
    int at = frame_disp(fn, -storage) + 8 * e;
    int ok;

    if (loc->kind == BINARY_ARG_IN_GP_REGISTER) {
      ok = binary_emit_mov_mem_reg(code, frame_base(fn), at, loc->gp_register);
    } else if (loc->kind == BINARY_ARG_IN_XMM_REGISTER) {
      ok = binary_emit_movq_reg_xmm(code, SCRATCH_A, loc->xmm_register) &&
           binary_emit_mov_mem_reg(code, frame_base(fn), at, SCRATCH_A);
    } else {
      ok = binary_emit_mov_reg_mem(
               code, SCRATCH_A, frame_base(fn),
               frame_disp(fn, mir_incoming_stack_offset(abi, loc))) &&
           binary_emit_mov_mem_reg(code, frame_base(fn), at, SCRATCH_A);
    }
    if (!ok) {
      return enc_err(fn, "out of memory rebuilding an aggregate parameter");
    }
  }
  return 1;
}

static int mir_home_direct_sse_param(MirFunction *fn, const MirParam *p,
                                     const BinaryArgLocation *loc) {
  MirOperand dst = mir_op_vreg(p->vreg);

  if (!binary_emit_movq_reg_xmm(&fn->context->code, SCRATCH_A,
                                loc->xmm_register) ||
      !store_from(fn, &dst, SCRATCH_A)) {
    return enc_err(fn, "out of memory homing a float-class aggregate");
  }
  return 1;
}

typedef struct {
  BinaryGpRegister src;
  int dst;
  int width;
  int is_signed;
  int is_spill;
  int done;
} MirGpMove;

static void mir_record_gp_move(MirFunction *fn, const MirParam *p,
                               BinaryGpRegister src, MirGpMove *gm,
                               int *ngm) {
  const MirVreg *vr = &fn->vregs[p->vreg];

  gm[*ngm].src = src;
  gm[*ngm].width = p->width;
  gm[*ngm].is_signed = p->is_signed;
  gm[*ngm].done = 0;
  gm[*ngm].is_spill = vr->in_register ? 0 : 1;
  gm[*ngm].dst = vr->in_register ? vr->phys : vr->spill_offset;
  (*ngm)++;
}

static int mir_emit_gp_widen(MirFunction *fn, BinaryGpRegister dst,
                             BinaryGpRegister src, int width, int is_signed) {
  BinaryCodeBuffer *code = &fn->context->code;
  if (width == 8) {
    return dst == src ? 1 : binary_emit_mov_reg_reg(code, dst, src);
  }
  if (width == 4) {
    return is_signed ? binary_emit_movsxd_reg_reg32(code, dst, src)
                     : binary_emit_movzx_reg_reg32(code, dst, src);
  }
  if (width == 2) {
    return is_signed ? binary_emit_movsx_reg_reg16(code, dst, src)
                     : binary_emit_movzx_reg_reg16(code, dst, src);
  }
  return is_signed ? binary_emit_movsx_reg_reg8(code, dst, src)
                   : binary_emit_movzx_reg_reg8(code, dst, src);
}

static int mir_home_gp_params(MirFunction *fn, MirGpMove *gm, int n) {
  BinaryCodeBuffer *code = &fn->context->code;
  int remaining = 0;

  for (int i = 0; i < n; i++) {
    MirOperand home;
    if (!gm[i].is_spill) {
      continue;
    }
    if (!mir_emit_gp_widen(fn, SCRATCH_A, gm[i].src, gm[i].width,
                           gm[i].is_signed)) {
      return enc_err(fn, "out of memory extending parameter");
    }
    home = mir_op_none();
    if (!binary_emit_mov_mem_reg(code, frame_base(fn),
                                 frame_disp(fn, -gm[i].dst), SCRATCH_A)) {
      return enc_err(fn, "out of memory homing parameter");
    }
    (void)home;
    gm[i].done = 1;
  }

  for (int i = 0; i < n; i++) {
    if (!gm[i].done && (BinaryGpRegister)gm[i].dst == gm[i].src &&
        gm[i].width == 8) {
      gm[i].done = 1;
    }
    if (!gm[i].done) {
      remaining++;
    }
  }

  while (remaining > 0) {
    int progressed = 0;
    for (int i = 0; i < n; i++) {
      int dst_is_src = 0;
      if (gm[i].done) {
        continue;
      }
      for (int j = 0; j < n; j++) {
        if (!gm[j].done && j != i &&
            gm[j].src == (BinaryGpRegister)gm[i].dst) {
          dst_is_src = 1;
          break;
        }
      }
      if (!dst_is_src) {
        if (!mir_emit_gp_widen(fn, (BinaryGpRegister)gm[i].dst, gm[i].src,
                               gm[i].width, gm[i].is_signed)) {
          return enc_err(fn, "out of memory homing parameter");
        }
        gm[i].done = 1;
        remaining--;
        progressed = 1;
      }
    }
    if (progressed) {
      continue;
    }
    {
      int i;
      for (i = 0; i < n; i++) {
        if (!gm[i].done) {
          break;
        }
      }
      if (i == n) {
        break;
      }
      if (!binary_emit_mov_reg_reg(code, SCRATCH_A,
                                   (BinaryGpRegister)gm[i].dst)) {
        return enc_err(fn, "out of memory breaking a parameter cycle");
      }
      for (int j = 0; j < n; j++) {
        if (!gm[j].done && gm[j].src == (BinaryGpRegister)gm[i].dst) {
          gm[j].src = SCRATCH_A;
        }
      }
      if (!mir_emit_gp_widen(fn, (BinaryGpRegister)gm[i].dst, gm[i].src,
                             gm[i].width, gm[i].is_signed)) {
        return enc_err(fn, "out of memory homing parameter");
      }
      gm[i].done = 1;
      remaining--;
    }
  }
  return 1;
}

static int mir_home_integer_param(MirFunction *fn, const MirParam *p,
                                  const BinaryArgLocation *loc,
                                  const BinaryAbi *abi) {
  if (loc->kind == BINARY_ARG_IN_GP_REGISTER) {
    return mir_home_gp_param(fn, p, loc->gp_register);
  }
  if (loc->kind == BINARY_ARG_ON_STACK) {
    return mir_home_gp_stack_param(fn, p,
                                   mir_incoming_stack_offset(abi, loc));
  }
  return enc_err(fn, "unsupported parameter location");
}

static int mir_home_float_stack_param(MirFunction *fn, const MirParam *p,
                                      int offset) {
  BinaryCodeBuffer *code = &fn->context->code;
  MirVreg *vr = &fn->vregs[p->vreg];
  int ok;

  if (vr->in_register) {
    ok = (p->width == 4) ? wcs_movss_xmm_mem(code, vr->phys, frame_base(fn),
                                             frame_disp(fn, offset))
                         : wcs_movsd_xmm_mem(code, vr->phys, frame_base(fn),
                                             frame_disp(fn, offset));
  } else {
    ok = binary_emit_mov_reg_mem(code, SCRATCH_A, frame_base(fn),
                                 frame_disp(fn, offset)) &&
         binary_emit_mov_mem_reg(code, frame_base(fn),
                                 frame_disp(fn, -vr->spill_offset), SCRATCH_A);
  }
  if (!ok) {
    return enc_err(fn, "out of memory homing float stack parameter");
  }
  return 1;
}

static void mir_record_xmm_move(MirFunction *fn, const MirParam *p,
                                const BinaryArgLocation *loc, MirXmmMove *xm,
                                int *nxm) {
  const MirVreg *vr = &fn->vregs[p->vreg];

  xm[*nxm].src = loc->xmm_register;
  xm[*nxm].width = p->width;
  xm[*nxm].done = 0;
  xm[*nxm].is_spill = vr->in_register ? 0 : 1;
  xm[*nxm].dst = vr->in_register ? vr->phys : vr->spill_offset;
  (*nxm)++;
}

static int mir_home_parameters(MirFunction *fn) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  BinaryArgLocation locs[MIR_PARAM_SLOTS];
  size_t first_slot[MIR_MAX_PARAMS];
  size_t nslots = 0;
  MirXmmMove xm[MIR_MAX_PARAMS];
  MirGpMove gm[MIR_MAX_PARAMS];
  const MirParam *fstack[MIR_MAX_PARAMS];
  int fstack_off[MIR_MAX_PARAMS];
  const MirParam *eb[MIR_MAX_PARAMS];
  const BinaryArgLocation *eb_loc[MIR_MAX_PARAMS];
  const MirParam *late[MIR_MAX_PARAMS];
  const BinaryArgLocation *late_loc[MIR_MAX_PARAMS];
  int late_kind[MIR_MAX_PARAMS];
  int nxm = 0;
  int ngm = 0;
  int nfstack = 0;
  int neb = 0;
  int nlate = 0;

  if (!mir_home_indirect_return(fn, abi)) {
    return 0;
  }
  if (fn->param_count == 0) {
    return 1;
  }
  if (!mir_param_layout(fn, abi, locs, first_slot, &nslots)) {
    return enc_err(fn, "failed to compute parameter layout");
  }
  for (size_t i = 0; i < fn->param_count; i++) {
    const MirParam *p = &fn->params[i];
    const BinaryArgLocation *loc = &locs[first_slot[i]];
    int ok = 1;

    if (!fn->vregs[p->vreg].assigned) {
      continue;
    }
    if (p->sysv_in_memory) {
      late[nlate] = p;
      late_loc[nlate] = loc;
      late_kind[nlate] = 0;
      nlate++;
      continue;
    } else if (p->sysv_eightbytes > 0) {
      eb[neb] = p;
      eb_loc[neb] = loc;
      neb++;
      late[nlate] = p;
      late_loc[nlate] = loc;
      late_kind[nlate] = 1;
      nlate++;
      continue;
    } else if (p->sysv_direct_sse && loc->kind == BINARY_ARG_IN_XMM_REGISTER) {
      late[nlate] = p;
      late_loc[nlate] = loc;
      late_kind[nlate] = 2;
      nlate++;
      continue;
    } else if (!p->is_float) {
      if (loc->kind == BINARY_ARG_IN_GP_REGISTER) {
        mir_record_gp_move(fn, p, loc->gp_register, gm, &ngm);
        continue;
      }
      if (loc->kind == BINARY_ARG_ON_STACK) {
        late[nlate] = p;
        late_loc[nlate] = loc;
        late_kind[nlate] = 3;
        nlate++;
        continue;
      }
      ok = mir_home_integer_param(fn, p, loc, abi);
    } else if (loc->kind == BINARY_ARG_ON_STACK) {
      fstack[nfstack] = p;
      fstack_off[nfstack] = mir_incoming_stack_offset(abi, loc);
      nfstack++;
      continue;
    } else if (loc->kind != BINARY_ARG_IN_XMM_REGISTER) {
      ok = enc_err(fn, "unsupported float parameter location");
    } else {
      mir_record_xmm_move(fn, p, loc, xm, &nxm);
      continue;
    }
    if (!ok) {
      return 0;
    }
  }
  for (int i = 0; i < neb; i++) {
    if (!mir_home_eightbyte_stores(fn, eb[i], eb_loc[i], abi)) {
      return 0;
    }
  }
  if (!mir_home_gp_params(fn, gm, ngm)) {
    return 0;
  }
  for (int i = 0; i < nlate; i++) {
    int ok;
    switch (late_kind[i]) {
    case 0:
      ok = mir_home_memory_param(fn, late[i], late_loc[i], abi);
      break;
    case 1:
      ok = mir_home_eightbyte_address(fn, late[i]);
      break;
    case 2:
      ok = mir_home_direct_sse_param(fn, late[i], late_loc[i]);
      break;
    default:
      ok = mir_home_integer_param(fn, late[i], late_loc[i], abi);
      break;
    }
    if (!ok) {
      return 0;
    }
  }
  if (!mir_home_float_params(fn, xm, nxm)) {
    return 0;
  }
  for (int i = 0; i < nfstack; i++) {
    if (!mir_home_float_stack_param(fn, fstack[i], fstack_off[i])) {
      return 0;
    }
  }
  return 1;
}

typedef struct {
  const char **names;
  size_t *values;
  size_t mask;
} MirEncNameMap;

static int mir_enc_name_map_init(MirEncNameMap *map, size_t expected) {
  size_t capacity = 16;
  while (capacity < expected * 2) {
    capacity *= 2;
  }
  map->names = (const char **)calloc(capacity, sizeof(*map->names));
  map->values = (size_t *)calloc(capacity, sizeof(*map->values));
  map->mask = capacity - 1;
  if (!map->names || !map->values) {
    free(map->names);
    free(map->values);
    map->names = NULL;
    map->values = NULL;
    return 0;
  }
  return 1;
}

static void mir_enc_name_map_free(MirEncNameMap *map) {
  free(map->names);
  free(map->values);
  map->names = NULL;
  map->values = NULL;
}

static void mir_enc_name_map_put_first(MirEncNameMap *map, const char *name,
                                       size_t value) {
  size_t h = mettle_fnv1a_hash(name) & map->mask;
  while (map->names[h]) {
    if (strcmp(map->names[h], name) == 0) {
      return;
    }
    h = (h + 1) & map->mask;
  }
  map->names[h] = name;
  map->values[h] = value;
}

static int mir_enc_name_map_get(const MirEncNameMap *map, const char *name,
                                size_t *value) {
  size_t h = mettle_fnv1a_hash(name) & map->mask;
  while (map->names[h]) {
    if (strcmp(map->names[h], name) == 0) {
      if (value) {
        *value = map->values[h];
      }
      return 1;
    }
    h = (h + 1) & map->mask;
  }
  return 0;
}

static int mir_insn_is_label_branch(const MirInst *b) {
  return (b->op == MIR_JMP || b->op == MIR_JCC || b->op == MIR_CMPBR ||
          b->op == MIR_BT || b->op == MIR_FCMPBR) &&
         b->dst.kind == MIR_OPK_LABEL && b->dst.sym;
}

static int mir_branch_targets_build(const MirFunction *fn,
                                    MirEncNameMap *targets) {
  size_t expected = 0;
  for (size_t k = 0; k < fn->insn_count; k++) {
    const MirInst *b = &fn->insns[k];
    if (b->op == MIR_JMP_TABLE && b->aux) {
      expected += ((const MirJumpTable *)b->aux)->count;
    } else if (mir_insn_is_label_branch(b)) {
      expected++;
    }
  }
  if (!mir_enc_name_map_init(targets, expected)) {
    return 0;
  }
  for (size_t k = 0; k < fn->insn_count; k++) {
    const MirInst *b = &fn->insns[k];
    if (b->op == MIR_JMP_TABLE) {
      const MirJumpTable *tbl = (const MirJumpTable *)b->aux;
      for (size_t t = 0; tbl && t < tbl->count; t++) {
        if (tbl->labels[t]) {
          mir_enc_name_map_put_first(targets, tbl->labels[t], k);
        }
      }
    } else if (mir_insn_is_label_branch(b)) {
      mir_enc_name_map_put_first(targets, b->dst.sym, k);
    }
  }
  return 1;
}

static int mir_label_is_branch_target(const MirFunction *fn,
                                      const MirEncNameMap *targets,
                                      const char *name) {
  if (!name) {
    return 1;
  }
  if (targets && targets->names) {
    return mir_enc_name_map_get(targets, name, NULL);
  }
  for (size_t k = 0; k < fn->insn_count; k++) {
    const MirInst *b = &fn->insns[k];
    if (b->op == MIR_JMP_TABLE) {
      const MirJumpTable *tbl = (const MirJumpTable *)b->aux;
      if (tbl) {
        for (size_t t = 0; t < tbl->count; t++) {
          if (tbl->labels[t] && strcmp(tbl->labels[t], name) == 0) {
            return 1;
          }
        }
      }
      continue;
    }
    if (b->op != MIR_JMP && b->op != MIR_JCC && b->op != MIR_CMPBR &&
        b->op != MIR_BT && b->op != MIR_FCMPBR) {
      continue;
    }
    if (b->dst.kind == MIR_OPK_LABEL && b->dst.sym &&
        strcmp(b->dst.sym, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int mir_cmp_gap_is_empty(const MirFunction *fn,
                                const MirEncNameMap *targets,
                                const MirInst *in) {
  if (in->op == MIR_NOP) {
    return 1;
  }
  if (in->op == MIR_LABEL) {
    return in->dst.kind == MIR_OPK_LABEL &&
           !mir_label_is_branch_target(fn, targets, in->dst.sym);
  }
  if (in->op == MIR_MOV && !in->is_float && in->dst.kind == MIR_OPK_VREG &&
      in->a.kind == MIR_OPK_VREG && in->dst.vreg != MIR_VREG_NONE &&
      in->a.vreg != MIR_VREG_NONE &&
      fn->vregs[in->dst.vreg].in_register &&
      fn->vregs[in->a.vreg].in_register) {
    return fn->vregs[in->dst.vreg].phys == fn->vregs[in->a.vreg].phys &&
           fn->vregs[in->dst.vreg].rclass == fn->vregs[in->a.vreg].rclass;
  }
  return 0;
}

static int mir_cmp_operand_reusable(const MirFunction *fn,
                                    const MirOperand *x,
                                    const MirOperand *y) {
  if (x->kind != y->kind) {
    return 0;
  }
  if (x->kind == MIR_OPK_IMM) {
    return x->imm == y->imm;
  }
  if (x->kind != MIR_OPK_VREG || x->vreg == MIR_VREG_NONE ||
      y->vreg == MIR_VREG_NONE) {
    return 0;
  }
  if (!fn->vregs[x->vreg].in_register || !fn->vregs[y->vreg].in_register) {
    return 0;
  }
  return fn->vregs[x->vreg].phys == fn->vregs[y->vreg].phys &&
         fn->vregs[x->vreg].rclass == fn->vregs[y->vreg].rclass;
}

static int mir_trap_stage_details(MirFunction *fn, const MirInst *in) {
  BinaryCodeBuffer *code = &fn->context->code;
  MirOperand zero = mir_op_imm(0);
  BinaryGpRegister first;
  int ok = 1;

  if (!binary_emit_push_reg(code, SCRATCH_A)) {
    return enc_err(fn, "out of memory staging a trap detail");
  }
  first = value_reg(fn, in->b.kind == MIR_OPK_NONE ? &zero : &in->b, SCRATCH_A,
                    &ok);
  if (!ok) {
    return 0;
  }
  if (first != SCRATCH_A &&
      !binary_emit_mov_reg_reg(code, SCRATCH_A, first)) {
    return enc_err(fn, "out of memory staging a trap detail");
  }
  if (!binary_emit_pop_reg(code, SCRATCH_B)) {
    return enc_err(fn, "out of memory staging a trap detail");
  }
  return 1;
}

static int mir_trap_open_site(MirFunction *fn, const IRInstruction *ir,
                              const char *filename, const char *pc_label,
                              const char *trap_symbol, int is_ex) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;

  if (!binary_label_table_define(&ctx->labels, pc_label, ctx->code.size) ||
      !code_generator_binary_record_debug_label_export(ctx, pc_label,
                                                       ctx->code.size)) {
    return enc_err(fn, "out of memory recording a trap label");
  }
  if (ir->location.line > 0 &&
      !code_generator_binary_emit_runtime_location_marker(
          g, ctx, ir->location.line, ir->location.column, filename)) {
    return 0;
  }
  if (is_ex && ir->argument_count >= 4) {
    uint32_t kind = ir->arguments[0].kind == IR_OPERAND_INT
                        ? (uint32_t)ir->arguments[0].int_value
                        : 0u;
    const char *message = ir->arguments[1].kind == IR_OPERAND_STRING
                              ? ir->arguments[1].name
                              : NULL;
    code_generator_record_runtime_trap_site(g, pc_label, kind,
                                            ir->location.line,
                                            ir->location.column, filename,
                                            message, NULL);
  }
  return code_generator_binary_declare_external_symbol(g, trap_symbol);
}

static int mir_trap_pass_staged_detail(MirFunction *fn, const BinaryAbi *abi,
                                       size_t index, BinaryGpRegister staged) {
  BinaryCodeBuffer *code = &fn->context->code;

  if (abi->int_param_count > index) {
    return binary_emit_mov_reg_reg(code, abi->int_param_registers[index],
                                   staged);
  }
  return binary_emit_mov_mem_reg(
      code, BINARY_GP_RSP,
      abi->shadow_space_size + (int)(index - 4) * 8, staged);
}

static int mir_trap_emit_detailed_call(MirFunction *fn, const MirInst *in,
                                       const IRInstruction *ir,
                                       const BinaryAbi *abi,
                                       const char *trap_symbol,
                                       const char *pc_label) {
  BinaryFunctionContext *ctx = fn->context;
  BinaryCodeBuffer *code = &ctx->code;
  int stacked = abi->int_param_count < 6 ? 6 - (int)abi->int_param_count : 0;
  uint32_t frame = (uint32_t)(abi->shadow_space_size + stacked * 8);
  size_t disp = 0;

  if ((frame > 0 && !binary_emit_sub_rsp_imm32(code, frame)) ||
      !binary_emit_mov_reg_imm64(code, abi->int_param_registers[0],
                                 ir->arguments[0].kind == IR_OPERAND_INT
                                     ? (uint64_t)ir->arguments[0].int_value
                                     : 0u) ||
      !code_generator_binary_emit_cstring_literal_address(
          fn->generator, ctx, in->a.sym ? in->a.sym : "",
          abi->int_param_registers[1]) ||
      !binary_emit_lea_reg_rip_placeholder(code, abi->int_param_registers[2],
                                           &disp) ||
      !binary_label_fixup_table_add(&ctx->label_fixups, pc_label, disp) ||
      !binary_emit_mov_reg_reg(code, abi->int_param_registers[3],
                               BINARY_GP_RBP) ||
      !mir_trap_pass_staged_detail(fn, abi, 4, SCRATCH_A) ||
      !mir_trap_pass_staged_detail(fn, abi, 5, SCRATCH_B) ||
      !binary_emit_call_placeholder(code, &disp) ||
      !binary_call_relocation_table_add(&ctx->call_relocations, trap_symbol,
                                        disp) ||
      (frame > 0 && !binary_emit_add_rsp_imm32(code, frame))) {
    return enc_err(fn, "out of memory emitting a traced trap");
  }
  return 1;
}

static int mir_trap_emit_message_call(MirFunction *fn, const MirInst *in,
                                      const BinaryAbi *abi,
                                      const char *trap_symbol,
                                      const char *pc_label) {
  BinaryFunctionContext *ctx = fn->context;
  BinaryCodeBuffer *code = &ctx->code;
  size_t disp = 0;

  if (!code_generator_binary_emit_cstring_literal_address(
          fn->generator, ctx, in->a.sym ? in->a.sym : "",
          abi->int_param_registers[0]) ||
      !binary_emit_lea_reg_rip_placeholder(code, abi->int_param_registers[1],
                                           &disp) ||
      !binary_label_fixup_table_add(&ctx->label_fixups, pc_label, disp) ||
      !binary_emit_mov_reg_reg(code, abi->int_param_registers[2],
                               BINARY_GP_RBP) ||
      !binary_emit_call_placeholder(code, &disp) ||
      !binary_call_relocation_table_add(&ctx->call_relocations, trap_symbol,
                                        disp)) {
    return enc_err(fn, "out of memory emitting a traced trap");
  }
  return 1;
}

static int mir_encode_traced_trap(MirFunction *fn, const MirInst *in,
                                  const IRInstruction *ir) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  int is_ex = ir->text && strcmp(ir->text, "mettle_crash_trap_ex") == 0;
  const char *trap_symbol =
      is_ex ? "mettle_crash_trap_ex" : "mettle_crash_trap";
  const char *filename =
      code_generator_runtime_filename(fn->generator, ir->location.filename);
  char *pc_label = NULL;
  int ok;

  if (is_ex && !mir_trap_stage_details(fn, in)) {
    return 0;
  }
  pc_label = code_generator_generate_label(fn->generator,
                                           "mettledbg_trap_pc");
  if (!pc_label) {
    return enc_err(fn, "out of memory creating a trap label");
  }
  ok = mir_trap_open_site(fn, ir, filename, pc_label, trap_symbol, is_ex);
  if (ok) {
    ok = is_ex ? mir_trap_emit_detailed_call(fn, in, ir, abi, trap_symbol,
                                             pc_label)
               : mir_trap_emit_message_call(fn, in, abi, trap_symbol,
                                            pc_label);
  }
  free(pc_label);
  return ok;
}

static int mir_emit_prologue(MirFunction *fn) {
  BinaryFunctionContext *ctx = fn->context;
  BinaryCodeBuffer *code = &ctx->code;
  if (fn->ir_function && fn->ir_function->is_interrupt &&
      !code_generator_binary_emit_interrupt_entry(
          fn->generator, (IRFunction *)fn->ir_function, ctx)) {
    return 0;
  }
  if (ctx->omit_frame_pointer) {
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
    if (!binary_emit_add_rsp_imm32(code, (uint32_t)(ctx->frame_size + 8)) ||
        !binary_emit_ret(code)) {
      return enc_err(fn, "out of memory in epilogue");
    }
  } else if (!binary_emit_mov_reg_reg(code, BINARY_GP_RSP, BINARY_GP_RBP) ||
             !binary_emit_pop_reg(code, BINARY_GP_RBP)) {
    return enc_err(fn, "out of memory in epilogue");
  } else if (fn->ir_function && fn->ir_function->is_interrupt) {
    if (!code_generator_binary_emit_interrupt_exit(
            fn->generator, (IRFunction *)fn->ir_function, ctx)) {
      return 0;
    }
  } else if (!binary_emit_ret(code)) {
    return enc_err(fn, "out of memory in epilogue");
  }
  return 1;
}

static int mir_epilogue_is_shareable(MirFunction *fn) {
  BinaryFunctionContext *ctx = fn->context;
  if (fn->ir_function && fn->ir_function->is_interrupt) {
    return 0;
  }
  if (fn->used_inline_vector) {
    return 0;
  }
  return ctx->saved_register_count + ctx->saved_xmm_count >= 3;
}

static int mir_ret_is_last(MirFunction *fn, size_t index) {
  for (size_t i = index + 1; i < fn->insn_count; i++) {
    MirOpcode op = fn->insns[i].op;
    if (op == MIR_LABEL) {
      continue;
    }
    return 0;
  }
  return 1;
}

static int mir_encode_inline_asm(MirFunction *fn, const MirInst *in) {
  const MirAsmAux *aux = (const MirAsmAux *)in->aux;
  BinaryFunctionContext *ctx = fn->context;
  if (!aux || !aux->ir) {
    return enc_err(fn, "inline asm without its source");
  }
  for (int k = 0; k < aux->count; k++) {
    const MirVreg *v = &fn->vregs[aux->vregs[k]];
    int off = spill_off(v);
    if (off <= 0) {
      return enc_err(fn, "an asm operand has no frame home");
    }
    if (!binary_named_slot_table_add(&ctx->local_slots, aux->names[k], off) ||
        !binary_named_slot_table_add(&ctx->address_taken_symbols,
                                     aux->names[k], off)) {
      return enc_err(fn, "out of memory binding an asm operand");
    }
  }
  return code_generator_binary_emit_inline_asm(fn->generator, ctx, aux->ir);
}

static int encode_load_global(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;
  const char *name = in->a.sym;
  if (!name) {
    return enc_err(fn, "MIR_LOAD_GLOBAL without a symbol");
  }

  if (in->dst.kind == MIR_OPK_VREG &&
      fn->vregs[in->dst.vreg].rclass == MIR_RC_XMM) {
    int width = fn->vregs[in->dst.vreg].width;
    const char *link = code_generator_get_link_symbol_name(g, name);
    const CgSym *s =
        (g && g->ir_program) ? code_generator_lookup_symbol(g, name)
                               : NULL;
    if (!link || !link[0] || !s) {
      return enc_err(fn, "unresolved global in MIR_LOAD_GLOBAL");
    }
    if (s->type && (s->type->kind == MTLC_TYPE_FLOAT16 || s->type->kind == MTLC_TYPE_BFLOAT16)) {
      if (!code_generator_binary_emit_global_symbol_load(g, ctx, link, s->type, s->is_extern, SCRATCH_A)) {
        return enc_err(fn, "out of memory loading float global");
      }
      if (s->type->kind == MTLC_TYPE_FLOAT16) {
        if (!binary_emit_movd_xmm_reg(&ctx->code, FSCRATCH_A, SCRATCH_A)) {
          return enc_err(fn, "out of memory staging float global to xmm");
        }
        if (!wcs_avx_vcvtph2ps_xmm(&ctx->code, (int)FSCRATCH_A, (int)FSCRATCH_A)) {
          return enc_err(fn, "out of memory converting float global");
        }
      } else {
        if (!binary_emit_shift_reg_imm8(&ctx->code, 4, SCRATCH_A, 16)) {
          return enc_err(fn, "out of memory converting float global");
        }
        if (!binary_emit_movd_xmm_reg(&ctx->code, FSCRATCH_A, SCRATCH_A)) {
          return enc_err(fn, "out of memory staging float global to xmm");
        }
      }
      return xmm_store(fn, &in->dst, FSCRATCH_A, width);
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
    const CgSym *s =
        (g && g->ir_program) ? code_generator_lookup_symbol(g, name)
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

static int encode_store_global(MirFunction *fn, const MirInst *in) {
  CodeGenerator *g = fn->generator;
  BinaryFunctionContext *ctx = fn->context;
  const char *name = in->a.sym;
  if (!name) {
    return enc_err(fn, "MIR_STORE_GLOBAL without a symbol");
  }
  BinaryGpRegister src;
  if (in->b.kind == MIR_OPK_VREG &&
      fn->vregs[in->b.vreg].rclass == MIR_RC_XMM) {
    int width = fn->vregs[in->b.vreg].width;
    int xok = 1;
    BinaryXmmRegister xsrc = xmm_value(fn, &in->b, FSCRATCH_A, width, &xok);
    if (!xok) {
      return 0;
    }
    const char *gname = in->a.sym;
    const CgSym *gs = (g && g->ir_program && gname) ? code_generator_lookup_symbol(g, gname) : NULL;
    if (gs && gs->type && (gs->type->kind == MTLC_TYPE_FLOAT16 || gs->type->kind == MTLC_TYPE_BFLOAT16)) {
      if (gs->type->kind == MTLC_TYPE_FLOAT16) {
        if (!wcs_avx_vcvtps2ph_xmm(&ctx->code, (int)FSCRATCH_A, (int)xsrc, 0)) {
          return enc_err(fn, "out of memory converting float global");
        }
        if (!binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A, FSCRATCH_A)) {
          return enc_err(fn, "out of memory staging float global from xmm");
        }
      } else {
        if (!binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A, xsrc)) {
          return enc_err(fn, "out of memory staging float global from xmm");
        }
        {
          BinaryGpRegister tmp = BINARY_GP_R11;
          if (!binary_emit_mov_reg_reg(&ctx->code, tmp, SCRATCH_A)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_shift_reg_imm8(&ctx->code, 5, tmp, 16)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_and_reg_imm32(&ctx->code, tmp, 1)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_alu_reg_imm32(&ctx->code, 0, tmp, 0x7FFF)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_alu_reg_reg(&ctx->code, 0x03, SCRATCH_A, tmp)) {
            return enc_err(fn, "out of memory converting float global");
          }
          if (!binary_emit_shift_reg_imm8(&ctx->code, 5, SCRATCH_A, 16)) {
            return enc_err(fn, "out of memory converting float global");
          }
        }
      }
      src = SCRATCH_A;
    } else {
      int moved = (width == 4)
                      ? binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A, xsrc)
                      : binary_emit_movq_reg_xmm(&ctx->code, SCRATCH_A, xsrc);
      if (!moved) {
        return enc_err(fn, "out of memory staging float global from xmm");
      }
      src = SCRATCH_A;
    }
  } else {
    int rok = 1;
    src = value_reg(fn, &in->b, SCRATCH_A, &rok);
    if (!rok) {
      return 0;
    }
  }
  const char *link = code_generator_get_link_symbol_name(g, name);
  const CgSym *s = (g && g->ir_program) ? code_generator_lookup_symbol(g, name)
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

static int mir_vreg_is_byte_load(const MirFunction *fn, size_t before,
                                 MirVregId v) {
  size_t limit = before > 16 ? before - 16 : 0;
  for (size_t k = before; k-- > limit;) {
    const MirInst *in = &fn->insns[k];
    if (in->dst.kind != MIR_OPK_VREG || in->dst.vreg != v) {
      continue;
    }
    return in->op == MIR_MOV && !in->is_float && in->width == 1 &&
           in->is_unsigned && in->a.kind == MIR_OPK_MEM;
  }
  return 0;
}

static int mir_mem_operand_in_registers(const MirFunction *fn,
                                        const MirOperand *op) {
  if (op->kind != MIR_OPK_MEM || op->mem.base == MIR_VREG_NONE ||
      op->mem.phys_base_valid || op->mem.frame_home_valid) {
    return 0;
  }
  if (!fn->vregs[op->mem.base].in_register) {
    return 0;
  }
  if (op->mem.index != MIR_VREG_NONE) {
    if (!fn->vregs[op->mem.index].in_register) {
      return 0;
    }
    if (op->mem.scale != 1 && op->mem.scale != 2 && op->mem.scale != 4 &&
        op->mem.scale != 8) {
      return 0;
    }
    if (fn->vregs[op->mem.index].phys == BINARY_GP_RSP) {
      return 0;
    }
  }
  if (fn->vregs[op->mem.base].phys == BINARY_GP_RSP) {
    return 0;
  }
  return 1;
}

static int mir_mask_test_fusable(const MirFunction *fn, const int *uses,
                                 size_t i) {
  const MirInst *and_op;
  const MirInst *cmp;
  const MirVreg *masked;
  if (i + 1 >= fn->insn_count) {
    return 0;
  }
  and_op = &fn->insns[i];
  cmp = &fn->insns[i + 1];
  if (and_op->op != MIR_AND || and_op->is_float ||
      and_op->dst.kind != MIR_OPK_VREG || and_op->a.kind != MIR_OPK_VREG ||
      and_op->b.kind != MIR_OPK_IMM || and_op->b.imm < 0 ||
      and_op->b.imm > 2147483647LL) {
    return 0;
  }
  if (cmp->op != MIR_CMPBR || cmp->is_float ||
      (cmp->cc != 0x84 && cmp->cc != 0x85) || cmp->a.kind != MIR_OPK_VREG ||
      cmp->a.vreg != and_op->dst.vreg || cmp->b.kind != MIR_OPK_IMM ||
      cmp->b.imm != 0) {
    return 0;
  }
  masked = &fn->vregs[and_op->dst.vreg];
  if (!masked->in_register || masked->live_end != (int)(i + 1)) {
    return 0;
  }
  if (!uses || uses[and_op->dst.vreg] != 1) {
    return 0;
  }
  return fn->vregs[and_op->a.vreg].in_register;
}

static int mir_byte_compare_fusable(const MirFunction *fn, const int *uses,
                                    size_t i) {
  const MirInst *load;
  const MirInst *cmp;
  const MirVreg *loaded;
  if (i + 1 >= fn->insn_count) {
    return 0;
  }
  load = &fn->insns[i];
  cmp = &fn->insns[i + 1];
  if (load->op != MIR_MOV || load->is_float || load->width != 1 ||
      !load->is_unsigned || load->dst.kind != MIR_OPK_VREG ||
      !mir_mem_operand_in_registers(fn, &load->a)) {
    return 0;
  }
  if (cmp->op != MIR_CMPBR || cmp->is_float ||
      (cmp->cc != 0x84 && cmp->cc != 0x85) || cmp->b.kind != MIR_OPK_VREG ||
      cmp->b.vreg != load->dst.vreg || cmp->a.kind != MIR_OPK_VREG) {
    return 0;
  }
  loaded = &fn->vregs[load->dst.vreg];
  if (!loaded->in_register || loaded->live_end != (int)(i + 1)) {
    return 0;
  }
  if (!uses || uses[load->dst.vreg] != 1) {
    return 0;
  }
  return mir_vreg_is_byte_load(fn, i, cmp->a.vreg);
}

static void mir_encode_operand_reads(const MirOperand *op, MirVregId out[2]) {
  out[0] = MIR_VREG_NONE;
  out[1] = MIR_VREG_NONE;
  if (!op) {
    return;
  }
  if (op->kind == MIR_OPK_VREG) {
    out[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    out[0] = op->mem.base;
    out[1] = op->mem.index;
  }
}

static int *mir_encode_count_vreg_uses(const MirFunction *fn) {
  int *uses = (int *)calloc(fn->vreg_count ? fn->vreg_count : 1, sizeof(int));
  size_t i;
  if (!uses) {
    return NULL;
  }
  for (i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    const MirOperand *ops[3];
    int k;
    if (in->op == MIR_NOP) {
      continue;
    }
    ops[0] = &in->a;
    ops[1] = &in->b;
    ops[2] = &in->dst;
    for (k = 0; k < 3; k++) {
      MirVregId r[2];
      int e;
      if (ops[k] == &in->dst && in->dst.kind == MIR_OPK_VREG) {
        continue;
      }
      mir_encode_operand_reads(ops[k], r);
      for (e = 0; e < 2; e++) {
        if (r[e] >= 0 && (size_t)r[e] < fn->vreg_count) {
          uses[r[e]]++;
        }
      }
    }
  }
  return uses;
}

static int mir_load_compare_fusable(const MirFunction *fn, const int *uses,
                                    size_t i, int *side) {
  const MirInst *load;
  const MirInst *cmp;
  const MirVreg *loaded;
  if (i + 1 >= fn->insn_count) {
    return 0;
  }
  load = &fn->insns[i];
  cmp = &fn->insns[i + 1];
  if (load->op != MIR_MOV || load->is_float ||
      (load->width != 4 && load->width != 8) ||
      load->dst.kind != MIR_OPK_VREG ||
      !mir_mem_operand_in_registers(fn, &load->a)) {
    return 0;
  }
  if (cmp->op != MIR_CMPBR || cmp->is_float || cmp->width != load->width) {
    return 0;
  }
  loaded = &fn->vregs[load->dst.vreg];
  if (!loaded->in_register || loaded->address_taken ||
      loaded->live_end != (int)(i + 1)) {
    return 0;
  }
  if (!uses || uses[load->dst.vreg] != 1) {
    return 0;
  }
  if (cmp->b.kind == MIR_OPK_VREG && cmp->b.vreg == load->dst.vreg &&
      cmp->a.kind == MIR_OPK_VREG && cmp->a.vreg != load->dst.vreg &&
      fn->vregs[cmp->a.vreg].in_register) {
    *side = 1;
    return 1;
  }
  if (cmp->a.kind == MIR_OPK_VREG && cmp->a.vreg == load->dst.vreg &&
      cmp->b.kind == MIR_OPK_VREG && cmp->b.vreg != load->dst.vreg &&
      fn->vregs[cmp->b.vreg].in_register) {
    *side = 0;
    return 1;
  }
  if (cmp->a.kind == MIR_OPK_VREG && cmp->a.vreg == load->dst.vreg &&
      cmp->b.kind == MIR_OPK_IMM && cmp->b.imm >= -2147483648LL &&
      cmp->b.imm <= 2147483647LL) {
    *side = 2;
    return 1;
  }
  return 0;
}

static int mir_jump_is_fallthrough(const MirFunction *fn, size_t index) {
  const MirInst *jmp = &fn->insns[index];
  if (jmp->dst.kind != MIR_OPK_LABEL || !jmp->dst.sym) {
    return 0;
  }
  for (size_t k = index + 1; k < fn->insn_count; k++) {
    const MirInst *in = &fn->insns[k];
    if (in->op == MIR_NOP) {
      continue;
    }
    if (in->op != MIR_LABEL) {
      return 0;
    }
    if (in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
        strcmp(in->dst.sym, jmp->dst.sym) == 0) {
      return 1;
    }
  }
  return 0;
}

typedef struct {
  size_t lea_off;
  const MirJumpTable *table;
} MirPendingTable;

typedef struct {
  MirFunction *fn;
  size_t index;
  size_t fused_byte_load;
  size_t fused_cmp_load;
  int fused_cmp_side;
  const int *vreg_uses;
  size_t fused_mask_test;
  size_t prev_cmpbr;
  MirPendingTable pending_tables[MIR_MAX_JUMP_TABLES];
  size_t pending_table_count;
  char epilogue_label[64];
  int epilogue_defined;
  int epilogue_referenced;
  MirEncNameMap branch_targets;
} MirEncodeState;

typedef int (*MirEncodeHandler)(MirEncodeState *st, const MirInst *in);

/* Every return but the last jumps forward into the final copy, so the shared
   epilogue is a taken forward branch and never a backward one. */
static int mir_encode_return(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  (void)in;

  if (!st->epilogue_label[0] || !mir_epilogue_is_shareable(fn)) {
    return mir_emit_epilogue(fn);
  }
  if (!mir_ret_is_last(fn, st->index)) {
    size_t off = 0;
    if (!binary_emit_jmp_placeholder(&ctx->code, &off) ||
        !binary_label_fixup_table_add(&ctx->label_fixups, st->epilogue_label,
                                      off)) {
      return enc_err(fn, "out of memory in shared epilogue jump");
    }
    st->epilogue_referenced = 1;
    return 1;
  }
  if (!binary_label_table_define(&ctx->labels, st->epilogue_label,
                                 ctx->code.size)) {
    return enc_err(fn, "duplicate shared epilogue label");
  }
  st->epilogue_defined = 1;
  return mir_emit_epilogue(fn);
}

static int mir_encode_scalar(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_NOP:
      break;
    case MIR_MOV: {
      int side = 0;
      if (mir_byte_compare_fusable(fn, st->vreg_uses, i)) {
        st->fused_byte_load = i;
        break;
      }
      if (mir_load_compare_fusable(fn, st->vreg_uses, i, &side)) {
        st->fused_cmp_load = i;
        st->fused_cmp_side = side;
        break;
      }
      ok = encode_mov(fn, in);
      break;
    }
    case MIR_ADD:
    case MIR_SUB:
    case MIR_AND:
    case MIR_OR:
    case MIR_XOR:
      if (in->op == MIR_AND && mir_mask_test_fusable(fn, st->vreg_uses, i)) {
        st->fused_mask_test = i;
        break;
      }
      ok = encode_alu(fn, in);
      break;
    case MIR_IMUL:
      ok = encode_imul(fn, in);
      break;
    case MIR_NEG:
    case MIR_NOT:
      ok = encode_neg_not(fn, in);
      break;
    case MIR_POPCNT: {
      BinaryGpRegister D;
      if (dst_is_reg(fn, &in->dst, &D)) {
        if (!operand_in_phys(fn, &in->a, D) && !materialize_into(fn, &in->a, D)) {
          ok = 0;
          break;
        }
        if (!wcs_popcnt_sized(&ctx->code, D, D, in->width == 8)) {
          ok = enc_err(fn, "out of memory in popcnt");
        }
        break;
      }
      if (!materialize_into(fn, &in->a, SCRATCH_A)) {
        ok = 0;
        break;
      }
      if (!wcs_popcnt_sized(&ctx->code, SCRATCH_A, SCRATCH_A,
                            in->width == 8)) {
        ok = enc_err(fn, "out of memory in popcnt");
        break;
      }
      ok = store_from(fn, &in->dst, SCRATCH_A);
      break;
    }
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_divide_shift(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
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
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_global_access(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_LOAD_GLOBAL:
      ok = encode_load_global(fn, in);
      break;
    case MIR_STORE_GLOBAL:
      ok = encode_store_global(fn, in);
      break;
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_float_arith(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_FDUP:
    case MIR_FEXTHI: {
      int lok;
      BinaryXmmRegister D;
      int dst_in_reg = dst_is_xmm_reg(fn, &in->dst, &D);
      if (!dst_in_reg) {
        D = FSCRATCH_A;
      }
      BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_B, in->width,
                                         &lok);
      if (!lok) {
        ok = enc_err(fn, "out of memory in lane op");
        break;
      }
      int done = (in->op == MIR_FDUP)
                     ? vex_xmm_3op(fn, 3, 0x12, D, 0, aval)
                     : vex_xmm_3op(fn, 1, 0x15, D, aval, aval);
      if (!done) {
        ok = enc_err(fn, "out of memory in lane op");
        break;
      }
      ok = dst_in_reg ? 1 : xmm_store(fn, &in->dst, FSCRATCH_A, in->width);
      break;
    }
    case MIR_FADD:
    case MIR_FSUB:
    case MIR_FMUL:
    case MIR_FDIV:
    case MIR_FXOR:
      ok = encode_fbinop(fn, in);
      break;
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_float_convert(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_CVTSI2F:
      ok = encode_cvtsi2f(fn, in);
      break;
    case MIR_CVTF2SI:
      ok = encode_cvtf2si(fn, in);
      break;
    case MIR_CVTF2F:
      ok = encode_cvtf2f(fn, in);
      break;
    case MIR_CVTPH2PS:
      ok = encode_cvtph2ps(fn, in);
      break;
    case MIR_CVTPS2PH:
      ok = encode_cvtps2ph(fn, in);
      break;
    case MIR_MOVD_TO_XMM:
    case MIR_MOVD_TO_GP: {
      int ok2;
      if (in->op == MIR_MOVD_TO_XMM) {
        BinaryGpRegister areg = value_reg(fn, &in->a, SCRATCH_A, &ok2);
        BinaryXmmRegister D;
        int dst_in_reg = dst_is_xmm_reg(fn, &in->dst, &D);
        BinaryXmmRegister target = dst_in_reg ? D : FSCRATCH_B;
        if (!ok2) { ok = 0; break; }
        if (!binary_emit_movd_xmm_reg(&fn->context->code, target, areg)) {
          ok = enc_err(fn, "out of memory in movd2xmm");
          break;
        }
        ok = dst_in_reg ? 1 : xmm_store(fn, &in->dst, FSCRATCH_B, 4);
        break;
      }
      BinaryXmmRegister aval = xmm_value(fn, &in->a, FSCRATCH_A, 4, &ok2);
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!ok2) { ok = 0; break; }
      if (!binary_emit_movd_reg_xmm(&fn->context->code, target, aval)) {
        ok = enc_err(fn, "out of memory in movd2gp");
        break;
      }
      ok = dst_in_reg ? 1 : store_from(fn, &in->dst, target);
      break;
    }
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_float_compare(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_FSETCC: {
      int rok;
      BinaryXmmRegister av = xmm_value(fn, &in->a, FSCRATCH_A, in->width, &rok);
      if (!rok) { ok = 0; break; }
      BinaryXmmRegister bv = xmm_value(fn, &in->b, FSCRATCH_B, in->width, &rok);
      if (!rok) { ok = 0; break; }
      int cmp = (in->width == 4)
                    ? binary_emit_ucomiss_xmm_xmm(&ctx->code, av, bv)
                    : binary_emit_ucomisd_xmm_xmm(&ctx->code, av, bv);
      int unordered_cc = mir_fsetcc_unordered_cc(in->cc);
      int merge_opcode = in->cc == 0x94 ? 0x20 : 0x08;
      if (!cmp || !binary_emit_setcc_reg8(&ctx->code, in->cc, BINARY_GP_RAX) ||
          (unordered_cc >= 0 &&
           (!binary_emit_setcc_reg8(&ctx->code, (unsigned char)unordered_cc,
                                    BINARY_GP_RCX) ||
            !binary_emit_alu_reg8_reg8(&ctx->code,
                                       (unsigned char)merge_opcode,
                                       BINARY_GP_RAX, BINARY_GP_RCX))) ||
          !binary_emit_movzx_eax_al(&ctx->code)) {
        ok = enc_err(fn, "out of memory in fsetcc");
        break;
      }
      ok = store_from(fn, &in->dst, BINARY_GP_RAX);
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
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_branch(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_LABEL:
      if (!binary_label_table_define(&ctx->labels, in->dst.sym,
                                     ctx->code.size)) {
        ok = enc_err(fn, "duplicate label");
      }
      break;
    case MIR_JMP: {
      if (mir_jump_is_fallthrough(fn, i)) {
        break;
      }
      size_t off = 0;
      if (!binary_emit_jmp_placeholder(&ctx->code, &off) ||
          !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
        ok = enc_err(fn, "out of memory in jmp");
      }
      break;
    }
    case MIR_JCC: {
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
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_call(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_CALL: {
      const char *link =
          code_generator_get_link_symbol_name(fn->generator, in->dst.sym);
      size_t off = 0;
      int keep = in->preserves_rax || in->preserves_xmm;
      if (keep && !mir_emit_preserve_volatiles(fn, in, 1)) {
        ok = enc_err(fn, "out of memory saving registers across a checked call");
        break;
      }
      if (!link || !binary_emit_call_placeholder(&ctx->code, &off) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, link, off)) {
        ok = enc_err(fn, "out of memory emitting call");
        break;
      }
      if (keep && !mir_emit_preserve_volatiles(fn, in, 0)) {
        ok = enc_err(fn,
                     "out of memory restoring registers across a checked call");
      }
      break;
    }
    case MIR_HEAP_NEW: {
      size_t d1 = 0;
      size_t d2 = 0;
      if (!code_generator_binary_declare_external_symbol(fn->generator,
                                                         "GetProcessHeap") ||
          !code_generator_binary_declare_external_symbol(fn->generator,
                                                         "HeapAlloc") ||
          !binary_emit_sub_rsp_imm32(&ctx->code, 48) ||
          !binary_emit_mov_mem_reg(&ctx->code, BINARY_GP_RSP, 40,
                                   BINARY_GP_R8) ||
          !binary_emit_call_placeholder(&ctx->code, &d1) ||
          !binary_call_relocation_table_add(&ctx->call_relocations,
                                            "GetProcessHeap", d1) ||
          !binary_emit_mov_reg_reg(&ctx->code, BINARY_GP_RCX, BINARY_GP_RAX) ||
          !binary_emit_mov_reg_imm64(&ctx->code, BINARY_GP_RDX,
                                     8 ) ||
          !binary_emit_mov_reg_mem(&ctx->code, BINARY_GP_R8, BINARY_GP_RSP,
                                   40) ||
          !binary_emit_call_placeholder(&ctx->code, &d2) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, "HeapAlloc",
                                            d2) ||
          !binary_emit_add_rsp_imm32(&ctx->code, 48)) {
        ok = enc_err(fn, "out of memory emitting heap allocation");
      }
      break;
    }
    case MIR_SYSCALL: {      if (!binary_emit_syscall(&ctx->code)) {
        ok = enc_err(fn, "out of memory emitting a system call");
      }
      break;
    }
    case MIR_CALL_INDIRECT: {
      int rok;
      BinaryGpRegister target = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok || !binary_emit_call_reg(&ctx->code, target)) {
        ok = enc_err(fn, "out of memory emitting indirect call");
      }
      break;
    }
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_string_op(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_REP_MOVSB:
    case MIR_REP_STOSB: {
      BinaryCodeBuffer *code = &ctx->code;
      const BinaryAbi *rep_abi = code_generator_binary_active_abi();
      if (!rep_abi || rep_abi->int_param_count < 3) {
        ok = enc_err(fn, "no argument registers for an inline block copy");
        break;
      }
      BinaryGpRegister rep_dst = rep_abi->int_param_registers[0];
      BinaryGpRegister rep_src = rep_abi->int_param_registers[1];
      BinaryGpRegister rep_count = rep_abi->int_param_registers[2];
      int rep_ok = binary_emit_mov_reg_reg(code, BINARY_GP_RAX, rep_dst) &&
                   binary_emit_push_reg(code, BINARY_GP_RDI);
      if (rep_ok && in->op == MIR_REP_MOVSB) {
        rep_ok = binary_emit_push_reg(code, BINARY_GP_RSI) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RDI, rep_dst) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RSI, rep_src) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RCX, rep_count) &&
                 binary_code_buffer_append_u8(code, 0xFC) &&
                 binary_code_buffer_append_u8(code, 0xF3) &&
                 binary_code_buffer_append_u8(code, 0xA4) &&
                 binary_emit_pop_reg(code, BINARY_GP_RSI);
      } else if (rep_ok) {
        rep_ok = binary_emit_mov_reg_reg(code, BINARY_GP_RDI, rep_dst) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RCX, rep_count) &&
                 binary_emit_push_reg(code, BINARY_GP_RAX) &&
                 binary_emit_mov_reg_reg(code, BINARY_GP_RAX, rep_src) &&
                 binary_code_buffer_append_u8(code, 0xFC) &&
                 binary_code_buffer_append_u8(code, 0xF3) &&
                 binary_code_buffer_append_u8(code, 0xAA) &&
                 binary_emit_pop_reg(code, BINARY_GP_RAX);
      }
      if (!rep_ok || !binary_emit_pop_reg(code, BINARY_GP_RDI)) {
        ok = enc_err(fn, "out of memory emitting inline block copy");
      }
      break;
    }
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_kernel(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_SIMD_SLP_MAC: {
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
      int fok = code_generator_binary_emit_simd_fill_splat(&ctx->code,
                                                           in->dst.imm);
      if (fok) {
        fok = (in->a.imm == 0)
                  ? code_generator_binary_emit_simd_fill_loop_mode0(&ctx->code,
                                                                    in->dst.imm)
                  : code_generator_binary_emit_simd_fill_loop_bytewalk(
                        &ctx->code, in->dst.imm, (int)in->a.imm);
      }
      if (!fok) {
        ok = enc_err(fn, "out of memory emitting inline fill kernel");
      }
      break;
    }
    case MIR_SIMD_AFFINE_MAP_F32: {
      if (!code_generator_binary_emit_simd_affine_map_f32_inline(
              &ctx->code, (unsigned)in->dst.imm, (unsigned)in->a.imm,
              (unsigned)in->b.imm, (in->cc & 1) != 0, (in->cc & 2) != 0,
              (in->cc & 4) != 0, (in->cc & 8) != 0)) {
        ok = enc_err(fn, "out of memory emitting inline affine-map kernel");
      }
      break;
    }
    case MIR_SIMD_AFFINE_MAP_F64: {
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
      const IRInstruction *vir = (const IRInstruction *)in->aux;
      if (!vir || !code_generator_binary_emit_simd_vloop_f64(
                      fn->generator, fn->context, vir, 1)) {
        ok = enc_err(fn, "out of memory emitting inline vloop kernel");
      }
      break;
    }
    case MIR_IR_KERNEL: {
      const MirKernelAux *ka = (const MirKernelAux *)in->aux;
      const MirIrKernel *kern = ka ? mir_ir_kernel_at(ka->kernel_index) : NULL;
      if (!ka || !kern || ka->operand_count > BINARY_MAX_MARSHALED_OPERANDS) {
        ok = enc_err(fn, "malformed inline kernel");
        break;
      }
      for (int s = 0; s < ka->operand_count; s++) {
        const MirVreg *sv = &fn->vregs[ka->slot_vreg[ka->operand_slot[s]]];
        ctx->marshaled_operands[s].operand = ka->operand[s];
        ctx->marshaled_operands[s].base_register = frame_base(fn);
        ctx->marshaled_operands[s].displacement =
            frame_disp(fn, -sv->spill_offset);
      }
      ctx->marshaled_operand_count = (size_t)ka->operand_count;
      int kok = kern->emit(fn->generator, ctx, ka->ir);
      ctx->marshaled_operand_count = 0;
      if (!kok) {
        ok = enc_err(fn, "failed to emit inline kernel");
        break;
      }
      fn->used_inline_vector = 1;
      break;
    }
    case MIR_SIMD_SILU_F32: {
      if (!code_generator_binary_emit_simd_silu_f32_inline(&ctx->code,
                                                           (int)in->dst.imm)) {
        ok = enc_err(fn, "out of memory emitting inline SiLU kernel");
      }
      break;
    }
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_outgoing(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_STORE_OUTARG: {
      int ok = 1;
      BinaryGpRegister r = SCRATCH_A;
      if (in->is_float && in->a.kind == MIR_OPK_VREG) {
        const MirVreg *v = &fn->vregs[in->a.vreg];
        if (v->in_register) {
          ok = (in->width == 4)
                   ? binary_emit_movd_reg_xmm(&ctx->code, SCRATCH_A,
                                              (BinaryXmmRegister)v->phys)
                   : binary_emit_movq_reg_xmm(&ctx->code, SCRATCH_A,
                                              (BinaryXmmRegister)v->phys);
        } else {
          ok = binary_emit_mov_reg_mem(&ctx->code, SCRATCH_A, frame_base(fn),
                                       frame_disp(fn, -v->spill_offset));
        }
        if (!ok) {
          ok = enc_err(fn, "out of memory staging float stack argument");
          break;
        }
      } else {
        r = value_reg(fn, &in->a, SCRATCH_A, &ok);
        if (!ok) {
          break;
        }
      }
      if (in->is_float && in->width == 4
              ? !binary_emit_mov_mem_reg32(&ctx->code, BINARY_GP_RSP,
                                           (int)in->b.imm, r)
              : !binary_emit_mov_mem_reg(&ctx->code, BINARY_GP_RSP,
                                         (int)in->b.imm, r)) {
        ok = enc_err(fn, "out of memory storing outgoing call argument");
      }
      break;
    }
    case MIR_CMOV: {
      int rok;
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = D;
      if (!dst_in_reg) {
        const MirVreg *v = &fn->vregs[in->dst.vreg];
        target = SCRATCH_A;
        if (!gp_home_load(fn, v, SCRATCH_A)) {
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
      if (in->a.kind != MIR_OPK_MEM || in->a.mem.index != MIR_VREG_NONE) {
        ok = enc_err(fn, "MIR_PREFETCH expects a base-only memory operand");
        break;
      }
      int prok;
      int pdisp;
      BinaryGpRegister pbase =
          mir_mem_base_reg(fn, &in->a.mem, SCRATCH_A, &pdisp, &prok);
      if (!prok) {
        break;
      }
      if (!binary_emit_prefetcht0_mem(&ctx->code, pbase, pdisp)) {
        ok = enc_err(fn, "out of memory in prefetch");
      }
      break;
    }
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_address(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_LEA: {
      if (in->a.kind != MIR_OPK_MEM) {
        ok = enc_err(fn, "MIR_LEA expects a memory operand");
        break;
      }
      int rok;
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      BinaryGpRegister taken[4];
      int base_disp;
      int tn = 0;
      mir_note_mem_base(fn, &in->a.mem, taken, &tn);
      MirOperand iop_probe = mir_op_vreg(in->a.mem.index);
      if (in->a.mem.index != MIR_VREG_NONE) {
        mir_note_fixed_reg(fn, &iop_probe, taken, &tn);
      }
      BinaryGpRegister vouch[1];
      int vn = mir_reg_in(target, taken, tn) ? 0 : 1;
      vouch[0] = target;
      size_t idx = (size_t)(in - fn->insns);
      BinaryGpRegister base_scratch, index_scratch;
      if (!mir_pick_scratch(fn, idx, SCRATCH_B, taken, tn, vouch, vn,
                            &base_scratch)) {
        ok = enc_err(fn, "no free scratch register for a scaled lea base");
        break;
      }
      BinaryGpRegister base_reg =
          mir_mem_base_reg(fn, &in->a.mem, base_scratch, &base_disp, &rok);
      if (!rok) {
        break;
      }
      taken[tn++] = base_reg;
      if (in->a.mem.index != MIR_VREG_NONE) {
        MirOperand iop = mir_op_vreg(in->a.mem.index);
        if (!mir_pick_scratch(fn, idx, BINARY_GP_RDX, taken, tn, vouch, vn,
                              &index_scratch)) {
          ok = enc_err(fn, "no free scratch register for a scaled lea index");
          break;
        }
        BinaryGpRegister index_reg = value_reg(fn, &iop, index_scratch, &rok);
        if (!rok) {
          break;
        }
        if (!binary_emit_lea_reg_base_index_scale_disp(
                &ctx->code, target, base_reg, index_reg, in->a.mem.scale,
                base_disp)) {
          ok = enc_err(fn, "out of memory in scaled lea");
          break;
        }
      } else if (!binary_emit_lea_reg_mem(&ctx->code, target, base_reg,
                                          base_disp)) {
        ok = enc_err(fn, "out of memory in lea");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
    case MIR_LEA_OUTARG: {
      const BinaryAbi *oa = code_generator_binary_active_abi();
      int off = in->b.kind == MIR_OPK_IMM && in->b.imm == 1
                    ? oa->shadow_space_size + (int)in->a.imm
                    : oa->shadow_space_size + fn->outgoing_stack_bytes +
                          (int)in->a.imm;
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
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_literal_address(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_LEA_LOCAL: {
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
    case MIR_LEA_STRLIT: {
      const char *s = in->a.sym ? in->a.sym : "";
      BinaryGpRegister D;
      int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
      BinaryGpRegister target = dst_in_reg ? D : SCRATCH_A;
      if (!code_generator_binary_emit_string_literal_value_address(
              fn->generator, ctx, s,
              in->a.imm > 0 ? (size_t)in->a.imm : strlen(s), target)) {
        ok = enc_err(fn, "out of memory emitting string-literal record address");
        break;
      }
      if (!dst_in_reg) {
        ok = store_from(fn, &in->dst, SCRATCH_A);
      }
      break;
    }
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_encode_trap(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_TRAP: {
      if ((fn->generator->generate_stack_trace_support || fn->generator->live) &&
          fn->ir_function && in->ir_index >= 0 &&
          (size_t)in->ir_index < fn->ir_function->instruction_count) {
        ok = mir_encode_traced_trap(
            fn, in, &fn->ir_function->instructions[in->ir_index]);
        break;
      }
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
      if (!code_generator_binary_emit_cstring_literal_address(
              fn->generator, ctx,
              "  rebuild with -s for the file, line and stack trace", arg0) ||
          !binary_emit_call_placeholder(&ctx->code, &off) ||
          !binary_call_relocation_table_add(&ctx->call_relocations, "puts",
                                            off)) {
        ok = enc_err(fn, "out of memory emitting trap hint");
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
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static int mir_cmpbr_reuses_flags(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  const MirInst *prev;
  size_t p = st->index;

  while (p > 0 && st->prev_cmpbr != p - 1 &&
         mir_cmp_gap_is_empty(fn, &st->branch_targets, &fn->insns[p - 1])) {
    p--;
  }
  if (p == 0 || st->prev_cmpbr != p - 1) {
    return 0;
  }
  prev = &fn->insns[st->prev_cmpbr];
  return prev->width == in->width && prev->is_unsigned == in->is_unsigned &&
         !prev->is_float && !in->is_float &&
         mir_cmp_operand_reusable(fn, &prev->a, &in->a) &&
         mir_cmp_operand_reusable(fn, &prev->b, &in->b);
}

static int mir_cmpbr_emit_branch(MirEncodeState *st, const MirInst *in) {
  BinaryFunctionContext *ctx = st->fn->context;
  size_t off = 0;

  if (!binary_emit_jcc_placeholder(&ctx->code, in->cc, &off) ||
      !binary_label_fixup_table_add(&ctx->label_fixups, in->dst.sym, off)) {
    return enc_err(st->fn, "out of memory in cmpbr");
  }
  return 1;
}

static int mir_cmpbr_fused_mask_test(MirEncodeState *st) {
  MirFunction *fn = st->fn;
  const MirInst *and_op = &fn->insns[st->index - 1];
  BinaryGpRegister src = (BinaryGpRegister)fn->vregs[and_op->a.vreg].phys;

  if (!binary_emit_test_reg_imm32(&fn->context->code, src,
                                  (uint32_t)and_op->b.imm)) {
    return enc_err(fn, "out of memory in fused mask test");
  }
  return 1;
}

static int mir_cmpbr_fused_byte_load(MirEncodeState *st,
                                     BinaryGpRegister areg) {
  MirFunction *fn = st->fn;
  BinaryCodeBuffer *code = &fn->context->code;
  const MirMem *m = &fn->insns[st->index - 1].a.mem;
  BinaryGpRegister base = (BinaryGpRegister)fn->vregs[m->base].phys;
  int need_rex = areg >= 4 && areg <= 7;
  int emitted;

  if (m->index != MIR_VREG_NONE) {
    BinaryGpRegister index = (BinaryGpRegister)fn->vregs[m->index].phys;
    emitted = need_rex ? binary_emit_memory_access_sib_forced(
                             code, 0, 0, 0x3A, 0, 0, areg, base, index,
                             m->scale, m->disp)
                       : binary_emit_memory_access_sib(code, 0, 0, 0x3A, 0, 0,
                                                       areg, base, index,
                                                       m->scale, m->disp);
  } else {
    emitted = need_rex ? binary_emit_memory_access_ex_forced(
                             code, 0, 0, 0x3A, 0, 0, areg, base, m->disp)
                       : binary_emit_memory_access_ex(code, 0, 0, 0x3A, 0, 0,
                                                      areg, base, m->disp);
  }
  if (!emitted) {
    return enc_err(fn, "out of memory in fused byte compare");
  }
  return 1;
}

static int mir_cmpbr_fused_load(MirEncodeState *st) {
  MirFunction *fn = st->fn;
  BinaryCodeBuffer *code = &fn->context->code;
  const MirInst *load = &fn->insns[st->index - 1];
  const MirInst *cmp = &fn->insns[st->index];
  const MirMem *m = &load->a.mem;
  BinaryGpRegister base = (BinaryGpRegister)fn->vregs[m->base].phys;
  BinaryGpRegister other;
  unsigned char opcode;
  int rex_w = load->width == 8;
  int emitted;

  if (st->fused_cmp_side == 2) {
    int has_index = m->index != MIR_VREG_NONE;
    BinaryGpRegister index =
        has_index ? (BinaryGpRegister)fn->vregs[m->index].phys : BINARY_GP_RAX;
    if (!binary_emit_cmp_mem_imm_width(code, base, has_index, index, m->scale,
                                       m->disp, cmp->b.imm, load->width)) {
      return enc_err(fn, "out of memory in fused load compare immediate");
    }
    return 1;
  }
  if (st->fused_cmp_side == 1) {
    other = (BinaryGpRegister)fn->vregs[cmp->a.vreg].phys;
    opcode = 0x3B;
  } else {
    other = (BinaryGpRegister)fn->vregs[cmp->b.vreg].phys;
    opcode = 0x39;
  }
  if (m->index != MIR_VREG_NONE) {
    BinaryGpRegister index = (BinaryGpRegister)fn->vregs[m->index].phys;
    emitted = binary_emit_memory_access_sib(code, 0, rex_w, opcode, 0, 0, other,
                                            base, index, m->scale, m->disp);
  } else {
    emitted = binary_emit_memory_access_ex(code, 0, rex_w, opcode, 0, 0, other,
                                           base, m->disp);
  }
  if (!emitted) {
    return enc_err(fn, "out of memory in fused load compare");
  }
  return 1;
}

static int mir_cmpbr_narrow_compare(MirEncodeState *st, const MirInst *in,
                                    BinaryGpRegister areg) {
  MirFunction *fn = st->fn;
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister base;
  int disp;
  int rok = 1;

  if (in->b.kind == MIR_OPK_IMM) {
    if (!binary_emit_cmp_reg_imm_w32(code, areg, (uint32_t)in->b.imm)) {
      return enc_err(fn, "out of memory in cmpbr32 imm");
    }
    return 1;
  }
  if (gp_home_mem(fn, &in->b, &base, &disp)) {
    if (!binary_emit_alu_reg_mem(code, 0x39, areg, base, disp, 4)) {
      return enc_err(fn, "out of memory in cmpbr32 mem");
    }
    return 1;
  }
  {
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &rok);
    if (!rok || !binary_emit_cmp_reg_reg32(code, areg, breg)) {
      return enc_err(fn, "out of memory in cmpbr32");
    }
  }
  return 1;
}

static int mir_cmpbr_wide_compare(MirEncodeState *st, const MirInst *in,
                                  BinaryGpRegister areg) {
  MirFunction *fn = st->fn;
  BinaryCodeBuffer *code = &fn->context->code;
  BinaryGpRegister base;
  int disp;
  int rok = 1;

  if (in->b.kind == MIR_OPK_IMM &&
      code_generator_binary_immediate_fits_signed_32(in->b.imm)) {
    if (!binary_emit_cmp_reg_imm32(code, areg, (uint32_t)in->b.imm)) {
      return enc_err(fn, "out of memory in cmpbr");
    }
    return 1;
  }
  if (gp_home_mem(fn, &in->b, &base, &disp)) {
    if (!binary_emit_alu_reg_mem(code, 0x39, areg, base, disp, 8)) {
      return enc_err(fn, "out of memory in cmpbr mem");
    }
    return 1;
  }
  {
    BinaryGpRegister breg = value_reg(fn, &in->b, SCRATCH_B, &rok);
    if (!rok || !binary_emit_cmp_reg_reg(code, areg, breg)) {
      return enc_err(fn, "out of memory in cmpbr");
    }
  }
  return 1;
}

static int mir_encode_compare_only(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryGpRegister areg;
  int rok = 1;
  int ok;

  areg = value_reg(fn, &in->a, SCRATCH_A, &rok);
  if (!rok) {
    return 0;
  }
  ok = (in->width == 4) ? mir_cmpbr_narrow_compare(st, in, areg)
                        : mir_cmpbr_wide_compare(st, in, areg);
  st->prev_cmpbr = (size_t)-1;
  return ok;
}

static int mir_encode_conditional_move(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  BinaryGpRegister D;
  BinaryGpRegister target;
  BinaryGpRegister src;
  int dst_in_reg = dst_is_reg(fn, &in->dst, &D);
  int rok = 1;

  if (in->cc < 0x80 || in->cc > 0x8F) {
    return enc_err(fn, "conditional move without a condition");
  }
  target = dst_in_reg ? D : SCRATCH_A;
  if (!dst_in_reg) {
    if (in->dst.kind != MIR_OPK_VREG ||
        !gp_home_load(fn, &fn->vregs[in->dst.vreg], SCRATCH_A)) {
      return enc_err(fn, "out of memory loading conditional move destination");
    }
  }
  src = value_reg(fn, &in->a, SCRATCH_B, &rok);
  if (!rok) {
    return 0;
  }
  if (!binary_emit_cmovcc_reg_reg(&ctx->code, (unsigned char)(in->cc - 0x40),
                                  target, src)) {
    return enc_err(fn, "out of memory in conditional move");
  }
  if (!dst_in_reg) {
    return store_from(fn, &in->dst, target);
  }
  return 1;
}

static int mir_encode_compare_branch(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  size_t i = st->index;
  BinaryGpRegister areg;
  int rok = 1;
  int ok;

  if (in->op == MIR_BT) {
    BinaryGpRegister base = (BinaryGpRegister)fn->vregs[in->a.vreg].phys;
    BinaryGpRegister offset = (BinaryGpRegister)fn->vregs[in->b.vreg].phys;
    if (!binary_emit_bt_reg_reg(&fn->context->code, base, offset)) {
      return enc_err(fn, "out of memory in bit test");
    }
    st->prev_cmpbr = (size_t)-1;
    return mir_cmpbr_emit_branch(st, in);
  }
  if (in->op != MIR_CMPBR) {
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  if (mir_cmpbr_reuses_flags(st, in)) {
    ok = mir_cmpbr_emit_branch(st, in);
    st->prev_cmpbr = i;
    return ok;
  }
  if (st->fused_cmp_load != (size_t)-1 && st->fused_cmp_load + 1 == i) {
    ok = mir_cmpbr_fused_load(st);
    if (!ok) {
      return 0;
    }
    ok = mir_cmpbr_emit_branch(st, in);
    st->prev_cmpbr = (size_t)-1;
    return ok;
  }
  areg = value_reg(fn, &in->a, SCRATCH_A, &rok);
  if (!rok) {
    return 0;
  }
  if (st->fused_mask_test != (size_t)-1 && st->fused_mask_test + 1 == i) {
    ok = mir_cmpbr_fused_mask_test(st);
  } else if (st->fused_byte_load != (size_t)-1 &&
             st->fused_byte_load + 1 == i) {
    ok = mir_cmpbr_fused_byte_load(st, areg);
  } else if (in->width == 4) {
    ok = mir_cmpbr_narrow_compare(st, in, areg);
  } else {
    ok = mir_cmpbr_wide_compare(st, in, areg);
  }
  if (!ok) {
    return 0;
  }
  ok = mir_cmpbr_emit_branch(st, in);
  st->prev_cmpbr = i;
  return ok;
}

static int mir_encode_jump_table(MirEncodeState *st, const MirInst *in) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;
  size_t i = st->index;
  int ok = 1;

  (void)ctx;
  (void)i;
  switch (in->op) {
    case MIR_JMP_TABLE: {
      const MirJumpTable *tbl = (const MirJumpTable *)in->aux;
      int rok;
      BinaryGpRegister idx;
      size_t lea_off = 0;
      if (!tbl || st->pending_table_count >= MIR_MAX_JUMP_TABLES) {
        ok = enc_err(fn, "jump table without a target list");
        break;
      }
      idx = value_reg(fn, &in->a, SCRATCH_A, &rok);
      if (!rok) {
        ok = 0;
        break;
      }
      if (!binary_emit_lea_reg_rip_placeholder(&ctx->code, SCRATCH_B,
                                               &lea_off) ||
          !emit_ext_load(&ctx->code, SCRATCH_A, SCRATCH_B, 1, idx, 4, 0, 4, 1) ||
          !binary_emit_alu_reg_reg(&ctx->code, 0x01, SCRATCH_A, SCRATCH_B) ||
          !binary_emit_jmp_reg(&ctx->code, SCRATCH_A)) {
        ok = enc_err(fn, "out of memory in jump table");
        break;
      }
      st->pending_tables[st->pending_table_count].lea_off = lea_off;
      st->pending_tables[st->pending_table_count].table = tbl;
      st->pending_table_count++;
      break;
    }
    case MIR_RET:
      ok = mir_encode_return(st, in);
      break;
    case MIR_INLINE_ASM:
      ok = mir_encode_inline_asm(fn, in);
      break;
  default:
    return enc_err(fn, "unsupported MIR opcode in encoder");
  }
  return ok;
}

static const MirEncodeHandler MIR_ENCODERS[MIR_OPCODE_COUNT] = {
    [MIR_NOP] = mir_encode_scalar,
    [MIR_MOV] = mir_encode_scalar,
    [MIR_ADD] = mir_encode_scalar,
    [MIR_SUB] = mir_encode_scalar,
    [MIR_AND] = mir_encode_scalar,
    [MIR_OR] = mir_encode_scalar,
    [MIR_XOR] = mir_encode_scalar,
    [MIR_IMUL] = mir_encode_scalar,
    [MIR_NEG] = mir_encode_scalar,
    [MIR_NOT] = mir_encode_scalar,
    [MIR_POPCNT] = mir_encode_scalar,
    [MIR_IDIV] = mir_encode_divide_shift,
    [MIR_MULHI] = mir_encode_divide_shift,
    [MIR_SHL] = mir_encode_divide_shift,
    [MIR_SHR] = mir_encode_divide_shift,
    [MIR_SAR] = mir_encode_divide_shift,
    [MIR_SETCC] = mir_encode_divide_shift,
    [MIR_MOVZX] = mir_encode_divide_shift,
    [MIR_MOVSX] = mir_encode_divide_shift,
    [MIR_LOAD_GLOBAL] = mir_encode_global_access,
    [MIR_STORE_GLOBAL] = mir_encode_global_access,
    [MIR_FDUP] = mir_encode_float_arith,
    [MIR_FEXTHI] = mir_encode_float_arith,
    [MIR_FADD] = mir_encode_float_arith,
    [MIR_FSUB] = mir_encode_float_arith,
    [MIR_FMUL] = mir_encode_float_arith,
    [MIR_FDIV] = mir_encode_float_arith,
    [MIR_FXOR] = mir_encode_float_arith,
    [MIR_CVTSI2F] = mir_encode_float_convert,
    [MIR_CVTF2SI] = mir_encode_float_convert,
    [MIR_CVTF2F] = mir_encode_float_convert,
    [MIR_CVTPH2PS] = mir_encode_float_convert,
    [MIR_CVTPS2PH] = mir_encode_float_convert,
    [MIR_MOVD_TO_XMM] = mir_encode_float_convert,
    [MIR_MOVD_TO_GP] = mir_encode_float_convert,
    [MIR_FSETCC] = mir_encode_float_compare,
    [MIR_FCMPBR] = mir_encode_float_compare,
    [MIR_LABEL] = mir_encode_branch,
    [MIR_JMP] = mir_encode_branch,
    [MIR_JCC] = mir_encode_branch,
    [MIR_CALL] = mir_encode_call,
    [MIR_HEAP_NEW] = mir_encode_call,
    [MIR_SYSCALL] = mir_encode_call,
    [MIR_CALL_INDIRECT] = mir_encode_call,
    [MIR_REP_MOVSB] = mir_encode_string_op,
    [MIR_REP_STOSB] = mir_encode_string_op,
    [MIR_SIMD_SLP_MAC] = mir_encode_kernel,
    [MIR_SIMD_FILL] = mir_encode_kernel,
    [MIR_SIMD_AFFINE_MAP_F32] = mir_encode_kernel,
    [MIR_SIMD_AFFINE_MAP_F64] = mir_encode_kernel,
    [MIR_SIMD_VLOOP] = mir_encode_kernel,
    [MIR_IR_KERNEL] = mir_encode_kernel,
    [MIR_SIMD_SILU_F32] = mir_encode_kernel,
    [MIR_STORE_OUTARG] = mir_encode_outgoing,
    [MIR_CMOV] = mir_encode_outgoing,
    [MIR_PREFETCH] = mir_encode_outgoing,
    [MIR_LEA] = mir_encode_address,
    [MIR_LEA_OUTARG] = mir_encode_address,
    [MIR_LEA_GLOBAL] = mir_encode_address,
    [MIR_LEA_FUNC] = mir_encode_address,
    [MIR_LEA_LOCAL] = mir_encode_literal_address,
    [MIR_LEA_CSTR] = mir_encode_literal_address,
    [MIR_LEA_STRLIT] = mir_encode_literal_address,
    [MIR_TRAP] = mir_encode_trap,
    [MIR_CMPBR] = mir_encode_compare_branch,
    [MIR_CMP] = mir_encode_compare_only,
    [MIR_CMOVCC] = mir_encode_conditional_move,
    [MIR_BT] = mir_encode_compare_branch,
    [MIR_JMP_TABLE] = mir_encode_jump_table,
    [MIR_RET] = mir_encode_jump_table,
    [MIR_INLINE_ASM] = mir_encode_jump_table,
};

static char *mir_scan_loop_alignment(const MirFunction *fn) {
  char *align_label = (char *)calloc(fn->insn_count ? fn->insn_count : 1, 1);
  MirEncNameMap labels = {NULL, NULL, 0};

  if (!align_label) {
    return NULL;
  }
  if (mir_enc_name_map_init(&labels, fn->insn_count)) {
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL &&
          in->dst.sym) {
        mir_enc_name_map_put_first(&labels, in->dst.sym, i);
      }
    }
  }
  for (size_t b = 0; b < fn->insn_count; b++) {
    const MirInst *in = &fn->insns[b];
    int header;

    if (in->op != MIR_JMP && in->op != MIR_JCC && in->op != MIR_CMPBR &&
        in->op != MIR_BT && in->op != MIR_FCMPBR) {
      continue;
    }
    if (in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
      continue;
    }
    if (labels.names) {
      size_t at = 0;
      header = mir_enc_name_map_get(&labels, in->dst.sym, &at) ? (int)at : -1;
    } else {
      header = mir_encode_label_index(fn, in->dst.sym);
    }
    if (header < 0 || (size_t)header >= b) {
      continue;
    }
    if (b - (size_t)header >= BINARY_LOOP_BIG_MIR_INSTRUCTIONS ||
        align_label[header] == 2) {
      align_label[header] = 2;
    } else if (b - (size_t)header <= BINARY_LOOP_TIGHT_MIR_INSTRUCTIONS &&
               align_label[header] != 1) {
      align_label[header] = 3;
    } else {
      align_label[header] = 1;
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    size_t prev;
    if (fn->insns[i].op != MIR_LABEL || align_label[i]) {
      continue;
    }
    prev = i;
    while (prev > 0 && fn->insns[prev - 1].op == MIR_NOP) {
      prev--;
    }
    if (prev > 0 && (fn->insns[prev - 1].op == MIR_JMP ||
                     fn->insns[prev - 1].op == MIR_RET ||
                     fn->insns[prev - 1].op == MIR_JMP_TABLE)) {
      align_label[i] = 4;
    }
  }
  mir_enc_name_map_free(&labels);
  return align_label;
}

static int mir_encode_loop_alignment(MirFunction *fn, char wanted) {
  BinaryFunctionContext *ctx = fn->context;

  if (wanted == 4) {
    return binary_emit_align_code(&ctx->code, BINARY_JUMP_TARGET_ALIGN,
                                  BINARY_JUMP_TARGET_ALIGN - 1);
  }
  if (wanted != 2 && wanted != 3) {
    return binary_emit_align_code(&ctx->code, BINARY_LOOP_ALIGN,
                                  BINARY_LOOP_ALIGN_MAX_PAD);
  }
  ctx->wants_wide_loop_alignment = 1;
  return binary_emit_align_code(&ctx->code, BINARY_LOOP_ALIGN_BIG,
                                BINARY_LOOP_ALIGN_BIG_MAX_PAD);
}

static int mir_encode_location_marker(MirFunction *fn, const MirInst *in) {
  const IRInstruction *src;

  if (!fn->generator->generate_stack_trace_support || !fn->ir_function ||
      in->ir_index < 0 ||
      (size_t)in->ir_index >= fn->ir_function->instruction_count) {
    return 1;
  }
  src = &fn->ir_function->instructions[in->ir_index];
  if (src->location.line <= 0) {
    return 1;
  }
  return code_generator_binary_emit_runtime_location_marker(
      fn->generator, fn->context, src->location.line, src->location.column,
      code_generator_runtime_filename(fn->generator, src->location.filename));
}

static int mir_encode_jump_tables(MirEncodeState *st) {
  MirFunction *fn = st->fn;
  BinaryFunctionContext *ctx = fn->context;

  for (size_t t = 0; t < st->pending_table_count; t++) {
    const MirJumpTable *tbl = st->pending_tables[t].table;
    size_t table_off;

    while ((ctx->code.size & 3u) != 0u) {
      if (!binary_code_buffer_append_u8(&ctx->code, 0xCC)) {
        return enc_err(fn, "out of memory in jump table");
      }
    }
    table_off = ctx->code.size;
    if (!binary_function_context_patch_rel32(ctx, st->pending_tables[t].lea_off,
                                             table_off)) {
      return enc_err(fn, "jump table out of range");
    }
    for (size_t e = 0; e < tbl->count; e++) {
      BinaryLabelEntry *label =
          binary_label_table_get(&ctx->labels, tbl->labels[e]);
      long long delta;

      if (!label) {
        return enc_err(fn, "undefined jump table target");
      }
      delta = (long long)label->offset - (long long)table_off;
      if (delta < -2147483648LL || delta > 2147483647LL ||
          !binary_code_buffer_append_u32(&ctx->code,
                                         (uint32_t)(int32_t)delta)) {
        return enc_err(fn, "jump table out of range");
      }
    }
  }
  return 1;
}

static int mir_encode_instruction(MirEncodeState *st, const MirInst *in) {
  MirEncodeHandler handler = (unsigned)in->op < (unsigned)MIR_OPCODE_COUNT
                                 ? MIR_ENCODERS[in->op]
                                 : NULL;

  if (!handler) {
    return enc_err(st->fn, "unsupported MIR opcode in encoder");
  }
  return handler(st, in);
}

int mir_encode(MirFunction *fn) {
  BinaryFunctionContext *ctx = NULL;
  MirEncodeState st;
  size_t annot_base;
  char *align_label = NULL;
  int annot;
  int ok = 1;

  if (!fn || !fn->context) {
    return 0;
  }
  ctx = fn->context;
  annot_base = ctx->code.size;
  annot = mir_annotate_enabled();
  memset(&st, 0, sizeof(st));
  st.fn = fn;
  st.fused_byte_load = (size_t)-1;
  st.fused_cmp_load = (size_t)-1;
  st.fused_mask_test = (size_t)-1;
  st.prev_cmpbr = (size_t)-1;
  snprintf(st.epilogue_label, sizeof(st.epilogue_label), ".Lmtlc.epi.%p",
           (void *)fn);

  home_fwd_clear();
  if (!mir_layout_frame(fn) || !mir_emit_prologue(fn)) {
    return 0;
  }
  st.vreg_uses = mir_encode_count_vreg_uses(fn);
  if (!st.vreg_uses) {
    return 0;
  }
  mir_branch_targets_build(fn, &st.branch_targets);
  if (annot && ctx->code.size > annot_base) {
    mir_annotate_record_synthetic("prologue", "frame", 0,
                                  ctx->code.size - annot_base,
                                  ctx->code.data + annot_base);
  }
  align_label = mir_scan_loop_alignment(fn);

  for (size_t i = 0; i < fn->insn_count && ok; i++) {
    const MirInst *in = &fn->insns[i];
    size_t annot_off = ctx->code.size;

    st.index = i;
    home_fwd_note_boundary(in->op);
    if (in->op == MIR_LABEL && align_label && align_label[i] &&
        !mir_encode_loop_alignment(fn, align_label[i])) {
      ok = 0;
      break;
    }
    if (!mir_encode_location_marker(fn, in)) {
      ok = 0;
      break;
    }
    ok = mir_encode_instruction(&st, in);
    if (annot && ok && ctx->code.size > annot_off) {
      mir_annotate_record(fn, in, (int)i, annot_off - annot_base,
                          ctx->code.size - annot_off,
                          ctx->code.data + annot_off);
    }
  }
  free(align_label);
  /* A referenced-but-undefined label means the last return was unreachable in
     layout order; land the shared copy here so no jump dangles. */
  if (ok && st.epilogue_label[0] && !st.epilogue_defined &&
      st.epilogue_referenced) {
    ok = binary_label_table_define(&ctx->labels, st.epilogue_label,
                                   ctx->code.size) &&
         mir_emit_epilogue(fn);
  }
  mir_enc_name_map_free(&st.branch_targets);
  if (!ok || !mir_encode_jump_tables(&st)) {
    free((void *)st.vreg_uses);
    return 0;
  }
  free((void *)st.vreg_uses);
  return code_generator_binary_resolve_fixups(fn->generator, ctx,
                                              ctx->code.size);
}

