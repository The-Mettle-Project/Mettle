/* C counterpart to fp_div.mettle - float64 serial divide chain. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 20000000

static double div_chain(void) {
    double acc = 1000000.0;
    for (int i = 1; i <= N; i++) {
        double d = (double)(i & 255) + 1.5;
        acc = acc / d + 1.0000001;
    }
    return acc;
}

int main(void) {
    const int passes = 20;
    printf("float64 serial divide chain, 20M iters x 20 passes\n");

    uint64_t t0 = bench_time_us();
    double acc = 0.0;
    for (int p = 0; p < passes; p++) {
        acc += div_chain();
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", (int64_t)(acc * 1000.0));
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
