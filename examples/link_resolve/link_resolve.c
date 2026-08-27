#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"


#define UNITS 1200
#define DEFS_PER 40
#define REFS_PER 72
#define SHARED_POOL 3000
#define STREAM_CAP 4194304
#define MAX_DEFS 65536
#define MAX_REFS 98304
#define SLOTS 131072
#define SLOT_MASK 131071
#define IMAGE_SIZE 1048576
#define SECTIONS 4
#define PASSES 5

typedef struct {
    int32_t name_off;
    int32_t name_len;
    int32_t section;
    int32_t offset;
    int32_t unit;
} Sym;

typedef struct {
    int32_t name_off;
    int32_t name_len;
    int32_t site;
    int32_t kind;
    int32_t unit;
} Ref;

typedef struct {
    uint8_t *stream;
    int32_t stream_len;
    Sym *def;
    int32_t ndefs;
    Ref *ref;
    int32_t nrefs;
    int32_t *slot;
    uint8_t *image;
    int32_t dupes;
    int32_t unresolved;
} Linker;

static int32_t put_u32(uint8_t *buf, int32_t pos, uint32_t v) {
  buf[pos] = (uint8_t)(v & 255);
  buf[pos + 1] = (uint8_t)((v >> 8) & 255);
  buf[pos + 2] = (uint8_t)((v >> 16) & 255);
  buf[pos + 3] = (uint8_t)((v >> 24) & 255);
  return pos + 4;
}

static uint32_t get_u32(uint8_t *buf, int32_t pos) {
  uint32_t a = (uint32_t)buf[pos];
  uint32_t b = (uint32_t)buf[pos + 1];
  uint32_t c = (uint32_t)buf[pos + 2];
  uint32_t d = (uint32_t)buf[pos + 3];
  return a | (b << 8) | (c << 16) | (d << 24);
}

static uint32_t next_rand(uint32_t *state) {
  uint32_t s = *state;
  s ^= (s << 13);
  s ^= (s >> 17);
  s ^= (s << 5);
  *state = s;
  return s;
}

static int32_t emit_name(uint8_t *buf, int32_t pos, int32_t prefix, int32_t id) {
  int32_t start = pos + 4;
  int32_t p = start;
  if (prefix >= 0) {
    buf[p] = 117;
    p += 1;
    int32_t u = prefix;
    int32_t digits = 1000;
    while (digits > 0) {
      buf[p] = (uint8_t)(48 + (u / digits) % 10);
      p += 1;
      digits /= 10;
    }
    buf[p] = 95;
    p += 1;
  } else {
    buf[p] = 115;
    p += 1;
    buf[p] = 104;
    p += 1;
    buf[p] = 95;
    p += 1;
  }
  int32_t v = id;
  int32_t d2 = 10000;
  while (d2 > 0) {
    buf[p] = (uint8_t)(48 + (v / d2) % 10);
    p += 1;
    d2 /= 10;
  }
  int32_t len = p - start;
  put_u32(buf, pos, (uint32_t)len);
  return p;
}

static int32_t generate_stream(Linker *l, uint32_t seed) {
  uint32_t state = seed;
  int32_t pos = 0;
  int32_t unit = 0;
  while (unit < UNITS) {
    pos = put_u32(l->stream, pos, 1296649793);
    pos = put_u32(l->stream, pos, (uint32_t)unit);
    pos = put_u32(l->stream, pos, (uint32_t)DEFS_PER);
    int32_t d = 0;
    while (d < DEFS_PER) {
      uint32_t r = next_rand(&state);
      if (d % 4 == 0) {
        pos = emit_name(l->stream, pos, -1, (int32_t)(r % (uint32_t)SHARED_POOL));
      } else {
        pos = emit_name(l->stream, pos, unit, d);
      }
      pos = put_u32(l->stream, pos, (uint32_t)((int32_t)(r >> 9) % SECTIONS));
      pos = put_u32(l->stream, pos, (uint32_t)((r >> 3) % 60000));
      d += 1;
    }
    pos = put_u32(l->stream, pos, (uint32_t)REFS_PER);
    int32_t f = 0;
    while (f < REFS_PER) {
      uint32_t q = next_rand(&state);
      if (q % 5 < 4) {
        pos = emit_name(l->stream, pos, -1, (int32_t)((q >> 2) % (uint32_t)SHARED_POOL));
      } else {
        int32_t other = (int32_t)((q >> 7) % (uint32_t)UNITS);
        pos = emit_name(l->stream, pos, other, (int32_t)((q >> 17) % (uint32_t)DEFS_PER));
      }
      pos = put_u32(l->stream, pos, (uint32_t)((q >> 5) % (uint32_t)(IMAGE_SIZE - 8)));
      pos = put_u32(l->stream, pos, (uint32_t)(q % 3));
      f += 1;
    }
    unit += 1;
  }
  l->stream_len = pos;
  return pos;
}

static int32_t parse_stream(Linker *l) {
  l->ndefs = 0;
  l->nrefs = 0;
  int32_t pos = 0;
  int32_t units = 0;
  while (pos < l->stream_len) {
    uint32_t magic = get_u32(l->stream, pos);
    pos += 4;
    if (magic != 1296649793) {
      return -1;
    }
    int32_t unit = (int32_t)get_u32(l->stream, pos);
    pos += 4;
    int32_t ndefs = (int32_t)get_u32(l->stream, pos);
    pos += 4;
    int32_t d = 0;
    while (d < ndefs) {
      int32_t nlen = (int32_t)get_u32(l->stream, pos);
      pos += 4;
      if (l->ndefs < MAX_DEFS) {
        l->def[l->ndefs].name_off = pos;
        l->def[l->ndefs].name_len = nlen;
        l->def[l->ndefs].unit = unit;
      }
      pos += nlen;
      int32_t sec = (int32_t)get_u32(l->stream, pos);
      pos += 4;
      int32_t off = (int32_t)get_u32(l->stream, pos);
      pos += 4;
      if (l->ndefs < MAX_DEFS) {
        l->def[l->ndefs].section = sec;
        l->def[l->ndefs].offset = off;
        l->ndefs += 1;
      }
      d += 1;
    }
    int32_t nrefs = (int32_t)get_u32(l->stream, pos);
    pos += 4;
    int32_t f = 0;
    while (f < nrefs) {
      int32_t rlen = (int32_t)get_u32(l->stream, pos);
      pos += 4;
      if (l->nrefs < MAX_REFS) {
        l->ref[l->nrefs].name_off = pos;
        l->ref[l->nrefs].name_len = rlen;
        l->ref[l->nrefs].unit = unit;
      }
      pos += rlen;
      int32_t site = (int32_t)get_u32(l->stream, pos);
      pos += 4;
      int32_t kind = (int32_t)get_u32(l->stream, pos);
      pos += 4;
      if (l->nrefs < MAX_REFS) {
        l->ref[l->nrefs].site = site;
        l->ref[l->nrefs].kind = kind;
        l->nrefs += 1;
      }
      f += 1;
    }
    units += 1;
  }
  return units;
}

static uint32_t name_hash(uint8_t *stream, int32_t off, int32_t len) {
  uint32_t h = 2166136261ULL;
  int32_t i = 0;
  while (i < len) {
    h ^= (uint32_t)stream[off + i];
    h *= 16777619;
    i += 1;
  }
  return h;
}

static int32_t name_equal(uint8_t *stream, int32_t a_off, int32_t a_len, int32_t b_off, int32_t b_len) {
  if (a_len != b_len) {
    return 0;
  }
  int32_t i = 0;
  while (i < a_len) {
    if (stream[a_off + i] != stream[b_off + i]) {
      return 0;
    }
    i += 1;
  }
  return 1;
}

static void intern_defs(Linker *l) {
  int32_t i = 0;
  while (i < SLOTS) {
    l->slot[i] = -1;
    i += 1;
  }
  l->dupes = 0;
  i = 0;
  while (i < l->ndefs) {
    uint32_t h = name_hash(l->stream, l->def[i].name_off, l->def[i].name_len);
    int32_t idx = (int32_t)(h & (uint32_t)SLOT_MASK);
    int32_t placed = 0;
    while (placed == 0) {
      int32_t cur = l->slot[idx];
      if (cur < 0) {
        l->slot[idx] = i;
        placed = 1;
      } else if (name_equal(l->stream, l->def[cur].name_off, l->def[cur].name_len,
                            l->def[i].name_off, l->def[i].name_len) != 0) {
        l->dupes += 1;
        placed = 1;
      } else {
        idx = (idx + 1) & SLOT_MASK;
      }
    }
    i += 1;
  }
}

static int32_t lookup(Linker *l, int32_t off, int32_t len) {
  uint32_t h = name_hash(l->stream, off, len);
  int32_t idx = (int32_t)(h & (uint32_t)SLOT_MASK);
  while (l->slot[idx] >= 0) {
    int32_t cand = l->slot[idx];
    if (name_equal(l->stream, l->def[cand].name_off, l->def[cand].name_len, off, len) != 0) {
      return cand;
    }
    idx = (idx + 1) & SLOT_MASK;
  }
  return -1;
}

static uint32_t section_base(int32_t section) {
  return (uint32_t)(4096 + section * 262144);
}

static int32_t resolve_and_relocate(Linker *l) {
  int32_t i = 0;
  while (i < IMAGE_SIZE) {
    l->image[i] = 0;
    i += 1;
  }
  l->unresolved = 0;
  int32_t applied = 0;
  i = 0;
  while (i < l->nrefs) {
    int32_t target = lookup(l, l->ref[i].name_off, l->ref[i].name_len);
    if (target < 0) {
      l->unresolved += 1;
    } else {
      uint32_t addr = section_base(l->def[target].section) + (uint32_t)l->def[target].offset;
      int32_t site = l->ref[i].site;
      if (l->ref[i].kind == 1) {
        addr = addr - (uint32_t)site - 4;
      } else if (l->ref[i].kind == 2) {
        addr += (uint32_t)(l->ref[i].unit * 16);
      }
      put_u32(l->image, site, addr ^ get_u32(l->image, site));
      applied += 1;
    }
    i += 1;
  }
  return applied;
}

static uint64_t round_trip(Linker *l, int32_t *out_applied) {
  int32_t units = parse_stream(l);
  intern_defs(l);
  int32_t applied = resolve_and_relocate(l);

  uint64_t h = 1469598103934665603ULL;
  h = h * 31 + (uint64_t)units;
  h = h * 31 + (uint64_t)l->ndefs;
  h = h * 31 + (uint64_t)l->nrefs;
  h = h * 31 + (uint64_t)l->dupes;
  h = h * 31 + (uint64_t)l->unresolved;
  h = h * 31 + (uint64_t)applied;
  int32_t i = 0;
  while (i < IMAGE_SIZE) {
    h = h ^ (uint64_t)l->image[i];
    h *= 1099511628211ULL;
    i += 4;
  }
  *out_applied = applied;
  return h;
}

int main(void) {
    Linker l;
    l.stream = (uint8_t *)malloc(STREAM_CAP);
    l.def = (Sym *)malloc((size_t)MAX_DEFS * sizeof(Sym));
    l.ref = (Ref *)malloc((size_t)MAX_REFS * sizeof(Ref));
    l.slot = (int32_t *)malloc((size_t)SLOTS * 4);
    l.image = (uint8_t *)malloc((size_t)IMAGE_SIZE);
    l.stream_len = 0;
    l.ndefs = 0;
    l.nrefs = 0;
    l.dupes = 0;
    l.unresolved = 0;
    if (l.stream == NULL || l.def == NULL || l.ref == NULL || l.slot == NULL || l.image == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    printf("Link resolve: %d units, %d defs and %d refs each\n", UNITS, DEFS_PER, REFS_PER);

    generate_stream(&l, 2463534242ULL);

    int32_t applied = 0;
    uint64_t check = round_trip(&l, &applied);
    printf("  stream = %d bytes, defs = %d, refs = %d\n", l.stream_len, l.ndefs, l.nrefs);
    printf("  duplicates = %d, unresolved = %d, relocations = %d\n",
           l.dupes, l.unresolved, applied);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int32_t a2 = 0;
        bench_hash = bench_hash * 1000003 + round_trip(&l, &a2);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(l.stream);
    free(l.def);
    free(l.ref);
    free(l.slot);
    free(l.image);
    return 0;
}
