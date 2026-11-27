#include <stdio.h>
struct P { int a, b, c; };
static struct P mk(int v) { struct P p; p.a = v; p.b = v * 2; p.c = v * 3; return p; }
static int take(struct P p) { return p.a + p.b + p.c; }
int main(void) {
    struct P (*f)(int) = mk;
    int (*g)(struct P) = take;
    struct P p = f(7);
    printf("%d %d %d %d\n", p.a, p.b, p.c, g(p));
    return 0;
}
