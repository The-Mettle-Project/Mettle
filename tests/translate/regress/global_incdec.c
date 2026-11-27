/* ++ and -- on a scalar global read 0 instead of the global's value.
 *
 * Only when the ++ is the first reference to that global in the function: any
 * earlier read or write makes it correct, which is why this survived so long.
 * A global whose address is taken elsewhere is also fine. genLvalueAddr calls
 * mtlc's addressOf on a global handle, and the note at the top of Lower.hs
 * says addressOf is only defined for locals and parameters. */
#include <stdio.h>
int g = 5;
int h = 5;
static int counter(void) { static int n = 5; return ++n; }
int main(void) {
    printf("%d %d %d\n", ++g, h++, counter());
    return 0;
}
