/*
 * C merge-sort benchmark - counterpart to merge_sort.mettle
 *
 * Recursive top-down merge sort over 2048 int32 values, 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o merge_sort_c.exe merge_sort.c -lkernel32)
 * Run: merge_sort_c.exe
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

static void merge(int32_t *data, int32_t *tmp, int32_t lo, int32_t mid, int32_t hi) {
    int32_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (data[i] <= data[j]) {
            tmp[k++] = data[i++];
        } else {
            tmp[k++] = data[j++];
        }
    }
    while (i < mid) { tmp[k++] = data[i++]; }
    while (j < hi) { tmp[k++] = data[j++]; }
    for (int32_t m = lo; m < hi; m++) {
        data[m] = tmp[m];
    }
}

static void merge_sort(int32_t *data, int32_t *tmp, int32_t lo, int32_t hi) {
    if (hi - lo <= 1) {
        return;
    }
    int32_t mid = lo + (hi - lo) / 2;
    merge_sort(data, tmp, lo, mid);
    merge_sort(data, tmp, mid, hi);
    merge(data, tmp, lo, mid, hi);
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
    int32_t tmp[DATA_LEN];

    fill_data(data, 42);

    printf("Merge sort: 2048 int32 values\n");

    memcpy(scratch, data, sizeof(data));
    merge_sort(scratch, tmp, 0, DATA_LEN);
    int64_t check = weighted_sum(scratch, DATA_LEN);
    printf("Sorted wsum = %" PRId64 "\n", check);

    printf("Benchmark: %d passes (merge_sort)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        memcpy(scratch, data, sizeof(data));
        merge_sort(scratch, tmp, 0, DATA_LEN);
        bench_sum += sum_array(scratch, DATA_LEN);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    return 0;
}
