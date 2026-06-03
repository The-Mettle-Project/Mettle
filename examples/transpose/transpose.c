/* C counterpart to transpose.mettle - 256x256 int32 transpose. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#include "../bench_time.h"

#define N 256

static void transpose(const int32_t *a, int32_t *b) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            b[j * N + i] = a[i * N + j];
        }
    }
}

int main(void) {
    int32_t *a = malloc((size_t)N * N * 4);
    int32_t *b = malloc((size_t)N * N * 4);
    for (int k = 0; k < N * N; k++) {
        a[k] = k * 2654435761u;
    }

    const int passes = 4000;
    printf("256x256 int32 transpose x 4000 passes\n");

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        transpose(a, b);
        acc = acc + (int64_t)b[(N - 1) * N + 1] + (int64_t)b[N + (N - 1)];
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(a);
    free(b);
    return 0;
}
