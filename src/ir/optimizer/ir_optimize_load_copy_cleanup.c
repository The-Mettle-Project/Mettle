#include "ir_optimize_internal.h"

/* Reads of `sym` in one instruction: lhs/rhs/arguments, plus dest on a STORE
 * (a store's dest is the address operand, which is read, not written). */
static size_t ir_load_copy_count_symbol_reads(const IRInstruction *ins,
                                              const char *sym) {
  size_t count = 0;
  if (ir_operand_is_symbol_named(&ins->lhs, sym)) {
    count++;
  }
  if (ir_operand_is_symbol_named(&ins->rhs, sym)) {
    count++;
  }
  if (ins->op == IR_OP_STORE && ir_operand_is_symbol_named(&ins->dest, sym)) {
    count++;
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    if (ir_operand_is_symbol_named(&ins->arguments[a], sym)) {
      count++;
    }
  }
  return count;
}

static void ir_load_copy_replace_operand(IROperand *operand, const char *sym,
                                         const char *temp) {
  if (!ir_operand_is_symbol_named(operand, sym)) {
    return;
  }
  int float_bits = operand->float_bits;
  ir_operand_destroy(operand);
  *operand = ir_operand_temp(temp);
  /* Keep the IEEE-754 width tag the symbol operand carried; consumers use it
   * to tell float32 values from the default double width. */
  operand->float_bits = float_bits;
}

int ir_eliminate_load_symbol_copy_pass(IRFunction *function,
                                              int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i + 1 < function->instruction_count; i++) {
    IRInstruction *load = &function->instructions[i];
    IRInstruction *assign = NULL;
    size_t assign_index = i + 1;
    const char *sym = NULL;
    const char *temp = NULL;
    size_t window_reads = 0;
    size_t total_reads = 0;
    size_t window_end = function->instruction_count;
    size_t j = 0;
    int unsafe_use = 0;

    if (load->op != IR_OP_LOAD || load->dest.kind != IR_OPERAND_TEMP ||
        !load->dest.name) {
      continue;
    }

    /* The assign usually follows the load directly, but the inliner's
     * parameter materialization interposes the parameter's DECLARE_LOCAL
     * (`%t <- *addr; local @p; @p <- %t`). Neither a NOP nor a declaration
     * reads or writes the temp or the symbol, so skip past them. */
    while (assign_index < function->instruction_count &&
           (function->instructions[assign_index].op == IR_OP_NOP ||
            function->instructions[assign_index].op == IR_OP_DECLARE_LOCAL)) {
      assign_index++;
    }
    if (assign_index >= function->instruction_count) {
      continue;
    }
    assign = &function->instructions[assign_index];

    if (assign->op != IR_OP_ASSIGN ||
        assign->dest.kind != IR_OPERAND_SYMBOL || !assign->dest.name ||
        assign->lhs.kind != IR_OPERAND_TEMP || !assign->lhs.name ||
        strcmp(assign->lhs.name, load->dest.name) != 0) {
      continue;
    }

    /* A float ASSIGN may carry an IEEE-754 width contract (e.g. the inliner
     * tags a float32 parameter copy with float_bits=32 so a float64-tracked
     * argument is narrowed). Replacing the symbol's uses with the raw load
     * temp drops that conversion, so only fold when the loaded scalar already
     * has the assign's exact width (4-byte load for float32, 8 for float64) --
     * then the conversion is an identity and the copy is safe to elide. */
    if (assign->is_float) {
      long long width_bytes = (assign->float_bits == 32) ? 4 : 8;
      if (load->rhs.kind != IR_OPERAND_INT ||
          load->rhs.int_value != width_bytes) {
        continue;
      }
    }

    sym = assign->dest.name;
    temp = load->dest.name;

    /* An address-taken symbol can be read through memory the operand scan
     * below cannot see. */
    if (ir_symbol_address_taken(function, sym)) {
      continue;
    }

    for (j = 0; j < function->instruction_count; j++) {
      total_reads += ir_load_copy_count_symbol_reads(&function->instructions[j],
                                                     sym);
    }

    /* Scan the straight-line window after the assign. It ends at the first
     * control-flow instruction or the first re-write of the symbol. */
    for (j = assign_index + 1; j < function->instruction_count; j++) {
      const IRInstruction *ins = &function->instructions[j];
      if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
          ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
        window_end = j;
        break;
      }
      if (ir_instruction_writes_symbol(ins) &&
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        /* A re-write that also reads the symbol (`@s = @s + 1`) would read a
         * stale value once the copy is gone. */
        if (ir_load_copy_count_symbol_reads(ins, sym) > 0) {
          unsafe_use = 1;
        }
        window_end = j;
        break;
      }
      if (ins->op == IR_OP_STORE &&
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        /* Store-through-symbol addresses are left alone; folding around one
         * would leave a stale read. */
        unsafe_use = 1;
        break;
      }
      window_reads += ir_load_copy_count_symbol_reads(ins, sym);
    }

    if (unsafe_use || window_reads == 0 || window_reads > 6) {
      continue;
    }

    /* The symbol may be read outside the window: beyond the control-flow edge
     * that ended it, or earlier in a loop body (reading the previous
     * iteration's value). Either read would go stale once the copy is nop'd,
     * so only fold when the window accounts for every read in the function. */
    if (window_reads != total_reads) {
      continue;
    }

    for (j = assign_index + 1; j < window_end; j++) {
      IRInstruction *ins = &function->instructions[j];
      ir_load_copy_replace_operand(&ins->lhs, sym, temp);
      ir_load_copy_replace_operand(&ins->rhs, sym, temp);
      for (size_t a = 0; a < ins->argument_count; a++) {
        ir_load_copy_replace_operand(&ins->arguments[a], sym, temp);
      }
    }

    ir_instruction_make_nop(assign);
    if (changed) {
      *changed = 1;
    }
  }

  /* Folding a copy can leave its DECLARE_LOCAL dead (the inliner's parameter
   * local once every read is rewritten to the argument temp). A dead
   * declaration in a loop body still spoils the vectorizers' body-shape
   * matching and the --explain diagnosis, so sweep declarations whose symbol
   * no other instruction references. */
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *decl = &function->instructions[i];
    int referenced = 0;

    if (decl->op != IR_OP_DECLARE_LOCAL ||
        decl->dest.kind != IR_OPERAND_SYMBOL || !decl->dest.name) {
      continue;
    }

    for (size_t j = 0; j < function->instruction_count && !referenced; j++) {
      const IRInstruction *ins = &function->instructions[j];
      if (j == i || ins->op == IR_OP_NOP) {
        continue;
      }
      if (ir_operand_is_symbol_named(&ins->dest, decl->dest.name) ||
          ir_operand_is_symbol_named(&ins->lhs, decl->dest.name) ||
          ir_operand_is_symbol_named(&ins->rhs, decl->dest.name)) {
        referenced = 1;
        break;
      }
      for (size_t a = 0; a < ins->argument_count; a++) {
        if (ir_operand_is_symbol_named(&ins->arguments[a], decl->dest.name)) {
          referenced = 1;
          break;
        }
      }
    }

    if (!referenced) {
      ir_instruction_make_nop(decl);
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}
