#include "ir_optimize_internal.h"

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

static int ir_nullhoist_find_entry_test(const IRFunction *function,
                                        size_t header_index,
                                        size_t backedge_index,
                                        size_t *branch_index_out) {
  for (size_t k = header_index + 1; k < backedge_index; k++) {
    const IRInstruction *insn = &function->instructions[k];
    size_t exit_index = 0;
    if (insn->op == IR_OP_BRANCH_ZERO && insn->text &&
        ir_find_label_index(function, insn->text, &exit_index) &&
        exit_index > backedge_index) {
      *branch_index_out = k;
      return 1;
    }
    if (!ir_nullhoist_cloneable_cond_op(insn)) {
      return 0;
    }
  }
  return 0;
}

int ir_instruction_is_hoisted_null_check(const IRInstruction *insn) {
  static const char *const kPrefixes[] = {"ir_nullhoist_skip_", "ir_trap_null_",
                                          "ir_nonnull_"};
  if (insn->op == IR_OP_CALL) {
    return insn->text && strcmp(insn->text, "mettle_crash_trap_ex") == 0;
  }
  if ((insn->op != IR_OP_LABEL && insn->op != IR_OP_JUMP &&
       insn->op != IR_OP_BRANCH_ZERO) ||
      !insn->text) {
    return 0;
  }
  for (size_t p = 0; p < sizeof(kPrefixes) / sizeof(kPrefixes[0]); p++) {
    if (strncmp(insn->text, kPrefixes[p], strlen(kPrefixes[p])) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_nullhoist_quiet_op(const IRInstruction *insn) {
  switch (insn->op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_JUMP:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_ASSIGN:
  case IR_OP_UNARY:
  case IR_OP_LOAD:
  case IR_OP_CAST:
  case IR_OP_ADDRESS_OF:
  case IR_OP_SELECT:
  case IR_OP_DECLARE_LOCAL:
    return 1;
  case IR_OP_BINARY:
    return insn->is_float || !insn->text ||
           (strcmp(insn->text, "/") != 0 && strcmp(insn->text, "%") != 0);
  default:
    return 0;
  }
}

static int ir_nullhoist_reached_first(const IRFunction *function,
                                      size_t entry_branch, size_t check) {
  for (size_t k = entry_branch + 1; k < check; k++) {
    const IRInstruction *insn = &function->instructions[k];
    if (!ir_nullhoist_quiet_op(insn)) {
      return 0;
    }
    size_t target = 0;
    int transfers = insn->op == IR_OP_JUMP || insn->op == IR_OP_BRANCH_ZERO ||
                    insn->op == IR_OP_BRANCH_EQ;
    if (transfers &&
        (!ir_find_label_index(function, insn->text, &target) ||
         target <= k || target >= check)) {
      return 0;
    }
  }
  return 1;
}

static void ir_nullhoist_drop_dominated(IRFunction *function, size_t from,
                                        size_t to, const char *symbol_name) {
  for (size_t k = from; k < to; k++) {
    size_t end = 0;
    const char *checked = NULL;
    if (!ir_match_null_trap_diamond(function, k, &end, &checked) ||
        end >= to || strcmp(checked, symbol_name) != 0) {
      continue;
    }
    for (size_t z = k; z <= end; z++) {
      ir_instruction_make_nop(&function->instructions[z]);
    }
    k = end;
  }
}

int ir_null_check_licm_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *header = &function->instructions[i];
    if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
      continue;
    }
    const char *loop_label = header->text;

    size_t backedge_index = ir_function_first_jump_to(function, i, loop_label);
    if (backedge_index == IR_BLOCK_NONE) {
      backedge_index = (size_t)-1;
    }
    if (backedge_index == (size_t)-1) {
      continue;
    }

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

      size_t entry_branch_index = 0;
      if (!ir_nullhoist_find_entry_test(function, i, backedge_index,
                                        &entry_branch_index)) {
        continue;
      }
      if (entry_branch_index >= j) {
        continue;
      }
      if (!ir_nullhoist_reached_first(function, entry_branch_index, j)) {
        continue;
      }
      size_t cond_span = entry_branch_index - i - 1;
      size_t span = diamond_end - j + 1;
      if (span > 16 || cond_span > 16) {
        continue;
      }

      static size_t nullhoist_label_counter = 0;
      char skip_label[48];
      snprintf(skip_label, sizeof(skip_label), "ir_nullhoist_skip_%zu",
               nullhoist_label_counter++);

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

      for (size_t k = 0; k < span; k++) {
        ir_instruction_make_nop(&function->instructions[j + k]);
      }
      ir_nullhoist_drop_dominated(function, i + 1, backedge_index,
                                  symbol_name);

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

      i = i + total - 1;
      break;
    }

    (void)hoisted_this_loop;
  }

  return 1;
}
