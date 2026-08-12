#include "linker/relocation.h"
#include "linker/linker_common.h"
#include "../common.h"
#include "linker/symbol_resolve.h"

/* Supported AMD64 relocation kinds here match object input after
 * binary_emitter_map_relocation_kind() in binary_emitter.c. Summary:
 * docs/linker-build-pipelines.md */

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  size_t merged_section_index;
  size_t merged_offset;
  uint64_t virtual_address;
} RelocationTarget;

static uint64_t relocation_read_u64(const unsigned char *bytes) {
  return linker_read_u64(bytes);
}

static int relocation_section_is_debug_only(const CoffSection *section) {
  const char *name = NULL;

  if (!section || !section->name) {
    return 0;
  }

  name = section->name;
  return strncmp(name, ".debug", 6u) == 0 || strncmp(name, ".zdebug", 7u) == 0;
}

static int relocation_resolve_target(const LinkResolution *resolution,
                                     const LinkedInputObject *input,
                                     uint32_t symbol_index,
                                     RelocationTarget *target_out,
                                     char **error_message_out) {
  const LinkedObjectSymbol *object_symbol = NULL;
  const LinkedSymbol *global_symbol = NULL;

  if (!resolution || !input || !target_out) {
    return 0;
  }
  if (symbol_index >= input->symbol_count) {
    mettle_set_error(error_message_out,
                         "Relocation refers to symbol index %u outside the "
                         "object symbol table",
                         symbol_index);
    return 0;
  }

  object_symbol = &input->symbols[symbol_index];
  if (object_symbol->name) {
    target_out->name = object_symbol->name;
  } else {
    target_out->name = "<unnamed>";
  }

  if (object_symbol->is_auxiliary) {
    mettle_set_error(error_message_out,
                         "Relocation refers to auxiliary symbol '%s'",
                         target_out->name);
    return 0;
  }

  if (object_symbol->is_defined &&
      object_symbol->merged_section_index != LINKED_SECTION_INDEX_NONE) {
    target_out->merged_section_index = object_symbol->merged_section_index;
    target_out->merged_offset = object_symbol->merged_offset;
    target_out->virtual_address = object_symbol->virtual_address;
    return 1;
  }

  global_symbol = link_resolution_find_symbol(resolution, object_symbol->name);
  if (!global_symbol || !global_symbol->is_defined ||
      global_symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
    mettle_set_error(error_message_out,
                         "Relocation target '%s' is unresolved",
                         target_out->name);
    return 0;
  }

  target_out->merged_section_index = global_symbol->merged_section_index;
  target_out->merged_offset = global_symbol->merged_offset;
  target_out->virtual_address = global_symbol->virtual_address;
  return 1;
}

int link_apply_relocations(LinkResolution *resolution,
                           const LinkRelocationOptions *options,
                           char **error_message_out) {
  uint64_t image_base = 0;
  size_t object_index = 0;

  if (error_message_out) {
    free(*error_message_out);
    *error_message_out = NULL;
  }
  if (!resolution) {
    mettle_set_error(error_message_out,
                         "Link resolution is required before applying relocations");
    return 0;
  }
  if (options) {
    image_base = options->image_base;
  }
  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    const LinkedInputObject *input = &resolution->objects[object_index];
    size_t section_index = 0;

    if (!input->object) {
      continue;
    }

    for (section_index = 0; section_index < input->object->section_count;
         section_index++) {
      const CoffSection *source_section = &input->object->sections[section_index];
      size_t merged_section_index = LINKED_SECTION_INDEX_NONE;
      size_t relocation_index = 0;

      merged_section_index = input->section_merged_indices[section_index];
      if (source_section->relocation_count == 0u) {
        continue;
      }
      if (merged_section_index == LINKED_SECTION_INDEX_NONE) {
        if (relocation_section_is_debug_only(source_section)) {
          continue;
        }
        mettle_set_error(error_message_out,
                             "Section '%s' has relocations but was not merged",
                             source_section->name ? source_section->name
                                                  : "<unknown>");
        return 0;
      }

      for (relocation_index = 0;
           relocation_index < source_section->relocation_count;
           relocation_index++) {
        const CoffRelocation *relocation =
            &source_section->relocations[relocation_index];
        LinkedSection *merged = &resolution->sections[merged_section_index];
        RelocationTarget target = {0};
        size_t patch_offset = input->section_merged_offsets[section_index] +
                              (size_t)relocation->virtual_address;
        uint64_t patch_address = merged->virtual_address + patch_offset;
        int64_t addend = 0;
        int64_t value = 0;
        size_t width = 0;

        if (!relocation_resolve_target(resolution, input,
                                       relocation->symbol_table_index, &target,
                                       error_message_out)) {
          return 0;
        }

        switch (relocation->type) {
        case COFF_RELOC_AMD64_ADDR64:
          width = 8u;
          break;
        case COFF_RELOC_AMD64_ADDR32NB:
        case COFF_RELOC_AMD64_REL32:
        case COFF_RELOC_AMD64_SECREL:
          width = 4u;
          break;
        default:
          mettle_set_error(error_message_out,
                               "Unsupported relocation type %s for symbol '%s'",
                               coff_relocation_type_name(relocation->type),
                               target.name);
          return 0;
        }

        if (patch_offset + width > merged->size) {
          mettle_set_error(error_message_out,
                               "Relocation for symbol '%s' writes past merged "
                               "section '%s'",
                               target.name, merged->name);
          return 0;
        }

        if (width == 8u) {
          addend = (int64_t)relocation_read_u64(merged->data + patch_offset);
        } else {
          addend = (int64_t)(int32_t)linker_read_u32(merged->data + patch_offset);
        }

        switch (relocation->type) {
        case COFF_RELOC_AMD64_REL32:
          value = (int64_t)target.virtual_address + addend -
                  (int64_t)(patch_address + 4u);
          if (value < INT32_MIN || value > INT32_MAX) {
            mettle_set_error(error_message_out,
                                 "REL32 relocation for symbol '%s' is out of range",
                                 target.name);
            return 0;
          }
          linker_write_u32(merged->data + patch_offset, (uint32_t)(int32_t)value);
          break;
        case COFF_RELOC_AMD64_ADDR64:
          value = (int64_t)target.virtual_address + addend;
          if (value < 0) {
            mettle_set_error(error_message_out,
                                 "ADDR64 relocation for symbol '%s' is negative",
                                 target.name);
            return 0;
          }
          linker_write_u64(merged->data + patch_offset, (uint64_t)value);
          break;
        case COFF_RELOC_AMD64_ADDR32NB:
          if (image_base != 0u && target.virtual_address >= image_base) {
            value = (int64_t)(target.virtual_address - image_base) + addend;
          } else {
            value = (int64_t)target.virtual_address + addend;
          }
          if (value < 0 || (uint64_t)value > UINT32_MAX) {
            mettle_set_error(error_message_out,
                                 "ADDR32NB relocation for symbol '%s' is out of range",
                                 target.name);
            return 0;
          }
          linker_write_u32(merged->data + patch_offset, (uint32_t)value);
          break;
        case COFF_RELOC_AMD64_SECREL:
          value = (int64_t)target.merged_offset + addend;
          if (value < 0 || (uint64_t)value > UINT32_MAX) {
            mettle_set_error(error_message_out,
                                 "SECREL relocation for symbol '%s' is out of range",
                                 target.name);
            return 0;
          }
          linker_write_u32(merged->data + patch_offset, (uint32_t)value);
          break;
        }
      }
    }
  }

  return 1;
}
