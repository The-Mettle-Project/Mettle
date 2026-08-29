#include "common.h"
#include "string_intern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
typedef long long MettleClockTicks;
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(MettleClockTicks *frequency);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(MettleClockTicks *counter);
#else
#include <time.h>
#endif

double mettle_now_ms(void) {
#if defined(_WIN32)
  static MettleClockTicks frequency = 0;
  MettleClockTicks counter = 0;

  if (frequency == 0 && !QueryPerformanceFrequency(&frequency)) {
    return 0.0;
  }
  if (frequency == 0) {
    return 0.0;
  }
  QueryPerformanceCounter(&counter);
  return (double)counter * 1000.0 / (double)frequency;
#else
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

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
