/* a signed bit-field holds negative values */
#include <stdio.h>
struct S { signed int a : 4; unsigned int b : 4; signed int c : 12; };
int main(void) {
    struct S s;
    s.a = -1; s.b = 15; s.c = -2048;
    printf("%d %u %d\n", s.a, s.b, s.c);
    return 0;
}
