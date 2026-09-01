#ifndef METTLE_ELF_DYNAMIC_H
#define METTLE_ELF_DYNAMIC_H

#include "linker/symbol_resolve.h"

#include <stdint.h>

typedef struct {
  const char *interpreter;
  const char *soname;
  const char *const *runpaths;
  size_t runpath_count;
  int produce_shared_library;
  int export_dynamic;
  uint64_t image_base;
} ElfDynamicOptions;

typedef struct {
  const char *name;
  size_t merged_section_index;
  uint64_t offset;
  uint64_t size;
  uint32_t section_type;
  uint64_t section_flags;
  uint64_t alignment;
  uint64_t entry_size;
  int links_dynstr;
  int links_dynsym;
  uint16_t section_header_index;
} ElfDynamicBlob;

typedef enum {
  ELF_DYNAMIC_BLOB_INTERP = 0,
  ELF_DYNAMIC_BLOB_DYNSYM,
  ELF_DYNAMIC_BLOB_DYNSTR,
  ELF_DYNAMIC_BLOB_HASH,
  ELF_DYNAMIC_BLOB_VERSYM,
  ELF_DYNAMIC_BLOB_VERNEED,
  ELF_DYNAMIC_BLOB_RELA_DYN,
  ELF_DYNAMIC_BLOB_RELA_PLT,
  ELF_DYNAMIC_BLOB_PLT,
  ELF_DYNAMIC_BLOB_GOT,
  ELF_DYNAMIC_BLOB_DYNAMIC,
  ELF_DYNAMIC_BLOB_COUNT
} ElfDynamicBlobKind;

typedef struct ElfDynamicPlan ElfDynamicPlan;

int elf_dynamic_plan_create(LinkResolution *resolution,
                            const ElfDynamicOptions *options,
                            ElfDynamicPlan **plan_out,
                            char **error_message_out);
int elf_dynamic_plan_write(ElfDynamicPlan *plan,
                           const uint16_t *merged_section_header_indices,
                           char **error_message_out);
int elf_dynamic_plan_is_active(const ElfDynamicPlan *plan);
void elf_dynamic_plan_destroy(ElfDynamicPlan *plan);

const ElfDynamicBlob *elf_dynamic_plan_blob(const ElfDynamicPlan *plan,
                                            ElfDynamicBlobKind kind);
void elf_dynamic_plan_set_section_index(ElfDynamicPlan *plan,
                                        ElfDynamicBlobKind kind,
                                        uint16_t section_index);
int elf_dynamic_plan_needs_text_relocations(const ElfDynamicPlan *plan);
size_t elf_dynamic_plan_verneed_count(const ElfDynamicPlan *plan);

#endif
