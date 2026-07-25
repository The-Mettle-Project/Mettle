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
 * platform heap (a frontend embedding libmtlc alongside another allocator) just
 * drops -DMETTLE_INTERNAL_ALLOC.
 *
 * It also stands down on its own under a sanitizer. ASan, TSan, MSan and LSan
 * each intercept malloc/calloc/realloc/free to attach their own metadata, and
 * two interposers on one heap is not a slow build, it is a crash before main:
 * blocks handed out by one and released by the other, and a bootstrap dlsym
 * that allocates while the allocator it is being asked to find does not exist
 * yet. Detecting it here rather than relying on the build to pass
 * INTERNAL_ALLOC=0 means an ad-hoc `-fsanitize=address` gets the platform heap
 * too, and gets it whether or not whoever typed it knew to.
 *
 * The predicate lives in the header so that the implementation, this header's
 * declarations and the driver's call site cannot disagree about it -- gating
 * them on different conditions is a link error, not a diagnostic. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||     \
    __has_feature(memory_sanitizer) || __has_feature(leak_sanitizer)
#define METTLE_ALLOC_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) ||           \
    defined(__SANITIZE_LEAK__)
#define METTLE_ALLOC_SANITIZED 1
#endif
#ifndef METTLE_ALLOC_SANITIZED
#define METTLE_ALLOC_SANITIZED 0
#endif

/* The cross-thread free lists need the compiler's atomic builtins, so the
 * allocator is GCC/Clang only; anything else gets the platform heap. */
#if defined(METTLE_INTERNAL_ALLOC) && !METTLE_ALLOC_SANITIZED &&               \
    (defined(__GNUC__) || defined(__clang__))
#define METTLE_ALLOC_ACTIVE 1
#else
#define METTLE_ALLOC_ACTIVE 0
#endif

#if METTLE_ALLOC_ACTIVE

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

#endif /* METTLE_ALLOC_ACTIVE */

#endif /* METTLE_ALLOC_H */
