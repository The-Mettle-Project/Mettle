/* The same string literal spelled in more than one branch.
 *
 * A literal is a pointer global filled in on first use. The check used to be
 * emitted where the literal first appeared in the function, which put it
 * inside whatever branch that was; a use in a sibling branch then read a null
 * pointer and printf walked off it. Mettle's own diagnostic renderer spells
 * "  --> %s" in two arms of an if, so 83 of its tests crashed on the arm that
 * did not come first. The check now goes at the function entry, which is the
 * only place that dominates every use.
 */
#include <stdio.h>
#include <string.h>

static void show(int which, const char *tail) {
  if (which == 1)
    printf("one %s\n", tail);
  else if (which == 2)
    printf("one %s\n", tail);
  else
    printf("one %s\n", tail);
}

static const char *pick(int which) {
  switch (which) {
  case 0:
    return "shared";
  case 1:
    return "shared";
  default:
    return "shared";
  }
}

/* the literal's first appearance is in a loop body that runs zero times */
static int never_first(int n) {
  int i, t = 0;
  for (i = 0; i < n; i++)
    t += (int)strlen("counted");
  return t + (int)strlen("counted");
}

int main(void) {
  show(3, "third");
  show(1, "first");
  printf("%s %s %s\n", pick(2), pick(0), pick(1));
  printf("%d\n", never_first(0));
  printf("%d\n", never_first(2));
  return 0;
}
