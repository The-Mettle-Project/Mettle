/* Declarators read inside out, at every nesting a C program actually uses.
 *
 * The suffixes that follow a parenthesized declarator apply to the base type
 * FIRST, and what they produce is the type the declarator declares: `(void)`
 * turns int into "function returning int", and only then does `*fp` make a
 * pointer of it. Carrying only the pointer DEPTH across that boundary loses
 * everything else, which is why `int (*fp_arr[3])(void)` used to parse as a
 * bare function type: the array lived inside the parens and did not survive.
 *
 * `(*p)[i]` is here too. An array lvalue decays to a pointer, and the decay
 * rewrites the expression's type in place, so a deref that yields an address
 * has to decide from what it points at rather than from what it now claims to
 * be. It used to load eight bytes through the array and index off those.
 */
#include <stdio.h>

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int sub(int a, int b) { return a - b; }
static int zero(void) { return 7; }
static int one(void) { return 8; }
static int arr3[3] = {70, 80, 90};
static int grid[2][3] = {{1, 2, 3}, {4, 5, 6}};

typedef int (*binop)(int, int);
typedef int (*binop_arr[2])(int, int);

/* array of pointers to function */
static int (*fp_arr[3])(int, int) = {add, mul, sub};
static int (*noarg[2])(void) = {zero, one};

/* two dimensions of them */
static int (*two_d[2][2])(int, int) = {{add, mul}, {sub, add}};

/* pointer to an array of them */
static int (*(*pp)[3])(int, int);

/* a function returning a pointer to a function */
static int (*pick(int which))(int, int) { return fp_arr[which]; }

/* a function returning a pointer to an array */
static int (*rows(void))[3] { return &arr3; }

/* a parameter that is itself a function type, which adjusts to a pointer */
static int through(int fn(int, int), int x) { return fn(x, x); }

/* the typedef spellings of the same shapes */
static binop td_one[2] = {add, mul};
static binop_arr td_two = {sub, add};

int main(void) {
  int i, t;
  int (*local[3])(int, int);
  int (*p)[3] = &arr3;
  int (*g)[3] = grid;
  int (*(*lpp)[3])(int, int) = &fp_arr;
  int (*const cfp)(int, int) = add;
  binop b;

  t = 0;
  for (i = 0; i < 3; i++)
    t += fp_arr[i](6, 3);
  printf("%d\n", t);

  t = 0;
  for (i = 0; i < 2; i++)
    t += noarg[i]();
  printf("%d\n", t);

  printf("%d %d %d %d\n", two_d[0][0](2, 3), two_d[0][1](2, 3),
         two_d[1][0](2, 3), two_d[1][1](2, 3));

  local[0] = sub;
  local[1] = add;
  local[2] = mul;
  printf("%d %d %d\n", local[0](10, 4), local[1](10, 4), local[2](10, 4));

  pp = &fp_arr;
  printf("%d %d\n", (*pp)[0](5, 5), (*lpp)[1](5, 5));

  printf("%d %d %d\n", (*p)[0], (*p)[2], p[0][1]);
  printf("%d %d %d\n", (*g)[2], (*(g + 1))[0], g[1][2]);
  printf("%d\n", (*rows())[1]);

  printf("%d %d\n", pick(1)(4, 4), through(add, 12));
  printf("%d %d %d\n", td_one[1](3, 3), td_two[0](9, 4), cfp(1, 2));

  b = mul;
  printf("%d\n", b(6, 7));

  /* the same shapes as abstract declarators */
  printf("%d %d %d %d\n", (int)sizeof fp_arr, (int)sizeof(int (*[4])(void)),
         (int)sizeof(int (*)(void)), (int)sizeof(int (*(*)[3])(int, int)));
  printf("%d %d\n", (int)sizeof(int (*)[3]), (int)sizeof(int[3]));
  printf("%d\n", ((binop)sub)(50, 8));

  /* A pointer to an array whose bound is only known at run time. The bound
     lives inside the parens, so it has to reach the declaration that owns it
     rather than being dropped on the way back out. */
  {
    int n = 3, j;
    int vla[2][3];
    int (*q)[n] = vla;
    t = 0;
    for (i = 0; i < 2; i++)
      for (j = 0; j < n; j++)
        q[i][j] = i * 10 + j;
    for (i = 0; i < 2; i++)
      for (j = 0; j < n; j++)
        t += q[i][j];
    printf("%d %d %d %d\n", t, (int)sizeof(*q), (*q)[2], q[1][1]);
  }
  return 0;
}
