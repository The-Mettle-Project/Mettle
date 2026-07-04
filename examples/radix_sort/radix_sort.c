/*
 * C radix-sort benchmark - counterpart to radix_sort.mettle
 *
 * LSD radix sort (4 passes of 8-bit digits) over 4096 uint32 values,
 * 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o radix_sort_c.exe radix_sort.c -lkernel32)
 * Run: radix_sort_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

#define DATA_LEN 4096

static void fill_data(uint32_t *data, uint32_t seed) {
    uint32_t state = seed;
    for (int32_t i = 0; i < DATA_LEN; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[i] = state;
    }
}

static void radix_pass(const uint32_t *src, uint32_t *dst, int32_t len,
                        int32_t shift, int32_t *count) {
    memset(count, 0, 256 * sizeof(int32_t));

    for (int32_t i = 0; i < len; i++) {
        int32_t digit = (int32_t)((src[i] >> shift) & 255);
        count[digit]++;
    }

    int32_t total = 0;
    for (int32_t i = 0; i < 256; i++) {
        int32_t c = count[i];
        count[i] = total;
        total += c;
    }

    for (int32_t i = 0; i < len; i++) {
        int32_t digit = (int32_t)((src[i] >> shift) & 255);
        dst[count[digit]++] = src[i];
    }
}

static void radix_sort(uint32_t *data, uint32_t *tmp, int32_t len, int32_t *count) {
    radix_pass(data, tmp, len, 0, count);
    radix_pass(tmp, data, len, 8, count);
    radix_pass(data, tmp, len, 16, count);
    radix_pass(tmp, data, len, 24, count);
}

static int64_t weighted_sum(const uint32_t *data, int32_t len) {
    int64_t sum = 0;
    for (int32_t i = 0; i < len; i++) {
        sum += (int64_t)data[i] * (int64_t)(i % 7 + 1);
    }
    return sum;
}

static int64_t sum_array(const uint32_t *data, int32_t len) {
    int64_t sum = 0;
    for (int32_t i = 0; i < len; i++) {
        sum += (int64_t)data[i];
    }
    return sum;
}

int main(void) {
    const int passes = 200;
    uint32_t data[DATA_LEN];
    uint32_t scratch[DATA_LEN];
    uint32_t tmp[DATA_LEN];
    int32_t count[256];

    fill_data(data, 88172645u);

    printf("Radix sort: 4096 uint32 values\n");

    memcpy(scratch, data, sizeof(data));
    radix_sort(scratch, tmp, DATA_LEN, count);
    int64_t check = weighted_sum(scratch, DATA_LEN);
    printf("Sorted wsum = %" PRId64 "\n", check);

    printf("Benchmark: %d passes (radix_sort)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        memcpy(scratch, data, sizeof(data));
        radix_sort(scratch, tmp, DATA_LEN, count);
        bench_sum += sum_array(scratch, DATA_LEN);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    return 0;
}
