#ifndef CODEGEN_BINARY_MIR_COLOR_H
#define CODEGEN_BINARY_MIR_COLOR_H

#include "codegen/binary/mir.h"
#include "codegen/binary/mir_cfg.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  MirCfg cfg;
  int valid;
  uint32_t *clobbered;
  unsigned char *a_dies;
  unsigned char *b_dies;
  unsigned char *undef_live;
  int max_live_gp;
  int max_live_xmm;
  size_t max_live_at;
} MirRaFacts;

typedef struct {
  uint32_t *idx;
  uint64_t *bits;
  uint32_t count;
  uint32_t capacity;
} MirInterRow;

typedef struct {
  MirFunction *fn;
  const MirRaFacts *facts;
  size_t count;
  size_t words;
  uint64_t *inter;
  MirInterRow *rows;
  uint32_t *mask;
  int *degree;
  int *cost;
  int *colorable;
  int *removed;
  int *reg_count;
  long long *metric;
  MirVregId *stack;
  MirVregId *narrow_src;
  unsigned char *use_depth;
  MirVregId *rep;
  int *phys_hint;
  unsigned char *avoid_shift_count;
  MirVregId *copy_partner;
  size_t merged_copies;
  unsigned long long coalesce_work;
  unsigned long long coalesce_budget;
} MirColorState;

static inline size_t mir_inter_row_len(const MirColorState *st, size_t a) {
  return st->rows ? st->rows[a].count : st->words;
}

static inline uint64_t mir_inter_row_bits(const MirColorState *st, size_t a,
                                          size_t w) {
  return st->rows ? st->rows[a].bits[w] : st->inter[a * st->words + w];
}

static inline uint64_t mir_inter_row_base(const MirColorState *st, size_t a,
                                          size_t w) {
  return st->rows ? (uint64_t)st->rows[a].idx[w] * 64u : (uint64_t)w * 64u;
}

#define MIR_INTER_FOR_EACH(st, a, bvar)                                        \
  for (size_t w_ = 0, n_ = mir_inter_row_len((st), (size_t)(a)); w_ < n_;     \
       w_++)                                                                   \
    for (uint64_t bits_ = mir_inter_row_bits((st), (size_t)(a), w_),          \
                  base_ = mir_inter_row_base((st), (size_t)(a), w_), bvar;    \
         bits_ && ((bvar = base_ + (size_t)__builtin_ctzll(bits_)), 1);        \
         bits_ &= bits_ - 1)

static inline uint32_t mir_inter_row_find(const MirInterRow *row,
                                          uint32_t word) {
  uint32_t lo = 0;
  uint32_t hi = row->count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    if (row->idx[mid] < word) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static inline int mir_inter_get(const MirColorState *st, size_t a, size_t b) {
  if (st->rows) {
    const MirInterRow *row = &st->rows[a];
    uint32_t at = mir_inter_row_find(row, (uint32_t)(b >> 6));
    return at < row->count && row->idx[at] == (uint32_t)(b >> 6) &&
           ((row->bits[at] >> (b & 63)) & 1u);
  }
  return (int)((st->inter[a * st->words + (b >> 6)] >> (b & 63)) & 1u);
}

void mir_inter_set(MirColorState *st, size_t a, size_t b);
void mir_inter_clear(MirColorState *st, size_t a, size_t b);
void mir_inter_clear_row(MirColorState *st, size_t a);

MirVregId mir_color_find(const MirColorState *st, MirVregId v);
int mir_color_coalesce(MirColorState *st);
void mir_color_note_phys_hints(MirColorState *st);
void mir_color_mirror_members(MirColorState *st);

#endif
