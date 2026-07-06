#ifndef CODEGEN_BINARY_ARM64_IR_H
#define CODEGEN_BINARY_ARM64_IR_H

/* Direct IR -> AArch64 lowering for the scalar-integer subset, bypassing the
 * x86-flavored MIR. Every IR temp/local gets a stack slot (the simple non-
 * optimizing model); each instruction loads its operands, computes, and stores
 * its result. Supports labels/jumps/branches/binary/unary/assign/return/cast
 * and function calls (AAPCS64: args in x0.., result in x0), so multi-function
 * .mettle programs with recursion compile and run. Floats, pointers, and
 * aggregates are out of scope (returns 0). */

#include "codegen/binary/arm64_emit.h"
#include "ir/ir.h"

/* Lower one IRFunction body (prologue, instructions, epilogue at each return).
 * Single-function mode: IR_OP_CALL is unsupported (use the program emitter). */
int arm64_ir_encode_function(Arm64Emit *e, const IRFunction *fn);

/* Lower a whole IRProgram: a _start that calls `entry` (default "main") and
 * exits with its return value, followed by every function body, with cross-
 * function calls resolved through the shared label table. The caller finalizes
 * the emitter. Returns 0 (sets e->error) on an unsupported op. */
int arm64_ir_encode_program(Arm64Emit *e, const IRProgram *prog,
                            const char *entry);

/* Write `code` as a minimal static AArch64 ELF executable (entry at the code
 * start, where _start lives). Returns 0 on I/O failure. */
int arm64_write_elf(const char *path, const unsigned char *code, size_t len);

#endif /* CODEGEN_BINARY_ARM64_IR_H */
