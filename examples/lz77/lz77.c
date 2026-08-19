/*
 * C LZ77 compressor benchmark - counterpart to lz77.mettle
 *
 * The match finder a real deflate-class compressor uses: a 3-byte rolling hash
 * into a head table, hash chains through a 32 KB window, and a bounded chain
 * walk that compares candidate strings byte by byte. Compresses a 256 KB
 * redundant buffer, decompresses it back with overlapping copies, and verifies.
 *
 * Build: build.bat (or: gcc -O3 -o lz77_c.exe lz77.c -lkernel32)
 * Run: lz77_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define SRC_TARGET 262144
#define SRC_CAP 327680
#define OUT_CAP 786432
#define WINDOW 32768
#define HASH_SIZE 65536
#define MIN_MATCH 3
#define MAX_MATCH 258
#define MAX_CHAIN 32
#define PASSES 4

typedef struct {
    int32_t *head;
    int32_t *prev;
    int32_t literals;
    int32_t matches;
    int32_t longest;
} Lz;

static int32_t make_word(uint8_t *dst, int32_t pos, int32_t idx) {
    int32_t len = 3 + idx % 6;
    int32_t v = idx;
    int32_t k = 0;
    while (k < len) {
        dst[pos + k] = (uint8_t)(97 + v % 26);
        v = v / 26 + k * 5 + 2;
        k += 1;
    }
    return pos + len;
}

static int32_t gen_source(uint8_t *dst) {
    uint32_t state = 88675123u;
    int32_t pos = 0;
    while (pos < SRC_TARGET) {
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        int32_t roll = (int32_t)(state % 100);
        if (roll < 12 && pos > 4096) {
            int32_t from = (int32_t)(state % (uint32_t)(pos - 1024));
            int32_t chunk = 16 + (int32_t)(state % 112);
            if (pos + chunk > SRC_TARGET) {
                chunk = SRC_TARGET - pos;
            }
            int32_t k = 0;
            while (k < chunk) {
                dst[pos + k] = dst[from + k];
                k += 1;
            }
            pos += chunk;
        } else {
            pos = make_word(dst, pos, (int32_t)(state % 1024));
            dst[pos] = 32;
            if (roll > 95) {
                dst[pos] = 10;
            }
            pos += 1;
        }
    }
    return pos;
}

static int32_t hash3(const uint8_t *src, int32_t pos) {
    int32_t a = (int32_t)src[pos];
    int32_t b = (int32_t)src[pos + 1];
    int32_t c = (int32_t)src[pos + 2];
    return ((a << 10) ^ (b << 5) ^ c) & (HASH_SIZE - 1);
}

static int32_t compress(Lz *z, const uint8_t *src, int32_t len, uint8_t *out) {
    int32_t i = 0;
    while (i < HASH_SIZE) {
        z->head[i] = -1;
        i += 1;
    }
    i = 0;
    while (i < WINDOW) {
        z->prev[i] = -1;
        i += 1;
    }
    z->literals = 0;
    z->matches = 0;
    z->longest = 0;

    int32_t mask = WINDOW - 1;
    int32_t opos = 0;
    int32_t pos = 0;
    while (pos < len) {
        int32_t best_len = 0;
        int32_t best_dist = 0;
        if (pos + MIN_MATCH <= len) {
            int32_t limit = len - pos;
            if (limit > MAX_MATCH) {
                limit = MAX_MATCH;
            }
            int32_t h = hash3(src, pos);
            int32_t cand = z->head[h];
            int32_t chain = 0;
            while (cand >= 0 && chain < MAX_CHAIN) {
                int32_t dist = pos - cand;
                if (dist <= 0 || dist > WINDOW) {
                    break;
                }
                int32_t run = 0;
                while (run < limit && src[cand + run] == src[pos + run]) {
                    run += 1;
                }
                if (run > best_len) {
                    best_len = run;
                    best_dist = dist;
                    if (run >= limit) {
                        break;
                    }
                }
                cand = z->prev[cand & mask];
                chain += 1;
            }
        }

        if (best_len >= MIN_MATCH) {
            out[opos] = 1;
            out[opos + 1] = (uint8_t)(best_dist & 255);
            out[opos + 2] = (uint8_t)((best_dist >> 8) & 255);
            out[opos + 3] = (uint8_t)(best_len - MIN_MATCH);
            opos += 4;
            z->matches += 1;
            if (best_len > z->longest) {
                z->longest = best_len;
            }
            int32_t k = 0;
            while (k < best_len) {
                if (pos + k + MIN_MATCH <= len) {
                    int32_t hh = hash3(src, pos + k);
                    z->prev[(pos + k) & mask] = z->head[hh];
                    z->head[hh] = pos + k;
                }
                k += 1;
            }
            pos += best_len;
        } else {
            out[opos] = 0;
            out[opos + 1] = src[pos];
            opos += 2;
            z->literals += 1;
            if (pos + MIN_MATCH <= len) {
                int32_t hh = hash3(src, pos);
                z->prev[pos & mask] = z->head[hh];
                z->head[hh] = pos;
            }
            pos += 1;
        }
    }
    return opos;
}

static int32_t decompress(const uint8_t *packed, int32_t packed_len, uint8_t *out) {
    int32_t ipos = 0;
    int32_t opos = 0;
    while (ipos < packed_len) {
        if (packed[ipos] == 0) {
            out[opos] = packed[ipos + 1];
            opos += 1;
            ipos += 2;
        } else {
            int32_t dist = (int32_t)packed[ipos + 1] | ((int32_t)packed[ipos + 2] << 8);
            int32_t run = (int32_t)packed[ipos + 3] + MIN_MATCH;
            int32_t from = opos - dist;
            int32_t k = 0;
            while (k < run) {
                out[opos + k] = out[from + k];
                k += 1;
            }
            opos += run;
            ipos += 4;
        }
    }
    return opos;
}

static uint64_t roundtrip(Lz *z, const uint8_t *src, int32_t len, uint8_t *packed, uint8_t *plain,
                          int32_t *out_packed, int32_t *out_mismatch) {
    int32_t packed_len = compress(z, src, len, packed);
    int32_t produced = decompress(packed, packed_len, plain);

    int32_t mismatch = 0;
    int32_t i = 0;
    while (i < len) {
        if (plain[i] != src[i]) {
            mismatch += 1;
        }
        i += 1;
    }
    if (produced != len) {
        mismatch += 1;
    }

    uint64_t h = 14695981039346656037ULL;
    i = 0;
    while (i < packed_len) {
        h = (h ^ (uint64_t)packed[i]) * 1099511628211ULL;
        i += 1;
    }
    h = h * 31 + (uint64_t)(z->matches * 7 + z->literals);
    *out_packed = packed_len;
    *out_mismatch = mismatch;
    return h;
}

int main(void) {
    uint8_t *src = (uint8_t *)malloc(SRC_CAP);
    uint8_t *packed = (uint8_t *)malloc(OUT_CAP);
    uint8_t *plain = (uint8_t *)malloc(SRC_CAP);
    int32_t *head = (int32_t *)malloc((size_t)HASH_SIZE * 4);
    int32_t *prev = (int32_t *)malloc((size_t)WINDOW * 4);
    if (src == NULL || packed == NULL || plain == NULL || head == NULL || prev == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    int32_t src_len = gen_source(src);

    Lz z;
    z.head = head;
    z.prev = prev;
    z.literals = 0;
    z.matches = 0;
    z.longest = 0;

    printf("LZ77: %d byte buffer, %d byte window, chain limit %d\n", src_len, WINDOW, MAX_CHAIN);

    int32_t packed_len = 0;
    int32_t mismatch = 0;
    uint64_t check = roundtrip(&z, src, src_len, packed, plain, &packed_len, &mismatch);
    printf("Packed = %d bytes, literals = %d, matches = %d, longest = %d\n",
           packed_len, z.literals, z.matches, z.longest);
    printf("Mismatches = %d checksum = %" PRIu64 "\n", mismatch, check);

    printf("Benchmark: %d passes (compress + decompress + verify)\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int64_t total_packed = 0;
    int64_t total_mismatch = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int32_t pass_packed = 0;
        int32_t pass_mismatch = 0;
        bench_hash = bench_hash * 1000003 + roundtrip(&z, src, src_len, packed, plain, &pass_packed, &pass_mismatch);
        total_packed = total_packed + (int64_t)pass_packed;
        total_mismatch = total_mismatch + (int64_t)pass_mismatch;
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Packed bytes = %" PRId64 " mismatches = %" PRId64 "\n", total_packed, total_mismatch);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(src);
    free(packed);
    free(plain);
    free(head);
    free(prev);
    return 0;
}
