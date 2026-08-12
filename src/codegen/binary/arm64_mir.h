#ifndef CODEGEN_BINARY_ARM64_MIR_H
#define CODEGEN_BINARY_ARM64_MIR_H

/* MIR -> AArch64 lowering. Translates the architecture-neutral scalar-integer
 * MIR subset (moves, ALU, compares, conditional set/branch, shifts, mul, ret)
 * to A64 via the emit layer. Operands are expected already resolved to physical
 * ARM registers (MIR_OPK_PHYS), immediates, or labels -- register allocation is
 * a separate brick.
 *
 * The x86-modeled MIR ops (CQO/RDX:RAX divide, CL shifts, fixed-register setcc
 * staging) are intentionally NOT handled here; they need an ARM-aware mir_lower.
 * The `cc` field carries an x86 condition opcode, translated by
 * arm64_cond_from_x86_cc (the condition lives in its low nibble). */

#include "codegen/binary/arm64_emit.h"
#include "codegen/binary/mir.h"

/* Map an x86 condition opcode (setcc 0x90+n / jcc 0x80+n / short 0x70+n; the
 * condition is the low nibble) to the equivalent AArch64 condition. */
Arm64Cond arm64_cond_from_x86_cc(unsigned char x86_cc);

/* Lower a scalar MIR instruction sequence into a complete AArch64 function:
 * emits the AAPCS64 prologue, translates each instruction, and turns MIR_RET
 * into the epilogue + ret. Branch/label operands use the MIR_OPK_LABEL `dst`
 * slot. Operands are physical registers (MIR_OPK_PHYS) or immediates. Returns 0
 * (sets e->error) on an unsupported op or emit failure. */
int arm64_mir_encode_seq(Arm64Emit *e, const MirInst *insns, size_t count);

/* Lower a vreg-based scalar MIR sequence, doing its own stack-slot allocation:
 * every vreg gets an 8-byte frame slot, the first `nparams` vregs are homed from
 * x0.. on entry, and each op loads its sources / stores its result through those
 * slots -- the simple non-optimizing model that consumes mir_lower's vreg output
 * directly. MIR_RET returns the vreg named by operand `a` (or void if NONE). */
int arm64_mir_encode_vregs(Arm64Emit *e, const MirInst *insns, size_t count,
                           int nvregs, int nparams);

#endif /* CODEGEN_BINARY_ARM64_MIR_H */
