/* Internal allocator for the Mettle toolchain.
 *
 * A Mettle compile is allocation-bound: a 226k-line input performs ~10.5M
 * mallocs, 73% of them 16 bytes or less, and holds ~5.7M live objects at peak.
 * Under the platform heap (RtlAllocateHeap on Windows) that costs roughly 45%
 * of total wall-clock and ~890MB of working set -- about 156 bytes resident per
 * live object, nearly all of it per-block bookkeeping.
 *
 * mettle_alloc.c replaces malloc/calloc/realloc/free with a size-class
 * allocator that carries no per-object header, so a 16-byte request occupies
 * exactly 16 bytes. It interposes on the CRT symbols, so the ~3700 existing
 * allocation sites are untouched; pointers that did not come from it (returned
 * across the CRT DLL boundary by, say, getcwd(NULL, 0)) are detected and
 * forwarded to the platform heap.
 *
 * This header exposes only the statistics/teardown seam used by the driver and
 * by the allocator's own unit test. The allocation entry points are the
 * standard ones and need no declaration here.
 */
#ifndef METTLE_ALLOC_H
#define METTLE_ALLOC_H

#include <stddef.h>

/* Compiled out entirely unless the build opts in, so a consumer that wants the
 * platform heap (a sanitizer build, a frontend embedding libmtlc alongside
 * another allocator) just drops -DMETTLE_INTERNAL_ALLOC. */
#ifdef METTLE_INTERNAL_ALLOC

typedef struct MettleAllocStats {
  size_t bytes_reserved;  /* address space reserved from the OS */
  size_t bytes_committed; /* of that, backed by the OS */
  size_t live_objects;    /* allocated and not yet freed (allocs minus frees) */
  size_t total_allocs;
  size_t total_frees;
  size_t foreign_frees; /* pointers forwarded to the platform heap */
  size_t huge_live;     /* live allocations served directly by the OS */
} MettleAllocStats;

/* Snapshot of this thread's heap plus the process-wide region totals. Cheap;
 * intended for --profile style reporting, not for a hot loop. */
void mettle_alloc_stats(MettleAllocStats *out);

/* Human-readable one-line summary, written to stderr. */
void mettle_alloc_report(void);

#endif /* METTLE_INTERNAL_ALLOC */

#endif /* METTLE_ALLOC_H */
