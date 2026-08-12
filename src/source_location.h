/* source_location.h - a neutral source position shared by the frontend AST and
 * the backend IR.
 *
 * A source location (file/line/column) is not specific to any frontend's AST or
 * to the backend's IR; both need to carry it for diagnostics and debug info. It
 * lives here, in a dependency-free header, so the backend IR (ir.h) can reference
 * it without pulling in a frontend's AST header. */
#ifndef MTLC_SOURCE_LOCATION_H
#define MTLC_SOURCE_LOCATION_H

#include <stddef.h>

typedef struct {
  size_t line;
  size_t column;
  const char *filename;
} SourceLocation;

#endif /* MTLC_SOURCE_LOCATION_H */
