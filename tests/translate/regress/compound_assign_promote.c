/* E1 op= E2 computes at the usual-arithmetic type, then converts back
   (C99 6.5.16.2p3). float f; f += 0.5; must add in double, or it rounds
   twice and drifts from the oracle. */
#include <stdio.h>

int main(void) {
  float a = 12.34f;
  a += 56.78;
  printf("%d\n", (int)(a * 1000000.0f));

  float m = 12.34f;
  m *= 56.78;
  printf("%d\n", (int)(m * 100.0f));

  int i = 7;
  i /= 2.0; /* computes 7/2.0 = 3.5, converts back to int */
  printf("%d\n", i);

  unsigned char ch = 200;
  ch += 100; /* wraps through int, truncates back */
  printf("%d\n", ch);
  return 0;
}
