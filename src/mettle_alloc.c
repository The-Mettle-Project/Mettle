/* Size-class allocator for the Mettle toolchain. See mettle_alloc.h for why
 * this exists; this comment covers how it works.
 *
 * Layout
 * ------
 * All memory comes from ARENAS: large address-space reservations (4GiB by
 * default) that are committed a segment at a time. Because every block the
 * allocator owns lives inside a known arena, free() can decide whether a
 * pointer is ours with a single range check -- no per-object header, no magic
 * word, and no risk of faulting while inspecting a foreign pointer.
 *
 *   arena   a reservation, carved into 4MiB SEG_SIZE-aligned segments
 *   segment 4MiB, whose first 64KiB page holds the header (page metadata)
 *   page    a run of bytes dedicated to one size class
 *   block   one object, exactly the size class's stride
 *
 * A segment comes in three shapes, distinguished only by how a pointer maps to
 * its page, which keeps that mapping branchless:
 *
 *   SEG_SMALL  64KiB pages (63 usable), classes 1..32     (16B .. 8KiB)
 *   SEG_LARGE  one page spanning the rest of the segment, classes 33..56
 *              (10KiB .. 512KiB)
 *   SEG_HUGE   a run of segments holding a single object   (> 512KiB)
 *
 * page index = (p - segment) >> segment->page_shift, where LARGE uses a shift
 * wide enough that every offset in the segment maps to page 0, and HUGE uses a
 * shift wide enough for any single allocation.
 *
 * Size classes
 * ------------
 * 16B steps to 128B, then four steps per doubling (160, 192, 224, 256, 320,
 * ...) up to 512KiB. Worst-case internal waste is 25%, and every stride is a
 * multiple of 16, so every block is 16-byte aligned as malloc requires.
 *
 * Allocation
 * ----------
 * Each thread owns a Heap holding, per size class, a list of pages that may
 * have room. A page hands out blocks from its local free list, or failing that
 * bumps a pointer through the part of the page never yet used. Both are a
 * handful of instructions with no atomics.
 *
 * Freeing
 * -------
 * A block freed by the thread that owns its page is pushed onto that page's
 * local free list. A block freed by any other thread is pushed onto the page's
 * atomic cross-thread list, which the owner drains when its local list runs
 * dry. So the common path stays lock-free and the shared path stays correct.
 *
 * When a page empties it is retired and returned to its segment; when a segment
 * empties it is decommitted and returned to the arena, which is what keeps peak
 * memory bounded rather than merely fast.
 */

#include "mettle_alloc.h"

/* The allocator interposes on the CRT and needs the compiler's atomic builtins
 * for its cross-thread free lists, so it is active only for GCC/Clang builds
 * that opt in. Anything else compiles this file to nothing and gets the
 * platform heap. */
#if defined(METTLE_INTERNAL_ALLOC) && (defined(__GNUC__) || defined(__clang__))

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ tunables */

#define SEG_SHIFT 22 /* 4MiB segments */
#define SEG_SIZE ((size_t)1 << SEG_SHIFT)
#define PAGE_SHIFT 16 /* 64KiB small pages */
#define PAGE_SIZE_ ((size_t)1 << PAGE_SHIFT)
#define PAGES_PER_SEG (SEG_SIZE / PAGE_SIZE_) /* 64; page 0 is the header */

/* Page-index shifts for the segment shapes that hold exactly one page. Chosen
 * so that every in-segment offset shifts down to 0 without a branch. */
#define LARGE_PAGE_SHIFT SEG_SHIFT
#define HUGE_PAGE_SHIFT 40 /* covers any single allocation up to 1TiB */

#define NCLASS 57          /* class 0 is "not a size class"; 1..56 are real */
#define SMALL_CLASS_MAX 32 /* classes 1..32 (<= 8KiB) live in 64KiB pages */
#define LARGE_SIZE_MAX ((size_t)512 * 1024) /* above this, one segment run each */

#define MAX_ARENAS 16
#define ARENA_RESERVE ((size_t)4 << 30) /* address space only; never committed */
#define MAX_FREE_RUNS 512

typedef enum { SEG_SMALL = 0, SEG_LARGE = 1, SEG_HUGE = 2 } SegKind;

/* Diagnostic builds (-DMETTLE_ALLOC_POISON) stamp every freed block with a
 * recognisable byte, so a read through a dangling pointer returns 0xDD... rather
 * than whatever happened to still be there. Recycling blocks promptly already
 * makes use-after-free far more likely to show up than the platform heap did;
 * poisoning makes it unmistakable, and makes the offending value traceable.
 * Off by default: it costs a memset per free. */
#ifdef METTLE_ALLOC_POISON
#define POISON_FREED(p, n) memset((p), 0xDD, (n))
#else
#define POISON_FREED(p, n) ((void)(p), (void)(n))
#endif

struct Heap;

/* ------------------------------------------------------- thread-local storage
 *
 * The allocator cannot use the compiler's `__thread`: this toolchain lowers it
 * to emulated TLS, and __emutls_get_address calls calloc, so the first
 * allocation on a thread would recurse into itself forever.
 *
 * Windows therefore gets a raw TLS slot. The fast path reads the slot straight
 * out of the TEB, which is one instruction; alloc_init checks that read against
 * TlsGetValue and falls back to the API call if it ever disagrees, so a layout
 * that does not match expectations degrades in speed rather than in
 * correctness. Elsewhere `__thread` is safe, pinned to the initial-exec model so
 * that it stays a direct register-relative access even if libmtlc is built into
 * a shared object. */

#if defined(_WIN32)
#include <intrin.h>
#define TEB_TLS_SLOTS 0x1480 /* TEB.TlsSlots, holding TLS indices 0..63 */
static DWORD g_tls_index = TLS_OUT_OF_INDEXES;
static int g_tls_direct;

static inline void *tls_get(void) {
#if defined(__x86_64__)
  if (g_tls_direct)
    return (void *)__readgsqword(TEB_TLS_SLOTS + 8 * g_tls_index);
#endif
  return g_tls_index == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(g_tls_index);
}

static void tls_set(void *v) {
  if (g_tls_index != TLS_OUT_OF_INDEXES) TlsSetValue(g_tls_index, v);
}
#else
static __thread struct Heap *g_tls_heap_storage
    __attribute__((tls_model("initial-exec")));
static inline void *tls_get(void) { return g_tls_heap_storage; }
static void tls_set(void *v) { g_tls_heap_storage = (struct Heap *)v; }
#endif

/* ------------------------------------------------------------- OS primitives */

static void *os_reserve(size_t size) {
#if defined(_WIN32)
  return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
#else
  void *p = mmap(NULL, size, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? NULL : p;
#endif
}

/* Both platforms guarantee that newly committed pages read as zero, whether the
 * range is virgin reservation or was decommitted earlier. calloc leans on that
 * to skip its memset, so keep the two implementations honest about it. */
static int os_commit(void *p, size_t size) {
#if defined(_WIN32)
  return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
  return mprotect(p, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

static void os_decommit(void *p, size_t size) {
#if defined(_WIN32)
  VirtualFree(p, size, MEM_DECOMMIT);
#else
  /* Drop the physical pages, then make the range inaccessible again so it
   * behaves like fresh reserved space on the next commit. */
  (void)madvise(p, size, MADV_DONTNEED);
  (void)mprotect(p, size, PROT_NONE);
#endif
}

/* -------------------------------------------------------------------- atomics
 *
 * Only the cross-thread free lists, arena publication, and the global spinlock
 * need these; every hot path below is thread-private. */

#define ATOMIC_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ATOMIC_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define ATOMIC_CAS(p, expected, desired)                                       \
  __atomic_compare_exchange_n((p), (expected), (desired), 0, __ATOMIC_ACQ_REL, \
                              __ATOMIC_ACQUIRE)
#define ATOMIC_EXCHANGE(p, v) __atomic_exchange_n((p), (v), __ATOMIC_ACQ_REL)

/* Guards the arena bump pointers, the free-run list, the abandoned-segment
 * list, and the heap registry. Taken once per segment allocated or released --
 * roughly once per 4MiB of churn -- so a spinlock is the right shape: no OS
 * object, no syscall, and the critical sections are a few dozen instructions. */
static volatile int g_lock;

static void lock_acquire(void) {
  int expected = 0;
  while (!ATOMIC_CAS(&g_lock, &expected, 1)) {
    expected = 0;
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
  }
}

static void lock_release(void) { ATOMIC_STORE(&g_lock, 0); }

/* --------------------------------------------------------------- data layout */

struct Segment;

typedef struct Page {
  void *local_free; /* LIFO of returned blocks; next pointer in block word 0 */
  char *bump;       /* start of the never-yet-handed-out tail */
  char *bump_end;
  size_t bsize;   /* size-class stride, or the payload size when HUGE */
  uint32_t used;  /* blocks handed out and not yet returned */
  uint16_t cls;
  uint8_t in_use;   /* page is assigned to a class */
  uint8_t in_full;  /* page sits on the heap's full list, not its active one */
  uint8_t pristine; /* the bump tail is still OS-fresh, hence already zero */
  void *xthread_free; /* blocks returned by non-owner threads (atomic) */
  struct Heap *owner; /* NULL once the owning thread has gone away (atomic) */
  struct Page *next, *prev;
  struct Segment *seg;
} Page;

typedef struct Segment {
  uint32_t page_shift;
  uint32_t page_count;  /* usable pages, excluding the header page */
  uint32_t pages_in_use;
  uint32_t page_hwm;    /* pages below this index have been used before */
  uint32_t nsegs;       /* segments in this run; > 1 only for HUGE */
  uint8_t kind;
  uint8_t hosts_heap;   /* the owning Heap struct lives in this header page */
  struct Heap *owner;
  struct Segment *next, *prev; /* owner's segment list, or the abandoned list */
  Page pages[PAGES_PER_SEG];
} Segment;

typedef struct Heap {
  Page *active[NCLASS]; /* pages that may still have room */
  Page *full[NCLASS];   /* pages with nothing left to hand out */
  Segment *segments;
  /* One emptied segment of each shape is kept committed rather than handed
   * back, so a malloc/free pair repeated in a loop cannot turn into a pair of
   * commit/decommit syscalls per iteration. */
  Segment *cached_empty[2];
  size_t live_objects, live_bytes;
  size_t total_allocs, total_frees, foreign_frees, huge_live;
  struct Heap *next_all;
} Heap;

/* -------------------------------------------------------------- global state */

static uintptr_t g_arena_base[MAX_ARENAS];
static size_t g_arena_size[MAX_ARENAS];
static uintptr_t g_arena_next[MAX_ARENAS]; /* bump pointer for virgin segments */
static int g_arena_count;                  /* published with release semantics */

typedef struct FreeRun {
  uintptr_t base;
  uint32_t nsegs;
} FreeRun;

static FreeRun g_free_runs[MAX_FREE_RUNS];
static int g_free_run_count;

static Segment *g_abandoned; /* segments whose owning thread has exited */
static Heap *g_all_heaps;    /* for process-wide statistics */
static size_t g_bytes_reserved, g_bytes_committed;

static size_t g_bsize[NCLASS];
static uint8_t g_small_map[129]; /* class for size n, indexed by (n + 7) >> 3 */
static int g_ready;

/* The platform heap, for pointers that were not ours to begin with (anything
 * the CRT allocated inside its own DLL and handed back across the boundary). */
static void (*g_sys_free)(void *);
static void *(*g_sys_realloc)(void *, size_t);
static void *(*g_sys_malloc)(size_t);
static size_t (*g_sys_usable)(void *);

/* -------------------------------------------------------------- size classes */

static int class_for_big(size_t n) {
  /* Classes 9 and up are (5 + j) << (5 + k) for j in 0..3. Start one band below
   * the one holding n's high bit and walk to the exact class; at most a handful
   * of steps, on a path that takes well under 1% of allocations. */
  int hb = 63 - __builtin_clzll((unsigned long long)n);
  int c = 9 + (hb >= 8 ? (hb - 8) * 4 : 0);
  if (c >= NCLASS) c = NCLASS - 1;
  while (c > 9 && g_bsize[c - 1] >= n) c--;
  while (c < NCLASS && g_bsize[c] < n) c++;
  return c < NCLASS ? c : 0; /* 0 means "too big for a class" */
}

static void init_classes(void) {
  int c, k, j;
  g_bsize[0] = 0;
  for (c = 1; c <= 8; c++) g_bsize[c] = (size_t)c * 16;
  for (k = 0; c < NCLASS; k++)
    for (j = 0; j < 4 && c < NCLASS; j++)
      g_bsize[c++] = (size_t)(5 + j) << (5 + k);

  g_small_map[0] = 1; /* malloc(0) still returns a distinct, usable block */
  for (c = 1; c <= 128; c++) {
    size_t n = (size_t)c * 8;
    int cc = 1;
    while (g_bsize[cc] < n) cc++;
    g_small_map[c] = (uint8_t)cc;
  }
}

/* ----------------------------------------------------------- initialization */

#if defined(_WIN32)
static DWORD g_fls_slot = FLS_OUT_OF_INDEXES;
static void WINAPI heap_thread_exit(void *value);
#else
static pthread_key_t g_tls_key;
static int g_tls_key_ready;
static void heap_thread_exit(void *value);
#endif

static void resolve_system_heap(void) {
#if defined(_WIN32)
  static const char *const crts[] = {"msvcrt.dll", "ucrtbase.dll",
                                     "api-ms-win-crt-heap-l1-1-0.dll"};
  size_t i;
  for (i = 0; i < sizeof crts / sizeof crts[0] && !g_sys_free; i++) {
    HMODULE m = GetModuleHandleA(crts[i]);
    if (!m) continue;
    g_sys_free = (void (*)(void *))(void *)GetProcAddress(m, "free");
    g_sys_realloc =
        (void *(*)(void *, size_t))(void *)GetProcAddress(m, "realloc");
    g_sys_malloc = (void *(*)(size_t))(void *)GetProcAddress(m, "malloc");
    g_sys_usable = (size_t(*)(void *))(void *)GetProcAddress(m, "_msize");
  }
#else
  g_sys_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
  g_sys_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
  g_sys_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
  g_sys_usable = (size_t(*)(void *))dlsym(RTLD_NEXT, "malloc_usable_size");
#endif
}

/* Reserve one arena. Over-reserves by a segment so a SEG_SIZE-aligned interior
 * is always available; the slack is address space that is never committed.
 * Caller holds g_lock. */
static int arena_add(size_t want) {
  char *raw = NULL;
  int slot = g_arena_count;

  if (slot >= MAX_ARENAS) return 0;
  while (want >= SEG_SIZE * 2) {
    raw = (char *)os_reserve(want + SEG_SIZE);
    if (raw) break;
    want /= 2;
  }
  if (!raw) return 0;

  g_arena_base[slot] = ((uintptr_t)raw + SEG_SIZE - 1) & ~(uintptr_t)(SEG_SIZE - 1);
  g_arena_size[slot] = want;
  g_arena_next[slot] = g_arena_base[slot];
  g_bytes_reserved += want;
  /* Publish base/size before the count, so any thread that can observe a
   * pointer from this arena can also observe the arena itself. */
  ATOMIC_STORE(&g_arena_count, slot + 1);
  return 1;
}

/* Idempotent, callable from any thread, and safe to reach from CRT startup
 * before main(). Returns 0 if no arena could be reserved, in which case every
 * entry point below falls back to the platform heap. */
static int alloc_init(void) {
  if (ATOMIC_LOAD(&g_ready)) return g_ready > 0;
  lock_acquire();
  if (!g_ready) {
    int ok;
    resolve_system_heap();
    init_classes();
#if defined(_WIN32)
    g_tls_index = TlsAlloc();
    g_fls_slot = FlsAlloc(heap_thread_exit);
#if defined(__x86_64__)
    /* Only indices below 64 live in TEB.TlsSlots, and only take the inlined
     * read once it demonstrably agrees with the API. */
    if (g_tls_index != TLS_OUT_OF_INDEXES && g_tls_index < 64) {
      TlsSetValue(g_tls_index, (void *)(uintptr_t)0x5A5A5A5A5A5A5A5AULL);
      g_tls_direct = (void *)__readgsqword(TEB_TLS_SLOTS + 8 * g_tls_index) ==
                     (void *)(uintptr_t)0x5A5A5A5A5A5A5A5AULL;
      TlsSetValue(g_tls_index, NULL);
    }
#endif
#else
    g_tls_key_ready = pthread_key_create(&g_tls_key, heap_thread_exit) == 0;
#endif
    ok = arena_add(ARENA_RESERVE);
    /* Written last: a non-zero g_ready is what publishes everything above. */
    ATOMIC_STORE(&g_ready, ok ? 1 : -1);
  }
  lock_release();
  return g_ready > 0;
}

static inline int mtlc_owned(const void *p) {
  uintptr_t a = (uintptr_t)p;
  int i, n;
  if (a - g_arena_base[0] < g_arena_size[0]) return 1;
  n = ATOMIC_LOAD(&g_arena_count);
  for (i = 1; i < n; i++)
    if (a - g_arena_base[i] < g_arena_size[i]) return 1;
  return 0;
}

/* ------------------------------------------------------ segment run manager */

/* Caller holds g_lock. */
static void free_run_insert(uintptr_t base, uint32_t nsegs) {
  int i;
  /* Coalesce with an adjacent run so a long compile cannot fragment the arena
   * into unusable slivers. */
  for (i = 0; i < g_free_run_count; i++) {
    if (g_free_runs[i].base + (size_t)g_free_runs[i].nsegs * SEG_SIZE == base) {
      g_free_runs[i].nsegs += nsegs;
      return;
    }
    if (base + (size_t)nsegs * SEG_SIZE == g_free_runs[i].base) {
      g_free_runs[i].base = base;
      g_free_runs[i].nsegs += nsegs;
      return;
    }
  }
  if (g_free_run_count < MAX_FREE_RUNS) {
    g_free_runs[g_free_run_count].base = base;
    g_free_runs[g_free_run_count].nsegs = nsegs;
    g_free_run_count++;
    return;
  }
  /* Table full: the run stays decommitted and reserved, so this costs address
   * space only, and only past 512 simultaneously free non-adjacent runs. */
}

/* Caller holds g_lock. */
static uintptr_t free_run_take(uint32_t nsegs) {
  int i, best = -1;
  for (i = 0; i < g_free_run_count; i++) {
    if (g_free_runs[i].nsegs < nsegs) continue;
    if (best < 0 || g_free_runs[i].nsegs < g_free_runs[best].nsegs) best = i;
  }
  if (best < 0) return 0;
  {
    uintptr_t base = g_free_runs[best].base;
    if (g_free_runs[best].nsegs == nsegs) {
      g_free_runs[best] = g_free_runs[--g_free_run_count];
    } else {
      g_free_runs[best].base += (size_t)nsegs * SEG_SIZE;
      g_free_runs[best].nsegs -= nsegs;
    }
    return base;
  }
}

/* Returns a committed, SEG_SIZE-aligned, zero-filled run of nsegs segments. */
static void *segment_run_alloc(uint32_t nsegs) {
  size_t bytes = (size_t)nsegs * SEG_SIZE;
  uintptr_t base;
  int i, n;

  lock_acquire();
  base = free_run_take(nsegs);
  if (!base) {
    n = g_arena_count;
    for (i = 0; i < n; i++) {
      if (g_arena_next[i] + bytes <= g_arena_base[i] + g_arena_size[i]) {
        base = g_arena_next[i];
        g_arena_next[i] += bytes;
        break;
      }
    }
    if (!base) {
      size_t want = ARENA_RESERVE;
      if (bytes + SEG_SIZE > want) want = bytes + SEG_SIZE;
      if (arena_add(want)) {
        int slot = g_arena_count - 1;
        base = g_arena_next[slot];
        g_arena_next[slot] += bytes;
      }
    }
  }
  if (base) g_bytes_committed += bytes;
  lock_release();

  if (!base) return NULL;
  if (!os_commit((void *)base, bytes)) {
    lock_acquire();
    g_bytes_committed -= bytes;
    free_run_insert(base, nsegs);
    lock_release();
    return NULL;
  }
  return (void *)base;
}

static void segment_run_release(Segment *seg) {
  size_t bytes = (size_t)seg->nsegs * SEG_SIZE;
  uintptr_t base = (uintptr_t)seg;
  os_decommit((void *)base, bytes);
  lock_acquire();
  g_bytes_committed -= bytes;
  free_run_insert(base, (uint32_t)(bytes / SEG_SIZE));
  lock_release();
}

/* ------------------------------------------------------------ heap plumbing */

static void segment_link(Heap *h, Segment *seg) {
  seg->owner = h;
  seg->prev = NULL;
  seg->next = h->segments;
  if (h->segments) h->segments->prev = seg;
  h->segments = seg;
}

static void segment_unlink(Heap *h, Segment *seg) {
  if (seg->prev) seg->prev->next = seg->next;
  else h->segments = seg->next;
  if (seg->next) seg->next->prev = seg->prev;
  seg->next = seg->prev = NULL;
}

static void segment_init(Segment *seg, uint32_t nsegs, int kind) {
  seg->nsegs = nsegs;
  seg->kind = (uint8_t)kind;
  seg->pages_in_use = 0;
  if (kind == SEG_SMALL) {
    seg->page_shift = PAGE_SHIFT;
    seg->page_count = PAGES_PER_SEG - 1;
    seg->page_hwm = 1; /* page 0 is the header */
  } else if (kind == SEG_LARGE) {
    seg->page_shift = LARGE_PAGE_SHIFT;
    seg->page_count = 1;
    seg->page_hwm = 0;
  } else {
    seg->page_shift = HUGE_PAGE_SHIFT;
    seg->page_count = 1;
    seg->page_hwm = 0;
  }
}

/* The heap itself is carved out of a segment rather than malloc'd, so heap
 * creation cannot recurse back into malloc. */
static Heap *heap_create(void) {
  Segment *seg = (Segment *)segment_run_alloc(1);
  Heap *h;
  if (!seg) return NULL;
  segment_init(seg, 1, SEG_SMALL);
  /* Park the Heap in the header page, after the metadata: the header page is
   * never handed out as blocks, so there is room to spare. */
  h = (Heap *)((char *)seg + ((sizeof(Segment) + 63) & ~(size_t)63));
  seg->hosts_heap = 1;
  segment_link(h, seg);

  lock_acquire();
  h->next_all = g_all_heaps;
  g_all_heaps = h;
  lock_release();
  return h;
}

static void heap_abandon(Heap *h) {
  Segment *seg, *tail = NULL;
  lock_acquire();
  for (seg = h->segments; seg; seg = seg->next) {
    uint32_t i;
    seg->owner = NULL;
    for (i = 0; i < PAGES_PER_SEG; i++)
      if (seg->pages[i].in_use)
        ATOMIC_STORE(&seg->pages[i].owner, (Heap *)NULL);
    tail = seg;
  }
  if (tail) {
    tail->next = g_abandoned;
    if (g_abandoned) g_abandoned->prev = tail;
    g_abandoned = h->segments;
    h->segments = NULL;
  }
  lock_release();
}

#if defined(_WIN32)
static void WINAPI heap_thread_exit(void *value) {
  if (value) heap_abandon((Heap *)value);
}
#else
static void heap_thread_exit(void *value) {
  if (value) heap_abandon((Heap *)value);
}
#endif

static Heap *heap_get_slow(void) {
  Heap *h;
  if (!alloc_init()) return NULL;
  h = heap_create();
  if (!h) return NULL;
  tls_set(h);
#if defined(_WIN32)
  if (g_fls_slot != FLS_OUT_OF_INDEXES) FlsSetValue(g_fls_slot, h);
#else
  if (g_tls_key_ready) pthread_setspecific(g_tls_key, h);
#endif
  return h;
}

static inline Heap *heap_get(void) {
  Heap *h = (Heap *)tls_get();
  return h ? h : heap_get_slow();
}

/* --------------------------------------------------------- page bookkeeping */

static void page_list_push(Page **list, Page *p) {
  p->prev = NULL;
  p->next = *list;
  if (*list) (*list)->prev = p;
  *list = p;
}

static void page_list_remove(Page **list, Page *p) {
  if (p->prev) p->prev->next = p->next;
  else *list = p->next;
  if (p->next) p->next->prev = p->prev;
  p->next = p->prev = NULL;
}

/* Move blocks other threads returned onto the local free list. Only the owner
 * calls this, so the exchange is the only synchronization needed. */
static int page_drain_xthread(Page *p) {
  void *list = ATOMIC_LOAD(&p->xthread_free);
  if (!list) return 0;
  list = ATOMIC_EXCHANGE(&p->xthread_free, (void *)NULL);
  if (!list) return 0;
  if (!p->local_free) {
    p->local_free = list;
  } else {
    void *tail = list;
    while (*(void **)tail) tail = *(void **)tail;
    *(void **)tail = p->local_free;
    p->local_free = list;
  }
  p->pristine = 0; /* recycled blocks are no longer OS-zero */
  return 1;
}

/* Hand `p`'s page back to its segment, releasing the segment to the arena once
 * that empties it. Retiring a page is pure bookkeeping, so it is unconditional;
 * only the segment release, which costs a syscall, gets hysteresis. */
static void page_retire(Heap *h, Page *p) {
  Segment *seg = p->seg;
  page_list_remove(p->in_full ? &h->full[p->cls] : &h->active[p->cls], p);
  p->in_use = 0;
  p->in_full = 0;
  p->local_free = NULL;
  p->bump = p->bump_end = NULL;
  p->cls = 0;
  p->bsize = 0;
  ATOMIC_STORE(&p->owner, (Heap *)NULL);

  if (--seg->pages_in_use > 0 || seg->hosts_heap) return;
  if (seg->kind == SEG_HUGE) { /* freed whole, by free_block, never page-wise */
    segment_unlink(h, seg);
    segment_run_release(seg);
    return;
  }
  if (!h->cached_empty[seg->kind]) {
    h->cached_empty[seg->kind] = seg;
    return;
  }
  if (h->cached_empty[seg->kind] == seg) return;
  segment_unlink(h, seg);
  segment_run_release(seg);
}

/* Take over a segment whose owning thread has exited, so its memory (and any
 * still-live blocks in it) rejoin circulation. */
static Segment *adopt_segment(Heap *h, int kind) {
  Segment *seg;
  lock_acquire();
  for (seg = g_abandoned; seg; seg = seg->next) {
    if (seg->kind != (uint8_t)kind) continue;
    if (seg->pages_in_use >= seg->page_count) continue;
    if (seg->prev) seg->prev->next = seg->next;
    else g_abandoned = seg->next;
    if (seg->next) seg->next->prev = seg->prev;
    seg->next = seg->prev = NULL;
    break;
  }
  lock_release();
  if (!seg) return NULL;
  /* Re-own the pages that still hold live blocks; their cross-thread lists are
   * drained lazily by the normal allocation path. */
  {
    uint32_t i;
    for (i = 0; i < PAGES_PER_SEG; i++) {
      Page *p = &seg->pages[i];
      if (!p->in_use) continue;
      ATOMIC_STORE(&p->owner, h);
      p->in_full = 1;
      page_list_push(&h->full[p->cls], p);
    }
  }
  segment_link(h, seg);
  return seg;
}

/* Claim an unused page from one of the heap's segments, or from a new segment.
 * `want_large` selects the segment shape. */
static Page *page_claim(Heap *h, int cls, int want_large) {
  int kind = want_large ? SEG_LARGE : SEG_SMALL;
  Segment *seg;
  Page *p = NULL;
  uint32_t idx = 0;

  for (seg = h->segments; seg; seg = seg->next) {
    uint32_t i, first;
    if (seg->kind != (uint8_t)kind) continue;
    if (seg->pages_in_use >= seg->page_count) continue;
    first = kind == SEG_SMALL ? 1 : 0;
    for (i = first; i < first + seg->page_count; i++) {
      if (!seg->pages[i].in_use) { p = &seg->pages[i]; idx = i; break; }
    }
    if (p) break;
  }

  if (!p) {
    seg = adopt_segment(h, kind);
    if (seg) {
      uint32_t i, first = kind == SEG_SMALL ? 1 : 0;
      for (i = first; i < first + seg->page_count; i++)
        if (!seg->pages[i].in_use) { p = &seg->pages[i]; idx = i; break; }
    }
  }

  if (!p) {
    seg = (Segment *)segment_run_alloc(1);
    if (!seg) return NULL;
    segment_init(seg, 1, kind);
    segment_link(h, seg);
    idx = kind == SEG_SMALL ? 1 : 0;
    p = &seg->pages[idx];
  }

  /* A page above the segment's high-water mark has never been handed out, so
   * its bytes are still the zeros the OS committed. */
  p->pristine = (uint8_t)(idx >= seg->page_hwm);
  if (idx >= seg->page_hwm) seg->page_hwm = idx + 1;

  p->seg = seg;
  p->cls = (uint16_t)cls;
  p->bsize = g_bsize[cls];
  p->in_use = 1;
  p->in_full = 0;
  p->used = 0;
  p->local_free = NULL;
  ATOMIC_STORE(&p->xthread_free, (void *)NULL);
  ATOMIC_STORE(&p->owner, h);

  if (kind == SEG_LARGE) {
    /* One page covering everything after the header page. */
    p->bump = (char *)seg + PAGE_SIZE_;
    p->bump_end = (char *)seg + SEG_SIZE;
  } else {
    p->bump = (char *)seg + (size_t)idx * PAGE_SIZE_;
    p->bump_end = p->bump + PAGE_SIZE_;
  }
  /* Trim the tail so every block fits wholly inside the page. */
  p->bump_end = p->bump + ((size_t)(p->bump_end - p->bump) / p->bsize) * p->bsize;
  if (seg->pages_in_use++ == 0 && h->cached_empty[seg->kind] == seg)
    h->cached_empty[seg->kind] = NULL; /* no longer empty, so no longer cached */
  page_list_push(&h->active[cls], p);
  return p;
}

/* --------------------------------------------------------- huge allocation */

static void *huge_alloc(Heap *h, size_t n) {
  uint32_t nsegs = (uint32_t)((n + PAGE_SIZE_ + SEG_SIZE - 1) / SEG_SIZE);
  Segment *seg = (Segment *)segment_run_alloc(nsegs);
  Page *p;
  if (!seg) return NULL;
  segment_init(seg, nsegs, SEG_HUGE);
  segment_link(h, seg);

  p = &seg->pages[0];
  p->seg = seg;
  p->cls = 0;
  p->bsize = (size_t)nsegs * SEG_SIZE - PAGE_SIZE_;
  p->in_use = 1;
  p->in_full = 0;
  p->used = 1;
  p->pristine = 1; /* the whole run was just committed, hence zero */
  p->local_free = NULL;
  p->bump = p->bump_end = NULL;
  ATOMIC_STORE(&p->xthread_free, (void *)NULL);
  ATOMIC_STORE(&p->owner, h);
  seg->pages_in_use = 1;

  h->live_objects++;
  h->live_bytes += p->bsize;
  h->total_allocs++;
  h->huge_live++;
  return (char *)seg + PAGE_SIZE_;
}

/* -------------------------------------------------------------- fast paths */

static inline Segment *segment_of(const void *p) {
  return (Segment *)((uintptr_t)p & ~(uintptr_t)(SEG_SIZE - 1));
}

static inline Page *page_of(const void *p) {
  Segment *seg = segment_of(p);
  size_t off = (size_t)((const char *)p - (const char *)seg);
  return &seg->pages[off >> seg->page_shift];
}

/* Cold half of malloc: the head page for this class had nothing to give. */
static void *alloc_slow(Heap *h, int cls) {
  Page *p = h->active[cls];
  while (p) {
    Page *next = p->next;
    if (page_drain_xthread(p) && p->local_free) {
      void *b = p->local_free;
      p->local_free = *(void **)b;
      p->used++;
      return b;
    }
    if (p->bump < p->bump_end) {
      void *b = p->bump;
      p->bump += p->bsize;
      p->used++;
      return b;
    }
    /* Genuinely full: park it so the active list stays short. */
    page_list_remove(&h->active[cls], p);
    p->in_full = 1;
    page_list_push(&h->full[cls], p);
    p = next;
  }
  p = page_claim(h, cls, cls > SMALL_CLASS_MAX);
  if (!p) return NULL;
  {
    void *b = p->bump;
    p->bump += p->bsize;
    p->used++;
    return b;
  }
}

static inline void *alloc_block(size_t n) {
  Heap *h = heap_get();
  int cls;
  Page *p;
  void *b;

  if (!h) return g_sys_malloc ? g_sys_malloc(n) : NULL;
  if (n <= 1024) {
    cls = g_small_map[(n + 7) >> 3];
  } else if (n <= LARGE_SIZE_MAX) {
    cls = class_for_big(n);
  } else {
    if (n + PAGE_SIZE_ + SEG_SIZE < n) return NULL; /* size overflow */
    return huge_alloc(h, n);
  }

  p = h->active[cls];
  if (p) {
    b = p->local_free;
    if (b) {
      p->local_free = *(void **)b;
      p->used++;
      goto counted;
    }
    if (p->bump < p->bump_end) {
      b = p->bump;
      p->bump += p->bsize;
      p->used++;
      goto counted;
    }
  }
  b = alloc_slow(h, cls);
  if (!b) return NULL;
counted:
  h->live_objects++;
  h->live_bytes += g_bsize[cls];
  h->total_allocs++;
  return b;
}

static void free_block(void *ptr) {
  Page *p = page_of(ptr);
  Heap *h = (Heap *)tls_get();

  if (ATOMIC_LOAD(&p->owner) != h || !h) {
    /* Another thread's page, or one abandoned by an exited thread: publish the
     * block on the page's cross-thread list for its owner to reclaim. */
    void *head = ATOMIC_LOAD(&p->xthread_free);
    do {
      *(void **)ptr = head;
    } while (!ATOMIC_CAS(&p->xthread_free, &head, ptr));
    return;
  }

  h->live_objects--;
  h->live_bytes -= p->bsize;
  h->total_frees++;

  if (p->seg->kind == SEG_HUGE) {
    Segment *seg = p->seg;
    h->huge_live--;
    p->in_use = 0;
    ATOMIC_STORE(&p->owner, (Heap *)NULL);
    segment_unlink(h, seg);
    segment_run_release(seg);
    return;
  }

  POISON_FREED(ptr, p->bsize); /* before the link below, which reuses word 0 */
  *(void **)ptr = p->local_free;
  p->local_free = ptr;
  p->pristine = 0; /* the page now holds recycled, non-zero blocks */

  if (--p->used == 0) {
    page_retire(h, p);
  } else if (p->in_full) {
    int cls = p->cls;
    page_list_remove(&h->full[cls], p);
    p->in_full = 0;
    page_list_push(&h->active[cls], p);
  }
}

/* ---------------------------------------------------------- CRT entry points */

/* MinGW's headers declare several of these dllimport; defining them here is the
 * whole point, so silence just that mismatch. */
#if defined(_WIN32)
#pragma GCC diagnostic ignored "-Wattributes"
#endif

void *malloc(size_t n) { return alloc_block(n); }

void *calloc(size_t count, size_t size) {
  size_t n;
  void *b;
  if (count && size > (size_t)-1 / count) return NULL; /* multiply overflow */
  n = count * size;
  b = alloc_block(n);
  if (!b) return NULL;
  /* Skip the clear when the block came straight from never-written OS memory,
   * which is the common case on a page's first pass. */
  if (mtlc_owned(b)) {
    Page *p = page_of(b);
    if (p->pristine && (char *)b + p->bsize == p->bump) return b;
    if (p->seg->kind == SEG_HUGE && p->pristine) return b;
  }
  memset(b, 0, n);
  return b;
}

void free(void *ptr) {
  if (!ptr) return;
  if (!mtlc_owned(ptr)) {
    Heap *h = (Heap *)tls_get();
    if (h) h->foreign_frees++;
    if (!g_sys_free) alloc_init();
    if (g_sys_free) g_sys_free(ptr);
    return;
  }
  free_block(ptr);
}

void *realloc(void *ptr, size_t n) {
  size_t old;
  void *b;

  if (!ptr) return alloc_block(n);
  if (!mtlc_owned(ptr)) {
    if (!g_sys_realloc) alloc_init();
    return g_sys_realloc ? g_sys_realloc(ptr, n) : NULL;
  }
  if (n == 0) {
    free_block(ptr);
    return alloc_block(0);
  }

  old = page_of(ptr)->bsize;
  /* The case that matters: a geometrically growing buffer whose new size still
   * lands in the same class. Returning the same block is what keeps repeated
   * growth linear rather than quadratic in bytes copied. */
  if (n <= old && (n > old / 2 || old <= 16 ||
                   page_of(ptr)->seg->kind == SEG_HUGE))
    return ptr;

  b = alloc_block(n);
  if (!b) return NULL;
  memcpy(b, ptr, n < old ? n : old);
  free_block(ptr);
  return b;
}

char *strdup(const char *s) {
  size_t n;
  char *b;
  if (!s) return NULL;
  n = strlen(s) + 1;
  b = (char *)alloc_block(n);
  if (b) memcpy(b, s, n);
  return b;
}

#if defined(_WIN32)
char *_strdup(const char *s) { return strdup(s); }

size_t _msize(void *ptr) {
  if (!ptr) return 0;
  if (!mtlc_owned(ptr)) return g_sys_usable ? g_sys_usable(ptr) : 0;
  return page_of(ptr)->bsize;
}
#else
char *strndup(const char *s, size_t cap) {
  size_t n = 0;
  char *b;
  if (!s) return NULL;
  while (n < cap && s[n]) n++;
  b = (char *)alloc_block(n + 1);
  if (!b) return NULL;
  memcpy(b, s, n);
  b[n] = '\0';
  return b;
}

size_t malloc_usable_size(void *ptr) {
  if (!ptr) return 0;
  if (!mtlc_owned(ptr)) return g_sys_usable ? g_sys_usable(ptr) : 0;
  return page_of(ptr)->bsize;
}
#endif

/* Deliberately NOT overridden: aligned_alloc, posix_memalign, memalign,
 * _aligned_malloc. Blocks here are only guaranteed 16-byte aligned, and the
 * over-allocate-and-offset trick would hand out interior pointers that free()
 * could not map back to a page. Letting them fall through to the platform heap
 * is correct: free() and _aligned_free() both recognise the result as foreign
 * and forward it. */

/* ---------------------------------------------------------------- reporting */

void mettle_alloc_stats(MettleAllocStats *out) {
  Heap *h;
  if (!out) return;
  memset(out, 0, sizeof *out);
  lock_acquire();
  out->bytes_reserved = g_bytes_reserved;
  out->bytes_committed = g_bytes_committed;
  for (h = g_all_heaps; h; h = h->next_all) {
    out->live_objects += h->live_objects;
    out->live_bytes += h->live_bytes;
    out->total_allocs += h->total_allocs;
    out->total_frees += h->total_frees;
    out->foreign_frees += h->foreign_frees;
    out->huge_live += h->huge_live;
  }
  lock_release();
}

void mettle_alloc_report(void) {
  MettleAllocStats s;
  char buf[256];
  int n;
  mettle_alloc_stats(&s);
  n = snprintf(buf, sizeof buf,
               "  allocator            %.2fM allocs, %.2fM frees, "
               "%.1f MB committed\n",
               (double)s.total_allocs / 1e6, (double)s.total_frees / 1e6,
               (double)s.bytes_committed / (1024.0 * 1024.0));
  if (n <= 0) return;
#if defined(_WIN32)
  {
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, (DWORD)n, &w, NULL);
  }
#else
  {
    ssize_t ignored = write(2, buf, (size_t)n);
    (void)ignored;
  }
#endif
}

#endif /* METTLE_INTERNAL_ALLOC && GCC/Clang */
