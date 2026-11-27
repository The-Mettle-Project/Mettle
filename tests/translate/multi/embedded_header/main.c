#include <stdio.h>
#include "api.h"

int main(void) {
  struct StoreConfig config = {3, API_NAME};
  Store *s;
  int rc = store_open(&config, &s);
  printf("%s open %d mode %d\n", config.label, rc, store_mode(s));
  for (int i = 1; i <= 4; i++) {
    printf("put %d -> %d\n", i * 10, store_put(s, i * 10));
  }
  printf("sum %d mode %d full %d\n", store_sum(s), store_mode(s), API_FULL);
  store_close(s);
  return 0;
}
