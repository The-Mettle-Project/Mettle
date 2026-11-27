#include <stdio.h>
static int fib(int n) { return n < 2 ? n : fib(n-1) + fib(n-2); }
static int ack(int m, int n) {
    if (m == 0) return n + 1;
    if (n == 0) return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}
int main(void) { printf("%d %d\n", fib(20), ack(2, 3)); return 0; }
