/* A volatile object lives in memory and is read back from it every time.
 *
 * The qualifier used to be parsed and dropped, so a volatile local was a value
 * handle like any other and the backend was free to keep it in a register.
 * That is wrong twice over: an optimizer may fold reads it must repeat, and
 * the value cannot survive a longjmp, which restores every register and
 * touches nothing on the stack (C99 7.13.2.1p3).
 */
#include <setjmp.h>
#include <stdio.h>

static jmp_buf env;

/* the loop must reload each time, so the count is what the writes say */
static int reload(void) {
  volatile int v = 0;
  int i, seen = 0;
  for (i = 0; i < 5; i++) {
    v = i;
    seen += v;
  }
  return seen;
}

/* a volatile local keeps what it was given when the jump lands */
static int survives(void) {
  volatile int calls = 0;
  int r = setjmp(env);
  calls++;
  if (r == 0 && calls < 4)
    longjmp(env, calls);
  return calls * 10 + r;
}

static int frame_intact(void) {
  volatile int pass = 0;
  char buf[128];
  int guard = 0x5a5a;
  buf[0] = 'z';
  if (setjmp(env) == 0) {
    pass = 1;
    longjmp(env, 3);
  }
  return pass && guard == 0x5a5a && buf[0] == 'z';
}

static volatile int gv = 5;
static volatile double gd = 1.25;

int main(void) {
  volatile int a = 1;
  volatile char c = 'q';
  volatile double d = 2.5;
  volatile unsigned u = 4000000000u;
  volatile long long big = -1234567890123LL;
  volatile int *p = &a;

  printf("%d %d %d\n", reload(), survives(), frame_intact());
  printf("%d %c %.2f %u\n", a, c, d, u);
  printf("%lld %d %d\n", big, gv, *p);
  a = a + 41;
  gv = gv * 2;
  gd = gd + 0.75;
  printf("%d %d %.2f\n", a, gv, gd);
  printf("%d\n", (int)sizeof a + (int)sizeof d);
  return 0;
}
