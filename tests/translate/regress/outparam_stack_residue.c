/* An address-taken narrow local written only through the callee's pointer.
 * The callee stores 4 bytes; the caller's local is memory-resident because
 * its address is taken. The MIR encoder used to read the whole 8-byte home,
 * so whatever the stack held above the int decided the branch: printf gave
 * every positive double a minus sign whenever the frame was dirty. The
 * function-pointer call keeps the callee out of the inliner, and the spray
 * guarantees the residue is there instead of hoping. */
#include <stdio.h>

typedef void (*unpack_fn)(double, int *, unsigned long long *, int *);

static void unpack(double x, int *neg, unsigned long long *m, int *e2) {
  unsigned long long bits;
  unsigned char *p = (unsigned char *)&x;
  int i;
  bits = 0;
  for (i = 7; i >= 0; i--)
    bits = (bits << 8) | p[i];
  *neg = (int)(bits >> 63);
  *m = bits & 0xFFFFFFFFFFFFFull;
  *e2 = (int)((bits >> 52) & 0x7FF) - 1075;
}

static unpack_fn up = unpack;

static void spray(void) {
  volatile unsigned long long junk[512];
  int i;
  for (i = 0; i < 512; i++)
    junk[i] = 0xFFFFFFFFFFFFFFFFull;
}

static int probe(double x) {
  /* the emit_float shape: big frame, narrow out-params, then a branch */
  char dig[800];
  int neg;
  unsigned long long m;
  int e2;
  up(x, &neg, &m, &e2);
  dig[0] = (char)m;
  dig[1] = (char)e2;
  if (neg)
    return 1;
  return dig[0] == dig[0] ? 0 : 2;
}

int main(void) {
  spray();
  printf("%d %d\n", probe(6.75), probe(-6.75));
  spray();
  printf("%.1f %.1f %.1f\n", 6.75, -6.75, 0.0);
  return 0;
}
