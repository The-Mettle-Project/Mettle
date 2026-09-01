#include "linker/elf_shared.h"
#include "linker/link_object.h"
#include "linker/linker_common.h"
#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELF_SHARED_EHDR_SIZE 64u
#define ELF_SHARED_PHDR_SIZE 56u
#define ELF_SHARED_SHDR_SIZE 64u
#define ELF_SHARED_SYM_SIZE 24u
#define ELF_SHARED_DYN_SIZE 16u
#define ELF_SHARED_VERDEF_SIZE 20u
#define ELF_SHARED_VERDAUX_SIZE 8u

#define ELF_SHARED_ET_DYN 3u
#define ELF_SHARED_EM_X86_64 62u

#define ELF_SHARED_PT_LOAD 1u
#define ELF_SHARED_PT_DYNAMIC 2u

#define ELF_SHARED_SHT_DYNSYM 11u
#define ELF_SHARED_SHT_DYNAMIC 6u
#define ELF_SHARED_SHT_GNU_VERDEF 0x6ffffffdu
#define ELF_SHARED_SHT_GNU_VERSYM 0x6fffffffu

#define ELF_SHARED_DT_NULL 0
#define ELF_SHARED_DT_HASH 4
#define ELF_SHARED_DT_STRTAB 5
#define ELF_SHARED_DT_SYMTAB 6
#define ELF_SHARED_DT_STRSZ 10
#define ELF_SHARED_DT_SONAME 14
#define ELF_SHARED_DT_GNU_HASH 0x6ffffef5
#define ELF_SHARED_DT_VERSYM 0x6ffffff0
#define ELF_SHARED_DT_VERDEF 0x6ffffffc
#define ELF_SHARED_DT_VERDEFNUM 0x6ffffffd

#define ELF_SHARED_SHN_UNDEF 0u
#define ELF_SHARED_STB_LOCAL 0u

typedef struct {
  const unsigned char *data;
  size_t size;
  const char *origin;
  uint64_t phoff;
  uint16_t phentsize;
  uint16_t phnum;
  uint64_t shoff;
  uint16_t shentsize;
  uint64_t shnum;
} ElfSharedImage;

typedef struct {
  const unsigned char *symbols;
  size_t symbol_count;
  const char *strings;
  size_t string_size;
  const unsigned char *versym;
  size_t versym_count;
  const unsigned char *verdef;
  size_t verdef_size;
} ElfSharedTables;

static int elf_shared_range_ok(const ElfSharedImage *image, uint64_t offset,
                               uint64_t length) {
  return link_object_range_ok(image->size, offset, length);
}

static const unsigned char *elf_shared_at(const ElfSharedImage *image,
                                          uint64_t offset, uint64_t length) {
  if (!elf_shared_range_ok(image, offset, length)) {
    return NULL;
  }
  return image->data + offset;
}

static const unsigned char *elf_shared_phdr(const ElfSharedImage *image,
                                            uint64_t index) {
  return elf_shared_at(image, image->phoff + index * image->phentsize,
                       ELF_SHARED_PHDR_SIZE);
}

static const unsigned char *elf_shared_shdr(const ElfSharedImage *image,
                                            uint64_t index) {
  return elf_shared_at(image, image->shoff + index * image->shentsize,
                       ELF_SHARED_SHDR_SIZE);
}

static int elf_shared_offset_for_address(const ElfSharedImage *image,
                                         uint64_t address, uint64_t *offset_out) {
  uint64_t i = 0u;

  for (i = 0u; i < image->phnum; i++) {
    const unsigned char *phdr = elf_shared_phdr(image, i);
    uint64_t vaddr = 0u;
    uint64_t filesz = 0u;

    if (!phdr || linker_read_u32(phdr) != ELF_SHARED_PT_LOAD) {
      continue;
    }
    vaddr = linker_read_u64(phdr + 16);
    filesz = linker_read_u64(phdr + 32);
    if (address >= vaddr && address - vaddr < filesz) {
      *offset_out = linker_read_u64(phdr + 8) + (address - vaddr);
      return 1;
    }
  }
  return 0;
}

static const char *elf_shared_string(const char *table, size_t table_size,
                                     uint32_t offset) {
  size_t i = 0u;

  if (!table || offset >= table_size) {
    return NULL;
  }
  for (i = offset; i < table_size; i++) {
    if (table[i] == '\0') {
      return table + offset;
    }
  }
  return NULL;
}

static int elf_shared_open_image(const unsigned char *data, size_t size,
                                 const char *origin, ElfSharedImage *image,
                                 char **error_message_out) {
  memset(image, 0, sizeof(*image));
  image->data = data;
  image->size = size;
  image->origin = origin;

  if (size < ELF_SHARED_EHDR_SIZE || data[0] != 0x7Fu || data[1] != 'E' ||
      data[2] != 'L' || data[3] != 'F') {
    mettle_set_error(error_message_out, "'%s' is not an ELF file", origin);
    return 0;
  }
  if (data[4] != 2u || data[5] != 1u) {
    mettle_set_error(error_message_out,
                     "'%s' is not a little endian 64-bit ELF file", origin);
    return 0;
  }
  if (linker_read_u16(data + 16) != ELF_SHARED_ET_DYN) {
    mettle_set_error(error_message_out,
                     "'%s' is not a shared object; the ELF type is %u, not "
                     "ET_DYN",
                     origin, (unsigned)linker_read_u16(data + 16));
    return 0;
  }
  if (linker_read_u16(data + 18) != ELF_SHARED_EM_X86_64) {
    mettle_set_error(error_message_out,
                     "Shared object '%s' is built for machine %u; this linker "
                     "emits x86-64 images",
                     origin, (unsigned)linker_read_u16(data + 18));
    return 0;
  }

  image->phoff = linker_read_u64(data + 32);
  image->phentsize = linker_read_u16(data + 54);
  image->phnum = linker_read_u16(data + 56);
  image->shoff = linker_read_u64(data + 40);
  image->shentsize = linker_read_u16(data + 58);
  image->shnum = linker_read_u16(data + 60);

  if (image->phentsize != 0u && image->phentsize < ELF_SHARED_PHDR_SIZE) {
    mettle_set_error(error_message_out,
                     "Shared object '%s' has undersized program headers",
                     origin);
    return 0;
  }
  if (image->shentsize != 0u && image->shentsize < ELF_SHARED_SHDR_SIZE) {
    mettle_set_error(error_message_out,
                     "Shared object '%s' has undersized section headers",
                     origin);
    return 0;
  }
  if (image->phentsize == 0u) {
    image->phnum = 0u;
  }
  if (image->shentsize == 0u) {
    image->shnum = 0u;
  }
  if (image->shnum == 0u && image->shoff != 0u && image->shentsize != 0u) {
    const unsigned char *first = elf_shared_shdr(image, 0u);
    if (first) {
      image->shnum = linker_read_u64(first + 32);
    }
  }
  if (image->phnum != 0u &&
      !elf_shared_range_ok(image, image->phoff,
                           (uint64_t)image->phnum * image->phentsize)) {
    mettle_set_error(error_message_out,
                     "Shared object '%s' has a truncated program header table",
                     origin);
    return 0;
  }
  if (image->shnum != 0u &&
      !elf_shared_range_ok(image, image->shoff,
                           image->shnum * image->shentsize)) {
    image->shnum = 0u;
  }
  return 1;
}

static const unsigned char *elf_shared_dynamic_segment(
    const ElfSharedImage *image, size_t *count_out) {
  uint64_t i = 0u;

  *count_out = 0u;
  for (i = 0u; i < image->phnum; i++) {
    const unsigned char *phdr = elf_shared_phdr(image, i);
    uint64_t offset = 0u;
    uint64_t size = 0u;

    if (!phdr || linker_read_u32(phdr) != ELF_SHARED_PT_DYNAMIC) {
      continue;
    }
    offset = linker_read_u64(phdr + 8);
    size = linker_read_u64(phdr + 32);
    if (!elf_shared_range_ok(image, offset, size)) {
      return NULL;
    }
    *count_out = (size_t)(size / ELF_SHARED_DYN_SIZE);
    return image->data + offset;
  }

  for (i = 0u; i < image->shnum; i++) {
    const unsigned char *shdr = elf_shared_shdr(image, i);
    uint64_t offset = 0u;
    uint64_t size = 0u;

    if (!shdr || linker_read_u32(shdr + 4) != ELF_SHARED_SHT_DYNAMIC) {
      continue;
    }
    offset = linker_read_u64(shdr + 24);
    size = linker_read_u64(shdr + 32);
    if (!elf_shared_range_ok(image, offset, size)) {
      return NULL;
    }
    *count_out = (size_t)(size / ELF_SHARED_DYN_SIZE);
    return image->data + offset;
  }
  return NULL;
}

static int elf_shared_dynamic_value(const unsigned char *dynamic, size_t count,
                                    int64_t tag, uint64_t *value_out) {
  size_t i = 0u;

  for (i = 0u; i < count; i++) {
    int64_t entry_tag = (int64_t)linker_read_u64(dynamic + i * ELF_SHARED_DYN_SIZE);
    if (entry_tag == ELF_SHARED_DT_NULL) {
      break;
    }
    if (entry_tag == tag) {
      *value_out = linker_read_u64(dynamic + i * ELF_SHARED_DYN_SIZE + 8);
      return 1;
    }
  }
  return 0;
}

static uint32_t elf_shared_gnu_hash_symbol_count(const unsigned char *table,
                                                 size_t available) {
  uint32_t bucket_count = 0u;
  uint32_t symbol_offset = 0u;
  uint32_t bloom_size = 0u;
  const unsigned char *buckets = NULL;
  const unsigned char *chain = NULL;
  uint32_t last = 0u;
  uint32_t i = 0u;
  size_t header = 0u;

  if (available < 16u) {
    return 0u;
  }
  bucket_count = linker_read_u32(table);
  symbol_offset = linker_read_u32(table + 4);
  bloom_size = linker_read_u32(table + 8);
  header = 16u + (size_t)bloom_size * 8u;
  if (bucket_count == 0u || available < header + (size_t)bucket_count * 4u) {
    return 0u;
  }
  buckets = table + header;
  chain = buckets + (size_t)bucket_count * 4u;
  for (i = 0u; i < bucket_count; i++) {
    uint32_t value = linker_read_u32(buckets + i * 4u);
    if (value > last) {
      last = value;
    }
  }
  if (last < symbol_offset) {
    return symbol_offset;
  }
  i = last - symbol_offset;
  while ((size_t)(chain - table) + (size_t)i * 4u + 4u <= available) {
    if (linker_read_u32(chain + (size_t)i * 4u) & 1u) {
      return symbol_offset + i + 1u;
    }
    i++;
  }
  return 0u;
}

static int elf_shared_locate_tables(const ElfSharedImage *image,
                                    ElfSharedTables *tables) {
  uint64_t i = 0u;
  const unsigned char *dynamic = NULL;
  size_t dynamic_count = 0u;
  uint64_t address = 0u;
  uint64_t offset = 0u;
  uint64_t size = 0u;

  memset(tables, 0, sizeof(*tables));

  for (i = 0u; i < image->shnum; i++) {
    const unsigned char *shdr = elf_shared_shdr(image, i);
    uint32_t type = 0u;
    uint64_t section_offset = 0u;
    uint64_t section_size = 0u;

    if (!shdr) {
      continue;
    }
    type = linker_read_u32(shdr + 4);
    section_offset = linker_read_u64(shdr + 24);
    section_size = linker_read_u64(shdr + 32);
    if (!elf_shared_range_ok(image, section_offset, section_size)) {
      continue;
    }
    if (type == ELF_SHARED_SHT_DYNSYM) {
      uint32_t link = linker_read_u32(shdr + 40);
      const unsigned char *strings = elf_shared_shdr(image, link);
      uint64_t entry_size = linker_read_u64(shdr + 56);

      if (entry_size != ELF_SHARED_SYM_SIZE || !strings) {
        continue;
      }
      tables->symbols = image->data + section_offset;
      tables->symbol_count = (size_t)(section_size / ELF_SHARED_SYM_SIZE);
      section_offset = linker_read_u64(strings + 24);
      section_size = linker_read_u64(strings + 32);
      if (!elf_shared_range_ok(image, section_offset, section_size)) {
        tables->symbols = NULL;
        tables->symbol_count = 0u;
        continue;
      }
      tables->strings = (const char *)image->data + section_offset;
      tables->string_size = (size_t)section_size;
    } else if (type == ELF_SHARED_SHT_GNU_VERSYM) {
      tables->versym = image->data + section_offset;
      tables->versym_count = (size_t)(section_size / 2u);
    } else if (type == ELF_SHARED_SHT_GNU_VERDEF) {
      tables->verdef = image->data + section_offset;
      tables->verdef_size = (size_t)section_size;
    }
  }

  if (tables->symbols && tables->strings) {
    return 1;
  }

  dynamic = elf_shared_dynamic_segment(image, &dynamic_count);
  if (!dynamic) {
    return 0;
  }
  if (!elf_shared_dynamic_value(dynamic, dynamic_count, ELF_SHARED_DT_SYMTAB,
                                &address) ||
      !elf_shared_offset_for_address(image, address, &offset)) {
    return 0;
  }
  tables->symbols = image->data + offset;
  if (!elf_shared_dynamic_value(dynamic, dynamic_count, ELF_SHARED_DT_STRTAB,
                                &address) ||
      !elf_shared_offset_for_address(image, address, &offset) ||
      !elf_shared_dynamic_value(dynamic, dynamic_count, ELF_SHARED_DT_STRSZ,
                                &size) ||
      !elf_shared_range_ok(image, offset, size)) {
    tables->symbols = NULL;
    return 0;
  }
  tables->strings = (const char *)image->data + offset;
  tables->string_size = (size_t)size;

  if (elf_shared_dynamic_value(dynamic, dynamic_count, ELF_SHARED_DT_HASH,
                               &address) &&
      elf_shared_offset_for_address(image, address, &offset) &&
      elf_shared_range_ok(image, offset, 8u)) {
    tables->symbol_count = linker_read_u32(image->data + offset + 4u);
  } else if (elf_shared_dynamic_value(dynamic, dynamic_count,
                                      ELF_SHARED_DT_GNU_HASH, &address) &&
             elf_shared_offset_for_address(image, address, &offset)) {
    tables->symbol_count = elf_shared_gnu_hash_symbol_count(
        image->data + offset, image->size - (size_t)offset);
  }
  if (tables->symbol_count == 0u ||
      !elf_shared_range_ok(image, (uint64_t)(tables->symbols - image->data),
                           (uint64_t)tables->symbol_count *
                               ELF_SHARED_SYM_SIZE)) {
    tables->symbols = NULL;
    tables->symbol_count = 0u;
    return 0;
  }

  if (elf_shared_dynamic_value(dynamic, dynamic_count, ELF_SHARED_DT_VERSYM,
                               &address) &&
      elf_shared_offset_for_address(image, address, &offset) &&
      elf_shared_range_ok(image, offset,
                          (uint64_t)tables->symbol_count * 2u)) {
    tables->versym = image->data + offset;
    tables->versym_count = tables->symbol_count;
  }
  if (elf_shared_dynamic_value(dynamic, dynamic_count, ELF_SHARED_DT_VERDEF,
                               &address) &&
      elf_shared_offset_for_address(image, address, &offset)) {
    tables->verdef = image->data + offset;
    tables->verdef_size = image->size - (size_t)offset;
  }
  return 1;
}

static const char *elf_shared_version_name(const ElfSharedTables *tables,
                                           uint16_t index) {
  size_t cursor = 0u;

  if (!tables->verdef || index <= 1u) {
    return NULL;
  }
  while (cursor + ELF_SHARED_VERDEF_SIZE <= tables->verdef_size) {
    const unsigned char *entry = tables->verdef + cursor;
    uint16_t entry_index = linker_read_u16(entry + 4);
    uint32_t aux = linker_read_u32(entry + 12);
    uint32_t next = linker_read_u32(entry + 16);

    if (entry_index == index && aux != 0u &&
        cursor + aux + ELF_SHARED_VERDAUX_SIZE <= tables->verdef_size) {
      return elf_shared_string(tables->strings, tables->string_size,
                               linker_read_u32(entry + aux));
    }
    if (next == 0u) {
      break;
    }
    cursor += next;
  }
  return NULL;
}

static int elf_shared_library_reserve(ElfSharedLibrary *library,
                                      size_t minimum_count) {
  ElfSharedSymbol *grown = NULL;
  size_t capacity = library->symbol_capacity;

  if (capacity >= minimum_count) {
    return 1;
  }
  capacity = capacity ? capacity : 64u;
  while (capacity < minimum_count) {
    capacity *= 2u;
  }
  grown = (ElfSharedSymbol *)realloc(library->symbols,
                                     capacity * sizeof(ElfSharedSymbol));
  if (!grown) {
    return 0;
  }
  memset(grown + library->symbol_capacity, 0,
         (capacity - library->symbol_capacity) * sizeof(ElfSharedSymbol));
  library->symbols = grown;
  library->symbol_capacity = capacity;
  return 1;
}

static int elf_shared_library_index(ElfSharedLibrary *library) {
  size_t buckets = 64u;
  size_t i = 0u;

  while (buckets < library->symbol_count * 2u) {
    buckets *= 2u;
  }
  free(library->buckets);
  library->buckets = (size_t *)calloc(buckets, sizeof(size_t));
  if (!library->buckets) {
    library->bucket_count = 0u;
    return 0;
  }
  library->bucket_count = buckets;
  for (i = 0u; i < library->symbol_count; i++) {
    size_t slot = 0u;

    if (!library->symbols[i].name) {
      continue;
    }
    slot = mettle_fnv1a_hash(library->symbols[i].name) & (buckets - 1u);
    while (library->buckets[slot]) {
      slot = (slot + 1u) & (buckets - 1u);
    }
    library->buckets[slot] = i + 1u;
  }
  return 1;
}

static int elf_shared_collect_symbols(const ElfSharedImage *image,
                                      const ElfSharedTables *tables,
                                      ElfSharedLibrary *library,
                                      char **error_message_out) {
  size_t i = 0u;

  (void)image;
  for (i = 1u; i < tables->symbol_count; i++) {
    const unsigned char *entry = tables->symbols + i * ELF_SHARED_SYM_SIZE;
    unsigned char info = entry[4];
    uint16_t shndx = linker_read_u16(entry + 6);
    const char *name = NULL;
    ElfSharedSymbol *symbol = NULL;
    uint16_t version_index = 0u;

    if (shndx == ELF_SHARED_SHN_UNDEF) {
      continue;
    }
    if ((info >> 4) == ELF_SHARED_STB_LOCAL) {
      continue;
    }
    name = elf_shared_string(tables->strings, tables->string_size,
                             linker_read_u32(entry));
    if (!name || name[0] == '\0') {
      continue;
    }
    if (tables->versym && i < tables->versym_count) {
      version_index = linker_read_u16(tables->versym + i * 2u);
      if (version_index & 0x8000u) {
        continue;
      }
    }

    if (!elf_shared_library_reserve(library, library->symbol_count + 1u)) {
      mettle_set_error(error_message_out,
                       "Out of memory while reading symbols from '%s'",
                       library->path);
      return 0;
    }
    symbol = &library->symbols[library->symbol_count];
    memset(symbol, 0, sizeof(*symbol));
    symbol->name = mettle_strdup(name);
    if (!symbol->name) {
      mettle_set_error(error_message_out,
                       "Out of memory while reading symbols from '%s'",
                       library->path);
      return 0;
    }
    symbol->type = (uint8_t)(info & 0x0Fu);
    symbol->is_weak = (info >> 4) == 2u;
    symbol->size = linker_read_u64(entry + 16);
    if (version_index > 1u) {
      const char *version = elf_shared_version_name(tables, version_index);
      if (version) {
        symbol->version = mettle_strdup(version);
        if (!symbol->version) {
          mettle_set_error(error_message_out,
                           "Out of memory while reading symbols from '%s'",
                           library->path);
          return 0;
        }
      }
    }
    library->symbol_count++;
  }
  return 1;
}

static char *elf_shared_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *back = strrchr(path, '\\');

  if (back && (!slash || back > slash)) {
    slash = back;
  }
  return mettle_strdup(slash ? slash + 1 : path);
}

static int elf_shared_read_soname(const ElfSharedImage *image,
                                  const ElfSharedTables *tables,
                                  ElfSharedLibrary *library) {
  const unsigned char *dynamic = NULL;
  size_t dynamic_count = 0u;
  uint64_t value = 0u;
  const char *name = NULL;

  dynamic = elf_shared_dynamic_segment(image, &dynamic_count);
  if (dynamic &&
      elf_shared_dynamic_value(dynamic, dynamic_count, ELF_SHARED_DT_SONAME,
                               &value) &&
      value <= UINT32_MAX) {
    name = elf_shared_string(tables->strings, tables->string_size,
                             (uint32_t)value);
  }
  library->soname = name ? mettle_strdup(name)
                         : elf_shared_basename(library->path);
  return library->soname != NULL;
}

static int elf_shared_script_target(const unsigned char *data, size_t size,
                                    char **path_out) {
  char *text = NULL;
  char *cursor = NULL;
  size_t i = 0u;
  int depth = 0;

  *path_out = NULL;
  text = (char *)malloc(size + 1u);
  if (!text) {
    return 0;
  }
  memcpy(text, data, size);
  text[size] = '\0';
  for (i = 0u; i + 1u < size; i++) {
    if (text[i] == '/' && text[i + 1u] == '*') {
      size_t j = i + 2u;
      while (j + 1u < size && !(text[j] == '*' && text[j + 1u] == '/')) {
        text[j++] = ' ';
      }
      text[i] = ' ';
      text[i + 1u] = ' ';
      if (j + 1u < size) {
        text[j] = ' ';
        text[j + 1u] = ' ';
      }
      i = j;
    }
  }

  cursor = text;
  while (*cursor) {
    char *token = cursor;
    size_t length = 0u;

    while (*cursor && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' ||
                       *cursor == '\r')) {
      cursor++;
    }
    token = cursor;
    if (*cursor == '(') {
      depth++;
      cursor++;
      continue;
    }
    if (*cursor == ')') {
      depth--;
      cursor++;
      continue;
    }
    while (cursor[length] && cursor[length] != ' ' && cursor[length] != '\t' &&
           cursor[length] != '\n' && cursor[length] != '\r' &&
           cursor[length] != '(' && cursor[length] != ')') {
      length++;
    }
    if (length == 0u) {
      if (!*cursor) {
        break;
      }
      cursor++;
      continue;
    }
    cursor += length;
    if (depth > 0 && token[0] == '/' &&
        !(length > 2u && token[length - 2u] == '.' &&
          token[length - 1u] == 'a')) {
      char *candidate = (char *)malloc(length + 1u);
      if (!candidate) {
        free(text);
        return 0;
      }
      memcpy(candidate, token, length);
      candidate[length] = '\0';
      if (elf_path_is_shared_library(candidate)) {
        *path_out = candidate;
        free(text);
        return 1;
      }
      free(candidate);
    }
  }
  free(text);
  return 0;
}

int elf_path_is_shared_library(const char *path) {
  FILE *file = NULL;
  unsigned char header[18];
  size_t read = 0u;

  if (!path) {
    return 0;
  }
  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  read = fread(header, 1u, sizeof(header), file);
  fclose(file);
  if (read < sizeof(header)) {
    return 0;
  }
  if (header[0] != 0x7Fu || header[1] != 'E' || header[2] != 'L' ||
      header[3] != 'F') {
    return 0;
  }
  return linker_read_u16(header + 16) == ELF_SHARED_ET_DYN;
}

int elf_shared_library_read(const char *path, ElfSharedLibrary **library_out,
                            char **error_message_out) {
  unsigned char *data = NULL;
  size_t size = 0u;
  ElfSharedImage image;
  ElfSharedTables tables;
  ElfSharedLibrary *library = NULL;
  char *script_target = NULL;

  if (library_out) {
    *library_out = NULL;
  }
  if (!path || !library_out) {
    mettle_set_error(error_message_out,
                     "Invalid arguments while reading a shared library");
    return 0;
  }
  if (!link_object_read_file(path, &data, &size, error_message_out)) {
    return 0;
  }
  if (size < 4u || data[0] != 0x7Fu || data[1] != 'E' || data[2] != 'L' ||
      data[3] != 'F') {
    if (!elf_shared_script_target(data, size, &script_target)) {
      free(data);
      mettle_set_error(error_message_out,
                       "'%s' is neither a shared object nor a linker script "
                       "naming one",
                       path);
      return 0;
    }
    free(data);
    if (!elf_shared_library_read(script_target, &library, error_message_out)) {
      free(script_target);
      return 0;
    }
    free(script_target);
    *library_out = library;
    return 1;
  }

  if (!elf_shared_open_image(data, size, path, &image, error_message_out)) {
    free(data);
    return 0;
  }

  library = (ElfSharedLibrary *)calloc(1u, sizeof(ElfSharedLibrary));
  if (!library) {
    free(data);
    mettle_set_error(error_message_out,
                     "Out of memory while loading shared library '%s'", path);
    return 0;
  }
  library->path = mettle_strdup(path);
  if (!library->path) {
    free(data);
    elf_shared_library_destroy(library);
    mettle_set_error(error_message_out,
                     "Out of memory while loading shared library '%s'", path);
    return 0;
  }

  if (!elf_shared_locate_tables(&image, &tables)) {
    free(data);
    elf_shared_library_destroy(library);
    mettle_set_error(error_message_out,
                     "Shared object '%s' has no readable dynamic symbol table",
                     path);
    return 0;
  }
  if (!elf_shared_read_soname(&image, &tables, library) ||
      !elf_shared_collect_symbols(&image, &tables, library,
                                  error_message_out) ||
      !elf_shared_library_index(library)) {
    free(data);
    elf_shared_library_destroy(library);
    if (error_message_out && !*error_message_out) {
      mettle_set_error(error_message_out,
                       "Out of memory while loading shared library '%s'", path);
    }
    return 0;
  }

  free(data);
  *library_out = library;
  return 1;
}

void elf_shared_library_destroy(ElfSharedLibrary *library) {
  size_t i = 0u;

  if (!library) {
    return;
  }
  for (i = 0u; i < library->symbol_count; i++) {
    free(library->symbols[i].name);
    free(library->symbols[i].version);
  }
  free(library->symbols);
  free(library->buckets);
  free(library->soname);
  free(library->path);
  free(library);
}

const ElfSharedSymbol *elf_shared_library_find(const ElfSharedLibrary *library,
                                               const char *name) {
  size_t slot = 0u;

  if (!library || !name || library->bucket_count == 0u) {
    return NULL;
  }
  slot = mettle_fnv1a_hash(name) & (library->bucket_count - 1u);
  while (library->buckets[slot]) {
    const ElfSharedSymbol *symbol = &library->symbols[library->buckets[slot] - 1u];
    if (symbol->name && strcmp(symbol->name, name) == 0) {
      return symbol;
    }
    slot = (slot + 1u) & (library->bucket_count - 1u);
  }
  return NULL;
}

static const char *const ELF_SHARED_DEFAULT_DIRECTORIES[] = {
    "/usr/local/lib/x86_64-linux-gnu",
    "/usr/local/lib64",
    "/usr/local/lib",
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib64",
    "/usr/lib",
    "/lib/x86_64-linux-gnu",
    "/lib64",
    "/lib",
};

static char *elf_shared_join(const char *directory, const char *file) {
  size_t directory_length = strlen(directory);
  size_t file_length = strlen(file);
  int needs_slash = directory_length != 0u &&
                    directory[directory_length - 1u] != '/';
  char *out = (char *)malloc(directory_length + (size_t)needs_slash +
                             file_length + 1u);

  if (!out) {
    return NULL;
  }
  memcpy(out, directory, directory_length);
  if (needs_slash) {
    out[directory_length] = '/';
  }
  memcpy(out + directory_length + (size_t)needs_slash, file, file_length + 1u);
  return out;
}

static char *elf_shared_probe(const char *directory, const char *file) {
  char *candidate = elf_shared_join(directory, file);
  FILE *handle = NULL;

  if (!candidate) {
    return NULL;
  }
  handle = fopen(candidate, "rb");
  if (!handle) {
    free(candidate);
    return NULL;
  }
  fclose(handle);
  return candidate;
}

char *elf_shared_library_locate(const char *library_name,
                                const char *const *directories,
                                size_t directory_count,
                                char **error_message_out) {
  char file[512];
  size_t i = 0u;
  char *found = NULL;

  if (!library_name || library_name[0] == '\0') {
    mettle_set_error(error_message_out, "An empty library name was requested");
    return NULL;
  }
  if (library_name[0] == ':') {
    if (snprintf(file, sizeof(file), "%s", library_name + 1) >= (int)sizeof(file)) {
      mettle_set_error(error_message_out, "Library name '%s' is too long",
                       library_name);
      return NULL;
    }
  } else if (snprintf(file, sizeof(file), "lib%s.so", library_name) >=
             (int)sizeof(file)) {
    mettle_set_error(error_message_out, "Library name '%s' is too long",
                     library_name);
    return NULL;
  }

  for (i = 0u; i < directory_count; i++) {
    if (!directories[i]) {
      continue;
    }
    found = elf_shared_probe(directories[i], file);
    if (found) {
      return found;
    }
  }
  for (i = 0u; i < sizeof(ELF_SHARED_DEFAULT_DIRECTORIES) /
                   sizeof(ELF_SHARED_DEFAULT_DIRECTORIES[0]);
       i++) {
    found = elf_shared_probe(ELF_SHARED_DEFAULT_DIRECTORIES[i], file);
    if (found) {
      return found;
    }
  }

  mettle_set_error(error_message_out,
                   "Could not find shared library '%s' for -l%s on the library "
                   "search path",
                   file, library_name);
  return NULL;
}
