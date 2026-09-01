#include "linker/elf_reader.h"
#include "linker/linker_common.h"
#include "../common.h"

#include <stdlib.h>
#include <string.h>

#define ELF_EHDR_SIZE 64u
#define ELF_SHDR_SIZE 64u
#define ELF_SYM_SIZE 24u
#define ELF_RELA_SIZE 24u
#define ELF_REL_SIZE 16u

#define ET_REL 1
#define EM_X86_64 62

#define SHT_PROGBITS 1u
#define SHT_SYMTAB 2u
#define SHT_STRTAB 3u
#define SHT_RELA 4u
#define SHT_NOBITS 8u
#define SHT_REL 9u
#define SHT_GROUP 17u

#define SHF_WRITE 0x1u
#define SHF_ALLOC 0x2u
#define SHF_EXECINSTR 0x4u
#define SHF_TLS 0x400u

#define SHN_UNDEF 0u
#define SHN_LORESERVE 0xFF00u
#define SHN_ABS 0xFFF1u
#define SHN_COMMON 0xFFF2u
#define SHN_XINDEX 0xFFFFu

#define STB_LOCAL 0u
#define STB_GLOBAL 1u
#define STB_WEAK 2u

#define R_X86_64_64 1u
#define R_X86_64_PC32 2u
#define R_X86_64_PLT32 4u
#define R_X86_64_GOTPCREL 9u
#define R_X86_64_32 10u
#define R_X86_64_32S 11u
#define R_X86_64_TPOFF32 23u
#define R_X86_64_PC64 24u
#define R_X86_64_GOTPCRELX 41u
#define R_X86_64_REX_GOTPCRELX 42u

typedef struct {
    const unsigned char *data;
    size_t size;
    const char *origin;
    uint64_t shoff;
    uint16_t shentsize;
    uint64_t shnum;
    uint32_t shstrndx;
} ElfImage;

static const unsigned char *elf_shdr(const ElfImage *image, uint64_t index) {
    return image->data + image->shoff + index * image->shentsize;
}

static uint32_t elf_sh_type(const unsigned char *shdr) {
    return linker_read_u32(shdr + 4);
}

static uint64_t elf_sh_flags(const unsigned char *shdr) {
    return linker_read_u64(shdr + 8);
}

static uint64_t elf_sh_offset(const unsigned char *shdr) {
    return linker_read_u64(shdr + 24);
}

static uint64_t elf_sh_size(const unsigned char *shdr) {
    return linker_read_u64(shdr + 32);
}


static char *elf_dup_string(const unsigned char *table, uint64_t table_size, uint32_t offset) {
    const char *start = NULL;
    uint64_t length = 0;
    char *copy = NULL;

    if (!table || offset >= table_size) {
        return NULL;
    }
    start = (const char *) table + offset;
    while (offset + length < table_size && start[length] != '\0') {
        length++;
    }
    if (offset + length >= table_size) {
        return NULL;
    }
    copy = malloc((size_t)length + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, (size_t)length);
    copy[length] = '\0';
    return copy;
}

static LinkSectionKind elf_section_kind(uint64_t flags, uint32_t type) {
    if ((flags & SHF_TLS) != 0u) {
        return LINK_SECTION_KIND_TLS;
    }
    if ((flags & SHF_ALLOC) == 0u) {
        return LINK_SECTION_KIND_UNKNOWN;
    }
    if (type == SHT_NOBITS) {
        return LINK_SECTION_KIND_BSS;
    }
    if ((flags & SHF_EXECINSTR) != 0u) {
        return LINK_SECTION_KIND_TEXT;
    }
    if ((flags & SHF_WRITE) != 0u) {
        return LINK_SECTION_KIND_DATA;
    }
    return LINK_SECTION_KIND_RDATA;
}

static LinkRelocKind elf_reloc_kind(uint32_t type) {
    switch (type) {
        case R_X86_64_64:
            return LINK_RELOC_ABS64;
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
            return LINK_RELOC_PC32;
        case R_X86_64_32:
        case R_X86_64_32S:
            return LINK_RELOC_ABS32;
        case R_X86_64_TPOFF32:
            return LINK_RELOC_TPOFF32;
        case R_X86_64_GOTPCREL:
            return LINK_RELOC_GOTPCREL32;
        case R_X86_64_GOTPCRELX:
        case R_X86_64_REX_GOTPCRELX:
            return LINK_RELOC_GOTPCRELX32;
        default:
            return LINK_RELOC_NONE;
    }
}

static int elf_parse_header(ElfImage *image, char **error_message_out) {
    const unsigned char *data = image->data;
    const unsigned char *first = NULL;

    if (image->size < ELF_EHDR_SIZE) {
        mettle_set_error(error_message_out, "ELF file '%s' is smaller than the file header", image->origin);
        return 0;
    }
    if (data[0] != 0x7Fu || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
        mettle_set_error(error_message_out, "File '%s' is not an ELF object", image->origin);
        return 0;
    }
    if (data[4] != 2u || data[5] != 1u) {
        mettle_set_error(error_message_out, "ELF file '%s' is not a little endian ELF64 object", image->origin);
        return 0;
    }
    if (linker_read_u16(data + 16) != ET_REL) {
        mettle_set_error(error_message_out, "ELF file '%s' is not a relocatable object (ET_REL)", image->origin);
        return 0;
    }
    if (linker_read_u16(data + 18) != EM_X86_64) {
        mettle_set_error(error_message_out, "Unsupported ELF machine '%u' in '%s' (expected x86-64)", (unsigned)linker_read_u16(data + 18), image->origin);
        return 0;
    }

    image->shoff = linker_read_u64(data + 40);
    image->shentsize = linker_read_u16(data + 58);
    image->shnum = linker_read_u16(data + 60);
    image->shstrndx = linker_read_u16(data + 62);

    if (image->shentsize < ELF_SHDR_SIZE) {
        mettle_set_error(error_message_out, "ELF file '%s' has an undersized section header entry", image->origin);
        return 0;
    }
    if (image->shoff == 0u) {
        mettle_set_error(error_message_out, "ELF file '%s' has no section header table", image->origin);
        return 0;
    }
    if (!link_object_range_ok(image->size, image->shoff, image->shentsize)) {
        mettle_set_error(error_message_out, "ELF file '%s' has a truncated section header table", image->origin);
        return 0;
    }

    first = image->data + image->shoff;
    if (image->shnum == 0u) {
        image->shnum = elf_sh_size(first);
    }
    if (image->shstrndx == SHN_XINDEX) {
        image->shstrndx = linker_read_u32(first + 40);
    }

    if (image->shnum > image->size / image->shentsize) {
        mettle_set_error(error_message_out, "ELF file '%s' has a truncated section header table", image->origin);
        return 0;
    }
    if (!link_object_range_ok(image->size, image->shoff, image->shnum * (uint64_t)image->shentsize)) {
        mettle_set_error(error_message_out, "ELF file '%s' has a truncated section header table", image->origin);
        return 0;
    }
    if (image->shstrndx >= image->shnum) {
        mettle_set_error(error_message_out, "ELF file '%s' names a section string table outside the section header table", image->origin);
        return 0;
    }
    return 1;
}

static int elf_parse_sections(const ElfImage *image, LinkObject *object, char **error_message_out) {
    const unsigned char *shstr_hdr = elf_shdr(image, image->shstrndx);
    const unsigned char *shstr = NULL;

    uint64_t shstr_size = elf_sh_size(shstr_hdr);
    uint64_t i = 0;

    if (!link_object_range_ok(image->size, elf_sh_offset(shstr_hdr), shstr_size)) {
        mettle_set_error(error_message_out, "ELF file '%s' has a truncated section string table", image->origin);
        return 0;
    }
    shstr = image->data + elf_sh_offset(shstr_hdr);

    object->sections = calloc((size_t)image->shnum, sizeof(LinkSection));
    if (!object->sections) {
        mettle_set_error(error_message_out, "Out of memory while reading ELF sections from '%s'", image->origin);
        return 0;
    }
    object->section_count = (size_t)image->shnum;

    for (i = 0; i < image->shnum; i++) {
        const unsigned char *shdr = elf_shdr(image, i);
        LinkSection *section = &object->sections[i];
        uint32_t type = elf_sh_type(shdr);
        uint64_t flags = elf_sh_flags(shdr);
        uint64_t size = elf_sh_size(shdr);
        uint64_t offset = elf_sh_offset(shdr);
        uint64_t align = linker_read_u64(shdr + 48);

        if (type == SHT_GROUP) {
            mettle_set_error(error_message_out, "ELF file '%s' uses section groups, which the internal linker does not resolve", image->origin);
            return 0;
        }

        section->name = elf_dup_string(shstr, shstr_size, linker_read_u32(shdr));
        section->kind = elf_section_kind(flags, type);
        section->alignment = align ? align : 1u;
        section->virtual_size = size;
        section->is_metadata = (flags & SHF_ALLOC) == 0u;
        
        if (type == SHT_NOBITS || size == 0u) {
            continue;
        }
        if (!link_object_range_ok(image->size, offset, size)) {
            mettle_set_error(error_message_out, "Section '%s' in '%s' runs past the end of the file", section->name ? section->name : "<unnamed>", image->origin);
            return 0;
        }
        section->raw_data = malloc((size_t)size);
        if (!section->raw_data) {
            mettle_set_error(error_message_out, "Out of memory while reading section '%s' from '%s'", section->name ? section->name : "<unnamed>", image->origin);
            return 0;
        }
        memcpy(section->raw_data, image->data + offset, (size_t)size);
        section->size_of_raw_data = size;
    }
    return 1;
}

static int elf_parse_symbols(const ElfImage *image, LinkObject *object, char **error_message_out) {
  const unsigned char *symtab_hdr = NULL;
  const unsigned char *strtab_hdr = NULL;
  const unsigned char *symtab = NULL;
  const unsigned char *strtab = NULL;
  uint64_t symtab_size = 0;
  uint64_t strtab_size = 0;
  uint64_t count = 0;
  uint64_t i = 0;
  uint32_t strtab_index = 0;

  for (i = 0; i < image->shnum; i++) {
    if (elf_sh_type(elf_shdr(image, i)) == SHT_SYMTAB) {
      symtab_hdr = elf_shdr(image, i);
      break;
    }
  }
  if (!symtab_hdr) {
    return 1;
  }

  strtab_index = linker_read_u32(symtab_hdr + 40);
  if (strtab_index >= image->shnum) {
    mettle_set_error(error_message_out,
                     "ELF file '%s' names a symbol string table outside the "
                     "section header table",
                     image->origin);
    return 0;
  }
  strtab_hdr = elf_shdr(image, strtab_index);
  if (elf_sh_type(strtab_hdr) != SHT_STRTAB) {
    mettle_set_error(error_message_out,
                     "ELF file '%s' links its symbol table to a section that "
                     "is not a string table",
                     image->origin);
    return 0;
  }
  if (linker_read_u64(symtab_hdr + 56) != 0u &&
      linker_read_u64(symtab_hdr + 56) != ELF_SYM_SIZE) {
    mettle_set_error(error_message_out,
                     "ELF file '%s' has an unexpected symbol table entry size",
                     image->origin);
    return 0;
  }
  strtab_size = elf_sh_size(strtab_hdr);
  symtab_size = elf_sh_size(symtab_hdr);

  if (!link_object_range_ok(image->size, elf_sh_offset(symtab_hdr),
                            symtab_size) ||
      !link_object_range_ok(image->size, elf_sh_offset(strtab_hdr),
                            strtab_size)) {
    mettle_set_error(error_message_out,
                     "ELF file '%s' has a truncated symbol table",
                     image->origin);
    return 0;
  }
  symtab = image->data + elf_sh_offset(symtab_hdr);
  strtab = image->data + elf_sh_offset(strtab_hdr);
  count = symtab_size / ELF_SYM_SIZE;

  object->symbols = calloc((size_t)(count ? count : 1u), sizeof(LinkSymbol));
  if (!object->symbols) {
    mettle_set_error(error_message_out,
                     "Out of memory while reading ELF symbols from '%s'",
                     image->origin);
    return 0;
  }
  object->symbol_count = (size_t)count;

  for (i = 0; i < count; i++) {
    const unsigned char *entry = symtab + i * ELF_SYM_SIZE;
    LinkSymbol *symbol = &object->symbols[i];
    uint8_t info = entry[4];
    uint16_t shndx = linker_read_u16(entry + 6);
    uint32_t bind = (uint32_t)(info >> 4);

    symbol->name = elf_dup_string(strtab, strtab_size, linker_read_u32(entry));
    symbol->value = linker_read_u64(entry + 8);
    symbol->size = linker_read_u64(entry + 16);
    symbol->elf_type = (uint8_t)(info & 0x0Fu);
    symbol->is_external = bind == STB_GLOBAL || bind == STB_WEAK;
    symbol->is_weak = bind == STB_WEAK;

    if (shndx == SHN_UNDEF) {
      symbol->section_index = LINK_SECTION_INDEX_UNDEFINED;
    } else if (shndx == SHN_ABS) {
      symbol->section_index = LINK_SECTION_INDEX_ABSOLUTE;
      symbol->is_defined = 1;
    } else if (shndx == SHN_COMMON) {
      symbol->section_index = LINK_SECTION_INDEX_COMMON;
      symbol->is_common = 1;
      symbol->is_defined = 1;
      symbol->value = linker_read_u64(entry + 16);
    } else if (shndx == SHN_XINDEX) {
      mettle_set_error(error_message_out,
                       "ELF file '%s' uses SHN_XINDEX symbol section indices",
                       image->origin);
      return 0;
    } else if (shndx >= SHN_LORESERVE || shndx >= image->shnum) {
      mettle_set_error(error_message_out,
                       "Symbol '%s' in '%s' names section %u, which does not "
                       "exist",
                       symbol->name ? symbol->name : "<unnamed>",
                       image->origin, (unsigned)shndx);
      return 0;
    } else {
      symbol->section_index = (int64_t)shndx;
      symbol->is_defined = 1;
    }
  }
  return 1;
}

static int elf_parse_relocations(const ElfImage *image, LinkObject *object, char **error_message_out) {
  uint64_t i = 0;

  for (i = 0; i < image->shnum; i++) {
    const unsigned char *shdr = elf_shdr(image, i);
    uint32_t type = elf_sh_type(shdr);
    uint32_t target_index = linker_read_u32(shdr + 44);
    const unsigned char *entries = NULL;
    LinkSection *target = NULL;
    LinkReloc *grown = NULL;
    uint64_t size = elf_sh_size(shdr);
    uint64_t count = 0;
    uint64_t r = 0;

    if (type == SHT_REL) {
      mettle_set_error(error_message_out,
                       "ELF file '%s' carries SHT_REL relocations; only RELA "
                       "is supported on x86-64",
                       image->origin);
      return 0;
    }
    if (type != SHT_RELA) {
      continue;
    }
    if (target_index >= image->shnum) {
      mettle_set_error(error_message_out,
                       "Relocation section in '%s' targets section %u, which "
                       "does not exist",
                       image->origin, (unsigned)target_index);
      return 0;
    }
    if (!link_object_range_ok(image->size, elf_sh_offset(shdr), size)) {
      mettle_set_error(error_message_out,
                       "ELF file '%s' has a truncated relocation section",
                       image->origin);
      return 0;
    }

    target = &object->sections[target_index];
    entries = image->data + elf_sh_offset(shdr);
    count = size / ELF_RELA_SIZE;
    if (count == 0u) {
      continue;
    }

    grown = realloc(target->relocations,
                    (target->relocation_count + (size_t)count) *
                        sizeof(LinkReloc));
    if (!grown) {
      mettle_set_error(error_message_out,
                       "Out of memory while reading relocations from '%s'",
                       image->origin);
      return 0;
    }
    target->relocations = grown;

    for (r = 0; r < count; r++) {
      const unsigned char *entry = entries + r * ELF_RELA_SIZE;
      uint64_t info = linker_read_u64(entry + 8);
      uint32_t reloc_type = (uint32_t)(info & 0xFFFFFFFFu);
      uint32_t symbol_index = (uint32_t)(info >> 32);
      LinkReloc *out = &target->relocations[target->relocation_count + r];
      uint64_t width = 0;

      if (symbol_index >= object->symbol_count) {
        mettle_set_error(error_message_out,
                         "Relocation in '%s' names symbol %u outside the "
                         "symbol table",
                         image->origin, (unsigned)symbol_index);
        return 0;
      }

      out->offset = linker_read_u64(entry);
      out->symbol_index = symbol_index;
      out->kind = elf_reloc_kind(reloc_type);
      out->addend = (int64_t)linker_read_u64(entry + 16);
      out->addend_is_explicit = 1;
      out->format_type = reloc_type;

      if (out->kind == LINK_RELOC_NONE && reloc_type != 0u) {
        mettle_set_error(error_message_out,
                         "Unsupported ELF relocation type %u in '%s' against "
                         "symbol '%s'",
                         (unsigned)reloc_type, image->origin,
                         object->symbols[symbol_index].name
                             ? object->symbols[symbol_index].name
                             : "<unnamed>");
        return 0;
      }

      width = out->kind == LINK_RELOC_ABS64 ? 8u
              : out->kind == LINK_RELOC_NONE ? 0u
                                             : 4u;
      if (width != 0u && (out->offset > target->virtual_size ||
                          target->virtual_size - out->offset < width)) {
        mettle_set_error(error_message_out,
                         "Relocation at offset %llu in '%s' falls outside "
                         "section '%s'",
                         (unsigned long long)out->offset, image->origin,
                         target->name ? target->name : "<unnamed>");
        return 0;
      }
    }
    target->relocation_count += (size_t)count;
  }
  return 1;
}

int elf_object_read_memory(const unsigned char *data, size_t size, const char *origin, LinkObject **object_out, char **error_message_out) {
  ElfImage image = {0};
  LinkObject *object = NULL;

  if (object_out) {
    *object_out = NULL;
  }
  if (!data || !object_out) {
    mettle_set_error(error_message_out,
                     "Invalid arguments while parsing ELF object");
    return 0;
  }

  image.data = data;
  image.size = size;
  image.origin = origin ? origin : "<memory>";

  if (!elf_parse_header(&image, error_message_out)) {
    return 0;
  }

  object = calloc(1, sizeof(LinkObject));
  if (!object) {
    mettle_set_error(error_message_out,
                     "Out of memory while creating ELF object");
    return 0;
  }
  object->format = LINK_FORMAT_ELF;

  if (!elf_parse_sections(&image, object, error_message_out) ||
      !elf_parse_symbols(&image, object, error_message_out) ||
      !elf_parse_relocations(&image, object, error_message_out)) {
    link_object_destroy(object);
    return 0;
  }

  *object_out = object;
  return 1;
}

int elf_object_read(const char *filename, LinkObject **object_out,
                    char **error_message_out) {
  unsigned char *data = NULL;
  size_t size = 0;
  int result = 0;

  if (!link_object_read_file(filename, &data, &size, error_message_out)) {
    return 0;
  }
  result = elf_object_read_memory(data, size, filename, object_out,
                                  error_message_out);
  free(data);
  return result;
}