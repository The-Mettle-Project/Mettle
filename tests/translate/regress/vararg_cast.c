/* An int whose value came from a float-to-int cast in the same function is
 * passed to a variadic function as the raw float bit pattern.
 * 2.5f prints as 1075838976, which is 0x40200000. */
#include <stdio.h>
int main(void) {
    double d = 3.75;
    float f = 2.5f;
    int i = (int)d;
    printf("%d %d %u\n", i, (int)f, (unsigned)f);
    printf("%.2f\n", (double)7);
    return 0;
}
