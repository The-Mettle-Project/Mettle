/* Preprocessor corners that real headers lean on.
 *
 * - A block comment inside a directive runs past the newline: phase 3 turns
 *   the whole comment into one space before phase 4 reads the directive, so
 *   the directive ends where the comment closes. SQLite hits this ~100 times.
 * - `?:` is a valid #if operator.
 * - `##` joins two tokens and nothing else: a spliced argument must not fuse
 *   with the text beside it.
 * - A replacement is rescanned together with what follows it, so a pasted
 *   name can name the macro that the following argument list belongs to.
 */
#include <stdio.h>

#define A 1  /* a comment that
  ** wraps over
  ** several lines */
#define B 2
#define LEAD(x) /* before
  */ ((x) + 1)

#if A + B == 3 /* the condition itself
   spans lines too */
#define OK 1
#endif

#if A > 0 ? B : 0
#define TERN (A > 0 ? 10 : 20)
#endif
#if (A ? 0 : 1) ? 9 : B == 2 ? 7 : 8
#define TERN2 7
#endif

#define PLUS(a, b) a##b+
#define SEMI(a, b) a##b ; second

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define XY(v) ((v) * 100)

int main(void) {
  int first = 21, second = 0;
  int n = 60 PLUS(+, ) 3;

  second = SEMI(first, ) *= 2;

  printf("%d %d %d\n", A, B, LEAD(9));
  printf("%d %d %d\n", OK, TERN, TERN2);
  printf("%d %d %d\n", n, first, second);
  printf("%d\n", CAT(X, Y)(4));
  return 0;
}
