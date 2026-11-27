/* A member of a struct that is not an lvalue.
 *
 * C99 6.5.2.3 allows `f().a` even though `f()` is not an lvalue. Lowering
 * used to reject it, because it looked for an lvalue address and a call is
 * not one. An expression of aggregate type already evaluates to the address
 * of its storage here, so the temporary the callee filled in is addressable
 * and the member access needs nothing else.
 */
#include <stdio.h>

struct S {
  int a;
  double b;
};

struct Big {
  int v[8];
};

static struct S mk(int x) {
  struct S s;
  s.a = x;
  s.b = x * 1.5;
  return s;
}

static struct Big mkbig(int x) {
  struct Big b;
  int i;
  for (i = 0; i < 8; i++)
    b.v[i] = x + i;
  return b;
}

int main(void) {
  int c = 1;
  printf("%d %.1f\n", mk(4).a, mk(4).b);
  printf("%d %d\n", mkbig(10).v[0], mkbig(10).v[7]);
  printf("%d\n", c ? mk(2).a : mk(3).a);
  printf("%.1f\n", mk(mk(2).a).b);
  return 0;
}
