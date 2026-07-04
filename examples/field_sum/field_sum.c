/*
 * C field-selective walk - counterpart to field_sum.mettle
 *
 * A pool of 1,048,576 records, each 32 bytes wide (eight int32 fields). The
 * hot loop sums only field 0 from every record, 100 passes. Array-of-structs
 * layout means each 4-byte read pulls a full record's cache line. GCC -O3
 * keeps base+offset addressing tight but cannot change the heap layout, so it
 * pays the same 8x memory-traffic overhead the Mettle SoA transform removes.
 *
 * Build: build.bat (or: gcc -O3 -o field_sum_c.exe field_sum.c -lkernel32)
 * Run: field_sum_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define REC_COUNT 1048576

typedef struct {
    int32_t f0;
    int32_t f1;
    int32_t f2;
    int32_t f3;
    int32_t f4;
    int32_t f5;
    int32_t f6;
    int32_t f7;
} Record;

static void build_records(Record *recs, int32_t count) {
    uint32_t state = 2463534242u;
    for (int32_t i = 0; i < count; i++) {
        recs[i].f0 = i & 4095;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        recs[i].f1 = (int32_t)(state & 65535u);
        recs[i].f2 = (int32_t)(state >> 16);
        recs[i].f3 = i * 3;
        recs[i].f4 = i - count;
        recs[i].f5 = (i ^ 12345) & 4095;
        recs[i].f6 = i + 7;
        recs[i].f7 = (int32_t)(state ^ (uint32_t)i);
    }
}

static int64_t sum_field0(const Record *recs, int32_t count) {
    int64_t total = 0;
    for (int32_t i = 0; i < count; i++) {
        total += (int64_t)recs[i].f0;
    }
    return total;
}

int main(void) {
    int32_t count = REC_COUNT;
    Record *pool = (Record *)malloc((size_t)count * sizeof(Record));
    if (!pool) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    build_records(pool, count);

    printf("Field-selective AoS walk: 1048576 records (32B), field 0 only\n");

    int64_t check = sum_field0(pool, count);
    printf("Checksum = %" PRId64 "\n", check);

    const int passes = 100;
    printf("Benchmark: %d passes (field_sum)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += sum_field0(pool, count);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(pool);
    return 0;
}
