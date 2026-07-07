#include "ir.h"
#include "../common.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define IR_OPERAND_FMT_BUFSIZE 128

IROperand ir_operand_none(void) {
  IROperand operand = {0};
  operand.kind = IR_OPERAND_NONE;
  return operand;
}

IROperand ir_operand_temp(const char *name) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_TEMP;
  operand.name = mettle_strdup(name);
  return operand;
}

IROperand ir_operand_symbol(const char *name) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_SYMBOL;
  operand.name = mettle_strdup(name);
  return operand;
}

IROperand ir_operand_int(long long value) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_INT;
  operand.int_value = value;
  return operand;
}

IROperand ir_operand_float(double value) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_FLOAT;
  operand.float_value = value;
  operand.float_bits = 64;
  return operand;
}

IROperand ir_operand_float_sized(double value, int float_bits) {
  IROperand operand = ir_operand_float(value);
  operand.float_bits = (float_bits == 32) ? 32 : 64;
  return operand;
}

IROperand ir_operand_string(const char *value) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_STRING;
  operand.name = mettle_strdup(value);
  return operand;
}

IROperand ir_operand_label(const char *name) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_LABEL;
  operand.name = mettle_strdup(name);
  return operand;
}

IROperand ir_operand_copy(const IROperand *operand) {
  if (!operand) {
    return ir_operand_none();
  }

  switch (operand->kind) {
  case IR_OPERAND_TEMP: {
    IROperand copy = ir_operand_temp(operand->name);
    copy.float_bits = operand->float_bits;
    return copy;
  }
  case IR_OPERAND_SYMBOL: {
    IROperand copy = ir_operand_symbol(operand->name);
    copy.float_bits = operand->float_bits;
    return copy;
  }
  case IR_OPERAND_INT:
    return ir_operand_int(operand->int_value);
  case IR_OPERAND_FLOAT:
    return ir_operand_float_sized(operand->float_value, operand->float_bits);
  case IR_OPERAND_STRING:
    return ir_operand_string(operand->name);
  case IR_OPERAND_LABEL:
    return ir_operand_label(operand->name);
  case IR_OPERAND_NONE:
  default:
    return ir_operand_none();
  }
}

void ir_operand_destroy(IROperand *operand) {
  if (!operand) {
    return;
  }

  switch (operand->kind) {
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_STRING:
  case IR_OPERAND_LABEL:
    if (operand->name) {
      free(operand->name);
    }
    break;
  default:
    break;
  }

  *operand = ir_operand_none();
}

static IROperand ir_operand_clone(const IROperand *operand) {
  return ir_operand_copy(operand);
}

static void ir_instruction_destroy(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  free(instruction->text);

  if (instruction->arguments) {
    for (size_t i = 0; i < instruction->argument_count; i++) {
      ir_operand_destroy(&instruction->arguments[i]);
    }
    free(instruction->arguments);
  }

  instruction->arguments = NULL;
  instruction->argument_count = 0;
}

static void ir_function_clear_parameters(IRFunction *function) {
  if (!function) {
    return;
  }

  if (function->parameter_names) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      free(function->parameter_names[i]);
    }
    free(function->parameter_names);
  }

  if (function->parameter_types) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      free(function->parameter_types[i]);
    }
    free(function->parameter_types);
  }

  function->parameter_names = NULL;
  function->parameter_types = NULL;
  function->parameter_count = 0;
}

void ir_function_clear_cfg(IRFunction *function) {
  if (!function) {
    return;
  }

  if (function->blocks) {
    for (size_t i = 0; i < function->block_count; i++) {
      free(function->blocks[i].successors);
      free(function->blocks[i].predecessors);
    }
    free(function->blocks);
  }

  function->blocks = NULL;
  function->block_count = 0;
  function->entry_block = 0;
  function->cfg_valid = 0;
}

IRFunction *ir_function_create(const char *name) {
  IRFunction *function = malloc(sizeof(IRFunction));
  if (!function) {
    return NULL;
  }

  function->name = mettle_strdup(name ? name : "<anonymous>");
  function->profile_id = IR_PROFILE_ID_NONE;
  function->parameter_names = NULL;
  function->parameter_types = NULL;
  function->parameter_count = 0;
  function->instructions = NULL;
  function->instruction_count = 0;
  function->instruction_capacity = 0;
  function->blocks = NULL;
  function->block_count = 0;
  function->entry_block = 0;
  function->cfg_valid = 0;
  function->is_inline = 0;
  function->is_inline_contract = 0;
  function->is_noinline = 0;
  function->is_pure = 0;
  function->is_noalloc = 0;
  return function;
}

int ir_function_set_parameters(IRFunction *function, const char **parameter_names,
                               const char **parameter_types,
                               size_t parameter_count) {
  if (!function) {
    return 0;
  }

  ir_function_clear_parameters(function);

  if (parameter_count == 0) {
    return 1;
  }

  char **name_copies = calloc(parameter_count, sizeof(char *));
  char **type_copies = calloc(parameter_count, sizeof(char *));
  if (!name_copies || !type_copies) {
    free(name_copies);
    free(type_copies);
    return 0;
  }

  for (size_t i = 0; i < parameter_count; i++) {
    if (!parameter_names || !parameter_names[i]) {
      goto fail;
    }

    name_copies[i] = mettle_strdup(parameter_names[i]);
    if (!name_copies[i]) {
      goto fail;
    }

    if (parameter_types && parameter_types[i]) {
      type_copies[i] = mettle_strdup(parameter_types[i]);
      if (!type_copies[i]) {
        goto fail;
      }
    }
  }

  function->parameter_names = name_copies;
  function->parameter_types = type_copies;
  function->parameter_count = parameter_count;
  return 1;

fail:
  for (size_t i = 0; i < parameter_count; i++) {
    free(name_copies[i]);
    free(type_copies[i]);
  }
  free(name_copies);
  free(type_copies);
  return 0;
}

void ir_function_destroy(IRFunction *function) {
  if (!function) {
    return;
  }

  free(function->name);
  ir_function_clear_parameters(function);
  ir_function_clear_cfg(function);
  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy(&function->instructions[i]);
  }
  free(function->instructions);
  free(function);
}

int ir_function_append_instruction(IRFunction *function,
                                   const IRInstruction *instruction) {
  if (!function || !instruction) {
    return 0;
  }

  if (function->instruction_count >= function->instruction_capacity) {
    size_t new_capacity = function->instruction_capacity == 0
                              ? 64
                              : function->instruction_capacity * 2;
    IRInstruction *new_instructions =
        realloc(function->instructions, new_capacity * sizeof(IRInstruction));
    if (!new_instructions) {
      return 0;
    }
    function->instructions = new_instructions;
    function->instruction_capacity = new_capacity;
  }

  IRInstruction *slot = &function->instructions[function->instruction_count];
  *slot = *instruction;

  slot->dest = ir_operand_clone(&instruction->dest);
  slot->lhs = ir_operand_clone(&instruction->lhs);
  slot->rhs = ir_operand_clone(&instruction->rhs);
  slot->text = mettle_strdup(instruction->text);
  slot->is_float = instruction->is_float;
  slot->arguments = NULL;
  slot->argument_count = instruction->argument_count;

  if (instruction->argument_count > 0) {
    slot->arguments = malloc(instruction->argument_count * sizeof(IROperand));
    if (!slot->arguments) {
      ir_instruction_destroy(slot);
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      slot->arguments[i] = ir_operand_clone(&instruction->arguments[i]);
    }
  }

  function->instruction_count++;
  ir_function_clear_cfg(function);
  return 1;
}

int ir_function_insert_instruction(IRFunction *function, size_t index,
                                   const IRInstruction *instruction) {
  if (!function || !instruction || index > function->instruction_count) {
    return 0;
  }

  if (function->instruction_count >= function->instruction_capacity) {
    size_t new_capacity = function->instruction_capacity == 0
                              ? 64
                              : function->instruction_capacity * 2;
    IRInstruction *new_instructions =
        realloc(function->instructions, new_capacity * sizeof(IRInstruction));
    if (!new_instructions) {
      return 0;
    }
    function->instructions = new_instructions;
    function->instruction_capacity = new_capacity;
  }

  if (index < function->instruction_count) {
    memmove(&function->instructions[index + 1], &function->instructions[index],
            (function->instruction_count - index) * sizeof(IRInstruction));
  }

  IRInstruction *slot = &function->instructions[index];
  memset(slot, 0, sizeof(*slot));
  slot->op = instruction->op;
  slot->location = instruction->location;
  slot->is_float = instruction->is_float;
  slot->float_bits = instruction->float_bits;
  slot->ast_ref = instruction->ast_ref;
  slot->dest = ir_operand_clone(&instruction->dest);
  slot->lhs = ir_operand_clone(&instruction->lhs);
  slot->rhs = ir_operand_clone(&instruction->rhs);
  slot->text = mettle_strdup(instruction->text);
  slot->argument_count = instruction->argument_count;
  slot->arguments = NULL;

  if (instruction->argument_count > 0) {
    slot->arguments = malloc(instruction->argument_count * sizeof(IROperand));
    if (!slot->arguments) {
      ir_instruction_destroy(slot);
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      slot->arguments[i] = ir_operand_clone(&instruction->arguments[i]);
    }
  }

  function->instruction_count++;
  ir_function_clear_cfg(function);
  return 1;
}

typedef struct {
  const char *label;
  size_t block_index;
} IRLabelBlock;

static int ir_instruction_is_terminator(const IRInstruction *instruction) {
  return instruction &&
         (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN);
}

static int ir_instruction_is_branch(const IRInstruction *instruction) {
  return instruction && (instruction->op == IR_OP_BRANCH_ZERO ||
                         instruction->op == IR_OP_BRANCH_EQ);
}

static int ir_block_append_index_unique(size_t **items, size_t *count,
                                        size_t value) {
  if (!items || !count) {
    return 0;
  }

  for (size_t i = 0; i < *count; i++) {
    if ((*items)[i] == value) {
      return 1;
    }
  }

  size_t *grown = realloc(*items, (*count + 1) * sizeof(size_t));
  if (!grown) {
    return 0;
  }

  grown[*count] = value;
  *items = grown;
  (*count)++;
  return 1;
}

static int ir_cfg_append_edge(IRFunction *function, size_t from, size_t to) {
  if (!function || from >= function->block_count || to >= function->block_count) {
    return 0;
  }

  IRBasicBlock *source = &function->blocks[from];
  IRBasicBlock *target = &function->blocks[to];
  if (!ir_block_append_index_unique(&source->successors,
                                    &source->successor_count, to)) {
    return 0;
  }
  return ir_block_append_index_unique(&target->predecessors,
                                      &target->predecessor_count, from);
}

static int ir_cfg_find_label_block(const IRLabelBlock *labels,
                                   size_t label_count, const char *label,
                                   size_t *out_block) {
  if (!labels || !label || !out_block) {
    return 0;
  }

  for (size_t i = 0; i < label_count; i++) {
    if (labels[i].label && strcmp(labels[i].label, label) == 0) {
      *out_block = labels[i].block_index;
      return 1;
    }
  }

  return 0;
}

static int ir_cfg_block_last_non_nop(const IRBasicBlock *block,
                                     size_t *out_offset) {
  if (!block || !out_offset || block->instruction_count == 0) {
    return 0;
  }

  for (size_t remaining = block->instruction_count; remaining > 0; remaining--) {
    size_t offset = remaining - 1;
    if (block->instructions[offset].op != IR_OP_NOP) {
      *out_offset = offset;
      return 1;
    }
  }

  return 0;
}

int ir_function_rebuild_cfg(IRFunction *function) {
  if (!function) {
    return 0;
  }

  ir_function_clear_cfg(function);

  size_t instruction_count = function->instruction_count;
  if (instruction_count == 0) {
    function->entry_block = 0;
    function->cfg_valid = 1;
    return 1;
  }

  unsigned char *block_starts = calloc(instruction_count, 1);
  if (!block_starts) {
    return 0;
  }

  block_starts[0] = 1;
  for (size_t i = 0; i < instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      block_starts[i] = 1;
    }
    if ((ir_instruction_is_terminator(instruction) ||
         ir_instruction_is_branch(instruction)) &&
        i + 1 < instruction_count) {
      block_starts[i + 1] = 1;
    }
  }

  size_t block_count = 0;
  for (size_t i = 0; i < instruction_count; i++) {
    if (block_starts[i]) {
      block_count++;
    }
  }

  IRBasicBlock *blocks = calloc(block_count, sizeof(IRBasicBlock));
  IRLabelBlock *labels = calloc(block_count, sizeof(IRLabelBlock));
  if (!blocks || !labels) {
    free(blocks);
    free(labels);
    free(block_starts);
    return 0;
  }

  size_t block_index = 0;
  size_t label_count = 0;
  for (size_t start = 0; start < instruction_count;) {
    if (!block_starts[start]) {
      start++;
      continue;
    }

    size_t end = start + 1;
    while (end < instruction_count && !block_starts[end]) {
      end++;
    }

    IRBasicBlock *block = &blocks[block_index];
    block->instructions = &function->instructions[start];
    block->instruction_count = end - start;
    block->first_instruction = start;

    IRInstruction *first = &function->instructions[start];
    if (first->op == IR_OP_LABEL && first->text) {
      block->label = first->text;
      labels[label_count].label = first->text;
      labels[label_count].block_index = block_index;
      label_count++;
    }

    block_index++;
    start = end;
  }

  function->blocks = blocks;
  function->block_count = block_count;
  function->entry_block = 0;

  for (size_t i = 0; i < block_count; i++) {
    IRBasicBlock *block = &function->blocks[i];
    size_t last_offset = 0;
    if (!ir_cfg_block_last_non_nop(block, &last_offset)) {
      if (i + 1 < block_count && !ir_cfg_append_edge(function, i, i + 1)) {
        ir_function_clear_cfg(function);
        free(labels);
        free(block_starts);
        return 0;
      }
      continue;
    }

    IRInstruction *last = &block->instructions[last_offset];
    if (last->op == IR_OP_JUMP) {
      size_t target = 0;
      if (ir_cfg_find_label_block(labels, label_count, last->text, &target) &&
          !ir_cfg_append_edge(function, i, target)) {
        ir_function_clear_cfg(function);
        free(labels);
        free(block_starts);
        return 0;
      }
    } else if (ir_instruction_is_branch(last)) {
      size_t target = 0;
      if (ir_cfg_find_label_block(labels, label_count, last->text, &target) &&
          !ir_cfg_append_edge(function, i, target)) {
        ir_function_clear_cfg(function);
        free(labels);
        free(block_starts);
        return 0;
      }
      if (i + 1 < block_count && !ir_cfg_append_edge(function, i, i + 1)) {
        ir_function_clear_cfg(function);
        free(labels);
        free(block_starts);
        return 0;
      }
    } else if (last->op != IR_OP_RETURN) {
      if (i + 1 < block_count && !ir_cfg_append_edge(function, i, i + 1)) {
        ir_function_clear_cfg(function);
        free(labels);
        free(block_starts);
        return 0;
      }
    }
  }

  function->cfg_valid = 1;
  free(labels);
  free(block_starts);
  return 1;
}

const IRBasicBlock *ir_function_blocks(IRFunction *function,
                                       size_t *block_count) {
  if (block_count) {
    *block_count = 0;
  }
  if (!function) {
    return NULL;
  }
  if (!function->cfg_valid && !ir_function_rebuild_cfg(function)) {
    return NULL;
  }
  if (block_count) {
    *block_count = function->block_count;
  }
  return function->blocks;
}

IRProgram *ir_program_create(void) {
  IRProgram *program = malloc(sizeof(IRProgram));
  if (!program) {
    return NULL;
  }

  program->functions = NULL;
  program->function_count = 0;
  program->function_capacity = 0;
  program->profile_entries = NULL;
  program->profile_entry_count = 0;
  program->profile_entry_capacity = 0;
  program->debug_local_entries = NULL;
  program->debug_local_entry_count = 0;
  program->debug_local_entry_capacity = 0;
  program->dead_functions_eliminated = 0;
  return program;
}

void ir_program_destroy(IRProgram *program) {
  if (!program) {
    return;
  }

  if (program->functions) {
    for (size_t i = 0; i < program->function_count; i++) {
      ir_function_destroy(program->functions[i]);
    }
    free(program->functions);
  }
  if (program->profile_entries) {
    for (size_t i = 0; i < program->profile_entry_count; i++) {
      free(program->profile_entries[i].name);
      free(program->profile_entries[i].filename);
    }
    free(program->profile_entries);
  }
  if (program->debug_local_entries) {
    for (size_t i = 0; i < program->debug_local_entry_count; i++) {
      free(program->debug_local_entries[i].name);
      free(program->debug_local_entries[i].type_name);
    }
    free(program->debug_local_entries);
  }
  free(program);
}

int ir_program_add_function(IRProgram *program, IRFunction *function) {
  if (!program || !function) {
    return 0;
  }

  if (program->function_count >= program->function_capacity) {
    size_t new_capacity =
        program->function_capacity == 0 ? 16 : program->function_capacity * 2;
    IRFunction **new_functions =
        realloc(program->functions, new_capacity * sizeof(IRFunction *));
    if (!new_functions) {
      return 0;
    }
    program->functions = new_functions;
    program->function_capacity = new_capacity;
  }

  program->functions[program->function_count++] = function;
  return 1;
}

const char *ir_opcode_name(IROpcode op) {
  switch (op) {
  case IR_OP_NOP:
    return "nop";
  case IR_OP_LABEL:
    return "label";
  case IR_OP_JUMP:
    return "jump";
  case IR_OP_BRANCH_ZERO:
    return "branch_zero";
  case IR_OP_BRANCH_EQ:
    return "branch_eq";
  case IR_OP_DECLARE_LOCAL:
    return "local";
  case IR_OP_ASSIGN:
    return "assign";
  case IR_OP_ADDRESS_OF:
    return "addr_of";
  case IR_OP_LOAD:
    return "load";
  case IR_OP_STORE:
    return "store";
  case IR_OP_PREFETCH:
    return "prefetch";
  case IR_OP_SELECT:
    return "select";
  case IR_OP_BINARY:
    return "binary";
  case IR_OP_ROTATE_ADD:
    return "rotate_add";
  case IR_OP_UNARY:
    return "unary";
  case IR_OP_CALL:
    return "call";
  case IR_OP_CALL_INDIRECT:
    return "call_indirect";
  case IR_OP_NEW:
    return "new";
  case IR_OP_RETURN:
    return "return";
  case IR_OP_INLINE_ASM:
    return "inline_asm";
  case IR_OP_CAST:
    return "cast";
  case IR_OP_COUNT_WORD_STARTS: return "count_word_starts";
  case IR_OP_MEMCPY_INLINE: return "memcpy_inline";
  case IR_OP_SIMD_SUM_I32: return "simd_sum_i32";
  case IR_OP_SIMD_SUM_U8: return "simd_sum_u8";
  case IR_OP_SIMD_BYTE_MAP: return "simd_byte_map";
  case IR_OP_SIMD_FILL: return "simd_fill";
  case IR_OP_SIMD_MATMUL_N32: return "simd_matmul_n32";
  case IR_OP_SIMD_INSERTION_SORT_I32: return "simd_insertion_sort_i32";
  case IR_OP_SIMD_DOT_I32: return "simd_dot_i32";
  case IR_OP_SIMD_DOT_I8: return "simd_dot_i8";
  case IR_OP_SIMD_SLP_MAC_I32: return "simd_slp_mac_i32";
  case IR_OP_SIMD_SLP_MAC_I8: return "simd_slp_mac_i8";
  case IR_OP_SIMD_SCALE_I32: return "simd_scale_i32";
  case IR_OP_SIMD_CLAMP_I32: return "simd_clamp_i32";
  case IR_OP_SIMD_REVERSE_COPY_I32: return "simd_reverse_copy_i32";
  case IR_OP_LOWER_BOUND_I32: return "lower_bound_i32";
  case IR_OP_PREFIX_SUM_I32: return "prefix_sum_i32";
  case IR_OP_SIMD_MINMAX_I32: return "simd_minmax_i32";
  case IR_OP_SIMD_SUM_F64: return "simd_sum_f64";
  case IR_OP_SIMD_SUM_F32: return "simd_sum_f32";
  case IR_OP_SIMD_DOT_F64: return "simd_dot_f64";
  case IR_OP_SIMD_DOT_F32: return "simd_dot_f32";
  case IR_OP_SIMD_AFFINE_MAP_F64: return "simd_affine_map_f64";
  case IR_OP_SIMD_AFFINE_MAP_F32: return "simd_affine_map_f32";
  case IR_OP_SIMD_EXP_F32: return "simd_exp_f32";
  case IR_OP_SIMD_I2F_REDUCE_F64: return "simd_i2f_reduce_f64";
  case IR_OP_SIMD_VLOOP_F64: return "simd_vloop_f64";
  case IR_OP_SIMD_VLOOP_I32: return "simd_vloop_i32";
  case IR_OP_SIMD_FIND: return "simd_find";
  case IR_OP_SIMD_OUTER_LANE_F64: return "simd_outer_lane_f64";
  default:
    return "unknown";
  }
}

static void ir_format_operand(const IROperand *operand, char *buffer,
                              size_t buffer_size) {
  if (!buffer || buffer_size == 0) {
    return;
  }

  if (!operand) {
    snprintf(buffer, buffer_size, "_");
    return;
  }

  switch (operand->kind) {
  case IR_OPERAND_NONE:
    snprintf(buffer, buffer_size, "_");
    break;
  case IR_OPERAND_TEMP:
    snprintf(buffer, buffer_size, "%%%s", operand->name ? operand->name : "?");
    break;
  case IR_OPERAND_SYMBOL:
    snprintf(buffer, buffer_size, "@%s", operand->name ? operand->name : "?");
    break;
  case IR_OPERAND_INT:
    snprintf(buffer, buffer_size, "%lld", operand->int_value);
    break;
  case IR_OPERAND_FLOAT:
    snprintf(buffer, buffer_size, "%f", operand->float_value);
    break;
  case IR_OPERAND_STRING:
    snprintf(buffer, buffer_size, "\"%s\"", operand->name ? operand->name : "");
    break;
  case IR_OPERAND_LABEL:
    snprintf(buffer, buffer_size, "%s", operand->name ? operand->name : "?");
    break;
  default:
    snprintf(buffer, buffer_size, "_");
    break;
  }
}

static int ir_format_instruction_line(const IRInstruction *instruction,
                                      char *buffer, size_t buffer_size) {
  char dest[IR_OPERAND_FMT_BUFSIZE];
  char lhs[IR_OPERAND_FMT_BUFSIZE];
  char rhs[IR_OPERAND_FMT_BUFSIZE];
  int written = 0;

  if (!instruction || !buffer || buffer_size == 0) {
    return 0;
  }

  ir_format_operand(&instruction->dest, dest, sizeof(dest));
  ir_format_operand(&instruction->lhs, lhs, sizeof(lhs));
  ir_format_operand(&instruction->rhs, rhs, sizeof(rhs));

  switch (instruction->op) {
  case IR_OP_LABEL:
    written = snprintf(buffer, buffer_size, "%s %s", ir_opcode_name(instruction->op),
                       instruction->text ? instruction->text : "<label>");
    break;
  case IR_OP_JUMP:
    written = snprintf(buffer, buffer_size, "%s %s", ir_opcode_name(instruction->op),
                       instruction->text ? instruction->text : "<target>");
    break;
  case IR_OP_BRANCH_ZERO:
    written = snprintf(buffer, buffer_size, "%s %s -> %s",
                       ir_opcode_name(instruction->op), lhs,
                       instruction->text ? instruction->text : "<target>");
    break;
  case IR_OP_BRANCH_EQ:
    written = snprintf(buffer, buffer_size, "%s %s, %s -> %s",
                       ir_opcode_name(instruction->op), lhs, rhs,
                       instruction->text ? instruction->text : "<target>");
    break;
  case IR_OP_DECLARE_LOCAL:
    written = snprintf(buffer, buffer_size, "%s %s : %s",
                       ir_opcode_name(instruction->op), dest,
                       instruction->text ? instruction->text : "<unknown>");
    break;
  case IR_OP_ASSIGN:
    written = snprintf(buffer, buffer_size, "%s <- %s", dest, lhs);
    break;
  case IR_OP_ADDRESS_OF:
    written = snprintf(buffer, buffer_size, "%s <- &%s", dest, lhs);
    break;
  case IR_OP_LOAD:
    written = snprintf(buffer, buffer_size, "%s <- *%s [%s]", dest, lhs, rhs);
    break;
  case IR_OP_STORE:
    written = snprintf(buffer, buffer_size, "*%s <- %s [%s]", dest, lhs, rhs);
    break;
  case IR_OP_BINARY:
    written = snprintf(buffer, buffer_size, "%s = %s %s%s %s", dest, lhs,
                       instruction->text ? instruction->text : "?",
                       instruction->is_float ? " (float)" : "", rhs);
    break;
  case IR_OP_ROTATE_ADD:
    written = snprintf(buffer, buffer_size, "%s = rotate_add(%s, %s)", dest, lhs,
                       rhs);
    break;
  case IR_OP_UNARY:
    written = snprintf(buffer, buffer_size, "%s = %s%s%s", dest,
                       instruction->text ? instruction->text : "?", lhs,
                       instruction->is_float ? " (float)" : "");
    break;
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT: {
    size_t offset = 0;
    offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "%s = %s(",
                               dest, instruction->text ? instruction->text
                                                       : "<callee>");
    for (size_t arg_i = 0; arg_i < instruction->argument_count; arg_i++) {
      char arg_buffer[128];
      ir_format_operand(&instruction->arguments[arg_i], arg_buffer,
                        sizeof(arg_buffer));
      offset +=
          (size_t)snprintf(buffer + offset, buffer_size - offset, "%s%s",
                           arg_i == 0 ? "" : ", ", arg_buffer);
      if (offset >= buffer_size) {
        break;
      }
    }
    written = (int)snprintf(buffer + offset, buffer_size - offset, ")");
    if (written >= 0) {
      written += (int)offset;
    }
    break;
  }
  case IR_OP_NEW:
    written = snprintf(buffer, buffer_size, "%s = %s [%s]", dest,
                       instruction->text ? instruction->text : "<type>", rhs);
    break;
  case IR_OP_RETURN:
    written = snprintf(buffer, buffer_size, "return %s", lhs);
    break;
  case IR_OP_INLINE_ASM:
    written = snprintf(buffer, buffer_size, "inline_asm \"%s\"",
                       instruction->text ? instruction->text : "");
    break;
  case IR_OP_CAST:
    written = snprintf(buffer, buffer_size, "%s = (%s)%s%s", dest,
                       instruction->text ? instruction->text : "<type>", lhs,
                       instruction->is_float ? " (float)" : "");
    break;
  case IR_OP_COUNT_WORD_STARTS:
    written = snprintf(buffer, buffer_size, "%s = count_word_starts(buf=%s, len=%s)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SELECT: {
    char else_val[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      else_val, sizeof(else_val));
    if (instruction->text && instruction->argument_count > 1) {
      char cmp_rhs[128];
      ir_format_operand(&instruction->arguments[1], cmp_rhs, sizeof(cmp_rhs));
      written = snprintf(buffer, buffer_size, "%s = select(%s %s %s, %s, %s)",
                         dest, lhs, instruction->text, cmp_rhs, rhs, else_val);
    } else {
      written = snprintf(buffer, buffer_size, "%s = select(%s, %s, %s)", dest,
                         lhs, rhs, else_val);
    }
    break;
  }
  case IR_OP_MEMCPY_INLINE:
    written = snprintf(buffer, buffer_size, "%s = memcpy_inline %s, %s", dest,
                       lhs, rhs);
    break;
  case IR_OP_SIMD_SUM_I32:
    written = snprintf(buffer, buffer_size, "%s += simd_sum_i32(base=%s, len=%s)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SIMD_SUM_U8:
    written = snprintf(buffer, buffer_size, "%s += simd_sum_u8(base=%s, len=%s)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SIMD_BYTE_MAP:
    written = snprintf(buffer, buffer_size,
                       "simd_byte_map(base=%s, len=%s, steps=%zu)", lhs, rhs,
                       instruction->argument_count / 2);
    break;
  case IR_OP_SIMD_FILL: {
    char value[128];
    ir_format_operand(instruction->argument_count > 2 ? &instruction->arguments[2]
                                                      : NULL,
                      value, sizeof(value));
    written = snprintf(
        buffer, buffer_size, "simd_fill(%s=%s, %s=%s, size=%lld, value=%s)",
        (instruction->argument_count > 1 &&
         instruction->arguments[1].int_value == 1)
            ? "begin"
            : "base",
        lhs,
        (instruction->argument_count > 1 &&
         instruction->arguments[1].int_value == 1)
            ? "end"
            : "len",
        rhs,
        instruction->argument_count > 0 ? instruction->arguments[0].int_value
                                        : 0,
        value);
    break;
  }
  case IR_OP_SIMD_MATMUL_N32:
    written = snprintf(buffer, buffer_size, "%s = matmul_n32(c=%s, a=%s, b=%s)",
                       dest, dest, lhs, rhs);
    break;
  case IR_OP_SIMD_INSERTION_SORT_I32:
    written = snprintf(buffer, buffer_size, "simd_insertion_sort_i32(base=%s, len=%s)",
                       dest, rhs);
    break;
  case IR_OP_SIMD_DOT_I32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size, "%s = dot_i32(a=%s, b=%s, len=%s)",
                       dest, lhs, rhs, len);
    break;
  }
  case IR_OP_SIMD_DOT_I8: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size, "%s = dot_i8(a=%s, b=%s, len=%s)",
                       dest, lhs, rhs, len);
    break;
  }
  case IR_OP_SIMD_SLP_MAC_I32:
    written = snprintf(buffer, buffer_size,
                       "slp_mac_i32(out=%s, a=%s, b=%s, K/n/aoff/boff/str/ooff)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SIMD_SLP_MAC_I8:
    written = snprintf(buffer, buffer_size,
                       "slp_mac_i8(out=%s, a=%s, b=%s, K/n/aoff/boff/str/ooff)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SIMD_SCALE_I32: {
    char len[128], mul[128], add[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    ir_format_operand(instruction->argument_count > 1 ? &instruction->arguments[1]
                                                      : NULL,
                      mul, sizeof(mul));
    ir_format_operand(instruction->argument_count > 2 ? &instruction->arguments[2]
                                                      : NULL,
                      add, sizeof(add));
    written = snprintf(buffer, buffer_size,
                       "%s = scale_i32(src=%s, dst=%s, len=%s, mul=%s, add=%s)",
                       dest, lhs, rhs, len, mul, add);
    break;
  }
  case IR_OP_SIMD_CLAMP_I32: {
    char len[128], lo[128], hi[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    ir_format_operand(instruction->argument_count > 1 ? &instruction->arguments[1]
                                                      : NULL,
                      lo, sizeof(lo));
    ir_format_operand(instruction->argument_count > 2 ? &instruction->arguments[2]
                                                      : NULL,
                      hi, sizeof(hi));
    written = snprintf(buffer, buffer_size,
                       "%s = clamp_i32(src=%s, dst=%s, len=%s, lo=%s, hi=%s)",
                       dest, lhs, rhs, len, lo, hi);
    break;
  }
  case IR_OP_SIMD_REVERSE_COPY_I32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size,
                       "%s = reverse_copy_i32(src=%s, dst=%s, len=%s)", dest,
                       lhs, rhs, len);
    break;
  }
  case IR_OP_LOWER_BOUND_I32: {
    char key[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      key, sizeof(key));
    written = snprintf(buffer, buffer_size,
                       "%s = lower_bound_i32(arr=%s, n=%s, key=%s)", dest, lhs,
                       rhs, key);
    break;
  }
  case IR_OP_PREFIX_SUM_I32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size,
                       "%s = prefix_sum_i32(src=%s, dst=%s, len=%s)", dest,
                       lhs, rhs, len);
    break;
  }
  case IR_OP_SIMD_MINMAX_I32: {
    char maxv[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      maxv, sizeof(maxv));
    written = snprintf(buffer, buffer_size,
                       "%s = minmax_i32(arr=%s, n=%s, max=%s)", dest, lhs, rhs,
                       maxv);
    break;
  }
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
    written = snprintf(buffer, buffer_size, "%s += %s(base=%s, len=%s)", dest,
                       ir_opcode_name(instruction->op), lhs, rhs);
    break;
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size, "%s += %s(a=%s, b=%s, len=%s)", dest,
                       ir_opcode_name(instruction->op), lhs, rhs, len);
    break;
  }
  case IR_OP_SIMD_I2F_REDUCE_F64: {
    char trip[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      trip, sizeof(trip));
    written = snprintf(buffer, buffer_size, "%s += %s(n=%s, steps=%zu)", dest,
                       ir_opcode_name(instruction->op), trip,
                       instruction->argument_count > 0
                           ? (instruction->argument_count - 1) / 2
                           : 0);
    break;
  }
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32: {
    long long reduce_op = (instruction->argument_count > 0 &&
                           instruction->arguments[0].kind == IR_OPERAND_INT)
                              ? instruction->arguments[0].int_value
                              : -1;
    long long n_nodes = (instruction->argument_count > 2 &&
                         instruction->arguments[2].kind == IR_OPERAND_INT)
                            ? instruction->arguments[2].int_value
                            : -1;
    written = snprintf(buffer, buffer_size, "%s %s %s(%s nodes=%lld)", dest,
                       reduce_op == 1 ? "+=" : "<-",
                       ir_opcode_name(instruction->op),
                       reduce_op == 1 ? "reduce" : "map", n_nodes);
    break;
  }
  case IR_OP_SIMD_FIND: {
    char base[128];
    ir_format_operand(&instruction->rhs, base, sizeof(base));
    written = snprintf(buffer, buffer_size, "%s <- %s(a=%s, n=%s)", dest,
                       ir_opcode_name(instruction->op), base, lhs);
    break;
  }
  case IR_OP_SIMD_OUTER_LANE_F64:
    written = snprintf(buffer, buffer_size, "%s += %s(outerP=%s, args=%zu)", dest,
                       ir_opcode_name(instruction->op), lhs,
                       instruction->argument_count);
    break;
  case IR_OP_SIMD_EXP_F32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size, "exp_f32(a=%s, len=%s)", dest, len);
    break;
  }
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32: {
    char len[128];
    char src_scale[128];
    char dst_scale[128];
    char bias[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    ir_format_operand(instruction->argument_count > 1 ? &instruction->arguments[1]
                                                      : NULL,
                      src_scale, sizeof(src_scale));
    ir_format_operand(instruction->argument_count > 2 ? &instruction->arguments[2]
                                                      : NULL,
                      dst_scale, sizeof(dst_scale));
    ir_format_operand(instruction->argument_count > 3 ? &instruction->arguments[3]
                                                      : NULL,
                      bias, sizeof(bias));
    written = snprintf(buffer, buffer_size,
                       "%s = %s(src=%s, dst=%s, len=%s, a=%s, b=%s, c=%s)",
                       dest, ir_opcode_name(instruction->op), lhs, rhs, len,
                       src_scale, dst_scale, bias);
    break;
  }
  case IR_OP_NOP:
  default:
    written = snprintf(buffer, buffer_size, "%s", ir_opcode_name(instruction->op));
    break;
  }

  return written > 0 && (size_t)written < buffer_size;
}

int ir_instruction_dump(const IRInstruction *instruction,
                        char *buffer, size_t capacity) {
  if (!instruction || !buffer || capacity == 0) {
    return 0;
  }
  return ir_format_instruction_line(instruction, buffer, capacity);
}

static void ir_dump_block_edges(FILE *output, const char *label,
                                const size_t *items, size_t count) {
  fprintf(output, " %s:", label);
  if (count == 0) {
    fprintf(output, " -");
    return;
  }

  for (size_t i = 0; i < count; i++) {
    fprintf(output, "%s%zu", i == 0 ? " " : ",", items[i]);
  }
}

/* Replace insn->text with a copy of `name`, freeing the old text. */
static int ir_retarget_call(IRInstruction *insn, const char *name) {
  char *copy = mettle_strdup(name);
  if (!copy) {
    return 0;
  }
  free(insn->text);
  insn->text = copy;
  return 1;
}

int ir_program_route_to_native_heap(IRProgram *program) {
  if (!program) {
    return 0;
  }
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    /* Never rewrite inside the allocator shims themselves. They bottom out on
     * the OS page layer (never malloc/free/new), so no rewrite is needed
     * today; the guard makes that invariant robust against a future shim edit
     * that would otherwise turn a literal free or malloc call into
     * self-recursion. The "mettle_heap_" prefix is distinctive enough not to
     * collide with user code, and the allocator core calls no allocation
     * surface either, so it needs no guard. */
    if (fn->name && strncmp(fn->name, "mettle_heap_", 12) == 0) {
      continue;
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *insn = &fn->instructions[i];

      if (insn->op == IR_OP_NEW) {
        /* new T  ->  mettle_heap_zeroed(sizeof T). The size is in rhs. An
         * absent or non-positive constant size means a default object (8
         * bytes), the same fallback the backend's original new lowering used.
         * Any other operand (the normal case is an INT constant; TEMP/SYMBOL
         * are copied faithfully, duplicating an owned name) is forwarded. */
        IROperand size_arg;
        memset(&size_arg, 0, sizeof(size_arg));
        if (insn->rhs.kind == IR_OPERAND_NONE ||
            (insn->rhs.kind == IR_OPERAND_INT && insn->rhs.int_value <= 0)) {
          size_arg.kind = IR_OPERAND_INT;
          size_arg.int_value = 8;
        } else {
          size_arg = insn->rhs;
          if (insn->rhs.name) {
            size_arg.name = mettle_strdup(insn->rhs.name);
            if (!size_arg.name) {
              return 0;
            }
          }
        }

        IROperand *args = (IROperand *)calloc(1, sizeof(IROperand));
        char *callee = mettle_strdup("mettle_heap_zeroed");
        if (!args || !callee) {
          free(args);
          free(callee);
          return 0;
        }
        args[0] = size_arg;
        free(insn->text);
        insn->text = callee;
        insn->arguments = args;
        insn->argument_count = 1;
        insn->op = IR_OP_CALL;
        /* rhs ownership moved into args[0] (TEMP name re-duplicated above), so
         * detach it from the instruction to avoid a double free. */
        memset(&insn->rhs, 0, sizeof(insn->rhs));
        insn->rhs.kind = IR_OPERAND_NONE;
        continue;
      }

      if (insn->op == IR_OP_CALL && insn->text) {
        if (strcmp(insn->text, "malloc") == 0 && insn->argument_count == 1) {
          if (!ir_retarget_call(insn, "mettle_heap_alloc")) return 0;
        } else if (strcmp(insn->text, "calloc") == 0 &&
                   insn->argument_count == 2) {
          if (!ir_retarget_call(insn, "mettle_heap_calloc")) return 0;
        } else if (strcmp(insn->text, "realloc") == 0 &&
                   insn->argument_count == 2) {
          if (!ir_retarget_call(insn, "mettle_heap_realloc")) return 0;
        } else if (strcmp(insn->text, "free") == 0 &&
                   insn->argument_count == 1) {
          if (!ir_retarget_call(insn, "mettle_heap_free")) return 0;
        }
      }
    }
    /* Calls were added/retyped; the cached CFG (if any) is unaffected in shape,
     * but mark it stale so any later consumer rebuilds rather than trusting
     * per-op metadata. */
    ir_function_clear_cfg(fn);
  }
  return 1;
}

/* Open-addressed name -> function-index table for the dead-function sweep.
 * Lookups happen once per operand of every instruction, so a linear name scan
 * would go quadratic on large multi-function programs (the same trap as the
 * historical per-element strcmp compile-speed bugs). */
typedef struct {
  const char **names;
  size_t *indices;
  size_t capacity; /* power of two */
} IrFnNameTable;

static size_t ir_fn_name_hash(const char *name) {
  /* FNV-1a. */
  size_t hash = 1469598103934665603ull;
  while (*name) {
    hash ^= (unsigned char)*name++;
    hash *= 1099511628211ull;
  }
  return hash;
}

static int ir_fn_table_init(IrFnNameTable *table, size_t function_count) {
  size_t capacity = 16;
  while (capacity < function_count * 2) {
    capacity *= 2;
  }
  table->names = (const char **)calloc(capacity, sizeof(*table->names));
  table->indices = (size_t *)calloc(capacity, sizeof(*table->indices));
  table->capacity = capacity;
  if (!table->names || !table->indices) {
    free(table->names);
    free(table->indices);
    return 0;
  }
  return 1;
}

static void ir_fn_table_insert(IrFnNameTable *table, const char *name,
                               size_t index) {
  size_t slot = ir_fn_name_hash(name) & (table->capacity - 1);
  while (table->names[slot]) {
    if (strcmp(table->names[slot], name) == 0) {
      return; /* First definition wins; duplicates would be a sema error. */
    }
    slot = (slot + 1) & (table->capacity - 1);
  }
  table->names[slot] = name;
  table->indices[slot] = index;
}

/* Returns the function index for `name`, or (size_t)-1. */
static size_t ir_fn_table_lookup(const IrFnNameTable *table, const char *name) {
  size_t slot = ir_fn_name_hash(name) & (table->capacity - 1);
  while (table->names[slot]) {
    if (strcmp(table->names[slot], name) == 0) {
      return table->indices[slot];
    }
    slot = (slot + 1) & (table->capacity - 1);
  }
  return (size_t)-1;
}

static void ir_dead_fn_mark(const IrFnNameTable *table, const char *name,
                            unsigned char *live, size_t *worklist,
                            size_t *worklist_count) {
  size_t index;

  if (!name) {
    return;
  }
  index = ir_fn_table_lookup(table, name);
  if (index != (size_t)-1 && !live[index]) {
    live[index] = 1;
    worklist[(*worklist_count)++] = index;
  }
}

int ir_program_eliminate_dead_functions(IRProgram *program) {
  IrFnNameTable table;
  unsigned char *live = NULL;
  size_t *worklist = NULL;
  size_t worklist_count = 0;
  size_t n;
  size_t kept = 0;
  int found_main = 0;

  if (!program || program->function_count == 0) {
    return 1;
  }
  n = program->function_count;

  if (!ir_fn_table_init(&table, n)) {
    return 0;
  }
  live = (unsigned char *)calloc(n, 1);
  worklist = (size_t *)malloc(n * sizeof(*worklist));
  if (!live || !worklist) {
    free(table.names);
    free(table.indices);
    free(live);
    free(worklist);
    return 0;
  }

  for (size_t i = 0; i < n; i++) {
    if (program->functions[i] && program->functions[i]->name) {
      ir_fn_table_insert(&table, program->functions[i]->name, i);
      if (strcmp(program->functions[i]->name, "main") == 0) {
        live[i] = 1;
        worklist[worklist_count++] = i;
        found_main = 1;
      }
    }
  }

  /* No main means this is not a normal executable image; touch nothing. */
  if (!found_main) {
    free(table.names);
    free(table.indices);
    free(live);
    free(worklist);
    return 1;
  }

  /* A function is referenced when any instruction of a live function carries
   * its name: in `text` (direct calls, defer captures, rewritten intrinsics),
   * in a SYMBOL operand (function-pointer uses like `run(mix)`), or in a
   * STRING operand (e.g. `dispatch` kernel-name launches). Local variables
   * shadowing a function name over-approximate to "live", which only costs
   * bytes, never correctness. */
  while (worklist_count > 0) {
    IRFunction *fn = program->functions[worklist[--worklist_count]];
    if (!fn) {
      continue;
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *insn = &fn->instructions[i];
      ir_dead_fn_mark(&table, insn->text, live, worklist, &worklist_count);
      if (insn->dest.kind == IR_OPERAND_SYMBOL) {
        ir_dead_fn_mark(&table, insn->dest.name, live, worklist,
                        &worklist_count);
      }
      if (insn->lhs.kind == IR_OPERAND_SYMBOL ||
          insn->lhs.kind == IR_OPERAND_STRING) {
        ir_dead_fn_mark(&table, insn->lhs.name, live, worklist,
                        &worklist_count);
      }
      if (insn->rhs.kind == IR_OPERAND_SYMBOL ||
          insn->rhs.kind == IR_OPERAND_STRING) {
        ir_dead_fn_mark(&table, insn->rhs.name, live, worklist,
                        &worklist_count);
      }
      for (size_t a = 0; a < insn->argument_count; a++) {
        const IROperand *arg = &insn->arguments[a];
        if (arg->kind == IR_OPERAND_SYMBOL || arg->kind == IR_OPERAND_STRING) {
          ir_dead_fn_mark(&table, arg->name, live, worklist, &worklist_count);
        }
      }
    }
  }

  /* Freeing thousands of dead post-inline bodies instruction-by-instruction
   * costs real time on large programs and the process reclaims it at exit;
   * only pay for it when deep teardown is explicitly requested. */
  static int full_cleanup = -1;
  if (full_cleanup < 0) {
    full_cleanup = getenv("METTLE_FULL_CLEANUP") ? 1 : 0;
  }
  for (size_t i = 0; i < n; i++) {
    if (live[i]) {
      program->functions[kept++] = program->functions[i];
    } else if (program->functions[i]) {
      if (full_cleanup) {
        ir_function_destroy(program->functions[i]);
      }
    }
  }
  program->function_count = kept;
  program->dead_functions_eliminated = 1;

  free(table.names);
  free(table.indices);
  free(live);
  free(worklist);
  return 1;
}

int ir_program_dump(IRProgram *program, FILE *output) {
  if (!program || !output) {
    return 0;
  }

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }

    fprintf(output, "function %s {\n",
            function->name ? function->name : "<anonymous>");

    if (!ir_function_rebuild_cfg(function)) {
      return 0;
    }

    for (size_t block_i = 0; block_i < function->block_count; block_i++) {
      IRBasicBlock *block = &function->blocks[block_i];
      const char *label = block->label ? block->label
                          : block_i == function->entry_block ? "<entry>"
                                                              : "<anon>";
      size_t last_instruction =
          block->instruction_count == 0
              ? block->first_instruction
              : block->first_instruction + block->instruction_count - 1;

      fprintf(output, "  block %zu %s [%zu..%zu]", block_i, label,
              block->first_instruction, last_instruction);
      ir_dump_block_edges(output, "preds", block->predecessors,
                          block->predecessor_count);
      ir_dump_block_edges(output, "succs", block->successors,
                          block->successor_count);
      fprintf(output, "\n");

      for (size_t j = 0; j < block->instruction_count; j++) {
        size_t instruction_index = block->first_instruction + j;
        IRInstruction *instruction = &block->instructions[j];
        char buffer[1024];
        ir_format_instruction_line(instruction, buffer, sizeof(buffer));
        fprintf(output, "    %4zu: %s\n", instruction_index, buffer);
      }
    }

    fprintf(output, "}\n\n");
  }

  return 1;
}
