#ifndef SYMBOL_RESOLVE_H
#define SYMBOL_RESOLVE_H

#include "linker/elf_shared.h"
#include "linker/link_object.h"

#include <stddef.h>
#include <stdint.h>

#define LINKED_SECTION_INDEX_NONE ((size_t)-1)
#define LINKED_SECTION_COUNT 7u
#define LINKED_LIBRARY_INDEX_NONE ((size_t)-1)

typedef struct {
  size_t object_index;
  size_t section_index;
  size_t merged_offset;
  size_t size;
  size_t alignment;
} LinkedSectionContribution;

typedef struct {
  LinkSectionKind kind;
  const char *name;
  unsigned char *data;
  size_t data_capacity;
  size_t size;
  size_t virtual_size;
  size_t alignment;
  uint64_t virtual_address;
  LinkedSectionContribution *contributions;
  size_t contribution_count;
  size_t contribution_capacity;
} LinkedSection;

typedef struct {
  char *name;
  size_t object_index;
  uint32_t symbol_index;
  int is_defined;
  int is_external;
  int is_local;
  int is_auxiliary;
  int64_t section_index;
  size_t merged_section_index;
  size_t merged_offset;
  uint64_t virtual_address;
  uint64_t size;
  uint8_t elf_type;
  int is_weak;
} LinkedObjectSymbol;

typedef struct {
  char *path;
  LinkObject *object;
  size_t *section_merged_indices;
  size_t *section_merged_offsets;
  size_t *section_merged_sizes;
  size_t *section_alignments;
  LinkedObjectSymbol *symbols;
  size_t symbol_count;
  int is_runtime_default;
  unsigned char *section_gc_dead;
  unsigned char *symbol_gc_referenced;
} LinkedInputObject;

typedef struct {
  char *name;
  int is_defined;
  int is_external;
  size_t defining_object_index;
  uint32_t defining_symbol_index;
  size_t merged_section_index;
  size_t merged_offset;
  uint64_t virtual_address;
  uint64_t size;
  uint8_t elf_type;
  int is_weak;
  int is_shared_import;
  size_t shared_import_index;
} LinkedSymbol;

typedef struct {
  size_t library_index;
  size_t symbol_index;
  char *version;
  uint64_t size;
  uint64_t alignment;
  uint8_t type;
  int is_weak;
  int needs_plt;
  int needs_copy;
  uint64_t got_offset;
  uint64_t plt_offset;
  uint64_t copy_offset;
  uint32_t dynamic_symbol_index;
} LinkedSharedImport;

typedef struct {
  const char *entry_symbol_name;
  size_t section_alignment;
  int allow_unresolved_externals;
  const unsigned char *object_is_runtime_default;
  const char *const *shared_library_paths;
  size_t shared_library_path_count;
  int produce_shared_library;
  int keep_all_sections;
} LinkResolutionOptions;

typedef struct {
  LinkedInputObject *objects;
  size_t object_count;
  LinkedSection sections[LINKED_SECTION_COUNT];
  LinkedSymbol *symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  size_t *symbol_buckets;
  size_t symbol_bucket_count;
  const LinkedSymbol *entry_symbol;
  ElfSharedLibrary **shared_libraries;
  size_t shared_library_count;
  unsigned char *shared_library_used;
  LinkedSharedImport *shared_imports;
  size_t shared_import_count;
  size_t shared_import_capacity;
} LinkResolution;

int link_resolution_build(const char **object_paths, size_t object_count,
                          const LinkResolutionOptions *options,
                          LinkResolution **resolution_out,
                          char **error_message_out);
void link_resolution_destroy(LinkResolution *resolution);

const LinkedSection *link_resolution_find_section(const LinkResolution *resolution,
                                                  LinkSectionKind kind);
const LinkedSymbol *link_resolution_find_symbol(const LinkResolution *resolution,
                                                const char *name);
LinkedSymbol *link_resolution_find_symbol_mutable(LinkResolution *resolution,
                                                  const char *name);

#endif
