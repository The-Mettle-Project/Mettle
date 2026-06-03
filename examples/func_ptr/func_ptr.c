/* C counterpart to func_ptr.mettle - indirect call hot loop.
 * The function pointer goes through a volatile sink so GCC cannot
 * devirtualize/inline it, matching Mettle's genuine indirect call. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 60000000

typedef int64_t (*mix_fn)(int64_t, int64_t);

static int64_t mix(int64_t a, int64_t b) {
    return (a * 1103515245 + 12345) ^ (b + (a >> 7));
}

static int64_t run(mix_fn f) {
    int64_t acc = 1;
    for (int64_t i = 0; i < (int64_t)N; i++) {
        acc = f(acc, i);
    }
    return acc;
}

int main(void) {
    const int passes = 5;
    printf("Indirect (fn-ptr) call, 60M iters x 5 passes\n");

    volatile mix_fn vf = mix;
    mix_fn f = vf;

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc ^= run(f);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
