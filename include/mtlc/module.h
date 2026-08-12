/* mtlc/module.h - a unit of IR the backend optimizes and lowers to code.
 *
 * MtlcModule is an opaque handle wrapping the backend's IR program. A frontend
 * produces IR -- either through the public IR builder (mtlc/build.h) or, like
 * the reference Mettle frontend, via its own lowering pass -- and hands it to a
 * module; the backend pipeline (optimize -> codegen -> link, in mtlc/pipeline.h)
 * then operates on the module without any knowledge of the frontend that built
 * it.
 *
 * The IR program is referenced through an opaque `void *` here so this public
 * header stays independent of the backend's internal IR layout (src/ir/ir.h).
 */
#ifndef MTLC_MODULE_H
#define MTLC_MODULE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtlcModule MtlcModule;

/* Wrap an already-lowered IR program (an `IRProgram *`, passed opaquely) into a
 * module. The module takes ownership: mtlc_module_destroy frees the IR program.
 * Returns NULL on allocation failure. */
MtlcModule *mtlc_module_adopt_ir(void *ir_program);

/* Borrow the underlying IR program (an `IRProgram *`). The module retains
 * ownership. Returns NULL for a NULL module. */
void *mtlc_module_ir(MtlcModule *module);

/* Number of functions in the module (0 if unknown/NULL). */
size_t mtlc_module_function_count(const MtlcModule *module);

/* Destroy the module and the IR program it owns. */
void mtlc_module_destroy(MtlcModule *module);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_MODULE_H */
