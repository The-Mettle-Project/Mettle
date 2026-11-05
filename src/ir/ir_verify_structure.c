#include "ir_verify_structure.h"

#include "../common.h"
#include "../compiler/compiler_crash.h"
#include "ir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  IR_STRUCT_MODE_UNSET = 0,
  IR_STRUCT_MODE_OFF,
  IR_STRUCT_MODE_CENSUS,
  IR_STRUCT_MODE_STRICT
} IRStructureMode;

static IRStructureMode g_structure_mode = IR_STRUCT_MODE_UNSET;
static size_t g_structure_violations = 0;
static size_t g_structure_regressions = 0;

static IRStructureMode ir_structure_mode(void) {
  if (g_structure_mode != IR_STRUCT_MODE_UNSET) {
    return g_structure_mode;
  }
  const char *setting = getenv("METTLE_IR_STRUCT");
  if (!setting || !*setting) {
    g_structure_mode = IR_STRUCT_MODE_OFF;
  } else if (strcmp(setting, "strict") == 0) {
    g_structure_mode = IR_STRUCT_MODE_STRICT;
  } else if (strcmp(setting, "census") == 0 || strcmp(setting, "1") == 0) {
    g_structure_mode = IR_STRUCT_MODE_CENSUS;
  } else {
    g_structure_mode = IR_STRUCT_MODE_OFF;
  }
  return g_structure_mode;
}

int ir_structure_enabled(void) {
  return ir_structure_mode() != IR_STRUCT_MODE_OFF;
}

size_t ir_structure_violation_count(void) { return g_structure_violations; }

size_t ir_structure_regression_count(void) { return g_structure_regressions; }

static int ir_structure_is_branch(const IRInstruction *instruction) {
  return instruction && (instruction->op == IR_OP_BRANCH_ZERO ||
                         instruction->op == IR_OP_BRANCH_EQ ||
                         instruction->op == IR_OP_JUMP);
}

static const IROperand *ir_structure_operand_at(const IRInstruction *instruction,
                                                size_t index) {
  switch (index) {
  case 0:
    return &instruction->dest;
  case 1:
    return &instruction->lhs;
  case 2:
    return &instruction->rhs;
  default:
    break;
  }
  const size_t argument = index - 3;
  if (!instruction->arguments || argument >= instruction->argument_count) {
    return NULL;
  }
  return &instruction->arguments[argument];
}

static void ir_structure_first_reason(char *why, size_t why_capacity, int ok,
                                      const char *format, ...) {
  if (!ok || !why || !why_capacity) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(why, why_capacity, format, args);
  va_end(args);
}

static int ir_structure_collect_labels(const IRFunction *function,
                                       IRValueTable *labels,
                                       IRStructureReport *report, char *why,
                                       size_t why_capacity) {
  int ok = 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_LABEL) {
      continue;
    }
    if (!instruction->text) {
      ir_structure_first_reason(why, why_capacity, ok,
                                "instruction %zu is a label with no name", i);
      ok = 0;
      if (report) {
        report->duplicate_labels++;
      }
      continue;
    }
    if (ir_value_table_lookup(labels, (unsigned char)IR_OPERAND_LABEL,
                              instruction->text) != IR_VALUE_ID_NONE) {
      ir_structure_first_reason(
          why, why_capacity, ok,
          "label '%s' is defined more than once, again at instruction %zu",
          instruction->text, i);
      ok = 0;
      if (report) {
        report->duplicate_labels++;
      }
      continue;
    }
    ir_value_table_intern(labels, (unsigned char)IR_OPERAND_LABEL,
                          instruction->text);
  }
  return ok;
}

static int ir_structure_check_branch(const IRInstruction *instruction,
                                     size_t index, IRValueTable *labels,
                                     IRStructureReport *report, char *why,
                                     size_t why_capacity, int ok) {
  if (!instruction->text) {
    ir_structure_first_reason(why, why_capacity, ok,
                              "instruction %zu branches to no target", index);
  } else if (ir_value_table_lookup(labels, (unsigned char)IR_OPERAND_LABEL,
                                   instruction->text) == IR_VALUE_ID_NONE) {
    ir_structure_first_reason(
        why, why_capacity, ok,
        "instruction %zu branches to '%s' which no label defines", index,
        instruction->text);
  } else {
    return 1;
  }
  if (report) {
    report->unresolved_targets++;
  }
  return 0;
}

static void ir_structure_count_unnumbered(const IRInstruction *instruction,
                                          IRStructureReport *report) {
  const size_t operands = 3 + instruction->argument_count;
  for (size_t j = 0; j < operands; j++) {
    const IROperand *operand = ir_structure_operand_at(instruction, j);
    if (operand && ir_operand_is_value(operand) &&
        operand->value_id == IR_VALUE_ID_NONE) {
      report->unnumbered_values++;
    }
  }
}

static void ir_structure_tally_defs(const IRFunction *function,
                                    IRStructureReport *report,
                                    const size_t *defs,
                                    size_t value_capacity) {
  for (uint32_t id = 1; id < (uint32_t)value_capacity; id++) {
    if (defs[id] == 0) {
      continue;
    }
    if (ir_value_table_kind(&function->values, id) == IR_OPERAND_TEMP) {
      report->temps++;
      if (defs[id] > 1) {
        report->temps_multi_def++;
      }
    } else {
      report->symbols++;
      if (defs[id] > 1) {
        report->symbols_multi_def++;
      }
    }
  }
}

int ir_function_check_structure(const IRFunction *function,
                                IRStructureReport *report, char *why,
                                size_t why_capacity) {
  if (report) {
    memset(report, 0, sizeof(*report));
  }
  if (!function) {
    return 1;
  }
  if (why && why_capacity) {
    why[0] = '\0';
  }

  IRValueTable labels;
  ir_value_table_init(&labels);
  int ok =
      ir_structure_collect_labels(function, &labels, report, why, why_capacity);

  const size_t value_capacity = ir_value_table_count(&function->values) + 1;
  size_t *defs = (size_t *)calloc(value_capacity, sizeof(size_t));

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];

    if (ir_structure_is_branch(instruction) &&
        !ir_structure_check_branch(instruction, i, &labels, report, why,
                                   why_capacity, ok)) {
      ok = 0;
    }

    if (defs && ir_instruction_writes_destination(instruction) &&
        ir_operand_is_value(&instruction->dest)) {
      const uint32_t id = instruction->dest.value_id;
      if (id != IR_VALUE_ID_NONE && id < value_capacity) {
        defs[id]++;
      }
    }

    if (report) {
      ir_structure_count_unnumbered(instruction, report);
    }
  }

  if (report && defs) {
    ir_structure_tally_defs(function, report, defs, value_capacity);
  }

  free(defs);
  ir_value_table_clear(&labels);
  return ok;
}

static int g_structure_sabotage_fired = 0;

void ir_structure_maybe_sabotage(IRFunction *function, const char *pass_name) {
  if (g_structure_sabotage_fired || !function || !pass_name) {
    return;
  }
  static MettleEnvOnce break_spec;
  const char *spec = mettle_env_once(&break_spec, "METTLE_IR_STRUCT_BREAK");
  if (!spec || !*spec) {
    return;
  }
  const char *colon = strchr(spec, ':');
  const char *kind = spec;
  if (colon) {
    if (strncmp(spec, pass_name, (size_t)(colon - spec)) != 0 ||
        strlen(pass_name) != (size_t)(colon - spec)) {
      return;
    }
    kind = colon + 1;
  }

  if (strcmp(kind, "dangle") == 0) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *instruction = &function->instructions[i];
      if (!ir_structure_is_branch(instruction) || !instruction->text) {
        continue;
      }
      free(instruction->text);
      instruction->text = mettle_strdup("__structure_sabotage_target");
      g_structure_sabotage_fired = 1;
      return;
    }
    return;
  }

  if (strcmp(kind, "dup") == 0) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *instruction = &function->instructions[i];
      if (instruction->op != IR_OP_LABEL || !instruction->text) {
        continue;
      }
      for (size_t j = i + 1; j < function->instruction_count; j++) {
        IRInstruction *other = &function->instructions[j];
        if (other->op != IR_OP_LABEL || !other->text ||
            strcmp(other->text, instruction->text) == 0) {
          continue;
        }
        free(other->text);
        other->text = mettle_strdup(instruction->text);
        g_structure_sabotage_fired = 1;
        return;
      }
    }
    return;
  }

  if (strcmp(kind, "redef") == 0) {
    for (size_t i = 0; i + 1 < function->instruction_count; i++) {
      IRInstruction *instruction = &function->instructions[i];
      if (instruction->dest.kind != IR_OPERAND_TEMP || !instruction->dest.name) {
        continue;
      }
      for (size_t j = i + 1; j < function->instruction_count; j++) {
        IRInstruction *other = &function->instructions[j];
        if (other->dest.kind != IR_OPERAND_TEMP || !other->dest.name ||
            strcmp(other->dest.name, instruction->dest.name) == 0) {
          continue;
        }
        ir_operand_destroy(&other->dest);
        other->dest = ir_operand_temp(instruction->dest.name);
        other->dest.value_id = instruction->dest.value_id;
        g_structure_sabotage_fired = 1;
        return;
      }
    }
    return;
  }
}

size_t ir_structure_dominance_violations(IRFunction *function, char *why,
                                         size_t why_capacity) {
  if (!function || function->instruction_count == 0) {
    return 0;
  }
  const IRAnalysis *analysis = ir_function_analysis(function);
  if (!analysis || !analysis->ud.built || !analysis->dom.built ||
      !analysis->instruction_block) {
    return 0;
  }

  size_t violations = 0;
  for (uint32_t id = 1; id < (uint32_t)analysis->ud.value_count; id++) {
    if (analysis->ud.def_count[id] != 1) {
      continue;
    }
    if (ir_value_table_kind(&function->values, id) != IR_OPERAND_TEMP) {
      continue;
    }
    const uint32_t def = analysis->ud.def_first[id];
    if (def == IR_INSTRUCTION_NONE) {
      continue;
    }
    size_t use_count = 0;
    const IRValueUse *uses = ir_function_value_uses(function, id, &use_count);
    for (size_t u = 0; u < use_count; u++) {
      const uint32_t at = uses[u].instruction;
      if (function->instructions[at].op == IR_OP_PHI ||
          function->instructions[at].op == IR_OP_ADDRESS_OF) {
        continue;
      }
      const size_t use_block = analysis->instruction_block[at];
      const size_t def_block = analysis->instruction_block[def];
      if (use_block == IR_BLOCK_NONE || def_block == IR_BLOCK_NONE ||
          use_block >= analysis->dom.block_count ||
          def_block >= analysis->dom.block_count ||
          analysis->dom.rpo_index[use_block] == IR_BLOCK_NONE ||
          analysis->dom.rpo_index[def_block] == IR_BLOCK_NONE) {
        continue;
      }
      if (ir_function_instruction_dominates(function, def, at)) {
        continue;
      }
      violations++;
      if (violations == 1 && why && why_capacity) {
        snprintf(why, why_capacity,
                 "instruction %u in block %zu uses '%s' which instruction %u "
                 "in block %zu defines without dominating it",
                 at, use_block, ir_value_table_name(&function->values, id), def,
                 def_block);
      }
    }
  }
  return violations;
}

static size_t g_phi_violations = 0;

size_t ir_phi_violation_count(void) { return g_phi_violations; }

size_t ir_function_check_phis(IRFunction *function, char *why,
                              size_t why_capacity) {
  if (!function || function->instruction_count == 0) {
    return 0;
  }
  size_t phi_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_PHI) {
      phi_count++;
    }
  }
  if (phi_count == 0) {
    return 0;
  }

  IRValueTable labels;
  ir_value_table_init(&labels);
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LABEL && in->text) {
      ir_value_table_intern(&labels, (unsigned char)IR_OPERAND_LABEL, in->text);
    }
  }

  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  size_t bad = 0;

  for (size_t b = 0; b < block_count; b++) {
    const size_t start = blocks[b].first_instruction;
    size_t head = 0;
    while (head < blocks[b].instruction_count &&
           (function->instructions[start + head].op == IR_OP_LABEL ||
            function->instructions[start + head].op == IR_OP_NOP ||
            function->instructions[start + head].op == IR_OP_PHI)) {
      head++;
    }
    for (size_t k = 0; k < blocks[b].instruction_count; k++) {
      const IRInstruction *phi = &function->instructions[start + k];
      if (phi->op != IR_OP_PHI) {
        continue;
      }
      if (k >= head) {
        bad++;
        if (bad == 1 && why && why_capacity) {
          snprintf(why, why_capacity,
                   "phi at %zu is not at the head of its block",
                   start + k);
        }
        continue;
      }
      const size_t incoming = phi->argument_count / 2;
      if (incoming != blocks[b].predecessor_count) {
        bad++;
        if (bad == 1 && why && why_capacity) {
          snprintf(why, why_capacity,
                   "phi at %zu has %zu incoming for %zu predecessors",
                   start + k, incoming, blocks[b].predecessor_count);
        }
        continue;
      }
      for (size_t a = 0; a + 1 < phi->argument_count; a += 2) {
        const IROperand *label = &phi->arguments[a + 1];
        if (label->kind != IR_OPERAND_LABEL || !label->name) {
          continue;
        }
        if (ir_value_table_lookup(&labels, (unsigned char)IR_OPERAND_LABEL,
                                  label->name) == IR_VALUE_ID_NONE) {
          bad++;
          if (bad == 1 && why && why_capacity) {
            snprintf(why, why_capacity,
                     "phi at %zu names predecessor '%s' which no label defines",
                     start + k, label->name);
          }
        }
      }
    }
  }

  ir_value_table_clear(&labels);
  return bad;
}

static void ir_phi_check_declarations(IRFunction *function,
                                      const char *pass_name) {
  IRValueTable declared;
  ir_value_table_init(&declared);
  for (size_t p = 0; p < function->parameter_count; p++) {
    if (function->parameter_names && function->parameter_names[p]) {
      ir_value_table_intern(&declared, (unsigned char)IR_OPERAND_SYMBOL,
                            function->parameter_names[p]);
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      ir_value_table_intern(&declared, (unsigned char)IR_OPERAND_SYMBOL,
                            in->dest.name);
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_PHI || !in->arguments) {
      continue;
    }
    for (size_t a = 0; a + 1 < in->argument_count; a += 2) {
      const IROperand *v = &in->arguments[a];
      if (v->kind != IR_OPERAND_SYMBOL || !v->name) {
        continue;
      }
      if (ir_value_table_lookup(&declared, (unsigned char)IR_OPERAND_SYMBOL,
                                v->name) == IR_VALUE_ID_NONE) {
        fprintf(stderr,
                "mettle: pass '%s' left phi incoming '%s' undeclared in '%s'\n",
                pass_name ? pass_name : "<unnamed>", v->name,
                function->name ? function->name : "<unnamed>");
        ir_value_table_clear(&declared);
        return;
      }
    }
  }
  ir_value_table_clear(&declared);
}

void ir_phi_check_after_pass(IRFunction *function, const char *pass_name) {
  static MettleEnvOnce audit;
  if (!mettle_env_once(&audit, "METTLE_PHI_AUDIT") || !function) {
    return;
  }
  ir_phi_check_declarations(function, pass_name);
  char why[320];
  why[0] = 0;
  const size_t bad = ir_function_check_phis(function, why, sizeof(why));
  if (bad == 0) {
    return;
  }
  g_phi_violations++;
  fprintf(stderr, "mettle: pass '%s' broke %zu phis in '%s': %s\n",
          pass_name ? pass_name : "<unnamed>", bad,
          function->name ? function->name : "<unnamed>", why);
}

size_t ir_structure_snapshot(const IRFunction *function) {
  if (!ir_structure_enabled() || !function) {
    return 0;
  }
  IRStructureReport report;
  ir_function_check_structure(function, &report, NULL, 0);
  return report.temps_multi_def + report.symbols_multi_def;
}

void ir_structure_check_after_pass(const IRFunction *function,
                                   const char *pass_name, size_t before) {
  const IRStructureMode mode = ir_structure_mode();
  if (mode == IR_STRUCT_MODE_OFF || !function) {
    return;
  }

  IRStructureReport report;
  char why[512];
  const int ok =
      ir_function_check_structure(function, &report, why, sizeof(why));
  const size_t after = report.temps_multi_def + report.symbols_multi_def;

  if (!ok) {
    g_structure_violations++;
    fprintf(stderr, "mettle: ir structure broken after pass '%s' in '%s': %s\n",
            pass_name ? pass_name : "<unnamed>",
            function->name ? function->name : "<unnamed>", why);
    if (mode == IR_STRUCT_MODE_STRICT) {
      char message[768];
      snprintf(message, sizeof(message),
               "optimization pass '%s' left '%s' structurally invalid: %s",
               pass_name ? pass_name : "<unnamed>",
               function->name ? function->name : "<unnamed>", why);
      mettle_compiler_ice(message);
    }
  }

  if (getenv("METTLE_IR_DOMCHECK")) {
    char dom_why[512];
    dom_why[0] = 0;
    const size_t dominance =
        ir_structure_dominance_violations((IRFunction *)function, dom_why,
                                          sizeof(dom_why));
    if (dominance > 0) {
      g_structure_violations++;
      fprintf(stderr,
              "mettle: %zu uses reach past their definition after pass '%s' in "
              "'%s': %s\n",
              dominance, pass_name ? pass_name : "<unnamed>",
              function->name ? function->name : "<unnamed>", dom_why);
      if (strcmp(getenv("METTLE_IR_DOMCHECK"), "strict") == 0) {
        char message[768];
        snprintf(message, sizeof(message),
                 "optimization pass '%s' left %zu uses not dominated by their "
                 "definition in '%s': %s",
                 pass_name ? pass_name : "<unnamed>", dominance,
                 function->name ? function->name : "<unnamed>", dom_why);
        mettle_compiler_ice(message);
      }
    }
  }

  if (after > before) {
    g_structure_regressions++;
    fprintf(stderr,
            "mettle: pass '%s' in '%s' raised the values defined more than "
            "once from %zu to %zu\n",
            pass_name ? pass_name : "<unnamed>",
            function->name ? function->name : "<unnamed>", before, after);
    if (mode == IR_STRUCT_MODE_STRICT) {
      char message[768];
      snprintf(message, sizeof(message),
               "optimization pass '%s' raised the values defined more than "
               "once in '%s' from %zu to %zu",
               pass_name ? pass_name : "<unnamed>",
               function->name ? function->name : "<unnamed>", before, after);
      mettle_compiler_ice(message);
    }
  }
}
