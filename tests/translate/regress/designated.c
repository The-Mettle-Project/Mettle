#include <stdio.h>
struct S { int a, b, c, d; };
int main(void) {
    struct S s = {.c = 3, .a = 1};
    int arr[6] = {[4] = 40, [1] = 10};
    printf("%d %d %d %d\n", s.a, s.b, s.c, s.d);
    printf("%d %d %d %d\n", arr[0], arr[1], arr[4], arr[5]);
    return 0;
}
