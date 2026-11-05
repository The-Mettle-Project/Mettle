#include "../common.h"
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_SSA_OPT_MAX_ROUNDS 16

static unsigned long long g_ssa_trivial_phis = 0;
static unsigned long long g_ssa_dead_phis = 0;
static unsigned long long g_ssa_uses_rewritten = 0;
static unsigned long long g_ssa_phis_repaired = 0;
static unsigned long long g_ssa_repair_bailouts = 0;

static int ir_ssa_opt_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *setting = getenv("METTLE_SSA_OPT");
    cached = (setting && *setting && strcmp(setting, "0") == 0) ? 0 : 1;
  }
  return cached;
}

static int ir_ssa_opt_stats_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_SSA_STATS") ? 1 : 0;
  }
  return cached;
}

void ir_ssa_opt_report_stats(void) {
  if (!ir_ssa_opt_stats_enabled()) {
    return;
  }
  fprintf(stderr, "SSA-OPT\ttrivial_phis\t%llu\n", g_ssa_trivial_phis);
  fprintf(stderr, "SSA-OPT\tdead_phis\t%llu\n", g_ssa_dead_phis);
  fprintf(stderr, "SSA-OPT\tuses_rewritten\t%llu\n", g_ssa_uses_rewritten);
  fprintf(stderr, "SSA-OPT\tphis_repaired\t%llu\n", g_ssa_phis_repaired);
  fprintf(stderr, "SSA-OPT\trepair_bailouts\t%llu\n", g_ssa_repair_bailouts);
}

static int ir_ssa_opt_function_is_eligible(const IRFunction *function,
                                           size_t *phi_count_out) {
  size_t phis = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IROpcode op = function->instructions[i].op;
    if (op == IR_OP_INLINE_ASM) {
      return 0;
    }
    if (op == IR_OP_PHI) {
      phis++;
    }
  }
  if (phi_count_out) {
    *phi_count_out = phis;
  }
  return phis > 0;
}

static int ir_ssa_opt_operand_is_versioned(const IRFunction *function,
                                           const IROperand *operand) {
  if (!operand || !ir_operand_is_value(operand) ||
      operand->value_id == IR_VALUE_ID_NONE) {
    return 0;
  }
  const char *name = ir_value_table_name(&function->values, operand->value_id);
  return name && strstr(name, "__ssa") != NULL;
}

static void ir_ssa_opt_write_operand(IRFunction *function, IROperand *operand,
                                     uint32_t id) {
  const char *name = ir_value_table_name(&function->values, id);
  if (!name) {
    return;
  }
  const unsigned char kind = ir_value_table_kind(&function->values, id);
  const int float_bits = operand->float_bits;
  ir_operand_destroy(operand);
  *operand = (kind == IR_OPERAND_TEMP) ? ir_operand_temp(name)
                                       : ir_operand_symbol(name);
  operand->float_bits = float_bits;
  operand->value_id = id;
}

static IROperand *ir_ssa_opt_operand_at(IRInstruction *instruction,
                                        size_t index) {
  if (index == 0) {
    return &instruction->dest;
  }
  if (index == 1) {
    return &instruction->lhs;
  }
  if (index == 2) {
    return &instruction->rhs;
  }
  if (instruction->arguments && index - 3 < instruction->argument_count) {
    return &instruction->arguments[index - 3];
  }
  return NULL;
}

static uint32_t ir_ssa_opt_resolve(const uint32_t *replacement, uint32_t id,
                                   size_t count) {
  uint32_t current = id;
  for (size_t step = 0; step < count && current < count; step++) {
    const uint32_t next = replacement[current];
    if (next == IR_VALUE_ID_NONE || next == current) {
      return current;
    }
    current = next;
  }
  return current;
}

static uint32_t ir_ssa_opt_sole_incoming(const IRInstruction *phi,
                                         uint32_t self, size_t value_count) {
  uint32_t only = IR_VALUE_ID_NONE;
  for (size_t p = 0; p * 2 < phi->argument_count; p++) {
    const IROperand *incoming = &phi->arguments[p * 2];
    if (incoming->kind == IR_OPERAND_NONE) {
      continue;
    }
    if (!ir_operand_is_value(incoming) ||
        incoming->value_id == IR_VALUE_ID_NONE) {
      return IR_VALUE_ID_NONE;
    }
    if (incoming->value_id == self) {
      continue;
    }
    if (only == IR_VALUE_ID_NONE) {
      only = incoming->value_id;
    } else if (only != incoming->value_id) {
      return IR_VALUE_ID_NONE;
    }
  }
  return only < value_count ? only : IR_VALUE_ID_NONE;
}

static size_t ir_ssa_opt_mark_trivial_phis(IRFunction *function,
                                           size_t value_count,
                                           uint32_t *replacement,
                                           unsigned char *drop) {
  size_t eliminated = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *phi = &function->instructions[i];
    if (phi->op != IR_OP_PHI || !phi->arguments) {
      continue;
    }
    const uint32_t self = phi->dest.value_id;
    if (self == IR_VALUE_ID_NONE || self >= value_count) {
      continue;
    }
    const uint32_t only = ir_ssa_opt_sole_incoming(phi, self, value_count);
    if (only == IR_VALUE_ID_NONE ||
        !ir_ssa_opt_operand_is_versioned(function, &phi->dest)) {
      continue;
    }
    replacement[self] = only;
    drop[i] = 1;
    eliminated++;
  }
  return eliminated;
}

static int ir_ssa_opt_replacement_cycles(const uint32_t *replacement,
                                         uint32_t self, size_t value_count) {
  uint32_t walk = replacement[self];
  for (size_t step = 0;
       step < value_count && walk != IR_VALUE_ID_NONE && walk < value_count;
       step++) {
    if (walk == self) {
      return 1;
    }
    if (replacement[walk] == IR_VALUE_ID_NONE) {
      return 0;
    }
    walk = replacement[walk];
  }
  return 0;
}

static void ir_ssa_opt_drop_cyclic_replacements(IRFunction *function,
                                                size_t value_count,
                                                uint32_t *replacement,
                                                unsigned char *drop,
                                                size_t *eliminated) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!drop[i]) {
      continue;
    }
    const uint32_t self = function->instructions[i].dest.value_id;
    if (!ir_ssa_opt_replacement_cycles(replacement, self, value_count)) {
      continue;
    }
    replacement[self] = IR_VALUE_ID_NONE;
    drop[i] = 0;
    (*eliminated)--;
  }
}

static void ir_ssa_opt_rewrite_replaced_uses(IRFunction *function,
                                             size_t value_count,
                                             const uint32_t *replacement,
                                             const unsigned char *drop) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (drop[i]) {
      continue;
    }
    const int writes = ir_instruction_writes_destination(instruction);
    const size_t operands = 3 + instruction->argument_count;
    for (size_t j = writes ? 1 : 0; j < operands; j++) {
      IROperand *operand = ir_ssa_opt_operand_at(instruction, j);
      if (!operand || !ir_operand_is_value(operand) ||
          operand->value_id == IR_VALUE_ID_NONE ||
          operand->value_id >= value_count ||
          replacement[operand->value_id] == IR_VALUE_ID_NONE) {
        continue;
      }
      const uint32_t target =
          ir_ssa_opt_resolve(replacement, operand->value_id, value_count);
      if (target == operand->value_id || target == IR_VALUE_ID_NONE) {
        continue;
      }
      ir_ssa_opt_write_operand(function, operand, target);
      g_ssa_uses_rewritten++;
    }
  }
}

static void ir_ssa_opt_compact(IRFunction *function,
                               const unsigned char *drop) {
  size_t write = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (drop[i]) {
      ir_instruction_destroy(&function->instructions[i]);
      continue;
    }
    if (write != i) {
      function->instructions[write] = function->instructions[i];
    }
    write++;
  }
  function->instruction_count = write;
}

static size_t ir_ssa_opt_eliminate_trivial(IRFunction *function,
                                           size_t value_count) {
  uint32_t *replacement =
      (uint32_t *)calloc(value_count, sizeof(uint32_t));
  unsigned char *drop =
      (unsigned char *)calloc(function->instruction_count, sizeof(unsigned char));
  if (!replacement || !drop) {
    free(replacement);
    free(drop);
    return 0;
  }

  size_t eliminated =
      ir_ssa_opt_mark_trivial_phis(function, value_count, replacement, drop);
  if (eliminated > 0) {
    ir_ssa_opt_drop_cyclic_replacements(function, value_count, replacement,
                                        drop, &eliminated);
  }
  if (eliminated == 0) {
    free(replacement);
    free(drop);
    return 0;
  }

  ir_ssa_opt_rewrite_replaced_uses(function, value_count, replacement, drop);
  ir_ssa_opt_compact(function, drop);

  free(replacement);
  free(drop);
  g_ssa_trivial_phis += eliminated;
  return eliminated;
}

static size_t ir_ssa_opt_remove_dead_phis(IRFunction *function,
                                          size_t value_count) {
  size_t *uses = (size_t *)calloc(value_count, sizeof(size_t));
  unsigned char *drop =
      (unsigned char *)calloc(function->instruction_count, sizeof(unsigned char));
  if (!uses || !drop) {
    free(uses);
    free(drop);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    const int writes = ir_instruction_writes_destination(instruction);
    const size_t operands = 3 + instruction->argument_count;
    const uint32_t own = (instruction->op == IR_OP_PHI)
                             ? instruction->dest.value_id
                             : IR_VALUE_ID_NONE;
    for (size_t j = writes ? 1 : 0; j < operands; j++) {
      const IROperand *operand = NULL;
      if (j == 0) {
        operand = &instruction->dest;
      } else if (j == 1) {
        operand = &instruction->lhs;
      } else if (j == 2) {
        operand = &instruction->rhs;
      } else if (instruction->arguments &&
                 j - 3 < instruction->argument_count) {
        operand = &instruction->arguments[j - 3];
      }
      if (!operand || !ir_operand_is_value(operand) ||
          operand->value_id == IR_VALUE_ID_NONE ||
          operand->value_id >= value_count) {
        continue;
      }
      if (operand->value_id == own) {
        continue;
      }
      uses[operand->value_id]++;
    }
  }

  size_t dead = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *phi = &function->instructions[i];
    if (phi->op != IR_OP_PHI) {
      continue;
    }
    const uint32_t self = phi->dest.value_id;
    if (self == IR_VALUE_ID_NONE || self >= value_count) {
      continue;
    }
    if (uses[self] != 0) {
      continue;
    }
    if (!ir_ssa_opt_operand_is_versioned(function, &phi->dest)) {
      continue;
    }
    drop[i] = 1;
    dead++;
  }

  if (dead == 0) {
    free(uses);
    free(drop);
    return 0;
  }

  size_t write = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (drop[i]) {
      ir_instruction_destroy(&function->instructions[i]);
      continue;
    }
    if (write != i) {
      function->instructions[write] = function->instructions[i];
    }
    write++;
  }
  function->instruction_count = write;

  free(uses);
  free(drop);
  g_ssa_dead_phis += dead;
  return dead;
}

static size_t ir_ssa_base_length(const char *name) {
  const char *last = NULL;
  const char *scan = name;
  while (scan && (scan = strstr(scan, "__ssa")) != NULL) {
    last = scan;
    scan += 5;
  }
  return last ? (size_t)(last - name) : 0;
}

static int ir_ssa_name_is_version_of(const char *name, const char *base,
                                     size_t base_length) {
  if (!name || !base || base_length == 0) {
    return 0;
  }
  return strncmp(name, base, base_length) == 0 &&
         strncmp(name + base_length, "__ssa", 5) == 0;
}

static const IROperand *ir_ssa_reaching_def(IRFunction *function,
                                            const IRBasicBlock *blocks,
                                            size_t block_count,
                                            const IRDomTree *dom, size_t from,
                                            const char *base,
                                            size_t base_length) {
  size_t block = from;
  for (size_t hops = 0; hops <= block_count && block != IR_BLOCK_NONE &&
                        block < block_count;
       hops++) {
    const IRBasicBlock *scan = &blocks[block];
    for (size_t k = scan->instruction_count; k > 0; k--) {
      const IRInstruction *instruction =
          &function->instructions[scan->first_instruction + k - 1];
      if (!ir_instruction_writes_destination(instruction) ||
          instruction->dest.kind != IR_OPERAND_SYMBOL) {
        continue;
      }
      if (ir_ssa_name_is_version_of(instruction->dest.name, base,
                                    base_length)) {
        return &instruction->dest;
      }
    }
    if (!dom || !dom->built || !dom->idom || block >= dom->block_count) {
      break;
    }
    const size_t next = dom->idom[block];
    if (next == block) {
      break;
    }
    block = next;
  }
  return NULL;
}

static int ir_repair_labels_match(const IRBasicBlock *blocks,
                                  size_t block_count,
                                  const IRBasicBlock *block,
                                  const IRInstruction *phi) {
  const size_t wanted = block->predecessor_count;
  if (phi->argument_count != wanted * 2) {
    return 0;
  }
  for (size_t p = 0; p < wanted; p++) {
    const size_t pred = block->predecessors[p];
    const char *want = (pred < block_count) ? blocks[pred].label : NULL;
    const IROperand *have = &phi->arguments[p * 2 + 1];
    if (want) {
      if (have->kind != IR_OPERAND_LABEL || !have->name ||
          strcmp(have->name, want) != 0) {
        return 0;
      }
    } else if (have->kind != IR_OPERAND_NONE) {
      return 0;
    }
  }
  return 1;
}

static int ir_repair_incomings_agree(const IRInstruction *phi) {
  IROperand agreed = ir_operand_none();
  for (size_t p = 0; p * 2 < phi->argument_count; p++) {
    const IROperand *incoming = &phi->arguments[p * 2];
    if (incoming->kind == IR_OPERAND_NONE) {
      continue;
    }
    if (ir_operand_is_value(incoming) &&
        incoming->value_id == phi->dest.value_id) {
      continue;
    }
    if (agreed.kind == IR_OPERAND_NONE) {
      agreed = *incoming;
    } else if (!ir_operand_same(&agreed, incoming)) {
      return 0;
    }
  }
  return 1;
}

static void ir_repair_report_missing(const IRBasicBlock *block,
                                     const IRInstruction *phi, size_t wanted,
                                     size_t old_pairs, const char *want) {
  if (!getenv("METTLE_SSA_REPAIR_DEBUG")) {
    return;
  }
  fprintf(stderr,
          "ssa-repair: block %s preds %zu incomings %zu wants pred %s; "
          "phi %s has",
          block->label ? block->label : "?", wanted, old_pairs,
          want ? want : "?", phi->dest.name ? phi->dest.name : "?");
  for (size_t q = 0; q * 2 + 1 < phi->argument_count; q++) {
    const IROperand *label = &phi->arguments[q * 2 + 1];
    fprintf(stderr, " %s",
            (label->kind == IR_OPERAND_LABEL && label->name) ? label->name
                                                             : "<none>");
  }
  fprintf(stderr, " agree=%d\n", ir_repair_incomings_agree(phi));
}

static const IROperand *ir_repair_claim_incoming(IRInstruction *phi,
                                                 unsigned char *claimed,
                                                 size_t old_pairs,
                                                 const char *want) {
  for (size_t q = 0; q < old_pairs; q++) {
    const IROperand *label = &phi->arguments[q * 2 + 1];
    if (claimed[q]) {
      continue;
    }
    if (want) {
      if (label->kind == IR_OPERAND_LABEL && label->name &&
          strcmp(label->name, want) == 0) {
        claimed[q] = 1;
        return &phi->arguments[q * 2];
      }
    } else if (label->kind == IR_OPERAND_NONE) {
      claimed[q] = 1;
      return &phi->arguments[q * 2];
    }
  }
  return NULL;
}

static int ir_repair_phi_incomings(IRFunction *function,
                                   const IRBasicBlock *blocks,
                                   size_t block_count, const IRDomTree *dom,
                                   size_t b, IRInstruction *phi) {
  const IRBasicBlock *block = &blocks[b];
  const size_t wanted = block->predecessor_count;
  if (ir_repair_labels_match(blocks, block_count, block, phi)) {
    return 1;
  }

  const size_t old_pairs = phi->argument_count / 2;
  unsigned char *claimed =
      (unsigned char *)calloc(old_pairs ? old_pairs : 1, sizeof(unsigned char));
  IROperand *rebuilt =
      (IROperand *)calloc(wanted ? wanted * 2 : 1, sizeof(IROperand));
  if (!rebuilt || !claimed) {
    free(rebuilt);
    free(claimed);
    return 0;
  }

  for (size_t p = 0; p < wanted; p++) {
    const size_t pred = block->predecessors[p];
    const char *want = (pred < block_count) ? blocks[pred].label : NULL;
    const IROperand *source =
        ir_repair_claim_incoming(phi, claimed, old_pairs, want);
    if (!source && pred < block_count) {
      const size_t base_length = ir_ssa_base_length(phi->dest.name);
      source = ir_ssa_reaching_def(function, blocks, block_count, dom, pred,
                                   phi->dest.name, base_length);
    }
    if (!source) {
      ir_repair_report_missing(block, phi, wanted, old_pairs, want);
      for (size_t d = 0; d < p * 2; d++) {
        ir_operand_destroy(&rebuilt[d]);
      }
      free(rebuilt);
      free(claimed);
      return 0;
    }
    rebuilt[p * 2] = ir_operand_copy(source);
    rebuilt[p * 2 + 1] = want ? ir_operand_label(want) : ir_operand_none();
  }

  free(claimed);
  for (size_t q = 0; q < phi->argument_count; q++) {
    ir_operand_destroy(&phi->arguments[q]);
  }
  free(phi->arguments);
  phi->arguments = rebuilt;
  phi->argument_count = wanted * 2;
  return 2;
}

static int ir_ssa_repair_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *setting = getenv("METTLE_SSA_REPAIR");
    cached = (setting && *setting && strcmp(setting, "0") == 0) ? 0 : 1;
  }
  return cached;
}

static int ir_ssa_sabotage_incomings(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *setting = getenv("METTLE_SSA_SABOTAGE");
    cached = (setting && strcmp(setting, "incoming") == 0) ? 1 : 0;
  }
  return cached;
}

int ir_ssa_sabotage_pass(IRFunction *function, int *changed) {
  if (changed) {
    *changed = 0;
  }
  if (!function || !ir_ssa_sabotage_incomings()) {
    return 1;
  }
  size_t scrambled = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *phi = &function->instructions[i];
    if (phi->op != IR_OP_PHI || !phi->arguments) {
      continue;
    }
    const size_t pairs = phi->argument_count / 2;
    if (pairs < 2) {
      continue;
    }
    for (size_t p = 1; p < pairs; p++) {
      for (size_t q = p; q > 0; q--) {
        const char *left = phi->arguments[(q - 1) * 2 + 1].name;
        const char *right = phi->arguments[q * 2 + 1].name;
        const int order = (!left && !right)  ? 0
                          : !left            ? -1
                          : !right           ? 1
                                             : strcmp(left, right);
        if (order <= 0) {
          break;
        }
        IROperand value = phi->arguments[(q - 1) * 2];
        IROperand label = phi->arguments[(q - 1) * 2 + 1];
        phi->arguments[(q - 1) * 2] = phi->arguments[q * 2];
        phi->arguments[(q - 1) * 2 + 1] = phi->arguments[q * 2 + 1];
        phi->arguments[q * 2] = value;
        phi->arguments[q * 2 + 1] = label;
        scrambled++;
      }
    }
  }
  if (changed) {
    *changed = scrambled != 0;
  }
  return 1;
}

int ir_repair_phis(IRFunction *function, int *changed) {
  if (changed) {
    *changed = 0;
  }
  if (!function || function->instruction_count == 0 ||
      !ir_ssa_repair_enabled()) {
    return 1;
  }
  size_t phis = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_PHI) {
      phis++;
    }
  }
  if (phis == 0) {
    return 1;
  }

  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!blocks || block_count == 0) {
    return ir_leave_ssa_pass(function, changed);
  }
  char why[320];
  why[0] = 0;
  if (ir_function_check_phis(function, why, sizeof(why)) > 0) {
    g_ssa_repair_bailouts++;
    return ir_leave_ssa_pass(function, changed);
  }

  const IRAnalysis *analysis = ir_function_analysis(function);
  const IRDomTree *dom = analysis ? &analysis->dom : NULL;

  int failed = 0;
  for (size_t b = 0; b < block_count && !failed; b++) {
    const IRBasicBlock *block = &blocks[b];
    const size_t start = block->first_instruction;
    for (size_t k = 0; k < block->instruction_count; k++) {
      IRInstruction *instruction = &function->instructions[start + k];
      if (instruction->op == IR_OP_LABEL) {
        continue;
      }
      if (instruction->op != IR_OP_PHI) {
        break;
      }
      const int outcome = ir_repair_phi_incomings(function, blocks,
                                                 block_count, dom, b,
                                                 instruction);
      if (outcome == 0) {
        failed = 1;
        break;
      }
      if (outcome == 2) {
        g_ssa_phis_repaired++;
      }
    }
  }

  if (failed) {
    g_ssa_repair_bailouts++;
    return ir_leave_ssa_pass(function, changed);
  }

  ir_function_number_values(function);
  if (changed) {
    *changed = 0;
  }
  return 1;
}

int ir_ssa_propagate_pass(IRFunction *function, int *changed) {
  if (changed) {
    *changed = 0;
  }
  if (!function || function->instruction_count == 0 || !ir_ssa_opt_enabled()) {
    return 1;
  }
  size_t phi_count = 0;
  if (!ir_ssa_opt_function_is_eligible(function, &phi_count)) {
    return 1;
  }

  int touched = 0;
  for (int round = 0; round < IR_SSA_OPT_MAX_ROUNDS; round++) {
    if (!ir_function_number_values(function)) {
      return 1;
    }
    const size_t value_count = ir_value_table_count(&function->values) + 1;
    size_t work = ir_ssa_opt_eliminate_trivial(function, value_count);
    work += ir_ssa_opt_remove_dead_phis(function, value_count);
    if (work == 0) {
      break;
    }
    touched = 1;
  }

  if (touched) {
    ir_function_clear_cfg(function);
    ir_function_number_values(function);
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}
