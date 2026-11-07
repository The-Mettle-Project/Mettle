#include "ir_lanes.h"

int ir_lanes_layout(const IRInstruction *instruction, LaneLayout *layout) {
  if (!instruction || instruction->op != IR_OP_SIMD_LANES ||
      instruction->argument_count < LANES_HEADER_ARGS) {
    return 0;
  }
  const IROperand *args = instruction->arguments;
  layout->n_bases = (int)args[0].int_value;
  layout->n_scalars = (int)args[1].int_value;
  layout->n_consts = (int)args[2].int_value;
  layout->n_ops = (int)args[3].int_value;
  layout->n_reduce = (int)args[4].int_value;
  layout->store_mask = (unsigned)args[5].int_value;
  layout->uses_iota = (args[6].int_value & LANES_FLAG_IOTA) != 0;
  layout->keep_last = (args[6].int_value & LANES_FLAG_KEEP_LAST) != 0;
  if (layout->n_bases < 0 || layout->n_bases > LANES_MAX_BASES ||
      layout->n_scalars < 0 || layout->n_scalars > LANES_MAX_SCALARS ||
      layout->n_consts < 0 || layout->n_consts > LANES_MAX_CONSTS ||
      layout->n_ops < 1 || layout->n_ops > LANES_MAX_OPS ||
      layout->n_reduce < 0 || layout->n_reduce > LANES_MAX_REDUCE) {
    return 0;
  }
  layout->bases_at = LANES_HEADER_ARGS;
  layout->scalars_at = layout->bases_at + (size_t)layout->n_bases;
  layout->partials_at = layout->scalars_at + (size_t)layout->n_scalars;
  layout->consts_at = layout->partials_at + (size_t)layout->n_reduce;
  layout->kinds_at = layout->consts_at + (size_t)layout->n_consts;
  layout->ops_at = layout->kinds_at + (size_t)layout->n_reduce;
  return instruction->argument_count ==
         layout->ops_at + 4 * (size_t)layout->n_ops;
}

int ir_lanes_operand_fields(int op, int fields[3]) {
  switch (op) {
  case LANE_LOAD:
  case LANE_CONST:
  case LANE_SCALAR:
  case LANE_IOTA:
  case LANE_CARRY:
    return 0;
  case LANE_FSQRT:
  case LANE_MASK01:
  case LANE_COPY:
  case LANE_CVTI2F:
  case LANE_CVTF2I:
  case LANE_SHLI:
  case LANE_SHRI:
  case LANE_SARI:
    fields[0] = 0;
    return 1;
  case LANE_STORE:
  case LANE_REDUCE:
  case LANE_MLOAD:
    fields[0] = 1;
    return 1;
  case LANE_MSTORE:
    fields[0] = 1;
    fields[1] = 2;
    return 2;
  case LANE_SELECT:
    fields[0] = 0;
    fields[1] = 1;
    fields[2] = 2;
    return 3;
  default:
    fields[0] = 0;
    fields[1] = 1;
    return 2;
  }
}

int ir_lanes_operands(const LaneInsn *insn, int out[3]) {
  int fields[3];
  int count = ir_lanes_operand_fields(insn->op, fields);
  const int values[3] = {insn->a, insn->b, insn->c};
  for (int q = 0; q < count; q++) {
    out[q] = values[fields[q]];
  }
  return count;
}

LaneInsn ir_lanes_insn(const IRInstruction *instruction,
                       const LaneLayout *layout, int index) {
  const IROperand *at = &instruction->arguments[layout->ops_at + 4 * index];
  LaneInsn insn;
  insn.op = (int)at[0].int_value;
  insn.a = (int)at[1].int_value;
  insn.b = (int)at[2].int_value;
  insn.c = (int)at[3].int_value;
  return insn;
}
