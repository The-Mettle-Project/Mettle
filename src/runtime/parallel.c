#include "runtime/parallel.h"
#include <stddef.h>
#include <stdint.h>

extern char *getenv(const char *name);

#define MTP_CACHE_LINE 64u
#define MTP_VECTOR_BYTES 16u
#define MTP_GRAINS_PER_THREAD 16u
#define MTP_IDLE_SPIN_NS 100000ll
#define MTP_JOIN_SPIN_MIN_NS 50000ll
#define MTP_JOIN_SPIN_MAX_NS 1000000ll
#define MTP_CLOCK_EVERY 64u
#define MTP_SPIN_ROUNDS_CAP (1u << 22)
#define MTP_WAKE_FANOUT 2u
#define MTP_MEMORY_CHUNK_BYTES 262144ll
#define MTP_STREAM_THRESHOLD_BYTES (8ll << 20)

typedef struct {
  MettleParallelBody body;
  MettleParallelSlotBody slot_body;
  void *ctx;
  long long lo;
  long long hi;
  long long grain;
} MtpJob;

typedef struct {
  uint32_t value;
#if defined(_WIN32) || defined(_WIN64)
  void *lock;
  void *ready;
#endif
} MtpWord;

typedef struct {
  uint64_t range __attribute__((aligned(MTP_CACHE_LINE)));
} MtpSlot;

typedef struct {
  MtpJob job __attribute__((aligned(MTP_CACHE_LINE)));
  MtpWord generation __attribute__((aligned(MTP_CACHE_LINE)));
  MtpWord pending __attribute__((aligned(MTP_CACHE_LINE)));
  int32_t joining __attribute__((aligned(MTP_CACHE_LINE)));
  int32_t parked __attribute__((aligned(MTP_CACHE_LINE)));
  int32_t wake_budget __attribute__((aligned(MTP_CACHE_LINE)));
  int32_t busy __attribute__((aligned(MTP_CACHE_LINE)));
  unsigned threads;
  unsigned workers;
  int spawned;
} MtpPool;

static MtpPool g_pool;
static MtpSlot g_slots[METTLE_PARALLEL_MAX_THREADS];

static void mtp_worker(unsigned index);

static void mtp_cpu_pause(void) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

#if defined(_WIN32) || defined(_WIN64)

typedef struct {
  unsigned long dwOemId;
  unsigned long dwPageSize;
  void *lpMinimumApplicationAddress;
  void *lpMaximumApplicationAddress;
  unsigned long long dwActiveProcessorMask;
  unsigned long dwNumberOfProcessors;
  unsigned long dwProcessorType;
  unsigned long dwAllocationGranularity;
  unsigned short wProcessorLevel;
  unsigned short wProcessorRevision;
} MtpSystemInfo;

__declspec(dllimport) void __stdcall GetSystemInfo(MtpSystemInfo *info);
__declspec(dllimport) void *__stdcall CreateThread(
    void *attributes, size_t stack_size,
    unsigned long(__stdcall *start)(void *), void *argument,
    unsigned long flags, unsigned long *thread_id);
__declspec(dllimport) int __stdcall CloseHandle(void *handle);
__declspec(dllimport) void __stdcall AcquireSRWLockExclusive(void **lock);
__declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(void **lock);
__declspec(dllimport) int __stdcall SleepConditionVariableSRW(
    void **condition, void **lock, unsigned long milliseconds,
    unsigned long flags);
__declspec(dllimport) void __stdcall WakeConditionVariable(void **condition);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *count);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(
    long long *frequency);

static long long g_mtp_ticks_per_second;

static long long mtp_os_now_ns(void) {
  long long count = 0;
  long long frequency = __atomic_load_n(&g_mtp_ticks_per_second,
                                        __ATOMIC_RELAXED);
  if (!frequency) {
    (void)QueryPerformanceFrequency(&frequency);
    __atomic_store_n(&g_mtp_ticks_per_second, frequency, __ATOMIC_RELAXED);
  }
  (void)QueryPerformanceCounter(&count);
  return count / frequency * 1000000000ll +
         count % frequency * 1000000000ll / frequency;
}

static unsigned long __stdcall mtp_thread_entry(void *argument) {
  mtp_worker((unsigned)(size_t)argument);
  return 0;
}

static unsigned mtp_os_hardware_threads(void) {
  MtpSystemInfo info;
  info.dwNumberOfProcessors = 0;
  GetSystemInfo(&info);
  return info.dwNumberOfProcessors ? (unsigned)info.dwNumberOfProcessors : 1u;
}

static int mtp_os_spawn(unsigned index) {
  void *handle =
      CreateThread(NULL, 0, mtp_thread_entry, (void *)(size_t)index, 0, NULL);
  if (!handle) {
    return 0;
  }
  (void)CloseHandle(handle);
  return 1;
}

static void mtp_os_wait(MtpWord *word, uint32_t seen) {
  AcquireSRWLockExclusive(&word->lock);
  if (__atomic_load_n(&word->value, __ATOMIC_SEQ_CST) == seen) {
    (void)SleepConditionVariableSRW(&word->ready, &word->lock, 0xffffffffu, 0);
  }
  ReleaseSRWLockExclusive(&word->lock);
}

static void mtp_os_wake(MtpWord *word, unsigned count) {
  AcquireSRWLockExclusive(&word->lock);
  ReleaseSRWLockExclusive(&word->lock);
  for (unsigned i = 0; i < count; i++) {
    WakeConditionVariable(&word->ready);
  }
}

#elif defined(MT_HOSTED)

#include "runtime/hosted_posix.h"

#define MTP_THREAD_STACK_BYTES (1024u * 1024u)

static void *mtp_thread_entry(void *argument) {
  mtp_worker((unsigned)(size_t)argument);
  return NULL;
}

static long long mtp_os_now_ns(void) {
  long long now[2] = {0, 0};
  (void)mt_hosted_monotonic(now);
  return now[0] * 1000000000ll + now[1];
}

static unsigned mtp_os_hardware_threads(void) {
  return mt_hosted_hardware_threads();
}

static int mtp_os_spawn(unsigned index) {
  return mt_hosted_spawn_detached(mtp_thread_entry, (void *)(size_t)index,
                                  MTP_THREAD_STACK_BYTES);
}

static void mtp_os_wait(MtpWord *word, uint32_t seen) {
  (void)mt_hosted_wait_on((volatile int *)&word->value, (int)seen,
                          MT_HOSTED_INFINITE);
}

static void mtp_os_wake(MtpWord *word, unsigned count) {
  (void)count;
  mt_hosted_wake((volatile int *)&word->value);
}

#else

#if defined(__x86_64__)
#define MTP_SYS_FUTEX 202L
#define MTP_SYS_SCHED_GETAFFINITY 204L
#define MTP_SYS_CLOCK_GETTIME 228L
#elif defined(__aarch64__)
#define MTP_SYS_FUTEX 98L
#define MTP_SYS_SCHED_GETAFFINITY 123L
#define MTP_SYS_CLOCK_GETTIME 113L
#endif

#define MTP_FUTEX_WAIT_PRIVATE 128L
#define MTP_FUTEX_WAKE_PRIVATE 129L

extern int pthread_create(unsigned long *thread, const void *attributes,
                          void *(*start)(void *), void *argument);
extern int pthread_detach(unsigned long thread);

static long mtp_syscall(long number, long first, long second, long third,
                        long fourth) {
#if defined(__x86_64__)
  long result;
  register long r10 __asm__("r10") = fourth;
  __asm__ __volatile__("syscall"
                       : "=a"(result)
                       : "a"(number), "D"(first), "S"(second), "d"(third),
                         "r"(r10)
                       : "rcx", "r11", "memory");
  return result;
#elif defined(__aarch64__)
  register long x8 __asm__("x8") = number;
  register long x0 __asm__("x0") = first;
  register long x1 __asm__("x1") = second;
  register long x2 __asm__("x2") = third;
  register long x3 __asm__("x3") = fourth;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                       : "memory", "cc");
  return x0;
#else
  (void)number;
  (void)first;
  (void)second;
  (void)third;
  (void)fourth;
  return -1;
#endif
}

static void *mtp_thread_entry(void *argument) {
  mtp_worker((unsigned)(size_t)argument);
  return NULL;
}

static long long mtp_os_now_ns(void) {
#if defined(MTP_SYS_CLOCK_GETTIME)
  long long now[2] = {0, 0};
  (void)mtp_syscall(MTP_SYS_CLOCK_GETTIME, 1L, (long)(size_t)now, 0L, 0L);
  return now[0] * 1000000000ll + now[1];
#else
  return 0;
#endif
}

static unsigned mtp_os_hardware_threads(void) {
#if defined(MTP_SYS_SCHED_GETAFFINITY)
  unsigned long long mask[16] = {0};
  long bytes = mtp_syscall(MTP_SYS_SCHED_GETAFFINITY, 0L, (long)sizeof(mask),
                           (long)(size_t)mask, 0L);
  unsigned count = 0;
  for (long i = 0; i < bytes / 8 && i < 16; i++) {
    count += (unsigned)__builtin_popcountll(mask[i]);
  }
  return count ? count : 1u;
#else
  return 1u;
#endif
}

static int mtp_os_spawn(unsigned index) {
  unsigned long thread = 0;
  if (pthread_create(&thread, NULL, mtp_thread_entry, (void *)(size_t)index)) {
    return 0;
  }
  (void)pthread_detach(thread);
  return 1;
}

static void mtp_os_wait(MtpWord *word, uint32_t seen) {
#if defined(MTP_SYS_FUTEX)
  (void)mtp_syscall(MTP_SYS_FUTEX, (long)(size_t)&word->value,
                    MTP_FUTEX_WAIT_PRIVATE, (long)seen, 0L);
#else
  (void)word;
  (void)seen;
  mtp_cpu_pause();
#endif
}

static void mtp_os_wake(MtpWord *word, unsigned count) {
#if defined(MTP_SYS_FUTEX)
  (void)mtp_syscall(MTP_SYS_FUTEX, (long)(size_t)&word->value,
                    MTP_FUTEX_WAKE_PRIVATE, (long)count, 0L);
#else
  (void)word;
  (void)count;
#endif
}

#endif

static long mtp_parse_count(const char *text) {
  long value = 0;
  if (!text) {
    return 0;
  }
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  while (*text >= '0' && *text <= '9') {
    value = value * 10 + (*text - '0');
    if (value > (long)METTLE_PARALLEL_MAX_THREADS) {
      return (long)METTLE_PARALLEL_MAX_THREADS;
    }
    text++;
  }
  return value;
}

static unsigned mtp_threads(void) {
  unsigned threads = __atomic_load_n(&g_pool.threads, __ATOMIC_RELAXED);
  if (!threads) {
    long requested = mtp_parse_count(getenv("METTLE_PARALLEL_THREADS"));
    if (requested < 1) {
      requested = (long)mtp_os_hardware_threads();
    }
    if (requested > (long)METTLE_PARALLEL_MAX_THREADS) {
      requested = (long)METTLE_PARALLEL_MAX_THREADS;
    }
    threads = (unsigned)requested;
    __atomic_store_n(&g_pool.threads, threads, __ATOMIC_RELAXED);
  }
  return threads;
}

static uint64_t mtp_pack(uint32_t begin, uint32_t end) {
  return ((uint64_t)end << 32) | begin;
}

static uint32_t mtp_begin(uint64_t range) { return (uint32_t)range; }

static uint32_t mtp_end(uint64_t range) { return (uint32_t)(range >> 32); }

static uint32_t mtp_left(uint64_t range) {
  return mtp_begin(range) < mtp_end(range) ? mtp_end(range) - mtp_begin(range)
                                           : 0u;
}

static int mtp_pop(unsigned slot, uint64_t *seen, uint32_t *grain) {
  uint64_t range = __atomic_load_n(&g_slots[slot].range, __ATOMIC_ACQUIRE);
  while (mtp_left(range)) {
    if (__atomic_compare_exchange_n(
            &g_slots[slot].range, &range,
            mtp_pack(mtp_begin(range) + 1u, mtp_end(range)), 1,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      *grain = mtp_begin(range);
      return 1;
    }
  }
  *seen = range;
  return 0;
}

static int mtp_steal(unsigned thief, unsigned participants, uint32_t *first,
                     uint32_t *last) {
  for (;;) {
    unsigned victim = participants;
    uint32_t most = 0;
    uint64_t seen = 0;
    for (unsigned step = 1; step < participants; step++) {
      unsigned v = thief + step < participants ? thief + step
                                               : thief + step - participants;
      uint64_t range = __atomic_load_n(&g_slots[v].range, __ATOMIC_ACQUIRE);
      if (mtp_left(range) > most) {
        most = mtp_left(range);
        victim = v;
        seen = range;
      }
    }
    if (victim == participants) {
      return 0;
    }
    uint32_t split = mtp_end(seen) - (most + 1u) / 2u;
    if (__atomic_compare_exchange_n(&g_slots[victim].range, &seen,
                                    mtp_pack(mtp_begin(seen), split), 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      *first = split;
      *last = mtp_end(seen);
      return 1;
    }
  }
}

static void mtp_run_grain(uint32_t grain, unsigned slot) {
  const MtpJob *job = &g_pool.job;
  long long lo = job->lo + job->grain * (long long)grain;
  long long hi = lo + job->grain < job->hi ? lo + job->grain : job->hi;
  if (job->slot_body) {
    job->slot_body(job->ctx, lo, hi, (long long)slot);
  } else {
    job->body(job->ctx, lo, hi);
  }
}

static unsigned mtp_work(unsigned slot, unsigned participants) {
  unsigned done = 0;
  for (;;) {
    uint64_t empty = 0;
    uint32_t first = 0;
    uint32_t last = 0;
    while (mtp_pop(slot, &empty, &first)) {
      mtp_run_grain(first, slot);
      done++;
    }
    if (!mtp_steal(slot, participants, &first, &last)) {
      return done;
    }
    if (last - first > 1u &&
        __atomic_compare_exchange_n(&g_slots[slot].range, &empty,
                                    mtp_pack(first + 1u, last), 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      last = first + 1u;
    }
    for (uint32_t grain = first; grain < last; grain++) {
      mtp_run_grain(grain, slot);
      done++;
    }
  }
}

static void mtp_finish(unsigned done) {
  if (done &&
      __atomic_sub_fetch(&g_pool.pending.value, done, __ATOMIC_SEQ_CST) == 0 &&
      __atomic_load_n(&g_pool.joining, __ATOMIC_SEQ_CST)) {
    mtp_os_wake(&g_pool.pending, 1u);
  }
}

static void mtp_wake_some(void) {
  int32_t budget = __atomic_load_n(&g_pool.wake_budget, __ATOMIC_ACQUIRE);
  while (budget > 0) {
    int32_t share =
        budget > (int32_t)MTP_WAKE_FANOUT ? (int32_t)MTP_WAKE_FANOUT : budget;
    if (__atomic_compare_exchange_n(&g_pool.wake_budget, &budget,
                                    budget - share, 1, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
      mtp_os_wake(&g_pool.generation, (unsigned)share);
      return;
    }
  }
}

typedef struct {
  long long start;
  long long limit;
  unsigned rounds;
} MtpSpin;

static void mtp_spin_begin(MtpSpin *spin, long long limit_ns) {
  spin->start = mtp_os_now_ns();
  spin->limit = limit_ns;
  spin->rounds = 0;
}

static int mtp_spin(MtpSpin *spin) {
  mtp_cpu_pause();
  spin->rounds++;
  if (spin->rounds % MTP_CLOCK_EVERY != 0) {
    return 1;
  }
  return spin->rounds < MTP_SPIN_ROUNDS_CAP &&
         mtp_os_now_ns() - spin->start < spin->limit;
}

static uint32_t mtp_await_generation(uint32_t seen) {
  MtpSpin spin;
  uint32_t generation;
  mtp_spin_begin(&spin, MTP_IDLE_SPIN_NS);
  while ((generation = __atomic_load_n(&g_pool.generation.value,
                                       __ATOMIC_ACQUIRE)) == seen) {
    if (mtp_spin(&spin)) {
      continue;
    }
    __atomic_add_fetch(&g_pool.parked, 1, __ATOMIC_SEQ_CST);
    mtp_os_wait(&g_pool.generation, seen);
    __atomic_sub_fetch(&g_pool.parked, 1, __ATOMIC_SEQ_CST);
    mtp_wake_some();
  }
  return generation;
}

static void mtp_worker(unsigned index) {
  uint32_t seen = 0;
  for (;;) {
    seen = mtp_await_generation(seen);
    mtp_finish(mtp_work(index, g_pool.workers + 1u));
  }
}

static void mtp_pool_release(void) {
  __atomic_store_n(&g_pool.busy, 0, __ATOMIC_RELEASE);
}

static void mtp_pool_spawn_once(void) {
  unsigned threads = mtp_threads();
  if (g_pool.spawned) {
    return;
  }
  g_pool.spawned = 1;
  while (g_pool.workers + 1u < threads && mtp_os_spawn(g_pool.workers)) {
    __atomic_add_fetch(&g_pool.workers, 1u, __ATOMIC_RELEASE);
  }
}

static int mtp_pool_acquire(void) {
  int32_t idle = 0;
  if (!__atomic_compare_exchange_n(&g_pool.busy, &idle, 1, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    return 0;
  }
  mtp_pool_spawn_once();
  if (!g_pool.workers) {
    mtp_pool_release();
    return 0;
  }
  return 1;
}

static void mtp_publish(const MtpJob *job, uint32_t grains,
                        unsigned participants) {
  g_pool.job = *job;
  __atomic_store_n(&g_pool.pending.value, grains, __ATOMIC_RELAXED);
  for (unsigned slot = 0; slot < participants; slot++) {
    uint32_t begin = (uint32_t)((uint64_t)grains * slot / participants);
    uint32_t end = (uint32_t)((uint64_t)grains * (slot + 1u) / participants);
    __atomic_store_n(&g_slots[slot].range, mtp_pack(begin, end),
                     __ATOMIC_RELEASE);
  }
  __atomic_add_fetch(&g_pool.generation.value, 1u, __ATOMIC_SEQ_CST);
  int32_t parked = __atomic_load_n(&g_pool.parked, __ATOMIC_SEQ_CST);
  if (parked > 0) {
    __atomic_store_n(&g_pool.wake_budget, parked, __ATOMIC_RELEASE);
    mtp_wake_some();
  }
}

static void mtp_join(long long grain_ns) {
  MtpSpin spin;
  uint32_t left;
  long long limit = 2 * grain_ns;
  if (limit < MTP_JOIN_SPIN_MIN_NS) {
    limit = MTP_JOIN_SPIN_MIN_NS;
  }
  if (limit > MTP_JOIN_SPIN_MAX_NS) {
    limit = MTP_JOIN_SPIN_MAX_NS;
  }
  mtp_spin_begin(&spin, limit);
  while ((left = __atomic_load_n(&g_pool.pending.value, __ATOMIC_ACQUIRE))) {
    if (mtp_spin(&spin)) {
      continue;
    }
    __atomic_store_n(&g_pool.joining, 1, __ATOMIC_SEQ_CST);
    left = __atomic_load_n(&g_pool.pending.value, __ATOMIC_SEQ_CST);
    if (left) {
      mtp_os_wait(&g_pool.pending, left);
    }
    __atomic_store_n(&g_pool.joining, 0, __ATOMIC_RELAXED);
  }
}

static uint32_t mtp_plan(long long total, long long min_chunk,
                         unsigned participants, long long *grain) {
  long long target = (long long)participants * MTP_GRAINS_PER_THREAD;
  long long size = (total + target - 1) / target;
  if (size < min_chunk) {
    size = min_chunk;
  }
  if (size < 1) {
    size = 1;
  }
  *grain = size;
  return (uint32_t)((total + size - 1) / size);
}

static void mtp_dispatch(MtpJob *job, long long min_chunk) {
  long long total = job->hi - job->lo;
  if (total <= 0) {
    return;
  }
  unsigned participants = mtp_threads();
  if (participants < 2u || total < 2 * (min_chunk < 1 ? 1 : min_chunk) ||
      !mtp_pool_acquire()) {
    if (job->slot_body) {
      job->slot_body(job->ctx, job->lo, job->hi, 0);
    } else {
      job->body(job->ctx, job->lo, job->hi);
    }
    return;
  }
  participants = g_pool.workers + 1u;
  uint32_t grains = mtp_plan(total, min_chunk, participants, &job->grain);
  mtp_publish(job, grains, participants);
  long long start = mtp_os_now_ns();
  unsigned done = mtp_work(g_pool.workers, participants);
  long long spent = mtp_os_now_ns() - start;
  mtp_finish(done);
  mtp_join(done ? spent / (long long)done : 0);
  mtp_pool_release();
}

void mettle_parallel_range(MettleParallelBody body, void *ctx, long long lo,
                           long long hi, long long min_chunk) {
  MtpJob job = {body, NULL, ctx, lo, hi, 0};
  if (body) {
    mtp_dispatch(&job, min_chunk);
  }
}

void mettle_parallel_range_slots(MettleParallelSlotBody body, void *ctx,
                                 long long lo, long long hi,
                                 long long min_chunk) {
  MtpJob job = {NULL, body, ctx, lo, hi, 0};
  if (body) {
    mtp_dispatch(&job, min_chunk);
  }
}

long long mettle_parallel_threads(void) { return (long long)mtp_threads(); }

typedef long long MtpVector __attribute__((vector_size(16), may_alias));
typedef MtpVector MtpUnalignedVector __attribute__((aligned(1)));

typedef struct {
  unsigned char *destination;
  const unsigned char *source;
  unsigned char pattern[16] __attribute__((aligned(16)));
  unsigned period;
  int stream;
} MtpMemoryJob;

static MtpVector mtp_load(const unsigned char *at) {
  return *(const MtpUnalignedVector *)at;
}

static void mtp_store(unsigned char *at, MtpVector value) {
  *(MtpUnalignedVector *)at = value;
}

static void mtp_stream(unsigned char *at, MtpVector value) {
#if defined(__x86_64__)
  __asm__ __volatile__("movntdq %1, %0" : "=m"(*(MtpVector *)at) : "x"(value));
#else
  *(MtpVector *)at = value;
#endif
}

static void mtp_stream_fence(void) {
#if defined(__x86_64__)
  __asm__ __volatile__("sfence" ::: "memory");
#endif
}

static long long mtp_bytes_to_alignment(const unsigned char *at) {
  return (long long)((MTP_VECTOR_BYTES - (uintptr_t)at % MTP_VECTOR_BYTES) %
                     MTP_VECTOR_BYTES);
}

static void mtp_fill_span(unsigned char *d, long long n,
                          const MtpMemoryJob *job) {
  unsigned mask = job->period - 1u;
  long long head = mtp_bytes_to_alignment(d);
  unsigned char block[16] __attribute__((aligned(16)));
  MtpVector value;
  long long i;
  if (head > n) {
    head = n;
  }
  for (i = 0; i < head; i++) {
    d[i] = job->pattern[(unsigned)i & mask];
  }
  for (i = 0; i < 16; i++) {
    block[i] = job->pattern[(unsigned)(head + i) & mask];
  }
  value = *(const MtpVector *)block;
  d += head;
  n -= head;
  if (job->stream) {
    for (; n >= 64; d += 64, n -= 64) {
      mtp_stream(d, value);
      mtp_stream(d + 16, value);
      mtp_stream(d + 32, value);
      mtp_stream(d + 48, value);
    }
    for (; n >= 16; d += 16, n -= 16) {
      mtp_stream(d, value);
    }
    mtp_stream_fence();
  } else {
    for (; n >= 64; d += 64, n -= 64) {
      mtp_store(d, value);
      mtp_store(d + 16, value);
      mtp_store(d + 32, value);
      mtp_store(d + 48, value);
    }
    for (; n >= 16; d += 16, n -= 16) {
      mtp_store(d, value);
    }
  }
  for (i = 0; i < n; i++) {
    d[i] = job->pattern[(unsigned)(head + i) & mask];
  }
}

static void mtp_copy_span(unsigned char *d, const unsigned char *s,
                          long long n, int stream) {
  long long head = mtp_bytes_to_alignment(d);
  long long i;
  if (head > n) {
    head = n;
  }
  for (i = 0; i < head; i++) {
    d[i] = s[i];
  }
  d += head;
  s += head;
  n -= head;
  if (stream) {
    for (; n >= 64; d += 64, s += 64, n -= 64) {
      mtp_stream(d, mtp_load(s));
      mtp_stream(d + 16, mtp_load(s + 16));
      mtp_stream(d + 32, mtp_load(s + 32));
      mtp_stream(d + 48, mtp_load(s + 48));
    }
    for (; n >= 16; d += 16, s += 16, n -= 16) {
      mtp_stream(d, mtp_load(s));
    }
    mtp_stream_fence();
  } else {
    for (; n >= 64; d += 64, s += 64, n -= 64) {
      mtp_store(d, mtp_load(s));
      mtp_store(d + 16, mtp_load(s + 16));
      mtp_store(d + 32, mtp_load(s + 32));
      mtp_store(d + 48, mtp_load(s + 48));
    }
    for (; n >= 16; d += 16, s += 16, n -= 16) {
      mtp_store(d, mtp_load(s));
    }
  }
  for (i = 0; i < n; i++) {
    d[i] = s[i];
  }
}

static void mtp_move_overlapping(unsigned char *d, const unsigned char *s,
                                 long long n) {
  long long i;
  if (d < s) {
    if (s - d >= 16) {
      mtp_copy_span(d, s, n, 0);
      return;
    }
    for (i = 0; i < n; i++) {
      d[i] = s[i];
    }
    return;
  }
  if (d - s >= 16) {
    for (; n >= 16; n -= 16) {
      mtp_store(d + n - 16, mtp_load(s + n - 16));
    }
  }
  for (i = n; i > 0; i--) {
    d[i - 1] = s[i - 1];
  }
}

static void mtp_fill_body(void *ctx, long long lo, long long hi) {
  const MtpMemoryJob *job = (const MtpMemoryJob *)ctx;
  long long period = (long long)job->period;
  mtp_fill_span(job->destination + lo * period, (hi - lo) * period, job);
}

static void mtp_copy_body(void *ctx, long long lo, long long hi) {
  const MtpMemoryJob *job = (const MtpMemoryJob *)ctx;
  mtp_copy_span(job->destination + lo, job->source + lo, hi - lo,
                job->stream);
}

void mettle_parallel_fill(void *destination, long long count, long long value,
                          int element_size) {
  MtpMemoryJob job;
  unsigned period = 1u;
  if (element_size == 2 || element_size == 4 || element_size == 8) {
    period = (unsigned)element_size;
  }
  if (!destination || count <= 0) {
    return;
  }
  for (unsigned i = 0; i < 16u; i++) {
    job.pattern[i] = (unsigned char)(value >> (8u * (i & (period - 1u))));
  }
  job.destination = (unsigned char *)destination;
  job.source = NULL;
  job.period = period;
  job.stream = count * (long long)period >= MTP_STREAM_THRESHOLD_BYTES;
  mettle_parallel_range(mtp_fill_body, &job, 0, count,
                        MTP_MEMORY_CHUNK_BYTES / (long long)period);
}

void mettle_parallel_copy_bytes(void *destination, const void *source,
                                long long bytes) {
  MtpMemoryJob job;
  unsigned char *d = (unsigned char *)destination;
  const unsigned char *s = (const unsigned char *)source;
  if (!destination || !source || bytes <= 0 || d == s) {
    return;
  }
  if (d < s + bytes && s < d + bytes) {
    mtp_move_overlapping(d, s, bytes);
    return;
  }
  job.destination = d;
  job.source = s;
  job.period = 1u;
  job.stream = bytes >= MTP_STREAM_THRESHOLD_BYTES;
  mettle_parallel_range(mtp_copy_body, &job, 0, bytes, MTP_MEMORY_CHUNK_BYTES);
}
