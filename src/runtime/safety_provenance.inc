
#define SAFETY_IDENTITY_MIXED UINT64_MAX

uint64_t mettle_safety_identity(const void *pointer) {
  if (!pointer) return 0;
  SafetyRegion region = safety_snapshot((uintptr_t)pointer);
  return region.state == SAFETY_STATE_FREELIST ? 0 : region.identity;
}

static SafetyRegion safety_identity_snapshot(uint64_t identity) {
  SafetyRegion result;
  if (!safety_region_read(safety_region((uint32_t)(identity & 0xffffffu)),
                          identity, &result)) {
    SafetyRegion none = {0};
    none.state = SAFETY_STATE_DEAD;
    return none;
  }
  if (result.state == SAFETY_STATE_FREELIST) result.state = SAFETY_STATE_DEAD;
  return result;
}

void mettle_safety_cache_fill(uint64_t *cache, uint64_t identity, int64_t size) {
  SafetyRegion region = safety_identity_snapshot(identity);
  int holds = identity && identity != UINT64_MAX && size > 0 &&
              region.state == SAFETY_STATE_LIVE && (uint64_t)size <= region.size;
  cache[0] = holds ? identity : METTLE_SAFETY_SHADOW_PARTIAL;
  cache[1] = holds ? (uint64_t)region.start : 0;
  cache[2] = holds ? region.size - (uint64_t)size : 0;
}

// Two different identities met in one value, as in a table pointer plus an
// index hashed from another pointer. Which one is the base is unknown, so
// the address decides.
static uint64_t safety_resolve_identity(const void *base, uint64_t identity) {
  return identity == UINT64_MAX ? mettle_safety_identity(base) : identity;
}

// A pointer with no identity may still sit in memory the program named with
// memory_region, in any module. Only those regions answer by address.
static uint64_t safety_foreign_identity(const void *base) {
  if (!base) return 0;
  SafetyRegion region = safety_snapshot((uintptr_t)base);
  return region.state == SAFETY_STATE_LIVE && region.foreign ? region.identity : 0;
}

void mettle_safety_check_identity(const void *base, int64_t offset, int64_t size,
                                  uint32_t kind, uint32_t line,
                                  uint64_t identity) {
  (void)kind;
  SafetyRegion region = {0};
  identity = safety_resolve_identity(base, identity);
  if (!identity) identity = safety_foreign_identity(base);
  if (!identity) {
    if (size <= 0 || safety_allocator_depth()) return;
    SafetyFailure failure = {base ? "memory access has no tracked allocation identity" :
        "null pointer dereference", line, offset, size, 0, 0};
    safety_report_and_trap(&failure, __builtin_return_address(0), __builtin_frame_address(0));
    return;
  }
  if (size <= 0) return;
  region = safety_identity_snapshot(identity);
  uintptr_t start = (uintptr_t)base;
  uintptr_t address = start + (uintptr_t)offset;
  int wrapped = offset >= 0 ? address < start : address > start;
  uint64_t width = (uint64_t)size;
  if (base && !wrapped && region.state == SAFETY_STATE_LIVE &&
      width <= region.size && address >= region.start &&
      address - region.start <= region.size - width) return;
  safety_check_failed(base, offset, size, line, &region,
                      __builtin_return_address(0), __builtin_frame_address(0));
}

int64_t mettle_safety_span_identity(const void *base, uint64_t identity) {
  identity = safety_resolve_identity(base, identity);
  if (!identity) identity = safety_foreign_identity(base);
  if (!identity) return 0;
  SafetyRegion region = safety_identity_snapshot(identity);
  uintptr_t address = (uintptr_t)base;
  if (!base || region.state != SAFETY_STATE_LIVE || address < region.start ||
      address - region.start >= region.size) return 0;
  return (int64_t)(region.size - (address - region.start));
}

uint64_t mettle_safety_merge_identity(uint64_t a, uint64_t b) {
  return !a ? b : !b || a == b ? a : SAFETY_IDENTITY_MIXED;
}

uint64_t mettle_safety_subtract_identity(uint64_t a, uint64_t b) {
  return a && b ? 0 : a ? a : b;
}

#define SAFETY_SHADOW_BITS 15u
#define SAFETY_SHADOW_SIZE (1u << SAFETY_SHADOW_BITS)
#define SAFETY_SHADOW_MASK (SAFETY_SHADOW_SIZE - 1u)
#define SAFETY_SHADOW_LEAF_SHIFT (SAFETY_SHADOW_BITS + 3u)
#define SAFETY_BYTE_FIRST_BUCKETS 4096u
#define SAFETY_BYTE_LOAD 8u

#if defined(__GNUC__) && !defined(__clang__)
#define SAFETY_PLAIN_LOOPS \
  __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define SAFETY_PLAIN_LOOPS
#endif

typedef struct SafetyByte {
  uintptr_t address;
  uint64_t identity;
  struct SafetyByte *next;
} SafetyByte;

typedef struct SafetyByteBlock {
  struct SafetyByteBlock *next;
} SafetyByteBlock;

static uintptr_t g_safety_shadow[SAFETY_SHADOW_SIZE];
static const uint64_t g_safety_shadow_unmapped = METTLE_SAFETY_SHADOW_PARTIAL;
static volatile long g_safety_byte_lock;
static uint64_t g_safety_partial_words;
static SafetyByte *g_safety_byte_first[SAFETY_BYTE_FIRST_BUCKETS];
static SafetyByte **g_safety_bytes = g_safety_byte_first;
static size_t g_safety_byte_buckets = SAFETY_BYTE_FIRST_BUCKETS;
static SafetyByte *g_safety_byte_free;
static size_t g_safety_byte_count;
static SafetyByteBlock *g_safety_byte_blocks;

static void safety_bytes_acquire(void) {
  while (__atomic_exchange_n(&g_safety_byte_lock, 1L, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&g_safety_byte_lock, __ATOMIC_RELAXED)) {
#if defined(__i386__) || defined(__x86_64__)
      __builtin_ia32_pause();
#endif
    }
  }
}

static void safety_bytes_release(void) {
  __atomic_store_n(&g_safety_byte_lock, 0L, __ATOMIC_RELEASE);
}

static uintptr_t safety_shadow_install(uintptr_t *slot, size_t bytes) {
  safety_lock();
  uintptr_t table = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (!table) {
    table = (uintptr_t)safety_map(bytes);
    if (table) __atomic_store_n(slot, table, __ATOMIC_RELEASE);
  }
  safety_unlock();
  return table;
}

static uint64_t *safety_shadow_leaf(uintptr_t address, int create) {
  if ((address >> SAFETY_ADDRESS_BITS) != 0) return NULL;
  uintptr_t word = address >> 3;
  uintptr_t *top = &g_safety_shadow[(size_t)(word >> (2 * SAFETY_SHADOW_BITS)) &
                                    SAFETY_SHADOW_MASK];
  uintptr_t middle = __atomic_load_n(top, __ATOMIC_ACQUIRE);
  if (!middle) {
    if (!create) return NULL;
    middle = safety_shadow_install(top, SAFETY_SHADOW_SIZE * sizeof(uintptr_t));
    if (!middle) return NULL;
  }
  uintptr_t *entry =
      &((uintptr_t *)middle)[(size_t)(word >> SAFETY_SHADOW_BITS) & SAFETY_SHADOW_MASK];
  uintptr_t leaf = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
  if (!leaf && create) {
    leaf = safety_shadow_install(entry, SAFETY_SHADOW_SIZE * sizeof(uint64_t));
  }
  return (uint64_t *)leaf;
}

static uint64_t *safety_shadow_word(uintptr_t address, int create) {
  uint64_t *leaf = safety_shadow_leaf(address, create);
  return leaf ? &leaf[(address >> 3) & SAFETY_SHADOW_MASK] : NULL;
}

static size_t safety_byte_bucket(uintptr_t address, size_t buckets) {
  uint64_t mixed = (uint64_t)address * UINT64_C(0x9E3779B97F4A7C15);
  return (size_t)(mixed >> 20) & (buckets - 1);
}

static void safety_bytes_grow(void) {
  size_t buckets = g_safety_byte_buckets * 4;
  SafetyByte **table = (SafetyByte **)safety_map(buckets * sizeof(SafetyByte *));
  if (!table) return;
  for (size_t i = 0; i < g_safety_byte_buckets; i++) {
    SafetyByte *entry = g_safety_bytes[i];
    while (entry) {
      SafetyByte *next = entry->next;
      size_t bucket = safety_byte_bucket(entry->address, buckets);
      entry->next = table[bucket];
      table[bucket] = entry;
      entry = next;
    }
  }
  if (g_safety_bytes != g_safety_byte_first)
    safety_unmap(g_safety_bytes, g_safety_byte_buckets * sizeof(SafetyByte *));
  g_safety_bytes = table;
  g_safety_byte_buckets = buckets;
}

static SafetyByte **safety_byte_link(uintptr_t address) {
  SafetyByte **link =
      &g_safety_bytes[safety_byte_bucket(address, g_safety_byte_buckets)];
  while (*link && (*link)->address != address) link = &(*link)->next;
  return link;
}

static uint64_t safety_byte_get(uintptr_t address) {
  const SafetyByte *entry = *safety_byte_link(address);
  return entry ? entry->identity : 0;
}

static SafetyByte *safety_byte_fresh(void) {
  if (!g_safety_byte_free) {
    size_t count = (4096 - sizeof(SafetyByteBlock)) / sizeof(SafetyByte);
    SafetyByteBlock *page = (SafetyByteBlock *)safety_map(4096);
    if (!page) return NULL;
    page->next = g_safety_byte_blocks;
    g_safety_byte_blocks = page;
    SafetyByte *block = (SafetyByte *)(page + 1);
    for (size_t i = 0; i < count; i++) {
      block[i].next = g_safety_byte_free;
      g_safety_byte_free = &block[i];
    }
  }
  SafetyByte *entry = g_safety_byte_free;
  g_safety_byte_free = entry->next;
  return entry;
}

static int safety_byte_put(uintptr_t address, uint64_t identity) {
  SafetyByte **link = safety_byte_link(address);
  SafetyByte *entry = *link;
  if (!identity) {
    if (entry) {
      *link = entry->next;
      entry->next = g_safety_byte_free;
      g_safety_byte_free = entry;
      g_safety_byte_count--;
    }
    return 1;
  }
  if (!entry) {
    if (g_safety_byte_count >= g_safety_byte_buckets * SAFETY_BYTE_LOAD)
      safety_bytes_grow();
    entry = safety_byte_fresh();
    if (!entry) return 0;
    size_t bucket = safety_byte_bucket(address, g_safety_byte_buckets);
    entry->address = address;
    entry->next = g_safety_bytes[bucket];
    g_safety_bytes[bucket] = entry;
    g_safety_byte_count++;
  }
  entry->identity = identity;
  return 1;
}

static void safety_partial_words_add(int64_t change) {
  __atomic_store_n(&g_safety_partial_words,
                   g_safety_partial_words + (uint64_t)change, __ATOMIC_RELAXED);
}

static int safety_word_split(uintptr_t base, uint64_t *word) {
  uint64_t held = *word;
  if (held == METTLE_SAFETY_SHADOW_PARTIAL) return 1;
  for (uintptr_t b = 0; held && b < 8; b++) {
    if (!safety_byte_put(base + b, held)) return 0;
  }
  *word = METTLE_SAFETY_SHADOW_PARTIAL;
  safety_partial_words_add(1);
  return 1;
}

static void safety_word_settle(uintptr_t base, uint64_t *word) {
  if (*word != METTLE_SAFETY_SHADOW_PARTIAL) return;
  uint64_t first = safety_byte_get(base);
  for (uintptr_t b = 1; b < 8; b++) {
    if (safety_byte_get(base + b) != first) return;
  }
  if (first == METTLE_SAFETY_SHADOW_PARTIAL) return;
  for (uintptr_t b = 0; first && b < 8; b++) safety_byte_put(base + b, 0);
  *word = first;
  safety_partial_words_add(-1);
}

static void safety_word_drop(uintptr_t base, uint64_t *word) {
  safety_bytes_acquire();
  if (*word == METTLE_SAFETY_SHADOW_PARTIAL) {
    for (uintptr_t b = 0; b < 8; b++) safety_byte_put(base + b, 0);
    *word = 0;
    safety_partial_words_add(-1);
  }
  safety_bytes_release();
}

static uint64_t safety_byte_identity(uintptr_t address) {
  const uint64_t *word = safety_shadow_word(address, 0);
  uint64_t held = word ? __atomic_load_n(word, __ATOMIC_RELAXED) : 0;
  if (held != METTLE_SAFETY_SHADOW_PARTIAL) return held;
  safety_bytes_acquire();
  held = safety_byte_get(address);
  safety_bytes_release();
  return held;
}

static int safety_byte_store(uintptr_t address, uint64_t identity, int settle) {
  uint64_t *word = safety_shadow_word(address, identity != 0);
  if (!word) return identity == 0;
  uint64_t held = *word;
  if (held == identity && held != METTLE_SAFETY_SHADOW_PARTIAL) return 1;
  uintptr_t base = address & ~(uintptr_t)7;
  safety_bytes_acquire();
  int ok = safety_word_split(base, word) && safety_byte_put(address, identity);
  if (ok && settle) safety_word_settle(base, word);
  safety_bytes_release();
  return ok;
}

uint64_t mettle_safety_value_load(const void *slot, uint64_t value, uint64_t size) {
  (void)value;
  uintptr_t address = (uintptr_t)slot;
  if (size >= 8 && !(address & 7)) {
    const uint64_t *word = safety_shadow_word(address, 0);
    uint64_t held = word ? __atomic_load_n(word, __ATOMIC_RELAXED) : 0;
    if (held != METTLE_SAFETY_SHADOW_PARTIAL) return held;
  }
  uint64_t identity = 0;
  for (uint64_t i = 0; i < size && i < 8; i++) {
    identity = mettle_safety_merge_identity(
        identity, safety_byte_identity(address + (uintptr_t)i));
  }
  return identity;
}

void mettle_safety_value_store(void *slot, uint64_t value, uint64_t identity,
                               uint64_t size) {
  (void)value;
  uintptr_t address = (uintptr_t)slot;
  if (!size) return;
  if (size >= 8 && !(address & 7) && identity != METTLE_SAFETY_SHADOW_PARTIAL) {
    uint64_t *word = safety_shadow_word(address, identity != 0);
    if (!word) {
      if (identity) {
        safety_internal_failure("Fatal error: unable to allocate pointer safety metadata");
      }
      return;
    }
    if (*word == METTLE_SAFETY_SHADOW_PARTIAL) safety_word_drop(address, word);
    __atomic_store_n(word, identity, __ATOMIC_RELAXED);
    return;
  }
  uint64_t width = size > 8 ? 8 : size;
  for (uint64_t i = 0; i < width; i++) {
    uintptr_t at = address + (uintptr_t)i;
    if (!safety_byte_store(at, identity, i + 1 == width || (at & 7) == 7)) {
      safety_internal_failure("Fatal error: unable to allocate pointer safety metadata");
    }
  }
}

const uint64_t *mettle_safety_shadow_slot(const void *slot) {
  uintptr_t address = (uintptr_t)slot;
  SafetyRegion region;
  if (address & 7) return &g_safety_shadow_unmapped;
  const uint64_t *word = safety_shadow_word(address, 0);
  if (!word && safety_snapshot_live(address, &region)) {
    word = safety_shadow_word(address, 1);
  }
  return word ? word : &g_safety_shadow_unmapped;
}

SAFETY_PLAIN_LOOPS static void safety_words_clear(uintptr_t address,
                                                  uint64_t *words, size_t count) {
  if (!__atomic_load_n(&g_safety_partial_words, __ATOMIC_RELAXED)) {
    for (size_t k = 0; k < count; k++) words[k] = 0;
    return;
  }
  for (size_t k = 0; k < count; k++) {
    if (words[k] == METTLE_SAFETY_SHADOW_PARTIAL) {
      safety_word_drop(address + (uintptr_t)k * 8, &words[k]);
    } else {
      words[k] = 0;
    }
  }
}

static void safety_values_clear(uintptr_t address, uint64_t size) {
  const uintptr_t limit = (uintptr_t)1 << SAFETY_ADDRESS_BITS;
  if (!size || address >= limit) return;
  if (size > (uint64_t)(limit - address)) size = (uint64_t)(limit - address);
  uintptr_t end = address + (uintptr_t)size;
  while (address < end && (address & 7)) {
    safety_byte_store(address, 0, 1);
    address++;
  }
  while (end - address >= 8) {
    uintptr_t stop = address + ((end - address) & ~(uintptr_t)7);
    uintptr_t leaf_end = ((address >> SAFETY_SHADOW_LEAF_SHIFT) + 1)
                         << SAFETY_SHADOW_LEAF_SHIFT;
    if (leaf_end < stop) stop = leaf_end;
    uint64_t *leaf = safety_shadow_leaf(address, 0);
    if (leaf) {
      safety_words_clear(address, &leaf[(address >> 3) & SAFETY_SHADOW_MASK],
                         (size_t)((stop - address) >> 3));
    }
    address = stop;
  }
  while (address < end) {
    safety_byte_store(address, 0, 1);
    address++;
  }
}

static void safety_values_reset(void) {
  safety_bytes_acquire();
  for (size_t top = 0; top < SAFETY_SHADOW_SIZE; top++) {
    uintptr_t *middle = (uintptr_t *)g_safety_shadow[top];
    if (!middle) continue;
    for (size_t entry = 0; entry < SAFETY_SHADOW_SIZE; entry++) {
      safety_unmap((void *)middle[entry], SAFETY_SHADOW_SIZE * sizeof(uint64_t));
    }
    safety_unmap(middle, SAFETY_SHADOW_SIZE * sizeof(uintptr_t));
    g_safety_shadow[top] = 0;
  }
  while (g_safety_byte_blocks) {
    SafetyByteBlock *page = g_safety_byte_blocks;
    g_safety_byte_blocks = page->next;
    safety_unmap(page, 4096);
  }
  if (g_safety_bytes != g_safety_byte_first)
    safety_unmap(g_safety_bytes, g_safety_byte_buckets * sizeof(SafetyByte *));
  g_safety_bytes = g_safety_byte_first;
  g_safety_byte_buckets = SAFETY_BYTE_FIRST_BUCKETS;
  for (size_t i = 0; i < SAFETY_BYTE_FIRST_BUCKETS; i++) g_safety_bytes[i] = NULL;
  g_safety_byte_free = NULL;
  g_safety_byte_count = 0;
  __atomic_store_n(&g_safety_partial_words, UINT64_C(0), __ATOMIC_RELAXED);
  safety_bytes_release();
}

static int safety_values_copy_bytes(uintptr_t dst, uintptr_t src, uint64_t size,
                                    int backward) {
  for (uint64_t n = 0; n < size; n++) {
    uintptr_t i = (uintptr_t)(backward ? size - 1 - n : n);
    uintptr_t to = dst + i;
    int settle = n + 1 == size || (to & 7) == (backward ? 0u : 7u);
    if (!safety_byte_store(to, safety_byte_identity(src + i), settle)) return 0;
  }
  return 1;
}

static int safety_values_copy_words(uintptr_t dst, uintptr_t src, uint64_t words,
                                    int backward) {
  const uint64_t *from_leaf = NULL;
  uint64_t *to_leaf = NULL;
  uintptr_t from_key = ~(uintptr_t)0;
  uintptr_t to_key = ~(uintptr_t)0;
  for (uint64_t n = 0; n < words; n++) {
    uintptr_t i = (uintptr_t)(backward ? words - 1 - n : n) * 8;
    uintptr_t from = src + i;
    uintptr_t to = dst + i;
    if ((from >> SAFETY_SHADOW_LEAF_SHIFT) != from_key) {
      from_key = from >> SAFETY_SHADOW_LEAF_SHIFT;
      from_leaf = safety_shadow_leaf(from, 0);
    }
    if ((to >> SAFETY_SHADOW_LEAF_SHIFT) != to_key) {
      to_key = to >> SAFETY_SHADOW_LEAF_SHIFT;
      to_leaf = safety_shadow_leaf(to, 0);
    }
    uint64_t held = from_leaf ? from_leaf[(from >> 3) & SAFETY_SHADOW_MASK] : 0;
    if (!to_leaf) {
      if (!held) continue;
      to_leaf = safety_shadow_leaf(to, 1);
      if (!to_leaf) return 0;
    }
    uint64_t *into = &to_leaf[(to >> 3) & SAFETY_SHADOW_MASK];
    if (held == METTLE_SAFETY_SHADOW_PARTIAL ||
        *into == METTLE_SAFETY_SHADOW_PARTIAL) {
      if (!safety_values_copy_bytes(to, from, 8, backward)) return 0;
      continue;
    }
    *into = held;
  }
  return 1;
}

void mettle_safety_value_copy(void *destination, const void *source, uint64_t size) {
  if (!size || destination == source) return;
  uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
  if (size - 1 > UINTPTR_MAX - dst || size - 1 > UINTPTR_MAX - src)
    safety_internal_failure("Fatal error: pointer metadata copy range overflow");
  int backward = dst > src && dst - src < size;
  int ok;
  if ((dst ^ src) & 7) {
    ok = safety_values_copy_bytes(dst, src, size, backward);
  } else {
    uint64_t head = (8 - (dst & 7)) & 7;
    if (head > size) head = size;
    uint64_t words = (size - head) >> 3;
    uintptr_t body = (uintptr_t)head;
    uintptr_t rest = body + (uintptr_t)(words << 3);
    uint64_t tail = size - head - (words << 3);
    ok = backward
        ? safety_values_copy_bytes(dst + rest, src + rest, tail, 1) &&
          safety_values_copy_words(dst + body, src + body, words, 1) &&
          safety_values_copy_bytes(dst, src, head, 1)
        : safety_values_copy_bytes(dst, src, head, 0) &&
          safety_values_copy_words(dst + body, src + body, words, 0) &&
          safety_values_copy_bytes(dst + rest, src + rest, tail, 0);
  }
  if (!ok) safety_internal_failure("Fatal error: unable to store copied pointer safety metadata");
}

void mettle_safety_value_clear(void *destination, uint64_t size) {
  safety_values_clear((uintptr_t)destination, size);
}

void mettle_safety_free_identity(void *pointer, uint64_t identity) {
  if (!pointer) return;
  if (identity == SAFETY_IDENTITY_MIXED || !identity) {
    identity = mettle_safety_identity(pointer);
  }
  if (!identity) {
    safety_internal_failure("Fatal error: free has no tracked allocation identity");
  }
  SafetyRegion region = identity ? safety_identity_snapshot(identity) :
      safety_snapshot((uintptr_t)pointer);
  if (region.state != SAFETY_STATE_LIVE || !region.heap || region.start != (uintptr_t)pointer) {
    SafetyFailure failure = {region.state == SAFETY_STATE_DEAD ?
        "free of memory after it was freed" : !region.heap ?
        "free of stack or global memory" : "free of an interior pointer",
        0, 0, 0, region.size, region.size != 0, 1};
    safety_report_and_trap(&failure, __builtin_return_address(0), __builtin_frame_address(0));
  }
}

void mettle_safety_region_begin(void *pointer, int64_t size) {
  if (!pointer || size <= 0)
    safety_internal_failure("Fatal error: a foreign memory region needs an address and a positive size");
  SafetyRegion known = safety_snapshot((uintptr_t)pointer);
  if (known.state != SAFETY_STATE_LIVE || (uintptr_t)pointer != known.start ||
      (uint64_t)size != known.size)
    mettle_safety_register_static(pointer, (uint64_t)size);
  safety_lock();
  uint32_t id = safety_lookup((uintptr_t)pointer);
  SafetyRegion *region = id >= SAFETY_ID_FIRST ? safety_region(id) : NULL;
  if (region && region->start == (uintptr_t)pointer) region->foreign = 1;
  safety_unlock();
}

void mettle_safety_entry_arguments(void *vector) {
  if (!vector) return;
  char **arguments = (char **)vector;
  uint64_t count = 0;
  while (arguments[count]) count++;
  mettle_safety_register_static(vector, (count + 1) * sizeof(char *));
  for (uint64_t i = 0; i < count; i++) {
    uint64_t length = 0;
    while (arguments[i][length]) length++;
    uint64_t identity = mettle_safety_literal_identity(arguments[i], length + 1);
    mettle_safety_value_store(&arguments[i], (uint64_t)(uintptr_t)arguments[i],
                              identity, sizeof(char *));
  }
}

void mettle_safety_region_end(void *pointer) {
  mettle_safety_unregister(pointer);
}

uint64_t mettle_safety_literal_identity(const void *pointer, uint64_t size) {
  SafetyRegion known = safety_snapshot((uintptr_t)pointer);
  if (known.state == SAFETY_STATE_LIVE && (uintptr_t)pointer >= known.start &&
      size <= known.size && (uintptr_t)pointer - known.start <= known.size - size)
    return known.identity;
  safety_register_region((void *)pointer, size, 0, 1);
  return mettle_safety_identity(pointer);
}

uint64_t mettle_safety_string_identity(const void *record, uint64_t size) {
  uint64_t identity = mettle_safety_literal_identity(record, 2 * sizeof(uint64_t));
  const void *data = *(const void *const *)record;
  uint64_t data_identity = mettle_safety_literal_identity(data, size);
  mettle_safety_value_store((void *)record, (uint64_t)(uintptr_t)data, data_identity, sizeof(void *));
  return identity;
}

// The runtime's own string builders allocate behind the tracker's back. The
// compiler calls this on each result, where the record sits.
void mettle_safety_global_pointer(void *slot, uint64_t mode, uint64_t size) {
  if (mode == 3) {
    mettle_safety_string_identity(slot, size);
    return;
  }
  const void *pointer = *(const void *const *)slot;
  uint64_t identity = mode == 2 ? mettle_safety_string_identity(pointer, size) :
      mode == 1 ? mettle_safety_literal_identity(pointer, size) : mettle_safety_identity(pointer);
  mettle_safety_value_store(slot, (uint64_t)(uintptr_t)pointer, identity, sizeof(void *));
}

void mettle_safety_buffer_check(void *pointer, int64_t size, uint32_t kind,
                                uint32_t line, uint64_t identity) {
  if (size < 0) {
    SafetyFailure failure = {"invalid memory operation size", line, 0, size, 0, 0};
    safety_report_and_trap(&failure, __builtin_return_address(0), __builtin_frame_address(0));
    return;
  }
  mettle_safety_check_identity(pointer, 0, size, kind, line, identity);
}

#define SAFETY_PACKET_MIN_CAPACITY 240u
typedef struct SafetyPacket {
  struct SafetyPacket *next;
  size_t size;
  size_t capacity;
  size_t phase;
  unsigned char data[];
} SafetyPacket;

typedef struct SafetyCall {
  struct SafetyCall *previous;
  void *callee;
  uint64_t count;
  uint64_t result;
  size_t bytes;
  int entered;
  SafetyPacket *packets;
  uint64_t arguments[];
} SafetyCall;

static void safety_packets_clear(SafetyCall *frame, SafetyThread *thread) {
  while (frame->packets) {
    SafetyPacket *packet = frame->packets;
    frame->packets = packet->next;
    safety_values_clear((uintptr_t)packet->data + packet->phase, packet->size);
    packet->next = thread->spare_packets;
    thread->spare_packets = packet;
  }
}

#if defined(_WIN32)
static void safety_thread_release(SafetyThread *thread) {
  SafetyCall *lists[2] = {thread->current, thread->spare};
  for (int i = 0; i < 2; i++) {
    SafetyCall *frame = lists[i];
    while (frame) {
      SafetyCall *previous = frame->previous;
      safety_packets_clear(frame, thread);
      safety_unmap(frame, frame->bytes);
      frame = previous;
    }
  }
  while (thread->spare_packets) {
    SafetyPacket *packet = thread->spare_packets;
    thread->spare_packets = packet->next;
    safety_unmap(packet, sizeof(*packet) + packet->capacity);
  }
}
#endif

void *mettle_safety_call_push(void *callee, uint64_t count) {
  if (count > (SIZE_MAX - sizeof(SafetyCall)) / sizeof(uint64_t))
    safety_internal_failure("Fatal error: pointer safety argument count overflow");
  SafetyThread *thread = safety_thread();
  size_t bytes = sizeof(SafetyCall) + (size_t)count * sizeof(uint64_t);
  SafetyCall **link = &thread->spare;
  while (*link && (*link)->bytes < bytes) link = &(*link)->previous;
  SafetyCall *frame = *link;
  if (frame) {
    *link = frame->previous;
  } else {
    frame = (SafetyCall *)safety_map(bytes);
    if (!frame) safety_internal_failure("Fatal error: unable to allocate pointer safety call frame");
    frame->bytes = bytes;
  }
  frame->previous = thread->current;
  frame->callee = callee;
  frame->count = count;
  frame->result = 0;
  frame->entered = 0;
  frame->packets = NULL;
  for (uint64_t i = 0; i < count; i++) frame->arguments[i] = 0;
  thread->current = frame;
  return frame;
}

void *mettle_safety_call_enter(void *callee) {
  SafetyCall *frame = safety_thread()->current;
  if (!frame || frame->entered || frame->callee != callee) return NULL;
  frame->entered = 1;
  return frame;
}

void mettle_safety_call_arg(void *call, uint64_t index, uint64_t identity) {
  SafetyCall *frame = (SafetyCall *)call;
  if (!frame || index >= frame->count)
    safety_internal_failure("Fatal error: invalid pointer safety argument slot");
  frame->arguments[index] = identity;
}

uint64_t mettle_safety_call_param(void *call, uint64_t index) {
  SafetyCall *frame = (SafetyCall *)call;
  return frame && index < frame->count ? frame->arguments[index] : 0;
}

uint64_t mettle_safety_call_param_value(void *call, uint64_t index, uint64_t value) {
  SafetyCall *frame = (SafetyCall *)call;
  if (frame) return index < frame->count ? frame->arguments[index] : 0;
  return mettle_safety_identity((const void *)(uintptr_t)value);
}

void mettle_safety_call_return(void *call, uint64_t identity) {
  SafetyCall *frame = (SafetyCall *)call;
  if (frame) frame->result = identity;
}

static SafetyPacket *safety_packet_capture(SafetyCall *frame, const void *source, uint64_t size) {
  if (size > SIZE_MAX - sizeof(SafetyPacket) - 8)
    safety_internal_failure("Fatal error: aggregate safety metadata size overflow");
  SafetyThread *thread = safety_thread();
  size_t wanted = (size_t)size + 8;
  SafetyPacket **link = &thread->spare_packets;
  while (*link && (*link)->capacity < wanted) link = &(*link)->next;
  SafetyPacket *packet = *link;
  if (packet) {
    *link = packet->next;
  } else {
    size_t capacity = wanted < SAFETY_PACKET_MIN_CAPACITY
                          ? SAFETY_PACKET_MIN_CAPACITY
                          : wanted;
    packet = safety_map(sizeof(*packet) + capacity);
    if (!packet) safety_internal_failure("Fatal error: unable to allocate aggregate safety metadata");
    packet->capacity = capacity;
  }
  packet->size = (size_t)size;
  packet->phase = (size_t)((uintptr_t)source & 7);
  packet->next = frame->packets;
  frame->packets = packet;
  mettle_safety_value_copy(packet->data + packet->phase, source, size);
  return packet;
}

void mettle_safety_call_arg_copy(void *call, uint64_t index, const void *source, uint64_t size) {
  SafetyCall *frame = call;
  if (!frame || index >= frame->count)
    safety_internal_failure("Fatal error: invalid aggregate safety argument slot");
  frame->arguments[index] = (uint64_t)(uintptr_t)safety_packet_capture(frame, source, size);
}

void mettle_safety_string_fresh(void *record) {
  const void *data;
  uint64_t chars;
  SafetyRegion known;
  if (!record) return;
  data = *(const void *const *)record;
  if (!data) return;
  chars = ((const uint64_t *)record)[1] + 1;
  known = safety_snapshot((uintptr_t)data);
  if (!(known.state == SAFETY_STATE_LIVE && (uintptr_t)data >= known.start &&
        chars <= known.size && (uintptr_t)data - known.start <= known.size - chars)) {
    mettle_safety_register((void *)data, chars);
    known = safety_snapshot((uintptr_t)data);
  }
  mettle_safety_value_store(record, (uint64_t)(uintptr_t)data, known.identity,
                            sizeof(void *));
}

void mettle_safety_string_release(const void *record) {
  const void *data = record ? *(const void *const *)record : NULL;
  SafetyRegion known;
  if (!data) return;
  known = safety_snapshot((uintptr_t)data);
  if (known.state == SAFETY_STATE_LIVE && known.heap &&
      known.start == (uintptr_t)data) {
    mettle_safety_unregister((void *)data);
  }
}

uint64_t mettle_safety_mapping_identity(void *pointer, int64_t size) {
  if (!pointer || size <= 0) return 0;
  mettle_safety_region_begin(pointer, size);
  return mettle_safety_identity(pointer);
}

uint64_t mettle_safety_c_string_identity(const void *text) {
  uint64_t length = 0;
  if (!text) return 0;
  while (((const char *)text)[length]) length++;
  return mettle_safety_literal_identity(text, length + 1);
}

void mettle_safety_string_contents(void *record, uint64_t chars) {
  const void *data;
  uint64_t identity;
  if (!record) return;
  data = *(const void *const *)record;
  if (!data) return;
  identity = mettle_safety_literal_identity(data, chars);
  mettle_safety_value_store(record, (uint64_t)(uintptr_t)data, identity,
                            sizeof(void *));
}

static void safety_packet_restore(SafetyCall *frame, uint64_t token, void *destination, uint64_t size) {
  SafetyPacket *packet = frame ? frame->packets : NULL;
  while (packet && (uint64_t)(uintptr_t)packet != token) packet = packet->next;
  if (!packet || packet->size != size) {
    mettle_safety_value_clear(destination, size);
    return;
  }
  mettle_safety_value_copy(destination, packet->data + packet->phase, size);
}

void mettle_safety_call_param_copy(void *call, uint64_t index, void *destination, uint64_t size) {
  SafetyCall *frame = call;
  safety_packet_restore(frame, mettle_safety_call_param(call, index), destination, size);
}

void mettle_safety_call_return_copy(void *call, const void *source, uint64_t size) {
  SafetyCall *frame = call;
  if (frame) frame->result = (uint64_t)(uintptr_t)safety_packet_capture(frame, source, size);
}

void mettle_safety_call_result_copy(void *call, void *destination, uint64_t size) {
  SafetyCall *frame = call;
  safety_packet_restore(frame, frame ? frame->result : 0, destination, size);
}

uint64_t mettle_safety_call_pop(void *call) {
  SafetyCall *frame = (SafetyCall *)call;
  SafetyThread *thread = safety_thread();
  if (!frame || thread->current != frame)
    safety_internal_failure("Fatal error: mismatched pointer safety call frame");
  uint64_t result = frame->result;
  safety_packets_clear(frame, thread);
  thread->current = frame->previous;
  frame->previous = thread->spare;
  thread->spare = frame;
  return result;
}
