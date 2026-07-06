/*
 * C CRC32 benchmark - counterpart to crc32.mettle
 *
 * CRC-32 (bit-by-bit, IEEE 802.3 polynomial) over a 256 KB buffer, 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o crc32_c.exe crc32.c -lkernel32)
 * Run: crc32_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

static uint32_t crc32_calc(const unsigned char *buf, int64_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int64_t i = 0; i < len; i++) {
        crc ^= (uint32_t)buf[i];
        for (int k = 0; k < 8; k++) {
            uint32_t mask = (uint32_t)(0 - (crc & 1));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

int main(void) {
    const int64_t buf_size = 262144;
    const char *template = "a b c d e f g h ";
    const size_t template_len = 16;

    unsigned char *buf = (unsigned char *)malloc((size_t)buf_size);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t pos = 0; pos < buf_size; pos += (int64_t)template_len) {
        size_t chunk = (size_t)(buf_size - pos);
        if (chunk > template_len) {
            chunk = template_len;
        }
        memcpy(buf + pos, template, chunk);
    }

    printf("CRC32: 256 KB buffer\n");

    uint32_t check = crc32_calc(buf, buf_size);
    printf("CRC = %" PRId64 "\n", (int64_t)check);

    const int passes = 200;
    printf("Benchmark: %d passes (crc32)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += (int64_t)crc32_calc(buf, buf_size);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(buf);
    return 0;
}
