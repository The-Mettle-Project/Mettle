/* a struct-valued conditional */
#include <stdio.h>
struct P { int a, b, c, d; };
int main(void) {
    struct P x = {1,2,3,4}, y = {5,6,7,8}, r;
    int c = 0;
    r = c ? x : y;
    printf("%d %d %d %d\n", r.a, r.b, r.c, r.d);
    r = 1 ? x : y;
    printf("%d %d %d %d\n", r.a, r.b, r.c, r.d);
    return 0;
}
