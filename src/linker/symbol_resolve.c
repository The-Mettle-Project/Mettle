#include "linker/symbol_resolve.h"
#include "linker/linker_common.h"
#include "../common.h"

/* Section merge and symbol resolution for COFF produced by the object backend;
 * pair with relocation.c and see docs/linker-build-pipelines.md for pipeline
 * triage (internal link vs external gcc). */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COFF_STORAGE_CLASS_EXTERNAL 2u
#define COFF_STORAGE_CLASS_STATIC 3u
#define IMAGE_SCN_ALIGN_MASK 0x00F00000u
#define IMAGE_SCN_ALIGN_1BYTES 0x00100000u
#define IMAGE_SCN_ALIGN_2BYTES 0x00200000u
#define IMAGE_SCN_ALIGN_4BYTES 0x00300000u
#define IMAGE_SCN_ALIGN_8BYTES 0x00400000u
#define IMAGE_SCN_ALIGN_16BYTES 0x00500000u
#define IMAGE_SCN_ALIGN_32BYTES 0x00600000u
#define IMAGE_SCN_ALIGN_64BYTES 0x00700000u
#define IMAGE_SCN_ALIGN_128BYTES 0x00800000u
#define IMAGE_SCN_ALIGN_256BYTES 0x00900000u
#define IMAGE_SCN_ALIGN_512BYTES 0x00A00000u
#define IMAGE_SCN_ALIGN_1024BYTES 0x00B00000u
#define IMAGE_SCN_ALIGN_2048BYTES 0x00C00000u
#define IMAGE_SCN_ALIGN_4096BYTES 0x00D00000u
#define IMAGE_SCN_ALIGN_8192BYTES 0x00E00000u


static size_t link_section_index_from_kind(CoffSectionKind kind) {
  switch (kind) {
  case COFF_SECTION_KIND_TEXT:
    return 0u;
  case COFF_SECTION_KIND_RDATA:
    return 1u;
  case COFF_SECTION_KIND_DATA:
    return 2u;
  case COFF_SECTION_KIND_BSS:
    return 3u;
  case COFF_SECTION_KIND_PDATA:
    return 4u;
  case COFF_SECTION_KIND_XDATA:
    return 5u;
  case COFF_SECTION_KIND_UNKNOWN:
  default:
    return LINKED_SECTION_INDEX_NONE;
  }
}

static const char *link_section_name_from_kind(CoffSectionKind kind) {
  switch (kind) {
  case COFF_SECTION_KIND_TEXT:
    return ".text";
  case COFF_SECTION_KIND_RDATA:
    return ".rdata";
  case COFF_SECTION_KIND_DATA:
    return ".data";
  case COFF_SECTION_KIND_BSS:
    return ".bss";
  case COFF_SECTION_KIND_PDATA:
    return ".pdata";
  case COFF_SECTION_KIND_XDATA:
    return ".xdata";
  case COFF_SECTION_KIND_UNKNOWN:
  default:
    return "<unknown>";
  }
}

static size_t link_alignment_from_characteristics(uint32_t characteristics) {
  switch (characteristics & IMAGE_SCN_ALIGN_MASK) {
  case IMAGE_SCN_ALIGN_1BYTES:
    return 1u;
  case IMAGE_SCN_ALIGN_2BYTES:
    return 2u;
  case IMAGE_SCN_ALIGN_4BYTES:
    return 4u;
  case IMAGE_SCN_ALIGN_8BYTES:
    return 8u;
  case IMAGE_SCN_ALIGN_16BYTES:
    return 16u;
  case IMAGE_SCN_ALIGN_32BYTES:
    return 32u;
  case IMAGE_SCN_ALIGN_64BYTES:
    return 64u;
  case IMAGE_SCN_ALIGN_128BYTES:
    return 128u;
  case IMAGE_SCN_ALIGN_256BYTES:
    return 256u;
  case IMAGE_SCN_ALIGN_512BYTES:
    return 512u;
  case IMAGE_SCN_ALIGN_1024BYTES:
    return 1024u;
  case IMAGE_SCN_ALIGN_2048BYTES:
    return 2048u;
  case IMAGE_SCN_ALIGN_4096BYTES:
    return 4096u;
  case IMAGE_SCN_ALIGN_8192BYTES:
    return 8192u;
  default:
    return 0u;
  }
}

static size_t link_default_section_alignment(CoffSectionKind kind,
                                             size_t fallback_alignment) {
  if (fallback_alignment > 1u) {
    return fallback_alignment;
  }

  switch (kind) {
  case COFF_SECTION_KIND_TEXT:
  case COFF_SECTION_KIND_RDATA:
  case COFF_SECTION_KIND_DATA:
  case COFF_SECTION_KIND_BSS:
  case COFF_SECTION_KIND_PDATA:
  case COFF_SECTION_KIND_XDATA:
    return 16u;
  case COFF_SECTION_KIND_UNKNOWN:
  default:
    return 1u;
  }
}

static size_t link_section_alignment(const CoffSection *section,
                                     size_t fallback_alignment) {
  size_t alignment = 0;

  if (!section) {
    return 1u;
  }

  alignment = link_alignment_from_characteristics(section->characteristics);
  if (alignment != 0u) {
    return alignment;
  }

  return link_default_section_alignment(section->kind, fallback_alignment);
}

static int link_section_reserve_data(LinkedSection *section, size_t minimum_size,
                                     char **error_message_out) {
  unsigned char *grown = NULL;
  size_t new_capacity = 0;

  if (!section) {
    return 0;
  }
  if (minimum_size <= section->data_capacity) {
    return 1;
  }

  new_capacity = section->data_capacity ? section->data_capacity : 64u;
  while (new_capacity < minimum_size) {
    new_capacity *= 2u;
  }

  grown = realloc(section->data, new_capacity);
  if (!grown) {
    mettle_set_error(error_message_out,
                              "Out of memory while growing merged section '%s'",
                              section->name);
    return 0;
  }

  section->data = grown;
  section->data_capacity = new_capacity;
  return 1;
}

static int link_section_reserve_contributions(LinkedSection *section,
                                              size_t minimum_count,
                                              char **error_message_out) {
  LinkedSectionContribution *grown = NULL;
  size_t new_capacity = 0;

  if (!section) {
    return 0;
  }
  if (section->contribution_capacity >= minimum_count) {
    return 1;
  }

  new_capacity = section->contribution_capacity ? section->contribution_capacity
                                                : 4u;
  while (new_capacity < minimum_count) {
    new_capacity *= 2u;
  }

  grown = realloc(section->contributions,
                  new_capacity * sizeof(LinkedSectionContribution));
  if (!grown) {
    mettle_set_error(
        error_message_out,
        "Out of memory while recording contributions for merged section '%s'",
        section->name);
    return 0;
  }

  section->contributions = grown;
  section->contribution_capacity = new_capacity;
  return 1;
}

static size_t link_estimate_section_size(const CoffObject *object,
                                         size_t section_index) {
  const CoffSection *section = NULL;
  size_t size = 0;
  size_t i = 0;
  int saw_symbol = 0;

  if (!object || section_index >= object->section_count) {
    return 0u;
  }

  section = &object->sections[section_index];
  for (i = 0; i < object->symbol_count; i++) {
    const CoffSymbol *symbol = &object->symbols[i];

    if (symbol->is_auxiliary || !symbol->has_auxiliary_record || !symbol->name) {
      continue;
    }
    if (symbol->storage_class != COFF_STORAGE_CLASS_STATIC ||
        symbol->section_number != (int16_t)(section_index + 1u)) {
      continue;
    }
    if (strcmp(symbol->name, section->name) != 0) {
      continue;
    }
    if (symbol->aux_section_length != 0u) {
      return (size_t)symbol->aux_section_length;
    }
  }

  size = section->size_of_raw_data;
  if (section->virtual_size > size) {
    size = section->virtual_size;
  }
  if (section->kind != COFF_SECTION_KIND_BSS) {
    return size;
  }

  for (i = 0; i < object->symbol_count; i++) {
    const CoffSymbol *symbol = &object->symbols[i];
    size_t end = 0;

    if (symbol->is_auxiliary || symbol->section_number != (int16_t)(section_index + 1u)) {
      continue;
    }
    saw_symbol = 1;
    end = (size_t)symbol->value + 1u;
    if (end > size) {
      size = end;
    }
  }

  if (size == 0u && saw_symbol) {
    size = 1u;
  }

  return size;
}

static int link_resolution_init_sections(LinkResolution *resolution) {
  static const CoffSectionKind kinds[LINKED_SECTION_COUNT] = {
      COFF_SECTION_KIND_TEXT, COFF_SECTION_KIND_RDATA, COFF_SECTION_KIND_DATA,
      COFF_SECTION_KIND_BSS, COFF_SECTION_KIND_PDATA, COFF_SECTION_KIND_XDATA};
  size_t i = 0;

  if (!resolution) {
    return 0;
  }

  for (i = 0; i < LINKED_SECTION_COUNT; i++) {
    resolution->sections[i].kind = kinds[i];
    resolution->sections[i].name = link_section_name_from_kind(kinds[i]);
    resolution->sections[i].alignment = 1u;
  }

  return 1;
}

static int link_resolution_load_objects(LinkResolution *resolution,
                                        const char **object_paths,
                                        size_t object_count,
                                        char **error_message_out) {
  size_t i = 0;

  if (!resolution || (!object_paths && object_count != 0u)) {
    return 0;
  }

  resolution->objects = calloc(object_count, sizeof(LinkedInputObject));
  if (!resolution->objects && object_count != 0u) {
    mettle_set_error(error_message_out,
                              "Out of memory while allocating input objects");
    return 0;
  }

  resolution->object_count = object_count;
  for (i = 0; i < object_count; i++) {
    LinkedInputObject *input = &resolution->objects[i];

    input->path = mettle_strdup(object_paths[i]);
    if (!input->path) {
      mettle_set_error(error_message_out,
                                "Out of memory while storing object path");
      return 0;
    }

    if (!coff_object_read(object_paths[i], &input->object, error_message_out)) {
      return 0;
    }
  }

  return 1;
}

static int link_resolution_merge_sections(LinkResolution *resolution,
                                          size_t fallback_alignment,
                                          char **error_message_out) {
  size_t object_index = 0;

  if (!resolution) {
    return 0;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];
    size_t section_count = input->object ? input->object->section_count : 0u;
    size_t section_index = 0;

    input->section_merged_indices = malloc(section_count * sizeof(size_t));
    input->section_merged_offsets = calloc(section_count, sizeof(size_t));
    input->section_merged_sizes = calloc(section_count, sizeof(size_t));
    input->section_alignments = calloc(section_count, sizeof(size_t));
    if ((!input->section_merged_indices && section_count != 0u) ||
        (!input->section_merged_offsets && section_count != 0u) ||
        (!input->section_merged_sizes && section_count != 0u) ||
        (!input->section_alignments && section_count != 0u)) {
      mettle_set_error(error_message_out,
                                "Out of memory while mapping object sections");
      return 0;
    }

    for (section_index = 0; section_index < section_count; section_index++) {
      const CoffSection *section = &input->object->sections[section_index];
      size_t merged_index = link_section_index_from_kind(section->kind);
      size_t alignment = link_section_alignment(section, fallback_alignment);
      size_t contribution_size = 0;
      LinkedSection *merged = NULL;
      size_t start = 0;

      input->section_merged_indices[section_index] = LINKED_SECTION_INDEX_NONE;
      if (merged_index == LINKED_SECTION_INDEX_NONE) {
        continue;
      }

      merged = &resolution->sections[merged_index];
      if (alignment > merged->alignment) {
        merged->alignment = alignment;
      }

      contribution_size = link_estimate_section_size(input->object, section_index);
      start = linker_align_up(merged->virtual_size, alignment);

      if (!link_section_reserve_contributions(merged, merged->contribution_count + 1u,
                                              error_message_out)) {
        return 0;
      }

      if (section->kind != COFF_SECTION_KIND_BSS) {
        if (!link_section_reserve_data(merged, start + section->size_of_raw_data,
                                       error_message_out)) {
          return 0;
        }
        if (start > merged->size) {
          memset(merged->data + merged->size, 0, start - merged->size);
        }
        if (section->size_of_raw_data > 0u) {
          memcpy(merged->data + start, section->raw_data, section->size_of_raw_data);
        }
        merged->size = start + section->size_of_raw_data;
        if (merged->virtual_size < merged->size) {
          merged->virtual_size = merged->size;
        }
      } else {
        if (merged->virtual_size < start + contribution_size) {
          merged->virtual_size = start + contribution_size;
        }
      }

      merged->contributions[merged->contribution_count].object_index = object_index;
      merged->contributions[merged->contribution_count].section_index = section_index;
      merged->contributions[merged->contribution_count].merged_offset = start;
      merged->contributions[merged->contribution_count].size = contribution_size;
      merged->contributions[merged->contribution_count].alignment = alignment;
      merged->contribution_count++;

      input->section_merged_indices[section_index] = merged_index;
      input->section_merged_offsets[section_index] = start;
      input->section_merged_sizes[section_index] = contribution_size;
      input->section_alignments[section_index] = alignment;
    }
  }

  return 1;
}

static int link_resolution_reindex_symbols(LinkResolution *resolution,
                                           size_t min_buckets) {
  size_t nb = 64;
  size_t *fresh = NULL;
  size_t i = 0;

  while (nb < min_buckets) {
    nb *= 2u;
  }
  fresh = calloc(nb, sizeof(size_t));
  if (!fresh) {
    return 0;
  }
  for (i = 0; i < resolution->symbol_count; i++) {
    if (resolution->symbols[i].name) {
      size_t b = mettle_fnv1a_hash(resolution->symbols[i].name) & (nb - 1);
      while (fresh[b]) {
        b = (b + 1) & (nb - 1);
      }
      fresh[b] = i + 1;
    }
  }
  free(resolution->symbol_buckets);
  resolution->symbol_buckets = fresh;
  resolution->symbol_bucket_count = nb;
  return 1;
}

static int link_resolution_index_insert(LinkResolution *resolution,
                                        size_t symbol_index) {
  size_t b = 0;

  if ((resolution->symbol_count + 1u) * 4u >=
      resolution->symbol_bucket_count * 3u) {
    if (!link_resolution_reindex_symbols(resolution,
                                         (resolution->symbol_count + 1u) * 2u)) {
      return 0;
    }
    return 1; /* reindex covered the new entry */
  }
  b = mettle_fnv1a_hash(resolution->symbols[symbol_index].name) &
      (resolution->symbol_bucket_count - 1);
  while (resolution->symbol_buckets[b]) {
    b = (b + 1) & (resolution->symbol_bucket_count - 1);
  }
  resolution->symbol_buckets[b] = symbol_index + 1u;
  return 1;
}

static LinkedSymbol *link_resolution_find_symbol_mutable(LinkResolution *resolution,
                                                         const char *name) {
  size_t i = 0;

  if (!resolution || !name) {
    return NULL;
  }

  if (resolution->symbol_bucket_count) {
    size_t b = mettle_fnv1a_hash(name) & (resolution->symbol_bucket_count - 1);
    while (resolution->symbol_buckets[b]) {
      LinkedSymbol *s = &resolution->symbols[resolution->symbol_buckets[b] - 1];
      if (s->name && strcmp(s->name, name) == 0) {
        return s;
      }
      b = (b + 1) & (resolution->symbol_bucket_count - 1);
    }
    return NULL;
  }

  for (i = 0; i < resolution->symbol_count; i++) {
    if (resolution->symbols[i].name &&
        strcmp(resolution->symbols[i].name, name) == 0) {
      return &resolution->symbols[i];
    }
  }

  return NULL;
}

static int link_resolution_reserve_symbols(LinkResolution *resolution,
                                           size_t minimum_count,
                                           char **error_message_out) {
  LinkedSymbol *grown = NULL;
  size_t new_capacity = 0;

  if (!resolution) {
    return 0;
  }
  if (resolution->symbol_capacity >= minimum_count) {
    return 1;
  }

  new_capacity = resolution->symbol_capacity ? resolution->symbol_capacity : 8u;
  while (new_capacity < minimum_count) {
    new_capacity *= 2u;
  }

  grown = realloc(resolution->symbols, new_capacity * sizeof(LinkedSymbol));
  if (!grown) {
    mettle_set_error(error_message_out,
                              "Out of memory while growing global symbol table");
    return 0;
  }

  memset(grown + resolution->symbol_capacity, 0,
         (new_capacity - resolution->symbol_capacity) * sizeof(LinkedSymbol));
  resolution->symbols = grown;
  resolution->symbol_capacity = new_capacity;
  return 1;
}

static int link_resolution_record_global_symbol(
    LinkResolution *resolution, const LinkedInputObject *input,
    const LinkedObjectSymbol *object_symbol, char **error_message_out) {
  LinkedSymbol *global_symbol = NULL;

  if (!resolution || !input || !object_symbol || !object_symbol->name) {
    return 0;
  }

  global_symbol =
      link_resolution_find_symbol_mutable(resolution, object_symbol->name);
  if (!global_symbol) {
    if (!link_resolution_reserve_symbols(resolution, resolution->symbol_count + 1u,
                                         error_message_out)) {
      return 0;
    }

    global_symbol = &resolution->symbols[resolution->symbol_count++];
    memset(global_symbol, 0, sizeof(*global_symbol));
    global_symbol->name = mettle_strdup(object_symbol->name);
    if (!global_symbol->name) {
      mettle_set_error(error_message_out,
                                "Out of memory while storing symbol '%s'",
                                object_symbol->name);
      return 0;
    }
    global_symbol->defining_object_index = LINKED_SECTION_INDEX_NONE;
    global_symbol->defining_symbol_index = UINT32_MAX;
    if (!link_resolution_index_insert(resolution,
                                      resolution->symbol_count - 1u)) {
      mettle_set_error(error_message_out,
                                "Out of memory while indexing symbol '%s'",
                                object_symbol->name);
      return 0;
    }
  }

  global_symbol->is_external = 1;
  if (!object_symbol->is_defined) {
    return 1;
  }

  if (global_symbol->is_defined) {
    mettle_set_error(
        error_message_out,
        "Duplicate external symbol '%s' in '%s' and object index %zu",
        object_symbol->name, input->path ? input->path : "<unknown>",
        global_symbol->defining_object_index);
    return 0;
  }

  global_symbol->is_defined = 1;
  global_symbol->defining_object_index = object_symbol->object_index;
  global_symbol->defining_symbol_index = object_symbol->symbol_index;
  global_symbol->merged_section_index = object_symbol->merged_section_index;
  global_symbol->merged_offset = object_symbol->merged_offset;
  return 1;
}

static int link_resolution_build_symbols(LinkResolution *resolution,
                                         char **error_message_out) {
  size_t object_index = 0;

  if (!resolution) {
    return 0;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];
    size_t symbol_index = 0;

    input->symbol_count = input->object ? input->object->symbol_count : 0u;
    input->symbols = calloc(input->symbol_count, sizeof(LinkedObjectSymbol));
    if (!input->symbols && input->symbol_count != 0u) {
      mettle_set_error(error_message_out,
                                "Out of memory while mapping object symbols");
      return 0;
    }

    for (symbol_index = 0; symbol_index < input->symbol_count; symbol_index++) {
      const CoffSymbol *symbol = &input->object->symbols[symbol_index];
      LinkedObjectSymbol *resolved = &input->symbols[symbol_index];
      size_t section_index = 0;
      size_t merged_index = LINKED_SECTION_INDEX_NONE;

      resolved->object_index = object_index;
      resolved->symbol_index = (uint32_t)symbol_index;
      resolved->section_number = symbol->section_number;
      resolved->merged_section_index = LINKED_SECTION_INDEX_NONE;
      resolved->is_auxiliary = symbol->is_auxiliary;
      resolved->name = mettle_strdup(symbol->name);
      if (symbol->name && !resolved->name) {
        mettle_set_error(error_message_out,
                                  "Out of memory while storing object symbol");
        return 0;
      }

      if (symbol->is_auxiliary) {
        resolved->is_local = 1;
        continue;
      }

      resolved->is_external = (symbol->storage_class == COFF_STORAGE_CLASS_EXTERNAL);
      resolved->is_local = !resolved->is_external;

      if (symbol->section_number > 0) {
        section_index = (size_t)(symbol->section_number - 1);
        if (section_index >= input->object->section_count) {
          mettle_set_error(error_message_out,
                                    "Symbol '%s' in '%s' refers to section %d "
                                    "outside the section table",
                                    symbol->name ? symbol->name : "<unnamed>",
                                    input->path ? input->path : "<unknown>",
                                    symbol->section_number);
          return 0;
        }

        merged_index = input->section_merged_indices[section_index];
        if (merged_index != LINKED_SECTION_INDEX_NONE) {
          resolved->is_defined = 1;
          resolved->merged_section_index = merged_index;
          resolved->merged_offset =
              input->section_merged_offsets[section_index] + (size_t)symbol->value;
        }
      }

      if (resolved->is_external && resolved->name) {
        if (!link_resolution_record_global_symbol(resolution, input, resolved,
                                                  error_message_out)) {
          return 0;
        }
      }
    }
  }

  return 1;
}

static int link_resolution_assign_virtual_addresses(
    LinkResolution *resolution, size_t section_alignment) {
  size_t section_index = 0;
  uint64_t current_address = 0;
  size_t object_index = 0;
  size_t symbol_index = 0;

  if (!resolution) {
    return 0;
  }

  for (section_index = 0; section_index < LINKED_SECTION_COUNT; section_index++) {
    LinkedSection *section = &resolution->sections[section_index];

    if (section->virtual_size == 0u) {
      continue;
    }

    current_address = (uint64_t)linker_align_up((size_t)current_address,
                                              section_alignment);
    section->virtual_address = current_address;
    current_address += (uint64_t)section->virtual_size;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];
    for (symbol_index = 0; symbol_index < input->symbol_count; symbol_index++) {
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

  for (symbol_index = 0; symbol_index < resolution->symbol_count; symbol_index++) {
    LinkedSymbol *symbol = &resolution->symbols[symbol_index];

    if (!symbol->is_defined ||
        symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
      continue;
    }
    symbol->virtual_address =
        resolution->sections[symbol->merged_section_index].virtual_address +
        (uint64_t)symbol->merged_offset;
  }

  return 1;
}

static int link_resolution_validate_externals(
    LinkResolution *resolution, const LinkResolutionOptions *options,
    char **error_message_out) {
  size_t symbol_index = 0;
  const char *entry_name = NULL;

  if (!resolution) {
    return 0;
  }

  if (!options || !options->allow_unresolved_externals) {
    for (symbol_index = 0; symbol_index < resolution->symbol_count;
         symbol_index++) {
      const LinkedSymbol *symbol = &resolution->symbols[symbol_index];
      if (symbol->is_external && !symbol->is_defined) {
        mettle_set_error(error_message_out,
                                  "Unresolved external symbol '%s'",
                                  symbol->name ? symbol->name : "<unnamed>");
        return 0;
      }
    }
  }

  entry_name = (options && options->entry_symbol_name)
                   ? options->entry_symbol_name
                   : NULL;
  if (entry_name && entry_name[0] != '\0') {
    resolution->entry_symbol = link_resolution_find_symbol(resolution, entry_name);
    if (!resolution->entry_symbol || !resolution->entry_symbol->is_defined) {
      mettle_set_error(error_message_out,
                                "Entry point symbol '%s' was not resolved",
                                entry_name);
      return 0;
    }
  }

  return 1;
}

int link_resolution_build(const char **object_paths, size_t object_count,
                          const LinkResolutionOptions *options,
                          LinkResolution **resolution_out,
                          char **error_message_out) {
  LinkResolution *resolution = NULL;
  size_t section_alignment = 16u;
  int ok = 0;

  if (resolution_out) {
    *resolution_out = NULL;
  }
  if (error_message_out) {
    free(*error_message_out);
    *error_message_out = NULL;
  }

  if (!object_paths || object_count == 0u || !resolution_out) {
    mettle_set_error(error_message_out,
                              "At least one object file is required");
    return 0;
  }

  resolution = calloc(1, sizeof(LinkResolution));
  if (!resolution) {
    mettle_set_error(error_message_out,
                              "Out of memory while creating link resolution");
    return 0;
  }

  link_resolution_init_sections(resolution);
  if (options && options->section_alignment > 1u) {
    section_alignment = options->section_alignment;
  }

  if (!link_resolution_load_objects(resolution, object_paths, object_count,
                                    error_message_out) ||
      !link_resolution_merge_sections(resolution, section_alignment,
                                      error_message_out) ||
      !link_resolution_build_symbols(resolution, error_message_out) ||
      !link_resolution_assign_virtual_addresses(resolution, section_alignment) ||
      !link_resolution_validate_externals(resolution, options,
                                          error_message_out)) {
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (!ok) {
    link_resolution_destroy(resolution);
    return 0;
  }

  *resolution_out = resolution;
  return 1;
}

void link_resolution_destroy(LinkResolution *resolution) {
  size_t object_index = 0;
  size_t section_index = 0;
  size_t symbol_index = 0;

  if (!resolution) {
    return;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];

    if (input->symbols) {
      for (symbol_index = 0; symbol_index < input->symbol_count; symbol_index++) {
        free(input->symbols[symbol_index].name);
      }
    }
    free(input->path);
    free(input->section_merged_indices);
    free(input->section_merged_offsets);
    free(input->section_merged_sizes);
    free(input->section_alignments);
    free(input->symbols);
    coff_object_destroy(input->object);
  }

  for (section_index = 0; section_index < LINKED_SECTION_COUNT; section_index++) {
    free(resolution->sections[section_index].data);
    free(resolution->sections[section_index].contributions);
  }

  for (symbol_index = 0; symbol_index < resolution->symbol_count;
       symbol_index++) {
    free(resolution->symbols[symbol_index].name);
  }

  free(resolution->objects);
  free(resolution->symbols);
  free(resolution->symbol_buckets);
  free(resolution);
}

const LinkedSection *link_resolution_find_section(
    const LinkResolution *resolution, CoffSectionKind kind) {
  size_t section_index = link_section_index_from_kind(kind);

  if (!resolution || section_index == LINKED_SECTION_INDEX_NONE) {
    return NULL;
  }

  return &resolution->sections[section_index];
}

const LinkedSymbol *link_resolution_find_symbol(const LinkResolution *resolution,
                                                const char *name) {
  size_t symbol_index = 0;

  if (!resolution || !name) {
    return NULL;
  }

  if (resolution->symbol_bucket_count) {
    size_t b = mettle_fnv1a_hash(name) & (resolution->symbol_bucket_count - 1);
    while (resolution->symbol_buckets[b]) {
      const LinkedSymbol *s =
          &resolution->symbols[resolution->symbol_buckets[b] - 1];
      if (s->name && strcmp(s->name, name) == 0) {
        return s;
      }
      b = (b + 1) & (resolution->symbol_bucket_count - 1);
    }
    return NULL;
  }

  for (symbol_index = 0; symbol_index < resolution->symbol_count;
       symbol_index++) {
    if (resolution->symbols[symbol_index].name &&
        strcmp(resolution->symbols[symbol_index].name, name) == 0) {
      return &resolution->symbols[symbol_index];
    }
  }

  return NULL;
}
