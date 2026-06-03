/* C counterpart to i2f_conv.mettle - int<->float64 conversion loop. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 30000000

static int64_t run(void) {
    int64_t acc = 0;
    for (int64_t i = 0; i < (int64_t)N; i++) {
        double f = (double)i * 0.5 + 1.0;
        double g = f * 1.25 - 0.5;
        acc = acc + (int64_t)g;
    }
    return acc;
}

int main(void) {
    const int passes = 20;
    printf("int<->float64 conversion loop, 30M iters x 20 passes\n");

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc += run();
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
