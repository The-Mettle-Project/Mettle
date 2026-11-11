
#include "safety.h"
#include "crash_handler.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define SAFETY_L1_BITS 12
#define SAFETY_L2_BITS 16
#define SAFETY_L3_BITS 16
#define SAFETY_L1_SIZE (1u << SAFETY_L1_BITS)
#define SAFETY_L2_SIZE (1u << SAFETY_L2_BITS)
#define SAFETY_L3_SIZE (1u << SAFETY_L3_BITS)
#define SAFETY_ADDRESS_BITS \
  (SAFETY_L1_BITS + SAFETY_L2_BITS + SAFETY_L3_BITS + 4)

#define SAFETY_ID_UNOWNED 0u
#define SAFETY_ID_CONTESTED 1u
#define SAFETY_ID_FIRST 2u

#define SAFETY_BLOCK_SHIFT 12
#define SAFETY_BLOCK_SIZE (1u << SAFETY_BLOCK_SHIFT)
#define SAFETY_MAX_BLOCKS 4096

typedef enum {
  SAFETY_STATE_FREELIST = 0,
  SAFETY_STATE_LIVE = 1,
  SAFETY_STATE_DEAD = 2
} SafetyState;

typedef struct {
  uintptr_t start;
  uint64_t size;
  uint64_t granules;
  uint32_t state;
  uint32_t next_free;
  uint64_t identity;
  uint32_t heap;
  uint32_t foreign;
} SafetyRegion;

#if defined(__GNUC__)
#define SAFETY_COLD __attribute__((noinline, cold))
#else
#define SAFETY_COLD
#endif

static uint32_t **g_safety_l1[SAFETY_L1_SIZE];
static SafetyRegion *g_safety_blocks[SAFETY_MAX_BLOCKS];
static uint32_t g_safety_region_next = SAFETY_ID_FIRST;
static uint32_t g_safety_region_free;
static volatile long g_safety_lock;
static volatile uint64_t g_safety_live_regions;
static uint64_t g_safety_generation;
static void safety_values_clear(uintptr_t address, uint64_t size);
static void safety_values_reset(void);

static void safety_internal_failure(const char *message) {
  mettle_crash_trap_ex(METTLE_CRASH_TRAP_UNKNOWN, message,
                       __builtin_return_address(0),
                       __builtin_frame_address(0), 0, 0);
  __builtin_trap();
}

static void *safety_map(size_t bytes) {
#ifdef METTLE_SAFETY_TESTING
  extern int mettle_safety_test_fail_map(void);
  if (mettle_safety_test_fail_map()) {
    return NULL;
  }
#endif
#if defined(_WIN32) || defined(_WIN64)
  return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return memory == MAP_FAILED ? NULL : memory;
#endif
}

static void safety_unmap(void *memory, size_t bytes) {
  if (!memory) {
    return;
  }
#if defined(_WIN32) || defined(_WIN64)
  (void)bytes;
  VirtualFree(memory, 0, MEM_RELEASE);
#else
  munmap(memory, bytes);
#endif
}

static void safety_lock(void) {
  while (__atomic_exchange_n(&g_safety_lock, 1L, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&g_safety_lock, __ATOMIC_RELAXED)) {
#if defined(__i386__) || defined(__x86_64__)
      __builtin_ia32_pause();
#endif
    }
  }
}

static void safety_unlock(void) {
  __atomic_store_n(&g_safety_lock, 0L, __ATOMIC_RELEASE);
}

static SafetyRegion *safety_region(uint32_t id) {
  uint32_t block = id >> SAFETY_BLOCK_SHIFT;
  if (block >= SAFETY_MAX_BLOCKS) {
    return NULL;
  }
  SafetyRegion *entries =
      __atomic_load_n(&g_safety_blocks[block], __ATOMIC_ACQUIRE);
  return entries ? &entries[id & (SAFETY_BLOCK_SIZE - 1)] : NULL;
}

static void safety_region_publish(SafetyRegion *region, uint32_t id,
                                  uintptr_t start, uint64_t size, uint32_t heap) {
  uint64_t generation = ++g_safety_generation;
  __atomic_store_n(&region->identity, UINT64_C(0), __ATOMIC_RELEASE);
  region->start = start;
  region->size = size;
  region->granules = 0;
  region->next_free = 0;
  region->heap = heap;
  region->foreign = 0;
  __atomic_store_n(&region->identity, (generation << 24) | id, __ATOMIC_RELEASE);
  __atomic_store_n(&region->state, (uint32_t)SAFETY_STATE_LIVE, __ATOMIC_RELEASE);
}

static uint32_t safety_region_acquire(uintptr_t start, uint64_t size,
                                      uint32_t heap) {
  if (g_safety_generation == ((UINT64_C(1) << 40) - 1)) {
    return 0;
  }
  uint32_t id = g_safety_region_free;
  if (id != 0) {
    SafetyRegion *reused = safety_region(id);
    g_safety_region_free = reused->next_free;
    safety_region_publish(reused, id, start, size, heap);
    return id;
  }

  id = g_safety_region_next;
  uint32_t block = id >> SAFETY_BLOCK_SHIFT;
  if (block >= SAFETY_MAX_BLOCKS) {
    return 0;
  }
  if (!g_safety_blocks[block]) {
    SafetyRegion *entries =
        (SafetyRegion *)safety_map(SAFETY_BLOCK_SIZE * sizeof(SafetyRegion));
    if (!entries) {
      return 0;
    }
    __atomic_store_n(&g_safety_blocks[block], entries, __ATOMIC_RELEASE);
  }
  g_safety_region_next = id + 1;
  safety_region_publish(safety_region(id), id, start, size, heap);
  return id;
}

static int safety_region_read(const SafetyRegion *region, uint64_t identity,
                              SafetyRegion *out) {
  if (!region ||
      __atomic_load_n(&region->identity, __ATOMIC_ACQUIRE) != identity) {
    return 0;
  }
  out->state = __atomic_load_n(&region->state, __ATOMIC_ACQUIRE);
  out->start = region->start;
  out->size = region->size;
  out->heap = region->heap;
  out->foreign = region->foreign;
  out->identity = identity;
  out->granules = 0;
  out->next_free = 0;
  return __atomic_load_n(&region->identity, __ATOMIC_ACQUIRE) == identity;
}

static void safety_region_recycle(uint32_t id, SafetyRegion *region) {
  region->size = 0;
  region->granules = 0;
  __atomic_store_n(&region->state, (uint32_t)SAFETY_STATE_FREELIST,
                   __ATOMIC_RELEASE);
  region->next_free = g_safety_region_free;
  g_safety_region_free = id;
}

static void safety_region_drop_granule(uint32_t id) {
  SafetyRegion *region = safety_region(id);
  if (!region || region->granules == 0) {
    return;
  }
  region->granules--;
  if (region->granules == 0 &&
      __atomic_load_n(&region->state, __ATOMIC_RELAXED) ==
          (uint32_t)SAFETY_STATE_DEAD) {
    safety_region_recycle(id, region);
  }
}

static uint32_t *safety_slot(uintptr_t address, int create) {
  if ((address >> SAFETY_ADDRESS_BITS) != 0) {
    return NULL;
  }
  uintptr_t granule = address / METTLE_SAFETY_GRANULE;
  size_t l1 = (size_t)(granule >> (SAFETY_L2_BITS + SAFETY_L3_BITS)) &
              (SAFETY_L1_SIZE - 1);
  size_t l2 = (size_t)(granule >> SAFETY_L3_BITS) & (SAFETY_L2_SIZE - 1);
  size_t l3 = (size_t)granule & (SAFETY_L3_SIZE - 1);

  uint32_t **level2 = __atomic_load_n(&g_safety_l1[l1], __ATOMIC_ACQUIRE);
  if (!level2) {
    if (!create) {
      return NULL;
    }
    level2 = (uint32_t **)safety_map(SAFETY_L2_SIZE * sizeof(uint32_t *));
    if (!level2) {
      return NULL;
    }
    __atomic_store_n(&g_safety_l1[l1], level2, __ATOMIC_RELEASE);
  }

  uint32_t *level3 = __atomic_load_n(&level2[l2], __ATOMIC_ACQUIRE);
  if (!level3) {
    if (!create) {
      return NULL;
    }
    level3 = (uint32_t *)safety_map(SAFETY_L3_SIZE * sizeof(uint32_t));
    if (!level3) {
      return NULL;
    }
    __atomic_store_n(&level2[l2], level3, __ATOMIC_RELEASE);
  }

  return &level3[l3];
}

static uint32_t safety_lookup(uintptr_t address) {
  uint32_t *slot = safety_slot(address, 0);
  uint32_t id = slot ? __atomic_load_n(slot, __ATOMIC_RELAXED) : SAFETY_ID_UNOWNED;
  if (id != SAFETY_ID_CONTESTED) return id;
  uint64_t newest = 0;
  uint32_t found = SAFETY_ID_UNOWNED;
  for (uint32_t candidate = SAFETY_ID_FIRST; candidate < g_safety_region_next; candidate++) {
    SafetyRegion *region = safety_region(candidate);
    if (region && region->state == SAFETY_STATE_LIVE &&
        address >= region->start && address - region->start < region->size &&
        region->identity > newest) {
      newest = region->identity;
      found = candidate;
    }
  }
  return found;
}

static int safety_claim_granules(uintptr_t start, uint64_t size, uint32_t id) {
  SafetyRegion *region = safety_region(id);
  uint64_t claimed = 0;
  uintptr_t first = start / METTLE_SAFETY_GRANULE;
  uintptr_t last = (start + size - 1) / METTLE_SAFETY_GRANULE;

  uint32_t *run = NULL;
  for (uintptr_t granule = first; granule <= last; granule++) {
    size_t within = (size_t)granule & (SAFETY_L3_SIZE - 1);
    if (!run || within == 0) {
      run = safety_slot((granule - within) * METTLE_SAFETY_GRANULE, 1);
      if (!run) {
        return 0;
      }
    }
    uint32_t *slot = &run[within];
    uint32_t current = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (current == id) {
      continue;
    }
    if (current >= SAFETY_ID_FIRST) {
      SafetyRegion *owner = safety_region(current);
      uint32_t state =
          owner ? __atomic_load_n(&owner->state, __ATOMIC_RELAXED) : 0;
      if (state == (uint32_t)SAFETY_STATE_LIVE) {
        __atomic_store_n(slot, SAFETY_ID_CONTESTED, __ATOMIC_RELAXED);
        safety_region_drop_granule(current);
        continue;
      }
      __atomic_store_n(slot, id, __ATOMIC_RELAXED);
      safety_region_drop_granule(current);
      claimed++;
      continue;
    }
    if (current == SAFETY_ID_CONTESTED) {
      continue;
    }
    __atomic_store_n(slot, id, __ATOMIC_RELAXED);
    claimed++;
  }

  if (region) {
    region->granules += claimed;
  }
  return 1;
}

int64_t mettle_safety_loop_length(int64_t bound, int64_t first_bound,
                                 int64_t counter_step, int64_t byte_step,
                                 int64_t access_size) {
  if (bound < first_bound) return 0;
  if (counter_step <= 0 || byte_step < 0 || access_size <= 0) return INT64_MAX;
  uint64_t rounds = ((uint64_t)bound - (uint64_t)first_bound) /
                    (uint64_t)counter_step;
  uint64_t room = (uint64_t)INT64_MAX - (uint64_t)access_size;
  if (byte_step && rounds > room / (uint64_t)byte_step) return INT64_MAX;
  return (int64_t)(rounds * (uint64_t)byte_step + (uint64_t)access_size);
}

static void safety_register_region(void *pointer, uint64_t size, uint32_t heap,
                                   int keep_values) {
  if (!pointer || size == 0) {
    return;
  }
  uintptr_t start = (uintptr_t)pointer;
  const uintptr_t address_limit = ((uintptr_t)1 << SAFETY_ADDRESS_BITS) - 1;
  if (start > address_limit || size - 1 > address_limit - start) {
    safety_internal_failure("Fatal error: memory registration exceeds the supported address range");
  }

  safety_lock();
  uint32_t existing = safety_lookup(start);
  if (existing >= SAFETY_ID_FIRST) {
    SafetyRegion *previous = safety_region(existing);
    if (previous && previous->start == start &&
        __atomic_load_n(&previous->state, __ATOMIC_RELAXED) ==
            (uint32_t)SAFETY_STATE_LIVE) {
      __atomic_store_n(&previous->state, (uint32_t)SAFETY_STATE_DEAD,
                       __ATOMIC_RELEASE);
      __atomic_sub_fetch(&g_safety_live_regions, 1, __ATOMIC_RELAXED);
      if (previous->granules == 0) safety_region_recycle(existing, previous);
    }
  }

  uint32_t id = safety_region_acquire(start, size, heap);
  if (id == 0 || !safety_claim_granules(start, size, id)) {
    safety_unlock();
    safety_internal_failure("Fatal error: unable to allocate memory safety metadata");
  }
  __atomic_add_fetch(&g_safety_live_regions, 1, __ATOMIC_RELAXED);
  safety_unlock();
  if (!keep_values) {
    safety_values_clear(start, size);
  }
}

void mettle_safety_register(void *pointer, uint64_t size) {
  safety_register_region(pointer, size, 1, 0);
}

void mettle_safety_register_static(void *pointer, uint64_t size) {
  safety_register_region(pointer, size, 0, 0);
}

static void safety_region_retire_locked(uint32_t id, SafetyRegion *region,
                                        uintptr_t *start, uint64_t *size) {
  __atomic_store_n(&region->state, (uint32_t)SAFETY_STATE_DEAD, __ATOMIC_RELEASE);
  *start = region->start;
  *size = region->size;
  __atomic_sub_fetch(&g_safety_live_regions, 1, __ATOMIC_RELAXED);
  if (region->granules == 0) safety_region_recycle(id, region);
}

void mettle_safety_unregister(void *pointer) {
  if (!pointer) {
    return;
  }
  uintptr_t start = (uintptr_t)pointer;
  uintptr_t retired_start = 0;
  uint64_t retired_size = 0;

  safety_lock();
  uint32_t id = safety_lookup(start);
  if (id >= SAFETY_ID_FIRST) {
    SafetyRegion *region = safety_region(id);
    if (region && region->start == start &&
        __atomic_load_n(&region->state, __ATOMIC_RELAXED) ==
            (uint32_t)SAFETY_STATE_LIVE) {
      safety_region_retire_locked(id, region, &retired_start, &retired_size);
    }
  }
  safety_unlock();
  safety_values_clear(retired_start, retired_size);
}

static void safety_retire_identity(uint64_t identity) {
  if (!identity) {
    return;
  }
  uintptr_t retired_start = 0;
  uint64_t retired_size = 0;
  safety_lock();
  uint32_t id = (uint32_t)(identity & 0xffffffu);
  SafetyRegion *region = safety_region(id);
  if (region && region->identity == identity &&
      __atomic_load_n(&region->state, __ATOMIC_RELAXED) ==
          (uint32_t)SAFETY_STATE_LIVE) {
    safety_region_retire_locked(id, region, &retired_start, &retired_size);
  }
  safety_unlock();
  safety_values_clear(retired_start, retired_size);
}

static uint64_t safety_identity_span(const void *pointer, uint64_t identity) {
  SafetyRegion region;
  if (!safety_region_read(safety_region((uint32_t)(identity & 0xffffffu)),
                          identity, &region) ||
      region.state != SAFETY_STATE_LIVE ||
      (uintptr_t)pointer < region.start ||
      (uintptr_t)pointer - region.start >= region.size) {
    return 0;
  }
  return region.size - ((uintptr_t)pointer - region.start);
}

/* A realloc that moves has already handed the old block back, so another thread
   may own that address by now. Only the identity read before the call may be
   retired. */
void mettle_safety_reregister_held(void *old_pointer, void *new_pointer,
                                   uint64_t size, uint64_t old_identity) {
  if (!new_pointer && size != 0) {
    return;
  }
  uint64_t old_size = old_identity ? safety_identity_span(old_pointer, old_identity) : 0;
  int in_place = old_pointer == new_pointer && new_pointer;
  safety_register_region(new_pointer, size, 1, in_place);
  if (in_place) {
    if (old_size > size) {
      safety_values_clear((uintptr_t)new_pointer + (uintptr_t)size, old_size - size);
    }
  } else if (old_pointer && new_pointer && old_size > 0) {
    uint64_t copy_size = old_size < size ? old_size : size;
    mettle_safety_value_copy(new_pointer, old_pointer, copy_size);
  }
  if (old_pointer && old_pointer != new_pointer) {
    safety_retire_identity(old_identity);
  }
}

void mettle_safety_reregister(void *old_pointer, void *new_pointer,
                              uint64_t size) {
  mettle_safety_reregister_held(old_pointer, new_pointer, size,
                                mettle_safety_identity(old_pointer));
}

struct SafetyCall;
struct SafetyPacket;

typedef struct {
  void *callee;
  uint64_t result;
  uint64_t arguments[METTLE_SAFETY_REGISTER_ARGUMENTS];
  uintptr_t allocator_depth;
  struct SafetyCall *current;
  struct SafetyCall *spare;
  struct SafetyPacket *spare_packets;
} SafetyThread;

#if defined(_WIN32)
static void safety_thread_release(SafetyThread *thread);

#define SAFETY_TEB_TLS_SLOTS 0x1480
#define SAFETY_TEB_TLS_COUNT 64u

static DWORD g_safety_thread_slot = TLS_OUT_OF_INDEXES;
static DWORD g_safety_thread_exit = FLS_OUT_OF_INDEXES;

static void NTAPI safety_thread_destroy(void *value) {
  SafetyThread *thread = (SafetyThread *)value;
  if (!thread) return;
  TlsSetValue(g_safety_thread_slot, NULL);
  safety_thread_release(thread);
  safety_unmap(thread, sizeof(*thread));
}

SAFETY_COLD static SafetyThread *safety_thread_create(void) {
  DWORD last_error = GetLastError();
  safety_lock();
  if (g_safety_thread_slot == TLS_OUT_OF_INDEXES) {
    g_safety_thread_exit = FlsAlloc(safety_thread_destroy);
    __atomic_store_n(&g_safety_thread_slot, TlsAlloc(), __ATOMIC_RELEASE);
  }
  DWORD slot = g_safety_thread_slot;
  DWORD exit_slot = g_safety_thread_exit;
  safety_unlock();
  if (slot == TLS_OUT_OF_INDEXES || exit_slot == FLS_OUT_OF_INDEXES) {
    safety_internal_failure("Fatal error: unable to create memory safety thread state");
  }
  SafetyThread *thread = (SafetyThread *)TlsGetValue(slot);
  if (!thread) {
    thread = (SafetyThread *)safety_map(sizeof(*thread));
    if (!thread || !TlsSetValue(slot, thread) || !FlsSetValue(exit_slot, thread)) {
      safety_internal_failure("Fatal error: unable to allocate memory safety thread state");
    }
  }
  SetLastError(last_error);
  return thread;
}

static SafetyThread *safety_thread(void) {
#if defined(_WIN64)
  DWORD slot = __atomic_load_n(&g_safety_thread_slot, __ATOMIC_ACQUIRE);
  if (slot < SAFETY_TEB_TLS_COUNT) {
    SafetyThread *thread =
        ((SafetyThread **)((char *)NtCurrentTeb() + SAFETY_TEB_TLS_SLOTS))[slot];
    if (thread) return thread;
  }
#endif
  return safety_thread_create();
}
#else
static __thread SafetyThread g_safety_thread;
static SafetyThread *safety_thread(void) { return &g_safety_thread; }
#endif

void *mettle_safety_thread(void) { return safety_thread(); }

static uintptr_t safety_allocator_depth(void) {
  return safety_thread()->allocator_depth;
}

void mettle_safety_enter_allocator(void) {
  SafetyThread *thread = safety_thread();
  if (thread->allocator_depth == UINTPTR_MAX) {
    safety_internal_failure("Fatal error: memory safety allocator nesting overflow");
  }
  thread->allocator_depth++;
}

void mettle_safety_leave_allocator(void) {
  SafetyThread *thread = safety_thread();
  if (thread->allocator_depth > 0) {
    thread->allocator_depth--;
  }
}

static void safety_append(char *buffer, size_t capacity, size_t *offset,
                          const char *text) {
  while (*text && *offset + 1 < capacity) {
    buffer[(*offset)++] = *text++;
  }
  buffer[*offset] = '\0';
}

static void safety_append_unsigned(char *buffer, size_t capacity,
                                   size_t *offset, uint64_t value) {
  char digits[24];
  size_t index = sizeof(digits);
  digits[--index] = '\0';
  do {
    digits[--index] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value != 0 && index > 0);
  safety_append(buffer, capacity, offset, digits + index);
}

static void safety_append_signed(char *buffer, size_t capacity, size_t *offset,
                                 int64_t value) {
  if (value < 0) {
    safety_append(buffer, capacity, offset, "-");
    safety_append_unsigned(buffer, capacity, offset,
                           (uint64_t)(-(value + 1)) + 1u);
    return;
  }
  safety_append_unsigned(buffer, capacity, offset, (uint64_t)value);
}

typedef struct {
  const char *headline;
  uint32_t line;
  int64_t offset;
  int64_t size;
  uint64_t extent;
  int have_extent;
  int about_free;
} SafetyFailure;

static void safety_report_and_trap(const SafetyFailure *failure,
                                   void *program_counter,
                                   void *frame_pointer) {
  char g_safety_message[512];
  size_t at = 0;
  const size_t capacity = sizeof(g_safety_message);
  g_safety_message[0] = '\0';

  safety_append(g_safety_message, capacity, &at, "Fatal error: ");
  safety_append(g_safety_message, capacity, &at, failure->headline);
  if (!failure->about_free) {
    safety_append(g_safety_message, capacity, &at, ": ");
    safety_append_signed(g_safety_message, capacity, &at, failure->size);
    safety_append(g_safety_message, capacity, &at, " bytes at offset ");
    safety_append_signed(g_safety_message, capacity, &at, failure->offset);
  }
  if (failure->have_extent) {
    safety_append(g_safety_message, capacity, &at,
                  failure->about_free ? ": a " : " of a ");
    safety_append_unsigned(g_safety_message, capacity, &at, failure->extent);
    safety_append(g_safety_message, capacity, &at, " byte allocation");
  }
  if (failure->line != 0) {
    safety_append(g_safety_message, capacity, &at, " (line ");
    safety_append_unsigned(g_safety_message, capacity, &at, failure->line);
    safety_append(g_safety_message, capacity, &at, ")");
  }

  mettle_crash_trap_ex(METTLE_CRASH_TRAP_UNKNOWN, g_safety_message,
                       program_counter, frame_pointer, 0, 0);
}

static int safety_snapshot_live(uintptr_t address, SafetyRegion *out) {
  uint32_t *slot = safety_slot(address, 0);
  uint32_t id = slot ? __atomic_load_n(slot, __ATOMIC_RELAXED) : SAFETY_ID_UNOWNED;
  if (id < SAFETY_ID_FIRST) {
    return 0;
  }
  const SafetyRegion *region = safety_region(id);
  if (!region) {
    return 0;
  }
  uint64_t identity = __atomic_load_n(&region->identity, __ATOMIC_ACQUIRE);
  return identity != 0 && safety_region_read(region, identity, out) &&
         out->state == SAFETY_STATE_LIVE && address >= out->start &&
         address - out->start < out->size;
}

static SafetyRegion safety_snapshot(uintptr_t address) {
  SafetyRegion live;
  if (safety_snapshot_live(address, &live)) {
    return live;
  }
  SafetyRegion snapshot = {0};
  safety_lock();
  uint32_t id = safety_lookup(address);
  if (id >= SAFETY_ID_FIRST) {
    const SafetyRegion *region = safety_region(id);
    if (region) {
      snapshot = *region;
    }
  }
  safety_unlock();
  return snapshot;
}

SAFETY_COLD static void safety_check_failed(const void *base, int64_t offset,
                                            int64_t size, uint32_t line,
                                            const SafetyRegion *region,
                                            void *program_counter,
                                            void *frame_pointer) {
  if (safety_allocator_depth() != 0) {
    return;
  }

  SafetyFailure failure;
  failure.line = line;
  failure.offset = offset;
  failure.size = size;
  failure.extent = 0;
  failure.have_extent = 0;
  failure.about_free = 0;
  if (base && region && region->start) {
    failure.offset = (int64_t)((uintptr_t)base + (uintptr_t)offset - region->start);
  }

  if (!base) {
    failure.headline = "null pointer dereference";
    safety_report_and_trap(&failure, program_counter, frame_pointer);
    return;
  }

  if (!region) {
    return;
  }
  uint32_t state = region->state;
  if (state == (uint32_t)SAFETY_STATE_FREELIST) {
    return;
  }

  failure.extent = region->size;
  failure.have_extent = region->size != 0;
  failure.headline = state == (uint32_t)SAFETY_STATE_DEAD
                         ? "use of memory after it was freed"
                         : "memory access outside its allocation";
  safety_report_and_trap(&failure, program_counter, frame_pointer);
}

void mettle_safety_check(const void *base, int64_t offset, int64_t size,
                         uint32_t access_kind, uint32_t line) {
  (void)access_kind;
  if (size <= 0) {
    return;
  }

  uintptr_t start = (uintptr_t)base;
  uintptr_t address = start + (uintptr_t)offset;

  SafetyRegion region = safety_snapshot(start);
  if (region.state != SAFETY_STATE_FREELIST) {
    if (region.state == SAFETY_STATE_LIVE) {
      uint64_t extent = region.size;
      uint64_t width = (uint64_t)size;
      int wrapped = offset >= 0 ? address < start : address > start;
      if (!wrapped && width <= extent && address >= region.start &&
          (uint64_t)(address - region.start) <= extent - width) {
        return;
      }
    }
    safety_check_failed(base, offset, size, line, &region,
                        __builtin_return_address(0),
                        __builtin_frame_address(0));
    return;
  }

  if (!base) {
    safety_check_failed(base, offset, size, line, NULL, __builtin_return_address(0),
                        __builtin_frame_address(0));
  }
}

int64_t mettle_safety_span(const void *base) {
  const int64_t unbounded = (int64_t)1 << 56;

  if (!base) {
    return 0;
  }
  uintptr_t start = (uintptr_t)base;
  SafetyRegion region = safety_snapshot(start);
  if (region.state == SAFETY_STATE_FREELIST) {
    return unbounded;
  }
  if (region.state != SAFETY_STATE_LIVE) {
    return 0;
  }
  uintptr_t end = region.start + region.size;
  if (start < region.start || start >= end) {
    return 0;
  }
  return (int64_t)(end - start);
}

#include "safety_provenance.inc"

void mettle_safety_check_affine(const void *base, int64_t offset, int64_t length,
    uint32_t kind, uint32_t line, uint64_t identity, int64_t width, int64_t step) {
  if (length <= 0) return;
  identity = safety_resolve_identity(base, identity);
  if (!identity || width <= 0 || step <= 0) {
    mettle_safety_check_identity(base, offset, width, kind, line, identity);
    return;
  }
  SafetyRegion region = safety_identity_snapshot(identity);
  uintptr_t start = (uintptr_t)base;
  uintptr_t address = start + (uintptr_t)offset;
  int wrapped = offset >= 0 ? address < start : address > start;
  if (base && !wrapped && region.state == SAFETY_STATE_LIVE &&
      address >= region.start && (uint64_t)length <= region.size &&
      address - region.start <= region.size - (uint64_t)length) return;

  if (base && !wrapped && region.state == SAFETY_STATE_LIVE &&
      address >= region.start && (uint64_t)width <= region.size &&
      address - region.start <= region.size - (uint64_t)width) {
    uint64_t room = region.size - (uint64_t)width - (address - region.start);
    uint64_t rounds = room / (uint64_t)step + 1;
    uint64_t limit = (uint64_t)INT64_MAX - (uint64_t)offset;
    offset = rounds <= limit / (uint64_t)step
        ? (int64_t)((uint64_t)offset + rounds * (uint64_t)step) : INT64_MAX;
  }
  safety_check_failed(base, offset, width, line, &region,
                      __builtin_return_address(0), __builtin_frame_address(0));
}

uint64_t mettle_safety_live_region_count(void) {
  return __atomic_load_n(&g_safety_live_regions, __ATOMIC_RELAXED);
}

uint64_t mettle_safety_descriptor_high_water(void) {
  safety_lock();
  uint64_t high_water = g_safety_region_next - SAFETY_ID_FIRST;
  safety_unlock();
  return high_water;
}

void mettle_safety_reset(void) {
  safety_lock();
  for (size_t l1 = 0; l1 < SAFETY_L1_SIZE; l1++) {
    uint32_t **level2 = g_safety_l1[l1];
    if (!level2) {
      continue;
    }
    for (size_t l2 = 0; l2 < SAFETY_L2_SIZE; l2++) {
      safety_unmap(level2[l2], SAFETY_L3_SIZE * sizeof(uint32_t));
    }
    safety_unmap(level2, SAFETY_L2_SIZE * sizeof(uint32_t *));
    g_safety_l1[l1] = NULL;
  }
  for (size_t block = 0; block < SAFETY_MAX_BLOCKS; block++) {
    safety_unmap(g_safety_blocks[block],
                 SAFETY_BLOCK_SIZE * sizeof(SafetyRegion));
    g_safety_blocks[block] = NULL;
  }
  g_safety_region_next = SAFETY_ID_FIRST;
  g_safety_region_free = 0;
  __atomic_store_n(&g_safety_live_regions, 0, __ATOMIC_RELAXED);
  safety_unlock();
  safety_values_reset();
}

#define SAFETY_TASK_STACK_SPAN (8u * 1024u * 1024u)

void *mettle_thread_stack_high(void);

static uintptr_t safety_stack_base(void) {
#if defined(_WIN64)
  const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
  return tib ? (uintptr_t)tib->StackBase : 0;
#elif defined(_WIN32)
  return 0;
#else
  return (uintptr_t)mettle_thread_stack_high();
#endif
}

static char g_task_message[512];

void mettle_safety_task_capture_check(const void *pointer, const char *task,
                               const char *sender, uint32_t line) {
  volatile char here = 0;
  uintptr_t low = (uintptr_t)(void *)&here;
  uintptr_t value = (uintptr_t)pointer;
  uintptr_t base = safety_stack_base();
  size_t at = 0;
  const size_t capacity = sizeof(g_task_message);
  (void)here;
  if (!pointer) {
    return;
  }
  if (base == 0 || base <= low) {
    base = low + SAFETY_TASK_STACK_SPAN;
  }
  if (value < low || value >= base) {
    return;
  }
  g_task_message[0] = '\0';
  safety_append(g_task_message, capacity, &at,
                "Fatal error: a pointer into the spawning thread's stack was "
                "handed to the task '");
  safety_append(g_task_message, capacity, &at, task ? task : "?");
  safety_append(g_task_message, capacity, &at, "' by '");
  safety_append(g_task_message, capacity, &at, sender ? sender : "?");
  safety_append(g_task_message, capacity, &at, "'");
  if (line != 0) {
    safety_append(g_task_message, capacity, &at, " (line ");
    safety_append_unsigned(g_task_message, capacity, &at, line);
    safety_append(g_task_message, capacity, &at, ")");
  }
  mettle_crash_trap_ex(METTLE_CRASH_TRAP_UNKNOWN, g_task_message, 0, 0, 0, 0);
}

#define SAFETY_DEADLINE_DEPTH 64

typedef struct {
  const char *name;
  int64_t limit;
  int64_t proven;
  int64_t spent;
} SafetyDeadline;

#if defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)
static SafetyDeadline g_deadline_stack[SAFETY_DEADLINE_DEPTH];
static unsigned g_deadline_depth;
#else
static __thread SafetyDeadline g_deadline_stack[SAFETY_DEADLINE_DEPTH];
static __thread unsigned g_deadline_depth;
#endif

static char g_deadline_message[512];

void mettle_safety_deadline_enter(const char *name, int64_t limit,
                                  int64_t proven) {
  if (g_deadline_depth >= SAFETY_DEADLINE_DEPTH) {
    g_deadline_depth++;
    return;
  }
  g_deadline_stack[g_deadline_depth].name = name;
  g_deadline_stack[g_deadline_depth].limit = limit;
  g_deadline_stack[g_deadline_depth].proven = proven;
  g_deadline_stack[g_deadline_depth].spent = 0;
  g_deadline_depth++;
}

void mettle_safety_deadline_step(int64_t cost) {
  if (g_deadline_depth == 0 || g_deadline_depth > SAFETY_DEADLINE_DEPTH) {
    return;
  }
  g_deadline_stack[g_deadline_depth - 1].spent += cost;
}

void mettle_safety_deadline_leave(void) {
  SafetyDeadline *frame = NULL;
  size_t at = 0;
  const size_t capacity = sizeof(g_deadline_message);
  if (g_deadline_depth == 0) {
    return;
  }
  g_deadline_depth--;
  if (g_deadline_depth >= SAFETY_DEADLINE_DEPTH) {
    return;
  }
  frame = &g_deadline_stack[g_deadline_depth];
  if (g_deadline_depth > 0) {
    g_deadline_stack[g_deadline_depth - 1].spent += frame->spent;
  }
  if (frame->spent <= frame->proven) {
    return;
  }
  g_deadline_message[0] = '\0';
  safety_append(g_deadline_message, capacity, &at,
                "Fatal error: the path '");
  safety_append(g_deadline_message, capacity, &at,
                frame->name ? frame->name : "?");
  safety_append(g_deadline_message, capacity, &at, "' actually took cost ");
  safety_append_signed(g_deadline_message, capacity, &at, frame->spent);
  safety_append(g_deadline_message, capacity, &at,
                ", more than the longest path the compiler proved (");
  safety_append_signed(g_deadline_message, capacity, &at, frame->proven);
  safety_append(g_deadline_message, capacity, &at, ")");
  mettle_crash_trap_ex(METTLE_CRASH_TRAP_UNKNOWN, g_deadline_message, 0, 0, 0,
                       0);
}
