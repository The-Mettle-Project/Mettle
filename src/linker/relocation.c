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

static int relocation_section_is_debug_only(const LinkSection *section) {
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

static int relocation_gotpcrelx_group_extension(unsigned char opcode,
                                                unsigned char *extension_out) {
  switch (opcode) {
  case 0x03u: *extension_out = 0u; return 1;
  case 0x0Bu: *extension_out = 1u; return 1;
  case 0x13u: *extension_out = 2u; return 1;
  case 0x1Bu: *extension_out = 3u; return 1;
  case 0x23u: *extension_out = 4u; return 1;
  case 0x2Bu: *extension_out = 5u; return 1;
  case 0x33u: *extension_out = 6u; return 1;
  case 0x3Bu: *extension_out = 7u; return 1;
  default: return 0;
  }
}

static int relocation_relax_gotpcrelx(LinkedSection *merged,
                                      size_t patch_offset,
                                      const char *symbol_name,
                                      int *becomes_absolute_out,
                                      char **error_message_out) {
  unsigned char *opcode;
  unsigned char *modrm;
  unsigned char *rex;
  unsigned char extension = 0u;
  unsigned char destination;

  *becomes_absolute_out = 0;
  if (patch_offset < 2u) {
    mettle_set_error(error_message_out,
                     "GOTPCRELX relocation for symbol '%s' has no room for the "
                     "instruction it belongs to",
                     symbol_name ? symbol_name : "<unknown>");
    return 0;
  }
  opcode = merged->data + patch_offset - 2u;
  modrm = merged->data + patch_offset - 1u;
  if ((*modrm & 0xC7u) != 0x05u) {
    mettle_set_error(error_message_out,
                     "GOTPCRELX relocation for symbol '%s' is not the "
                     "RIP-relative load this linker can relax (opcode %02x, "
                     "modrm %02x)",
                     symbol_name ? symbol_name : "<unknown>",
                     (unsigned)*opcode, (unsigned)*modrm);
    return 0;
  }
  if (*opcode == 0x8Bu) {
    *opcode = 0x8Du;
    return 1;
  }
  if (!relocation_gotpcrelx_group_extension(*opcode, &extension) &&
      *opcode != 0x85u) {
    mettle_set_error(error_message_out,
                     "GOTPCRELX relocation for symbol '%s' is not the "
                     "RIP-relative load this linker can relax (opcode %02x, "
                     "modrm %02x)",
                     symbol_name ? symbol_name : "<unknown>",
                     (unsigned)*opcode, (unsigned)*modrm);
    return 0;
  }
  if (patch_offset < 3u) {
    mettle_set_error(error_message_out,
                     "GOTPCRELX relocation for symbol '%s' has no room for the "
                     "instruction it belongs to",
                     symbol_name ? symbol_name : "<unknown>");
    return 0;
  }
  rex = merged->data + patch_offset - 3u;
  if ((*rex & 0xF0u) != 0x40u) {
    mettle_set_error(error_message_out,
                     "GOTPCRELX relocation for symbol '%s' folds into an "
                     "immediate, which needs the REX prefix its operand width "
                     "is written with (opcode %02x, prefix %02x)",
                     symbol_name ? symbol_name : "<unknown>",
                     (unsigned)*opcode, (unsigned)*rex);
    return 0;
  }
  destination = (unsigned char)((*modrm >> 3) & 0x07u);
  if (*opcode == 0x85u) {
    *opcode = 0xF7u;
    extension = 0u;
  } else {
    *opcode = 0x81u;
  }
  *modrm = (unsigned char)(0xC0u | (extension << 3) | destination);
  *rex = (unsigned char)((*rex & 0xFAu) | ((*rex & 0x04u) >> 2));
  *becomes_absolute_out = 1;
  return 1;
}

static int link_apply_section_relocations(
    LinkResolution *resolution, const LinkedInputObject *input,
    const LinkSection *source_section, size_t section_index,
    size_t merged_section_index, uint64_t image_base, size_t tls_size,
    char **error_message_out) {
  size_t relocation_index = 0;

  for (relocation_index = 0;
       relocation_index < source_section->relocation_count;
       relocation_index++) {
    const LinkReloc *relocation =
        &source_section->relocations[relocation_index];
    LinkedSection *merged = &resolution->sections[merged_section_index];
    RelocationTarget target = {0};
    size_t patch_offset = input->section_merged_offsets[section_index] +
                          (size_t)relocation->offset;
    uint64_t patch_address = merged->virtual_address + patch_offset;
    int64_t addend = 0;
    int64_t value = 0;
    size_t width = 0;
    int gotpcrelx_absolute = 0;

    if (!relocation_resolve_target(resolution, input,
                                   relocation->symbol_index, &target,
                                   error_message_out)) {
      return 0;
    }

    switch (relocation->kind) {
    case LINK_RELOC_ABS64:
      width = 8u;
      break;
    case LINK_RELOC_PC32:
    case LINK_RELOC_ABS32:
    case LINK_RELOC_IMAGE_REL32:
    case LINK_RELOC_SECREL32:
    case LINK_RELOC_TPOFF32:
    case LINK_RELOC_GOTPCRELX32:
      width = 4u;
      break;
    default:
      mettle_set_error(error_message_out,
                           "Unsupported relocation kind %s for symbol '%s'",
                           link_reloc_kind_name(relocation->kind),
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

    if (relocation->addend_is_explicit) {
      addend = relocation->addend;
    } else if (width == 8u) {
      addend = (int64_t)relocation_read_u64(merged->data + patch_offset);
    } else {
      addend = (int64_t)(int32_t)linker_read_u32(merged->data + patch_offset);
    }

    if (relocation->kind == LINK_RELOC_GOTPCRELX32 &&
        !relocation_relax_gotpcrelx(merged, patch_offset, target.name,
                                    &gotpcrelx_absolute, error_message_out)) {
      return 0;
    }
    if (gotpcrelx_absolute) {
      value = (int64_t)target.virtual_address + addend + 4;
      if (value < 0 || value > INT32_MAX) {
        mettle_set_error(error_message_out,
                             "GOTPCRELX relocation for symbol '%s' folds into "
                             "an immediate that is out of range",
                             target.name);
        return 0;
      }
      linker_write_u32(merged->data + patch_offset, (uint32_t)(int32_t)value);
      continue;
    }
    switch (relocation->kind) {
    case LINK_RELOC_PC32:
    case LINK_RELOC_GOTPCRELX32:
      value = (int64_t)target.virtual_address + addend -
              (int64_t)patch_address;
      if (!relocation->addend_is_explicit) {
        value -= 4;
      }
      if (value < INT32_MIN || value > INT32_MAX) {
        mettle_set_error(error_message_out,
                             "PC32 relocation for symbol '%s' is out of range",
                             target.name);
        return 0;
      }
      linker_write_u32(merged->data + patch_offset, (uint32_t)(int32_t)value);
      break;
    case LINK_RELOC_ABS64:
      value = (int64_t)target.virtual_address + addend;
      if (value < 0) {
        mettle_set_error(error_message_out,
                             "ABS64 relocation for symbol '%s' is negative",
                             target.name);
        return 0;
      }
      linker_write_u64(merged->data + patch_offset, (uint64_t)value);
      break;
    case LINK_RELOC_ABS32:
      value = (int64_t)target.virtual_address + addend;
      if (value < 0 || (uint64_t)value > UINT32_MAX) {
        mettle_set_error(error_message_out,
                             "ABS32 relocation for symbol '%s' is out of range",
                             target.name);
        return 0;
      }
      linker_write_u32(merged->data + patch_offset, (uint32_t)value);
      break;
    case LINK_RELOC_IMAGE_REL32:
      if (image_base != 0u && target.virtual_address >= image_base) {
        value = (int64_t)(target.virtual_address - image_base) + addend;
      } else {
        value = (int64_t)target.virtual_address + addend;
      }
      if (value < 0 || (uint64_t)value > UINT32_MAX) {
        mettle_set_error(error_message_out,
                             "IMAGE_REL32 relocation for symbol '%s' is out of range",
                             target.name);
        return 0;
      }
      linker_write_u32(merged->data + patch_offset, (uint32_t)value);
      break;
    case LINK_RELOC_SECREL32:
      value = (int64_t)target.merged_offset + addend;
      if (value < 0 || (uint64_t)value > UINT32_MAX) {
        mettle_set_error(error_message_out,
                             "SECREL32 relocation for symbol '%s' is out of range",
                             target.name);
        return 0;
      }
      linker_write_u32(merged->data + patch_offset, (uint32_t)value);
      break;
    case LINK_RELOC_TPOFF32:
      value = (int64_t)target.merged_offset + addend - (int64_t)tls_size;
      if (value < INT32_MIN || value > INT32_MAX) {
        mettle_set_error(error_message_out,
                             "TPOFF32 relocation for symbol '%s' is out of range",
                             target.name);
        return 0;
      }
      linker_write_u32(merged->data + patch_offset, (uint32_t)(int32_t)value);
      break;
    default:
      break;
    }
  }

  return 1;
}

int link_apply_relocations(LinkResolution *resolution,
                           const LinkRelocationOptions *options,
                           char **error_message_out) {
  uint64_t image_base = 0;
  size_t tls_size = 0;
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
  {
    const LinkedSection *tls =
        link_resolution_find_section(resolution, LINK_SECTION_KIND_TLS);
    if (tls) {
      tls_size = tls->virtual_size > tls->size ? tls->virtual_size : tls->size;
    }
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
      const LinkSection *source_section = &input->object->sections[section_index];
      size_t merged_section_index = LINKED_SECTION_INDEX_NONE;

      merged_section_index = input->section_merged_indices[section_index];
      if (source_section->relocation_count == 0u) {
        continue;
      }
      if (input->section_gc_dead && input->section_gc_dead[section_index]) {
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

      if (!link_apply_section_relocations(resolution, input, source_section,
                                          section_index,
                                          merged_section_index, image_base,
                                          tls_size, error_message_out)) {
        return 0;
      }
    }
  }

  return 1;
}
