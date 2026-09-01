#ifndef METTLE_ELF_IMAGE_H
#define METTLE_ELF_IMAGE_H

#include "linker/symbol_resolve.h"

#include <stdint.h>

typedef struct {
  uint64_t image_base;
  uint64_t page_size;
  int strip_symbols;
  /* Path of the program loader written into PT_INTERP. NULL leaves the image
   * self-contained; it is required as soon as a shared library supplies a
   * symbol, because nothing else can bind one at run time. */
  const char *interpreter;
  /* Emit ET_DYN with a DT_SONAME rather than a program. */
  int produce_shared_library;
  /* Publish the program's own globals in .dynsym so a library it loads can
     bind back to them, the way -rdynamic does. */
  int export_dynamic;
  const char *soname;
  const char *const *runpaths;
  size_t runpath_count;
} ElfImageOptions;

int elf_image_emit_executable(LinkResolution *resolution,
                              const char *output_path,
                              const ElfImageOptions *options,
                              char **error_message_out);

#endif
