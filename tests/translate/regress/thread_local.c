/* `__thread` and `_Thread_local` parse as a storage class.
 *
 * libmtlc has no thread-local storage, so the object is an ordinary global
 * and every thread shares it. That is right for a single-threaded program and
 * wrong for any other, so the compiler warns (-Wno-thread-local silences it).
 * This test only pins the parse and the single-threaded behaviour, which is
 * what gcc agrees with here.
 */
#include <stdio.h>

static __thread int counter = 5;
static __thread const char *name;
_Thread_local int shared;
__thread struct P {
  int x;
  int y;
} point = {1, 2};

static int bump(void) {
  static __thread int calls;
  calls++;
  return calls;
}

int main(void) {
  counter += 2;
  name = "ok";
  shared = 9;
  point.x += 10;
  printf("%d %s %d\n", counter, name, shared);
  printf("%d %d\n", point.x, point.y);
  /* one per statement: argument evaluation order is unspecified */
  printf("%d ", bump());
  printf("%d ", bump());
  printf("%d\n", bump());
  return 0;
}
