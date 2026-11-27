/* An unsized array compound literal takes its bound from the initializer.
   The bound used to stay 0, the literal got one 8-byte slot, and every
   element past the first landed outside its storage (tinyexpr's NEW_EXPR). */
#include <stdio.h>

static int sum(const int *arr[], int n) {
  int t = 0;
  for (int i = 0; i < n; i++)
    t += *arr[i];
  return t;
}

int main(void) {
  int a = 3, b = 4, c = 5;
  const int **q = (const int *[]){&a, &b, &c};
  int *p = (int[]){10, 20, 30};

  printf("%d %d %d\n", *q[0], *q[1], *q[2]);
  printf("%d %d %d\n", p[0], p[1], p[2]);
  printf("%d\n", sum((const int *[]){&a, &b, &c}, 3));
  return 0;
}
