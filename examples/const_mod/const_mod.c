/* C counterpart to const_mod.mettle - constant-divisor div/mod. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 20000000

static int64_t run(void) {
    int64_t acc = 0;
    int64_t x = 123456789;
    for (int64_t i = 0; i < (int64_t)N; i++) {
        acc = acc + (x % 7) + (x / 9) + (x % 100) + (x / 13);
        x = x + 2654435761;
    }
    return acc;
}

int main(void) {
    const int passes = 20;
    printf("Const-divisor div/mod, 20M iters x 20 passes\n");

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
