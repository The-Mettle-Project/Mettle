#include "linker/elf_dynamic.h"
#include "linker/elf_image.h"
#include "linker/linker_common.h"
#include "linker/relocation.h"
#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#define ELF_EHDR_SIZE 64u
#define ELF_PHDR_SIZE 56u
#define ELF_SHDR_SIZE 64u
#define ELF_SYM_SIZE 24u

#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_TLS 7u
#define PT_GNU_STACK 0x6474e551u
#define PT_GNU_RELRO 0x6474e552u

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

#define SHT_PROGBITS 1u
#define SHT_SYMTAB 2u
#define SHT_STRTAB 3u
#define SHT_NOBITS 8u
#define ET_EXEC 2u
#define ET_DYN 3u

#define SHF_WRITE 1u
#define SHF_ALLOC 2u
#define SHF_EXECINSTR 4u
#define SHF_TLS 1024u

#define STB_LOCAL 0u
#define STB_GLOBAL 1u
#define STT_NOTYPE 0u

#define ELF_IMAGE_MAX_PARTS LINKED_SECTION_COUNT

typedef struct {
  size_t merged_index;
  const char *name;
  uint64_t virtual_address;
  uint64_t file_offset;
  uint64_t file_size;
  uint64_t memory_size;
  uint64_t alignment;
  uint32_t section_type;
  uint64_t section_flags;
  int is_tls;
  uint16_t section_header_index;
} ElfImagePart;

typedef struct {
  unsigned char *data;
  size_t size;
  size_t capacity;
} ElfImageBuffer;

static void elf_image_buffer_free(ElfImageBuffer *buffer) {
  if (!buffer) {
    return;
  }
  free(buffer->data);
  buffer->data = NULL;
  buffer->size = 0u;
  buffer->capacity = 0u;
}

static int elf_image_buffer_reserve(ElfImageBuffer *buffer, size_t needed) {
  size_t capacity = 0u;
  unsigned char *grown = NULL;

  if (needed <= buffer->capacity) {
    return 1;
  }
  capacity = buffer->capacity ? buffer->capacity : 4096u;
  while (capacity < needed) {
    capacity *= 2u;
  }
  grown = (unsigned char *)realloc(buffer->data, capacity);
  if (!grown) {
    return 0;
  }
  memset(grown + buffer->size, 0, capacity - buffer->size);
  buffer->data = grown;
  buffer->capacity = capacity;
  return 1;
}

static int elf_image_buffer_pad_to(ElfImageBuffer *buffer, size_t offset) {
  if (offset <= buffer->size) {
    return 1;
  }
  if (!elf_image_buffer_reserve(buffer, offset)) {
    return 0;
  }
  memset(buffer->data + buffer->size, 0, offset - buffer->size);
  buffer->size = offset;
  return 1;
}

static int elf_image_buffer_write(ElfImageBuffer *buffer, size_t offset,
                                  const void *bytes, size_t length) {
  if (length == 0u) {
    return 1;
  }
  if (!elf_image_buffer_reserve(buffer, offset + length)) {
    return 0;
  }
  memcpy(buffer->data + offset, bytes, length);
  if (offset + length > buffer->size) {
    buffer->size = offset + length;
  }
  return 1;
}

static void elf_image_put_u16(unsigned char *data, uint16_t value) {
  data[0] = (unsigned char)(value & 0xFFu);
  data[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static uint64_t elf_image_section_memory_size(const LinkedSection *section) {
  uint64_t size = (uint64_t)section->size;
  if ((uint64_t)section->virtual_size > size) {
    size = (uint64_t)section->virtual_size;
  }
  return size;
}

static int elf_image_section_has_content(const LinkedSection *section) {
  return section && elf_image_section_memory_size(section) != 0u;
}

static uint32_t elf_image_flags_for_kind(LinkSectionKind kind) {
  switch (kind) {
  case LINK_SECTION_KIND_TEXT:
    return PF_R | PF_X;
  case LINK_SECTION_KIND_RDATA:
  case LINK_SECTION_KIND_PDATA:
  case LINK_SECTION_KIND_XDATA:
    return PF_R;
  default:
    return PF_R | PF_W;
  }
}

static uint64_t elf_image_section_flags_for_kind(LinkSectionKind kind) {
  switch (kind) {
  case LINK_SECTION_KIND_TEXT:
    return SHF_ALLOC | SHF_EXECINSTR;
  case LINK_SECTION_KIND_RDATA:
  case LINK_SECTION_KIND_PDATA:
  case LINK_SECTION_KIND_XDATA:
    return SHF_ALLOC;
  case LINK_SECTION_KIND_TLS:
    return SHF_ALLOC | SHF_WRITE | SHF_TLS;
  default:
    return SHF_ALLOC | SHF_WRITE;
  }
}

static int elf_image_kind_is_nobits(LinkSectionKind kind) {
  return kind == LINK_SECTION_KIND_BSS || kind == LINK_SECTION_KIND_TLS;
}

static const char *elf_image_section_name(LinkSectionKind kind) {
  switch (kind) {
  case LINK_SECTION_KIND_TEXT:
    return ".text";
  case LINK_SECTION_KIND_RDATA:
    return ".rodata";
  case LINK_SECTION_KIND_DATA:
    return ".data";
  case LINK_SECTION_KIND_BSS:
    return ".bss";
  case LINK_SECTION_KIND_TLS:
    return ".tbss";
  case LINK_SECTION_KIND_PDATA:
    return ".pdata";
  case LINK_SECTION_KIND_XDATA:
    return ".xdata";
  default:
    return ".data";
  }
}

static const LinkSectionKind ELF_IMAGE_ORDER[ELF_IMAGE_MAX_PARTS] = {
    LINK_SECTION_KIND_TEXT,  LINK_SECTION_KIND_RDATA,
    LINK_SECTION_KIND_PDATA, LINK_SECTION_KIND_XDATA,
    LINK_SECTION_KIND_TLS,   LINK_SECTION_KIND_DATA,
    LINK_SECTION_KIND_BSS};

static size_t elf_image_merged_index(const LinkResolution *resolution,
                                     LinkSectionKind kind) {
  size_t i = 0u;
  for (i = 0u; i < LINKED_SECTION_COUNT; i++) {
    if (resolution->sections[i].kind == kind) {
      return i;
    }
  }
  return LINKED_SECTION_INDEX_NONE;
}

static void elf_image_normalize_tls(LinkResolution *resolution) {
  size_t i = 0u;

  for (i = 0u; i < LINKED_SECTION_COUNT; i++) {
    LinkedSection *section = &resolution->sections[i];
    size_t alignment = 0u;
    size_t size = 0u;

    if (section->kind != LINK_SECTION_KIND_TLS) {
      continue;
    }
    size = section->virtual_size > section->size ? section->virtual_size
                                                 : section->size;
    if (size == 0u) {
      continue;
    }
    alignment = section->alignment > 16u ? section->alignment : 16u;
    section->alignment = alignment;
    section->virtual_size = linker_align_up(size, alignment);
  }
}

static int elf_image_collect_parts(LinkResolution *resolution,
                                   ElfImagePart *parts, size_t *part_count_out,
                                   size_t *read_only_count_out) {
  size_t count = 0u;
  size_t read_only = 0u;
  size_t i = 0u;

  for (i = 0u; i < ELF_IMAGE_MAX_PARTS; i++) {
    LinkSectionKind kind = ELF_IMAGE_ORDER[i];
    size_t merged = elf_image_merged_index(resolution, kind);
    LinkedSection *section = NULL;
    ElfImagePart *part = NULL;

    if (merged == LINKED_SECTION_INDEX_NONE) {
      continue;
    }
    section = &resolution->sections[merged];
    if (!elf_image_section_has_content(section)) {
      continue;
    }

    part = &parts[count];
    part->merged_index = merged;
    part->name = elf_image_section_name(kind);
    part->alignment = section->alignment ? (uint64_t)section->alignment : 1u;
    part->memory_size = elf_image_section_memory_size(section);
    part->file_size = elf_image_kind_is_nobits(kind) ? 0u : (uint64_t)section->size;
    part->section_type =
        elf_image_kind_is_nobits(kind) ? SHT_NOBITS : SHT_PROGBITS;
    part->section_flags = elf_image_section_flags_for_kind(kind);
    part->is_tls = kind == LINK_SECTION_KIND_TLS;
    part->virtual_address = 0u;
    part->file_offset = 0u;
    part->section_header_index = 0u;
    if ((elf_image_flags_for_kind(kind) & PF_W) == 0u) {
      read_only++;
    }
    count++;
  }

  *part_count_out = count;
  *read_only_count_out = read_only;
  return 1;
}

static void elf_image_assign_addresses(ElfImagePart *parts, size_t part_count,
                                       size_t read_only_count,
                                       uint64_t image_base, uint64_t page_size,
                                       uint64_t headers_size,
                                       uint64_t *read_only_end_out,
                                       uint64_t *read_only_file_end_out) {
  uint64_t offset = headers_size;
  uint64_t delta = image_base;
  uint64_t address = offset + delta;
  size_t i = 0u;

  for (i = 0u; i < part_count; i++) {
    uint64_t alignment = parts[i].alignment ? parts[i].alignment : 1u;

    if (i == read_only_count && read_only_count != part_count) {
      *read_only_end_out = address;
      *read_only_file_end_out = offset;
      offset = linker_align_up((size_t)offset, (size_t)alignment);
      delta = linker_align_up((size_t)address, (size_t)page_size) + page_size -
              (offset - offset % page_size);
    }

    offset = linker_align_up((size_t)offset, (size_t)alignment);
    address = offset + delta;
    parts[i].virtual_address = address;
    parts[i].file_offset = offset;

    if (parts[i].file_size != 0u) {
      offset += parts[i].file_size;
    }
    if (!parts[i].is_tls) {
      address += parts[i].memory_size;
    }
  }

  if (read_only_count == part_count) {
    *read_only_end_out = address;
    *read_only_file_end_out = offset;
  }
}

static int elf_image_apply_layout(LinkResolution *resolution,
                                  const ElfImagePart *parts, size_t part_count,
                                  char **error_message_out) {
  size_t i = 0u;
  size_t object_index = 0u;
  size_t symbol_index = 0u;

  for (i = 0u; i < LINKED_SECTION_COUNT; i++) {
    resolution->sections[i].virtual_address = 0u;
  }
  for (i = 0u; i < part_count; i++) {
    resolution->sections[parts[i].merged_index].virtual_address =
        parts[i].virtual_address;
  }

  for (object_index = 0u; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];

    for (symbol_index = 0u; symbol_index < input->symbol_count;
         symbol_index++) {
      LinkedObjectSymbol *symbol = &input->symbols[symbol_index];

      if (!symbol->is_defined ||
          symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
        continue;
      }
      symbol->virtual_address =
          resolution->sections[symbol->merged_section_index].virtual_address +
          (uint64_t)symbol->merged_offset;
    }
  }

  for (symbol_index = 0u; symbol_index < resolution->symbol_count;
       symbol_index++) {
    LinkedSymbol *symbol = &resolution->symbols[symbol_index];

    if (!symbol->is_defined ||
        symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
      continue;
    }
    symbol->virtual_address =
        resolution->sections[symbol->merged_section_index].virtual_address +
        (uint64_t)symbol->merged_offset;
  }

  (void)error_message_out;
  return 1;
}

static int elf_image_blob_location(const ElfImagePart *parts,
                                   size_t part_count,
                                   const ElfDynamicBlob *blob,
                                   uint64_t *offset_out,
                                   uint64_t *address_out) {
  size_t i = 0u;

  if (!blob) {
    return 0;
  }
  for (i = 0u; i < part_count; i++) {
    if (parts[i].merged_index == blob->merged_section_index) {
      *offset_out = parts[i].file_offset + blob->offset;
      *address_out = parts[i].virtual_address + blob->offset;
      return 1;
    }
  }
  return 0;
}

static int elf_image_write_program_headers(ElfImageBuffer *image,
                                           const ElfImagePart *parts,
                                           size_t part_count,
                                           size_t read_only_count,
                                           uint64_t image_base,
                                           uint64_t page_size,
                                           uint64_t read_only_end,
                                           uint64_t read_only_file_end,
                                           const ElfDynamicPlan *plan,
                                           uint16_t *phnum_out) {
  unsigned char phdr[ELF_PHDR_SIZE];
  size_t offset = ELF_EHDR_SIZE;
  uint16_t count = 0u;
  size_t i = 0u;

  memset(phdr, 0, sizeof(phdr));
  linker_write_u32(phdr + 0, PT_LOAD);
  linker_write_u32(phdr + 4, PF_R | PF_X);
  linker_write_u64(phdr + 8, 0u);
  linker_write_u64(phdr + 16, image_base);
  linker_write_u64(phdr + 24, image_base);
  linker_write_u64(phdr + 32, read_only_file_end);
  linker_write_u64(phdr + 40, read_only_end - image_base);
  linker_write_u64(phdr + 48, page_size);
  if (!elf_image_buffer_write(image, offset, phdr, sizeof(phdr))) {
    return 0;
  }
  offset += ELF_PHDR_SIZE;
  count++;

  if (read_only_count != part_count) {
    uint64_t start_address = parts[read_only_count].virtual_address;
    uint64_t start_offset = parts[read_only_count].file_offset;
    uint64_t file_end = start_offset;
    uint64_t memory_end = start_address;

    for (i = read_only_count; i < part_count; i++) {
      if (parts[i].is_tls) {
        continue;
      }
      if (parts[i].file_size != 0u) {
        file_end = parts[i].file_offset + parts[i].file_size;
      }
      memory_end = parts[i].virtual_address + parts[i].memory_size;
    }

    memset(phdr, 0, sizeof(phdr));
    linker_write_u32(phdr + 0, PT_LOAD);
    linker_write_u32(phdr + 4, PF_R | PF_W);
    linker_write_u64(phdr + 8, start_offset);
    linker_write_u64(phdr + 16, start_address);
    linker_write_u64(phdr + 24, start_address);
    linker_write_u64(phdr + 32, file_end - start_offset);
    linker_write_u64(phdr + 40, memory_end - start_address);
    linker_write_u64(phdr + 48, page_size);
    if (!elf_image_buffer_write(image, offset, phdr, sizeof(phdr))) {
      return 0;
    }
    offset += ELF_PHDR_SIZE;
    count++;
  }

  if (elf_dynamic_plan_is_active(plan)) {
    static const struct {
      ElfDynamicBlobKind kind;
      uint32_t type;
      uint32_t flags;
      uint64_t alignment;
    } segments[] = {
        {ELF_DYNAMIC_BLOB_INTERP, PT_INTERP, PF_R, 1u},
        {ELF_DYNAMIC_BLOB_DYNAMIC, PT_DYNAMIC, PF_R | PF_W, 8u},
    };
    size_t segment = 0u;

    for (segment = 0u; segment < sizeof(segments) / sizeof(segments[0]);
         segment++) {
      const ElfDynamicBlob *blob =
          elf_dynamic_plan_blob(plan, segments[segment].kind);
      uint64_t blob_offset = 0u;
      uint64_t blob_address = 0u;

      if (!elf_image_blob_location(parts, part_count, blob, &blob_offset,
                                   &blob_address)) {
        continue;
      }
      memset(phdr, 0, sizeof(phdr));
      linker_write_u32(phdr + 0, segments[segment].type);
      linker_write_u32(phdr + 4, segments[segment].flags);
      linker_write_u64(phdr + 8, blob_offset);
      linker_write_u64(phdr + 16, blob_address);
      linker_write_u64(phdr + 24, blob_address);
      linker_write_u64(phdr + 32, blob->size);
      linker_write_u64(phdr + 40, blob->size);
      linker_write_u64(phdr + 48, segments[segment].alignment);
      if (!elf_image_buffer_write(image, offset, phdr, sizeof(phdr))) {
        return 0;
      }
      offset += ELF_PHDR_SIZE;
      count++;
    }

    memset(phdr, 0, sizeof(phdr));
    linker_write_u32(phdr + 0, PT_GNU_STACK);
    linker_write_u32(phdr + 4, PF_R | PF_W);
    linker_write_u64(phdr + 48, 16u);
    if (!elf_image_buffer_write(image, offset, phdr, sizeof(phdr))) {
      return 0;
    }
    offset += ELF_PHDR_SIZE;
    count++;
  }

  for (i = 0u; i < part_count; i++) {
    if (!parts[i].is_tls) {
      continue;
    }
    memset(phdr, 0, sizeof(phdr));
    linker_write_u32(phdr + 0, PT_TLS);
    linker_write_u32(phdr + 4, PF_R);
    linker_write_u64(phdr + 8, parts[i].file_offset);
    linker_write_u64(phdr + 16, parts[i].virtual_address);
    linker_write_u64(phdr + 24, parts[i].virtual_address);
    linker_write_u64(phdr + 32, parts[i].file_size);
    linker_write_u64(phdr + 40, parts[i].memory_size);
    linker_write_u64(phdr + 48, parts[i].alignment);
    if (!elf_image_buffer_write(image, offset, phdr, sizeof(phdr))) {
      return 0;
    }
    offset += ELF_PHDR_SIZE;
    count++;
  }

  *phnum_out = count;
  return 1;
}

static uint16_t elf_image_count_program_headers(const ElfImagePart *parts,
                                                size_t part_count,
                                                size_t read_only_count,
                                                const ElfDynamicPlan *plan) {
  uint16_t count = 1u;
  size_t i = 0u;

  if (read_only_count != part_count) {
    count++;
  }
  for (i = 0u; i < part_count; i++) {
    if (parts[i].is_tls) {
      count++;
    }
  }
  if (elf_dynamic_plan_is_active(plan)) {
    count++;
    if (elf_dynamic_plan_blob(plan, ELF_DYNAMIC_BLOB_INTERP)) {
      count++;
    }
    if (elf_dynamic_plan_blob(plan, ELF_DYNAMIC_BLOB_DYNAMIC)) {
      count++;
    }
  }
  return count;
}

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} ElfImageStrings;

static int elf_image_strings_init(ElfImageStrings *strings) {
  strings->capacity = 256u;
  strings->data = (char *)calloc(strings->capacity, 1u);
  if (!strings->data) {
    return 0;
  }
  strings->size = 1u;
  return 1;
}

static uint32_t elf_image_strings_add(ElfImageStrings *strings,
                                      const char *text) {
  size_t length = 0u;
  uint32_t offset = 0u;

  if (!text) {
    return 0u;
  }
  length = strlen(text) + 1u;
  if (strings->size + length > strings->capacity) {
    size_t capacity = strings->capacity ? strings->capacity : 256u;
    char *grown = NULL;
    while (capacity < strings->size + length) {
      capacity *= 2u;
    }
    grown = (char *)realloc(strings->data, capacity);
    if (!grown) {
      return 0u;
    }
    memset(grown + strings->size, 0, capacity - strings->size);
    strings->data = grown;
    strings->capacity = capacity;
  }
  offset = (uint32_t)strings->size;
  memcpy(strings->data + strings->size, text, length);
  strings->size += length;
  return offset;
}

static int elf_image_emit_symbols(ElfImageBuffer *image,
                                  const LinkResolution *resolution,
                                  const ElfImagePart *parts, size_t part_count,
                                  uint64_t *symtab_offset_out,
                                  uint64_t *symtab_size_out,
                                  uint64_t *strtab_offset_out,
                                  uint64_t *strtab_size_out,
                                  uint32_t *local_count_out) {
  ElfImageStrings strings;
  unsigned char entry[ELF_SYM_SIZE];
  size_t offset = linker_align_up(image->size, 8u);
  size_t written = 0u;
  size_t symbol_index = 0u;
  size_t i = 0u;

  if (!elf_image_strings_init(&strings)) {
    return 0;
  }

  memset(entry, 0, sizeof(entry));
  if (!elf_image_buffer_pad_to(image, offset) ||
      !elf_image_buffer_write(image, offset, entry, sizeof(entry))) {
    free(strings.data);
    return 0;
  }
  written = 1u;

  for (symbol_index = 0u; symbol_index < resolution->symbol_count;
       symbol_index++) {
    const LinkedSymbol *symbol = &resolution->symbols[symbol_index];
    uint16_t section_header_index = 0u;
    uint32_t name_offset = 0u;

    if (!symbol->is_defined || !symbol->name ||
        symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
      continue;
    }
    for (i = 0u; i < part_count; i++) {
      if (parts[i].merged_index == symbol->merged_section_index) {
        section_header_index = parts[i].section_header_index;
        break;
      }
    }
    if (section_header_index == 0u) {
      continue;
    }

    name_offset = elf_image_strings_add(&strings, symbol->name);
    memset(entry, 0, sizeof(entry));
    linker_write_u32(entry + 0, name_offset);
    entry[4] = (unsigned char)((STB_GLOBAL << 4) | STT_NOTYPE);
    entry[5] = 0u;
    elf_image_put_u16(entry + 6, section_header_index);
    linker_write_u64(entry + 8, symbol->virtual_address);
    linker_write_u64(entry + 16, 0u);
    if (!elf_image_buffer_write(image, offset + written * ELF_SYM_SIZE, entry,
                                sizeof(entry))) {
      free(strings.data);
      return 0;
    }
    written++;
  }

  *symtab_offset_out = offset;
  *symtab_size_out = (uint64_t)written * ELF_SYM_SIZE;
  *local_count_out = 1u;

  offset = offset + (size_t)(*symtab_size_out);
  if (!elf_image_buffer_pad_to(image, offset) ||
      !elf_image_buffer_write(image, offset, strings.data, strings.size)) {
    free(strings.data);
    return 0;
  }
  *strtab_offset_out = offset;
  *strtab_size_out = strings.size;
  free(strings.data);
  return 1;
}

static int elf_image_write_file(const char *path, const ElfImageBuffer *image,
                                char **error_message_out) {
  FILE *file = fopen(path, "wb");

  if (!file) {
    mettle_set_error(error_message_out,
                     "Could not open '%s' for writing the linked image", path);
    return 0;
  }
  if (image->size != 0u &&
      fwrite(image->data, 1u, image->size, file) != image->size) {
    fclose(file);
    mettle_set_error(error_message_out, "Could not write the linked image '%s'",
                     path);
    return 0;
  }
  fclose(file);
#ifndef _WIN32
  if (chmod(path, 0755) != 0) {
    mettle_set_error(error_message_out,
                     "Could not make the linked image '%s' executable", path);
    return 0;
  }
#endif
  return 1;
}

static void elf_image_read_options(const ElfImageOptions *options,
                                   uint64_t *image_base, uint64_t *page_size,
                                   int *emit_symbols,
                                   int *produce_shared_library,
                                   ElfDynamicOptions *dynamic_options) {
  memset(dynamic_options, 0, sizeof(*dynamic_options));
  if (!options) {
    dynamic_options->produce_shared_library = *produce_shared_library;
    dynamic_options->image_base = *image_base;
    return;
  }
  *produce_shared_library = options->produce_shared_library ? 1 : 0;
  if (*produce_shared_library) {
    *image_base = 0u;
  }
  if (options->image_base != 0u) {
    *image_base = options->image_base;
  }
  if (options->page_size != 0u) {
    *page_size = options->page_size;
  }
  *emit_symbols = !options->strip_symbols;
  dynamic_options->interpreter = options->interpreter;
  dynamic_options->soname = options->soname;
  dynamic_options->runpaths = options->runpaths;
  dynamic_options->runpath_count = options->runpath_count;
  dynamic_options->export_dynamic = options->export_dynamic ? 1 : 0;
  dynamic_options->produce_shared_library = *produce_shared_library;
  dynamic_options->image_base = *image_base;
}

typedef struct {
  uint64_t symtab_offset;
  uint64_t symtab_size;
  uint64_t strtab_offset;
  uint64_t strtab_size;
  uint32_t local_symbol_count;
  uint16_t symtab_index;
  int emit_symbols;
} ElfImageSymbolTables;

static int elf_image_write_section_headers(
    ElfImageBuffer *image, ElfImagePart *parts, size_t part_count,
    ElfDynamicPlan *plan, const ElfImageSymbolTables *tables,
    uint16_t shstrndx, uint64_t *section_header_offset_out,
    char **error_message_out) {
  ElfImageStrings shstrings;
  unsigned char shdr[ELF_SHDR_SIZE];
  uint64_t shstrtab_offset = 0u;
  uint64_t section_header_offset = 0u;
  uint64_t symtab_offset = tables->symtab_offset;
  uint64_t symtab_size = tables->symtab_size;
  uint64_t strtab_offset = tables->strtab_offset;
  uint64_t strtab_size = tables->strtab_size;
  uint32_t local_symbol_count = tables->local_symbol_count;
  uint32_t shstrtab_name = 0u;
  uint32_t symtab_name = 0u;
  uint32_t strtab_name = 0u;
  uint16_t symtab_index = tables->symtab_index;
  int emit_symbols = tables->emit_symbols;
  size_t i = 0u;

  if (!elf_image_strings_init(&shstrings)) {
    mettle_set_error(error_message_out, "Out of memory while emitting ELF");
    return 0;
  }
  for (i = 0u; i < part_count; i++) {
    parts[i].alignment = parts[i].alignment ? parts[i].alignment : 1u;
  }
  {
    uint32_t part_names[ELF_IMAGE_MAX_PARTS];
    uint32_t blob_names[ELF_DYNAMIC_BLOB_COUNT];

    memset(blob_names, 0, sizeof(blob_names));
    for (i = 0u; i < part_count; i++) {
      part_names[i] = elf_image_strings_add(&shstrings, parts[i].name);
    }
    for (i = 0u; i < ELF_DYNAMIC_BLOB_COUNT; i++) {
      const ElfDynamicBlob *blob =
          elf_dynamic_plan_blob(plan, (ElfDynamicBlobKind)i);
      if (blob) {
        blob_names[i] = elf_image_strings_add(&shstrings, blob->name);
      }
    }
    if (emit_symbols) {
      symtab_name = elf_image_strings_add(&shstrings, ".symtab");
      strtab_name = elf_image_strings_add(&shstrings, ".strtab");
    }
    shstrtab_name = elf_image_strings_add(&shstrings, ".shstrtab");

    shstrtab_offset = linker_align_up(image->size, 1u);
    if (!elf_image_buffer_pad_to(image, (size_t)shstrtab_offset) ||
        !elf_image_buffer_write(image, (size_t)shstrtab_offset,
                                shstrings.data, shstrings.size)) {
      free(shstrings.data);
      mettle_set_error(error_message_out, "Out of memory while emitting ELF");
      return 0;
    }

    section_header_offset = linker_align_up(image->size, 8u);
    if (!elf_image_buffer_pad_to(image, (size_t)section_header_offset)) {
      free(shstrings.data);
      mettle_set_error(error_message_out, "Out of memory while emitting ELF");
      return 0;
    }

    memset(shdr, 0, sizeof(shdr));
    if (!elf_image_buffer_write(image, (size_t)section_header_offset, shdr,
                                sizeof(shdr))) {
      free(shstrings.data);
      mettle_set_error(error_message_out, "Out of memory while emitting ELF");
      return 0;
    }

    for (i = 0u; i < part_count; i++) {
      size_t at = (size_t)section_header_offset +
                  (size_t)parts[i].section_header_index * ELF_SHDR_SIZE;

      memset(shdr, 0, sizeof(shdr));
      linker_write_u32(shdr + 0, part_names[i]);
      linker_write_u32(shdr + 4, parts[i].section_type);
      linker_write_u64(shdr + 8, parts[i].section_flags);
      linker_write_u64(shdr + 16, parts[i].virtual_address);
      linker_write_u64(shdr + 24, parts[i].file_offset);
      linker_write_u64(shdr + 32, parts[i].memory_size);
      linker_write_u64(shdr + 48, parts[i].alignment);
      if (!elf_image_buffer_write(image, at, shdr, sizeof(shdr))) {
        free(shstrings.data);
        mettle_set_error(error_message_out, "Out of memory while emitting ELF");
        return 0;
      }
    }

    for (i = 0u; i < ELF_DYNAMIC_BLOB_COUNT; i++) {
      const ElfDynamicBlob *blob =
          elf_dynamic_plan_blob(plan, (ElfDynamicBlobKind)i);
      const ElfDynamicBlob *dynsym =
          elf_dynamic_plan_blob(plan, ELF_DYNAMIC_BLOB_DYNSYM);
      const ElfDynamicBlob *dynstr =
          elf_dynamic_plan_blob(plan, ELF_DYNAMIC_BLOB_DYNSTR);
      const ElfDynamicBlob *got =
          elf_dynamic_plan_blob(plan, ELF_DYNAMIC_BLOB_GOT);
      uint64_t blob_offset = 0u;
      uint64_t blob_address = 0u;
      uint32_t link = 0u;
      uint32_t info = 0u;
      size_t at = 0u;

      if (!blob || !elf_image_blob_location(parts, part_count, blob,
                                            &blob_offset, &blob_address)) {
        continue;
      }
      if (blob->links_dynstr && dynstr) {
        link = dynstr->section_header_index;
      } else if (blob->links_dynsym && dynsym) {
        link = dynsym->section_header_index;
      }
      if (i == ELF_DYNAMIC_BLOB_DYNSYM) {
        info = 1u;
      } else if (i == ELF_DYNAMIC_BLOB_RELA_PLT && got) {
        info = got->section_header_index;
      } else if (i == ELF_DYNAMIC_BLOB_VERNEED) {
        info = (uint32_t)elf_dynamic_plan_verneed_count(plan);
      }

      at = (size_t)section_header_offset +
           (size_t)blob->section_header_index * ELF_SHDR_SIZE;
      memset(shdr, 0, sizeof(shdr));
      linker_write_u32(shdr + 0, blob_names[i]);
      linker_write_u32(shdr + 4, blob->section_type);
      linker_write_u64(shdr + 8, blob->section_flags);
      linker_write_u64(shdr + 16, blob_address);
      linker_write_u64(shdr + 24, blob_offset);
      linker_write_u64(shdr + 32, blob->size);
      linker_write_u32(shdr + 40, link);
      linker_write_u32(shdr + 44, info);
      linker_write_u64(shdr + 48, blob->alignment);
      linker_write_u64(shdr + 56, blob->entry_size);
      if (!elf_image_buffer_write(image, at, shdr, sizeof(shdr))) {
        free(shstrings.data);
        mettle_set_error(error_message_out, "Out of memory while emitting ELF");
        return 0;
      }
    }

    if (emit_symbols) {
      size_t at = (size_t)section_header_offset +
                  (size_t)symtab_index * ELF_SHDR_SIZE;

      memset(shdr, 0, sizeof(shdr));
      linker_write_u32(shdr + 0, symtab_name);
      linker_write_u32(shdr + 4, SHT_SYMTAB);
      linker_write_u64(shdr + 24, symtab_offset);
      linker_write_u64(shdr + 32, symtab_size);
      linker_write_u32(shdr + 40, (uint32_t)symtab_index + 1u);
      linker_write_u32(shdr + 44, local_symbol_count);
      linker_write_u64(shdr + 48, 8u);
      linker_write_u64(shdr + 56, ELF_SYM_SIZE);
      if (!elf_image_buffer_write(image, at, shdr, sizeof(shdr))) {
        free(shstrings.data);
        mettle_set_error(error_message_out, "Out of memory while emitting ELF");
        return 0;
      }

      memset(shdr, 0, sizeof(shdr));
      linker_write_u32(shdr + 0, strtab_name);
      linker_write_u32(shdr + 4, SHT_STRTAB);
      linker_write_u64(shdr + 24, strtab_offset);
      linker_write_u64(shdr + 32, strtab_size);
      linker_write_u64(shdr + 48, 1u);
      if (!elf_image_buffer_write(image, at + ELF_SHDR_SIZE, shdr,
                                  sizeof(shdr))) {
        free(shstrings.data);
        mettle_set_error(error_message_out, "Out of memory while emitting ELF");
        return 0;
      }
    }

    {
      size_t at = (size_t)section_header_offset + (size_t)shstrndx * ELF_SHDR_SIZE;

      memset(shdr, 0, sizeof(shdr));
      linker_write_u32(shdr + 0, shstrtab_name);
      linker_write_u32(shdr + 4, SHT_STRTAB);
      linker_write_u64(shdr + 24, shstrtab_offset);
      linker_write_u64(shdr + 32, shstrings.size);
      linker_write_u64(shdr + 48, 1u);
      if (!elf_image_buffer_write(image, at, shdr, sizeof(shdr))) {
        free(shstrings.data);
        mettle_set_error(error_message_out, "Out of memory while emitting ELF");
        return 0;
      }
    }
  }
  free(shstrings.data);
  *section_header_offset_out = section_header_offset;
  return 1;
}

int elf_image_emit_executable(LinkResolution *resolution,
                              const char *output_path,
                              const ElfImageOptions *options,
                              char **error_message_out) {
  ElfImagePart parts[ELF_IMAGE_MAX_PARTS];
  ElfImageBuffer image = {0};
  ElfDynamicPlan *plan = NULL;
  ElfDynamicOptions dynamic_options;
  LinkRelocationOptions relocation_options = {0};
  unsigned char ehdr[ELF_EHDR_SIZE];
  uint64_t image_base = 0x400000u;
  uint64_t page_size = 0x1000u;
  uint64_t headers_size = 0u;
  uint64_t read_only_end = 0u;
  uint64_t read_only_file_end = 0u;
  uint64_t symtab_offset = 0u;
  uint64_t symtab_size = 0u;
  uint64_t strtab_offset = 0u;
  uint64_t strtab_size = 0u;
  uint64_t section_header_offset = 0u;
  uint32_t local_symbol_count = 1u;
  uint16_t phnum = 0u;
  uint16_t shnum = 0u;
  uint16_t shstrndx = 0u;
  uint16_t symtab_index = 0u;
  size_t part_count = 0u;
  size_t read_only_count = 0u;
  size_t i = 0u;
  int emit_symbols = 0;
  int produce_shared_library = 0;

  if (error_message_out) {
    free(*error_message_out);
    *error_message_out = NULL;
  }
  if (!resolution || !output_path) {
    mettle_set_error(error_message_out, "Invalid arguments while emitting ELF");
    return 0;
  }
  elf_image_read_options(options, &image_base, &page_size, &emit_symbols,
                         &produce_shared_library, &dynamic_options);
  if (!resolution->entry_symbol && !produce_shared_library) {
    mettle_set_error(error_message_out,
                     "The entry symbol was not resolved before ELF emission");
    return 0;
  }

  if (!elf_dynamic_plan_create(resolution, &dynamic_options, &plan,
                               error_message_out)) {
    return 0;
  }
  if (elf_dynamic_plan_is_active(plan) && !produce_shared_library &&
      !elf_dynamic_plan_blob(plan, ELF_DYNAMIC_BLOB_INTERP)) {
    elf_dynamic_plan_destroy(plan);
    mettle_set_error(error_message_out,
                     "A shared library supplies a symbol this program needs, "
                     "but no program loader was named for PT_INTERP");
    return 0;
  }

  elf_image_normalize_tls(resolution);

  memset(parts, 0, sizeof(parts));
  if (!elf_image_collect_parts(resolution, parts, &part_count,
                               &read_only_count)) {
    elf_dynamic_plan_destroy(plan);
    mettle_set_error(error_message_out, "Out of memory while planning ELF");
    return 0;
  }
  if (part_count == 0u) {
    elf_dynamic_plan_destroy(plan);
    mettle_set_error(error_message_out, "The link produced no loadable content");
    return 0;
  }

  shnum = 1u;
  for (i = 0u; i < part_count; i++) {
    parts[i].section_header_index = shnum;
    shnum++;
  }
  for (i = 0u; i < ELF_DYNAMIC_BLOB_COUNT; i++) {
    if (!elf_dynamic_plan_blob(plan, (ElfDynamicBlobKind)i)) {
      continue;
    }
    elf_dynamic_plan_set_section_index(plan, (ElfDynamicBlobKind)i, shnum);
    shnum++;
  }

  phnum = elf_image_count_program_headers(parts, part_count, read_only_count,
                                          plan);
  headers_size = ELF_EHDR_SIZE + (uint64_t)phnum * ELF_PHDR_SIZE;

  elf_image_assign_addresses(parts, part_count, read_only_count, image_base,
                             page_size, headers_size, &read_only_end,
                             &read_only_file_end);

  if (!elf_image_apply_layout(resolution, parts, part_count,
                              error_message_out)) {
    elf_dynamic_plan_destroy(plan);
    return 0;
  }

  relocation_options.image_base = image_base;
  if (!link_apply_relocations(resolution, &relocation_options,
                              error_message_out)) {
    elf_dynamic_plan_destroy(plan);
    return 0;
  }

  if (elf_dynamic_plan_is_active(plan)) {
    uint16_t merged_section_header_indices[LINKED_SECTION_COUNT];

    memset(merged_section_header_indices, 0,
           sizeof(merged_section_header_indices));
    for (i = 0u; i < part_count; i++) {
      merged_section_header_indices[parts[i].merged_index] =
          parts[i].section_header_index;
    }
    if (!elf_dynamic_plan_write(plan, merged_section_header_indices,
                                error_message_out)) {
      elf_dynamic_plan_destroy(plan);
      return 0;
    }
  }

  if (!elf_image_buffer_pad_to(&image, (size_t)headers_size)) {
    elf_dynamic_plan_destroy(plan);
    mettle_set_error(error_message_out, "Out of memory while emitting ELF");
    return 0;
  }

  for (i = 0u; i < part_count; i++) {
    const LinkedSection *section = &resolution->sections[parts[i].merged_index];

    if (parts[i].file_size == 0u) {
      continue;
    }
    if (!elf_image_buffer_pad_to(&image, (size_t)parts[i].file_offset) ||
        !elf_image_buffer_write(&image, (size_t)parts[i].file_offset,
                                section->data, (size_t)parts[i].file_size)) {
      elf_image_buffer_free(&image);
      mettle_set_error(error_message_out,
                       "Out of memory while writing section '%s'",
                       parts[i].name);
      return 0;
    }
  }

  if (emit_symbols) {
    if (!elf_image_emit_symbols(&image, resolution, parts, part_count,
                                &symtab_offset, &symtab_size, &strtab_offset,
                                &strtab_size, &local_symbol_count)) {
      elf_dynamic_plan_destroy(plan);
      elf_image_buffer_free(&image);
      mettle_set_error(error_message_out,
                       "Out of memory while emitting the ELF symbol table");
      return 0;
    }
    symtab_index = shnum;
    shnum = (uint16_t)(shnum + 2u);
  }
  shstrndx = shnum;
  shnum++;

  {
    ElfImageSymbolTables tables;

    tables.symtab_offset = symtab_offset;
    tables.symtab_size = symtab_size;
    tables.strtab_offset = strtab_offset;
    tables.strtab_size = strtab_size;
    tables.local_symbol_count = local_symbol_count;
    tables.symtab_index = symtab_index;
    tables.emit_symbols = emit_symbols;
    if (!elf_image_write_section_headers(&image, parts, part_count, plan,
                                         &tables, shstrndx,
                                         &section_header_offset,
                                         error_message_out)) {
      elf_dynamic_plan_destroy(plan);
      elf_image_buffer_free(&image);
      return 0;
    }
  }

  memset(ehdr, 0, sizeof(ehdr));
  ehdr[0] = 0x7Fu;
  ehdr[1] = 'E';
  ehdr[2] = 'L';
  ehdr[3] = 'F';
  ehdr[4] = 2u;
  ehdr[5] = 1u;
  ehdr[6] = 1u;
  elf_image_put_u16(ehdr + 16, produce_shared_library ? ET_DYN : ET_EXEC);
  elf_image_put_u16(ehdr + 18, 62u);
  linker_write_u32(ehdr + 20, 1u);
  linker_write_u64(ehdr + 24, resolution->entry_symbol
                                  ? resolution->entry_symbol->virtual_address
                                  : 0u);
  linker_write_u64(ehdr + 32, ELF_EHDR_SIZE);
  linker_write_u64(ehdr + 40, section_header_offset);
  linker_write_u32(ehdr + 48, 0u);
  elf_image_put_u16(ehdr + 52, (uint16_t)ELF_EHDR_SIZE);
  elf_image_put_u16(ehdr + 54, (uint16_t)ELF_PHDR_SIZE);
  elf_image_put_u16(ehdr + 56, phnum);
  elf_image_put_u16(ehdr + 58, (uint16_t)ELF_SHDR_SIZE);
  elf_image_put_u16(ehdr + 60, shnum);
  elf_image_put_u16(ehdr + 62, shstrndx);
  if (!elf_image_buffer_write(&image, 0u, ehdr, sizeof(ehdr))) {
    elf_image_buffer_free(&image);
    mettle_set_error(error_message_out, "Out of memory while emitting ELF");
    return 0;
  }

  if (!elf_image_write_program_headers(&image, parts, part_count,
                                       read_only_count, image_base, page_size,
                                       read_only_end, read_only_file_end, plan,
                                       &phnum)) {
    elf_dynamic_plan_destroy(plan);
    elf_image_buffer_free(&image);
    mettle_set_error(error_message_out, "Out of memory while emitting ELF");
    return 0;
  }

  elf_dynamic_plan_destroy(plan);
  if (!elf_image_write_file(output_path, &image, error_message_out)) {
    elf_image_buffer_free(&image);
    return 0;
  }

  elf_image_buffer_free(&image);
  return 1;
}
