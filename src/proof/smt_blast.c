#include "smt_blast.h"
#include <stdlib.h>
#include <string.h>

enum { GATE_AND = 1, GATE_XOR = 2, GATE_MUX = 3 };

#define VW (SMT_MAX_WIDTH + 1u)

static void clause2(Blaster *b, SatLit x, SatLit y) {
  SatLit c[2] = {x, y};
  sat_add_clause(b->sat, c, 2);
}

static void clause3(Blaster *b, SatLit x, SatLit y, SatLit z) {
  SatLit c[3] = {x, y, z};
  sat_add_clause(b->sat, c, 3);
}

void blast_init(Blaster *b, SmtCtx *ctx, SatSolver *sat) {
  memset(b, 0, sizeof(*b));
  b->ctx = ctx;
  b->sat = sat;
  uint32_t v = sat_new_var(sat);
  b->lit_true = SAT_LIT(v, 0);
  b->lit_false = SAT_LIT(v, 1);
  sat_add_clause(sat, &b->lit_true, 1);
  b->gate_bucket_cap = 4096;
  b->gate_buckets = malloc(b->gate_bucket_cap * sizeof(uint32_t));
  memset(b->gate_buckets, 0xff, b->gate_bucket_cap * sizeof(uint32_t));
}

void blast_free(Blaster *b) {
  free(b->offset);
  free(b->pool);
  free(b->gates);
  free(b->gate_buckets);
  free(b->stack);
  memset(b, 0, sizeof(*b));
}

static uint32_t gate_hash(uint32_t kind, SatLit a, SatLit x, SatLit y) {
  uint64_t h = kind * 0x9E3779B97F4A7C15ULL;
  h ^= (uint64_t)a * 0xC2B2AE3D27D4EB4FULL;
  h ^= (uint64_t)x * 0x165667B19E3779F9ULL;
  h ^= (uint64_t)y * 0x27D4EB2F165667C5ULL;
  return (uint32_t)(h ^ (h >> 29));
}

static SatLit gate_lookup(Blaster *b, uint32_t kind, SatLit a, SatLit x,
                          SatLit y, int *found) {
  uint32_t mask = b->gate_bucket_cap - 1u;
  uint32_t slot = gate_hash(kind, a, x, y) & mask;
  while (b->gate_buckets[slot] != BLAST_NONE) {
    BlastGate *g = &b->gates[b->gate_buckets[slot]];
    if (g->kind == kind && g->a == a && g->b == x && g->c == y) {
      *found = 1;
      return g->out;
    }
    slot = (slot + 1u) & mask;
  }
  *found = 0;
  return 0;
}

static void gate_insert(Blaster *b, uint32_t kind, SatLit a, SatLit x,
                        SatLit y, SatLit out) {
  if ((b->gate_count + 1u) * 2u > b->gate_bucket_cap) {
    uint32_t cap = b->gate_bucket_cap * 2u;
    uint32_t *buckets = malloc(cap * sizeof(uint32_t));
    memset(buckets, 0xff, cap * sizeof(uint32_t));
    for (uint32_t i = 0; i < b->gate_count; i++) {
      BlastGate *g = &b->gates[i];
      uint32_t slot = gate_hash(g->kind, g->a, g->b, g->c) & (cap - 1u);
      while (buckets[slot] != BLAST_NONE) {
        slot = (slot + 1u) & (cap - 1u);
      }
      buckets[slot] = i;
    }
    free(b->gate_buckets);
    b->gate_buckets = buckets;
    b->gate_bucket_cap = cap;
  }
  if (b->gate_count == b->gate_cap) {
    b->gate_cap = b->gate_cap ? b->gate_cap * 2u : 4096u;
    b->gates = realloc(b->gates, b->gate_cap * sizeof(BlastGate));
  }
  uint32_t id = b->gate_count++;
  BlastGate *g = &b->gates[id];
  g->kind = kind;
  g->a = a;
  g->b = x;
  g->c = y;
  g->out = out;
  uint32_t slot = gate_hash(kind, a, x, y) & (b->gate_bucket_cap - 1u);
  while (b->gate_buckets[slot] != BLAST_NONE) {
    slot = (slot + 1u) & (b->gate_bucket_cap - 1u);
  }
  b->gate_buckets[slot] = id;
}

static SatLit new_lit(Blaster *b) {
  return SAT_LIT(sat_new_var(b->sat), 0);
}

static SatLit g_and(Blaster *b, SatLit x, SatLit y) {
  if (x == b->lit_false || y == b->lit_false || x == SAT_NEG(y)) {
    return b->lit_false;
  }
  if (x == b->lit_true) {
    return y;
  }
  if (y == b->lit_true || x == y) {
    return x;
  }
  if (x > y) {
    SatLit t = x;
    x = y;
    y = t;
  }
  int found;
  SatLit out = gate_lookup(b, GATE_AND, x, y, 0, &found);
  if (found) {
    return out;
  }
  out = new_lit(b);
  clause2(b, SAT_NEG(out), x);
  clause2(b, SAT_NEG(out), y);
  clause3(b, out, SAT_NEG(x), SAT_NEG(y));
  gate_insert(b, GATE_AND, x, y, 0, out);
  return out;
}

static SatLit g_or(Blaster *b, SatLit x, SatLit y) {
  return SAT_NEG(g_and(b, SAT_NEG(x), SAT_NEG(y)));
}

static SatLit g_xor(Blaster *b, SatLit x, SatLit y) {
  if (x == b->lit_false) {
    return y;
  }
  if (y == b->lit_false) {
    return x;
  }
  if (x == b->lit_true) {
    return SAT_NEG(y);
  }
  if (y == b->lit_true) {
    return SAT_NEG(x);
  }
  if (x == y) {
    return b->lit_false;
  }
  if (x == SAT_NEG(y)) {
    return b->lit_true;
  }
  uint32_t parity = SAT_SIGN(x) ^ SAT_SIGN(y);
  x &= ~1u;
  y &= ~1u;
  if (x > y) {
    SatLit t = x;
    x = y;
    y = t;
  }
  int found;
  SatLit out = gate_lookup(b, GATE_XOR, x, y, 0, &found);
  if (!found) {
    out = new_lit(b);
    clause3(b, SAT_NEG(out), x, y);
    clause3(b, SAT_NEG(out), SAT_NEG(x), SAT_NEG(y));
    clause3(b, out, SAT_NEG(x), y);
    clause3(b, out, x, SAT_NEG(y));
    gate_insert(b, GATE_XOR, x, y, 0, out);
  }
  return parity ? SAT_NEG(out) : out;
}

static SatLit g_mux(Blaster *b, SatLit c, SatLit t, SatLit e) {
  if (c == b->lit_true || t == e) {
    return t;
  }
  if (c == b->lit_false) {
    return e;
  }
  if (t == SAT_NEG(e)) {
    return SAT_NEG(g_xor(b, c, t));
  }
  if (t == b->lit_true || t == c) {
    return g_or(b, c, e);
  }
  if (t == b->lit_false || t == SAT_NEG(c)) {
    return g_and(b, SAT_NEG(c), e);
  }
  if (e == b->lit_true || e == SAT_NEG(c)) {
    return g_or(b, SAT_NEG(c), t);
  }
  if (e == b->lit_false || e == c) {
    return g_and(b, c, t);
  }
  if (SAT_SIGN(c)) {
    c = SAT_NEG(c);
    SatLit tmp = t;
    t = e;
    e = tmp;
  }
  int found;
  SatLit out = gate_lookup(b, GATE_MUX, c, t, e, &found);
  if (found) {
    return out;
  }
  out = new_lit(b);
  clause3(b, SAT_NEG(c), SAT_NEG(t), out);
  clause3(b, SAT_NEG(c), t, SAT_NEG(out));
  clause3(b, c, SAT_NEG(e), out);
  clause3(b, c, e, SAT_NEG(out));
  clause3(b, SAT_NEG(t), SAT_NEG(e), out);
  clause3(b, t, e, SAT_NEG(out));
  gate_insert(b, GATE_MUX, c, t, e, out);
  return out;
}

static void v_add(Blaster *b, SatLit *r, const SatLit *x, const SatLit *y,
                  unsigned w, SatLit carry, SatLit *carry_out) {
  for (unsigned i = 0; i < w; i++) {
    SatLit p = g_xor(b, x[i], y[i]);
    SatLit s = g_xor(b, p, carry);
    SatLit c = g_or(b, g_and(b, x[i], y[i]), g_and(b, p, carry));
    r[i] = s;
    carry = c;
  }
  if (carry_out) {
    *carry_out = carry;
  }
}

static void v_not(Blaster *b, SatLit *r, const SatLit *x, unsigned w) {
  (void)b;
  for (unsigned i = 0; i < w; i++) {
    r[i] = SAT_NEG(x[i]);
  }
}

static void v_sub(Blaster *b, SatLit *r, const SatLit *x, const SatLit *y,
                  unsigned w, SatLit *no_borrow) {
  SatLit ny[VW];
  v_not(b, ny, y, w);
  v_add(b, r, x, ny, w, b->lit_true, no_borrow);
}

static void v_neg(Blaster *b, SatLit *r, const SatLit *x, unsigned w) {
  SatLit zero[VW];
  for (unsigned i = 0; i < w; i++) {
    zero[i] = b->lit_false;
  }
  v_sub(b, r, zero, x, w, NULL);
}

static void v_mux(Blaster *b, SatLit *r, SatLit c, const SatLit *t,
                  const SatLit *e, unsigned w) {
  for (unsigned i = 0; i < w; i++) {
    r[i] = g_mux(b, c, t[i], e[i]);
  }
}

static SatLit v_ult(Blaster *b, const SatLit *x, const SatLit *y, unsigned w) {
  SatLit lt = b->lit_false;
  for (unsigned i = 0; i < w; i++) {
    SatLit less_here = g_and(b, SAT_NEG(x[i]), y[i]);
    SatLit same = SAT_NEG(g_xor(b, x[i], y[i]));
    lt = g_or(b, less_here, g_and(b, same, lt));
  }
  return lt;
}

static SatLit v_slt(Blaster *b, const SatLit *x, const SatLit *y, unsigned w) {
  SatLit xs[VW], ys[VW];
  memcpy(xs, x, w * sizeof(SatLit));
  memcpy(ys, y, w * sizeof(SatLit));
  xs[w - 1u] = SAT_NEG(x[w - 1u]);
  ys[w - 1u] = SAT_NEG(y[w - 1u]);
  return v_ult(b, xs, ys, w);
}

static SatLit v_eq(Blaster *b, const SatLit *x, const SatLit *y, unsigned w) {
  SatLit acc = b->lit_true;
  for (unsigned i = 0; i < w; i++) {
    acc = g_and(b, acc, SAT_NEG(g_xor(b, x[i], y[i])));
  }
  return acc;
}

static int v_is_const(const Blaster *b, const SatLit *x, unsigned w) {
  for (unsigned i = 0; i < w; i++) {
    if (x[i] != b->lit_true && x[i] != b->lit_false) {
      return 0;
    }
  }
  return 1;
}

static unsigned v_const_ones(const Blaster *b, const SatLit *x, unsigned w) {
  unsigned n = 0;
  for (unsigned i = 0; i < w; i++) {
    n += x[i] == b->lit_true;
  }
  return n;
}

static void v_mul(Blaster *b, SatLit *r, const SatLit *x, const SatLit *y,
                  unsigned w) {
  if (v_is_const(b, x, w) &&
      (!v_is_const(b, y, w) || v_const_ones(b, x, w) < v_const_ones(b, y, w))) {
    const SatLit *t = x;
    x = y;
    y = t;
  }
  SatLit acc[VW], row[VW], tmp[VW];
  for (unsigned i = 0; i < w; i++) {
    acc[i] = b->lit_false;
  }
  for (unsigned i = 0; i < w; i++) {
    if (y[i] == b->lit_false) {
      continue;
    }
    for (unsigned j = 0; j < w; j++) {
      row[j] = j < i ? b->lit_false : g_and(b, x[j - i], y[i]);
    }
    v_add(b, tmp + i, acc + i, row + i, w - i, b->lit_false, NULL);
    for (unsigned j = i; j < w; j++) {
      acc[j] = tmp[j];
    }
  }
  memcpy(r, acc, w * sizeof(SatLit));
}

static void v_udivrem(Blaster *b, SatLit *q, SatLit *rem_out, const SatLit *x,
                      const SatLit *y, unsigned w) {
  SatLit rem[VW], ye[VW], diff[VW], quot[VW];
  for (unsigned i = 0; i <= w; i++) {
    rem[i] = b->lit_false;
  }
  memcpy(ye, y, w * sizeof(SatLit));
  ye[w] = b->lit_false;
  for (int i = (int)w - 1; i >= 0; i--) {
    for (unsigned k = w; k > 0; k--) {
      rem[k] = rem[k - 1u];
    }
    rem[0] = x[i];
    SatLit no_borrow;
    v_sub(b, diff, rem, ye, w + 1u, &no_borrow);
    quot[i] = no_borrow;
    v_mux(b, rem, no_borrow, diff, rem, w + 1u);
  }
  if (q) {
    memcpy(q, quot, w * sizeof(SatLit));
  }
  if (rem_out) {
    memcpy(rem_out, rem, w * sizeof(SatLit));
  }
}

static void v_shift(Blaster *b, SatLit *r, const SatLit *x, const SatLit *s,
                    unsigned w, int kind) {
  SatLit cur[VW], next[VW], fill;
  if (w == 0 || w > SMT_MAX_WIDTH) {
    return;
  }
  memcpy(cur, x, w * sizeof(SatLit));
  fill = kind == SMT_K_BVASHR ? x[w - 1u] : b->lit_false;
  unsigned stages = 0;
  while ((1u << stages) < w) {
    stages++;
  }
  for (unsigned k = 0; k < stages && k < w; k++) {
    unsigned amount = 1u << k;
    for (unsigned i = 0; i < w; i++) {
      SatLit shifted;
      if (kind == SMT_K_BVSHL) {
        shifted = i >= amount ? cur[i - amount] : b->lit_false;
      } else {
        shifted = i + amount < w ? cur[i + amount] : fill;
      }
      next[i] = g_mux(b, s[k], shifted, cur[i]);
    }
    memcpy(cur, next, w * sizeof(SatLit));
  }
  SatLit wbits[VW];
  for (unsigned i = 0; i < w; i++) {
    wbits[i] = (i < 32u && ((w >> i) & 1u)) ? b->lit_true : b->lit_false;
  }
  SatLit in_range = v_ult(b, s, wbits, w);
  for (unsigned i = 0; i < w; i++) {
    r[i] = g_mux(b, in_range, cur[i], fill);
  }
}

static void v_signed_divrem(Blaster *b, SatLit *r, const SatLit *x,
                            const SatLit *y, unsigned w, SmtKind kind) {
  if (w == 0 || w > SMT_MAX_WIDTH) {
    return;
  }
  SatLit sx = x[w - 1u], sy = y[w - 1u];
  SatLit nx[VW], ny[VW], ax[VW], ay[VW], q[VW], rem[VW], tmp[VW];
  v_neg(b, nx, x, w);
  v_neg(b, ny, y, w);
  v_mux(b, ax, sx, nx, x, w);
  v_mux(b, ay, sy, ny, y, w);
  v_udivrem(b, q, rem, ax, ay, w);
  if (kind == SMT_K_BVSDIV) {
    v_neg(b, tmp, q, w);
    v_mux(b, r, g_xor(b, sx, sy), tmp, q, w);
    return;
  }
  if (kind == SMT_K_BVSREM) {
    v_neg(b, tmp, rem, w);
    v_mux(b, r, sx, tmp, rem, w);
    return;
  }
  SatLit zero[VW], neg_rem[VW], a1[VW], a2[VW], pick[VW];
  for (unsigned i = 0; i < w; i++) {
    zero[i] = b->lit_false;
  }
  SatLit rem_zero = v_eq(b, rem, zero, w);
  v_neg(b, neg_rem, rem, w);
  v_add(b, a1, neg_rem, y, w, b->lit_false, NULL);
  v_add(b, a2, rem, y, w, b->lit_false, NULL);
  for (unsigned i = 0; i < w; i++) {
    SatLit both_pos = rem[i];
    SatLit neg_pos = a1[i];
    SatLit pos_neg = a2[i];
    SatLit both_neg = neg_rem[i];
    SatLit when_sx = g_mux(b, sy, both_neg, neg_pos);
    SatLit when_not_sx = g_mux(b, sy, pos_neg, both_pos);
    pick[i] = g_mux(b, sx, when_sx, when_not_sx);
  }
  v_mux(b, r, rem_zero, rem, pick, w);
}

static uint32_t pool_alloc(Blaster *b, unsigned w) {
  if (b->pool_count + w > b->pool_cap) {
    size_t cap = b->pool_cap ? b->pool_cap : 4096u;
    while (cap < b->pool_count + w) {
      cap *= 2u;
    }
    b->pool = realloc(b->pool, cap * sizeof(SatLit));
    b->pool_cap = cap;
  }
  uint32_t off = (uint32_t)b->pool_count;
  b->pool_count += w;
  return off;
}

static void ensure_offset(Blaster *b, SmtTerm t) {
  if (t < b->offset_cap) {
    return;
  }
  uint32_t old = b->offset_cap;
  uint32_t cap = old ? old : 1024u;
  while (cap <= t || cap < b->ctx->node_count) {
    cap *= 2u;
  }
  b->offset = realloc(b->offset, cap * sizeof(uint32_t));
  memset(b->offset + old, 0xff, (cap - old) * sizeof(uint32_t));
  b->offset_cap = cap;
}

const SatLit *blast_bits(const Blaster *b, SmtTerm t) {
  if (t >= b->offset_cap || b->offset[t] == BLAST_NONE) {
    return NULL;
  }
  return &b->pool[b->offset[t]];
}

static unsigned term_bits(const SmtCtx *ctx, SmtTerm t) {
  const SmtSortInfo *s = &ctx->sorts[ctx->nodes[t].sort];
  return s->kind == SMT_SORT_BV ? s->width : 1u;
}

static void blast_fail(Blaster *b, const char *why) {
  if (!b->failed) {
    b->failed = 1;
    b->fail_reason = why;
  }
}

static void blast_node(Blaster *b, SmtTerm t) {
  SmtCtx *ctx = b->ctx;
  const SmtNode *n = &ctx->nodes[t];
  SmtKind k = (SmtKind)n->kind;
  unsigned w = term_bits(ctx, t);
  SatLit r[VW], x[VW], y[VW];
  unsigned xw = 0, yw = 0;
  if (n->nargs > 0 && k != SMT_K_ITE) {
    SmtTerm a0 = smt_arg(ctx, t, 0);
    if (smt_sort_is_array(ctx, ctx->nodes[a0].sort)) {
      blast_fail(b, "array term reached the bit-blaster");
      return;
    }
    xw = term_bits(ctx, a0);
    memcpy(x, &b->pool[b->offset[a0]], xw * sizeof(SatLit));
  }
  if (n->nargs > 1 && k != SMT_K_ITE) {
    SmtTerm a1 = smt_arg(ctx, t, 1);
    yw = term_bits(ctx, a1);
    memcpy(y, &b->pool[b->offset[a1]], yw * sizeof(SatLit));
  }
  switch (k) {
  case SMT_K_TRUE:
    r[0] = b->lit_true;
    break;
  case SMT_K_FALSE:
    r[0] = b->lit_false;
    break;
  case SMT_K_CONST: {
    const SmtBits *bits = &ctx->consts[n->p0];
    for (unsigned i = 0; i < w; i++) {
      r[i] = smt_bits_get(bits, i) ? b->lit_true : b->lit_false;
    }
    break;
  }
  case SMT_K_VAR:
    for (unsigned i = 0; i < w; i++) {
      r[i] = new_lit(b);
    }
    break;
  case SMT_K_NOT:
    r[0] = SAT_NEG(x[0]);
    break;
  case SMT_K_AND:
    r[0] = g_and(b, x[0], y[0]);
    break;
  case SMT_K_OR:
    r[0] = g_or(b, x[0], y[0]);
    break;
  case SMT_K_XOR:
    r[0] = g_xor(b, x[0], y[0]);
    break;
  case SMT_K_EQ:
    r[0] = v_eq(b, x, y, xw);
    break;
  case SMT_K_ITE: {
    SmtTerm c = smt_arg(ctx, t, 0), a1 = smt_arg(ctx, t, 1),
            a2 = smt_arg(ctx, t, 2);
    if (smt_sort_is_array(ctx, n->sort)) {
      blast_fail(b, "array term reached the bit-blaster");
      return;
    }
    SatLit cl = b->pool[b->offset[c]];
    memcpy(x, &b->pool[b->offset[a1]], w * sizeof(SatLit));
    memcpy(y, &b->pool[b->offset[a2]], w * sizeof(SatLit));
    v_mux(b, r, cl, x, y, w);
    break;
  }
  case SMT_K_BVNOT:
    v_not(b, r, x, w);
    break;
  case SMT_K_BVNEG:
    v_neg(b, r, x, w);
    break;
  case SMT_K_BVAND:
    for (unsigned i = 0; i < w; i++) {
      r[i] = g_and(b, x[i], y[i]);
    }
    break;
  case SMT_K_BVOR:
    for (unsigned i = 0; i < w; i++) {
      r[i] = g_or(b, x[i], y[i]);
    }
    break;
  case SMT_K_BVXOR:
    for (unsigned i = 0; i < w; i++) {
      r[i] = g_xor(b, x[i], y[i]);
    }
    break;
  case SMT_K_BVADD:
    v_add(b, r, x, y, w, b->lit_false, NULL);
    break;
  case SMT_K_BVSUB:
    v_sub(b, r, x, y, w, NULL);
    break;
  case SMT_K_BVMUL:
    v_mul(b, r, x, y, w);
    break;
  case SMT_K_BVUDIV:
    v_udivrem(b, r, NULL, x, y, w);
    break;
  case SMT_K_BVUREM:
    v_udivrem(b, NULL, r, x, y, w);
    break;
  case SMT_K_BVSDIV:
  case SMT_K_BVSREM:
  case SMT_K_BVSMOD:
    v_signed_divrem(b, r, x, y, w, k);
    break;
  case SMT_K_BVSHL:
  case SMT_K_BVLSHR:
  case SMT_K_BVASHR:
    v_shift(b, r, x, y, w, k);
    break;
  case SMT_K_CONCAT:
    memcpy(r, y, yw * sizeof(SatLit));
    memcpy(r + yw, x, xw * sizeof(SatLit));
    break;
  case SMT_K_EXTRACT:
    memcpy(r, x + n->p1, w * sizeof(SatLit));
    break;
  case SMT_K_ZEXT:
  case SMT_K_SEXT:
    memcpy(r, x, xw * sizeof(SatLit));
    for (unsigned i = xw; i < w; i++) {
      r[i] = k == SMT_K_ZEXT ? b->lit_false : x[xw - 1u];
    }
    break;
  case SMT_K_BVULT:
    r[0] = v_ult(b, x, y, xw);
    break;
  case SMT_K_BVULE:
    r[0] = SAT_NEG(v_ult(b, y, x, xw));
    break;
  case SMT_K_BVSLT:
    r[0] = v_slt(b, x, y, xw);
    break;
  case SMT_K_BVSLE:
    r[0] = SAT_NEG(v_slt(b, y, x, xw));
    break;
  default:
    blast_fail(b, "term kind cannot be bit-blasted");
    return;
  }
  uint32_t off = pool_alloc(b, w);
  memcpy(&b->pool[off], r, w * sizeof(SatLit));
  b->offset[t] = off;
}

static void stack_push(Blaster *b, uint32_t x) {
  if (b->stack_count == b->stack_cap) {
    b->stack_cap = b->stack_cap ? b->stack_cap * 2u : 1024u;
    b->stack = realloc(b->stack, b->stack_cap * sizeof(uint32_t));
  }
  b->stack[b->stack_count++] = x;
}

int blast_term(Blaster *b, SmtTerm root) {
  SmtCtx *ctx = b->ctx;
  ensure_offset(b, ctx->node_count);
  if (b->offset[root] != BLAST_NONE) {
    return !b->failed;
  }
  b->stack_count = 0;
  stack_push(b, root);
  while (b->stack_count && !b->failed) {
    SmtTerm t = b->stack[b->stack_count - 1u];
    if (b->offset[t] != BLAST_NONE) {
      b->stack_count--;
      continue;
    }
    const SmtNode *n = &ctx->nodes[t];
    if (n->kind == SMT_K_FORALL || n->kind == SMT_K_EXISTS ||
        n->kind == SMT_K_SELECT || n->kind == SMT_K_STORE ||
        n->kind == SMT_K_APPLY) {
      blast_fail(b, "quantifier, array or function term reached the "
                    "bit-blaster");
      break;
    }
    int ready = 1;
    for (unsigned i = n->nargs; i-- > 0;) {
      SmtTerm a = smt_arg(ctx, t, i);
      if (b->offset[a] == BLAST_NONE) {
        stack_push(b, a);
        ready = 0;
      }
    }
    if (ready) {
      b->stack_count--;
      blast_node(b, t);
    }
  }
  return !b->failed;
}

void blast_assert(Blaster *b, SmtTerm formula) {
  SmtCtx *ctx = b->ctx;
  if (ctx->nodes[formula].kind == SMT_K_AND) {
    blast_assert(b, smt_arg(ctx, formula, 0));
    blast_assert(b, smt_arg(ctx, formula, 1));
    return;
  }
  if (ctx->nodes[formula].kind == SMT_K_NOT) {
    SmtTerm inner = smt_arg(ctx, formula, 0);
    if (ctx->nodes[inner].kind == SMT_K_OR) {
      blast_assert(b, smt_not(ctx, smt_arg(ctx, inner, 0)));
      blast_assert(b, smt_not(ctx, smt_arg(ctx, inner, 1)));
      return;
    }
  }
  if (!blast_term(b, formula)) {
    return;
  }
  SatLit l = b->pool[b->offset[formula]];
  sat_add_clause(b->sat, &l, 1);
}
