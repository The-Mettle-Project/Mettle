#include <stdio.h>
union U { int i; unsigned u; char c[4]; };
int main(void) {
    union U u; u.i = 0x01020304;
    printf("%u %d %d\n", u.u, (int)sizeof(u), u.c[0] != 0);
    return 0;
}
