/* C counterpart to switch_vm.mettle - bytecode interpreter switch dispatch. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#include "../bench_time.h"

#define PROG 4096

static int64_t run(const int32_t *ops, const int64_t *args) {
    int64_t acc = 1;
    for (int i = 0; i < PROG; i++) {
        int64_t operand = args[i];
        switch (ops[i]) {
            case 0: acc = acc + operand; break;
            case 1: acc = acc ^ operand; break;
            case 2: acc = acc * 3 + operand; break;
            case 3: acc = acc - operand; break;
            case 4: acc = (acc << 1) + operand; break;
            case 5: acc = acc + (operand >> 1); break;
            case 6: acc = acc ^ (operand + i); break;
            default: acc = acc + 1;
        }
    }
    return acc;
}

int main(void) {
    int32_t *ops = malloc((size_t)PROG * sizeof(int32_t));
    int64_t *args = malloc((size_t)PROG * sizeof(int64_t));
    int64_t seed = 12345;
    for (int k = 0; k < PROG; k++) {
        seed = seed * 1103515245 + 12345;
        ops[k] = (int32_t)((seed >> 16) & 7);
        args[k] = (seed >> 8) & 1023;
    }

    const int passes = 30000;
    printf("Bytecode VM switch dispatch, 4096 ops x 30000 passes\n");

    uint64_t t0 = bench_time_us();
    int64_t acc = 0;
    for (int p = 0; p < passes; p++) {
        acc ^= run(ops, args);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Checksum = %" PRId64 "\n", acc);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(ops);
    free(args);
    return 0;
}
