/* C counterpart to float32_sum.mettle - single-precision reduction+scale. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#include "../bench_time.h"

#define N 65536

static float sum_f32(const float *a, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; i++) {
        s += a[i] * 1.5f;
    }
    return s;
}

int main(void) {
    float *a = malloc((size_t)N * sizeof(float));
    for (int i = 0; i < N; i++) {
        a[i] = (float)i * 0.5f + 1.25f;
    }

    const int passes = 4000;
    printf("float32 sum+scale: 65536 elems x 4000 passes\n");

    volatile int n = N;
    uint64_t t0 = bench_time_us();
    float acc = 0.0f;
    for (int p = 0; p < passes; p++) {
        acc += sum_f32(a, n);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", (int64_t)acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(a);
    return 0;
}
