#include "ir_optimize_internal.h"

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

      /* Clone the diamond instructions, then NOP out the originals, then
       * insert the clones before the header. We work in two phases: first
       * snapshot the operand data we need (operands borrow pointers), then
       * mutate. */
      IRInstruction snapshot[16];
      size_t span = diamond_end - j + 1;
      if (span > 16) {
        continue;
      }
      for (size_t k = 0; k < span; k++) {
        if (!ir_clone_instruction_plain(&function->instructions[j + k],
                                        &snapshot[k])) {
          for (size_t z = 0; z < k; z++) {
            ir_instruction_destroy_storage(&snapshot[z]);
          }
          return 0;
        }
      }

      /* NOP the originals first; this preserves the instruction array layout
       * so 'i' (header index) remains valid as long as we insert at i. */
      for (size_t k = 0; k < span; k++) {
        ir_instruction_make_nop(&function->instructions[j + k]);
      }

      /* Insert the snapshot before the header. We have to grow the array. */
      if (function->instruction_count + span >
          function->instruction_capacity) {
        size_t new_cap = function->instruction_capacity
                             ? function->instruction_capacity * 2
                             : 64;
        while (new_cap < function->instruction_count + span) {
          new_cap *= 2;
        }
        IRInstruction *grown = realloc(function->instructions,
                                       new_cap * sizeof(IRInstruction));
        if (!grown) {
          for (size_t z = 0; z < span; z++) {
            ir_instruction_destroy_storage(&snapshot[z]);
          }
          return 0;
        }
        function->instructions = grown;
        function->instruction_capacity = new_cap;
      }

      /* Shift instructions [i, count) to [i+span, count+span). */
      memmove(&function->instructions[i + span], &function->instructions[i],
              (function->instruction_count - i) * sizeof(IRInstruction));
      for (size_t k = 0; k < span; k++) {
        function->instructions[i + k] = snapshot[k];
      }
      function->instruction_count += span;

      hoisted_this_loop = 1;
      if (changed) {
        *changed = 1;
      }

      /* The header label has shifted from index i to i+span. Re-run the outer
       * for-loop iteration at the new header position by setting i = i+span-1
       * (the loop's ++ will land on the header again, exposing any further
       * hoistable diamonds in the same loop). */
      i = i + span - 1;
      break;
    }

    (void)hoisted_this_loop;
  }

  return 1;
}
