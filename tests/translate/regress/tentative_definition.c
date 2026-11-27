/* Tentative definitions, C99 6.9.2.
 *
 * At file scope a declaration with no initializer and no `extern` is a
 * tentative definition. It may be written as often as you like; only a second
 * initializer is a redefinition, and if no declaration ever carries one the
 * object exists anyway, zeroed. The check used to reject the second
 * declaration outright, which is three of the errors SQLite's amalgamation
 * hit and c-testsuite 00094 through 00096.
 */
#include <stdio.h>

int x;
int x = 3;

int y;
int y;
int y;

static int s;
static int s = 7;

struct P {
  int a, b;
};
struct P p;
struct P p = {4, 5};

int arr[4];
int arr[4] = {1, 2, 3, 4};

/* the initializer first, the tentative declaration after */
double d = 1.5;
double d;

/* a tentative definition whose bound arrives with the initializer */
int late[];
int late[3] = {10, 20, 30};

/* extern and a definition still agree */
extern int e;
int e = 11;

const char *msg;
const char *msg = "set";

int main(void) {
  printf("%d %d %d\n", x, y, s);
  printf("%d %d %d\n", p.a, p.b, arr[3]);
  printf("%.1f %d %d\n", d, late[2], e);
  printf("%s %d\n", msg, (int)sizeof late);
  return 0;
}
