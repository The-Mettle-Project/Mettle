#include "string_intern.h"
#include "common.h"

int string_is_interned(const char *value) {
  (void)value;
  return 0;
}

const char *string_intern(const char *value) {
  return mettle_strdup(value);
}
