/* C counterpart to global_acc.mettle - global read-modify-write hot loop. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 100000000

int64_t g_a = 1;
int64_t g_b = 2;
int64_t g_c = 3;

static void run(void) {
    for (int64_t i = 0; i < (int64_t)N; i++) {
        g_a = g_a + i;
        g_b = g_b ^ (g_a >> 3);
        g_c = g_c + g_a - g_b;
    }
}

int main(void) {
    const int passes = 3;
    printf("Global RMW hot loop, 100M iters x 3 passes\n");

    uint64_t t0 = bench_time_us();
    for (int p = 0; p < passes; p++) {
        g_a = 1; g_b = 2; g_c = 3;
        run();
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", g_a ^ g_b ^ g_c);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
