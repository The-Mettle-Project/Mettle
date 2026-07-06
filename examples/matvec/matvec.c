/*
 * C matrix-vector multiply benchmark - counterpart to matvec.mettle
 *
 * float64 512x512 matrix times a 512-element vector, 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o matvec_c.exe matvec.c -lkernel32)
 * Run: matvec_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define DIM 512

static void matvec(const double *mat, const double *vec, double *out, int64_t dim) {
    for (int64_t i = 0; i < dim; i++) {
        double sum = 0.0;
        int64_t base = i * dim;
        for (int64_t j = 0; j < dim; j++) {
            sum += mat[base + j] * vec[j];
        }
        out[i] = sum;
    }
}

int main(void) {
    int64_t dim = DIM;
    const int passes = 200;

    double *mat = (double *)malloc((size_t)(dim * dim) * sizeof(double));
    double *vec = (double *)malloc((size_t)dim * sizeof(double));
    double *out = (double *)malloc((size_t)dim * sizeof(double));
    if (!mat || !vec || !out) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t i = 0; i < dim; i++) {
        vec[i] = (double)(i % 17) * 0.5 + 1.0;
        for (int64_t j = 0; j < dim; j++) {
            mat[i * dim + j] = (double)((i + j) % 13) * 0.25;
        }
    }

    printf("Matrix-vector multiply: 512x512 float64 x 200 passes\n");

    uint64_t t0 = bench_time_us();
    double bench_sum = 0.0;
    for (int p = 0; p < passes; p++) {
        matvec(mat, vec, out, dim);
        bench_sum += out[0] + out[dim - 1];
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", (int64_t)bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(mat);
    free(vec);
    free(out);
    return 0;
}
