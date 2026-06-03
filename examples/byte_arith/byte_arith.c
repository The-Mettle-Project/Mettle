/* C counterpart to byte_arith.mettle - narrow uint8 buffer arithmetic. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#include "../bench_time.h"

#define N 262144

static int64_t run(uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) {
        buf[i] = buf[i] * 3 + 7;
        buf[i] = buf[i] ^ 0xA5;
    }
    int64_t acc = 0;
    for (int k = 0; k < n; k++) {
        acc += (int64_t)buf[k];
    }
    return acc;
}

int main(void) {
    uint8_t *buf = malloc((size_t)N);
    for (int i = 0; i < N; i++) {
        buf[i] = (uint8_t)(i & 255);
    }

    const int passes = 2000;
    printf("uint8 buffer arithmetic: 256 KB x 2000 passes\n");

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc ^= run(buf, N);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(buf);
    return 0;
}
