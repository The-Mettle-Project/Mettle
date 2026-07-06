/*
 * C heapsort benchmark - counterpart to heapsort.mettle
 *
 * In-place binary-heap sort over 2048 int32 values, 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o heapsort_c.exe heapsort.c -lkernel32)
 * Run: heapsort_c.exe
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

static void sift_down(int32_t *data, int32_t start, int32_t end) {
    int32_t root = start;
    while (root * 2 + 1 <= end) {
        int32_t child = root * 2 + 1;
        int32_t swap_idx = root;

        if (data[swap_idx] < data[child]) {
            swap_idx = child;
        }
        if (child + 1 <= end && data[swap_idx] < data[child + 1]) {
            swap_idx = child + 1;
        }
        if (swap_idx == root) {
            return;
        }

        int32_t tmp = data[root];
        data[root] = data[swap_idx];
        data[swap_idx] = tmp;
        root = swap_idx;
    }
}

static void heapsort(int32_t *data, int32_t len) {
    for (int32_t start = (len - 2) / 2; start >= 0; start--) {
        sift_down(data, start, len - 1);
    }

    for (int32_t end = len - 1; end > 0; end--) {
        int32_t tmp = data[0];
        data[0] = data[end];
        data[end] = tmp;
        sift_down(data, 0, end - 1);
    }
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

    printf("Heapsort: 2048 int32 values\n");

    memcpy(scratch, data, sizeof(data));
    heapsort(scratch, DATA_LEN);
    int64_t check = weighted_sum(scratch, DATA_LEN);
    printf("Sorted wsum = %" PRId64 "\n", check);

    printf("Benchmark: %d passes (heapsort)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        memcpy(scratch, data, sizeof(data));
        heapsort(scratch, DATA_LEN);
        bench_sum += sum_array(scratch, DATA_LEN);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    return 0;
}
