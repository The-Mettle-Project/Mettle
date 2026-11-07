#ifndef IR_LANES_H
#define IR_LANES_H

#include "ir.h"
#include <stddef.h>
#include <stdint.h>

#define LANES_MAX_BASES 8
#define LANES_MAX_SCALARS 4
#define LANES_MAX_REDUCE 4
#define LANES_MAX_CONSTS 96
#define LANES_MAX_OPS 512

#define LANES_HEADER_ARGS 7

#define LANES_FLAG_IOTA 1
#define LANES_FLAG_KEEP_LAST 2

typedef enum {
  LANE_LOAD = 1,
  LANE_CONST,
  LANE_SCALAR,
  LANE_IOTA,
  LANE_CARRY,
  LANE_FADD,
  LANE_FSUB,
  LANE_FMUL,
  LANE_FDIV,
  LANE_FMIN,
  LANE_FMAX,
  LANE_FSQRT,
  LANE_FCMP,
  LANE_IADD,
  LANE_ISUB,
  LANE_IMUL,
  LANE_AND,
  LANE_OR,
  LANE_XOR,
  LANE_SHLI,
  LANE_SHRI,
  LANE_SARI,
  LANE_SHLV,
  LANE_SHRV,
  LANE_SARV,
  LANE_ICMPEQ,
  LANE_ICMPGT,
  LANE_IMIN,
  LANE_IMAX,
  LANE_UMIN,
  LANE_UMAX,
  LANE_SELECT,
  LANE_MASK01,
  LANE_COPY,
  LANE_CVTI2F,
  LANE_CVTF2I,
  LANE_STORE,
  LANE_REDUCE,
  LANE_MLOAD,
  LANE_MSTORE,
  LANE_OP_COUNT
} LaneOp;

typedef enum {
  LANE_CMP_EQ = 0x00,
  LANE_CMP_LT = 0x01,
  LANE_CMP_LE = 0x02,
  LANE_CMP_NE = 0x04
} LaneCompare;

typedef enum { LANE_REDUCE_FSUM = 0, LANE_REDUCE_ISUM = 1 } LaneReduceKind;

typedef struct {
  int op;
  int a;
  int b;
  int c;
} LaneInsn;

typedef struct {
  int n_bases;
  int n_scalars;
  int n_consts;
  int n_ops;
  int n_reduce;
  unsigned store_mask;
  int uses_iota;
  int keep_last;
  size_t bases_at;
  size_t scalars_at;
  size_t partials_at;
  size_t consts_at;
  size_t kinds_at;
  size_t ops_at;
} LaneLayout;

int ir_lanes_layout(const IRInstruction *instruction, LaneLayout *layout);
int ir_lanes_operand_fields(int op, int fields[3]);
int ir_lanes_operands(const LaneInsn *insn, int out[3]);
LaneInsn ir_lanes_insn(const IRInstruction *instruction,
                       const LaneLayout *layout, int index);

#endif
