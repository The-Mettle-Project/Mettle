/* A call with many arguments overruns the caller's outgoing-argument area and
 * overwrites its own spilled locals. Floating-point locals go first.
 *
 * A backend fault, reachable with no variadic call anywhere. In this shape 17
 * arguments is enough; the exact threshold moves with the frame, so a wider
 * frame can be hit at 16. The frontend used to manufacture the condition for
 * every printf by padding each variadic call to 32 arguments, which is why
 * `double d = 3.75; printf("x"); (int)d` read 0. That padding is now 16, so
 * ordinary variadic code is clear of it, but the fault itself is still here.
 */
#include <stdio.h>
static int wide(int a,int b,int c,int d,int e,int f,int g,int h,int i,
                int j,int k,int l,int m,int n,int o,int p,int q) { return a + q; }
int main(void) {
    double keep = 3.75;
    wide(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);
    printf("%d\n", (int)keep);
    return 0;
}
