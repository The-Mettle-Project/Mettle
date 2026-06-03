/* C counterpart to saxpy.mettle - float64 y = a*x + y map. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#include "../bench_time.h"

#define N 65536

int main(void) {
    double *x = malloc((size_t)N * sizeof(double));
    double *y = malloc((size_t)N * sizeof(double));
    for (int i = 0; i < N; i++) {
        x[i] = (double)i * 0.25 + 1.0;
        y[i] = (double)(N - i) * 0.5;
    }

    double a = 2.5;
    const int passes = 3000;
    printf("float64 SAXPY y=a*x+y: 65536 elems x 3000 passes\n");

    uint64_t t0 = bench_time_us();
    for (int p = 0; p < passes; p++) {
        for (int j = 0; j < N; j++) {
            y[j] = a * x[j] + y[j];
        }
        a = a * 1.0000001;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", (int64_t)y[0] + (int64_t)y[N - 1]);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(x);
    free(y);
    return 0;
}
