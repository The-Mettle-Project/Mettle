/* A static-duration initializer that is not a bare integer literal.
 *
 * `-1` is a negation of 1, not the integer -1, and the declaration path only
 * knew how to read a literal. Anything else silently became 0: every negative
 * global, every `1 << 10`, every enum constant. Mettle's PGO hot threshold is
 * `static long long cached = -1;` guarding a lazy init, so it read back 0,
 * reported "hot threshold 0 calls" and treated every function as hot.
 */
#include <stdio.h>

enum { E_A = 3, E_B = 40 };

static int neg = -5;
static long long big = -1;
int wide = -7;
static int shifted = 1 << 10;
static int summed = E_A + E_B;
static unsigned int mask = ~0u;
static char ch = 'Z';
static int paren = -(2 + 3);
static double dneg = -1.5;
static const char *label = "text";
static int arr[4] = {-1, 2, -3, 4};

static int counter(void) {
  static int n = -5;
  n++;
  return n;
}

static long long threshold(void) {
  static long long cached = -1;
  if (cached < 0) {
    cached = 1024;
    if (cached < 1)
      cached = 1;
  }
  return cached;
}

int main(void) {
  int a, b, c;
  printf("%d %lld %d %d\n", neg, big, wide, shifted);
  printf("%d %u %d %d\n", summed, mask, (int)ch, paren);
  printf("%.1f %s\n", dneg, label);
  printf("%d %d %d %d\n", arr[0], arr[1], arr[2], arr[3]);
  /* one per statement: argument evaluation order is unspecified */
  a = counter();
  b = counter();
  c = counter();
  printf("%d %d %d\n", a, b, c);
  printf("%lld %lld\n", threshold(), threshold());
  return 0;
}
