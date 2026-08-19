/*
 * C Huffman codec benchmark - counterpart to huffman.mettle
 *
 * Histograms a 128 KB skewed buffer, builds the Huffman tree through a binary
 * min-heap, assigns codes by walking the tree, bit-packs the input, then
 * decodes the bitstream one bit at a time and verifies the roundtrip.
 *
 * Build: build.bat (or: gcc -O3 -o huffman_c.exe huffman.c -lkernel32)
 * Run: huffman_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define SRC_LEN 131072
#define OUT_CAP 262144
#define NODE_CAP 512
#define PASSES 6

typedef struct {
    int64_t freq;
    int32_t left;
    int32_t right;
} HNode;

typedef struct {
    HNode *nodes;
    int32_t node_count;
    int32_t root;
    int32_t *heap;
    int32_t heap_size;
    uint32_t *code_bits;
    int32_t *code_len;
    int32_t max_len;
} Codec;

static void gen_source(uint8_t *dst) {
    uint32_t state = 1103515245u;
    int32_t i = 0;
    while (i < SRC_LEN) {
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        uint32_t roll = state % 1000;
        uint32_t value = 0;
        if (roll < 400) {
            value = state % 8;
        } else if (roll < 700) {
            value = 8u + state % 24;
        } else if (roll < 900) {
            value = 32u + state % 64;
        } else {
            value = state % 256;
        }
        dst[i] = (uint8_t)value;
        i += 1;
    }
}

static int32_t heap_less(Codec *c, int32_t a, int32_t b) {
    if (c->nodes[a].freq < c->nodes[b].freq) {
        return 1;
    }
    if (c->nodes[a].freq > c->nodes[b].freq) {
        return 0;
    }
    if (a < b) {
        return 1;
    }
    return 0;
}

static void heap_push(Codec *c, int32_t node) {
    int32_t i = c->heap_size;
    c->heap[i] = node;
    c->heap_size += 1;
    while (i > 0) {
        int32_t parent = (i - 1) / 2;
        if (heap_less(c, c->heap[i], c->heap[parent]) == 0) {
            break;
        }
        int32_t tmp = c->heap[i];
        c->heap[i] = c->heap[parent];
        c->heap[parent] = tmp;
        i = parent;
    }
}

static int32_t heap_pop(Codec *c) {
    int32_t top = c->heap[0];
    c->heap_size -= 1;
    c->heap[0] = c->heap[c->heap_size];
    int32_t i = 0;
    for (;;) {
        int32_t left = i * 2 + 1;
        int32_t right = left + 1;
        int32_t best = i;
        if (left < c->heap_size && heap_less(c, c->heap[left], c->heap[best]) != 0) {
            best = left;
        }
        if (right < c->heap_size && heap_less(c, c->heap[right], c->heap[best]) != 0) {
            best = right;
        }
        if (best == i) {
            break;
        }
        int32_t tmp = c->heap[i];
        c->heap[i] = c->heap[best];
        c->heap[best] = tmp;
        i = best;
    }
    return top;
}

static void build_tree(Codec *c, const int64_t *freq) {
    c->node_count = 256;
    c->heap_size = 0;
    int32_t s = 0;
    while (s < 256) {
        c->nodes[s].freq = freq[s];
        c->nodes[s].left = -1;
        c->nodes[s].right = -1;
        if (freq[s] > 0) {
            heap_push(c, s);
        }
        s += 1;
    }
    while (c->heap_size > 1) {
        int32_t a = heap_pop(c);
        int32_t b = heap_pop(c);
        int32_t idx = c->node_count;
        c->node_count += 1;
        c->nodes[idx].freq = c->nodes[a].freq + c->nodes[b].freq;
        c->nodes[idx].left = a;
        c->nodes[idx].right = b;
        heap_push(c, idx);
    }
    c->root = heap_pop(c);
}

static void assign_codes(Codec *c, int32_t node, uint32_t bits, int32_t len) {
    if (c->nodes[node].left < 0) {
        c->code_bits[node] = bits;
        c->code_len[node] = len;
        if (len > c->max_len) {
            c->max_len = len;
        }
        return;
    }
    assign_codes(c, c->nodes[node].left, bits << 1, len + 1);
    assign_codes(c, c->nodes[node].right, (bits << 1) | 1, len + 1);
}

static int64_t encode(Codec *c, const uint8_t *src, int32_t len, uint8_t *out) {
    uint32_t acc = 0;
    int32_t nbits = 0;
    int32_t opos = 0;
    int64_t total = 0;
    int32_t i = 0;
    while (i < len) {
        int32_t sym = (int32_t)src[i];
        uint32_t code = c->code_bits[sym];
        int32_t clen = c->code_len[sym];
        total = total + (int64_t)clen;
        while (clen > 0) {
            clen -= 1;
            acc = (acc << 1) | ((code >> (uint32_t)clen) & 1);
            nbits += 1;
            if (nbits == 8) {
                out[opos] = (uint8_t)acc;
                opos += 1;
                nbits = 0;
                acc = 0;
            }
        }
        i += 1;
    }
    if (nbits > 0) {
        out[opos] = (uint8_t)(acc << (uint32_t)(8 - nbits));
    }
    return total;
}

static int32_t decode(Codec *c, const uint8_t *packed, int64_t bit_count, uint8_t *out) {
    int32_t node = c->root;
    int32_t produced = 0;
    int64_t bit = 0;
    while (bit < bit_count) {
        int32_t byte = (int32_t)packed[(int32_t)(bit >> 3)];
        int32_t value = (byte >> (7 - (int32_t)(bit & 7))) & 1;
        if (value != 0) {
            node = c->nodes[node].right;
        } else {
            node = c->nodes[node].left;
        }
        if (c->nodes[node].left < 0) {
            out[produced] = (uint8_t)node;
            produced += 1;
            node = c->root;
        }
        bit += 1;
    }
    return produced;
}

static uint64_t roundtrip(Codec *c, const uint8_t *src, uint8_t *packed, uint8_t *plain,
                          int64_t *freq, int64_t *out_bits, int32_t *out_mismatch) {
    int32_t s = 0;
    while (s < 256) {
        freq[s] = 0;
        c->code_bits[s] = 0;
        c->code_len[s] = 0;
        s += 1;
    }
    int32_t i = 0;
    while (i < SRC_LEN) {
        freq[(int32_t)src[i]] = freq[(int32_t)src[i]] + 1;
        i += 1;
    }
    c->max_len = 0;
    build_tree(c, freq);
    assign_codes(c, c->root, 0, 0);

    int64_t bits = encode(c, src, SRC_LEN, packed);
    int32_t produced = decode(c, packed, bits, plain);

    int32_t mismatch = 0;
    i = 0;
    while (i < SRC_LEN) {
        if (plain[i] != src[i]) {
            mismatch += 1;
        }
        i += 1;
    }
    if (produced != SRC_LEN) {
        mismatch += 1;
    }

    uint64_t h = 14695981039346656037ULL;
    s = 0;
    while (s < 256) {
        h = h * 1000003 + (uint64_t)(c->code_len[s] * (s + 1));
        h = h ^ (uint64_t)c->code_bits[s];
        s += 1;
    }
    h = h * 31 + (uint64_t)bits;
    *out_bits = bits;
    *out_mismatch = mismatch;
    return h;
}

int main(void) {
    uint8_t *src = (uint8_t *)malloc(SRC_LEN);
    uint8_t *packed = (uint8_t *)malloc(OUT_CAP);
    uint8_t *plain = (uint8_t *)malloc(SRC_LEN + 8);
    int64_t *freq = (int64_t *)malloc(256 * 8);
    HNode *nodes = (HNode *)malloc((size_t)NODE_CAP * sizeof(HNode));
    int32_t *heap = (int32_t *)malloc((size_t)NODE_CAP * 4);
    uint32_t *code_bits = (uint32_t *)malloc(256 * 4);
    int32_t *code_len = (int32_t *)malloc(256 * 4);
    if (src == NULL || packed == NULL || plain == NULL || freq == NULL || nodes == NULL ||
        heap == NULL || code_bits == NULL || code_len == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    gen_source(src);

    Codec c;
    c.nodes = nodes;
    c.node_count = 0;
    c.root = -1;
    c.heap = heap;
    c.heap_size = 0;
    c.code_bits = code_bits;
    c.code_len = code_len;
    c.max_len = 0;

    printf("Huffman codec: %d byte buffer\n", SRC_LEN);

    int64_t bits = 0;
    int32_t mismatch = 0;
    uint64_t check = roundtrip(&c, src, packed, plain, freq, &bits, &mismatch);
    int64_t packed_bytes = (bits + 7) / 8;
    printf("Packed = %" PRId64 " bytes, longest code = %d bits, mismatches = %d\n",
           packed_bytes, c.max_len, mismatch);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes (histogram + tree + encode + decode)\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int64_t total_bits = 0;
    int64_t total_mismatch = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int64_t pass_bits = 0;
        int32_t pass_mismatch = 0;
        bench_hash = bench_hash * 1000003 + roundtrip(&c, src, packed, plain, freq, &pass_bits, &pass_mismatch);
        total_bits = total_bits + pass_bits;
        total_mismatch = total_mismatch + (int64_t)pass_mismatch;
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Bits coded = %" PRId64 " mismatches = %" PRId64 "\n", total_bits, total_mismatch);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(src);
    free(packed);
    free(plain);
    free(freq);
    free(nodes);
    free(heap);
    free(code_bits);
    free(code_len);
    return 0;
}
