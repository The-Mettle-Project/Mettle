#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"


#define CODE_CAP 512
#define MEM_CELLS 65536
#define NREG 16
#define RANGE_LO 2
#define RANGE_HI 9000
#define PASSES 3

#define OP_HALT 0
#define OP_LOADI 1
#define OP_MOV 2
#define OP_ADD 3
#define OP_SUB 4
#define OP_MUL 5
#define OP_DIV 6
#define OP_MOD 7
#define OP_LT 8
#define OP_EQ 9
#define OP_JMP 10
#define OP_JZ 11
#define OP_JNZ 12
#define OP_LOAD 13
#define OP_STORE 14
#define OP_ADDI 15
#define OP_GT 16

typedef struct {
    int32_t op;
    int32_t a;
    int32_t b;
    int32_t c;
    int64_t imm;
} Inst;

typedef struct {
    Inst *code;
    int32_t count;
} Program;

static int32_t emit(Program *p, int32_t op, int32_t a, int32_t b, int32_t c, int64_t imm) {
  int32_t at = p->count;
  p->code[at].op = op;
  p->code[at].a = a;
  p->code[at].b = b;
  p->code[at].c = c;
  p->code[at].imm = imm;
  p->count += 1;
  return at;
}

static void patch(Program *p, int32_t at, int32_t target) {
  p->code[at].imm = (int64_t)target;
}

static void assemble_collatz(Program *p, int32_t lo, int32_t hi) {
  p->count = 0;
  emit(p, OP_LOADI, 0, 0, 0, (int64_t)lo);
  emit(p, OP_LOADI, 1, 0, 0, (int64_t)hi);
  emit(p, OP_LOADI, 8, 0, 0, 1);
  emit(p, OP_LOADI, 9, 0, 0, 2);
  emit(p, OP_LOADI, 10, 0, 0, 3);

  int32_t outer = p->count;
  emit(p, OP_MOV, 2, 0, 0, 0);
  emit(p, OP_LOADI, 3, 0, 0, 0);

  int32_t inner = p->count;
  emit(p, OP_EQ, 4, 2, 8, 0);
  int32_t to_done = emit(p, OP_JNZ, 4, 0, 0, 0);
  emit(p, OP_MOD, 5, 2, 9, 0);
  int32_t to_even = emit(p, OP_JZ, 5, 0, 0, 0);
  emit(p, OP_MUL, 6, 2, 10, 0);
  emit(p, OP_ADD, 2, 6, 8, 0);
  int32_t to_cont = emit(p, OP_JMP, 0, 0, 0, 0);

  int32_t even_at = p->count;
  emit(p, OP_DIV, 2, 2, 9, 0);

  int32_t cont_at = p->count;
  emit(p, OP_ADD, 3, 3, 8, 0);
  emit(p, OP_JMP, 0, 0, 0, (int64_t)inner);

  int32_t done_at = p->count;
  emit(p, OP_STORE, 0, 3, 0, 0);
  emit(p, OP_ADD, 0, 0, 8, 0);
  emit(p, OP_LT, 7, 0, 1, 0);
  emit(p, OP_JNZ, 7, 0, 0, (int64_t)outer);
  emit(p, OP_HALT, 0, 0, 0, 0);

  patch(p, to_done, done_at);
  patch(p, to_even, even_at);
  patch(p, to_cont, cont_at);
}

static int64_t run(Program *p, int64_t *reg, int64_t *mem, int64_t *out_steps) {
  int32_t i = 0;
  while (i < NREG) {
    reg[i] = 0;
    i += 1;
  }
  i = 0;
  while (i < MEM_CELLS) {
    mem[i] = 0;
    i += 1;
  }

  int32_t pc = 0;
  int64_t steps = 0;
  int32_t running = 1;
  while (running != 0) {
    int32_t op = p->code[pc].op;
    int32_t a = p->code[pc].a;
    int32_t b = p->code[pc].b;
    int32_t c = p->code[pc].c;
    int64_t imm = p->code[pc].imm;
    pc += 1;
    steps += 1;

    switch (op) {
      case 0:
        running = 0;
        break;
      case 1:
        reg[a] = imm;
        break;
      case 2:
        reg[a] = reg[b];
        break;
      case 3:
        reg[a] = reg[b] + reg[c];
        break;
      case 4:
        reg[a] = reg[b] - reg[c];
        break;
      case 5:
        reg[a] = reg[b] * reg[c];
        break;
      case 6:
        reg[a] = reg[b] / reg[c];
        break;
      case 7:
        reg[a] = reg[b] % reg[c];
        break;
      case 8:
        if (reg[b] < reg[c]) {
          reg[a] = 1;
        } else {
          reg[a] = 0;
        }
        break;
      case 9:
        if (reg[b] == reg[c]) {
          reg[a] = 1;
        } else {
          reg[a] = 0;
        }
        break;
      case 10:
        pc = (int32_t)imm;
        break;
      case 11:
        if (reg[a] == 0) {
          pc = (int32_t)imm;
        }
        break;
      case 12:
        if (reg[a] != 0) {
          pc = (int32_t)imm;
        }
        break;
      case 13:
        reg[a] = mem[(int32_t)reg[b] & 65535];
        break;
      case 14:
        mem[(int32_t)reg[a] & 65535] = reg[b];
        break;
      case 15:
        reg[a] = reg[b] + imm;
        break;
      case 16:
        if (reg[b] > reg[c]) {
          reg[a] = 1;
        } else {
          reg[a] = 0;
        }
        break;
      default:
        running = 0;
        break;
    }
  }

  uint64_t h = 14695981039346656037ULL;
  i = RANGE_LO;
  while (i < RANGE_HI) {
    h ^= (uint64_t)mem[i];
    h *= 1099511628211ULL;
    i += 1;
  }
  i = 0;
  while (i < NREG) {
    h = h * 31 + (uint64_t)reg[i];
    i += 1;
  }
  *out_steps = steps;
  return (int64_t)h;
}

static uint64_t round_trip(Program *p, int64_t *reg, int64_t *mem, int64_t *out_steps) {
  assemble_collatz(p, RANGE_LO, RANGE_HI);
  int64_t steps = 0;
  int64_t r = run(p, reg, mem, &steps);
  uint64_t h = 1469598103934665603ULL;
  h = h * 1000003 + (uint64_t)r;
  h = h * 31 + (uint64_t)p->count;
  h = h * 31 + (uint64_t)steps;
  *out_steps = steps;
  return h;
}

int main(void) {
    Inst *code = (Inst *)malloc((size_t)CODE_CAP * sizeof(Inst));
    int64_t *reg = (int64_t *)malloc((size_t)NREG * 8);
    int64_t *mem = (int64_t *)malloc((size_t)MEM_CELLS * 8);
    if (code == NULL || reg == NULL || mem == NULL) {
        printf("malloc failed\n");
        return 1;
    }
    Program p;
    p.code = code;
    p.count = 0;

    printf("Bytecode VM: collatz over [%d,%d), %d registers\n", RANGE_LO, RANGE_HI, NREG);

    int64_t steps = 0;
    uint64_t check = round_trip(&p, reg, mem, &steps);
    printf("  program = %d instructions, executed %" PRId64 " steps\n", p.count, steps);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int64_t s2 = 0;
        bench_hash = bench_hash * 1000003 + round_trip(&p, reg, mem, &s2);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(code);
    free(reg);
    free(mem);
    return 0;
}
