/* Conversions into float32. The backend marked a cast's destination as
   floating only when the target was float64, so (float)i and (float)d took
   the integer path and the slot read back as a denormal. */
#include <stdio.h>

int main(void) {
  int i = 4;
  double d = 4.5;
  float f1 = i;
  float f2 = d;
  float f3 = (float)(double)i;
  float a = 3.25f;
  float p = a * i;
  double back = f2;

  printf("%d %d %d\n", (int)f1, (int)f2, (int)f3);
  printf("%d %d\n", (int)(p * 4), (int)(back * 2));
  printf("%d %d\n", (int)(a * 4), a < 1);
  return 0;
}
