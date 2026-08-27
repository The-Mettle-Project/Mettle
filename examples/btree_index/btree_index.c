#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define T_ORDER 8
#define MAX_KEY 15
#define MAX_CHILD 16
#define NODE_CAP 20480
#define N_KEYS 50000
#define PASSES 3

typedef struct {
    int32_t count;
    int32_t leaf;
    int64_t key[15];
    int32_t val[15];
    int32_t child[16];
} Node;

typedef struct {
    Node *node;
    int32_t used;
    int32_t root;
} Tree;

static int32_t node_new(Tree *t, int32_t leaf) {
    int32_t id = t->used;
    t->used += 1;
    t->node[id].count = 0;
    t->node[id].leaf = leaf;
    int32_t i = 0;
    while (i < MAX_CHILD) {
        t->node[id].child[i] = -1;
        i += 1;
    }
    return id;
}

static void tree_init(Tree *t) {
    t->used = 0;
    t->root = node_new(t, 1);
}

static void split_child(Tree *t, int32_t parent, int32_t idx) {
    int32_t full = t->node[parent].child[idx];
    int32_t fresh = node_new(t, t->node[full].leaf);
    t->node[fresh].count = T_ORDER - 1;

    int32_t j = 0;
    while (j < T_ORDER - 1) {
        t->node[fresh].key[j] = t->node[full].key[j + T_ORDER];
        t->node[fresh].val[j] = t->node[full].val[j + T_ORDER];
        j += 1;
    }
    if (t->node[full].leaf == 0) {
        j = 0;
        while (j < T_ORDER) {
            t->node[fresh].child[j] = t->node[full].child[j + T_ORDER];
            j += 1;
        }
    }
    t->node[full].count = T_ORDER - 1;

    j = t->node[parent].count;
    while (j > idx) {
        t->node[parent].child[j + 1] = t->node[parent].child[j];
        j -= 1;
    }
    t->node[parent].child[idx + 1] = fresh;

    j = t->node[parent].count - 1;
    while (j >= idx) {
        t->node[parent].key[j + 1] = t->node[parent].key[j];
        t->node[parent].val[j + 1] = t->node[parent].val[j];
        j -= 1;
    }
    t->node[parent].key[idx] = t->node[full].key[T_ORDER - 1];
    t->node[parent].val[idx] = t->node[full].val[T_ORDER - 1];
    t->node[parent].count += 1;
}

static void insert_nonfull(Tree *t, int32_t id, int64_t key, int32_t val) {
    int32_t i = t->node[id].count - 1;
    if (t->node[id].leaf != 0) {
        while (i >= 0 && t->node[id].key[i] > key) {
            t->node[id].key[i + 1] = t->node[id].key[i];
            t->node[id].val[i + 1] = t->node[id].val[i];
            i -= 1;
        }
        t->node[id].key[i + 1] = key;
        t->node[id].val[i + 1] = val;
        t->node[id].count += 1;
        return;
    }
    while (i >= 0 && t->node[id].key[i] > key) {
        i -= 1;
    }
    i += 1;
    int32_t kid = t->node[id].child[i];
    if (t->node[kid].count == MAX_KEY) {
        split_child(t, id, i);
        if (key > t->node[id].key[i]) {
            i += 1;
        }
        kid = t->node[id].child[i];
    }
    insert_nonfull(t, kid, key, val);
}

static void tree_insert(Tree *t, int64_t key, int32_t val) {
    int32_t r = t->root;
    if (t->node[r].count == MAX_KEY) {
        int32_t fresh = node_new(t, 0);
        t->node[fresh].child[0] = r;
        t->root = fresh;
        split_child(t, fresh, 0);
        insert_nonfull(t, fresh, key, val);
        return;
    }
    insert_nonfull(t, r, key, val);
}

static int32_t tree_lookup(Tree *t, int64_t key) {
    int32_t id = t->root;
    while (id >= 0) {
        int32_t lo = 0;
        int32_t hi = t->node[id].count;
        while (lo < hi) {
            int32_t mid = (lo + hi) / 2;
            if (t->node[id].key[mid] < key) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < t->node[id].count && t->node[id].key[lo] == key) {
            return t->node[id].val[lo];
        }
        if (t->node[id].leaf != 0) {
            return -1;
        }
        id = t->node[id].child[lo];
    }
    return -1;
}

static void walk(Tree *t, int32_t id, uint64_t *acc, int32_t *seen, int64_t *prev, int32_t *ordered) {
    int32_t i = 0;
    while (i < t->node[id].count) {
        if (t->node[id].leaf == 0) {
            walk(t, t->node[id].child[i], acc, seen, prev, ordered);
        }
        int64_t k = t->node[id].key[i];
        if (k <= *prev) {
            *ordered = 0;
        }
        *prev = k;
        *acc = (*acc ^ (uint64_t)k) * 1099511628211ULL;
        *acc = *acc + (uint64_t)t->node[id].val[i];
        *seen += 1;
        i += 1;
    }
    if (t->node[id].leaf == 0) {
        walk(t, t->node[id].child[i], acc, seen, prev, ordered);
    }
}

static int64_t gen_key(uint32_t *state) {
    uint32_t s = *state;
    s = s ^ (s << 13);
    s = s ^ (s >> 17);
    s = s ^ (s << 5);
    *state = s;
    return (int64_t)(s % 4000000u) * 7 + 3;
}

static uint64_t round_trip(Tree *t) {
    tree_init(t);

    uint32_t state = 2463534242u;
    int32_t i = 0;
    while (i < N_KEYS) {
        int64_t k = gen_key(&state);
        tree_insert(t, k, i + 1);
        i += 1;
    }

    uint64_t h = 14695981039346656037ULL;
    h = h * 31 + (uint64_t)t->used;

    state = 2463534242u;
    int32_t hits = 0;
    i = 0;
    while (i < N_KEYS) {
        int64_t k = gen_key(&state);
        if (tree_lookup(t, k) > 0) {
            hits += 1;
        }
        i += 1;
    }
    h = h * 1000003 + (uint64_t)hits;

    int32_t misses = 0;
    state = 88675123u;
    i = 0;
    while (i < N_KEYS / 2) {
        int64_t k = gen_key(&state) + 1;
        if (tree_lookup(t, k) < 0) {
            misses += 1;
        }
        i += 1;
    }
    h = h * 1000003 + (uint64_t)misses;

    uint64_t acc = 1469598103934665603ULL;
    int32_t seen = 0;
    int64_t prev = -1;
    int32_t ordered = 1;
    walk(t, t->root, &acc, &seen, &prev, &ordered);
    h = h * 1000003 + acc;
    h = h * 31 + (uint64_t)seen;
    h = h * 31 + (uint64_t)ordered;
    return h;
}

int main(void) {
    Node *nodes = (Node *)malloc((size_t)NODE_CAP * sizeof(Node));
    if (nodes == NULL) {
        printf("malloc failed\n");
        return 1;
    }
    Tree t;
    t.node = nodes;
    t.used = 0;
    t.root = 0;

    printf("B-tree: %d inserts, order %d, node %d bytes\n",
           N_KEYS, T_ORDER, (int)sizeof(Node));

    uint64_t check = round_trip(&t);
    printf("  nodes used = %d, root = %d\n", t.used, t.root);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        bench_hash = bench_hash * 1000003 + round_trip(&t);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(nodes);
    return 0;
}
