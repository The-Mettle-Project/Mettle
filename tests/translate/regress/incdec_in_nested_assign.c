/* An increment inside an assignment that is itself inside a larger
 * expression. The pointer moves once per increment, the way SQLite's varint
 * reader walks a cell: iKey = (iKey << 7) ^ (x = *++p).
 */
#include <stdio.h>

static unsigned long long read_key(const unsigned char *p, int *used) {
  const unsigned char *start = p;
  unsigned long long key = *p;
  unsigned char x;
  if (key >= 0x80) {
    key = (key << 7) ^ (x = *++p);
    if (x >= 0x80) {
      key = (key << 7) ^ 0x10204000 ^ (x = *++p);
    } else {
      key ^= 0x4000;
    }
  }
  *used = (int)(p - start) + 1;
  return key;
}

int main(void) {
  const unsigned char one[] = {0x05};
  const unsigned char two[] = {0x81, 0x02};
  const unsigned char three[] = {0x81, 0x82, 0x03};
  int used;
  unsigned long long k = read_key(one, &used);
  printf("%llu %d\n", k, used);
  k = read_key(two, &used);
  printf("%llu %d\n", k, used);
  k = read_key(three, &used);
  printf("%llu %d\n", k, used);
  int a[4] = {10, 20, 30, 40};
  int *q = a;
  int y;
  int z = (y = *++q) * 2 + 1;
  z += (y = *q++);
  printf("%d %d %d\n", z, y, (int)(q - a));
  return 0;
}
