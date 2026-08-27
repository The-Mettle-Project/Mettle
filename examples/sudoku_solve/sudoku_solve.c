#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"


#define CELLS 81
#define DIGIT_MASK 1022
#define PUZZLES 120
#define REMOVE 61
#define PASSES 3

typedef struct {
    int32_t *cell;
    int32_t *rowmask;
    int32_t *colmask;
    int32_t *boxmask;
    int64_t nodes;
} Board;

static int32_t box_of(int32_t idx) {
  return (idx / 27) * 3 + (idx % 9) / 3;
}

static int32_t popcount9(int32_t v) {
  int32_t n = 0;
  int32_t x = v;
  while (x != 0) {
    x = x & (x - 1);
    n += 1;
  }
  return n;
}

static void board_clear(Board *b) {
  int32_t i = 0;
  while (i < CELLS) {
    b->cell[i] = 0;
    i += 1;
  }
  i = 0;
  while (i < 9) {
    b->rowmask[i] = 0;
    b->colmask[i] = 0;
    b->boxmask[i] = 0;
    i += 1;
  }
  b->nodes = 0;
}

static void place(Board *b, int32_t idx, int32_t d) {
  int32_t bit = 1 << d;
  b->cell[idx] = d;
  b->rowmask[idx / 9] = b->rowmask[idx / 9] | bit;
  b->colmask[idx % 9] = b->colmask[idx % 9] | bit;
  b->boxmask[box_of(idx)] = b->boxmask[box_of(idx)] | bit;
}

static void unplace(Board *b, int32_t idx, int32_t d) {
  int32_t bit = 1 << d;
  b->cell[idx] = 0;
  b->rowmask[idx / 9] = b->rowmask[idx / 9] & ~bit;
  b->colmask[idx % 9] = b->colmask[idx % 9] & ~bit;
  b->boxmask[box_of(idx)] = b->boxmask[box_of(idx)] & ~bit;
}

static int32_t candidates(Board *b, int32_t idx) {
  int32_t used = b->rowmask[idx / 9] | b->colmask[idx % 9] | b->boxmask[box_of(idx)];
  return DIGIT_MASK & ~used;
}

static int32_t solve(Board *b) {
  b->nodes += 1;
  int32_t best = -1;
  int32_t best_count = 10;
  int32_t best_mask = 0;
  int32_t i = 0;
  while (i < CELLS) {
    if (b->cell[i] == 0) {
      int32_t mask = candidates(b, i);
      int32_t count = popcount9(mask);
      if (count == 0) {
        return 0;
      }
      if (count < best_count) {
        best_count = count;
        best = i;
        best_mask = mask;
        if (count == 1) {
          i = CELLS;
        }
      }
    }
    i += 1;
  }
  if (best < 0) {
    return 1;
  }
  int32_t d = 1;
  while (d <= 9) {
    if ((best_mask >> d) & 1) {
      place(b, best, d);
      if (solve(b) != 0) {
        return 1;
      }
      unplace(b, best, d);
    }
    d += 1;
  }
  return 0;
}

static uint32_t next_rand(uint32_t *state) {
  uint32_t s = *state;
  s = s ^ (s << 13);
  s = s ^ (s >> 17);
  s = s ^ (s << 5);
  *state = s;
  return s;
}

static void build_solved(int32_t *grid, uint32_t *state) {
  int32_t band[3];
  int32_t stack[3];
  int32_t i = 0;
  while (i < 3) {
    band[i] = i;
    stack[i] = i;
    i += 1;
  }
  i = 2;
  while (i > 0) {
    int32_t j = (int32_t)(next_rand(state) % (uint32_t)(i + 1));
    int32_t tmp = band[i];
    band[i] = band[j];
    band[j] = tmp;
    int32_t k = (int32_t)(next_rand(state) % (uint32_t)(i + 1));
    tmp = stack[i];
    stack[i] = stack[k];
    stack[k] = tmp;
    i -= 1;
  }
  int32_t shift = (int32_t)(next_rand(state) % 9);
  int32_t r = 0;
  while (r < 9) {
    int32_t c = 0;
    while (c < 9) {
      int32_t sr = band[r / 3] * 3 + r % 3;
      int32_t sc = stack[c / 3] * 3 + c % 3;
      int32_t v = (sr * 3 + sr / 3 + sc + shift) % 9 + 1;
      grid[r * 9 + c] = v;
      c += 1;
    }
    r += 1;
  }
}

static void make_puzzle(Board *b, int32_t *grid, uint32_t *state) {
  board_clear(b);
  int32_t hole[81];
  int32_t i = 0;
  while (i < CELLS) {
    hole[i] = 0;
    i += 1;
  }
  int32_t removed = 0;
  int32_t guard = 0;
  while (removed < REMOVE && guard < 4000) {
    int32_t p = (int32_t)(next_rand(state) % (uint32_t)CELLS);
    if (hole[p] == 0) {
      hole[p] = 1;
      removed += 1;
    }
    guard += 1;
  }
  i = 0;
  while (i < CELLS) {
    if (hole[i] == 0) {
      place(b, i, grid[i]);
    }
    i += 1;
  }
}

static uint64_t round_trip(Board *b, int32_t *grid) {
  uint32_t state = 2463534242ULL;
  uint64_t h = 14695981039346656037ULL;
  int32_t solved_count = 0;
  int64_t total_nodes = 0;
  int32_t p = 0;
  while (p < PUZZLES) {
    build_solved(grid, &state);
    make_puzzle(b, grid, &state);
    int32_t ok = solve(b);
    solved_count += ok;
    total_nodes += b->nodes;
    int32_t i = 0;
    while (i < CELLS) {
      h = h ^ (uint64_t)b->cell[i];
      h = h * 1099511628211ULL;
      i += 1;
    }
    h = h * 31 + (uint64_t)b->nodes;
    p += 1;
  }
  h = h * 1000003 + (uint64_t)solved_count;
  h = h * 31 + (uint64_t)total_nodes;
  return h;
}

int main(void) {
    int32_t *cell = (int32_t *)malloc((size_t)CELLS * 4);
    int32_t *rowmask = (int32_t *)malloc(36);
    int32_t *colmask = (int32_t *)malloc(36);
    int32_t *boxmask = (int32_t *)malloc(36);
    int32_t *grid = (int32_t *)malloc((size_t)CELLS * 4);
    if (cell == NULL || rowmask == NULL || colmask == NULL || boxmask == NULL || grid == NULL) {
        printf("malloc failed\n");
        return 1;
    }
    Board b;
    b.cell = cell;
    b.rowmask = rowmask;
    b.colmask = colmask;
    b.boxmask = boxmask;
    b.nodes = 0;

    printf("Sudoku: %d puzzles, %d holes each, MRV backtracking\n", PUZZLES, REMOVE);

    uint64_t check = round_trip(&b, grid);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        bench_hash = bench_hash * 1000003 + round_trip(&b, grid);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(cell);
    free(rowmask);
    free(colmask);
    free(boxmask);
    free(grid);
    return 0;
}
