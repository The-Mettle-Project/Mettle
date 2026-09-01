#include "linker/elf_dynamic.h"
#include "linker/linker_common.h"
#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELF_DYN_SYM_SIZE 24u
#define ELF_DYN_RELA_SIZE 24u
#define ELF_DYN_ENTRY_SIZE 16u
#define ELF_DYN_PLT_ENTRY_SIZE 16u
#define ELF_DYN_GOT_RESERVED 3u
#define ELF_DYN_VERNEED_SIZE 16u
#define ELF_DYN_VERNAUX_SIZE 16u

#define SHT_PROGBITS 1u
#define SHT_NOBITS 8u
#define SHT_DYNAMIC 6u
#define SHT_STRTAB 3u
#define SHT_HASH 5u
#define SHT_RELA 4u
#define SHT_DYNSYM 11u
#define SHT_GNU_VERNEED 0x6ffffffeu
#define SHT_GNU_VERSYM 0x6fffffffu

#define SHF_WRITE 1u
#define SHF_ALLOC 2u
#define SHF_EXECINSTR 4u
#define SHF_INFO_LINK 0x40u

#define STB_GLOBAL 1u
#define STB_WEAK 2u

#define R_X86_64_64 1u
#define R_X86_64_COPY 5u
#define R_X86_64_GLOB_DAT 6u
#define R_X86_64_JUMP_SLOT 7u
#define R_X86_64_RELATIVE 8u

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_SONAME 14
#define DT_SYMBOLIC 16
#define DT_REL 17
#define DT_TEXTREL 22
#define DT_JMPREL 23
#define DT_BIND_NOW 24
#define DT_FLAGS 30
#define DT_RUNPATH 29
#define DT_DEBUG 21
#define DT_PLTREL 20
#define DT_FLAGS_1 0x6ffffffb
#define DT_VERSYM 0x6ffffff0
#define DT_VERNEED 0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

#define DF_SYMBOLIC 0x02u
#define DF_TEXTREL 0x04u
#define DF_BIND_NOW 0x08u
#define DF_1_NOW 0x00000001u

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} ElfDynamicStrings;

typedef struct {
  uint32_t name_offset;
  uint8_t info;
  uint16_t version_index;
  size_t import_index;
  size_t resolution_symbol_index;
  int is_definition;
} ElfDynamicSymbolRecord;

typedef struct {
  size_t library_index;
  uint32_t file_name_offset;
  uint32_t *version_hashes;
  uint32_t *version_name_offsets;
  uint16_t *version_indices;
  char **version_names;
  size_t version_count;
  size_t version_capacity;
} ElfDynamicNeeded;

struct ElfDynamicPlan {
  int active;
  int produce_shared_library;
  int export_dynamic;
  int needs_text_relocations;
  int uses_versions;
  LinkResolution *resolution;
  char *interpreter;
  char *soname;
  const char *const *runpaths;
  size_t runpath_count;
  uint64_t image_base;

  ElfDynamicStrings strings;
  uint32_t soname_offset;
  uint32_t runpath_offset;
  char *runpath;
  ElfDynamicSymbolRecord *symbols;
  size_t symbol_count;
  size_t symbol_capacity;

  ElfDynamicNeeded *needed;
  size_t needed_count;

  size_t plt_count;
  size_t got_count;
  size_t rela_dyn_count;
  size_t rela_plt_count;
  size_t dynamic_entry_count;
  uint16_t next_version_index;

  ElfDynamicBlob blobs[ELF_DYNAMIC_BLOB_COUNT];
};

static uint32_t elf_dynamic_hash(const char *name) {
  uint32_t hash = 0u;
  uint32_t high = 0u;

  while (*name) {
    hash = (hash << 4) + (unsigned char)*name++;
    high = hash & 0xF0000000u;
    if (high) {
      hash ^= high >> 24;
    }
    hash &= ~high;
  }
  return hash;
}

static int elf_dynamic_strings_init(ElfDynamicStrings *strings) {
  strings->capacity = 256u;
  strings->data = (char *)calloc(strings->capacity, 1u);
  if (!strings->data) {
    return 0;
  }
  strings->size = 1u;
  return 1;
}

static int elf_dynamic_strings_add(ElfDynamicStrings *strings, const char *text,
                                   uint32_t *offset_out) {
  size_t length = 0u;

  if (!text) {
    *offset_out = 0u;
    return 1;
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
      return 0;
    }
    memset(grown + strings->size, 0, capacity - strings->size);
    strings->data = grown;
    strings->capacity = capacity;
  }
  *offset_out = (uint32_t)strings->size;
  memcpy(strings->data + strings->size, text, length);
  strings->size += length;
  return 1;
}

static int elf_dynamic_reserve_symbols(ElfDynamicPlan *plan,
                                       size_t minimum_count) {
  ElfDynamicSymbolRecord *grown = NULL;
  size_t capacity = plan->symbol_capacity;

  if (capacity >= minimum_count) {
    return 1;
  }
  capacity = capacity ? capacity : 32u;
  while (capacity < minimum_count) {
    capacity *= 2u;
  }
  grown = (ElfDynamicSymbolRecord *)realloc(
      plan->symbols, capacity * sizeof(ElfDynamicSymbolRecord));
  if (!grown) {
    return 0;
  }
  memset(grown + plan->symbol_capacity, 0,
         (capacity - plan->symbol_capacity) * sizeof(ElfDynamicSymbolRecord));
  plan->symbols = grown;
  plan->symbol_capacity = capacity;
  return 1;
}

static int elf_dynamic_needed_add_version(ElfDynamicPlan *plan,
                                          ElfDynamicNeeded *needed,
                                          const char *version,
                                          uint16_t *index_out) {
  size_t i = 0u;
  uint32_t name_offset = 0u;

  for (i = 0u; i < needed->version_count; i++) {
    if (strcmp(needed->version_names[i], version) == 0) {
      *index_out = needed->version_indices[i];
      return 1;
    }
  }
  if (needed->version_count == needed->version_capacity) {
    size_t capacity = needed->version_capacity ? needed->version_capacity * 2u : 4u;
    uint32_t *hashes = (uint32_t *)realloc(needed->version_hashes,
                                           capacity * sizeof(uint32_t));
    uint32_t *offsets = NULL;
    uint16_t *indices = NULL;
    char **names = NULL;

    if (!hashes) {
      return 0;
    }
    needed->version_hashes = hashes;
    offsets = (uint32_t *)realloc(needed->version_name_offsets,
                                  capacity * sizeof(uint32_t));
    if (!offsets) {
      return 0;
    }
    needed->version_name_offsets = offsets;
    indices = (uint16_t *)realloc(needed->version_indices,
                                  capacity * sizeof(uint16_t));
    if (!indices) {
      return 0;
    }
    needed->version_indices = indices;
    names = (char **)realloc(needed->version_names, capacity * sizeof(char *));
    if (!names) {
      return 0;
    }
    needed->version_names = names;
    needed->version_capacity = capacity;
  }

  if (!elf_dynamic_strings_add(&plan->strings, version, &name_offset)) {
    return 0;
  }
  needed->version_names[needed->version_count] = mettle_strdup(version);
  if (!needed->version_names[needed->version_count]) {
    return 0;
  }
  needed->version_hashes[needed->version_count] = elf_dynamic_hash(version);
  needed->version_name_offsets[needed->version_count] = name_offset;
  needed->version_indices[needed->version_count] = plan->next_version_index;
  *index_out = plan->next_version_index;
  plan->next_version_index++;
  needed->version_count++;
  plan->uses_versions = 1;
  return 1;
}

static ElfDynamicNeeded *elf_dynamic_find_needed(ElfDynamicPlan *plan,
                                                 size_t library_index) {
  size_t i = 0u;

  for (i = 0u; i < plan->needed_count; i++) {
    if (plan->needed[i].library_index == library_index) {
      return &plan->needed[i];
    }
  }
  return NULL;
}

static int elf_dynamic_build_needed(ElfDynamicPlan *plan) {
  LinkResolution *resolution = plan->resolution;
  size_t used = 0u;
  size_t i = 0u;

  for (i = 0u; i < resolution->shared_library_count; i++) {
    if (resolution->shared_library_used[i]) {
      used++;
    }
  }
  if (used == 0u) {
    return 1;
  }
  plan->needed = (ElfDynamicNeeded *)calloc(used, sizeof(ElfDynamicNeeded));
  if (!plan->needed) {
    return 0;
  }
  for (i = 0u; i < resolution->shared_library_count; i++) {
    ElfDynamicNeeded *entry = NULL;

    if (!resolution->shared_library_used[i]) {
      continue;
    }
    entry = &plan->needed[plan->needed_count];
    entry->library_index = i;
    if (!elf_dynamic_strings_add(&plan->strings,
                                 resolution->shared_libraries[i]->soname,
                                 &entry->file_name_offset)) {
      return 0;
    }
    plan->needed_count++;
  }
  return 1;
}

static int elf_dynamic_classify_imports(ElfDynamicPlan *plan,
                                        char **error_message_out) {
  LinkResolution *resolution = plan->resolution;
  size_t i = 0u;

  for (i = 0u; i < resolution->shared_import_count; i++) {
    LinkedSharedImport *import = &resolution->shared_imports[i];
    const LinkedSymbol *symbol = &resolution->symbols[import->symbol_index];

    if (import->type == ELF_SHARED_TYPE_TLS) {
      mettle_set_error(error_message_out,
                       "Symbol '%s' is thread-local storage in a shared "
                       "library; this linker cannot bind dynamic TLS",
                       symbol->name ? symbol->name : "<unnamed>");
      return 0;
    }
    if (import->type == ELF_SHARED_TYPE_OBJECT) {
      if (plan->produce_shared_library) {
        mettle_set_error(
            error_message_out,
            "Shared object output cannot reference the data symbol '%s' from "
            "another shared library; the backend emits absolute addresses, "
            "which a loaded library cannot resolve",
            symbol->name ? symbol->name : "<unnamed>");
        return 0;
      }
      if (import->size == 0u) {
        mettle_set_error(error_message_out,
                         "Shared library data symbol '%s' has no recorded size, "
                         "so no storage can be reserved for it",
                         symbol->name ? symbol->name : "<unnamed>");
        return 0;
      }
      import->needs_copy = 1;
    } else {
      import->needs_plt = 1;
      plan->plt_count++;
    }
  }
  return 1;
}

static int elf_dynamic_add_symbol_record(ElfDynamicPlan *plan, const char *name,
                                         uint8_t info, size_t import_index,
                                         size_t resolution_symbol_index,
                                         int is_definition,
                                         uint16_t version_index,
                                         uint32_t *index_out) {
  ElfDynamicSymbolRecord *record = NULL;
  uint32_t name_offset = 0u;

  if (!elf_dynamic_reserve_symbols(plan, plan->symbol_count + 1u)) {
    return 0;
  }
  if (!elf_dynamic_strings_add(&plan->strings, name, &name_offset)) {
    return 0;
  }
  record = &plan->symbols[plan->symbol_count];
  record->name_offset = name_offset;
  record->info = info;
  record->import_index = import_index;
  record->resolution_symbol_index = resolution_symbol_index;
  record->is_definition = is_definition;
  record->version_index = version_index;
  *index_out = (uint32_t)plan->symbol_count;
  plan->symbol_count++;
  return 1;
}

static int elf_dynamic_build_symbols(ElfDynamicPlan *plan,
                                     char **error_message_out) {
  LinkResolution *resolution = plan->resolution;
  uint32_t index = 0u;
  size_t i = 0u;

  if (!elf_dynamic_reserve_symbols(plan, 1u)) {
    return 0;
  }
  memset(&plan->symbols[0], 0, sizeof(plan->symbols[0]));
  plan->symbols[0].import_index = LINKED_SECTION_INDEX_NONE;
  plan->symbols[0].resolution_symbol_index = LINKED_SECTION_INDEX_NONE;
  plan->symbol_count = 1u;

  for (i = 0u; i < resolution->shared_import_count; i++) {
    LinkedSharedImport *import = &resolution->shared_imports[i];
    const LinkedSymbol *symbol = &resolution->symbols[import->symbol_index];
    uint8_t bind = import->is_weak ? (uint8_t)STB_WEAK : (uint8_t)STB_GLOBAL;
    uint8_t type = import->needs_copy ? (uint8_t)ELF_SHARED_TYPE_OBJECT
                                      : import->type;
    uint16_t version_index = 0u;

    if (import->version && import->library_index != LINKED_LIBRARY_INDEX_NONE) {
      ElfDynamicNeeded *needed =
          elf_dynamic_find_needed(plan, import->library_index);
      if (needed && !elf_dynamic_needed_add_version(plan, needed,
                                                    import->version,
                                                    &version_index)) {
        mettle_set_error(error_message_out,
                         "Out of memory while recording version requirements");
        return 0;
      }
    }
    if (!elf_dynamic_add_symbol_record(plan, symbol->name,
                                       (uint8_t)((bind << 4) | type), i,
                                       import->symbol_index,
                                       import->needs_copy, version_index,
                                       &index)) {
      mettle_set_error(error_message_out,
                       "Out of memory while building the dynamic symbol table");
      return 0;
    }
    import->dynamic_symbol_index = index;
  }

  if (!plan->produce_shared_library && !plan->export_dynamic) {
    return 1;
  }

  for (i = 0u; i < resolution->symbol_count; i++) {
    const LinkedSymbol *symbol = &resolution->symbols[i];
    uint8_t type = ELF_SHARED_TYPE_NOTYPE;

    if (!symbol->is_defined || !symbol->is_external || !symbol->name ||
        symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
      continue;
    }
    /* The bundled runtime rides along so the library is self-contained, but
     * publishing its malloc and its memcpy would interpose them on whoever
     * loads the result. Only what the program itself defines is exported. */
    if (symbol->defining_object_index < resolution->object_count &&
        resolution->objects[symbol->defining_object_index].is_runtime_default) {
      continue;
    }
    type = symbol->elf_type ? symbol->elf_type
                            : (resolution->sections[symbol->merged_section_index]
                                           .kind == LINK_SECTION_KIND_TEXT
                                   ? (uint8_t)ELF_SHARED_TYPE_FUNC
                                   : (uint8_t)ELF_SHARED_TYPE_OBJECT);
    if (!elf_dynamic_add_symbol_record(plan, symbol->name,
                                       (uint8_t)((STB_GLOBAL << 4) | type),
                                       LINKED_SECTION_INDEX_NONE, i, 1, 0u,
                                       &index)) {
      mettle_set_error(error_message_out,
                       "Out of memory while exporting '%s'", symbol->name);
      return 0;
    }
  }
  return 1;
}

typedef int (*ElfDynamicSiteVisitor)(ElfDynamicPlan *plan,
                                     LinkedSection *merged,
                                     size_t patch_offset,
                                     const LinkedSymbol *global,
                                     void *user, char **error_message_out);

static int elf_dynamic_visit_absolute_sites(ElfDynamicPlan *plan,
                                            ElfDynamicSiteVisitor visit,
                                            void *user,
                                            char **error_message_out) {
  LinkResolution *resolution = plan->resolution;
  size_t object_index = 0u;

  for (object_index = 0u; object_index < resolution->object_count;
       object_index++) {
    const LinkedInputObject *input = &resolution->objects[object_index];
    size_t section_index = 0u;

    if (!input->object) {
      continue;
    }
    for (section_index = 0u; section_index < input->object->section_count;
         section_index++) {
      const LinkSection *source = &input->object->sections[section_index];
      size_t merged_index = input->section_merged_indices[section_index];
      size_t relocation_index = 0u;

      if (source->relocation_count == 0u ||
          merged_index == LINKED_SECTION_INDEX_NONE) {
        continue;
      }
      if (input->section_gc_dead && input->section_gc_dead[section_index]) {
        continue;
      }
      for (relocation_index = 0u; relocation_index < source->relocation_count;
           relocation_index++) {
        const LinkReloc *relocation = &source->relocations[relocation_index];
        const LinkedObjectSymbol *object_symbol = NULL;
        const LinkedSymbol *global = NULL;
        size_t patch_offset = 0u;

        if (relocation->kind == LINK_RELOC_ABS32 ||
            relocation->kind == LINK_RELOC_IMAGE_REL32) {
          if (relocation->symbol_index < input->symbol_count) {
            object_symbol = &input->symbols[relocation->symbol_index];
          }
          mettle_set_error(
              error_message_out,
              "Shared object output cannot use the 32-bit absolute address of "
              "'%s' in '%s'; the code must be position independent",
              object_symbol && object_symbol->name ? object_symbol->name
                                                   : "<unnamed>",
              input->path ? input->path : "<unknown>");
          return 0;
        }
        if (relocation->kind != LINK_RELOC_ABS64) {
          continue;
        }
        if (relocation->symbol_index >= input->symbol_count) {
          continue;
        }
        object_symbol = &input->symbols[relocation->symbol_index];
        if (!object_symbol->is_defined && object_symbol->name) {
          global = link_resolution_find_symbol(resolution, object_symbol->name);
        }
        patch_offset = input->section_merged_offsets[section_index] +
                       (size_t)relocation->offset;
        if (!visit(plan, &resolution->sections[merged_index], patch_offset,
                   global, user, error_message_out)) {
          return 0;
        }
      }
    }
  }
  return 1;
}

static int elf_dynamic_count_site(ElfDynamicPlan *plan, LinkedSection *merged,
                                  size_t patch_offset,
                                  const LinkedSymbol *global, void *user,
                                  char **error_message_out) {
  (void)patch_offset;
  (void)global;
  (void)user;
  (void)error_message_out;
  if (merged->kind != LINK_SECTION_KIND_DATA &&
      merged->kind != LINK_SECTION_KIND_BSS) {
    plan->needs_text_relocations = 1;
  }
  plan->rela_dyn_count++;
  return 1;
}

static int elf_dynamic_section_append(LinkedSection *section, uint64_t size,
                                      uint64_t alignment, uint64_t *offset_out,
                                      char **error_message_out) {
  size_t start = linker_align_up(section->virtual_size, (size_t)alignment);

  if (alignment > section->alignment) {
    section->alignment = (size_t)alignment;
  }
  if (section->kind != LINK_SECTION_KIND_BSS &&
      section->kind != LINK_SECTION_KIND_TLS) {
    size_t needed = start + (size_t)size;
    if (needed > section->data_capacity) {
      size_t capacity = section->data_capacity ? section->data_capacity : 64u;
      unsigned char *grown = NULL;
      while (capacity < needed) {
        capacity *= 2u;
      }
      grown = (unsigned char *)realloc(section->data, capacity);
      if (!grown) {
        mettle_set_error(error_message_out,
                         "Out of memory while reserving dynamic linking tables "
                         "in '%s'",
                         section->name ? section->name : "<unknown>");
        return 0;
      }
      section->data = grown;
      section->data_capacity = capacity;
    }
    if (needed > section->size) {
      memset(section->data + section->size, 0, needed - section->size);
      section->size = needed;
    }
  }
  if (section->virtual_size < start + (size_t)size) {
    section->virtual_size = start + (size_t)size;
  }
  *offset_out = (uint64_t)start;
  return 1;
}

static int elf_dynamic_reserve_blob(ElfDynamicPlan *plan,
                                    ElfDynamicBlobKind kind, const char *name,
                                    LinkSectionKind host, uint64_t size,
                                    uint64_t alignment, uint32_t section_type,
                                    uint64_t section_flags, uint64_t entry_size,
                                    int links_dynstr, int links_dynsym,
                                    char **error_message_out) {
  ElfDynamicBlob *blob = &plan->blobs[kind];
  size_t merged_index = 0u;
  LinkedSection *section = NULL;

  if (size == 0u) {
    blob->size = 0u;
    blob->merged_section_index = LINKED_SECTION_INDEX_NONE;
    return 1;
  }
  for (merged_index = 0u; merged_index < LINKED_SECTION_COUNT; merged_index++) {
    if (plan->resolution->sections[merged_index].kind == host) {
      break;
    }
  }
  if (merged_index == LINKED_SECTION_COUNT) {
    mettle_set_error(error_message_out,
                     "The link has no section to hold '%s'", name);
    return 0;
  }
  section = &plan->resolution->sections[merged_index];
  blob->name = name;
  blob->merged_section_index = merged_index;
  blob->size = size;
  blob->alignment = alignment;
  blob->section_type = section_type;
  blob->section_flags = section_flags;
  blob->entry_size = entry_size;
  blob->links_dynstr = links_dynstr;
  blob->links_dynsym = links_dynsym;
  return elf_dynamic_section_append(section, size, alignment, &blob->offset,
                                    error_message_out);
}

static uint64_t elf_dynamic_verneed_size(const ElfDynamicPlan *plan) {
  size_t i = 0u;
  uint64_t size = 0u;

  for (i = 0u; i < plan->needed_count; i++) {
    if (plan->needed[i].version_count == 0u) {
      continue;
    }
    size += ELF_DYN_VERNEED_SIZE +
            (uint64_t)plan->needed[i].version_count * ELF_DYN_VERNAUX_SIZE;
  }
  return size;
}

static size_t elf_dynamic_verneed_count(const ElfDynamicPlan *plan) {
  size_t i = 0u;
  size_t count = 0u;

  for (i = 0u; i < plan->needed_count; i++) {
    if (plan->needed[i].version_count != 0u) {
      count++;
    }
  }
  return count;
}

static size_t elf_dynamic_count_entries(const ElfDynamicPlan *plan) {
  size_t count = plan->needed_count;

  count += 5u;
  if (plan->rela_dyn_count != 0u) {
    count += 3u;
  }
  if (plan->rela_plt_count != 0u) {
    count += 4u;
  }
  if (plan->uses_versions) {
    count += 3u;
  }
  count += 3u;
  if (!plan->produce_shared_library) {
    count += 1u;
  } else {
    count += 1u;
    if (plan->soname) {
      count += 1u;
    }
  }
  if (plan->runpath_count != 0u) {
    count += 1u;
  }
  if (plan->needs_text_relocations) {
    count += 1u;
  }
  return count + 1u;
}

static int elf_dynamic_reserve_layout(ElfDynamicPlan *plan,
                                      char **error_message_out) {
  LinkResolution *resolution = plan->resolution;
  uint64_t hash_size = 0u;
  uint32_t bucket_count = plan->symbol_count > 1u
                              ? (uint32_t)(plan->symbol_count - 1u)
                              : 1u;
  size_t i = 0u;

  plan->got_count = ELF_DYN_GOT_RESERVED + plan->plt_count;
  plan->rela_plt_count = plan->plt_count;
  hash_size = (uint64_t)(2u + bucket_count + plan->symbol_count) * 4u;

  for (i = 0u; i < resolution->shared_import_count; i++) {
    if (resolution->shared_imports[i].needs_copy) {
      plan->rela_dyn_count++;
    }
  }
  if (plan->produce_shared_library &&
      !elf_dynamic_visit_absolute_sites(plan, elf_dynamic_count_site, NULL,
                                        error_message_out)) {
    return 0;
  }
  plan->dynamic_entry_count = elf_dynamic_count_entries(plan);

  if (plan->interpreter &&
      !elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_INTERP, ".interp",
                                LINK_SECTION_KIND_RDATA,
                                (uint64_t)strlen(plan->interpreter) + 1u, 1u,
                                SHT_PROGBITS, SHF_ALLOC, 0u, 0, 0,
                                error_message_out)) {
    return 0;
  }
  if (!elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_DYNSYM, ".dynsym",
                                LINK_SECTION_KIND_RDATA,
                                (uint64_t)plan->symbol_count * ELF_DYN_SYM_SIZE,
                                8u, SHT_DYNSYM, SHF_ALLOC, ELF_DYN_SYM_SIZE, 1,
                                0, error_message_out) ||
      !elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_DYNSTR, ".dynstr",
                                LINK_SECTION_KIND_RDATA,
                                (uint64_t)plan->strings.size, 1u, SHT_STRTAB,
                                SHF_ALLOC, 0u, 0, 0, error_message_out) ||
      !elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_HASH, ".hash",
                                LINK_SECTION_KIND_RDATA, hash_size, 8u,
                                SHT_HASH, SHF_ALLOC, 4u, 0, 1,
                                error_message_out)) {
    return 0;
  }
  if (plan->uses_versions &&
      (!elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_VERSYM, ".gnu.version",
                                 LINK_SECTION_KIND_RDATA,
                                 (uint64_t)plan->symbol_count * 2u, 2u,
                                 SHT_GNU_VERSYM, SHF_ALLOC, 2u, 0, 1,
                                 error_message_out) ||
       !elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_VERNEED,
                                 ".gnu.version_r", LINK_SECTION_KIND_RDATA,
                                 elf_dynamic_verneed_size(plan), 8u,
                                 SHT_GNU_VERNEED, SHF_ALLOC, 0u, 1, 0,
                                 error_message_out))) {
    return 0;
  }
  if (!elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_RELA_DYN, ".rela.dyn",
                                LINK_SECTION_KIND_RDATA,
                                (uint64_t)plan->rela_dyn_count *
                                    ELF_DYN_RELA_SIZE,
                                8u, SHT_RELA, SHF_ALLOC, ELF_DYN_RELA_SIZE, 0,
                                1, error_message_out) ||
      !elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_RELA_PLT, ".rela.plt",
                                LINK_SECTION_KIND_RDATA,
                                (uint64_t)plan->rela_plt_count *
                                    ELF_DYN_RELA_SIZE,
                                8u, SHT_RELA, SHF_ALLOC | SHF_INFO_LINK,
                                ELF_DYN_RELA_SIZE, 0, 1, error_message_out) ||
      !elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_PLT, ".plt",
                                LINK_SECTION_KIND_TEXT,
                                (uint64_t)plan->plt_count *
                                    ELF_DYN_PLT_ENTRY_SIZE,
                                16u, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
                                0u, 0, 0, error_message_out) ||
      !elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_GOT, ".got.plt",
                                LINK_SECTION_KIND_DATA,
                                (uint64_t)plan->got_count * 8u, 8u,
                                SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 8u, 0, 0,
                                error_message_out) ||
      !elf_dynamic_reserve_blob(plan, ELF_DYNAMIC_BLOB_DYNAMIC, ".dynamic",
                                LINK_SECTION_KIND_DATA,
                                (uint64_t)plan->dynamic_entry_count *
                                    ELF_DYN_ENTRY_SIZE,
                                8u, SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE,
                                ELF_DYN_ENTRY_SIZE, 1, 0, error_message_out)) {
    return 0;
  }
  return 1;
}

static int elf_dynamic_define_imports(ElfDynamicPlan *plan,
                                      char **error_message_out) {
  LinkResolution *resolution = plan->resolution;
  size_t bss_index = LINKED_SECTION_INDEX_NONE;
  size_t plt_slot = 0u;
  size_t i = 0u;

  for (i = 0u; i < LINKED_SECTION_COUNT; i++) {
    if (resolution->sections[i].kind == LINK_SECTION_KIND_BSS) {
      bss_index = i;
      break;
    }
  }

  for (i = 0u; i < resolution->shared_import_count; i++) {
    LinkedSharedImport *import = &resolution->shared_imports[i];
    LinkedSymbol *symbol = &resolution->symbols[import->symbol_index];

    if (import->needs_plt) {
      import->plt_offset = plan->blobs[ELF_DYNAMIC_BLOB_PLT].offset +
                           plt_slot * ELF_DYN_PLT_ENTRY_SIZE;
      import->got_offset = plan->blobs[ELF_DYNAMIC_BLOB_GOT].offset +
                           (ELF_DYN_GOT_RESERVED + plt_slot) * 8u;
      symbol->is_defined = 1;
      symbol->merged_section_index =
          plan->blobs[ELF_DYNAMIC_BLOB_PLT].merged_section_index;
      symbol->merged_offset = (size_t)import->plt_offset;
      plt_slot++;
      continue;
    }
    if (bss_index == LINKED_SECTION_INDEX_NONE) {
      mettle_set_error(error_message_out,
                       "The link has no .bss to hold a copy of '%s'",
                       symbol->name ? symbol->name : "<unnamed>");
      return 0;
    }
    if (!elf_dynamic_section_append(&resolution->sections[bss_index],
                                    import->size, 16u, &import->copy_offset,
                                    error_message_out)) {
      return 0;
    }
    symbol->is_defined = 1;
    symbol->merged_section_index = bss_index;
    symbol->merged_offset = (size_t)import->copy_offset;
  }
  return 1;
}

int elf_dynamic_plan_create(LinkResolution *resolution,
                            const ElfDynamicOptions *options,
                            ElfDynamicPlan **plan_out,
                            char **error_message_out) {
  ElfDynamicPlan *plan = NULL;
  size_t i = 0u;

  *plan_out = NULL;
  plan = (ElfDynamicPlan *)calloc(1u, sizeof(ElfDynamicPlan));
  if (!plan) {
    mettle_set_error(error_message_out,
                     "Out of memory while planning dynamic linking");
    return 0;
  }
  for (i = 0u; i < ELF_DYNAMIC_BLOB_COUNT; i++) {
    plan->blobs[i].merged_section_index = LINKED_SECTION_INDEX_NONE;
  }
  plan->resolution = resolution;
  plan->next_version_index = 2u;
  plan->produce_shared_library =
      options && options->produce_shared_library ? 1 : 0;
  plan->export_dynamic = options && options->export_dynamic ? 1 : 0;
  plan->image_base = options ? options->image_base : 0u;
  plan->runpaths = options ? options->runpaths : NULL;
  plan->runpath_count = options ? options->runpath_count : 0u;

  if (resolution->shared_import_count == 0u && !plan->produce_shared_library &&
      !plan->export_dynamic) {
    *plan_out = plan;
    return 1;
  }
  plan->active = 1;

  if (options && options->interpreter && !plan->produce_shared_library) {
    plan->interpreter = mettle_strdup(options->interpreter);
    if (!plan->interpreter) {
      mettle_set_error(error_message_out,
                       "Out of memory while planning dynamic linking");
      elf_dynamic_plan_destroy(plan);
      return 0;
    }
  }
  if (options && options->soname && plan->produce_shared_library) {
    plan->soname = mettle_strdup(options->soname);
    if (!plan->soname) {
      mettle_set_error(error_message_out,
                       "Out of memory while planning dynamic linking");
      elf_dynamic_plan_destroy(plan);
      return 0;
    }
  }

  if (!elf_dynamic_strings_init(&plan->strings)) {
    mettle_set_error(error_message_out,
                     "Out of memory while planning dynamic linking");
    elf_dynamic_plan_destroy(plan);
    return 0;
  }
  if (plan->soname &&
      !elf_dynamic_strings_add(&plan->strings, plan->soname,
                               &plan->soname_offset)) {
    mettle_set_error(error_message_out,
                     "Out of memory while planning dynamic linking");
    elf_dynamic_plan_destroy(plan);
    return 0;
  }
  if (plan->runpath_count != 0u) {
    size_t length = 0u;
    char *cursor = NULL;

    for (i = 0u; i < plan->runpath_count; i++) {
      length += strlen(plan->runpaths[i]) + 1u;
    }
    plan->runpath = (char *)malloc(length);
    if (!plan->runpath) {
      mettle_set_error(error_message_out,
                       "Out of memory while planning dynamic linking");
      elf_dynamic_plan_destroy(plan);
      return 0;
    }
    cursor = plan->runpath;
    for (i = 0u; i < plan->runpath_count; i++) {
      if (i != 0u) {
        *cursor++ = ':';
      }
      memcpy(cursor, plan->runpaths[i], strlen(plan->runpaths[i]));
      cursor += strlen(plan->runpaths[i]);
    }
    *cursor = 0;
    if (!elf_dynamic_strings_add(&plan->strings, plan->runpath,
                                 &plan->runpath_offset)) {
      mettle_set_error(error_message_out,
                       "Out of memory while planning dynamic linking");
      elf_dynamic_plan_destroy(plan);
      return 0;
    }
  }

  if (!elf_dynamic_classify_imports(plan, error_message_out) ||
      !elf_dynamic_build_needed(plan) ||
      !elf_dynamic_build_symbols(plan, error_message_out) ||
      !elf_dynamic_reserve_layout(plan, error_message_out) ||
      !elf_dynamic_define_imports(plan, error_message_out)) {
    if (error_message_out && !*error_message_out) {
      mettle_set_error(error_message_out,
                       "Out of memory while planning dynamic linking");
    }
    elf_dynamic_plan_destroy(plan);
    return 0;
  }

  *plan_out = plan;
  return 1;
}

typedef struct {
  unsigned char *cursor;
  size_t written;
  size_t capacity;
} ElfDynamicWriter;

static void elf_dynamic_put_u16(unsigned char *data, uint16_t value) {
  data[0] = (unsigned char)(value & 0xFFu);
  data[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static unsigned char *elf_dynamic_blob_data(ElfDynamicPlan *plan,
                                            ElfDynamicBlobKind kind) {
  const ElfDynamicBlob *blob = &plan->blobs[kind];

  if (blob->merged_section_index == LINKED_SECTION_INDEX_NONE) {
    return NULL;
  }
  return plan->resolution->sections[blob->merged_section_index].data +
         blob->offset;
}

static uint64_t elf_dynamic_blob_address(const ElfDynamicPlan *plan,
                                         ElfDynamicBlobKind kind) {
  const ElfDynamicBlob *blob = &plan->blobs[kind];

  if (blob->merged_section_index == LINKED_SECTION_INDEX_NONE) {
    return 0u;
  }
  return plan->resolution->sections[blob->merged_section_index].virtual_address +
         blob->offset;
}

static void elf_dynamic_write_hash(ElfDynamicPlan *plan) {
  unsigned char *table = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_HASH);
  uint32_t bucket_count = plan->symbol_count > 1u
                              ? (uint32_t)(plan->symbol_count - 1u)
                              : 1u;
  unsigned char *buckets = NULL;
  unsigned char *chains = NULL;
  const char *strings = plan->strings.data;
  size_t i = 0u;

  if (!table) {
    return;
  }
  linker_write_u32(table, bucket_count);
  linker_write_u32(table + 4, (uint32_t)plan->symbol_count);
  buckets = table + 8;
  chains = buckets + (size_t)bucket_count * 4u;
  for (i = 1u; i < plan->symbol_count; i++) {
    const char *name = strings + plan->symbols[i].name_offset;
    uint32_t bucket = elf_dynamic_hash(name) % bucket_count;
    linker_write_u32(chains + i * 4u, linker_read_u32(buckets + bucket * 4u));
    linker_write_u32(buckets + bucket * 4u, (uint32_t)i);
  }
}

static void elf_dynamic_write_symbols(ElfDynamicPlan *plan,
                                      const uint16_t *section_indices) {
  unsigned char *table = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_DYNSYM);
  unsigned char *versions = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_VERSYM);
  size_t i = 0u;

  if (!table) {
    return;
  }
  for (i = 0u; i < plan->symbol_count; i++) {
    const ElfDynamicSymbolRecord *record = &plan->symbols[i];
    unsigned char *entry = table + i * ELF_DYN_SYM_SIZE;
    uint16_t section_index = 0u;
    uint64_t value = 0u;
    uint64_t size = 0u;

    memset(entry, 0, ELF_DYN_SYM_SIZE);
    if (versions) {
      elf_dynamic_put_u16(versions + i * 2u,
                          i == 0u ? 0u
                                  : (record->version_index ? record->version_index
                                                           : 1u));
    }
    if (i == 0u) {
      continue;
    }
    linker_write_u32(entry, record->name_offset);
    entry[4] = record->info;
    if (record->is_definition &&
        record->resolution_symbol_index != LINKED_SECTION_INDEX_NONE) {
      const LinkedSymbol *symbol =
          &plan->resolution->symbols[record->resolution_symbol_index];
      if (symbol->merged_section_index != LINKED_SECTION_INDEX_NONE) {
        section_index = section_indices[symbol->merged_section_index];
      }
      value = symbol->virtual_address;
      size = record->import_index != LINKED_SECTION_INDEX_NONE
                 ? plan->resolution->shared_imports[record->import_index].size
                 : symbol->size;
    }
    elf_dynamic_put_u16(entry + 6, section_index);
    linker_write_u64(entry + 8, value);
    linker_write_u64(entry + 16, size);
  }
}

static void elf_dynamic_write_verneed(ElfDynamicPlan *plan) {
  unsigned char *table = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_VERNEED);
  size_t cursor = 0u;
  size_t remaining = elf_dynamic_verneed_count(plan);
  size_t i = 0u;

  if (!table) {
    return;
  }
  for (i = 0u; i < plan->needed_count; i++) {
    const ElfDynamicNeeded *needed = &plan->needed[i];
    unsigned char *entry = table + cursor;
    size_t j = 0u;

    if (needed->version_count == 0u) {
      continue;
    }
    remaining--;
    elf_dynamic_put_u16(entry, 1u);
    elf_dynamic_put_u16(entry + 2, (uint16_t)needed->version_count);
    linker_write_u32(entry + 4, needed->file_name_offset);
    linker_write_u32(entry + 8, ELF_DYN_VERNEED_SIZE);
    linker_write_u32(entry + 12,
                     remaining ? (uint32_t)(ELF_DYN_VERNEED_SIZE +
                                            needed->version_count *
                                                ELF_DYN_VERNAUX_SIZE)
                               : 0u);
    cursor += ELF_DYN_VERNEED_SIZE;
    for (j = 0u; j < needed->version_count; j++) {
      unsigned char *aux = table + cursor;

      linker_write_u32(aux, needed->version_hashes[j]);
      elf_dynamic_put_u16(aux + 4, 0u);
      elf_dynamic_put_u16(aux + 6, needed->version_indices[j]);
      linker_write_u32(aux + 8, needed->version_name_offsets[j]);
      linker_write_u32(aux + 12,
                       j + 1u < needed->version_count ? ELF_DYN_VERNAUX_SIZE
                                                      : 0u);
      cursor += ELF_DYN_VERNAUX_SIZE;
    }
  }
}

static void elf_dynamic_write_plt(ElfDynamicPlan *plan) {
  LinkResolution *resolution = plan->resolution;
  unsigned char *plt = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_PLT);
  unsigned char *got = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_GOT);
  size_t i = 0u;

  if (got) {
    linker_write_u64(got, elf_dynamic_blob_address(plan,
                                                   ELF_DYNAMIC_BLOB_DYNAMIC));
  }
  if (!plt) {
    return;
  }
  for (i = 0u; i < resolution->shared_import_count; i++) {
    const LinkedSharedImport *import = &resolution->shared_imports[i];
    uint64_t entry_address = 0u;
    uint64_t slot_address = 0u;
    unsigned char *code = NULL;
    int64_t displacement = 0;

    if (!import->needs_plt) {
      continue;
    }
    entry_address = resolution->sections[plan->blobs[ELF_DYNAMIC_BLOB_PLT]
                                             .merged_section_index]
                        .virtual_address +
                    import->plt_offset;
    slot_address = resolution->sections[plan->blobs[ELF_DYNAMIC_BLOB_GOT]
                                            .merged_section_index]
                       .virtual_address +
                   import->got_offset;
    code = plt + (import->plt_offset - plan->blobs[ELF_DYNAMIC_BLOB_PLT].offset);
    displacement = (int64_t)slot_address - (int64_t)(entry_address + 6);
    code[0] = 0xFFu;
    code[1] = 0x25u;
    linker_write_u32(code + 2, (uint32_t)(int32_t)displacement);
    memset(code + 6, 0xCC, ELF_DYN_PLT_ENTRY_SIZE - 6u);
    if (got) {
      linker_write_u64(got + (import->got_offset -
                              plan->blobs[ELF_DYNAMIC_BLOB_GOT].offset),
                       entry_address);
    }
  }
}

typedef struct {
  unsigned char *table;
  size_t count;
} ElfDynamicRelaCursor;

static void elf_dynamic_write_rela(unsigned char *entry, uint64_t offset,
                                   uint32_t type, uint32_t symbol_index,
                                   int64_t addend) {
  linker_write_u64(entry, offset);
  linker_write_u32(entry + 8, type);
  linker_write_u32(entry + 12, symbol_index);
  linker_write_u64(entry + 16, (uint64_t)addend);
}

static int elf_dynamic_emit_site(ElfDynamicPlan *plan, LinkedSection *merged,
                                 size_t patch_offset,
                                 const LinkedSymbol *global, void *user,
                                 char **error_message_out) {
  ElfDynamicRelaCursor *cursor = (ElfDynamicRelaCursor *)user;
  uint64_t address = merged->virtual_address + (uint64_t)patch_offset;
  unsigned char *entry = cursor->table + cursor->count * ELF_DYN_RELA_SIZE;
  int64_t stored = 0;

  (void)error_message_out;
  if (merged->data && patch_offset + 8u <= merged->size) {
    stored = (int64_t)linker_read_u64(merged->data + patch_offset);
  }
  if (global && global->is_shared_import) {
    const LinkedSharedImport *import =
        &plan->resolution->shared_imports[global->shared_import_index];
    elf_dynamic_write_rela(entry, address, R_X86_64_64,
                           import->dynamic_symbol_index, 0);
  } else {
    elf_dynamic_write_rela(entry, address, R_X86_64_RELATIVE, 0u, stored);
  }
  cursor->count++;
  return 1;
}

static int elf_dynamic_write_relocations(ElfDynamicPlan *plan,
                                         char **error_message_out) {
  LinkResolution *resolution = plan->resolution;
  unsigned char *rela_dyn = elf_dynamic_blob_data(plan,
                                                  ELF_DYNAMIC_BLOB_RELA_DYN);
  unsigned char *rela_plt = elf_dynamic_blob_data(plan,
                                                  ELF_DYNAMIC_BLOB_RELA_PLT);
  ElfDynamicRelaCursor cursor;
  size_t plt_written = 0u;
  size_t i = 0u;

  cursor.table = rela_dyn;
  cursor.count = 0u;

  for (i = 0u; i < resolution->shared_import_count; i++) {
    const LinkedSharedImport *import = &resolution->shared_imports[i];
    const LinkedSymbol *symbol = &resolution->symbols[import->symbol_index];

    if (import->needs_copy && rela_dyn) {
      elf_dynamic_write_rela(rela_dyn + cursor.count * ELF_DYN_RELA_SIZE,
                             symbol->virtual_address, R_X86_64_COPY,
                             import->dynamic_symbol_index, 0);
      cursor.count++;
    }
    if (import->needs_plt && rela_plt) {
      uint64_t slot =
          resolution->sections[plan->blobs[ELF_DYNAMIC_BLOB_GOT]
                                   .merged_section_index]
              .virtual_address +
          import->got_offset;
      elf_dynamic_write_rela(rela_plt + plt_written * ELF_DYN_RELA_SIZE, slot,
                             R_X86_64_JUMP_SLOT, import->dynamic_symbol_index,
                             0);
      plt_written++;
    }
  }

  if (plan->produce_shared_library && rela_dyn &&
      !elf_dynamic_visit_absolute_sites(plan, elf_dynamic_emit_site, &cursor,
                                        error_message_out)) {
    return 0;
  }
  return 1;
}

static void elf_dynamic_write_table(ElfDynamicPlan *plan) {
  unsigned char *table = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_DYNAMIC);
  size_t written = 0u;
  size_t i = 0u;
  uint64_t flags = DF_BIND_NOW;

  if (!table) {
    return;
  }

#define ELF_DYNAMIC_PUT(tag, value)                                            \
  do {                                                                         \
    if (written < plan->dynamic_entry_count) {                                 \
      linker_write_u64(table + written * ELF_DYN_ENTRY_SIZE,                   \
                       (uint64_t)(int64_t)(tag));                              \
      linker_write_u64(table + written * ELF_DYN_ENTRY_SIZE + 8,               \
                       (uint64_t)(value));                                     \
      written++;                                                               \
    }                                                                          \
  } while (0)

  for (i = 0u; i < plan->needed_count; i++) {
    ELF_DYNAMIC_PUT(DT_NEEDED, plan->needed[i].file_name_offset);
  }
  if (plan->soname) {
    ELF_DYNAMIC_PUT(DT_SONAME, plan->soname_offset);
  }
  if (plan->runpath) {
    ELF_DYNAMIC_PUT(DT_RUNPATH, plan->runpath_offset);
  }
  ELF_DYNAMIC_PUT(DT_HASH, elf_dynamic_blob_address(plan, ELF_DYNAMIC_BLOB_HASH));
  ELF_DYNAMIC_PUT(DT_STRTAB,
                  elf_dynamic_blob_address(plan, ELF_DYNAMIC_BLOB_DYNSTR));
  ELF_DYNAMIC_PUT(DT_SYMTAB,
                  elf_dynamic_blob_address(plan, ELF_DYNAMIC_BLOB_DYNSYM));
  ELF_DYNAMIC_PUT(DT_STRSZ, plan->strings.size);
  ELF_DYNAMIC_PUT(DT_SYMENT, ELF_DYN_SYM_SIZE);
  if (plan->rela_dyn_count != 0u) {
    ELF_DYNAMIC_PUT(DT_RELA,
                    elf_dynamic_blob_address(plan, ELF_DYNAMIC_BLOB_RELA_DYN));
    ELF_DYNAMIC_PUT(DT_RELASZ,
                    (uint64_t)plan->rela_dyn_count * ELF_DYN_RELA_SIZE);
    ELF_DYNAMIC_PUT(DT_RELAENT, ELF_DYN_RELA_SIZE);
  }
  if (plan->rela_plt_count != 0u) {
    ELF_DYNAMIC_PUT(DT_PLTGOT,
                    elf_dynamic_blob_address(plan, ELF_DYNAMIC_BLOB_GOT));
    ELF_DYNAMIC_PUT(DT_PLTRELSZ,
                    (uint64_t)plan->rela_plt_count * ELF_DYN_RELA_SIZE);
    ELF_DYNAMIC_PUT(DT_PLTREL, DT_RELA);
    ELF_DYNAMIC_PUT(DT_JMPREL,
                    elf_dynamic_blob_address(plan, ELF_DYNAMIC_BLOB_RELA_PLT));
  }
  if (plan->uses_versions) {
    ELF_DYNAMIC_PUT(DT_VERSYM,
                    elf_dynamic_blob_address(plan, ELF_DYNAMIC_BLOB_VERSYM));
    ELF_DYNAMIC_PUT(DT_VERNEED,
                    elf_dynamic_blob_address(plan, ELF_DYNAMIC_BLOB_VERNEED));
    ELF_DYNAMIC_PUT(DT_VERNEEDNUM, elf_dynamic_verneed_count(plan));
  }
  if (plan->produce_shared_library) {
    flags |= DF_SYMBOLIC;
    ELF_DYNAMIC_PUT(DT_SYMBOLIC, 0u);
  }
  if (plan->needs_text_relocations) {
    flags |= DF_TEXTREL;
    ELF_DYNAMIC_PUT(DT_TEXTREL, 0u);
  }
  ELF_DYNAMIC_PUT(DT_BIND_NOW, 0u);
  ELF_DYNAMIC_PUT(DT_FLAGS, flags);
  ELF_DYNAMIC_PUT(DT_FLAGS_1, DF_1_NOW);
  if (!plan->produce_shared_library) {
    ELF_DYNAMIC_PUT(DT_DEBUG, 0u);
  }
  while (written < plan->dynamic_entry_count) {
    ELF_DYNAMIC_PUT(DT_NULL, 0u);
  }
#undef ELF_DYNAMIC_PUT
}

int elf_dynamic_plan_write(ElfDynamicPlan *plan,
                           const uint16_t *merged_section_header_indices,
                           char **error_message_out) {
  unsigned char *interp = NULL;
  unsigned char *strings = NULL;

  if (!plan || !plan->active) {
    return 1;
  }
  interp = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_INTERP);
  if (interp && plan->interpreter) {
    memcpy(interp, plan->interpreter, strlen(plan->interpreter) + 1u);
  }
  strings = elf_dynamic_blob_data(plan, ELF_DYNAMIC_BLOB_DYNSTR);
  if (strings) {
    memcpy(strings, plan->strings.data, plan->strings.size);
  }
  elf_dynamic_write_hash(plan);
  elf_dynamic_write_symbols(plan, merged_section_header_indices);
  elf_dynamic_write_verneed(plan);
  elf_dynamic_write_plt(plan);
  if (!elf_dynamic_write_relocations(plan, error_message_out)) {
    return 0;
  }
  elf_dynamic_write_table(plan);
  return 1;
}

void elf_dynamic_plan_destroy(ElfDynamicPlan *plan) {
  size_t i = 0u;
  size_t j = 0u;

  if (!plan) {
    return;
  }
  for (i = 0u; i < plan->needed_count; i++) {
    for (j = 0u; j < plan->needed[i].version_count; j++) {
      free(plan->needed[i].version_names[j]);
    }
    free(plan->needed[i].version_names);
    free(plan->needed[i].version_hashes);
    free(plan->needed[i].version_name_offsets);
    free(plan->needed[i].version_indices);
  }
  free(plan->needed);
  free(plan->symbols);
  free(plan->strings.data);
  free(plan->runpath);
  free(plan->interpreter);
  free(plan->soname);
  free(plan);
}

const ElfDynamicBlob *elf_dynamic_plan_blob(const ElfDynamicPlan *plan,
                                            ElfDynamicBlobKind kind) {
  if (!plan || kind >= ELF_DYNAMIC_BLOB_COUNT) {
    return NULL;
  }
  if (plan->blobs[kind].merged_section_index == LINKED_SECTION_INDEX_NONE) {
    return NULL;
  }
  return &plan->blobs[kind];
}

void elf_dynamic_plan_set_section_index(ElfDynamicPlan *plan,
                                        ElfDynamicBlobKind kind,
                                        uint16_t section_index) {
  if (!plan || kind >= ELF_DYNAMIC_BLOB_COUNT) {
    return;
  }
  plan->blobs[kind].section_header_index = section_index;
}

int elf_dynamic_plan_needs_text_relocations(const ElfDynamicPlan *plan) {
  return plan && plan->needs_text_relocations;
}

int elf_dynamic_plan_is_active(const ElfDynamicPlan *plan) {
  return plan && plan->active;
}

size_t elf_dynamic_plan_verneed_count(const ElfDynamicPlan *plan) {
  return plan ? elf_dynamic_verneed_count(plan) : 0u;
}
