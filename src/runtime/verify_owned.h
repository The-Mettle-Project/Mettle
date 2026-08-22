#ifndef METTLE_RUNTIME_VERIFY_OWNED_H
#define METTLE_RUNTIME_VERIFY_OWNED_H

#include <stddef.h>

/* Check a linked PE32+ or ELF64 executable for a C runtime dependency.
 * Returns 1 on success. On failure, reason receives a short diagnostic. */
int mettle_verify_owned_executable(const char *path, char *reason,
                                   size_t reason_size);

/* The same check against an image already in memory. Reopening a file that was
 * just written costs whatever the machine's virus scanner charges to inspect a
 * fresh executable -- on this machine, a third of a second for a 95 KB binary,
 * which was most of the link. Anything that emits the image itself should
 * verify the bytes it already has and never read them back. */
int mettle_verify_owned_image(const unsigned char *data, size_t size,
                              char *reason, size_t reason_size);

/* Reject explicit linker arguments that name a C or compiler runtime. */
int mettle_link_argument_uses_forbidden_runtime(const char *argument);

#endif
