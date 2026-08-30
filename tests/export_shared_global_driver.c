/* Drives tests/export_shared_global.mettle from C: writes the exported globals
 * and asks Mettle what it sees. Prints one line per check so a failure names
 * which one. */
#include <stdio.h>

extern long long SHARED;
extern int SHARED_COUNT;
extern double SHARED_FLOAT;

long long get_shared(void);
int get_count(void);
double get_float(void);
long long get_private(void);
long long add_shared(long long v);
long long bump_shared(void);

int main(void) {
  int failures = 0;

  if (get_shared() != 7) { printf("initial int wrong: %lld\n", get_shared()); failures++; }
  if (get_count() != 3) { printf("initial count wrong: %d\n", get_count()); failures++; }
  if (get_float() != 1.5) { printf("initial float wrong: %f\n", get_float()); failures++; }
  if (get_private() != 41) { printf("private wrong: %lld\n", get_private()); failures++; }

  SHARED = 99;
  if (get_shared() != 99) { printf("write not seen: %lld\n", get_shared()); failures++; }
  if (add_shared(1) != 100) { printf("write not seen in arithmetic: %lld\n", add_shared(1)); failures++; }

  SHARED_COUNT = 55;
  if (get_count() != 55) { printf("count write not seen: %d\n", get_count()); failures++; }

  SHARED_FLOAT = 2.5;
  if (get_float() != 2.5) { printf("float write not seen: %f\n", get_float()); failures++; }

  /* A write from the Mettle side is visible here too. */
  if (bump_shared() != 100) { printf("bump wrong: %lld\n", SHARED); failures++; }
  if (SHARED != 100) { printf("bump not visible in C: %lld\n", SHARED); failures++; }

  if (failures == 0) {
    printf("ok\n");
  }
  return failures;
}
