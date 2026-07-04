/*
 * C binary-search-tree benchmark - counterpart to bst_insert.mettle
 *
 * Builds a BST from 4096 pseudo-random int32 values (node pool, no per-node
 * malloc) and repeatedly walks it with a recursive in-order sum, 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o bst_insert_c.exe bst_insert.c -lkernel32)
 * Run: bst_insert_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define NODE_COUNT 4096

typedef struct {
    int32_t value;
    int32_t left;
    int32_t right;
} Node;

static int32_t bst_insert(Node *pool, int32_t root, int32_t new_idx, int32_t value) {
    if (root < 0) {
        return new_idx;
    }
    if (value < pool[root].value) {
        pool[root].left = bst_insert(pool, pool[root].left, new_idx, value);
    } else {
        pool[root].right = bst_insert(pool, pool[root].right, new_idx, value);
    }
    return root;
}

static int32_t build_tree(Node *pool, int32_t count) {
    uint32_t state = 2246822519u;
    int32_t root = -1;
    for (int32_t i = 0; i < count; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        int32_t v = (int32_t)(state % 1000000);
        pool[i].value = v;
        pool[i].left = -1;
        pool[i].right = -1;
        root = bst_insert(pool, root, i, v);
    }
    return root;
}

static int64_t inorder_sum(const Node *pool, int32_t idx) {
    if (idx < 0) {
        return 0;
    }
    int64_t left_sum = inorder_sum(pool, pool[idx].left);
    int64_t here = (int64_t)pool[idx].value;
    int64_t right_sum = inorder_sum(pool, pool[idx].right);
    return left_sum + here + right_sum;
}

int main(void) {
    int32_t count = NODE_COUNT;
    Node *pool = (Node *)malloc((size_t)count * sizeof(Node));
    if (!pool) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    int32_t root = build_tree(pool, count);

    printf("BST insert + in-order traversal: 4096 nodes\n");

    int64_t check = inorder_sum(pool, root);
    printf("Checksum = %" PRId64 "\n", check);

    const int passes = 200;
    printf("Benchmark: %d passes (bst_insert)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += inorder_sum(pool, root);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(pool);
    return 0;
}
