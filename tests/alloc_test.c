/* Unit test for src/mettle_alloc.c.
 *
 * The allocator interposes on malloc/free for the whole process, so a bug here
 * corrupts the compiler in ways that are hard to attribute. This test exercises
 * the parts that are easy to get wrong -- size-class boundaries, realloc growth
 * and shrink, calloc's skip-the-memset path, page retirement, segment recycling,
 * huge allocations, and cross-thread frees -- and verifies block contents rather
 * than merely that nothing crashed.
 *
 * Build: gcc -O2 -std=c99 -D_GNU_SOURCE -DMETTLE_INTERNAL_ALLOC -Isrc \
 *            tests/alloc_test.c src/mettle_alloc.c -o bin/alloc_test
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mettle_alloc.h"

/* The subject of this test compiles to nothing when the allocator stands down
 * (no opt-in, a non-GNU compiler, or a sanitizer that owns malloc already), and
 * then every check below would be measuring the platform heap. Say so and pass
 * rather than failing to build, so a sanitized configure of the tree is not a
 * mystery compile error in a file that has nothing to report. */
#if !METTLE_ALLOC_ACTIVE
int main(void) {
  printf("[SKIP] alloc_test (internal allocator inactive in this build)\n");
  return 0;
}
#else

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

static int failures;

/* GCC treats malloc/calloc/realloc/free as builtins: it will fold a comparison
 * against the result, or delete an allocate-and-free pair outright, which would
 * quietly turn a check here into a test of the optimizer instead of the
 * allocator. Calling through volatile pointers keeps every call in the program.
 * Taking their addresses still resolves to the interposed definitions, so this
 * costs no coverage. */
static void *(*volatile p_malloc)(size_t) = malloc;
static void *(*volatile p_calloc)(size_t, size_t) = calloc;
static void *(*volatile p_realloc)(void *, size_t) = realloc;
static void (*volatile p_free)(void *) = free;

static void check(int cond, const char *what) {
  if (!cond) {
    printf("[FAIL] %s\n", what);
    failures++;
  }
}

/* A deterministic PRNG, so a failure is reproducible. */
static uint64_t rng_state = 0x243F6A8885A308D3ull;
static uint64_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

/* Fill a block with a byte pattern derived from its size, so any overlap
 * between two live blocks shows up as a content mismatch. */
static void paint(unsigned char *p, size_t n, unsigned seed) {
  size_t i;
  for (i = 0; i < n; i++) p[i] = (unsigned char)(seed + i * 31u);
}

static int verify(const unsigned char *p, size_t n, unsigned seed) {
  size_t i;
  for (i = 0; i < n; i++)
    if (p[i] != (unsigned char)(seed + i * 31u)) return 0;
  return 1;
}

/* ---------------------------------------------------------------- test cases */

static void test_alignment_and_sizes(void) {
  /* Every size across the small classes and both band boundaries must come back
   * 16-byte aligned and be writable to its full requested length. */
  size_t n;
  int ok_align = 1, ok_write = 1;
  for (n = 0; n <= 2048; n++) {
    unsigned char *p = (unsigned char *)p_malloc(n);
    if (!p) { check(0, "malloc returned NULL for a small size"); return; }
    if ((uintptr_t)p & 15u) ok_align = 0;
    if (n) {
      paint(p, n, (unsigned)n);
      if (!verify(p, n, (unsigned)n)) ok_write = 0;
    }
    p_free(p);
  }
  check(ok_align, "every block is 16-byte aligned");
  check(ok_write, "every block holds its full requested size");
}

static void test_no_overlap(void) {
  /* Hold many live blocks of mixed sizes at once and confirm none of them
   * shares storage with another. */
  enum { N = 20000 };
  unsigned char **ptrs = (unsigned char **)p_malloc(N * sizeof *ptrs);
  size_t *sizes = (size_t *)p_malloc(N * sizeof *sizes);
  int i, ok = 1;
  check(ptrs != NULL && sizes != NULL, "bookkeeping arrays allocated");
  if (!ptrs || !sizes) return;

  for (i = 0; i < N; i++) {
    sizes[i] = (size_t)(rng() % 700) + 1;
    ptrs[i] = (unsigned char *)p_malloc(sizes[i]);
    if (!ptrs[i]) { ok = 0; break; }
    paint(ptrs[i], sizes[i], (unsigned)i);
  }
  for (i = 0; i < N && ok; i++)
    if (!verify(ptrs[i], sizes[i], (unsigned)i)) ok = 0;
  check(ok, "20000 live mixed-size blocks do not overlap");

  /* Free half, allocate a different mix into the gaps, and re-verify the
   * survivors: this is the path that reuses local free lists. */
  for (i = 0; i < N; i += 2) { p_free(ptrs[i]); ptrs[i] = NULL; }
  for (i = 0; i < N; i += 2) {
    sizes[i] = (size_t)(rng() % 700) + 1;
    ptrs[i] = (unsigned char *)p_malloc(sizes[i]);
    if (ptrs[i]) paint(ptrs[i], sizes[i], (unsigned)(i + N));
  }
  ok = 1;
  for (i = 1; i < N; i += 2)
    if (!verify(ptrs[i], sizes[i], (unsigned)i)) ok = 0;
  for (i = 0; i < N; i += 2)
    if (ptrs[i] && !verify(ptrs[i], sizes[i], (unsigned)(i + N))) ok = 0;
  check(ok, "reused blocks do not overlap survivors");

  for (i = 0; i < N; i++) p_free(ptrs[i]);
  p_free(ptrs);
  p_free(sizes);
}

static void test_realloc(void) {
  /* Growth must preserve contents at every step, including across the small ->
   * large -> huge transitions. */
  size_t n = 1;
  unsigned char *p = (unsigned char *)p_malloc(n);
  int ok = 1;
  paint(p, n, 7);
  while (n < (size_t)3 << 20) {
    size_t next = n + n / 2 + 1;
    unsigned char *q = (unsigned char *)p_realloc(p, next);
    if (!q) { ok = 0; break; }
    if (!verify(q, n, 7)) ok = 0;
    paint(q, next, 7);
    p = q;
    n = next;
  }
  check(ok, "realloc growth preserves contents from 1B past 3MiB");
  p_free(p);

  /* Shrinking must preserve the surviving prefix. */
  n = 100000;
  p = (unsigned char *)p_malloc(n);
  paint(p, n, 11);
  ok = 1;
  while (n > 1) {
    size_t next = n / 3;
    unsigned char *q = (unsigned char *)p_realloc(p, next);
    if (!q) { ok = 0; break; }
    if (!verify(q, next, 11)) ok = 0;
    p = q;
    n = next;
  }
  check(ok, "realloc shrink preserves the surviving prefix");
  p_free(p);

  check(p_realloc(NULL, 32) != NULL, "realloc(NULL, n) allocates");
  {
    void *z = p_malloc(64);
    void *r = p_realloc(z, 0);
    check(r != NULL, "realloc(p, 0) returns a usable block");
    p_free(r);
  }
}

static void test_calloc(void) {
  /* calloc skips its memset when a block comes from never-written OS memory, so
   * the zeroing has to be right on both the fresh and the recycled path. */
  int i, ok = 1;
  void *keep[64];
  for (i = 0; i < 64; i++) {
    size_t n = (size_t)(rng() % 4000) + 1;
    unsigned char *p = (unsigned char *)p_calloc(1, n);
    size_t j;
    if (!p) { ok = 0; break; }
    for (j = 0; j < n; j++)
      if (p[j]) { ok = 0; break; }
    keep[i] = p;
  }
  check(ok, "calloc zeroes freshly committed blocks");
  for (i = 0; i < 64; i++) p_free(keep[i]);

  /* Dirty a class, free it, then calloc the same class back out of the now
   * recycled blocks. */
  ok = 1;
  for (i = 0; i < 64; i++) {
    keep[i] = p_malloc(200);
    memset(keep[i], 0xAB, 200);
  }
  for (i = 0; i < 64; i++) p_free(keep[i]);
  for (i = 0; i < 64; i++) {
    unsigned char *p = (unsigned char *)p_calloc(1, 200);
    size_t j;
    for (j = 0; j < 200; j++)
      if (p[j]) { ok = 0; break; }
    keep[i] = p;
  }
  check(ok, "calloc zeroes recycled blocks");
  for (i = 0; i < 64; i++) p_free(keep[i]);

  /* Kept opaque so the compiler does not fold the overflow it is meant to test. */
  {
    volatile size_t huge_count = (size_t)-1 / 2;
    check(p_calloc(huge_count, 4) == NULL, "calloc rejects a size overflow");
  }
}

static void test_huge(void) {
  /* Above the largest size class each allocation gets its own segment run. */
  int i, ok = 1;
  void *p[8];
  for (i = 0; i < 8; i++) {
    size_t n = (size_t)(i + 1) * 700 * 1024;
    p[i] = p_malloc(n);
    if (!p[i]) { ok = 0; continue; }
    paint((unsigned char *)p[i], n, (unsigned)(i + 200));
  }
  for (i = 0; i < 8; i++) {
    size_t n = (size_t)(i + 1) * 700 * 1024;
    if (p[i] && !verify((unsigned char *)p[i], n, (unsigned)(i + 200))) ok = 0;
  }
  check(ok, "huge allocations are distinct and hold their contents");
  for (i = 0; i < 8; i++) p_free(p[i]);
}

static void test_memory_is_returned(void) {
  /* Churning far more memory than the process should hold at once must not grow
   * committed memory without bound: emptied pages have to return to their
   * segments and emptied segments to the arena. */
  MettleAllocStats before, after;
  int round;
  mettle_alloc_stats(&before);
  for (round = 0; round < 40; round++) {
    void *keep[4096];
    int i;
    for (i = 0; i < 4096; i++) keep[i] = p_malloc(4096);
    for (i = 0; i < 4096; i++) p_free(keep[i]);
  }
  mettle_alloc_stats(&after);
  /* 40 rounds x 16MiB churned. Allow generous slack for retained head pages,
   * but not for anything resembling a leak of the full 640MiB. */
  check(after.bytes_committed < before.bytes_committed + (64u << 20),
        "committed memory stays bounded across 640MiB of churn");
  check(after.live_objects == before.live_objects,
        "live-object count returns to its starting value");
}

/* --------------------------------------------------------- cross-thread free */

#define XT_COUNT 20000
static void *xt_blocks[XT_COUNT];

static void xt_free_all(void) {
  int i;
  for (i = 0; i < XT_COUNT; i++) p_free(xt_blocks[i]);
}

#if defined(_WIN32)
static DWORD WINAPI xt_thread(LPVOID arg) {
  (void)arg;
  xt_free_all();
  return 0;
}
#else
static void *xt_thread(void *arg) {
  (void)arg;
  xt_free_all();
  return NULL;
}
#endif

static void test_cross_thread_free(void) {
  /* Blocks allocated here, freed on another thread, then reallocated here: the
   * owner has to pick them back up off the page's cross-thread list. */
  int i, ok = 1;
  for (i = 0; i < XT_COUNT; i++) {
    xt_blocks[i] = p_malloc(48);
    if (!xt_blocks[i]) ok = 0;
  }
  check(ok, "cross-thread test blocks allocated");

#if defined(_WIN32)
  {
    HANDLE t = CreateThread(NULL, 0, xt_thread, NULL, 0, NULL);
    check(t != NULL, "helper thread started");
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
  }
#else
  {
    pthread_t t;
    check(pthread_create(&t, NULL, xt_thread, NULL) == 0, "helper thread started");
    pthread_join(t, NULL);
  }
#endif

  ok = 1;
  for (i = 0; i < XT_COUNT; i++) {
    unsigned char *p = (unsigned char *)p_malloc(48);
    if (!p) { ok = 0; break; }
    paint(p, 48, (unsigned)i);
    xt_blocks[i] = p;
  }
  for (i = 0; i < XT_COUNT && ok; i++)
    if (!verify((unsigned char *)xt_blocks[i], 48, (unsigned)i)) ok = 0;
  check(ok, "blocks freed by another thread are reclaimed intact");
  xt_free_all();
}

static void test_random_workload(void) {
  /* A long randomized mix, shaped like a compile: mostly small, mostly
   * short-lived, with a slow-growing set of survivors. */
  enum { SLOTS = 8192, STEPS = 400000 };
  static unsigned char *slot[SLOTS];
  static size_t slot_size[SLOTS];
  int step, ok = 1;
  for (step = 0; step < STEPS && ok; step++) {
    unsigned i = (unsigned)(rng() % SLOTS);
    if (slot[i]) {
      if (!verify(slot[i], slot_size[i], i)) { ok = 0; break; }
      if (rng() % 4 == 0) {
        size_t next = (size_t)(rng() % 1500) + 1;
        unsigned char *q = (unsigned char *)p_realloc(slot[i], next);
        if (!q) { ok = 0; break; }
        if (next > slot_size[i]) paint(q, next, i);
        else if (!verify(q, next, i)) { ok = 0; break; }
        slot[i] = q;
        slot_size[i] = next;
        continue;
      }
      p_free(slot[i]);
      slot[i] = NULL;
      continue;
    }
    slot_size[i] = (size_t)(rng() % 400) + 1;
    slot[i] = (unsigned char *)p_malloc(slot_size[i]);
    if (!slot[i]) { ok = 0; break; }
    paint(slot[i], slot_size[i], i);
  }
  check(ok, "400000-step randomized malloc/realloc/free workload is consistent");
  {
    int i;
    for (i = 0; i < SLOTS; i++) p_free(slot[i]);
  }
}

int main(void) {
  test_alignment_and_sizes();
  test_no_overlap();
  test_realloc();
  test_calloc();
  test_huge();
  test_cross_thread_free();
  test_random_workload();
  test_memory_is_returned();

  if (failures == 0) {
    printf("[PASS] alloc_test (all checks)\n");
    mettle_alloc_report();
    return 0;
  }
  printf("alloc_test: %d failure(s)\n", failures);
  return 1;
}

#endif /* METTLE_ALLOC_ACTIVE */
