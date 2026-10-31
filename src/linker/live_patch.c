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
  char *name;
  size_t section;
  uint64_t thunk_offset;
  uint64_t body_offset;
  uint64_t end;
} LiveFunction;

typedef struct {
  const LinkObject *object;
  LiveNamed **named;
  size_t *named_count;
  size_t *section_function;
  LiveFunction *functions;
  size_t function_count;
  LiveMap function_by_name;
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
} LiveState;

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
  memset(index, 0, sizeof(*index));
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
    const LinkSymbol *thunk = link_object_find_symbol(object, function->name);
    if (thunk && thunk->section_index == (int64_t)section) {
      function->thunk_offset = thunk->value;
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

static int64_t live_reloc_addend(const LinkSection *section,
                                 const LinkReloc *relocation) {
  if (relocation->addend_is_explicit) {
    return relocation->addend;
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
      if (loc < function->body_offset) {
        target.kind = LIVE_T_THUNK;
        target.delta = (int64_t)(loc - function->thunk_offset);
      } else {
        target.kind = LIVE_T_BODY;
        target.delta = (int64_t)(loc - function->body_offset);
      }
      return target;
    }
    const LinkSymbol *covering = live_covering_symbol(index, section, loc, NULL);
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
    for (size_t i = 0; i < source->relocation_count; i++) {
      const LinkReloc *relocation = &source->relocations[i];
      if (relocation->offset >= start && relocation->offset < end) {
        size_t width = live_reloc_width(relocation);
        size_t at = (size_t)(relocation->offset - start);
        memset(copy + at, 0, at + width <= size ? width : size - at);
      }
    }
    live_hash_bytes(&hash, copy, size);
    free(copy);
  }
  for (size_t i = 0; i < source->relocation_count; i++) {
    const LinkReloc *relocation = &source->relocations[i];
    if (relocation->offset < start || relocation->offset >= end) {
      continue;
    }
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
  char line[4096];
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
    } else if (line[0] && line[1] == ' ' &&
               sscanf(line + 2, "%llu %4095s", &a, name) == 2) {
      LiveMap *map = line[0] == 's'   ? &state->symbols
                     : line[0] == 'f' ? &state->hashes
                     : line[0] == 'g' ? &state->sizes
                     : line[0] == 'n' ? &state->spares
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
  live_map_destroy(&state->symbols);
  live_map_destroy(&state->hashes);
  live_map_destroy(&state->sizes);
  live_map_destroy(&state->spares);
}

static int live_record_object(LiveState *state, const LinkObject *object) {
  LiveObjectIndex index;
  int ok = live_index_build(&index, object);
  for (size_t i = 0; ok && i < index.function_count; i++) {
    ok = live_map_put(&state->hashes, index.functions[i].name,
                      live_function_hash(&index, i), 0) != NULL;
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
  size_t new_globals;
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
  const LinkSymbol *symbol = link_object_find_symbol(object, name);
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
          link_object_find_symbol(builder->index->object, target->name);
      if (size && symbol && live_symbol_is_named(symbol) &&
          live_global_size(builder->index, symbol) != size->a) {
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
    snprintf(key, sizeof(key), "%llu:%llu", (unsigned long long)target->section,
             (unsigned long long)region_start);
    entry = live_map_find(&builder->rodata_pieces, key);
    if (!entry) {
      size_t piece = live_add_piece(builder, target->section, region_start,
                                    region_end, 0, 0);
      if (piece == LIVE_NONE ||
          !(entry = live_map_put(&builder->rodata_pieces, key, piece, 0))) {
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

static int live_process_piece(LiveBuilder *builder, size_t piece_index) {
  LivePiece piece = builder->pieces[piece_index];
  const LinkSection *source = &builder->index->object->sections[piece.section];
  for (size_t i = 0; i < source->relocation_count; i++) {
    const LinkReloc *relocation = &source->relocations[i];
    uint64_t value = 0;
    int host = 0;
    uint32_t kind = 0;
    uint64_t site = 0;
    if (relocation->offset < piece.start || relocation->offset >= piece.end) {
      continue;
    }
    if (relocation->kind == LINK_RELOC_ABS64) {
      kind = LIVE_FIXUP_ABS64;
    } else if (relocation->kind == LINK_RELOC_PC32) {
      kind = LIVE_FIXUP_PC32;
    } else {
      return live_builder_fail(builder,
                               "The new code uses a %s relocation, which a "
                               "live patch cannot apply",
                               link_reloc_kind_name(relocation->kind));
    }
    LiveTarget target = live_classify(
        builder->index, relocation->symbol_index,
        live_reloc_addend(source, relocation));
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
  }
  return 1;
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
           live_buffer_u32(&out, (uint32_t)(builder->changed + builder->created)) &&
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
                     LivePatchSummary *summary, char **error_message_out) {
  LiveState state;
  LiveObjectIndex index;
  LiveBuilder builder;
  LinkObject *object = NULL;
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
  if (object->format != LINK_FORMAT_COFF) {
    mettle_set_error(error_message_out,
                     "Live patches are built for Windows x64 objects only");
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
  for (size_t i = 0; i < index.function_count; i++) {
    uint64_t hash = live_function_hash(&index, i);
    LiveEntry *previous = live_map_find(&state.hashes, index.functions[i].name);
    uint64_t ignored = 0;
    if (previous && previous->a == hash) {
      continue;
    }
    if (previous) {
      builder.changed++;
    } else {
      builder.created++;
    }
    if (summary) {
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
  state.patch_index++;
  live_make_directory(directory);
  {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%06u.mpatch", directory, state.patch_index);
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
    summary->new_globals = builder.new_globals;
    summary->blob_size = builder.blob.size;
  }
  if (!ok && error_message_out && !*error_message_out) {
    mettle_set_error(error_message_out, "Out of memory building a live patch");
  }
  free(builder.blob.data);
  free(builder.pieces);
  free(builder.fixups);
  free(builder.swaps);
  free(builder.inits);
  free(builder.function_piece);
  free(builder.function_thunk);
  live_map_destroy(&builder.rodata_pieces);
  live_index_destroy(&index);
  link_object_destroy(object);
  live_state_destroy(&state);
  free(state_path);
  free(directory);
  return ok;
}
