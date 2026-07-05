/*
 * C random-gather reduction - counterpart to gather_sum.mettle
 *
 * sum += A[B[i]] over a shuffled permutation B into a 64 MB table A. The
 * random address stream defeats hardware prefetchers; GCC -O3 does not insert
 * software prefetches here, so it pays full memory latency per element.
 *
 * Build: build.bat (or: gcc -O3 -o gather_sum_c.exe gather_sum.c -lkernel32)
 * Run: gather_sum_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 16777216

static void build_permutation(int32_t *idx, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        idx[i] = i;
    }
    uint32_t state = 362436069u;
    for (int32_t i = count - 1; i > 0; i--) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        int32_t j = (int32_t)(state % (uint32_t)(i + 1));
        int32_t tmp = idx[i];
        idx[i] = idx[j];
        idx[j] = tmp;
    }
}

static int64_t gather(const int32_t *a, const int32_t *idx, int32_t count) {
    int64_t total = 0;
    for (int32_t i = 0; i < count; i++) {
        total += (int64_t)a[idx[i]];
    }
    return total;
}

int main(void) {
    int32_t count = N;
    int32_t *a = (int32_t *)malloc((size_t)count * sizeof(int32_t));
    int32_t *idx = (int32_t *)malloc((size_t)count * sizeof(int32_t));
    if (!a || !idx) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int32_t i = 0; i < count; i++) {
        a[i] = i & 8191;
    }
    build_permutation(idx, count);

    printf("Random gather: 16777216 elements (64MB table)\n");

    int64_t check = gather(a, idx, count);
    printf("Checksum = %" PRId64 "\n", check);

    const int passes = 20;
    printf("Benchmark: %d passes (gather_sum)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        /* Mutate the table each pass so no compiler can hoist the pure
         * gather out of the timing loop. */
        a[p] = a[p] + 1;
        bench_sum += gather(a, idx, count);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(a);
    free(idx);
    return 0;
}
