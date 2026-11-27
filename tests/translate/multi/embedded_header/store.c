/* The library carries its header's text inline, the way an amalgamation does,
 * while main.c includes the header. Both units then declare the same structs,
 * enums and macros, and the public handle is complete only here.
 */
#include <stdlib.h>

#define API_OK 0
#define API_FULL 5
#define API_NAME "store"

typedef struct Store Store;

struct StoreConfig {
  int capacity;
  const char *label;
};

enum StoreMode { MODE_READ = 1, MODE_WRITE = 2 };

struct Store {
  struct StoreConfig config;
  int count;
  int values[16];
};

int store_open(const struct StoreConfig *config, Store **out) {
  Store *s = calloc(1, sizeof(Store));
  s->config = *config;
  *out = s;
  return API_OK;
}

int store_put(Store *s, int value) {
  if (s->count >= s->config.capacity) {
    return API_FULL;
  }
  s->values[s->count++] = value;
  return API_OK;
}

int store_sum(Store *s) {
  int total = 0;
  for (int i = 0; i < s->count; i++) {
    total += s->values[i];
  }
  return total;
}

enum StoreMode store_mode(Store *s) { return s->count ? MODE_WRITE : MODE_READ; }

void store_close(Store *s) { free(s); }
