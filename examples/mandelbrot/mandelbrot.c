/*
 * C Mandelbrot benchmark - counterpart to mandelbrot.mettle.
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define W 600
#define H 400
#define MAXIT 256

static int64_t mandel_sum(void) {
    int64_t total = 0;
    for (int py = 0; py < H; py++) {
        double y0 = ((double)py / (double)H) * 2.0 - 1.0;
        for (int px = 0; px < W; px++) {
            double x0 = ((double)px / (double)W) * 3.0 - 2.0;
            double x = 0.0, y = 0.0;
            int it = 0;
            while (it < MAXIT) {
                double x2 = x * x;
                double y2 = y * y;
                if (x2 + y2 > 4.0) {
                    it = MAXIT;
                } else {
                    double xt = x2 - y2 + x0;
                    y = 2.0 * x * y + y0;
                    x = xt;
                    it = it + 1;
                    total = total + 1;
                }
            }
        }
    }
    return total;
}

int main(void) {
    const int passes = 30;
    printf("Mandelbrot 600x400, 256 iters x 30 passes\n");

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc += mandel_sum();
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
