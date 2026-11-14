#include "codegen/binary/mir.h"
#include "codegen/binary/mir_prove.h"
#include "common.h"
#include "ir/ir_smt.h"
#include "proof/smt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mir_probe_const_divmod(MirFunction *fn, MirVregId dst, MirVregId a,
                           int64_t C, int uns, int mod);

static SmtTerm mp_read(SmtCtx *ctx, const SmtTerm *regs, size_t nregs,
                       const MirOperand *op, int *ok) {
  if (op->kind == MIR_OPK_VREG && op->vreg >= 0 && (size_t)op->vreg < nregs &&
      regs[op->vreg]) {
    return regs[op->vreg];
  }
  if (op->kind == MIR_OPK_IMM) {
    return smt_bv_i64(ctx, 64, op->imm);
  }
  *ok = 0;
  return smt_bv_u64(ctx, 64, 0);
}

static SmtTerm mp_mulhi(SmtCtx *ctx, SmtTerm a, SmtTerm b, int is_unsigned) {
  SmtTerm wa = is_unsigned ? smt_zext(ctx, a, 64) : smt_sext(ctx, a, 64);
  SmtTerm wb = is_unsigned ? smt_zext(ctx, b, 64) : smt_sext(ctx, b, 64);
  return smt_extract(ctx, smt_mk2(ctx, SMT_K_BVMUL, wa, wb), 127, 64);
}

static int mp_execute(SmtCtx *ctx, const MirFunction *fn, SmtTerm *regs,
                      size_t nregs, char *why, size_t cap) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    int ok = 1;
    if (in->width != 8) {
      snprintf(why, cap, "an instruction of width %d", in->width);
      return 0;
    }
    SmtTerm a = 0, b = 0, r = 0;
    SmtTerm count;
    switch (in->op) {
    case MIR_MOV:
      r = mp_read(ctx, regs, nregs, &in->a, &ok);
      break;
    case MIR_ADD:
    case MIR_SUB:
    case MIR_AND:
    case MIR_OR:
    case MIR_XOR:
    case MIR_IMUL:
      a = mp_read(ctx, regs, nregs, &in->a, &ok);
      b = mp_read(ctx, regs, nregs, &in->b, &ok);
      r = smt_mk2(ctx,
                  in->op == MIR_ADD   ? SMT_K_BVADD
                  : in->op == MIR_SUB ? SMT_K_BVSUB
                  : in->op == MIR_AND ? SMT_K_BVAND
                  : in->op == MIR_OR  ? SMT_K_BVOR
                  : in->op == MIR_XOR ? SMT_K_BVXOR
                                      : SMT_K_BVMUL,
                  a, b);
      break;
    case MIR_NEG:
      r = smt_mk1(ctx, SMT_K_BVNEG, mp_read(ctx, regs, nregs, &in->a, &ok));
      break;
    case MIR_NOT:
      r = smt_mk1(ctx, SMT_K_BVNOT, mp_read(ctx, regs, nregs, &in->a, &ok));
      break;
    case MIR_SHL:
    case MIR_SHR:
    case MIR_SAR:
      a = mp_read(ctx, regs, nregs, &in->a, &ok);
      count = smt_mk2(ctx, SMT_K_BVAND, mp_read(ctx, regs, nregs, &in->b, &ok),
                      smt_bv_u64(ctx, 64, 63));
      r = smt_mk2(ctx,
                  in->op == MIR_SHL   ? SMT_K_BVSHL
                  : in->op == MIR_SHR ? SMT_K_BVLSHR
                                      : SMT_K_BVASHR,
                  a, count);
      break;
    case MIR_MULHI:
      r = mp_mulhi(ctx, mp_read(ctx, regs, nregs, &in->a, &ok),
                   mp_read(ctx, regs, nregs, &in->b, &ok), in->is_unsigned);
      break;
    default:
      snprintf(why, cap, "MIR opcode %d, which the prover does not model",
               (int)in->op);
      return 0;
    }
    if (!ok || in->dst.kind != MIR_OPK_VREG || in->dst.vreg < 0 ||
        (size_t)in->dst.vreg >= nregs) {
      snprintf(why, cap, "an operand the prover does not model");
      return 0;
    }
    regs[in->dst.vreg] = r;
  }
  return 1;
}

typedef struct {
  int total;
  int proved;
  int refuted;
  int undecided;
} MpTally;

static void mp_check(FILE *out, int verbose, MpTally *tally, int64_t C,
                     int uns, int mod) {
  MirFunction fn;
  mir_function_init(&fn, NULL);
  MirVregId a = mir_new_vreg(&fn, MIR_RC_GP, 8);
  MirVregId dst = mir_new_vreg(&fn, MIR_RC_GP, 8);
  const char *op = mod ? "%" : "/";
  tally->total++;
  if (!mir_probe_const_divmod(&fn, dst, a, C, uns, mod)) {
    tally->undecided++;
    if (verbose) {
      fprintf(out, "  skipped  x %s %lld (%s): the lowering declined\n", op,
              (long long)C, uns ? "unsigned" : "signed");
    }
    mir_function_destroy(&fn);
    return;
  }
  SmtCtx *ctx = smt_ctx_new();
  size_t nregs = fn.vreg_count;
  SmtTerm *regs = calloc(nregs ? nregs : 1, sizeof(SmtTerm));
  SmtTerm x = smt_var(ctx, "x", smt_bv_sort(ctx, 64));
  regs[a] = x;
  char why[160] = "";
  int ran = mp_execute(ctx, &fn, regs, nregs, why, sizeof(why));
  if (!ran || !regs[dst]) {
    tally->undecided++;
    fprintf(out, "  UNKNOWN  x %s %lld (%s): %s\n", op, (long long)C,
            uns ? "unsigned" : "signed", ran ? "no result" : why);
  } else {
    SmtTerm expect = ir_smt_binary(ctx, op, uns, x, smt_bv_i64(ctx, 64, C),
                                   NULL);
    SmtSolver *solver = NULL;
    SmtResult r = smt_prove(ctx, smt_eq(ctx, regs[dst], expect), 1, 30000,
                            &solver);
    if (r == SMT_UNSAT) {
      tally->proved++;
      if (verbose) {
        fprintf(out, "  proved   x %s %lld (%s), %zu MIR instructions\n", op,
                (long long)C, uns ? "unsigned" : "signed", fn.insn_count);
      }
    } else if (r == SMT_SAT) {
      uint64_t w[4];
      smt_solver_value(solver, x, w);
      tally->refuted++;
      fprintf(out, "  REFUTED  x %s %lld (%s): wrong at x = %lld\n", op,
              (long long)C, uns ? "unsigned" : "signed", (long long)w[0]);
    } else {
      tally->undecided++;
      if (verbose) {
        fprintf(out, "  open     x %s %lld (%s): %s\n", op, (long long)C,
                uns ? "unsigned" : "signed",
                smt_solver_reason(solver) ? smt_solver_reason(solver) : "");
      }
    }
    smt_solver_free(solver);
  }
  free(regs);
  smt_ctx_free(ctx);
  mir_function_destroy(&fn);
}

int mir_prove_divmod_lowering(FILE *out, int verbose) {
  MpTally tally = {0, 0, 0, 0};
  double t0 = mettle_now_ms();
  for (int uns = 0; uns < 2; uns++) {
    for (int mod = 0; mod < 2; mod++) {
      for (int k = 0; k <= 62; k++) {
        int64_t c = (int64_t)1 << k;
        mp_check(out, verbose, &tally, c, uns, mod);
        if (!uns) {
          mp_check(out, verbose, &tally, -c, uns, mod);
        }
      }
      if (!uns) {
        mp_check(out, verbose, &tally, -1, uns, mod);
        mp_check(out, verbose, &tally, INT64_MIN, uns, mod);
      }
    }
  }
  fprintf(out,
          "%d constant divisions lowered by mir_emit_const_divmod: %d proved, "
          "%d refuted, %d not decided, %.1f ms\n",
          tally.total, tally.proved, tally.refuted, tally.undecided,
          mettle_now_ms() - t0);
  return tally.refuted == 0;
}
