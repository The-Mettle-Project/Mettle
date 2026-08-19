/*
 * C A* pathfinding benchmark - counterpart to astar_grid.mettle
 *
 * Grows a 192x192 cave with a cellular automaton, assigns terrain costs, then
 * runs sixteen A* searches over it with a binary min-heap open set and lazy
 * deletion, reconstructing each path through its parent chain.
 *
 * Build: build.bat (or: gcc -O3 -o astar_grid_c.exe astar_grid.c -lkernel32)
 * Run: astar_grid_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define DIM 192
#define CELLS 36864
#define QUERIES 16
#define SMOOTH_STEPS 3
#define INF 2000000000
#define PASSES 3

typedef struct {
    uint8_t *wall;
    uint8_t *cost;
    int32_t *g;
    int32_t *parent;
    uint8_t *closed;
    int32_t *heap_key;
    int32_t *heap_node;
    int32_t heap_size;
    int32_t expanded;
} Grid;

static void gen_grid(Grid *gr, uint8_t *scratch) {
    uint32_t state = 1973272912u;
    int32_t i = 0;
    while (i < CELLS) {
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        if (state % 100 < 42) {
            gr->wall[i] = 1;
        } else {
            gr->wall[i] = 0;
        }
        i += 1;
    }

    int32_t step = 0;
    while (step < SMOOTH_STEPS) {
        int32_t y = 0;
        while (y < DIM) {
            int32_t x = 0;
            while (x < DIM) {
                int32_t neighbours = 0;
                int32_t dy = -1;
                while (dy <= 1) {
                    int32_t dx = -1;
                    while (dx <= 1) {
                        int32_t ny = y + dy;
                        int32_t nx = x + dx;
                        if (nx < 0 || ny < 0 || nx >= DIM || ny >= DIM) {
                            neighbours += 1;
                        } else if (gr->wall[ny * DIM + nx] != 0) {
                            neighbours += 1;
                        }
                        dx += 1;
                    }
                    dy += 1;
                }
                if (neighbours >= 5) {
                    scratch[y * DIM + x] = 1;
                } else {
                    scratch[y * DIM + x] = 0;
                }
                x += 1;
            }
            y += 1;
        }
        i = 0;
        while (i < CELLS) {
            gr->wall[i] = scratch[i];
            i += 1;
        }
        step += 1;
    }

    int32_t y2 = 0;
    while (y2 < DIM) {
        int32_t x2 = 0;
        while (x2 < DIM) {
            gr->cost[y2 * DIM + x2] = (uint8_t)(1 + (x2 * 7 + y2 * 13) % 4);
            x2 += 1;
        }
        y2 += 1;
    }
}

static int32_t find_open(Grid *gr, int32_t from) {
    int32_t idx = from;
    int32_t tries = 0;
    while (tries < CELLS) {
        if (gr->wall[idx] == 0) {
            return idx;
        }
        idx += 1;
        if (idx >= CELLS) {
            idx = 0;
        }
        tries += 1;
    }
    return 0;
}

static int32_t heuristic(int32_t node, int32_t goal) {
    int32_t nx = node % DIM;
    int32_t ny = node / DIM;
    int32_t gx = goal % DIM;
    int32_t gy = goal / DIM;
    int32_t dx = nx - gx;
    if (dx < 0) {
        dx = -dx;
    }
    int32_t dy = ny - gy;
    if (dy < 0) {
        dy = -dy;
    }
    return dx + dy;
}

static void heap_push(Grid *gr, int32_t key, int32_t node) {
    int32_t i = gr->heap_size;
    gr->heap_key[i] = key;
    gr->heap_node[i] = node;
    gr->heap_size += 1;
    while (i > 0) {
        int32_t parent = (i - 1) / 2;
        int32_t swap = 0;
        if (gr->heap_key[i] < gr->heap_key[parent]) {
            swap = 1;
        } else if (gr->heap_key[i] == gr->heap_key[parent] && gr->heap_node[i] < gr->heap_node[parent]) {
            swap = 1;
        }
        if (swap == 0) {
            break;
        }
        int32_t tk = gr->heap_key[i];
        int32_t tn = gr->heap_node[i];
        gr->heap_key[i] = gr->heap_key[parent];
        gr->heap_node[i] = gr->heap_node[parent];
        gr->heap_key[parent] = tk;
        gr->heap_node[parent] = tn;
        i = parent;
    }
}

static int32_t heap_pop(Grid *gr) {
    int32_t top = gr->heap_node[0];
    gr->heap_size -= 1;
    gr->heap_key[0] = gr->heap_key[gr->heap_size];
    gr->heap_node[0] = gr->heap_node[gr->heap_size];
    int32_t i = 0;
    for (;;) {
        int32_t left = i * 2 + 1;
        int32_t right = left + 1;
        int32_t best = i;
        if (left < gr->heap_size) {
            if (gr->heap_key[left] < gr->heap_key[best] ||
                (gr->heap_key[left] == gr->heap_key[best] && gr->heap_node[left] < gr->heap_node[best])) {
                best = left;
            }
        }
        if (right < gr->heap_size) {
            if (gr->heap_key[right] < gr->heap_key[best] ||
                (gr->heap_key[right] == gr->heap_key[best] && gr->heap_node[right] < gr->heap_node[best])) {
                best = right;
            }
        }
        if (best == i) {
            break;
        }
        int32_t tk = gr->heap_key[i];
        int32_t tn = gr->heap_node[i];
        gr->heap_key[i] = gr->heap_key[best];
        gr->heap_node[i] = gr->heap_node[best];
        gr->heap_key[best] = tk;
        gr->heap_node[best] = tn;
        i = best;
    }
    return top;
}

static void relax(Grid *gr, int32_t node, int32_t nb, int32_t goal) {
    if (gr->wall[nb] != 0 || gr->closed[nb] != 0) {
        return;
    }
    int32_t tentative = gr->g[node] + (int32_t)gr->cost[nb];
    if (tentative < gr->g[nb]) {
        gr->g[nb] = tentative;
        gr->parent[nb] = node;
        heap_push(gr, tentative + heuristic(nb, goal), nb);
    }
}

static int32_t astar(Grid *gr, int32_t start, int32_t goal) {
    int32_t i = 0;
    while (i < CELLS) {
        gr->g[i] = INF;
        gr->parent[i] = -1;
        gr->closed[i] = 0;
        i += 1;
    }
    gr->heap_size = 0;
    gr->expanded = 0;
    gr->g[start] = 0;
    heap_push(gr, heuristic(start, goal), start);

    while (gr->heap_size > 0) {
        int32_t node = heap_pop(gr);
        if (gr->closed[node] != 0) {
            continue;
        }
        gr->closed[node] = 1;
        gr->expanded += 1;
        if (node == goal) {
            return gr->g[goal];
        }
        int32_t x = node % DIM;
        int32_t y = node / DIM;
        if (x > 0) {
            relax(gr, node, node - 1, goal);
        }
        if (x + 1 < DIM) {
            relax(gr, node, node + 1, goal);
        }
        if (y > 0) {
            relax(gr, node, node - DIM, goal);
        }
        if (y + 1 < DIM) {
            relax(gr, node, node + DIM, goal);
        }
    }
    return -1;
}

static uint64_t path_hash(Grid *gr, int32_t start, int32_t goal, int32_t *steps) {
    uint64_t h = 14695981039346656037ULL;
    int32_t node = goal;
    int32_t count = 0;
    while (node >= 0 && count <= CELLS) {
        h = h * 1000003 + (uint64_t)(node + 1);
        if (node == start) {
            break;
        }
        node = gr->parent[node];
        count += 1;
    }
    *steps = count;
    return h;
}

static uint64_t run_queries(Grid *gr) {
    uint32_t state = 305419896u;
    uint64_t h = 14695981039346656037ULL;
    int32_t q = 0;
    while (q < QUERIES) {
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        int32_t start = find_open(gr, (int32_t)(state % (uint32_t)CELLS));
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        int32_t goal = find_open(gr, (int32_t)(state % (uint32_t)CELLS));

        int32_t cost = astar(gr, start, goal);
        int32_t steps = 0;
        uint64_t ph = 0;
        if (cost >= 0) {
            ph = path_hash(gr, start, goal, &steps);
        }
        h = h * 1000003 + (uint64_t)(int64_t)cost;
        h = h * 31 + (uint64_t)gr->expanded;
        h = h * 31 + (uint64_t)steps;
        h = h ^ ph;
        q += 1;
    }
    return h;
}

int main(void) {
    uint8_t *wall = (uint8_t *)malloc(CELLS);
    uint8_t *scratch = (uint8_t *)malloc(CELLS);
    uint8_t *cost = (uint8_t *)malloc(CELLS);
    int32_t *g = (int32_t *)malloc((size_t)CELLS * 4);
    int32_t *parent = (int32_t *)malloc((size_t)CELLS * 4);
    uint8_t *closed = (uint8_t *)malloc(CELLS);
    int32_t *heap_key = (int32_t *)malloc((size_t)CELLS * 16);
    int32_t *heap_node = (int32_t *)malloc((size_t)CELLS * 16);
    if (wall == NULL || scratch == NULL || cost == NULL || g == NULL || parent == NULL ||
        closed == NULL || heap_key == NULL || heap_node == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    Grid gr;
    gr.wall = wall;
    gr.cost = cost;
    gr.g = g;
    gr.parent = parent;
    gr.closed = closed;
    gr.heap_key = heap_key;
    gr.heap_node = heap_node;
    gr.heap_size = 0;
    gr.expanded = 0;

    gen_grid(&gr, scratch);

    int32_t open_cells = 0;
    int32_t i = 0;
    while (i < CELLS) {
        if (wall[i] == 0) {
            open_cells += 1;
        }
        i += 1;
    }

    printf("A* pathfinding: %dx%d grid, %d open cells, %d queries\n", DIM, DIM, open_cells, QUERIES);

    uint64_t check = run_queries(&gr);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes (%d searches each)\n", PASSES, QUERIES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        bench_hash = bench_hash * 1000003 + run_queries(&gr);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(wall);
    free(scratch);
    free(cost);
    free(g);
    free(parent);
    free(closed);
    free(heap_key);
    free(heap_node);
    return 0;
}
