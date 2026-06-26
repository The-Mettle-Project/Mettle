#ifndef CODEGEN_BINARY_ARM64_IR_H
#define CODEGEN_BINARY_ARM64_IR_H

/* Direct IR -> AArch64 lowering for the scalar-integer subset, bypassing the
 * x86-flavored MIR. Every IR temp/local gets a stack slot (the simple non-
 * optimizing model); each instruction loads its operands, computes, and stores
 * its result. Handles labels/jumps/branches/binary/unary/assign/return/cast so
 * a real .mettle leaf function (locals, arithmetic, if/while) compiles and runs.
 * Calls, floats, pointers, and aggregates are out of scope (returns 0). */

#include "codegen/binary/arm64_emit.h"
#include "ir/ir.h"

/* Lower one IRFunction into a complete AAPCS64 function body (prologue, the
 * lowered instructions, epilogue at each return). Parameters are homed from
 * x0.. . Returns 0 (sets e->error) on an unsupported op. */
int arm64_ir_encode_function(Arm64Emit *e, const IRFunction *fn);

#endif /* CODEGEN_BINARY_ARM64_IR_H */
