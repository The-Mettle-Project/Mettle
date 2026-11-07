#include "ir_values.h"

#include "../common.h"
#include "../compiler/compiler_crash.h"
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_VALUE_TABLE_MIN_BUCKETS 64u

static uint64_t ir_value_hash(unsigned char kind, const char *name) {
  uint64_t hash = 1469598103934665603ull;
  hash ^= (uint64_t)kind;
  hash *= 1099511628211ull;
  if (name) {
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
      hash ^= (uint64_t)*p;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

void ir_value_table_init(IRValueTable *table) {
  if (!table) {
    return;
  }
  table->entries = NULL;
  table->count = 0;
  table->capacity = 0;
  table->buckets = NULL;
  table->bucket_count = 0;
}

void ir_value_table_clear(IRValueTable *table) {
  if (!table) {
    return;
  }
  for (size_t i = 0; i < table->count; i++) {
    mettle_free_string(table->entries[i].name);
  }
  free(table->entries);
  free(table->buckets);
  ir_value_table_init(table);
}

static int ir_value_table_rehash(IRValueTable *table, size_t bucket_count) {
  if (bucket_count < IR_VALUE_TABLE_MIN_BUCKETS) {
    bucket_count = IR_VALUE_TABLE_MIN_BUCKETS;
  }
  uint32_t *buckets = (uint32_t *)calloc(bucket_count, sizeof(uint32_t));
  if (!buckets) {
    return 0;
  }
  const size_t mask = bucket_count - 1;
  for (size_t i = 0; i < table->count; i++) {
    const IRValueEntry *entry = &table->entries[i];
    size_t slot = (size_t)(ir_value_hash(entry->kind, entry->name) & mask);
    while (buckets[slot]) {
      slot = (slot + 1) & mask;
    }
    buckets[slot] = (uint32_t)(i + 1);
  }
  free(table->buckets);
  table->buckets = buckets;
  table->bucket_count = bucket_count;
  return 1;
}

uint32_t ir_value_table_lookup(const IRValueTable *table, unsigned char kind,
                               const char *name) {
  if (!table || !name || table->bucket_count == 0) {
    return IR_VALUE_ID_NONE;
  }
  const size_t mask = table->bucket_count - 1;
  size_t slot = (size_t)(ir_value_hash(kind, name) & mask);
  while (table->buckets[slot]) {
    const uint32_t id = table->buckets[slot];
    const IRValueEntry *entry = &table->entries[id - 1];
    if (entry->kind == kind && entry->name &&
        (entry->name == name || strcmp(entry->name, name) == 0)) {
      return id;
    }
    slot = (slot + 1) & mask;
  }
  return IR_VALUE_ID_NONE;
}

static uint32_t ir_value_table_add(IRValueTable *table, unsigned char kind,
                                   const char *name, uint32_t version,
                                   uint32_t base_id) {
  if (table->count >= table->capacity) {
    size_t capacity = table->capacity ? table->capacity * 2 : 64;
    IRValueEntry *entries =
        (IRValueEntry *)realloc(table->entries, capacity * sizeof(IRValueEntry));
    if (!entries) {
      return IR_VALUE_ID_NONE;
    }
    table->entries = entries;
    table->capacity = capacity;
  }

  char *stored = ir_name_share(name);
  if (!stored) {
    return IR_VALUE_ID_NONE;
  }

  IRValueEntry *entry = &table->entries[table->count];
  entry->name = stored;
  entry->kind = kind;
  entry->version = version;
  entry->base_id = base_id;
  table->count++;

  const uint32_t id = (uint32_t)table->count;
  if (table->bucket_count == 0 ||
      (table->count * 2) >= table->bucket_count) {
    size_t bucket_count =
        table->bucket_count ? table->bucket_count * 2 : IR_VALUE_TABLE_MIN_BUCKETS;
    if (!ir_value_table_rehash(table, bucket_count)) {
      table->count--;
      mettle_free_string(stored);
      return IR_VALUE_ID_NONE;
    }
    return id;
  }

  const size_t mask = table->bucket_count - 1;
  size_t slot = (size_t)(ir_value_hash(kind, name) & mask);
  while (table->buckets[slot]) {
    slot = (slot + 1) & mask;
  }
  table->buckets[slot] = id;
  return id;
}

uint32_t ir_value_table_intern(IRValueTable *table, unsigned char kind,
                               const char *name) {
  if (!table || !name) {
    return IR_VALUE_ID_NONE;
  }
  const uint32_t existing = ir_value_table_lookup(table, kind, name);
  if (existing != IR_VALUE_ID_NONE) {
    return existing;
  }
  return ir_value_table_add(table, kind, name, 0, IR_VALUE_ID_NONE);
}

uint32_t ir_value_table_intern_version(IRValueTable *table, uint32_t base_id,
                                       const char *name) {
  if (!table || !name || base_id == IR_VALUE_ID_NONE ||
      base_id > table->count) {
    return IR_VALUE_ID_NONE;
  }
  const unsigned char kind = table->entries[base_id - 1].kind;
  const uint32_t existing = ir_value_table_lookup(table, kind, name);
  if (existing != IR_VALUE_ID_NONE) {
    return existing;
  }
  const uint32_t root = table->entries[base_id - 1].base_id != IR_VALUE_ID_NONE
                            ? table->entries[base_id - 1].base_id
                            : base_id;
  uint32_t version = 1;
  for (size_t i = 0; i < table->count; i++) {
    if (table->entries[i].base_id == root &&
        table->entries[i].version >= version) {
      version = table->entries[i].version + 1;
    }
  }
  return ir_value_table_add(table, kind, name, version, root);
}

const char *ir_value_table_name(const IRValueTable *table, uint32_t id) {
  if (!table || id == IR_VALUE_ID_NONE || id > table->count) {
    return NULL;
  }
  return table->entries[id - 1].name;
}

unsigned char ir_value_table_kind(const IRValueTable *table, uint32_t id) {
  if (!table || id == IR_VALUE_ID_NONE || id > table->count) {
    return 0;
  }
  return table->entries[id - 1].kind;
}

uint32_t ir_value_table_version(const IRValueTable *table, uint32_t id) {
  if (!table || id == IR_VALUE_ID_NONE || id > table->count) {
    return 0;
  }
  return table->entries[id - 1].version;
}

uint32_t ir_value_table_base(const IRValueTable *table, uint32_t id) {
  if (!table || id == IR_VALUE_ID_NONE || id > table->count) {
    return IR_VALUE_ID_NONE;
  }
  return table->entries[id - 1].base_id;
}

size_t ir_value_table_count(const IRValueTable *table) {
  return table ? table->count : 0;
}

typedef enum {
  IR_VALUE_MODE_UNSET = 0,
  IR_VALUE_MODE_OFF,
  IR_VALUE_MODE_CHECK,
  IR_VALUE_MODE_STRICT
} IRValueCheckMode;

static IRValueCheckMode g_value_check_mode = IR_VALUE_MODE_UNSET;
static size_t g_value_stale_reports = 0;
static size_t g_value_unnumbered_total = 0;

static IRValueCheckMode ir_value_check_mode(void) {
  if (g_value_check_mode != IR_VALUE_MODE_UNSET) {
    return g_value_check_mode;
  }
  const char *setting = getenv("METTLE_VALUE_IDS");
  if (!setting || !*setting) {
    g_value_check_mode = IR_VALUE_MODE_OFF;
  } else if (strcmp(setting, "strict") == 0) {
    g_value_check_mode = IR_VALUE_MODE_STRICT;
  } else if (strcmp(setting, "check") == 0) {
    g_value_check_mode = IR_VALUE_MODE_CHECK;
  } else {
    g_value_check_mode = IR_VALUE_MODE_OFF;
  }
  return g_value_check_mode;
}

size_t ir_value_stale_report_count(void) { return g_value_stale_reports; }

size_t ir_value_unnumbered_count(void) { return g_value_unnumbered_total; }

static int g_value_sabotage_fired = 0;

void ir_value_maybe_sabotage(IRFunction *function, const char *pass_name) {
  if (g_value_sabotage_fired || !function || !pass_name) {
    return;
  }
  static MettleEnvOnce break_spec;
  const char *spec = mettle_env_once(&break_spec, "METTLE_VALUE_IDS_BREAK");
  if (!spec || !*spec) {
    return;
  }
  if (strcmp(spec, "*") != 0 && strcmp(spec, pass_name) != 0) {
    return;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IROperand *dest = &function->instructions[i].dest;
    if (dest->kind != IR_OPERAND_TEMP || !dest->name ||
        dest->value_id == IR_VALUE_ID_NONE) {
      continue;
    }
    const size_t length = strlen(dest->name);
    char *renamed = (char *)malloc(length + 9);
    if (!renamed) {
      return;
    }
    memcpy(renamed, dest->name, length);
    memcpy(renamed + length, "_sabotage", 10);
    mettle_free_string(dest->name);
    dest->name = renamed;
    g_value_sabotage_fired = 1;
    return;
  }
}

void ir_value_check_after_pass(const IRFunction *function,
                               const char *pass_name) {
  const IRValueCheckMode mode = ir_value_check_mode();
  if (mode == IR_VALUE_MODE_OFF) {
    return;
  }
  char why[512];
  why[0] = '\0';
  size_t unnumbered = 0;
  const int ok =
      ir_function_values_audit(function, why, sizeof(why), &unnumbered);
  g_value_unnumbered_total += unnumbered;
  if (ok) {
    return;
  }
  g_value_stale_reports++;
  fprintf(stderr,
          "mettle: value id disagrees with its name after pass '%s' in '%s': "
          "%s\n",
          pass_name ? pass_name : "<unnamed>",
          (function && function->name) ? function->name : "<unnamed>", why);
  if (mode == IR_VALUE_MODE_STRICT) {
    char message[768];
    snprintf(message, sizeof(message),
             "optimization pass '%s' left a value id naming a different value "
             "in '%s': %s",
             pass_name ? pass_name : "<unnamed>",
             (function && function->name) ? function->name : "<unnamed>", why);
    mettle_compiler_ice(message);
  }
}

static const int IR_WRITES_DESTINATION[IR_OP_KIND_COUNT] = {
    [IR_OP_ASSIGN] = 1,
    [IR_OP_ADDRESS_OF] = 1,
    [IR_OP_LOAD] = 1,
    [IR_OP_BINARY] = 1,
    [IR_OP_UNARY] = 1,
    [IR_OP_ROTATE_ADD] = 1,
    [IR_OP_CALL] = 1,
    [IR_OP_CALL_INDIRECT] = 1,
    [IR_OP_NEW] = 1,
    [IR_OP_CAST] = 1,
    [IR_OP_COUNT_WORD_STARTS] = 1,
    [IR_OP_MEMCPY_INLINE] = 1,
    [IR_OP_SIMD_FILL] = 1,
    [IR_OP_SIMD_COPY] = 1,
    [IR_OP_SIMD_SUM_I32] = 1,
    [IR_OP_SIMD_SUM_U8] = 1,
    [IR_OP_SIMD_MATMUL_N32] = 1,
    [IR_OP_SIMD_INSERTION_SORT_I32] = 1,
    [IR_OP_SIMD_DOT_I32] = 1,
    [IR_OP_SIMD_DOT_I8] = 1,
    [IR_OP_SIMD_SLP_MAC_I32] = 1,
    [IR_OP_SIMD_SLP_MAC_I8] = 1,
    [IR_OP_SIMD_SCALE_I32] = 1,
    [IR_OP_SIMD_CLAMP_I32] = 1,
    [IR_OP_SIMD_REVERSE_COPY_I32] = 1,
    [IR_OP_LOWER_BOUND_I32] = 1,
    [IR_OP_PREFIX_SUM_I32] = 1,
    [IR_OP_SIMD_MINMAX_I32] = 1,
    [IR_OP_SIMD_SUM_F64] = 1,
    [IR_OP_SIMD_SUM_F32] = 1,
    [IR_OP_SIMD_DOT_F64] = 1,
    [IR_OP_SIMD_DOT_F32] = 1,
    [IR_OP_SIMD_AFFINE_MAP_F64] = 1,
    [IR_OP_SIMD_AFFINE_MAP_F32] = 1,
    [IR_OP_SIMD_EXP_F32] = 1,
    [IR_OP_SIMD_SILU_F32] = 1,
    [IR_OP_SIMD_I2F_REDUCE_F64] = 1,
    [IR_OP_SIMD_VLOOP_F64] = 1,
    [IR_OP_SIMD_VLOOP_I32] = 1,
    [IR_OP_SIMD_FIND] = 1,
    [IR_OP_SIMD_OUTER_LANE_F64] = 1,
    [IR_OP_SELECT] = 1,
    [IR_OP_SIMD_LCG_U32] = 1,
    [IR_OP_SIMD_LANES] = 1,
    [IR_OP_ADDRESS_SPACE_ALLOC] = 1,
    [IR_OP_PHI] = 1,
    [IR_OP_ASM_RESULT] = 1,
};

int ir_instruction_writes_destination(const IRInstruction *instruction) {
  if (!instruction || instruction->dest.kind == IR_OPERAND_NONE) {
    return 0;
  }

  return (unsigned)instruction->op < (unsigned)IR_OP_KIND_COUNT
             ? IR_WRITES_DESTINATION[instruction->op]
             : 0;
}

static void ir_fingerprint_mix(uint64_t *hash, const char *text) {
  *hash ^= 0x9e3779b97f4a7c15ull;
  *hash *= 1099511628211ull;
  if (!text) {
    return;
  }
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    *hash ^= (uint64_t)*p;
    *hash *= 1099511628211ull;
  }
}

uint64_t ir_function_fingerprint(const IRFunction *function) {
  if (!function) {
    return 0;
  }
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    hash ^= (uint64_t)in->op;
    hash *= 1099511628211ull;
    ir_fingerprint_mix(&hash, in->dest.name);
    ir_fingerprint_mix(&hash, in->lhs.name);
    ir_fingerprint_mix(&hash, in->rhs.name);
    ir_fingerprint_mix(&hash, in->text);
    for (size_t j = 0; j < in->argument_count; j++) {
      if (in->arguments) {
        ir_fingerprint_mix(&hash, in->arguments[j].name);
      }
    }
  }
  return hash;
}

uint64_t ir_function_structure_fingerprint(const IRFunction *function) {
  if (!function) {
    return 0;
  }
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_LABEL && in->op != IR_OP_JUMP &&
        in->op != IR_OP_BRANCH_ZERO && in->op != IR_OP_BRANCH_EQ &&
        in->op != IR_OP_RETURN) {
      continue;
    }
    hash ^= (uint64_t)in->op;
    hash *= 1099511628211ull;
    ir_fingerprint_mix(&hash, in->text);
  }
  return hash;
}

static size_t g_silent_mutations = 0;
static size_t g_silent_structure = 0;

size_t ir_silent_structure_count(void) { return g_silent_structure; }

void ir_check_silent_structure(const IRFunction *function,
                               const char *pass_name, uint64_t before,
                               uint64_t structure_before) {
  static MettleEnvOnce audit;
  if (!mettle_env_once(&audit, "METTLE_CFG_AUDIT") || !function) {
    return;
  }
  const uint64_t after = ir_function_structure_fingerprint(function);
  if (after == before) {
    return;
  }
  if (function->structure_generation != structure_before) {
    return;
  }
  g_silent_structure++;
  fprintf(stderr,
          "mettle: pass '%s' changed the control flow of '%s' without "
          "clearing the graph\n",
          pass_name ? pass_name : "<unnamed>",
          function->name ? function->name : "<unnamed>");
}


size_t ir_silent_mutation_count(void) { return g_silent_mutations; }

void ir_check_silent_mutation(const IRFunction *function, const char *pass_name,
                              uint64_t before, uint64_t generation_before) {
  static MettleEnvOnce audit;
  if (!mettle_env_once(&audit, "METTLE_CACHE_AUDIT") || !function) {
    return;
  }
  const uint64_t after = ir_function_fingerprint(function);
  if (after == before) {
    return;
  }
  if (function->generation != generation_before) {
    return;
  }
  g_silent_mutations++;
  fprintf(stderr,
          "mettle: pass '%s' changed '%s' without invalidating the analysis\n",
          pass_name ? pass_name : "<unnamed>",
          function->name ? function->name : "<unnamed>");
}

int ir_operand_names_match(const IROperand *a, const IROperand *b) {
  if (!a || !b || !a->name || !b->name) {
    return 0;
  }
  if (a->kind == b->kind && a->value_id != IR_VALUE_ID_NONE &&
      b->value_id != IR_VALUE_ID_NONE) {
    return a->value_id == b->value_id;
  }
  return strcmp(a->name, b->name) == 0;
}

int ir_operand_is_temp(const IROperand *operand) {
  return operand && operand->kind == IR_OPERAND_TEMP && operand->name != NULL;
}

int ir_operand_is_symbol(const IROperand *operand) {
  return operand && operand->kind == IR_OPERAND_SYMBOL && operand->name != NULL;
}

int ir_operand_is_value(const IROperand *operand) {
  if (!operand || !operand->name) {
    return 0;
  }
  return operand->kind == IR_OPERAND_TEMP || operand->kind == IR_OPERAND_SYMBOL;
}

uint32_t ir_function_value_id(IRFunction *function, const IROperand *operand) {
  if (!function || !ir_operand_is_value(operand)) {
    return IR_VALUE_ID_NONE;
  }
  return ir_value_table_intern(&function->values, (unsigned char)operand->kind,
                               operand->name);
}

const char *ir_function_value_name(const IRFunction *function, uint32_t id) {
  if (!function) {
    return NULL;
  }
  return ir_value_table_name(&function->values, id);
}

void ir_function_touch(IRFunction *function) {
  if (function) {
    function->generation++;
  }
}

void ir_function_touch_structure(IRFunction *function) {
  if (function) {
    function->generation++;
    function->structure_generation++;
  }
}

static IROperand *ir_instruction_operand_at(IRInstruction *instruction,
                                            size_t index) {
  if (!instruction) {
    return NULL;
  }
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

static size_t ir_instruction_operand_count(const IRInstruction *instruction) {
  return instruction ? 3 + instruction->argument_count : 0;
}

int ir_function_number_values(IRFunction *function) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    const size_t operands = ir_instruction_operand_count(instruction);
    for (size_t j = 0; j < operands; j++) {
      IROperand *operand = ir_instruction_operand_at(instruction, j);
      if (!operand) {
        continue;
      }
      if (!ir_operand_is_value(operand)) {
        operand->value_id = IR_VALUE_ID_NONE;
        continue;
      }
      if (operand->value_id != IR_VALUE_ID_NONE &&
          operand->value_id <= function->values.count) {
        const IRValueEntry *carried =
            &function->values.entries[operand->value_id - 1];
        if (carried->kind == (unsigned char)operand->kind && carried->name &&
            (carried->name == operand->name ||
             strcmp(carried->name, operand->name) == 0)) {
          continue;
        }
      }
      const uint32_t id = ir_value_table_intern(
          &function->values, (unsigned char)operand->kind, operand->name);
      if (id == IR_VALUE_ID_NONE) {
        return 0;
      }
      operand->value_id = id;
    }
  }
  return 1;
}

int ir_function_number_values_if_stale(IRFunction *function) {
  if (!function) {
    return 0;
  }
  if (function->numbered_generation == function->generation &&
      function->numbered_instruction_count == function->instruction_count &&
      function->generation != 0) {
    return 1;
  }
  if (!ir_function_number_values(function)) {
    return 0;
  }
  function->numbered_generation = function->generation;
  function->numbered_instruction_count = function->instruction_count;
  return 1;
}

int ir_function_values_agree(const IRFunction *function, char *why,
                             size_t why_capacity) {
  size_t unnumbered = 0;
  return ir_function_values_audit(function, why, why_capacity, &unnumbered);
}

int ir_function_values_audit(const IRFunction *function, char *why,
                             size_t why_capacity, size_t *unnumbered_out) {
  size_t unnumbered = 0;
  int ok = 1;
  if (!function) {
    if (unnumbered_out) {
      *unnumbered_out = 0;
    }
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    const size_t operands = ir_instruction_operand_count(instruction);
    for (size_t j = 0; j < operands; j++) {
      IROperand *operand = ir_instruction_operand_at(instruction, j);
      if (!operand) {
        continue;
      }
      if (!ir_operand_is_value(operand)) {
        if (operand->value_id != IR_VALUE_ID_NONE) {
          if (ok && why && why_capacity) {
            snprintf(why, why_capacity,
                     "instruction %zu operand %zu carries value id %u but no "
                     "longer names a value",
                     i, j, operand->value_id);
          }
          ok = 0;
        }
        continue;
      }
      if (operand->value_id == IR_VALUE_ID_NONE) {
        unnumbered++;
        continue;
      }
      const char *carried =
          ir_value_table_name(&function->values, operand->value_id);
      if (!carried) {
        if (ok && why && why_capacity) {
          snprintf(why, why_capacity,
                   "instruction %zu operand %zu names '%s' and carries id %u "
                   "which is not in the value table",
                   i, j, operand->name, operand->value_id);
        }
        ok = 0;
        continue;
      }
      if (ir_value_table_kind(&function->values, operand->value_id) !=
              (unsigned char)operand->kind ||
          strcmp(carried, operand->name) != 0) {
        if (ok && why && why_capacity) {
          snprintf(why, why_capacity,
                   "instruction %zu operand %zu names '%s' but carries id %u "
                   "which is '%s'",
                   i, j, operand->name, operand->value_id, carried);
        }
        ok = 0;
      }
    }
  }
  if (unnumbered_out) {
    *unnumbered_out = unnumbered;
  }
  return ok;
}
