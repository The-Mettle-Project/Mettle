#include "common.h"
#include "string_intern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *mettle_strdup(const char *text) {
  if (!text) {
    return NULL;
  }
  size_t length = strlen(text) + 1;
  char *copy = malloc(length);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, length);
  return copy;
}

size_t mettle_fnv1a_hash(const char *str) {
  size_t hash = METTLE_FNV1A_OFFSET_BASIS;
  for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
    hash ^= (size_t)*p;
    hash *= METTLE_FNV1A_PRIME;
  }
  return hash;
}

void mettle_set_error(char **dest, const char *fmt, ...) {
  char buffer[512];
  va_list args;

  if (!dest) {
    return;
  }

  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  char *copy = mettle_strdup(buffer);
  if (!copy) {
    return;
  }

  free(*dest);
  *dest = copy;
}

/* Free a string unless it is interned (shared and managed by the interner). */
void mettle_free_string(char *str) {
  if (!str) {
    return;
  }
  if (!string_is_interned(str)) {
    free(str);
  }
}

void mettle_free_string_array(char **values, size_t count) {
  if (!values) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(values[i]);
  }
  free(values);
}
