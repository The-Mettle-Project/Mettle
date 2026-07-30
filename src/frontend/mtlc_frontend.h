/* mtlc_frontend.h - reference-frontend adapter to libmtlc.
 *
 * These helpers live on the FRONTEND side of the backend boundary (they compile
 * into the mettle driver, not into libmtlc) because they are the one place that
 * sees both the Mettle frontend's `Type` and the backend's `MtlcType`. A frontend
 * calls mtlc_type_from_frontend() while building IR to hand the backend a type in
 * its own vocabulary.
 *
 * In Phase 1 this translation is available but not yet on the codegen hot path;
 * Phase 2 routes codegen's type queries through baked-in MtlcType so codegen no
 * longer reaches back into the frontend `Type`/`TypeChecker`. */
#ifndef MTLC_FRONTEND_H
#define MTLC_FRONTEND_H

#include "mtlc/type.h"
#include "semantic/symbol_table.h" /* frontend Type */

/* Translate a frontend Type into a backend MtlcType. The result is memoized:
 * calling twice with the same Type pointer returns the same MtlcType, so shared
 * and self-referential types (structs holding pointers to themselves) translate
 * once and cycles terminate. Returns NULL for a NULL input. The returned graph
 * is owned by the adapter's process-lifetime arena and must not be freed. */
MtlcType *mtlc_type_from_frontend(const Type *type);

#endif /* MTLC_FRONTEND_H */
