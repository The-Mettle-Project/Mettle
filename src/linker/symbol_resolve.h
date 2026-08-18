#ifndef SYMBOL_RESOLVE_H
#define SYMBOL_RESOLVE_H

#include "linker/coff_reader.h"

#include <stddef.h>
#include <stdint.h>

#define LINKED_SECTION_INDEX_NONE ((size_t)-1)
#define LINKED_SECTION_COUNT 6u

typedef struct {
  size_t object_index;
  size_t section_index;
  size_t merged_offset;
  size_t size;
  size_t alignment;
} LinkedSectionContribution;

typedef struct {
  CoffSectionKind kind;
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
  int16_t section_number;
  size_t merged_section_index;
  size_t merged_offset;
  uint64_t virtual_address;
} LinkedObjectSymbol;

typedef struct {
  char *path;
  CoffObject *object;
  size_t *section_merged_indices;
  size_t *section_merged_offsets;
  size_t *section_merged_sizes;
  size_t *section_alignments;
  LinkedObjectSymbol *symbols;
  size_t symbol_count;
  /* A bundled runtime object: its definitions are defaults a program object may
   * replace. See object_is_runtime_default in LinkResolutionOptions. */
  int is_runtime_default;
  /* Per-section: 1 marks a granular (-ffunction-sections/-fdata-sections)
   * section proven unreachable from any retained section; it is left out of
   * the merged image entirely. */
  unsigned char *section_gc_dead;
  /* Per-symbol: 1 marks a symbol-table entry some retained section relocates
   * against. Undefined externals nothing retained references are not recorded
   * globally, so they neither fail resolution nor become DLL imports. */
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
} LinkedSymbol;

typedef struct {
  const char *entry_symbol_name;
  size_t section_alignment;
  int allow_unresolved_externals;
  /* Optional, parallel to object_paths: non-zero marks a bundled runtime
   * object. The runtime and the standard library share one flat symbol
   * namespace, so freestanding.o alone puts ~330 C names in front of every
   * program. Marking those definitions as defaults lets a program (or a
   * stdlib module, e.g. std/conv's strlen) define the same name and win,
   * instead of failing the link on a duplicate symbol. Two program-object
   * definitions of one name are still an error. NULL means "no defaults". */
  const unsigned char *object_is_runtime_default;
} LinkResolutionOptions;

typedef struct {
  LinkedInputObject *objects;
  size_t object_count;
  LinkedSection sections[LINKED_SECTION_COUNT];
  LinkedSymbol *symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  /* Open-addressing name index over symbols (slot+1; 0 = empty). Lookups run
   * per input symbol and per relocation; linear scans here were quadratic on
   * programs with hundreds of thousands of globals. */
  size_t *symbol_buckets;
  size_t symbol_bucket_count;
  const LinkedSymbol *entry_symbol;
} LinkResolution;

int link_resolution_build(const char **object_paths, size_t object_count,
                          const LinkResolutionOptions *options,
                          LinkResolution **resolution_out,
                          char **error_message_out);
void link_resolution_destroy(LinkResolution *resolution);

const LinkedSection *link_resolution_find_section(const LinkResolution *resolution,
                                                  CoffSectionKind kind);
const LinkedSymbol *link_resolution_find_symbol(const LinkResolution *resolution,
                                                const char *name);

#endif // SYMBOL_RESOLVE_H
