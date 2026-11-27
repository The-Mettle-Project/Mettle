/* A shift whose LEFT operand is itself a shift lowers to a logical shift.
 *
 * Storing the intermediate in a variable makes it correct, which is the tell:
 *   int y = x << 28; y >> 28   is right
 *   (x << 28) >> 28            is wrong
 * The second is the textbook way to sign-extend a narrow field by hand. */
#include <stdio.h>
int main(void) {
    int x = 15;
    int y = x << 28;
    printf("%d %d\n", (x << 28) >> 28, y >> 28);
    return 0;
}
