/*
 * C linked-list pointer-chase benchmark - counterpart to linked_list_sum.mettle
 *
 * Builds a 65536-node singly-linked list in shuffled memory order and sums
 * node values by chasing `next` pointers. 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o linked_list_sum_c.exe linked_list_sum.c -lkernel32)
 * Run: linked_list_sum_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define NODE_COUNT 65536

typedef struct {
    int32_t next;
    int32_t value;
} Node;

static void build_list(Node *nodes, int32_t *order, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        order[i] = i;
        nodes[i].value = i % 4096;
    }

    uint32_t state = 88172645u;
    for (int32_t i = count - 1; i > 0; i--) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        int32_t j = (int32_t)(state % (uint32_t)(i + 1));
        int32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    for (int32_t i = 0; i < count - 1; i++) {
        nodes[order[i]].next = order[i + 1];
    }
    nodes[order[count - 1]].next = -1;
}

static int64_t sum_chase(const Node *nodes, int32_t head) {
    int64_t sum = 0;
    int32_t idx = head;
    while (idx >= 0) {
        sum += (int64_t)nodes[idx].value;
        idx = nodes[idx].next;
    }
    return sum;
}

int main(void) {
    int32_t count = NODE_COUNT;
    Node *nodes = (Node *)malloc((size_t)count * sizeof(Node));
    int32_t *order = (int32_t *)malloc((size_t)count * sizeof(int32_t));
    if (!nodes || !order) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    build_list(nodes, order, count);
    int32_t head = order[0];

    printf("Linked-list pointer chase: 65536 nodes\n");

    int64_t check = sum_chase(nodes, head);
    printf("Checksum = %" PRId64 "\n", check);

    const int passes = 200;
    printf("Benchmark: %d passes (linked_list_sum)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += sum_chase(nodes, head);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(nodes);
    free(order);
    return 0;
}
