/* __builtin_ctz/clz/popcount and __func__.
 *
 * The Mettle register allocator walks a bitset with __builtin_ctzll and sizes
 * a register pool with __builtin_popcount, so the self-host build stopped
 * dead until these existed. popcount used to be a preprocessor macro that
 * evaluated its argument eight times; all six are one call each now, which is
 * what `side` checks.
 *
 * Zero is never passed: ctz(0) and clz(0) are undefined for gcc, so the
 * oracle has no answer to agree with.
 */
#include <stdio.h>
#include <string.h>

static int calls;

static unsigned long long once(void) {
  calls++;
  return 0x100ULL;
}

int main(void) {
  unsigned long long w = 0x8000000100000010ULL;
  unsigned n = 0x80u;
  int side;

  printf("%d %d %d\n", __builtin_ctz(n), __builtin_clz(n), __builtin_popcount(n));
  printf("%d %d %d\n", __builtin_ctzll(w), __builtin_clzll(w), __builtin_popcountll(w));
  printf("%d %d\n", __builtin_ctzll(1ULL), __builtin_clzll(1ULL));
  printf("%d %d\n", __builtin_ctzll(1ULL << 63), __builtin_clzll(1ULL << 63));
  printf("%d %d\n", __builtin_ctz(1u), __builtin_clz(1u));
  printf("%d %d\n", __builtin_popcount(0xffffffffu), __builtin_popcountll(~0ULL));

  /* the high-bit index the allocator computes */
  printf("%d %d\n", 63 - __builtin_clzll(1000ULL), 63 - __builtin_clzll(~0ULL));

  calls = 0;
  side = __builtin_ctzll(once());
  printf("%d %d\n", side, calls);

  printf("%s %d\n", __func__, (int)sizeof(__func__));
  printf("%d\n", strcmp(__func__, "main"));
  return 0;
}
