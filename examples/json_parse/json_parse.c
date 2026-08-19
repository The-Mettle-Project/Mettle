/*
 * C JSON document parser benchmark - counterpart to json_parse.mettle
 *
 * Builds a ~350 KB JSON catalogue in memory, then tokenizes and parses it with
 * a recursive-descent parser into a node arena and walks the resulting tree.
 *
 * Build: build.bat (or: gcc -O3 -o json_parse_c.exe json_parse.c -lkernel32)
 * Run: json_parse_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define REC_COUNT 3000
#define DOC_CAP 1048576
#define NODE_CAP 65536
#define PASSES 30

#define K_NULL 0
#define K_FALSE 1
#define K_TRUE 2
#define K_NUM 3
#define K_STR 4
#define K_ARR 5
#define K_OBJ 6

typedef struct {
    double num;
    int32_t kind;
    int32_t key_off;
    int32_t key_len;
    int32_t val_off;
    int32_t val_len;
    int32_t first;
    int32_t next;
} Node;

typedef struct {
    uint8_t *text;
    int32_t len;
    int32_t pos;
    Node *nodes;
    int32_t count;
    int32_t failed;
} Parser;

static int32_t parse_value(Parser *p);

static int32_t emit_cstr(uint8_t *dst, int32_t pos, const char *s) {
    int32_t i = 0;
    while (s[i] != 0) {
        dst[pos + i] = (uint8_t)s[i];
        i += 1;
    }
    return pos + i;
}

static int32_t emit_int(uint8_t *dst, int32_t pos, int32_t v) {
    uint8_t tmp[24];
    if (v < 0) {
        dst[pos] = 45;
        pos += 1;
        v = -v;
    }
    if (v == 0) {
        dst[pos] = 48;
        return pos + 1;
    }
    int32_t n = 0;
    while (v > 0) {
        tmp[n] = (uint8_t)(48 + v % 10);
        v = v / 10;
        n += 1;
    }
    while (n > 0) {
        n -= 1;
        dst[pos] = tmp[n];
        pos += 1;
    }
    return pos;
}

static int32_t emit_fixed2(uint8_t *dst, int32_t pos, int32_t hundredths) {
    pos = emit_int(dst, pos, hundredths / 100);
    dst[pos] = 46;
    pos += 1;
    int32_t frac = hundredths % 100;
    dst[pos] = (uint8_t)(48 + frac / 10);
    dst[pos + 1] = (uint8_t)(48 + frac % 10);
    return pos + 2;
}

static int32_t emit_tag(uint8_t *dst, int32_t pos, int32_t which) {
    switch (which) {
        case 0: return emit_cstr(dst, pos, "\"alpha\"");
        case 1: return emit_cstr(dst, pos, "\"beta-two\"");
        case 2: return emit_cstr(dst, pos, "\"gamma ray\"");
        default: return emit_cstr(dst, pos, "\"delta\\\\core\"");
    }
}

static int32_t gen_doc(uint8_t *dst) {
    int32_t pos = 0;
    pos = emit_cstr(dst, pos, "{\"meta\":{\"version\":3,\"source\":\"synthetic\",\"records\":");
    pos = emit_int(dst, pos, REC_COUNT);
    pos = emit_cstr(dst, pos, "},\n\"records\":[\n");

    int32_t i = 0;
    while (i < REC_COUNT) {
        int32_t spaced = i % 2;
        pos = emit_cstr(dst, pos, "{\"id\":");
        if (spaced != 0) {
            dst[pos] = 32;
            pos += 1;
        }
        pos = emit_int(dst, pos, i);
        pos = emit_cstr(dst, pos, ",\"name\":\"");
        if (i % 7 == 0) {
            pos = emit_cstr(dst, pos, "it\\\"em\\\"-");
        } else {
            pos = emit_cstr(dst, pos, "item-");
        }
        pos = emit_int(dst, pos, i);
        pos = emit_cstr(dst, pos, "\",\"tags\":[");
        int32_t tag_count = i % 4;
        int32_t t = 0;
        while (t < tag_count) {
            if (t > 0) {
                dst[pos] = 44;
                pos += 1;
            }
            pos = emit_tag(dst, pos, (i + t) % 4);
            t += 1;
        }
        pos = emit_cstr(dst, pos, "],\"score\":");
        if (i % 11 == 0) {
            pos = emit_fixed2(dst, pos, (i * 37) % 1000);
            pos = emit_cstr(dst, pos, "e2");
        } else if (i % 5 == 0) {
            pos = emit_cstr(dst, pos, "-");
            pos = emit_fixed2(dst, pos, (i * 37) % 1000);
        } else {
            pos = emit_fixed2(dst, pos, (i * 37) % 1000);
        }
        pos = emit_cstr(dst, pos, ",\"active\":");
        if (i % 3 != 0) {
            pos = emit_cstr(dst, pos, "true");
        } else {
            pos = emit_cstr(dst, pos, "false");
        }
        if (i % 13 == 0) {
            pos = emit_cstr(dst, pos, ",\"extra\":null");
        }
        pos = emit_cstr(dst, pos, ",\"dims\":{\"w\":");
        pos = emit_int(dst, pos, 4 + i % 97);
        pos = emit_cstr(dst, pos, ",\"h\":");
        pos = emit_int(dst, pos, 3 + (i * 7) % 61);
        pos = emit_cstr(dst, pos, "}}");
        if (i + 1 < REC_COUNT) {
            dst[pos] = 44;
            pos += 1;
        }
        dst[pos] = 10;
        pos += 1;
        i += 1;
    }

    pos = emit_cstr(dst, pos, "]}");
    dst[pos] = 0;
    return pos;
}

static void skip_ws(Parser *p) {
    while (p->pos < p->len) {
        int32_t c = (int32_t)p->text[p->pos];
        if (c == 32 || c == 9 || c == 10 || c == 13) {
            p->pos += 1;
        } else {
            return;
        }
    }
}

static int32_t new_node(Parser *p, int32_t kind) {
    if (p->count >= NODE_CAP) {
        p->failed = 1;
        return -1;
    }
    int32_t idx = p->count;
    p->count += 1;
    p->nodes[idx].num = 0.0;
    p->nodes[idx].kind = kind;
    p->nodes[idx].key_off = 0;
    p->nodes[idx].key_len = 0;
    p->nodes[idx].val_off = 0;
    p->nodes[idx].val_len = 0;
    p->nodes[idx].first = -1;
    p->nodes[idx].next = -1;
    return idx;
}

static int32_t scan_string(Parser *p, int32_t *out_off, int32_t *out_len) {
    if (p->pos >= p->len || p->text[p->pos] != 34) {
        p->failed = 1;
        return 0;
    }
    p->pos += 1;
    int32_t start = p->pos;
    int32_t escapes = 0;
    while (p->pos < p->len) {
        int32_t c = (int32_t)p->text[p->pos];
        if (c == 92) {
            escapes += 1;
            p->pos += 2;
        } else if (c == 34) {
            *out_off = start;
            *out_len = p->pos - start - escapes;
            p->pos += 1;
            return 1;
        } else {
            p->pos += 1;
        }
    }
    p->failed = 1;
    return 0;
}

static double pow10_loop(int32_t exp) {
    double scale = 1.0;
    int32_t e = exp;
    if (e < 0) {
        e = -e;
        while (e > 0) {
            scale = scale / 10.0;
            e -= 1;
        }
        return scale;
    }
    while (e > 0) {
        scale = scale * 10.0;
        e -= 1;
    }
    return scale;
}

static double scan_number(Parser *p) {
    double sign = 1.0;
    if (p->pos < p->len && p->text[p->pos] == 45) {
        sign = -1.0;
        p->pos += 1;
    }
    double value = 0.0;
    while (p->pos < p->len) {
        int32_t c = (int32_t)p->text[p->pos];
        if (c < 48 || c > 57) {
            break;
        }
        value = value * 10.0 + (double)(c - 48);
        p->pos += 1;
    }
    if (p->pos < p->len && p->text[p->pos] == 46) {
        p->pos += 1;
        double scale = 0.1;
        while (p->pos < p->len) {
            int32_t c = (int32_t)p->text[p->pos];
            if (c < 48 || c > 57) {
                break;
            }
            value = value + (double)(c - 48) * scale;
            scale = scale * 0.1;
            p->pos += 1;
        }
    }
    if (p->pos < p->len && (p->text[p->pos] == 101 || p->text[p->pos] == 69)) {
        p->pos += 1;
        int32_t esign = 1;
        if (p->pos < p->len && p->text[p->pos] == 43) {
            p->pos += 1;
        } else if (p->pos < p->len && p->text[p->pos] == 45) {
            esign = -1;
            p->pos += 1;
        }
        int32_t exp = 0;
        while (p->pos < p->len) {
            int32_t c = (int32_t)p->text[p->pos];
            if (c < 48 || c > 57) {
                break;
            }
            exp = exp * 10 + (c - 48);
            p->pos += 1;
        }
        value = value * pow10_loop(esign * exp);
    }
    return sign * value;
}

static int32_t match_word(Parser *p, const char *word) {
    int32_t i = 0;
    while (word[i] != 0) {
        if (p->pos + i >= p->len || (int32_t)p->text[p->pos + i] != (int32_t)word[i]) {
            p->failed = 1;
            return 0;
        }
        i += 1;
    }
    p->pos += i;
    return 1;
}

static int32_t parse_array(Parser *p) {
    int32_t idx = new_node(p, K_ARR);
    if (idx < 0) {
        return -1;
    }
    p->pos += 1;
    skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == 93) {
        p->pos += 1;
        return idx;
    }
    int32_t tail = -1;
    for (;;) {
        int32_t child = parse_value(p);
        if (child < 0) {
            return -1;
        }
        if (tail < 0) {
            p->nodes[idx].first = child;
        } else {
            p->nodes[tail].next = child;
        }
        tail = child;
        skip_ws(p);
        if (p->pos >= p->len) {
            p->failed = 1;
            return -1;
        }
        int32_t c = (int32_t)p->text[p->pos];
        p->pos += 1;
        if (c == 93) {
            return idx;
        }
        if (c != 44) {
            p->failed = 1;
            return -1;
        }
    }
}

static int32_t parse_object(Parser *p) {
    int32_t idx = new_node(p, K_OBJ);
    if (idx < 0) {
        return -1;
    }
    p->pos += 1;
    skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == 125) {
        p->pos += 1;
        return idx;
    }
    int32_t tail = -1;
    for (;;) {
        skip_ws(p);
        int32_t key_off = 0;
        int32_t key_len = 0;
        if (scan_string(p, &key_off, &key_len) == 0) {
            return -1;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->text[p->pos] != 58) {
            p->failed = 1;
            return -1;
        }
        p->pos += 1;
        int32_t child = parse_value(p);
        if (child < 0) {
            return -1;
        }
        p->nodes[child].key_off = key_off;
        p->nodes[child].key_len = key_len;
        if (tail < 0) {
            p->nodes[idx].first = child;
        } else {
            p->nodes[tail].next = child;
        }
        tail = child;
        skip_ws(p);
        if (p->pos >= p->len) {
            p->failed = 1;
            return -1;
        }
        int32_t c = (int32_t)p->text[p->pos];
        p->pos += 1;
        if (c == 125) {
            return idx;
        }
        if (c != 44) {
            p->failed = 1;
            return -1;
        }
    }
}

static int32_t parse_value(Parser *p) {
    skip_ws(p);
    if (p->pos >= p->len || p->failed != 0) {
        p->failed = 1;
        return -1;
    }
    int32_t c = (int32_t)p->text[p->pos];
    if (c == 123) {
        return parse_object(p);
    }
    if (c == 91) {
        return parse_array(p);
    }
    if (c == 34) {
        int32_t sidx = new_node(p, K_STR);
        if (sidx < 0) {
            return -1;
        }
        int32_t off = 0;
        int32_t len = 0;
        scan_string(p, &off, &len);
        p->nodes[sidx].val_off = off;
        p->nodes[sidx].val_len = len;
        return sidx;
    }
    if (c == 116) {
        int32_t tidx = new_node(p, K_TRUE);
        match_word(p, "true");
        return tidx;
    }
    if (c == 102) {
        int32_t fidx = new_node(p, K_FALSE);
        match_word(p, "false");
        return fidx;
    }
    if (c == 110) {
        int32_t nidx = new_node(p, K_NULL);
        match_word(p, "null");
        return nidx;
    }
    int32_t idx = new_node(p, K_NUM);
    if (idx < 0) {
        return -1;
    }
    p->nodes[idx].num = scan_number(p);
    return idx;
}

static void walk(Parser *p, int32_t idx, int32_t depth, uint64_t *acc) {
    int32_t node = idx;
    while (node >= 0) {
        int32_t kind = p->nodes[node].kind;
        uint64_t h = *acc;
        h = h * 1000003 + (uint64_t)(kind + 1);
        h = h * 31 + (uint64_t)(p->nodes[node].key_len * 7 + depth);
        if (kind == K_NUM) {
            h = h + (uint64_t)(int64_t)(p->nodes[node].num * 100.0);
        } else if (kind == K_STR) {
            int32_t off = p->nodes[node].val_off;
            int32_t len = p->nodes[node].val_len;
            int32_t s = 0;
            int32_t k = 0;
            while (k < len) {
                s = s + (int32_t)p->text[off + k] * (k + 1);
                k += 1;
            }
            h = h + (uint64_t)s;
        }
        *acc = h;
        if (p->nodes[node].first >= 0) {
            walk(p, p->nodes[node].first, depth + 1, acc);
        }
        node = p->nodes[node].next;
    }
}

int main(void) {
    uint8_t *doc = (uint8_t *)malloc(DOC_CAP);
    Node *nodes = (Node *)malloc((size_t)NODE_CAP * sizeof(Node));
    if (doc == NULL || nodes == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    int32_t doc_len = gen_doc(doc);
    printf("JSON parse: %d byte document, %d records\n", doc_len, REC_COUNT);

    Parser p;
    p.text = doc;
    p.len = doc_len;
    p.pos = 0;
    p.nodes = nodes;
    p.count = 0;
    p.failed = 0;

    int32_t root = parse_value(&p);
    uint64_t check = 14695981039346656037ULL;
    walk(&p, root, 0, &check);
    printf("Nodes = %d failed = %d checksum = %" PRIu64 "\n", p.count, p.failed, check);

    printf("Benchmark: %d passes (parse + walk)\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int64_t nodes_seen = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        p.pos = 0;
        p.count = 0;
        p.failed = 0;
        int32_t r = parse_value(&p);
        uint64_t h = 14695981039346656037ULL;
        walk(&p, r, 0, &h);
        bench_hash = bench_hash * 1000003 + h;
        nodes_seen = nodes_seen + (int64_t)p.count;
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Nodes seen = %" PRId64 "\n", nodes_seen);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(doc);
    free(nodes);
    return 0;
}
