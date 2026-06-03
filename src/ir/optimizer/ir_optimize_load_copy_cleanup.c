#include "ir_optimize_internal.h"

int ir_eliminate_load_symbol_copy_pass(IRFunction *function,
                                              int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i + 1 < function->instruction_count; i++) {
    IRInstruction *load = &function->instructions[i];
    IRInstruction *assign = &function->instructions[i + 1];
    const char *sym = NULL;
    const char *temp = NULL;
    size_t use_count = 0;
    size_t j = 0;
    int stopped_on_control = 0;

    if (load->op != IR_OP_LOAD || load->dest.kind != IR_OPERAND_TEMP ||
        !load->dest.name || assign->op != IR_OP_ASSIGN ||
        assign->dest.kind != IR_OPERAND_SYMBOL || !assign->dest.name ||
        assign->lhs.kind != IR_OPERAND_TEMP || !assign->lhs.name ||
        strcmp(assign->lhs.name, load->dest.name) != 0) {
      continue;
    }

    sym = assign->dest.name;
    temp = load->dest.name;

    for (j = i + 2; j < function->instruction_count; j++) {
      const IRInstruction *ins = &function->instructions[j];
      if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
          ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
        stopped_on_control = 1;
        break;
      }
      if (ir_instruction_writes_symbol(ins) &&
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        break;
      }
      if (ir_operand_is_symbol_named(&ins->lhs, sym) ||
          ir_operand_is_symbol_named(&ins->rhs, sym) ||
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        use_count++;
      }
      for (size_t a = 0; a < ins->argument_count; a++) {
        if (ir_operand_is_symbol_named(&ins->arguments[a], sym)) {
          use_count++;
        }
      }
    }

    if (stopped_on_control || use_count == 0 || use_count > 6) {
      continue;
    }

    for (j = i + 2; j < function->instruction_count; j++) {
      IRInstruction *ins = &function->instructions[j];
      if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
          ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
        break;
      }
      if (ir_instruction_writes_symbol(ins) &&
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        break;
      }
      if (ir_operand_is_symbol_named(&ins->lhs, sym)) {
        ir_operand_destroy(&ins->lhs);
        ins->lhs = ir_operand_temp(temp);
      }
      if (ir_operand_is_symbol_named(&ins->rhs, sym)) {
        ir_operand_destroy(&ins->rhs);
        ins->rhs = ir_operand_temp(temp);
      }
      if (ir_operand_is_symbol_named(&ins->dest, sym) &&
          ins->op != IR_OP_STORE) {
        ir_operand_destroy(&ins->dest);
        ins->dest = ir_operand_temp(temp);
      }
    }

    ir_instruction_make_nop(assign);
    if (changed) {
      *changed = 1;
    }
  }

  return 1;
}

