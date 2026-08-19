/*
 * C particle simulation benchmark - counterpart to physics_grid.mettle
 *
 * A game-engine step loop: bucket 8192 particles into a 64x64 uniform grid with
 * a counting sort, resolve collisions against the nine neighbouring cells, then
 * integrate and bounce off the walls.
 *
 * The collision response is written in its sqrt-free form (the normal impulse
 * is (v_rel . delta) / |delta|^2 * delta, which needs only the squared
 * distance) so both languages perform the identical IEEE sequence. Every float
 * constant here is exactly representable in binary for the same reason: a
 * decimal like 0.35 must round, and the two front ends need not round it the
 * same way.
 *
 * Build: build.bat (or: gcc -O3 -o physics_grid_c.exe physics_grid.c -lkernel32)
 * Run: physics_grid_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define COUNT 8192
#define GRID 64
#define CELLS 4096
#define STEPS 12
#define PASSES 2
#define BOX 1024.0
#define CELL_SIZE 16.0
#define RADIUS 3.0
#define MIN_DIST2 36.0
#define DT 0.25

typedef struct {
    double x;
    double y;
    double vx;
    double vy;
} Particle;

typedef struct {
    Particle *live;
    Particle *start;
    int32_t *cell_start;
    int32_t *cursor;
    int32_t *items;
    int64_t collisions;
} World;

static void seed_world(World *w) {
    uint32_t state = 2654435761u;
    int32_t i = 0;
    while (i < COUNT) {
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        double px = RADIUS + (double)(state % 4000) * 0.25;
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        double py = RADIUS + (double)(state % 4000) * 0.25;
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        double vx = ((double)(state % 256) - 128.0) / 128.0;
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);
        double vy = ((double)(state % 256) - 128.0) / 128.0;
        w->start[i].x = px;
        w->start[i].y = py;
        w->start[i].vx = vx;
        w->start[i].vy = vy;
        i += 1;
    }
}

static void reset_world(World *w) {
    int32_t i = 0;
    while (i < COUNT) {
        w->live[i].x = w->start[i].x;
        w->live[i].y = w->start[i].y;
        w->live[i].vx = w->start[i].vx;
        w->live[i].vy = w->start[i].vy;
        i += 1;
    }
    w->collisions = 0;
}

static int32_t cell_of(World *w, int32_t i) {
    int32_t cx = (int32_t)(w->live[i].x / CELL_SIZE);
    int32_t cy = (int32_t)(w->live[i].y / CELL_SIZE);
    if (cx < 0) {
        cx = 0;
    }
    if (cy < 0) {
        cy = 0;
    }
    if (cx >= GRID) {
        cx = GRID - 1;
    }
    if (cy >= GRID) {
        cy = GRID - 1;
    }
    return cy * GRID + cx;
}

static void bucket(World *w) {
    int32_t c = 0;
    while (c <= CELLS) {
        w->cell_start[c] = 0;
        c += 1;
    }
    int32_t i = 0;
    while (i < COUNT) {
        w->cell_start[cell_of(w, i) + 1] += 1;
        i += 1;
    }
    c = 0;
    while (c < CELLS) {
        w->cell_start[c + 1] += w->cell_start[c];
        c += 1;
    }
    c = 0;
    while (c <= CELLS) {
        w->cursor[c] = w->cell_start[c];
        c += 1;
    }
    i = 0;
    while (i < COUNT) {
        int32_t cell = cell_of(w, i);
        w->items[w->cursor[cell]] = i;
        w->cursor[cell] += 1;
        i += 1;
    }
}

static void resolve(World *w, int32_t i, int32_t j) {
    double dx = w->live[j].x - w->live[i].x;
    double dy = w->live[j].y - w->live[i].y;
    double d2 = dx * dx + dy * dy;
    if (d2 >= MIN_DIST2 || d2 <= 0.0) {
        return;
    }
    w->collisions = w->collisions + 1;

    double rvx = w->live[j].vx - w->live[i].vx;
    double rvy = w->live[j].vy - w->live[i].vy;
    double approach = rvx * dx + rvy * dy;
    if (approach < 0.0) {
        double factor = approach / d2;
        w->live[i].vx = w->live[i].vx + factor * dx;
        w->live[i].vy = w->live[i].vy + factor * dy;
        w->live[j].vx = w->live[j].vx - factor * dx;
        w->live[j].vy = w->live[j].vy - factor * dy;
    }

    double push = (MIN_DIST2 - d2) / (MIN_DIST2 * 8.0);
    w->live[i].x = w->live[i].x - dx * push;
    w->live[i].y = w->live[i].y - dy * push;
    w->live[j].x = w->live[j].x + dx * push;
    w->live[j].y = w->live[j].y + dy * push;
}

static void collide(World *w) {
    int32_t i = 0;
    while (i < COUNT) {
        int32_t cx = (int32_t)(w->live[i].x / CELL_SIZE);
        int32_t cy = (int32_t)(w->live[i].y / CELL_SIZE);
        if (cx < 0) {
            cx = 0;
        }
        if (cy < 0) {
            cy = 0;
        }
        if (cx >= GRID) {
            cx = GRID - 1;
        }
        if (cy >= GRID) {
            cy = GRID - 1;
        }
        int32_t gy = cy - 1;
        while (gy <= cy + 1) {
            if (gy >= 0 && gy < GRID) {
                int32_t gx = cx - 1;
                while (gx <= cx + 1) {
                    if (gx >= 0 && gx < GRID) {
                        int32_t cell = gy * GRID + gx;
                        int32_t k = w->cell_start[cell];
                        int32_t stop = w->cell_start[cell + 1];
                        while (k < stop) {
                            int32_t j = w->items[k];
                            if (j > i) {
                                resolve(w, i, j);
                            }
                            k += 1;
                        }
                    }
                    gx += 1;
                }
            }
            gy += 1;
        }
        i += 1;
    }
}

static void integrate(World *w) {
    int32_t i = 0;
    while (i < COUNT) {
        w->live[i].x = w->live[i].x + w->live[i].vx * DT;
        w->live[i].y = w->live[i].y + w->live[i].vy * DT;
        if (w->live[i].x < RADIUS) {
            w->live[i].x = RADIUS;
            w->live[i].vx = -w->live[i].vx;
        }
        if (w->live[i].x > BOX - RADIUS) {
            w->live[i].x = BOX - RADIUS;
            w->live[i].vx = -w->live[i].vx;
        }
        if (w->live[i].y < RADIUS) {
            w->live[i].y = RADIUS;
            w->live[i].vy = -w->live[i].vy;
        }
        if (w->live[i].y > BOX - RADIUS) {
            w->live[i].y = BOX - RADIUS;
            w->live[i].vy = -w->live[i].vy;
        }
        i += 1;
    }
}

static uint64_t simulate(World *w) {
    reset_world(w);
    int32_t step = 0;
    while (step < STEPS) {
        bucket(w);
        collide(w);
        integrate(w);
        step += 1;
    }
    uint64_t h = 14695981039346656037ULL;
    int32_t i = 0;
    while (i < COUNT) {
        h = h * 1000003 + (uint64_t)(int64_t)(w->live[i].x * 1000.0);
        h = h * 31 + (uint64_t)(int64_t)(w->live[i].y * 1000.0);
        h = h ^ (uint64_t)(int64_t)(w->live[i].vx * 100000.0);
        h = h ^ (uint64_t)(int64_t)(w->live[i].vy * 100000.0);
        i += 1;
    }
    h = h * 31 + (uint64_t)w->collisions;
    return h;
}

int main(void) {
    Particle *live = (Particle *)malloc((size_t)COUNT * sizeof(Particle));
    Particle *start = (Particle *)malloc((size_t)COUNT * sizeof(Particle));
    int32_t *cell_start = (int32_t *)malloc((size_t)(CELLS + 1) * 4);
    int32_t *cursor = (int32_t *)malloc((size_t)(CELLS + 1) * 4);
    int32_t *items = (int32_t *)malloc((size_t)COUNT * 4);
    if (live == NULL || start == NULL || cell_start == NULL || cursor == NULL || items == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    World w;
    w.live = live;
    w.start = start;
    w.cell_start = cell_start;
    w.cursor = cursor;
    w.items = items;
    w.collisions = 0;

    seed_world(&w);

    printf("Particle sim: %d particles, %dx%d grid, %d steps\n", COUNT, GRID, GRID, STEPS);

    uint64_t check = simulate(&w);
    printf("Collisions = %" PRId64 " checksum = %" PRIu64 "\n", w.collisions, check);

    printf("Benchmark: %d passes (%d steps each)\n", PASSES, STEPS);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int64_t total_collisions = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        bench_hash = bench_hash * 1000003 + simulate(&w);
        total_collisions = total_collisions + w.collisions;
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Collisions = %" PRId64 "\n", total_collisions);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(live);
    free(start);
    free(cell_start);
    free(cursor);
    free(items);
    return 0;
}
