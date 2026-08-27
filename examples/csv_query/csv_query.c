#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"


#define ROWS 60000
#define TEXT_CAP 4194304
#define REGIONS 16
#define PRODUCTS 256
#define MAP_SLOTS 16384
#define MAP_MASK 16383
#define TOP_K 24
#define PASSES 5

typedef struct {
    uint64_t key;
    int64_t qty;
    int64_t value;
    int32_t count;
    int32_t peak;
} Group;

static int32_t emit_cstr(uint8_t *dst, int32_t pos, const char *s) {
  int32_t i = 0;
  while (s[i] != 0) {
    dst[pos + i] = (uint8_t)s[i];
    i += 1;
  }
  return pos + i;
}

static int32_t emit_int(uint8_t *dst, int32_t pos, int32_t v) {
  uint8_t tmp[16];
  int32_t n = 0;
  int32_t x = v;
  if (x == 0) {
    tmp[0] = 48;
    n = 1;
  }
  while (x > 0) {
    tmp[n] = (uint8_t)(48 + x % 10);
    x = x / 10;
    n += 1;
  }
  while (n > 0) {
    n -= 1;
    dst[pos] = tmp[n];
    pos += 1;
  }
  return pos;
}

static int32_t emit_pad(uint8_t *dst, int32_t pos, int32_t v, int32_t width) {
  uint8_t tmp[16];
  int32_t n = 0;
  int32_t x = v;
  if (x == 0) {
    tmp[0] = 48;
    n = 1;
  }
  while (x > 0) {
    tmp[n] = (uint8_t)(48 + x % 10);
    x = x / 10;
    n += 1;
  }
  while (n < width) {
    tmp[n] = 48;
    n += 1;
  }
  while (n > 0) {
    n -= 1;
    dst[pos] = tmp[n];
    pos += 1;
  }
  return pos;
}

static uint32_t next_rand(uint32_t *state) {
  uint32_t s = *state;
  s = s ^ (s << 13);
  s = s ^ (s >> 17);
  s = s ^ (s << 5);
  *state = s;
  return s;
}

static int32_t generate_csv(uint8_t *text, uint32_t seed) {
  uint32_t state = seed;
  int32_t pos = 0;
  pos = emit_cstr(text, pos, "id,region,product,qty,price\n");
  int32_t row = 0;
  while (row < ROWS) {
    uint32_t r = next_rand(&state);
    pos = emit_int(text, pos, row + 1);
    text[pos] = 44;
    pos += 1;
    pos = emit_cstr(text, pos, "region-");
    pos = emit_pad(text, pos, (int32_t)(r % (uint32_t)REGIONS), 2);
    text[pos] = 44;
    pos += 1;
    pos = emit_cstr(text, pos, "sku-");
    pos = emit_pad(text, pos, (int32_t)((r >> 8) % (uint32_t)PRODUCTS), 5);
    text[pos] = 44;
    pos += 1;
    pos = emit_int(text, pos, 1 + (int32_t)((r >> 17) % 50));
    text[pos] = 44;
    pos += 1;
    pos = emit_int(text, pos, 100 + (int32_t)((r >> 5) % 9900));
    text[pos] = 10;
    pos += 1;
    row += 1;
  }
  text[pos] = 0;
  return pos;
}

static int32_t scan_field(uint8_t *text, int32_t pos, int32_t stop) {
  int32_t p = pos;
  while ((int32_t)text[p] != 44 && (int32_t)text[p] != 10 && p < stop) {
    p += 1;
  }
  return p;
}

static int32_t parse_int(uint8_t *text, int32_t from, int32_t to) {
  int32_t v = 0;
  int32_t p = from;
  while (p < to) {
    v = v * 10 + ((int32_t)text[p] - 48);
    p += 1;
  }
  return v;
}

static uint64_t field_hash(uint8_t *text, int32_t from, int32_t to, uint64_t seed) {
  uint64_t h = seed;
  int32_t p = from;
  while (p < to) {
    h = h ^ (uint64_t)text[p];
    h = h * 1099511628211ULL;
    p += 1;
  }
  return h;
}

static void map_reset(int32_t *slots) {
  int32_t i = 0;
  while (i < MAP_SLOTS) {
    slots[i] = -1;
    i += 1;
  }
}

static int32_t map_find(int32_t *slots, Group *groups, uint64_t key, int32_t *used) {
  int32_t idx = (int32_t)(key & (uint64_t)MAP_MASK);
  while (slots[idx] >= 0) {
    if (groups[slots[idx]].key == key) {
      return slots[idx];
    }
    idx = (idx + 1) & MAP_MASK;
  }
  int32_t id = *used;
  *used = id + 1;
  groups[id].key = key;
  groups[id].qty = 0;
  groups[id].value = 0;
  groups[id].count = 0;
  groups[id].peak = 0;
  slots[idx] = id;
  return id;
}

static int32_t ingest(uint8_t *text, int32_t len, int32_t *slots, Group *groups) {
  map_reset(slots);
  int32_t used = 0;
  int32_t pos = 0;
  while (pos < len && (int32_t)text[pos] != 10) {
    pos += 1;
  }
  pos += 1;
  while (pos < len) {
    int32_t e = scan_field(text, pos, len);
    pos = e + 1;

    int32_t rs = pos;
    e = scan_field(text, pos, len);
    uint64_t rh = field_hash(text, rs, e, 14695981039346656037ULL);
    pos = e + 1;

    int32_t ps = pos;
    e = scan_field(text, pos, len);
    uint64_t ph = field_hash(text, ps, e, rh);
    pos = e + 1;

    int32_t qs = pos;
    e = scan_field(text, pos, len);
    int32_t qty = parse_int(text, qs, e);
    pos = e + 1;

    int32_t vs = pos;
    e = scan_field(text, pos, len);
    int32_t price = parse_int(text, vs, e);
    pos = e + 1;

    int32_t id = map_find(slots, groups, ph, &used);
    groups[id].qty = groups[id].qty + (int64_t)qty;
    groups[id].value = groups[id].value + (int64_t)qty * (int64_t)price;
    groups[id].count += 1;
    if (price > groups[id].peak) {
      groups[id].peak = price;
    }
  }
  return used;
}

static int32_t group_less(Group *a, Group *b) {
  if (a->value != b->value) {
    if (a->value > b->value) {
      return 1;
    }
    return 0;
  }
  if (a->key < b->key) {
    return 1;
  }
  return 0;
}

static void sort_groups(Group *groups, int32_t lo, int32_t hi) {
  if (lo >= hi) {
    return;
  }
  int32_t mid = lo + (hi - lo) / 2;
  Group pivot = groups[mid];
  int32_t i = lo;
  int32_t j = hi;
  while (i <= j) {
    while (group_less(&groups[i], &pivot) != 0) {
      i += 1;
    }
    while (group_less(&pivot, &groups[j]) != 0) {
      j -= 1;
    }
    if (i <= j) {
      Group tmp = groups[i];
      groups[i] = groups[j];
      groups[j] = tmp;
      i += 1;
      j -= 1;
    }
  }
  sort_groups(groups, lo, j);
  sort_groups(groups, i, hi);
}

static uint64_t round_trip(uint8_t *text, int32_t *slots, Group *groups, int32_t *out_groups) {
  int32_t len = generate_csv(text, 2463534242ULL);
  int32_t used = ingest(text, len, slots, groups);
  sort_groups(groups, 0, used - 1);

  uint64_t h = 1469598103934665603ULL;
  h = h * 31 + (uint64_t)len;
  h = h * 31 + (uint64_t)used;
  int32_t i = 0;
  while (i < TOP_K && i < used) {
    h = h ^ groups[i].key;
    h = h * 1099511628211ULL;
    h = h * 31 + (uint64_t)groups[i].qty;
    h = h * 31 + (uint64_t)groups[i].value;
    h = h * 31 + (uint64_t)(groups[i].count * 7 + groups[i].peak);
    i += 1;
  }
  int64_t total = 0;
  i = 0;
  while (i < used) {
    total = total + groups[i].value;
    i += 1;
  }
  h = h * 1000003 + (uint64_t)total;
  *out_groups = used;
  return h;
}

int main(void) {
    uint8_t *text = (uint8_t *)malloc(TEXT_CAP);
    int32_t *slots = (int32_t *)malloc((size_t)MAP_SLOTS * 4);
    Group *groups = (Group *)malloc((size_t)MAP_SLOTS * sizeof(Group));
    if (text == NULL || slots == NULL || groups == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    printf("CSV query: %d rows, %d regions x %d products, group-by then sort\n",
           ROWS, REGIONS, PRODUCTS);

    int32_t used = 0;
    uint64_t check = round_trip(text, slots, groups, &used);
    printf("  groups = %d, top value = %" PRId64 "\n", used, groups[0].value);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int32_t g2 = 0;
        bench_hash = bench_hash * 1000003 + round_trip(text, slots, groups, &g2);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(text);
    free(slots);
    free(groups);
    return 0;
}
