/*
 * C dot-product microbenchmark - counterpart to dot_product.mettle
 *
 * int32 dot product of two 65536-element vectors, 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int64_t dot_product(const int32_t *a, const int32_t *b, int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        sum += (int64_t)a[i] * (int64_t)b[i];
    }
    return sum;
}

int main(void) {
    const int64_t n = 65536;
    const int passes = 200;

    int32_t *a = (int32_t *)malloc((size_t)(n * (int64_t)sizeof(int32_t)));
    int32_t *b = (int32_t *)malloc((size_t)(n * (int64_t)sizeof(int32_t)));
    if (!a || !b) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t i = 0; i < n; i++) {
        a[i] = (int32_t)((i * 17 + 3) % 997);
        b[i] = (int32_t)((i * 31 + 7) % 991);
    }

    printf("Dot product: %d int32 pairs x %d passes\n", (int)n, passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += dot_product(a, b, n);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(a);
    free(b);
    return 0;
}