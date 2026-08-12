/* mtlc/context.h - a backend session handle.
 *
 * MtlcContext replaces the process-global compiler state the backend historically
 * relied on (explain sinks, ML-opt model paths, optimization knobs). Holding this
 * state on an explicit handle is what lets any frontend drive libmtlc, and is a
 * prerequisite for reentrancy: the backend keeps its remaining mutable
 * per-compile state thread-local, so separate threads can each drive a context.
 *
 * The handle owns the high-level knobs below and is threaded through the mtlc_*
 * pipeline entry points (optimize, ML-opt, codegen, link) in mtlc/pipeline.h.
 */
#ifndef MTLC_CONTEXT_H
#define MTLC_CONTEXT_H

#include "diag.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtlcContext MtlcContext;

/* Create/destroy a backend session. Returns NULL on allocation failure. */
MtlcContext *mtlc_context_create(void);
void mtlc_context_destroy(MtlcContext *ctx);

/* Route every pipeline diagnostic -- optimize, ML-opt, codegen, link -- to
 * `handler` instead of stderr. Pass a NULL handler to restore the default,
 * which writes errors to stderr as "mtlc: <message>". `user_data` is passed
 * back to the handler unchanged and is neither copied nor freed. */
void mtlc_context_set_diagnostic_handler(MtlcContext *ctx,
                                         MtlcDiagHandler handler,
                                         void *user_data);

/* The most recent error reported through this context, or NULL if none has
 * been. The string belongs to the context and is replaced by the next error,
 * so copy it if you keep it. This is the low-ceremony alternative to a
 * handler: call a pipeline entry point, and on a 0 return ask what went
 * wrong. */
const char *mtlc_context_last_error(const MtlcContext *ctx);

/* Forget the recorded error, so a later mtlc_context_last_error reports only
 * what happened after this call. */
void mtlc_context_clear_error(MtlcContext *ctx);

/* Directory containing libmtlc's target runtime objects. Executable builds
 * require freestanding.obj on Windows or freestanding.o on ELF hosts. The
 * context copies the path. Returns 1 on success. */
int mtlc_context_set_runtime_directory(MtlcContext *ctx, const char *path);
const char *mtlc_context_runtime_directory(const MtlcContext *ctx);

/* Optimization level: 0 = none, >=1 = the classical pipeline. */
void mtlc_context_set_opt_level(MtlcContext *ctx, int level);
int mtlc_context_opt_level(const MtlcContext *ctx);

/* Enable the GNN-driven, translation-validation-gated ML optimizer. */
void mtlc_context_set_ml_opt(MtlcContext *ctx, int enabled);
int mtlc_context_ml_opt(const MtlcContext *ctx);

/* Whole-program mode: set when a single executable link with a known entry
 * point makes every call site visible (gates whole-program transforms). */
void mtlc_context_set_whole_program(MtlcContext *ctx, int enabled);
int mtlc_context_whole_program(const MtlcContext *ctx);

/* --explain: report each optimization decision. focus_file, when non-NULL,
 * limits remarks to that source file. The string is borrowed, not copied. */
void mtlc_context_set_explain(MtlcContext *ctx, int enabled,
                              const char *focus_file);
int mtlc_context_explain(const MtlcContext *ctx);
const char *mtlc_context_explain_focus_file(const MtlcContext *ctx);

/* PTX code-generation target. `target` is a PTX target ISA name such as
 * "sm_121a" (GB10 performance profile), "sm_121" (compatible GB10 baseline),
 * or "compute_75" (portable baseline); `isa_major.minor` selects
 * the emitted `.version`. The context copies the target string. Returns 1 on
 * success, 0 for a malformed/oversized target or version. A new context
 * defaults to PTX 8.8 / sm_121a, the NVIDIA GB10 performance profile. */
int mtlc_context_set_ptx_target(MtlcContext *ctx, const char *target,
                                int isa_major, int isa_minor);
const char *mtlc_context_ptx_target(const MtlcContext *ctx);
int mtlc_context_ptx_isa_major(const MtlcContext *ctx);
int mtlc_context_ptx_isa_minor(const MtlcContext *ctx);

/* Backend-only tensor residency policy. `tuple_budget == 0` selects the PTX
 * backend's architecture default; 1..4096 supplies an explicit logical
 * fragment-tuple ceiling used to choose resident versus exact replay lowering.
 * This does not alter frontend/shared-IR semantics. Returns 0 on invalid input
 * without changing the context. */
int mtlc_context_set_ptx_tensor_tuple_budget(MtlcContext *ctx,
                                             int tuple_budget);
int mtlc_context_ptx_tensor_tuple_budget(const MtlcContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_CONTEXT_H */
