#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"


#define N_OBJ 160000
#define MAX_SLOTS 6
#define N_ROOTS 96
#define CYCLES 3
#define PASSES 2

typedef struct {
    int32_t kind;
    int32_t nslots;
    int32_t mark;
    int32_t payload;
    int32_t slot[6];
} Obj;

typedef struct {
    Obj *obj;
    int32_t count;
    int32_t *work;
    int32_t *fwd;
    int32_t *root;
    int32_t nroots;
    int32_t freed;
} Heap;

static uint32_t next_rand(uint32_t *state) {
  uint32_t s = *state;
  s ^= (s << 13);
  s ^= (s >> 17);
  s ^= (s << 5);
  *state = s;
  return s;
}

static void build_graph(Heap *h, uint32_t *state) {
  int32_t n = N_OBJ;
  h->count = n;
  int32_t i = 0;
  while (i < n) {
    uint32_t r = next_rand(state);
    h->obj[i].kind = (int32_t)(r % 4);
    h->obj[i].payload = (int32_t)((r >> 3) % 65536);
    h->obj[i].mark = 0;
    int32_t ns = 1 + (int32_t)((r >> 19) % (uint32_t)MAX_SLOTS);
    h->obj[i].nslots = ns;
    int32_t s = 0;
    while (s < MAX_SLOTS) {
      h->obj[i].slot[s] = -1;
      s += 1;
    }
    s = 0;
    while (s < ns) {
      uint32_t q = next_rand(state);
      int32_t target = -1;
      if (q % 8 < 2) {
        int32_t span = 1 + (int32_t)((q >> 4) % 512);
        target = i + span;
        if (target >= n) {
          target = -1;
        }
      } else if (q % 8 < 3) {
        target = (int32_t)((q >> 6) % (uint32_t)n);
      }
      h->obj[i].slot[s] = target;
      s += 1;
    }
    i += 1;
  }
  i = 0;
  while (i < N_ROOTS) {
    uint32_t r2 = next_rand(state);
    h->root[i] = (int32_t)(r2 % (uint32_t)n);
    i += 1;
  }
  h->nroots = N_ROOTS;
}

static int32_t mark_from_roots(Heap *h) {
  int32_t i = 0;
  while (i < h->count) {
    h->obj[i].mark = 0;
    i += 1;
  }
  int32_t top = 0;
  i = 0;
  while (i < h->nroots) {
    int32_t r = h->root[i];
    if (r >= 0 && r < h->count && h->obj[r].mark == 0) {
      h->obj[r].mark = 1;
      h->work[top] = r;
      top += 1;
    }
    i += 1;
  }
  int32_t live = 0;
  while (top > 0) {
    top -= 1;
    int32_t id = h->work[top];
    live += 1;
    int32_t ns = h->obj[id].nslots;
    int32_t s = 0;
    while (s < ns) {
      int32_t t = h->obj[id].slot[s];
      if (t >= 0 && h->obj[t].mark == 0) {
        h->obj[t].mark = 1;
        h->work[top] = t;
        top += 1;
      }
      s += 1;
    }
  }
  return live;
}

static int32_t sweep(Heap *h) {
  int32_t dead = 0;
  int32_t i = 0;
  while (i < h->count) {
    if (h->obj[i].mark == 0) {
      dead += 1;
    }
    i += 1;
  }
  h->freed = dead;
  return dead;
}

static int32_t compact(Heap *h) {
  int32_t n = h->count;
  int32_t live = 0;
  int32_t i = 0;
  while (i < n) {
    if (h->obj[i].mark != 0) {
      h->fwd[i] = live;
      live += 1;
    } else {
      h->fwd[i] = -1;
    }
    i += 1;
  }
  i = 0;
  while (i < n) {
    if (h->obj[i].mark != 0) {
      int32_t dst = h->fwd[i];
      h->obj[dst].kind = h->obj[i].kind;
      h->obj[dst].nslots = h->obj[i].nslots;
      h->obj[dst].payload = h->obj[i].payload;
      h->obj[dst].mark = 1;
      int32_t s = 0;
      while (s < MAX_SLOTS) {
        int32_t t = h->obj[i].slot[s];
        if (t >= 0) {
          h->obj[dst].slot[s] = h->fwd[t];
        } else {
          h->obj[dst].slot[s] = -1;
        }
        s += 1;
      }
    }
    i += 1;
  }
  i = 0;
  while (i < h->nroots) {
    int32_t r = h->root[i];
    if (r >= 0) {
      h->root[i] = h->fwd[r];
    }
    i += 1;
  }
  h->count = live;
  return live;
}

static void mutate(Heap *h, uint32_t *state) {
  int32_t n = h->count;
  if (n <= 1) {
    return;
  }
  int32_t edits = n / 6;
  int32_t e = 0;
  while (e < edits) {
    uint32_t r = next_rand(state);
    int32_t id = (int32_t)(r % (uint32_t)n);
    int32_t ns = h->obj[id].nslots;
    if (ns > 0) {
      int32_t s = (int32_t)((r >> 12) % (uint32_t)ns);
      if ((r >> 20) % 3 == 0) {
        h->obj[id].slot[s] = -1;
      } else {
        h->obj[id].slot[s] = (int32_t)((r >> 6) % (uint32_t)n);
      }
    }
    e += 1;
  }
  int32_t i = 0;
  while (i < h->nroots) {
    if (i % 3 == 0) {
      uint32_t q = next_rand(state);
      h->root[i] = (int32_t)(q % (uint32_t)n);
    }
    i += 1;
  }
}

static uint64_t verify(Heap *h) {
  uint64_t v = 14695981039346656037ULL;
  int32_t i = 0;
  while (i < h->count) {
    v = v ^ (uint64_t)h->obj[i].payload;
    v *= 1099511628211ULL;
    v += (uint64_t)(h->obj[i].kind * 7 + h->obj[i].nslots);
    int32_t s = 0;
    while (s < h->obj[i].nslots) {
      v = v * 31 + (uint64_t)(h->obj[i].slot[s] + 2);
      s += 1;
    }
    i += 1;
  }
  return v;
}

static uint64_t round_trip(Heap *h, int32_t *out_live) {
  uint32_t state = 2463534242ULL;
  build_graph(h, &state);

  uint64_t res = 1469598103934665603ULL;
  int32_t cycle = 0;
  while (cycle < CYCLES) {
    int32_t live = mark_from_roots(h);
    int32_t dead = sweep(h);
    int32_t kept = compact(h);
    res = res * 1000003 + (uint64_t)live;
    res = res * 31 + (uint64_t)dead;
    res = res * 31 + (uint64_t)kept;
    res = res * 1000003 + verify(h);
    mutate(h, &state);
    cycle += 1;
  }
  *out_live = h->count;
  return res;
}

int main(void) {
    Obj *objs = (Obj *)malloc((size_t)N_OBJ * sizeof(Obj));
    int32_t *work = (int32_t *)malloc((size_t)N_OBJ * 4);
    int32_t *fwd = (int32_t *)malloc((size_t)N_OBJ * 4);
    int32_t *root = (int32_t *)malloc((size_t)N_ROOTS * 4);
    if (objs == NULL || work == NULL || fwd == NULL || root == NULL) {
        printf("malloc failed\n");
        return 1;
    }
    Heap h;
    h.obj = objs;
    h.count = 0;
    h.work = work;
    h.fwd = fwd;
    h.root = root;
    h.nroots = 0;
    h.freed = 0;

    printf("Mark-sweep: %d objects, %d roots, %d collect-compact cycles\n",
           N_OBJ, N_ROOTS, CYCLES);

    int32_t live = 0;
    uint64_t check = round_trip(&h, &live);
    printf("  surviving objects = %d, object size = %d bytes\n", live, (int)sizeof(Obj));
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int32_t l2 = 0;
        bench_hash = bench_hash * 1000003 + round_trip(&h, &l2);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(objs);
    free(work);
    free(fwd);
    free(root);
    return 0;
}
