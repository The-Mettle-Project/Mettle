#include <stdio.h>
struct P { int x, y; };
static int sum(struct P p) { return p.x + p.y; }
int main(void) {
    struct P a = (struct P){3, 4};
    int *arr = (int[]){10, 20, 30};
    printf("%d %d %d\n", sum(a), sum((struct P){5, 6}), arr[0] + arr[1] + arr[2]);
    return 0;
}
