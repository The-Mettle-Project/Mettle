#ifndef STRING_INTERN_H
#define STRING_INTERN_H

#include <stddef.h>

const char *string_intern(const char *value);
const char *string_intern_n(const char *value, size_t length);
int string_is_interned(const char *value);
void string_intern_clear(void);

#endif // STRING_INTERN_H
