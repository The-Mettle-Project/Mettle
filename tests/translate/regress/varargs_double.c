#include <stdio.h>
#include <stdarg.h>
static double sumd(int n, ...) {
    va_list ap; double t = 0; int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) t += va_arg(ap, double);
    va_end(ap);
    return t;
}
static long long suml(int n, ...) {
    va_list ap; long long t = 0; int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) t += va_arg(ap, long long);
    va_end(ap);
    return t;
}
int main(void) {
    printf("%.1f %lld\n", sumd(3, 1.5, 2.25, 3.0), suml(3, 10LL, 20LL, 30LL));
    return 0;
}
