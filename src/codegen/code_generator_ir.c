#include "code_generator_internal.h"
#include "../common.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STACK_FRAME_WARN_THRESHOLD (256 * 1024)

typedef struct {
  char *name;
  int offset;
} IRTempSlot;

// Open-addressing hash index over IRTempTable::items. Stores (slot_index + 1)
// so that 0 means "empty bucket". The table is append-only while building a
// function frame and read-only during emission, so we never need tombstones.
typedef struct {
  size_t *buckets;
  size_t bucket_count;
} IRNameIndex;

typedef struct {
  IRTempSlot *items;
  size_t count;
  size_t capacity;
  IRNameIndex index;
} IRTempTable;

static uint64_t ir_name_hash(const char *name) {
  // FNV-1a 64-bit. unsigned long is 32-bit on Windows, so use a fixed-width
  // type to keep the full mixing behavior.
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    hash ^= (uint64_t)*p;
    hash *= 1099511628211ULL;
  }
  return hash;
}

static int ir_name_index_rehash(IRNameIndex *index, IRTempSlot *items,
                                size_t count, size_t new_bucket_count) {
  size_t *buckets = calloc(new_bucket_count, sizeof(size_t));
  if (!buckets) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    if (!items[i].name) {
      continue;
    }
    size_t mask = new_bucket_count - 1;
    size_t pos = (size_t)(ir_name_hash(items[i].name) & (uint64_t)mask);
    while (buckets[pos] != 0) {
      pos = (pos + 1) & mask;
    }
    buckets[pos] = i + 1;
  }
  free(index->buckets);
  index->buckets = buckets;
  index->bucket_count = new_bucket_count;
  return 1;
}

// Returns the slot index for `name`, or -1 if absent. O(1) amortized.
static int ir_name_index_lookup(const IRNameIndex *index,
                                const IRTempSlot *items, const char *name) {
  if (!index->buckets || index->bucket_count == 0) {
    return -1;
  }
  size_t mask = index->bucket_count - 1;
  size_t pos = (size_t)(ir_name_hash(name) & (uint64_t)mask);
  while (index->buckets[pos] != 0) {
    size_t slot = index->buckets[pos] - 1;
    if (items[slot].name && strcmp(items[slot].name, name) == 0) {
      return (int)slot;
    }
    pos = (pos + 1) & mask;
  }
  return -1;
}

// Records that items[slot_index] now exists. Grows/rehashes when the index
// would exceed a 0.7 load factor.
static int ir_name_index_insert(IRNameIndex *index, IRTempSlot *items,
                                size_t count, size_t slot_index) {
  if (index->bucket_count == 0 ||
      (count * 10) >= (index->bucket_count * 7)) {
    size_t next = index->bucket_count == 0 ? 64 : index->bucket_count * 2;
    if (!ir_name_index_rehash(index, items, count, next)) {
      return 0;
    }
  }
  size_t mask = index->bucket_count - 1;
  size_t pos =
      (size_t)(ir_name_hash(items[slot_index].name) & (uint64_t)mask);
  while (index->buckets[pos] != 0) {
    pos = (pos + 1) & mask;
  }
  index->buckets[pos] = slot_index + 1;
  return 1;
}

static void ir_name_index_destroy(IRNameIndex *index) {
  free(index->buckets);
  index->buckets = NULL;
  index->bucket_count = 0;
}

typedef struct {
  char *name;
  const char *type_name;
  SourceLocation location;
  int size;
  int alignment;
} IRLocalSlot;

typedef struct {
  IRLocalSlot *items;
  size_t count;
  size_t capacity;
} IRLocalTable;

typedef struct {
  char *name;
  size_t use_count;
  int address_taken;
} IRSymbolStatsEntry;

typedef struct {
  IRSymbolStatsEntry *items;
  size_t count;
  size_t capacity;
} IRSymbolStatsMap;

typedef struct {
  char *name;
  size_t use_count;
} IRTempUseEntry;

typedef struct {
  IRTempUseEntry *items;
  size_t count;
  size_t capacity;
} IRTempUseMap;

typedef struct {
  const char *name;
  x86Register reg;
  int save_offset;
} IRPromotedSymbol;

static const x86Register IR_PROMOTION_REGISTERS[] = {
    REG_R12, REG_R13, REG_R14, REG_R15, REG_RBX, REG_RSI, REG_RDI};

static int ir_binary_operator_is_comparison(const char *op);
static int ir_binary_operator_is_commutative(const char *op);

/* Per-instruction loop nesting depth, derived from backward branches.
 *
 * A label that is targeted by a later JUMP/BRANCH forms a loop whose body is
 * [label_index, branch_index]. Counting how many such bodies enclose each
 * instruction yields its nesting depth. Used to weight register-promotion so
 * variables live inside hot loops outrank one-shot setup code. Returns a
 * malloc'd array of length instruction_count (caller frees), or NULL on OOM
 * or when there are no instructions. */
static unsigned char *ir_compute_loop_depths(const IRFunction *function) {
  if (!function || function->instruction_count == 0) {
    return NULL;
  }
  size_t n = function->instruction_count;

  /* delta[i] = (loops opening at i) - (loops closing just after i). A prefix
   * sum then gives the depth at each instruction. int is sufficient: depth is
   * bounded by the number of branches, far below INT_MAX in practice. */
  int *delta = calloc(n + 1, sizeof(int));
  unsigned char *depth = calloc(n, sizeof(unsigned char));
  if (!delta || !depth) {
    free(delta);
    free(depth);
    return NULL;
  }

  for (size_t i = 0; i < n; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op != IR_OP_JUMP && insn->op != IR_OP_BRANCH_ZERO &&
        insn->op != IR_OP_BRANCH_EQ) {
      continue;
    }
    const char *target = insn->text;
    if (!target) {
      continue;
    }
    /* Find the label this branch targets; only backward edges form loops. */
    for (size_t j = 0; j <= i; j++) {
      const IRInstruction *cand = &function->instructions[j];
      if (cand->op == IR_OP_LABEL && cand->text &&
          strcmp(cand->text, target) == 0) {
        delta[j] += 1;
        delta[i + 1] -= 1;
        break;
      }
    }
  }

  int level = 0;
  for (size_t i = 0; i < n; i++) {
    level += delta[i];
    if (level < 0) {
      level = 0; /* defensive: overlapping/irreducible edges */
    }
    depth[i] = (level > 255) ? 255 : (unsigned char)level;
  }

  free(delta);
  return depth;
}

/* Register-promotion weight for an instruction at the given loop depth. Each
 * nesting level multiplies pressure by 16, capped so the saturating add in
 * ir_symbol_stats_map_add_use_weighted stays well-behaved. Depth 0 -> 1,
 * 1 -> 16, 2 -> 256, deep loops clamp at 4096. */
static size_t ir_loop_depth_weight(unsigned char depth) {
  if (depth == 0) {
    return 1;
  }
  if (depth >= 3) {
    return 4096;
  }
  return (size_t)1 << (4 * depth);
}

static int ir_symbol_stats_map_find(const IRSymbolStatsMap *map,
                                    const char *name) {
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

static IRSymbolStatsEntry *ir_symbol_stats_map_get_or_add(IRSymbolStatsMap *map,
                                                          const char *name) {
  if (!map || !name) {
    return NULL;
  }

  int existing = ir_symbol_stats_map_find(map, name);
  if (existing >= 0) {
    return &map->items[existing];
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = (map->capacity == 0) ? 16 : map->capacity * 2;
    IRSymbolStatsEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRSymbolStatsEntry));
    if (!new_items) {
      return NULL;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return NULL;
  }

  IRSymbolStatsEntry *entry = &map->items[map->count++];
  entry->name = name_copy;
  entry->use_count = 0;
  entry->address_taken = 0;
  return entry;
}

static int ir_symbol_stats_map_add_use_weighted(IRSymbolStatsMap *map,
                                                const char *name,
                                                size_t weight) {
  IRSymbolStatsEntry *entry = ir_symbol_stats_map_get_or_add(map, name);
  if (!entry) {
    return 0;
  }
  /* Saturating add: a runaway weight must never wrap use_count back to a
   * small value and demote a genuinely hot symbol. */
  if (entry->use_count > (size_t)-1 - weight) {
    entry->use_count = (size_t)-1;
  } else {
    entry->use_count += weight;
  }
  return 1;
}

static int ir_symbol_stats_map_add_use(IRSymbolStatsMap *map,
                                       const char *name) {
  return ir_symbol_stats_map_add_use_weighted(map, name, 1);
}

static int ir_symbol_stats_map_mark_address_taken(IRSymbolStatsMap *map,
                                                  const char *name) {
  IRSymbolStatsEntry *entry = ir_symbol_stats_map_get_or_add(map, name);
  if (!entry) {
    return 0;
  }
  entry->address_taken = 1;
  return 1;
}

static size_t ir_symbol_stats_map_get_use_count(const IRSymbolStatsMap *map,
                                                const char *name) {
  int index = ir_symbol_stats_map_find(map, name);
  if (index < 0) {
    return 0;
  }
  return map->items[index].use_count;
}

static int ir_symbol_stats_map_is_address_taken(const IRSymbolStatsMap *map,
                                                const char *name) {
  int index = ir_symbol_stats_map_find(map, name);
  if (index < 0) {
    return 0;
  }
  return map->items[index].address_taken;
}

static void ir_symbol_stats_map_destroy(IRSymbolStatsMap *map) {
  if (!map) {
    return;
  }
  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].name);
  }
  free(map->items);
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
}

static int ir_symbol_stats_map_record_operand(IRSymbolStatsMap *map,
                                              const IROperand *operand,
                                              size_t weight) {
  if (!map || !operand || operand->kind != IR_OPERAND_SYMBOL ||
      !operand->name) {
    return 1;
  }
  return ir_symbol_stats_map_add_use_weighted(map, operand->name, weight);
}

static int ir_symbol_stats_map_record_instruction(IRSymbolStatsMap *map,
                                                  const IRInstruction *instr,
                                                  size_t weight) {
  if (!map || !instr) {
    return 0;
  }

  if (!ir_symbol_stats_map_record_operand(map, &instr->dest, weight) ||
      !ir_symbol_stats_map_record_operand(map, &instr->lhs, weight) ||
      !ir_symbol_stats_map_record_operand(map, &instr->rhs, weight)) {
    return 0;
  }

  for (size_t i = 0; i < instr->argument_count; i++) {
    if (!ir_symbol_stats_map_record_operand(map, &instr->arguments[i],
                                            weight)) {
      return 0;
    }
  }

  if (instr->op == IR_OP_ADDRESS_OF && instr->lhs.kind == IR_OPERAND_SYMBOL &&
      instr->lhs.name) {
    if (!ir_symbol_stats_map_mark_address_taken(map, instr->lhs.name)) {
      return 0;
    }
  }

  return 1;
}

static int ir_temp_use_map_find(const IRTempUseMap *map, const char *name) {
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
    size_t new_capacity = (map->capacity == 0) ? 16 : map->capacity * 2;
    IRTempUseEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRTempUseEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return 0;
  }

  map->items[map->count].name = name_copy;
  map->items[map->count].use_count = 1;
  map->count++;
  return 1;
}

static size_t ir_temp_use_map_get(const IRTempUseMap *map, const char *name) {
  int index = ir_temp_use_map_find(map, name);
  if (index < 0) {
    return 0;
  }
  return map->items[index].use_count;
}

static void ir_temp_use_map_destroy(IRTempUseMap *map) {
  if (!map) {
    return;
  }
  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].name);
  }
  free(map->items);
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
}

static int ir_temp_use_map_record_operand(IRTempUseMap *map,
                                          const IROperand *operand) {
  if (!map || !operand || operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 1;
  }
  return ir_temp_use_map_add(map, operand->name);
}

static int ir_temp_use_map_record_instruction(IRTempUseMap *map,
                                              const IRInstruction *instr) {
  if (!map || !instr) {
    return 0;
  }

  if (instr->op == IR_OP_STORE) {
    if (!ir_temp_use_map_record_operand(map, &instr->dest) ||
        !ir_temp_use_map_record_operand(map, &instr->lhs) ||
        !ir_temp_use_map_record_operand(map, &instr->rhs)) {
      return 0;
    }
  } else {
    if (!ir_temp_use_map_record_operand(map, &instr->lhs) ||
        !ir_temp_use_map_record_operand(map, &instr->rhs)) {
      return 0;
    }
  }

  for (size_t i = 0; i < instr->argument_count; i++) {
    if (!ir_temp_use_map_record_operand(map, &instr->arguments[i])) {
      return 0;
    }
  }

  return 1;
}

static int ir_is_integer_or_pointer_promotable(Type *type) {
  if (!type || type->size == 0 || type->size > 8) {
    return 0;
  }

  switch (type->kind) {
  case TYPE_BOOL:
  case TYPE_INT8:
  case TYPE_INT16:
  case TYPE_INT32:
  case TYPE_INT64:
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_UINT64:
  case TYPE_POINTER:
  case TYPE_FUNCTION_POINTER:
  case TYPE_ENUM:
    return 1;
  default:
    return 0;
  }
}

static int ir_promoted_symbol_find(IRPromotedSymbol *symbols, size_t count,
                                   const char *name) {
  if (!symbols || !name) {
    return -1;
  }
  for (size_t i = 0; i < count; i++) {
    if (symbols[i].name && strcmp(symbols[i].name, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static int ir_immediate_fits_signed_32(long long value) {
  return value >= (long long)INT_MIN && value <= (long long)INT_MAX;
}

static Symbol *code_generator_ir_lookup_register_symbol(CodeGenerator *generator,
                                                        const IROperand *operand) {
  if (!generator || !operand || operand->kind != IR_OPERAND_SYMBOL ||
      !operand->name) {
    return NULL;
  }

  Symbol *symbol = symbol_table_lookup(generator->symbol_table, operand->name);
  if (!symbol ||
      (symbol->kind != SYMBOL_VARIABLE && symbol->kind != SYMBOL_PARAMETER) ||
      !symbol->data.variable.is_in_register) {
    return NULL;
  }

  return symbol;
}

static int code_generator_ir_symbol_width_bits(const Symbol *symbol) {
  int size = 8;
  if (symbol && symbol->type && symbol->type->size > 0 && symbol->type->size <= 8) {
    size = (int)symbol->type->size;
  }

  if (size <= 1) {
    return 8;
  }
  if (size == 2) {
    return 16;
  }
  if (size == 4) {
    return 32;
  }
  return 64;
}

static const char *code_generator_ir_symbol_register_name(const Symbol *symbol,
                                                          int width_bits) {
  if (!symbol) {
    return NULL;
  }

  x86Register reg = (x86Register)symbol->data.variable.register_id;
  if (width_bits == 64) {
    return code_generator_get_register_name(reg);
  }
  return code_generator_get_subregister_name(reg, width_bits);
}

static const char *code_generator_ir_binary_register_instruction(const char *op) {
  if (!op) {
    return NULL;
  }
  if (strcmp(op, "+") == 0) {
    return "add";
  }
  if (strcmp(op, "-") == 0) {
    return "sub";
  }
  if (strcmp(op, "&") == 0) {
    return "and";
  }
  if (strcmp(op, "|") == 0) {
    return "or";
  }
  if (strcmp(op, "^") == 0) {
    return "xor";
  }
  return NULL;
}

static int code_generator_try_emit_ir_assign_symbol_to_symbol(
    CodeGenerator *generator, const IRInstruction *instruction, int *emitted) {
  if (emitted) {
    *emitted = 0;
  }
  if (!generator || !instruction) {
    return 0;
  }

  Symbol *dest_symbol =
      code_generator_ir_lookup_register_symbol(generator, &instruction->dest);
  Symbol *lhs_symbol =
      code_generator_ir_lookup_register_symbol(generator, &instruction->lhs);
  if (!dest_symbol || !lhs_symbol) {
    return 1;
  }

  int dest_bits = code_generator_ir_symbol_width_bits(dest_symbol);
  int lhs_bits = code_generator_ir_symbol_width_bits(lhs_symbol);
  if (dest_bits != lhs_bits) {
    return 1;
  }

  const char *dest_reg =
      code_generator_ir_symbol_register_name(dest_symbol, dest_bits);
  const char *lhs_reg =
      code_generator_ir_symbol_register_name(lhs_symbol, lhs_bits);
  if (!dest_reg || !lhs_reg) {
    code_generator_set_error(generator,
                             "Invalid register in IR symbol assign fast path");
    return 0;
  }

  if (strcmp(dest_reg, lhs_reg) != 0) {
    code_generator_emit(generator, "    mov %s, %s\n", dest_reg, lhs_reg);
  }

  if (emitted) {
    *emitted = 1;
  }
  return 1;
}

static int code_generator_try_emit_ir_binary_register_fastpath(
    CodeGenerator *generator, const IRInstruction *instruction, int *emitted) {
  if (emitted) {
    *emitted = 0;
  }
  if (!generator || !instruction || !instruction->text) {
    return 0;
  }
  if (instruction->is_float) {
    return 1;
  }

  const char *arith_instruction =
      code_generator_ir_binary_register_instruction(instruction->text);
  if (!arith_instruction) {
    return 1;
  }

  Symbol *dest_symbol =
      code_generator_ir_lookup_register_symbol(generator, &instruction->dest);
  Symbol *lhs_symbol =
      code_generator_ir_lookup_register_symbol(generator, &instruction->lhs);
  if (!dest_symbol || !lhs_symbol) {
    return 1;
  }

  int bits = code_generator_ir_symbol_width_bits(dest_symbol);
  if (bits != code_generator_ir_symbol_width_bits(lhs_symbol)) {
    return 1;
  }

  const char *dest_reg = code_generator_ir_symbol_register_name(dest_symbol, bits);
  const char *lhs_reg = code_generator_ir_symbol_register_name(lhs_symbol, bits);
  if (!dest_reg || !lhs_reg) {
    code_generator_set_error(generator,
                             "Invalid register in IR binary fast path");
    return 0;
  }

  if (instruction->rhs.kind == IR_OPERAND_INT &&
      ir_immediate_fits_signed_32(instruction->rhs.int_value) &&
      instruction->lhs.kind == IR_OPERAND_SYMBOL &&
      instruction->lhs.name && instruction->dest.kind == IR_OPERAND_SYMBOL &&
      instruction->dest.name &&
      strcmp(instruction->lhs.name, instruction->dest.name) == 0) {
    code_generator_emit(generator, "    %s %s, %lld\n", arith_instruction,
                        dest_reg, instruction->rhs.int_value);
    if (emitted) {
      *emitted = 1;
    }
    return 1;
  }

  Symbol *rhs_symbol =
      code_generator_ir_lookup_register_symbol(generator, &instruction->rhs);
  if (!rhs_symbol || bits != code_generator_ir_symbol_width_bits(rhs_symbol)) {
    return 1;
  }

  const char *rhs_reg = code_generator_ir_symbol_register_name(rhs_symbol, bits);
  if (!rhs_reg) {
    code_generator_set_error(generator,
                             "Invalid RHS register in IR binary fast path");
    return 0;
  }

  if (instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
      instruction->rhs.kind != IR_OPERAND_SYMBOL || !instruction->rhs.name) {
    return 1;
  }

  int dest_is_lhs = strcmp(instruction->dest.name, instruction->lhs.name) == 0;
  int dest_is_rhs = strcmp(instruction->dest.name, instruction->rhs.name) == 0;

  if (!dest_is_lhs) {
    if (dest_is_rhs) {
      if (strcmp(instruction->text, "-") == 0) {
        return 1;
      }
      const char *tmp_reg = lhs_reg;
      lhs_reg = rhs_reg;
      rhs_reg = tmp_reg;
    } else {
      code_generator_emit(generator, "    mov %s, %s\n", dest_reg, lhs_reg);
    }
  }
  code_generator_emit(generator, "    %s %s, %s\n", arith_instruction, dest_reg,
                      rhs_reg);

  if (emitted) {
    *emitted = 1;
  }
  return 1;
}

static int ir_binary_operator_is_comparison(const char *op) {
  if (!op) {
    return 0;
  }
  return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
         strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
         strcmp(op, ">") == 0 || strcmp(op, ">=") == 0;
}

static int ir_binary_operator_is_commutative(const char *op) {
  if (!op) {
    return 0;
  }
  return strcmp(op, "+") == 0 || strcmp(op, "*") == 0 ||
         strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
         strcmp(op, "^") == 0 || strcmp(op, "==") == 0 ||
         strcmp(op, "!=") == 0;
}

static const char *ir_false_jump_for_comparison(const char *op) {
  if (!op) {
    return NULL;
  }
  if (strcmp(op, "==") == 0) {
    return "jne";
  }
  if (strcmp(op, "!=") == 0) {
    return "je";
  }
  if (strcmp(op, "<") == 0) {
    return "jge";
  }
  if (strcmp(op, "<=") == 0) {
    return "jg";
  }
  if (strcmp(op, ">") == 0) {
    return "jle";
  }
  if (strcmp(op, ">=") == 0) {
    return "jl";
  }
  return NULL;
}

static const char *ir_invert_conditional_jump(const char *jump) {
  if (!jump) {
    return NULL;
  }
  if (strcmp(jump, "je") == 0) {
    return "jne";
  }
  if (strcmp(jump, "jne") == 0) {
    return "je";
  }
  if (strcmp(jump, "jl") == 0) {
    return "jge";
  }
  if (strcmp(jump, "jge") == 0) {
    return "jl";
  }
  if (strcmp(jump, "jle") == 0) {
    return "jg";
  }
  if (strcmp(jump, "jg") == 0) {
    return "jle";
  }
  return NULL;
}

static int code_generator_try_emit_ir_compare_branch_register_fastpath(
    CodeGenerator *generator, const IRInstruction *compare_instruction,
    const char *false_jump, const char *target_label, int *emitted) {
  if (emitted) {
    *emitted = 0;
  }
  if (!generator || !compare_instruction || !false_jump || !target_label) {
    return 0;
  }
  if (compare_instruction->is_float) {
    return 1;
  }

  Symbol *lhs_symbol = code_generator_ir_lookup_register_symbol(
      generator, &compare_instruction->lhs);
  if (!lhs_symbol) {
    return 1;
  }

  int lhs_bits = code_generator_ir_symbol_width_bits(lhs_symbol);
  const char *lhs_reg =
      code_generator_ir_symbol_register_name(lhs_symbol, lhs_bits);
  if (!lhs_reg) {
    code_generator_set_error(
        generator, "Invalid lhs register in IR compare fast path");
    return 0;
  }

  if (compare_instruction->rhs.kind == IR_OPERAND_INT &&
      ir_immediate_fits_signed_32(compare_instruction->rhs.int_value)) {
    code_generator_emit(generator, "    cmp %s, %lld\n", lhs_reg,
                        compare_instruction->rhs.int_value);
    code_generator_emit(generator, "    %s %s\n", false_jump, target_label);
    if (emitted) {
      *emitted = 1;
    }
    return 1;
  }

  Symbol *rhs_symbol = code_generator_ir_lookup_register_symbol(
      generator, &compare_instruction->rhs);
  if (!rhs_symbol ||
      code_generator_ir_symbol_width_bits(rhs_symbol) != lhs_bits) {
    return 1;
  }

  const char *rhs_reg =
      code_generator_ir_symbol_register_name(rhs_symbol, lhs_bits);
  if (!rhs_reg) {
    code_generator_set_error(
        generator, "Invalid rhs register in IR compare fast path");
    return 0;
  }

  code_generator_emit(generator, "    cmp %s, %s\n", lhs_reg, rhs_reg);
  code_generator_emit(generator, "    %s %s\n", false_jump, target_label);
  if (emitted) {
    *emitted = 1;
  }
  return 1;
}

static int ir_call_ignores_register_promotion_barrier(const char *callee_name) {
  if (!callee_name) {
    return 0;
  }
  /* Pure native helpers that do not clobber promoted arithmetic state. */
  if (strcmp(callee_name, "mettle_crash_trap") == 0 ||
      strcmp(callee_name, "mettle_crash_trap_ex") == 0 ||
      strcmp(callee_name, "GetTickCount64") == 0 ||
      strcmp(callee_name, "QueryPerformanceCounter") == 0 ||
      strcmp(callee_name, "QueryPerformanceFrequency") == 0) {
    return 1;
  }
  return 0;
}

static int ir_function_blocks_register_promotion(const IRFunction *function) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction) {
      continue;
    }

    if (instruction->op == IR_OP_CALL) {
      if (instruction->text &&
          ir_call_ignores_register_promotion_barrier(instruction->text)) {
        continue;
      }
      return 1;
    }

    if (instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_NEW ||
        instruction->op == IR_OP_INLINE_ASM) {
      return 1;
    }
  }

  return 0;
}

static int ir_local_table_add(IRLocalTable *table, const char *name,
                              const char *type_name, SourceLocation location,
                              int size, int alignment) {
  if (!table || !name || size <= 0) {
    return 0;
  }

  for (size_t i = 0; i < table->count; i++) {
    if (table->items[i].name && strcmp(table->items[i].name, name) == 0) {
      return 1;
    }
  }

  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity == 0 ? 16 : table->capacity * 2;
    IRLocalSlot *new_items =
        realloc(table->items, new_capacity * sizeof(IRLocalSlot));
    if (!new_items) {
      return 0;
    }
    table->items = new_items;
    table->capacity = new_capacity;
  }

  size_t name_len = strlen(name) + 1;
  char *name_copy = malloc(name_len);
  if (!name_copy) {
    return 0;
  }
  memcpy(name_copy, name, name_len);

  IRLocalSlot *slot = &table->items[table->count++];
  slot->name = name_copy;
  slot->type_name = type_name;
  slot->location = location;
  slot->size = size;
  slot->alignment = alignment;
  return 1;
}

static void ir_local_table_destroy(IRLocalTable *table) {
  if (!table) {
    return;
  }
  for (size_t i = 0; i < table->count; i++) {
    free(table->items[i].name);
  }
  free(table->items);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
}

static int ir_temp_table_get_offset(IRTempTable *table, const char *name) {
  if (!table || !name) {
    return -1;
  }
  int slot = ir_name_index_lookup(&table->index, table->items, name);
  if (slot < 0) {
    return -1;
  }
  return table->items[slot].offset;
}

static int ir_temp_table_add(IRTempTable *table, const char *name) {
  if (!table || !name) {
    return 0;
  }

  if (ir_name_index_lookup(&table->index, table->items, name) >= 0) {
    return 1;
  }

  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity == 0 ? 32 : table->capacity * 2;
    IRTempSlot *new_items =
        realloc(table->items, new_capacity * sizeof(IRTempSlot));
    if (!new_items) {
      return 0;
    }
    table->items = new_items;
    table->capacity = new_capacity;
  }

  size_t name_len = strlen(name) + 1;
  char *name_copy = malloc(name_len);
  if (!name_copy) {
    return 0;
  }
  memcpy(name_copy, name, name_len);

  table->items[table->count].name = name_copy;
  table->items[table->count].offset = -1;
  if (!ir_name_index_insert(&table->index, table->items, table->count,
                            table->count)) {
    free(name_copy);
    table->items[table->count].name = NULL;
    return 0;
  }
  table->count++;
  return 1;
}

static void ir_temp_table_destroy(IRTempTable *table) {
  if (!table) {
    return;
  }
  ir_name_index_destroy(&table->index);
  for (size_t i = 0; i < table->count; i++) {
    free(table->items[i].name);
  }
  free(table->items);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
}

static int code_generator_load_ir_operand(CodeGenerator *generator,
                                          const IROperand *operand,
                                          IRTempTable *temp_table) {
  if (!generator || !operand) {
    return 0;
  }

  switch (operand->kind) {
  case IR_OPERAND_NONE:
    code_generator_emit(generator, "    mov rax, 0\n");
    return 1;

  case IR_OPERAND_INT:
    code_generator_emit(generator, "    mov rax, %lld\n", operand->int_value);
    return 1;

  case IR_OPERAND_FLOAT: {
    if (operand->float_bits == 32) {
      /* float32 literal: emit the true 32-bit IEEE-754 single pattern
       * (zero-extended). Using the float64 bit pattern here and then storing
       * only its low 32 bits would write 0 for almost every value. */
      union {
        float value;
        unsigned int bits;
      } converter;
      converter.value = (float)operand->float_value;
      code_generator_emit(generator, "    mov rax, 0x%08x\n", converter.bits);
      return 1;
    }
    union {
      double value;
      unsigned long long bits;
    } converter;
    converter.value = operand->float_value;
    code_generator_emit(generator, "    mov rax, 0x%016llx\n", converter.bits);
    return 1;
  }

  case IR_OPERAND_STRING:
    if (!operand->name) {
      code_generator_set_error(generator, "Malformed IR string operand");
      return 0;
    }
    code_generator_load_string_literal(generator, operand->name);
    return 1;

  case IR_OPERAND_SYMBOL:
    if (!operand->name) {
      code_generator_set_error(generator, "Malformed IR symbol operand");
      return 0;
    }
    code_generator_load_variable(generator, operand->name);
    return !generator->has_error;

  case IR_OPERAND_TEMP: {
    if (!operand->name) {
      code_generator_set_error(generator, "Malformed IR temp operand");
      return 0;
    }
    int offset = ir_temp_table_get_offset(temp_table, operand->name);
    if (offset <= 0) {
      code_generator_set_error(generator, "Unknown IR temp '%s'",
                               operand->name);
      return 0;
    }
    /* Deferred-spill peephole. If this temp's spill is still pending (nothing
     * emitted since it was produced), rax still holds the value. */
    if (offset == generator->pending_spill_offset &&
        generator->emit_seq == generator->rax_cached_emit_seq) {
      if (generator->pending_spill_single_use) {
        /* This load is the temp's only use: the spill is dead. Drop it. */
        generator->pending_spill_offset = 0;
        generator->pending_spill_single_use = 0;
      } else {
        /* More uses follow: the slot must hold the value. Emit the store now
         * (flush), but the value is still in rax afterwards so no reload. */
        code_generator_flush_pending_spill(generator);
      }
      return 1; /* value already in rax */
    }
    /* Reload-elision: value spilled to this slot and nothing emitted since. */
    if (offset == generator->rax_cached_temp_offset &&
        generator->emit_seq == generator->rax_cached_emit_seq) {
      return 1;
    }
    code_generator_emit(generator, "    mov rax, [rbp - %d]\n", offset);
    return 1;
  }

  case IR_OPERAND_LABEL:
    if (!operand->name) {
      code_generator_set_error(generator, "Malformed IR label operand");
      return 0;
    }
    code_generator_emit(generator, "    lea rax, [rel %s]\n", operand->name);
    return 1;

  default:
    code_generator_set_error(generator, "Unhandled IR operand kind: %d",
                             operand->kind);
    return 0;
  }
}

static int code_generator_load_ir_operand_into_register(
    CodeGenerator *generator, const IROperand *operand,
    IRTempTable *temp_table, x86Register target_register) {
  if (!generator) {
    return 0;
  }

  if (!code_generator_load_ir_operand(generator, operand, temp_table)) {
    return 0;
  }

  if (target_register != REG_RAX) {
    const char *target_name = code_generator_get_register_name(target_register);
    if (!target_name) {
      code_generator_set_error(generator, "Invalid target register in IR load");
      return 0;
    }
    code_generator_emit(generator, "    mov %s, rax\n", target_name);
  }

  return 1;
}

static int code_generator_store_ir_destination(CodeGenerator *generator,
                                               const IROperand *destination,
                                               IRTempTable *temp_table) {
  if (!generator || !destination) {
    return 0;
  }

  switch (destination->kind) {
  case IR_OPERAND_NONE:
    return 1;

  case IR_OPERAND_SYMBOL:
    if (!destination->name) {
      code_generator_set_error(generator, "Malformed IR destination symbol");
      return 0;
    }
    code_generator_store_variable(generator, destination->name, "rax");
    return !generator->has_error;

  case IR_OPERAND_TEMP: {
    if (!destination->name) {
      code_generator_set_error(generator, "Malformed IR destination temp");
      return 0;
    }
    int offset = ir_temp_table_get_offset(temp_table, destination->name);
    if (offset <= 0) {
      code_generator_set_error(generator, "Unknown IR temp '%s'",
                               destination->name);
      return 0;
    }
    /* Defer the spill. rax already holds the value; emit nothing yet. If the
     * next IR operand load wants this same temp we may be able to drop the
     * store entirely (dead, when single-use) or at least skip the reload. Any
     * other emission flushes this first (code_generator_emit), so the slot is
     * always written before any reader or rax clobber.
     *
     * A still-pending earlier spill (different temp) must be flushed now: we
     * are about to overwrite rax, so its value can no longer be recovered. */
    if (generator->pending_spill_offset != 0 &&
        generator->pending_spill_offset != offset) {
      code_generator_flush_pending_spill(generator);
    }
    {
      const IRTempUseMap *use_map =
          (const IRTempUseMap *)generator->current_temp_use_map;
      generator->pending_spill_single_use =
          (use_map &&
           ir_temp_use_map_get(use_map, destination->name) == 1)
              ? 1
              : 0;
    }
    generator->pending_spill_offset = offset;
    /* rax still holds the value. Mark the reload cache too, so even the
     * multi-use path (store flushed, then read) skips the reload. */
    generator->rax_cached_temp_offset = offset;
    generator->rax_cached_emit_seq = generator->emit_seq;
    return 1;
  }

  default:
    code_generator_set_error(generator, "Invalid IR destination kind: %d",
                             destination->kind);
    return 0;
  }
}

static int code_generator_get_ir_access_size(CodeGenerator *generator,
                                             const IROperand *size_operand) {
  if (!generator || !size_operand || size_operand->kind != IR_OPERAND_INT) {
    code_generator_set_error(generator,
                             "IR memory access width must be integer");
    return 0;
  }

  long long size = size_operand->int_value;
  if (size > 0) {
    return (int)size;
  }

  code_generator_set_error(generator, "Invalid IR memory access width: %lld",
                           size);
  return 0;
}

static void code_generator_emit_ir_load_from_address(CodeGenerator *generator,
                                                     int size) {
  if (!generator) {
    return;
  }

  switch (size) {
  case 1:
    code_generator_emit(generator, "    movzx rax, byte [rax]\n");
    break;
  case 2:
    code_generator_emit(generator, "    movzx rax, word [rax]\n");
    break;
  case 4:
    code_generator_emit(generator, "    mov eax, dword [rax]\n");
    break;
  default:
    code_generator_emit(generator, "    mov rax, qword [rax]\n");
    break;
  }
}

static void code_generator_emit_ir_store_to_address(CodeGenerator *generator,
                                                    int size) {
  if (!generator) {
    return;
  }

  if (size > 8) {
    code_generator_emit(generator, "    push rsi\n");
    code_generator_emit(generator, "    push rdi\n");
    code_generator_emit(generator, "    mov rsi, rcx\n");
    code_generator_emit(generator, "    mov rdi, rax\n");
    code_generator_emit(generator, "    mov rcx, %d\n", size);
    code_generator_emit(generator, "    rep movsb\n");
    code_generator_emit(generator, "    pop rdi\n");
    code_generator_emit(generator, "    pop rsi\n");
    return;
  }

  switch (size) {
  case 1:
    code_generator_emit(generator, "    mov byte [rax], cl\n");
    break;
  case 2:
    code_generator_emit(generator, "    mov word [rax], cx\n");
    break;
  case 4:
    code_generator_emit(generator, "    mov dword [rax], ecx\n");
    break;
  default:
    code_generator_emit(generator, "    mov qword [rax], rcx\n");
    break;
  }
}

static int code_generator_emit_ir_address_of(CodeGenerator *generator,
                                             const IRInstruction *instruction,
                                             IRTempTable *temp_table) {
  if (!generator || !instruction ||
      instruction->lhs.kind != IR_OPERAND_SYMBOL || !instruction->lhs.name) {
    code_generator_set_error(generator, "IR addr_of requires symbol operand");
    return 0;
  }

  Symbol *symbol =
      symbol_table_lookup(generator->symbol_table, instruction->lhs.name);
  if (!symbol) {
    code_generator_set_error(generator, "Unknown addr_of symbol '%s'",
                             instruction->lhs.name);
    return 0;
  }

  if (symbol->kind == SYMBOL_FUNCTION) {
    const char *func_name =
        code_generator_get_link_symbol_name(generator, instruction->lhs.name);
    if (!func_name) {
      code_generator_set_error(generator,
                               "Invalid function symbol in IR addr_of");
      return 0;
    }
    if (symbol->is_extern &&
        !code_generator_emit_extern_symbol(generator, func_name)) {
      return 0;
    }
    code_generator_emit(generator, "    lea rax, [rel %s]\n", func_name);
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  }

  if (symbol->kind != SYMBOL_VARIABLE && symbol->kind != SYMBOL_PARAMETER) {
    code_generator_set_error(generator, "Unknown addr_of symbol '%s'",
                             instruction->lhs.name);
    return 0;
  }

  if (symbol->data.variable.is_in_register) {
    code_generator_set_error(
        generator, "IR addr_of cannot target register-allocated symbol '%s'",
        instruction->lhs.name);
    return 0;
  }

  if (symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
    const char *symbol_name =
        code_generator_get_link_symbol_name(generator, instruction->lhs.name);
    if (!symbol_name) {
      code_generator_set_error(generator,
                               "Invalid global symbol in IR addr_of");
      return 0;
    }
    if (symbol->is_extern &&
        !code_generator_emit_extern_symbol(generator, symbol_name)) {
      return 0;
    }
    code_generator_emit(generator, "    lea rax, [rel %s]\n", symbol_name);
  } else if (symbol->kind == SYMBOL_PARAMETER &&
             symbol->data.variable.is_indirect_param) {
    /* The struct lives in the caller's frame; the home slot holds the
     * pointer. Loading the pointer IS the address. Note: mutations through
     * this pointer escape back to the caller — by-value semantics for
     * indirect params is preserved by the caller-side temp copy, not here. */
    code_generator_emit(
        generator,
        "    mov rax, qword [rbp - %d]  ; addr_of indirect param\n",
        symbol->data.variable.memory_offset);
  } else {
    code_generator_emit(generator, "    lea rax, [rbp - %d]\n",
                        symbol->data.variable.memory_offset);
  }

  return code_generator_store_ir_destination(generator, &instruction->dest,
                                             temp_table);
}

/* SSE2 lowering of IR_OP_COUNT_WORD_STARTS for the text-assembly backend.
 *
 * Counts maximal non-whitespace runs in buf[0..len-1] (whitespace =
 * space/tab/LF/CR), 16 bytes per iteration, plus a scalar tail. The result is
 * ADDED to the prior value of the count symbol (the recognizer only fires when
 * the source initialized count to 0, so this matches the scalar semantics).
 *
 * Register use: rcx=cursor, rdx=bytes remaining, rax=running count,
 * r8d=carry (1 iff the byte immediately before the cursor was non-whitespace;
 * 0 at the start, which is correct since position -1 counts as whitespace).
 * Per chunk: ws-mask via 4x pcmpeqb+por, pmovmskb to a GPR, nw=~ws, a word
 * start is a non-ws byte whose predecessor was ws, i.e. popcount(nw & ~prev)
 * where prev = (nw<<1)|carry; carry for the next chunk = bit 15 of nw. */
static int code_generator_emit_ir_count_word_starts(
    CodeGenerator *generator, const IRInstruction *instruction) {
  if (!generator || !instruction ||
      instruction->dest.kind != IR_OPERAND_SYMBOL ||
      instruction->lhs.kind != IR_OPERAND_SYMBOL ||
      instruction->rhs.kind != IR_OPERAND_SYMBOL) {
    code_generator_set_error(generator, "Malformed count_word_starts");
    return 0;
  }

  char *l_vec = code_generator_generate_label(generator, "_wcs_vec");
  char *l_tail = code_generator_generate_label(generator, "_wcs_tail");
  char *l_tailloop = code_generator_generate_label(generator, "_wcs_tl");
  char *l_done = code_generator_generate_label(generator, "_wcs_done");
  char *l_ws = code_generator_generate_label(generator, "_wcs_ws");
  if (!l_vec || !l_tail || !l_tailloop || !l_done || !l_ws) {
    free(l_vec); free(l_tail); free(l_tailloop); free(l_done); free(l_ws);
    code_generator_set_error(generator, "OOM in count_word_starts");
    return 0;
  }

  /* rax <- current count value; rcx <- buf; rdx <- len. */
  code_generator_load_variable(generator, instruction->lhs.name);
  code_generator_emit(generator, "    mov rcx, rax\n");
  code_generator_load_variable(generator, instruction->rhs.name);
  code_generator_emit(generator, "    mov rdx, rax\n");
  code_generator_load_variable(generator, instruction->dest.name);
  code_generator_emit(generator, "    xor r8d, r8d\n");

  /* Broadcast the four whitespace bytes into xmm1..xmm4. */
  code_generator_emit(generator, "    mov r9d, 0x20202020\n");
  code_generator_emit(generator, "    movd xmm1, r9d\n");
  code_generator_emit(generator, "    pshufd xmm1, xmm1, 0\n");
  code_generator_emit(generator, "    mov r9d, 0x09090909\n");
  code_generator_emit(generator, "    movd xmm2, r9d\n");
  code_generator_emit(generator, "    pshufd xmm2, xmm2, 0\n");
  code_generator_emit(generator, "    mov r9d, 0x0A0A0A0A\n");
  code_generator_emit(generator, "    movd xmm3, r9d\n");
  code_generator_emit(generator, "    pshufd xmm3, xmm3, 0\n");
  code_generator_emit(generator, "    mov r9d, 0x0D0D0D0D\n");
  code_generator_emit(generator, "    movd xmm4, r9d\n");
  code_generator_emit(generator, "    pshufd xmm4, xmm4, 0\n");

  /* ---- vector loop: while (rdx >= 16) ---- */
  code_generator_emit(generator, "%s:\n", l_vec);
  code_generator_emit(generator, "    cmp rdx, 16\n");
  code_generator_emit(generator, "    jb %s\n", l_tail);

  code_generator_emit(generator, "    movdqu xmm0, [rcx]\n");
  code_generator_emit(generator, "    movdqa xmm5, xmm0\n");
  code_generator_emit(generator, "    pcmpeqb xmm0, xmm1\n");
  code_generator_emit(generator, "    movdqa xmm6, xmm5\n");
  code_generator_emit(generator, "    pcmpeqb xmm6, xmm2\n");
  code_generator_emit(generator, "    por xmm0, xmm6\n");
  code_generator_emit(generator, "    movdqa xmm6, xmm5\n");
  code_generator_emit(generator, "    pcmpeqb xmm6, xmm3\n");
  code_generator_emit(generator, "    por xmm0, xmm6\n");
  code_generator_emit(generator, "    movdqa xmm6, xmm5\n");
  code_generator_emit(generator, "    pcmpeqb xmm6, xmm4\n");
  code_generator_emit(generator, "    por xmm0, xmm6\n");
  /* r9d = ws bitmask (bit k set iff byte k is whitespace). */
  code_generator_emit(generator, "    pmovmskb r9d, xmm0\n");
  /* r10d = nw = ~ws restricted to 16 bits. */
  code_generator_emit(generator, "    mov r10d, r9d\n");
  code_generator_emit(generator, "    not r10d\n");
  code_generator_emit(generator, "    and r10d, 0xFFFF\n");
  /* r11d = prev = (nw<<1)|carry  (bit k = "byte k-1 was non-ws"). */
  code_generator_emit(generator, "    mov r11d, r10d\n");
  code_generator_emit(generator, "    shl r11d, 1\n");
  code_generator_emit(generator, "    or r11d, r8d\n");
  /* new carry = nw bit 15. */
  code_generator_emit(generator, "    mov r8d, r10d\n");
  code_generator_emit(generator, "    shr r8d, 15\n");
  code_generator_emit(generator, "    and r8d, 1\n");
  /* starts = nw & ~prev ; count += popcount(starts). */
  code_generator_emit(generator, "    not r11d\n");
  code_generator_emit(generator, "    and r11d, r10d\n");
  code_generator_emit(generator, "    and r11d, 0xFFFF\n");
  code_generator_emit(generator, "    popcnt r11d, r11d\n");
  code_generator_emit(generator, "    add rax, r11\n");

  code_generator_emit(generator, "    add rcx, 16\n");
  code_generator_emit(generator, "    sub rdx, 16\n");
  code_generator_emit(generator, "    jmp %s\n", l_vec);

  /* ---- scalar tail: process remaining rdx (<16) bytes ---- */
  code_generator_emit(generator, "%s:\n", l_tail);
  code_generator_emit(generator, "    test rdx, rdx\n");
  code_generator_emit(generator, "    jz %s\n", l_done);
  code_generator_emit(generator, "%s:\n", l_tailloop);
  /* r9d = byte; treat space/tab/LF/CR as whitespace. */
  code_generator_emit(generator, "    movzx r9d, byte [rcx]\n");
  code_generator_emit(generator, "    cmp r9d, 32\n");
  code_generator_emit(generator, "    je %s\n", l_ws);
  code_generator_emit(generator, "    cmp r9d, 9\n");
  code_generator_emit(generator, "    je %s\n", l_ws);
  code_generator_emit(generator, "    cmp r9d, 10\n");
  code_generator_emit(generator, "    je %s\n", l_ws);
  code_generator_emit(generator, "    cmp r9d, 13\n");
  code_generator_emit(generator, "    je %s\n", l_ws);
  /* non-whitespace: a word starts here iff carry==0 (prev was ws). */
  code_generator_emit(generator, "    test r8d, r8d\n");
  code_generator_emit(generator, "    jnz %s_nw\n", l_tailloop);
  code_generator_emit(generator, "    add rax, 1\n");
  code_generator_emit(generator, "%s_nw:\n", l_tailloop);
  code_generator_emit(generator, "    mov r8d, 1\n");
  code_generator_emit(generator, "    jmp %s_next\n", l_tailloop);
  code_generator_emit(generator, "%s:\n", l_ws);
  code_generator_emit(generator, "    xor r8d, r8d\n");
  code_generator_emit(generator, "%s_next:\n", l_tailloop);
  code_generator_emit(generator, "    add rcx, 1\n");
  code_generator_emit(generator, "    sub rdx, 1\n");
  code_generator_emit(generator, "    jnz %s\n", l_tailloop);

  code_generator_emit(generator, "%s:\n", l_done);
  code_generator_store_variable(generator, instruction->dest.name, "rax");

  free(l_vec); free(l_tail); free(l_tailloop); free(l_done); free(l_ws);
  return !generator->has_error;
}

static int code_generator_emit_ir_load(CodeGenerator *generator,
                                       const IRInstruction *instruction,
                                       IRTempTable *temp_table) {
  if (!generator || !instruction) {
    return 0;
  }

  int size = code_generator_get_ir_access_size(generator, &instruction->rhs);
  if (size <= 0) {
    return 0;
  }

  if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                      temp_table)) {
    return 0;
  }

  code_generator_emit_ir_load_from_address(generator, size);
  return code_generator_store_ir_destination(generator, &instruction->dest,
                                             temp_table);
}

static int code_generator_emit_ir_store(CodeGenerator *generator,
                                        const IRInstruction *instruction,
                                        IRTempTable *temp_table) {
  if (!generator || !instruction) {
    return 0;
  }

  int size = code_generator_get_ir_access_size(generator, &instruction->rhs);
  if (size <= 0) {
    return 0;
  }

  if (!code_generator_load_ir_operand_into_register(
          generator, &instruction->lhs, temp_table, REG_RCX)) {
    return 0;
  }
  if (!code_generator_load_ir_operand(generator, &instruction->dest,
                                      temp_table)) {
    return 0;
  }
  code_generator_emit_ir_store_to_address(generator, size);
  return 1;
}

static int code_generator_emit_ir_new(CodeGenerator *generator,
                                      const IRInstruction *instruction,
                                      IRTempTable *temp_table) {
  if (!generator || !instruction) {
    return 0;
  }

  int allocation_size = 8;
  if (instruction->rhs.kind == IR_OPERAND_INT &&
      instruction->rhs.int_value > 0) {
    allocation_size = (int)instruction->rhs.int_value;
  }

  const char *size_register = "rdi";
  CallingConventionSpec *conv_spec =
      generator->register_allocator
          ? generator->register_allocator->calling_convention
          : NULL;
  if (conv_spec && conv_spec->int_param_count > 0) {
    const char *candidate =
        code_generator_get_register_name(conv_spec->int_param_registers[0]);
    if (candidate) {
      size_register = candidate;
    }
  }

  code_generator_emit(generator, "    ; IR new: %s (%d bytes)\n",
                      instruction->text ? instruction->text : "type",
                      allocation_size);

  if (instruction->rhs.kind == IR_OPERAND_TEMP) {
    int offset = ir_temp_table_get_offset(temp_table, instruction->rhs.name);
    code_generator_emit(generator, "    mov %s, [rbp - %d]\n", size_register,
                        offset);
  } else {
    code_generator_emit(generator, "    mov %s, %d\n", size_register,
                        allocation_size);
  }
  code_generator_emit_calloc_call(generator, size_register);
  return code_generator_store_ir_destination(generator, &instruction->dest,
                                             temp_table);
}

static int code_generator_extract_positive_power_of_two(long long value,
                                                        unsigned int *shift_out,
                                                        unsigned long long *mask_out) {
  if (!shift_out || !mask_out || value <= 0) {
    return 0;
  }

  unsigned long long uvalue = (unsigned long long)value;
  if ((uvalue & (uvalue - 1ULL)) != 0ULL) {
    return 0;
  }

  unsigned int shift = 0;
  unsigned long long cursor = uvalue;
  while (cursor > 1ULL) {
    cursor >>= 1ULL;
    shift++;
  }

  *shift_out = shift;
  *mask_out = uvalue - 1ULL;
  return 1;
}

static int code_generator_ir_operand_float_bits(CodeGenerator *generator,
                                                const IROperand *operand);

static void code_generator_emit_and_mask(CodeGenerator *generator,
                                         const char *target_register,
                                         unsigned long long mask) {
  if (!generator || !target_register) {
    return;
  }

  if (mask <= 0x7fffffffULL) {
    code_generator_emit(generator, "    and %s, %llu\n", target_register, mask);
  } else {
    code_generator_emit(generator, "    mov r10, 0x%016llx\n", mask);
    code_generator_emit(generator, "    and %s, r10\n", target_register);
  }
}

/* Materialize raw float bits in RAX or R10 into an XMM register at want_bits
 * (32 or 64), using movd/movq plus cvtss2sd/cvtsd2ss when the storage width
 * differs. Matches the object backend so float32 * float64 promotions do not
 * reinterpret single-precision bits as double. */
static int code_generator_ir_emit_float_gp_into_xmm(CodeGenerator *generator,
                                                    const char *xmm_reg,
                                                    x86Register gp_reg,
                                                    int value_bits,
                                                    int want_bits) {
  const char *gp64 = (gp_reg == REG_RAX) ? "rax" : "r10";
  const char *gp32 = (gp_reg == REG_RAX) ? "eax" : "r10d";

  if (!generator || !xmm_reg) {
    return 0;
  }
  if (want_bits != 32 && want_bits != 64) {
    want_bits = 64;
  }
  if (value_bits != 32 && value_bits != 64) {
    value_bits = 64;
  }

  if (value_bits == 32) {
    code_generator_emit(generator, "    movd %s, %s\n", xmm_reg, gp32);
    if (want_bits == 64) {
      code_generator_emit(generator, "    cvtss2sd %s, %s\n", xmm_reg, xmm_reg);
    }
  } else {
    code_generator_emit(generator, "    movq %s, %s\n", xmm_reg, gp64);
    if (want_bits == 32) {
      code_generator_emit(generator, "    cvtsd2ss %s, %s\n", xmm_reg, xmm_reg);
    }
  }
  return 1;
}

static int
code_generator_emit_ir_binary_fallback(CodeGenerator *generator,
                                       const IRInstruction *instruction,
                                       IRTempTable *temp_table) {
  if (!generator || !instruction || !instruction->text) {
    return 0;
  }

  const char *op = instruction->text;

  {
    int emitted_fastpath = 0;
    if (!code_generator_try_emit_ir_binary_register_fastpath(
            generator, instruction, &emitted_fastpath)) {
      return 0;
    }
    if (emitted_fastpath) {
      return 1;
    }
  }

  if (!instruction->is_float &&
      (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) &&
      instruction->rhs.kind == IR_OPERAND_INT) {
    unsigned int shift = 0;
    unsigned long long mask = 0;
    if (code_generator_extract_positive_power_of_two(
            instruction->rhs.int_value, &shift, &mask)) {
      if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                          temp_table)) {
        return 0;
      }

      if (strcmp(op, "/") == 0) {
        // Signed division by 2^k with truncation toward zero.
        if (shift != 0) {
          code_generator_emit(generator, "    mov rcx, rax\n");
          code_generator_emit(generator, "    sar rcx, 63\n");
          code_generator_emit_and_mask(generator, "rcx", mask);
          code_generator_emit(generator, "    add rax, rcx\n");
          code_generator_emit(generator, "    sar rax, %u\n", shift);
        }
      } else {
        // Signed remainder by 2^k: r = x - (q << k), q = trunc(x / 2^k).
        if (shift == 0) {
          code_generator_emit(generator, "    mov rax, 0\n");
        } else {
          code_generator_emit(generator, "    mov r11, rax\n");
          code_generator_emit(generator, "    mov rcx, rax\n");
          code_generator_emit(generator, "    sar rcx, 63\n");
          code_generator_emit_and_mask(generator, "rcx", mask);
          code_generator_emit(generator, "    add rax, rcx\n");
          code_generator_emit(generator, "    sar rax, %u\n", shift);
          code_generator_emit(generator, "    shl rax, %u\n", shift);
          code_generator_emit(generator, "    sub r11, rax\n");
          code_generator_emit(generator, "    mov rax, r11\n");
        }
      }

      return code_generator_store_ir_destination(generator, &instruction->dest,
                                                 temp_table);
    }
  }

  if (!instruction->is_float && instruction->rhs.kind == IR_OPERAND_INT &&
      ir_immediate_fits_signed_32(instruction->rhs.int_value)) {
    const char *arith = code_generator_get_arithmetic_instruction(op, 0);
    long long immediate = instruction->rhs.int_value;

    if (arith && strcmp(op, "/") != 0 && strcmp(op, "%") != 0 &&
        ((strcmp(op, "<<") != 0 && strcmp(op, ">>") != 0) ||
         (immediate >= 0 && immediate < 64))) {
      if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                          temp_table)) {
        return 0;
      }

      if (strcmp(op, "*") == 0) {
        code_generator_emit(generator, "    imul rax, rax, %lld\n",
                            immediate);
      } else if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
        code_generator_emit(generator, "    %s rax, %lld\n", arith,
                            immediate);
      } else {
        code_generator_emit(generator, "    %s rax, %lld\n", arith,
                            immediate);
      }

      return code_generator_store_ir_destination(generator, &instruction->dest,
                                                 temp_table);
    }

    if (ir_binary_operator_is_comparison(op)) {
      if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                          temp_table)) {
        return 0;
      }

      code_generator_emit(generator, "    cmp rax, %lld\n", immediate);
      if (strcmp(op, "==") == 0) {
        code_generator_emit(generator, "    sete al\n");
      } else if (strcmp(op, "!=") == 0) {
        code_generator_emit(generator, "    setne al\n");
      } else if (strcmp(op, "<") == 0) {
        code_generator_emit(generator, "    setl al\n");
      } else if (strcmp(op, "<=") == 0) {
        code_generator_emit(generator, "    setle al\n");
      } else if (strcmp(op, ">") == 0) {
        code_generator_emit(generator, "    setg al\n");
      } else if (strcmp(op, ">=") == 0) {
        code_generator_emit(generator, "    setge al\n");
      }
      code_generator_emit(generator, "    movzx rax, al\n");
      return code_generator_store_ir_destination(generator, &instruction->dest,
                                                 temp_table);
    }
  }

  if (!code_generator_load_ir_operand_into_register(
          generator, &instruction->rhs, temp_table, REG_R10)) {
    return 0;
  }
  if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                      temp_table)) {
    return 0;
  }

  if (instruction->is_float) {
    /* Operation precision comes from the inferred expression type; each operand
     * may be float32 or float64 (or a mixed promotion). Widen/narrow into XMM
     * before addss/addsd, etc. */
    int want_bits = (instruction->float_bits == 32) ? 32 : 64;
    int lhs_bits =
        code_generator_ir_operand_float_bits(generator, &instruction->lhs);
    int rhs_bits =
        code_generator_ir_operand_float_bits(generator, &instruction->rhs);
    if (lhs_bits != 32 && lhs_bits != 64) {
      lhs_bits = 64;
    }
    if (rhs_bits != 32 && rhs_bits != 64) {
      rhs_bits = 64;
    }
    if (!code_generator_ir_emit_float_gp_into_xmm(generator, "xmm0", REG_RAX,
                                                  lhs_bits, want_bits) ||
        !code_generator_ir_emit_float_gp_into_xmm(generator, "xmm1", REG_R10,
                                                  rhs_bits, want_bits)) {
      return 0;
    }

    const char *mov_x = (want_bits == 32) ? "movd" : "movq";
    const char *suf = (want_bits == 32) ? "ss" : "sd";
    const char *ga = (want_bits == 32) ? "eax" : "rax";

    if (strcmp(op, "+") == 0) {
      code_generator_emit(generator, "    add%s xmm0, xmm1\n", suf);
      code_generator_emit(generator, "    %s %s, xmm0\n", mov_x, ga);
    } else if (strcmp(op, "-") == 0) {
      code_generator_emit(generator, "    sub%s xmm0, xmm1\n", suf);
      code_generator_emit(generator, "    %s %s, xmm0\n", mov_x, ga);
    } else if (strcmp(op, "*") == 0) {
      code_generator_emit(generator, "    mul%s xmm0, xmm1\n", suf);
      code_generator_emit(generator, "    %s %s, xmm0\n", mov_x, ga);
    } else if (strcmp(op, "/") == 0) {
      code_generator_emit(generator, "    div%s xmm0, xmm1\n", suf);
      code_generator_emit(generator, "    %s %s, xmm0\n", mov_x, ga);
    } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
               strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
               strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
      code_generator_emit(generator, "    ucomi%s xmm0, xmm1\n", suf);
      if (strcmp(op, "==") == 0) {
        code_generator_emit(generator, "    sete al\n");
        code_generator_emit(generator, "    setnp cl\n");
        code_generator_emit(generator, "    and al, cl\n");
      } else if (strcmp(op, "!=") == 0) {
        code_generator_emit(generator, "    setne al\n");
        code_generator_emit(generator, "    setp cl\n");
        code_generator_emit(generator, "    or al, cl\n");
      } else if (strcmp(op, "<") == 0) {
        code_generator_emit(generator, "    setb al\n");
      } else if (strcmp(op, "<=") == 0) {
        code_generator_emit(generator, "    setbe al\n");
      } else if (strcmp(op, ">") == 0) {
        code_generator_emit(generator, "    seta al\n");
      } else if (strcmp(op, ">=") == 0) {
        code_generator_emit(generator, "    setae al\n");
      }
      code_generator_emit(generator, "    movzx rax, al\n");
    } else {
      code_generator_set_error(generator,
                               "Unsupported float binary operator '%s'", op);
      return 0;
    }
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  }

  const char *arith = code_generator_get_arithmetic_instruction(op, 0);
  if (arith) {
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
      code_generator_emit(generator, "    cqo\n");
      code_generator_emit(generator, "    idiv r10\n");
      if (strcmp(op, "%") == 0) {
        code_generator_emit(generator, "    mov rax, rdx\n");
      }
    } else if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
      code_generator_emit(generator, "    mov rcx, r10\n");
      code_generator_emit(generator, "    %s rax, cl\n", arith);
    } else {
      code_generator_emit(generator, "    %s rax, r10\n", arith);
    }
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  }

  code_generator_emit(generator, "    cmp rax, r10\n");
  if (strcmp(op, "==") == 0) {
    code_generator_emit(generator, "    sete al\n");
  } else if (strcmp(op, "!=") == 0) {
    code_generator_emit(generator, "    setne al\n");
  } else if (strcmp(op, "<") == 0) {
    code_generator_emit(generator, "    setl al\n");
  } else if (strcmp(op, "<=") == 0) {
    code_generator_emit(generator, "    setle al\n");
  } else if (strcmp(op, ">") == 0) {
    code_generator_emit(generator, "    setg al\n");
  } else if (strcmp(op, ">=") == 0) {
    code_generator_emit(generator, "    setge al\n");
  } else if (strcmp(op, "&&") == 0) {
    code_generator_emit(generator, "    and rax, r10\n");
    code_generator_emit(generator, "    setne al\n");
  } else if (strcmp(op, "||") == 0) {
    code_generator_emit(generator, "    or rax, r10\n");
    code_generator_emit(generator, "    setne al\n");
  } else if (strcmp(op, "<<") == 0) {
    code_generator_emit(generator, "    mov rcx, r10\n");
    code_generator_emit(generator, "    shl rax, cl\n");
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  } else if (strcmp(op, ">>") == 0) {
    code_generator_emit(generator, "    mov rcx, r10\n");
    code_generator_emit(generator, "    sar rax, cl\n");
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  } else {
    code_generator_set_error(generator, "Unsupported IR binary operator '%s'",
                             op);
    return 0;
  }
  code_generator_emit(generator, "    movzx rax, al\n");
  return code_generator_store_ir_destination(generator, &instruction->dest,
                                             temp_table);
}

static int code_generator_emit_ir_integer_binary_with_rhs_register(
    CodeGenerator *generator, const char *op, const char *rhs_register) {
  if (!generator || !op || !rhs_register) {
    return 0;
  }

  const char *arith = code_generator_get_arithmetic_instruction(op, 0);
  if (arith) {
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
      if (strcmp(rhs_register, "r10") != 0) {
        code_generator_emit(generator, "    mov r10, %s\n", rhs_register);
      }
      code_generator_emit(generator, "    cqo\n");
      code_generator_emit(generator, "    idiv r10\n");
      if (strcmp(op, "%") == 0) {
        code_generator_emit(generator, "    mov rax, rdx\n");
      }
    } else if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
      if (strcmp(rhs_register, "rcx") != 0) {
        code_generator_emit(generator, "    mov rcx, %s\n", rhs_register);
      }
      code_generator_emit(generator, "    %s rax, cl\n", arith);
    } else {
      code_generator_emit(generator, "    %s rax, %s\n", arith, rhs_register);
    }
    return 1;
  }

  code_generator_emit(generator, "    cmp rax, %s\n", rhs_register);
  if (strcmp(op, "==") == 0) {
    code_generator_emit(generator, "    sete al\n");
  } else if (strcmp(op, "!=") == 0) {
    code_generator_emit(generator, "    setne al\n");
  } else if (strcmp(op, "<") == 0) {
    code_generator_emit(generator, "    setl al\n");
  } else if (strcmp(op, "<=") == 0) {
    code_generator_emit(generator, "    setle al\n");
  } else if (strcmp(op, ">") == 0) {
    code_generator_emit(generator, "    setg al\n");
  } else if (strcmp(op, ">=") == 0) {
    code_generator_emit(generator, "    setge al\n");
  } else if (strcmp(op, "&&") == 0) {
    code_generator_emit(generator, "    and rax, %s\n", rhs_register);
    code_generator_emit(generator, "    setne al\n");
  } else if (strcmp(op, "||") == 0) {
    code_generator_emit(generator, "    or rax, %s\n", rhs_register);
    code_generator_emit(generator, "    setne al\n");
  } else {
    code_generator_set_error(generator,
                             "Unsupported integer binary operator '%s'", op);
    return 0;
  }
  code_generator_emit(generator, "    movzx rax, al\n");
  return 1;
}

static int code_generator_emit_ir_integer_binary_with_rhs_immediate(
    CodeGenerator *generator, const char *op, long long immediate) {
  if (!generator || !op || !ir_immediate_fits_signed_32(immediate)) {
    return 0;
  }

  const char *arith = code_generator_get_arithmetic_instruction(op, 0);
  if (arith && strcmp(op, "/") != 0 && strcmp(op, "%") != 0 &&
      ((strcmp(op, "<<") != 0 && strcmp(op, ">>") != 0) ||
       (immediate >= 0 && immediate < 64))) {
    if (strcmp(op, "*") == 0) {
      code_generator_emit(generator, "    imul rax, rax, %lld\n", immediate);
    } else {
      code_generator_emit(generator, "    %s rax, %lld\n", arith, immediate);
    }
    return 1;
  }

  if (!ir_binary_operator_is_comparison(op)) {
    return 0;
  }

  code_generator_emit(generator, "    cmp rax, %lld\n", immediate);
  if (strcmp(op, "==") == 0) {
    code_generator_emit(generator, "    sete al\n");
  } else if (strcmp(op, "!=") == 0) {
    code_generator_emit(generator, "    setne al\n");
  } else if (strcmp(op, "<") == 0) {
    code_generator_emit(generator, "    setl al\n");
  } else if (strcmp(op, "<=") == 0) {
    code_generator_emit(generator, "    setle al\n");
  } else if (strcmp(op, ">") == 0) {
    code_generator_emit(generator, "    setg al\n");
  } else if (strcmp(op, ">=") == 0) {
    code_generator_emit(generator, "    setge al\n");
  }
  code_generator_emit(generator, "    movzx rax, al\n");
  return 1;
}

static int code_generator_compute_ir_binary_to_r11(
    CodeGenerator *generator, const IRInstruction *instruction,
    IRTempTable *temp_table) {
  if (!generator || !instruction || instruction->op != IR_OP_BINARY ||
      instruction->is_float || !instruction->text) {
    return 0;
  }

  if (instruction->rhs.kind == IR_OPERAND_INT &&
      ir_immediate_fits_signed_32(instruction->rhs.int_value)) {
    if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                        temp_table)) {
      return 0;
    }
    if (code_generator_emit_ir_integer_binary_with_rhs_immediate(
            generator, instruction->text, instruction->rhs.int_value)) {
      code_generator_emit(generator, "    mov r11, rax\n");
      return 1;
    }
  }

  if (instruction->lhs.kind == IR_OPERAND_INT &&
      ir_immediate_fits_signed_32(instruction->lhs.int_value) &&
      ir_binary_operator_is_commutative(instruction->text)) {
    if (!code_generator_load_ir_operand(generator, &instruction->rhs,
                                        temp_table)) {
      return 0;
    }
    if (code_generator_emit_ir_integer_binary_with_rhs_immediate(
            generator, instruction->text, instruction->lhs.int_value)) {
      code_generator_emit(generator, "    mov r11, rax\n");
      return 1;
    }
  }

  if (!code_generator_load_ir_operand_into_register(
          generator, &instruction->rhs, temp_table, REG_R10)) {
    return 0;
  }
  if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                      temp_table)) {
    return 0;
  }
  if (!code_generator_emit_ir_integer_binary_with_rhs_register(
          generator, instruction->text, "r10")) {
    return 0;
  }
  code_generator_emit(generator, "    mov r11, rax\n");
  return 1;
}

static int code_generator_try_emit_ir_binary_temp_pair(
    CodeGenerator *generator, const IRInstruction *producer,
    const IRInstruction *consumer, const IRTempUseMap *temp_use_map,
    IRTempTable *temp_table, int *emitted) {
  if (emitted) {
    *emitted = 0;
  }
  if (!generator || !producer || !consumer || !temp_use_map || !temp_table) {
    return 0;
  }

  if (producer->op != IR_OP_BINARY || consumer->op != IR_OP_BINARY ||
      producer->is_float || consumer->is_float || producer->ast_ref ||
      consumer->ast_ref || producer->dest.kind != IR_OPERAND_TEMP ||
      !producer->dest.name || !producer->text || !consumer->text ||
      ir_temp_use_map_get(temp_use_map, producer->dest.name) != 1) {
    return 1;
  }

  int temp_is_lhs =
      consumer->lhs.kind == IR_OPERAND_TEMP && consumer->lhs.name &&
      strcmp(consumer->lhs.name, producer->dest.name) == 0;
  int temp_is_rhs =
      consumer->rhs.kind == IR_OPERAND_TEMP && consumer->rhs.name &&
      strcmp(consumer->rhs.name, producer->dest.name) == 0;
  if (!temp_is_lhs && !temp_is_rhs) {
    return 1;
  }

  if (!code_generator_compute_ir_binary_to_r11(generator, producer,
                                               temp_table)) {
    return 0;
  }

  if (temp_is_rhs) {
    if (consumer->lhs.kind == IR_OPERAND_INT &&
        ir_immediate_fits_signed_32(consumer->lhs.int_value) &&
        ir_binary_operator_is_commutative(consumer->text)) {
      code_generator_emit(generator, "    mov rax, r11\n");
      if (code_generator_emit_ir_integer_binary_with_rhs_immediate(
              generator, consumer->text, consumer->lhs.int_value)) {
        if (!code_generator_store_ir_destination(generator, &consumer->dest,
                                                 temp_table)) {
          return 0;
        }
        if (emitted) {
          *emitted = 1;
        }
        return 1;
      }
    }

    if (!code_generator_load_ir_operand(generator, &consumer->lhs,
                                        temp_table)) {
      return 0;
    }
    if (!code_generator_emit_ir_integer_binary_with_rhs_register(
            generator, consumer->text, "r11")) {
      return 0;
    }
  } else {
    if (consumer->rhs.kind == IR_OPERAND_INT &&
        ir_immediate_fits_signed_32(consumer->rhs.int_value)) {
      code_generator_emit(generator, "    mov rax, r11\n");
      if (code_generator_emit_ir_integer_binary_with_rhs_immediate(
              generator, consumer->text, consumer->rhs.int_value)) {
        if (!code_generator_store_ir_destination(generator, &consumer->dest,
                                                 temp_table)) {
          return 0;
        }
        if (emitted) {
          *emitted = 1;
        }
        return 1;
      }
    }

    if (!code_generator_load_ir_operand_into_register(
            generator, &consumer->rhs, temp_table, REG_R10)) {
      return 0;
    }
    code_generator_emit(generator, "    mov rax, r11\n");
    if (!code_generator_emit_ir_integer_binary_with_rhs_register(
            generator, consumer->text, "r10")) {
      return 0;
    }
  }

  if (!code_generator_store_ir_destination(generator, &consumer->dest,
                                           temp_table)) {
    return 0;
  }

  if (emitted) {
    *emitted = 1;
  }
  return 1;
}

static int code_generator_emit_ir_compare_r11_branch(
    CodeGenerator *generator, const IRInstruction *compare_instruction,
    int temp_is_lhs, const char *jump, const char *target_label,
    IRTempTable *temp_table) {
  if (!generator || !compare_instruction || !jump || !target_label ||
      !temp_table) {
    return 0;
  }

  if (temp_is_lhs) {
    if (compare_instruction->rhs.kind == IR_OPERAND_INT &&
        ir_immediate_fits_signed_32(compare_instruction->rhs.int_value)) {
      code_generator_emit(generator, "    cmp r11, %lld\n",
                          compare_instruction->rhs.int_value);
    } else {
      if (!code_generator_load_ir_operand_into_register(
              generator, &compare_instruction->rhs, temp_table, REG_R10)) {
        return 0;
      }
      code_generator_emit(generator, "    cmp r11, r10\n");
    }
  } else {
    if (!code_generator_load_ir_operand(generator, &compare_instruction->lhs,
                                        temp_table)) {
      return 0;
    }
    code_generator_emit(generator, "    cmp rax, r11\n");
  }

  code_generator_emit(generator, "    %s %s\n", jump, target_label);
  return 1;
}

static int code_generator_try_emit_ir_binary_compare_branch_chain(
    CodeGenerator *generator, const IRInstruction *producer,
    const IRInstruction *compare_instruction,
    const IRInstruction *branch_instruction, const IRTempUseMap *temp_use_map,
    IRTempTable *temp_table, int *emitted) {
  if (emitted) {
    *emitted = 0;
  }
  if (!generator || !producer || !compare_instruction || !branch_instruction ||
      !temp_use_map || !temp_table) {
    return 0;
  }

  if (producer->op != IR_OP_BINARY || producer->is_float ||
      producer->ast_ref || producer->dest.kind != IR_OPERAND_TEMP ||
      !producer->dest.name ||
      ir_temp_use_map_get(temp_use_map, producer->dest.name) != 1 ||
      compare_instruction->op != IR_OP_BINARY ||
      compare_instruction->is_float || compare_instruction->ast_ref ||
      !ir_binary_operator_is_comparison(compare_instruction->text) ||
      compare_instruction->dest.kind != IR_OPERAND_TEMP ||
      !compare_instruction->dest.name ||
      ir_temp_use_map_get(temp_use_map, compare_instruction->dest.name) != 1 ||
      branch_instruction->op != IR_OP_BRANCH_ZERO ||
      branch_instruction->lhs.kind != IR_OPERAND_TEMP ||
      !branch_instruction->lhs.name || !branch_instruction->text ||
      strcmp(branch_instruction->lhs.name, compare_instruction->dest.name) !=
          0) {
    return 1;
  }

  int temp_is_lhs = compare_instruction->lhs.kind == IR_OPERAND_TEMP &&
                    compare_instruction->lhs.name &&
                    strcmp(compare_instruction->lhs.name,
                           producer->dest.name) == 0;
  int temp_is_rhs = compare_instruction->rhs.kind == IR_OPERAND_TEMP &&
                    compare_instruction->rhs.name &&
                    strcmp(compare_instruction->rhs.name,
                           producer->dest.name) == 0;
  if (!temp_is_lhs && !temp_is_rhs) {
    return 1;
  }

  const char *false_jump =
      ir_false_jump_for_comparison(compare_instruction->text);
  if (!false_jump) {
    return 1;
  }

  if (!code_generator_compute_ir_binary_to_r11(generator, producer,
                                               temp_table)) {
    return 0;
  }
  if (!code_generator_emit_ir_compare_r11_branch(
          generator, compare_instruction, temp_is_lhs, false_jump,
          branch_instruction->text, temp_table)) {
    return 0;
  }

  if (emitted) {
    *emitted = 1;
  }
  return 1;
}

static int code_generator_ir_binary_temp_use_side(
    const IRInstruction *instruction, const char *temp_name, int *is_lhs_out,
    int *is_rhs_out) {
  if (!instruction || !temp_name || !is_lhs_out || !is_rhs_out ||
      instruction->op != IR_OP_BINARY || instruction->is_float ||
      instruction->ast_ref || !instruction->text) {
    return 0;
  }

  int is_lhs = instruction->lhs.kind == IR_OPERAND_TEMP &&
               instruction->lhs.name &&
               strcmp(instruction->lhs.name, temp_name) == 0;
  int is_rhs = instruction->rhs.kind == IR_OPERAND_TEMP &&
               instruction->rhs.name &&
               strcmp(instruction->rhs.name, temp_name) == 0;
  *is_lhs_out = is_lhs;
  *is_rhs_out = is_rhs;
  return is_lhs || is_rhs;
}

static int code_generator_ir_binary_chain_candidate(
    const IRInstruction *instruction) {
  return instruction && instruction->op == IR_OP_BINARY &&
         !instruction->is_float && !instruction->ast_ref && instruction->text;
}

static int code_generator_emit_ir_binary_extend_r11(
    CodeGenerator *generator, const IRInstruction *instruction,
    const char *previous_temp, IRTempTable *temp_table) {
  if (!generator || !instruction || !previous_temp || !temp_table ||
      !code_generator_ir_binary_chain_candidate(instruction)) {
    return 0;
  }

  int temp_is_lhs = 0;
  int temp_is_rhs = 0;
  if (!code_generator_ir_binary_temp_use_side(instruction, previous_temp,
                                              &temp_is_lhs, &temp_is_rhs)) {
    return 0;
  }

  if (temp_is_lhs) {
    if (instruction->rhs.kind == IR_OPERAND_INT &&
        ir_immediate_fits_signed_32(instruction->rhs.int_value)) {
      code_generator_emit(generator, "    mov rax, r11\n");
      if (code_generator_emit_ir_integer_binary_with_rhs_immediate(
              generator, instruction->text, instruction->rhs.int_value)) {
        code_generator_emit(generator, "    mov r11, rax\n");
        return 1;
      }
    }

    if (!code_generator_load_ir_operand_into_register(
            generator, &instruction->rhs, temp_table, REG_R10)) {
      return 0;
    }
    code_generator_emit(generator, "    mov rax, r11\n");
    if (!code_generator_emit_ir_integer_binary_with_rhs_register(
            generator, instruction->text, "r10")) {
      return 0;
    }
    code_generator_emit(generator, "    mov r11, rax\n");
    return 1;
  }

  if (instruction->lhs.kind == IR_OPERAND_INT &&
      ir_immediate_fits_signed_32(instruction->lhs.int_value) &&
      ir_binary_operator_is_commutative(instruction->text)) {
    code_generator_emit(generator, "    mov rax, r11\n");
    if (code_generator_emit_ir_integer_binary_with_rhs_immediate(
            generator, instruction->text, instruction->lhs.int_value)) {
      code_generator_emit(generator, "    mov r11, rax\n");
      return 1;
    }
  }

  if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                      temp_table)) {
    return 0;
  }
  if (!code_generator_emit_ir_integer_binary_with_rhs_register(
          generator, instruction->text, "r11")) {
    return 0;
  }
  code_generator_emit(generator, "    mov r11, rax\n");
  return 1;
}

static int code_generator_find_next_non_nop_in_block(const IRFunction *function,
                                                     size_t start_index,
                                                     size_t *out_index);

static int code_generator_try_emit_ir_binary_expression_chain(
    CodeGenerator *generator, const IRFunction *function, size_t start_index,
    const IRTempUseMap *temp_use_map, IRTempTable *temp_table,
    size_t *last_index_out, int *consumed_branch_out, int *emitted) {
  if (emitted) {
    *emitted = 0;
  }
  if (last_index_out) {
    *last_index_out = start_index;
  }
  if (consumed_branch_out) {
    *consumed_branch_out = 0;
  }
  if (!generator || !function || !temp_use_map || !temp_table ||
      start_index >= function->instruction_count) {
    return 0;
  }

  const IRInstruction *first = &function->instructions[start_index];
  if (!code_generator_ir_binary_chain_candidate(first) ||
      first->dest.kind != IR_OPERAND_TEMP || !first->dest.name ||
      ir_temp_use_map_get(temp_use_map, first->dest.name) != 1) {
    return 1;
  }

  size_t last = start_index;
  const char *previous_temp = first->dest.name;
  while (last + 1 < function->instruction_count) {
    const IRInstruction *next = &function->instructions[last + 1];
    int temp_is_lhs = 0;
    int temp_is_rhs = 0;
    if (!code_generator_ir_binary_temp_use_side(next, previous_temp,
                                                &temp_is_lhs, &temp_is_rhs)) {
      break;
    }

    last++;
    if (next->dest.kind != IR_OPERAND_TEMP || !next->dest.name ||
        ir_temp_use_map_get(temp_use_map, next->dest.name) != 1) {
      break;
    }
    previous_temp = next->dest.name;
  }

  if (last < start_index + 2) {
    return 1;
  }

  int consumes_branch = 0;
  const IRInstruction *last_instruction = &function->instructions[last];
  if (last + 1 < function->instruction_count &&
      last_instruction->dest.kind == IR_OPERAND_TEMP &&
      last_instruction->dest.name &&
      ir_temp_use_map_get(temp_use_map, last_instruction->dest.name) == 1 &&
      ir_binary_operator_is_comparison(last_instruction->text)) {
    const IRInstruction *branch = &function->instructions[last + 1];
    if (branch->op == IR_OP_BRANCH_ZERO && branch->lhs.kind == IR_OPERAND_TEMP &&
        branch->lhs.name &&
        strcmp(branch->lhs.name, last_instruction->dest.name) == 0 &&
        branch->text) {
      consumes_branch = 1;
    }
  }

  size_t return_index = last;
  int consumes_return = 0;
  if (!consumes_branch && last_instruction->dest.kind == IR_OPERAND_TEMP &&
      last_instruction->dest.name) {
    size_t next_index = 0;
    if (code_generator_find_next_non_nop_in_block(function, last + 1,
                                                  &next_index)) {
      const IRInstruction *ret = &function->instructions[next_index];
      if (ret->op == IR_OP_RETURN && ret->lhs.kind == IR_OPERAND_TEMP &&
          ret->lhs.name && strcmp(ret->lhs.name, last_instruction->dest.name) == 0) {
        consumes_return = 1;
        return_index = next_index;
      }
    }
  }

  if (!code_generator_compute_ir_binary_to_r11(generator, first, temp_table)) {
    return 0;
  }

  previous_temp = first->dest.name;
  for (size_t i = start_index + 1; i <= last; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!code_generator_emit_ir_binary_extend_r11(generator, instruction,
                                                  previous_temp, temp_table)) {
      return 0;
    }
    if (i < last) {
      if (instruction->dest.kind != IR_OPERAND_TEMP ||
          !instruction->dest.name) {
        return 0;
      }
      previous_temp = instruction->dest.name;
    }
  }

  if (consumes_branch) {
    const IRInstruction *branch = &function->instructions[last + 1];
    code_generator_emit(generator, "    test r11, r11\n");
    code_generator_emit(generator, "    jz %s\n", branch->text);
  } else if (consumes_return) {
    code_generator_emit(generator, "    mov rax, r11\n");
    code_generator_emit(generator, "    jmp L%s_exit\n",
                        generator->current_function_name
                            ? generator->current_function_name
                            : "function");
  } else {
    code_generator_emit(generator, "    mov rax, r11\n");
    if (!code_generator_store_ir_destination(generator,
                                             &last_instruction->dest,
                                             temp_table)) {
      return 0;
    }
  }

  if (last_index_out) {
    *last_index_out = consumes_return ? return_index : last;
  }
  if (consumed_branch_out) {
    *consumed_branch_out = consumes_branch;
  }
  if (emitted) {
    *emitted = 1;
  }
  return 1;
}

static int code_generator_find_next_non_nop_in_block(const IRFunction *function,
                                                     size_t start_index,
                                                     size_t *out_index) {
  if (!function || !out_index || start_index >= function->instruction_count) {
    return 0;
  }

  for (size_t i = start_index; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      return 0;
    }
    if (instruction->op != IR_OP_NOP) {
      *out_index = i;
      return 1;
    }
  }

  return 0;
}

static int code_generator_try_emit_ir_binary_return_temp(
    CodeGenerator *generator, const IRFunction *function, size_t start_index,
    const IRTempUseMap *temp_use_map, IRTempTable *temp_table,
    size_t *return_index_out, int *emitted) {
  if (emitted) {
    *emitted = 0;
  }
  if (return_index_out) {
    *return_index_out = start_index;
  }
  if (!generator || !function || !temp_use_map || !temp_table ||
      start_index >= function->instruction_count) {
    return 0;
  }

  const IRInstruction *instruction = &function->instructions[start_index];
  if (!instruction || instruction->op != IR_OP_BINARY || instruction->ast_ref ||
      instruction->dest.kind != IR_OPERAND_TEMP || !instruction->dest.name ||
      ir_temp_use_map_get(temp_use_map, instruction->dest.name) != 1) {
    return 1;
  }

  size_t return_index = 0;
  if (!code_generator_find_next_non_nop_in_block(function, start_index + 1,
                                                 &return_index)) {
    return 1;
  }

  const IRInstruction *ret = &function->instructions[return_index];
  if (ret->op != IR_OP_RETURN || ret->lhs.kind != IR_OPERAND_TEMP ||
      !ret->lhs.name || strcmp(ret->lhs.name, instruction->dest.name) != 0) {
    return 1;
  }

  IRInstruction value_instruction = *instruction;
  value_instruction.dest = ir_operand_none();
  if (!code_generator_emit_ir_binary_fallback(generator, &value_instruction,
                                              temp_table)) {
    return 0;
  }

  code_generator_emit(generator, "    jmp L%s_exit\n",
                      generator->current_function_name
                          ? generator->current_function_name
                          : "function");

  if (return_index_out) {
    *return_index_out = return_index;
  }
  if (emitted) {
    *emitted = 1;
  }
  return 1;
}

static int
code_generator_emit_ir_unary_fallback(CodeGenerator *generator,
                                      const IRInstruction *instruction,
                                      IRTempTable *temp_table) {
  if (!generator || !instruction || !instruction->text) {
    return 0;
  }

  const char *op = instruction->text;
  if (strcmp(op, "&") == 0) {
    if (instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name) {
      Symbol *symbol =
          symbol_table_lookup(generator->symbol_table, instruction->lhs.name);
      if (!symbol) {
        code_generator_set_error(generator,
                                 "Unknown symbol '%s' in IR unary '&'",
                                 instruction->lhs.name);
        return 0;
      }
      if (symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
        const char *symbol_name = code_generator_get_link_symbol_name(
            generator, instruction->lhs.name);
        if (!symbol_name) {
          code_generator_set_error(generator,
                                   "Invalid global symbol in IR unary '&'");
          return 0;
        }
        if (symbol->is_extern &&
            !code_generator_emit_extern_symbol(generator, symbol_name)) {
          return 0;
        }
        code_generator_emit(generator, "    lea rax, [rel %s]\n", symbol_name);
      } else {
        code_generator_emit(generator, "    lea rax, [rbp - %d]\n",
                            symbol->data.variable.memory_offset);
      }
    } else {
      code_generator_set_error(generator,
                               "IR unary '&' requires symbol operand");
      return 0;
    }
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  }

  if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                      temp_table)) {
    return 0;
  }

  if (instruction->is_float) {
    int want_bits = (instruction->float_bits == 32) ? 32 : 64;
    int lhs_bits =
        code_generator_ir_operand_float_bits(generator, &instruction->lhs);
    if (lhs_bits != 32 && lhs_bits != 64) {
      lhs_bits = 64;
    }
    const char *mov_x = (want_bits == 32) ? "movd" : "movq";
    const char *suf = (want_bits == 32) ? "ss" : "sd";
    const char *ga = (want_bits == 32) ? "eax" : "rax";
    if (!code_generator_ir_emit_float_gp_into_xmm(generator, "xmm0", REG_RAX,
                                                  lhs_bits, want_bits)) {
      return 0;
    }
    if (strcmp(op, "-") == 0) {
      /* Negate as 0 - x at the operand precision. */
      code_generator_emit(generator, "    pxor xmm1, xmm1\n");
      code_generator_emit(generator, "    sub%s xmm1, xmm0\n", suf);
      code_generator_emit(generator, "    %s %s, xmm1\n", mov_x, ga);
    } else if (strcmp(op, "+") == 0) {
      /* Round-trip through XMM so mixed-width operands match -/. */
      code_generator_emit(generator, "    %s %s, xmm0\n", mov_x, ga);
    } else {
      code_generator_set_error(generator,
                               "Unsupported float unary operator '%s'", op);
      return 0;
    }
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  }

  if (strcmp(op, "-") == 0) {
    code_generator_emit(generator, "    neg rax\n");
  } else if (strcmp(op, "!") == 0) {
    code_generator_emit(generator, "    test rax, rax\n");
    code_generator_emit(generator, "    setz al\n");
    code_generator_emit(generator, "    movzx rax, al\n");
  } else if (strcmp(op, "~") == 0) {
    code_generator_emit(generator, "    not rax\n");
  } else if (strcmp(op, "+") == 0) {
    // No-op
  } else if (strcmp(op, "*") == 0) {
    code_generator_emit(generator, "    mov rax, [rax]\n");
  } else {
    code_generator_set_error(generator, "Unsupported IR unary operator '%s'",
                             op);
    return 0;
  }

  return code_generator_store_ir_destination(generator, &instruction->dest,
                                             temp_table);
}

static int code_generator_ir_operand_is_float(const IROperand *operand) {
  return operand && operand->kind == IR_OPERAND_FLOAT;
}

static int code_generator_ir_operand_has_float_type(CodeGenerator *generator,
                                                    const IROperand *operand) {
  if (!generator || !operand) {
    return 0;
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    Symbol *symbol =
        symbol_table_lookup(generator->symbol_table, operand->name);
    if (symbol && symbol->type &&
        code_generator_is_floating_point_type(symbol->type)) {
      return 1;
    }
  }

  return code_generator_ir_operand_is_float(operand);
}

/* IEEE-754 width (0/32/64) of an operand for the text backend. Prefers the
 * IR-carried float_bits (set by ir_lowering); falls back to the declared
 * symbol type. Mirrors the internal backend's operand_float_bits so both
 * backends agree on single vs double precision. */
static int code_generator_ir_operand_float_bits(CodeGenerator *generator,
                                                 const IROperand *operand) {
  if (!operand) {
    return 0;
  }
  if (operand->float_bits == 32 || operand->float_bits == 64) {
    return operand->float_bits;
  }
  if (operand->kind == IR_OPERAND_FLOAT) {
    return 64; /* literal with no explicit width: default double */
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && generator &&
      generator->symbol_table) {
    Symbol *symbol =
        symbol_table_lookup(generator->symbol_table, operand->name);
    if (symbol && symbol->type) {
      if (symbol->type->kind == TYPE_FLOAT32) {
        return 32;
      }
      if (symbol->type->kind == TYPE_FLOAT64) {
        return 64;
      }
    }
  }
  return 0;
}

static int code_generator_ir_call_argument_is_float(
    CodeGenerator *generator, Symbol *function_symbol,
    const IRInstruction *instruction, size_t argument_index) {
  if (!generator || !instruction ||
      argument_index >= instruction->argument_count) {
    return 0;
  }

  if (function_symbol && function_symbol->kind == SYMBOL_FUNCTION &&
      function_symbol->data.function.parameter_types &&
      argument_index < function_symbol->data.function.parameter_count) {
    Type *parameter_type =
        function_symbol->data.function.parameter_types[argument_index];
    if (parameter_type) {
      return code_generator_is_floating_point_type(parameter_type);
    }
  }

  return code_generator_ir_operand_has_float_type(
      generator, &instruction->arguments[argument_index]);
}

/* Resolve the declared (callee-visible) type of the i-th call argument, used
 * to classify INDIRECT vs DIRECT. NULL if the callee's signature is unknown
 * (e.g. indirect calls without a function symbol) — caller treats NULL as
 * DIRECT for backward compatibility. */
static Type *code_generator_ir_call_argument_declared_type(
    Symbol *function_symbol, size_t argument_index) {
  if (!function_symbol || function_symbol->kind != SYMBOL_FUNCTION ||
      !function_symbol->data.function.parameter_types ||
      argument_index >= function_symbol->data.function.parameter_count) {
    return NULL;
  }
  return function_symbol->data.function.parameter_types[argument_index];
}

static int ir_function_can_promote_rsi_rdi(CodeGenerator *generator,
                                           const IRFunction *function,
                                           Type *return_type) {
  if (!generator || !function || !generator->register_allocator ||
      !generator->register_allocator->calling_convention) {
    return 0;
  }

  CallingConventionSpec *conv_spec =
      generator->register_allocator->calling_convention;
  if (conv_spec->convention != CALLING_CONV_MS_X64) {
    return 0;
  }

  if (code_generator_abi_classify(return_type) == ABI_PASS_INDIRECT) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction) {
      continue;
    }

    if (instruction->ast_ref || instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_INLINE_ASM || instruction->op == IR_OP_NEW) {
      return 0;
    }

    if (instruction->op != IR_OP_CALL || !instruction->text) {
      continue;
    }

    Symbol *callee = symbol_table_lookup(generator->symbol_table,
                                         instruction->text);
    Type *callee_return = NULL;
    if (callee && callee->kind == SYMBOL_FUNCTION) {
      callee_return = callee->data.function.return_type
                          ? callee->data.function.return_type
                          : callee->type;
    }
    if (code_generator_abi_classify(callee_return) == ABI_PASS_INDIRECT) {
      return 0;
    }

    for (size_t arg_i = 0; arg_i < instruction->argument_count; arg_i++) {
      Type *arg_type = code_generator_ir_call_argument_declared_type(callee,
                                                                     arg_i);
      if (code_generator_abi_classify(arg_type) == ABI_PASS_INDIRECT) {
        return 0;
      }
    }
  }

  return 1;
}

static int code_generator_emit_ir_call_argument_stack(CodeGenerator *generator,
                                                      const IROperand *operand,
                                                      IRTempTable *temp_table,
                                                      int stack_slot_offset) {
  if (!code_generator_load_ir_operand(generator, operand, temp_table)) {
    return 0;
  }
  code_generator_emit(generator, "    mov [rsp + %d], rax\n",
                      stack_slot_offset);
  return 1;
}

/* Per-function side-table: which IR temps currently hold a POINTER to an
 * indirect-returned struct, and the byte size of that struct. Populated by
 * emit_ir_call when the callee's return is INDIRECT and the call's dest is
 * a temp. Consumed by IR_OP_ASSIGN when the source is a tagged temp. Cleared
 * between functions. */
typedef struct {
  char **names;     /* interned IR temp names */
  size_t *sizes;    /* byte size of the struct each temp points at */
  size_t count;
  size_t capacity;
} IRIndirectTempTable;

static IRIndirectTempTable g_indirect_temps = {0};

static void ir_indirect_temp_table_reset(void) {
  g_indirect_temps.count = 0;
  /* Names are borrowed pointers (interned IR strings); do not free them. */
}

static void ir_indirect_temp_table_destroy(void) {
  free(g_indirect_temps.names);
  free(g_indirect_temps.sizes);
  g_indirect_temps.names = NULL;
  g_indirect_temps.sizes = NULL;
  g_indirect_temps.count = 0;
  g_indirect_temps.capacity = 0;
}

static int ir_indirect_temp_table_add(const char *name, size_t size) {
  if (!name) return 0;
  if (g_indirect_temps.count >= g_indirect_temps.capacity) {
    size_t new_cap = g_indirect_temps.capacity ? g_indirect_temps.capacity * 2 : 8;
    char **g_names = realloc(g_indirect_temps.names, new_cap * sizeof(char *));
    if (!g_names) return 0;
    g_indirect_temps.names = g_names;
    size_t *g_sizes = realloc(g_indirect_temps.sizes, new_cap * sizeof(size_t));
    if (!g_sizes) return 0;
    g_indirect_temps.sizes = g_sizes;
    g_indirect_temps.capacity = new_cap;
  }
  g_indirect_temps.names[g_indirect_temps.count] = (char *)name;
  g_indirect_temps.sizes[g_indirect_temps.count] = size;
  g_indirect_temps.count++;
  return 1;
}

/* Look up a temp's struct size, 0 if not registered. */
static size_t ir_indirect_temp_table_get(const char *name) {
  if (!name) return 0;
  for (size_t i = 0; i < g_indirect_temps.count; i++) {
    if (g_indirect_temps.names[i] == name ||
        (g_indirect_temps.names[i] && name &&
         strcmp(g_indirect_temps.names[i], name) == 0)) {
      return g_indirect_temps.sizes[i];
    }
  }
  return 0;
}

/* Append a frame offset to the per-function FIFO of indirect-return slots.
 * Called from the function pre-pass once per IR_OP_CALL that returns an
 * INDIRECT type. Slots are dispensed in the same order during emission. */
static int code_generator_ir_push_indirect_return_offset(CodeGenerator *generator,
                                                         int rbp_offset) {
  if (!generator) return 0;
  if (generator->indirect_return_slot_count >=
      generator->indirect_return_slot_capacity) {
    size_t new_cap = generator->indirect_return_slot_capacity
                         ? generator->indirect_return_slot_capacity * 2
                         : 8;
    int *grown = realloc(generator->indirect_return_slot_offsets,
                         new_cap * sizeof(int));
    if (!grown) return 0;
    generator->indirect_return_slot_offsets = grown;
    generator->indirect_return_slot_capacity = new_cap;
  }
  generator->indirect_return_slot_offsets[generator->indirect_return_slot_count++] =
      rbp_offset;
  return 1;
}

/* Dispense the next FIFO entry to a call site. Returns 0 if the FIFO is
 * empty — that indicates a pre-pass/emission mismatch and should be an error. */
static int code_generator_ir_take_pending_indirect_return_offset(
    CodeGenerator *generator) {
  if (!generator) return 0;
  if (generator->indirect_return_slot_cursor >=
      generator->indirect_return_slot_count) {
    return 0;
  }
  return generator->indirect_return_slot_offsets
      [generator->indirect_return_slot_cursor++];
}

static void code_generator_ir_reset_indirect_return_slots(CodeGenerator *generator) {
  if (!generator) return;
  generator->indirect_return_slot_count = 0;
  generator->indirect_return_slot_cursor = 0;
}

/* Load the address of an INDIRECT call argument's source into rax. Today
 * indirect args are always SYMBOL (a struct local/param) or TEMP (a struct
 * produced by an earlier op). For SYMBOL on an indirect param, the source
 * is itself a pointer — load it directly. */
static int code_generator_emit_ir_indirect_arg_source_address(
    CodeGenerator *generator, const IROperand *operand,
    IRTempTable *temp_table) {
  if (!generator || !operand) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL) {
    if (!operand->name) {
      code_generator_set_error(generator,
                               "Malformed IR symbol operand (indirect arg)");
      return 0;
    }
    Symbol *symbol = symbol_table_lookup(generator->symbol_table, operand->name);
    if (!symbol) {
      code_generator_set_error(generator,
                               "Unknown symbol '%s' for indirect call arg",
                               operand->name);
      return 0;
    }
    if (symbol->kind == SYMBOL_PARAMETER &&
        symbol->data.variable.is_indirect_param) {
      code_generator_emit(
          generator, "    mov rax, qword [rbp - %d]  ; indirect arg src ptr\n",
          symbol->data.variable.memory_offset);
      return 1;
    }
    if (symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
      const char *resolved =
          code_generator_get_link_symbol_name(generator, operand->name);
      if (!resolved) {
        code_generator_set_error(generator,
                                 "Invalid global symbol for indirect arg");
        return 0;
      }
      code_generator_emit(generator,
                          "    lea rax, [rel %s]  ; indirect arg src\n",
                          resolved);
      return 1;
    }
    code_generator_emit(generator,
                        "    lea rax, [rbp - %d]  ; indirect arg src\n",
                        symbol->data.variable.memory_offset);
    return 1;
  }
  if (operand->kind == IR_OPERAND_TEMP) {
    if (!operand->name) {
      code_generator_set_error(generator,
                               "Malformed IR temp operand (indirect arg)");
      return 0;
    }
    int offset = ir_temp_table_get_offset(temp_table, operand->name);
    if (offset <= 0) {
      code_generator_set_error(generator,
                               "Unknown IR temp '%s' for indirect arg",
                               operand->name);
      return 0;
    }
    /* If this temp was tagged as holding a pointer to an indirect-returned
     * struct (chained call pattern), the source address IS the value
     * stored in the slot, not the slot itself. */
    if (ir_indirect_temp_table_get(operand->name) > 0) {
      code_generator_emit(
          generator,
          "    mov rax, qword [rbp - %d]  ; indirect arg src (ret-temp)\n",
          offset);
      return 1;
    }
    code_generator_emit(generator,
                        "    lea rax, [rbp - %d]  ; indirect arg src (temp)\n",
                        offset);
    return 1;
  }
  code_generator_set_error(
      generator, "Indirect call argument must be a struct value (kind=%d)",
      operand->kind);
  return 0;
}

/* Emit a byte-for-byte memcpy from [rsi] (src) to [rdi] (dst) of n bytes.
 * Caller must place src/dst in rsi/rdi; this preserves them by saving the
 * non-volatile registers on Win64. Clobbers rcx. Used by call lowering for
 * indirect arg copies. */
static void code_generator_emit_rep_movsb(CodeGenerator *generator,
                                          size_t n) {
  code_generator_emit(generator, "    mov rcx, %zu\n", n);
  code_generator_emit(generator, "    cld\n");
  code_generator_emit(generator, "    rep movsb\n");
}

static int code_generator_emit_ir_call_argument_register(
    CodeGenerator *generator, const IROperand *operand, IRTempTable *temp_table,
    x86Register target_register, int is_float) {
  if (!code_generator_load_ir_operand(generator, operand, temp_table)) {
    return 0;
  }

  const char *register_name = code_generator_get_register_name(target_register);
  if (!register_name) {
    code_generator_set_error(generator,
                             "Invalid parameter register in IR call");
    return 0;
  }

  if (is_float) {
    /* Move the raw IEEE bits into the XMM parameter register at the value's
     * precision: movd for float32 (32-bit pattern in eax), movq otherwise. */
    int bits = code_generator_ir_operand_float_bits(generator, operand);
    if (bits == 32) {
      code_generator_emit(generator, "    movd %s, eax\n", register_name);
    } else {
      code_generator_emit(generator, "    movq %s, rax\n", register_name);
    }
  } else {
    code_generator_emit(generator, "    mov %s, rax\n", register_name);
  }
  return 1;
}

static int code_generator_emit_ir_runtime_trap_call(
    CodeGenerator *generator, const IRInstruction *instruction,
    IRTempTable *temp_table) {
  if (!generator || !instruction || instruction->argument_count == 0) {
    return 0;
  }

  CallingConventionSpec *conv_spec =
      generator->register_allocator->calling_convention;
  if (!conv_spec || conv_spec->int_param_count < 3) {
    code_generator_set_error(generator,
                              "Runtime trap helper requires three integer registers");
    return 0;
  }

  if (!generator->generate_stack_trace_support) {
    const char *first_param_reg =
        code_generator_get_register_name(conv_spec->int_param_registers[0]);
    if (!first_param_reg) {
      code_generator_set_error(generator,
                               "Runtime trap helper requires an integer register");
      return 0;
    }
    if (!code_generator_emit_extern_symbol(generator, "puts") ||
        !code_generator_emit_extern_symbol(generator, "exit")) {
      return 0;
    }
    code_generator_emit(generator, "    ; IR runtime trap\n");
    if (instruction->arguments[0].kind == IR_OPERAND_STRING) {
      code_generator_load_string_literal_as_cstring(
          generator, instruction->arguments[0].name);
    } else if (!code_generator_load_ir_operand(generator, &instruction->arguments[0],
                                               temp_table)) {
      return 0;
    }
    code_generator_emit(generator, "    mov %s, rax\n", first_param_reg);
    if (conv_spec->convention == CALLING_CONV_MS_X64) {
      code_generator_emit(generator,
                          "    sub rsp, %d      ; Shadow space for puts\n",
                          conv_spec->shadow_space_size);
      code_generator_emit(generator, "    call puts\n");
      code_generator_emit(generator, "    add rsp, %d\n",
                          conv_spec->shadow_space_size);
      code_generator_emit(generator, "    mov ecx, 1\n");
      code_generator_emit(generator,
                          "    sub rsp, %d      ; Shadow space for exit\n",
                          conv_spec->shadow_space_size);
      code_generator_emit(generator, "    call exit\n");
      code_generator_emit(generator, "    add rsp, %d\n",
                          conv_spec->shadow_space_size);
    } else {
      code_generator_emit(generator, "    call puts\n");
      code_generator_emit(generator, "    mov edi, 1\n");
      code_generator_emit(generator, "    call exit\n");
    }
    return 1;
  }

  if (!code_generator_emit_extern_symbol(generator, "mettle_crash_trap")) {
    return 0;
  }

  char *trap_pc_label = code_generator_generate_label(generator, "mettledbg_trap_pc");
  if (!trap_pc_label) {
    code_generator_set_error(generator,
                             "Out of memory while creating runtime trap label");
    return 0;
  }

  int shadow_space =
      (conv_spec->convention == CALLING_CONV_MS_X64) ? conv_spec->shadow_space_size
                                                     : 0;
  int call_stack_total = shadow_space;
  if ((call_stack_total % 16) != 0) {
    call_stack_total += 8;
  }

  code_generator_emit(generator, "    ; IR runtime trap\n");
  code_generator_emit(generator, "%s:\n", trap_pc_label);
  if (instruction->arguments[0].kind == IR_OPERAND_STRING) {
    code_generator_load_string_literal_as_cstring(
        generator, instruction->arguments[0].name);
  } else if (!code_generator_load_ir_operand(generator, &instruction->arguments[0],
                                             temp_table)) {
    free(trap_pc_label);
    return 0;
  }
  code_generator_emit(generator, "    mov %s, rax\n",
                      code_generator_get_register_name(
                          conv_spec->int_param_registers[0]));
  code_generator_emit(generator, "    lea %s, [rel %s]\n",
                      code_generator_get_register_name(
                          conv_spec->int_param_registers[1]),
                      trap_pc_label);
  code_generator_emit(generator, "    mov %s, rbp\n",
                      code_generator_get_register_name(
                          conv_spec->int_param_registers[2]));
  if (call_stack_total > 0) {
    code_generator_emit(generator, "    sub rsp, %d\n", call_stack_total);
  }
  code_generator_emit(generator, "    call mettle_crash_trap\n");
  if (call_stack_total > 0) {
    code_generator_emit(generator, "    add rsp, %d\n", call_stack_total);
  }

  free(trap_pc_label);
  return 1;
}

static int code_generator_emit_ir_call(CodeGenerator *generator,
                                       const IRInstruction *instruction,
                                       IRTempTable *temp_table) {
  if (!generator || !instruction || !instruction->text) {
    return 0;
  }

  CallingConventionSpec *conv_spec =
      generator->register_allocator->calling_convention;
  if (!conv_spec) {
    code_generator_set_error(generator, "No calling convention configured");
    return 0;
  }
  Symbol *function_symbol =
      symbol_table_lookup(generator->symbol_table, instruction->text);
  const char *call_target =
      code_generator_get_link_symbol_name(generator, instruction->text);
  if (!call_target) {
    code_generator_set_error(generator, "Invalid IR call target");
    return 0;
  }
  if (strcmp(call_target, "mettle_crash_trap") == 0 ||
      strcmp(call_target, "mettle_crash_trap_ex") == 0) {
    IRInstruction adapted = *instruction;
    if (strcmp(call_target, "mettle_crash_trap_ex") == 0 &&
        instruction->argument_count >= 2 &&
        instruction->arguments[1].kind == IR_OPERAND_STRING) {
      adapted.text = "mettle_crash_trap";
      adapted.argument_count = 1;
      adapted.arguments = (IROperand *)&instruction->arguments[1];
    }
    return code_generator_emit_ir_runtime_trap_call(generator, &adapted,
                                                    temp_table);
  }
  if ((function_symbol && function_symbol->is_extern) || !function_symbol) {
    if (!code_generator_emit_extern_symbol(generator, call_target)) {
      return 0;
    }
  }

  /* Resolve return type up-front: needed to decide whether to allocate a
   * caller-side return slot and prepend a hidden out-pointer as arg 0. */
  Type *return_type = NULL;
  if (function_symbol && function_symbol->kind == SYMBOL_FUNCTION &&
      function_symbol->type) {
    return_type = function_symbol->type;
    if (function_symbol->data.function.return_type) {
      return_type = function_symbol->data.function.return_type;
    }
  }
  int return_is_indirect =
      (code_generator_abi_classify(return_type) == ABI_PASS_INDIRECT) ? 1 : 0;
  size_t return_size =
      return_is_indirect ? code_generator_abi_type_size(return_type) : 0;

  size_t argument_count = instruction->argument_count;
  int *is_float = NULL;
  int *goes_on_stack = NULL;
  int *is_indirect = NULL;            /* INDIRECT (struct > 8B / non-pow2) */
  int *indirect_temp_offset = NULL;   /* per-arg byte offset within temp region */
  size_t *indirect_size = NULL;       /* per-arg sizeof(struct) */
  if (argument_count > 0) {
    is_float = calloc(argument_count, sizeof(int));
    goes_on_stack = calloc(argument_count, sizeof(int));
    is_indirect = calloc(argument_count, sizeof(int));
    indirect_temp_offset = calloc(argument_count, sizeof(int));
    indirect_size = calloc(argument_count, sizeof(size_t));
    if (!is_float || !goes_on_stack || !is_indirect ||
        !indirect_temp_offset || !indirect_size) {
      free(is_float);
      free(goes_on_stack);
      free(is_indirect);
      free(indirect_temp_offset);
      free(indirect_size);
      code_generator_set_error(
          generator, "Out of memory while planning IR call arguments");
      return 0;
    }
  }

  int int_reg_cursor = 0;
  int float_reg_cursor = 0;
  size_t ms_param_slot_count = 0;
  if (conv_spec->convention == CALLING_CONV_MS_X64) {
    ms_param_slot_count = conv_spec->int_param_count;
    if (conv_spec->float_param_count < ms_param_slot_count) {
      ms_param_slot_count = conv_spec->float_param_count;
    }
  }
  /* The hidden out-pointer for an INDIRECT return occupies ABI slot 0
   * (rcx on Win64), shifting every user arg up by one. */
  size_t hidden_arg_count = return_is_indirect ? 1 : 0;
  /* SysV-style cursors must also reserve the hidden slot if present. */
  if (return_is_indirect && conv_spec->convention != CALLING_CONV_MS_X64) {
    int_reg_cursor = 1;
  }
  int stack_argument_count = 0;
  int indirect_temp_region = 0;
  for (size_t i = 0; i < argument_count; i++) {
    Type *declared_param =
        code_generator_ir_call_argument_declared_type(function_symbol, i);
    AbiPassKind abi_kind = code_generator_abi_classify(declared_param);
    is_indirect[i] = (abi_kind == ABI_PASS_INDIRECT) ? 1 : 0;
    if (is_indirect[i]) {
      size_t sz = code_generator_abi_type_size(declared_param);
      indirect_size[i] = sz;
      /* 8-byte alignment per temp; the temp region begins at rsp+0 after
       * the call's sub-rsp, before the shadow space + stack-arg slots. */
      size_t aligned = (sz + 7u) & ~(size_t)7;
      indirect_temp_offset[i] = indirect_temp_region;
      indirect_temp_region += (int)aligned;
      /* INDIRECT args always travel in an integer slot (pointer in a GPR). */
      is_float[i] = 0;
    } else {
      is_float[i] = code_generator_ir_call_argument_is_float(
          generator, function_symbol, instruction, i);
    }
    /* Win64 uses positional slots; the hidden out-ptr (when present) takes
     * slot 0, so the user arg's effective ABI slot is i + hidden_arg_count. */
    size_t abi_slot = i + hidden_arg_count;
    if (conv_spec->convention == CALLING_CONV_MS_X64) {
      if (abi_slot >= ms_param_slot_count) {
        goes_on_stack[i] = 1;
        stack_argument_count++;
      }
    } else if (is_float[i]) {
      if (float_reg_cursor < (int)conv_spec->float_param_count) {
        float_reg_cursor++;
      } else {
        goes_on_stack[i] = 1;
        stack_argument_count++;
      }
    } else {
      if (int_reg_cursor < (int)conv_spec->int_param_count) {
        int_reg_cursor++;
      } else {
        goes_on_stack[i] = 1;
        stack_argument_count++;
      }
    }
  }
  /* If the hidden out-ptr itself does not fit in a register (won't happen on
   * the supported ABIs, but be defensive), force it onto the stack. We
   * always have at least one int arg register on both supported ABIs, so
   * this is just an assert in spirit. */
  if (return_is_indirect && ms_param_slot_count == 0 &&
      conv_spec->convention == CALLING_CONV_MS_X64) {
    code_generator_set_error(
        generator, "Hidden return pointer needs at least one Win64 arg slot");
    free(is_float);
    free(goes_on_stack);
    free(is_indirect);
    free(indirect_temp_offset);
    free(indirect_size);
    return 0;
  }

  code_generator_emit(generator, "    ; IR call: %s (%zu args)\n",
                      instruction->text, argument_count);

  int omit_shadow_space = 0;
  if (conv_spec->convention == CALLING_CONV_MS_X64 && function_symbol &&
      function_symbol->kind == SYMBOL_FUNCTION && !function_symbol->is_extern &&
      (argument_count + hidden_arg_count) <= ms_param_slot_count &&
      indirect_temp_region == 0 && !return_is_indirect) {
    // Internal register-only calls do not need caller home slots.
    omit_shadow_space = 1;
  }
  int shadow_space =
      (conv_spec->convention == CALLING_CONV_MS_X64 && !omit_shadow_space)
          ? conv_spec->shadow_space_size
          : 0;
  int stack_arg_space = stack_argument_count * 8;
  /* Layout of the call's sub-rsp region, low to high:
   *   [rsp + 0 .. indirect_temp_region)              indirect arg temps
   *   [rsp + post_slots_base .. + shadow_space)      Win64 home space
   *   [rsp + post_slots_base + shadow .. )           extra stack arg slots
   *
   * The hidden return slot for INDIRECT returns lives at a FUNCTION-LEVEL
   * frame offset (not in this per-call region), because the consumer
   * accesses it after the call's `add rsp` reclaims the per-call bytes.
   * That offset is assigned in the function prologue pre-pass and looked
   * up here via the generator's pending_indirect_return_offset slot. */
  if (indirect_temp_region > 0) {
    /* Keep the region a multiple of 16 so downstream offsets stay aligned. */
    indirect_temp_region = (indirect_temp_region + 15) & ~15;
  }
  int post_slots_base = indirect_temp_region;
  int call_stack_total = post_slots_base + shadow_space + stack_arg_space;
  if ((call_stack_total % 16) != 0) {
    call_stack_total += 8;
  }
  if (call_stack_total > 0) {
    code_generator_emit(generator, "    sub rsp, %d\n", call_stack_total);
  }
  int return_slot_rbp_offset = 0;
  if (return_is_indirect) {
    return_slot_rbp_offset =
        code_generator_ir_take_pending_indirect_return_offset(generator);
    if (return_slot_rbp_offset <= 0) {
      code_generator_set_error(
          generator,
          "Indirect-return frame slot not assigned for call '%s'",
          instruction->text);
      free(is_float);
      free(goes_on_stack);
      free(is_indirect);
      free(indirect_temp_offset);
      free(indirect_size);
      return 0;
    }
    code_generator_emit(
        generator,
        "    ; hidden return slot (%zu B) at [rbp - %d]\n",
        return_size, return_slot_rbp_offset);
  }

  /* Materialize INDIRECT arguments first: memcpy each struct into its temp.
   * After this loop the temp region holds the copies and rsi/rdi are
   * clobbered, which is fine — argument loading happens after. */
  if (indirect_temp_region > 0) {
    code_generator_emit(generator,
                        "    ; %d byte(s) of indirect-arg temp region\n",
                        indirect_temp_region);
    for (size_t i = 0; i < argument_count; i++) {
      if (!is_indirect[i]) {
        continue;
      }
      /* Load source address into rax. */
      if (!code_generator_emit_ir_indirect_arg_source_address(
              generator, &instruction->arguments[i], temp_table)) {
        free(is_float);
        free(goes_on_stack);
        free(is_indirect);
        free(indirect_temp_offset);
        free(indirect_size);
        return 0;
      }
      /* rsi = source, rdi = dest in temp region. */
      code_generator_emit(generator, "    mov rsi, rax\n");
      code_generator_emit(generator, "    lea rdi, [rsp + %d]\n",
                          indirect_temp_offset[i]);
      code_generator_emit_rep_movsb(generator, indirect_size[i]);
    }
  }

  // Materialize stack arguments into ABI-defined stack slots.
  int stack_arg_index = 0;
  for (size_t i = 0; i < argument_count; i++) {
    if (!goes_on_stack || !goes_on_stack[i]) {
      continue;
    }
    int slot_offset =
        post_slots_base + shadow_space + (stack_arg_index * 8);
    if (is_indirect[i]) {
      /* Place &temp into the stack slot. */
      code_generator_emit(generator, "    lea rax, [rsp + %d]\n",
                          indirect_temp_offset[i]);
      code_generator_emit(generator, "    mov [rsp + %d], rax\n", slot_offset);
    } else if (!code_generator_emit_ir_call_argument_stack(
                   generator, &instruction->arguments[i], temp_table,
                   slot_offset)) {
      free(is_float);
      free(goes_on_stack);
      free(is_indirect);
      free(indirect_temp_offset);
      free(indirect_size);
      return 0;
    }
    stack_arg_index++;
  }

  // Fill register arguments left-to-right.
  int_reg_cursor = 0;
  float_reg_cursor = 0;
  for (size_t i = 0; i < argument_count; i++) {
    if (goes_on_stack && goes_on_stack[i]) {
      continue;
    }

    /* INDIRECT args: pass &temp in the integer arg register for this slot. */
    if (is_indirect[i]) {
      x86Register target_register;
      size_t abi_slot = i + hidden_arg_count;
      if (conv_spec->convention == CALLING_CONV_MS_X64) {
        if (abi_slot >= ms_param_slot_count) {
          code_generator_set_error(
              generator,
              "IR call indirect-arg slot mismatch (Win64)");
          free(is_float);
          free(goes_on_stack);
          free(is_indirect);
          free(indirect_temp_offset);
          free(indirect_size);
          return 0;
        }
        target_register = conv_spec->int_param_registers[abi_slot];
      } else {
        if (int_reg_cursor >= (int)conv_spec->int_param_count) {
          code_generator_set_error(
              generator,
              "IR call indirect-arg slot mismatch (SysV)");
          free(is_float);
          free(goes_on_stack);
          free(is_indirect);
          free(indirect_temp_offset);
          free(indirect_size);
          return 0;
        }
        target_register = conv_spec->int_param_registers[int_reg_cursor++];
      }
      const char *reg_name = code_generator_get_register_name(target_register);
      if (!reg_name) {
        code_generator_set_error(generator,
                                 "Invalid register for indirect call arg");
        free(is_float);
        free(goes_on_stack);
        free(is_indirect);
        free(indirect_temp_offset);
        free(indirect_size);
        return 0;
      }
      code_generator_emit(generator, "    lea %s, [rsp + %d]\n", reg_name,
                          indirect_temp_offset[i]);
      continue;
    }

    if (conv_spec->convention == CALLING_CONV_MS_X64) {
      size_t abi_slot = i + hidden_arg_count;
      if (abi_slot >= ms_param_slot_count) {
        code_generator_set_error(
            generator, "IR call argument classification mismatch (Win64)");
        free(is_float);
        free(goes_on_stack);
        free(is_indirect);
        free(indirect_temp_offset);
        free(indirect_size);
        return 0;
      }
      x86Register target_register =
          (is_float && is_float[i])
              ? conv_spec->float_param_registers[abi_slot]
              : conv_spec->int_param_registers[abi_slot];
      if (!code_generator_emit_ir_call_argument_register(
              generator, &instruction->arguments[i], temp_table,
              target_register, (is_float && is_float[i]) ? 1 : 0)) {
        free(is_float);
        free(goes_on_stack);
        free(is_indirect);
        free(indirect_temp_offset);
        free(indirect_size);
        return 0;
      }
    } else {
      if (is_float && is_float[i]) {
        if (float_reg_cursor >= (int)conv_spec->float_param_count) {
          code_generator_set_error(
              generator, "IR call argument classification mismatch (float)");
          free(is_float);
          free(goes_on_stack);
          free(is_indirect);
          free(indirect_temp_offset);
          free(indirect_size);
          return 0;
        }
        x86Register target_register =
            conv_spec->float_param_registers[float_reg_cursor++];
        if (!code_generator_emit_ir_call_argument_register(
                generator, &instruction->arguments[i], temp_table,
                target_register, 1)) {
          free(is_float);
          free(goes_on_stack);
          free(is_indirect);
          free(indirect_temp_offset);
          free(indirect_size);
          return 0;
        }
      } else {
        if (int_reg_cursor >= (int)conv_spec->int_param_count) {
          code_generator_set_error(
              generator, "IR call argument classification mismatch (integer)");
          free(is_float);
          free(goes_on_stack);
          free(is_indirect);
          free(indirect_temp_offset);
          free(indirect_size);
          return 0;
        }
        x86Register target_register =
            conv_spec->int_param_registers[int_reg_cursor++];
        if (!code_generator_emit_ir_call_argument_register(
                generator, &instruction->arguments[i], temp_table,
                target_register, 0)) {
          free(is_float);
          free(goes_on_stack);
          free(is_indirect);
          free(indirect_temp_offset);
          free(indirect_size);
          return 0;
        }
      }
    }
  }

  /* Hidden out-pointer for an INDIRECT return: emitted LAST, after any
   * user-arg memcpy that may have clobbered rcx via rep movsb. The slot
   * lives in the caller's function frame (allocated by the pre-pass), so
   * the value outlives the call's `add rsp`. */
  if (return_is_indirect) {
    x86Register out_reg = conv_spec->int_param_registers[0];
    const char *out_name = code_generator_get_register_name(out_reg);
    code_generator_emit(
        generator,
        "    lea %s, [rbp - %d]  ; hidden out-ptr (return slot)\n",
        out_name, return_slot_rbp_offset);
  }

  code_generator_emit(generator, "    call %s\n", call_target);

  if (call_stack_total > 0) {
    code_generator_emit(generator, "    add rsp, %d\n", call_stack_total);
  }

  if (return_is_indirect) {
    /* The callee placed the value at [rbp - return_slot_rbp_offset] and
     * (per ABI) also returned that address in rax. Re-materialize rax
     * from the known frame slot so subsequent code can treat the call
     * result as "address of the returned struct" without relying on
     * callee discipline. */
    code_generator_emit(generator,
                        "    lea rax, [rbp - %d]  ; return value base\n",
                        return_slot_rbp_offset);
  } else if (return_type && code_generator_is_floating_point_type(return_type)) {
    /* Win64 returns floats in XMM0; move back at the return precision. */
    if (return_type->kind == TYPE_FLOAT32) {
      code_generator_emit(generator, "    movd eax, xmm0\n");
    } else {
      code_generator_emit(generator, "    movq rax, xmm0\n");
    }
  }
  if (!return_is_indirect) {
    code_generator_handle_return_value(generator, return_type);
  }

  free(is_float);
  free(goes_on_stack);
  free(is_indirect);
  free(indirect_temp_offset);
  free(indirect_size);
  return !generator->has_error;
}

static int code_generator_emit_ir_rotate_add(CodeGenerator *generator,
                                             const IRInstruction *instruction,
                                             IRTempTable *temp_table) {
  if (!generator || !instruction) {
    return 0;
  }

  Symbol *sym_next =
      code_generator_ir_lookup_register_symbol(generator, &instruction->dest);
  Symbol *sym_a =
      code_generator_ir_lookup_register_symbol(generator, &instruction->lhs);
  Symbol *sym_b =
      code_generator_ir_lookup_register_symbol(generator, &instruction->rhs);

  if (sym_next && sym_a && sym_b) {
    const char *reg_next =
        code_generator_ir_symbol_register_name(sym_next, 64);
    const char *reg_a = code_generator_ir_symbol_register_name(sym_a, 64);
    const char *reg_b = code_generator_ir_symbol_register_name(sym_b, 64);
    if (!reg_next || !reg_a || !reg_b) {
      code_generator_set_error(generator,
                               "Invalid register in IR rotate_add fast path");
      return 0;
    }
    code_generator_emit(generator, "    lea %s, [%s + %s]\n", reg_next, reg_a,
                        reg_b);
    code_generator_emit(generator, "    mov %s, %s\n", reg_a, reg_b);
    code_generator_emit(generator, "    mov %s, %s\n", reg_b, reg_next);
    return 1;
  }

  if (!instruction->dest.name || !instruction->lhs.name ||
      !instruction->rhs.name) {
    code_generator_set_error(generator, "Malformed IR rotate_add operands");
    return 0;
  }

  code_generator_load_variable(generator, instruction->lhs.name);
  if (generator->has_error) {
    return 0;
  }

  if (sym_b) {
    const char *reg_b = code_generator_ir_symbol_register_name(sym_b, 64);
    if (!reg_b) {
      code_generator_set_error(generator,
                               "Invalid RHS register in IR rotate_add");
      return 0;
    }
    code_generator_emit(generator, "    add rax, %s\n", reg_b);
  } else {
    code_generator_load_variable(generator, instruction->rhs.name);
    if (generator->has_error) {
      return 0;
    }
    code_generator_emit(generator, "    add rax, rbx\n");
  }

  code_generator_emit(generator, "    mov r11, rax\n");

  if (sym_next) {
    const char *reg_next =
        code_generator_ir_symbol_register_name(sym_next, 64);
    if (!reg_next) {
      code_generator_set_error(generator,
                               "Invalid next register in IR rotate_add");
      return 0;
    }
    code_generator_emit(generator, "    mov %s, r11\n", reg_next);
  } else {
    code_generator_store_variable(generator, instruction->dest.name, "r11");
  }

  if (sym_a && sym_b) {
    const char *reg_a = code_generator_ir_symbol_register_name(sym_a, 64);
    const char *reg_b = code_generator_ir_symbol_register_name(sym_b, 64);
    code_generator_emit(generator, "    mov %s, %s\n", reg_a, reg_b);
  } else if (sym_a) {
    const char *reg_a = code_generator_ir_symbol_register_name(sym_a, 64);
    code_generator_load_variable(generator, instruction->rhs.name);
    if (generator->has_error) {
      return 0;
    }
    code_generator_emit(generator, "    mov %s, rax\n", reg_a);
  } else {
    code_generator_load_variable(generator, instruction->rhs.name);
    if (generator->has_error) {
      return 0;
    }
    code_generator_store_variable(generator, instruction->lhs.name, "rax");
  }

  if (sym_b) {
    const char *reg_b = code_generator_ir_symbol_register_name(sym_b, 64);
    code_generator_emit(generator, "    mov %s, r11\n", reg_b);
  } else {
    code_generator_store_variable(generator, instruction->rhs.name, "r11");
  }

  return !generator->has_error;
}

static int code_generator_emit_ir_cast(CodeGenerator *generator,
                                       const IRInstruction *instruction,
                                       IRTempTable *temp_table) {
  if (!generator || !instruction || !instruction->text) {
    return 0;
  }

  Type *target_type = NULL;
  if (generator->type_checker) {
    target_type = type_checker_get_type_by_name(generator->type_checker,
                                                instruction->text);
  }

  if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                      temp_table)) {
    return 0;
  }

  int source_is_float = instruction->is_float;
  int target_is_float =
      target_type ? code_generator_is_floating_point_type(target_type) : 0;

  int target_is_unsigned = 0;
  if (target_type) {
    if (target_type->kind == TYPE_UINT8 || target_type->kind == TYPE_UINT16 ||
        target_type->kind == TYPE_UINT32 || target_type->kind == TYPE_UINT64) {
      target_is_unsigned = 1;
    }
  }

  int target_size = target_type ? (int)target_type->size : 8;
  if (target_size <= 0)
    target_size = 8;
  if (target_type && (target_type->kind == TYPE_POINTER ||
                      target_type->kind == TYPE_FUNCTION_POINTER)) {
    target_size = 8;
  }

  /* Source float width comes from the CAST instruction (set by ir_lowering);
   * target float width from the cast's destination type. */
  int src_fb32 = (instruction->float_bits == 32);
  int dst_fb32 = (target_type && target_type->kind == TYPE_FLOAT32);

  if (source_is_float && !target_is_float) {
    /* float -> int: truncate at the source precision. */
    if (src_fb32) {
      code_generator_emit(generator, "    movd xmm0, eax\n");
      code_generator_emit(generator, "    cvttss2si rax, xmm0\n");
    } else {
      code_generator_emit(generator, "    movq xmm0, rax\n");
      code_generator_emit(generator, "    cvttsd2si rax, xmm0\n");
    }
  } else if (!source_is_float && target_is_float) {
    /* int -> float: produce a value at the target precision. */
    if (dst_fb32) {
      code_generator_emit(generator, "    cvtsi2ss xmm0, rax\n");
      code_generator_emit(generator, "    movd eax, xmm0\n");
    } else {
      code_generator_emit(generator, "    cvtsi2sd xmm0, rax\n");
      code_generator_emit(generator, "    movq rax, xmm0\n");
    }
  } else if (source_is_float && target_is_float) {
    /* float -> float: convert only when the precision differs. */
    if (src_fb32 && !dst_fb32) {
      code_generator_emit(generator, "    movd xmm0, eax\n");
      code_generator_emit(generator, "    cvtss2sd xmm0, xmm0\n");
      code_generator_emit(generator, "    movq rax, xmm0\n");
    } else if (!src_fb32 && dst_fb32) {
      code_generator_emit(generator, "    movq xmm0, rax\n");
      code_generator_emit(generator, "    cvtsd2ss xmm0, xmm0\n");
      code_generator_emit(generator, "    movd eax, xmm0\n");
    }
    /* same width: raw bits already correct. */
  } else if (!source_is_float && !target_is_float) {
    if (target_size == 1) {
      if (target_is_unsigned) {
        code_generator_emit(generator, "    movzx rax, al\n");
      } else {
        code_generator_emit(generator, "    movsx rax, al\n");
      }
    } else if (target_size == 2) {
      if (target_is_unsigned) {
        code_generator_emit(generator, "    movzx rax, ax\n");
      } else {
        code_generator_emit(generator, "    movsx rax, ax\n");
      }
    } else if (target_size == 4) {
      if (target_is_unsigned) {
        code_generator_emit(generator, "    mov eax, eax\n");
      } else {
        code_generator_emit(generator, "    movsxd rax, eax\n");
      }
    }
  }

  return code_generator_store_ir_destination(generator, &instruction->dest,
                                             temp_table);
}

static int code_generator_emit_ir_compare_branch_with_jump(
    CodeGenerator *generator, const IRInstruction *compare_instruction,
    const char *jump, const char *target_label, IRTempTable *temp_table) {
  if (!generator || !compare_instruction || !jump || !target_label ||
      !temp_table) {
    return 0;
  }

  {
    int emitted_fastpath = 0;
    if (!code_generator_try_emit_ir_compare_branch_register_fastpath(
            generator, compare_instruction, jump, target_label,
            &emitted_fastpath)) {
      return 0;
    }
    if (emitted_fastpath) {
      return 1;
    }
  }

  if (!compare_instruction->is_float &&
      compare_instruction->rhs.kind == IR_OPERAND_INT &&
      ir_immediate_fits_signed_32(compare_instruction->rhs.int_value)) {
    if (!code_generator_load_ir_operand(generator, &compare_instruction->lhs,
                                        temp_table)) {
      return 0;
    }
    code_generator_emit(generator, "    cmp rax, %lld\n",
                        compare_instruction->rhs.int_value);
    code_generator_emit(generator, "    %s %s\n", jump, target_label);
    return 1;
  }

  if (!code_generator_load_ir_operand_into_register(
          generator, &compare_instruction->rhs, temp_table, REG_R10)) {
    return 0;
  }
  if (!code_generator_load_ir_operand(generator, &compare_instruction->lhs,
                                      temp_table)) {
    return 0;
  }
  code_generator_emit(generator, "    cmp rax, r10\n");
  code_generator_emit(generator, "    %s %s\n", jump, target_label);
  return 1;
}

static int code_generator_emit_ir_compare_branch_zero(
    CodeGenerator *generator, const IRInstruction *compare_instruction,
    const IRInstruction *branch_instruction, IRTempTable *temp_table) {
  if (!generator || !compare_instruction || !branch_instruction ||
      !temp_table || !branch_instruction->text) {
    return 0;
  }

  const char *false_jump =
      ir_false_jump_for_comparison(compare_instruction->text);
  if (!false_jump) {
    return 0;
  }

  return code_generator_emit_ir_compare_branch_with_jump(
      generator, compare_instruction, false_jump, branch_instruction->text,
      temp_table);
}

static const char *code_generator_ir_memory_operand_for_size(int size_bytes);

static int code_generator_emit_ir_branch_zero_jump_over_label(
    CodeGenerator *generator, const IRInstruction *branch_instruction,
    const IRInstruction *jump_instruction, IRTempTable *temp_table) {
  if (!generator || !branch_instruction || !jump_instruction || !temp_table ||
      !jump_instruction->text) {
    return 0;
  }

  if (branch_instruction->lhs.kind == IR_OPERAND_INT) {
    if (branch_instruction->lhs.int_value != 0) {
      code_generator_emit(generator, "    jmp %s\n", jump_instruction->text);
    }
    return 1;
  }

  if (branch_instruction->lhs.kind == IR_OPERAND_SYMBOL &&
      branch_instruction->lhs.name) {
    Symbol *symbol = symbol_table_lookup(generator->symbol_table,
                                         branch_instruction->lhs.name);
    if (symbol &&
        (symbol->kind == SYMBOL_VARIABLE || symbol->kind == SYMBOL_PARAMETER)) {
      if (symbol->data.variable.is_in_register) {
        const char *reg_name = code_generator_get_register_name(
            (x86Register)symbol->data.variable.register_id);
        if (!reg_name) {
          code_generator_set_error(
              generator,
              "Invalid register in IR branch-nonzero fast path");
          return 0;
        }
        code_generator_emit(generator, "    test %s, %s\n", reg_name, reg_name);
        code_generator_emit(generator, "    jnz %s\n", jump_instruction->text);
        return 1;
      }

      if (symbol->data.variable.memory_offset > 0) {
        int size_bytes = 8;
        if (symbol->type && symbol->type->size > 0 && symbol->type->size <= 8) {
          size_bytes = (int)symbol->type->size;
        }
        const char *mem_op = code_generator_ir_memory_operand_for_size(size_bytes);
        code_generator_emit(generator, "    cmp %s [rbp - %d], 0\n", mem_op,
                            symbol->data.variable.memory_offset);
        code_generator_emit(generator, "    jnz %s\n", jump_instruction->text);
        return 1;
      }
    }
  }

  if (branch_instruction->lhs.kind == IR_OPERAND_TEMP &&
      branch_instruction->lhs.name) {
    int offset = ir_temp_table_get_offset(temp_table, branch_instruction->lhs.name);
    if (offset > 0) {
      code_generator_emit(generator, "    cmp qword [rbp - %d], 0\n", offset);
      code_generator_emit(generator, "    jnz %s\n", jump_instruction->text);
      return 1;
    }
  }

  if (!code_generator_load_ir_operand(generator, &branch_instruction->lhs,
                                      temp_table)) {
    return 0;
  }
  code_generator_emit(generator, "    test rax, rax\n");
  code_generator_emit(generator, "    jnz %s\n", jump_instruction->text);
  return 1;
}

static const char *code_generator_ir_memory_operand_for_size(int size_bytes) {
  if (size_bytes <= 1) {
    return "byte";
  }
  if (size_bytes == 2) {
    return "word";
  }
  if (size_bytes == 4) {
    return "dword";
  }
  return "qword";
}

static int code_generator_try_emit_ir_branch_zero_fastpath(
    CodeGenerator *generator, const IRInstruction *instruction,
    IRTempTable *temp_table, int *emitted) {
  if (emitted) {
    *emitted = 0;
  }
  if (!generator || !instruction || !instruction->text || !temp_table) {
    return 0;
  }

  if (instruction->lhs.kind == IR_OPERAND_INT) {
    if (instruction->lhs.int_value == 0) {
      code_generator_emit(generator, "    jmp %s\n", instruction->text);
    }
    if (emitted) {
      *emitted = 1;
    }
    return 1;
  }

  if (instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name) {
    Symbol *symbol =
        symbol_table_lookup(generator->symbol_table, instruction->lhs.name);
    if (symbol &&
        (symbol->kind == SYMBOL_VARIABLE || symbol->kind == SYMBOL_PARAMETER)) {
      if (symbol->data.variable.is_in_register) {
        const char *reg_name = code_generator_get_register_name(
            (x86Register)symbol->data.variable.register_id);
        if (!reg_name) {
          code_generator_set_error(
              generator, "Invalid register in IR branch-zero fast path");
          return 0;
        }
        code_generator_emit(generator, "    test %s, %s\n", reg_name, reg_name);
        code_generator_emit(generator, "    jz %s\n", instruction->text);
        if (emitted) {
          *emitted = 1;
        }
        return 1;
      }

      if (symbol->data.variable.memory_offset > 0) {
        int size_bytes = 8;
        if (symbol->type && symbol->type->size > 0 && symbol->type->size <= 8) {
          size_bytes = (int)symbol->type->size;
        }
        const char *mem_op = code_generator_ir_memory_operand_for_size(size_bytes);
        code_generator_emit(generator, "    cmp %s [rbp - %d], 0\n", mem_op,
                            symbol->data.variable.memory_offset);
        code_generator_emit(generator, "    jz %s\n", instruction->text);
        if (emitted) {
          *emitted = 1;
        }
        return 1;
      }
    }
  }

  if (instruction->lhs.kind == IR_OPERAND_TEMP && instruction->lhs.name) {
    int offset = ir_temp_table_get_offset(temp_table, instruction->lhs.name);
    if (offset > 0) {
      code_generator_emit(generator, "    cmp qword [rbp - %d], 0\n", offset);
      code_generator_emit(generator, "    jz %s\n", instruction->text);
      if (emitted) {
        *emitted = 1;
      }
      return 1;
    }
  }

  return 1;
}

static int code_generator_emit_ir_instruction(CodeGenerator *generator,
                                              const IRInstruction *instruction,
                                              IRTempTable *temp_table) {
  if (!generator || !instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_NOP:
  case IR_OP_DECLARE_LOCAL:
    return 1;

  case IR_OP_LABEL:
    code_generator_emit(generator, "%s:\n",
                        instruction->text ? instruction->text : "ir_label");
    return 1;

  case IR_OP_JUMP:
    code_generator_emit(generator, "    jmp %s\n",
                        instruction->text ? instruction->text : "ir_missing");
    return 1;

  case IR_OP_BRANCH_ZERO:
    {
      int emitted_fastpath = 0;
      if (!code_generator_try_emit_ir_branch_zero_fastpath(
              generator, instruction, temp_table, &emitted_fastpath)) {
        return 0;
      }
      if (emitted_fastpath) {
        return 1;
      }
    }
    if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                        temp_table)) {
      return 0;
    }
    code_generator_emit(generator, "    test rax, rax\n");
    code_generator_emit(generator, "    jz %s\n",
                        instruction->text ? instruction->text : "ir_missing");
    return 1;

  case IR_OP_BRANCH_EQ: {
    const char *eq_target =
        instruction->text ? instruction->text : "ir_missing";
    /* Fast path: comparing against a 32-bit immediate (e.g. the whitespace
     * `c == 32 || c == 9 ...` chain). Emit `cmp <lhs>, imm; je` directly
     * instead of materializing the constant into r10 first. Works whichever
     * side holds the immediate since equality is symmetric. */
    const IROperand *cmp_value = NULL;
    const IROperand *cmp_imm = NULL;
    if (instruction->rhs.kind == IR_OPERAND_INT &&
        ir_immediate_fits_signed_32(instruction->rhs.int_value)) {
      cmp_value = &instruction->lhs;
      cmp_imm = &instruction->rhs;
    } else if (instruction->lhs.kind == IR_OPERAND_INT &&
               ir_immediate_fits_signed_32(instruction->lhs.int_value)) {
      cmp_value = &instruction->rhs;
      cmp_imm = &instruction->lhs;
    }
    if (cmp_value && cmp_value->kind != IR_OPERAND_INT) {
      if (!code_generator_load_ir_operand(generator, cmp_value, temp_table)) {
        return 0;
      }
      code_generator_emit(generator, "    cmp rax, %lld\n",
                          cmp_imm->int_value);
      code_generator_emit(generator, "    je %s\n", eq_target);
      return 1;
    }

    if (!code_generator_load_ir_operand_into_register(
            generator, &instruction->rhs, temp_table, REG_R10)) {
      return 0;
    }
    if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                        temp_table)) {
      return 0;
    }
    code_generator_emit(generator, "    cmp rax, r10\n");
    code_generator_emit(generator, "    je %s\n", eq_target);
    return 1;
  }

  case IR_OP_ASSIGN:
    {
      /* Indirect-return propagation: if the source is a temp known to hold
       * a pointer to a struct returned by an INDIRECT-returning call, and
       * the dest is a struct symbol, memcpy from the temp's struct through
       * to the dest's storage instead of doing an 8-byte pointer copy. */
      if (instruction->lhs.kind == IR_OPERAND_TEMP && instruction->lhs.name &&
          instruction->dest.kind == IR_OPERAND_SYMBOL &&
          instruction->dest.name) {
        size_t struct_bytes = ir_indirect_temp_table_get(instruction->lhs.name);
        if (struct_bytes > 0) {
          Symbol *dest_sym = symbol_table_lookup(generator->symbol_table,
                                                 instruction->dest.name);
          if (dest_sym && dest_sym->type &&
              code_generator_type_is_aggregate(dest_sym->type)) {
            /* Load the temp's pointer value into rax via the normal path,
             * then memcpy struct_bytes from [rax] to &dest. */
            if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                                temp_table)) {
              return 0;
            }
            code_generator_emit(generator, "    mov rsi, rax\n");
            code_generator_emit(
                generator,
                "    lea rdi, [rbp - %d]  ; struct dest (indirect ret)\n",
                dest_sym->data.variable.memory_offset);
            code_generator_emit_rep_movsb(generator, struct_bytes);
            return 1;
          }
        }
      }
      int emitted_fastpath = 0;
      if (!code_generator_try_emit_ir_assign_symbol_to_symbol(
              generator, instruction, &emitted_fastpath)) {
        return 0;
      }
      if (emitted_fastpath) {
        return 1;
      }
    }
    if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                        temp_table)) {
      return 0;
    }
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);

  case IR_OP_ADDRESS_OF:
    return code_generator_emit_ir_address_of(generator, instruction,
                                             temp_table);

  case IR_OP_LOAD:
    return code_generator_emit_ir_load(generator, instruction, temp_table);

  case IR_OP_STORE:
    return code_generator_emit_ir_store(generator, instruction, temp_table);

  case IR_OP_BINARY:
    if (instruction->ast_ref) {
      code_generator_generate_expression(generator, instruction->ast_ref);
      if (generator->has_error) {
        return 0;
      }
      return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
    }
    return code_generator_emit_ir_binary_fallback(generator, instruction,
                                                  temp_table);

  case IR_OP_ROTATE_ADD:
    return code_generator_emit_ir_rotate_add(generator, instruction, temp_table);

  case IR_OP_UNARY:
    if (instruction->ast_ref) {
      code_generator_generate_expression(generator, instruction->ast_ref);
      if (generator->has_error) {
        return 0;
      }
      return code_generator_store_ir_destination(generator, &instruction->dest,
                                                 temp_table);
    }
    return code_generator_emit_ir_unary_fallback(generator, instruction,
                                                 temp_table);

  case IR_OP_CALL: {
    if (!code_generator_emit_ir_call(generator, instruction, temp_table)) {
      return 0;
    }
    /* If the callee returns an INDIRECT struct and the dest is a symbol of
     * a struct type, memcpy from rax (the returned pointer) into dest's
     * storage. The default 8-byte store would only capture the pointer's
     * low word, defeating the whole point of the indirect return. */
    Symbol *callee_sym =
        instruction->text
            ? symbol_table_lookup(generator->symbol_table, instruction->text)
            : NULL;
    Type *callee_ret = NULL;
    if (callee_sym && callee_sym->kind == SYMBOL_FUNCTION) {
      callee_ret = callee_sym->data.function.return_type
                       ? callee_sym->data.function.return_type
                       : callee_sym->type;
    }
    int callee_indirect =
        (code_generator_abi_classify(callee_ret) == ABI_PASS_INDIRECT) ? 1 : 0;
    /* Direct memcpy fast path: CALL whose dest is a struct symbol. */
    if (callee_indirect && instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name) {
      Symbol *dest_sym =
          symbol_table_lookup(generator->symbol_table, instruction->dest.name);
      if (dest_sym && dest_sym->type &&
          code_generator_type_is_aggregate(dest_sym->type)) {
        size_t copy_bytes = code_generator_abi_type_size(callee_ret);
        code_generator_emit(generator, "    mov rsi, rax\n");
        code_generator_emit(generator,
                            "    lea rdi, [rbp - %d]  ; struct dest\n",
                            dest_sym->data.variable.memory_offset);
        code_generator_emit_rep_movsb(generator, copy_bytes);
        return 1;
      }
    }
    /* Temp dest: register the temp as "holds a struct pointer of size N" so
     * the next IR_OP_ASSIGN (temp -> struct symbol) can memcpy correctly. */
    if (callee_indirect && instruction->dest.kind == IR_OPERAND_TEMP &&
        instruction->dest.name) {
      size_t sz = code_generator_abi_type_size(callee_ret);
      if (!ir_indirect_temp_table_add(instruction->dest.name, sz)) {
        code_generator_set_error(generator,
                                 "Out of memory tracking indirect-return temp");
        return 0;
      }
    }
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  }

  case IR_OP_CALL_INDIRECT: {
    CallingConventionSpec *conv_spec =
        generator->register_allocator->calling_convention;
    if (!conv_spec) {
      code_generator_set_error(generator, "No calling convention configured");
      return 0;
    }

    size_t argument_count = instruction->argument_count;
    int *is_float = NULL;
    int *goes_on_stack = NULL;
    if (argument_count > 0) {
      is_float = calloc(argument_count, sizeof(int));
      goes_on_stack = calloc(argument_count, sizeof(int));
      if (!is_float || !goes_on_stack) {
        free(is_float);
        free(goes_on_stack);
        code_generator_set_error(generator, "Out of memory for indirect call");
        return 0;
      }
    }

    int int_reg_cursor = 0;
    int float_reg_cursor = 0;
    size_t ms_param_slot_count = 0;
    if (conv_spec->convention == CALLING_CONV_MS_X64) {
      ms_param_slot_count = conv_spec->int_param_count;
      if (conv_spec->float_param_count < ms_param_slot_count) {
        ms_param_slot_count = conv_spec->float_param_count;
      }
    }
    int stack_argument_count = 0;
    for (size_t i = 0; i < argument_count; i++) {
      is_float[i] = code_generator_ir_call_argument_is_float(generator, NULL,
                                                             instruction, i);
      if (conv_spec->convention == CALLING_CONV_MS_X64) {
        if (i >= ms_param_slot_count) {
          goes_on_stack[i] = 1;
          stack_argument_count++;
        }
      } else if (is_float[i]) {
        if (float_reg_cursor < (int)conv_spec->float_param_count) {
          float_reg_cursor++;
        } else {
          goes_on_stack[i] = 1;
          stack_argument_count++;
        }
      } else {
        if (int_reg_cursor < (int)conv_spec->int_param_count) {
          int_reg_cursor++;
        } else {
          goes_on_stack[i] = 1;
          stack_argument_count++;
        }
      }
    }

    code_generator_emit(generator, "    ; Indirect function call\n");

    int shadow_space = (conv_spec->convention == CALLING_CONV_MS_X64)
                           ? conv_spec->shadow_space_size
                           : 0;
    int stack_arg_space = stack_argument_count * 8;
    int call_stack_total = shadow_space + stack_arg_space;
    if ((call_stack_total % 16) != 0) {
      call_stack_total += 8;
    }
    if (call_stack_total > 0) {
      code_generator_emit(generator, "    sub rsp, %d\n", call_stack_total);
    }

    int stack_arg_index = 0;
    for (size_t i = 0; i < argument_count; i++) {
      if (!goes_on_stack || !goes_on_stack[i]) {
        continue;
      }
      int slot_offset = shadow_space + (stack_arg_index * 8);
      if (!code_generator_emit_ir_call_argument_stack(
              generator, &instruction->arguments[i], temp_table, slot_offset)) {
        free(is_float);
        free(goes_on_stack);
        return 0;
      }
      stack_arg_index++;
    }

    int_reg_cursor = 0;
    float_reg_cursor = 0;
    for (size_t i = 0; i < argument_count; i++) {
      if (goes_on_stack && goes_on_stack[i]) {
        continue;
      }
      if (conv_spec->convention == CALLING_CONV_MS_X64) {
        x86Register target_register = (is_float && is_float[i])
                                          ? conv_spec->float_param_registers[i]
                                          : conv_spec->int_param_registers[i];
        if (!code_generator_emit_ir_call_argument_register(
                generator, &instruction->arguments[i], temp_table,
                target_register, (is_float && is_float[i]) ? 1 : 0)) {
          free(is_float);
          free(goes_on_stack);
          return 0;
        }
      } else {
        if (is_float && is_float[i]) {
          x86Register target_register =
              conv_spec->float_param_registers[float_reg_cursor++];
          if (!code_generator_emit_ir_call_argument_register(
                  generator, &instruction->arguments[i], temp_table,
                  target_register, 1)) {
            free(is_float);
            free(goes_on_stack);
            return 0;
          }
        } else {
          x86Register target_register =
              conv_spec->int_param_registers[int_reg_cursor++];
          if (!code_generator_emit_ir_call_argument_register(
                  generator, &instruction->arguments[i], temp_table,
                  target_register, 0)) {
            free(is_float);
            free(goes_on_stack);
            return 0;
          }
        }
      }
    }

    if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                        temp_table)) {
      free(is_float);
      free(goes_on_stack);
      return 0;
    }
    code_generator_emit(generator, "    call rax\n");

    if (call_stack_total > 0) {
      code_generator_emit(generator, "    add rsp, %d\n", call_stack_total);
    }

    free(is_float);
    free(goes_on_stack);
    code_generator_handle_return_value(generator, NULL);
    return code_generator_store_ir_destination(generator, &instruction->dest,
                                               temp_table);
  }

  case IR_OP_NEW:
    return code_generator_emit_ir_new(generator, instruction, temp_table);

  case IR_OP_CAST:
    return code_generator_emit_ir_cast(generator, instruction, temp_table);

  case IR_OP_RETURN:
    if (generator->current_fn_returns_indirect &&
        instruction->lhs.kind != IR_OPERAND_NONE) {
      /* Struct return: memcpy the operand's struct value through the hidden
       * out-pointer stored at [rbp - 8], then put that pointer into rax. */
      if (!code_generator_emit_ir_indirect_arg_source_address(
              generator, &instruction->lhs, temp_table)) {
        return 0;
      }
      code_generator_emit(generator, "    mov rsi, rax\n");
      code_generator_emit(generator,
                          "    mov rdi, qword [rbp - 8]  ; hidden out-ptr\n");
      code_generator_emit_rep_movsb(
          generator, generator->current_fn_indirect_return_size);
      code_generator_emit(generator,
                          "    mov rax, qword [rbp - 8]  ; return out-ptr\n");
    } else if (instruction->lhs.kind != IR_OPERAND_NONE) {
      if (!code_generator_load_ir_operand(generator, &instruction->lhs,
                                          temp_table)) {
        return 0;
      }
    }
    code_generator_emit(generator, "    jmp L%s_exit\n",
                        generator->current_function_name
                            ? generator->current_function_name
                            : "function");
    return 1;

  case IR_OP_INLINE_ASM:
    code_generator_emit(generator, "    ; Begin inline assembly block\n");
    code_generator_preserve_registers_for_inline_asm(generator);
    code_generator_emit(generator, "%s\n",
                        instruction->text ? instruction->text : "");
    code_generator_restore_registers_after_inline_asm(generator);
    code_generator_emit(generator, "    ; End inline assembly block\n");
    return 1;
  case IR_OP_COUNT_WORD_STARTS:
    return code_generator_emit_ir_count_word_starts(generator, instruction);

  case IR_OP_MEMCPY_INLINE:
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
  case IR_OP_SIMD_BYTE_MAP:
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
    code_generator_set_error(
        generator,
        "IR opcode %d requires the direct object (--emit-obj) backend",
        (int)instruction->op);
    return 0;

  default:
    code_generator_set_error(generator, "Unhandled IR opcode: %d",
                             instruction->op);
    return 0;
  }
}

static int code_generator_add_stack_size(CodeGenerator *generator,
                                         int *stack_size, int amount,
                                         const char *function_name) {
  if (!generator || !stack_size || amount < 0) {
    return 0;
  }
  if (*stack_size > INT_MAX - amount) {
    code_generator_set_error(generator,
                             "Stack frame too large in function '%s' (integer "
                             "overflow while sizing)",
                             function_name ? function_name : "<unknown>");
    return 0;
  }
  *stack_size += amount;
  return 1;
}

int code_generator_generate_function_from_ir(CodeGenerator *generator,
                                             ASTNode *function_declaration,
                                             IRFunction *ir_function) {
  if (!generator || !function_declaration || !ir_function ||
      function_declaration->type != AST_FUNCTION_DECLARATION) {
    return 0;
  }

  FunctionDeclaration *function_data =
      (FunctionDeclaration *)function_declaration->data;
  if (!function_data || !function_data->name) {
    code_generator_set_error(generator, "Malformed function declaration");
    return 0;
  }
  char *runtime_end_label = NULL;
  if (generator->debug_info) {
    runtime_end_label = code_generator_generate_label(generator, "mettledbg_func_end");
    if (!runtime_end_label) {
      code_generator_set_error(generator,
                               "Out of memory while tracking function debug range");
      return 0;
    }
    code_generator_add_runtime_function_mapping(
        generator, function_data->name, function_data->name, runtime_end_label,
        function_declaration->location.line,
        function_declaration->location.column,
        code_generator_runtime_filename(generator,
                                        function_declaration->location.filename));
  }

  if (!symbol_table_enter_scope(generator->symbol_table, SCOPE_FUNCTION)) {
    code_generator_set_error(generator,
                             "Out of memory while entering function scope");
    return 0;
  }

  if (generator->generate_debug_info) {
    code_generator_add_debug_symbol(
        generator, function_data->name, DEBUG_SYMBOL_FUNCTION,
        function_data->return_type, function_declaration->location.line,
        function_declaration->location.column);
    code_generator_add_line_mapping(generator,
                                    function_declaration->location.line,
                                    function_declaration->location.column,
                                    generator->debug_info->source_filename);
  }

  code_generator_emit(generator, "\nglobal %s\n", function_data->name);

  /* Does this function return INDIRECT? If so, we reserve home slot 0 for
   * the hidden out-pointer and shift user parameter homes up by one slot. */
  Type *fn_return_type = NULL;
  if (function_data->return_type) {
    fn_return_type = type_checker_get_type_by_name(generator->type_checker,
                                                   function_data->return_type);
  }
  int has_hidden_return =
      (code_generator_abi_classify(fn_return_type) == ABI_PASS_INDIRECT) ? 1 : 0;

  int parameter_home_size = 0;
  size_t home_slot_count = function_data->parameter_count + (size_t)has_hidden_return;
  if (home_slot_count > 0) {
    if (home_slot_count > (size_t)(INT_MAX / 8)) {
      code_generator_set_error(generator,
                               "Too many parameters in function '%s'",
                               function_data->name);
      symbol_table_exit_scope(generator->symbol_table);
      return 0;
    }
    parameter_home_size = (int)(home_slot_count * 8);
  }

  int stack_size = parameter_home_size;
  IRTempTable temp_table = {0};
  IRLocalTable local_table = {0};
  IRSymbolStatsMap symbol_stats = {0};
  IRPromotedSymbol promoted_symbols[sizeof(IR_PROMOTION_REGISTERS) /
                                    sizeof(IR_PROMOTION_REGISTERS[0])] = {0};
  size_t promoted_symbol_count = 0;
  int promoted_save_bytes = 0;
  int stack_offset_base = parameter_home_size;

  /* Loop-depth weighting: a symbol used inside a nested loop deserves a
   * promotion register far more than one touched only in straight-line setup
   * code, even if the latter has more static occurrences. NULL is fine for a
   * loop-free function (every weight falls back to 1). */
  unsigned char *loop_depths = ir_compute_loop_depths(ir_function);

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    IRInstruction *instruction = &ir_function->instructions[i];
    if (!instruction) {
      continue;
    }

    size_t use_weight =
        loop_depths ? ir_loop_depth_weight(loop_depths[i]) : 1;
    if (!ir_symbol_stats_map_record_instruction(&symbol_stats, instruction,
                                                use_weight)) {
      code_generator_set_error(generator,
                               "Out of memory while tracking IR symbol usage");
      free(loop_depths);
      ir_temp_table_destroy(&temp_table);
      ir_local_table_destroy(&local_table);
      ir_symbol_stats_map_destroy(&symbol_stats);
      symbol_table_exit_scope(generator->symbol_table);
      return 0;
    }

    if (instruction->op == IR_OP_DECLARE_LOCAL &&
        instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name != NULL) {
      const char *type_name =
          (instruction->text && instruction->text[0] != '\0')
              ? instruction->text
              : "int64";
      Type *local_type =
          type_checker_get_type_by_name(generator->type_checker, type_name);
      if (!local_type) {
        local_type =
            type_checker_get_type_by_name(generator->type_checker, "int64");
      }

      int local_size = 0;
      if (local_type && local_type->size > 0) {
        local_size = (int)local_type->size;
      } else {
        local_size = code_generator_calculate_variable_size(
            generator, instruction->text ? instruction->text : "int64");
      }
      if (local_size <= 0) {
        local_size = 8;
      }
      int alignment = (local_size > 4) ? 8 : local_size;
      if (alignment < 1) {
        alignment = 1;
      }

      Symbol *existing = symbol_table_lookup_current_scope(
          generator->symbol_table, instruction->dest.name);
      if (!existing) {
        Symbol *local_symbol =
            symbol_create(instruction->dest.name, SYMBOL_VARIABLE, local_type);
        if (!local_symbol ||
            !symbol_table_declare(generator->symbol_table, local_symbol)) {
          symbol_destroy(local_symbol);
           code_generator_set_error(generator,
                                    "Failed to declare local '%s' in IR backend",
                                    instruction->dest.name);
           ir_temp_table_destroy(&temp_table);
           ir_local_table_destroy(&local_table);
           ir_symbol_stats_map_destroy(&symbol_stats);
           symbol_table_exit_scope(generator->symbol_table);
           return 0;
         }
       }

      if (!ir_local_table_add(&local_table, instruction->dest.name,
                              instruction->text, instruction->location,
                              local_size, alignment)) {
        code_generator_set_error(generator,
                                 "Out of memory while tracking IR local '%s'",
                                 instruction->dest.name);
        ir_temp_table_destroy(&temp_table);
        ir_local_table_destroy(&local_table);
        ir_symbol_stats_map_destroy(&symbol_stats);
        symbol_table_exit_scope(generator->symbol_table);
        return 0;
      }
      if (!code_generator_add_stack_size(
              generator, &stack_size, local_size + 15, function_data->name)) {
        ir_temp_table_destroy(&temp_table);
        ir_local_table_destroy(&local_table);
        ir_symbol_stats_map_destroy(&symbol_stats);
        symbol_table_exit_scope(generator->symbol_table);
        return 0;
      }
    }

    if (instruction->dest.kind == IR_OPERAND_TEMP && instruction->dest.name) {
      if (!ir_temp_table_add(&temp_table, instruction->dest.name)) {
        code_generator_set_error(generator,
                                 "Out of memory while tracking IR temp '%s'",
                                 instruction->dest.name);
        ir_temp_table_destroy(&temp_table);
        ir_local_table_destroy(&local_table);
        ir_symbol_stats_map_destroy(&symbol_stats);
        symbol_table_exit_scope(generator->symbol_table);
        return 0;
      }
    }
  }
  free(loop_depths);
  loop_depths = NULL;
  if (temp_table.count > (size_t)(INT_MAX / (8 + 15))) {
    code_generator_set_error(
        generator, "Stack frame too large in function '%s' (too many temps)",
        function_data->name);
    ir_temp_table_destroy(&temp_table);
    ir_local_table_destroy(&local_table);
    ir_symbol_stats_map_destroy(&symbol_stats);
    symbol_table_exit_scope(generator->symbol_table);
    return 0;
  }
  if (!code_generator_add_stack_size(generator, &stack_size,
                                     (int)(temp_table.count * (8 + 15)),
                                     function_data->name)) {
    ir_temp_table_destroy(&temp_table);
    ir_local_table_destroy(&local_table);
    ir_symbol_stats_map_destroy(&symbol_stats);
    symbol_table_exit_scope(generator->symbol_table);
    return 0;
  }

  /* Reserve a function-level slot for each IR_OP_CALL whose return type is
   * INDIRECT. Each slot is sized to the return type and 16-byte aligned. The
   * call-site emit code pops these in instruction order. */
  code_generator_ir_reset_indirect_return_slots(generator);
  for (size_t pp_i = 0; pp_i < ir_function->instruction_count; pp_i++) {
    const IRInstruction *pp_insn = &ir_function->instructions[pp_i];
    if (pp_insn->op != IR_OP_CALL || !pp_insn->text) continue;
    Symbol *callee =
        symbol_table_lookup(generator->symbol_table, pp_insn->text);
    Type *ret_t = NULL;
    if (callee && callee->kind == SYMBOL_FUNCTION) {
      ret_t = callee->data.function.return_type
                  ? callee->data.function.return_type
                  : callee->type;
    }
    if (code_generator_abi_classify(ret_t) != ABI_PASS_INDIRECT) continue;
    size_t sz = code_generator_abi_type_size(ret_t);
    int slot_bytes = (int)((sz + 15u) & ~(size_t)15);
    int prev_stack = stack_size;
    if (!code_generator_add_stack_size(generator, &stack_size, slot_bytes,
                                       function_data->name)) {
      ir_temp_table_destroy(&temp_table);
      ir_local_table_destroy(&local_table);
      ir_symbol_stats_map_destroy(&symbol_stats);
      symbol_table_exit_scope(generator->symbol_table);
      return 0;
    }
    /* Slot occupies [rbp - (prev_stack + slot_bytes) .. rbp - prev_stack).
     * We hand out the high address (start of the slot in low-to-high
     * terms): the rbp-relative offset of the slot base. */
    int slot_rbp_offset = prev_stack + slot_bytes;
    if (!code_generator_ir_push_indirect_return_offset(generator,
                                                        slot_rbp_offset)) {
      code_generator_set_error(
          generator,
          "Out of memory recording indirect-return slot in '%s'",
          function_data->name);
      ir_temp_table_destroy(&temp_table);
      ir_local_table_destroy(&local_table);
      ir_symbol_stats_map_destroy(&symbol_stats);
      symbol_table_exit_scope(generator->symbol_table);
      return 0;
    }
  }

  size_t max_promoted =
      sizeof(IR_PROMOTION_REGISTERS) / sizeof(IR_PROMOTION_REGISTERS[0]);
  if (!ir_function_can_promote_rsi_rdi(generator, ir_function, fn_return_type) &&
      max_promoted >= 2) {
    max_promoted -= 2;
  }
  int function_has_no_calls =
      !ir_function_blocks_register_promotion(ir_function);

  if (function_has_no_calls) {
    for (size_t insn_i = 0;
         insn_i < ir_function->instruction_count &&
         promoted_symbol_count < max_promoted;
         insn_i++) {
      const IRInstruction *insn = &ir_function->instructions[insn_i];
      if (insn->op != IR_OP_ROTATE_ADD) {
        continue;
      }

      const IROperand *operands[3] = {&insn->dest, &insn->lhs, &insn->rhs};
      for (size_t op_i = 0;
           op_i < 3 && promoted_symbol_count < max_promoted;
           op_i++) {
        const IROperand *operand = operands[op_i];
        if (operand->kind != IR_OPERAND_SYMBOL || !operand->name ||
            ir_promoted_symbol_find(promoted_symbols, promoted_symbol_count,
                                    operand->name) >= 0 ||
            ir_symbol_stats_map_is_address_taken(&symbol_stats,
                                                 operand->name)) {
          continue;
        }

        Symbol *sym = symbol_table_lookup_current_scope(generator->symbol_table,
                                                        operand->name);
        if (!sym || sym->kind != SYMBOL_VARIABLE ||
            !ir_is_integer_or_pointer_promotable(sym->type)) {
          continue;
        }

        promoted_symbols[promoted_symbol_count].name = operand->name;
        promoted_symbols[promoted_symbol_count].reg =
            IR_PROMOTION_REGISTERS[promoted_symbol_count];
        promoted_symbol_count++;
      }
    }
  }

  for (size_t reg_index = promoted_symbol_count;
       reg_index < max_promoted;
       reg_index++) {
    const char *best_name = NULL;
    size_t best_use_count = 0;

    for (size_t i = 0; i < function_data->parameter_count; i++) {
      const char *name = function_data->parameter_names[i];
      const char *type_name = function_data->parameter_types[i];
      if (!name || ir_promoted_symbol_find(promoted_symbols, promoted_symbol_count,
                                           name) >= 0 ||
          ir_symbol_stats_map_is_address_taken(&symbol_stats, name)) {
        continue;
      }

      Type *param_type = NULL;
      if (type_name) {
        param_type =
            type_checker_get_type_by_name(generator->type_checker, type_name);
      }
      if (!param_type || !ir_is_integer_or_pointer_promotable(param_type)) {
        continue;
      }

      size_t use_count = ir_symbol_stats_map_get_use_count(&symbol_stats, name);
      if (use_count > best_use_count) {
        best_use_count = use_count;
        best_name = name;
      }
    }

    for (size_t i = 0; i < local_table.count; i++) {
      const char *name = local_table.items[i].name;
      if (!name || ir_promoted_symbol_find(promoted_symbols, promoted_symbol_count,
                                           name) >= 0 ||
          ir_symbol_stats_map_is_address_taken(&symbol_stats, name)) {
        continue;
      }

      Symbol *local_symbol =
          symbol_table_lookup_current_scope(generator->symbol_table, name);
      if (!local_symbol || local_symbol->kind != SYMBOL_VARIABLE ||
          !ir_is_integer_or_pointer_promotable(local_symbol->type)) {
        continue;
      }

      size_t use_count = ir_symbol_stats_map_get_use_count(&symbol_stats, name);
      if (use_count > best_use_count) {
        best_use_count = use_count;
        best_name = name;
      }
    }

    if (!best_name || best_use_count < 3) {
      break;
    }

    promoted_symbols[promoted_symbol_count].name = best_name;
    promoted_symbols[promoted_symbol_count].reg =
        IR_PROMOTION_REGISTERS[reg_index];
    promoted_symbol_count++;
  }

  if (promoted_symbol_count > (size_t)(INT_MAX / 8)) {
    code_generator_set_error(
        generator, "Too many promoted symbols in function '%s'",
        function_data->name);
    ir_temp_table_destroy(&temp_table);
    ir_local_table_destroy(&local_table);
    ir_symbol_stats_map_destroy(&symbol_stats);
    symbol_table_exit_scope(generator->symbol_table);
    return 0;
  }
  promoted_save_bytes = (int)(promoted_symbol_count * 8);
  if (promoted_save_bytes > 0) {
    if (!code_generator_add_stack_size(generator, &stack_size, promoted_save_bytes,
                                       function_data->name)) {
      ir_temp_table_destroy(&temp_table);
      ir_local_table_destroy(&local_table);
      ir_symbol_stats_map_destroy(&symbol_stats);
      symbol_table_exit_scope(generator->symbol_table);
      return 0;
    }
    stack_offset_base += promoted_save_bytes;
    for (size_t i = 0; i < promoted_symbol_count; i++) {
      promoted_symbols[i].save_offset = parameter_home_size + (int)((i + 1) * 8);
    }
  }

  if (generator->type_checker && generator->type_checker->error_reporter &&
      stack_size >= STACK_FRAME_WARN_THRESHOLD) {
    char warning[256];
    snprintf(warning, sizeof(warning),
             "Large stack frame in function '%s' (%d bytes); this increases "
             "stack overflow risk",
             function_data->name, stack_size);
    error_reporter_add_warning(generator->type_checker->error_reporter,
                               ERROR_SEMANTIC, function_declaration->location,
                               warning);
  }

  code_generator_function_prologue(generator, function_data->name, stack_size);
  generator->current_stack_offset = stack_offset_base;
  code_generator_register_function_parameters(generator, function_data,
                                              parameter_home_size,
                                              has_hidden_return);
  generator->current_fn_returns_indirect = has_hidden_return;
  generator->current_fn_indirect_return_size =
      has_hidden_return ? code_generator_abi_type_size(fn_return_type) : 0;
  for (size_t i = 0; i < promoted_symbol_count; i++) {
    const char *reg_name = code_generator_get_register_name(promoted_symbols[i].reg);
    if (!reg_name) {
      code_generator_set_error(generator,
                               "Invalid promoted register in function '%s'",
                               function_data->name);
      break;
    }
    code_generator_emit(generator, "    mov [rbp - %d], %s\n",
                        promoted_symbols[i].save_offset, reg_name);
  }

  for (size_t i = 0; i < promoted_symbol_count; i++) {
    const char *name = promoted_symbols[i].name;
    Symbol *symbol =
        name ? symbol_table_lookup_current_scope(generator->symbol_table, name) : NULL;
    if (!symbol ||
        (symbol->kind != SYMBOL_VARIABLE && symbol->kind != SYMBOL_PARAMETER)) {
      code_generator_set_error(
          generator, "Failed to activate promoted symbol '%s' in IR backend",
          name ? name : "<unknown>");
      break;
    }

    int is_parameter = (symbol->kind == SYMBOL_PARAMETER);
    if (symbol->kind == SYMBOL_PARAMETER) {
      code_generator_load_variable(generator, name);
      if (generator->has_error) {
        break;
      }
    }

    symbol->data.variable.register_id = promoted_symbols[i].reg;
    symbol->data.variable.is_in_register = 1;

    if (is_parameter) {
      code_generator_store_variable(generator, name, "rax");
      if (generator->has_error) {
        break;
      }
    }
  }
  if (generator->has_error) {
    ir_temp_table_destroy(&temp_table);
    ir_local_table_destroy(&local_table);
    ir_symbol_stats_map_destroy(&symbol_stats);
    symbol_table_exit_scope(generator->symbol_table);
    return 0;
  }

  for (size_t i = 0; i < local_table.count; i++) {
    IRLocalSlot *local = &local_table.items[i];
    Symbol *symbol =
        symbol_table_lookup_current_scope(generator->symbol_table, local->name);
    if (!symbol || symbol->kind != SYMBOL_VARIABLE) {
      code_generator_set_error(
          generator, "Missing local symbol '%s' in IR backend", local->name);
      ir_temp_table_destroy(&temp_table);
      ir_local_table_destroy(&local_table);
      ir_symbol_stats_map_destroy(&symbol_stats);
      symbol_table_exit_scope(generator->symbol_table);
      return 0;
    }

    int promoted_index =
        ir_promoted_symbol_find(promoted_symbols, promoted_symbol_count, local->name);
    if (promoted_index >= 0) {
      symbol->data.variable.register_id = promoted_symbols[promoted_index].reg;
      symbol->data.variable.is_in_register = 1;
      symbol->data.variable.memory_offset = 0;
      continue;
    }

    int offset = code_generator_allocate_stack_space(generator, local->size,
                                                     local->alignment);
    symbol->data.variable.is_in_register = 0;
    symbol->data.variable.memory_offset = offset;

    if (generator->generate_debug_info) {
      code_generator_add_debug_symbol(
          generator, local->name, DEBUG_SYMBOL_VARIABLE,
          local->type_name ? local->type_name : "unknown", local->location.line,
          local->location.column);
      debug_info_set_symbol_stack_offset(generator->debug_info, local->name,
                                         -offset);
    }
  }

  for (size_t i = 0; i < temp_table.count; i++) {
    temp_table.items[i].offset =
        code_generator_allocate_stack_space(generator, 8, 8);
  }

  IRTempUseMap temp_use_map = {0};
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    if (!ir_temp_use_map_record_instruction(&temp_use_map,
                                            &ir_function->instructions[i])) {
      code_generator_set_error(generator,
                               "Out of memory while tracking IR temp uses");
      break;
    }
  }
  /* Expose the use map to the deferred-spill peephole, and start this
   * function with no inherited pending/cached state. */
  generator->current_temp_use_map = &temp_use_map;
  generator->pending_spill_offset = 0;
  generator->pending_spill_single_use = 0;
  generator->rax_cached_temp_offset = 0;

  if (!generator->has_error) {
    for (size_t i = 0; i < ir_function->instruction_count; i++) {
      const IRInstruction *instruction = &ir_function->instructions[i];
      if (generator->debug_info && instruction->location.line > 0 &&
          generator->generate_stack_trace_support) {
        code_generator_emit_runtime_location_marker(
            generator, instruction->location.line,
            instruction->location.column,
            code_generator_runtime_filename(generator,
                                            instruction->location.filename));
      }
      if (i + 1 < ir_function->instruction_count) {
        const IRInstruction *next = &ir_function->instructions[i + 1];
        if (instruction->op == IR_OP_BINARY &&
            instruction->dest.kind == IR_OPERAND_TEMP &&
            instruction->dest.name) {
          size_t return_index = i;
          int emitted_return = 0;
          if (!code_generator_try_emit_ir_binary_return_temp(
                  generator, ir_function, i, &temp_use_map, &temp_table,
                  &return_index, &emitted_return)) {
            break;
          }
          if (emitted_return) {
            i = return_index;
            continue;
          }

          size_t chain_last = i;
          int chain_consumed_branch = 0;
          int emitted_chain = 0;
          if (!code_generator_try_emit_ir_binary_expression_chain(
                  generator, ir_function, i, &temp_use_map, &temp_table,
                  &chain_last, &chain_consumed_branch, &emitted_chain)) {
            break;
          }
          if (emitted_chain) {
            i = chain_last + (chain_consumed_branch ? 1 : 0);
            continue;
          }
        }

        if (i + 2 < ir_function->instruction_count &&
            instruction->op == IR_OP_BINARY &&
            instruction->dest.kind == IR_OPERAND_TEMP &&
            instruction->dest.name && next->op == IR_OP_BINARY &&
            ir_function->instructions[i + 2].op == IR_OP_BRANCH_ZERO) {
          int emitted_chain = 0;
          if (!code_generator_try_emit_ir_binary_compare_branch_chain(
                  generator, instruction, next, &ir_function->instructions[i + 2],
                  &temp_use_map, &temp_table, &emitted_chain)) {
            break;
          }
          if (emitted_chain) {
            i += 2;
            continue;
          }
        }

        if (instruction->op == IR_OP_BINARY &&
            instruction->dest.kind == IR_OPERAND_TEMP &&
            instruction->dest.name && next->op == IR_OP_BINARY) {
          int emitted_pair = 0;
          if (!code_generator_try_emit_ir_binary_temp_pair(
                  generator, instruction, next, &temp_use_map, &temp_table,
                  &emitted_pair)) {
            break;
          }
          if (emitted_pair) {
            i++;
            continue;
          }
        }

        if (instruction->op == IR_OP_BINARY && !instruction->is_float &&
            instruction->dest.kind == IR_OPERAND_TEMP &&
            instruction->dest.name && ir_binary_operator_is_comparison(instruction->text) &&
            next->op == IR_OP_BRANCH_ZERO &&
            next->lhs.kind == IR_OPERAND_TEMP && next->lhs.name &&
            strcmp(next->lhs.name, instruction->dest.name) == 0 &&
            ir_temp_use_map_get(&temp_use_map, instruction->dest.name) == 1) {
          if (i + 3 < ir_function->instruction_count &&
              next->text &&
              ir_function->instructions[i + 2].op == IR_OP_JUMP &&
              ir_function->instructions[i + 2].text &&
              ir_function->instructions[i + 3].op == IR_OP_LABEL &&
              ir_function->instructions[i + 3].text &&
              strcmp(next->text, ir_function->instructions[i + 3].text) == 0) {
            const char *false_jump =
                ir_false_jump_for_comparison(instruction->text);
            const char *true_jump = ir_invert_conditional_jump(false_jump);
            if (true_jump) {
              if (!code_generator_emit_ir_compare_branch_with_jump(
                      generator, instruction, true_jump,
                      ir_function->instructions[i + 2].text, &temp_table)) {
                break;
              }
              i += 2;
              continue;
            }
          }

          if (!code_generator_emit_ir_compare_branch_zero(
                  generator, instruction, next, &temp_table)) {
            break;
          }
          i++;
          continue;
        }

        if (instruction->op == IR_OP_BRANCH_ZERO && instruction->text &&
            i + 2 < ir_function->instruction_count && next->text &&
            next->op == IR_OP_JUMP &&
            ir_function->instructions[i + 2].op == IR_OP_LABEL &&
            ir_function->instructions[i + 2].text &&
            strcmp(instruction->text, ir_function->instructions[i + 2].text) ==
                0) {
          if (!code_generator_emit_ir_branch_zero_jump_over_label(
                  generator, instruction, next, &temp_table)) {
            break;
          }
          i++;
          continue;
        }
      }

      if (!code_generator_emit_ir_instruction(generator, instruction,
                                              &temp_table)) {
        break;
      }
    }
  }

  code_generator_emit(generator, "L%s_exit:\n", function_data->name);
  for (size_t i = promoted_symbol_count; i > 0; i--) {
    size_t index = i - 1;
    const char *reg_name = code_generator_get_register_name(promoted_symbols[index].reg);
    if (!reg_name) {
      code_generator_set_error(generator,
                               "Invalid promoted register in function '%s'",
                               function_data->name);
      break;
    }
    code_generator_emit(generator, "    mov %s, [rbp - %d]\n", reg_name,
                        promoted_symbols[index].save_offset);
  }
  code_generator_function_epilogue(generator, fn_return_type);
  if (runtime_end_label) {
    code_generator_emit(generator, "%s:\n", runtime_end_label);
  }

  /* No spill may outlive the function. The epilogue's own emits will have
   * flushed any pending store; clear the borrowed map pointer before it dies
   * and drop any stale cache so the next function starts clean. */
  generator->current_temp_use_map = NULL;
  generator->pending_spill_offset = 0;
  generator->pending_spill_single_use = 0;
  generator->rax_cached_temp_offset = 0;
  generator->current_fn_returns_indirect = 0;
  generator->current_fn_indirect_return_size = 0;
  code_generator_ir_reset_indirect_return_slots(generator);
  ir_indirect_temp_table_reset();

  ir_temp_use_map_destroy(&temp_use_map);
  ir_temp_table_destroy(&temp_table);
  ir_local_table_destroy(&local_table);
  ir_symbol_stats_map_destroy(&symbol_stats);
  symbol_table_exit_scope(generator->symbol_table);
  free(runtime_end_label);

  return generator->has_error ? 0 : 1;
}
