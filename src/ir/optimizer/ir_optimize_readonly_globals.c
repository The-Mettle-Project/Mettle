#include "ir_optimize_internal.h"
#include "../ir_optimize.h"

/* Fold reads of never-written global integer vars to their initializer
 * constant. main.c supplies the candidates (globals with literal integer
 * initializers, from the AST); this pass proves each one is never stored to
 * and never has its address taken anywhere in the program, then rewrites
 * every read operand to the constant. Turns `idx * NODE_BYTES` into a
 * strength-reducible constant multiply and removes the per-call global-cache
 * reload the MIR backend would otherwise emit. */

static int ir_rg_operand_reads(const IROperand *operand, const char *name) {
  return operand->kind == IR_OPERAND_SYMBOL && operand->name &&
         strcmp(operand->name, name) == 0;
}

static int ir_rg_global_is_written(const IRProgram *program,
                                   const char *name) {
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    /* A local or parameter of the same name shadows the global throughout
     * this function; skip it entirely so its writes don't disqualify (its
     * reads are also not rewritten - see the same check in the fold). */
    if (ir_function_local_declared_type((IRFunction *)fn, name) ||
        ir_function_symbol_is_parameter(fn, name)) {
      continue;
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *ins = &fn->instructions[i];
      if (ins->op == IR_OP_ADDRESS_OF && ir_rg_operand_reads(&ins->lhs, name)) {
        return 1;
      }
      if (ir_instruction_writes_destination(ins) &&
          ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
          strcmp(ins->dest.name, name) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static void ir_rg_fold_reads(IRProgram *program, const char *name,
                             long long value) {
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    if (ir_function_local_declared_type(fn, name) ||
        ir_function_symbol_is_parameter(fn, name)) {
      continue;
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *ins = &fn->instructions[i];
      if (ins->op == IR_OP_ADDRESS_OF) {
        continue;
      }
      if (ir_rg_operand_reads(&ins->lhs, name)) {
        ir_operand_destroy(&ins->lhs);
        ins->lhs = ir_operand_int(value);
      }
      if (ir_rg_operand_reads(&ins->rhs, name)) {
        ir_operand_destroy(&ins->rhs);
        ins->rhs = ir_operand_int(value);
      }
      for (size_t a = 0; a < ins->argument_count; a++) {
        if (ir_rg_operand_reads(&ins->arguments[a], name)) {
          ir_operand_destroy(&ins->arguments[a]);
          ins->arguments[a] = ir_operand_int(value);
        }
      }
    }
  }
}

int ir_fold_readonly_globals_pass(IRProgram *program,
                                  const IRGlobalIntConst *consts,
                                  size_t count, int *changed) {
  if (!program || !consts || count == 0) {
    return 1;
  }
  for (size_t c = 0; c < count; c++) {
    if (!consts[c].name || ir_rg_global_is_written(program, consts[c].name)) {
      continue;
    }
    ir_rg_fold_reads(program, consts[c].name, consts[c].value);
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}
