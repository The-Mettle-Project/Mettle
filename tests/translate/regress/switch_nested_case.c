/* case labels inside a nested block still belong to the switch */
#include <stdio.h>
int f(int v) {
    switch (v) {
        case 0: return 10;
        {
            case 1: return 11;
            case 2: return 12;
        }
        default: return 99;
    }
}
int main(void) { printf("%d %d %d %d\n", f(0), f(1), f(2), f(5)); return 0; }
