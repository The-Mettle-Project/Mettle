/* A float == or != anywhere in a function used to corrupt every float32
   local in it: the whole function fell back to the legacy emitter, and a
   cast built through mtlc/build.h carried the TARGET type where the emitter
   expected the SOURCE, so a float32 value read back as its raw bits. */
#include <stdio.h>

int main(void) {
  float a = 69.12f;
  float b = 1.5f, c = 2.5f;
  double p = 1.5, q = 2.5;
  int r1 = (b == c);
  int r2 = (b != c);
  int r3 = (p == q);

  printf("%d %d %d\n", r1, r2, r3);
  printf("%d %d\n", (int)a, (int)(a * 100));
  printf("%d\n", (int)((double)a * 1000));
  return 0;
}
