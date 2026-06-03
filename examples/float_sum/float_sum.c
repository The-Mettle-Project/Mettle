/*
 * C float64 array reduction benchmark - counterpart to float_sum.mettle.
 *
 * Sums a 65536-element double array, 4000 passes. GCC -O3 auto-vectorizes
 * this reduction; the volatile pass count keeps the outer loop honest.
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#include "../bench_time.h"

#define N 65536

static void fill(double *a, int n) {
    for (int i = 0; i < n; i++) {
        a[i] = (double)i * 0.5 + 1.25;
    }
}

static double sum_array(const double *a, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        s += a[i];
    }
    return s;
}

int main(void) {
    double *a = malloc((size_t)N * sizeof(double));
    fill(a, N);

    const int passes = 4000;
    printf("float64 sum reduction: 65536 elems x 4000 passes\n");

    volatile int bench_n = N;

    uint64_t t0 = bench_time_us();
    double acc = 0.0;
    for (int p = 0; p < passes; p++) {
        acc += sum_array(a, bench_n);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", (int64_t)acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(a);
    return 0;
}
