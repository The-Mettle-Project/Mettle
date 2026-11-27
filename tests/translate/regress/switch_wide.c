/* switch on a value that does not fit in 32 bits */
#include <stdio.h>
int pick(long long v) {
    switch (v) {
    case 1LL: return 1;
    case 4294967296LL: return 2;   /* 2^32 */
    case 4294967297LL: return 3;   /* 2^32 + 1 */
    default: return 0;
    }
}
int main(void) {
    printf("%d %d %d %d\n", pick(1), pick(4294967296LL), pick(4294967297LL), pick(7));
    return 0;
}
