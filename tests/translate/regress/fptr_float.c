/* Indirect calls with floating arguments and returns. Without a typed
   function-pointer descriptor, libmtlc classified every argument as integer
   and read the result from RAX, so any float in or out came back 0. */
#include <stdio.h>

static double addd(double a, double b) { return a + b; }
static double i2d(int a, int b) { return (double)(a + b); }
static int d2i(double a, double b) { return (int)(a + b); }
static float addf(float a, float b) { return a + b; }

int main(void) {
  double (*p1)(double, double) = addd;
  double (*p2)(int, int) = i2d;
  int (*p3)(double, double) = d2i;
  float (*p4)(float, float) = addf;
  const void *v = (const void *)addd;

  printf("%d\n", (int)p1(3.0, 4.0));
  printf("%d\n", (int)p2(3, 4));
  printf("%d\n", p3(3.0, 4.0));
  printf("%d\n", (int)(p4(1.25f, 2.25f) * 2));
  printf("%d\n", (int)((double (*)(double, double))v)(5.0, 6.0));
  return 0;
}
