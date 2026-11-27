/* Arguments convert to their parameter types for every prototyped call
   (C99 6.5.2.2p7), not just extern ones. `f(1920)` into a double parameter
   used to pass the integer's bit pattern, which printed as 9.5e-321 in
   cJSON's test program. */
#include <stdio.h>

struct S {
  int a, b;
};

static double takes_double(double d) { return d * 2; }
static int takes_int(double x, int i) { return i + (int)x; }
static struct S takes_mixed(double d, int i) {
  struct S s;
  s.a = (int)d;
  s.b = i;
  return s;
}

int main(void) {
  printf("%d\n", (int)takes_double(1920));
  printf("%d\n", (int)takes_double('A'));
  printf("%d\n", takes_int(3, 4));
  struct S s = takes_mixed(7, 8);
  printf("%d %d\n", s.a, s.b);
  return 0;
}
