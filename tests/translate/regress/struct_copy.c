#include <stdio.h>
struct Inner { int a, b; };
struct Outer { struct Inner in; int c; char pad[6]; };
static struct Outer id(struct Outer o) { return o; }
int main(void) {
    struct Outer o = {{1,2},3,{4,5,6,7,8,9}};
    struct Outer p = id(o);
    printf("%d %d %d %d %d\n", p.in.a, p.in.b, p.c, p.pad[0], p.pad[5]);
    return 0;
}
