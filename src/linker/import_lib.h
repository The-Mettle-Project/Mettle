#ifndef IMPORT_LIB_H
#define IMPORT_LIB_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  IMPORT_OBJECT_TYPE_CODE = 0,
  IMPORT_OBJECT_TYPE_DATA = 1,
  IMPORT_OBJECT_TYPE_CONST = 2,
} ImportObjectType;

typedef enum {
  IMPORT_OBJECT_NAME_TYPE_ORDINAL = 0,
  IMPORT_OBJECT_NAME_TYPE_NAME = 1,
  IMPORT_OBJECT_NAME_TYPE_NOPREFIX = 2,
  IMPORT_OBJECT_NAME_TYPE_UNDECORATE = 3,
  IMPORT_OBJECT_NAME_TYPE_EXPORTAS = 4,
} ImportObjectNameType;

typedef struct {
  char *symbol_name;
  char *dll_name;
  uint16_t machine;
  uint16_t ordinal_or_hint;
  uint16_t import_type;
  uint16_t name_type;
  int is_ordinal;
} ImportLibrarySymbol;

typedef struct {
  char *path;
  ImportLibrarySymbol *symbols;
  size_t symbol_count;
  size_t symbol_capacity;
} ImportLibrary;

int import_library_read(const char *path, ImportLibrary **library_out,
                        char **error_message_out);
void import_library_destroy(ImportLibrary *library);

const ImportLibrarySymbol *import_library_find_symbol(const ImportLibrary *library,
                                                      const char *symbol_name);

#endif // IMPORT_LIB_H
