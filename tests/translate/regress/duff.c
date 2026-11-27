/* Duff's device: the canonical case labels inside a loop body. The switch
 * dispatch has to jump into the middle of the do/while. */
#include <stdio.h>
static void copy(char *to, const char *from, int count) {
    int n = (count + 7) / 8;
    switch (count % 8) {
    case 0: do { *to++ = *from++;
    case 7:      *to++ = *from++;
    case 6:      *to++ = *from++;
    case 5:      *to++ = *from++;
    case 4:      *to++ = *from++;
    case 3:      *to++ = *from++;
    case 2:      *to++ = *from++;
    case 1:      *to++ = *from++;
            } while (--n > 0);
    }
}
int main(void) {
    char d[24]; const char *s = "abcdefghijklmnopqrstuvw"; int i;
    for (i = 0; i < 24; i++) d[i] = 0;
    copy(d, s, 23); d[23] = 0;
    printf("%s\n", d);
    return 0;
}
