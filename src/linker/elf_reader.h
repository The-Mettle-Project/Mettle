#ifndef METTLE_ELF_READER_H
#define METTLE_ELF_READER_H

#include "linker/link_object.h"

int elf_object_read(const char *filename, LinkObject **object_out,
                    char **error_message_out);
int elf_object_read_memory(const unsigned char *data, size_t size,
                            const char *origin, LinkObject **object_out,
                            char **error_message_out);

#endif