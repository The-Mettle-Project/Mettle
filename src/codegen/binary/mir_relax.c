#include "codegen/binary/mir_relax.h"

#include <stdlib.h>
#include <string.h>

#define MIR_RELAX_SHORT_LENGTH 2u
#define MIR_RELAX_MAX_ROUNDS 24
#define MIR_RELAX_PIN_BOUNDARY 32u

static MirRelaxItem *mir_relax_push(MirRelax *r) {
  if (r->count == r->capacity) {
    size_t capacity = r->capacity ? r->capacity * 2u : 32u;
    MirRelaxItem *grown =
        (MirRelaxItem *)realloc(r->items, capacity * sizeof(MirRelaxItem));
    if (!grown) {
      return NULL;
    }
    r->items = grown;
    r->capacity = capacity;
  }
  memset(&r->items[r->count], 0, sizeof(MirRelaxItem));
  return &r->items[r->count++];
}

int mir_relax_note_branch(MirRelax *r, size_t start, size_t length,
                          unsigned char cc, const char *label, size_t fixup) {
  MirRelaxItem *it = mir_relax_push(r);
  if (!it) {
    return 0;
  }
  it->kind = MIR_RELAX_BRANCH;
  it->start = start;
  it->old_length = length;
  it->new_length = length;
  it->cc = cc;
  it->label = label;
  it->fixup = fixup;
  return 1;
}

int mir_relax_note_pad(MirRelax *r, size_t start, size_t length, size_t boundary,
                       size_t max_pad) {
  MirRelaxItem *it = mir_relax_push(r);
  if (!it) {
    return 0;
  }
  it->kind = MIR_RELAX_PAD;
  it->start = start;
  it->old_length = length;
  it->new_length = length;
  it->boundary = boundary;
  it->max_pad = max_pad;
  return 1;
}

int mir_relax_note_pin(MirRelax *r, size_t start, size_t phase) {
  MirRelaxItem *it = mir_relax_push(r);
  if (!it) {
    return 0;
  }
  it->kind = MIR_RELAX_PIN;
  it->start = start;
  it->phase = phase;
  return 1;
}

static size_t mir_relax_first_ending_after(const MirRelax *r, size_t offset) {
  size_t lo = 0;
  size_t hi = r->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (r->items[mid].start + r->items[mid].old_length <= offset) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return lo;
}

size_t mir_relax_map(const MirRelax *r, size_t old_offset) {
  if (!r->delta) {
    return old_offset;
  }
  return (size_t)((long long)old_offset +
                  r->delta[mir_relax_first_ending_after(r, old_offset)]);
}

static size_t mir_relax_pad_length(size_t position, size_t boundary,
                                   size_t max_pad) {
  size_t pad = (boundary - (position & (boundary - 1u))) & (boundary - 1u);
  return pad > max_pad ? 0u : pad;
}

static int mir_relax_lay_out(MirRelax *r, int frozen) {
  long long delta = 0;
  int changed = 0;
  for (size_t i = 0; i < r->count; i++) {
    MirRelaxItem *it = &r->items[i];
    size_t position = (size_t)((long long)it->start + delta);
    size_t length = it->new_length;
    if (it->kind == MIR_RELAX_BRANCH) {
      length = it->stays_long ? it->old_length : MIR_RELAX_SHORT_LENGTH;
    } else if (it->kind == MIR_RELAX_PIN) {
      length = (it->phase - position) & (MIR_RELAX_PIN_BOUNDARY - 1u);
    } else if (!frozen) {
      length = mir_relax_pad_length(position, it->boundary, it->max_pad);
    }
    changed |= length != it->new_length;
    it->new_length = length;
    r->delta[i] = delta;
    delta += (long long)length - (long long)it->old_length;
  }
  r->delta[r->count] = delta;
  return changed;
}

static int mir_relax_mark_long_branches(MirRelax *r) {
  int grew = 0;
  for (size_t i = 0; i < r->count; i++) {
    MirRelaxItem *it = &r->items[i];
    long long from;
    long long to;
    long long disp;
    if (it->kind != MIR_RELAX_BRANCH || it->stays_long) {
      continue;
    }
    if (!it->has_target) {
      it->stays_long = 1;
      grew = 1;
      continue;
    }
    from = (long long)it->start + r->delta[i] + (long long)it->new_length;
    to = (long long)mir_relax_map(r, it->target);
    disp = to - from;
    if (disp < -128 || disp > 127) {
      it->stays_long = 1;
      grew = 1;
    }
  }
  return grew;
}

static void mir_relax_settle(MirRelax *r) {
  int frozen = 0;
  for (int round = 0;; round++) {
    int changed;
    int grew;
    frozen = frozen || round >= MIR_RELAX_MAX_ROUNDS;
    changed = mir_relax_lay_out(r, frozen);
    grew = mir_relax_mark_long_branches(r);
    if (!changed && !grew) {
      return;
    }
  }
}

static void mir_relax_write_nops(unsigned char *out, size_t pad) {
  static const unsigned char kNops[10][9] = {
      {0},
      {0x90},
      {0x66, 0x90},
      {0x0F, 0x1F, 0x00},
      {0x0F, 0x1F, 0x40, 0x00},
      {0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
      {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  while (pad > 0) {
    size_t chunk = pad > 9u ? 9u : pad;
    memcpy(out, kNops[chunk], chunk);
    out += chunk;
    pad -= chunk;
  }
}

static void mir_relax_write_short(const MirRelax *r, const MirRelaxItem *it,
                                  size_t index, unsigned char *out) {
  long long from = (long long)it->start + r->delta[index] +
                   (long long)MIR_RELAX_SHORT_LENGTH;
  long long to = (long long)mir_relax_map(r, it->target);
  out[0] = it->cc ? (unsigned char)(0x70u | (it->cc & 0x0Fu)) : 0xEBu;
  out[1] = (unsigned char)(signed char)(to - from);
}

static int mir_relax_rewrite_code(const MirRelax *r, BinaryCodeBuffer *code) {
  size_t size = (size_t)((long long)code->size + r->delta[r->count]);
  unsigned char *out = (unsigned char *)malloc(size ? size : 1u);
  size_t in_at = 0;
  size_t out_at = 0;
  if (!out) {
    return 0;
  }
  for (size_t i = 0; i < r->count; i++) {
    const MirRelaxItem *it = &r->items[i];
    memcpy(out + out_at, code->data + in_at, it->start - in_at);
    out_at += it->start - in_at;
    if (it->kind == MIR_RELAX_BRANCH) {
      if (it->stays_long) {
        memcpy(out + out_at, code->data + it->start, it->old_length);
      } else {
        mir_relax_write_short(r, it, i, out + out_at);
      }
    } else {
      mir_relax_write_nops(out + out_at, it->new_length);
    }
    out_at += it->new_length;
    in_at = it->start + it->old_length;
  }
  memcpy(out + out_at, code->data + in_at, code->size - in_at);
  free(code->data);
  code->data = out;
  code->size = size;
  code->capacity = size ? size : 1u;
  return 1;
}

static void mir_relax_map_fixups(const MirRelax *r, BinaryFunctionContext *ctx) {
  BinaryLabelFixupTable *table = &ctx->label_fixups;
  size_t kept = 0;
  for (size_t i = 0; i < r->count; i++) {
    const MirRelaxItem *it = &r->items[i];
    if (it->kind == MIR_RELAX_BRANCH && !it->stays_long) {
      free(table->items[it->fixup].name);
      table->items[it->fixup].name = NULL;
    }
  }
  for (size_t i = 0; i < table->count; i++) {
    if (table->items[i].name) {
      table->items[kept] = table->items[i];
      table->items[kept].displacement_offset =
          mir_relax_map(r, table->items[kept].displacement_offset);
      kept++;
    }
  }
  table->count = kept;
}

static void mir_relax_map_tables(const MirRelax *r, BinaryFunctionContext *ctx) {
  for (size_t i = 0; i < ctx->labels.count; i++) {
    ctx->labels.items[i].offset = mir_relax_map(r, ctx->labels.items[i].offset);
  }
  for (size_t i = 0; i < ctx->call_relocations.count; i++) {
    ctx->call_relocations.items[i].displacement_offset =
        mir_relax_map(r, ctx->call_relocations.items[i].displacement_offset);
  }
  for (size_t i = 0; i < ctx->asm_relocations.count; i++) {
    ctx->asm_relocations.items[i].offset =
        mir_relax_map(r, ctx->asm_relocations.items[i].offset);
  }
  for (size_t i = 0; i < ctx->return_fixups.count; i++) {
    ctx->return_fixups.items[i] = mir_relax_map(r, ctx->return_fixups.items[i]);
  }
  for (size_t i = 0; i < ctx->debug_export_labels.count; i++) {
    ctx->debug_export_labels.items[i].offset =
        mir_relax_map(r, ctx->debug_export_labels.items[i].offset);
  }
}

static void mir_relax_resolve_targets(MirRelax *r, BinaryFunctionContext *ctx) {
  for (size_t i = 0; i < r->count; i++) {
    MirRelaxItem *it = &r->items[i];
    if (it->kind == MIR_RELAX_BRANCH) {
      BinaryLabelEntry *label = binary_label_table_get(&ctx->labels, it->label);
      if (label) {
        it->has_target = 1;
        it->target = label->offset;
      }
    }
  }
}

int mir_relax_apply(MirRelax *r, BinaryFunctionContext *ctx) {
  if (r->disabled || r->count == 0) {
    return 1;
  }
  r->delta = (long long *)calloc(r->count + 1u, sizeof(long long));
  if (!r->delta) {
    return 0;
  }
  mir_relax_resolve_targets(r, ctx);
  mir_relax_settle(r);
  if (!mir_relax_rewrite_code(r, &ctx->code)) {
    return 0;
  }
  mir_relax_map_fixups(r, ctx);
  mir_relax_map_tables(r, ctx);
  return 1;
}

void mir_relax_free(MirRelax *r) {
  free(r->items);
  free(r->delta);
  memset(r, 0, sizeof(*r));
}
