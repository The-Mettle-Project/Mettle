#include "codegen/binary/arm64_emit.h"

#include <stdlib.h>
#include <string.h>

void arm64_emit_init(Arm64Emit *e) { memset(e, 0, sizeof(*e)); }

void arm64_emit_free(Arm64Emit *e) {
  free(e->code.data);
  free(e->label_off);
  free(e->label_bound);
  free(e->fixups);
  memset(e, 0, sizeof(*e));
}

size_t arm64_here(const Arm64Emit *e) { return e->code.len; }

static int buf_reserve(Arm64Buf *b, size_t need) {
  if (b->len + need <= b->cap) {
    return 1;
  }
  size_t cap = b->cap ? b->cap * 2 : 64;
  while (cap < b->len + need) {
    cap *= 2;
  }
  unsigned char *grown = realloc(b->data, cap);
  if (!grown) {
    return 0;
  }
  b->data = grown;
  b->cap = cap;
  return 1;
}

int arm64_emit_word(Arm64Emit *e, uint32_t word) {
  if (e->error || !buf_reserve(&e->code, 4)) {
    e->error = 1;
    return 0;
  }
  /* A64 instruction stream is little-endian; build host is LE so a raw copy. */
  memcpy(e->code.data + e->code.len, &word, 4);
  e->code.len += 4;
  return 1;
}

int arm64_emit_mov(Arm64Emit *e, int is64, Arm64Reg rd, Arm64Reg rn) {
  if (rd == ARM64_SP || rn == ARM64_SP) {
    return arm64_emit_word(e, arm64_mov_sp(rd, rn));
  }
  return arm64_emit_word(e, arm64_mov_reg(is64, rd, rn));
}

int arm64_emit_bytes(Arm64Emit *e, const void *data, size_t len) {
  size_t pad = (4 - (len & 3)) & 3;
  if (e->error || !buf_reserve(&e->code, len + pad)) {
    e->error = 1;
    return 0;
  }
  memcpy(e->code.data + e->code.len, data, len);
  e->code.len += len;
  memset(e->code.data + e->code.len, 0, pad);
  e->code.len += pad;
  return 1;
}

int arm64_new_label(Arm64Emit *e) {
  if (e->error) {
    return -1;
  }
  if (e->label_count == e->label_cap) {
    int cap = e->label_cap ? e->label_cap * 2 : 16;
    size_t *off = realloc(e->label_off, (size_t)cap * sizeof(*off));
    int *bound = realloc(e->label_bound, (size_t)cap * sizeof(*bound));
    if (off) e->label_off = off;
    if (bound) e->label_bound = bound;
    if (!off || !bound) {
      e->error = 1;
      return -1;
    }
    e->label_cap = cap;
  }
  int id = e->label_count++;
  e->label_off[id] = 0;
  e->label_bound[id] = 0;
  return id;
}

void arm64_bind_label(Arm64Emit *e, int label) {
  if (e->error || label < 0 || label >= e->label_count) {
    e->error = 1;
    return;
  }
  e->label_off[label] = e->code.len;
  e->label_bound[label] = 1;
}

static int add_fixup(Arm64Emit *e, size_t at, int label, Arm64FixKind kind) {
  if (e->fixup_count == e->fixup_cap) {
    int cap = e->fixup_cap ? e->fixup_cap * 2 : 16;
    Arm64Fixup *f = realloc(e->fixups, (size_t)cap * sizeof(*f));
    if (!f) {
      e->error = 1;
      return 0;
    }
    e->fixups = f;
    e->fixup_cap = cap;
  }
  e->fixups[e->fixup_count].at = at;
  e->fixups[e->fixup_count].label = label;
  e->fixups[e->fixup_count].kind = kind;
  e->fixup_count++;
  return 1;
}

int arm64_emit_b(Arm64Emit *e, int label) {
  size_t at = e->code.len;
  return arm64_emit_word(e, arm64_b(0)) && add_fixup(e, at, label, ARM64_FIX_B26);
}
int arm64_emit_bl(Arm64Emit *e, int label) {
  size_t at = e->code.len;
  return arm64_emit_word(e, arm64_bl(0)) &&
         add_fixup(e, at, label, ARM64_FIX_B26);
}
int arm64_emit_bcond(Arm64Emit *e, Arm64Cond cond, int label) {
  size_t at = e->code.len;
  return arm64_emit_word(e, arm64_bcond(cond, 0)) &&
         add_fixup(e, at, label, ARM64_FIX_IMM19);
}
int arm64_emit_cbz(Arm64Emit *e, int is64, Arm64Reg rt, int label) {
  size_t at = e->code.len;
  return arm64_emit_word(e, arm64_cbz(is64, rt, 0)) &&
         add_fixup(e, at, label, ARM64_FIX_IMM19);
}
int arm64_emit_cbnz(Arm64Emit *e, int is64, Arm64Reg rt, int label) {
  size_t at = e->code.len;
  return arm64_emit_word(e, arm64_cbnz(is64, rt, 0)) &&
         add_fixup(e, at, label, ARM64_FIX_IMM19);
}

int arm64_emit_finalize(Arm64Emit *e) {
  if (e->error) {
    return 0;
  }
  for (int i = 0; i < e->fixup_count; i++) {
    Arm64Fixup *f = &e->fixups[i];
    if (f->label < 0 || f->label >= e->label_count || !e->label_bound[f->label]) {
      e->error = 1;
      return 0;
    }
    long disp = (long)e->label_off[f->label] - (long)f->at;
    long words = disp >> 2;
    uint32_t word;
    memcpy(&word, e->code.data + f->at, 4);
    if (f->kind == ARM64_FIX_B26) {
      if (words < -(1L << 25) || words >= (1L << 25)) {
        e->error = 1;
        return 0;
      }
      word = (word & ~0x03FFFFFFu) | ((uint32_t)words & 0x03FFFFFFu);
    } else { /* ARM64_FIX_IMM19 */
      if (words < -(1L << 18) || words >= (1L << 18)) {
        e->error = 1;
        return 0;
      }
      word = (word & ~(0x7FFFFu << 5)) | (((uint32_t)words & 0x7FFFFu) << 5);
    }
    memcpy(e->code.data + f->at, &word, 4);
  }
  return 1;
}

/* mov to/from SP is the add-immediate-#0 form (reg 31 = SP there), NOT the
 * ORR/mov-reg form (reg 31 = XZR). */
int arm64_emit_prologue(Arm64Emit *e, int frame_bytes, const Arm64Reg *saved,
                        int n_saved) {
  if (frame_bytes < 0 || (frame_bytes & 15) != 0) {
    e->error = 1;
    return 0;
  }
  int ok = arm64_emit_word(e, arm64_stp_pre(1, ARM64_X29, ARM64_X30, ARM64_SP,
                                            -16)) &&
           arm64_emit_word(e, arm64_mov_sp(ARM64_X29, ARM64_SP));
  if (ok && frame_bytes > 0 && frame_bytes <= 4095) {
    ok = arm64_emit_word(e, arm64_sub_imm(1, ARM64_SP, ARM64_SP,
                                          (uint32_t)frame_bytes, 0));
  } else if (ok && frame_bytes > 4095) {
    /* Large frame: materialize the size in the IP0 scratch (x16) and subtract
     * via register. The epilogue restores sp from x29, so no symmetric work. */
    uint32_t v = (uint32_t)frame_bytes;
    ok = arm64_emit_word(e, arm64_movz(1, ARM64_X16, (uint16_t)(v & 0xFFFF), 0));
    if (ok && (v >> 16)) {
      ok = arm64_emit_word(e,
                           arm64_movk(1, ARM64_X16, (uint16_t)(v >> 16), 1));
    }
    ok = ok && arm64_emit_word(e, arm64_sub_reg(1, ARM64_SP, ARM64_SP,
                                                ARM64_X16));
  }
  for (int i = 0; ok && i < n_saved; i++) {
    ok = arm64_emit_word(e, arm64_str_imm(1, saved[i], ARM64_SP, 8 * i));
  }
  if (!ok) {
    e->error = 1;
  }
  return ok;
}

int arm64_emit_epilogue(Arm64Emit *e, int frame_bytes, const Arm64Reg *saved,
                        int n_saved) {
  (void)frame_bytes;
  int ok = 1;
  for (int i = 0; ok && i < n_saved; i++) {
    ok = arm64_emit_word(e, arm64_ldr_imm(1, saved[i], ARM64_SP, 8 * i));
  }
  /* Restore sp to the FP/LR save slot (mov sp, x29), pop {FP,LR}, return. */
  ok = ok && arm64_emit_word(e, arm64_mov_sp(ARM64_SP, ARM64_X29)) &&
       arm64_emit_word(e, arm64_ldp_post(1, ARM64_X29, ARM64_X30, ARM64_SP,
                                         16)) &&
       arm64_emit_word(e, arm64_ret(ARM64_X30));
  if (!ok) {
    e->error = 1;
  }
  return ok;
}
