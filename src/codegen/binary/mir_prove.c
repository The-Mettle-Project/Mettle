#include "codegen/binary/mir.h"
#include "codegen/binary/mir_prove.h"
#include "common.h"
#include "ir/ir_smt.h"
#include "proof/lia.h"
#include "proof/smt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mir_probe_const_divmod(MirFunction *fn, MirVregId dst, MirVregId a,
                           int64_t C, int uns, int mod);

static int lin_prove_divmod(const MirFunction *fn, MirVregId a, MirVregId dst,
                            int64_t C, int uns, int mod, char *why,
                            size_t cap);

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
  int linear;
  int linear_failed;
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
  uint64_t mag = uns ? (uint64_t)C
                     : (C < 0 ? (uint64_t)0 - (uint64_t)C : (uint64_t)C);
  if (mag > 1 && (mag & (mag - 1)) != 0) {
    char lin_why[160] = "";
    double t0 = mettle_now_ms();
    if (lin_prove_divmod(&fn, a, dst, C, uns, mod, lin_why,
                         sizeof(lin_why))) {
      tally->proved++;
      tally->linear++;
      if (verbose) {
        fprintf(out, "  proved   x %s %lld (%s), %zu MIR instructions, by "
                     "linear integer arithmetic in %.1f ms\n",
                op, (long long)C, uns ? "unsigned" : "signed", fn.insn_count,
                mettle_now_ms() - t0);
      }
      mir_function_destroy(&fn);
      return;
    }
    tally->linear_failed++;
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
      if (uns) {
        fprintf(out, "  REFUTED  x %s %llu (unsigned): wrong at x = %llu\n",
                op, (unsigned long long)C, (unsigned long long)w[0]);
      } else {
        fprintf(out, "  REFUTED  x %s %lld (signed): wrong at x = %lld\n", op,
                (long long)C, (long long)w[0]);
      }
    } else {
      tally->undecided++;
      fprintf(out, "  open     x %s %lld (%s): %s\n", op, (long long)C,
              uns ? "unsigned" : "signed",
              smt_solver_reason(solver) ? smt_solver_reason(solver) : "");
    }
    smt_solver_free(solver);
  }
  free(regs);
  smt_ctx_free(ctx);
  mir_function_destroy(&fn);
}

enum { LV_CONST, LV_U, LV_S };

typedef struct {
  int kind;
  uint64_t raw;
  unsigned var;
  LiaBig lo;
  LiaBig hi;
} LinValue;

typedef struct {
  unsigned var;
  LiaBig values[3];
  unsigned count;
} WrapChoice;

typedef struct {
  LiaSystem *sys;
  LinValue *vals;
  size_t nvals;
  WrapChoice choices[16];
  unsigned nchoices;
  struct {
    unsigned var;
    unsigned of;
  } signs[4];
  unsigned nsigns;
  int failed;
  char why[160];
} LinCtx;

static void lb_u(LiaBig *b, uint64_t v) { lia_big_from_u64(b, v); }
static void lb_i(LiaBig *b, int64_t v) { lia_big_from_i64(b, v); }

static void lin_row(LinCtx *c, const unsigned *vars, const LiaBig *coef,
                    unsigned n, const LiaBig *rhs) {
  unsigned total = lia_var_count(c->sys);
  LiaBig *row = calloc(total ? total : 1, sizeof(LiaBig));
  for (unsigned i = 0; i < n; i++) {
    LiaBig sum;
    if (!lia_big_add(&sum, &row[vars[i]], &coef[i])) {
      c->failed = 1;
    }
    row[vars[i]] = sum;
  }
  lia_add_le(c->sys, row, rhs);
  free(row);
}

static void lin_bounds(LinCtx *c, unsigned v, const LiaBig *lo,
                       const LiaBig *hi) {
  LiaBig one, minus_one, nlo;
  lb_i(&one, 1);
  lb_i(&minus_one, -1);
  lin_row(c, &v, &one, 1, hi);
  lia_big_neg(&nlo, lo);
  lin_row(c, &v, &minus_one, 1, &nlo);
}

static void domain_bounds(int kind, LiaBig *lo, LiaBig *hi) {
  if (kind == LV_U) {
    lb_u(lo, 0);
    lia_big_pow2(hi, 64);
    LiaBig one;
    lb_u(&one, 1);
    lia_big_sub(hi, hi, &one);
  } else {
    lia_big_pow2(hi, 63);
    lia_big_neg(lo, hi);
    LiaBig one;
    lb_u(&one, 1);
    lia_big_sub(hi, hi, &one);
  }
}

static unsigned lin_fresh(LinCtx *c, int kind, LiaBig *lo, LiaBig *hi) {
  unsigned v = lia_add_var(c->sys);
  LiaBig dlo, dhi;
  domain_bounds(kind, &dlo, &dhi);
  if (lia_big_cmp(lo, &dlo) < 0) {
    *lo = dlo;
  }
  if (lia_big_cmp(hi, &dhi) > 0) {
    *hi = dhi;
  }
  lin_bounds(c, v, lo, hi);
  return v;
}

static int lin_operand(LinCtx *c, const MirOperand *op, LinValue *out) {
  if (op->kind == MIR_OPK_IMM) {
    out->kind = LV_CONST;
    out->raw = (uint64_t)op->imm;
    return 1;
  }
  if (op->kind == MIR_OPK_VREG && op->vreg >= 0 &&
      (size_t)op->vreg < c->nvals && c->vals[op->vreg].kind >= 0) {
    *out = c->vals[op->vreg];
    return 1;
  }
  c->failed = 1;
  snprintf(c->why, sizeof(c->why), "an operand with no value");
  return 0;
}

static void const_as(const LinValue *v, int kind, LiaBig *out) {
  if (kind == LV_S) {
    lb_i(out, (int64_t)v->raw);
  } else {
    lb_u(out, v->raw);
  }
}

static void lin_floordiv_pow2(LiaBig *q, const LiaBig *a, unsigned k) {
  LiaBig p;
  lia_big_pow2(&p, k);
  lia_big_floordiv(q, a, &p);
}

static int lin_shift(LinCtx *c, const LinValue *a, unsigned k, int kind,
                     LinValue *out) {
  LiaBig lo, hi, rlo, rhi, one;
  lin_floordiv_pow2(&lo, &a->lo, k);
  lin_floordiv_pow2(&hi, &a->hi, k);
  unsigned v = lin_fresh(c, kind, &lo, &hi);
  lb_u(&rlo, 0);
  lia_big_pow2(&rhi, k);
  lb_u(&one, 1);
  lia_big_sub(&rhi, &rhi, &one);
  unsigned r = lin_fresh(c, LV_S, &rlo, &rhi);
  LiaBig coef[3], zero, pk;
  lia_big_pow2(&pk, k);
  lb_i(&coef[0], 1);
  lia_big_neg(&coef[1], &pk);
  lb_i(&coef[2], -1);
  unsigned vars[3] = {a->var, v, r};
  lb_u(&zero, 0);
  lin_row(c, vars, coef, 3, &zero);
  for (int i = 0; i < 3; i++) {
    lia_big_neg(&coef[i], &coef[i]);
  }
  lin_row(c, vars, coef, 3, &zero);
  out->kind = kind;
  out->var = v;
  out->lo = lo;
  out->hi = hi;
  return 1;
}

static int lin_wrap(LinCtx *c, int kind, const LiaBig *sum_lo,
                    const LiaBig *sum_hi, const unsigned *vars,
                    const LiaBig *coef, unsigned n, LinValue *out) {
  LiaBig dlo, dhi, two64;
  domain_bounds(kind, &dlo, &dhi);
  lia_big_pow2(&two64, 64);
  LiaBig lo = *sum_lo, hi = *sum_hi;
  LiaBig wlo, whi;
  LiaBig t;
  lia_big_sub(&t, sum_lo, &dhi);
  LiaBig adj;
  lia_big_add(&adj, &t, &two64);
  LiaBig one;
  lb_u(&one, 1);
  lia_big_sub(&adj, &adj, &one);
  lia_big_floordiv(&wlo, &adj, &two64);
  lia_big_sub(&t, sum_hi, &dlo);
  lia_big_floordiv(&whi, &t, &two64);
  int64_t wl, wh;
  if (!lia_big_to_i64(&wlo, &wl) || !lia_big_to_i64(&whi, &wh) ||
      wh - wl > 2) {
    c->failed = 1;
    snprintf(c->why, sizeof(c->why), "an addition that can wrap too often");
    return 0;
  }
  unsigned v;
  LiaBig vlo = lo, vhi = hi;
  if (wl == 0 && wh == 0) {
    v = lin_fresh(c, kind, &vlo, &vhi);
  } else {
    vlo = dlo;
    vhi = dhi;
    v = lin_fresh(c, kind, &vlo, &vhi);
  }
  unsigned all[8];
  LiaBig k[8];
  for (unsigned i = 0; i < n; i++) {
    all[i] = vars[i];
    k[i] = coef[i];
  }
  all[n] = v;
  lb_i(&k[n], -1);
  unsigned m = n + 1;
  if (!(wl == 0 && wh == 0)) {
    LiaBig wl_b, wh_b;
    lb_i(&wl_b, wl);
    lb_i(&wh_b, wh);
    unsigned w = lin_fresh(c, LV_S, &wl_b, &wh_b);
    all[m] = w;
    lia_big_neg(&k[m], &two64);
    m++;
    if (c->nchoices >= 16) {
      c->failed = 1;
      snprintf(c->why, sizeof(c->why), "too many wrapping additions");
      return 0;
    }
    WrapChoice *ch = &c->choices[c->nchoices++];
    ch->var = w;
    ch->count = 0;
    for (int64_t x = wl; x <= wh; x++) {
      lb_i(&ch->values[ch->count++], x);
    }
  }
  LiaBig zero;
  lb_u(&zero, 0);
  lin_row(c, all, k, m, &zero);
  for (unsigned i = 0; i < m; i++) {
    lia_big_neg(&k[i], &k[i]);
  }
  lin_row(c, all, k, m, &zero);
  out->kind = kind;
  out->var = v;
  out->lo = vlo;
  out->hi = vhi;
  return 1;
}

static void lin_interval_mul(const LinValue *a, const LiaBig *m, LiaBig *lo,
                             LiaBig *hi) {
  LiaBig p, q;
  lia_big_mul(&p, &a->lo, m);
  lia_big_mul(&q, &a->hi, m);
  if (lia_big_cmp(&p, &q) <= 0) {
    *lo = p;
    *hi = q;
  } else {
    *lo = q;
    *hi = p;
  }
}

static int lin_execute(LinCtx *c, const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count && !c->failed; i++) {
    const MirInst *in = &fn->insns[i];
    LinValue a, b, r;
    memset(&r, 0, sizeof(r));
    if (in->dst.kind != MIR_OPK_VREG || in->dst.vreg < 0 ||
        (size_t)in->dst.vreg >= c->nvals || in->width != 8) {
      c->failed = 1;
      snprintf(c->why, sizeof(c->why), "an instruction with no register");
      return 0;
    }
    switch (in->op) {
    case MIR_MOV:
      if (!lin_operand(c, &in->a, &r)) {
        return 0;
      }
      break;
    case MIR_MULHI: {
      if (!lin_operand(c, &in->a, &a) || !lin_operand(c, &in->b, &b)) {
        return 0;
      }
      if (a.kind == LV_CONST) {
        LinValue t = a;
        a = b;
        b = t;
      }
      int kind = in->is_unsigned ? LV_U : LV_S;
      if (b.kind != LV_CONST || a.kind != kind) {
        c->failed = 1;
        snprintf(c->why, sizeof(c->why), "a multiply-high of two registers");
        return 0;
      }
      LiaBig m, plo, phi, lo, hi, two64;
      const_as(&b, kind, &m);
      lin_interval_mul(&a, &m, &plo, &phi);
      lia_big_pow2(&two64, 64);
      lia_big_floordiv(&lo, &plo, &two64);
      lia_big_floordiv(&hi, &phi, &two64);
      unsigned t = lin_fresh(c, kind, &lo, &hi);
      LiaBig rlo, rhi, one;
      lb_u(&rlo, 0);
      lb_u(&one, 1);
      lia_big_sub(&rhi, &two64, &one);
      unsigned rem = lin_fresh(c, LV_U, &rlo, &rhi);
      unsigned vars[3] = {a.var, t, rem};
      LiaBig coef[3], zero;
      coef[0] = m;
      lia_big_neg(&coef[1], &two64);
      lb_i(&coef[2], -1);
      lb_u(&zero, 0);
      lin_row(c, vars, coef, 3, &zero);
      for (int k = 0; k < 3; k++) {
        lia_big_neg(&coef[k], &coef[k]);
      }
      lin_row(c, vars, coef, 3, &zero);
      r.kind = kind;
      r.var = t;
      r.lo = lo;
      r.hi = hi;
      break;
    }
    case MIR_SHR:
    case MIR_SAR: {
      if (!lin_operand(c, &in->a, &a) || !lin_operand(c, &in->b, &b) ||
          b.kind != LV_CONST || b.raw == 0 || b.raw > 63 ||
          a.kind == LV_CONST) {
        c->failed = 1;
        snprintf(c->why, sizeof(c->why), "a shift the prover does not model");
        return 0;
      }
      unsigned k = (unsigned)b.raw;
      if (in->op == MIR_SAR) {
        if (a.kind != LV_S) {
          c->failed = 1;
          snprintf(c->why, sizeof(c->why), "an arithmetic shift of an "
                                           "unsigned value");
          return 0;
        }
        lin_shift(c, &a, k, LV_S, &r);
      } else if (a.kind == LV_U) {
        lin_shift(c, &a, k, LV_U, &r);
      } else if (k == 63 && c->nsigns < 4) {
        LiaBig lo, hi;
        lb_u(&lo, 0);
        lb_u(&hi, 1);
        unsigned v = lin_fresh(c, LV_U, &lo, &hi);
        c->signs[c->nsigns].var = v;
        c->signs[c->nsigns].of = a.var;
        c->nsigns++;
        r.kind = LV_U;
        r.var = v;
        r.lo = lo;
        r.hi = hi;
      } else {
        c->failed = 1;
        snprintf(c->why, sizeof(c->why), "a logical shift of a signed value");
        return 0;
      }
      break;
    }
    case MIR_ADD:
    case MIR_SUB: {
      if (!lin_operand(c, &in->a, &a) || !lin_operand(c, &in->b, &b)) {
        return 0;
      }
      int kind = a.kind == LV_CONST ? b.kind : a.kind;
      if (kind == LV_CONST) {
        c->failed = 1;
        return 0;
      }
      if (kind == LV_U && ((a.kind == LV_S) || (b.kind == LV_S))) {
        if (a.kind == LV_U && b.kind == LV_S) {
          kind = LV_S;
        } else if (!(a.kind == LV_S && b.kind == LV_U)) {
          c->failed = 1;
          return 0;
        }
      }
      if (a.kind == LV_S || b.kind == LV_S) {
        kind = LV_S;
      }
      unsigned vars[2];
      LiaBig coef[2], slo, shi;
      unsigned n = 0;
      LiaBig klo, khi;
      lb_u(&klo, 0);
      lb_u(&khi, 0);
      LinValue *ops[2] = {&a, &b};
      LiaBig sign[2];
      lb_i(&sign[0], 1);
      lb_i(&sign[1], in->op == MIR_ADD ? 1 : -1);
      lb_u(&slo, 0);
      lb_u(&shi, 0);
      for (int k = 0; k < 2; k++) {
        if (ops[k]->kind == LV_CONST) {
          LiaBig cv, sc;
          const_as(ops[k], kind, &cv);
          lia_big_mul(&sc, &cv, &sign[k]);
          lia_big_add(&slo, &slo, &sc);
          lia_big_add(&shi, &shi, &sc);
          lia_big_add(&klo, &klo, &sc);
          continue;
        }
        vars[n] = ops[k]->var;
        coef[n] = sign[k];
        n++;
        LiaBig p, q;
        lia_big_mul(&p, &ops[k]->lo, &sign[k]);
        lia_big_mul(&q, &ops[k]->hi, &sign[k]);
        if (lia_big_cmp(&p, &q) > 0) {
          LiaBig t = p;
          p = q;
          q = t;
        }
        lia_big_add(&slo, &slo, &p);
        lia_big_add(&shi, &shi, &q);
      }
      if (!lia_big_is_zero(&klo)) {
        LiaBig one;
        lb_u(&one, 1);
        unsigned cv = lin_fresh(c, LV_S, &klo, &klo);
        vars[n] = cv;
        coef[n] = one;
        n++;
        (void)khi;
      }
      lin_wrap(c, kind, &slo, &shi, vars, coef, n, &r);
      break;
    }
    case MIR_NEG: {
      if (!lin_operand(c, &in->a, &a) || a.kind == LV_CONST) {
        c->failed = 1;
        return 0;
      }
      unsigned vars[1] = {a.var};
      LiaBig coef[1], lo, hi;
      lb_i(&coef[0], -1);
      lia_big_neg(&lo, &a.hi);
      lia_big_neg(&hi, &a.lo);
      lin_wrap(c, a.kind, &lo, &hi, vars, coef, 1, &r);
      break;
    }
    case MIR_IMUL: {
      if (!lin_operand(c, &in->a, &a) || !lin_operand(c, &in->b, &b)) {
        return 0;
      }
      if (a.kind == LV_CONST) {
        LinValue t = a;
        a = b;
        b = t;
      }
      if (b.kind != LV_CONST || a.kind == LV_CONST) {
        c->failed = 1;
        snprintf(c->why, sizeof(c->why), "a multiply of two registers");
        return 0;
      }
      LiaBig m, lo, hi;
      const_as(&b, a.kind, &m);
      lin_interval_mul(&a, &m, &lo, &hi);
      unsigned vars[1] = {a.var};
      lin_wrap(c, a.kind, &lo, &hi, vars, &m, 1, &r);
      break;
    }
    default:
      c->failed = 1;
      snprintf(c->why, sizeof(c->why), "MIR opcode %d", (int)in->op);
      return 0;
    }
    c->vals[in->dst.vreg] = r;
  }
  return !c->failed;
}

static void lin_eq(LiaSystem *sys, const unsigned *vars, const LiaBig *coef,
                   unsigned n, const LiaBig *rhs) {
  unsigned total = lia_var_count(sys);
  LiaBig *row = calloc(total ? total : 1, sizeof(LiaBig));
  for (unsigned i = 0; i < n; i++) {
    LiaBig sum;
    lia_big_add(&sum, &row[vars[i]], &coef[i]);
    row[vars[i]] = sum;
  }
  lia_add_le(sys, row, rhs);
  for (unsigned i = 0; i < total; i++) {
    lia_big_neg(&row[i], &row[i]);
  }
  LiaBig nr;
  lia_big_neg(&nr, rhs);
  lia_add_le(sys, row, &nr);
  free(row);
}

static void lin_le(LiaSystem *sys, const unsigned *vars, const LiaBig *coef,
                   unsigned n, const LiaBig *rhs) {
  unsigned total = lia_var_count(sys);
  LiaBig *row = calloc(total ? total : 1, sizeof(LiaBig));
  for (unsigned i = 0; i < n; i++) {
    LiaBig sum;
    lia_big_add(&sum, &row[vars[i]], &coef[i]);
    row[vars[i]] = sum;
  }
  lia_add_le(sys, row, rhs);
  free(row);
}

static int lin_all_cases_infeasible(LinCtx *c, LiaSystem *base,
                                    unsigned choice, LiaVerdict *worst) {
  if (choice == c->nchoices) {
    if (c->nsigns) {
      unsigned combos = 1u << c->nsigns;
      for (unsigned mask = 0; mask < combos; mask++) {
        LiaSystem *s = lia_copy(base);
        for (unsigned k = 0; k < c->nsigns; k++) {
          int set = (mask >> k) & 1u;
          LiaBig one, val, zero, minus_one;
          lb_i(&one, 1);
          lb_i(&minus_one, -1);
          lb_u(&zero, 0);
          lb_i(&val, set);
          unsigned v = c->signs[k].var;
          lin_eq(s, &v, &one, 1, &val);
          unsigned of = c->signs[k].of;
          if (set) {
            lin_le(s, &of, &one, 1, &minus_one);
          } else {
            lin_le(s, &of, &minus_one, 1, &zero);
          }
        }
        LiaVerdict v = lia_check(s);
        lia_free(s);
        if (v != LIA_INFEASIBLE) {
          *worst = v;
          return 0;
        }
      }
      return 1;
    }
    LiaVerdict v = lia_check(base);
    if (v != LIA_INFEASIBLE) {
      *worst = v;
      return 0;
    }
    return 1;
  }
  WrapChoice *ch = &c->choices[choice];
  for (unsigned k = 0; k < ch->count; k++) {
    LiaSystem *s = lia_copy(base);
    LiaBig one;
    lb_i(&one, 1);
    lin_eq(s, &ch->var, &one, 1, &ch->values[k]);
    int ok = lin_all_cases_infeasible(c, s, choice + 1, worst);
    lia_free(s);
    if (!ok) {
      return 0;
    }
  }
  return 1;
}

static int lin_prove_divmod(const MirFunction *fn, MirVregId a, MirVregId dst,
                            int64_t C, int uns, int mod, char *why,
                            size_t cap) {
  LinCtx c;
  memset(&c, 0, sizeof(c));
  c.sys = lia_new(0);
  c.nvals = fn->vreg_count;
  c.vals = calloc(c.nvals ? c.nvals : 1, sizeof(LinValue));
  for (size_t i = 0; i < c.nvals; i++) {
    c.vals[i].kind = -1;
  }
  int kind = uns ? LV_U : LV_S;
  LiaBig lo, hi;
  domain_bounds(kind, &lo, &hi);
  unsigned x = lin_fresh(&c, kind, &lo, &hi);
  c.vals[a].kind = kind;
  c.vals[a].var = x;
  c.vals[a].lo = lo;
  c.vals[a].hi = hi;
  int proved = 0;
  if (!lin_execute(&c, fn) || c.vals[dst].kind < 0) {
    snprintf(why, cap, "%s", c.why[0] ? c.why : "no result");
    goto out;
  }
  LinValue y = c.vals[dst];
  if (y.kind == LV_CONST) {
    snprintf(why, cap, "a constant result");
    goto out;
  }
  LiaBig cb, absc, one, zero, minus_one;
  lb_i(&one, 1);
  lb_i(&minus_one, -1);
  lb_u(&zero, 0);
  if (uns) {
    lb_u(&cb, (uint64_t)C);
  } else {
    lb_i(&cb, C);
  }
  absc = cb;
  absc.neg = 0;
  LiaBig qlo, qhi;
  domain_bounds(LV_S, &qlo, &qhi);
  LiaBig wide_lo, wide_hi;
  lia_big_pow2(&wide_hi, 65);
  lia_big_neg(&wide_lo, &wide_hi);
  unsigned q = lia_add_var(c.sys);
  lin_bounds(&c, q, &wide_lo, &wide_hi);
  (void)qlo;
  (void)qhi;
  if (y.kind != kind) {
    snprintf(why, cap, "the result is read with the other signedness");
    goto out;
  }
  LiaBig absm1;
  lia_big_sub(&absm1, &absc, &one);
  int sign_cases = uns ? 1 : 2;
  LiaVerdict worst = LIA_INFEASIBLE;
  int all = 1;
  for (int xs = 0; xs < sign_cases && all; xs++) {
    LiaSystem *base = lia_copy(c.sys);
    {
      unsigned vars[2] = {x, q};
      LiaBig coef[2];
      lb_i(&coef[0], -1);
      coef[1] = cb;
      if (uns || xs == 0) {
        lin_le(base, vars, coef, 2, &zero);
        lb_i(&coef[0], 1);
        lia_big_neg(&coef[1], &cb);
        lin_le(base, vars, coef, 2, &absm1);
        if (!uns) {
          lin_le(base, &x, &minus_one, 1, &zero);
        }
      } else {
        LiaBig nabsm1;
        lia_big_neg(&nabsm1, &absm1);
        lin_le(base, vars, coef, 2, &absm1);
        LiaBig k2[2];
        lb_i(&k2[0], 1);
        lia_big_neg(&k2[1], &cb);
        LiaBig tmp;
        lia_big_neg(&tmp, &zero);
        lin_le(base, vars, k2, 2, &zero);
        (void)nabsm1;
        lin_le(base, &x, &one, 1, &minus_one);
      }
    }
    for (int side = 0; side < 2 && all; side++) {
      LiaSystem *s = lia_copy(base);
      if (!mod) {
        unsigned vars[2] = {y.var, q};
        LiaBig coef[2];
        if (side == 0) {
          lb_i(&coef[0], 1);
          lb_i(&coef[1], -1);
        } else {
          lb_i(&coef[0], -1);
          lb_i(&coef[1], 1);
        }
        lin_le(s, vars, coef, 2, &minus_one);
      } else {
        unsigned vars[3] = {y.var, x, q};
        LiaBig coef[3];
        if (side == 0) {
          lb_i(&coef[0], 1);
          lb_i(&coef[1], -1);
          coef[2] = cb;
        } else {
          lb_i(&coef[0], -1);
          lb_i(&coef[1], 1);
          lia_big_neg(&coef[2], &cb);
        }
        lin_le(s, vars, coef, 3, &minus_one);
      }
      if (!lin_all_cases_infeasible(&c, s, 0, &worst)) {
        all = 0;
      }
      lia_free(s);
    }
    lia_free(base);
  }
  if (all && !c.failed) {
    proved = 1;
  } else {
    snprintf(why, cap, "%s",
             c.failed ? "a coefficient outgrew the arithmetic"
             : worst == LIA_TOO_BIG
                 ? "the elimination grew past its row budget"
                 : "a case the linear relaxation does not refute");
  }
out:
  free(c.vals);
  lia_free(c.sys);
  return proved;
}

int mir_prove_divmod_lowering(FILE *out, int verbose) {
  MpTally tally = {0, 0, 0, 0, 0, 0};
  double t0 = mettle_now_ms();
  const char *limit_env = getenv("METTLE_PROVE_DIVISORS");
  long limit = limit_env ? (long)strtoull(limit_env, NULL, 10) : 100;
  for (int uns = 0; uns < 2; uns++) {
    for (int mod = 0; mod < 2; mod++) {
      for (long d = 3; d <= limit; d++) {
        if ((d & (d - 1)) == 0) {
          continue;
        }
        mp_check(out, verbose, &tally, d, uns, mod);
        if (!uns) {
          mp_check(out, verbose, &tally, -d, uns, mod);
        }
      }
      uint64_t big[160];
      unsigned nbig = 0;
      for (unsigned k = 7; k < 64; k += 4) {
        big[nbig++] = ((uint64_t)1 << k) - 1;
        big[nbig++] = ((uint64_t)1 << k) + 1;
      }
      uint64_t ten = 1000;
      for (int k = 3; k <= 19; k++, ten *= 10) {
        big[nbig++] = ten;
        if (k == 19) {
          break;
        }
      }
      big[nbig++] = 0x7fffffffffffffffULL;
      big[nbig++] = 0xfffffffffffffffeULL;
      big[nbig++] = 0x8000000000000001ULL;
      big[nbig++] = 0x123456789abcdefULL;
      for (unsigned i = 0; i < nbig; i++) {
        uint64_t d = big[i];
        if (!uns && d > 0x7fffffffffffffffULL) {
          continue;
        }
        if (d <= (uint64_t)limit) {
          continue;
        }
        mp_check(out, verbose, &tally, (int64_t)d, uns, mod);
        if (!uns) {
          mp_check(out, verbose, &tally, -(int64_t)d, uns, mod);
        }
      }
    }
  }
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
