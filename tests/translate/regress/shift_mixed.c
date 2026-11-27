/* C99 6.5.7p3: the result type of a shift is the promoted LEFT operand only.
   The right operand's type must not make it a logical shift. */
#include <stdio.h>
int main(void) {
    int x = -8;
    unsigned s = 1;
    long long y = -1024;
    unsigned char c = 2;
    printf("%d %d %lld\n", x >> s, x >> 1, y >> c);
    return 0;
}
