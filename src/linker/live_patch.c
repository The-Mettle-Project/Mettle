#include "linker/live_patch.h"

#include "../common.h"
#include "linker/link_object.h"
#include "linker/linker_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define LIVE_NONE ((size_t)-1)
#define LIVE_FNV_BASIS 1469598103934665603ULL
#define LIVE_FNV_PRIME 1099511628211ULL

#define LIVE_RESUME_SUFFIX "$resume"

typedef struct {
  char *key;
  uint64_t a;
  uint64_t b;
} LiveEntry;

typedef struct {
  LiveEntry *items;
  size_t capacity;
  size_t count;
} LiveMap;

typedef struct {
  uint64_t value;
  size_t symbol;
} LiveNamed;

typedef struct {
  char **names;
  char **texts;
  size_t count;
} LiveTexts;

typedef struct {
  char *name;
  size_t section;
  uint64_t thunk_offset;
  uint64_t body_offset;
  uint64_t end;
  uint64_t signature;
  int entry;
} LiveFunction;

typedef struct {
  const LinkObject *object;
  LiveNamed **named;
  size_t *named_count;
  size_t *section_function;
  LiveFunction *functions;
  size_t function_count;
  LiveMap function_by_name;
  LiveMap symbol_by_name;
  const LinkReloc ***reloc_order;
} LiveObjectIndex;

typedef enum {
  LIVE_T_BAD = 0,
  LIVE_T_THUNK,
  LIVE_T_BODY,
  LIVE_T_GLOBAL,
  LIVE_T_RODATA,
  LIVE_T_EXTERN,
  LIVE_T_TEXT
} LiveTargetKind;

typedef struct {
  LiveTargetKind kind;
  const char *name;
  size_t section;
  uint64_t loc;
  int64_t delta;
  size_t function;
} LiveTarget;

typedef struct {
  unsigned char *data;
  size_t size;
  size_t capacity;
} LiveBuffer;

typedef struct {
  size_t section;
  uint64_t start;
  uint64_t end;
  uint64_t blob_offset;
  int is_init;
  uint64_t host_rva;
} LivePiece;

typedef struct {
  uint32_t site;
  uint32_t kind;
  uint64_t target;
  int64_t addend;
} LiveFixup;

typedef struct {
  uint64_t slot_rva;
  uint64_t body_offset;
} LiveSwap;

typedef struct {
  uint64_t rva;
  uint64_t blob_offset;
  uint64_t size;
} LiveInit;

typedef struct {
  uint64_t image_base;
  uint64_t anchor_rva;
  uint64_t spare_rva;
  uint64_t spare_count;
  uint64_t spare_used;
  uint64_t arena_rva;
  uint64_t arena_size;
  uint64_t arena_used;
  unsigned patch_index;
  LiveMap symbols;
  LiveMap hashes;
  LiveMap sizes;
  LiveMap spares;
  LiveMap signatures;
  unsigned *applied;
  size_t applied_count;
  LiveTexts layouts;
  LiveTexts types;
} LiveState;

static int live_state_push_applied(LiveState *state, unsigned index) {
  unsigned *grown = realloc(state->applied,
                            (state->applied_count + 1u) * sizeof(unsigned));
  if (!grown) {
    return 0;
  }
  state->applied = grown;
  state->applied[state->applied_count++] = index;
  return 1;
}

static void live_texts_free(LiveTexts *texts) {
  for (size_t i = 0; i < texts->count; i++) {
    free(texts->names[i]);
    free(texts->texts[i]);
  }
  free(texts->names);
  free(texts->texts);
  texts->names = NULL;
  texts->texts = NULL;
  texts->count = 0;
}

static int live_texts_set(LiveTexts *texts, const char *name,
                          const char *text) {
  char **names;
  char **values;
  for (size_t i = 0; i < texts->count; i++) {
    if (strcmp(texts->names[i], name) == 0) {
      char *copy = mettle_strdup(text);
      if (!copy) {
        return 0;
      }
      free(texts->texts[i]);
      texts->texts[i] = copy;
      return 1;
    }
  }
  names = realloc(texts->names, (texts->count + 1u) * sizeof(char *));
  if (!names) {
    return 0;
  }
  texts->names = names;
  values = realloc(texts->texts, (texts->count + 1u) * sizeof(char *));
  if (!values) {
    return 0;
  }
  texts->texts = values;
  texts->names[texts->count] = mettle_strdup(name);
  texts->texts[texts->count] = mettle_strdup(text);
  if (!texts->names[texts->count] || !texts->texts[texts->count]) {
    return 0;
  }
  texts->count++;
  return 1;
}

static const char *live_texts_get(const LiveTexts *texts, const char *name) {
  for (size_t i = 0; i < texts->count; i++) {
    if (strcmp(texts->names[i], name) == 0) {
      return texts->texts[i];
    }
  }
  return NULL;
}

static void live_texts_replace(LiveTexts *texts, const char *const *names,
                               const char *const *values, size_t count) {
  live_texts_free(texts);
  for (size_t i = 0; i < count; i++) {
    if (!live_texts_set(texts, names[i], values[i])) {
      return;
    }
  }
}

static uint64_t g_live_patch_tick = 0;

void live_set_patch_tick(uint64_t tick) { g_live_patch_tick = tick; }

static LiveTexts g_live_layouts = {0};
static LiveTexts g_live_types = {0};

void live_set_global_layouts(const char *const *names,
                             const char *const *layouts, size_t count) {
  live_texts_replace(&g_live_layouts, names, layouts, count);
}

void live_set_type_layouts(const char *const *names,
                           const char *const *layouts, size_t count) {
  live_texts_replace(&g_live_types, names, layouts, count);
}

static const char *live_registry_layout(const char *name) {
  return live_texts_get(&g_live_layouts, name);
}

static int live_state_set_layout(LiveState *state, const char *name,
                                 const char *layout) {
  return live_texts_set(&state->layouts, name, layout);
}

static const char *live_state_layout(const LiveState *state,
                                     const char *name) {
  return live_texts_get(&state->layouts, name);
}

static int live_copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  FILE *out = NULL;
  char buffer[65536];
  size_t got;
  int ok = in != NULL;
  if (ok) {
    out = fopen(to, "wb");
    ok = out != NULL;
  }
  while (ok && (got = fread(buffer, 1u, sizeof(buffer), in)) > 0u) {
    ok = fwrite(buffer, 1u, got, out) == got;
  }
  if (in) {
    fclose(in);
  }
  if (out && fclose(out) != 0) {
    ok = 0;
  }
  return ok;
}

static uint64_t live_text_hash(const char *text) {
  uint64_t hash = LIVE_FNV_BASIS;
  while (*text) {
    hash ^= (unsigned char)*text++;
    hash *= LIVE_FNV_PRIME;
  }
  return hash;
}

static void live_hash_bytes(uint64_t *hash, const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  for (size_t i = 0; i < size; i++) {
    *hash ^= bytes[i];
    *hash *= LIVE_FNV_PRIME;
  }
}

static void live_hash_u64(uint64_t *hash, uint64_t value) {
  live_hash_bytes(hash, &value, sizeof(value));
}

static void live_hash_text(uint64_t *hash, const char *text) {
  live_hash_bytes(hash, text, strlen(text) + 1u);
}

static void live_map_destroy(LiveMap *map) {
  for (size_t i = 0; i < map->capacity; i++) {
    free(map->items[i].key);
  }
  free(map->items);
  memset(map, 0, sizeof(*map));
}

static LiveEntry *live_map_find(const LiveMap *map, const char *key) {
  if (!map->capacity) {
    return NULL;
  }
  size_t mask = map->capacity - 1u;
  size_t at = (size_t)live_text_hash(key) & mask;
  while (map->items[at].key) {
    if (strcmp(map->items[at].key, key) == 0) {
      return &map->items[at];
    }
    at = (at + 1u) & mask;
  }
  return NULL;
}

static int live_map_grow(LiveMap *map) {
  size_t capacity = map->capacity ? map->capacity * 2u : 64u;
  LiveEntry *items = calloc(capacity, sizeof(LiveEntry));
  if (!items) {
    return 0;
  }
  for (size_t i = 0; i < map->capacity; i++) {
    if (map->items[i].key) {
      size_t at = (size_t)live_text_hash(map->items[i].key) & (capacity - 1u);
      while (items[at].key) {
        at = (at + 1u) & (capacity - 1u);
      }
      items[at] = map->items[i];
    }
  }
  free(map->items);
  map->items = items;
  map->capacity = capacity;
  return 1;
}

static LiveEntry *live_map_put(LiveMap *map, const char *key, uint64_t a,
                               uint64_t b) {
  LiveEntry *entry = live_map_find(map, key);
  if (entry) {
    entry->a = a;
    entry->b = b;
    return entry;
  }
  if ((map->count + 1u) * 2u > map->capacity && !live_map_grow(map)) {
    return NULL;
  }
  size_t mask = map->capacity - 1u;
  size_t at = (size_t)live_text_hash(key) & mask;
  while (map->items[at].key) {
    at = (at + 1u) & mask;
  }
  map->items[at].key = mettle_strdup(key);
  if (!map->items[at].key) {
    return NULL;
  }
  map->items[at].a = a;
  map->items[at].b = b;
  map->count++;
  return &map->items[at];
}

static int live_buffer_append(LiveBuffer *buffer, const void *data,
                              size_t size) {
  if (buffer->size + size > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < buffer->size + size) {
      capacity *= 2u;
    }
    unsigned char *grown = realloc(buffer->data, capacity);
    if (!grown) {
      return 0;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
  }
  if (data) {
    memcpy(buffer->data + buffer->size, data, size);
  } else {
    memset(buffer->data + buffer->size, 0, size);
  }
  buffer->size += size;
  return 1;
}

static int live_buffer_align(LiveBuffer *buffer, size_t alignment,
                             unsigned char fill) {
  while (buffer->size % alignment) {
    if (!live_buffer_append(buffer, &fill, 1u)) {
      return 0;
    }
  }
  return 1;
}

static int live_buffer_u32(LiveBuffer *buffer, uint32_t value) {
  unsigned char bytes[4];
  linker_write_u32(bytes, value);
  return live_buffer_append(buffer, bytes, sizeof(bytes));
}

static int live_buffer_u64(LiveBuffer *buffer, uint64_t value) {
  return live_buffer_u32(buffer, (uint32_t)value) &&
         live_buffer_u32(buffer, (uint32_t)(value >> 32));
}

static uint64_t live_section_size(const LinkSection *section) {
  return section->size_of_raw_data > section->virtual_size
             ? section->size_of_raw_data
             : section->virtual_size;
}

static int live_symbol_is_named(const LinkSymbol *symbol) {
  return symbol->name && symbol->name[0] != '\0' && symbol->name[0] != '.' &&
         !symbol->is_auxiliary && symbol->is_defined &&
         symbol->section_index >= 0;
}

static int live_named_compare(const void *left, const void *right) {
  const LiveNamed *a = (const LiveNamed *)left;
  const LiveNamed *b = (const LiveNamed *)right;
  if (a->value != b->value) {
    return a->value < b->value ? -1 : 1;
  }
  return a->symbol < b->symbol ? -1 : (a->symbol > b->symbol ? 1 : 0);
}

static int live_name_has_suffix(const char *name, const char *suffix) {
  size_t length = strlen(name);
  size_t suffix_length = strlen(suffix);
  return length > suffix_length &&
         strcmp(name + length - suffix_length, suffix) == 0;
}

static void live_index_destroy(LiveObjectIndex *index) {
  if (index->object) {
    for (size_t i = 0; i < index->object->section_count; i++) {
      free(index->named ? index->named[i] : NULL);
    }
  }
  for (size_t i = 0; i < index->function_count; i++) {
    free(index->functions[i].name);
  }
  free(index->named);
  free(index->named_count);
  free(index->section_function);
  free(index->functions);
  live_map_destroy(&index->function_by_name);
  live_map_destroy(&index->symbol_by_name);
  for (size_t i = 0; index->reloc_order && i < index->object->section_count;
       i++) {
    free(index->reloc_order[i]);
  }
  free(index->reloc_order);
  memset(index, 0, sizeof(*index));
}

static int live_reloc_compare(const void *left, const void *right) {
  const LinkReloc *a = *(const LinkReloc *const *)left;
  const LinkReloc *b = *(const LinkReloc *const *)right;
  if (a->offset != b->offset) {
    return a->offset < b->offset ? -1 : 1;
  }
  return a < b ? -1 : (a > b ? 1 : 0);
}

static size_t live_first_reloc(const LiveObjectIndex *index, size_t section,
                               uint64_t start) {
  const LinkReloc **order = index->reloc_order[section];
  size_t low = 0;
  size_t high = index->object->sections[section].relocation_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2u;
    if (order[mid]->offset < start) {
      low = mid + 1u;
    } else {
      high = mid;
    }
  }
  return low;
}

static const LinkSymbol *live_find_symbol(const LiveObjectIndex *index,
                                          const char *name) {
  LiveEntry *entry = name ? live_map_find(&index->symbol_by_name, name) : NULL;
  return entry ? &index->object->symbols[entry->a] : NULL;
}

static int live_index_build(LiveObjectIndex *index, const LinkObject *object) {
  size_t sections = object->section_count;
  memset(index, 0, sizeof(*index));
  index->object = object;
  index->named = calloc(sections ? sections : 1u, sizeof(LiveNamed *));
  index->named_count = calloc(sections ? sections : 1u, sizeof(size_t));
  index->section_function = malloc((sections ? sections : 1u) * sizeof(size_t));
  if (!index->named || !index->named_count || !index->section_function) {
    return 0;
  }
  for (size_t i = 0; i < sections; i++) {
    index->section_function[i] = LIVE_NONE;
  }
  for (size_t i = 0; i < object->symbol_count; i++) {
    const LinkSymbol *symbol = &object->symbols[i];
    if (live_symbol_is_named(symbol) &&
        (size_t)symbol->section_index < sections) {
      index->named_count[symbol->section_index]++;
    }
  }
  for (size_t i = 0; i < sections; i++) {
    if (index->named_count[i]) {
      index->named[i] = calloc(index->named_count[i], sizeof(LiveNamed));
      if (!index->named[i]) {
        return 0;
      }
      index->named_count[i] = 0;
    }
  }
  for (size_t i = 0; i < object->symbol_count; i++) {
    const LinkSymbol *symbol = &object->symbols[i];
    if (live_symbol_is_named(symbol) &&
        (size_t)symbol->section_index < sections) {
      size_t section = (size_t)symbol->section_index;
      index->named[section][index->named_count[section]].value = symbol->value;
      index->named[section][index->named_count[section]].symbol = i;
      index->named_count[section]++;
    }
  }
  for (size_t i = 0; i < sections; i++) {
    if (index->named_count[i] > 1u) {
      qsort(index->named[i], index->named_count[i], sizeof(LiveNamed),
            live_named_compare);
    }
  }
  for (size_t i = 0; i < object->symbol_count; i++) {
    const char *name = object->symbols[i].name;
    if (name && !live_map_find(&index->symbol_by_name, name) &&
        !live_map_put(&index->symbol_by_name, name, i, 0)) {
      return 0;
    }
  }
  index->reloc_order = calloc(sections ? sections : 1u, sizeof(*index->reloc_order));
  if (!index->reloc_order) {
    return 0;
  }
  for (size_t i = 0; i < sections; i++) {
    const LinkSection *section = &object->sections[i];
    index->reloc_order[i] =
        malloc((section->relocation_count ? section->relocation_count : 1u) *
               sizeof(const LinkReloc *));
    if (!index->reloc_order[i]) {
      return 0;
    }
    for (size_t r = 0; r < section->relocation_count; r++) {
      index->reloc_order[i][r] = &section->relocations[r];
    }
    qsort(index->reloc_order[i], section->relocation_count,
          sizeof(const LinkReloc *), live_reloc_compare);
  }
  for (size_t i = 0; i < object->symbol_count; i++) {
    const LinkSymbol *symbol = &object->symbols[i];
    if (!live_symbol_is_named(symbol) ||
        !live_name_has_suffix(symbol->name, "$body") ||
        (size_t)symbol->section_index >= sections ||
        object->sections[symbol->section_index].kind !=
            LINK_SECTION_KIND_TEXT) {
      continue;
    }
    size_t section = (size_t)symbol->section_index;
    size_t base_length = strlen(symbol->name) - 5u;
    LiveFunction *grown = realloc(index->functions,
                                  (index->function_count + 1u) *
                                      sizeof(LiveFunction));
    if (!grown) {
      return 0;
    }
    index->functions = grown;
    LiveFunction *function = &index->functions[index->function_count];
    memset(function, 0, sizeof(*function));
    function->name = malloc(base_length + 1u);
    if (!function->name) {
      return 0;
    }
    memcpy(function->name, symbol->name, base_length);
    function->name[base_length] = '\0';
    function->section = section;
    function->body_offset = symbol->value;
    function->thunk_offset = symbol->value;
    function->end = live_section_size(&object->sections[section]);
    const LinkSymbol *thunk = live_find_symbol(index, function->name);
    if (thunk && thunk->section_index == (int64_t)section) {
      function->thunk_offset = thunk->value;
    }
    if (object->sections[section].raw_data &&
        function->body_offset >= function->thunk_offset + 16u) {
      function->signature = linker_read_u64(object->sections[section].raw_data +
                                            function->thunk_offset + 8u);
    } else if (thunk && thunk->value == function->body_offset &&
               function->body_offset >= 8u) {
      function->entry = 1;
      if (object->sections[section].raw_data) {
        function->signature = linker_read_u64(
            object->sections[section].raw_data + function->body_offset - 8u);
      }
    }
    index->section_function[section] = index->function_count;
    if (!live_map_put(&index->function_by_name, function->name,
                      index->function_count, 0)) {
      return 0;
    }
    index->function_count++;
  }
  return 1;
}

static const LinkSymbol *live_covering_symbol(const LiveObjectIndex *index,
                                              size_t section, uint64_t loc,
                                              uint64_t *next_out) {
  const LinkSymbol *found = NULL;
  uint64_t start = 0;
  uint64_t next = live_section_size(&index->object->sections[section]);
  for (size_t i = 0; i < index->named_count[section]; i++) {
    const LiveNamed *named = &index->named[section][i];
    if (named->value <= loc) {
      found = &index->object->symbols[named->symbol];
      start = named->value;
    }
  }
  for (size_t i = 0; i < index->named_count[section]; i++) {
    const LiveNamed *named = &index->named[section][i];
    if (named->value > start && named->value < next &&
        (named->value > loc || !found)) {
      next = named->value;
      break;
    }
  }
  if (next_out) {
    *next_out = next;
  }
  return found;
}

static const LinkSymbol *live_covering_external(const LiveObjectIndex *index,
                                                size_t section, uint64_t loc) {
  const LinkSymbol *found = NULL;
  for (size_t i = 0; i < index->named_count[section]; i++) {
    const LiveNamed *named = &index->named[section][i];
    const LinkSymbol *symbol = &index->object->symbols[named->symbol];
    if (named->value <= loc && symbol->is_external) {
      found = symbol;
    }
  }
  return found ? found : live_covering_symbol(index, section, loc, NULL);
}

static int64_t live_reloc_addend(const LinkSection *section,
                                 const LinkReloc *relocation) {
  if (relocation->addend_is_explicit) {
    return relocation->kind == LINK_RELOC_PC32 ? relocation->addend + 4
                                               : relocation->addend;
  }
  if (!section->raw_data) {
    return 0;
  }
  if (relocation->kind == LINK_RELOC_ABS64) {
    return (int64_t)linker_read_u64(section->raw_data + relocation->offset);
  }
  return (int64_t)(int32_t)linker_read_u32(section->raw_data +
                                           relocation->offset);
}

static size_t live_reloc_width(const LinkReloc *relocation) {
  return relocation->kind == LINK_RELOC_ABS64 ? 8u : 4u;
}

static LiveTarget live_classify(const LiveObjectIndex *index,
                                uint32_t symbol_index, int64_t addend) {
  LiveTarget target;
  const LinkObject *object = index->object;
  memset(&target, 0, sizeof(target));
  target.function = LIVE_NONE;
  if (symbol_index >= object->symbol_count) {
    return target;
  }
  const LinkSymbol *symbol = &object->symbols[symbol_index];
  if (!symbol->is_defined ||
      symbol->section_index == LINK_SECTION_INDEX_UNDEFINED) {
    target.kind = symbol->name ? LIVE_T_EXTERN : LIVE_T_BAD;
    target.name = symbol->name;
    target.delta = addend;
    return target;
  }
  if (symbol->section_index < 0 ||
      (size_t)symbol->section_index >= object->section_count) {
    return target;
  }
  size_t section = (size_t)symbol->section_index;
  uint64_t loc = symbol->value + (uint64_t)addend;
  target.section = section;
  target.loc = loc;
  switch (object->sections[section].kind) {
  case LINK_SECTION_KIND_TEXT: {
    size_t function_index = index->section_function[section];
    if (function_index != LIVE_NONE) {
      const LiveFunction *function = &index->functions[function_index];
      target.name = function->name;
      target.function = function_index;
      if (loc < function->body_offset ||
          (function->entry && loc == function->body_offset)) {
        target.kind = LIVE_T_THUNK;
        target.delta = (int64_t)(loc - function->thunk_offset);
      } else {
        target.kind = LIVE_T_BODY;
        target.delta = (int64_t)(loc - function->body_offset);
      }
      return target;
    }
    const LinkSymbol *covering = live_covering_external(index, section, loc);
    if (covering) {
      target.kind = LIVE_T_TEXT;
      target.name = covering->name;
      target.delta = (int64_t)(loc - covering->value);
    }
    return target;
  }
  case LINK_SECTION_KIND_DATA:
  case LINK_SECTION_KIND_BSS: {
    const LinkSymbol *covering = live_covering_symbol(index, section, loc, NULL);
    if (covering) {
      target.kind = LIVE_T_GLOBAL;
      target.name = covering->name;
      target.delta = (int64_t)(loc - covering->value);
    }
    return target;
  }
  case LINK_SECTION_KIND_RDATA:
    target.kind = LIVE_T_RODATA;
    return target;
  default:
    return target;
  }
}

static uint64_t live_hash_region(const LiveObjectIndex *index, size_t section,
                                 uint64_t start, uint64_t end,
                                 size_t self_function, int depth) {
  const LinkSection *source = &index->object->sections[section];
  uint64_t hash = LIVE_FNV_BASIS;
  size_t size = (size_t)(end > start ? end - start : 0u);
  unsigned char *copy = NULL;

  live_hash_u64(&hash, size);
  if (source->raw_data && size) {
    copy = malloc(size);
    if (!copy) {
      return 0;
    }
    memcpy(copy, source->raw_data + start, size);
    for (size_t i = live_first_reloc(index, section, start);
         i < source->relocation_count &&
         index->reloc_order[section][i]->offset < end;
         i++) {
      const LinkReloc *relocation = index->reloc_order[section][i];
      size_t width = live_reloc_width(relocation);
      size_t at = (size_t)(relocation->offset - start);
      memset(copy + at, 0, at + width <= size ? width : size - at);
    }
    live_hash_bytes(&hash, copy, size);
    free(copy);
  }
  for (size_t i = live_first_reloc(index, section, start);
       i < source->relocation_count &&
       index->reloc_order[section][i]->offset < end;
       i++) {
    const LinkReloc *relocation = index->reloc_order[section][i];
    int64_t addend = live_reloc_addend(source, relocation);
    LiveTarget target =
        live_classify(index, relocation->symbol_index, addend);
    live_hash_u64(&hash, relocation->offset - start);
    live_hash_u64(&hash, (uint64_t)relocation->kind);
    live_hash_u64(&hash, (uint64_t)target.kind);
    switch (target.kind) {
    case LIVE_T_BODY:
      if (target.function == self_function) {
        live_hash_text(&hash, "$self");
      } else {
        live_hash_text(&hash, target.name);
      }
      live_hash_u64(&hash, (uint64_t)target.delta);
      break;
    case LIVE_T_THUNK:
    case LIVE_T_GLOBAL:
    case LIVE_T_EXTERN:
    case LIVE_T_TEXT:
      live_hash_text(&hash, target.name ? target.name : "");
      live_hash_u64(&hash, (uint64_t)target.delta);
      break;
    case LIVE_T_RODATA: {
      uint64_t region_end = 0;
      const LinkSymbol *covering =
          live_covering_symbol(index, target.section, target.loc, &region_end);
      uint64_t region_start = covering ? covering->value : 0u;
      if (depth < 8) {
        live_hash_u64(&hash,
                      live_hash_region(index, target.section, region_start,
                                       region_end, LIVE_NONE, depth + 1));
      } else {
        live_hash_u64(&hash, live_text_hash(index->object->sections[target.section].name
                                                ? index->object->sections[target.section].name
                                                : ""));
      }
      live_hash_u64(&hash, target.loc - region_start);
      break;
    }
    default:
      live_hash_u64(&hash, relocation->symbol_index);
      break;
    }
  }
  return hash;
}

static int live_function_holds_quiesce(const LiveObjectIndex *index,
                                       size_t function_index) {
  const LiveFunction *function = &index->functions[function_index];
  const LinkSection *source = &index->object->sections[function->section];
  for (size_t i = 0; i < source->relocation_count; i++) {
    const LinkReloc *relocation = &source->relocations[i];
    const LinkSymbol *symbol = NULL;
    if (relocation->offset < function->body_offset ||
        relocation->symbol_index >= index->object->symbol_count) {
      continue;
    }
    symbol = &index->object->symbols[relocation->symbol_index];
    if (symbol->name && (strcmp(symbol->name, "mettle_live_quiesce") == 0 ||
                         strcmp(symbol->name, "mettle_live_frame_check") == 0 ||
                         strcmp(symbol->name, "mettle_live_site_check") == 0 ||
                         strcmp(symbol->name, "mettle_live_frame_moved") == 0)) {
      return 1;
    }
  }
  return 0;
}

static size_t live_family_length(const char *name) {
  size_t length = strlen(name);
  size_t suffix = strlen(LIVE_RESUME_SUFFIX);
  if (length > suffix &&
      strcmp(name + length - suffix, LIVE_RESUME_SUFFIX) == 0) {
    return length - suffix;
  }
  return length;
}

static int live_is_continuation(const char *name) {
  return live_family_length(name) != strlen(name);
}

static int live_same_family(const char *a, const char *b) {
  size_t length = live_family_length(a);
  return length == live_family_length(b) && strncmp(a, b, length) == 0;
}

static size_t live_family_owner(const LiveObjectIndex *index, size_t i) {
  const char *name = index->functions[i].name;
  size_t length = live_family_length(name);
  char owner[1024];
  LiveEntry *entry;
  if (length == strlen(name) || length >= sizeof(owner)) {
    return i;
  }
  memcpy(owner, name, length);
  owner[length] = '\0';
  entry = live_map_find(&index->function_by_name, owner);
  return entry ? (size_t)entry->a : i;
}

static size_t live_continuation_of(const LiveObjectIndex *index, size_t i) {
  char name[1024];
  LiveEntry *entry;
  snprintf(name, sizeof(name), "%s" LIVE_RESUME_SUFFIX,
           index->functions[i].name);
  entry = live_map_find(&index->function_by_name, name);
  return entry ? (size_t)entry->a : LIVE_NONE;
}

static int live_function_callees(const LiveObjectIndex *index, size_t caller,
                                 size_t **callees_out, size_t *count_out) {
  const LiveFunction *function = &index->functions[caller];
  const LinkSection *source = &index->object->sections[function->section];
  size_t *callees = NULL;
  size_t count = 0;
  for (size_t i = 0; i < source->relocation_count; i++) {
    const LinkReloc *relocation = &source->relocations[i];
    size_t *grown;
    int seen = 0;
    if (relocation->offset < function->body_offset) {
      continue;
    }
    LiveTarget target = live_classify(index, relocation->symbol_index,
                                      live_reloc_addend(source, relocation));
    if (target.kind != LIVE_T_THUNK) {
      continue;
    }
    for (size_t k = 0; k < count && !seen; k++) {
      seen = callees[k] == target.function;
    }
    if (seen) {
      continue;
    }
    grown = realloc(callees, (count + 1u) * sizeof(size_t));
    if (!grown) {
      free(callees);
      return 0;
    }
    callees = grown;
    callees[count++] = target.function;
  }
  *callees_out = callees;
  *count_out = count;
  return 1;
}

static int live_list_has(const size_t *items, size_t count, size_t value) {
  for (size_t i = 0; i < count; i++) {
    if (items[i] == value) {
      return 1;
    }
  }
  return 0;
}

static int live_check_signatures(const LiveState *state,
                                 const LiveObjectIndex *index,
                                 const unsigned char *moves,
                                 char **error_message_out) {
  size_t n = index->function_count;
  unsigned char *holder = NULL;
  unsigned char *reaches = NULL;
  size_t **callees = NULL;
  size_t *callee_count = NULL;
  int any = 0;
  int ok = 0;
  for (size_t i = 0; i < n && !any; i++) {
    const LiveEntry *previous =
        live_map_find(&state->signatures, index->functions[i].name);
    any = previous && previous->a != index->functions[i].signature;
  }
  if (!any) {
    return 1;
  }
  holder = calloc(n ? n : 1u, 1u);
  reaches = calloc(n ? n : 1u, 1u);
  callees = calloc(n ? n : 1u, sizeof(size_t *));
  callee_count = calloc(n ? n : 1u, sizeof(size_t));
  if (!holder || !reaches || !callees || !callee_count) {
    mettle_set_error(error_message_out, "Out of memory checking signatures");
    goto done;
  }
  for (size_t i = 0; i < n; i++) {
    holder[i] = (unsigned char)live_function_holds_quiesce(index, i);
    if (!live_function_callees(index, i, &callees[i], &callee_count[i])) {
      mettle_set_error(error_message_out, "Out of memory checking signatures");
      goto done;
    }
  }
  for (int changed = 1; changed;) {
    changed = 0;
    for (size_t i = 0; i < n; i++) {
      if (reaches[i]) {
        continue;
      }
      for (size_t k = 0; k < callee_count[i]; k++) {
        size_t j = callees[i][k];
        if ((holder[j] || reaches[j]) &&
            !live_same_family(index->functions[i].name,
                              index->functions[j].name)) {
          reaches[i] = 1;
          changed = 1;
          break;
        }
      }
    }
  }
  for (size_t h = 0; h < n; h++) {
    const LiveEntry *previous =
        live_map_find(&state->signatures, index->functions[h].name);
    if (!previous || previous->a == index->functions[h].signature) {
      continue;
    }
    for (size_t i = 0; i < n; i++) {
      size_t owner = live_family_owner(index, i);
      int exposed = reaches[i] || (holder[i] && !moves[owner]);
      if (!exposed ||
          live_same_family(index->functions[i].name,
                           index->functions[h].name) ||
          !live_list_has(callees[i], callee_count[i], h)) {
        continue;
      }
      mettle_set_error(error_message_out,
                       "'%s' changed its parameters or result, and '%s' may "
                       "be on the stack when the patch lands and would still "
                       "call it the old way. Keep the old signature and add a "
                       "new function, or restart",
                       index->functions[h].name,
                       index->functions[owner].name);
      goto done;
    }
  }
  ok = 1;
done:
  for (size_t i = 0; callees && i < n; i++) {
    free(callees[i]);
  }
  free(callees);
  free(callee_count);
  free(holder);
  free(reaches);
  return ok;
}

static uint64_t live_function_hash(const LiveObjectIndex *index,
                                   size_t function_index) {
  const LiveFunction *function = &index->functions[function_index];
  return live_hash_region(index, function->section, function->body_offset,
                          function->end, function_index, 0);
}

static uint64_t live_global_size(const LiveObjectIndex *index,
                                 const LinkSymbol *symbol) {
  uint64_t next = 0;
  size_t section = (size_t)symbol->section_index;
  live_covering_symbol(index, section, symbol->value, &next);
  return next > symbol->value ? next - symbol->value : 0u;
}

static int live_replace_file(const char *from, const char *to) {
#ifdef _WIN32
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return rename(from, to) == 0;
#endif
}

static void live_make_directory(const char *path) {
#ifdef _WIN32
  CreateDirectoryA(path, NULL);
#else
  mkdir(path, 0777);
#endif
}

static char *live_join(const char *left, const char *right) {
  size_t a = strlen(left);
  size_t b = strlen(right);
  char *joined = malloc(a + b + 1u);
  if (joined) {
    memcpy(joined, left, a);
    memcpy(joined + a, right, b + 1u);
  }
  return joined;
}

static int live_state_save(const LiveState *state, const char *path,
                           char **error_message_out) {
  char *temporary = live_join(path, ".tmp");
  FILE *file = temporary ? fopen(temporary, "wb") : NULL;
  if (!file) {
    mettle_set_error(error_message_out, "Cannot write the live state '%s'",
                     path);
    free(temporary);
    return 0;
  }
  fprintf(file, "mettle-live 1\n");
  fprintf(file, "image_base %llu\n", (unsigned long long)state->image_base);
  fprintf(file, "anchor %llu\n", (unsigned long long)state->anchor_rva);
  fprintf(file, "spare %llu %llu %llu\n", (unsigned long long)state->spare_rva,
          (unsigned long long)state->spare_count,
          (unsigned long long)state->spare_used);
  fprintf(file, "arena %llu %llu %llu\n", (unsigned long long)state->arena_rva,
          (unsigned long long)state->arena_size,
          (unsigned long long)state->arena_used);
  fprintf(file, "patch %u\n", state->patch_index);
  for (size_t i = 0; i < state->applied_count; i++) {
    fprintf(file, "a %u\n", state->applied[i]);
  }
  for (size_t i = 0; i < state->layouts.count; i++) {
    fprintf(file, "t %s %s\n", state->layouts.names[i],
            state->layouts.texts[i]);
  }
  for (size_t i = 0; i < state->types.count; i++) {
    fprintf(file, "y %s %s\n", state->types.names[i], state->types.texts[i]);
  }
  for (size_t i = 0; i < state->symbols.capacity; i++) {
    const LiveEntry *entry = &state->symbols.items[i];
    if (entry->key) {
      fprintf(file, "s %llu %s\n", (unsigned long long)entry->a, entry->key);
    }
  }
  for (size_t i = 0; i < state->hashes.capacity; i++) {
    const LiveEntry *entry = &state->hashes.items[i];
    if (entry->key) {
      fprintf(file, "f %llu %s\n", (unsigned long long)entry->a, entry->key);
    }
  }
  for (size_t i = 0; i < state->sizes.capacity; i++) {
    const LiveEntry *entry = &state->sizes.items[i];
    if (entry->key) {
      fprintf(file, "g %llu %s\n", (unsigned long long)entry->a, entry->key);
    }
  }
  for (size_t i = 0; i < state->spares.capacity; i++) {
    const LiveEntry *entry = &state->spares.items[i];
    if (entry->key) {
      fprintf(file, "n %llu %s\n", (unsigned long long)entry->a, entry->key);
    }
  }
  for (size_t i = 0; i < state->signatures.capacity; i++) {
    const LiveEntry *entry = &state->signatures.items[i];
    if (entry->key) {
      fprintf(file, "q %llu %s\n", (unsigned long long)entry->a, entry->key);
    }
  }
  if (fclose(file) != 0) {
    mettle_set_error(error_message_out, "Cannot write the live state '%s'",
                     path);
    free(temporary);
    return 0;
  }
  if (!live_replace_file(temporary, path)) {
    mettle_set_error(error_message_out, "Cannot replace the live state '%s'",
                     path);
    free(temporary);
    return 0;
  }
  free(temporary);
  return 1;
}

static int live_state_load(LiveState *state, const char *path,
                           char **error_message_out) {
  FILE *file = fopen(path, "rb");
  static char line[65536];
  memset(state, 0, sizeof(*state));
  if (!file) {
    mettle_set_error(error_message_out,
                     "No live state at '%s'. Build the program with --live "
                     "first",
                     path);
    return 0;
  }
  if (!fgets(line, sizeof(line), file) ||
      strncmp(line, "mettle-live 1", 13) != 0) {
    fclose(file);
    mettle_set_error(error_message_out, "'%s' is not a live state file", path);
    return 0;
  }
  while (fgets(line, sizeof(line), file)) {
    unsigned long long a = 0;
    unsigned long long b = 0;
    unsigned long long c = 0;
    char name[4096];
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
      line[--length] = '\0';
    }
    if (sscanf(line, "image_base %llu", &a) == 1) {
      state->image_base = a;
    } else if (sscanf(line, "anchor %llu", &a) == 1) {
      state->anchor_rva = a;
    } else if (sscanf(line, "spare %llu %llu %llu", &a, &b, &c) == 3) {
      state->spare_rva = a;
      state->spare_count = b;
      state->spare_used = c;
    } else if (sscanf(line, "arena %llu %llu %llu", &a, &b, &c) == 3) {
      state->arena_rva = a;
      state->arena_size = b;
      state->arena_used = c;
    } else if (sscanf(line, "patch %llu", &a) == 1) {
      state->patch_index = (unsigned)a;
    } else if ((line[0] == 't' || line[0] == 'y') && line[1] == ' ') {
      char *space = strchr(line + 2, ' ');
      if (space) {
        *space = '\0';
        if (!live_texts_set(line[0] == 't' ? &state->layouts : &state->types,
                            line + 2, space + 1)) {
          fclose(file);
          mettle_set_error(error_message_out, "Out of memory reading '%s'",
                           path);
          return 0;
        }
      }
    } else if (line[0] == 'a' && line[1] == ' ' &&
               sscanf(line + 2, "%llu", &a) == 1 && !strchr(line + 2, ' ')) {
      if (!live_state_push_applied(state, (unsigned)a)) {
        fclose(file);
        mettle_set_error(error_message_out, "Out of memory reading '%s'",
                         path);
        return 0;
      }
    } else if (line[0] && line[1] == ' ' &&
               sscanf(line + 2, "%llu %4095s", &a, name) == 2) {
      LiveMap *map = line[0] == 's'   ? &state->symbols
                     : line[0] == 'f' ? &state->hashes
                     : line[0] == 'g' ? &state->sizes
                     : line[0] == 'n' ? &state->spares
                     : line[0] == 'q' ? &state->signatures
                                      : NULL;
      if (map && !live_map_put(map, name, a, 0)) {
        fclose(file);
        mettle_set_error(error_message_out, "Out of memory reading '%s'",
                         path);
        return 0;
      }
    }
  }
  fclose(file);
  return 1;
}

static void live_state_destroy(LiveState *state) {
  live_texts_free(&state->layouts);
  live_texts_free(&state->types);
  free(state->applied);
  state->applied = NULL;
  state->applied_count = 0;
  live_map_destroy(&state->symbols);
  live_map_destroy(&state->hashes);
  live_map_destroy(&state->sizes);
  live_map_destroy(&state->spares);
  live_map_destroy(&state->signatures);
}

static int live_record_object(LiveState *state, const LinkObject *object) {
  LiveObjectIndex index;
  int ok = live_index_build(&index, object);
  for (size_t i = 0; ok && i < index.function_count; i++) {
    ok = live_map_put(&state->hashes, index.functions[i].name,
                      live_function_hash(&index, i), 0) != NULL &&
         live_map_put(&state->signatures, index.functions[i].name,
                      index.functions[i].signature, 0) != NULL;
  }
  for (size_t i = 0; ok && index.function_count && i < object->symbol_count;
       i++) {
    const LinkSymbol *symbol = &object->symbols[i];
    if (!live_symbol_is_named(symbol) || !symbol->is_external ||
        (size_t)symbol->section_index >= object->section_count) {
      continue;
    }
    LinkSectionKind kind = object->sections[symbol->section_index].kind;
    if ((kind == LINK_SECTION_KIND_DATA || kind == LINK_SECTION_KIND_BSS) &&
        !live_name_has_suffix(symbol->name, "$slot")) {
      ok = live_map_put(&state->sizes, symbol->name,
                        live_global_size(&index, symbol), 0) != NULL;
    }
  }
  live_index_destroy(&index);
  return ok;
}

int live_write_state(const LinkResolution *resolution, uint64_t image_base,
                     const char *executable_path, char **error_message_out) {
  LiveState state;
  const LinkedSymbol *anchor =
      link_resolution_find_symbol(resolution, "mettle_live_anchor");
  const LinkedSymbol *spare =
      link_resolution_find_symbol(resolution, "mettle_live_spare_slots");
  const LinkedSymbol *arena =
      link_resolution_find_symbol(resolution, "mettle_live_arena");
  char *path = NULL;
  int ok = 0;

  memset(&state, 0, sizeof(state));
  if (!anchor || !spare || !arena || !anchor->is_defined ||
      !spare->is_defined || !arena->is_defined) {
    mettle_set_error(error_message_out,
                     "A --live build has no quiesce point, so it could never "
                     "take a patch. Add 'quiesce;' where the program may "
                     "change, such as the top of its main loop");
    return 0;
  }
  state.image_base = image_base;
  state.anchor_rva = anchor->virtual_address - image_base;
  state.spare_rva = spare->virtual_address - image_base;
  state.spare_count = LIVE_SPARE_SLOT_COUNT;
  state.arena_rva = arena->virtual_address - image_base;
  state.arena_size = LIVE_ARENA_SIZE;
  for (size_t i = 0; i < resolution->symbol_count; i++) {
    const LinkedSymbol *symbol = &resolution->symbols[i];
    if (symbol->name && symbol->is_defined &&
        symbol->virtual_address >= image_base &&
        !live_map_put(&state.symbols, symbol->name,
                      symbol->virtual_address - image_base, 0)) {
      goto done;
    }
  }
  for (size_t i = 0; i < resolution->object_count; i++) {
    if (resolution->objects[i].object &&
        !live_record_object(&state, resolution->objects[i].object)) {
      goto done;
    }
  }
  for (size_t i = 0; i < g_live_layouts.count; i++) {
    if (!live_state_set_layout(&state, g_live_layouts.names[i],
                               g_live_layouts.texts[i])) {
      goto done;
    }
  }
  for (size_t i = 0; i < g_live_types.count; i++) {
    if (!live_texts_set(&state.types, g_live_types.names[i],
                        g_live_types.texts[i])) {
      goto done;
    }
  }
  path = live_join(executable_path, ".live.watched");
  if (path) {
    remove(path);
    free(path);
  }
  path = live_join(executable_path, ".live");
  ok = path && live_state_save(&state, path, error_message_out);
  for (unsigned stale = 1u; ok; stale++) {
    char patch_path[1100];
    snprintf(patch_path, sizeof(patch_path), "%s.live.d/%06u.mpatch",
             executable_path, stale);
    if (remove(patch_path) != 0) {
      break;
    }
  }

done:
  if (!ok && error_message_out && !*error_message_out) {
    mettle_set_error(error_message_out, "Out of memory writing the live state");
  }
  free(path);
  live_state_destroy(&state);
  return ok;
}

typedef struct {
  LiveState *state;
  LiveObjectIndex *index;
  LiveBuffer blob;
  LivePiece *pieces;
  size_t piece_count;
  size_t processed;
  LiveFixup *fixups;
  size_t fixup_count;
  LiveSwap *swaps;
  size_t swap_count;
  LiveInit *inits;
  size_t init_count;
  size_t *function_piece;
  size_t *function_thunk;
  LiveMap rodata_pieces;
  size_t changed;
  size_t created;
  size_t continuations;
  size_t new_globals;
  uint64_t debug_functions;
  uint32_t debug_function_count;
  uint64_t debug_locations;
  uint32_t debug_location_count;
  LiveBuffer imports;
  uint32_t import_count;
  LiveBuffer migrations;
  uint32_t migration_count;
  uint32_t type_migration_count;
  LiveMap import_slots;
  char **error_message_out;
} LiveBuilder;

static int live_builder_fail(LiveBuilder *builder, const char *format,
                             const char *name) {
  mettle_set_error(builder->error_message_out, format, name ? name : "?");
  return 0;
}

static size_t live_add_piece(LiveBuilder *builder, size_t section,
                             uint64_t start, uint64_t end, int is_init,
                             uint64_t host_rva) {
  const LinkSection *source = &builder->index->object->sections[section];
  LivePiece *grown = realloc(builder->pieces,
                             (builder->piece_count + 1u) * sizeof(LivePiece));
  if (!grown) {
    return LIVE_NONE;
  }
  builder->pieces = grown;
  if (!live_buffer_align(&builder->blob, 16u, 0xCC)) {
    return LIVE_NONE;
  }
  LivePiece *piece = &builder->pieces[builder->piece_count];
  piece->section = section;
  piece->start = start;
  piece->end = end;
  piece->blob_offset = builder->blob.size;
  piece->is_init = is_init;
  piece->host_rva = host_rva;
  if (!live_buffer_append(&builder->blob,
                          source->raw_data ? source->raw_data + start : NULL,
                          (size_t)(end - start))) {
    return LIVE_NONE;
  }
  return builder->piece_count++;
}

static int live_add_fixup(LiveBuilder *builder, uint32_t site, uint32_t kind,
                          uint64_t target) {
  LiveFixup *grown = realloc(builder->fixups,
                             (builder->fixup_count + 1u) * sizeof(LiveFixup));
  if (!grown) {
    return 0;
  }
  builder->fixups = grown;
  builder->fixups[builder->fixup_count].site = site;
  builder->fixups[builder->fixup_count].kind = kind;
  builder->fixups[builder->fixup_count].target = target;
  builder->fixups[builder->fixup_count].addend = 0;
  builder->fixup_count++;
  return 1;
}

static int live_is_host_function(const LiveBuilder *builder, const char *name) {
  return live_map_find(&builder->state->symbols, name) &&
         !live_map_find(&builder->state->spares, name);
}

static int live_spare_index(LiveBuilder *builder, const char *name,
                            uint64_t *index_out) {
  LiveEntry *entry = live_map_find(&builder->state->spares, name);
  if (!entry) {
    if (builder->state->spare_used >= builder->state->spare_count) {
      return live_builder_fail(builder,
                               "The running program has no spare slot left "
                               "for the new function '%s'. Restart it",
                               name);
    }
    entry = live_map_put(&builder->state->spares, name,
                         builder->state->spare_used++, 0);
    if (!entry) {
      return 0;
    }
  }
  *index_out = entry->a;
  return 1;
}

static int live_function_slot(LiveBuilder *builder, const char *name,
                              uint64_t *slot_rva_out) {
  char slot_name[1024];
  uint64_t spare = 0;
  if (live_is_host_function(builder, name)) {
    snprintf(slot_name, sizeof(slot_name), "%s$slot", name);
    LiveEntry *slot = live_map_find(&builder->state->symbols, slot_name);
    if (!slot) {
      LiveEntry *entry = live_map_find(&builder->state->symbols, name);
      LiveEntry *body = NULL;
      snprintf(slot_name, sizeof(slot_name), "%s$body", name);
      body = live_map_find(&builder->state->symbols, slot_name);
      if (entry && body && entry->a == body->a) {
        *slot_rva_out = entry->a | LIVE_SWAP_ENTRY;
        return 1;
      }
      return live_builder_fail(builder,
                               "'%s' was not built with a slot, so it cannot "
                               "be replaced. Rebuild with --live",
                               name);
    }
    *slot_rva_out = slot->a;
    return 1;
  }
  if (!live_spare_index(builder, name, &spare)) {
    return 0;
  }
  *slot_rva_out = builder->state->spare_rva + spare * 8u;
  return 1;
}

static int live_ensure_function(LiveBuilder *builder, size_t function_index,
                                uint64_t *blob_offset_out) {
  if (builder->function_piece[function_index] == LIVE_NONE) {
    const LiveFunction *function = &builder->index->functions[function_index];
    size_t piece = live_add_piece(builder, function->section,
                                  function->body_offset, function->end, 0, 0);
    if (piece == LIVE_NONE) {
      return 0;
    }
    builder->function_piece[function_index] = piece;
  }
  *blob_offset_out =
      builder->pieces[builder->function_piece[function_index]].blob_offset;
  return 1;
}

static int live_local_thunk(LiveBuilder *builder, size_t function_index,
                            uint64_t *blob_offset_out) {
  static const unsigned char thunk[8] = {0xFF, 0x25, 0, 0, 0, 0, 0xCC, 0xCC};
  if (builder->function_thunk[function_index] == LIVE_NONE) {
    uint64_t slot_rva = 0;
    const char *name = builder->index->functions[function_index].name;
    if (!live_function_slot(builder, name, &slot_rva) ||
        !live_buffer_align(&builder->blob, 16u, 0xCC)) {
      return 0;
    }
    builder->function_thunk[function_index] = builder->blob.size;
    if (!live_buffer_append(&builder->blob, thunk, sizeof(thunk)) ||
        !live_add_fixup(builder,
                        (uint32_t)(builder->function_thunk[function_index] + 2u),
                        LIVE_FIXUP_PC32 | LIVE_FIXUP_TARGET_HOST, slot_rva)) {
      return 0;
    }
  }
  *blob_offset_out = builder->function_thunk[function_index];
  return 1;
}

static int live_new_global(LiveBuilder *builder, const char *name,
                           uint64_t *rva_out) {
  const LinkObject *object = builder->index->object;
  const LinkSymbol *symbol = live_find_symbol(builder->index, name);
  if (!symbol || !live_symbol_is_named(symbol)) {
    return live_builder_fail(builder, "Cannot place the new global '%s'", name);
  }
  uint64_t size = live_global_size(builder->index, symbol);
  uint64_t offset = (builder->state->arena_used + 15u) & ~(uint64_t)15u;
  if (offset + size > builder->state->arena_size) {
    return live_builder_fail(builder,
                             "The running program has no room left for the "
                             "new global '%s'. Restart it",
                             name);
  }
  uint64_t rva = builder->state->arena_rva + offset;
  builder->state->arena_used = offset + size;
  if (!live_map_put(&builder->state->symbols, name, rva, 0) ||
      !live_map_put(&builder->state->sizes, name, size, 0)) {
    return 0;
  }
  builder->new_globals++;
  size_t section = (size_t)symbol->section_index;
  if (object->sections[section].kind == LINK_SECTION_KIND_DATA && size) {
    size_t piece = live_add_piece(builder, section, symbol->value,
                                  symbol->value + size, 1, rva);
    LiveInit *grown;
    if (piece == LIVE_NONE) {
      return 0;
    }
    grown = realloc(builder->inits, (builder->init_count + 1u) * sizeof(LiveInit));
    if (!grown) {
      return 0;
    }
    builder->inits = grown;
    builder->inits[builder->init_count].rva = rva;
    builder->inits[builder->init_count].blob_offset =
        builder->pieces[piece].blob_offset;
    builder->inits[builder->init_count].size = size;
    builder->init_count++;
  }
  *rva_out = rva;
  return 1;
}

static const char *const LIVE_IMPORT_DLLS[] = {
    "kernel32.dll", "ws2_32.dll", "user32.dll", "gdi32.dll",
    "advapi32.dll", "winmm.dll",  "shell32.dll"};

static const char *live_import_dll(const char *name) {
#ifdef _WIN32
  for (size_t i = 0; i < sizeof(LIVE_IMPORT_DLLS) / sizeof(LIVE_IMPORT_DLLS[0]);
       i++) {
    HMODULE module = LoadLibraryA(LIVE_IMPORT_DLLS[i]);
    int has = module && GetProcAddress(module, name) != NULL;
    if (module) {
      FreeLibrary(module);
    }
    if (has) {
      return LIVE_IMPORT_DLLS[i];
    }
  }
#else
  (void)name;
#endif
  return NULL;
}

static int live_import(LiveBuilder *builder, const char *symbol,
                       uint64_t *offset_out) {
  static const unsigned char stub[8] = {0xFF, 0x25, 0, 0, 0, 0, 0xCC, 0xCC};
  const char *name = strncmp(symbol, "__imp_", 6) == 0 ? symbol + 6 : symbol;
  int wants_slot = name != symbol;
  LiveEntry *entry = live_map_find(&builder->import_slots, name);
  if (!entry) {
    const char *dll = live_import_dll(name);
    uint64_t slot;
    uint64_t jump;
    if (!dll) {
      return 0;
    }
    if (!live_buffer_align(&builder->blob, 16u, 0)) {
      return -1;
    }
    slot = builder->blob.size;
    jump = slot + 8u;
    if (!live_buffer_append(&builder->blob, NULL, 8u) ||
        !live_buffer_append(&builder->blob, stub, sizeof(stub)) ||
        !live_add_fixup(builder, (uint32_t)(jump + 2u), LIVE_FIXUP_PC32,
                        slot) ||
        !live_buffer_u32(&builder->imports, (uint32_t)slot) ||
        !live_buffer_u32(&builder->imports, 0u) ||
        !live_buffer_append(&builder->imports, dll, strlen(dll) + 1u) ||
        !live_buffer_append(&builder->imports, name, strlen(name) + 1u) ||
        !live_buffer_align(&builder->imports, 8u, 0) ||
        !(entry = live_map_put(&builder->import_slots, name, slot, jump))) {
      return -1;
    }
    builder->import_count++;
  }
  *offset_out = wants_slot ? entry->a : entry->b;
  return 1;
}

static int live_function_references(const LiveObjectIndex *index,
                                    size_t function_index, const char *name) {
  const LiveFunction *function = &index->functions[function_index];
  const LinkSection *source = &index->object->sections[function->section];
  for (size_t i = 0; i < source->relocation_count; i++) {
    const LinkReloc *relocation = &source->relocations[i];
    LiveTarget target;
    if (relocation->offset < function->body_offset ||
        relocation->offset >= function->end) {
      continue;
    }
    target = live_classify(index, relocation->symbol_index,
                           live_reloc_addend(source, relocation));
    if (target.kind == LIVE_T_GLOBAL && target.name &&
        strcmp(target.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static uint64_t live_layout_size(const char *text) {
  char kind = *text++;
  char *end = NULL;
  uint64_t count = (uint64_t)strtoull(text, &end, 10);
  if (kind == 'A' && end && *end == '[') {
    return count * live_layout_size(end + 1);
  }
  return count;
}

static int live_convert_in_place(LiveBuilder *builder, const char *name,
                                 uint64_t rva, uint64_t size,
                                 const char *old_layout,
                                 const char *new_layout) {
  uint64_t old_text;
  uint64_t new_text;
  if (!live_buffer_align(&builder->blob, 8u, 0)) {
    return 0;
  }
  old_text = builder->blob.size;
  if (!live_buffer_append(&builder->blob, old_layout, strlen(old_layout)) ||
      !live_buffer_align(&builder->blob, 8u, 0)) {
    return 0;
  }
  new_text = builder->blob.size;
  if (!live_buffer_append(&builder->blob, new_layout, strlen(new_layout)) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)rva) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)rva) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)size) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)old_text) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)strlen(old_layout)) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)new_text) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)strlen(new_layout)) ||
      !live_buffer_u32(&builder->migrations, 0u) ||
      !live_state_set_layout(builder->state, name, new_layout)) {
    return 0;
  }
  builder->migration_count++;
  return 1;
}

static int live_migrate_global(LiveBuilder *builder, const char *name,
                               const LinkSymbol *symbol,
                               const char *old_layout,
                               const char *new_layout) {
  LiveEntry *old_entry = live_map_find(&builder->state->symbols, name);
  LiveEntry *room = live_map_find(&builder->state->sizes, name);
  uint64_t size = live_global_size(builder->index, symbol);
  uint64_t needed = live_layout_size(new_layout);
  uint64_t offset = (builder->state->arena_used + 15u) & ~(uint64_t)15u;
  uint64_t rva;
  uint64_t old_rva;
  uint64_t old_text;
  uint64_t new_text;
  if (!old_entry || size == 0) {
    return live_builder_fail(builder, "Cannot move the global '%s'", name);
  }
  if (room && needed > 0 && needed <= room->a) {
    return live_convert_in_place(builder, name, old_entry->a, needed,
                                 old_layout, new_layout);
  }
  if (offset + size > builder->state->arena_size) {
    return live_builder_fail(builder,
                             "The running program has no room left to give "
                             "'%s' its new layout. Restart it",
                             name);
  }
  old_rva = old_entry->a;
  rva = builder->state->arena_rva + offset;
  builder->state->arena_used = offset + size;
  if (!live_buffer_align(&builder->blob, 8u, 0)) {
    return 0;
  }
  old_text = builder->blob.size;
  if (!live_buffer_append(&builder->blob, old_layout, strlen(old_layout)) ||
      !live_buffer_align(&builder->blob, 8u, 0)) {
    return 0;
  }
  new_text = builder->blob.size;
  if (!live_buffer_append(&builder->blob, new_layout, strlen(new_layout)) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)old_rva) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)rva) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)size) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)old_text) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)strlen(old_layout)) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)new_text) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)strlen(new_layout)) ||
      !live_buffer_u32(&builder->migrations, 0u) ||
      !live_map_put(&builder->state->symbols, name, rva, 0) ||
      !live_map_put(&builder->state->sizes, name, size, 0) ||
      !live_state_set_layout(builder->state, name, new_layout)) {
    return 0;
  }
  builder->migration_count++;
  for (size_t f = 0; f < builder->index->function_count; f++) {
    uint64_t ignored = 0;
    if (builder->function_piece[f] == LIVE_NONE &&
        live_function_references(builder->index, f, name) &&
        !live_ensure_function(builder, f, &ignored)) {
      return 0;
    }
  }
  return 1;
}

static int live_blob_text(LiveBuilder *builder, const char *text,
                          uint64_t *offset_out) {
  if (!live_buffer_align(&builder->blob, 8u, 0)) {
    return 0;
  }
  *offset_out = builder->blob.size;
  return live_buffer_append(&builder->blob, text, strlen(text));
}

static int live_migrate_type(LiveBuilder *builder, const char *name,
                             const char *old_layout, const char *new_layout) {
  uint64_t name_at = 0;
  uint64_t old_at = 0;
  uint64_t new_at = 0;
  if (old_layout[0] == 'C' || new_layout[0] == 'C') {
    return live_builder_fail(
        builder,
        "Heap arrays of '%s' keep their fields in separate regions, and a "
        "running program does not hold their element counts. Restart it",
        name);
  }
  if (!live_blob_text(builder, name, &name_at) ||
      !live_blob_text(builder, old_layout, &old_at) ||
      !live_blob_text(builder, new_layout, &new_at) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)name_at) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)strlen(name)) ||
      !live_buffer_u32(&builder->migrations, 0u) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)old_at) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)strlen(old_layout)) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)new_at) ||
      !live_buffer_u32(&builder->migrations, (uint32_t)strlen(new_layout)) ||
      !live_buffer_u32(&builder->migrations, 1u)) {
    return 0;
  }
  builder->migration_count++;
  builder->type_migration_count++;
  return 1;
}

static int live_resolve(LiveBuilder *builder, const LiveTarget *target,
                        uint64_t *value_out, int *host_out) {
  LiveEntry *entry = NULL;
  uint64_t base = 0;
  *host_out = 1;
  switch (target->kind) {
  case LIVE_T_THUNK:
    if (live_is_host_function(builder, target->name)) {
      entry = live_map_find(&builder->state->symbols, target->name);
      *value_out = entry->a + (uint64_t)target->delta;
      return 1;
    }
    *host_out = 0;
    if (!live_local_thunk(builder, target->function, &base)) {
      return 0;
    }
    *value_out = base + (uint64_t)target->delta;
    return 1;
  case LIVE_T_BODY:
    *host_out = 0;
    if (!live_ensure_function(builder, target->function, &base)) {
      return 0;
    }
    *value_out = base + (uint64_t)target->delta;
    return 1;
  case LIVE_T_GLOBAL: {
    if (live_name_has_suffix(target->name, "$slot")) {
      char name[1024];
      size_t length = strlen(target->name) - 5u;
      if (length >= sizeof(name)) {
        return live_builder_fail(builder, "Name too long: '%s'", target->name);
      }
      memcpy(name, target->name, length);
      name[length] = '\0';
      if (!live_function_slot(builder, name, &base)) {
        return 0;
      }
      *value_out = base + (uint64_t)target->delta;
      return 1;
    }
    entry = live_map_find(&builder->state->symbols, target->name);
    if (entry) {
      LiveEntry *size = live_map_find(&builder->state->sizes, target->name);
      const LinkSymbol *symbol =
          live_find_symbol(builder->index, target->name);
      const char *old_layout = live_state_layout(builder->state, target->name);
      const char *new_layout = live_registry_layout(target->name);
      int described = old_layout && new_layout && symbol &&
                      live_symbol_is_named(symbol);
      int resized = !described && size && symbol &&
                    live_symbol_is_named(symbol) &&
                    live_global_size(builder->index, symbol) != size->a;
      if (described && strcmp(old_layout, new_layout) != 0) {
        if (!live_migrate_global(builder, target->name, symbol, old_layout,
                                 new_layout)) {
          return 0;
        }
        entry = live_map_find(&builder->state->symbols, target->name);
        *value_out = entry->a + (uint64_t)target->delta;
        return 1;
      }
      if (resized) {
        return live_builder_fail(builder,
                                 "The global '%s' changed size, so the running "
                                 "program's value no longer fits. Restart to "
                                 "take this change",
                                 target->name);
      }
      *value_out = entry->a + (uint64_t)target->delta;
      return 1;
    }
    if (!live_new_global(builder, target->name, &base)) {
      return 0;
    }
    *value_out = base + (uint64_t)target->delta;
    return 1;
  }
  case LIVE_T_RODATA: {
    char key[64];
    uint64_t region_end = 0;
    const LinkSymbol *covering = live_covering_symbol(
        builder->index, target->section, target->loc, &region_end);
    uint64_t region_start = covering ? covering->value : 0u;
    snprintf(key, sizeof(key), "r%llu:%llu", (unsigned long long)target->section,
             (unsigned long long)region_start);
    entry = live_map_find(&builder->rodata_pieces, key);
    if (!entry) {
      char content[64];
      uint64_t hash = live_hash_region(builder->index, target->section,
                                       region_start, region_end, LIVE_NONE, 0);
      snprintf(content, sizeof(content), "c%016llx:%llu",
               (unsigned long long)hash,
               (unsigned long long)(region_end - region_start));
      entry = live_map_find(&builder->rodata_pieces, content);
      if (!entry) {
        size_t piece = live_add_piece(builder, target->section, region_start,
                                      region_end, 0, 0);
        if (piece == LIVE_NONE ||
            !(entry = live_map_put(&builder->rodata_pieces, content, piece,
                                   region_start))) {
          return 0;
        }
      }
      if (!(entry = live_map_put(&builder->rodata_pieces, key, entry->a,
                                 entry->b))) {
        return 0;
      }
    }
    *host_out = 0;
    *value_out = builder->pieces[entry->a].blob_offset +
                 (target->loc - region_start);
    return 1;
  }
  case LIVE_T_TEXT:
  case LIVE_T_EXTERN:
    entry = target->name ? live_map_find(&builder->state->symbols, target->name)
                         : NULL;
    if (!entry && target->kind == LIVE_T_EXTERN && target->name) {
      int found = live_import(builder, target->name, &base);
      if (found < 0) {
        return 0;
      }
      if (found) {
        *host_out = 0;
        *value_out = base + (uint64_t)target->delta;
        return 1;
      }
    }
    if (!entry) {
      return live_builder_fail(builder,
                               "The new code needs '%s', which the running "
                               "program never linked. Restart to take this "
                               "change",
                               target->name);
    }
    *value_out = entry->a + (uint64_t)target->delta;
    return 1;
  default:
    return live_builder_fail(builder,
                             "A relocation in the new code points at "
                             "something a live patch cannot place (%s)",
                             target->name);
  }
}

static int live_family_table(LiveBuilder *builder, size_t function_index,
                             uint64_t *offset_out) {
  const LiveFunction *function = &builder->index->functions[function_index];
  const LinkSection *source =
      &builder->index->object->sections[function->section];
  for (size_t i = 0; i < source->relocation_count; i++) {
    const LinkReloc *relocation = &source->relocations[i];
    const LinkSection *data;
    LiveTarget target;
    uint64_t value = 0;
    int host = 0;
    if (relocation->offset < function->body_offset) {
      continue;
    }
    target = live_classify(builder->index, relocation->symbol_index,
                           live_reloc_addend(source, relocation));
    if (target.kind != LIVE_T_RODATA) {
      continue;
    }
    data = &builder->index->object->sections[target.section];
    if (data->raw_data && target.loc + 4u <= data->size_of_raw_data &&
        memcmp(data->raw_data + target.loc, "MTLV", 4u) != 0) {
      for (size_t r = 0; r < data->relocation_count; r++) {
        const LinkReloc *field = &data->relocations[r];
        if (field->offset == target.loc) {
          target = live_classify(builder->index, field->symbol_index,
                                 live_reloc_addend(data, field));
          data = &builder->index->object->sections[target.section];
          break;
        }
      }
    }
    if (target.kind != LIVE_T_RODATA || !data->raw_data ||
        target.loc + 4u > data->size_of_raw_data ||
        memcmp(data->raw_data + target.loc, "MTLV", 4u) != 0) {
      continue;
    }
    if (!live_resolve(builder, &target, &value, &host)) {
      return -1;
    }
    *offset_out = value;
    return host ? 0 : 1;
  }
  return 0;
}

static int live_process_piece(LiveBuilder *builder, size_t piece_index) {
  LivePiece piece = builder->pieces[piece_index];
  const LinkSection *source = &builder->index->object->sections[piece.section];
  for (size_t i = live_first_reloc(builder->index, piece.section, piece.start);
       i < source->relocation_count &&
       builder->index->reloc_order[piece.section][i]->offset < piece.end;
       i++) {
    const LinkReloc *relocation = builder->index->reloc_order[piece.section][i];
    uint64_t value = 0;
    int host = 0;
    uint32_t kind = 0;
    uint64_t site = 0;
    if (relocation->kind == LINK_RELOC_ABS64) {
      kind = LIVE_FIXUP_ABS64;
    } else if (relocation->kind == LINK_RELOC_PC32) {
      kind = LIVE_FIXUP_PC32;
    } else if (relocation->kind == LINK_RELOC_ABS32) {
      kind = LIVE_FIXUP_ABS32;
    } else {
      return live_builder_fail(builder,
                               "The new code uses a %s relocation, which a "
                               "live patch cannot apply",
                               link_reloc_kind_name(relocation->kind));
    }
    int64_t addend = live_reloc_addend(source, relocation);
    int64_t extra = 0;
    if (relocation->kind == LINK_RELOC_PC32 && relocation->addend_is_explicit &&
        addend < 0) {
      extra = addend;
      addend = 0;
    }
    LiveTarget target = live_classify(builder->index,
                                      relocation->symbol_index, addend);
    if (!live_resolve(builder, &target, &value, &host)) {
      return 0;
    }
    if (piece.is_init) {
      site = piece.host_rva + (relocation->offset - piece.start);
      kind |= LIVE_FIXUP_SITE_HOST;
    } else {
      site = piece.blob_offset + (relocation->offset - piece.start);
    }
    if (host) {
      kind |= LIVE_FIXUP_TARGET_HOST;
    }
    if (!live_add_fixup(builder, (uint32_t)site, kind, value)) {
      return 0;
    }
    builder->fixups[builder->fixup_count - 1u].addend = extra;
  }
  return 1;
}

static int live_reloc_offset_compare(const void *a, const void *b) {
  const LinkReloc *x = *(const LinkReloc *const *)a;
  const LinkReloc *y = *(const LinkReloc *const *)b;
  return x->offset < y->offset ? -1 : (x->offset > y->offset ? 1 : 0);
}

static const LinkReloc *live_sorted_reloc_at(const LinkReloc **sorted,
                                             size_t count, uint64_t offset) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2u;
    if (sorted[mid]->offset < offset) {
      low = mid + 1u;
    } else if (sorted[mid]->offset > offset) {
      high = mid;
    } else {
      return sorted[mid];
    }
  }
  return NULL;
}

static uint32_t live_debug_header_count(const LiveObjectIndex *index,
                                        size_t word) {
  const LinkSymbol *header =
      live_find_symbol(index, "mettle_debug_header");
  const LinkSection *section;
  if (!header || header->section_index < 0 ||
      (size_t)header->section_index >= index->object->section_count) {
    return 0;
  }
  section = &index->object->sections[header->section_index];
  if (!section->raw_data ||
      header->value + (word + 1u) * 4u > section->size_of_raw_data) {
    return 0;
  }
  return linker_read_u32(section->raw_data + header->value + word * 4u);
}

static int live_append_debug_rows(LiveBuilder *builder, const char *table_name,
                                  uint32_t row_count, size_t row_size,
                                  size_t code_fields, size_t pointer_fields,
                                  uint64_t *offset_out, uint32_t *count_out) {
  const LinkObject *object = builder->index->object;
  const LinkSymbol *table = live_find_symbol(builder->index, table_name);
  const LinkSection *source;
  const LinkReloc **sorted = NULL;
  size_t sorted_count = 0;
  uint64_t *rows = NULL;
  size_t *functions = NULL;
  uint64_t start;
  uint64_t end;
  uint32_t count = 0;
  int ok = 0;
  *offset_out = 0;
  *count_out = 0;
  if (!table || row_count == 0 || table->section_index < 0 ||
      (size_t)table->section_index >= object->section_count) {
    return 1;
  }
  source = &object->sections[table->section_index];
  start = table->value;
  end = start + (uint64_t)row_count * row_size;
  if (!source->raw_data || end > source->size_of_raw_data) {
    return 1;
  }
  sorted = malloc((source->relocation_count ? source->relocation_count : 1u) *
                  sizeof(*sorted));
  if (!sorted) {
    return 0;
  }
  for (size_t i = 0; i < source->relocation_count; i++) {
    if (source->relocations[i].offset >= start &&
        source->relocations[i].offset < end) {
      sorted[sorted_count++] = &source->relocations[i];
    }
  }
  qsort(sorted, sorted_count, sizeof(*sorted), live_reloc_offset_compare);
  if (!live_buffer_align(&builder->blob, 16u, 0)) {
    goto done;
  }
  *offset_out = builder->blob.size;
  rows = malloc((size_t)row_count * sizeof(*rows));
  functions = malloc((size_t)row_count * sizeof(*functions));
  if (!rows || !functions) {
    goto done;
  }
  for (uint32_t r = 0; r < row_count; r++) {
    uint64_t row = start + (uint64_t)r * row_size;
    const LinkReloc *first = live_sorted_reloc_at(sorted, sorted_count, row);
    LiveTarget code;
    if (!first) {
      continue;
    }
    code = live_classify(builder->index, first->symbol_index,
                         live_reloc_addend(source, first));
    if ((code.kind != LIVE_T_BODY && code.kind != LIVE_T_THUNK) ||
        code.function == LIVE_NONE ||
        builder->function_piece[code.function] == LIVE_NONE) {
      continue;
    }
    if (!live_buffer_append(&builder->blob, source->raw_data + row,
                            row_size)) {
      goto done;
    }
    rows[count] = row;
    functions[count] = code.function;
    count++;
  }
  for (uint32_t k = 0; k < count; k++) {
    uint64_t row = rows[k];
    uint64_t row_blob = *offset_out + (uint64_t)k * row_size;
    size_t function = functions[k];
    uint64_t function_blob =
        builder->pieces[builder->function_piece[function]].blob_offset;
    for (size_t field = 0; field < pointer_fields; field++) {
      const LinkReloc *relocation =
          live_sorted_reloc_at(sorted, sorted_count, row + field * 8u);
      LiveTarget target;
      uint64_t value = 0;
      int host = 0;
      if (!relocation) {
        continue;
      }
      target = live_classify(builder->index, relocation->symbol_index,
                             live_reloc_addend(source, relocation));
      if (field < code_fields) {
        if (target.function != function ||
            (target.kind != LIVE_T_BODY && target.kind != LIVE_T_THUNK)) {
          continue;
        }
        value = function_blob +
                (target.kind == LIVE_T_BODY ? (uint64_t)target.delta : 0u);
      } else if (!live_resolve(builder, &target, &value, &host)) {
        goto done;
      }
      if (!live_add_fixup(builder, (uint32_t)(row_blob + field * 8u),
                          LIVE_FIXUP_ABS64 |
                              (host ? LIVE_FIXUP_TARGET_HOST : 0u),
                          value)) {
        goto done;
      }
    }
  }
  *count_out = count;
  ok = 1;
done:
  free(sorted);
  free(rows);
  free(functions);
  return ok;
}

static int live_add_debug_tables(LiveBuilder *builder) {
  uint32_t functions = live_debug_header_count(builder->index, 2u);
  uint32_t locations = live_debug_header_count(builder->index, 3u);
  return live_append_debug_rows(builder, "mettle_debug_functions", functions,
                                48u, 2u, 4u, &builder->debug_functions,
                                &builder->debug_function_count) &&
         live_append_debug_rows(builder, "mettle_debug_locations", locations,
                                40u, 1u, 3u, &builder->debug_locations,
                                &builder->debug_location_count);
}

static int live_write_patch(LiveBuilder *builder, const char *path) {
  LiveBuffer out = {0};
  char *temporary = live_join(path, ".tmp");
  FILE *file = NULL;
  int ok = temporary &&
           live_buffer_u64(&out, LIVE_PATCH_MAGIC) &&
           live_buffer_u32(&out, LIVE_PATCH_VERSION) &&
           live_buffer_u32(&out, builder->state->patch_index) &&
           live_buffer_u64(&out, builder->state->anchor_rva) &&
           live_buffer_u64(&out, builder->blob.size) &&
           live_buffer_u32(&out, (uint32_t)builder->fixup_count) &&
           live_buffer_u32(&out, (uint32_t)builder->swap_count) &&
           live_buffer_u32(&out, (uint32_t)builder->init_count) &&
           live_buffer_u32(&out, (uint32_t)(builder->changed + builder->created -
                                            builder->continuations)) &&
           live_buffer_u32(&out, (uint32_t)builder->debug_functions) &&
           live_buffer_u32(&out, builder->debug_function_count) &&
           live_buffer_u32(&out, (uint32_t)builder->debug_locations) &&
           live_buffer_u32(&out, builder->debug_location_count) &&
           live_buffer_u32(&out, builder->import_count) &&
           live_buffer_u32(&out, (uint32_t)builder->imports.size) &&
           live_buffer_u32(&out, 0u) &&
           live_buffer_u32(&out, builder->migration_count) &&
           live_buffer_u64(&out, g_live_patch_tick) &&
           live_buffer_append(&out, NULL, LIVE_PATCH_HEADER_SIZE - out.size) &&
           live_buffer_append(&out, builder->blob.data, builder->blob.size) &&
           live_buffer_align(&out, 8u, 0);
  for (size_t i = 0; ok && i < builder->fixup_count; i++) {
    ok = live_buffer_u32(&out, builder->fixups[i].site) &&
         live_buffer_u32(&out, builder->fixups[i].kind) &&
         live_buffer_u64(&out, builder->fixups[i].target) &&
         live_buffer_u64(&out, (uint64_t)builder->fixups[i].addend);
  }
  for (size_t i = 0; ok && i < builder->swap_count; i++) {
    ok = live_buffer_u64(&out, builder->swaps[i].slot_rva) &&
         live_buffer_u64(&out, builder->swaps[i].body_offset);
  }
  for (size_t i = 0; ok && i < builder->init_count; i++) {
    ok = live_buffer_u64(&out, builder->inits[i].rva) &&
         live_buffer_u64(&out, builder->inits[i].blob_offset) &&
         live_buffer_u64(&out, builder->inits[i].size);
  }
  ok = ok &&
       live_buffer_append(&out, builder->imports.data, builder->imports.size) &&
       live_buffer_append(&out, builder->migrations.data,
                          builder->migrations.size);
  if (ok) {
    file = fopen(temporary, "wb");
    ok = file && fwrite(out.data, 1u, out.size, file) == out.size;
    if (file && fclose(file) != 0) {
      ok = 0;
    }
    ok = ok && live_replace_file(temporary, path);
  }
  if (!ok) {
    mettle_set_error(builder->error_message_out, "Cannot write the patch '%s'",
                     path);
    if (temporary) {
      remove(temporary);
    }
  }
  free(temporary);
  free(out.data);
  return ok;
}

int live_build_patch(const char *object_path, const char *executable_path,
                     int replace_all, LivePatchSummary *summary,
                     char **error_message_out) {
  LiveState state;
  LiveObjectIndex index;
  LiveBuilder builder;
  LinkObject *object = NULL;
  unsigned char *moves = NULL;
  char *state_path = live_join(executable_path, ".live");
  char *directory = live_join(executable_path, ".live.d");
  int ok = 0;

  memset(&index, 0, sizeof(index));
  memset(&builder, 0, sizeof(builder));
  memset(&state, 0, sizeof(state));
  if (summary) {
    memset(summary, 0, sizeof(*summary));
  }
  if (!state_path || !directory ||
      !live_state_load(&state, state_path, error_message_out) ||
      !link_object_read(object_path, &object, error_message_out)) {
    goto done;
  }
  if (object->format != LINK_FORMAT_COFF && object->format != LINK_FORMAT_ELF) {
    mettle_set_error(error_message_out,
                     "Live patches are built for Windows and Linux x64 objects "
                     "only");
    goto done;
  }
  if (!live_index_build(&index, object)) {
    mettle_set_error(error_message_out, "Out of memory indexing '%s'",
                     object_path);
    goto done;
  }
  builder.state = &state;
  builder.index = &index;
  builder.error_message_out = error_message_out;
  builder.function_piece =
      malloc((index.function_count ? index.function_count : 1u) * sizeof(size_t));
  builder.function_thunk =
      malloc((index.function_count ? index.function_count : 1u) * sizeof(size_t));
  if (!builder.function_piece || !builder.function_thunk) {
    goto done;
  }
  for (size_t i = 0; i < index.function_count; i++) {
    builder.function_piece[i] = LIVE_NONE;
    builder.function_thunk[i] = LIVE_NONE;
  }
  moves = calloc(index.function_count ? index.function_count : 1u, 1u);
  if (!moves) {
    goto done;
  }
  for (size_t i = 0; i < index.function_count; i++) {
    uint64_t hash = live_function_hash(&index, i);
    LiveEntry *previous = live_map_find(&state.hashes, index.functions[i].name);
    uint64_t ignored = 0;
    if (previous && previous->a == hash && !replace_all) {
      continue;
    }
    if (previous) {
      builder.changed++;
    } else {
      builder.created++;
    }
    if (live_is_continuation(index.functions[i].name)) {
      builder.continuations++;
    } else if (summary) {
      size_t used = strlen(summary->names);
      snprintf(summary->names + used, sizeof(summary->names) - used, "%s%s%s",
               used ? ", " : "", index.functions[i].name,
               previous ? "" : " (new)");
    }
    if (!live_map_put(&state.hashes, index.functions[i].name, hash, 0) ||
        !live_ensure_function(&builder, i, &ignored)) {
      goto done;
    }
  }
  if (builder.changed + builder.created == 0u) {
    ok = 1;
    goto done;
  }
  for (size_t i = 0; i < index.function_count; i++) {
    size_t partner = live_is_continuation(index.functions[i].name)
                         ? live_family_owner(&index, i)
                         : live_continuation_of(&index, i);
    uint64_t ignored = 0;
    if (builder.function_piece[i] == LIVE_NONE || partner == LIVE_NONE ||
        partner == i || builder.function_piece[partner] != LIVE_NONE) {
      continue;
    }
    if (!live_ensure_function(&builder, partner, &ignored)) {
      goto done;
    }
  }
  for (size_t i = 0; i < index.function_count; i++) {
    const char *owner_name = index.functions[i].name;
    char flag[1024];
    LiveEntry *flag_rva;
    LiveEntry *signature;
    uint64_t table = 0;
    int found;
    if (builder.function_piece[i] == LIVE_NONE ||
        live_is_continuation(owner_name) ||
        live_continuation_of(&index, i) == LIVE_NONE) {
      continue;
    }
    snprintf(flag, sizeof(flag), "%s$live_frame", owner_name);
    flag_rva = live_map_find(&state.symbols, flag);
    if (!flag_rva) {
      continue;
    }
    found = live_family_table(&builder, i, &table);
    if (found < 0) {
      goto done;
    }
    if (!found) {
      continue;
    }
    if (!live_add_fixup(&builder, (uint32_t)flag_rva->a,
                        LIVE_FIXUP_ABS64 | LIVE_FIXUP_SITE_HOST |
                            LIVE_FIXUP_UNDO,
                        table)) {
      goto done;
    }
    signature = live_map_find(&state.signatures, owner_name);
    moves[i] = signature && signature->a == index.functions[i].signature;
  }
  if (summary) {
    for (size_t i = 0; i < index.function_count; i++) {
      const char *name = index.functions[i].name;
      size_t used;
      if (live_is_continuation(name)) {
        continue;
      }
      if (moves[i]) {
        used = strlen(summary->moving);
        snprintf(summary->moving + used, sizeof(summary->moving) - used,
                 "%s%s", used ? ", " : "", name);
      } else if (builder.function_piece[i] != LIVE_NONE &&
                 live_map_find(&state.signatures, name) &&
                 live_function_holds_quiesce(&index, i)) {
        used = strlen(summary->running);
        snprintf(summary->running + used, sizeof(summary->running) - used,
                 "%s%s", used ? ", " : "", name);
      }
    }
  }
  if (!live_check_signatures(&state, &index, moves, error_message_out)) {
    goto done;
  }
  for (size_t i = 0; i < index.function_count; i++) {
    if (!live_map_put(&state.signatures, index.functions[i].name,
                      index.functions[i].signature, 0)) {
      goto done;
    }
  }
  if (!live_add_debug_tables(&builder)) {
    goto done;
  }
  while (builder.processed < builder.piece_count) {
    if (!live_process_piece(&builder, builder.processed++)) {
      goto done;
    }
  }
  for (size_t i = 0; i < index.function_count; i++) {
    LiveSwap *grown;
    uint64_t slot_rva = 0;
    if (builder.function_piece[i] == LIVE_NONE) {
      continue;
    }
    if (!live_function_slot(&builder, index.functions[i].name, &slot_rva)) {
      goto done;
    }
    grown = realloc(builder.swaps, (builder.swap_count + 1u) * sizeof(LiveSwap));
    if (!grown) {
      goto done;
    }
    builder.swaps = grown;
    builder.swaps[builder.swap_count].slot_rva = slot_rva;
    builder.swaps[builder.swap_count].body_offset =
        builder.pieces[builder.function_piece[i]].blob_offset;
    builder.swap_count++;
  }
  for (size_t i = 0; i < g_live_types.count; i++) {
    const char *old_layout = live_texts_get(&state.types, g_live_types.names[i]);
    if (old_layout && strcmp(old_layout, g_live_types.texts[i]) != 0 &&
        !live_migrate_type(&builder, g_live_types.names[i], old_layout,
                           g_live_types.texts[i])) {
      goto done;
    }
    if (!live_texts_set(&state.types, g_live_types.names[i],
                        g_live_types.texts[i])) {
      goto done;
    }
  }
  state.patch_index++;
  live_make_directory(directory);
  {
    char path[1024];
    char snapshot[1024];
    snprintf(path, sizeof(path), "%s/%06u.mpatch", directory, state.patch_index);
    snprintf(snapshot, sizeof(snapshot), "%s/%06u.state", directory,
             state.patch_index);
    if (!live_copy_file(state_path, snapshot) ||
        !live_state_push_applied(&state, state.patch_index)) {
      mettle_set_error(error_message_out, "Cannot keep the state for undo");
      goto done;
    }
    if (!live_write_patch(&builder, path) ||
        !live_state_save(&state, state_path, error_message_out)) {
      goto done;
    }
    if (summary) {
      snprintf(summary->patch_path, sizeof(summary->patch_path), "%s", path);
    }
  }
  ok = 1;

done:
  if (summary) {
    summary->patch_index = builder.changed + builder.created ? state.patch_index : 0u;
    summary->changed_functions = builder.changed;
    summary->new_functions = builder.created;
    summary->continuations = builder.continuations;
    summary->new_globals = builder.new_globals;
    summary->converted_globals =
        builder.migration_count - builder.type_migration_count;
    summary->converted_types = builder.type_migration_count;
    summary->at_tick = g_live_patch_tick;
    summary->blob_size = builder.blob.size;
  }
  if (!ok && error_message_out && !*error_message_out) {
    mettle_set_error(error_message_out, "Out of memory building a live patch");
  }
  free(builder.blob.data);
  free(builder.imports.data);
  free(builder.migrations.data);
  live_map_destroy(&builder.import_slots);
  free(builder.pieces);
  free(builder.fixups);
  free(builder.swaps);
  free(builder.inits);
  free(builder.function_piece);
  free(builder.function_thunk);
  free(moves);
  live_map_destroy(&builder.rodata_pieces);
  live_index_destroy(&index);
  link_object_destroy(object);
  live_state_destroy(&state);
  free(state_path);
  free(directory);
  return ok;
}

int live_build_undo(const char *executable_path, LivePatchSummary *summary,
                    char **error_message_out) {
  LiveState state;
  LiveState previous;
  LiveBuffer out = {0};
  char *state_path = live_join(executable_path, ".live");
  char *directory = live_join(executable_path, ".live.d");
  char snapshot[1024];
  char path[1024];
  unsigned target;
  FILE *file = NULL;
  int ok = 0;

  memset(&state, 0, sizeof(state));
  memset(&previous, 0, sizeof(previous));
  if (summary) {
    memset(summary, 0, sizeof(*summary));
  }
  if (!state_path || !directory ||
      !live_state_load(&state, state_path, error_message_out)) {
    goto done;
  }
  if (state.applied_count == 0) {
    mettle_set_error(error_message_out,
                     "The running program has no patch left to undo");
    goto done;
  }
  target = state.applied[state.applied_count - 1u];
  snprintf(snapshot, sizeof(snapshot), "%s/%06u.state", directory, target);
  if (!live_state_load(&previous, snapshot, error_message_out)) {
    goto done;
  }
  free(previous.applied);
  previous.applied = NULL;
  previous.applied_count = 0;
  for (size_t i = 0; i + 1u < state.applied_count; i++) {
    if (!live_state_push_applied(&previous, state.applied[i])) {
      goto done;
    }
  }
  previous.patch_index = state.patch_index + 1u;
  snprintf(path, sizeof(path), "%s/%06u.mpatch", directory,
           previous.patch_index);
  ok = live_buffer_u64(&out, LIVE_PATCH_MAGIC) &&
       live_buffer_u32(&out, LIVE_PATCH_VERSION) &&
       live_buffer_u32(&out, previous.patch_index) &&
       live_buffer_u64(&out, previous.anchor_rva) &&
       live_buffer_append(&out, NULL, 72u - out.size) &&
       live_buffer_u32(&out, target) &&
       live_buffer_append(&out, NULL, LIVE_PATCH_HEADER_SIZE - out.size);
  if (ok) {
    file = fopen(path, "wb");
    ok = file && fwrite(out.data, 1u, out.size, file) == out.size;
    if (file && fclose(file) != 0) {
      ok = 0;
    }
  }
  if (!ok) {
    mettle_set_error(error_message_out, "Cannot write the undo '%s'", path);
    goto done;
  }
  ok = live_state_save(&previous, state_path, error_message_out);
  if (ok && summary) {
    summary->patch_index = previous.patch_index;
    summary->undone = target;
    snprintf(summary->patch_path, sizeof(summary->patch_path), "%s", path);
  }

done:
  if (!ok && error_message_out && !*error_message_out) {
    mettle_set_error(error_message_out, "Out of memory building an undo");
  }
  free(out.data);
  live_state_destroy(&state);
  live_state_destroy(&previous);
  free(state_path);
  free(directory);
  return ok;
}
