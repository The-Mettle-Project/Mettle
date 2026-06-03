/* C counterpart to struct_byval.mettle - 32-byte struct passed by value. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 40000000

typedef struct {
    int64_t a, b, c, d;
} V4;

static V4 step(V4 v) {
    V4 r;
    r.a = v.a + v.d;
    r.b = v.b ^ (v.a >> 3);
    r.c = v.c + v.b + 1;
    r.d = v.d * 2654435761 + 1;
    return r;
}

static int64_t run(void) {
    V4 s = {1, 2, 3, 4};
    for (int64_t i = 0; i < (int64_t)N; i++) {
        s = step(s);
    }
    return s.a ^ s.b ^ s.c ^ s.d;
}

int main(void) {
    const int passes = 5;
    printf("Struct-by-value (32B) call, 40M iters x 5 passes\n");

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc ^= run();
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    return 0;
}
