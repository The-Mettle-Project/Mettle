#ifndef MTLC_FLAT_EMITTER_H
#define MTLC_FLAT_EMITTER_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "binary_emitter.h"

int binary_emitter_write_flat(BinaryEmitter *emitter, const char *path,
                              uint64_t image_base, const char *entry_symbol,
                              size_t pad_to, unsigned char pad_byte,
                              const unsigned char *trailer,
                              size_t trailer_size, char *error,
                              size_t error_size);

#endif
