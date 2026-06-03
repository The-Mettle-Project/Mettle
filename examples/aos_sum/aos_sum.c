/* C counterpart to aos_sum.mettle - array-of-structs field walk. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#include "../bench_time.h"

#define N 65536

typedef struct {
    int64_t x, y, vx, vy;
} Body;

static int64_t step(Body *b, int n) {
    int64_t acc = 0;
    for (int i = 0; i < n; i++) {
        b[i].x = b[i].x + b[i].vx;
        b[i].y = b[i].y + b[i].vy;
        acc = acc + b[i].x ^ b[i].y;
    }
    return acc;
}

int main(void) {
    Body *bodies = malloc((size_t)N * sizeof(Body));
    for (int i = 0; i < N; i++) {
        bodies[i].x = (int64_t)i;
        bodies[i].y = (int64_t)(N - i);
        bodies[i].vx = (int64_t)(i & 7) - 3;
        bodies[i].vy = (int64_t)(i & 15) - 7;
    }

    const int passes = 2000;
    printf("Array-of-structs field walk: 65536 bodies x 2000 passes\n");

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc ^= step(bodies, N);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(bodies);
    return 0;
}
