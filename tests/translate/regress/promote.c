/* integer promotions and the usual arithmetic conversions */
#include <stdio.h>
int main(void) {
    signed char sc = -1;
    unsigned char uc = 255;
    short sh = -2;
    unsigned short ush = 65535;
    printf("%d %d %d %d\n", sc + 0, uc + 0, sh + 0, ush + 0);
    printf("%d %d\n", sc < 0, (sc + uc));
    unsigned u = 1;
    int i = -1;
    printf("%d\n", i < u);      /* i converts to unsigned: false */
    return 0;
}
