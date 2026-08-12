#ifndef RELOCATION_H
#define RELOCATION_H

#include "linker/symbol_resolve.h"

#include <stdint.h>

typedef struct {
  uint64_t image_base;
} LinkRelocationOptions;

int link_apply_relocations(LinkResolution *resolution,
                           const LinkRelocationOptions *options,
                           char **error_message_out);

#endif // RELOCATION_H
