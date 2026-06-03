/* C counterpart to leaf_call.mettle - direct leaf call hot loop. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 60000000

static int64_t mix(int64_t a, int64_t b) {
    return (a * 1103515245 + 12345) ^ (b + (a >> 7));
}

static int64_t run(void) {
    int64_t acc = 1;
    for (int64_t i = 0; i < (int64_t)N; i++) {
        acc = mix(acc, i);
    }
    return acc;
}

int main(void) {
    const int passes = 5;
    printf("Direct leaf call, 60M iters x 5 passes\n");

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc ^= run();
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
