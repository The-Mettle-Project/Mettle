#include <stdio.h>
int main(void) {
    int i, s = 0, n = 0;
    for (i = 0; i < 10; i++) { if (i % 3 == 0) continue; if (i == 8) break; s += i; }
    do { n++; } while (n < 5);
    int w = 0; while (w < 3) w++;
    printf("%d %d %d %d\n", s, i, n, w);
    return 0;
}
