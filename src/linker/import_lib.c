#include "linker/import_lib.h"
#include "linker/linker_common.h"
#include "../common.h"

#include "linker/coff_reader.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMPORT_ARCHIVE_MEMBER_HEADER_SIZE 60u
#define IMPORT_OBJECT_HEADER_SIZE 20u

typedef struct {
  uint16_t sig1;
  uint16_t sig2;
  uint16_t version;
  uint16_t machine;
  uint32_t time_date_stamp;
  uint32_t size_of_data;
  uint16_t ordinal_or_hint;
  uint16_t type_info;
} ImportObjectHeader;

static int import_library_read_file(const char *path,
                                    unsigned char **file_data_out,
                                    size_t *file_size_out,
                                    char **error_message_out) {
  FILE *file = NULL;
  unsigned char *file_data = NULL;
  long size = 0;

  if (file_data_out) {
    *file_data_out = NULL;
  }
  if (file_size_out) {
    *file_size_out = 0u;
  }
  if (!path || !file_data_out || !file_size_out) {
    mettle_set_error(error_message_out,
                             "Invalid arguments while reading import library");
    return 0;
  }

  file = fopen(path, "rb");
  if (!file) {
    mettle_set_error(error_message_out,
                             "Failed to open import library '%s'", path);
    return 0;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    mettle_set_error(error_message_out,
                             "Failed to seek import library '%s'", path);
    fclose(file);
    return 0;
  }

  size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    mettle_set_error(error_message_out,
                             "Failed to measure import library '%s'", path);
    fclose(file);
    return 0;
  }

  file_data = malloc((size_t)size);
  if (!file_data) {
    mettle_set_error(error_message_out,
                             "Out of memory while reading import library '%s'",
                             path);
    fclose(file);
    return 0;
  }

  if (size > 0 && fread(file_data, 1u, (size_t)size, file) != (size_t)size) {
    mettle_set_error(error_message_out,
                             "Failed while reading import library '%s'", path);
    free(file_data);
    fclose(file);
    return 0;
  }

  fclose(file);
  *file_data_out = file_data;
  *file_size_out = (size_t)size;
  return 1;
}


static int import_library_reserve_symbols(ImportLibrary *library,
                                          size_t minimum_count,
                                          char **error_message_out) {
  ImportLibrarySymbol *grown = NULL;
  size_t new_capacity = 0u;

  if (!library) {
    return 0;
  }
  if (library->symbol_capacity >= minimum_count) {
    return 1;
  }

  new_capacity = library->symbol_capacity ? library->symbol_capacity : 16u;
  while (new_capacity < minimum_count) {
    new_capacity *= 2u;
  }

  grown = realloc(library->symbols, new_capacity * sizeof(ImportLibrarySymbol));
  if (!grown) {
    mettle_set_error(error_message_out,
                             "Out of memory while growing import symbol table");
    return 0;
  }

  memset(grown + library->symbol_capacity, 0,
         (new_capacity - library->symbol_capacity) * sizeof(ImportLibrarySymbol));
  library->symbols = grown;
  library->symbol_capacity = new_capacity;
  return 1;
}

static int import_library_member_name_is_special(const char *member_name) {
  if (!member_name) {
    return 0;
  }

  return strcmp(member_name, "/") == 0 || strcmp(member_name, "//") == 0 ||
         strcmp(member_name, "/<XFGHASHMAP>/") == 0;
}

static void import_library_trim_member_name(char *member_name) {
  size_t length = 0u;

  if (!member_name) {
    return;
  }

  length = strlen(member_name);
  while (length > 0u &&
         (member_name[length - 1u] == ' ' || member_name[length - 1u] == '/')) {
    member_name[--length] = '\0';
  }
}

static int import_library_parse_archive_member_size(const unsigned char *header,
                                                    size_t *size_out,
                                                    char **error_message_out) {
  char size_text[11];
  char *end = NULL;
  unsigned long long parsed = 0u;

  if (!header || !size_out) {
    return 0;
  }

  memcpy(size_text, header + 48u, 10u);
  size_text[10] = '\0';
  parsed = strtoull(size_text, &end, 10);
  while (end && *end != '\0' && isspace((unsigned char)*end)) {
    end++;
  }
  if (!end || *end != '\0') {
    mettle_set_error(error_message_out,
                             "Malformed import library member size");
    return 0;
  }

  *size_out = (size_t)parsed;
  return 1;
}

static int import_library_parse_import_header(const unsigned char *member_data,
                                              size_t member_size,
                                              ImportObjectHeader *header_out) {
  if (!member_data || !header_out || member_size < IMPORT_OBJECT_HEADER_SIZE) {
    return 0;
  }

  header_out->sig1 = linker_read_u16(member_data);
  header_out->sig2 = linker_read_u16(member_data + 2u);
  header_out->version = linker_read_u16(member_data + 4u);
  header_out->machine = linker_read_u16(member_data + 6u);
  header_out->time_date_stamp = linker_read_u32(member_data + 8u);
  header_out->size_of_data = linker_read_u32(member_data + 12u);
  header_out->ordinal_or_hint = linker_read_u16(member_data + 16u);
  header_out->type_info = linker_read_u16(member_data + 18u);
  return 1;
}

static int import_library_parse_strings(const unsigned char *member_data,
                                        size_t member_size,
                                        char **symbol_name_out,
                                        char **dll_name_out,
                                        char **error_message_out) {
  const unsigned char *cursor = NULL;
  size_t remaining = 0u;
  size_t symbol_length = 0u;
  size_t dll_length = 0u;
  char *symbol_name = NULL;
  char *dll_name = NULL;

  if (symbol_name_out) {
    *symbol_name_out = NULL;
  }
  if (dll_name_out) {
    *dll_name_out = NULL;
  }
  if (!member_data || member_size < IMPORT_OBJECT_HEADER_SIZE || !symbol_name_out ||
      !dll_name_out) {
    mettle_set_error(error_message_out,
                             "Malformed short import library member");
    return 0;
  }

  cursor = member_data + IMPORT_OBJECT_HEADER_SIZE;
  remaining = member_size - IMPORT_OBJECT_HEADER_SIZE;
  while (symbol_length < remaining && cursor[symbol_length] != '\0') {
    symbol_length++;
  }
  if (symbol_length >= remaining) {
    mettle_set_error(error_message_out,
                             "Import library member is missing the symbol name");
    return 0;
  }

  symbol_name = malloc(symbol_length + 1u);
  if (!symbol_name) {
    mettle_set_error(error_message_out,
                             "Out of memory while parsing import symbol");
    return 0;
  }
  memcpy(symbol_name, cursor, symbol_length);
  symbol_name[symbol_length] = '\0';

  cursor += symbol_length + 1u;
  remaining -= symbol_length + 1u;
  while (dll_length < remaining && cursor[dll_length] != '\0') {
    dll_length++;
  }
  if (dll_length >= remaining) {
    mettle_set_error(error_message_out,
                             "Import library member is missing the DLL name");
    free(symbol_name);
    return 0;
  }

  dll_name = malloc(dll_length + 1u);
  if (!dll_name) {
    mettle_set_error(error_message_out,
                             "Out of memory while parsing import DLL name");
    free(symbol_name);
    return 0;
  }
  memcpy(dll_name, cursor, dll_length);
  dll_name[dll_length] = '\0';

  *symbol_name_out = symbol_name;
  *dll_name_out = dll_name;
  return 1;
}

static int import_library_append_symbol(ImportLibrary *library,
                                        const ImportObjectHeader *header,
                                        char *symbol_name, char *dll_name,
                                        char **error_message_out) {
  ImportLibrarySymbol *symbol = NULL;

  if (!library || !header || !symbol_name || !dll_name) {
    free(symbol_name);
    free(dll_name);
    return 0;
  }

  if (!import_library_reserve_symbols(library, library->symbol_count + 1u,
                                      error_message_out)) {
    free(symbol_name);
    free(dll_name);
    return 0;
  }

  symbol = &library->symbols[library->symbol_count++];
  memset(symbol, 0, sizeof(*symbol));
  symbol->symbol_name = symbol_name;
  symbol->dll_name = dll_name;
  symbol->machine = header->machine;
  symbol->ordinal_or_hint = header->ordinal_or_hint;
  symbol->import_type = (uint16_t)(header->type_info & 0x0003u);
  symbol->name_type = (uint16_t)((header->type_info >> 2u) & 0x0007u);
  symbol->is_ordinal = (symbol->name_type == IMPORT_OBJECT_NAME_TYPE_ORDINAL);
  return 1;
}

int import_library_read(const char *path, ImportLibrary **library_out,
                        char **error_message_out) {
  unsigned char *file_data = NULL;
  size_t file_size = 0u;
  ImportLibrary *library = NULL;
  size_t offset = 0u;
  int ok = 0;

  if (library_out) {
    *library_out = NULL;
  }
  if (error_message_out) {
    free(*error_message_out);
    *error_message_out = NULL;
  }

  if (!path || !library_out) {
    mettle_set_error(error_message_out,
                             "An import library path is required");
    return 0;
  }

  if (!import_library_read_file(path, &file_data, &file_size,
                                error_message_out)) {
    return 0;
  }
  if (file_size < 8u || memcmp(file_data, "!<arch>\n", 8u) != 0) {
    mettle_set_error(error_message_out,
                             "Import library '%s' is not a COFF archive", path);
    goto cleanup;
  }

  library = calloc(1, sizeof(ImportLibrary));
  if (!library) {
    mettle_set_error(error_message_out,
                             "Out of memory while creating import library");
    goto cleanup;
  }

  library->path = mettle_strdup(path);
  if (!library->path) {
    mettle_set_error(error_message_out,
                             "Out of memory while storing import library path");
    goto cleanup;
  }

  offset = 8u;
  while (offset + IMPORT_ARCHIVE_MEMBER_HEADER_SIZE <= file_size) {
    const unsigned char *member_header = file_data + offset;
    size_t member_size = 0u;
    size_t member_data_offset = 0u;
    char member_name[17];
    ImportObjectHeader header;
    char *symbol_name = NULL;
    char *dll_name = NULL;

    if (member_header[58] != '`' || member_header[59] != '\n') {
      mettle_set_error(error_message_out,
                               "Malformed archive member header in '%s'", path);
      goto cleanup;
    }
    if (!import_library_parse_archive_member_size(member_header, &member_size,
                                                  error_message_out)) {
      goto cleanup;
    }

    if (offset > SIZE_MAX - IMPORT_ARCHIVE_MEMBER_HEADER_SIZE) {
      mettle_set_error(error_message_out,
                       "Import library '%s' archive offset overflows",
                       path);
      goto cleanup;
    }

    member_data_offset = offset + IMPORT_ARCHIVE_MEMBER_HEADER_SIZE;
    if (member_data_offset > file_size ||
        member_size > file_size - member_data_offset) {
      mettle_set_error(error_message_out,
                               "Import library '%s' has a truncated archive member",
                               path);
      goto cleanup;
    }

    memcpy(member_name, member_header, 16u);
    member_name[16] = '\0';
    import_library_trim_member_name(member_name);

    if (!import_library_member_name_is_special(member_name) &&
        import_library_parse_import_header(file_data + member_data_offset,
                                           member_size, &header) &&
        header.sig1 == 0u && header.sig2 == 0xFFFFu &&
        header.machine == COFF_MACHINE_AMD64) {
      if (!import_library_parse_strings(file_data + member_data_offset, member_size,
                                        &symbol_name, &dll_name,
                                        error_message_out) ||
          !import_library_append_symbol(library, &header, symbol_name, dll_name,
                                        error_message_out)) {
        goto cleanup;
      }
    }

    if (member_data_offset > SIZE_MAX - member_size) {
      mettle_set_error(error_message_out,
                       "Import library '%s' archive member size overflows",
                       path);
      goto cleanup;
    }

    offset = member_data_offset + member_size;
    if ((member_size & 1u) != 0u) {
      if (offset == SIZE_MAX) {
        mettle_set_error(error_message_out,
                         "Import library '%s' archive offset overflows",
                         path);
        goto cleanup;
      }
      offset++;
    }
  }

  if (library->symbol_count == 0u) {
    mettle_set_error(error_message_out,
                             "Import library '%s' did not contain AMD64 import members",
                             path);
    goto cleanup;
  }

  ok = 1;

cleanup:
  free(file_data);
  if (!ok) {
    import_library_destroy(library);
    return 0;
  }

  *library_out = library;
  return 1;
}

void import_library_destroy(ImportLibrary *library) {
  size_t i = 0u;

  if (!library) {
    return;
  }

  for (i = 0u; i < library->symbol_count; i++) {
    free(library->symbols[i].symbol_name);
    free(library->symbols[i].dll_name);
  }

  free(library->path);
  free(library->symbols);
  free(library);
}

const ImportLibrarySymbol *import_library_find_symbol(const ImportLibrary *library,
                                                      const char *symbol_name) {
  size_t i = 0u;

  if (!library || !symbol_name) {
    return NULL;
  }

  for (i = 0u; i < library->symbol_count; i++) {
    if (library->symbols[i].symbol_name &&
        strcmp(library->symbols[i].symbol_name, symbol_name) == 0) {
      return &library->symbols[i];
    }
  }

  return NULL;
}
