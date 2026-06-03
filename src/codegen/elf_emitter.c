/* ELF64 (x86-64) relocatable object writer.
 *
 * Consumes the format-neutral BinaryEmitter model (sections, symbols,
 * relocations) and serializes a relocatable ELF object that the system linker
 * (ld/gcc) accepts. The COFF writer for the same model lives in
 * binary_emitter.c; this file is the ELF half of the dispatch in
 * binary_emitter_write_object_file.
 *
 * Layout produced (in file order):
 *   ELF header
 *   section payloads: .text/.rodata/.data (.bss occupies no file bytes)
 *   .rela.<name> payloads (one per content section that has relocations)
 *   .symtab payload
 *   .strtab payload (symbol names)
 *   .shstrtab payload (section header names)
 *   section header table
 *
 * Relocation note: callers built the model for COFF, where the AMD64 REL32
 * relocation type implicitly biases by the 4-byte field width (it resolves
 * S - P with P at the field *end*). ELF R_X86_64_PC32 resolves S + A - P with P
 * at the field *start* and the code buffer stores 0 in the field, so we inject
 * an addend of -4 for PC-relative kinds to reproduce call/branch displacement
 * semantics. Any explicit emitter addend is added on top.
 */

#include "binary_emitter.h"
#include "binary_emitter_internal.h"
#include "../common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- ELF constants (subset we need) --- */
#define ELF_NIDENT 16
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ELFOSABI_SYSV 0
#define ET_REL 1
#define EM_X86_64 62

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_FUNC 2
#define SHN_UNDEF 0

#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4

#define ELF64_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))
#define ELF64_R_INFO(sym, type) (((uint64_t)(sym) << 32) | ((uint32_t)(type)))

/* A growable byte buffer for assembling the string tables before we know their
 * final size. */
typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} ElfStrtab;

static int elf_strtab_init(ElfStrtab *table) {
  /* ELF string tables begin with a NUL so offset 0 is the empty string. */
  table->capacity = 64;
  table->data = malloc(table->capacity);
  if (!table->data) {
    return 0;
  }
  table->data[0] = '\0';
  table->size = 1;
  return 1;
}

static void elf_strtab_free(ElfStrtab *table) {
  free(table->data);
  table->data = NULL;
  table->size = 0;
  table->capacity = 0;
}

/* Appends a name and returns its byte offset within the table. The empty
 * string and NULL both map to offset 0 (the leading NUL). */
static int elf_strtab_add(ElfStrtab *table, const char *name, uint32_t *out) {
  if (!name || name[0] == '\0') {
    *out = 0;
    return 1;
  }
  size_t length = strlen(name) + 1;
  if (table->size + length > table->capacity) {
    size_t new_capacity = table->capacity * 2;
    while (table->size + length > new_capacity) {
      new_capacity *= 2;
    }
    char *grown = realloc(table->data, new_capacity);
    if (!grown) {
      return 0;
    }
    table->data = grown;
    table->capacity = new_capacity;
  }
  *out = (uint32_t)table->size;
  memcpy(table->data + table->size, name, length);
  table->size += length;
  return 1;
}

/* One ELF section header we will emit, plus bookkeeping to locate its payload
 * and tie relocations/symbols back to it. */
typedef struct {
  uint32_t name_offset; /* into .shstrtab */
  uint32_t type;
  uint64_t flags;
  uint64_t offset; /* file offset of payload */
  uint64_t size;
  uint32_t link;
  uint32_t info;
  uint64_t addralign;
  uint64_t entsize;
} ElfSectionHeader;

static uint32_t elf_section_flags(BinarySectionKind kind) {
  switch (kind) {
  case BINARY_SECTION_TEXT:
    return SHF_ALLOC | SHF_EXECINSTR;
  case BINARY_SECTION_RDATA:
    return SHF_ALLOC;
  case BINARY_SECTION_DATA:
  case BINARY_SECTION_BSS:
  case BINARY_SECTION_INIT_ARRAY:
  case BINARY_SECTION_FINI_ARRAY:
    return SHF_ALLOC | SHF_WRITE;
  case BINARY_SECTION_DEBUG:
    return 0;
  default:
    return SHF_ALLOC;
  }
}

static uint32_t elf_section_type(BinarySectionKind kind) {
  switch (kind) {
  case BINARY_SECTION_BSS:
    return SHT_NOBITS;
  case BINARY_SECTION_INIT_ARRAY:
    return SHT_INIT_ARRAY;
  case BINARY_SECTION_FINI_ARRAY:
    return SHT_FINI_ARRAY;
  default:
    return SHT_PROGBITS;
  }
}

/* Translates an abstract relocation kind into an ELF x86-64 relocation type and
 * the implicit addend that reproduces the model's COFF-derived semantics. */
static int elf_map_relocation(BinaryRelocationKind kind, uint32_t *type_out,
                              int64_t *implicit_addend_out) {
  switch (kind) {
  case BINARY_RELOCATION_REL32:
    *type_out = R_X86_64_PC32;
    *implicit_addend_out = -4;
    return 1;
  case BINARY_RELOCATION_ADDR64:
    *type_out = R_X86_64_64;
    *implicit_addend_out = 0;
    return 1;
  default:
    /* ADDR32NB / SECTION_REL32 are COFF debug-table relocations with no direct
     * ELF analogue; debug tables are not emitted on ELF yet. */
    return 0;
  }
}

static int elf_write_all(FILE *file, const void *data, size_t size) {
  return size == 0 || fwrite(data, 1, size, file) == size;
}

static int elf_pad_to(FILE *file, uint64_t current, uint64_t target) {
  static const unsigned char zeros[16] = {0};
  while (current < target) {
    uint64_t chunk = target - current;
    if (chunk > sizeof(zeros)) {
      chunk = sizeof(zeros);
    }
    if (fwrite(zeros, 1, (size_t)chunk, file) != chunk) {
      return 0;
    }
    current += chunk;
  }
  return 1;
}

static uint64_t elf_align_up(uint64_t value, uint64_t align) {
  if (align <= 1) {
    return value;
  }
  uint64_t remainder = value % align;
  return remainder ? value + (align - remainder) : value;
}

int binary_emitter_write_elf_object_file(BinaryEmitter *emitter,
                                         const char *filename) {
  /* Section header layout:
   *   [0]                 SHT_NULL
   *   [1 .. N]            one per emitter content section (.text, ...)
   *   per content section with relocs: a .rela.<name> section
   *   [.symtab]
   *   [.strtab]
   *   [.shstrtab]
   */
  size_t content_count = emitter->section_count;
  int ok = 0;
  FILE *file = NULL;

  ElfStrtab shstrtab = {0};
  ElfStrtab strtab = {0};
  ElfSectionHeader *headers = NULL;
  size_t header_count = 0;
  size_t header_capacity = 0;

  /* Maps emitter content-section index -> ELF section header index. */
  uint32_t *content_shndx = NULL;
  /* Maps emitter symbol index -> .symtab entry index. */
  uint32_t *symbol_symtab_index = NULL;
  /* Number of relocations targeting each content section. */
  size_t *reloc_counts = NULL;
  /* ELF header index of the .rela section for each content section, or 0. */
  size_t *rela_shndx = NULL;

  unsigned char *symtab_bytes = NULL;
  size_t symtab_size = 0;

  if (!elf_strtab_init(&shstrtab) || !elf_strtab_init(&strtab)) {
    binary_emitter_record_error(emitter,
                                "Out of memory preparing ELF string tables");
    goto cleanup;
  }

  if (content_count > 0) {
    content_shndx = calloc(content_count, sizeof(uint32_t));
    reloc_counts = calloc(content_count, sizeof(size_t));
    rela_shndx = calloc(content_count, sizeof(size_t));
    if (!content_shndx || !reloc_counts || !rela_shndx) {
      binary_emitter_record_error(emitter, "Out of memory preparing ELF tables");
      goto cleanup;
    }
  }
  if (emitter->symbol_count > 0) {
    symbol_symtab_index = calloc(emitter->symbol_count, sizeof(uint32_t));
    if (!symbol_symtab_index) {
      binary_emitter_record_error(emitter, "Out of memory preparing ELF tables");
      goto cleanup;
    }
  }

  /* Tally relocations per section and validate their kinds up front. */
  for (size_t i = 0; i < emitter->relocation_count; i++) {
    const BinaryRelocation *reloc = &emitter->relocations[i];
    uint32_t type;
    int64_t addend;
    if (reloc->section_index >= content_count) {
      binary_emitter_record_error(emitter,
                                  "ELF relocation refers to an invalid section");
      goto cleanup;
    }
    if (!elf_map_relocation(reloc->kind, &type, &addend)) {
      binary_emitter_record_error(
          emitter, "Unsupported relocation kind for ELF object output");
      goto cleanup;
    }
    reloc_counts[reloc->section_index]++;
  }

  /* Reserve header slots: null + content + one rela per relocated content
   * section + symtab + strtab + shstrtab. */
  header_capacity = 1 + content_count;
  for (size_t i = 0; i < content_count; i++) {
    if (reloc_counts[i] > 0) {
      header_capacity++;
    }
  }
  header_capacity += 3;
  header_capacity += 1; /* .note.GNU-stack */
  headers = calloc(header_capacity, sizeof(ElfSectionHeader));
  if (!headers) {
    binary_emitter_record_error(emitter, "Out of memory preparing ELF headers");
    goto cleanup;
  }

  /* [0] null section. */
  header_count = 1;

  /* Content sections. */
  for (size_t i = 0; i < content_count; i++) {
    const BinarySection *section = &emitter->sections[i];
    ElfSectionHeader *header = &headers[header_count];
    if (!elf_strtab_add(&shstrtab, section->name, &header->name_offset)) {
      binary_emitter_record_error(emitter, "Out of memory naming ELF section");
      goto cleanup;
    }
    header->type = elf_section_type(section->kind);
    header->flags = elf_section_flags(section->kind);
    header->size = section->kind == BINARY_SECTION_BSS
                       ? (section->virtual_size > section->size
                              ? section->virtual_size
                              : section->size)
                       : section->size;
    header->addralign = section->alignment ? section->alignment : 1;
    content_shndx[i] = (uint32_t)header_count;
    header_count++;
  }

  /* An empty, non-executable .note.GNU-stack section. Its presence (with no
   * SHF_EXECINSTR flag) tells the linker the program does not need an
   * executable stack; without it ld defaults to marking the stack executable
   * and warns. Zero-size with no payload, so it does not affect offsets. */
  {
    ElfSectionHeader *header = &headers[header_count];
    if (!elf_strtab_add(&shstrtab, ".note.GNU-stack", &header->name_offset)) {
      binary_emitter_record_error(emitter,
                                  "Out of memory naming .note.GNU-stack");
      goto cleanup;
    }
    header->type = SHT_PROGBITS;
    header->flags = 0;
    header->size = 0;
    header->addralign = 1;
    header_count++;
  }

  /* Build .symtab. ELF requires all local symbols before global ones, and the
   * symtab's sh_info must be the index of the first global. Entry 0 is the
   * reserved undefined symbol. We emit locals first, then globals/externals. */
  {
    size_t entry_count = 1 + emitter->symbol_count;
    symtab_size = entry_count * 24; /* sizeof(Elf64_Sym) */
    symtab_bytes = calloc(entry_count, 24);
    if (!symtab_bytes) {
      binary_emitter_record_error(emitter, "Out of memory building ELF symtab");
      goto cleanup;
    }

    size_t cursor = 1; /* entry 0 left zeroed (STN_UNDEF) */
    uint32_t first_global = (uint32_t)entry_count; /* default: no globals */

    /* Pass 1: locals. */
    for (size_t pass = 0; pass < 2; pass++) {
      int want_local = (pass == 0);
      if (pass == 1) {
        first_global = (uint32_t)cursor;
      }
      for (size_t i = 0; i < emitter->symbol_count; i++) {
        const BinarySymbol *symbol = &emitter->symbols[i];
        int is_local = (symbol->binding == BINARY_SYMBOL_LOCAL);
        if (is_local != want_local) {
          continue;
        }

        uint32_t name_offset;
        if (!elf_strtab_add(&strtab, symbol->name, &name_offset)) {
          binary_emitter_record_error(emitter,
                                      "Out of memory naming ELF symbol");
          goto cleanup;
        }

        uint16_t shndx;
        unsigned char type = STT_NOTYPE;
        unsigned char bind;
        if (symbol->section_index == BINARY_EMITTER_SECTION_INDEX_NONE) {
          shndx = SHN_UNDEF;
          bind = STB_GLOBAL; /* undefined references are global/external */
        } else {
          if (symbol->section_index >= content_count) {
            binary_emitter_record_error(emitter,
                                        "ELF symbol refers to an invalid section");
            goto cleanup;
          }
          shndx = (uint16_t)content_shndx[symbol->section_index];
          if (emitter->sections[symbol->section_index].kind ==
              BINARY_SECTION_TEXT) {
            type = STT_FUNC;
          }
          bind = is_local ? STB_LOCAL : STB_GLOBAL;
        }

        unsigned char *entry = symtab_bytes + cursor * 24;
        uint32_t st_name = name_offset;
        unsigned char st_info = ELF64_ST_INFO(bind, type);
        unsigned char st_other = 0;
        uint16_t st_shndx = shndx;
        uint64_t st_value = symbol->value;
        uint64_t st_size = symbol->size;
        memcpy(entry + 0, &st_name, 4);
        entry[4] = st_info;
        entry[5] = st_other;
        memcpy(entry + 6, &st_shndx, 2);
        memcpy(entry + 8, &st_value, 8);
        memcpy(entry + 16, &st_size, 8);

        symbol_symtab_index[i] = (uint32_t)cursor;
        cursor++;
      }
    }

    /* If there were no globals at all, sh_info points just past the locals. */
    if (first_global > (uint32_t)cursor) {
      first_global = (uint32_t)cursor;
    }

    /* .rela sections, one per content section with relocations. They must
     * precede symtab in our index assignment-independent layout, but the link
     * fields reference symtab, so reserve symtab's index now. */
    uint32_t symtab_index =
        (uint32_t)(header_count + /* rela sections counted next */ 0);
    /* Count rela sections to know symtab's eventual index. */
    size_t rela_section_count = 0;
    for (size_t i = 0; i < content_count; i++) {
      if (reloc_counts[i] > 0) {
        rela_section_count++;
      }
    }
    symtab_index = (uint32_t)(header_count + rela_section_count);

    /* Emit .rela.<name> headers. */
    for (size_t i = 0; i < content_count; i++) {
      if (reloc_counts[i] == 0) {
        continue;
      }
      ElfSectionHeader *header = &headers[header_count];
      char rela_name[64];
      const char *base = emitter->sections[i].name ? emitter->sections[i].name
                                                   : ".sec";
      snprintf(rela_name, sizeof(rela_name), ".rela%s", base);
      if (!elf_strtab_add(&shstrtab, rela_name, &header->name_offset)) {
        binary_emitter_record_error(emitter,
                                    "Out of memory naming ELF rela section");
        goto cleanup;
      }
      header->type = SHT_RELA;
      header->flags = 0;
      header->size = reloc_counts[i] * 24; /* sizeof(Elf64_Rela) */
      header->addralign = 8;
      header->entsize = 24;
      header->link = symtab_index;
      header->info = content_shndx[i];
      rela_shndx[i] = header_count;
      header_count++;
    }

    /* .symtab header. */
    {
      ElfSectionHeader *header = &headers[header_count];
      if (!elf_strtab_add(&shstrtab, ".symtab", &header->name_offset)) {
        binary_emitter_record_error(emitter, "Out of memory naming .symtab");
        goto cleanup;
      }
      header->type = SHT_SYMTAB;
      header->size = symtab_size;
      header->addralign = 8;
      header->entsize = 24;
      header->link = (uint32_t)(header_count + 1); /* .strtab follows */
      header->info = first_global;
      header_count++;
    }
  }

  /* .strtab header. */
  {
    ElfSectionHeader *header = &headers[header_count];
    if (!elf_strtab_add(&shstrtab, ".strtab", &header->name_offset)) {
      binary_emitter_record_error(emitter, "Out of memory naming .strtab");
      goto cleanup;
    }
    header->type = SHT_STRTAB;
    header->size = strtab.size;
    header->addralign = 1;
    header_count++;
  }

  /* .shstrtab header (its own name must be in itself). */
  size_t shstrtab_header_index = header_count;
  {
    ElfSectionHeader *header = &headers[header_count];
    if (!elf_strtab_add(&shstrtab, ".shstrtab", &header->name_offset)) {
      binary_emitter_record_error(emitter, "Out of memory naming .shstrtab");
      goto cleanup;
    }
    header->type = SHT_STRTAB;
    header->addralign = 1;
    header_count++;
  }
  headers[shstrtab_header_index].size = shstrtab.size;

  /* --- Assign file offsets. ELF header is 64 bytes; section headers are 64
   * bytes each. Payloads go between the ELF header and the section header
   * table. --- */
  uint64_t offset = 64;
  /* Content payloads (NOBITS/.bss take no file space). */
  for (size_t i = 0; i < content_count; i++) {
    ElfSectionHeader *header = &headers[content_shndx[i]];
    if (header->type == SHT_NOBITS) {
      header->offset = offset; /* conventional, occupies no bytes */
      continue;
    }
    offset = elf_align_up(offset, header->addralign);
    header->offset = offset;
    offset += header->size;
  }
  /* rela payloads. */
  for (size_t i = 0; i < content_count; i++) {
    if (reloc_counts[i] == 0) {
      continue;
    }
    ElfSectionHeader *header = &headers[rela_shndx[i]];
    offset = elf_align_up(offset, 8);
    header->offset = offset;
    offset += header->size;
  }
  /* symtab, strtab, shstrtab — find them by walking the tail headers. They were
   * appended in this order: [rela...] symtab strtab shstrtab. */
  size_t symtab_header_index = shstrtab_header_index - 2;
  size_t strtab_header_index = shstrtab_header_index - 1;
  offset = elf_align_up(offset, 8);
  headers[symtab_header_index].offset = offset;
  offset += headers[symtab_header_index].size;
  headers[strtab_header_index].offset = offset;
  offset += headers[strtab_header_index].size;
  headers[shstrtab_header_index].offset = offset;
  offset += headers[shstrtab_header_index].size;

  offset = elf_align_up(offset, 8);
  uint64_t section_header_offset = offset;

  /* --- Write the file. --- */
  file = fopen(filename, "wb");
  if (!file) {
    binary_emitter_record_error(emitter, "Failed to open ELF output file");
    goto cleanup;
  }
  setvbuf(file, NULL, _IOFBF, 1 << 20);

  /* ELF header (Elf64_Ehdr, 64 bytes). */
  {
    unsigned char e_ident[ELF_NIDENT] = {0};
    e_ident[0] = 0x7f;
    e_ident[1] = 'E';
    e_ident[2] = 'L';
    e_ident[3] = 'F';
    e_ident[4] = ELFCLASS64;
    e_ident[5] = ELFDATA2LSB;
    e_ident[6] = EV_CURRENT;
    e_ident[7] = ELFOSABI_SYSV;
    if (!elf_write_all(file, e_ident, ELF_NIDENT)) {
      goto write_error;
    }
    uint16_t e_type = ET_REL;
    uint16_t e_machine = EM_X86_64;
    uint32_t e_version = EV_CURRENT;
    uint64_t e_entry = 0;
    uint64_t e_phoff = 0;
    uint64_t e_shoff = section_header_offset;
    uint32_t e_flags = 0;
    uint16_t e_ehsize = 64;
    uint16_t e_phentsize = 0;
    uint16_t e_phnum = 0;
    uint16_t e_shentsize = 64;
    uint16_t e_shnum = (uint16_t)header_count;
    uint16_t e_shstrndx = (uint16_t)shstrtab_header_index;
    if (!elf_write_all(file, &e_type, 2) ||
        !elf_write_all(file, &e_machine, 2) ||
        !elf_write_all(file, &e_version, 4) ||
        !elf_write_all(file, &e_entry, 8) ||
        !elf_write_all(file, &e_phoff, 8) ||
        !elf_write_all(file, &e_shoff, 8) ||
        !elf_write_all(file, &e_flags, 4) ||
        !elf_write_all(file, &e_ehsize, 2) ||
        !elf_write_all(file, &e_phentsize, 2) ||
        !elf_write_all(file, &e_phnum, 2) ||
        !elf_write_all(file, &e_shentsize, 2) ||
        !elf_write_all(file, &e_shnum, 2) ||
        !elf_write_all(file, &e_shstrndx, 2)) {
      goto write_error;
    }
  }

  uint64_t written = 64;

  /* Content payloads. */
  for (size_t i = 0; i < content_count; i++) {
    const BinarySection *section = &emitter->sections[i];
    ElfSectionHeader *header = &headers[content_shndx[i]];
    if (header->type == SHT_NOBITS) {
      continue;
    }
    if (!elf_pad_to(file, written, header->offset)) {
      goto write_error;
    }
    written = header->offset;
    if (!elf_write_all(file, section->data, section->size)) {
      goto write_error;
    }
    written += section->size;
  }

  /* rela payloads. */
  for (size_t i = 0; i < content_count; i++) {
    if (reloc_counts[i] == 0) {
      continue;
    }
    ElfSectionHeader *header = &headers[rela_shndx[i]];
    if (!elf_pad_to(file, written, header->offset)) {
      goto write_error;
    }
    written = header->offset;
    for (size_t r = 0; r < emitter->relocation_count; r++) {
      const BinaryRelocation *reloc = &emitter->relocations[r];
      if (reloc->section_index != i) {
        continue;
      }
      uint32_t type = 0;
      int64_t implicit_addend = 0;
      if (!elf_map_relocation(reloc->kind, &type, &implicit_addend)) {
        binary_emitter_record_error(emitter,
                                    "Unsupported relocation kind for ELF");
        goto write_error;
      }

      int symbol_index =
          binary_emitter_lookup_symbol_index(emitter, reloc->symbol_name);
      if (symbol_index < 0) {
        binary_emitter_record_error(
            emitter, "ELF relocation refers to an undefined symbol");
        goto write_error;
      }
      uint64_t r_offset = reloc->offset;
      uint64_t r_info =
          ELF64_R_INFO(symbol_symtab_index[(size_t)symbol_index], type);
      int64_t r_addend = implicit_addend + reloc->addend;
      if (!elf_write_all(file, &r_offset, 8) ||
          !elf_write_all(file, &r_info, 8) ||
          !elf_write_all(file, &r_addend, 8)) {
        goto write_error;
      }
      written += 24;
    }
  }

  /* symtab. */
  if (!elf_pad_to(file, written, headers[symtab_header_index].offset)) {
    goto write_error;
  }
  written = headers[symtab_header_index].offset;
  if (!elf_write_all(file, symtab_bytes, symtab_size)) {
    goto write_error;
  }
  written += symtab_size;

  /* strtab. */
  if (!elf_pad_to(file, written, headers[strtab_header_index].offset)) {
    goto write_error;
  }
  written = headers[strtab_header_index].offset;
  if (!elf_write_all(file, strtab.data, strtab.size)) {
    goto write_error;
  }
  written += strtab.size;

  /* shstrtab. */
  if (!elf_pad_to(file, written, headers[shstrtab_header_index].offset)) {
    goto write_error;
  }
  written = headers[shstrtab_header_index].offset;
  if (!elf_write_all(file, shstrtab.data, shstrtab.size)) {
    goto write_error;
  }
  written += shstrtab.size;

  /* Section header table. */
  if (!elf_pad_to(file, written, section_header_offset)) {
    goto write_error;
  }
  for (size_t i = 0; i < header_count; i++) {
    const ElfSectionHeader *h = &headers[i];
    uint32_t sh_name = h->name_offset;
    uint32_t sh_type = h->type;
    uint64_t sh_flags = h->flags;
    uint64_t sh_addr = 0;
    uint64_t sh_offset = h->offset;
    uint64_t sh_size = h->size;
    uint32_t sh_link = h->link;
    uint32_t sh_info = h->info;
    uint64_t sh_addralign = h->addralign;
    uint64_t sh_entsize = h->entsize;
    if (i == 0) {
      /* Null section header is all zeros. */
      sh_name = 0;
      sh_type = 0;
      sh_flags = 0;
      sh_offset = 0;
      sh_size = 0;
      sh_link = 0;
      sh_info = 0;
      sh_addralign = 0;
      sh_entsize = 0;
    }
    if (!elf_write_all(file, &sh_name, 4) ||
        !elf_write_all(file, &sh_type, 4) ||
        !elf_write_all(file, &sh_flags, 8) ||
        !elf_write_all(file, &sh_addr, 8) ||
        !elf_write_all(file, &sh_offset, 8) ||
        !elf_write_all(file, &sh_size, 8) ||
        !elf_write_all(file, &sh_link, 4) ||
        !elf_write_all(file, &sh_info, 4) ||
        !elf_write_all(file, &sh_addralign, 8) ||
        !elf_write_all(file, &sh_entsize, 8)) {
      goto write_error;
    }
  }

  ok = 1;
  goto cleanup;

write_error:
  binary_emitter_record_error(emitter, "Failed while writing ELF object file");

cleanup:
  if (file) {
    fclose(file);
    if (!ok) {
      remove(filename);
    }
  }
  elf_strtab_free(&shstrtab);
  elf_strtab_free(&strtab);
  free(headers);
  free(content_shndx);
  free(symbol_symtab_index);
  free(reloc_counts);
  free(rela_shndx);
  free(symtab_bytes);
  return ok;
}
