#ifndef COFF_READER_H
#define COFF_READER_H

#include <stddef.h>
#include <stdint.h>

enum {
  COFF_MACHINE_AMD64 = 0x8664,
};

typedef enum {
  COFF_SECTION_KIND_UNKNOWN = 0,
  COFF_SECTION_KIND_TEXT,
  COFF_SECTION_KIND_RDATA,
  COFF_SECTION_KIND_DATA,
  COFF_SECTION_KIND_BSS,
  COFF_SECTION_KIND_PDATA,
  COFF_SECTION_KIND_XDATA,
} CoffSectionKind;

typedef struct {
  uint32_t virtual_address;
  uint32_t symbol_table_index;
  uint16_t type;
} CoffRelocation;

typedef struct {
  char *name;
  CoffSectionKind kind;
  uint32_t virtual_size;
  uint32_t virtual_address;
  uint32_t size_of_raw_data;
  uint32_t pointer_to_raw_data;
  uint32_t pointer_to_relocations;
  uint32_t pointer_to_line_numbers;
  uint16_t number_of_relocations;
  uint16_t number_of_line_numbers;
  uint32_t characteristics;
  unsigned char *raw_data;
  CoffRelocation *relocations;
  size_t relocation_count;
} CoffSection;

typedef struct {
  uint32_t raw_index;
  char *name;
  uint32_t value;
  int16_t section_number;
  uint16_t type;
  uint8_t storage_class;
  uint8_t auxiliary_count;
  int is_auxiliary;
  uint32_t primary_symbol_index;
  int has_auxiliary_record;
  uint32_t aux_section_length;
  uint16_t aux_section_relocation_count;
  uint16_t aux_section_line_number_count;
} CoffSymbol;

typedef struct {
  uint16_t machine;
  uint16_t section_count;
  uint32_t time_date_stamp;
  uint32_t pointer_to_symbol_table;
  uint32_t symbol_count;
  uint16_t size_of_optional_header;
  uint16_t characteristics;
  CoffSection *sections;
  CoffSymbol *symbols;
  unsigned char *string_table;
  uint32_t string_table_size;
} CoffObject;

int coff_object_read(const char *filename, CoffObject **object_out,
                     char **error_message_out);
void coff_object_destroy(CoffObject *object);

const CoffSection *coff_object_find_section_by_kind(const CoffObject *object,
                                                    CoffSectionKind kind);
const CoffSymbol *coff_object_find_symbol(const CoffObject *object,
                                          const char *name);

CoffSectionKind coff_section_kind_from_name(const char *name);
const char *coff_section_kind_name(CoffSectionKind kind);
const char *coff_relocation_type_name(uint16_t type);

#endif // COFF_READER_H
