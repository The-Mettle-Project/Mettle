#ifndef SPIRV_EMITTER_H
#define SPIRV_EMITTER_H

#include "code_generator.h"
#include "ir/ir.h"
#include <stdio.h>

/* Lower an IR program to a SPIR-V binary module (one OpEntryPoint per kernel,
 * execution model Kernel), targeting the OpenCL 2.0 execution environment:
 * Physical64 addressing, the Kernel capability, and the OpenCL memory model.
 * This is the flavor that fits Mettle's GPU kernel ABI -- kernels take raw
 * typed pointers as parameters and perform pointer arithmetic + loads/stores
 * plus semantic work-item intrinsics, i.e. the CUDA/OpenCL model. (Vulkan/Logical
 * SPIR-V would force a descriptor-buffer redesign of the kernel model.)
 *
 * The emitter is the SPIR-V sibling of ptx_emitter.c and covers the same IR
 * subset: scalar/pointer parameters, locals, load/store, binary/unary/cast,
 * work-item queries, workgroup barriers, f32 math, h2f/f2h fp16 conversion,
 * and unsigned atomics. These arrive as MtlcIntrinsic identities, independent
 * of the source frontend's spelling.
 *
 * Control flow maps directly onto SPIR-V blocks (OpBranch /
 * OpBranchConditional), exactly like PTX `bra`: SPIR-V's structured-control-flow
 * rules are mandated only by the Shader capability, and Kernel (OpenCL) modules
 * may branch freely (spirv-val --target-env opencl2.0 confirms). Values that
 * cross basic blocks live in Function-storage variables (reg2mem); a driver's
 * SPIR-V consumer promotes them back to registers.
 *
 * Returns 1 on success. On failure returns 0 and, if error is non-NULL, sets
 * *error to a malloc'd message the caller must free. */
int spirv_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                       char **error);

#endif /* SPIRV_EMITTER_H */
