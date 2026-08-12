#include "ir_optimize_internal.h"


void ir_collect_function_features(const IRFunction *function,
                                         IROptFunctionFeatures *features) {
  if (!features) {
    return;
  }

  memset(features, 0, sizeof(*features));
  if (!function) {
    return;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    switch (instruction->op) {
    case IR_OP_LABEL:
      features->has_label = 1;
      if (instruction->text &&
          strncmp(instruction->text, "ir_while_", 9) == 0) {
        features->has_while_label = 1;
      }
      break;
    case IR_OP_JUMP:
      features->has_jump = 1;
      break;
    case IR_OP_BRANCH_ZERO:
      features->has_branch_zero = 1;
      break;
    case IR_OP_BRANCH_EQ:
      features->has_branch_eq = 1;
      break;
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
      features->has_call = 1;
      break;
    case IR_OP_LOAD:
      features->has_load = 1;
      break;
    case IR_OP_ASSIGN:
      features->has_assign = 1;
      break;
    case IR_OP_BINARY:
      features->has_binary = 1;
      if (instruction->text && strcmp(instruction->text, "/") == 0) {
        features->has_div = 1;
      }
      break;
    default:
      break;
    }

    if (ir_instruction_writes_temp(instruction)) {
      features->has_temp_write = 1;
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Constant memcpy call specialization                                        */
/* -------------------------------------------------------------------------- */

const IRInstruction *ir_find_temp_producer_before(const IRFunction *function,
                                                  size_t before_index,
                                                  const char *temp_name) {
  if (!function || !temp_name) {
    return NULL;
  }

  for (size_t i = before_index; i > 0;) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (instruction->dest.kind == IR_OPERAND_TEMP && instruction->dest.name &&
        strcmp(instruction->dest.name, temp_name) == 0) {
      return instruction;
    }
    if (instruction->op == IR_OP_LABEL) {
      break;
    }
  }
  return NULL;
}

