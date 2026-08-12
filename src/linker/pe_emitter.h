#ifndef PE_EMITTER_H
#define PE_EMITTER_H

#include "linker/symbol_resolve.h"

#include <stdint.h>

typedef struct {
  uint64_t image_base;
  uint32_t section_alignment;
  uint32_t file_alignment;
  uint16_t subsystem;
  const char **import_library_paths;
  size_t import_library_count;
  const char **import_dll_names;
  size_t import_dll_count;
} PeEmissionOptions;

int pe_emit_executable(LinkResolution *resolution, const char *output_path,
                       const PeEmissionOptions *options,
                       char **error_message_out);

#endif // PE_EMITTER_H
