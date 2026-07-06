/*
 * C run-length-encode benchmark - counterpart to rle_encode.mettle
 *
 * RLE-encodes a 256 KB buffer of runs into a scratch output buffer,
 * 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o rle_encode_c.exe rle_encode.c -lkernel32)
 * Run: rle_encode_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

static int64_t rle_encode(const unsigned char *src, int64_t len, unsigned char *dst) {
    int64_t i = 0, o = 0;
    while (i < len) {
        unsigned char b = src[i];
        int64_t run = 1;
        while (i + run < len && run < 255 && src[i + run] == b) {
            run++;
        }
        dst[o] = b;
        dst[o + 1] = (unsigned char)run;
        o += 2;
        i += run;
    }
    return o;
}

int main(void) {
    const int64_t buf_size = 262144;

    unsigned char *buf = (unsigned char *)malloc((size_t)buf_size);
    unsigned char *out = (unsigned char *)malloc((size_t)buf_size * 2);
    if (!buf || !out) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    uint32_t state = 2463534242u;
    int64_t pos = 0;
    while (pos < buf_size) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        int64_t run = 1 + (int64_t)(state % 40);
        unsigned char val = (unsigned char)(state % 8);
        for (int64_t k = 0; k < run && pos < buf_size; k++) {
            buf[pos++] = val;
        }
    }

    printf("RLE encode: 256 KB buffer\n");

    int64_t out_len = rle_encode(buf, buf_size, out);
    printf("Encoded len = %" PRId64 "\n", out_len);

    const int passes = 200;
    printf("Benchmark: %d passes (rle_encode)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += rle_encode(buf, buf_size, out);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(buf);
    free(out);
    return 0;
}
