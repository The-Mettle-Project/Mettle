#ifndef CODEGEN_BINARY_ARM64_EMIT_H
#define CODEGEN_BINARY_ARM64_EMIT_H

/* AArch64 emit layer: a growable instruction buffer with label/branch fixups
 * and AAPCS64 frame helpers, over the pure encoders in arm64_encode.c. Branches
 * to labels are emitted with a zero displacement and a recorded fixup;
 * arm64_emit_finalize patches them once every label offset is known. */

#include "codegen/binary/arm64.h"

#include <stddef.h>

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} Arm64Buf;

typedef enum {
  ARM64_FIX_B26 = 0,  /* B/BL imm26 */
  ARM64_FIX_IMM19 = 1 /* B.cond/CBZ/CBNZ imm19 */
} Arm64FixKind;

typedef struct {
  size_t at;
  int label;
  Arm64FixKind kind;
} Arm64Fixup;

typedef struct {
  Arm64Buf code;
  size_t *label_off;
  int *label_bound;
  int label_count;
  int label_cap;
  Arm64Fixup *fixups;
  int fixup_count;
  int fixup_cap;
  int error;
} Arm64Emit;

void arm64_emit_init(Arm64Emit *e);
void arm64_emit_free(Arm64Emit *e);
size_t arm64_here(const Arm64Emit *e);
int arm64_emit_word(Arm64Emit *e, uint32_t word);
/* Append raw bytes (e.g. an embedded string literal), padding to a 4-byte
 * boundary so following instructions stay aligned. */
int arm64_emit_bytes(Arm64Emit *e, const void *data, size_t len);

/* Register move that picks the correct encoding automatically: the add-#0 form
 * when either operand is SP (reg 31), the ORR form otherwise. Prefer this over
 * raw arm64_mov_reg so an SP operand can never silently become XZR. */
int arm64_emit_mov(Arm64Emit *e, int is64, Arm64Reg rd, Arm64Reg rn);

int arm64_new_label(Arm64Emit *e);
void arm64_bind_label(Arm64Emit *e, int label);

int arm64_emit_b(Arm64Emit *e, int label);
int arm64_emit_bl(Arm64Emit *e, int label);
int arm64_emit_bcond(Arm64Emit *e, Arm64Cond cond, int label);
int arm64_emit_cbz(Arm64Emit *e, int is64, Arm64Reg rt, int label);
int arm64_emit_cbnz(Arm64Emit *e, int is64, Arm64Reg rt, int label);

/* Patch every fixup against bound labels. Returns 0 (sets e->error) on an
 * unbound target or an out-of-range displacement. */
int arm64_emit_finalize(Arm64Emit *e);

/* AAPCS64 frame: prologue saves {FP,LR} via stp x29,x30,[sp,#-16]!, sets
 * x29=sp, lowers sp by frame_bytes (16-aligned, <=4095), and stores each
 * `saved` register at [sp,#8*i]; epilogue reverses it and returns. */
int arm64_emit_prologue(Arm64Emit *e, int frame_bytes, const Arm64Reg *saved,
                        int n_saved);
int arm64_emit_epilogue(Arm64Emit *e, int frame_bytes, const Arm64Reg *saved,
                        int n_saved);

#endif /* CODEGEN_BINARY_ARM64_EMIT_H */
