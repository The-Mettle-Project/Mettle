#include <stdio.h>
int main(void) {
    int a[8]; int *p = a, *q = a + 5;
    printf("%d %d %d %d\n", (int)(q - p), p < q, q > p, p == a);
    const char *s = "hello";
    printf("%d %d\n", (int)(s[1]), s[5] == 0);
    return 0;
}
