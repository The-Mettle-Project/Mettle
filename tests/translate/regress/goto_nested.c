#include <stdio.h>
int main(void) {
    int i, j, hits = 0;
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 5; j++) {
            if (i * j > 6) goto done;
            hits++;
        }
    }
done:
    printf("%d %d %d\n", hits, i, j);
    return 0;
}
