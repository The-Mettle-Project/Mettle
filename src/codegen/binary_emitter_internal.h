#ifndef BINARY_EMITTER_INTERNAL_H
#define BINARY_EMITTER_INTERNAL_H

/* Shared internals between the format-neutral emitter core (binary_emitter.c)
 * and the per-format object writers (COFF in the same file, ELF in
 * elf_emitter.c). The public BinaryEmitter struct lives in binary_emitter.h;
 * these are the helpers a format writer needs that are not part of the public
 * API. */

#include "binary_emitter.h"

/* A symbol whose section_index equals this is undefined/external (no defining
 * section). Mirrors BINARY_SECTION_INDEX_NONE used inside binary_emitter.c. */
#define BINARY_EMITTER_SECTION_INDEX_NONE ((size_t)-1)

/* Records a human-readable failure on the emitter, replacing any prior one.
 * Defined in binary_emitter.c. */
void binary_emitter_record_error(BinaryEmitter *emitter, const char *message);

/* Finds a symbol's index by name using the emitter's hash index, or -1.
 * Defined in binary_emitter.c. */
int binary_emitter_lookup_symbol_index(const BinaryEmitter *emitter,
                                       const char *name);

/* Serializes the emitter's sections/symbols/relocations as a relocatable
 * ELF64 (x86-64 or AArch64) object. Defined in elf_emitter.c. */
int binary_emitter_write_elf_object_file(BinaryEmitter *emitter,
                                         const char *filename);

#endif /* BINARY_EMITTER_INTERNAL_H */
