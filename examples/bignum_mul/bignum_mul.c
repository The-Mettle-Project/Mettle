#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define LIMBS 1024
#define FACT_N 1000
#define CHAIN 3
#define DEC_BASE 1000000000u
#define PASSES 24

typedef struct {
    uint32_t *limb;
    int32_t len;
} Bignum;

static void bn_zero(Bignum *a) {
    int32_t i = 0;
    while (i < LIMBS) {
        a->limb[i] = 0;
        i += 1;
    }
    a->len = 0;
}

static void bn_set_u32(Bignum *a, uint32_t v) {
    bn_zero(a);
    if (v != 0) {
        a->limb[0] = v;
        a->len = 1;
    }
}

static void bn_copy(Bignum *dst, Bignum *src) {
    int32_t i = 0;
    while (i < src->len) {
        dst->limb[i] = src->limb[i];
        i += 1;
    }
    while (i < dst->len) {
        dst->limb[i] = 0;
        i += 1;
    }
    dst->len = src->len;
}

static void bn_mul_small(Bignum *a, uint32_t m) {
    uint64_t carry = 0;
    int32_t i = 0;
    while (i < a->len) {
        uint64_t cur = (uint64_t)a->limb[i] * (uint64_t)m + carry;
        a->limb[i] = (uint32_t)(cur & 4294967295u);
        carry = cur >> 32;
        i += 1;
    }
    while (carry != 0 && i < LIMBS) {
        a->limb[i] = (uint32_t)(carry & 4294967295u);
        carry >>= 32;
        i += 1;
    }
    if (i > a->len) {
        a->len = i;
    }
}

static void bn_add_small(Bignum *a, uint32_t v) {
    uint64_t carry = (uint64_t)v;
    int32_t i = 0;
    while (carry != 0 && i < LIMBS) {
        uint64_t cur = (uint64_t)a->limb[i] + carry;
        a->limb[i] = (uint32_t)(cur & 4294967295u);
        carry = cur >> 32;
        i += 1;
    }
    if (i > a->len) {
        a->len = i;
    }
}

static void bn_mul(Bignum *dst, Bignum *x, Bignum *y) {
    bn_zero(dst);
    int32_t total = x->len + y->len;
    if (total > LIMBS) {
        total = LIMBS;
    }
    int32_t i = 0;
    while (i < x->len) {
        uint64_t carry = 0;
        uint64_t xi = (uint64_t)x->limb[i];
        int32_t j = 0;
        while (j < y->len && i + j < LIMBS) {
            uint64_t cur = xi * (uint64_t)y->limb[j] + (uint64_t)dst->limb[i + j] + carry;
            dst->limb[i + j] = (uint32_t)(cur & 4294967295u);
            carry = cur >> 32;
            j += 1;
        }
        int32_t k = i + j;
        while (carry != 0 && k < LIMBS) {
            uint64_t cur2 = (uint64_t)dst->limb[k] + carry;
            dst->limb[k] = (uint32_t)(cur2 & 4294967295u);
            carry = cur2 >> 32;
            k += 1;
        }
        i += 1;
    }
    while (total > 0 && dst->limb[total - 1] == 0) {
        total -= 1;
    }
    dst->len = total;
}

static uint32_t bn_divmod_small(Bignum *a, uint32_t d) {
    uint64_t rem = 0;
    int32_t i = a->len;
    while (i > 0) {
        i -= 1;
        uint64_t cur = (rem << 32) | (uint64_t)a->limb[i];
        a->limb[i] = (uint32_t)(cur / (uint64_t)d);
        rem = cur % (uint64_t)d;
    }
    int32_t n = a->len;
    while (n > 0 && a->limb[n - 1] == 0) {
        n -= 1;
    }
    a->len = n;
    return (uint32_t)rem;
}

static int32_t bn_is_zero(Bignum *a) {
    if (a->len == 0) {
        return 1;
    }
    return 0;
}

static void build_factorial(Bignum *f, int32_t n) {
    bn_set_u32(f, 1);
    int32_t i = 2;
    while (i <= n) {
        bn_mul_small(f, (uint32_t)i);
        i += 1;
    }
}

static void build_seeded(Bignum *a, uint32_t seed, int32_t limbs) {
    bn_zero(a);
    uint32_t state = seed;
    int32_t i = 0;
    while (i < limbs) {
        state ^= (state << 13);
        state ^= (state >> 17);
        state ^= (state << 5);
        a->limb[i] = state | 1u;
        i += 1;
    }
    a->len = limbs;
}

static uint64_t decimal_checksum(Bignum *work, Bignum *src, int32_t *out_digits) {
    bn_copy(work, src);
    uint64_t h = 14695981039346656037ULL;
    int32_t digits = 0;
    while (bn_is_zero(work) == 0) {
        uint32_t chunk = bn_divmod_small(work, DEC_BASE);
        int32_t c = 0;
        while (c < 9) {
            uint32_t d = chunk % 10u;
            chunk /= 10u;
            h ^= (uint64_t)d;
            h *= 1099511628211ULL;
            digits += 1;
            c += 1;
        }
    }
    *out_digits = digits;
    return h;
}

static uint64_t round_trip(Bignum *fact, Bignum *seed_a, Bignum *seed_b, Bignum *prod,
                           Bignum *acc, Bignum *work, int32_t *out_digits) {
    build_factorial(fact, FACT_N);

    uint64_t h = 1469598103934665603ULL;
    h = h * 31 + (uint64_t)fact->len;

    bn_copy(acc, fact);
    int32_t round = 0;
    while (round < CHAIN) {
        build_seeded(seed_a, (uint32_t)(2166136261LL + (int64_t)round * 7919), 24 + round * 8);
        build_seeded(seed_b, (uint32_t)(3735928559LL + (int64_t)round * 104729), 24 + round * 8);
        bn_mul(prod, seed_a, seed_b);
        bn_add_small(prod, (uint32_t)(round + 1));
        bn_mul(work, acc, prod);
        bn_copy(acc, work);
        h = h * 1000003 + (uint64_t)acc->len;
        h ^= (uint64_t)prod->limb[0];
        round += 1;
    }

    int32_t digits = 0;
    uint64_t dh = decimal_checksum(work, acc, &digits);
    h = h * 1000003 + dh;
    h = h * 31 + (uint64_t)digits;
    *out_digits = digits;
    return h;
}

int main(void) {
    uint32_t *pool = (uint32_t *)malloc((size_t)LIMBS * 4 * 6);
    if (pool == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    Bignum fact;
    Bignum seed_a;
    Bignum seed_b;
    Bignum prod;
    Bignum acc;
    Bignum work;

    fact.limb = pool;
    fact.len = 0;
    seed_a.limb = pool + LIMBS;
    seed_a.len = 0;
    seed_b.limb = pool + LIMBS * 2;
    seed_b.len = 0;
    prod.limb = pool + LIMBS * 3;
    prod.len = 0;
    acc.limb = pool + LIMBS * 4;
    acc.len = 0;
    work.limb = pool + LIMBS * 5;
    work.len = 0;

    bn_zero(&fact);
    bn_zero(&seed_a);
    bn_zero(&seed_b);
    bn_zero(&prod);
    bn_zero(&acc);
    bn_zero(&work);

    printf("Bignum: factorial(%d) then %d wide multiplies, base 2^32 limbs\n", FACT_N, CHAIN);

    int32_t digits = 0;
    uint64_t check = round_trip(&fact, &seed_a, &seed_b, &prod, &acc, &work, &digits);
    printf("  factorial limbs = %d, product limbs = %d, decimal digits = %d\n",
           fact.len, acc.len, digits);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int32_t d2 = 0;
        bench_hash = bench_hash * 1000003 + round_trip(&fact, &seed_a, &seed_b, &prod, &acc, &work, &d2);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(pool);
    return 0;
}
