#include "flat_emitter.h"

#include <stdarg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t section_index;
  uint64_t address;
  size_t file_offset;
} FlatPlacement;

static void flat_fail(char *error, size_t error_size, const char *format, ...) {
  va_list arguments;
  if (!error || !error_size) {
    return;
  }
  va_start(arguments, format);
  vsnprintf(error, error_size, format, arguments);
  va_end(arguments);
}

static int flat_section_is_loadable(const BinarySection *section) {
  return section && section->kind != BINARY_SECTION_DEBUG && section->size > 0;
}

static int flat_section_rank(const BinarySection *section) {
  switch (section->kind) {
  case BINARY_SECTION_TEXT:
    return 0;
  case BINARY_SECTION_RDATA:
    return 1;
  case BINARY_SECTION_DATA:
    return 2;
  case BINARY_SECTION_INIT_ARRAY:
    return 3;
  case BINARY_SECTION_FINI_ARRAY:
    return 4;
  case BINARY_SECTION_BSS:
    return 6;
  default:
    return 5;
  }
}

static const BinarySymbol *flat_find_symbol(const BinaryEmitter *emitter,
                                            const char *name) {
  size_t i;
  for (i = 0; i < emitter->symbol_count; i++) {
    const BinarySymbol *symbol = &emitter->symbols[i];
    if (symbol->name && strcmp(symbol->name, name) == 0 &&
        symbol->binding != BINARY_SYMBOL_EXTERNAL) {
      return symbol;
    }
  }
  return NULL;
}

int binary_emitter_write_flat(BinaryEmitter *emitter, const char *path,
                              uint64_t image_base, const char *entry_symbol,
                              size_t pad_to, unsigned char pad_byte,
                              const unsigned char *trailer,
                              size_t trailer_size, char *error,
                              size_t error_size) {
  FlatPlacement *placements = NULL;
  size_t placement_count = 0;
  size_t i;
  size_t j;
  size_t cursor = 0;
  unsigned char *image = NULL;
  size_t image_size = 0;
  size_t entry_section = (size_t)-1;
  FILE *file = NULL;
  int result = 0;

  if (!emitter || !path) {
    flat_fail(error, error_size, "no emitter or output path");
    return 0;
  }

  if (entry_symbol && entry_symbol[0]) {
    const BinarySymbol *entry = flat_find_symbol(emitter, entry_symbol);
    if (!entry) {
      flat_fail(error, error_size,
                "flat image entry point '%s' is not defined in this program",
                entry_symbol);
      return 0;
    }
    if (entry->value != 0) {
      flat_fail(error, error_size,
                "flat image entry point '%s' is not the first byte of its "
                "section",
                entry_symbol);
      return 0;
    }
    entry_section = entry->section_index;
  }

  placements =
      (FlatPlacement *)calloc(emitter->section_count + 1, sizeof(FlatPlacement));
  if (!placements) {
    flat_fail(error, error_size, "out of memory");
    return 0;
  }

  if (entry_section != (size_t)-1) {
    const BinarySection *section =
        binary_emitter_get_section_const(emitter, entry_section);
    if (flat_section_is_loadable(section)) {
      placements[placement_count].section_index = entry_section;
      placements[placement_count].file_offset = 0;
      placement_count++;
      cursor = section->size;
    }
  }

  for (int rank = 0; rank <= 6; rank++) {
    for (i = 0; i < emitter->section_count; i++) {
      const BinarySection *section =
          binary_emitter_get_section_const(emitter, i);
      size_t alignment;
      if (!flat_section_is_loadable(section) || i == entry_section ||
          flat_section_rank(section) != rank) {
        continue;
      }
      alignment = section->alignment ? section->alignment : 1;
      if (alignment > 1 && cursor % alignment) {
        cursor += alignment - (cursor % alignment);
      }
      placements[placement_count].section_index = i;
      placements[placement_count].file_offset = cursor;
      placement_count++;
      cursor += section->size;
    }
  }

  image_size = cursor;
  if (pad_to > image_size) {
    image_size = pad_to;
  }
  if (trailer && trailer_size) {
    if (pad_to) {
      if (pad_to < trailer_size || cursor > pad_to - trailer_size) {
        flat_fail(error, error_size,
                  "flat image is %zu bytes, which leaves no room for its %zu "
                  "byte trailer inside %zu bytes",
                  cursor, trailer_size, pad_to);
        free(placements);
        return 0;
      }
    } else {
      image_size = cursor + trailer_size;
    }
  }

  image = (unsigned char *)malloc(image_size ? image_size : 1);
  if (!image) {
    flat_fail(error, error_size, "out of memory");
    free(placements);
    return 0;
  }
  memset(image, pad_byte, image_size);

  for (i = 0; i < placement_count; i++) {
    const BinarySection *section = binary_emitter_get_section_const(
        emitter, placements[i].section_index);
    placements[i].address = image_base + placements[i].file_offset;
    if (section->kind == BINARY_SECTION_BSS || !section->data) {
      memset(image + placements[i].file_offset, 0, section->size);
      continue;
    }
    memcpy(image + placements[i].file_offset, section->data, section->size);
  }

  for (i = 0; i < emitter->relocation_count; i++) {
    const BinaryRelocation *relocation = &emitter->relocations[i];
    const BinarySymbol *symbol = NULL;
    uint64_t target = 0;
    uint64_t site = 0;
    size_t site_offset = 0;
    int found_site = 0;
    int found_target = 0;

    for (j = 0; j < placement_count; j++) {
      if (placements[j].section_index == relocation->section_index) {
        site_offset = placements[j].file_offset + relocation->offset;
        site = image_base + site_offset;
        found_site = 1;
        break;
      }
    }
    if (!found_site) {
      continue;
    }

    symbol = flat_find_symbol(emitter, relocation->symbol_name);
    if (!symbol) {
      const BinarySection *from =
          binary_emitter_get_section_const(emitter, relocation->section_index);
      flat_fail(error, error_size,
                "%s references '%s', which nothing in this program defines. A "
                "flat image links no library, so every name it uses has to be "
                "defined in it",
                from && from->name ? from->name : "the image",
                relocation->symbol_name ? relocation->symbol_name : "<null>");
      goto cleanup;
    }
    for (j = 0; j < placement_count; j++) {
      if (placements[j].section_index == symbol->section_index) {
        target = image_base + placements[j].file_offset + symbol->value;
        found_target = 1;
        break;
      }
    }
    if (!found_target) {
      flat_fail(error, error_size,
                "flat image references '%s', which lives in a section the "
                "image does not load",
                relocation->symbol_name);
      goto cleanup;
    }

    switch (relocation->kind) {
    case BINARY_RELOCATION_REL32: {
      int64_t displacement =
          (int64_t)target - (int64_t)(site + 4) + relocation->addend;
      if (displacement < -2147483648LL || displacement > 2147483647LL) {
        flat_fail(error, error_size,
                  "flat image displacement to '%s' does not fit in 32 bits",
                  relocation->symbol_name);
        goto cleanup;
      }
      for (j = 0; j < 4; j++) {
        image[site_offset + j] =
            (unsigned char)(((uint64_t)displacement >> (8 * j)) & 0xFFu);
      }
      break;
    }
    case BINARY_RELOCATION_ADDR64: {
      uint64_t value = target + (uint64_t)(int64_t)relocation->addend;
      for (j = 0; j < 8; j++) {
        image[site_offset + j] = (unsigned char)((value >> (8 * j)) & 0xFFu);
      }
      break;
    }
    case BINARY_RELOCATION_REL16: {
      int64_t displacement =
          (int64_t)target - (int64_t)(site + 2) + relocation->addend;
      if (displacement < -32768LL || displacement > 32767LL) {
        flat_fail(error, error_size,
                  "flat image displacement to '%s' does not fit in 16 bits",
                  relocation->symbol_name);
        goto cleanup;
      }
      for (j = 0; j < 2; j++) {
        image[site_offset + j] =
            (unsigned char)(((uint64_t)displacement >> (8 * j)) & 0xFFu);
      }
      break;
    }
    case BINARY_RELOCATION_ADDR16: {
      uint64_t value = target + (uint64_t)(int64_t)relocation->addend;
      if (value > 0xFFFFu) {
        flat_fail(error, error_size,
                  "flat image address of '%s' does not fit in 16 bits",
                  relocation->symbol_name);
        goto cleanup;
      }
      for (j = 0; j < 2; j++) {
        image[site_offset + j] = (unsigned char)((value >> (8 * j)) & 0xFFu);
      }
      break;
    }
    case BINARY_RELOCATION_ADDR32NB: {
      uint64_t value = target + (uint64_t)(int64_t)relocation->addend;
      for (j = 0; j < 4; j++) {
        image[site_offset + j] = (unsigned char)((value >> (8 * j)) & 0xFFu);
      }
      break;
    }
    default:
      flat_fail(error, error_size,
                "flat image cannot apply relocation kind %d to '%s'",
                (int)relocation->kind, relocation->symbol_name);
      goto cleanup;
    }
  }

  if (trailer && trailer_size) {
    memcpy(image + image_size - trailer_size, trailer, trailer_size);
  }

  file = fopen(path, "wb");
  if (!file) {
    flat_fail(error, error_size, "could not open '%s' for writing", path);
    goto cleanup;
  }
  if (image_size && fwrite(image, 1, image_size, file) != image_size) {
    flat_fail(error, error_size, "could not write '%s'", path);
    fclose(file);
    goto cleanup;
  }
  fclose(file);
  result = 1;

cleanup:
  free(image);
  free(placements);
  return result;
}
