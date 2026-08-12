/* mtlc/pipeline.h - the backend pipeline: optimize -> codegen -> link.
 *
 * These are the frontend-agnostic entry points into libmtlc: the classical
 * optimizer, the GNN-driven ML optimizer, native object emission, and linking a
 * native executable. A frontend builds a module (mtlc/build.h), then drives it
 * through these stages -- see examples/calc for a complete non-Mettle frontend.
 */
#ifndef MTLC_PIPELINE_H
#define MTLC_PIPELINE_H

#include "context.h"
#include "module.h"
#include "target.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optimize without an explicit target. Modules containing kernels take the
 * conservative target-neutral, kernel-reachable path; other modules take the
 * full x86-64 pipeline. Prefer mtlc_optimize_for when the consumer is known.
 * A NULL ctx enables optimization with conservative context defaults. */
int mtlc_optimize(MtlcContext *ctx, MtlcModule *module);

/* Optimize for a concrete consumer while preserving that target's accepted IR
 * subset. X86_64 uses the full pipeline. ARM64 uses scalar target-neutral
 * transforms. PTX/SPIR-V additionally restrict work to kernel-reachable device
 * code and retain every semantic GPU operation. */
int mtlc_optimize_for(MtlcContext *ctx, MtlcModule *module, MtlcArch arch);

/* Statistics from the ML optimizer (mirrors the backend's MLOptStats). */
typedef struct {
  int proposals;
  int validated;
  int proven;
  int rejected;
  int skipped;
} MtlcMlOptStats;

/* Run the GNN-driven, translation-validation-gated ML optimizer, then hoist
 * constants. No-op unless ml-opt is enabled on `ctx`. `stats` may be NULL.
 * Returns 1 on success, 0 on error. */
int mtlc_apply_ml_opt(MtlcContext *ctx, MtlcModule *module,
                      MtlcMlOptStats *stats);

/* Generate native code for the module and write a relocatable object file (the
 * host object format: COFF on Windows, ELF elsewhere) to `path`. Run
 * mtlc_optimize first for optimized code. Returns 1 on success, 0 on error
 * (message printed to stderr). */
int mtlc_emit_object(MtlcContext *ctx, MtlcModule *module, const char *path);

/* Lower the module for `arch` and write the target's natural product to `path`:
 *   MTLC_ARCH_X86_64  a host-format relocatable object (same as mtlc_emit_object)
 *   MTLC_ARCH_ARM64   an AArch64 ELF64 relocatable object (AAPCS64)
 *   MTLC_ARCH_PTX     an NVIDIA PTX module (text), one .entry per declared kernel
 *   MTLC_ARCH_SPIRV   a SPIR-V binary module (OpenCL 2.0), one entry per kernel
 * Every path accepts unoptimized IR. For optimized portable products, call
 * mtlc_optimize_for with the same arch first; never feed ARM64/PTX/SPIR-V the
 * x86-only full-pipeline shape. Returns 1 on success, 0 on error. */
int mtlc_emit(MtlcContext *ctx, MtlcModule *module, MtlcArch arch,
              const char *path);

/* Compile the module to a native executable at `output_path`. The context must
 * name libmtlc's runtime directory with mtlc_context_set_runtime_directory.
 * The result uses Mettle startup, allocation, memory, and I/O code. It never
 * links a C runtime. Returns 1 on success and 0 on error. */
int mtlc_build_executable(MtlcContext *ctx, MtlcModule *module,
                          const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_PIPELINE_H */
