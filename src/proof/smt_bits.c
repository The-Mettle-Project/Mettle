#include "smt_internal.h"
#include "../common.h"
#include <string.h>

double smt_now_seconds(void) { return mettle_now_ms() / 1000.0; }

void smt_bits_zero(SmtBits *b) { memset(b, 0, sizeof(*b)); }

void smt_bits_mask(SmtBits *b, unsigned width) {
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    unsigned lo = i * 64u;
    if (lo >= width) {
      b->w[i] = 0;
    } else if (width - lo < 64u) {
      b->w[i] &= (((uint64_t)1) << (width - lo)) - 1u;
    }
  }
}

void smt_bits_from_u64(SmtBits *b, uint64_t v, unsigned width) {
  smt_bits_zero(b);
  b->w[0] = v;
  smt_bits_mask(b, width);
}

int smt_bits_eq(const SmtBits *a, const SmtBits *b) {
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    if (a->w[i] != b->w[i]) {
      return 0;
    }
  }
  return 1;
}

int smt_bits_is_zero(const SmtBits *a) {
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    if (a->w[i]) {
      return 0;
    }
  }
  return 1;
}

int smt_bits_get(const SmtBits *a, unsigned i) {
  return (int)((a->w[i >> 6] >> (i & 63u)) & 1u);
}

void smt_bits_set(SmtBits *a, unsigned i, int v) {
  uint64_t bit = ((uint64_t)1) << (i & 63u);
  if (v) {
    a->w[i >> 6] |= bit;
  } else {
    a->w[i >> 6] &= ~bit;
  }
}

void smt_bits_ones(SmtBits *b, unsigned width) {
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    b->w[i] = ~(uint64_t)0;
  }
  smt_bits_mask(b, width);
}

int smt_bits_ult(const SmtBits *a, const SmtBits *b) {
  for (int i = (int)SMT_WORDS - 1; i >= 0; i--) {
    if (a->w[i] != b->w[i]) {
      return a->w[i] < b->w[i];
    }
  }
  return 0;
}

int smt_bits_slt(const SmtBits *a, const SmtBits *b, unsigned width) {
  int sa = smt_bits_get(a, width - 1u);
  int sb = smt_bits_get(b, width - 1u);
  if (sa != sb) {
    return sa;
  }
  return smt_bits_ult(a, b);
}

void smt_bits_add(SmtBits *r, const SmtBits *a, const SmtBits *b,
                  unsigned width) {
  uint64_t carry = 0;
  SmtBits out;
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    uint64_t s = a->w[i] + b->w[i];
    uint64_t c1 = s < a->w[i];
    uint64_t t = s + carry;
    uint64_t c2 = t < s;
    out.w[i] = t;
    carry = c1 | c2;
  }
  smt_bits_mask(&out, width);
  *r = out;
}

void smt_bits_not(SmtBits *r, const SmtBits *a, unsigned width) {
  SmtBits out;
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    out.w[i] = ~a->w[i];
  }
  smt_bits_mask(&out, width);
  *r = out;
}

void smt_bits_neg(SmtBits *r, const SmtBits *a, unsigned width) {
  SmtBits one, inv;
  smt_bits_not(&inv, a, width);
  smt_bits_from_u64(&one, 1, width);
  smt_bits_add(r, &inv, &one, width);
}

void smt_bits_sub(SmtBits *r, const SmtBits *a, const SmtBits *b,
                  unsigned width) {
  SmtBits nb;
  smt_bits_neg(&nb, b, width);
  smt_bits_add(r, a, &nb, width);
}

void smt_bits_mul(SmtBits *r, const SmtBits *a, const SmtBits *b,
                  unsigned width) {
  uint64_t out[SMT_WORDS] = {0, 0, 0, 0};
  for (unsigned i = 0; i < SMT_WORDS; i++) {
    unsigned __int128 carry = 0;
    if (!a->w[i]) {
      continue;
    }
    for (unsigned j = 0; i + j < SMT_WORDS; j++) {
      unsigned __int128 p =
          (unsigned __int128)a->w[i] * b->w[j] + out[i + j] + carry;
      out[i + j] = (uint64_t)p;
      carry = p >> 64;
    }
  }
  memcpy(r->w, out, sizeof(out));
  smt_bits_mask(r, width);
}

static unsigned smt_bits_top(const SmtBits *a) {
  for (int i = (int)SMT_WORDS - 1; i >= 0; i--) {
    if (a->w[i]) {
      return (unsigned)i * 64u + 64u - (unsigned)__builtin_clzll(a->w[i]);
    }
  }
  return 0;
}

static void smt_bits_shl1(SmtBits *a) {
  for (int i = (int)SMT_WORDS - 1; i > 0; i--) {
    a->w[i] = (a->w[i] << 1) | (a->w[i - 1] >> 63);
  }
  a->w[0] <<= 1;
}

void smt_bits_udivrem(SmtBits *q, SmtBits *r, const SmtBits *a,
                      const SmtBits *b, unsigned width) {
  SmtBits quotient, remainder;
  if (smt_bits_is_zero(b)) {
    if (q) {
      smt_bits_ones(q, width);
    }
    if (r) {
      *r = *a;
    }
    return;
  }
  if (width <= 64u) {
    uint64_t x = a->w[0], y = b->w[0];
    if (q) {
      smt_bits_from_u64(q, x / y, width);
    }
    if (r) {
      smt_bits_from_u64(r, x % y, width);
    }
    return;
  }
  smt_bits_zero(&quotient);
  smt_bits_zero(&remainder);
  unsigned top = smt_bits_top(a);
  for (int i = (int)top - 1; i >= 0; i--) {
    smt_bits_shl1(&remainder);
    if (smt_bits_get(a, (unsigned)i)) {
      remainder.w[0] |= 1u;
    }
    if (!smt_bits_ult(&remainder, b)) {
      smt_bits_sub(&remainder, &remainder, b, SMT_MAX_WIDTH);
      smt_bits_set(&quotient, (unsigned)i, 1);
    }
  }
  if (q) {
    *q = quotient;
  }
  if (r) {
    *r = remainder;
  }
}

static unsigned smt_bits_shift_amount(const SmtBits *s, unsigned width) {
  for (unsigned i = 1; i < SMT_WORDS; i++) {
    if (s->w[i]) {
      return width;
    }
  }
  return s->w[0] >= width ? width : (unsigned)s->w[0];
}

void smt_bits_shl(SmtBits *r, const SmtBits *a, const SmtBits *s,
                  unsigned width) {
  unsigned k = smt_bits_shift_amount(s, width);
  SmtBits out;
  smt_bits_zero(&out);
  for (unsigned i = k; i < width; i++) {
    if (smt_bits_get(a, i - k)) {
      smt_bits_set(&out, i, 1);
    }
  }
  *r = out;
}

void smt_bits_lshr(SmtBits *r, const SmtBits *a, const SmtBits *s,
                   unsigned width) {
  unsigned k = smt_bits_shift_amount(s, width);
  SmtBits out;
  smt_bits_zero(&out);
  for (unsigned i = 0; i + k < width; i++) {
    if (smt_bits_get(a, i + k)) {
      smt_bits_set(&out, i, 1);
    }
  }
  *r = out;
}

void smt_bits_ashr(SmtBits *r, const SmtBits *a, const SmtBits *s,
                   unsigned width) {
  unsigned k = smt_bits_shift_amount(s, width);
  int sign = smt_bits_get(a, width - 1u);
  SmtBits out;
  smt_bits_zero(&out);
  for (unsigned i = 0; i < width; i++) {
    int v = i + k < width ? smt_bits_get(a, i + k) : sign;
    if (v) {
      smt_bits_set(&out, i, 1);
    }
  }
  *r = out;
}

static void smt_bits_signed_divrem(SmtKind kind, SmtBits *r, const SmtBits *a,
                                   const SmtBits *b, unsigned width) {
  int sa = smt_bits_get(a, width - 1u);
  int sb = smt_bits_get(b, width - 1u);
  SmtBits ua = *a, ub = *b, q, rem;
  if (sa) {
    smt_bits_neg(&ua, a, width);
  }
  if (sb) {
    smt_bits_neg(&ub, b, width);
  }
  smt_bits_udivrem(&q, &rem, &ua, &ub, width);
  if (kind == SMT_K_BVSDIV) {
    if (sa != sb) {
      smt_bits_neg(&q, &q, width);
    }
    *r = q;
    return;
  }
  if (kind == SMT_K_BVSREM) {
    if (sa) {
      smt_bits_neg(&rem, &rem, width);
    }
    *r = rem;
    return;
  }
  if (smt_bits_is_zero(&rem)) {
    *r = rem;
    return;
  }
  if (!sa && !sb) {
    *r = rem;
  } else if (sa && !sb) {
    smt_bits_neg(&rem, &rem, width);
    smt_bits_add(r, &rem, b, width);
  } else if (!sa && sb) {
    smt_bits_add(r, &rem, b, width);
  } else {
    smt_bits_neg(r, &rem, width);
  }
}

int smt_bits_apply(SmtKind kind, SmtBits *r, const SmtBits *a,
                   const SmtBits *b, unsigned width, uint32_t p0,
                   uint32_t p1, unsigned bwidth) {
  SmtBits out;
  smt_bits_zero(&out);
  switch (kind) {
  case SMT_K_BVNOT:
    smt_bits_not(&out, a, width);
    break;
  case SMT_K_BVNEG:
    smt_bits_neg(&out, a, width);
    break;
  case SMT_K_BVAND:
    for (unsigned i = 0; i < SMT_WORDS; i++) {
      out.w[i] = a->w[i] & b->w[i];
    }
    break;
  case SMT_K_BVOR:
    for (unsigned i = 0; i < SMT_WORDS; i++) {
      out.w[i] = a->w[i] | b->w[i];
    }
    break;
  case SMT_K_BVXOR:
    for (unsigned i = 0; i < SMT_WORDS; i++) {
      out.w[i] = a->w[i] ^ b->w[i];
    }
    break;
  case SMT_K_BVADD:
    smt_bits_add(&out, a, b, width);
    break;
  case SMT_K_BVSUB:
    smt_bits_sub(&out, a, b, width);
    break;
  case SMT_K_BVMUL:
    smt_bits_mul(&out, a, b, width);
    break;
  case SMT_K_BVUDIV:
    smt_bits_udivrem(&out, NULL, a, b, width);
    break;
  case SMT_K_BVUREM:
    smt_bits_udivrem(NULL, &out, a, b, width);
    break;
  case SMT_K_BVSDIV:
  case SMT_K_BVSREM:
  case SMT_K_BVSMOD:
    smt_bits_signed_divrem(kind, &out, a, b, width);
    break;
  case SMT_K_BVSHL:
    smt_bits_shl(&out, a, b, width);
    break;
  case SMT_K_BVLSHR:
    smt_bits_lshr(&out, a, b, width);
    break;
  case SMT_K_BVASHR:
    smt_bits_ashr(&out, a, b, width);
    break;
  case SMT_K_BVULT:
    out.w[0] = (uint64_t)smt_bits_ult(a, b);
    break;
  case SMT_K_BVULE:
    out.w[0] = (uint64_t)!smt_bits_ult(b, a);
    break;
  case SMT_K_BVSLT:
    out.w[0] = (uint64_t)smt_bits_slt(a, b, bwidth);
    break;
  case SMT_K_BVSLE:
    out.w[0] = (uint64_t)!smt_bits_slt(b, a, bwidth);
    break;
  case SMT_K_CONCAT:
    for (unsigned i = 0; i < bwidth; i++) {
      if (smt_bits_get(b, i)) {
        smt_bits_set(&out, i, 1);
      }
    }
    for (unsigned i = 0; i + bwidth < width; i++) {
      if (smt_bits_get(a, i)) {
        smt_bits_set(&out, i + bwidth, 1);
      }
    }
    break;
  case SMT_K_EXTRACT:
    for (unsigned i = p1; i <= p0; i++) {
      if (smt_bits_get(a, i)) {
        smt_bits_set(&out, i - p1, 1);
      }
    }
    break;
  case SMT_K_ZEXT:
    out = *a;
    break;
  case SMT_K_SEXT: {
    out = *a;
    unsigned from = width - p0;
    if (smt_bits_get(a, from - 1u)) {
      for (unsigned i = from; i < width; i++) {
        smt_bits_set(&out, i, 1);
      }
    }
    break;
  }
  default:
    return 0;
  }
  smt_bits_mask(&out, width);
  *r = out;
  return 1;
}

void smt_bits_print_hex(const SmtBits *b, unsigned width, FILE *out) {
  if (width % 4u == 0) {
    fputs("#x", out);
    for (int i = (int)width / 4 - 1; i >= 0; i--) {
      unsigned nib = (unsigned)((b->w[(unsigned)i * 4u / 64u] >>
                                 (((unsigned)i * 4u) & 63u)) &
                                15u);
      fputc("0123456789abcdef"[nib], out);
    }
  } else {
    fputs("#b", out);
    for (int i = (int)width - 1; i >= 0; i--) {
      fputc(smt_bits_get(b, (unsigned)i) ? '1' : '0', out);
    }
  }
}
