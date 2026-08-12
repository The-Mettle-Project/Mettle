#include "ir_profile.h"
#include "../common.h"

#include "../runtime/profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t ir_profile_registry_add(IRProgram *program, const char *name,
                                 const char *filename, uint64_t line) {
  IRProfileEntry *entries = NULL;
  size_t new_index = 0;

  if (!program || !name) {
    return IR_PROFILE_ID_NONE;
  }

  new_index = program->profile_entry_count;
  if (program->profile_entry_count >= program->profile_entry_capacity) {
    size_t new_capacity = program->profile_entry_capacity == 0
                              ? 16u
                              : program->profile_entry_capacity * 2u;
    entries = realloc(program->profile_entries,
                      new_capacity * sizeof(IRProfileEntry));
    if (!entries) {
      return IR_PROFILE_ID_NONE;
    }
    program->profile_entries = entries;
    program->profile_entry_capacity = new_capacity;
  }

  program->profile_entries[new_index].name = mettle_strdup(name);
  program->profile_entries[new_index].filename =
      filename ? mettle_strdup(filename) : NULL;
  program->profile_entries[new_index].line = line;
  if (!program->profile_entries[new_index].name) {
    free(program->profile_entries[new_index].filename);
    program->profile_entries[new_index].filename = NULL;
    return IR_PROFILE_ID_NONE;
  }

  program->profile_entry_count = new_index + 1u;
  return (uint32_t)new_index;
}

uint32_t ir_profile_register_inline_site(IRProgram *program,
                                         const char *callee_name,
                                         size_t inline_site_id,
                                         SourceLocation call_site) {
  char name_buffer[256];
  const char *filename = call_site.filename;
  uint64_t line = call_site.line;

  if (!program || !callee_name) {
    return IR_PROFILE_ID_NONE;
  }

  snprintf(name_buffer, sizeof(name_buffer), "__inl_%s_%zu", callee_name,
           inline_site_id);
  return ir_profile_registry_add(program, name_buffer, filename, line);
}

static SourceLocation ir_profile_function_location(const IRFunction *function) {
  SourceLocation location = {0};

  if (!function) {
    return location;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    SourceLocation candidate = function->instructions[i].location;
    if (candidate.line > 0) {
      return candidate;
    }
  }

  return location;
}

static int ir_profile_should_instrument(const IRFunction *function) {
  const char *name = NULL;

  if (!function || !function->name) {
    return 0;
  }

  name = function->name;
  if (strncmp(name, "mettle_profile_", 15) == 0) {
    return 0;
  }
  if (strncmp(name, "__inl_", 6) == 0) {
    return 0;
  }
  return 1;
}

int ir_profile_instruction_is_enter(const IRInstruction *instruction,
                                    uint32_t *profile_id_out) {
  if (!instruction || instruction->op != IR_OP_CALL || !instruction->text ||
      strcmp(instruction->text, "mettle_profile_enter") != 0 ||
      instruction->argument_count != 1 ||
      instruction->arguments[0].kind != IR_OPERAND_INT) {
    return 0;
  }

  if (profile_id_out) {
    *profile_id_out = (uint32_t)instruction->arguments[0].int_value;
  }
  return 1;
}

int ir_profile_build_enter_instruction(IRInstruction *instruction,
                                       uint32_t profile_id,
                                       SourceLocation location) {
  if (!instruction) {
    return 0;
  }

  memset(instruction, 0, sizeof(*instruction));
  instruction->op = IR_OP_CALL;
  instruction->location = location;
  instruction->text = mettle_strdup("mettle_profile_enter");
  if (!instruction->text) {
    return 0;
  }
  instruction->argument_count = 1;
  instruction->arguments = malloc(sizeof(IROperand));
  if (!instruction->arguments) {
    mettle_free_string(instruction->text);
    instruction->text = NULL;
    return 0;
  }
  instruction->arguments[0] = ir_operand_int((long long)profile_id);
  return 1;
}

static IRInstruction ir_profile_make_exit(SourceLocation location) {
  IRInstruction instruction = {0};
  instruction.op = IR_OP_CALL;
  instruction.location = location;
  instruction.text = mettle_strdup("mettle_profile_exit");
  return instruction;
}

static int ir_profile_build_op_instruction(IRInstruction *instruction,
                                           uint32_t op_class,
                                           uint64_t amount,
                                           SourceLocation location) {
  if (!instruction) {
    return 0;
  }

  memset(instruction, 0, sizeof(*instruction));
  instruction->op = IR_OP_CALL;
  instruction->location = location;
  instruction->text = mettle_strdup("mettle_profile_op");
  if (!instruction->text) {
    return 0;
  }
  instruction->argument_count = 2;
  instruction->arguments = malloc(2 * sizeof(IROperand));
  if (!instruction->arguments) {
    mettle_free_string(instruction->text);
    instruction->text = NULL;
    return 0;
  }
  instruction->arguments[0] = ir_operand_int((long long)op_class);
  instruction->arguments[1] = ir_operand_int((long long)amount);
  return 1;
}

static void ir_profile_destroy_op_instruction(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }
  for (size_t i = 0; i < instruction->argument_count; i++) {
    ir_operand_destroy(&instruction->arguments[i]);
  }
  free(instruction->arguments);
  instruction->arguments = NULL;
  instruction->argument_count = 0;
  mettle_free_string(instruction->text);
  instruction->text = NULL;
}

static void ir_profile_destroy_enter(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }
  if (instruction->arguments) {
    ir_operand_destroy(&instruction->arguments[0]);
    free(instruction->arguments);
    instruction->arguments = NULL;
  }
  mettle_free_string(instruction->text);
  instruction->text = NULL;
  instruction->argument_count = 0;
}

static int ir_profile_instrument_function(IRProgram *program,
                                          IRFunction *function) {
  SourceLocation location = {0};
  IRInstruction enter = {0};
  IRInstruction exit = {0};
  uint32_t profile_id = IR_PROFILE_ID_NONE;

  if (!program || !function) {
    return 0;
  }

  location = ir_profile_function_location(function);
  profile_id = ir_profile_registry_add(program, function->name,
                                       location.filename, location.line);
  if (profile_id == IR_PROFILE_ID_NONE) {
    return 0;
  }

  function->profile_id = profile_id;

  if (!ir_profile_build_enter_instruction(&enter, profile_id, location) ||
      !ir_function_insert_instruction(function, 0, &enter)) {
    ir_profile_destroy_enter(&enter);
    return 0;
  }
  ir_profile_destroy_enter(&enter);

  for (size_t i = function->instruction_count; i > 0; i--) {
    size_t index = i - 1;
    if (function->instructions[index].op != IR_OP_RETURN) {
      continue;
    }

    location = function->instructions[index].location;
    exit = ir_profile_make_exit(location);
    if (!exit.text ||
        !ir_function_insert_instruction(function, index, &exit)) {
      mettle_free_string(exit.text);
      return 0;
    }
    mettle_free_string(exit.text);
    exit.text = NULL;
  }

  return 1;
}

int ir_profile_instrument_program(IRProgram *program) {
  if (!program) {
    return 0;
  }

  program->profile_entry_count = 0;

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function || !ir_profile_should_instrument(function)) {
      if (function) {
        function->profile_id = IR_PROFILE_ID_NONE;
      }
      continue;
    }

    if (!ir_profile_instrument_function(program, function)) {
      return 0;
    }
  }

  return 1;
}

static int ir_profile_call_is_mem_primitive(const IRInstruction *instruction) {
  if (!instruction || instruction->op != IR_OP_CALL || !instruction->text) {
    return 0;
  }
  return strcmp(instruction->text, "memcpy") == 0 ||
         strcmp(instruction->text, "memmove") == 0 ||
         strcmp(instruction->text, "memset") == 0;
}

static int ir_profile_instruction_op_class(const IRInstruction *instruction,
                                           uint32_t *op_class_out) {
  if (!instruction || !op_class_out) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_LOAD:
    *op_class_out = METTLE_PROFILE_OP_LOAD;
    return 1;
  case IR_OP_STORE:
    *op_class_out = METTLE_PROFILE_OP_STORE;
    return 1;
  case IR_OP_JUMP:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
    *op_class_out = METTLE_PROFILE_OP_BRANCH;
    return 1;
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
    if (instruction->text &&
        strncmp(instruction->text, "mettle_profile_", 15) == 0) {
      return 0;
    }
    if (ir_profile_call_is_mem_primitive(instruction)) {
      *op_class_out = METTLE_PROFILE_OP_MEM_PRIMITIVE;
    } else {
      *op_class_out = METTLE_PROFILE_OP_CALL;
    }
    return 1;
  case IR_OP_MEMCPY_INLINE:
    *op_class_out = METTLE_PROFILE_OP_MEM_PRIMITIVE;
    return 1;
  case IR_OP_COUNT_WORD_STARTS:
    *op_class_out = METTLE_PROFILE_OP_POPCNT;
    return 1;
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
  case IR_OP_SIMD_BYTE_MAP:
  case IR_OP_SIMD_FILL:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
  case IR_OP_SIMD_DOT_I8:
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32:
  case IR_OP_LOWER_BOUND_I32:
  case IR_OP_PREFIX_SUM_I32:
  case IR_OP_SIMD_MINMAX_I32:
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32:
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_FIND:
  case IR_OP_SIMD_OUTER_LANE_F64:
    *op_class_out = METTLE_PROFILE_OP_SIMD;
    return 1;
  case IR_OP_BINARY:
    if (!instruction->text) {
      return 0;
    }
    if (strcmp(instruction->text, "+") == 0 ||
        strcmp(instruction->text, "-") == 0) {
      *op_class_out = METTLE_PROFILE_OP_ADD;
      return 1;
    }
    if (strcmp(instruction->text, "*") == 0) {
      *op_class_out = METTLE_PROFILE_OP_MUL;
      return 1;
    }
    if (strcmp(instruction->text, "/") == 0) {
      *op_class_out = METTLE_PROFILE_OP_DIV;
      return 1;
    }
    if (strcmp(instruction->text, "%") == 0) {
      *op_class_out = METTLE_PROFILE_OP_MOD;
      return 1;
    }
    if (strcmp(instruction->text, "<<") == 0 ||
        strcmp(instruction->text, ">>") == 0) {
      *op_class_out = METTLE_PROFILE_OP_SHIFT;
      return 1;
    }
    if (strcmp(instruction->text, "&") == 0 || strcmp(instruction->text, "|") == 0 ||
        strcmp(instruction->text, "^") == 0) {
      *op_class_out = METTLE_PROFILE_OP_BITWISE;
      return 1;
    }
    return 0;
  case IR_OP_UNARY:
    if (instruction->text && strcmp(instruction->text, "~") == 0) {
      *op_class_out = METTLE_PROFILE_OP_BITWISE;
      return 1;
    }
    return 0;
  default:
    return 0;
  }
}

static int ir_profile_instrument_function_ops(IRFunction *function) {
  if (!function || function->profile_id == IR_PROFILE_ID_NONE) {
    return 1;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    uint32_t op_class = 0;
    IRInstruction op_counter = {0};
    IRInstruction *instruction = &function->instructions[i];

    if (!ir_profile_instruction_op_class(instruction, &op_class)) {
      continue;
    }

    if (!ir_profile_build_op_instruction(&op_counter, op_class, 1u,
                                         instruction->location) ||
        !ir_function_insert_instruction(function, i, &op_counter)) {
      ir_profile_destroy_op_instruction(&op_counter);
      return 0;
    }

    ir_profile_destroy_op_instruction(&op_counter);
    i++;
  }

  return 1;
}

static int ir_profile_build_block_instruction(IRInstruction *instruction,
                                              uint32_t block_id,
                                              SourceLocation location) {
  if (!instruction) {
    return 0;
  }

  memset(instruction, 0, sizeof(*instruction));
  instruction->op = IR_OP_CALL;
  instruction->location = location;
  instruction->text = mettle_strdup("mettle_profile_block");
  if (!instruction->text) {
    return 0;
  }
  instruction->argument_count = 1;
  instruction->arguments = malloc(sizeof(IROperand));
  if (!instruction->arguments) {
    mettle_free_string(instruction->text);
    instruction->text = NULL;
    return 0;
  }
  instruction->arguments[0] = ir_operand_int((long long)block_id);
  return 1;
}

int ir_profile_instruction_is_block(const IRInstruction *instruction,
                                    uint32_t *block_id_out) {
  if (!instruction || instruction->op != IR_OP_CALL || !instruction->text ||
      strcmp(instruction->text, "mettle_profile_block") != 0 ||
      instruction->argument_count != 1 ||
      instruction->arguments[0].kind != IR_OPERAND_INT) {
    return 0;
  }
  if (block_id_out) {
    *block_id_out = (uint32_t)instruction->arguments[0].int_value;
  }
  return 1;
}

static int ir_profile_insert_block_marker(IRFunction *function, size_t index,
                                          uint32_t block_id,
                                          SourceLocation location) {
  IRInstruction marker = {0};

  if (!ir_profile_build_block_instruction(&marker, block_id, location) ||
      !ir_function_insert_instruction(function, index, &marker)) {
    ir_profile_destroy_op_instruction(&marker);
    return 0;
  }
  ir_profile_destroy_op_instruction(&marker);
  return 1;
}

static int ir_profile_instrument_function_blocks(IRFunction *function,
                                                 uint32_t *next_block_id) {
  SourceLocation location = {0};

  if (!function) {
    return 1;
  }

  /* Entry block. Use the first real instruction's location for line mapping. */
  if (function->instruction_count > 0) {
    location = function->instructions[0].location;
  }
  if (!ir_profile_insert_block_marker(function, 0, *next_block_id, location)) {
    return 0;
  }
  (*next_block_id)++;

  /* Every label heads a basic block. Insert the counter just after the label
   * so the label stays the branch target and the counter runs on block entry.
   * The scan starts past the entry marker we just inserted at index 0. */
  for (size_t i = 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL) {
      continue;
    }
    location = function->instructions[i].location;
    if (!ir_profile_insert_block_marker(function, i + 1, *next_block_id,
                                        location)) {
      return 0;
    }
    (*next_block_id)++;
    i++; /* skip the marker we just inserted */
  }

  return 1;
}

int ir_profile_instrument_blocks(IRProgram *program) {
  uint32_t next_block_id = 0;

  if (!program) {
    return 0;
  }

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function || !ir_profile_should_instrument(function)) {
      continue;
    }
    if (!ir_profile_instrument_function_blocks(function, &next_block_id)) {
      return 0;
    }
  }

  return 1;
}

int ir_profile_instrument_operation_counters(IRProgram *program) {
  if (!program) {
    return 0;
  }

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function || !ir_profile_should_instrument(function)) {
      continue;
    }
    if (!ir_profile_instrument_function_ops(function)) {
      return 0;
    }
  }

  return 1;
}
