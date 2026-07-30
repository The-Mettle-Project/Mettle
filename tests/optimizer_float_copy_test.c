#include "ir/optimizer/ir_optimize_internal.h"

#include <stdio.h>

static int append_instruction(IRFunction *function, IROpcode op,
                              IROperand dest, IROperand lhs, IROperand rhs,
                              const char *text, int is_float) {
  IRInstruction instruction = {0};
  int ok;
  instruction.op = op;
  instruction.dest = dest;
  instruction.lhs = lhs;
  instruction.rhs = rhs;
  instruction.text = (char *)text;
  instruction.is_float = is_float;
  instruction.float_bits = is_float ? 64 : 0;
  ok = ir_function_append_instruction(function, &instruction);
  ir_operand_destroy(&dest);
  ir_operand_destroy(&lhs);
  ir_operand_destroy(&rhs);
  return ok;
}

int main(void) {
  IRFunction *function = ir_function_create("float_loop");
  int changed = 0;
  int ok;
  if (!function)
    return 2;

  ok = append_instruction(function, IR_OP_BINARY,
                          ir_operand_symbol("term"), ir_operand_temp("num"),
                          ir_operand_temp("denom"), NULL, 1) &&
       append_instruction(function, IR_OP_BINARY,
                          ir_operand_symbol("sum"), ir_operand_symbol("sum"),
                          ir_operand_symbol("term"), NULL, 1) &&
       append_instruction(function, IR_OP_JUMP, ir_operand_none(),
                          ir_operand_none(), ir_operand_none(), ".Lloop", 0);
  if (!ok) {
    ir_function_destroy(function);
    return 2;
  }

  ok = ir_eliminate_single_use_float_symbol_copies_pass(function, &changed);
  if (!ok || changed ||
      function->instructions[0].dest.kind != IR_OPERAND_SYMBOL ||
      function->instructions[1].rhs.kind != IR_OPERAND_SYMBOL) {
    fprintf(stderr, "float-copy pass rewrote a loop-carried symbol\n");
    ir_function_destroy(function);
    return 1;
  }

  ir_function_destroy(function);
  return 0;
}
