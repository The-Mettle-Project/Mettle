/* A compound literal zeroes what its initializer does not mention.
 *
 * C99 6.7.8p21: a member no initializer reaches is initialized as an object
 * with static storage duration, which means zero. The storage is a fresh stack
 * slot, and it was never cleared, so `(T){0}` left every member past the first
 * holding whatever the frame last used that slot for. Mettle's own lowering
 * resets an IR instruction that way between emits; the stale operand pointer
 * it kept crashed the compiler on `a && b`.
 *
 * The dirty() call plants a known pattern so the slot cannot be clean by luck.
 */
#include <stdio.h>

typedef struct {
  int kind;
  char *name;
  long long v;
} Op;

typedef struct {
  int op;
  Op lhs, rhs, dest;
  char *text;
  int loc[6];
  double d;
  char pad[64];
} Ins;

static int nonzero(const void *p, unsigned n) {
  const unsigned char *b = (const unsigned char *)p;
  unsigned i, c = 0;
  for (i = 0; i < n; i++)
    if (b[i])
      c++;
  return (int)c;
}

static void dirty(void) {
  volatile unsigned char junk[sizeof(Ins) * 4];
  unsigned i;
  for (i = 0; i < sizeof junk; i++)
    junk[i] = (unsigned char)(i | 1);
}

static int used(const Ins *p) { return p->op + p->lhs.kind + nonzero(p, sizeof *p); }

int main(void) {
  Ins ins;
  int a, b, c;

  dirty();
  ins = (Ins){0};
  a = nonzero(&ins, sizeof ins);

  ins.op = 7;
  ins.lhs.kind = 2;
  ins.lhs.name = "x";
  ins.rhs.kind = 3;
  ins.dest.v = 99;
  ins.text = "t";
  ins.loc[5] = 5;
  ins.d = 1.5;
  ins.pad[40] = 'z';

  dirty();
  ins = (Ins){0};
  b = nonzero(&ins, sizeof ins);

  /* only the named member survives */
  dirty();
  ins = (Ins){.op = 4};
  c = nonzero(&ins, sizeof ins);

  printf("%d %d %d\n", a, b, c);
  printf("%d %p %d %d\n", ins.lhs.kind, (void *)ins.lhs.name, ins.rhs.kind,
         (int)ins.dest.v);
  printf("%d\n", used(&ins));
  return 0;
}
