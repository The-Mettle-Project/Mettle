#ifndef LINK_UNRESOLVED_HINT_H
#define LINK_UNRESOLVED_HINT_H

#include "linker/symbol_resolve.h"

void link_unresolved_format(const LinkResolution *resolution,
                            const char *symbol_name,
                            char **error_message_out);

int link_unresolved_format_all(const LinkResolution *resolution,
                               int skip_shared_imports,
                               char **error_message_out);

#endif
