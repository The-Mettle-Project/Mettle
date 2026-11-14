#include "smt_internal.h"
#include <stdlib.h>
#include <string.h>

#define NORM_MAX_FACTORS 8u
#define NORM_MAX_MONOS 64u

typedef struct {
  SmtBits coeff;
  uint32_t nf;
  SmtTerm f[NORM_MAX_FACTORS];
} Mono;

typedef struct {
  Mono m[NORM_MAX_MONOS];
  unsigned n;
  unsigned w;
  int ok;
} Poly;

typedef struct {
  SmtCtx *ctx;
  SmtMap memo[3];
  unsigned case_budget;
} Norm;

enum { POL_BOTH = 0, POL_POS = 1, POL_NEG = 2 };

static int mono_cmp(const Mono *a, const Mono *b) {
  if (a->nf != b->nf) {
    return a->nf < b->nf ? -1 : 1;
  }
  for (uint32_t i = 0; i < a->nf; i++) {
    if (a->f[i] != b->f[i]) {
      return a->f[i] < b->f[i] ? -1 : 1;
    }
  }
  return 0;
}

static void poly_add_mono(Poly *p, const Mono *m) {
  if (!p->ok || smt_bits_is_zero(&m->coeff)) {
    return;
  }
  for (unsigned i = 0; i < p->n; i++) {
    if (mono_cmp(&p->m[i], m) == 0) {
      smt_bits_add(&p->m[i].coeff, &p->m[i].coeff, &m->coeff, p->w);
      if (smt_bits_is_zero(&p->m[i].coeff)) {
        p->m[i] = p->m[--p->n];
      }
      return;
    }
  }
  if (p->n == NORM_MAX_MONOS) {
    p->ok = 0;
    return;
  }
  p->m[p->n++] = *m;
}

static void poly_init(Poly *p, unsigned w) {
  p->n = 0;
  p->w = w;
  p->ok = 1;
}

static void poly_const(Poly *p, unsigned w, const SmtBits *c) {
  poly_init(p, w);
  Mono m;
  memset(&m, 0, sizeof(m));
  m.coeff = *c;
  smt_bits_mask(&m.coeff, w);
  poly_add_mono(p, &m);
}

static void poly_atom(Poly *p, unsigned w, SmtTerm t) {
  poly_init(p, w);
  Mono m;
  memset(&m, 0, sizeof(m));
  smt_bits_from_u64(&m.coeff, 1, w);
  m.nf = 1;
  m.f[0] = t;
  poly_add_mono(p, &m);
}

static void poly_scale(Poly *p, const SmtBits *c) {
  unsigned j = 0;
  for (unsigned i = 0; i < p->n; i++) {
    smt_bits_mul(&p->m[i].coeff, &p->m[i].coeff, c, p->w);
    if (!smt_bits_is_zero(&p->m[i].coeff)) {
      p->m[j++] = p->m[i];
    }
  }
  p->n = j;
}

static void poly_add(Poly *p, const Poly *q) {
  if (!q->ok) {
    p->ok = 0;
    return;
  }
  for (unsigned i = 0; i < q->n && p->ok; i++) {
    poly_add_mono(p, &q->m[i]);
  }
}

static int poly_is_const(const Poly *p, SmtBits *c) {
  smt_bits_zero(c);
  for (unsigned i = 0; i < p->n; i++) {
    if (p->m[i].nf != 0) {
      return 0;
    }
    *c = p->m[i].coeff;
  }
  return 1;
}

static int factor_cmp(const void *a, const void *b) {
  SmtTerm x = *(const SmtTerm *)a, y = *(const SmtTerm *)b;
  return x < y ? -1 : x > y;
}

static void poly_mul(Poly *out, const Poly *a, const Poly *b,
                     unsigned max_products) {
  poly_init(out, a->w);
  if (!a->ok || !b->ok || a->n * b->n > max_products) {
    out->ok = 0;
    return;
  }
  for (unsigned i = 0; i < a->n && out->ok; i++) {
    for (unsigned j = 0; j < b->n && out->ok; j++) {
      const Mono *x = &a->m[i], *y = &b->m[j];
      if (x->nf + y->nf > NORM_MAX_FACTORS) {
        out->ok = 0;
        return;
      }
      Mono m;
      memset(&m, 0, sizeof(m));
      smt_bits_mul(&m.coeff, &x->coeff, &y->coeff, a->w);
      m.nf = x->nf + y->nf;
      memcpy(m.f, x->f, x->nf * sizeof(SmtTerm));
      memcpy(m.f + x->nf, y->f, y->nf * sizeof(SmtTerm));
      qsort(m.f, m.nf, sizeof(SmtTerm), factor_cmp);
      poly_add_mono(out, &m);
    }
  }
}

static int mono_qsort(const void *a, const void *b) {
  return mono_cmp((const Mono *)a, (const Mono *)b);
}

static int is_arith(SmtKind k) {
  return k == SMT_K_BVADD || k == SMT_K_BVSUB || k == SMT_K_BVMUL ||
         k == SMT_K_BVNEG || k == SMT_K_BVNOT || k == SMT_K_BVSHL ||
         k == SMT_K_CONST;
}

static void poly_of(Norm *nm, SmtTerm t, Poly *p, unsigned budget) {
  SmtCtx *ctx = nm->ctx;
  const SmtNode *n = &ctx->nodes[t];
  unsigned w = ctx->sorts[n->sort].width;
  SmtKind k = (SmtKind)n->kind;
  if (budget == 0 || !is_arith(k)) {
    poly_atom(p, w, t);
    return;
  }
  SmtTerm a = n->nargs > 0 ? smt_arg(ctx, t, 0) : 0;
  SmtTerm b = n->nargs > 1 ? smt_arg(ctx, t, 1) : 0;
  Poly q;
  SmtBits c;
  switch (k) {
  case SMT_K_CONST:
    poly_const(p, w, &ctx->consts[n->p0]);
    return;
  case SMT_K_BVADD:
  case SMT_K_BVSUB:
    poly_of(nm, a, p, budget - 1u);
    poly_of(nm, b, &q, budget - 1u);
    if (k == SMT_K_BVSUB) {
      smt_bits_ones(&c, w);
      poly_scale(&q, &c);
    }
    poly_add(p, &q);
    if (!p->ok) {
      poly_atom(p, w, t);
    }
    return;
  case SMT_K_BVNEG:
  case SMT_K_BVNOT:
    poly_of(nm, a, p, budget - 1u);
    smt_bits_ones(&c, w);
    poly_scale(p, &c);
    if (k == SMT_K_BVNOT) {
      poly_const(&q, w, &c);
      poly_add(p, &q);
    }
    if (!p->ok) {
      poly_atom(p, w, t);
    }
    return;
  case SMT_K_BVSHL: {
    SmtBits s;
    if (!smt_term_const_words(ctx, b, s.w)) {
      poly_atom(p, w, t);
      return;
    }
    poly_of(nm, a, p, budget - 1u);
    SmtBits one, factor;
    smt_bits_from_u64(&one, 1, w);
    smt_bits_shl(&factor, &one, &s, w);
    poly_scale(p, &factor);
    if (!p->ok) {
      poly_atom(p, w, t);
    }
    return;
  }
  case SMT_K_BVMUL: {
    Poly *pa = malloc(sizeof(Poly));
    Poly *pb = malloc(sizeof(Poly));
    poly_of(nm, a, pa, budget - 1u);
    poly_of(nm, b, pb, budget - 1u);
    SmtBits ca;
    if (poly_is_const(pa, &ca)) {
      *p = *pb;
      poly_scale(p, &ca);
    } else if (poly_is_const(pb, &ca)) {
      *p = *pa;
      poly_scale(p, &ca);
    } else {
      poly_mul(p, pa, pb, 4u);
    }
    free(pa);
    free(pb);
    if (!p->ok) {
      poly_atom(p, w, t);
    }
    return;
  }
  default:
    poly_atom(p, w, t);
    return;
  }
}

static unsigned popcount_bits(const SmtBits *b) {
  unsigned n = 0;
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    n += (unsigned)__builtin_popcountll(b->w[i]);
  }
  return n;
}

static int power_of_two(const SmtBits *b, unsigned *log2) {
  if (popcount_bits(b) != 1u) {
    return 0;
  }
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    if (b->w[i]) {
      *log2 = i * 64u + (unsigned)__builtin_ctzll(b->w[i]);
      return 1;
    }
  }
  return 0;
}

static SmtTerm scaled(SmtCtx *ctx, SmtTerm m, const SmtBits *c, unsigned w) {
  unsigned k;
  SmtBits one;
  smt_bits_from_u64(&one, 1, w);
  if (smt_bits_eq(c, &one)) {
    return m;
  }
  if (power_of_two(c, &k)) {
    return smt_mk2(ctx, SMT_K_BVSHL, m, smt_bv_u64(ctx, w, k));
  }
  return smt_mk2(ctx, SMT_K_BVMUL, smt_bv_words(ctx, w, c->w), m);
}

static void poly_split(SmtCtx *ctx, Poly *p, SmtTerm *pos_out,
                       SmtTerm *neg_out, SmtBits *constant_out) {
  unsigned w = p->w;
  qsort(p->m, p->n, sizeof(Mono), mono_qsort);
  SmtTerm pos = 0, neg = 0;
  SmtBits constant;
  smt_bits_zero(&constant);
  for (unsigned i = 0; i < p->n; i++) {
    Mono *m = &p->m[i];
    if (m->nf == 0) {
      constant = m->coeff;
      continue;
    }
    SmtTerm prod = m->f[0];
    for (uint32_t j = 1; j < m->nf; j++) {
      prod = smt_mk2(ctx, SMT_K_BVMUL, prod, m->f[j]);
    }
    SmtBits negc;
    smt_bits_neg(&negc, &m->coeff, w);
    if (popcount_bits(&negc) < popcount_bits(&m->coeff)) {
      SmtTerm term = scaled(ctx, prod, &negc, w);
      neg = neg ? smt_mk2(ctx, SMT_K_BVADD, neg, term) : term;
    } else {
      SmtTerm term = scaled(ctx, prod, &m->coeff, w);
      pos = pos ? smt_mk2(ctx, SMT_K_BVADD, pos, term) : term;
    }
  }
  *pos_out = pos;
  *neg_out = neg;
  *constant_out = constant;
}

static SmtTerm poly_build(SmtCtx *ctx, Poly *p) {
  unsigned w = p->w;
  SmtTerm pos, neg;
  SmtBits constant;
  poly_split(ctx, p, &pos, &neg, &constant);
  SmtTerm out;
  if (pos && neg) {
    out = smt_mk2(ctx, SMT_K_BVSUB, pos, neg);
  } else if (pos) {
    out = pos;
  } else if (neg) {
    out = smt_mk1(ctx, SMT_K_BVNEG, neg);
  } else {
    return smt_bv_words(ctx, w, constant.w);
  }
  if (!smt_bits_is_zero(&constant)) {
    out = smt_mk2(ctx, SMT_K_BVADD, out, smt_bv_words(ctx, w, constant.w));
  }
  return out;
}

static int low_mask_bits(SmtCtx *ctx, SmtTerm t, unsigned *bits) {
  uint64_t w[SMT_WORDS];
  if (!smt_term_const_words(ctx, t, w)) {
    return 0;
  }
  SmtBits b, plus;
  memcpy(b.w, w, sizeof(b.w));
  SmtBits one;
  smt_bits_from_u64(&one, 1, SMT_MAX_WIDTH);
  smt_bits_add(&plus, &b, &one, SMT_MAX_WIDTH);
  unsigned k;
  if (smt_bits_is_zero(&b) || !power_of_two(&plus, &k)) {
    return 0;
  }
  *bits = k;
  return 1;
}

static int find_split(SmtCtx *ctx, const Poly *p, SmtTerm *v_out,
                      SmtTerm *hi_out, SmtTerm *lo_out, unsigned *k_out) {
  for (unsigned i = 0; i < p->n; i++) {
    for (uint32_t f = 0; f < p->m[i].nf; f++) {
      SmtTerm hi = p->m[i].f[f];
      if (smt_term_kind(ctx, hi) != SMT_K_BVLSHR) {
        continue;
      }
      SmtTerm v = smt_term_arg(ctx, hi, 0);
      uint64_t kw[SMT_WORDS];
      if (!smt_term_const_words(ctx, smt_term_arg(ctx, hi, 1), kw) ||
          kw[1] || kw[2] || kw[3] || kw[0] == 0 ||
          kw[0] >= smt_sort_width(ctx, smt_term_sort(ctx, v))) {
        continue;
      }
      unsigned k = (unsigned)kw[0];
      for (unsigned j = 0; j < p->n; j++) {
        for (uint32_t g = 0; g < p->m[j].nf; g++) {
          SmtTerm lo = p->m[j].f[g];
          if (smt_term_kind(ctx, lo) != SMT_K_BVAND) {
            continue;
          }
          SmtTerm x0 = smt_term_arg(ctx, lo, 0), x1 = smt_term_arg(ctx, lo, 1);
          SmtTerm other = x0 == v ? x1 : x1 == v ? x0 : 0;
          unsigned bits;
          if (!other || !low_mask_bits(ctx, other, &bits) || bits != k) {
            continue;
          }
          for (unsigned q = 0; q < p->n; q++) {
            for (uint32_t h = 0; h < p->m[q].nf; h++) {
              if (p->m[q].f[h] == v) {
                *v_out = v;
                *hi_out = hi;
                *lo_out = lo;
                *k_out = k;
                return 1;
              }
            }
          }
        }
      }
    }
  }
  return 0;
}

static int split_variable(SmtCtx *ctx, Poly *p) {
  SmtTerm v, hi, lo;
  unsigned k;
  if (!find_split(ctx, p, &v, &hi, &lo, &k)) {
    return 0;
  }
  Poly *out = malloc(sizeof(Poly));
  poly_init(out, p->w);
  SmtBits scale, one, kb;
  smt_bits_from_u64(&one, 1, p->w);
  smt_bits_from_u64(&kb, k, p->w);
  smt_bits_shl(&scale, &one, &kb, p->w);
  for (unsigned i = 0; i < p->n && out->ok; i++) {
    Mono m = p->m[i];
    uint32_t at = m.nf;
    for (uint32_t f = 0; f < m.nf; f++) {
      if (m.f[f] == v) {
        at = f;
        break;
      }
    }
    if (at == m.nf) {
      poly_add_mono(out, &m);
      continue;
    }
    Mono with_hi = m, with_lo = m;
    with_hi.f[at] = hi;
    with_lo.f[at] = lo;
    qsort(with_hi.f, with_hi.nf, sizeof(SmtTerm), factor_cmp);
    qsort(with_lo.f, with_lo.nf, sizeof(SmtTerm), factor_cmp);
    smt_bits_mul(&with_hi.coeff, &with_hi.coeff, &scale, p->w);
    poly_add_mono(out, &with_hi);
    poly_add_mono(out, &with_lo);
  }
  int ok = out->ok;
  if (ok) {
    *p = *out;
  }
  free(out);
  return ok;
}

static SmtTerm zero_test(Norm *nm, Poly *p, int split, unsigned depth);

static void poly_under(Norm *nm, const Poly *p, SmtTerm condition, int holds,
                       Poly *out) {
  SmtCtx *ctx = nm->ctx;
  Poly *product = malloc(sizeof(Poly));
  Poly *factor = malloc(sizeof(Poly));
  Poly *next = malloc(sizeof(Poly));
  poly_init(out, p->w);
  for (unsigned i = 0; i < p->n && out->ok; i++) {
    const Mono *m = &p->m[i];
    poly_const(product, p->w, &m->coeff);
    for (uint32_t f = 0; f < m->nf && product->ok; f++) {
      SmtTerm t = m->f[f];
      if (smt_term_kind(ctx, t) == SMT_K_ITE &&
          smt_term_arg(ctx, t, 0) == condition) {
        poly_of(nm, smt_term_arg(ctx, t, holds ? 1 : 2), factor, 64);
      } else {
        poly_atom(factor, p->w, t);
      }
      poly_mul(next, product, factor, NORM_MAX_MONOS);
      *product = *next;
    }
    poly_add(out, product);
  }
  free(product);
  free(factor);
  free(next);
}

static int hides_arithmetic(SmtCtx *ctx, SmtTerm ite) {
  for (unsigned branch = 1; branch <= 2u; branch++) {
    SmtKind k = smt_term_kind(ctx, smt_term_arg(ctx, ite, branch));
    if (is_arith(k) && k != SMT_K_CONST) {
      return 1;
    }
  }
  return 0;
}

static SmtTerm split_on_condition(Norm *nm, const Poly *p, int split,
                                  unsigned depth) {
  SmtCtx *ctx = nm->ctx;
  if (depth >= 3u) {
    return 0;
  }
  SmtTerm tried = 0;
  unsigned tries = 0;
  SmtTerm out = 0;
  Poly *yes_poly = malloc(sizeof(Poly));
  Poly *no_poly = malloc(sizeof(Poly));
  for (unsigned i = 0; i < p->n && !out && tries < 2u; i++) {
    for (uint32_t f = 0; f < p->m[i].nf && !out && tries < 2u; f++) {
      SmtTerm it = p->m[i].f[f];
      if (smt_term_kind(ctx, it) != SMT_K_ITE || !hides_arithmetic(ctx, it)) {
        continue;
      }
      SmtTerm condition = smt_term_arg(ctx, it, 0);
      if (condition == tried) {
        continue;
      }
      if (nm->case_budget == 0) {
        tries = 2u;
        break;
      }
      nm->case_budget--;
      tried = condition;
      tries++;
      poly_under(nm, p, condition, 1, yes_poly);
      poly_under(nm, p, condition, 0, no_poly);
      SmtTerm yes = zero_test(nm, yes_poly, split, depth + 1u);
      SmtTerm no = zero_test(nm, no_poly, split, depth + 1u);
      if (yes && no &&
          (depth == 0 || smt_term_is_const(ctx, yes) ||
           smt_term_is_const(ctx, no))) {
        out = smt_ite(ctx, condition, yes, no);
      }
    }
  }
  free(yes_poly);
  free(no_poly);
  return out;
}

static SmtTerm zero_test(Norm *nm, Poly *p, int split, unsigned depth) {
  SmtCtx *ctx = nm->ctx;
  unsigned w = p->w;
  SmtBits c;
  if (!p->ok) {
    return 0;
  }
  if (poly_is_const(p, &c)) {
    return smt_bool(ctx, smt_bits_is_zero(&c));
  }
  SmtTerm cases = split_on_condition(nm, p, split, depth);
  if (cases) {
    return cases;
  }
  if (split) {
    Poly *saved = malloc(sizeof(Poly));
    *saved = *p;
    for (int round = 0; round < 4 && split_variable(ctx, p); round++) {
    }
    int vanished = poly_is_const(p, &c) && smt_bits_is_zero(&c);
    if (!vanished) {
      *p = *saved;
    }
    free(saved);
    if (vanished) {
      return ctx->bool_true;
    }
  }
  SmtBits minus, constant;
  smt_bits_ones(&minus, w);
  smt_bits_zero(&constant);
  unsigned j = 0;
  for (unsigned i = 0; i < p->n; i++) {
    if (p->m[i].nf == 0) {
      constant = p->m[i].coeff;
    } else {
      p->m[j++] = p->m[i];
    }
  }
  p->n = j;
  qsort(p->m, p->n, sizeof(Mono), mono_qsort);
  if (smt_bits_get(&p->m[0].coeff, w - 1u)) {
    poly_scale(p, &minus);
    smt_bits_neg(&constant, &constant, w);
  }
  SmtTerm pos, neg;
  SmtBits ignored;
  poly_split(ctx, p, &pos, &neg, &ignored);
  if (!pos) {
    return smt_eq(ctx, neg, smt_bv_words(ctx, w, constant.w));
  }
  SmtBits rhs;
  smt_bits_neg(&rhs, &constant, w);
  SmtTerm right = neg ? neg : smt_bv_words(ctx, w, rhs.w);
  SmtTerm left = pos;
  if (neg && !smt_bits_is_zero(&constant)) {
    left = smt_mk2(ctx, SMT_K_BVADD, pos, smt_bv_words(ctx, w, constant.w));
  }
  return smt_eq(ctx, left, right);
}

static SmtTerm normalize_eq(Norm *nm, SmtTerm a, SmtTerm b, int split) {
  SmtCtx *ctx = nm->ctx;
  unsigned w = smt_width(ctx, a);
  Poly *p = malloc(sizeof(Poly));
  Poly *q = malloc(sizeof(Poly));
  poly_of(nm, a, p, 64);
  poly_of(nm, b, q, 64);
  SmtBits minus;
  smt_bits_ones(&minus, w);
  poly_scale(q, &minus);
  poly_add(p, q);
  nm->case_budget = 12u;
  SmtTerm out = zero_test(nm, p, split, 0);
  free(p);
  free(q);
  return out ? out : smt_eq(ctx, a, b);
}

static int child_polarity(SmtCtx *ctx, SmtKind k, SmtTerm t, unsigned i,
                          int pol) {
  switch (k) {
  case SMT_K_NOT:
    return pol == POL_POS ? POL_NEG : pol == POL_NEG ? POL_POS : POL_BOTH;
  case SMT_K_AND:
  case SMT_K_OR:
    return pol;
  case SMT_K_ITE:
    if (i == 0 || !smt_sort_is_bool(ctx, ctx->nodes[t].sort)) {
      return POL_BOTH;
    }
    return pol;
  default:
    return POL_BOTH;
  }
}

static SmtTerm normalize(Norm *nm, SmtTerm t, int pol) {
  SmtCtx *ctx = nm->ctx;
  if (ctx->nodes[t].nargs == 0) {
    return t;
  }
  SmtTerm cached = smt_map_get(&nm->memo[pol], t);
  if (cached) {
    return cached;
  }
  SmtKind k = (SmtKind)ctx->nodes[t].kind;
  unsigned nargs = ctx->nodes[t].nargs;
  uint32_t p0 = ctx->nodes[t].p0, p1 = ctx->nodes[t].p1;
  SmtTerm out = t;
  if (k == SMT_K_FORALL || k == SMT_K_EXISTS || k == SMT_K_APPLY ||
      k == SMT_K_SELECT || k == SMT_K_STORE || nargs > 3u) {
    smt_map_put(&nm->memo[pol], t, t);
    return t;
  }
  SmtTerm args[3];
  int changed = 0;
  for (unsigned i = 0; i < nargs; i++) {
    SmtTerm a = smt_arg(ctx, t, i);
    args[i] = normalize(nm, a, child_polarity(ctx, k, t, i, pol));
    changed |= args[i] != a;
  }
  if (changed) {
    if (k == SMT_K_EXTRACT) {
      out = smt_extract(ctx, args[0], p0, p1);
    } else if (k == SMT_K_ZEXT) {
      out = smt_zext(ctx, args[0], p0);
    } else if (k == SMT_K_SEXT) {
      out = smt_sext(ctx, args[0], p0);
    } else {
      out = smt_mk(ctx, k, nargs, args);
    }
  }
  SmtKind ok = (SmtKind)ctx->nodes[out].kind;
  if (ok == SMT_K_EQ &&
      smt_sort_is_bv(ctx, ctx->nodes[smt_arg(ctx, out, 0)].sort)) {
    out = normalize_eq(nm, smt_arg(ctx, out, 0), smt_arg(ctx, out, 1),
                       pol == POL_NEG);
  } else if (is_arith(ok) && ok != SMT_K_CONST) {
    Poly *p = malloc(sizeof(Poly));
    poly_of(nm, out, p, 64);
    if (p->ok && !(p->n == 1 && p->m[0].nf == 1 &&
                   p->m[0].f[0] == out)) {
      out = poly_build(ctx, p);
    }
    free(p);
  }
  smt_map_put(&nm->memo[pol], t, out);
  return out;
}

static uint64_t node_circuit_size(const SmtCtx *ctx, SmtTerm t) {
  const SmtNode *n = &ctx->nodes[t];
  uint64_t w = smt_sort_is_bv(ctx, n->sort) ? ctx->sorts[n->sort].width : 1u;
  SmtTerm a = n->nargs > 0 ? smt_arg(ctx, t, 0) : 0;
  SmtTerm b = n->nargs > 1 ? smt_arg(ctx, t, 1) : 0;
  uint64_t aw = a && smt_sort_is_bv(ctx, ctx->nodes[a].sort)
                    ? ctx->sorts[ctx->nodes[a].sort].width
                    : 1u;
  switch ((SmtKind)n->kind) {
  case SMT_K_AND:
  case SMT_K_OR:
  case SMT_K_XOR:
    return 1;
  case SMT_K_ITE:
  case SMT_K_BVAND:
  case SMT_K_BVOR:
  case SMT_K_BVXOR:
    return w;
  case SMT_K_EQ:
    return aw + 1u;
  case SMT_K_BVULT:
  case SMT_K_BVULE:
  case SMT_K_BVSLT:
  case SMT_K_BVSLE:
    return 2u * aw;
  case SMT_K_BVNEG:
  case SMT_K_BVADD:
  case SMT_K_BVSUB:
    return 3u * w;
  case SMT_K_BVMUL: {
    SmtTerm k = smt_term_is_const(ctx, a) ? a : smt_term_is_const(ctx, b) ? b : 0;
    if (k) {
      return 3u * w * (uint64_t)popcount_bits(&ctx->consts[ctx->nodes[k].p0]);
    }
    return 3u * w * w;
  }
  case SMT_K_BVUDIV:
  case SMT_K_BVUREM:
  case SMT_K_BVSDIV:
  case SMT_K_BVSREM:
  case SMT_K_BVSMOD:
    return 4u * w * w;
  case SMT_K_BVSHL:
  case SMT_K_BVLSHR:
  case SMT_K_BVASHR:
    return smt_term_is_const(ctx, b) ? 0 : 9u * w;
  default:
    return 0;
  }
}

uint64_t smt_circuit_size(SmtCtx *ctx, SmtTerm root) {
  uint32_t stamp = smt_mark_begin(ctx);
  size_t cap = 1024, n = 0;
  SmtTerm *stack = malloc(cap * sizeof(SmtTerm));
  uint64_t total = 0;
  stack[n++] = root;
  while (n) {
    SmtTerm t = stack[--n];
    if (smt_mark_test_set(ctx, t, stamp)) {
      continue;
    }
    total += node_circuit_size(ctx, t);
    for (unsigned i = 0; i < ctx->nodes[t].nargs; i++) {
      if (n == cap) {
        cap *= 2u;
        stack = realloc(stack, cap * sizeof(SmtTerm));
      }
      stack[n++] = smt_arg(ctx, t, i);
    }
  }
  free(stack);
  return total;
}

SmtTerm smt_normalize(SmtCtx *ctx, SmtTerm t) {
  Norm nm;
  nm.ctx = ctx;
  nm.case_budget = 0;
  for (int i = 0; i < 3; i++) {
    smt_map_init(&nm.memo[i]);
  }
  SmtTerm out = normalize(&nm, t, POL_POS);
  for (int i = 0; i < 3; i++) {
    smt_map_free(&nm.memo[i]);
  }
  return out;
}
