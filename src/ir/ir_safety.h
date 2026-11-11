#ifndef IR_SAFETY_H
#define IR_SAFETY_H

#include "ir.h"
#include <stddef.h>

typedef struct {
  size_t emitted;
  size_t proved;
  size_t hoisted;
  size_t spanned;
  size_t exempt;
  size_t extent_tests;
  size_t region_calls;
} IRSafetyStats;

typedef enum {
  IR_SAFETY_INTRINSIC_NONE,
  IR_SAFETY_INTRINSIC_CHECK,
  IR_SAFETY_INTRINSIC_READ_ORIGIN,
  IR_SAFETY_INTRINSIC_WRITE_ORIGIN,
  IR_SAFETY_INTRINSIC_LIFETIME
} IRSafetyIntrinsic;
IRSafetyIntrinsic ir_safety_intrinsic(const IRInstruction *instruction);

int ir_safety_analyze_origins(IRProgram *program);

int ir_safety_elide_inlined_frames(IRProgram *program);

int ir_safety_resolve_program(IRProgram *program, IRSafetyStats *stats);

int ir_safety_register_allocations(IRProgram *program);

int ir_safety_retire_dangling_notes(IRProgram *program);

#endif
