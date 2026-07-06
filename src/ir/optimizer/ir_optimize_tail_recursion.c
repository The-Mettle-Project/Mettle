#include "ir_optimize_internal.h"

/* Tail-recursion elimination. Converts a function's direct self tail calls
 * into parameter rebinding plus a jump back to the function entry, so each
 * eliminated call site costs a loop iteration instead of a stack frame.
 *
 * Three shapes are handled, matched against the exact lowering output
 * (including the empty errdefer diamond emitted before every return):
 *
 *   1. `return self(args)`            -> rebind params, jump entry
 *   2. `self(args); return`  (void)   -> rebind params, jump entry
 *   3. `return E1 + E2 + self(args)`  -> acc += E1 + E2, rebind, jump entry
 *
 * Shape 3 introduces a hidden int64 accumulator initialized to 0 at entry;
 * every OTHER return in the function is rewritten to `return acc + value`.
 * Reordering the additions (accumulating on the way down instead of on the
 * unwind) relies on integer + being associative and commutative, so shape 3
 * is refused for float adds. Runs before the inliner so the remaining
 * non-tail self calls still get bounded self-recursion expansion on the
 * already-looped body. */

typedef struct {
  size_t call_index;
  size_t return_index;
  /* Instruction indices consumed by this site (call, chain adds, diamond,
   * return): replaced by the rebind+jump sequence. */
  size_t first_index;
  size_t last_index;
  /* Accumulator terms: the non-call operand of each chain add, in order. */
  IROperand chain_terms[8];
  /* Dest temp of each chain add (for the outside-use soundness check). */
  const char *chain_dests[8];
  size_t chain_term_count;
} IRTailSite;

#define IR_TRE_MAX_SITES 8

static int ir_tre_operand_is_temp_named(const IROperand *operand,
                                        const char *name) {
  return operand && name && operand->kind == IR_OPERAND_TEMP &&
         operand->name && strcmp(operand->name, name) == 0;
}

static size_t ir_tre_skip_nops(const IRFunction *function, size_t index) {
  while (index < function->instruction_count &&
         function->instructions[index].op == IR_OP_NOP) {
    index++;
  }
  return index;
}

/* Count uses of a temp across the whole function, excluding instructions in
 * [first, last] (the candidate site region). */
static size_t ir_tre_temp_uses_outside(const IRFunction *function,
                                       const char *temp_name, size_t first,
                                       size_t last) {
  size_t uses = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (i >= first && i <= last) {
      continue;
    }
    const IRInstruction *ins = &function->instructions[i];
    if (ir_tre_operand_is_temp_named(&ins->lhs, temp_name) ||
        ir_tre_operand_is_temp_named(&ins->rhs, temp_name)) {
      uses++;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_tre_operand_is_temp_named(&ins->arguments[a], temp_name)) {
        uses++;
      }
    }
  }
  return uses;
}

static int ir_tre_label_referenced_outside(const IRFunction *function,
                                           const char *label, size_t first,
                                           size_t last) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (i >= first && i <= last) {
      continue;
    }
    const IRInstruction *ins = &function->instructions[i];
    if ((ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
         ins->op == IR_OP_BRANCH_EQ) &&
        ins->text && strcmp(ins->text, label) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Match the empty errdefer diamond the lowerer emits before every return:
 *   ASSIGN tX <- <anything>
 *   BRANCH_ZERO tX -> Lok
 *   JUMP Lend
 *   LABEL Lok
 *   LABEL Lend
 * Returns the index just past the diamond, or `index` unchanged if the shape
 * is not present. A diamond with real errdefer handlers has code between the
 * labels and will not match, which correctly refuses the transform. */
static size_t ir_tre_skip_errdefer_diamond(const IRFunction *function,
                                           size_t index) {
  size_t i = ir_tre_skip_nops(function, index);
  if (i + 4 >= function->instruction_count) {
    return index;
  }
  const IRInstruction *assign = &function->instructions[i];
  const IRInstruction *branch = &function->instructions[i + 1];
  const IRInstruction *jump = &function->instructions[i + 2];
  const IRInstruction *label_ok = &function->instructions[i + 3];
  const IRInstruction *label_end = &function->instructions[i + 4];

  if (assign->op != IR_OP_ASSIGN || assign->dest.kind != IR_OPERAND_TEMP ||
      !assign->dest.name) {
    return index;
  }
  if (branch->op != IR_OP_BRANCH_ZERO || !branch->text ||
      !ir_tre_operand_is_temp_named(&branch->lhs, assign->dest.name)) {
    return index;
  }
  if (jump->op != IR_OP_JUMP || !jump->text) {
    return index;
  }
  if (label_ok->op != IR_OP_LABEL || !label_ok->text ||
      strcmp(label_ok->text, branch->text) != 0) {
    return index;
  }
  if (label_end->op != IR_OP_LABEL || !label_end->text ||
      strcmp(label_end->text, jump->text) != 0) {
    return index;
  }
  /* The guard temp must not be read anywhere else. */
  if (ir_tre_temp_uses_outside(function, assign->dest.name, i, i + 4) != 0) {
    return index;
  }
  /* The diamond's labels must not be jump targets from elsewhere. */
  if (ir_tre_label_referenced_outside(function, label_ok->text, i, i + 4) ||
      ir_tre_label_referenced_outside(function, label_end->text, i, i + 4)) {
    return index;
  }
  return i + 5;
}

/* Try to match a tail site starting at the self call at `call_index`.
 * On success fills `site` and returns 1. */
static int ir_tre_match_site(const IRFunction *function, size_t call_index,
                             IRTailSite *site) {
  const IRInstruction *call = &function->instructions[call_index];
  const char *traced = (call->dest.kind == IR_OPERAND_TEMP && call->dest.name)
                           ? call->dest.name
                           : NULL;

  memset(site, 0, sizeof(*site));
  site->call_index = call_index;
  site->first_index = call_index;

  size_t i = ir_tre_skip_nops(function, call_index + 1);

  /* Integer add chain folding the call result toward the return value. */
  while (traced && i < function->instruction_count) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_BINARY || ins->is_float || !ins->text ||
        strcmp(ins->text, "+") != 0 || ins->dest.kind != IR_OPERAND_TEMP ||
        !ins->dest.name) {
      break;
    }
    const IROperand *other = NULL;
    if (ir_tre_operand_is_temp_named(&ins->lhs, traced)) {
      other = &ins->rhs;
    } else if (ir_tre_operand_is_temp_named(&ins->rhs, traced)) {
      other = &ins->lhs;
    } else {
      break;
    }
    if (other->kind != IR_OPERAND_INT && other->kind != IR_OPERAND_TEMP &&
        other->kind != IR_OPERAND_SYMBOL) {
      return 0;
    }
    if (site->chain_term_count >= IR_TRE_MAX_SITES) {
      return 0;
    }
    site->chain_terms[site->chain_term_count] = *other;
    site->chain_dests[site->chain_term_count] = ins->dest.name;
    site->chain_term_count++;
    traced = ins->dest.name;
    i = ir_tre_skip_nops(function, i + 1);
  }

  size_t after_diamond = ir_tre_skip_errdefer_diamond(function, i);
  size_t ret_index = ir_tre_skip_nops(function, after_diamond);
  if (ret_index >= function->instruction_count ||
      function->instructions[ret_index].op != IR_OP_RETURN) {
    return 0;
  }
  const IRInstruction *ret = &function->instructions[ret_index];

  if (site->chain_term_count > 0) {
    /* Shape 3: the chain's final temp must be exactly what is returned. */
    if (!ir_tre_operand_is_temp_named(&ret->lhs, traced)) {
      return 0;
    }
  } else if (traced && ir_tre_operand_is_temp_named(&ret->lhs, traced)) {
    /* Shape 1: return self(args). */
  } else if (ret->lhs.kind == IR_OPERAND_NONE) {
    /* Shape 2: void self call falling through to `return`. */
  } else {
    return 0;
  }

  /* Soundness: the call result and every chain temp must be consumed only
   * inside this site region - a reader elsewhere would observe a value the
   * rewrite no longer computes. */
  if (call->dest.kind == IR_OPERAND_TEMP && call->dest.name &&
      ir_tre_temp_uses_outside(function, call->dest.name, call_index,
                               ret_index) != 0) {
    return 0;
  }
  for (size_t t = 0; t < site->chain_term_count; t++) {
    if (ir_tre_temp_uses_outside(function, site->chain_dests[t], call_index,
                                 ret_index) != 0) {
      return 0;
    }
  }

  site->return_index = ret_index;
  site->last_index = ret_index;
  return 1;
}

/* A base-case return operand the accumulator rewrite can widen: an integer
 * constant, a symbol with a known integer declared type (param or local), or
 * a temp produced by a non-float instruction. */
static int ir_tre_return_operand_is_integer(const IRFunction *function,
                                            size_t return_index) {
  const IROperand *operand = &function->instructions[return_index].lhs;
  if (operand->kind == IR_OPERAND_INT) {
    return 1;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    const char *type = ir_function_local_declared_type(
        (IRFunction *)function, operand->name);
    if (!type) {
      for (size_t p = 0; p < function->parameter_count; p++) {
        if (function->parameter_names[p] &&
            strcmp(function->parameter_names[p], operand->name) == 0) {
          type = function->parameter_types[p];
          break;
        }
      }
    }
    if (!type) {
      return 0;
    }
    return strcmp(type, "int8") == 0 || strcmp(type, "int16") == 0 ||
           strcmp(type, "int32") == 0 || strcmp(type, "int64") == 0 ||
           strcmp(type, "uint8") == 0 || strcmp(type, "uint16") == 0 ||
           strcmp(type, "uint32") == 0 || strcmp(type, "uint64") == 0 ||
           strcmp(type, "bool") == 0;
  }
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    for (size_t i = return_index; i > 0;) {
      i--;
      const IRInstruction *ins = &function->instructions[i];
      if (ir_instruction_writes_temp(ins) && ins->dest.name &&
          strcmp(ins->dest.name, operand->name) == 0) {
        return !ins->is_float;
      }
    }
    return 0;
  }
  return 0;
}

static int ir_tre_site_contains(const IRTailSite *sites, size_t site_count,
                                size_t index, const IRTailSite **out) {
  for (size_t s = 0; s < site_count; s++) {
    if (index >= sites[s].first_index && index <= sites[s].last_index) {
      if (out) {
        *out = &sites[s];
      }
      return 1;
    }
  }
  return 0;
}

static int ir_tre_emit(IRInstructionVector *vector, IRInstruction *ins) {
  if (!ir_instruction_vector_append_move(vector, ins)) {
    ir_instruction_destroy_storage(ins);
    return 0;
  }
  return 1;
}

static int ir_tre_rewrite_function(IRFunction *function,
                                   const IRTailSite *sites, size_t site_count,
                                   int use_acc, int *changed) {
  char loop_label[160];
  char acc_name[160];
  IRInstructionVector vector = {0};
  int ok = 0;

  snprintf(loop_label, sizeof(loop_label), "__tre_loop_%s", function->name);
  snprintf(acc_name, sizeof(acc_name), "__tre_acc_%s", function->name);

  size_t start = 0;
  /* Keep a leading entry label ahead of the preamble. */
  if (function->instruction_count > 0 &&
      function->instructions[0].op == IR_OP_LABEL) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[0], &cloned) ||
        !ir_tre_emit(&vector, &cloned)) {
      goto done;
    }
    start = 1;
  }

  if (use_acc) {
    IRInstruction decl = {0};
    IRInstruction init = {0};
    decl.op = IR_OP_DECLARE_LOCAL;
    decl.dest = ir_operand_symbol(acc_name);
    decl.text = mettle_strdup("int64");
    init.op = IR_OP_ASSIGN;
    init.dest = ir_operand_symbol(acc_name);
    init.lhs = ir_operand_int(0);
    if (!decl.dest.name || !decl.text || !init.dest.name ||
        !ir_tre_emit(&vector, &decl)) {
      ir_instruction_destroy_storage(&decl);
      ir_instruction_destroy_storage(&init);
      goto done;
    }
    if (!ir_tre_emit(&vector, &init)) {
      ir_instruction_destroy_storage(&init);
      goto done;
    }
  }

  {
    IRInstruction loop = {0};
    loop.op = IR_OP_LABEL;
    loop.text = mettle_strdup(loop_label);
    if (!loop.text || !ir_tre_emit(&vector, &loop)) {
      ir_instruction_destroy_storage(&loop);
      goto done;
    }
  }

  for (size_t i = start; i < function->instruction_count; i++) {
    const IRTailSite *site = NULL;
    if (ir_tre_site_contains(sites, site_count, i, &site)) {
      if (i != site->call_index) {
        continue; /* rest of the site region is dropped */
      }
      const IRInstruction *call = &function->instructions[site->call_index];

      /* acc += chain terms (they were computed before the call and are still
       * live; the callee cannot touch them - no address escapes). */
      for (size_t t = 0; t < site->chain_term_count; t++) {
        IRInstruction add = {0};
        add.op = IR_OP_BINARY;
        add.location = call->location;
        add.text = mettle_strdup("+");
        add.dest = ir_operand_symbol(acc_name);
        add.lhs = ir_operand_symbol(acc_name);
        if (!ir_operand_clone(&site->chain_terms[t], &add.rhs) || !add.text ||
            !add.dest.name || !add.lhs.name || !ir_tre_emit(&vector, &add)) {
          ir_instruction_destroy_storage(&add);
          goto done;
        }
      }

      /* Copy args to fresh temps first, then rebind the params, so a swapped
       * or shifted argument list never reads an already-overwritten param. */
      for (size_t a = 0; a < call->argument_count; a++) {
        char temp_name[64];
        snprintf(temp_name, sizeof(temp_name), ".tre%zu_%zu", site->call_index,
                 a);
        IRInstruction copy = {0};
        copy.op = IR_OP_ASSIGN;
        copy.location = call->location;
        copy.dest = ir_operand_temp(temp_name);
        if (!ir_operand_clone(&call->arguments[a], &copy.lhs) ||
            !copy.dest.name || !ir_tre_emit(&vector, &copy)) {
          ir_instruction_destroy_storage(&copy);
          goto done;
        }
      }
      for (size_t a = 0; a < call->argument_count; a++) {
        char temp_name[64];
        snprintf(temp_name, sizeof(temp_name), ".tre%zu_%zu", site->call_index,
                 a);
        IRInstruction bind = {0};
        bind.op = IR_OP_ASSIGN;
        bind.location = call->location;
        bind.dest = ir_operand_symbol(function->parameter_names[a]);
        bind.lhs = ir_operand_temp(temp_name);
        if (!bind.dest.name || !bind.lhs.name || !ir_tre_emit(&vector, &bind)) {
          ir_instruction_destroy_storage(&bind);
          goto done;
        }
      }

      IRInstruction jump = {0};
      jump.op = IR_OP_JUMP;
      jump.location = call->location;
      jump.text = mettle_strdup(loop_label);
      if (!jump.text || !ir_tre_emit(&vector, &jump)) {
        ir_instruction_destroy_storage(&jump);
        goto done;
      }
      continue;
    }

    if (use_acc && function->instructions[i].op == IR_OP_RETURN &&
        function->instructions[i].lhs.kind != IR_OPERAND_NONE) {
      char temp_name[64];
      snprintf(temp_name, sizeof(temp_name), ".treret%zu", i);
      IRInstruction add = {0};
      IRInstruction ret = {0};
      add.op = IR_OP_BINARY;
      add.location = function->instructions[i].location;
      add.text = mettle_strdup("+");
      add.dest = ir_operand_temp(temp_name);
      add.lhs = ir_operand_symbol(acc_name);
      if (!ir_operand_clone(&function->instructions[i].lhs, &add.rhs) ||
          !add.text || !add.dest.name || !add.lhs.name ||
          !ir_tre_emit(&vector, &add)) {
        ir_instruction_destroy_storage(&add);
        goto done;
      }
      ret.op = IR_OP_RETURN;
      ret.location = function->instructions[i].location;
      ret.lhs = ir_operand_temp(temp_name);
      if (!ret.lhs.name || !ir_tre_emit(&vector, &ret)) {
        ir_instruction_destroy_storage(&ret);
        goto done;
      }
      continue;
    }

    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_tre_emit(&vector, &cloned)) {
      goto done;
    }
  }

  if (!ir_function_replace_instructions(function, &vector)) {
    goto done;
  }
  if (changed) {
    *changed = 1;
  }
  ok = 1;

done:
  ir_instruction_vector_destroy(&vector);
  return ok;
}

static int ir_tre_function(IRFunction *function, int *changed) {
  if (!function || !function->name || function->instruction_count == 0 ||
      function->parameter_count == 0) {
    return 1;
  }

  /* If any local or parameter has its address taken, frames are observable
   * and merging them into one loop body is unsound. */
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_INLINE_ASM) {
      return 1;
    }
    if (ins->op == IR_OP_ADDRESS_OF && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ins->lhs.name) {
      if (ir_function_local_declared_type(function, ins->lhs.name) ||
          ir_function_symbol_is_parameter(function, ins->lhs.name)) {
        return 1;
      }
    }
  }

  IRTailSite sites[IR_TRE_MAX_SITES];
  size_t site_count = 0;
  int use_acc = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_CALL || !ins->text ||
        strcmp(ins->text, function->name) != 0 ||
        ins->argument_count != function->parameter_count) {
      continue;
    }
    if (site_count >= IR_TRE_MAX_SITES) {
      break;
    }
    if (ir_tre_match_site(function, i, &sites[site_count])) {
      if (sites[site_count].chain_term_count > 0) {
        use_acc = 1;
      }
      site_count++;
    }
  }

  if (site_count == 0) {
    return 1;
  }

  if (use_acc) {
    /* Every return NOT belonging to a matched site must be rewritable as
     * `return acc + value`, which requires an integer value. */
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (function->instructions[i].op != IR_OP_RETURN ||
          ir_tre_site_contains(sites, site_count, i, NULL)) {
        continue;
      }
      if (function->instructions[i].lhs.kind == IR_OPERAND_NONE ||
          !ir_tre_return_operand_is_integer(function, i)) {
        return 1; /* refuse the whole transform */
      }
    }
  }

  return ir_tre_rewrite_function(function, sites, site_count, use_acc,
                                 changed);
}

int ir_tail_recursion_elimination_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }
  for (size_t f = 0; f < program->function_count; f++) {
    if (!ir_tre_function(program->functions[f], changed)) {
      return 0;
    }
  }
  return 1;
}
