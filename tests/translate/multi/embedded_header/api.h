#ifndef API_H
#define API_H

#define API_OK 0
#define API_FULL 5
#define API_NAME "store"

typedef struct Store Store;

struct StoreConfig {
  int capacity;
  const char *label;
};

enum StoreMode { MODE_READ = 1, MODE_WRITE = 2 };

int store_open(const struct StoreConfig *config, Store **out);
int store_put(Store *s, int value);
int store_sum(Store *s);
enum StoreMode store_mode(Store *s);
void store_close(Store *s);

#endif
