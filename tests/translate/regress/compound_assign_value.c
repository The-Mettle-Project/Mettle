/* A compound assignment used as a value, with a left side that has a side
 * effect. The left side runs once and the operator applies to the old value.
 */
#include <stdio.h>

static int calls;

static int pick(int i) {
  calls++;
  return i;
}

int main(void) {
  int a[4] = {1, 2, 3, 4};
  int i = 0;
  int v = (a[i++] += 5);
  int w = (a[pick(2)] *= 10);
  unsigned char bytes[2] = {250, 7};
  int x = (bytes[pick(0)] += 10);
  printf("%d %d %d %d\n", v, i, a[0], a[1]);
  printf("%d %d %d\n", w, a[2], calls);
  printf("%d %d\n", x, bytes[0]);
  return 0;
}
