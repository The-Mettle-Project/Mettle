
#define SAFETY_IDENTITY_MIXED UINT64_MAX

uint64_t mettle_safety_identity(const void *pointer) {
  if (!pointer) return 0;
  SafetyRegion region = safety_snapshot((uintptr_t)pointer);
  return region.state == SAFETY_STATE_FREELIST ? 0 : region.identity;
}

static SafetyRegion safety_identity_snapshot(uint64_t identity) {
  SafetyRegion result = {0};
  safety_lock();
  const SafetyRegion *region = safety_region((uint32_t)(identity & 0xffffffu));
  if (region && region->identity == identity) result = *region;
  safety_unlock();
  if (result.state == SAFETY_STATE_FREELIST) result.state = SAFETY_STATE_DEAD;
  return result;
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

#define SAFETY_VALUE_FIRST_BUCKETS 4096u
#define SAFETY_VALUE_LINE_SHIFT 4u
#define SAFETY_VALUE_LOAD 8u
#define SAFETY_VALUE_COPY_INLINE 64u
typedef struct SafetyValue {
  uintptr_t address;
  uint64_t value;
  uint64_t identity;
  uint64_t owner;
  struct SafetyValue *next;
} SafetyValue;
static SafetyValue *g_safety_value_first_table[SAFETY_VALUE_FIRST_BUCKETS];
static SafetyValue **g_safety_values = g_safety_value_first_table;
static size_t g_safety_value_buckets = SAFETY_VALUE_FIRST_BUCKETS;
static SafetyValue *g_safety_value_free;
static size_t g_safety_value_count;
typedef struct SafetyValueBlock {
  struct SafetyValueBlock *next;
} SafetyValueBlock;
static SafetyValueBlock *g_safety_value_blocks;

static size_t safety_value_line_bucket(uintptr_t line, size_t buckets) {
  uint64_t mixed = (uint64_t)line * UINT64_C(0x9E3779B97F4A7C15);
  return (size_t)(mixed >> 20) & (buckets - 1);
}

static size_t safety_value_bucket(uintptr_t address) {
  return safety_value_line_bucket(address >> SAFETY_VALUE_LINE_SHIFT,
                                  g_safety_value_buckets);
}

static void safety_values_grow(void) {
  size_t buckets = g_safety_value_buckets * 4;
  SafetyValue **table = (SafetyValue **)safety_map(buckets * sizeof(SafetyValue *));
  if (!table) return;
  for (size_t i = 0; i < g_safety_value_buckets; i++) {
    SafetyValue *entry = g_safety_values[i];
    while (entry) {
      SafetyValue *next = entry->next;
      size_t bucket = safety_value_line_bucket(entry->address >> SAFETY_VALUE_LINE_SHIFT, buckets);
      entry->next = table[bucket];
      table[bucket] = entry;
      entry = next;
    }
  }
  if (g_safety_values != g_safety_value_first_table)
    safety_unmap(g_safety_values, g_safety_value_buckets * sizeof(SafetyValue *));
  g_safety_values = table;
  g_safety_value_buckets = buckets;
}

typedef int (*SafetyValueVisit)(SafetyValue **link, SafetyValue *entry, void *state);

static void safety_values_visit_bucket(size_t bucket, uintptr_t address, uint64_t size,
                                       uintptr_t line, int match_line,
                                       SafetyValueVisit visit, void *state) {
  SafetyValue **link = &g_safety_values[bucket];
  while (*link) {
    SafetyValue *entry = *link;
    int wanted = entry->address >= address && entry->address - address < size &&
                 (!match_line || entry->address >> SAFETY_VALUE_LINE_SHIFT == line);
    if (!wanted || !visit(link, entry, state)) link = &entry->next;
  }
}

static void safety_values_visit_range(uintptr_t address, uint64_t size,
                                      SafetyValueVisit visit, void *state) {
  if (!size || !g_safety_value_count) return;
  uintptr_t last = size - 1 > UINTPTR_MAX - address ? UINTPTR_MAX : address + (uintptr_t)(size - 1);
  uintptr_t first_line = address >> SAFETY_VALUE_LINE_SHIFT;
  uintptr_t last_line = last >> SAFETY_VALUE_LINE_SHIFT;
  if (last_line - first_line >= g_safety_value_buckets) {
    for (size_t bucket = 0; bucket < g_safety_value_buckets; bucket++)
      safety_values_visit_bucket(bucket, address, size, 0, 0, visit, state);
    return;
  }
  for (uintptr_t line = first_line;; line++) {
    size_t bucket = safety_value_line_bucket(line, g_safety_value_buckets);
    safety_values_visit_bucket(bucket, address, size, line, 1, visit, state);
    if (line == last_line) break;
  }
}

static uint64_t safety_owner_identity(uintptr_t address) {
  uint32_t id = safety_lookup(address);
  SafetyRegion *region = id >= SAFETY_ID_FIRST ? safety_region(id) : NULL;
  return region && address >= region->start &&
                 address - region->start < region->size
             ? region->identity
             : 0;
}

static SafetyValue *safety_value_find(uintptr_t address) {
  SafetyValue *entry = g_safety_values[safety_value_bucket(address)];
  while (entry && entry->address != address) entry = entry->next;
  return entry;
}

static int safety_value_set(uintptr_t address, uint64_t value, uint64_t identity) {
  SafetyValue *entry = safety_value_find(address);
  if (!entry) {
    if (!identity) return 1;
    if (g_safety_value_count >= g_safety_value_buckets * SAFETY_VALUE_LOAD)
      safety_values_grow();
    if (!g_safety_value_free) {
      size_t count = (4096 - sizeof(SafetyValueBlock)) / sizeof(SafetyValue);
      SafetyValueBlock *page = (SafetyValueBlock *)safety_map(4096);
      if (!page) return 0;
      page->next = g_safety_value_blocks;
      g_safety_value_blocks = page;
      SafetyValue *block = (SafetyValue *)(page + 1);
      for (size_t i = 0; i < count; i++) {
        block[i].next = g_safety_value_free;
        g_safety_value_free = &block[i];
      }
    }
    entry = g_safety_value_free;
    g_safety_value_free = entry->next;
    size_t bucket = safety_value_bucket(address);
    entry->next = g_safety_values[bucket];
    g_safety_values[bucket] = entry;
    entry->address = address;
    g_safety_value_count++;
  }
  entry->value = value;
  entry->identity = identity;
  entry->owner = safety_owner_identity(address);
  return 1;
}

uint64_t mettle_safety_value_load(const void *slot, uint64_t value, uint64_t size) {
  (void)value;
  safety_lock();
  uint64_t identity = 0;
  for (uint64_t i = 0; i < size && i < 8; i++) {
    uintptr_t address = (uintptr_t)slot + (uintptr_t)i;
    SafetyValue *entry = safety_value_find(address);
    if (entry && entry->owner == safety_owner_identity(address))
      identity = mettle_safety_merge_identity(identity, entry->identity);
  }
  safety_unlock();
  return identity;
}

void mettle_safety_value_store(void *slot, uint64_t value, uint64_t identity,
                               uint64_t size) {
  uintptr_t address = (uintptr_t)slot;
  if (!size) return;
  safety_lock();
  uint64_t width = size > 8 ? 8 : size;
  int ok = 1;
  for (uint64_t i = 0; i < width; i++) {
    if (!safety_value_set(address + (uintptr_t)i, (value >> (8 * i)) & 255, identity)) {
      ok = 0;
      break;
    }
  }
  safety_unlock();
  if (!ok) safety_internal_failure("Fatal error: unable to allocate pointer safety metadata");
}

static int safety_value_visit_clear(SafetyValue **link, SafetyValue *entry, void *state) {
  (void)state;
  *link = entry->next;
  entry->next = g_safety_value_free;
  g_safety_value_free = entry;
  g_safety_value_count--;
  return 1;
}

static void safety_values_clear_locked(uintptr_t address, uint64_t size) {
  safety_values_visit_range(address, size, safety_value_visit_clear, NULL);
}

static void safety_values_reset_locked(void) {
  while (g_safety_value_blocks) {
    SafetyValueBlock *page = g_safety_value_blocks;
    g_safety_value_blocks = page->next;
    safety_unmap(page, 4096);
  }
  if (g_safety_values != g_safety_value_first_table)
    safety_unmap(g_safety_values, g_safety_value_buckets * sizeof(SafetyValue *));
  g_safety_values = g_safety_value_first_table;
  g_safety_value_buckets = SAFETY_VALUE_FIRST_BUCKETS;
  for (size_t i = 0; i < SAFETY_VALUE_FIRST_BUCKETS; i++) g_safety_values[i] = NULL;
  g_safety_value_free = NULL;
  g_safety_value_count = 0;
}

typedef struct {
  uint64_t old_identity;
  uint64_t owner;
} SafetyValueReown;

static int safety_value_visit_reown(SafetyValue **link, SafetyValue *entry, void *state) {
  (void)link;
  SafetyValueReown *reown = (SafetyValueReown *)state;
  if (entry->owner == reown->old_identity) entry->owner = reown->owner;
  return 0;
}

static void safety_values_reown(void *pointer, uint64_t size, uint64_t old_identity) {
  safety_lock();
  uintptr_t address = (uintptr_t)pointer;
  SafetyValueReown reown = {old_identity, safety_owner_identity(address)};
  safety_values_visit_range(address, size, safety_value_visit_reown, &reown);
  safety_unlock();
}

typedef struct {
  SafetyValue *copy;
  size_t count;
} SafetyValueCapture;

static int safety_value_visit_capture(SafetyValue **link, SafetyValue *entry, void *state) {
  (void)link;
  SafetyValueCapture *capture = (SafetyValueCapture *)state;
  if (entry->identity && entry->owner == safety_owner_identity(entry->address)) {
    if (capture->copy) capture->copy[capture->count] = *entry;
    capture->count++;
  }
  return 0;
}

void mettle_safety_value_copy(void *destination, const void *source, uint64_t size) {
  if (!size || destination == source) return;
  uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
  if (size - 1 > UINTPTR_MAX - dst || size - 1 > UINTPTR_MAX - src)
    safety_internal_failure("Fatal error: pointer metadata copy range overflow");
  safety_lock();
  SafetyValueCapture capture = {NULL, 0};
  safety_values_visit_range(src, size, safety_value_visit_capture, &capture);
  size_t count = capture.count;
  SafetyValue inline_copy[SAFETY_VALUE_COPY_INLINE];
  SafetyValue *copy = !count ? NULL : count <= SAFETY_VALUE_COPY_INLINE ? inline_copy :
      (SafetyValue *)safety_map(count * sizeof(*copy));
  if (count && !copy) {
    safety_unlock();
    safety_internal_failure("Fatal error: unable to copy pointer safety metadata");
  }
  capture.copy = copy;
  capture.count = 0;
  safety_values_visit_range(src, size, safety_value_visit_capture, &capture);
  safety_values_clear_locked(dst, size);
  int ok = 1;
  for (size_t i = 0; i < count; i++) {
    if (!safety_value_set(dst + (copy[i].address - src), copy[i].value, copy[i].identity)) {
      ok = 0;
      break;
    }
  }
  safety_unlock();
  if (copy != inline_copy) safety_unmap(copy, count * sizeof(*copy));
  if (!ok) safety_internal_failure("Fatal error: unable to store copied pointer safety metadata");
}

void mettle_safety_value_clear(void *destination, uint64_t size) {
  if (!size) return;
  safety_lock();
  safety_values_clear_locked((uintptr_t)destination, size);
  safety_unlock();
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
  mettle_safety_register_static((void *)pointer, size);
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
typedef struct {
  SafetyCall *current;
  SafetyCall *spare;
  SafetyPacket *spare_packets;
} SafetyCalls;

static void safety_packets_clear(SafetyCall *frame, SafetyCalls *calls) {
  while (frame->packets) {
    SafetyPacket *packet = frame->packets;
    frame->packets = packet->next;
    mettle_safety_value_clear(packet->data, packet->size);
    packet->next = calls->spare_packets;
    calls->spare_packets = packet;
  }
}

static void safety_packets_release(SafetyCalls *calls) {
  while (calls->spare_packets) {
    SafetyPacket *packet = calls->spare_packets;
    calls->spare_packets = packet->next;
    safety_unmap(packet, sizeof(*packet) + packet->capacity);
  }
}

#if defined(_WIN32)
static DWORD g_safety_calls_slot = FLS_OUT_OF_INDEXES;
static void NTAPI safety_calls_destroy(void *value) {
  SafetyCalls *calls = (SafetyCalls *)value;
  if (!calls) return;
  SafetyCall *lists[2] = {calls->current, calls->spare};
  for (int i = 0; i < 2; i++) {
    SafetyCall *frame = lists[i];
    while (frame) {
      SafetyCall *previous = frame->previous;
      safety_packets_clear(frame, calls);
      safety_unmap(frame, frame->bytes);
      frame = previous;
    }
  }
  safety_packets_release(calls);
  safety_unmap(calls, sizeof(*calls));
}
static SafetyCalls *safety_calls(void) {
  DWORD last_error = GetLastError();
  DWORD slot = __atomic_load_n(&g_safety_calls_slot, __ATOMIC_ACQUIRE);
  if (slot == FLS_OUT_OF_INDEXES) {
    safety_lock();
    slot = g_safety_calls_slot;
    if (slot == FLS_OUT_OF_INDEXES) {
      slot = FlsAlloc(safety_calls_destroy);
      __atomic_store_n(&g_safety_calls_slot, slot, __ATOMIC_RELEASE);
    }
    safety_unlock();
    if (slot == FLS_OUT_OF_INDEXES)
      safety_internal_failure("Fatal error: unable to create pointer safety thread state");
  }
  SafetyCalls *calls = (SafetyCalls *)FlsGetValue(slot);
  if (!calls) {
    calls = (SafetyCalls *)safety_map(sizeof(*calls));
    if (!calls || !FlsSetValue(slot, calls))
      safety_internal_failure("Fatal error: unable to allocate pointer safety thread state");
  }
  SetLastError(last_error);
  return calls;
}
#else
static __thread SafetyCalls g_safety_calls;
static SafetyCalls *safety_calls(void) { return &g_safety_calls; }
#endif

void *mettle_safety_call_push(void *callee, uint64_t count) {
  if (count > (SIZE_MAX - sizeof(SafetyCall)) / sizeof(uint64_t))
    safety_internal_failure("Fatal error: pointer safety argument count overflow");
  SafetyCalls *calls = safety_calls();
  size_t bytes = sizeof(SafetyCall) + (size_t)count * sizeof(uint64_t);
  SafetyCall **link = &calls->spare;
  while (*link && (*link)->bytes < bytes) link = &(*link)->previous;
  SafetyCall *frame = *link;
  if (frame) {
    *link = frame->previous;
  } else {
    frame = (SafetyCall *)safety_map(bytes);
    if (!frame) safety_internal_failure("Fatal error: unable to allocate pointer safety call frame");
    frame->bytes = bytes;
  }
  frame->previous = calls->current;
  frame->callee = callee;
  frame->count = count;
  frame->result = 0;
  frame->entered = 0;
  frame->packets = NULL;
  for (uint64_t i = 0; i < count; i++) frame->arguments[i] = 0;
  calls->current = frame;
  return frame;
}

void *mettle_safety_call_enter(void *callee) {
  SafetyCall *frame = safety_calls()->current;
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
  if (size > SIZE_MAX - sizeof(SafetyPacket))
    safety_internal_failure("Fatal error: aggregate safety metadata size overflow");
  SafetyCalls *calls = safety_calls();
  SafetyPacket **link = &calls->spare_packets;
  while (*link && (*link)->capacity < (size_t)size) link = &(*link)->next;
  SafetyPacket *packet = *link;
  if (packet) {
    *link = packet->next;
  } else {
    size_t capacity = (size_t)size < SAFETY_PACKET_MIN_CAPACITY
                          ? SAFETY_PACKET_MIN_CAPACITY
                          : (size_t)size;
    packet = safety_map(sizeof(*packet) + capacity);
    if (!packet) safety_internal_failure("Fatal error: unable to allocate aggregate safety metadata");
    packet->capacity = capacity;
  }
  packet->size = (size_t)size;
  packet->next = frame->packets;
  frame->packets = packet;
  mettle_safety_value_copy(packet->data, source, size);
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
  mettle_safety_value_copy(destination, packet->data, size);
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
  SafetyCalls *calls = safety_calls();
  if (!frame || calls->current != frame)
    safety_internal_failure("Fatal error: mismatched pointer safety call frame");
  uint64_t result = frame->result;
  safety_packets_clear(frame, calls);
  calls->current = frame->previous;
  frame->previous = calls->spare;
  calls->spare = frame;
  return result;
}
