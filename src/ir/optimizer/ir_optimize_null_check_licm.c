#include "ir_optimize_internal.h"

/* An instruction that may be duplicated to re-evaluate the loop-entry
 * condition ahead of the loop: no side effects, no control flow. */
static int ir_nullhoist_cloneable_cond_op(const IRInstruction *insn) {
  switch (insn->op) {
  case IR_OP_NOP:
  case IR_OP_ASSIGN:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_LOAD:
  case IR_OP_CAST:
  case IR_OP_ADDRESS_OF:
    return 1;
  default:
    return 0;
  }
}

/* Locate the loop's entry test: the first branch_zero after the header label
 * that exits to an "ir_while_end"-style label, with only cloneable condition
 * ops in between. Returns 1 with the branch index. */
static int ir_nullhoist_find_entry_test(const IRFunction *function,
                                        size_t header_index,
                                        size_t backedge_index,
                                        size_t *branch_index_out) {
  for (size_t k = header_index + 1; k < backedge_index; k++) {
    const IRInstruction *insn = &function->instructions[k];
    if (insn->op == IR_OP_BRANCH_ZERO && insn->text &&
        strncmp(insn->text, "ir_while_end", 12) == 0) {
      *branch_index_out = k;
      return 1;
    }
    if (!ir_nullhoist_cloneable_cond_op(insn)) {
      return 0;
    }
  }
  return 0;
}

int ir_null_check_licm_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *header = &function->instructions[i];
    if (header->op != IR_OP_LABEL || !header->text ||
        strncmp(header->text, "ir_while_", 9) != 0) {
      continue;
    }
    const char *loop_label = header->text;

    /* Find the back-edge jump. */
    size_t backedge_index = (size_t)-1;
    for (size_t j = i + 1; j < function->instruction_count; j++) {
      const IRInstruction *probe = &function->instructions[j];
      if (probe->op == IR_OP_JUMP && probe->text &&
          strcmp(probe->text, loop_label) == 0) {
        backedge_index = j;
        break;
      }
    }
    if (backedge_index == (size_t)-1) {
      continue;
    }

    /* Walk the loop body looking for null-trap diamonds. Scan repeatedly so
     * that hoisting one diamond exposes the next one (the body shrinks). */
    int hoisted_this_loop = 0;
    for (size_t j = i + 1; j + 4 < backedge_index; j++) {
      size_t diamond_end = 0;
      const char *symbol_name = NULL;
      if (!ir_match_null_trap_diamond(function, j, &diamond_end,
                                      &symbol_name)) {
        continue;
      }
      if (diamond_end >= backedge_index) {
        continue;
      }

      /* The symbol must not be modified anywhere in the body, excluding the
       * diamond itself. Conservatively, exclude *all* identified diamonds for
       * the same symbol; but the helper just checks plain writes/stores/calls,
       * and the diamond contains a call we have to ignore. Easiest: temporarily
       * blank the diamond's call op for the check. We instead bound the scan
       * to skip the diamond range. */
      /* Stack symbols whose address is never taken cannot be written by a
       * call or by a store through an unrelated pointer. So we only need to
       * worry about other instructions that name @symbol as their dest. */
      int address_taken = ir_symbol_address_taken(function, symbol_name);
      int safe = 1;
      for (size_t k = i + 1; k < backedge_index; k++) {
        if (k >= j && k <= diamond_end) {
          continue;
        }
        const IRInstruction *inst = &function->instructions[k];
        if (ir_instruction_writes_symbol(inst) && inst->dest.name &&
            strcmp(inst->dest.name, symbol_name) == 0) {
          safe = 0;
          break;
        }
        if (address_taken && inst->op == IR_OP_STORE) {
          safe = 0;
          break;
        }
        if (address_taken &&
            (inst->op == IR_OP_CALL || inst->op == IR_OP_CALL_INDIRECT)) {
          safe = 0;
          break;
        }
      }
      if (!safe) {
        continue;
      }

      /* Soundness: the trap must not fire for a loop that executes zero
       * iterations (the original program never evaluates the check then).
       * Guard the hoisted diamond with a re-evaluation of the loop's entry
       * condition: only hoist when the header's test is a cloneable pure
       * computation, and branch past the check when the loop wouldn't run.
       * (Found by --verify: unguarded hoisting trapped on null pointer +
       * zero-trip loop, a state the original program tolerates.) */
      size_t entry_branch_index = 0;
      if (!ir_nullhoist_find_entry_test(function, i, backedge_index,
                                        &entry_branch_index)) {
        continue;
      }
      if (entry_branch_index >= j) {
        continue;
      }
      /* The check must execute on EVERY iteration to be hoistable: require a
       * straight line from the loop entry test to the diamond - any label or
       * branch in between means the check can sit under a condition (e.g.
       * `if (keep) { buf[i] = x; }`), and hoisting it would trap on
       * iterations that never reach it. (Found by --verify.) */
      int straight_line = 1;
      for (size_t k = entry_branch_index + 1; k < j; k++) {
        switch (function->instructions[k].op) {
        case IR_OP_LABEL:
        case IR_OP_JUMP:
        case IR_OP_BRANCH_ZERO:
        case IR_OP_BRANCH_EQ:
          straight_line = 0;
          break;
        default:
          break;
        }
        if (!straight_line) {
          break;
        }
      }
      if (!straight_line) {
        continue;
      }
      size_t cond_span = entry_branch_index - i - 1; /* label+1 .. branch-1 */
      size_t span = diamond_end - j + 1;
      if (span > 16 || cond_span > 16) {
        continue;
      }

      /* Fresh skip label for the guard. */
      static size_t nullhoist_label_counter = 0;
      char skip_label[48];
      snprintf(skip_label, sizeof(skip_label), "ir_nullhoist_skip_%zu",
               nullhoist_label_counter++);

      /* Build the insertion: [cond clones][guard branch][diamond][skip label]. */
      IRInstruction snapshot[35];
      size_t total = 0;
      int build_failed = 0;
      for (size_t k = 0; k < cond_span && !build_failed; k++) {
        if (!ir_clone_instruction_plain(&function->instructions[i + 1 + k],
                                        &snapshot[total])) {
          build_failed = 1;
          break;
        }
        total++;
      }
      if (!build_failed) {
        IRInstruction *guard = &snapshot[total];
        memset(guard, 0, sizeof(*guard));
        guard->op = IR_OP_BRANCH_ZERO;
        guard->location = function->instructions[entry_branch_index].location;
        guard->lhs =
            ir_operand_copy(&function->instructions[entry_branch_index].lhs);
        guard->text = mettle_strdup(skip_label);
        if (!guard->text) {
          ir_instruction_destroy_storage(guard);
          build_failed = 1;
        } else {
          total++;
        }
      }
      for (size_t k = 0; k < span && !build_failed; k++) {
        if (!ir_clone_instruction_plain(&function->instructions[j + k],
                                        &snapshot[total])) {
          build_failed = 1;
          break;
        }
        total++;
      }
      if (!build_failed) {
        IRInstruction *skip = &snapshot[total];
        memset(skip, 0, sizeof(*skip));
        skip->op = IR_OP_LABEL;
        skip->location = function->instructions[i].location;
        skip->text = mettle_strdup(skip_label);
        if (!skip->text) {
          ir_instruction_destroy_storage(skip);
          build_failed = 1;
        } else {
          total++;
        }
      }
      if (build_failed) {
        for (size_t z = 0; z < total; z++) {
          ir_instruction_destroy_storage(&snapshot[z]);
        }
        return 0;
      }

      /* NOP the original diamond first; this preserves the instruction array
       * layout so 'i' (header index) remains valid as long as we insert at i. */
      for (size_t k = 0; k < span; k++) {
        ir_instruction_make_nop(&function->instructions[j + k]);
      }

      /* Insert before the header. We have to grow the array. */
      if (function->instruction_count + total >
          function->instruction_capacity) {
        size_t new_cap = function->instruction_capacity
                             ? function->instruction_capacity * 2
                             : 64;
        while (new_cap < function->instruction_count + total) {
          new_cap *= 2;
        }
        IRInstruction *grown = realloc(function->instructions,
                                       new_cap * sizeof(IRInstruction));
        if (!grown) {
          for (size_t z = 0; z < total; z++) {
            ir_instruction_destroy_storage(&snapshot[z]);
          }
          return 0;
        }
        function->instructions = grown;
        function->instruction_capacity = new_cap;
      }

      /* Shift instructions [i, count) to [i+total, count+total). */
      memmove(&function->instructions[i + total], &function->instructions[i],
              (function->instruction_count - i) * sizeof(IRInstruction));
      for (size_t k = 0; k < total; k++) {
        function->instructions[i + k] = snapshot[k];
      }
      function->instruction_count += total;

      hoisted_this_loop = 1;
      if (changed) {
        *changed = 1;
      }

      /* The header label has shifted from index i to i+total. Re-run the outer
       * for-loop iteration at the new header position by setting i = i+total-1
       * (the loop's ++ will land on the header again, exposing any further
       * hoistable diamonds in the same loop). */
      i = i + total - 1;
      break;
    }

    (void)hoisted_this_loop;
  }

  return 1;
}
