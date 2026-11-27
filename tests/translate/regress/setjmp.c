/* setjmp and longjmp.
 *
 * Everything the pair needs is already in kernel32: RtlCaptureContext writes
 * its CALLER's registers and resume point into a CONTEXT, RtlRestoreContext
 * puts them back. setjmp is a macro (C99 7.13 allows it), so the capture runs
 * in the function that wrote setjmp and records that function's own frame,
 * still live when longjmp arrives from deeper down.
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf env;
static jmp_buf inner;
static int trace[16];
static int ntrace;

static void mark(int v) {
  if (ntrace < 16)
    trace[ntrace++] = v;
}

static void deep(int n) {
  mark(n);
  if (n == 0)
    longjmp(env, 42);
  deep(n - 1);
  mark(100 + n); /* never reached */
}

/* volatile survives a longjmp; the rest is only read after it is set again */
static int counted(void) {
  volatile int calls = 0;
  int r = setjmp(env);
  calls++;
  if (r == 0 && calls < 4)
    longjmp(env, calls);
  return calls * 10 + r;
}

/* a longjmp with 0 has to arrive as 1 */
static int zero_becomes_one(void) {
  int r = setjmp(inner);
  if (r == 0)
    longjmp(inner, 0);
  return r;
}

/* the frame the jump lands in keeps its locals and its big buffers */
static int keeps_frame(void) {
  char buf[256];
  int guard = 0x5a5a;
  volatile int pass = 0;
  memset(buf, 'k', sizeof buf);
  buf[255] = '\0';
  if (setjmp(env) == 0) {
    pass = 1;
    longjmp(env, 3);
  }
  return (guard == 0x5a5a) && (buf[0] == 'k') && (buf[254] == 'k') && pass;
}

/* doubles live in xmm registers, which the context carries too */
static double keeps_float(void) {
  double a = 1.5, b = 2.25;
  if (setjmp(env) == 0)
    longjmp(env, 1);
  return a * b + 0.25;
}

int main(void) {
  int i, r;

  r = setjmp(env);
  if (r == 0) {
    deep(3);
    printf("UNREACHABLE\n");
  }
  printf("%d %d\n", r, ntrace);
  for (i = 0; i < ntrace; i++)
    printf("%s%d", i ? " " : "", trace[i]);
  printf("\n");

  printf("%d\n", counted());
  printf("%d\n", zero_becomes_one());
  printf("%d\n", keeps_frame());
  printf("%.2f\n", keeps_float());
  return 0;
}
