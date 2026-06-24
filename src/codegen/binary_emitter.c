#include "binary_emitter.h"
#include "binary_emitter_internal.h"
#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BINARY_SECTION_INDEX_NONE ((size_t)-1)

#define COFF_MACHINE_AMD64 0x8664
#define COFF_STORAGE_CLASS_EXTERNAL 2
#define COFF_STORAGE_CLASS_STATIC 3
#define COFF_SYMBOL_RECORD_SIZE 18u

#define COFF_SECTION_TEXT_CHARACTERISTICS 0x60000020u
#define COFF_SECTION_RDATA_CHARACTERISTICS 0x40000040u
#define COFF_SECTION_DATA_CHARACTERISTICS 0xC0000040u
#define COFF_SECTION_BSS_CHARACTERISTICS 0xC0000080u

#define COFF_RELOC_AMD64_ADDR64 0x0001
#define COFF_RELOC_AMD64_ADDR32NB 0x0003
#define COFF_RELOC_AMD64_REL32 0x0004
#define COFF_RELOC_AMD64_SECREL 0x000Bu

static void binary_emitter_set_error(BinaryEmitter *emitter,
                                     const char *message) {
  if (!emitter || !message) {
    return;
  }

  free(emitter->error_message);
  emitter->error_message = mettle_strdup(message);
}

static void binary_section_clear(BinarySection *section) {
  if (!section) {
    return;
  }

  free(section->name);
  free(section->data);
  memset(section, 0, sizeof(*section));
}

static void binary_symbol_clear(BinarySymbol *symbol) {
  if (!symbol) {
    return;
  }

  free(symbol->name);
  memset(symbol, 0, sizeof(*symbol));
}

static void binary_relocation_clear(BinaryRelocation *relocation) {
  if (!relocation) {
    return;
  }

  free(relocation->symbol_name);
  memset(relocation, 0, sizeof(*relocation));
}

static int binary_emitter_reserve_sections(BinaryEmitter *emitter,
                                           size_t minimum_capacity) {
  if (!emitter) {
    return 0;
  }

  if (emitter->section_capacity >= minimum_capacity) {
    return 1;
  }

  size_t new_capacity = emitter->section_capacity ? emitter->section_capacity : 4;
  while (new_capacity < minimum_capacity) {
    new_capacity *= 2;
  }

  BinarySection *grown =
      realloc(emitter->sections, new_capacity * sizeof(BinarySection));
  if (!grown) {
    binary_emitter_set_error(emitter,
                             "Out of memory while growing section table");
    return 0;
  }

  for (size_t i = emitter->section_capacity; i < new_capacity; i++) {
    memset(&grown[i], 0, sizeof(grown[i]));
  }

  emitter->sections = grown;
  emitter->section_capacity = new_capacity;
  return 1;
}

static int binary_emitter_reserve_symbols(BinaryEmitter *emitter,
                                          size_t minimum_capacity) {
  if (!emitter) {
    return 0;
  }

  if (emitter->symbol_capacity >= minimum_capacity) {
    return 1;
  }

  size_t new_capacity = emitter->symbol_capacity ? emitter->symbol_capacity : 8;
  while (new_capacity < minimum_capacity) {
    new_capacity *= 2;
  }

  BinarySymbol *grown =
      realloc(emitter->symbols, new_capacity * sizeof(BinarySymbol));
  if (!grown) {
    binary_emitter_set_error(emitter,
                             "Out of memory while growing symbol table");
    return 0;
  }

  for (size_t i = emitter->symbol_capacity; i < new_capacity; i++) {
    memset(&grown[i], 0, sizeof(grown[i]));
    grown[i].section_index = BINARY_SECTION_INDEX_NONE;
  }

  emitter->symbols = grown;
  emitter->symbol_capacity = new_capacity;
  return 1;
}

static int binary_emitter_reserve_relocations(BinaryEmitter *emitter,
                                              size_t minimum_capacity) {
  if (!emitter) {
    return 0;
  }

  if (emitter->relocation_capacity >= minimum_capacity) {
    return 1;
  }

  size_t new_capacity =
      emitter->relocation_capacity ? emitter->relocation_capacity : 8;
  while (new_capacity < minimum_capacity) {
    new_capacity *= 2;
  }

  BinaryRelocation *grown =
      realloc(emitter->relocations, new_capacity * sizeof(BinaryRelocation));
  if (!grown) {
    binary_emitter_set_error(emitter,
                             "Out of memory while growing relocation table");
    return 0;
  }

  for (size_t i = emitter->relocation_capacity; i < new_capacity; i++) {
    memset(&grown[i], 0, sizeof(grown[i]));
  }

  emitter->relocations = grown;
  emitter->relocation_capacity = new_capacity;
  return 1;
}

static int binary_section_reserve(BinaryEmitter *emitter, BinarySection *section,
                                  size_t minimum_capacity) {
  if (!emitter || !section) {
    return 0;
  }

  if (section->capacity >= minimum_capacity) {
    return 1;
  }

  size_t new_capacity = section->capacity ? section->capacity : 32;
  while (new_capacity < minimum_capacity) {
    new_capacity *= 2;
  }

  unsigned char *grown = realloc(section->data, new_capacity);
  if (!grown) {
    binary_emitter_set_error(emitter,
                             "Out of memory while growing section payload");
    return 0;
  }

  section->data = grown;
  section->capacity = new_capacity;
  return 1;
}

static uint64_t binary_emitter_hash_name(const char *name) {
  /* FNV-1a 64-bit. unsigned long is 32-bit on Windows, so use a fixed width. */
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    hash ^= (uint64_t)*p;
    hash *= 1099511628211ULL;
  }
  return hash;
}

/* Rebuilds the symbol hash index from scratch over the current symbol array.
 * Called when the index would exceed a 0.7 load factor. */
static int binary_emitter_symbol_index_rehash(BinaryEmitter *emitter,
                                              size_t new_bucket_count) {
  size_t *buckets = calloc(new_bucket_count, sizeof(size_t));
  if (!buckets) {
    return 0;
  }
  size_t mask = new_bucket_count - 1;
  for (size_t i = 0; i < emitter->symbol_count; i++) {
    if (!emitter->symbols[i].name) {
      continue;
    }
    size_t pos =
        (size_t)(binary_emitter_hash_name(emitter->symbols[i].name) &
                 (uint64_t)mask);
    while (buckets[pos] != 0) {
      pos = (pos + 1) & mask;
    }
    buckets[pos] = i + 1;
  }
  free(emitter->symbol_index_buckets);
  emitter->symbol_index_buckets = buckets;
  emitter->symbol_index_bucket_count = new_bucket_count;
  return 1;
}

/* Records that emitter->symbols[symbol_index] now exists in the hash index.
 * Grows the bucket array first if needed. */
static int binary_emitter_symbol_index_insert(BinaryEmitter *emitter,
                                              size_t symbol_index) {
  size_t live = symbol_index + 1;
  if (emitter->symbol_index_bucket_count == 0 ||
      (live * 10) >= (emitter->symbol_index_bucket_count * 7)) {
    size_t next = emitter->symbol_index_bucket_count == 0
                      ? 256
                      : emitter->symbol_index_bucket_count * 2;
    if (!binary_emitter_symbol_index_rehash(emitter, next)) {
      return 0;
    }
    /* Rehash already placed every existing symbol, including this one if it
     * was appended before the call. Re-insert below is still safe because the
     * caller invokes this exactly once per new symbol. */
  }
  size_t mask = emitter->symbol_index_bucket_count - 1;
  const char *name = emitter->symbols[symbol_index].name;
  size_t pos = (size_t)(binary_emitter_hash_name(name) & (uint64_t)mask);
  while (emitter->symbol_index_buckets[pos] != 0) {
    if (emitter->symbol_index_buckets[pos] == symbol_index + 1) {
      return 1; /* already present (placed by a rehash) */
    }
    pos = (pos + 1) & mask;
  }
  emitter->symbol_index_buckets[pos] = symbol_index + 1;
  return 1;
}

static int binary_emitter_find_symbol_index(const BinaryEmitter *emitter,
                                            const char *name) {
  if (!emitter || !name) {
    return -1;
  }

  if (emitter->symbol_index_buckets &&
      emitter->symbol_index_bucket_count > 0) {
    size_t mask = emitter->symbol_index_bucket_count - 1;
    size_t pos =
        (size_t)(binary_emitter_hash_name(name) & (uint64_t)mask);
    while (emitter->symbol_index_buckets[pos] != 0) {
      size_t idx = emitter->symbol_index_buckets[pos] - 1;
      if (emitter->symbols[idx].name &&
          strcmp(emitter->symbols[idx].name, name) == 0) {
        return (int)idx;
      }
      pos = (pos + 1) & mask;
    }
    return -1;
  }

  /* Index not built yet (no symbols defined) — linear fallback. */
  for (size_t i = 0; i < emitter->symbol_count; i++) {
    if (emitter->symbols[i].name &&
        strcmp(emitter->symbols[i].name, name) == 0) {
      return (int)i;
    }
  }

  return -1;
}

static int binary_emitter_write_u16(FILE *file, uint16_t value) {
  return fwrite(&value, sizeof(value), 1, file) == 1;
}

static int binary_emitter_write_u32(FILE *file, uint32_t value) {
  return fwrite(&value, sizeof(value), 1, file) == 1;
}

static int binary_emitter_write_i16(FILE *file, int16_t value) {
  return fwrite(&value, sizeof(value), 1, file) == 1;
}

static int binary_emitter_write_section_name(FILE *file, const char *name,
                                             size_t string_table_offset) {
  unsigned char field[8] = {0};
  if (!name) {
    return fwrite(field, sizeof(field), 1, file) == 1;
  }

  size_t length = strlen(name);
  if (length <= sizeof(field)) {
    memcpy(field, name, length);
    return fwrite(field, sizeof(field), 1, file) == 1;
  }

  int written = snprintf((char *)field, sizeof(field), "/%zu",
                         string_table_offset);
  if (written <= 0 || (size_t)written >= sizeof(field)) {
    return 0;
  }
  return fwrite(field, sizeof(field), 1, file) == 1;
}

static int binary_emitter_write_symbol_name(FILE *file, const char *name,
                                            size_t string_table_offset) {
  unsigned char field[8] = {0};
  if (!name) {
    return fwrite(field, sizeof(field), 1, file) == 1;
  }

  size_t length = strlen(name);
  if (length <= sizeof(field)) {
    memcpy(field, name, length);
    return fwrite(field, sizeof(field), 1, file) == 1;
  }

  uint32_t zero_prefix = 0;
  uint32_t offset = (uint32_t)string_table_offset;
  memcpy(field, &zero_prefix, sizeof(zero_prefix));
  memcpy(field + sizeof(zero_prefix), &offset, sizeof(offset));
  return fwrite(field, sizeof(field), 1, file) == 1;
}

static int binary_emitter_write_zero_bytes(FILE *file, size_t count) {
  unsigned char zeroes[COFF_SYMBOL_RECORD_SIZE] = {0};

  while (count > 0) {
    size_t chunk = count < sizeof(zeroes) ? count : sizeof(zeroes);
    if (fwrite(zeroes, 1, chunk, file) != chunk) {
      return 0;
    }
    count -= chunk;
  }

  return 1;
}

static uint32_t binary_emitter_default_section_characteristics(
    BinarySectionKind kind) {
  switch (kind) {
  case BINARY_SECTION_TEXT:
    return COFF_SECTION_TEXT_CHARACTERISTICS;
  case BINARY_SECTION_RDATA:
  case BINARY_SECTION_INIT_ARRAY:
  case BINARY_SECTION_FINI_ARRAY:
  case BINARY_SECTION_DEBUG:
    return COFF_SECTION_RDATA_CHARACTERISTICS;
  case BINARY_SECTION_DATA:
    return COFF_SECTION_DATA_CHARACTERISTICS;
  case BINARY_SECTION_BSS:
    return COFF_SECTION_BSS_CHARACTERISTICS;
  default:
    return COFF_SECTION_DATA_CHARACTERISTICS;
  }
}

static uint16_t binary_emitter_map_relocation_kind(BinaryRelocationKind kind) {
  switch (kind) {
  case BINARY_RELOCATION_ADDR64:
    return COFF_RELOC_AMD64_ADDR64;
  case BINARY_RELOCATION_ADDR32NB:
    return COFF_RELOC_AMD64_ADDR32NB;
  case BINARY_RELOCATION_SECTION_REL32:
    return COFF_RELOC_AMD64_SECREL;
  case BINARY_RELOCATION_REL32:
  default:
    return COFF_RELOC_AMD64_REL32;
  }
}

BinaryTargetFormat binary_target_format_host_default(void) {
#ifdef _WIN32
  return BINARY_TARGET_FORMAT_COFF_WIN64;
#else
  return BINARY_TARGET_FORMAT_ELF_X64;
#endif
}

BinaryEmitter *binary_emitter_create(BinaryTargetFormat target_format) {
  BinaryEmitter *emitter = calloc(1, sizeof(BinaryEmitter));
  if (!emitter) {
    return NULL;
  }

  emitter->target_format = target_format;
  return emitter;
}

void binary_emitter_reset(BinaryEmitter *emitter) {
  if (!emitter) {
    return;
  }

  for (size_t i = 0; i < emitter->section_count; i++) {
    binary_section_clear(&emitter->sections[i]);
  }
  for (size_t i = 0; i < emitter->symbol_count; i++) {
    binary_symbol_clear(&emitter->symbols[i]);
  }
  for (size_t i = 0; i < emitter->relocation_count; i++) {
    binary_relocation_clear(&emitter->relocations[i]);
  }

  emitter->section_count = 0;
  emitter->symbol_count = 0;
  emitter->relocation_count = 0;
  free(emitter->symbol_index_buckets);
  emitter->symbol_index_buckets = NULL;
  emitter->symbol_index_bucket_count = 0;
  free(emitter->error_message);
  emitter->error_message = NULL;
}

void binary_emitter_destroy(BinaryEmitter *emitter) {
  if (!emitter) {
    return;
  }

  binary_emitter_reset(emitter);
  free(emitter->sections);
  free(emitter->symbols);
  free(emitter->relocations);
  free(emitter);
}

size_t binary_emitter_get_or_create_section(BinaryEmitter *emitter,
                                            const char *name,
                                            BinarySectionKind kind,
                                            uint32_t characteristics,
                                            size_t alignment) {
  if (!emitter || !name || name[0] == '\0') {
    return BINARY_SECTION_INDEX_NONE;
  }

  for (size_t i = 0; i < emitter->section_count; i++) {
    if (emitter->sections[i].name &&
        strcmp(emitter->sections[i].name, name) == 0) {
      if (alignment > emitter->sections[i].alignment) {
        emitter->sections[i].alignment = alignment;
      }
      if (characteristics != 0) {
        emitter->sections[i].characteristics = characteristics;
      }
      return i;
    }
  }

  if (!binary_emitter_reserve_sections(emitter, emitter->section_count + 1)) {
    return BINARY_SECTION_INDEX_NONE;
  }

  BinarySection *section = &emitter->sections[emitter->section_count];
  memset(section, 0, sizeof(*section));
  section->name = mettle_strdup(name);
  if (!section->name) {
    binary_emitter_set_error(emitter,
                             "Out of memory while storing section name");
    return BINARY_SECTION_INDEX_NONE;
  }

  section->kind = kind;
  section->characteristics = characteristics;
  section->alignment = alignment ? alignment : 1;

  emitter->section_count++;
  return emitter->section_count - 1;
}

BinarySection *binary_emitter_get_section(BinaryEmitter *emitter,
                                          size_t section_index) {
  if (!emitter || section_index >= emitter->section_count) {
    return NULL;
  }

  return &emitter->sections[section_index];
}

const BinarySection *binary_emitter_get_section_const(
    const BinaryEmitter *emitter, size_t section_index) {
  if (!emitter || section_index >= emitter->section_count) {
    return NULL;
  }

  return &emitter->sections[section_index];
}

int binary_emitter_align_section(BinaryEmitter *emitter, size_t section_index,
                                 size_t alignment, unsigned char fill_byte) {
  BinarySection *section = binary_emitter_get_section(emitter, section_index);
  if (!section || alignment == 0) {
    return 0;
  }

  if (alignment > section->alignment) {
    section->alignment = alignment;
  }

  size_t remainder = section->size % alignment;
  if (remainder == 0) {
    return 1;
  }

  size_t padding = alignment - remainder;
  if (!binary_section_reserve(emitter, section, section->size + padding)) {
    return 0;
  }

  memset(section->data + section->size, fill_byte, padding);
  section->size += padding;
  if (section->virtual_size < section->size) {
    section->virtual_size = section->size;
  }
  return 1;
}

int binary_emitter_append_bytes(BinaryEmitter *emitter, size_t section_index,
                                const void *data, size_t size,
                                size_t *offset_out) {
  BinarySection *section = binary_emitter_get_section(emitter, section_index);
  if (!section || (!data && size != 0)) {
    return 0;
  }

  if (!binary_section_reserve(emitter, section, section->size + size)) {
    return 0;
  }

  if (offset_out) {
    *offset_out = section->size;
  }
  if (size != 0) {
    memcpy(section->data + section->size, data, size);
    section->size += size;
  }
  if (section->virtual_size < section->size) {
    section->virtual_size = section->size;
  }
  return 1;
}

int binary_emitter_append_zeros(BinaryEmitter *emitter, size_t section_index,
                                size_t size, size_t *offset_out) {
  BinarySection *section = binary_emitter_get_section(emitter, section_index);
  if (!section) {
    return 0;
  }

  if (!binary_section_reserve(emitter, section, section->size + size)) {
    return 0;
  }

  if (offset_out) {
    *offset_out = section->size;
  }
  if (size != 0) {
    memset(section->data + section->size, 0, size);
    section->size += size;
  }
  if (section->virtual_size < section->size) {
    section->virtual_size = section->size;
  }
  return 1;
}

int binary_emitter_set_section_virtual_size(BinaryEmitter *emitter,
                                            size_t section_index,
                                            size_t virtual_size) {
  BinarySection *section = binary_emitter_get_section(emitter, section_index);
  if (!section || virtual_size < section->size) {
    return 0;
  }

  section->virtual_size = virtual_size;
  return 1;
}

int binary_emitter_define_symbol(BinaryEmitter *emitter, const char *name,
                                 BinarySymbolBinding binding,
                                 size_t section_index, size_t value,
                                 size_t size) {
  if (!emitter || !name || name[0] == '\0') {
    return 0;
  }

  int existing_index = binary_emitter_find_symbol_index(emitter, name);
  if (existing_index >= 0) {
    BinarySymbol *symbol = &emitter->symbols[(size_t)existing_index];
    symbol->binding = binding;
    symbol->section_index = section_index;
    symbol->value = value;
    symbol->size = size;
    return 1;
  }

  if (!binary_emitter_reserve_symbols(emitter, emitter->symbol_count + 1)) {
    return 0;
  }

  BinarySymbol *symbol = &emitter->symbols[emitter->symbol_count];
  memset(symbol, 0, sizeof(*symbol));
  symbol->name = mettle_strdup(name);
  if (!symbol->name) {
    binary_emitter_set_error(emitter,
                             "Out of memory while storing symbol name");
    return 0;
  }

  symbol->binding = binding;
  symbol->section_index = section_index;
  symbol->value = value;
  symbol->size = size;
  size_t new_index = emitter->symbol_count;
  emitter->symbol_count++;
  if (!binary_emitter_symbol_index_insert(emitter, new_index)) {
    emitter->symbol_count--;
    free(symbol->name);
    symbol->name = NULL;
    binary_emitter_set_error(emitter,
                             "Out of memory while indexing symbol name");
    return 0;
  }
  return 1;
}

int binary_emitter_declare_external(BinaryEmitter *emitter, const char *name) {
  return binary_emitter_define_symbol(emitter, name, BINARY_SYMBOL_EXTERNAL,
                                      BINARY_SECTION_INDEX_NONE, 0, 0);
}

const BinarySymbol *binary_emitter_find_symbol(const BinaryEmitter *emitter,
                                               const char *name) {
  int index = binary_emitter_find_symbol_index(emitter, name);
  if (index < 0) {
    return NULL;
  }

  return &emitter->symbols[(size_t)index];
}

int binary_emitter_add_relocation(BinaryEmitter *emitter, size_t section_index,
                                  size_t offset, BinaryRelocationKind kind,
                                  const char *symbol_name, int32_t addend) {
  if (!emitter || !symbol_name || symbol_name[0] == '\0' ||
      section_index >= emitter->section_count) {
    return 0;
  }

  if (!binary_emitter_reserve_relocations(emitter,
                                          emitter->relocation_count + 1)) {
    return 0;
  }

  BinaryRelocation *relocation =
      &emitter->relocations[emitter->relocation_count];
  memset(relocation, 0, sizeof(*relocation));
  relocation->symbol_name = mettle_strdup(symbol_name);
  if (!relocation->symbol_name) {
    binary_emitter_set_error(emitter,
                             "Out of memory while storing relocation symbol");
    return 0;
  }

  relocation->section_index = section_index;
  relocation->offset = offset;
  relocation->kind = kind;
  relocation->addend = addend;
  emitter->relocation_count++;
  return 1;
}

const char *binary_emitter_get_error(const BinaryEmitter *emitter) {
  return emitter ? emitter->error_message : NULL;
}

static int binary_emitter_write_coff_object_file(BinaryEmitter *emitter,
                                                 const char *filename) {
  if (emitter->section_count > 0xFFFFu) {
    binary_emitter_set_error(emitter, "Too many sections for COFF object file");
    return 0;
  }
  if (emitter->relocation_count > 0xFFFFFFFFu ||
      emitter->symbol_count > 0xFFFFFFFFu) {
    binary_emitter_set_error(emitter, "Emitter tables exceed COFF limits");
    return 0;
  }
  if (emitter->section_count >
      ((0xFFFFFFFFu - (uint32_t)emitter->symbol_count) / 2u)) {
    binary_emitter_set_error(emitter,
                             "Section symbol records exceed COFF limits");
    return 0;
  }

  FILE *file = fopen(filename, "wb");
  if (!file) {
    binary_emitter_set_error(emitter, "Failed to open object output file");
    return 0;
  }
  /* The COFF writer emits the header, section table, and symbol table as many
   * tiny 2/4-byte fwrites. A large stdio buffer collapses those into memory
   * copies instead of one libc/syscall round trip per field. */
  setvbuf(file, NULL, _IOFBF, 1 << 20);

  uint32_t *section_name_offsets = NULL;
  uint32_t *symbol_name_offsets = NULL;
  uint32_t *section_raw_offsets = NULL;
  uint32_t *section_reloc_offsets = NULL;
  uint32_t *section_reloc_counts = NULL;
  uint32_t *symbol_table_indices = NULL;
  uint32_t string_table_size = 4;
  uint32_t total_symbol_records = 0;
  int ok = 0;

  if (emitter->section_count > 0) {
    section_name_offsets = calloc(emitter->section_count, sizeof(uint32_t));
    section_raw_offsets = calloc(emitter->section_count, sizeof(uint32_t));
    section_reloc_offsets = calloc(emitter->section_count, sizeof(uint32_t));
    section_reloc_counts = calloc(emitter->section_count, sizeof(uint32_t));
  }
  if (emitter->symbol_count > 0) {
    symbol_name_offsets = calloc(emitter->symbol_count, sizeof(uint32_t));
    symbol_table_indices = calloc(emitter->symbol_count, sizeof(uint32_t));
  }
  if ((emitter->section_count > 0 &&
       (!section_name_offsets || !section_raw_offsets ||
        !section_reloc_offsets || !section_reloc_counts)) ||
      (emitter->symbol_count > 0 &&
       (!symbol_name_offsets || !symbol_table_indices))) {
    binary_emitter_set_error(emitter,
                             "Out of memory while preparing COFF tables");
    goto cleanup;
  }

  for (size_t i = 0; i < emitter->section_count; i++) {
    const BinarySection *section = &emitter->sections[i];
    if (section->name && strlen(section->name) > 8) {
      section_name_offsets[i] = string_table_size;
      string_table_size += (uint32_t)strlen(section->name) + 1;
    }
  }
  for (size_t i = 0; i < emitter->symbol_count; i++) {
    const BinarySymbol *symbol = &emitter->symbols[i];
    if (symbol->name && strlen(symbol->name) > 8) {
      symbol_name_offsets[i] = string_table_size;
      string_table_size += (uint32_t)strlen(symbol->name) + 1;
    }
    symbol_table_indices[i] = (uint32_t)i;
  }
  total_symbol_records =
      (uint32_t)emitter->symbol_count + (uint32_t)(emitter->section_count * 2u);

  for (size_t i = 0; i < emitter->relocation_count; i++) {
    const BinaryRelocation *relocation = &emitter->relocations[i];
    if (relocation->section_index >= emitter->section_count) {
      binary_emitter_set_error(emitter,
                               "Relocation refers to an invalid section");
      goto cleanup;
    }
    section_reloc_counts[relocation->section_index]++;
  }
  /* COFF stores a section's relocation count in a 16-bit field. When a section
   * has more than 0xFFFF relocations we use the IMAGE_SCN_LNK_NRELOC_OVFL
   * mechanism: the count field is set to 0xFFFF, the flag is set in the section
   * characteristics, and a synthetic first relocation record carries the real
   * count (+1, to include itself) in its VirtualAddress. */
  uint32_t reloc_overflow_max = 0xFFFFu;
  for (size_t i = 0; i < emitter->section_count; i++) {
    if (section_reloc_counts[i] > reloc_overflow_max) {
      reloc_overflow_max = section_reloc_counts[i];
    }
  }
  /* A section with exactly 0xFFFF real relocations would be ambiguous with the
   * overflow sentinel, so it must also use the overflow form. */

  uint32_t offset = 20u + (uint32_t)(emitter->section_count * 40u);
  for (size_t i = 0; i < emitter->section_count; i++) {
    const BinarySection *section = &emitter->sections[i];
    if (section->kind != BINARY_SECTION_BSS && section->size > 0) {
      section_raw_offsets[i] = offset;
      offset += (uint32_t)section->size;
    }
  }
  for (size_t i = 0; i < emitter->section_count; i++) {
    if (section_reloc_counts[i] > 0) {
      section_reloc_offsets[i] = offset;
      uint32_t records = section_reloc_counts[i];
      if (records >= 0xFFFFu) {
        records += 1u; /* synthetic overflow-count record */
      }
      offset += records * 10u;
    }
  }
  uint32_t pointer_to_symbol_table = offset;

  if (!binary_emitter_write_u16(file, COFF_MACHINE_AMD64) ||
      !binary_emitter_write_u16(file, (uint16_t)emitter->section_count) ||
      !binary_emitter_write_u32(file, 0) ||
      !binary_emitter_write_u32(file, pointer_to_symbol_table) ||
      !binary_emitter_write_u32(file, total_symbol_records) ||
      !binary_emitter_write_u16(file, 0) ||
      !binary_emitter_write_u16(file, 0)) {
    binary_emitter_set_error(emitter, "Failed while writing COFF file header");
    goto cleanup;
  }

  for (size_t i = 0; i < emitter->section_count; i++) {
    const BinarySection *section = &emitter->sections[i];
    uint32_t characteristics = section->characteristics;
    if (characteristics == 0) {
      characteristics =
          binary_emitter_default_section_characteristics(section->kind);
    }
    /* >= 0xFFFF relocations -> overflow form: 0xFFFF in the count field plus the
     * IMAGE_SCN_LNK_NRELOC_OVFL (0x01000000) flag. */
    uint16_t reloc_count_field = (uint16_t)section_reloc_counts[i];
    if (section_reloc_counts[i] >= 0xFFFFu) {
      reloc_count_field = 0xFFFFu;
      characteristics |= 0x01000000u;
    }
    if (!binary_emitter_write_section_name(file, section->name,
                                           section_name_offsets[i]) ||
        !binary_emitter_write_u32(file, 0) ||
        !binary_emitter_write_u32(file, 0) ||
        !binary_emitter_write_u32(
            file, section->kind == BINARY_SECTION_BSS ? 0u : (uint32_t)section->size) ||
        !binary_emitter_write_u32(file, section_raw_offsets[i]) ||
        !binary_emitter_write_u32(file, section_reloc_offsets[i]) ||
        !binary_emitter_write_u32(file, 0) ||
        !binary_emitter_write_u16(file, reloc_count_field) ||
        !binary_emitter_write_u16(file, 0) ||
        !binary_emitter_write_u32(file, characteristics)) {
      binary_emitter_set_error(emitter,
                               "Failed while writing COFF section headers");
      goto cleanup;
    }
  }

  for (size_t i = 0; i < emitter->section_count; i++) {
    const BinarySection *section = &emitter->sections[i];
    if (section->kind == BINARY_SECTION_BSS || section->size == 0) {
      continue;
    }
    if (fwrite(section->data, 1, section->size, file) != section->size) {
      binary_emitter_set_error(emitter,
                               "Failed while writing COFF section payload");
      goto cleanup;
    }
  }

  /* Emit relocations grouped by section. The previous implementation rescanned
   * every relocation once per section (O(sections * relocations)). Instead do
   * a single counting sort: compute each section's start index in a combined
   * ordering, then place every relocation in one O(relocations) pass. Combined
   * with the O(1) symbol-name hash index this drops the whole step from
   * O(sections * relocations * symbols) to O(relocations). */
  if (emitter->relocation_count > 0) {
    size_t *section_reloc_start =
        calloc(emitter->section_count + 1, sizeof(size_t));
    size_t *ordered_relocations =
        calloc(emitter->relocation_count, sizeof(size_t));
    if (!section_reloc_start || !ordered_relocations) {
      free(section_reloc_start);
      free(ordered_relocations);
      binary_emitter_set_error(emitter,
                               "Out of memory while ordering relocations");
      goto cleanup;
    }

    /* Prefix sums of per-section counts give each section's slot range. */
    for (size_t i = 0; i < emitter->section_count; i++) {
      section_reloc_start[i + 1] =
          section_reloc_start[i] + section_reloc_counts[i];
    }

    /* Stable bucket placement preserves original within-section order. */
    size_t *cursor = calloc(emitter->section_count, sizeof(size_t));
    if (!cursor) {
      free(section_reloc_start);
      free(ordered_relocations);
      binary_emitter_set_error(emitter,
                               "Out of memory while ordering relocations");
      goto cleanup;
    }
    for (size_t r = 0; r < emitter->relocation_count; r++) {
      size_t sec = emitter->relocations[r].section_index;
      size_t dst = section_reloc_start[sec] + cursor[sec]++;
      ordered_relocations[dst] = r;
    }
    free(cursor);

    /* Write relocations per section so an overflow section can be prefixed by
     * its synthetic count record (VirtualAddress = real count + 1). */
    int order_ok = 1;
    for (size_t s = 0; s < emitter->section_count && order_ok; s++) {
      if (section_reloc_counts[s] == 0) {
        continue;
      }
      if (section_reloc_counts[s] >= 0xFFFFu) {
        if (!binary_emitter_write_u32(file, section_reloc_counts[s] + 1u) ||
            !binary_emitter_write_u32(file, 0) ||
            !binary_emitter_write_u16(file, 0)) {
          binary_emitter_set_error(
              emitter, "Failed while writing COFF relocation overflow record");
          order_ok = 0;
          break;
        }
      }
      size_t k = section_reloc_start[s];
      size_t k_end = section_reloc_start[s + 1];
      for (; k < k_end && order_ok; k++) {
        const BinaryRelocation *relocation =
            &emitter->relocations[ordered_relocations[k]];

        int symbol_index =
            binary_emitter_find_symbol_index(emitter, relocation->symbol_name);
        if (symbol_index < 0) {
          char error_buffer[256];
          snprintf(error_buffer, sizeof(error_buffer),
                   "Relocation refers to an undefined symbol '%s'",
                   relocation->symbol_name ? relocation->symbol_name : "<null>");
          binary_emitter_set_error(emitter, error_buffer);
          order_ok = 0;
          break;
        }

        if (!binary_emitter_write_u32(file, (uint32_t)relocation->offset) ||
            !binary_emitter_write_u32(
                file, symbol_table_indices[(size_t)symbol_index]) ||
            !binary_emitter_write_u16(
                file, binary_emitter_map_relocation_kind(relocation->kind))) {
          binary_emitter_set_error(emitter,
                                   "Failed while writing COFF relocations");
          order_ok = 0;
          break;
        }
      }
    }

    free(section_reloc_start);
    free(ordered_relocations);
    if (!order_ok) {
      goto cleanup;
    }
  }

  for (size_t i = 0; i < emitter->symbol_count; i++) {
    const BinarySymbol *symbol = &emitter->symbols[i];
    int16_t section_number = 0;
    uint16_t type = 0;
    unsigned char storage_class = COFF_STORAGE_CLASS_EXTERNAL;

    if (symbol->binding == BINARY_SYMBOL_LOCAL) {
      storage_class = COFF_STORAGE_CLASS_STATIC;
    }
    if (symbol->section_index != BINARY_SECTION_INDEX_NONE) {
      if (symbol->section_index >= emitter->section_count) {
        binary_emitter_set_error(emitter,
                                 "Symbol refers to an invalid section");
        goto cleanup;
      }
      section_number = (int16_t)(symbol->section_index + 1);
      if (emitter->sections[symbol->section_index].kind == BINARY_SECTION_TEXT) {
        type = 0x0020u;
      }
    }

    if (!binary_emitter_write_symbol_name(file, symbol->name,
                                          symbol_name_offsets[i]) ||
        !binary_emitter_write_u32(file, (uint32_t)symbol->value) ||
        !binary_emitter_write_i16(file, section_number) ||
        !binary_emitter_write_u16(file, type) ||
        fwrite(&storage_class, sizeof(storage_class), 1, file) != 1) {
      binary_emitter_set_error(emitter,
                               "Failed while writing COFF symbols");
      goto cleanup;
    }

    unsigned char aux_symbols = 0;
    if (fwrite(&aux_symbols, sizeof(aux_symbols), 1, file) != 1) {
      binary_emitter_set_error(emitter,
                               "Failed while finalizing COFF symbols");
      goto cleanup;
    }
  }

  for (size_t i = 0; i < emitter->section_count; i++) {
    const BinarySection *section = &emitter->sections[i];
    uint32_t section_length =
        (uint32_t)(section->virtual_size > section->size ? section->virtual_size
                                                         : section->size);
    unsigned char storage_class = COFF_STORAGE_CLASS_STATIC;
    unsigned char aux_symbols = 1;

    if (!binary_emitter_write_symbol_name(file, section->name,
                                          section_name_offsets[i]) ||
        !binary_emitter_write_u32(file, 0) ||
        !binary_emitter_write_i16(file, (int16_t)(i + 1)) ||
        !binary_emitter_write_u16(file, 0) ||
        fwrite(&storage_class, sizeof(storage_class), 1, file) != 1 ||
        fwrite(&aux_symbols, sizeof(aux_symbols), 1, file) != 1 ||
        !binary_emitter_write_u32(file, section_length) ||
        !binary_emitter_write_u16(file, (uint16_t)section_reloc_counts[i]) ||
        !binary_emitter_write_u16(file, 0) ||
        !binary_emitter_write_u32(file, 0) ||
        !binary_emitter_write_u16(file, 0) ||
        !binary_emitter_write_zero_bytes(file, 4)) {
      binary_emitter_set_error(emitter,
                               "Failed while writing COFF section symbols");
      goto cleanup;
    }
  }

  if (!binary_emitter_write_u32(file, string_table_size)) {
    binary_emitter_set_error(emitter, "Failed while writing COFF string table");
    goto cleanup;
  }
  for (size_t i = 0; i < emitter->section_count; i++) {
    const BinarySection *section = &emitter->sections[i];
    if (section_name_offsets[i] != 0 && section->name) {
      size_t length = strlen(section->name) + 1;
      if (fwrite(section->name, 1, length, file) != length) {
        binary_emitter_set_error(emitter,
                                 "Failed while writing section name strings");
        goto cleanup;
      }
    }
  }
  for (size_t i = 0; i < emitter->symbol_count; i++) {
    const BinarySymbol *symbol = &emitter->symbols[i];
    if (symbol_name_offsets[i] != 0 && symbol->name) {
      size_t length = strlen(symbol->name) + 1;
      if (fwrite(symbol->name, 1, length, file) != length) {
        binary_emitter_set_error(emitter,
                                 "Failed while writing symbol name strings");
        goto cleanup;
      }
    }
  }

  ok = 1;

cleanup:
  if (file) {
    fclose(file);
    if (!ok) {
      remove(filename);
    }
  }
  free(section_name_offsets);
  free(symbol_name_offsets);
  free(section_raw_offsets);
  free(section_reloc_offsets);
  free(section_reloc_counts);
  free(symbol_table_indices);
  return ok;
}

void binary_emitter_record_error(BinaryEmitter *emitter, const char *message) {
  binary_emitter_set_error(emitter, message);
}

int binary_emitter_lookup_symbol_index(const BinaryEmitter *emitter,
                                       const char *name) {
  return binary_emitter_find_symbol_index(emitter, name);
}

int binary_emitter_write_object_file(BinaryEmitter *emitter,
                                     const char *filename) {
  if (!emitter || !filename || filename[0] == '\0') {
    return 0;
  }
  switch (emitter->target_format) {
  case BINARY_TARGET_FORMAT_COFF_WIN64:
    return binary_emitter_write_coff_object_file(emitter, filename);
  case BINARY_TARGET_FORMAT_ELF_X64:
    return binary_emitter_write_elf_object_file(emitter, filename);
  default:
    binary_emitter_set_error(emitter, "Unknown target object format");
    return 0;
  }
}
