/*
 * C integer division/modulo benchmark - counterpart to int_divmod.mettle.
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 2000000

static uint64_t divmod_sum(void) {
    uint64_t acc = 0;
    uint64_t x = 1469598103934665603ULL;
    for (uint64_t i = 1; i <= (uint64_t)N; i++) {
        uint64_t d = (i & 1023) + 3;
        acc += x % d;
        acc += x / d;
        x += 1099511628211ULL;
    }
    return acc;
}

int main(void) {
    const int passes = 40;
    printf("64-bit div+mod, 2M iters x 40 passes\n");

    uint64_t t0 = bench_time_us();
    uint64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc += divmod_sum();
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", (int64_t)acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
