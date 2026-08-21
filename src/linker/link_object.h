#ifndef METTLE_LINK_OBJECT_H
#define METTLE_LINK_OBJECT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  LINK_FORMAT_COFF = 0,
  LINK_FORMAT_ELF,
} LinkObjectFormat;

typedef enum {
  LINK_SECTION_KIND_UNKNOWN = 0,
  LINK_SECTION_KIND_TEXT,
  LINK_SECTION_KIND_RDATA,
  LINK_SECTION_KIND_DATA,
  LINK_SECTION_KIND_BSS,
  LINK_SECTION_KIND_TLS,
  LINK_SECTION_KIND_PDATA,
  LINK_SECTION_KIND_XDATA,
} LinkSectionKind;

typedef enum {
  LINK_RELOC_NONE = 0,
  LINK_RELOC_ABS64,
  LINK_RELOC_PC32,
  LINK_RELOC_ABS32,
  LINK_RELOC_IMAGE_REL32,
  LINK_RELOC_SECREL32,
  LINK_RELOC_TPOFF32,
  LINK_RELOC_GOTPCREL32,
} LinkRelocKind;

typedef struct {
  uint64_t offset;
  uint32_t symbol_index;
  LinkRelocKind kind;
  int64_t addend;
  int addend_is_explicit;
  uint32_t format_type;
} LinkReloc;

typedef struct {
  char *name;
  LinkSectionKind kind;
  uint64_t virtual_size;
  uint64_t size_of_raw_data;
  uint64_t alignment;
  unsigned char *raw_data;
  LinkReloc *relocations;
  size_t relocation_count;
  int is_metadata;
} LinkSection;

typedef struct {
  char *name;
  uint64_t value;
  int64_t section_index;
  int is_external;
  int is_defined;
  int is_auxiliary;
  int is_weak;
  int is_common;
} LinkSymbol;

#define LINK_SECTION_INDEX_UNDEFINED ((int64_t)-1)
#define LINK_SECTION_INDEX_ABSOLUTE  ((int64_t)-2)
#define LINK_SECTION_INDEX_COMMON    ((int64_t)-3)

typedef struct {
  LinkObjectFormat format;
  LinkSection *sections;
  size_t section_count;
  LinkSymbol *symbols;
  size_t symbol_count;
  unsigned char *string_table;
  size_t string_table_size;
} LinkObject;

void link_object_destroy(LinkObject *object);

const LinkSection *link_object_find_section_by_kind(const LinkObject *object,
                                                    LinkSectionKind kind);
const LinkSymbol *link_object_find_symbol(const LinkObject *object,
                                          const char *name);

const char *link_section_kind_name(LinkSectionKind kind);
const char *link_reloc_kind_name(LinkRelocKind kind);

int link_object_read(const char *filename, LinkObject **object_out,
                     char **error_message_out);

int link_object_read_file(const char *filename, unsigned char **data_out, 
                            size_t *size_out, char **error_message_out);

int link_object_range_ok(size_t file_size, uint64_t offset, uint64_t length);

#endif /* METTLE_LINK_OBJECT_H */
