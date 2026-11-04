#ifndef CODEGEN_BINARY_MIR_RELAX_H
#define CODEGEN_BINARY_MIR_RELAX_H

#include "codegen/binary/mir.h"

typedef enum {
  MIR_RELAX_BRANCH = 0,
  MIR_RELAX_PAD,
  MIR_RELAX_PIN
} MirRelaxKind;

typedef struct {
  MirRelaxKind kind;
  size_t start;
  size_t old_length;
  size_t new_length;
  size_t fixup;
  const char *label;
  unsigned char cc;
  size_t boundary;
  size_t max_pad;
  size_t phase;
  int is_loop;
  int stays_long;
  int has_target;
  size_t target;
  size_t last_back_branch;
  int has_back_branch;
} MirRelaxItem;

typedef struct {
  MirRelaxItem *items;
  size_t count;
  size_t capacity;
  long long *delta;
  int disabled;
} MirRelax;

int mir_relax_note_branch(MirRelax *r, size_t start, size_t length,
                          unsigned char cc, const char *label, size_t fixup);
int mir_relax_note_pad(MirRelax *r, size_t start, size_t length, size_t boundary,
                       size_t max_pad, int is_loop);
int mir_relax_note_pin(MirRelax *r, size_t start, size_t phase);

int mir_relax_apply(MirRelax *r, BinaryFunctionContext *ctx);
size_t mir_relax_map(const MirRelax *r, size_t old_offset);
void mir_relax_free(MirRelax *r);

#endif
