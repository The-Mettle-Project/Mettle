#ifndef PTX_EMITTER_H
#define PTX_EMITTER_H

#include "code_generator.h"
#include "ir/ir.h"
#include <stdio.h>

/* Lower an IR program to NVIDIA PTX text (one `.visible .entry` per function),
 * targeting the CUDA Driver API's cuModuleLoadData JIT. Every function in the
 * program is treated as a GPU kernel. Type resolution for symbol operands uses
 * `generator` (its symbol table / type checker); temp/SSA value types are
 * inferred from each defining instruction. PTX has unlimited typed virtual
 * registers, so there is no register allocation -- each IR value maps to a
 * fresh %r/%rd/%f/%fd/%p register of the appropriate class.
 *
 * GPU thread/block indices are exposed to kernel source as nullary intrinsics
 * the emitter recognizes by name: gpu_tid_x/y/z, gpu_ntid_x/y/z (blockDim),
 * gpu_ctaid_x/y/z (blockIdx), gpu_nctaid_x/y/z (gridDim), and gpu_barrier().
 *
 * Returns 1 on success. On failure returns 0 and, if error is non-NULL, sets
 * *error to a malloc'd message the caller must free. */
int ptx_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                     char **error);

#endif /* PTX_EMITTER_H */
