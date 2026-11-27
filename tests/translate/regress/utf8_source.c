/* Non-ASCII bytes in comments and string literals: café, naïve, →, ünïcöde.
 *
 * A C file may hold any bytes at all, and any of them can end up quoted back
 * in a diagnostic. Printing one used to throw an encoding exception in place
 * of the error the user asked about, which killed the compiler outright on a
 * source file whose only sin was an accented word in a comment.
 */
#include <stdio.h>
#include <string.h>

static const char *greet = "héllo wörld";

int main(void) {
  const char *arrow = "→";
  printf("%s %s\n", greet, arrow);
  printf("%d %d\n", (int)strlen(greet), (int)strlen(arrow));
  printf("%d\n", (unsigned char)greet[1]);
  return 0;
}
