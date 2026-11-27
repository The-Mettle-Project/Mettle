/* unsigned division, modulo and comparison near the sign boundary */
#include <stdio.h>
int main(void) {
    unsigned a = 4000000000u, b = 3u;
    unsigned long long c = 18000000000000000000ull, d = 7ull;
    printf("%u %u\n", a / b, a % b);
    printf("%llu %llu\n", c / d, c % d);
    printf("%d %d\n", a > 5u, c > 5ull);
    return 0;
}
