#include <stdio.h>
int main(void) {
    int a[3][4]; int i, j, s = 0;
    for (i = 0; i < 3; i++) for (j = 0; j < 4; j++) a[i][j] = i * 10 + j;
    for (i = 0; i < 3; i++) for (j = 0; j < 4; j++) s += a[i][j];
    printf("%d %d %d %d\n", s, a[2][3], (int)sizeof(a), (int)sizeof(a[0]));
    return 0;
}
