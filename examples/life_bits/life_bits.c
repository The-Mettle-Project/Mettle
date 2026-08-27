#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"


#define WORDS 16
#define ROWS 512
#define CELLS 1024
#define GENS 200
#define PASSES 3

static uint64_t row_shl(uint64_t *src, int32_t base, int32_t i) {
  uint64_t cur = src[base + i] >> 1;
  if (i + 1 < WORDS) {
    cur = cur | (src[base + i + 1] << 63);
  }
  return cur;
}

static uint64_t row_shr(uint64_t *src, int32_t base, int32_t i) {
  uint64_t cur = src[base + i] << 1;
  if (i > 0) {
    cur = cur | (src[base + i - 1] >> 63);
  }
  return cur;
}

static void seed_grid(uint64_t *g, uint32_t seed) {
  uint32_t state = seed;
  int32_t i = 0;
  while (i < ROWS * WORDS) {
    g[i] = 0;
    i += 1;
  }
  int32_t r = 64;
  while (r < ROWS - 64) {
    int32_t w = 2;
    while (w < WORDS - 2) {
      state = state ^ (state << 13);
      state = state ^ (state >> 17);
      state = state ^ (state << 5);
      uint32_t hi = state;
      state = state ^ (state << 13);
      state = state ^ (state >> 17);
      state = state ^ (state << 5);
      uint32_t lo = state;
      g[r * WORDS + w] = ((uint64_t)hi << 32) | (uint64_t)lo;
      w += 1;
    }
    r += 1;
  }
}

static void step(uint64_t *src, uint64_t *dst) {
  int32_t r = 0;
  while (r < ROWS) {
    int32_t up = (r - 1) * WORDS;
    int32_t mid = r * WORDS;
    int32_t dn = (r + 1) * WORDS;
    int32_t has_up = 1;
    int32_t has_dn = 1;
    if (r == 0) { has_up = 0; }
    if (r == ROWS - 1) { has_dn = 0; }

    int32_t i = 0;
    while (i < WORDS) {
      uint64_t nw = 0;
      uint64_t nn = 0;
      uint64_t ne = 0;
      if (has_up != 0) {
        nw = row_shr(src, up, i);
        nn = src[up + i];
        ne = row_shl(src, up, i);
      }
      uint64_t ww = row_shr(src, mid, i);
      uint64_t ee = row_shl(src, mid, i);
      uint64_t sw = 0;
      uint64_t ss = 0;
      uint64_t se = 0;
      if (has_dn != 0) {
        sw = row_shr(src, dn, i);
        ss = src[dn + i];
        se = row_shl(src, dn, i);
      }

      uint64_t xa = nw ^ nn;
      uint64_t s0 = xa ^ ne;
      uint64_t s1 = (nw & nn) | (xa & ne);

      uint64_t xb = ww ^ ee;
      uint64_t t0 = xb ^ sw;
      uint64_t t1 = (ww & ee) | (xb & sw);

      uint64_t u0 = ss ^ se;
      uint64_t u1 = ss & se;

      uint64_t xc = s0 ^ t0;
      uint64_t c0 = xc ^ u0;
      uint64_t k0 = (s0 & t0) | (xc & u0);

      uint64_t xd = s1 ^ t1;
      uint64_t v0 = xd ^ u1;
      uint64_t v1 = (s1 & t1) | (xd & u1);

      uint64_t c1 = v0 ^ k0;
      uint64_t k1 = v0 & k0;

      uint64_t c2 = v1 ^ k1;
      uint64_t c3 = v1 & k1;

      uint64_t alive = src[mid + i];
      uint64_t high_clear = ~c2 & ~c3;
      uint64_t eq3 = c0 & c1 & high_clear;
      uint64_t eq2 = ~c0 & c1 & high_clear;
      dst[mid + i] = eq3 | (alive & eq2);
      i += 1;
    }
    r += 1;
  }
}

static int32_t popcount64(uint64_t v) {
  uint64_t x = v;
  int32_t n = 0;
  while (x != 0) {
    x = x & (x - 1);
    n += 1;
  }
  return n;
}

static int64_t population(uint64_t *g) {
  int64_t total = 0;
  int32_t i = 0;
  while (i < ROWS * WORDS) {
    total = total + (int64_t)popcount64(g[i]);
    i += 1;
  }
  return total;
}

static uint64_t round_trip(uint64_t *a, uint64_t *b, int64_t *out_pop) {
  seed_grid(a, 2463534242ULL);
  uint64_t h = 14695981039346656037ULL;
  uint64_t *src = a;
  uint64_t *dst = b;
  int32_t gen = 0;
  while (gen < GENS) {
    step(src, dst);
    uint64_t *tmp = src;
    src = dst;
    dst = tmp;
    if (gen % 8 == 0) {
      h = h * 1000003 + (uint64_t)population(src);
    }
    gen += 1;
  }
  int64_t pop = population(src);
  int32_t i = 0;
  while (i < ROWS * WORDS) {
    h = h ^ src[i];
    h = h * 1099511628211ULL;
    i += 1;
  }
  h = h * 31 + (uint64_t)pop;
  *out_pop = pop;
  return h;
}

int main(void) {
    uint64_t *a = (uint64_t *)malloc((size_t)ROWS * WORDS * 8);
    uint64_t *b = (uint64_t *)malloc((size_t)ROWS * WORDS * 8);
    if (a == NULL || b == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    printf("Life: %dx%d bit grid, %d generations, word-parallel adders\n", ROWS, CELLS, GENS);

    int64_t pop = 0;
    uint64_t check = round_trip(a, b, &pop);
    printf("  final population = %" PRId64 "\n", pop);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int64_t p2 = 0;
        bench_hash = bench_hash * 1000003 + round_trip(a, b, &p2);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(a);
    free(b);
    return 0;
}
