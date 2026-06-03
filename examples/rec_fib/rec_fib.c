/*
 * C recursive Fibonacci benchmark - counterpart to rec_fib.mettle.
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

static int64_t fib(int n) {
    if (n < 2) {
        return (int64_t)n;
    }
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    const int passes = 5;
    printf("Recursive fib(34) x 5 passes\n");

    volatile int arg = 34;

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc += fib(arg);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
