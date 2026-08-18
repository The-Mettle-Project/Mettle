#include "codegen/binary/strength_rules.h"

/* ============================================================================
 * Declarative strength-reduction table.
 *
 * A rule matches `x <op> C` by operator, the SHAPE of the constant, and the
 * signedness of x, then names the rewrite kind. The classifier resolves the
 * kind's parameters (shift amount, mask, magic pair) so a backend only
 * pattern-matches on the kind. Mirrors the IR-level rewrite engine
 * (ir_optimize_rewrite.c): one reduction = one row, and the engine does the
 * matching.
 * ==========================================================================*/

typedef enum {
  SR_SHAPE_POW2,          /* C == 2^k, k >= 1 */
  SR_SHAPE_POW2_PLUS_1,   /* C == 2^k + 1, k >= 2 (3 and 9 stay lea-shaped) */
  SR_SHAPE_POW2_MINUS_1,  /* C == 2^k - 1, k >= 2 */
  SR_SHAPE_CONST_GENERAL  /* any other C with |C| >= 2 (magic candidates) */
} CgShape;

typedef struct {
  char op;
  CgShape shape;
  int signedness; /* -1 = either, 0 = signed only, 1 = unsigned only */
  CgStrengthKind kind;
} CgStrengthRule;

static const CgStrengthRule g_cg_strength_rules[] = {
    {'*', SR_SHAPE_POW2, -1, CG_SR_MUL_SHL},
    {'*', SR_SHAPE_POW2_PLUS_1, -1, CG_SR_MUL_SHL_ADD},
    {'*', SR_SHAPE_POW2_MINUS_1, -1, CG_SR_MUL_SHL_SUB},
    {'/', SR_SHAPE_POW2, 1, CG_SR_UDIV_SHR},
    {'%', SR_SHAPE_POW2, 1, CG_SR_UREM_AND},
    {'/', SR_SHAPE_POW2, 0, CG_SR_SDIV_POW2},
    {'%', SR_SHAPE_POW2, 0, CG_SR_SREM_POW2},
    {'/', SR_SHAPE_CONST_GENERAL, -1, CG_SR_DIV_MAGIC},
    {'%', SR_SHAPE_CONST_GENERAL, -1, CG_SR_REM_MAGIC},
};

static int cg_pow2_log(unsigned long long v) {
  if (v == 0 || (v & (v - 1)) != 0) {
    return -1;
  }
  int k = 0;
  while ((v >> k) != 1) {
    k++;
  }
  return k;
}

static int cg_shape_matches(CgShape shape, long long c, int *k_out) {
  switch (shape) {
  case SR_SHAPE_POW2: {
    int k = c > 1 ? cg_pow2_log((unsigned long long)c) : -1;
    if (k < 1) {
      return 0;
    }
    *k_out = k;
    return 1;
  }
  case SR_SHAPE_POW2_PLUS_1: {
    int k = c > 2 ? cg_pow2_log((unsigned long long)(c - 1)) : -1;
    if (k < 2) {
      return 0; /* 3 = 2+1 is the lea/add form, not worth a shift pair */
    }
    *k_out = k;
    return 1;
  }
  case SR_SHAPE_POW2_MINUS_1: {
    int k = c > 2 ? cg_pow2_log((unsigned long long)(c + 1)) : -1;
    if (k < 2) {
      return 0;
    }
    *k_out = k;
    return 1;
  }
  case SR_SHAPE_CONST_GENERAL:
    /* 0 keeps the divide (the /0 trap must fire); +/-1 is handled by simpler
     * folds; powers of two are claimed by the rows above. */
    if (c == 0 || c == 1 || c == -1) {
      return 0;
    }
    *k_out = 0;
    return 1;
  }
  return 0;
}

int cg_strength_classify(char op, long long c, int is_unsigned,
                         CgStrengthRewrite *out) {
  if (!out) {
    return 0;
  }
  out->kind = CG_SR_NONE;
  /* An unsigned operand with the top bit set is a huge value, not a shape:
   * its "power of two" reading would be wrong under the signed compare
   * below, and no magic is worth it either. */
  if (is_unsigned && c < 0) {
    return 0;
  }

  for (size_t i = 0;
       i < sizeof(g_cg_strength_rules) / sizeof(g_cg_strength_rules[0]); i++) {
    const CgStrengthRule *rule = &g_cg_strength_rules[i];
    int k = 0;
    if (rule->op != op) {
      continue;
    }
    if (rule->signedness >= 0 && rule->signedness != (is_unsigned ? 1 : 0)) {
      continue;
    }
    /* Signed pow2 shapes only claim positive divisors; a negative divisor
     * falls through to the magic rows, whose parameters carry its sign. */
    if (!cg_shape_matches(rule->shape, c, &k)) {
      continue;
    }

    out->kind = rule->kind;
    out->shift = k;
    out->mask = 0;
    out->magic = 0;
    out->magic_add = 0;
    switch (rule->kind) {
    case CG_SR_UREM_AND:
    case CG_SR_SREM_POW2:
      out->mask = c - 1;
      break;
    case CG_SR_DIV_MAGIC:
    case CG_SR_REM_MAGIC:
      if (is_unsigned) {
        uint64_t magic;
        int shift, add;
        cg_magic_u64((uint64_t)c, &magic, &shift, &add);
        out->magic = (long long)magic;
        out->shift = shift;
        out->magic_add = add;
      } else {
        int64_t magic;
        int shift;
        cg_magic_s64(c, &magic, &shift);
        out->magic = magic;
        out->shift = shift;
      }
      break;
    default:
      break;
    }
    return 1;
  }
  return 0;
}

/* Granlund-Montgomery magic for SIGNED 64-bit division (Hacker's Delight,
 * Fig. 10-1, widened to 64-bit). Moved verbatim from mir_lower.c so every
 * backend reads the one implementation. */
void cg_magic_s64(int64_t d, int64_t *Mout, int *sout) {
  const uint64_t two63 = 0x8000000000000000ULL;
  uint64_t ad = (uint64_t)(d < 0 ? -d : d);
  uint64_t t = two63 + ((uint64_t)d >> 63);
  uint64_t anc = t - 1 - t % ad; /* |nc| */
  int p = 63;
  uint64_t q1 = two63 / anc, r1 = two63 - q1 * anc;
  uint64_t q2 = two63 / ad, r2 = two63 - q2 * ad;
  uint64_t delta;
  do {
    p++;
    q1 <<= 1; r1 <<= 1;
    if (r1 >= anc) { q1++; r1 -= anc; }
    q2 <<= 1; r2 <<= 1;
    if (r2 >= ad) { q2++; r2 -= ad; }
    delta = ad - r2;
  } while (q1 < delta || (q1 == delta && r1 == 0));
  int64_t M = (int64_t)(q2 + 1);
  if (d < 0) M = -M;
  *Mout = M;
  *sout = p - 64;
}

/* Magic for UNSIGNED 64-bit division (Hacker's Delight, Fig. 10-3, widened to
 * 64-bit). *addout selects the overflow-safe reconstruction. */
void cg_magic_u64(uint64_t d, uint64_t *Mout, int *sout, int *addout) {
  const uint64_t two63 = 0x8000000000000000ULL;
  *addout = 0;
  uint64_t nc = (uint64_t)(-1) - ((uint64_t)0 - d) % d;
  int p = 63;
  uint64_t q1 = two63 / nc, r1 = two63 - q1 * nc;
  uint64_t q2 = (two63 - 1) / d, r2 = (two63 - 1) - q2 * d;
  uint64_t delta;
  do {
    p++;
    if (r1 >= nc - r1) { q1 = 2 * q1 + 1; r1 = 2 * r1 - nc; }
    else { q1 = 2 * q1; r1 = 2 * r1; }
    if (r2 + 1 >= d - r2) {
      if (q2 >= two63 - 1) *addout = 1;
      q2 = 2 * q2 + 1; r2 = 2 * r2 + 1 - d;
    } else {
      if (q2 >= two63) *addout = 1;
      q2 = 2 * q2; r2 = 2 * r2 + 1;
    }
    delta = d - 1 - r2;
  } while (p < 128 && (q1 < delta || (q1 == delta && r1 == 0)));
  *Mout = q2 + 1;
  *sout = p - 64;
}
