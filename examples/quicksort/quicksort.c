/*
 * C quicksort benchmark - counterpart to quicksort.mettle
 *
 * Recursive quicksort (Lomuto partition) over 2048 int32 values, 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o quicksort_c.exe quicksort.c -lkernel32)
 * Run: quicksort_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

#define DATA_LEN 2048

static void fill_data(int32_t *data, int32_t seed) {
    for (int32_t i = 0; i < DATA_LEN; i++) {
        data[i] = (int32_t)((i * 1103515245 + seed + 12345) % 65536);
    }
}

static void quicksort(int32_t *data, int32_t lo, int32_t hi) {
    if (lo >= hi) {
        return;
    }
    int32_t pivot = data[hi];
    int32_t i = lo - 1;
    for (int32_t j = lo; j < hi; j++) {
        if (data[j] < pivot) {
            i++;
            int32_t tmp = data[i];
            data[i] = data[j];
            data[j] = tmp;
        }
    }
    int32_t tmp2 = data[i + 1];
    data[i + 1] = data[hi];
    data[hi] = tmp2;

    quicksort(data, lo, i);
    quicksort(data, i + 2, hi);
}

static int64_t weighted_sum(const int32_t *data, int32_t len) {
    int64_t sum = 0;
    for (int32_t i = 0; i < len; i++) {
        sum += (int64_t)data[i] * (int64_t)(i + 1);
    }
    return sum;
}

static int64_t sum_array(const int32_t *data, int32_t len) {
    int64_t sum = 0;
    for (int32_t i = 0; i < len; i++) {
        sum += (int64_t)data[i];
    }
    return sum;
}

int main(void) {
    const int passes = 200;
    int32_t data[DATA_LEN];
    int32_t scratch[DATA_LEN];

    fill_data(data, 42);

    printf("Quicksort: 2048 int32 values\n");

    memcpy(scratch, data, sizeof(data));
    quicksort(scratch, 0, DATA_LEN - 1);
    int64_t check = weighted_sum(scratch, DATA_LEN);
    printf("Sorted wsum = %" PRId64 "\n", check);

    printf("Benchmark: %d passes (quicksort)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        memcpy(scratch, data, sizeof(data));
        quicksort(scratch, 0, DATA_LEN - 1);
        bench_sum += sum_array(scratch, DATA_LEN);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    return 0;
}
