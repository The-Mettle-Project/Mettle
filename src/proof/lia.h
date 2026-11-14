#ifndef METTLE_LIA_H
#define METTLE_LIA_H

#include <stddef.h>
#include <stdint.h>

#define LIA_LIMBS 40u

typedef struct {
  int neg;
  uint32_t d[LIA_LIMBS];
} LiaBig;

void lia_big_zero(LiaBig *a);
void lia_big_from_i64(LiaBig *a, int64_t v);
void lia_big_from_u64(LiaBig *a, uint64_t v);
void lia_big_pow2(LiaBig *a, unsigned k);
int lia_big_is_zero(const LiaBig *a);
int lia_big_cmp(const LiaBig *a, const LiaBig *b);
int lia_big_add(LiaBig *r, const LiaBig *a, const LiaBig *b);
int lia_big_sub(LiaBig *r, const LiaBig *a, const LiaBig *b);
int lia_big_mul(LiaBig *r, const LiaBig *a, const LiaBig *b);
void lia_big_neg(LiaBig *r, const LiaBig *a);
int lia_big_floordiv(LiaBig *q, const LiaBig *a, const LiaBig *b);
void lia_big_gcd(LiaBig *r, const LiaBig *a, const LiaBig *b);
int lia_big_to_i64(const LiaBig *a, int64_t *out);

typedef struct LiaSystem LiaSystem;

LiaSystem *lia_new(unsigned nvars);
void lia_free(LiaSystem *s);
LiaSystem *lia_copy(const LiaSystem *s);
unsigned lia_var_count(const LiaSystem *s);
unsigned lia_add_var(LiaSystem *s);
void lia_add_le(LiaSystem *s, const LiaBig *coeffs, const LiaBig *rhs);

typedef enum {
  LIA_INFEASIBLE = 0,
  LIA_FEASIBLE_OR_UNKNOWN = 1,
  LIA_TOO_BIG = 2
} LiaVerdict;

LiaVerdict lia_check(const LiaSystem *s);

#endif
