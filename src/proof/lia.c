#include "lia.h"
#include <stdlib.h>
#include <string.h>

static int mag_is_zero(const uint32_t *a) {
  for (unsigned i = 0; i < LIA_LIMBS; i++) {
    if (a[i]) {
      return 0;
    }
  }
  return 1;
}

static int mag_cmp(const uint32_t *a, const uint32_t *b) {
  for (unsigned i = LIA_LIMBS; i-- > 0;) {
    if (a[i] != b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }
  return 0;
}

static int mag_add(uint32_t *r, const uint32_t *a, const uint32_t *b) {
  uint64_t carry = 0;
  for (unsigned i = 0; i < LIA_LIMBS; i++) {
    uint64_t s = (uint64_t)a[i] + b[i] + carry;
    r[i] = (uint32_t)s;
    carry = s >> 32;
  }
  return carry == 0;
}

static void mag_sub(uint32_t *r, const uint32_t *a, const uint32_t *b) {
  int64_t borrow = 0;
  for (unsigned i = 0; i < LIA_LIMBS; i++) {
    int64_t s = (int64_t)a[i] - b[i] - borrow;
    borrow = s < 0;
    r[i] = (uint32_t)(s + (borrow ? ((int64_t)1 << 32) : 0));
  }
}

static unsigned mag_bits(const uint32_t *a) {
  for (unsigned i = LIA_LIMBS; i-- > 0;) {
    if (a[i]) {
      return i * 32u + 32u - (unsigned)__builtin_clz(a[i]);
    }
  }
  return 0;
}

static void mag_shl1(uint32_t *a) {
  for (unsigned i = LIA_LIMBS; i-- > 1;) {
    a[i] = (a[i] << 1) | (a[i - 1] >> 31);
  }
  a[0] <<= 1;
}

static void mag_shr1(uint32_t *a) {
  for (unsigned i = 0; i + 1 < LIA_LIMBS; i++) {
    a[i] = (a[i] >> 1) | (a[i + 1] << 31);
  }
  a[LIA_LIMBS - 1] >>= 1;
}

static void mag_divmod(uint32_t *q, uint32_t *r, const uint32_t *a,
                       const uint32_t *b) {
  uint32_t quot[LIA_LIMBS], rem[LIA_LIMBS];
  memset(quot, 0, sizeof(quot));
  memset(rem, 0, sizeof(rem));
  unsigned bbits = mag_bits(b);
  if (bbits <= 32) {
    uint64_t div = b[0], carry = 0;
    for (unsigned i = LIA_LIMBS; i-- > 0;) {
      uint64_t cur = (carry << 32) | a[i];
      quot[i] = (uint32_t)(cur / div);
      carry = cur % div;
    }
    rem[0] = (uint32_t)carry;
  } else {
    for (unsigned i = mag_bits(a); i-- > 0;) {
      mag_shl1(rem);
      rem[0] |= (a[i / 32u] >> (i % 32u)) & 1u;
      if (mag_cmp(rem, b) >= 0) {
        mag_sub(rem, rem, b);
        quot[i / 32u] |= 1u << (i % 32u);
      }
    }
  }
  if (q) {
    memcpy(q, quot, sizeof(quot));
  }
  if (r) {
    memcpy(r, rem, sizeof(rem));
  }
}

void lia_big_zero(LiaBig *a) { memset(a, 0, sizeof(*a)); }

void lia_big_from_u64(LiaBig *a, uint64_t v) {
  lia_big_zero(a);
  a->d[0] = (uint32_t)v;
  a->d[1] = (uint32_t)(v >> 32);
}

void lia_big_from_i64(LiaBig *a, int64_t v) {
  uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  lia_big_from_u64(a, m);
  a->neg = v < 0;
}

void lia_big_pow2(LiaBig *a, unsigned k) {
  lia_big_zero(a);
  if (k < LIA_LIMBS * 32u) {
    a->d[k / 32u] = 1u << (k % 32u);
  }
}

int lia_big_is_zero(const LiaBig *a) { return mag_is_zero(a->d); }

int lia_big_cmp(const LiaBig *a, const LiaBig *b) {
  int az = lia_big_is_zero(a), bz = lia_big_is_zero(b);
  int an = a->neg && !az, bn = b->neg && !bz;
  if (an != bn) {
    return an ? -1 : 1;
  }
  int c = mag_cmp(a->d, b->d);
  return an ? -c : c;
}

int lia_big_add(LiaBig *r, const LiaBig *a, const LiaBig *b) {
  LiaBig out;
  lia_big_zero(&out);
  if (a->neg == b->neg) {
    if (!mag_add(out.d, a->d, b->d)) {
      return 0;
    }
    out.neg = a->neg;
  } else if (mag_cmp(a->d, b->d) >= 0) {
    mag_sub(out.d, a->d, b->d);
    out.neg = a->neg;
  } else {
    mag_sub(out.d, b->d, a->d);
    out.neg = b->neg;
  }
  if (mag_is_zero(out.d)) {
    out.neg = 0;
  }
  if (mag_bits(out.d) > LIA_LIMBS * 32u - 2u) {
    return 0;
  }
  *r = out;
  return 1;
}

void lia_big_neg(LiaBig *r, const LiaBig *a) {
  *r = *a;
  r->neg = mag_is_zero(a->d) ? 0 : !a->neg;
}

int lia_big_sub(LiaBig *r, const LiaBig *a, const LiaBig *b) {
  LiaBig nb;
  lia_big_neg(&nb, b);
  return lia_big_add(r, a, &nb);
}

int lia_big_mul(LiaBig *r, const LiaBig *a, const LiaBig *b) {
  if (mag_bits(a->d) + mag_bits(b->d) > LIA_LIMBS * 32u - 2u) {
    return 0;
  }
  uint64_t acc[LIA_LIMBS * 2u];
  memset(acc, 0, sizeof(acc));
  for (unsigned i = 0; i < LIA_LIMBS; i++) {
    if (!a->d[i]) {
      continue;
    }
    uint64_t carry = 0;
    for (unsigned j = 0; j < LIA_LIMBS; j++) {
      uint64_t cur = acc[i + j] + (uint64_t)a->d[i] * b->d[j] + carry;
      acc[i + j] = cur & 0xffffffffu;
      carry = cur >> 32;
    }
    for (unsigned k = i + LIA_LIMBS; carry && k < LIA_LIMBS * 2u; k++) {
      uint64_t cur = acc[k] + carry;
      acc[k] = cur & 0xffffffffu;
      carry = cur >> 32;
    }
  }
  LiaBig out;
  lia_big_zero(&out);
  for (unsigned i = 0; i < LIA_LIMBS; i++) {
    out.d[i] = (uint32_t)acc[i];
  }
  out.neg = (a->neg != b->neg) && !mag_is_zero(out.d);
  *r = out;
  return 1;
}

int lia_big_floordiv(LiaBig *q, const LiaBig *a, const LiaBig *b) {
  if (lia_big_is_zero(b) || b->neg) {
    return 0;
  }
  LiaBig quot, rem;
  lia_big_zero(&quot);
  lia_big_zero(&rem);
  mag_divmod(quot.d, rem.d, a->d, b->d);
  if (a->neg && !mag_is_zero(a->d)) {
    LiaBig one;
    lia_big_from_u64(&one, 1);
    if (!mag_is_zero(rem.d)) {
      mag_add(quot.d, quot.d, one.d);
    }
    quot.neg = !mag_is_zero(quot.d);
  }
  *q = quot;
  return 1;
}

void lia_big_gcd(LiaBig *r, const LiaBig *a, const LiaBig *b) {
  uint32_t x[LIA_LIMBS], y[LIA_LIMBS];
  memcpy(x, a->d, sizeof(x));
  memcpy(y, b->d, sizeof(y));
  lia_big_zero(r);
  if (mag_is_zero(x)) {
    memcpy(r->d, y, sizeof(y));
    return;
  }
  if (mag_is_zero(y)) {
    memcpy(r->d, x, sizeof(x));
    return;
  }
  unsigned shift = 0;
  while (!((x[0] | y[0]) & 1u)) {
    mag_shr1(x);
    mag_shr1(y);
    shift++;
  }
  while (!(x[0] & 1u)) {
    mag_shr1(x);
  }
  while (!mag_is_zero(y)) {
    while (!(y[0] & 1u)) {
      mag_shr1(y);
    }
    if (mag_cmp(x, y) > 0) {
      uint32_t t[LIA_LIMBS];
      memcpy(t, x, sizeof(t));
      memcpy(x, y, sizeof(t));
      memcpy(y, t, sizeof(t));
    }
    mag_sub(y, y, x);
  }
  while (shift--) {
    mag_shl1(x);
  }
  memcpy(r->d, x, sizeof(x));
}

int lia_big_to_i64(const LiaBig *a, int64_t *out) {
  if (mag_bits(a->d) > 63) {
    return 0;
  }
  int64_t v = (int64_t)(((uint64_t)a->d[1] << 32) | a->d[0]);
  *out = a->neg ? -v : v;
  return 1;
}

typedef struct {
  LiaBig *c;
  LiaBig b;
} LiaRow;

struct LiaSystem {
  unsigned nvars;
  unsigned var_cap;
  LiaRow *rows;
  size_t nrows;
  size_t cap;
};

LiaSystem *lia_new(unsigned nvars) {
  LiaSystem *s = calloc(1, sizeof(LiaSystem));
  s->nvars = nvars;
  s->var_cap = nvars < 8 ? 8 : nvars;
  return s;
}

static void row_free(LiaRow *r) { free(r->c); }

void lia_free(LiaSystem *s) {
  if (!s) {
    return;
  }
  for (size_t i = 0; i < s->nrows; i++) {
    row_free(&s->rows[i]);
  }
  free(s->rows);
  free(s);
}

unsigned lia_var_count(const LiaSystem *s) { return s->nvars; }

unsigned lia_add_var(LiaSystem *s) {
  unsigned v = s->nvars++;
  if (s->nvars > s->var_cap) {
    unsigned cap = s->var_cap * 2;
    for (size_t i = 0; i < s->nrows; i++) {
      s->rows[i].c = realloc(s->rows[i].c, cap * sizeof(LiaBig));
      for (unsigned k = s->var_cap; k < cap; k++) {
        lia_big_zero(&s->rows[i].c[k]);
      }
    }
    s->var_cap = cap;
  }
  for (size_t i = 0; i < s->nrows; i++) {
    lia_big_zero(&s->rows[i].c[v]);
  }
  return v;
}

static LiaRow *push_row(LiaSystem *s) {
  if (s->nrows == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 32;
    s->rows = realloc(s->rows, s->cap * sizeof(LiaRow));
  }
  LiaRow *r = &s->rows[s->nrows++];
  r->c = calloc(s->var_cap, sizeof(LiaBig));
  lia_big_zero(&r->b);
  return r;
}

void lia_add_le(LiaSystem *s, const LiaBig *coeffs, const LiaBig *rhs) {
  LiaRow *r = push_row(s);
  memcpy(r->c, coeffs, s->nvars * sizeof(LiaBig));
  r->b = *rhs;
}

LiaSystem *lia_copy(const LiaSystem *s) {
  LiaSystem *c = lia_new(s->nvars);
  c->var_cap = s->var_cap;
  for (size_t i = 0; i < s->nrows; i++) {
    LiaRow *r = push_row(c);
    memcpy(r->c, s->rows[i].c, s->var_cap * sizeof(LiaBig));
    r->b = s->rows[i].b;
  }
  return c;
}

enum { ROW_OK, ROW_TRIVIAL, ROW_CONTRADICTION, ROW_OVERFLOW };

static int row_normalize(LiaRow *r, unsigned nvars) {
  LiaBig g;
  lia_big_zero(&g);
  for (unsigned i = 0; i < nvars; i++) {
    if (!lia_big_is_zero(&r->c[i])) {
      lia_big_gcd(&g, &g, &r->c[i]);
    }
  }
  if (lia_big_is_zero(&g)) {
    LiaBig zero;
    lia_big_zero(&zero);
    return lia_big_cmp(&r->b, &zero) < 0 ? ROW_CONTRADICTION : ROW_TRIVIAL;
  }
  LiaBig one;
  lia_big_from_u64(&one, 1);
  if (lia_big_cmp(&g, &one) != 0) {
    for (unsigned i = 0; i < nvars; i++) {
      if (!lia_big_is_zero(&r->c[i])) {
        int neg = r->c[i].neg;
        LiaBig q;
        LiaBig mag = r->c[i];
        mag.neg = 0;
        lia_big_floordiv(&q, &mag, &g);
        q.neg = neg && !lia_big_is_zero(&q);
        r->c[i] = q;
      }
    }
    LiaBig q;
    if (!lia_big_floordiv(&q, &r->b, &g)) {
      return ROW_OVERFLOW;
    }
    r->b = q;
  }
  return ROW_OK;
}

static int rows_same_lhs(const LiaRow *a, const LiaRow *b, unsigned nvars) {
  for (unsigned i = 0; i < nvars; i++) {
    if (lia_big_cmp(&a->c[i], &b->c[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

#define LIA_MAX_ROWS 2000u

LiaVerdict lia_check(const LiaSystem *input) {
  LiaSystem *s = lia_copy(input);
  unsigned n = s->nvars;
  LiaVerdict verdict = LIA_FEASIBLE_OR_UNKNOWN;
  size_t j = 0;
  for (size_t i = 0; i < s->nrows; i++) {
    int st = row_normalize(&s->rows[i], n);
    if (st == ROW_CONTRADICTION) {
      verdict = LIA_INFEASIBLE;
      goto done;
    }
    if (st == ROW_OVERFLOW) {
      verdict = LIA_TOO_BIG;
      goto done;
    }
    if (st == ROW_TRIVIAL) {
      row_free(&s->rows[i]);
      continue;
    }
    s->rows[j++] = s->rows[i];
  }
  s->nrows = j;
  for (int changed = 1; changed && verdict == LIA_FEASIBLE_OR_UNKNOWN;) {
    changed = 0;
    for (size_t a = 0; a < s->nrows && !changed; a++) {
      for (size_t b = a + 1; b < s->nrows && !changed; b++) {
        LiaRow *ra = &s->rows[a], *rb = &s->rows[b];
        LiaBig nb;
        lia_big_neg(&nb, &rb->b);
        if (lia_big_cmp(&ra->b, &nb) != 0) {
          continue;
        }
        int negated = 1;
        unsigned unit = n;
        for (unsigned k = 0; k < n && negated; k++) {
          LiaBig nk;
          lia_big_neg(&nk, &rb->c[k]);
          negated = lia_big_cmp(&ra->c[k], &nk) == 0;
          LiaBig mag = ra->c[k];
          mag.neg = 0;
          LiaBig one;
          lia_big_from_u64(&one, 1);
          if (unit == n && lia_big_cmp(&mag, &one) == 0) {
            unit = k;
          }
        }
        if (!negated || unit == n) {
          continue;
        }
        LiaRow eq;
        eq.c = malloc(s->var_cap * sizeof(LiaBig));
        memcpy(eq.c, ra->c, s->var_cap * sizeof(LiaBig));
        eq.b = ra->b;
        int eq_neg = eq.c[unit].neg;
        row_free(&s->rows[b]);
        row_free(&s->rows[a]);
        s->rows[b] = s->rows[--s->nrows];
        s->rows[a] = s->rows[--s->nrows];
        size_t keep = 0;
        size_t k = 0;
        for (; k < s->nrows && verdict == LIA_FEASIBLE_OR_UNKNOWN; k++) {
          LiaRow *r = &s->rows[k];
          if (!lia_big_is_zero(&r->c[unit])) {
            LiaBig f = r->c[unit];
            if (!eq_neg) {
              lia_big_neg(&f, &f);
            }
            int ok = 1;
            for (unsigned v = 0; v < n && ok; v++) {
              LiaBig prod;
              ok = lia_big_mul(&prod, &eq.c[v], &f) &&
                   lia_big_add(&r->c[v], &r->c[v], &prod);
            }
            LiaBig prod;
            ok = ok && lia_big_mul(&prod, &eq.b, &f) &&
                 lia_big_add(&r->b, &r->b, &prod);
            if (!ok) {
              verdict = LIA_TOO_BIG;
              break;
            }
            int st = row_normalize(r, n);
            if (st == ROW_CONTRADICTION) {
              verdict = LIA_INFEASIBLE;
              break;
            }
            if (st == ROW_OVERFLOW) {
              verdict = LIA_TOO_BIG;
              break;
            }
            if (st == ROW_TRIVIAL) {
              row_free(r);
              continue;
            }
          }
          s->rows[keep++] = *r;
        }
        if (verdict != LIA_FEASIBLE_OR_UNKNOWN && k < s->nrows) {
          s->rows[keep++] = s->rows[k];
          for (size_t z = k + 1; z < s->nrows; z++) {
            s->rows[keep++] = s->rows[z];
          }
        }
        s->nrows = keep;
        free(eq.c);
        changed = 1;
      }
    }
  }
  if (verdict != LIA_FEASIBLE_OR_UNKNOWN) {
    goto done;
  }
  unsigned char *gone = calloc(n ? n : 1, 1);
  for (;;) {
    long best = 0;
    int found = 0;
    unsigned best_var = 0;
    for (unsigned v = 0; v < n; v++) {
      if (gone[v]) {
        continue;
      }
      long pos = 0, neg = 0;
      for (size_t i = 0; i < s->nrows; i++) {
        if (lia_big_is_zero(&s->rows[i].c[v])) {
          continue;
        }
        if (s->rows[i].c[v].neg) {
          neg++;
        } else {
          pos++;
        }
      }
      if (pos + neg == 0) {
        gone[v] = 1;
        continue;
      }
      long cost = pos * neg - pos - neg;
      if (!found || cost < best) {
        best = cost;
        best_var = v;
        found = 1;
      }
    }
    if (!found) {
      break;
    }
    unsigned v = best_var;
    gone[v] = 1;
    size_t old = s->nrows;
    LiaRow *rows = s->rows;
    s->rows = NULL;
    s->nrows = 0;
    s->cap = 0;
    for (size_t i = 0; i < old; i++) {
      if (lia_big_is_zero(&rows[i].c[v])) {
        LiaRow *r = push_row(s);
        memcpy(r->c, rows[i].c, s->var_cap * sizeof(LiaBig));
        r->b = rows[i].b;
      }
    }
    for (size_t p = 0; p < old && verdict == LIA_FEASIBLE_OR_UNKNOWN; p++) {
      if (lia_big_is_zero(&rows[p].c[v]) || rows[p].c[v].neg) {
        continue;
      }
      for (size_t q = 0; q < old; q++) {
        if (lia_big_is_zero(&rows[q].c[v]) || !rows[q].c[v].neg) {
          continue;
        }
        if (s->nrows >= LIA_MAX_ROWS) {
          verdict = LIA_TOO_BIG;
          break;
        }
        LiaBig fp = rows[q].c[v], fq = rows[p].c[v];
        fp.neg = 0;
        LiaRow *r = push_row(s);
        int ok = 1;
        for (unsigned k = 0; k < n && ok; k++) {
          LiaBig x, y;
          ok = lia_big_mul(&x, &rows[p].c[k], &fp) &&
               lia_big_mul(&y, &rows[q].c[k], &fq) &&
               lia_big_add(&r->c[k], &x, &y);
        }
        LiaBig x, y;
        ok = ok && lia_big_mul(&x, &rows[p].b, &fp) &&
             lia_big_mul(&y, &rows[q].b, &fq) && lia_big_add(&r->b, &x, &y);
        lia_big_zero(&r->c[v]);
        if (!ok) {
          verdict = LIA_TOO_BIG;
          break;
        }
        int st = row_normalize(r, n);
        if (st == ROW_CONTRADICTION) {
          verdict = LIA_INFEASIBLE;
          break;
        }
        if (st == ROW_OVERFLOW) {
          verdict = LIA_TOO_BIG;
          break;
        }
        if (st == ROW_TRIVIAL) {
          row_free(r);
          s->nrows--;
          continue;
        }
        for (size_t e = 0; e + 1 < s->nrows; e++) {
          if (rows_same_lhs(&s->rows[e], r, n)) {
            if (lia_big_cmp(&r->b, &s->rows[e].b) < 0) {
              s->rows[e].b = r->b;
            }
            row_free(r);
            s->nrows--;
            break;
          }
        }
      }
    }
    for (size_t i = 0; i < old; i++) {
      row_free(&rows[i]);
    }
    free(rows);
    if (verdict != LIA_FEASIBLE_OR_UNKNOWN) {
      break;
    }
  }
  free(gone);
done:
  lia_free(s);
  return verdict;
}
