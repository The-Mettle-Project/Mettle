#ifndef CODEGEN_BINARY_ARM64_IR_H
#define CODEGEN_BINARY_ARM64_IR_H

/* Direct IR -> AArch64 lowering for the scalar subset, bypassing the
 * x86-flavored MIR. Every IR temp/local gets a stack slot (the simple non-
 * optimizing model); each instruction loads its operands, computes, and stores
 * its result. Covers labels/jumps/branches/binary/unary/assign/return/cast,
 * scalar floats, pointers and address-of, module globals, heap allocation, and
 * both direct and indirect (function-pointer) calls under AAPCS64 (args in
 * x0.. and v0.., result in x0/d0). Aggregate-by-value calls and the optimized
 * SIMD super-ops are out of scope; each one that is missing names itself in the
 * failure reason (see arm64_error_reason). */

#include "codegen/binary/arm64_emit.h"
#include "ir/ir.h"

/* Lower one IRFunction body (prologue, instructions, epilogue at each return).
 * Single-function mode: IR_OP_CALL is unsupported (use the program emitter). */
int arm64_ir_encode_function(Arm64Emit *e, const IRFunction *fn);

/* Lower a whole IRProgram: a _start that calls `entry` (default "main") and
 * exits with its return value, followed by every function body, with cross-
 * function calls resolved through the shared label table. The caller finalizes
 * the emitter. Returns 0 (sets e->error) on an unsupported op.
 *
 * On success `*data_out` receives the malloc'd writable data image (module
 * globals and the heap-allocator state) to hand to arm64_write_elf; the caller
 * frees it. Both out-parameters may be NULL to discard it. */
int arm64_ir_encode_program(Arm64Emit *e, const IRProgram *prog,
                            const char *entry, unsigned char **data_out,
                            size_t *data_len_out);

/* Emit an AArch64 ELF64 relocatable object suitable for the native system
 * linker. Unlike arm64_ir_encode_program, this has no synthetic _start: it
 * preserves normal function/global linkage, external calls, and .rodata/.data
 * relocations. `error` may be NULL. */
int arm64_ir_write_object(const IRProgram *prog, const char *path, char *error,
                          size_t error_capacity);

/* Write `code` as a minimal static AArch64 ELF executable (entry at the code
 * start, where _start lives) with `data` as a second, writable PT_LOAD segment
 * at a fixed virtual address. Returns 0 on I/O failure, or when the text is
 * large enough to reach that address. */
int arm64_write_elf(const char *path, const unsigned char *code, size_t len,
                    const unsigned char *data, size_t data_len);

#endif /* CODEGEN_BINARY_ARM64_IR_H */
