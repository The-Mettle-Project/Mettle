#include <stdio.h>
#include <stdlib.h>
struct V { int n; int d[]; };
int main(void) {
    struct V *v = malloc(sizeof(struct V) + 4 * sizeof(int));
    int i; v->n = 4;
    for (i = 0; i < 4; i++) v->d[i] = i * 10;
    printf("%d %d %d %d %d\n", v->n, v->d[0], v->d[1], v->d[2], v->d[3]);
    free(v);
    return 0;
}
