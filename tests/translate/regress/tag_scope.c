/* Struct/union/enum tags are scoped to their block. The tag table used to be
   flat, so an inner `struct T { int y; }` overwrote the members of the
   file-scope `struct T` and every earlier use of it lost its fields. */
#include <stdio.h>

struct T {
  int x;
};
struct node {
  int v;
  struct node *next;
};
enum E { A = 1 };

static void inner(void) {
  struct T {
    int y;
  } a;
  a.y = 7;
  printf("%d %d\n", a.y, A);
}

static void outer(void) {
  struct T b;
  b.x = 9;
  printf("%d %d\n", b.x, A);
}

int main(void) {
  struct node n2 = {2, 0};
  struct node n1 = {1, &n2};
  struct T c;

  inner();
  outer();
  printf("%d %d\n", n1.v, n1.next->v);

  c.x = 3;
  {
    struct T d;
    d.x = 4;
    printf("%d %d\n", c.x, d.x);
  }
  return 0;
}
