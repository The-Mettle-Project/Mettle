/* are-we-fast-yet :: Sieve  (C reference, matches sieve.mettle 1:1) */
#include <stdio.h>
#include <stdint.h>
#include "../bench_time.h"

#define SIZE 5000

static int sieve(unsigned char* flags, int size) {
    int primeCount = 0;
    for (int i = 2; i <= size; i++) {
        if (flags[i - 1]) {
            primeCount++;
            int k = i + i;
            while (k <= size) {
                flags[k - 1] = 0;
                k += i;
            }
        }
    }
    return primeCount;
}

static int benchmark(unsigned char* flags) {
    for (int i = 0; i < SIZE; i++) flags[i] = 1;
    return sieve(flags, SIZE);
}

int main(void) {
    int passes = 3000;
    unsigned char flags[SIZE];

    int last = 0;
    uint64_t t0 = bench_time_us();
    for (int it = 0; it < passes; it++) {
        last = benchmark(flags);
    }
    uint64_t elapsed = bench_time_us() - t0;

    printf("Sieve\n");
    printf("  result   = %d\n", last);
    printf("  expected = 669\n");
    printf("  iters    = %d\n", passes);
    printf("  time_us  = %llu\n", (unsigned long long)elapsed);
    return last == 669 ? 0 : 1;
}
