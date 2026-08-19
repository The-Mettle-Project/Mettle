/*
 * C word-frequency benchmark - counterpart to word_freq.mettle
 *
 * Generates a 1 MB document over a skewed 4096-word vocabulary, tokenizes it,
 * counts every word in an open-addressing hash map that grows and rehashes as
 * it fills, then selects the sixteen most frequent words.
 *
 * Build: build.bat (or: gcc -O3 -o word_freq_c.exe word_freq.c -lkernel32)
 * Run: word_freq_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define VOCAB 4096
#define DOC_CAP 1310720
#define DOC_TARGET 1048576
#define INITIAL_CAP 256
#define MAX_CAP 65536
#define TOP_K 16
#define PASSES 15

typedef struct {
    int32_t *key_off;
    int32_t *key_len;
    uint32_t *hashes;
    int32_t *counts;
    int32_t cap;
    int32_t used;
} Table;

static int32_t make_word(uint8_t *dst, int32_t pos, int32_t idx) {
    int32_t len = 3 + idx % 7;
    int32_t v = idx;
    int32_t k = 0;
    while (k < len) {
        dst[pos + k] = (uint8_t)(97 + v % 26);
        v = v / 26 + k * 7 + 3;
        k += 1;
    }
    return pos + len;
}

static int32_t gen_doc(uint8_t *dst) {
    uint32_t state = 2463534242u;
    int32_t pos = 0;
    int32_t column = 0;
    while (pos < DOC_TARGET) {
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        int32_t roll = (int32_t)(state % 1000);
        int32_t word = 0;
        if (roll < 500) {
            word = (int32_t)(state % 32);
        } else if (roll < 850) {
            word = (int32_t)(state % 512);
        } else {
            word = (int32_t)(state % (uint32_t)VOCAB);
        }
        pos = make_word(dst, pos, word);
        if (roll % 17 == 0) {
            dst[pos] = 46;
            pos += 1;
        } else if (roll % 23 == 0) {
            dst[pos] = 44;
            pos += 1;
        }
        column += 1;
        if (column >= 12) {
            dst[pos] = 10;
            column = 0;
        } else {
            dst[pos] = 32;
        }
        pos += 1;
    }
    dst[pos] = 0;
    return pos;
}

static uint32_t hash_word(const uint8_t *doc, int32_t off, int32_t len) {
    uint32_t h = 2166136261u;
    int32_t i = 0;
    while (i < len) {
        h = (h ^ (uint32_t)doc[off + i]) * 16777619u;
        i += 1;
    }
    return h;
}

static int32_t keys_equal(const uint8_t *doc, int32_t a_off, int32_t b_off, int32_t len) {
    int32_t i = 0;
    while (i < len) {
        if (doc[a_off + i] != doc[b_off + i]) {
            return 0;
        }
        i += 1;
    }
    return 1;
}

static void table_reset(Table *t, int32_t cap) {
    t->cap = cap;
    t->used = 0;
    int32_t i = 0;
    while (i < cap) {
        t->key_len[i] = 0;
        t->counts[i] = 0;
        i += 1;
    }
}

static void table_insert_raw(Table *t, const uint8_t *doc, int32_t off, int32_t len,
                             uint32_t hash, int32_t count) {
    int32_t mask = t->cap - 1;
    int32_t slot = (int32_t)(hash & (uint32_t)mask);
    for (;;) {
        if (t->key_len[slot] == 0) {
            t->key_off[slot] = off;
            t->key_len[slot] = len;
            t->hashes[slot] = hash;
            t->counts[slot] = count;
            t->used += 1;
            return;
        }
        if (t->hashes[slot] == hash && t->key_len[slot] == len &&
            keys_equal(doc, t->key_off[slot], off, len) != 0) {
            t->counts[slot] += count;
            return;
        }
        slot = (slot + 1) & mask;
    }
}

static void table_grow(Table *t, const uint8_t *doc, Table *spare) {
    int32_t old_cap = t->cap;
    int32_t new_cap = old_cap * 2;
    if (new_cap > MAX_CAP) {
        return;
    }
    table_reset(spare, new_cap);
    int32_t i = 0;
    while (i < old_cap) {
        if (t->key_len[i] != 0) {
            table_insert_raw(spare, doc, t->key_off[i], t->key_len[i], t->hashes[i], t->counts[i]);
        }
        i += 1;
    }
    int32_t *swap_off = t->key_off;
    int32_t *swap_len = t->key_len;
    uint32_t *swap_hash = t->hashes;
    int32_t *swap_count = t->counts;
    t->key_off = spare->key_off;
    t->key_len = spare->key_len;
    t->hashes = spare->hashes;
    t->counts = spare->counts;
    t->cap = spare->cap;
    t->used = spare->used;
    spare->key_off = swap_off;
    spare->key_len = swap_len;
    spare->hashes = swap_hash;
    spare->counts = swap_count;
    spare->cap = old_cap;
}

static int32_t count_words(Table *t, Table *spare, const uint8_t *doc, int32_t doc_len) {
    table_reset(t, INITIAL_CAP);
    int32_t tokens = 0;
    int32_t i = 0;
    while (i < doc_len) {
        int32_t c = (int32_t)doc[i];
        if (c < 97 || c > 122) {
            i += 1;
            continue;
        }
        int32_t start = i;
        while (i < doc_len) {
            int32_t d = (int32_t)doc[i];
            if (d < 97 || d > 122) {
                break;
            }
            i += 1;
        }
        int32_t len = i - start;
        uint32_t hash = hash_word(doc, start, len);
        table_insert_raw(t, doc, start, len, hash, 1);
        tokens += 1;
        if (t->used * 10 >= t->cap * 7) {
            table_grow(t, doc, spare);
        }
    }
    return tokens;
}

static uint64_t top_k_hash(Table *t, const uint8_t *doc, int32_t *taken) {
    int32_t i = 0;
    while (i < t->cap) {
        taken[i] = 0;
        i += 1;
    }
    uint64_t h = 14695981039346656037ULL;
    int32_t rank = 0;
    while (rank < TOP_K) {
        int32_t best = -1;
        int32_t best_count = 0;
        i = 0;
        while (i < t->cap) {
            if (t->key_len[i] != 0 && taken[i] == 0 && t->counts[i] > best_count) {
                best = i;
                best_count = t->counts[i];
            }
            i += 1;
        }
        if (best < 0) {
            break;
        }
        taken[best] = 1;
        h = h * 1000003 + (uint64_t)(best_count * (rank + 1));
        int32_t k = 0;
        int32_t letters = 0;
        while (k < t->key_len[best]) {
            letters = letters * 31 + (int32_t)doc[t->key_off[best] + k];
            k += 1;
        }
        h = h * 31 + (uint64_t)letters;
        rank += 1;
    }
    return h;
}

int main(void) {
    uint8_t *doc = (uint8_t *)malloc(DOC_CAP);
    int32_t *taken = (int32_t *)malloc((size_t)MAX_CAP * 4);
    if (doc == NULL || taken == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    Table t;
    Table spare;
    t.key_off = (int32_t *)malloc((size_t)MAX_CAP * 4);
    t.key_len = (int32_t *)malloc((size_t)MAX_CAP * 4);
    t.hashes = (uint32_t *)malloc((size_t)MAX_CAP * 4);
    t.counts = (int32_t *)malloc((size_t)MAX_CAP * 4);
    t.cap = 0;
    t.used = 0;
    spare.key_off = (int32_t *)malloc((size_t)MAX_CAP * 4);
    spare.key_len = (int32_t *)malloc((size_t)MAX_CAP * 4);
    spare.hashes = (uint32_t *)malloc((size_t)MAX_CAP * 4);
    spare.counts = (int32_t *)malloc((size_t)MAX_CAP * 4);
    spare.cap = 0;
    spare.used = 0;

    int32_t doc_len = gen_doc(doc);
    printf("Word frequency: %d byte document, %d word vocabulary\n", doc_len, VOCAB);

    int32_t tokens = count_words(&t, &spare, doc, doc_len);
    uint64_t check = top_k_hash(&t, doc, taken);
    printf("Tokens = %d distinct = %d slots = %d checksum = %" PRIu64 "\n",
           tokens, t.used, t.cap, check);

    printf("Benchmark: %d passes (tokenize + count + top %d)\n", PASSES, TOP_K);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int64_t total_tokens = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        total_tokens = total_tokens + (int64_t)count_words(&t, &spare, doc, doc_len);
        bench_hash = bench_hash * 1000003 + top_k_hash(&t, doc, taken);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Tokens seen = %" PRId64 "\n", total_tokens);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(doc);
    free(taken);
    return 0;
}
