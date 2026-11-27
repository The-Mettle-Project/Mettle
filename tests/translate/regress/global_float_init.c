/* A scalar global with a floating initializer. The IR builder's global
   constructor takes an integer constant, so the value used to be truncated to
   zero; aggregates were fine because they go through the constructor. */
#include <stdio.h>

double gd = 100.5;
float gf = 3.25f;
double gwhole = 7;
double gneg = -2.75;
double garr[3] = {1.5, 2.5, 3.5};
struct S {
  double d;
  int i;
} gs = {9.75, 5};

int main(void) {
  printf("%d %d\n", (int)(gd * 4), gd < 1);
  printf("%d %d\n", (int)(gf * 4), gf < 1);
  printf("%d %d\n", (int)(gwhole * 4), (int)(gneg * 4));
  printf("%d %d %d\n", (int)(garr[0] * 2), (int)(garr[1] * 2), (int)(garr[2] * 2));
  printf("%d %d\n", (int)(gs.d * 4), gs.i);
  return 0;
}
