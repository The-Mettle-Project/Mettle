/* Variably modified types: an array bound that is only known at run time.
 *
 * A row of `int a[n][m]` is m*4 bytes wide, a number the type cannot hold, so
 * every stride through such an object has to be computed. Getting it wrong is
 * silent: `int a[3][n]` used to step by one element instead of one row and
 * summed the wrong memory without complaint.
 *
 * Covered here: a VLA parameter in each dimension position, three dimensions,
 * a pointer to a VLA (arithmetic, subtraction, and indexing), sizeof on a VLA
 * object and on one of its rows, and a VLA local declared from an expression.
 */
#include <stdio.h>
#include <string.h>

static int sum2(int r, int c, int a[r][c]) {
  int i, j, t = 0;
  for (i = 0; i < r; i++)
    for (j = 0; j < c; j++)
      t += a[i][j];
  return t;
}

/* the outer bound fixed, the inner one dynamic: the case that miscompiled */
static int sum_inner(int n, int a[3][n]) {
  int i, t = 0;
  for (i = 0; i < 3; i++)
    t += a[i][0];
  return t;
}

/* the outer bound dynamic, the inner one fixed: a constant stride */
static int sum_outer(int n, int a[n][3]) {
  int i, t = 0;
  for (i = 0; i < n; i++)
    t += a[i][0];
  return t;
}

/* an unspecified outer bound still decays to a pointer to a VLA */
static int elem(int n, int a[][n], int i, int j) { return a[i][j]; }

static int sum3(int x, int y, int z, int a[x][y][z]) {
  int i, j, k, t = 0;
  for (i = 0; i < x; i++)
    for (j = 0; j < y; j++)
      for (k = 0; k < z; k++)
        t += a[i][j][k];
  return t;
}

static long rows_between(int c, int (*p)[c], int (*q)[c]) { return (long)(q - p); }

int main(void) {
  int r = 2, c = 3, i, j, k, n = 0;
  int a[r][c];
  int b[2][3][4];
  int fixed[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};

  for (i = 0; i < r; i++)
    for (j = 0; j < c; j++)
      a[i][j] = ++n;
  n = 0;
  for (i = 0; i < 2; i++)
    for (j = 0; j < 3; j++)
      for (k = 0; k < 4; k++)
        b[i][j][k] = ++n;

  printf("%d %d\n", sum2(r, c, a), sum3(2, 3, 4, b));
  printf("%d %d %d\n", sum_inner(3, fixed), sum_outer(3, fixed), elem(3, fixed, 2, 1));
  printf("%d %d\n", (int)sizeof a, (int)sizeof a[0]);
  printf("%ld\n", rows_between(c, &a[0], &a[1]));

  /* sizeof has to be right for this to clear the whole object */
  memset(a, 0, sizeof a);
  n = 0;
  for (i = 0; i < r; i++)
    for (j = 0; j < c; j++)
      n += a[i][j];
  printf("%d\n", n);

  {
    int (*p)[c] = a;
    p[1][2] = 77;
    printf("%d %d\n", a[1][2], (int)((char *)(p + 1) - (char *)p));
  }

  /* a bound that is an expression, read once where the declaration is */
  {
    int m = c * 2;
    int wide[r][m];
    for (i = 0; i < r; i++)
      for (j = 0; j < m; j++)
        wide[i][j] = i * 100 + j;
    m = 1; /* changing it afterwards must not change the object */
    printf("%d %d %d\n", wide[1][5], (int)sizeof wide, (int)sizeof wide[0]);
  }
  return 0;
}
