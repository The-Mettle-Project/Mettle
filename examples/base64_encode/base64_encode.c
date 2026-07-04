/*
 * C base64-encode benchmark - counterpart to base64_encode.mettle
 *
 * Base64-encodes a 256 KB buffer into a scratch output buffer, 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o base64_encode_c.exe base64_encode.c -lkernel32)
 * Run: base64_encode_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

static const char ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int64_t base64_encode(const unsigned char *src, int64_t len, char *dst) {
    int64_t i = 0, o = 0;
    while (i + 3 <= len) {
        int b0 = src[i], b1 = src[i + 1], b2 = src[i + 2];
        dst[o]     = ALPHABET[(b0 >> 2) & 63];
        dst[o + 1] = ALPHABET[((b0 << 4) | (b1 >> 4)) & 63];
        dst[o + 2] = ALPHABET[((b1 << 2) | (b2 >> 6)) & 63];
        dst[o + 3] = ALPHABET[b2 & 63];
        i += 3;
        o += 4;
    }
    int64_t rem = len - i;
    if (rem == 1) {
        int c0 = src[i];
        dst[o]     = ALPHABET[(c0 >> 2) & 63];
        dst[o + 1] = ALPHABET[(c0 << 4) & 63];
        dst[o + 2] = '=';
        dst[o + 3] = '=';
        o += 4;
    } else if (rem == 2) {
        int d0 = src[i], d1 = src[i + 1];
        dst[o]     = ALPHABET[(d0 >> 2) & 63];
        dst[o + 1] = ALPHABET[((d0 << 4) | (d1 >> 4)) & 63];
        dst[o + 2] = ALPHABET[(d1 << 2) & 63];
        dst[o + 3] = '=';
        o += 4;
    }
    return o;
}

int main(void) {
    const int64_t buf_size = 262144;
    const int64_t out_size = 349528;
    const char *template = "a b c d e f g h ";
    const size_t template_len = 16;

    unsigned char *buf = (unsigned char *)malloc((size_t)buf_size);
    char *out = (char *)malloc((size_t)out_size);
    if (!buf || !out) {
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

    printf("Base64 encode: 256 KB buffer\n");

    int64_t out_len = base64_encode(buf, buf_size, out);
    printf("Encoded len = %" PRId64 "\n", out_len);

    const int passes = 200;
    printf("Benchmark: %d passes (base64_encode)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += base64_encode(buf, buf_size, out);
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
