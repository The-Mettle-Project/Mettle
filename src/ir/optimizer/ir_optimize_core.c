#include "ir_optimize_internal.h"


int ir_operand_clone(const IROperand *source, IROperand *out) {
  if (!out) {
    return 0;
  }

  *out = ir_operand_none();
  if (!source) {
    return 1;
  }

  out->kind = source->kind;
  out->int_value = source->int_value;
  out->float_value = source->float_value;
  out->float_bits = source->float_bits;

  switch (source->kind) {
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_STRING:
  case IR_OPERAND_LABEL:
    if (!source->name) {
      return 0;
    }
    out->name = mettle_strdup(source->name);
    if (!out->name) {
      *out = ir_operand_none();
      return 0;
    }
    break;
  default:
    break;
  }

  return 1;
}

static int ir_name_map_find(const IRNameMap *map, const char *from) {
  if (!map || !from) {
    return -1;
  }

  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].from && strcmp(map->items[i].from, from) == 0) {
      return (int)i;
    }
  }

  return -1;
}

const char *ir_name_map_lookup(const IRNameMap *map, const char *from) {
  int index = ir_name_map_find(map, from);
  if (index < 0) {
    return NULL;
  }
  return map->items[index].to;
}

int ir_name_map_add(IRNameMap *map, const char *from, const char *to) {
  if (!map || !from || !to) {
    return 0;
  }

  if (ir_name_map_find(map, from) >= 0) {
    return 1;
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRNameMapEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRNameMapEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  char *from_copy = mettle_strdup(from);
  char *to_copy = mettle_strdup(to);
  if (!from_copy || !to_copy) {
    free(from_copy);
    free(to_copy);
    return 0;
  }

  map->items[map->count].from = from_copy;
  map->items[map->count].to = to_copy;
  map->count++;
  return 1;
}

void ir_name_map_destroy(IRNameMap *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].from);
    free(map->items[i].to);
  }
  free(map->items);
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
}

char *ir_make_inline_prefix(const char *callee_name, size_t inline_id) {
  const char *name = callee_name ? callee_name : "func";
  int length = snprintf(NULL, 0, "__inl_%s_%zu", name, inline_id);
  if (length < 0) {
    return NULL;
  }

  size_t size = (size_t)length + 1;
  char *prefix = malloc(size);
  if (!prefix) {
    return NULL;
  }

  snprintf(prefix, size, "__inl_%s_%zu", name, inline_id);
  return prefix;
}

char *ir_make_inline_name(const char *prefix, const char *kind,
                                 const char *base) {
  if (!prefix || !kind || !base) {
    return NULL;
  }

  int length = snprintf(NULL, 0, "%s_%s_%s", prefix, kind, base);
  if (length < 0) {
    return NULL;
  }

  size_t size = (size_t)length + 1;
  char *name = malloc(size);
  if (!name) {
    return NULL;
  }
  snprintf(name, size, "%s_%s_%s", prefix, kind, base);
  return name;
}

const char *ir_name_map_get_or_create(IRNameMap *map, const char *from,
                                             const char *prefix,
                                             const char *kind) {
  const char *existing = ir_name_map_lookup(map, from);
  if (existing) {
    return existing;
  }

  char *generated = ir_make_inline_name(prefix, kind, from);
  if (!generated) {
    return NULL;
  }

  int ok = ir_name_map_add(map, from, generated);
  free(generated);
  if (!ok) {
    return NULL;
  }

  return ir_name_map_lookup(map, from);
}

int ir_instruction_writes_temp(const IRInstruction *instruction) {
  if (!instruction || instruction->dest.kind != IR_OPERAND_TEMP ||
      !instruction->dest.name) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_NEW:
  case IR_OP_CAST:
    return 1;
  default:
    return 0;
  }
}

int ir_instruction_writes_symbol(const IRInstruction *instruction) {
  if (!instruction || instruction->dest.kind != IR_OPERAND_SYMBOL ||
      !instruction->dest.name) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_NEW:
  case IR_OP_CAST:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
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
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_OUTER_LANE_F64:
    return 1;
  default:
    return 0;
  }
}

int ir_instruction_writes_destination(const IRInstruction *instruction) {
  if (!instruction || instruction->dest.kind == IR_OPERAND_NONE) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_NEW:
  case IR_OP_CAST:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
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
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_OUTER_LANE_F64:
    return 1;
  default:
    return 0;
  }
}

int ir_instruction_is_trivially_dead_if_dest_unused(
    const IRInstruction *instruction) {
  if (!instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
    return 1;
  default:
    return 0;
  }
}

int ir_operand_is_propagatable_value(const IROperand *operand) {
  if (!operand) {
    return 0;
  }

  switch (operand->kind) {
  case IR_OPERAND_INT:
  case IR_OPERAND_FLOAT:
  case IR_OPERAND_STRING:
  case IR_OPERAND_LABEL:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_TEMP:
    return 1;
  default:
    return 0;
  }
}

void ir_instruction_clear_arguments(IRInstruction *instruction) {
  if (!instruction || !instruction->arguments) {
    return;
  }

  for (size_t i = 0; i < instruction->argument_count; i++) {
    ir_operand_destroy(&instruction->arguments[i]);
  }
  free(instruction->arguments);
  instruction->arguments = NULL;
  instruction->argument_count = 0;
}

void ir_instruction_make_nop(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  free(instruction->text);
  instruction->text = NULL;
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  instruction->op = IR_OP_NOP;
}

void ir_instruction_make_jump(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  instruction->op = IR_OP_JUMP;
}

void ir_instruction_destroy_storage(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  free(instruction->text);
  instruction->text = NULL;
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  instruction->op = IR_OP_NOP;
}

int ir_instruction_vector_append_move(IRInstructionVector *vector,
                                             IRInstruction *instruction) {
  if (!vector || !instruction) {
    return 0;
  }

  if (vector->count >= vector->capacity) {
    size_t new_capacity = vector->capacity == 0 ? 64 : vector->capacity * 2;
    IRInstruction *new_items =
        realloc(vector->items, new_capacity * sizeof(IRInstruction));
    if (!new_items) {
      return 0;
    }
    vector->items = new_items;
    vector->capacity = new_capacity;
  }

  vector->items[vector->count++] = *instruction;
  instruction->op = IR_OP_NOP;
  instruction->dest = ir_operand_none();
  instruction->lhs = ir_operand_none();
  instruction->rhs = ir_operand_none();
  instruction->text = NULL;
  instruction->arguments = NULL;
  instruction->argument_count = 0;
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  return 1;
}

void ir_instruction_vector_destroy(IRInstructionVector *vector) {
  if (!vector) {
    return;
  }

  for (size_t i = 0; i < vector->count; i++) {
    ir_instruction_destroy_storage(&vector->items[i]);
  }
  free(vector->items);
  vector->items = NULL;
  vector->count = 0;
  vector->capacity = 0;
}

int ir_index_vector_append(IRIndexVector *vector, size_t value) {
  if (!vector) {
    return 0;
  }

  if (vector->count >= vector->capacity) {
    size_t new_capacity = vector->capacity == 0 ? 16 : vector->capacity * 2;
    size_t *new_items = realloc(vector->items, new_capacity * sizeof(size_t));
    if (!new_items) {
      return 0;
    }
    vector->items = new_items;
    vector->capacity = new_capacity;
  }

  vector->items[vector->count++] = value;
  return 1;
}

void ir_index_vector_destroy(IRIndexVector *vector) {
  if (!vector) {
    return;
  }
  free(vector->items);
  vector->items = NULL;
  vector->count = 0;
  vector->capacity = 0;
}

int ir_rewrite_to_assign_operand(IRInstruction *instruction,
                                        const IROperand *value, int *changed) {
  IROperand cloned = ir_operand_none();
  if (!instruction || !value || !ir_operand_clone(value, &cloned)) {
    return 0;
  }

  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  free(instruction->text);
  instruction->text = NULL;

  instruction->op = IR_OP_ASSIGN;
  instruction->lhs = cloned;
  instruction->rhs = ir_operand_none();
  instruction->is_float = (cloned.kind == IR_OPERAND_FLOAT) ? 1 : 0;
  if (instruction->is_float) {
    instruction->float_bits = (cloned.float_bits == 32) ? 32 : 64;
  }
  instruction->ast_ref = NULL;

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_rewrite_to_assign_int(IRInstruction *instruction, long long value,
                                    int *changed) {
  IROperand constant = ir_operand_int(value);
  return ir_rewrite_to_assign_operand(instruction, &constant, changed);
}

int ir_temp_value_map_init(IRTempValueMap *map) {
  if (!map) {
    return 0;
  }

  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  return 1;
}

static int ir_temp_value_map_find(const IRTempValueMap *map, const char *name) {
  if (!map || !name) {
    return -1;
  }

  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].name && strcmp(map->items[i].name, name) == 0) {
      return (int)i;
    }
  }

  return -1;
}

void ir_temp_value_map_remove(IRTempValueMap *map, const char *name) {
  if (!map || !name) {
    return;
  }

  int index = ir_temp_value_map_find(map, name);
  if (index < 0) {
    return;
  }

  size_t idx = (size_t)index;
  free(map->items[idx].name);
  ir_operand_destroy(&map->items[idx].value);

  for (size_t i = idx + 1; i < map->count; i++) {
    map->items[i - 1] = map->items[i];
  }
  map->count--;
}

int ir_temp_value_map_set(IRTempValueMap *map, const char *name,
                                 const IROperand *value) {
  if (!map || !name || !value) {
    return 0;
  }

  int existing = ir_temp_value_map_find(map, name);
  if (existing >= 0) {
    IROperand cloned = ir_operand_none();
    if (!ir_operand_clone(value, &cloned)) {
      return 0;
    }

    ir_operand_destroy(&map->items[existing].value);
    map->items[existing].value = cloned;
    return 1;
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRTempValueEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRTempValueEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  char *name_copy = mettle_strdup(name);
  IROperand cloned = ir_operand_none();
  if (!name_copy || !ir_operand_clone(value, &cloned)) {
    free(name_copy);
    ir_operand_destroy(&cloned);
    return 0;
  }

  map->items[map->count].name = name_copy;
  map->items[map->count].value = cloned;
  map->count++;
  return 1;
}

void ir_temp_value_map_remove_symbol_values(IRTempValueMap *map,
                                                   const char *symbol_name) {
  if (!map) {
    return;
  }

  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    IRTempValueEntry *entry = &map->items[read];
    int remove = 0;

    if (entry->value.kind == IR_OPERAND_SYMBOL && entry->value.name) {
      if (!symbol_name || strcmp(entry->value.name, symbol_name) == 0) {
        remove = 1;
      }
    }

    if (remove) {
      free(entry->name);
      ir_operand_destroy(&entry->value);
      continue;
    }

    if (write != read) {
      map->items[write] = map->items[read];
    }
    write++;
  }

  map->count = write;
}

const IROperand *ir_temp_value_map_lookup(const IRTempValueMap *map,
                                                 const char *name) {
  if (!map || !name) {
    return NULL;
  }

  int index = ir_temp_value_map_find(map, name);
  if (index < 0) {
    return NULL;
  }

  return &map->items[index].value;
}

void ir_temp_value_map_clear(IRTempValueMap *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].name);
    ir_operand_destroy(&map->items[i].value);
  }
  map->count = 0;
}

void ir_temp_value_map_destroy(IRTempValueMap *map) {
  if (!map) {
    return;
  }

  ir_temp_value_map_clear(map);
  free(map->items);
  map->items = NULL;
  map->capacity = 0;
}

int ir_temp_value_map_clone(IRTempValueMap *dest,
                                   const IRTempValueMap *src) {
  if (!dest || !src) {
    return 0;
  }

  ir_temp_value_map_clear(dest);
  for (size_t i = 0; i < src->count; i++) {
    if (!ir_temp_value_map_set(dest, src->items[i].name, &src->items[i].value)) {
      return 0;
    }
  }
  return 1;
}

static int ir_temp_value_map_intersect_with(IRTempValueMap *dest,
                                            const IRTempValueMap *other,
                                            int *changed) {
  if (!dest || !other) {
    return 0;
  }

  size_t i = 0;
  while (i < dest->count) {
    IRTempValueEntry *entry = &dest->items[i];
    const IROperand *other_value =
        ir_temp_value_map_lookup(other, entry->name);
    if (!other_value || !ir_operand_equals(&entry->value, other_value)) {
      ir_temp_value_map_remove(dest, entry->name);
      if (changed) {
        *changed = 1;
      }
      continue;
    }
    i++;
  }

  return 1;
}

int ir_label_value_map_init(IRLabelValueMap *map) {
  if (!map) {
    return 0;
  }
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  return 1;
}

static int ir_label_value_map_find(const IRLabelValueMap *map,
                                   const char *label) {
  if (!map || !label) {
    return -1;
  }
  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].label && strcmp(map->items[i].label, label) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static IRLabelValueEntry *ir_label_value_map_get_or_add(IRLabelValueMap *map,
                                                        const char *label) {
  if (!map || !label) {
    return NULL;
  }

  int existing = ir_label_value_map_find(map, label);
  if (existing >= 0) {
    return &map->items[existing];
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRLabelValueEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRLabelValueEntry));
    if (!new_items) {
      return NULL;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  IRLabelValueEntry *entry = &map->items[map->count];
  memset(entry, 0, sizeof(*entry));
  entry->label = mettle_strdup(label);
  if (!entry->label || !ir_temp_value_map_init(&entry->in_map)) {
    free(entry->label);
    entry->label = NULL;
    return NULL;
  }
  entry->initialized = 0;
  map->count++;
  return entry;
}

int ir_label_value_map_merge_incoming(IRLabelValueMap *map,
                                             const char *label,
                                             const IRTempValueMap *incoming,
                                             int *changed) {
  if (!map || !label || !incoming) {
    return 0;
  }

  IRLabelValueEntry *entry = ir_label_value_map_get_or_add(map, label);
  if (!entry) {
    return 0;
  }

  if (!entry->initialized) {
    if (!ir_temp_value_map_clone(&entry->in_map, incoming)) {
      return 0;
    }
    entry->initialized = 1;
    if (changed) {
      *changed = 1;
    }
    return 1;
  }

  return ir_temp_value_map_intersect_with(&entry->in_map, incoming, changed);
}

const IRLabelValueEntry *ir_label_value_map_lookup(
    const IRLabelValueMap *map, const char *label) {
  if (!map || !label) {
    return NULL;
  }
  int index = ir_label_value_map_find(map, label);
  if (index < 0) {
    return NULL;
  }
  return &map->items[index];
}

void ir_label_value_map_destroy(IRLabelValueMap *map) {
  if (!map) {
    return;
  }
  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].label);
    ir_temp_value_map_destroy(&map->items[i].in_map);
  }
  free(map->items);
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
}

int ir_operand_equals(const IROperand *lhs, const IROperand *rhs) {
  if (!lhs || !rhs || lhs->kind != rhs->kind) {
    return 0;
  }

  switch (lhs->kind) {
  case IR_OPERAND_NONE:
    return 1;
  case IR_OPERAND_INT:
    return lhs->int_value == rhs->int_value;
  case IR_OPERAND_FLOAT:
    /* Same numeric value at different IEEE-754 widths is NOT the same operand
     * for CSE/propagation purposes: a float32 0.1 and float64 0.1 have
     * distinct bit patterns and must not be coalesced. Treat unspecified (0)
     * as the default 64 so legacy float64-only IR keeps matching. */
    return lhs->float_value == rhs->float_value &&
           ((lhs->float_bits == 32) == (rhs->float_bits == 32));
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_STRING:
  case IR_OPERAND_LABEL:
    if (!lhs->name || !rhs->name) {
      return 0;
    }
    return strcmp(lhs->name, rhs->name) == 0;
  default:
    return 0;
  }
}

static int ir_binary_op_is_commutative(const char *op_text) {
  if (!op_text) {
    return 0;
  }
  return strcmp(op_text, "+") == 0 || strcmp(op_text, "*") == 0 ||
         strcmp(op_text, "&") == 0 || strcmp(op_text, "|") == 0 ||
         strcmp(op_text, "^") == 0 || strcmp(op_text, "==") == 0 ||
         strcmp(op_text, "!=") == 0;
}

static void ir_expression_entry_destroy(IRExpressionEntry *entry) {
  if (!entry) {
    return;
  }

  free(entry->op_text);
  entry->op_text = NULL;
  ir_operand_destroy(&entry->lhs);
  ir_operand_destroy(&entry->rhs);
  ir_operand_destroy(&entry->value);
  entry->kind = IR_EXPR_BINARY;
  entry->is_float = 0;
}

static void ir_expression_map_clear(IRExpressionMap *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    ir_expression_entry_destroy(&map->items[i]);
  }
  map->count = 0;
}

static void ir_expression_map_destroy(IRExpressionMap *map) {
  if (!map) {
    return;
  }

  ir_expression_map_clear(map);
  free(map->items);
  map->items = NULL;
  map->capacity = 0;
}

static int ir_instruction_is_cse_candidate(const IRInstruction *instruction) {
  if (!instruction || !ir_instruction_writes_destination(instruction)) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_BINARY:
    return instruction->text != NULL;
  case IR_OP_UNARY:
    if (!instruction->text) {
      return 0;
    }
    return strcmp(instruction->text, "*") != 0 &&
           strcmp(instruction->text, "&") != 0;
  case IR_OP_CAST:
    return instruction->text != NULL;
  case IR_OP_ADDRESS_OF:
    return instruction->lhs.kind == IR_OPERAND_SYMBOL &&
           instruction->lhs.name != NULL;
  default:
    return 0;
  }
}

static int ir_expression_entry_matches_instruction(
    const IRExpressionEntry *entry, const IRInstruction *instruction) {
  if (!entry || !instruction) {
    return 0;
  }

  switch (entry->kind) {
  case IR_EXPR_BINARY:
    if (instruction->op != IR_OP_BINARY || !instruction->text ||
        !entry->op_text || strcmp(entry->op_text, instruction->text) != 0 ||
        entry->is_float != instruction->is_float) {
      return 0;
    }
    if (ir_operand_equals(&entry->lhs, &instruction->lhs) &&
        ir_operand_equals(&entry->rhs, &instruction->rhs)) {
      return 1;
    }
    if (ir_binary_op_is_commutative(instruction->text) &&
        ir_operand_equals(&entry->lhs, &instruction->rhs) &&
        ir_operand_equals(&entry->rhs, &instruction->lhs)) {
      return 1;
    }
    return 0;
  case IR_EXPR_UNARY:
    return instruction->op == IR_OP_UNARY && instruction->text &&
           entry->op_text && strcmp(entry->op_text, instruction->text) == 0 &&
           entry->is_float == instruction->is_float &&
           ir_operand_equals(&entry->lhs, &instruction->lhs);
  case IR_EXPR_CAST:
    return instruction->op == IR_OP_CAST && instruction->text &&
           entry->op_text && strcmp(entry->op_text, instruction->text) == 0 &&
           entry->is_float == instruction->is_float &&
           ir_operand_equals(&entry->lhs, &instruction->lhs);
  case IR_EXPR_ADDRESS_OF:
    return instruction->op == IR_OP_ADDRESS_OF &&
           ir_operand_equals(&entry->lhs, &instruction->lhs);
  default:
    return 0;
  }
}

static int ir_expression_map_find_matching_instruction(
    const IRExpressionMap *map, const IRInstruction *instruction) {
  if (!map || !instruction) {
    return -1;
  }

  for (size_t i = 0; i < map->count; i++) {
    if (ir_expression_entry_matches_instruction(&map->items[i], instruction)) {
      return (int)i;
    }
  }

  return -1;
}

static const IROperand *ir_expression_map_lookup(const IRExpressionMap *map,
                                                 const IRInstruction *instruction) {
  int index = ir_expression_map_find_matching_instruction(map, instruction);
  if (index < 0) {
    return NULL;
  }
  return &map->items[index].value;
}

static int ir_expression_map_store_value_for_instruction(
    IRExpressionMap *map, const IRInstruction *instruction) {
  if (!map || !instruction || !ir_instruction_is_cse_candidate(instruction) ||
      instruction->dest.kind == IR_OPERAND_NONE) {
    return 0;
  }

  int existing_index =
      ir_expression_map_find_matching_instruction(map, instruction);
  if (existing_index >= 0) {
    IROperand new_value = ir_operand_none();
    if (!ir_operand_clone(&instruction->dest, &new_value)) {
      return 0;
    }
    ir_operand_destroy(&map->items[existing_index].value);
    map->items[existing_index].value = new_value;
    return 1;
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRExpressionEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRExpressionEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  IRExpressionEntry *entry = &map->items[map->count];
  memset(entry, 0, sizeof(*entry));
  entry->lhs = ir_operand_none();
  entry->rhs = ir_operand_none();
  entry->value = ir_operand_none();

  switch (instruction->op) {
  case IR_OP_BINARY:
    entry->kind = IR_EXPR_BINARY;
    entry->is_float = instruction->is_float;
    entry->op_text = mettle_strdup(instruction->text);
    if (!entry->op_text) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    if (!ir_operand_clone(&instruction->lhs, &entry->lhs) ||
        !ir_operand_clone(&instruction->rhs, &entry->rhs)) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    break;
  case IR_OP_UNARY:
    entry->kind = IR_EXPR_UNARY;
    entry->is_float = instruction->is_float;
    entry->op_text = mettle_strdup(instruction->text);
    if (!entry->op_text) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    if (!ir_operand_clone(&instruction->lhs, &entry->lhs)) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    break;
  case IR_OP_CAST:
    entry->kind = IR_EXPR_CAST;
    entry->is_float = instruction->is_float;
    entry->op_text = mettle_strdup(instruction->text);
    if (!entry->op_text) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    if (!ir_operand_clone(&instruction->lhs, &entry->lhs)) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    break;
  case IR_OP_ADDRESS_OF:
    entry->kind = IR_EXPR_ADDRESS_OF;
    if (!ir_operand_clone(&instruction->lhs, &entry->lhs)) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    break;
  default:
    return 0;
  }

  if (!ir_operand_clone(&instruction->dest, &entry->value)) {
    ir_expression_entry_destroy(entry);
    return 0;
  }

  map->count++;
  return 1;
}

static int ir_operand_matches_named(const IROperand *operand,
                                    IROperandKind kind, const char *name) {
  if (!operand || !name || operand->kind != kind || !operand->name) {
    return 0;
  }
  return strcmp(operand->name, name) == 0;
}

static int ir_expression_entry_uses_named(const IRExpressionEntry *entry,
                                          IROperandKind kind,
                                          const char *name) {
  if (!entry || !name) {
    return 0;
  }

  if (ir_operand_matches_named(&entry->lhs, kind, name) ||
      ir_operand_matches_named(&entry->rhs, kind, name) ||
      ir_operand_matches_named(&entry->value, kind, name)) {
    return 1;
  }

  return 0;
}

static void ir_expression_map_invalidate_named(IRExpressionMap *map,
                                               IROperandKind kind,
                                               const char *name) {
  if (!map || !name) {
    return;
  }

  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    IRExpressionEntry *entry = &map->items[read];
    if (ir_expression_entry_uses_named(entry, kind, name)) {
      ir_expression_entry_destroy(entry);
      continue;
    }

    if (write != read) {
      map->items[write] = map->items[read];
      map->items[read].op_text = NULL;
      map->items[read].lhs = ir_operand_none();
      map->items[read].rhs = ir_operand_none();
      map->items[read].value = ir_operand_none();
    }
    write++;
  }

  map->count = write;
}

int ir_common_subexpression_elimination_pass(IRFunction *function,
                                                    int *changed) {
  if (!function) {
    return 0;
  }

  IRExpressionMap map = {0};

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_LABEL) {
      ir_expression_map_clear(&map);
      continue;
    }

    if (ir_instruction_writes_temp(instruction) && instruction->dest.name) {
      ir_expression_map_invalidate_named(&map, IR_OPERAND_TEMP,
                                         instruction->dest.name);
    }
    if (ir_instruction_writes_symbol(instruction) && instruction->dest.name) {
      ir_expression_map_invalidate_named(&map, IR_OPERAND_SYMBOL,
                                         instruction->dest.name);
    }

    if (ir_instruction_is_cse_candidate(instruction)) {
      const IROperand *existing = ir_expression_map_lookup(&map, instruction);
      if (existing) {
        if (!ir_rewrite_to_assign_operand(instruction, existing, changed)) {
          ir_expression_map_destroy(&map);
          return 0;
        }
      } else if (!ir_expression_map_store_value_for_instruction(&map,
                                                                instruction)) {
        ir_expression_map_destroy(&map);
        return 0;
      }
    }

    if (instruction->op == IR_OP_STORE || instruction->op == IR_OP_CALL ||
        instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_INLINE_ASM) {
      ir_expression_map_clear(&map);
    }

    if (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_BRANCH_ZERO ||
        instruction->op == IR_OP_BRANCH_EQ ||
        instruction->op == IR_OP_RETURN) {
      ir_expression_map_clear(&map);
    }
  }

  ir_expression_map_destroy(&map);
  return 1;
}

int ir_temp_use_map_init(IRTempUseMap *map) {
  if (!map) {
    return 0;
  }

  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  map->hash = NULL;
  map->hash_count = 0;
  return 1;
}

/* Insert items[index] into the hash table (hash table must have room). */
static void ir_temp_use_map_hash_put(IRTempUseMap *map, size_t index) {
  size_t mask = map->hash_count - 1;
  size_t h = mettle_fnv1a_hash(map->items[index].name) & mask;
  while (map->hash[h] != 0) {
    h = (h + 1) & mask;
  }
  map->hash[h] = index + 1; /* store index+1; 0 == empty */
}

/* Grow/allocate the hash table so it can hold map->count entries at <0.5 load,
 * rehashing all existing items. Returns 0 on allocation failure. */
static int ir_temp_use_map_hash_reserve(IRTempUseMap *map, size_t needed) {
  size_t target = 16;
  while (target < needed * 2) {
    target *= 2;
  }
  if (map->hash && map->hash_count >= target) {
    return 1;
  }

  size_t *new_hash = calloc(target, sizeof(size_t));
  if (!new_hash) {
    return 0;
  }
  free(map->hash);
  map->hash = new_hash;
  map->hash_count = target;
  for (size_t i = 0; i < map->count; i++) {
    ir_temp_use_map_hash_put(map, i);
  }
  return 1;
}

static int ir_temp_use_map_find(const IRTempUseMap *map, const char *name) {
  if (!map || !name || !map->hash) {
    return -1;
  }

  size_t mask = map->hash_count - 1;
  size_t h = mettle_fnv1a_hash(name) & mask;
  while (map->hash[h] != 0) {
    size_t idx = map->hash[h] - 1;
    if (map->items[idx].name && strcmp(map->items[idx].name, name) == 0) {
      return (int)idx;
    }
    h = (h + 1) & mask;
  }
  return -1;
}

static int ir_temp_use_map_add(IRTempUseMap *map, const char *name) {
  if (!map || !name) {
    return 0;
  }

  int existing = ir_temp_use_map_find(map, name);
  if (existing >= 0) {
    map->items[existing].use_count++;
    return 1;
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRTempUseEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRTempUseEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  /* Ensure the hash can hold one more entry (rehashes existing items if so). */
  if (!ir_temp_use_map_hash_reserve(map, map->count + 1)) {
    return 0;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return 0;
  }

  map->items[map->count].name = name_copy;
  map->items[map->count].use_count = 1;
  ir_temp_use_map_hash_put(map, map->count);
  map->count++;
  return 1;
}

size_t ir_temp_use_map_get(const IRTempUseMap *map, const char *name) {
  if (!map || !name) {
    return 0;
  }

  int index = ir_temp_use_map_find(map, name);
  if (index < 0) {
    return 0;
  }

  return map->items[index].use_count;
}

static void ir_temp_use_map_discard(IRTempUseMap *map, const char *name) {
  if (!map || !name) {
    return;
  }

  int index = ir_temp_use_map_find(map, name);
  if (index >= 0) {
    map->items[index].use_count = 0;
  }
}

void ir_temp_use_map_destroy(IRTempUseMap *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].name);
  }
  free(map->items);
  free(map->hash);
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  map->hash = NULL;
  map->hash_count = 0;
}

static int ir_collect_operand_temp_use(IRTempUseMap *uses,
                                       const IROperand *operand) {
  if (!uses || !operand) {
    return 0;
  }

  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 1;
  }

  return ir_temp_use_map_add(uses, operand->name);
}

int ir_collect_instruction_temp_uses(IRTempUseMap *uses,
                                            const IRInstruction *instruction) {
  if (!uses || !instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_STORE:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
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
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_OUTER_LANE_F64:
    if (!ir_collect_operand_temp_use(uses, &instruction->dest) ||
        !ir_collect_operand_temp_use(uses, &instruction->lhs) ||
        !ir_collect_operand_temp_use(uses, &instruction->rhs)) {
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      if (!ir_collect_operand_temp_use(uses, &instruction->arguments[i])) {
        return 0;
      }
    }
    break;

  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_NEW:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_RETURN:
    if (!ir_collect_operand_temp_use(uses, &instruction->lhs) ||
        !ir_collect_operand_temp_use(uses, &instruction->rhs)) {
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      if (!ir_collect_operand_temp_use(uses, &instruction->arguments[i])) {
        return 0;
      }
    }
    break;

  default:
    break;
  }

  return 1;
}

int ir_eliminate_dead_temp_writes_pass(IRFunction *function,
                                              int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap live;
  if (!ir_temp_use_map_init(&live)) {
    return 0;
  }

  int crossed_label = 0;
  for (size_t i = function->instruction_count; i > 0;) {
    i--;
    IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_LABEL) {
      crossed_label = 1;
    }

    if (ir_instruction_writes_temp(instruction) && instruction->dest.name) {
      int dest_live = ir_temp_use_map_get(&live, instruction->dest.name) != 0;
      if (!dest_live &&
          ir_instruction_is_trivially_dead_if_dest_unused(instruction)) {
        ir_instruction_make_nop(instruction);
        if (changed) {
          *changed = 1;
        }
        continue;
      }

      if (!crossed_label) {
        ir_temp_use_map_discard(&live, instruction->dest.name);
      }
    }

    if (!ir_collect_instruction_temp_uses(&live, instruction)) {
      ir_temp_use_map_destroy(&live);
      return 0;
    }
  }

  ir_temp_use_map_destroy(&live);
  return 1;
}
