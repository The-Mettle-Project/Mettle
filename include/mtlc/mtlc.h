/* mtlc.h - umbrella header for libmtlc, the standalone Mettle compiler backend.
 *
 * libmtlc is a from-scratch, frontend-agnostic compiler backend: a custom IR, a
 * classical optimizer plus a GNN-driven optimizer behind a translation-validation
 * gate, native x86-64 (AVX2) / ARM64 / PTX code generation, and native PE/ELF
 * linking. Any frontend can lower its own AST into the backend IR (see mtlc/type.h
 * for the type descriptor a frontend translates into) and drive the pipeline
 * through the entry points below.
 *
 *   #include <mtlc/mtlc.h>
 *
 * The reference Mettle frontend in this repository is the first consumer of this
 * API and exercises it end to end.
 */
#ifndef MTLC_H
#define MTLC_H

#include "context.h"
#include "diag.h"
#include "intrinsic.h"
#include "memory.h"
#include "tensor.h"
#include "module.h"
#include "pipeline.h"
#include "target.h"
#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend version string (e.g. "libmtlc 0.1.0"). */
const char *mtlc_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_H */
