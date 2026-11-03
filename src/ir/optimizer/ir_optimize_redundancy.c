
#include "ir_optimize_internal.h"

#define RE_MAX_ADDR_DEPTH 8
#define RE_KEY_MAX 224
#define RE_NAME_MAX 128

typedef struct {
  char **keys;
  long long *values;
  size_t capacity;
  size_t count;
} REMap;

static unsigned long long re_hash(const char *text) {
  unsigned long long h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    h ^= (unsigned long long)*p;
    h *= 1099511628211ULL;
  }
  return h;
}

static void re_map_destroy(REMap *map) {
  if (!map || !map->keys) {
    return;
  }
  for (size_t i = 0; i < map->capacity; i++) {
    free(map->keys[i]);
  }
  free(map->keys);
  free(map->values);
  map->keys = NULL;
  map->values = NULL;
  map->capacity = 0;
  map->count = 0;
}

static int re_map_grow(REMap *map, size_t capacity);

static long long *re_map_slot(REMap *map, const char *key, int create) {
  if (!map || !key) {
    return NULL;
  }
  if (map->capacity == 0 && (!create || !re_map_grow(map, 64))) {
    return NULL;
  }
  if (create && (map->count + 1) * 2 >= map->capacity &&
      !re_map_grow(map, map->capacity * 2)) {
    return NULL;
  }

  size_t mask = map->capacity - 1;
  size_t slot = (size_t)re_hash(key) & mask;
  for (;;) {
    if (!map->keys[slot]) {
      if (!create) {
        return NULL;
      }
      map->keys[slot] = mettle_strdup(key);
      if (!map->keys[slot]) {
        return NULL;
      }
      map->values[slot] = 0;
      map->count++;
      return &map->values[slot];
    }
    if (map->keys[slot][0] == key[0] && strcmp(map->keys[slot], key) == 0) {
      return &map->values[slot];
    }
    slot = (slot + 1) & mask;
  }
}

static int re_map_grow(REMap *map, size_t capacity) {
  REMap grown = {0};
  if (capacity < 64) {
    capacity = 64;
  }
  grown.keys = calloc(capacity, sizeof(char *));
  grown.values = calloc(capacity, sizeof(long long));
  if (!grown.keys || !grown.values) {
    free(grown.keys);
    free(grown.values);
    return 0;
  }
  grown.capacity = capacity;

  for (size_t i = 0; i < map->capacity; i++) {
    if (!map->keys[i]) {
      continue;
    }
    size_t mask = capacity - 1;
    size_t slot = (size_t)re_hash(map->keys[i]) & mask;
    while (grown.keys[slot]) {
      slot = (slot + 1) & mask;
    }
    grown.keys[slot] = map->keys[i];
    grown.values[slot] = map->values[i];
    grown.count++;
    map->keys[i] = NULL;
  }
  re_map_destroy(map);
  *map = grown;
  return 1;
}

static long long re_map_get(const REMap *map, const char *key) {
  long long *slot = re_map_slot((REMap *)map, key, 0);
  return slot ? *slot : 0;
}

static int re_map_add(REMap *map, const char *key, long long delta) {
  long long *slot = re_map_slot(map, key, 1);
  if (!slot) {
    return 0;
  }
  *slot += delta;
  return 1;
}

static int re_map_set(REMap *map, const char *key, long long value) {
  long long *slot = re_map_slot(map, key, 1);
  if (!slot) {
    return 0;
  }
  *slot = value;
  return 1;
}

static int re_name_key(char *out, size_t size, IROperandKind kind,
                       const char *name) {
  if (!name) {
    return 0;
  }
  char tag = (kind == IR_OPERAND_SYMBOL) ? 's' : 't';
  int written = snprintf(out, size, "%c%s", tag, name);
  return written > 0 && (size_t)written < size;
}

typedef struct {
  REMap defs;
  REMap def_at;
  const IRFunction *function;
  const IRTempValueMap *addr_taken;
} REDefs;

static int re_note_def(REDefs *defs, const IROperand *operand, size_t index) {
  char key[RE_NAME_MAX];
  if (!operand ||
      (operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL) ||
      !re_name_key(key, sizeof(key), operand->kind, operand->name)) {
    return 1;
  }
  if (!re_map_add(&defs->defs, key, 1)) {
    return 0;
  }
  return re_map_set(&defs->def_at, key, (long long)index + 1);
}

static int re_opcode_is_scalar(IROpcode op) {
  switch (op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_JUMP:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_RETURN:
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_STORE:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
    return 1;
  default:
    return 0;
  }
}

static int re_opcode_writes_through_arguments(IROpcode op) {
  return op != IR_OP_CALL && op != IR_OP_CALL_INDIRECT;
}

static int re_collect_defs(const IRFunction *function, REDefs *defs) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ir_instruction_writes_destination(ins) &&
        !re_note_def(defs, &ins->dest, i)) {
      return 0;
    }
    if (re_opcode_is_scalar(ins->op) && ins->op != IR_OP_ROTATE_ADD) {
      continue;
    }
    if (!re_opcode_writes_through_arguments(ins->op)) {
      continue;
    }
    if (!re_note_def(defs, &ins->lhs, i) || !re_note_def(defs, &ins->rhs, i)) {
      return 0;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ins->arguments[a].kind == IR_OPERAND_SYMBOL &&
          !re_note_def(defs, &ins->arguments[a], i)) {
        return 0;
      }
    }
  }
  return 1;
}

static long long re_def_count(const REDefs *defs, IROperandKind kind,
                              const char *name) {
  char key[RE_NAME_MAX];
  if (!re_name_key(key, sizeof(key), kind, name)) {
    return 2;
  }
  return re_map_get(&defs->defs, key);
}

static int re_symbol_is_aliasable(const REDefs *defs, const char *name) {
  if (!name) {
    return 1;
  }
  if (ir_temp_value_map_lookup(defs->addr_taken, name)) {
    return 1;
  }
  return !ir_function_symbol_is_parameter(defs->function, name) &&
         ir_function_local_declared_type(defs->function, name) == NULL;
}

static int re_name_is_stable(const REDefs *defs, IROperandKind kind,
                             const char *name) {
  if (!name || re_def_count(defs, kind, name) != 0) {
    return 0;
  }
  if (kind == IR_OPERAND_SYMBOL) {
    return !re_symbol_is_aliasable(defs, name);
  }
  return kind == IR_OPERAND_TEMP;
}

static const IRInstruction *re_unique_def(const IRFunction *function,
                                          const REDefs *defs,
                                          IROperandKind kind,
                                          const char *name) {
  char key[RE_NAME_MAX];
  if (!re_name_key(key, sizeof(key), kind, name)) {
    return NULL;
  }
  if (re_map_get(&defs->defs, key) != 1) {
    return NULL;
  }
  long long at = re_map_get(&defs->def_at, key);
  if (at <= 0 || (size_t)(at - 1) >= function->instruction_count) {
    return NULL;
  }
  return &function->instructions[at - 1];
}

typedef struct {
  int is_address_of;
  IROperandKind kind;
  const char *name;
  long long offset;
  int portable;
  int aliasable;
  int valid;
} REAddr;

static void re_resolve_addr(const IRFunction *function, const REDefs *defs,
                            const IROperand *operand, REAddr *out, int depth) {
  out->valid = 0;
  if (!operand || depth > RE_MAX_ADDR_DEPTH) {
    return;
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    const IRInstruction *def =
        re_unique_def(function, defs, IR_OPERAND_SYMBOL, operand->name);
    if (def && !re_symbol_is_aliasable(defs, operand->name)) {
      if (def->op == IR_OP_ASSIGN && (def->lhs.kind == IR_OPERAND_TEMP ||
                                      def->lhs.kind == IR_OPERAND_SYMBOL)) {
        re_resolve_addr(function, defs, &def->lhs, out, depth + 1);
        if (out->valid) {
          return;
        }
      }
      if (def->op == IR_OP_ADDRESS_OF && def->lhs.kind == IR_OPERAND_SYMBOL &&
          def->lhs.name) {
        out->valid = 1;
        out->is_address_of = 1;
        out->kind = IR_OPERAND_SYMBOL;
        out->name = def->lhs.name;
        out->offset = 0;
        out->portable = 1;
        out->aliasable = 0;
        return;
      }
    }
    out->valid = 1;
    out->is_address_of = 0;
    out->kind = IR_OPERAND_SYMBOL;
    out->name = operand->name;
    out->offset = 0;
    out->portable = re_name_is_stable(defs, IR_OPERAND_SYMBOL, operand->name);
    out->aliasable = re_symbol_is_aliasable(defs, operand->name);
    return;
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return;
  }

  const IRInstruction *def =
      re_unique_def(function, defs, IR_OPERAND_TEMP, operand->name);
  if (def) {
    if (def->op == IR_OP_ADDRESS_OF && def->lhs.kind == IR_OPERAND_SYMBOL &&
        def->lhs.name) {
      out->valid = 1;
      out->is_address_of = 1;
      out->kind = IR_OPERAND_SYMBOL;
      out->name = def->lhs.name;
      out->offset = 0;
      out->portable = 1;
      out->aliasable = 0;
      return;
    }
    if (def->op == IR_OP_ASSIGN &&
        (def->lhs.kind == IR_OPERAND_TEMP ||
         def->lhs.kind == IR_OPERAND_SYMBOL)) {
      re_resolve_addr(function, defs, &def->lhs, out, depth + 1);
      return;
    }
    if (def->op == IR_OP_BINARY && !def->is_float && def->text &&
        strcmp(def->text, "+") == 0) {
      if (def->rhs.kind == IR_OPERAND_INT) {
        re_resolve_addr(function, defs, &def->lhs, out, depth + 1);
        if (out->valid) {
          out->offset += def->rhs.int_value;
        }
        return;
      }
      if (def->lhs.kind == IR_OPERAND_INT) {
        re_resolve_addr(function, defs, &def->rhs, out, depth + 1);
        if (out->valid) {
          out->offset += def->lhs.int_value;
        }
        return;
      }
    }
  }

  out->valid = 1;
  out->is_address_of = 0;
  out->kind = IR_OPERAND_TEMP;
  out->name = operand->name;
  out->offset = 0;
  out->portable = re_name_is_stable(defs, IR_OPERAND_TEMP, operand->name);
  out->aliasable = 0;
}

static int re_operand_key(char *out, size_t size, const IROperand *operand) {
  int written;
  switch (operand->kind) {
  case IR_OPERAND_TEMP:
    written = snprintf(out, size, "t:%s", operand->name ? operand->name : "");
    break;
  case IR_OPERAND_SYMBOL:
    written = snprintf(out, size, "s:%s", operand->name ? operand->name : "");
    break;
  case IR_OPERAND_INT:
    written = snprintf(out, size, "i:%lld", operand->int_value);
    break;
  case IR_OPERAND_FLOAT: {
    double value = operand->float_value;
    unsigned long long bits;
    memcpy(&bits, &value, sizeof(bits));
    written = snprintf(out, size, "f:%d:%llu", operand->float_bits, bits);
    break;
  }
  case IR_OPERAND_STRING:
    written = snprintf(out, size, "S:%s", operand->name ? operand->name : "");
    break;
  case IR_OPERAND_NONE:
    written = snprintf(out, size, "n");
    break;
  default:
    return 0;
  }
  return written > 0 && (size_t)written < size;
}

static int re_operand_portable(const REDefs *defs, const IROperand *operand) {
  switch (operand->kind) {
  case IR_OPERAND_INT:
  case IR_OPERAND_FLOAT:
  case IR_OPERAND_STRING:
  case IR_OPERAND_NONE:
    return 1;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return re_name_is_stable(defs, operand->kind, operand->name);
  default:
    return 0;
  }
}

static int re_operand_aliasable(const REDefs *defs, const IROperand *operand) {
  return operand->kind == IR_OPERAND_SYMBOL &&
         re_symbol_is_aliasable(defs, operand->name);
}

static const char *re_dep_name(const IROperand *operand) {
  if ((operand->kind == IR_OPERAND_TEMP ||
       operand->kind == IR_OPERAND_SYMBOL) &&
      operand->name) {
    return operand->name;
  }
  return NULL;
}

#define RE_MEM_WHOLE (1LL << 40)

typedef struct {
  char *base;
  long long off;
  long long size;
  unsigned char alias_class;
} REMemRegion;

typedef struct {
  REMemRegion *items;
  size_t count;
  size_t capacity;
} REKillLog;

static void re_kills_destroy(REKillLog *log) {
  for (size_t i = 0; i < log->count; i++) {
    free(log->items[i].base);
  }
  free(log->items);
  log->items = NULL;
  log->count = 0;
  log->capacity = 0;
}

static int re_kills_append(REKillLog *log, const char *base, long long off,
                           unsigned alias_class, long long size) {
  if (log->count == log->capacity) {
    size_t capacity = log->capacity ? log->capacity * 2 : 16;
    REMemRegion *grown = realloc(log->items, capacity * sizeof(REMemRegion));
    if (!grown) {
      return 0;
    }
    log->items = grown;
    log->capacity = capacity;
  }
  REMemRegion *slot = &log->items[log->count];
  slot->base = base ? mettle_strdup(base) : NULL;
  if (base && !slot->base) {
    return 0;
  }
  slot->off = off;
  slot->size = size;
  slot->alias_class = (unsigned char)alias_class;
  log->count++;
  return 1;
}

static int re_kill_hits(const IRFunction *function, const REMemRegion *kill,
                        const char *mem_base, long long mem_off,
                        long long mem_size, unsigned mem_class) {
  if (kill->base && mem_base && strcmp(kill->base, mem_base) == 0) {
    return kill->off < mem_off + mem_size && mem_off < kill->off + kill->size;
  }
  if (ir_alias_classes_distinct(kill->alias_class, mem_class)) {
    return 0;
  }
  if (!kill->base || !mem_base) {
    return 1;
  }
  if (kill->base[0] == '&' && mem_base[0] == '&') {
    return 0;
  }
  if (ir_alias_bases_distinct(function, kill->base, mem_base)) {
    return 0;
  }
  return 1;
}

typedef struct {
  char *key;
  char *value;
  char *dep[2];
  char *mem_base;
  long long mem_off;
  long long mem_size;
  unsigned char mem_class;
  size_t kill_pos;
  unsigned merge_epoch;
  int survives_summary;
  int volatile_value;
} REEntry;

static void re_entry_release(REEntry *entry) {
  free(entry->key);
  free(entry->value);
  free(entry->dep[0]);
  free(entry->dep[1]);
  free(entry->mem_base);
  entry->key = NULL;
  entry->value = NULL;
  entry->dep[0] = NULL;
  entry->dep[1] = NULL;
  entry->mem_base = NULL;
}

typedef struct {
  REEntry *items;
  size_t count;
  size_t capacity;
} RETable;

static void re_table_destroy(RETable *table) {
  if (!table) {
    return;
  }
  for (size_t i = 0; i < table->count; i++) {
    re_entry_release(&table->items[i]);
  }
  free(table->items);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
}

static void re_table_truncate(RETable *table, size_t mark) {
  while (table->count > mark) {
    table->count--;
    re_entry_release(&table->items[table->count]);
  }
}

static int re_table_push(RETable *table, const char *key, const char *value,
                         const char *dep0, const char *dep1, int volatile_value,
                         const char *mem_base, long long mem_off,
                         long long mem_size, unsigned mem_class,
                         size_t kill_pos, unsigned merge_epoch,
                         int survives_summary) {
  if (table->count == table->capacity) {
    size_t capacity = table->capacity ? table->capacity * 2 : 32;
    REEntry *grown = realloc(table->items, capacity * sizeof(REEntry));
    if (!grown) {
      return 0;
    }
    table->items = grown;
    table->capacity = capacity;
  }
  REEntry *entry = &table->items[table->count];
  memset(entry, 0, sizeof(*entry));
  entry->key = mettle_strdup(key);
  entry->value = mettle_strdup(value);
  entry->dep[0] = dep0 ? mettle_strdup(dep0) : NULL;
  entry->dep[1] = dep1 ? mettle_strdup(dep1) : NULL;
  entry->mem_base = mem_base ? mettle_strdup(mem_base) : NULL;
  if (!entry->key || !entry->value || (dep0 && !entry->dep[0]) ||
      (dep1 && !entry->dep[1]) || (mem_base && !entry->mem_base)) {
    re_entry_release(entry);
    return 0;
  }
  entry->mem_off = mem_off;
  entry->mem_size = mem_size;
  entry->mem_class = (unsigned char)mem_class;
  entry->kill_pos = kill_pos;
  entry->merge_epoch = merge_epoch;
  entry->survives_summary = survives_summary;
  entry->volatile_value = volatile_value;
  table->count++;
  return 1;
}

static int re_entry_valid(const IRFunction *function, const REEntry *entry,
                          const REKillLog *kills, unsigned merge_epoch) {
  if (!entry->volatile_value) {
    return 1;
  }
  if (entry->merge_epoch != merge_epoch && !entry->survives_summary) {
    return 0;
  }
  for (size_t k = entry->kill_pos; k < kills->count; k++) {
    if (re_kill_hits(function, &kills->items[k], entry->mem_base,
                     entry->mem_off, entry->mem_size, entry->mem_class)) {
      return 0;
    }
  }
  return 1;
}

static const char *re_table_lookup(const IRFunction *function,
                                   const RETable *table, const char *key,
                                   const REKillLog *kills,
                                   unsigned merge_epoch) {
  for (size_t i = table->count; i-- > 0;) {
    const REEntry *entry = &table->items[i];
    if (!re_entry_valid(function, entry, kills, merge_epoch)) {
      continue;
    }
    if (strcmp(entry->key, key) == 0) {
      return entry->value;
    }
  }
  return NULL;
}

static void re_table_kill_name(RETable *table, const char *name) {
  if (!name) {
    return;
  }
  size_t write = 0;
  for (size_t read = 0; read < table->count; read++) {
    REEntry *entry = &table->items[read];
    int reads_name = (entry->dep[0] && strcmp(entry->dep[0], name) == 0) ||
                     (entry->dep[1] && strcmp(entry->dep[1], name) == 0) ||
                     strcmp(entry->value, name) == 0;
    if (reads_name) {
      re_entry_release(entry);
      continue;
    }
    if (write != read) {
      table->items[write] = *entry;
    }
    write++;
  }
  table->count = write;
}

typedef struct {
  IRFunction *function;
  const REDefs *defs;
  const IRTempValueMap *addr_taken;
  const IRBasicBlock *blocks;
  size_t block_count;
  RETable global;
  RETable local;
  REKillLog kills;
  REKillLog summary;
  int summary_unknown;
  unsigned merge_epoch;
  int *changed;
  int failed;
} REWalk;

static int re_instruction_write_region(const IRFunction *function,
                                       const REDefs *defs,
                                       const IRTempValueMap *addr_taken,
                                       const IRInstruction *ins, char *base,
                                       size_t base_size, long long *off,
                                       long long *size, unsigned *klass) {
  base[0] = '\0';
  *off = 0;
  *size = RE_MEM_WHOLE;
  *klass = ins->op == IR_OP_STORE ? ins->alias_class : IR_ALIAS_CLASS_NONE;

  if (ins->op == IR_OP_STORE) {
    REAddr addr = {0};
    re_resolve_addr(function, defs, &ins->dest, &addr, 0);
    if (!addr.valid || !addr.name ||
        snprintf(base, base_size, "%c%s",
                 addr.is_address_of
                     ? '&'
                     : (addr.kind == IR_OPERAND_SYMBOL ? 's' : 't'),
                 addr.name) >= (int)base_size) {
      base[0] = '\0';
      return 1;
    }
    *off = addr.offset;
    *size = ins->rhs.kind == IR_OPERAND_INT ? ins->rhs.int_value
                                            : RE_MEM_WHOLE;
    return 1;
  }

  switch (ins->op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_JUMP:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_RETURN:
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_SELECT:
    if (ir_operand_is_symbol(&ins->dest) &&
        (ir_temp_value_map_lookup(addr_taken, ins->dest.name) ||
         (!ir_function_symbol_is_parameter(function, ins->dest.name) &&
          ir_function_local_declared_type(function, ins->dest.name) == NULL))) {
      if (snprintf(base, base_size, "&%s", ins->dest.name) >= (int)base_size) {
        base[0] = '\0';
      }
      return 1;
    }
    return 0;
  case IR_OP_PREFETCH:
    return 0;
  default:
    return 1;
  }
}

static int re_load_key(const REWalk *walk, const IRInstruction *ins,
                       char *key, size_t size, REAddr *addr) {
  if (ins->is_volatile) {
    return 0;
  }
  if (ins->dest.kind != IR_OPERAND_TEMP || !ins->dest.name ||
      ins->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  re_resolve_addr(walk->function, walk->defs, &ins->lhs, addr, 0);
  if (!addr->valid || !addr->name) {
    return 0;
  }
  int written =
      snprintf(key, size, "L|%c%s|%lld|%lld|%d%d%d", addr->is_address_of ? '&'
                                                     : (addr->kind == IR_OPERAND_SYMBOL ? 's' : 't'),
               addr->name, addr->offset, ins->rhs.int_value, ins->is_unsigned,
               ins->is_float, ins->float_bits);
  return written > 0 && (size_t)written < size;
}

static int re_pure_key(const REWalk *walk, const IRInstruction *ins, char *key,
                       size_t size, int *portable) {
  char lhs[RE_NAME_MAX];
  char rhs[RE_NAME_MAX];
  int written;

  if (ins->dest.kind != IR_OPERAND_TEMP || !ins->dest.name) {
    return 0;
  }
  switch (ins->op) {
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
    if (!ins->text) {
      return 0;
    }
    break;
  case IR_OP_ADDRESS_OF:
    if (ins->lhs.kind != IR_OPERAND_SYMBOL || !ins->lhs.name) {
      return 0;
    }
    break;
  default:
    return 0;
  }
  if (ins->op == IR_OP_UNARY &&
      (strcmp(ins->text, "*") == 0 || strcmp(ins->text, "&") == 0)) {
    return 0;
  }
  if (!re_operand_key(lhs, sizeof(lhs), &ins->lhs) ||
      !re_operand_key(rhs, sizeof(rhs), &ins->rhs)) {
    return 0;
  }

  const char *tag = ins->op == IR_OP_BINARY   ? "B"
                    : ins->op == IR_OP_UNARY  ? "U"
                    : ins->op == IR_OP_CAST   ? "C"
                                              : "A";
  written = snprintf(key, size, "%s|%s|%s|%s|%d%d", tag,
                     ins->text ? ins->text : "", lhs, rhs, ins->is_float,
                     ins->float_bits);
  if (written <= 0 || (size_t)written >= size) {
    return 0;
  }
  *portable = re_operand_portable(walk->defs, &ins->lhs) &&
              re_operand_portable(walk->defs, &ins->rhs);
  if (ins->op == IR_OP_ADDRESS_OF) {
    *portable = 1;
  }
  return 1;
}

static void re_replace_with_copy(IRInstruction *ins, const char *value,
                                 int *changed) {
  int is_float = ins->is_float;
  int float_bits = ins->float_bits;
  int is_unsigned = ins->is_unsigned;
  IROperand source = ir_operand_temp(value);
  if (!ir_rewrite_to_assign_operand(ins, &source, changed)) {
    ir_operand_destroy(&source);
    return;
  }
  ir_operand_destroy(&source);
  ins->is_float = is_float;
  ins->float_bits = float_bits;
  ins->is_unsigned = is_unsigned;
}

static int re_survives_summary(const REWalk *walk, const char *mem_base,
                               long long mem_off, long long mem_size,
                               unsigned mem_class) {
  if (walk->summary_unknown || !mem_base) {
    return walk->summary.count == 0 && !walk->summary_unknown && mem_base;
  }
  for (size_t k = 0; k < walk->summary.count; k++) {
    if (re_kill_hits(walk->function, &walk->summary.items[k], mem_base,
                     mem_off, mem_size, mem_class)) {
      return 0;
    }
  }
  return 1;
}

static void re_process_block(REWalk *walk, size_t block_index,
                             const IRDomTree *dom) {
  if (walk->failed) {
    return;
  }
  const IRBasicBlock *block = &walk->blocks[block_index];
  size_t global_mark = walk->global.count;
  re_table_truncate(&walk->local, 0);

  if (block->predecessor_count != 1) {
    walk->merge_epoch++;
  }

  for (size_t i = 0; i < block->instruction_count; i++) {
    IRInstruction *ins = &block->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }

    if (ins->op == IR_OP_LOAD || ins->op == IR_OP_BINARY ||
        ins->op == IR_OP_UNARY || ins->op == IR_OP_CAST ||
        ins->op == IR_OP_ADDRESS_OF) {
      char key[RE_KEY_MAX];
      char membuf[RE_NAME_MAX + 1];
      const char *mem_base = NULL;
      long long mem_off = 0;
      long long mem_size = RE_MEM_WHOLE;
      REAddr addr = {0};
      int is_load = ins->op == IR_OP_LOAD;
      int portable = 0;
      int volatile_value = is_load;
      int have_key = is_load
                         ? re_load_key(walk, ins, key, sizeof(key), &addr)
                         : re_pure_key(walk, ins, key, sizeof(key), &portable);
      if (have_key) {
        if (is_load) {
          portable = addr.portable;
          if (snprintf(membuf, sizeof(membuf), "%c%s",
                       addr.is_address_of
                           ? '&'
                           : (addr.kind == IR_OPERAND_SYMBOL ? 's' : 't'),
                       addr.name) < (int)sizeof(membuf)) {
            mem_base = membuf;
            mem_off = addr.offset;
            mem_size = ins->rhs.int_value;
          }
        } else {
          int lhs_alias = re_operand_aliasable(walk->defs, &ins->lhs);
          int rhs_alias = re_operand_aliasable(walk->defs, &ins->rhs);
          volatile_value = lhs_alias || rhs_alias;
          if (lhs_alias != rhs_alias) {
            const char *name = lhs_alias ? ins->lhs.name : ins->rhs.name;
            if (snprintf(membuf, sizeof(membuf), "&%s", name) <
                (int)sizeof(membuf)) {
              mem_base = membuf;
            }
          }
        }
        const char *hit =
            re_table_lookup(walk->function, &walk->local, key, &walk->kills,
                            walk->merge_epoch);
        if (!hit) {
          hit = re_table_lookup(walk->function, &walk->global, key,
                                &walk->kills,
                                walk->merge_epoch);
        }
        if (hit && ins->dest.name && strcmp(hit, ins->dest.name) != 0) {
          char *dest = mettle_strdup(ins->dest.name);
          re_replace_with_copy(ins, hit, walk->changed);
          if (dest) {
            re_table_kill_name(&walk->local, dest);
            free(dest);
          }
          continue;
        }
        if (!hit) {
          const char *dep0 = is_load ? (addr.is_address_of ? NULL : addr.name)
                                     : re_dep_name(&ins->lhs);
          const char *dep1 = is_load ? NULL : re_dep_name(&ins->rhs);
          unsigned mem_class =
              is_load ? ins->alias_class : IR_ALIAS_CLASS_NONE;
          int survives =
              volatile_value ? re_survives_summary(walk, mem_base, mem_off,
                                                   mem_size, mem_class)
                             : 1;
          if (!re_table_push(&walk->local, key, ins->dest.name, dep0, dep1,
                             volatile_value, mem_base, mem_off, mem_size,
                             mem_class, walk->kills.count, walk->merge_epoch,
                             survives)) {
            walk->failed = 1;
            return;
          }
          if (is_load && portable &&
              re_def_count(walk->defs, IR_OPERAND_TEMP, ins->dest.name) == 1 &&
              !re_table_push(&walk->global, key, ins->dest.name, NULL, NULL,
                             volatile_value, mem_base, mem_off, mem_size,
                             mem_class, walk->kills.count, walk->merge_epoch,
                             survives)) {
            walk->failed = 1;
            return;
          }
        }
      }
    }

    {
      char wbase[RE_NAME_MAX + 1];
      long long woff, wsize;
      unsigned wtype = IR_ALIAS_CLASS_NONE;
      if (re_instruction_write_region(walk->function, walk->defs,
                                      walk->addr_taken, ins, wbase,
                                      sizeof(wbase), &woff, &wsize, &wtype) &&
          !re_kills_append(&walk->kills, wbase[0] ? wbase : NULL, woff, wtype,
                           wsize)) {
        walk->failed = 1;
        return;
      }
    }
    if (ir_instruction_writes_destination(ins) && ins->dest.name &&
        (ins->dest.kind == IR_OPERAND_TEMP ||
         ins->dest.kind == IR_OPERAND_SYMBOL)) {
      re_table_kill_name(&walk->local, ins->dest.name);
    }
  }

  for (size_t child = dom->child_head[block_index]; child != IR_BLOCK_NONE;
       child = dom->child_next[child]) {
    re_process_block(walk, child, dom);
    if (walk->failed) {
      return;
    }
  }

  re_table_truncate(&walk->global, global_mark);
}

int ir_redundancy_elimination_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }

  ir_function_clear_cfg(function);
  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!blocks || block_count == 0) {
    return 1;
  }

  REDefs defs = {0};
  const IRAnalysis *analysis = ir_function_analysis(function);
  IRTempValueMap addr_taken;
  int ok = 1;

  if (!analysis || !analysis->dom.built) {
    return 1;
  }

  if (!ir_temp_value_map_init(&addr_taken)) {
    return 1;
  }
  defs.function = function;
  defs.addr_taken = &addr_taken;
  if (!ir_addr_taken_set_build(function, &addr_taken) ||
      !re_collect_defs(function, &defs)) {
    ok = 0;
  }

  if (ok) {
    REWalk walk = {0};
    walk.function = function;
    walk.defs = &defs;
    walk.addr_taken = &addr_taken;
    walk.blocks = blocks;
    walk.block_count = block_count;
    walk.changed = changed;
    for (size_t i = 0; i < function->instruction_count; i++) {
      char wbase[RE_NAME_MAX + 1];
      long long woff, wsize;
      unsigned wtype = IR_ALIAS_CLASS_NONE;
      const IRInstruction *ins = &function->instructions[i];
      if (ins->op == IR_OP_NOP ||
          !re_instruction_write_region(function, &defs, &addr_taken, ins,
                                       wbase, sizeof(wbase), &woff, &wsize,
                                       &wtype)) {
        continue;
      }
      if (!wbase[0]) {
        walk.summary_unknown = 1;
      } else if (!re_kills_append(&walk.summary, wbase, woff, wtype, wsize)) {
        walk.failed = 1;
        break;
      }
    }
    if (!walk.failed) {
      re_process_block(&walk, function->entry_block, &analysis->dom);
    }
    re_table_destroy(&walk.global);
    re_table_destroy(&walk.local);
    re_kills_destroy(&walk.kills);
    re_kills_destroy(&walk.summary);
  }

  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}

#define SEL_MAX_ARM 24

typedef struct {
  size_t index[SEL_MAX_ARM];
  size_t count;
} SelArm;

static int sel_collect(const IRFunction *function, size_t lo, size_t hi,
                       SelArm *arm) {
  arm->count = 0;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (arm->count >= SEL_MAX_ARM) {
      return 0;
    }
    arm->index[arm->count++] = i;
  }
  return 1;
}

static int sel_instruction_is_speculatable(const IRInstruction *ins) {
  switch (ins->op) {
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
    break;
  default:
    return 0;
  }
  if (ins->op == IR_OP_UNARY && ins->text &&
      (strcmp(ins->text, "*") == 0 || strcmp(ins->text, "&") == 0)) {
    return 0;
  }
  if (ins->op == IR_OP_BINARY && ins->text &&
      (strcmp(ins->text, "/") == 0 || strcmp(ins->text, "%") == 0)) {
    return 0;
  }
  return 1;
}

static int sel_operand_matches(const IROperand *a, const IROperand *b,
                               const REMap *rename) {
  if (a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case IR_OPERAND_TEMP: {
    char key[RE_NAME_MAX];
    if (!a->name || !b->name) {
      return 0;
    }
    if (ir_operand_names_match(a, b)) {
      return 1;
    }
    if (!re_name_key(key, sizeof(key), IR_OPERAND_TEMP, b->name)) {
      return 0;
    }
    return re_map_get(rename, key) == 1;
  }
  case IR_OPERAND_STRING: {
    size_t a_length = ir_operand_string_length(a);
    return a->name && b->name && a_length == ir_operand_string_length(b) &&
           memcmp(a->name, b->name, a_length) == 0;
  }
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_LABEL:
    return a->name && b->name && ir_operand_names_match(a, b);
  case IR_OPERAND_INT:
    return a->int_value == b->int_value;
  case IR_OPERAND_FLOAT:
    return a->float_value == b->float_value && a->float_bits == b->float_bits;
  default:
    return 1;
  }
}

static int sel_text_equal(const char *a, const char *b) {
  if (!a && !b) {
    return 1;
  }
  return a && b && strcmp(a, b) == 0;
}

static int sel_condition_is_boolean(const IRFunction *function,
                                    const REDefs *defs,
                                    const IROperand *cond) {
  const IRInstruction *def;
  if (cond->kind != IR_OPERAND_TEMP && cond->kind != IR_OPERAND_SYMBOL) {
    return 0;
  }
  def = re_unique_def(function, defs, cond->kind, cond->name);
  if (!def || def->op != IR_OP_BINARY || def->is_float || !def->text) {
    return 0;
  }
  if (strcmp(def->text, "&") == 0) {
    return def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value == 1;
  }
  return strcmp(def->text, "<") == 0 || strcmp(def->text, ">") == 0 ||
         strcmp(def->text, "<=") == 0 || strcmp(def->text, ">=") == 0 ||
         strcmp(def->text, "==") == 0 || strcmp(def->text, "!=") == 0;
}

static int sel_label_index(const IRFunction *function, const char *label,
                           size_t from, size_t *out) {
  if (!label) {
    return 0;
  }
  for (size_t i = from; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text && strcmp(ins->text, label) == 0) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

static int sel_label_referenced_elsewhere(const IRFunction *function,
                                          const char *label, size_t except) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i == except) {
      continue;
    }
    if ((ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
         ins->op == IR_OP_BRANCH_EQ) &&
        ins->text && strcmp(ins->text, label) == 0) {
      return 1;
    }
  }
  return 0;
}

static int sel_reads_temp(const IRInstruction *ins, const char *name) {
  const IROperand *slots[3];
  slots[0] = &ins->lhs;
  slots[1] = &ins->rhs;
  slots[2] = (ins->op == IR_OP_STORE) ? &ins->dest : NULL;
  for (int k = 0; k < 3; k++) {
    if (slots[k] && slots[k]->kind == IR_OPERAND_TEMP && slots[k]->name &&
        strcmp(slots[k]->name, name) == 0) {
      return 1;
    }
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    if (ins->arguments[a].kind == IR_OPERAND_TEMP && ins->arguments[a].name &&
        strcmp(ins->arguments[a].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int sel_temp_escapes(const IRFunction *function, size_t lo, size_t hi,
                            const char *name) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (i >= lo && i < hi) {
      continue;
    }
    if (sel_reads_temp(&function->instructions[i], name)) {
      return 1;
    }
  }
  return 0;
}

typedef struct {
  size_t branch;
  size_t else_label;
  size_t end_label;
  SelArm then_arm;
  SelArm else_arm;
  size_t diff_at;
  long long then_const;
  long long else_const;
} SelMatch;

static int sel_match_at(const IRFunction *function, size_t branch,
                        SelMatch *out) {
  const IRInstruction *br = &function->instructions[branch];
  const IRInstruction *tail;
  const IRInstruction *diff_a;
  const IRInstruction *diff_b;
  const IRInstruction *last_a;
  SelArm then_arm;
  SelArm else_arm;
  REMap rename = {0};
  size_t else_label = 0;
  size_t end_label = 0;
  size_t diff_count = 0;
  size_t diff_at = 0;
  int ok = 1;

  if (br->op != IR_OP_BRANCH_ZERO || !br->text) {
    return 0;
  }
  if (!sel_label_index(function, br->text, branch + 1, &else_label) ||
      sel_label_referenced_elsewhere(function, br->text, branch)) {
    return 0;
  }
  if (!sel_collect(function, branch + 1, else_label, &then_arm) ||
      then_arm.count < 2) {
    return 0;
  }
  tail = &function->instructions[then_arm.index[then_arm.count - 1]];
  if (tail->op != IR_OP_JUMP || !tail->text) {
    return 0;
  }
  if (!sel_label_index(function, tail->text, else_label + 1, &end_label)) {
    return 0;
  }
  then_arm.count--;

  if (!sel_collect(function, else_label + 1, end_label, &else_arm) ||
      else_arm.count != then_arm.count) {
    return 0;
  }

  for (size_t k = 0; k < then_arm.count && ok; k++) {
    const IRInstruction *a = &function->instructions[then_arm.index[k]];
    const IRInstruction *b = &function->instructions[else_arm.index[k]];
    const IROperand *slots_a[2];
    const IROperand *slots_b[2];
    if (a->op != b->op || !sel_text_equal(a->text, b->text) ||
        a->is_float != b->is_float || a->float_bits != b->float_bits ||
        a->is_unsigned != b->is_unsigned || a->argument_count != 0 ||
        b->argument_count != 0 || !sel_instruction_is_speculatable(a)) {
      ok = 0;
      break;
    }
    slots_a[0] = &a->lhs;
    slots_a[1] = &a->rhs;
    slots_b[0] = &b->lhs;
    slots_b[1] = &b->rhs;
    for (int s = 0; s < 2 && ok; s++) {
      if (sel_operand_matches(slots_a[s], slots_b[s], &rename)) {
        continue;
      }
      if (s == 1 && slots_a[s]->kind == IR_OPERAND_INT &&
          slots_b[s]->kind == IR_OPERAND_INT && diff_count == 0) {
        diff_count = 1;
        diff_at = k;
        continue;
      }
      ok = 0;
    }
    if (!ok) {
      break;
    }
    if (a->dest.kind != b->dest.kind) {
      ok = 0;
      break;
    }
    if (a->dest.kind == IR_OPERAND_TEMP) {
      char key[RE_NAME_MAX];
      if (!a->dest.name || !b->dest.name ||
          !re_name_key(key, sizeof(key), IR_OPERAND_TEMP, b->dest.name) ||
          !re_map_set(&rename, key, 1)) {
        ok = 0;
      }
    } else if (a->dest.kind != IR_OPERAND_NONE) {
      if (!a->dest.name || !b->dest.name ||
          !ir_operand_names_match(&a->dest, &b->dest) || k + 1 != then_arm.count) {
        ok = 0;
      }
    }
  }
  re_map_destroy(&rename);
  if (!ok || diff_count != 1) {
    return 0;
  }

  diff_a = &function->instructions[then_arm.index[diff_at]];
  diff_b = &function->instructions[else_arm.index[diff_at]];
  if (diff_a->op != IR_OP_BINARY || diff_a->is_float || !diff_a->text ||
      strcmp(diff_a->text, "+") != 0 ||
      diff_a->rhs.int_value == diff_b->rhs.int_value) {
    return 0;
  }

  last_a = &function->instructions[then_arm.index[then_arm.count - 1]];
  if (last_a->dest.kind != IR_OPERAND_SYMBOL || !last_a->dest.name) {
    return 0;
  }
  for (size_t k = 0; k + 1 < then_arm.count; k++) {
    const IRInstruction *a = &function->instructions[then_arm.index[k]];
    const IRInstruction *b = &function->instructions[else_arm.index[k]];
    if (a->dest.kind != IR_OPERAND_TEMP || !a->dest.name ||
        b->dest.kind != IR_OPERAND_TEMP || !b->dest.name) {
      return 0;
    }
    if (sel_temp_escapes(function, branch + 1, else_label, a->dest.name) ||
        sel_temp_escapes(function, else_label + 1, end_label, b->dest.name)) {
      return 0;
    }
  }

  out->branch = branch;
  out->else_label = else_label;
  out->end_label = end_label;
  out->then_arm = then_arm;
  out->else_arm = else_arm;
  out->diff_at = diff_at;
  out->then_const = diff_a->rhs.int_value;
  out->else_const = diff_b->rhs.int_value;
  return 1;
}

static int sel_open_room(IRFunction *function, SelMatch *m, size_t body_count) {
  size_t needed = body_count + 3;
  size_t have = m->else_label - m->branch;
  size_t extra;
  IRInstruction nop = {0};

  if (have >= needed) {
    return 0;
  }
  extra = needed - have;
  nop.op = IR_OP_NOP;
  nop.location = function->instructions[m->branch].location;
  for (size_t i = 0; i < extra; i++) {
    if (!ir_function_insert_instruction(function, m->branch, &nop)) {
      return -1;
    }
  }
  m->else_label += extra;
  m->end_label += extra;
  for (size_t k = 0; k < m->then_arm.count; k++) {
    m->then_arm.index[k] += extra;
  }
  for (size_t k = 0; k < m->else_arm.count; k++) {
    m->else_arm.index[k] += extra;
  }
  return 1;
}

static int sel_apply(IRFunction *function, const REDefs *defs, SelMatch *m,
                     int *changed) {
  static int counter;
  IRInstruction body[SEL_MAX_ARM];
  IRInstruction *br = &function->instructions[m->branch];
  IROperand cond = ir_operand_copy(&br->lhs);
  SourceLocation location = br->location;
  int cond_is_boolean = sel_condition_is_boolean(function, defs, &br->lhs);
  long long delta = m->then_const - m->else_const;
  size_t body_count = m->then_arm.count;
  int opened;
  size_t at;
  char sel_name[48];
  char scaled_name[48];
  char offset_name[48];

  snprintf(sel_name, sizeof(sel_name), "__fsel_%d", counter);
  snprintf(scaled_name, sizeof(scaled_name), "__fselm_%d", counter);
  snprintf(offset_name, sizeof(offset_name), "__fselk_%d", counter);
  counter++;

  opened = sel_open_room(function, m, body_count);
  if (opened < 0) {
    ir_operand_destroy(&cond);
    return -1;
  }

  for (size_t k = 0; k < body_count; k++) {
    body[k] = function->instructions[m->then_arm.index[k]];
    memset(&function->instructions[m->then_arm.index[k]], 0,
           sizeof(IRInstruction));
    function->instructions[m->then_arm.index[k]].op = IR_OP_NOP;
  }
  for (size_t i = m->branch; i < m->else_label; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  for (size_t i = m->else_label + 1; i < m->end_label; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }

  at = m->branch;
  {
    IRInstruction *slot = &function->instructions[at++];
    slot->location = location;
    slot->dest = ir_operand_temp(sel_name);
    slot->lhs = cond;
    if (cond_is_boolean) {
      slot->op = IR_OP_ASSIGN;
    } else {
      slot->op = IR_OP_BINARY;
      slot->text = mettle_strdup("!=");
      slot->rhs = ir_operand_int(0);
    }
  }
  {
    IRInstruction *slot = &function->instructions[at++];
    slot->op = IR_OP_BINARY;
    slot->location = location;
    slot->text = mettle_strdup("*");
    slot->dest = ir_operand_temp(scaled_name);
    slot->lhs = ir_operand_temp(sel_name);
    slot->rhs = ir_operand_int(delta);
  }
  {
    IRInstruction *slot = &function->instructions[at++];
    slot->op = IR_OP_BINARY;
    slot->location = location;
    slot->text = mettle_strdup("+");
    slot->dest = ir_operand_temp(offset_name);
    slot->lhs = ir_operand_temp(scaled_name);
    slot->rhs = ir_operand_int(m->else_const);
  }
  for (size_t k = 0; k < body_count; k++) {
    if (k == m->diff_at) {
      ir_operand_destroy(&body[k].rhs);
      body[k].rhs = ir_operand_temp(offset_name);
    }
    function->instructions[at++] = body[k];
  }

  if (changed) {
    *changed = 1;
  }
  return opened;
}

int ir_select_adjacent_field_pass(IRFunction *function, int *changed) {
  REDefs defs = {0};
  IRTempValueMap addr_taken;

  if (!function || function->instruction_count == 0) {
    return 1;
  }
  if (!ir_temp_value_map_init(&addr_taken)) {
    return 1;
  }
  defs.function = function;
  defs.addr_taken = &addr_taken;
  if (ir_addr_taken_set_build(function, &addr_taken) &&
      re_collect_defs(function, &defs)) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      SelMatch match;
      if (function->instructions[i].op != IR_OP_BRANCH_ZERO) {
        continue;
      }
      if (sel_match_at(function, i, &match)) {
        if (sel_apply(function, &defs, &match, changed) != 0) {
          break;
        }
        i = match.end_label;
      }
    }
  }

  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}

static int subword_target_is_wider(const char *type_name, long long load_size) {
  if (!type_name) {
    return 0;
  }
  if (strcmp(type_name, "int32") == 0 || strcmp(type_name, "uint32") == 0 ||
      strcmp(type_name, "int64") == 0 || strcmp(type_name, "uint64") == 0) {
    return 1;
  }
  if (load_size == 1 &&
      (strcmp(type_name, "int16") == 0 || strcmp(type_name, "uint16") == 0)) {
    return 1;
  }
  return 0;
}

int ir_widen_subword_load_cast_pass(IRFunction *function, int *changed) {
  REDefs defs = {0};
  IRTempValueMap addr_taken;

  if (!function || function->instruction_count == 0) {
    return 1;
  }
  if (!ir_temp_value_map_init(&addr_taken)) {
    return 1;
  }
  defs.function = function;
  defs.addr_taken = &addr_taken;
  if (ir_addr_taken_set_build(function, &addr_taken) &&
      re_collect_defs(function, &defs)) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *ins = &function->instructions[i];
      const IRInstruction *src;
      if (ins->op != IR_OP_CAST || ins->is_float ||
          ins->lhs.kind != IR_OPERAND_TEMP || !ins->lhs.name) {
        continue;
      }
      src = re_unique_def(function, &defs, IR_OPERAND_TEMP, ins->lhs.name);
      if (!src || src->op != IR_OP_LOAD || src->is_float || !src->is_unsigned ||
          src->rhs.kind != IR_OPERAND_INT ||
          (src->rhs.int_value != 1 && src->rhs.int_value != 2)) {
        continue;
      }
      if (!subword_target_is_wider(ins->text, src->rhs.int_value)) {
        continue;
      }
      {
        IROperand value = ir_operand_copy(&ins->lhs);
        ir_rewrite_to_assign_operand(ins, &value, changed);
        ir_operand_destroy(&value);
      }
    }
  }

  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}

static int re_operand_entry_constant(const IRFunction *function,
                                     const REDefs *defs, size_t before,
                                     const IROperand *op, long long *out) {
  if (!op) {
    return 0;
  }
  if (op->kind == IR_OPERAND_INT) {
    *out = op->int_value;
    return 1;
  }
  if ((op->kind != IR_OPERAND_SYMBOL && op->kind != IR_OPERAND_TEMP) ||
      !op->name) {
    return 0;
  }
  (void)defs;
  for (size_t k = before; k-- > 0;) {
    const IRInstruction *ins = &function->instructions[k];
    if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_RETURN || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ) {
      return 0;
    }
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind != op->kind || !ins->dest.name ||
        strcmp(ins->dest.name, op->name) != 0) {
      continue;
    }
    if (ins->op == IR_OP_ASSIGN && ins->lhs.kind == IR_OPERAND_INT) {
      *out = ins->lhs.int_value;
      return 1;
    }
    return 0;
  }
  return 0;
}

static int re_loop_runs_at_least_once(const IRFunction *function,
                                      const REDefs *defs, size_t header,
                                      size_t latch) {
  const IRInstruction *cmp = NULL;
  const IRInstruction *branch = NULL;
  long long lhs = 0;
  long long rhs = 0;

  for (size_t i = header + 1; i < latch; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->dest.kind == IR_OPERAND_TEMP) {
      cmp = ins;
      continue;
    }
    if (ins->op == IR_OP_BRANCH_ZERO) {
      branch = ins;
    }
    break;
  }
  if (!cmp || !branch || !cmp->text || !cmp->dest.name ||
      !ir_operand_is_temp_named(&branch->lhs, cmp->dest.name)) {
    return 0;
  }
  if (!re_operand_entry_constant(function, defs, header, &cmp->lhs, &lhs) ||
      !re_operand_entry_constant(function, defs, header, &cmp->rhs, &rhs)) {
    return 0;
  }
  if (strcmp(cmp->text, "<") == 0) {
    return lhs < rhs;
  }
  if (strcmp(cmp->text, "<=") == 0) {
    return lhs <= rhs;
  }
  if (strcmp(cmp->text, ">") == 0) {
    return lhs > rhs;
  }
  if (strcmp(cmp->text, ">=") == 0) {
    return lhs >= rhs;
  }
  if (strcmp(cmp->text, "!=") == 0) {
    return lhs != rhs;
  }
  return 0;
}

static int re_label_is_loop_header(const char *text) {
  if (!text) {
    return 0;
  }
  return strncmp(text, "ir_while_", 9) == 0 ||
         strstr(text, "_lbl_ir_while_") != NULL ||
         strncmp(text, "ir_for_cond_", 12) == 0 ||
         strstr(text, "_lbl_ir_for_cond_") != NULL;
}

static size_t re_loop_latch(const IRFunction *function, size_t header) {
  const char *label = function->instructions[header].text;
  const size_t latch = ir_function_last_jump_to(function, header, label);
  return latch == IR_BLOCK_NONE ? 0 : latch;
}

static int re_collect_loop_writes(const IRFunction *function,
                                  const REDefs *defs,
                                  const IRTempValueMap *addr_taken, size_t lo,
                                  size_t hi, REKillLog *log) {
  for (size_t i = lo; i <= hi; i++) {
    char wbase[RE_NAME_MAX + 1];
    long long woff, wsize;
    unsigned wtype = IR_ALIAS_CLASS_NONE;
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP ||
        !re_instruction_write_region(function, defs, addr_taken, ins, wbase,
                                     sizeof(wbase), &woff, &wsize, &wtype)) {
      continue;
    }
    if (!re_kills_append(log, wbase[0] ? wbase : NULL, woff, wtype, wsize)) {
      return 0;
    }
  }
  return 1;
}

static int re_region_survives_log(const IRFunction *function,
                                  const REKillLog *log, const char *base,
                                  long long off, long long size,
                                  unsigned klass) {
  for (size_t k = 0; k < log->count; k++) {
    if (re_kill_hits(function, &log->items[k], base, off, size, klass)) {
      return 0;
    }
  }
  return 1;
}

static int re_access_reaches(const IRFunction *function, const REDefs *defs,
                             const IRInstruction *ins, const REAddr *addr,
                             long long reach) {
  const IROperand *ao = ins->op == IR_OP_LOAD    ? &ins->lhs
                        : ins->op == IR_OP_STORE ? &ins->dest
                                                 : NULL;
  REAddr pa = {0};
  if (!ao || ins->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  re_resolve_addr(function, defs, ao, &pa, 0);
  return pa.valid && pa.name && pa.is_address_of == addr->is_address_of &&
         pa.kind == addr->kind && strcmp(pa.name, addr->name) == 0 &&
         pa.offset >= 0 && pa.offset + ins->rhs.int_value >= reach;
}

static size_t re_straight_line_end(const IRFunction *function, size_t from,
                                   size_t limit, size_t fallback) {
  for (size_t i = from; i <= limit && i < function->instruction_count; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ ||
        op == IR_OP_JUMP || op == IR_OP_LABEL || op == IR_OP_RETURN) {
      return i;
    }
  }
  return fallback;
}

static int re_function_declares_local(const IRFunction *function,
                                      const char *name) {
  for (size_t k = 0; k < function->instruction_count; k++) {
    const IRInstruction *ins = &function->instructions[k];
    if (ins->op == IR_OP_DECLARE_LOCAL && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && strcmp(ins->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int re_hoist_base_is_dereferenceable(
    const IRFunction *function, const REDefs *defs, const REAddr *addr,
    long long reach, size_t header, size_t at, size_t prefix_end,
    size_t body_prefix_end, size_t entry_end, int runs_at_least_once) {
  if (at < prefix_end) {
    return 1;
  }
  if (addr->is_address_of && addr->name &&
      (ir_function_symbol_is_parameter(function, addr->name) ||
       re_function_declares_local(function, addr->name))) {
    for (size_t k = 0; k < function->instruction_count; k++) {
      if (re_access_reaches(function, defs, &function->instructions[k], addr,
                            reach)) {
        return 1;
      }
    }
  }
  for (size_t k = header + 1; k < prefix_end; k++) {
    if (re_access_reaches(function, defs, &function->instructions[k], addr,
                          reach)) {
      return 1;
    }
  }
  for (size_t k = header; k-- > 0;) {
    const IRInstruction *back = &function->instructions[k];
    if (back->op == IR_OP_LABEL || back->op == IR_OP_JUMP ||
        back->op == IR_OP_RETURN || back->op == IR_OP_BRANCH_ZERO ||
        back->op == IR_OP_BRANCH_EQ) {
      break;
    }
    if (re_access_reaches(function, defs, back, addr, reach)) {
      return 1;
    }
  }
  if (!addr->is_address_of &&
      ir_function_symbol_is_parameter(function, addr->name)) {
    for (size_t k = 0; k < entry_end; k++) {
      if (re_access_reaches(function, defs, &function->instructions[k], addr,
                            reach)) {
        return 1;
      }
    }
  }
  return at < body_prefix_end && runs_at_least_once;
}

static size_t re_entry_block_end(const IRFunction *function) {
  size_t entry_end = 0;
  while (entry_end < function->instruction_count) {
    IROpcode op = function->instructions[entry_end].op;
    if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_RETURN ||
        op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ) {
      break;
    }
    entry_end++;
  }
  return entry_end;
}

static int re_header_has_preheader(const IRFunction *function, size_t header) {
  const IRInstruction *prev;
  size_t p;
  if (header == 0) {
    return 1;
  }
  p = header - 1;
  prev = &function->instructions[p];
  while (p > 0 && prev->op == IR_OP_NOP) {
    prev = &function->instructions[--p];
  }
  return prev->op != IR_OP_JUMP && prev->op != IR_OP_RETURN &&
         prev->op != IR_OP_BRANCH_ZERO && prev->op != IR_OP_BRANCH_EQ;
}

static int re_symbol_load_dest_is_hoistable(const IRFunction *function,
                                            size_t header, size_t index,
                                            size_t latch, const char *name) {
  size_t writes = 0;
  if (!name || !re_function_declares_local(function, name) ||
      ir_symbol_address_taken(function, name) ||
      (strncmp(name, "ir_row_", 7) != 0 && strncmp(name, "ir_view_", 8) != 0)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, name)) {
      writes++;
    }
  }
  if (writes != 1) {
    return 0;
  }
  for (size_t i = header; i < index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_operand_is_symbol_named(&ins->lhs, name) ||
        ir_operand_is_symbol_named(&ins->rhs, name) ||
        (ins->op == IR_OP_STORE &&
         ir_operand_is_symbol_named(&ins->dest, name))) {
      return 0;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_symbol_named(&ins->arguments[a], name)) {
        return 0;
      }
    }
  }
  return !ir_symbol_live_after_loop(function, latch + 1, name);
}

static int re_hoist_descriptor_loads_only;

static int re_symbol_is_descriptor(const IRFunction *function,
                                   const char *name) {
  const char *type = NULL;
  size_t length;
  if (!function || !name) {
    return 0;
  }
  if (strncmp(name, "ir_row_", 7) == 0 || strncmp(name, "ir_view_", 8) == 0) {
    return 1;
  }
  type = ir_function_local_declared_type(function, name);
  if (!type && function->parameter_names && function->parameter_types) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      if (function->parameter_names[i] &&
          strcmp(function->parameter_names[i], name) == 0) {
        type = function->parameter_types[i];
        break;
      }
    }
  }
  if (!type) {
    return 0;
  }
  length = strlen(type);
  if (length > 5 && strcmp(type + length - 5, "__ref") == 0) {
    return 1;
  }
  if (length > 1 && type[length - 1] == '*') {
    const MtlcType *held = ir_program_lookup_type(ir_optimize_get_program(), type);
    return held && held->kind == MTLC_TYPE_STRUCT;
  }
  return length > 2 && type[length - 1] == ']' &&
         (type[length - 2] == '[' || type[length - 2] == ',');
}

static int re_load_is_hoistable(const IRFunction *function, const REDefs *defs,
                                REKillLog *writes, size_t header, size_t index,
                                size_t latch, size_t prefix_end,
                                size_t body_prefix_end, size_t entry_end,
                                int runs_at_least_once, REAddr *addr) {
  IRInstruction *load = &function->instructions[index];
  char membuf[RE_NAME_MAX + 1];

  if (load->op != IR_OP_LOAD || load->is_volatile || !load->dest.name ||
      load->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  if (load->dest.kind == IR_OPERAND_SYMBOL) {
    if (!re_symbol_load_dest_is_hoistable(function, header, index, latch,
                                          load->dest.name)) {
      return 0;
    }
  } else if (load->dest.kind != IR_OPERAND_TEMP) {
    return 0;
  }
  re_resolve_addr(function, defs, &load->lhs, addr, 0);
  if (!addr->valid || !addr->name || !addr->portable || addr->offset < 0 ||
      addr->offset >= 4096 ||
      (addr->kind == IR_OPERAND_TEMP && !addr->is_address_of)) {
    return 0;
  }
  if (re_hoist_descriptor_loads_only &&
      (!addr->is_address_of || load->rhs.int_value != 8 ||
       !re_symbol_is_descriptor(function, addr->name) ||
       (load->alias_class != IR_ALIAS_CLASS_POINTER &&
        load->alias_class != IR_ALIAS_CLASS_I64))) {
    return 0;
  }
  if (load->dest.kind == IR_OPERAND_TEMP &&
      re_def_count(defs, IR_OPERAND_TEMP, load->dest.name) != 1) {
    return 0;
  }
  if (snprintf(membuf, sizeof(membuf), "%c%s", addr->is_address_of ? '&' : 's',
               addr->name) >= (int)sizeof(membuf) ||
      !re_region_survives_log(function, writes, membuf, addr->offset,
                              load->rhs.int_value, load->alias_class)) {
    return 0;
  }
  return re_hoist_base_is_dereferenceable(
      function, defs, addr, addr->offset + load->rhs.int_value, header, index,
      prefix_end, body_prefix_end, entry_end, runs_at_least_once);
}

static int re_emit_hoist(IRFunction *function, size_t header, size_t index,
                         const REAddr *addr, const char *addr_name,
                         int *changed) {
  IRInstruction *load = &function->instructions[index];
  IRInstruction lead = {0};
  IRInstruction body = {0};
  size_t inserted = 0;
  int failed = 0;

  char base_name[RE_NAME_MAX + 8];
  snprintf(base_name, sizeof(base_name), "%sb", addr_name);
  if (addr->is_address_of) {
    lead.op = IR_OP_ADDRESS_OF;
    lead.dest = ir_operand_temp(addr->offset != 0 ? base_name : addr_name);
    lead.lhs = ir_operand_symbol(addr->name);
  } else {
    lead.op = IR_OP_BINARY;
    lead.text = mettle_strdup("+");
    lead.dest = ir_operand_temp(addr_name);
    lead.lhs = ir_operand_symbol(addr->name);
    lead.rhs = ir_operand_int(0);
  }
  lead.location = load->location;
  if (!ir_function_insert_instruction(function, header, &lead)) {
    failed = 1;
  }
  ir_instruction_destroy_storage(&lead);
  if (!failed) {
    IRInstruction *moved;
    inserted++;
    moved = &function->instructions[index + inserted];
    body.op = IR_OP_LOAD;
    body.location = moved->location;
    body.dest = moved->dest.kind == IR_OPERAND_SYMBOL
                    ? ir_operand_symbol(moved->dest.name)
                    : ir_operand_temp(moved->dest.name);
    body.lhs = ir_operand_temp(addr_name);
    body.rhs = ir_operand_int(moved->rhs.int_value);
    body.is_float = moved->is_float;
    body.float_bits = moved->float_bits;
    body.is_unsigned = moved->is_unsigned;
    body.value_type = moved->value_type;
    body.alias_class = moved->alias_class;
    if (addr->offset != 0) {
      IRInstruction *lead_in = &function->instructions[header];
      if (lead_in->op == IR_OP_BINARY) {
        lead_in->rhs.int_value = addr->offset;
      } else {
        IRInstruction add = {0};
        add.op = IR_OP_BINARY;
        add.text = mettle_strdup("+");
        add.dest = ir_operand_temp(addr_name);
        add.lhs = ir_operand_temp(base_name);
        add.rhs = ir_operand_int(addr->offset);
        add.location = body.location;
        if (!ir_function_insert_instruction(function, header + 1, &add)) {
          failed = 1;
        }
        ir_instruction_destroy_storage(&add);
        if (!failed) {
          inserted++;
        }
      }
    }
  }
  if (!failed &&
      ir_function_insert_instruction(function, header + inserted, &body)) {
    inserted++;
    ir_instruction_make_nop(&function->instructions[index + inserted]);
    if (changed) {
      *changed = 1;
    }
  }
  ir_instruction_destroy_storage(&body);
  return failed ? 0 : 1;
}


#define RE_PRESSURE_MAX_NAMES 64

/* Names, not registers, so this over-counts: it includes values that live in
   memory anyway. Swept against Suite 3 at 6, 8, 10, 12, 16, 20, 24, 28 and 34.
   Below this the loops that legitimately need many live values lose their
   hoists, above it the saturated ones keep making spills. */
#define RE_HOIST_MAX_LOOP_LIVE_IN 28

/* The inner budget is far smaller because an inner loop's live-ins are the
   ones actually competing for registers, and because this gate only has to
   catch the loops that are already full. Swept at 20, 16, 12, 10, 8, 7, 6, 5,
   4, 3 and 2: at 7 and 6 only diff_lcs changes, and below 5 interp_ast and
   word_freq start losing hoists they were paying for. */
#define RE_HOIST_MAX_NESTED_LIVE_IN 6

typedef struct {
  const char *names[RE_PRESSURE_MAX_NAMES];
  size_t count;
  int overflowed;
} RENameSet;

static int re_name_set_add(RENameSet *set, const char *name) {
  if (!name) {
    return 0;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (set->names[i] == name || strcmp(set->names[i], name) == 0) {
      return 0;
    }
  }
  if (set->count >= RE_PRESSURE_MAX_NAMES) {
    set->overflowed = 1;
    return 0;
  }
  set->names[set->count++] = name;
  return 1;
}

static int re_name_set_has(const RENameSet *set, const char *name) {
  if (!name) {
    return 0;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (set->names[i] == name || strcmp(set->names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void re_pressure_note_read(RENameSet *set, const IROperand *op) {
  if (!op || (op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL)) {
    return;
  }
  re_name_set_add(set, op->name);
}

/* Everything the loop reads without writing has to sit somewhere for the whole
   loop, so that count is what a hoist competes with for registers. Hoisting a
   load into a loop that is already over the machine's register file only moves
   the load into the spill slot it will be reloaded from, once per iteration,
   which is what it cost in the first place plus the frame traffic. */
/* A hoist out of a loop that has another loop inside it holds its register
   across every iteration of that inner loop, competing with everything the
   inner loop needs, for the same one load saved per outer iteration. That is
   a different price from a hoist out of an innermost loop, so it gets a
   different budget. */
static size_t re_loop_live_in_count(const IRFunction *function, size_t header,
                                    size_t latch);

/* The register a hoist takes is not contended where the hoist happens, it is
   contended in the innermost loop it is held across. Ask that loop how full it
   already is. */
static size_t re_max_nested_live_in(const IRFunction *function, size_t header,
                                    size_t latch) {
  size_t worst = 0;
  for (size_t i = header + 1; i < latch; i++) {
    const IRInstruction *in = &function->instructions[i];
    size_t inner_latch;
    size_t count;
    if (in->op != IR_OP_LABEL || !re_label_is_loop_header(in->text)) {
      continue;
    }
    inner_latch = re_loop_latch(function, i);
    if (!inner_latch || inner_latch <= i || inner_latch > latch) {
      continue;
    }
    count = re_loop_live_in_count(function, i, inner_latch);
    if (count > worst) {
      worst = count;
    }
  }
  return worst;
}

static size_t re_loop_live_in_count(const IRFunction *function, size_t header,
                                    size_t latch) {
  RENameSet written = {{0}, 0, 0};
  RENameSet live_in = {{0}, 0, 0};

  for (size_t i = header; i < latch; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (!ir_instruction_writes_destination(in)) {
      continue;
    }
    if (in->dest.kind == IR_OPERAND_TEMP || in->dest.kind == IR_OPERAND_SYMBOL) {
      re_name_set_add(&written, in->dest.name);
    }
  }
  if (written.overflowed) {
    return RE_PRESSURE_MAX_NAMES;
  }

  for (size_t i = header; i < latch; i++) {
    const IRInstruction *in = &function->instructions[i];
    re_pressure_note_read(&live_in, &in->lhs);
    re_pressure_note_read(&live_in, &in->rhs);
    for (size_t a = 0; a < in->argument_count; a++) {
      re_pressure_note_read(&live_in, &in->arguments[a]);
    }
    if (in->op == IR_OP_STORE) {
      re_pressure_note_read(&live_in, &in->dest);
    }
  }
  if (live_in.overflowed) {
    return RE_PRESSURE_MAX_NAMES;
  }

  {
    size_t live = 0;
    for (size_t i = 0; i < live_in.count; i++) {
      if (!re_name_set_has(&written, live_in.names[i])) {
        live++;
      }
    }
    return live;
  }
}


static int re_try_hoist_one_load(IRFunction *function, const REDefs *defs_in,
                                 const IRTempValueMap *addr_taken,
                                 int *changed) {
  const REDefs defs = *defs_in;
  static int counter;
  size_t entry_end = re_entry_block_end(function);

  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    REKillLog writes = {0};
    size_t latch;
    size_t prefix_end;
    size_t body_prefix_end;
    int runs_at_least_once;

    if (label->op != IR_OP_LABEL || !re_label_is_loop_header(label->text)) {
      continue;
    }
    latch = re_loop_latch(function, header);
    if (!latch || !re_header_has_preheader(function, header)) {
      continue;
    }

    if (!re_collect_loop_writes(function, &defs, addr_taken, header + 1, latch,
                                &writes)) {
      re_kills_destroy(&writes);
      continue;
    }

    if (re_loop_live_in_count(function, header, latch) >=
            RE_HOIST_MAX_LOOP_LIVE_IN ||
        re_max_nested_live_in(function, header, latch) >=
            RE_HOIST_MAX_NESTED_LIVE_IN) {
      re_kills_destroy(&writes);
      continue;
    }

    prefix_end = re_straight_line_end(function, header + 1, latch, latch);

    body_prefix_end = re_straight_line_end(function, prefix_end + 1, latch,
                                           latch);
    runs_at_least_once =
        re_loop_runs_at_least_once(function, &defs, header, latch);

    for (size_t i = header + 1; i < latch; i++) {
      REAddr addr = {0};
      char addr_name[48];
      int hoisted;
      if (!re_load_is_hoistable(function, &defs, &writes, header, i, latch,
                                prefix_end, body_prefix_end, entry_end,
                                runs_at_least_once, &addr)) {
        continue;
      }
      snprintf(addr_name, sizeof(addr_name), "__licm_%d", counter++);
      hoisted = re_emit_hoist(function, header, i, &addr, addr_name, changed);
      re_kills_destroy(&writes);
      return hoisted;
    }
    re_kills_destroy(&writes);
  }
  return 0;
}

int ir_hoist_descriptor_loads_pass(IRFunction *function, int *changed) {
  int ok;
  re_hoist_descriptor_loads_only = 1;
  ok = ir_hoist_invariant_loads_pass(function, changed);
  re_hoist_descriptor_loads_only = 0;
  return ok;
}

int ir_hoist_invariant_loads_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  for (;;) {
    REDefs defs = {0};
    IRTempValueMap addr_taken;
    int moved = 0;
    if (!ir_temp_value_map_init(&addr_taken)) {
      return 1;
    }
    defs.function = function;
    defs.addr_taken = &addr_taken;
    if (ir_addr_taken_set_build(function, &addr_taken) &&
        re_collect_defs(function, &defs)) {
      moved = re_try_hoist_one_load(function, &defs, &addr_taken, changed);
    }
    re_map_destroy(&defs.defs);
    re_map_destroy(&defs.def_at);
    ir_temp_value_map_destroy(&addr_taken);
    if (!moved) {
      break;
    }
  }
  return 1;
}

typedef struct {
  size_t at;
  int is_return;
} REExit;

#define RE_PROMOTE_MAX_EXITS 16
#define RE_PROMOTE_MAX_SITES 48

static int re_label_index_of(const IRFunction *function, const char *label,
                             size_t *out) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text && strcmp(ins->text, label) == 0) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

typedef struct {
  size_t loads[RE_PROMOTE_MAX_SITES];
  size_t stores[RE_PROMOTE_MAX_SITES];
  size_t load_count;
  size_t store_count;
  REExit exits[RE_PROMOTE_MAX_EXITS];
  size_t exit_count;
  int header_exit;
  int viable;
  int strong;
  int is_float;
  int float_bits;
  int is_unsigned;
  MtlcType *value_type;
  unsigned char promoted_class;
} REPromoteSites;

#define RE_ACCESS_ELSEWHERE 0
#define RE_ACCESS_PARTIAL 1
#define RE_ACCESS_EXACT 2

static int re_promote_classify_access(const IRFunction *function,
                                      const REDefs *defs,
                                      const IRInstruction *ins,
                                      const char *region_base,
                                      const REAddr *region, long long size,
                                      unsigned seed_class, int known_float) {
  REAddr a = {0};
  const IROperand *ao = ins->op == IR_OP_LOAD ? &ins->lhs : &ins->dest;
  char abase[RE_NAME_MAX + 1];
  int resolvable;
  long long asize;

  re_resolve_addr(function, defs, ao, &a, 0);
  resolvable = a.valid && a.name &&
               snprintf(abase, sizeof(abase), "%c%s",
                        a.is_address_of ? '&' : 's',
                        a.name) < (int)sizeof(abase);
  asize = ins->rhs.kind == IR_OPERAND_INT ? ins->rhs.int_value : RE_MEM_WHOLE;
  {
    REMemRegion probe = {resolvable ? abase : NULL, a.offset, asize,
                         ins->alias_class};
    int hits = re_kill_hits(function, &probe, region_base, region->offset,
                            size, seed_class);
    if (getenv("METTLE_PROM_TRACE")) {
      fprintf(stderr,
              "[probe] %s @%zu %s cls=%u seed=%u distinct=%d base=%s "
              "region=%s hits=%d line=%zu:%zu\n",
              function->name ? function->name : "?",
              (size_t)(ins - function->instructions),
              ins->op == IR_OP_LOAD ? "load" : "store",
              (unsigned)ins->alias_class, seed_class,
              ir_alias_classes_distinct(ins->alias_class, seed_class),
              resolvable ? abase : "-", region_base, hits,
              ins->location.line, ins->location.column);
    }
    if (!hits) {
      return RE_ACCESS_ELSEWHERE;
    }
  }
  if (!resolvable || strcmp(abase, region_base) != 0 ||
      a.offset != region->offset || asize != size ||
      ins->is_float != known_float) {
    return RE_ACCESS_PARTIAL;
  }
  return RE_ACCESS_EXACT;
}

static void re_promote_collect_sites(const IRFunction *function,
                                     const REDefs *defs_in,
                                     const IRTempValueMap *addr_taken,
                                     size_t header, size_t latch,
                                     const char *region_base,
                                     const REAddr *region_in, long long size,
                                     const IRInstruction *seed,
                                     REPromoteSites *out) {
  const REDefs defs = *defs_in;
  const REAddr region = *region_in;
  size_t *loads = out->loads;
  size_t *stores = out->stores;
  size_t load_count = 0;
  size_t store_count = 0;
  REExit *exits = out->exits;
  size_t exit_count = 0;
  int header_exit = -1;
  int viable = 1;
  int strong = 1;
  int is_float = 0;
  int float_bits = 0;
  int is_unsigned = 0;
  MtlcType *value_type = NULL;
  unsigned char promoted_class = IR_ALIAS_CLASS_NONE;

  for (size_t i = header + 1; i < latch && viable; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_LOAD || ins->op == IR_OP_STORE) {
      int known_float = (load_count || store_count) ? is_float : ins->is_float;
      int verdict = re_promote_classify_access(function, &defs, ins,
                                               region_base, &region, size,
                                               seed->alias_class, known_float);
      if (verdict == RE_ACCESS_ELSEWHERE) {
        continue;
      }
      if (verdict == RE_ACCESS_PARTIAL) {
        if (ins->op == IR_OP_LOAD) {
          strong = 0;
          continue;
        }
        viable = 0;
        break;
      }
      if (ins->op == IR_OP_LOAD) {
        if (ins->dest.kind != IR_OPERAND_TEMP || !ins->dest.name ||
            load_count >= RE_PROMOTE_MAX_SITES) {
          strong = 0;
          continue;
        }
        loads[load_count++] = i;
      } else {
        if (store_count >= RE_PROMOTE_MAX_SITES) {
          viable = 0;
          break;
        }
        stores[store_count++] = i;
      }
      is_float = ins->is_float;
      float_bits = ins->float_bits;
      if (ins->op == IR_OP_LOAD) {
        is_unsigned = ins->is_unsigned;
        if (ins->value_type) {
          value_type = ins->value_type;
        }
      }
      if (ins->alias_class != IR_ALIAS_CLASS_NONE) {
        promoted_class = ins->alias_class;
      }
      continue;
    }
    {
      char wbase[RE_NAME_MAX + 1];
      long long woff, wsize;
      unsigned wtype = IR_ALIAS_CLASS_NONE;
      if (re_instruction_write_region(function, &defs, addr_taken, ins,
                                      wbase, sizeof(wbase), &woff, &wsize,
                                      &wtype)) {
        REMemRegion probe = {wbase[0] ? wbase : NULL, woff, wsize,
                             (unsigned char)wtype};
        if (re_kill_hits(function, &probe, region_base, region.offset, size,
                         seed->alias_class)) {
          viable = 0;
          break;
        }
      }
    }
    if (ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT) {
      viable = 0;
      break;
    }
    if (ins->op == IR_OP_RETURN) {
      if (exit_count >= RE_PROMOTE_MAX_EXITS) {
        viable = 0;
        break;
      }
      exits[exit_count].at = i;
      exits[exit_count].is_return = 1;
      exit_count++;
    } else if ((ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
                ins->op == IR_OP_BRANCH_EQ) &&
               ins->text) {
      size_t target = 0;
      if (!re_label_index_of(function, ins->text, &target)) {
        viable = 0;
        break;
      }
      if (target <= header || target > latch) {
        if (exit_count >= RE_PROMOTE_MAX_EXITS) {
          viable = 0;
          break;
        }
        exits[exit_count].at = i;
        exits[exit_count].is_return = 0;
        if ((size_t)i <= header) {
          viable = 0;
          break;
        }
        exit_count++;
      }
    }
  }

  out->load_count = load_count;
  out->store_count = store_count;
  out->exit_count = exit_count;
  out->header_exit = header_exit;
  out->viable = viable;
  out->strong = strong;
  out->is_float = is_float;
  out->float_bits = float_bits;
  out->is_unsigned = is_unsigned;
  out->value_type = value_type;
  out->promoted_class = promoted_class;
}

static int re_promote_insert_exit_store(IRFunction *function, size_t at,
                                        int is_address_of,
                                        const char *region_name,
                                        long long region_offset,
                                        const char *local_name, long long size,
                                        unsigned char promoted_class,
                                        SourceLocation location, int counter,
                                        size_t exit_index,
                                        size_t *inserted_out) {
  char addr_name[64];
  char root_name[64];
  IRInstruction pieces[3];
  size_t inserted = 0;
  int failed = 0;
  snprintf(addr_name, sizeof(addr_name), "__proma_%d_x%zu", counter,
           exit_index);
  snprintf(root_name, sizeof(root_name), "__proma_%d_x%zu_r", counter,
           exit_index);
  memset(pieces, 0, sizeof(pieces));
  if (is_address_of) {
    pieces[0].op = IR_OP_ADDRESS_OF;
    pieces[0].dest = ir_operand_temp(root_name);
    pieces[0].lhs = ir_operand_symbol(region_name);
    pieces[1].op = IR_OP_BINARY;
    pieces[1].text = mettle_strdup("+");
    pieces[1].dest = ir_operand_temp(addr_name);
    pieces[1].lhs = ir_operand_temp(root_name);
    pieces[1].rhs = ir_operand_int(region_offset);
  } else {
    pieces[0].op = IR_OP_NOP;
    pieces[1].op = IR_OP_BINARY;
    pieces[1].text = mettle_strdup("+");
    pieces[1].dest = ir_operand_temp(addr_name);
    pieces[1].lhs = ir_operand_symbol(region_name);
    pieces[1].rhs = ir_operand_int(region_offset);
  }
  pieces[2].op = IR_OP_STORE;
  pieces[2].dest = ir_operand_temp(addr_name);
  pieces[2].lhs = ir_operand_symbol(local_name);
  pieces[2].rhs = ir_operand_int(size);
  pieces[2].alias_class = promoted_class;
  for (int k = 0; k < 3; k++) {
    pieces[k].location = location;
  }
  for (int k = 0; k < 3; k++) {
    if (pieces[k].op != IR_OP_NOP && !failed &&
        !ir_function_insert_instruction(function, at + inserted, &pieces[k])) {
      failed = 1;
    }
    if (pieces[k].op != IR_OP_NOP && !failed) {
      inserted++;
    }
    ir_instruction_destroy_storage(&pieces[k]);
  }
  *inserted_out = inserted;
  return !failed;
}

static size_t re_promote_exit_block_position(const IRFunction *function,
                                             const char *target,
                                             size_t after) {
  size_t end = function->instruction_count;
  for (size_t i = after + 1; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_LABEL || !in->text || strcmp(in->text, target) != 0) {
      continue;
    }
    size_t p = i;
    while (p > 0 && function->instructions[p - 1].op == IR_OP_NOP) {
      p--;
    }
    if (p == 0) {
      return end;
    }
    const IRInstruction *prev = &function->instructions[p - 1];
    if (prev->op == IR_OP_JUMP || prev->op == IR_OP_RETURN) {
      return i;
    }
    return end;
  }
  return end;
}

typedef struct {
  IRFunction *function;
  const REAddr *region;
  const char *region_name;
  const char *local_name;
  long long size;
  unsigned char promoted_class;
  int id;
} REPromotion;

static int re_prom_trace(void) {
  static int cached = -1;

  if (cached < 0) {
    cached = getenv("METTLE_PROM_TRACE") ? 1 : 0;
  }
  return cached;
}

static int re_preheader_falls_in(const IRFunction *function, size_t header) {
  const IRInstruction *prev;
  size_t p;

  if (header == 0) {
    return 1;
  }
  p = header - 1;
  prev = &function->instructions[p];
  while (p > 0 && prev->op == IR_OP_NOP) {
    prev = &function->instructions[--p];
  }
  return prev->op != IR_OP_JUMP && prev->op != IR_OP_RETURN &&
         prev->op != IR_OP_BRANCH_ZERO && prev->op != IR_OP_BRANCH_EQ;
}

static int re_promote_region_is_usable(const REAddr *region) {
  return region->valid && region->name && region->portable &&
         region->offset >= 0 && region->offset < 4096 &&
         !(region->kind == IR_OPERAND_TEMP && !region->is_address_of);
}

static int re_promote_region_names(const REAddr *region, char *base,
                                   size_t base_size, char *name,
                                   size_t name_size) {
  return snprintf(base, base_size, "%c%s", region->is_address_of ? '&' : 's',
                  region->name) < (int)base_size &&
         snprintf(name, name_size, "%s", region->name) < (int)name_size;
}

static int re_promote_function_ends_in_terminator(const IRFunction *function) {
  size_t last = function->instruction_count;

  while (last > 0 && function->instructions[last - 1].op == IR_OP_NOP) {
    last--;
  }
  return last != 0 && (function->instructions[last - 1].op == IR_OP_RETURN ||
                       function->instructions[last - 1].op == IR_OP_JUMP);
}

static size_t re_promote_prefix_end(const IRFunction *function, size_t header,
                                    size_t latch) {
  for (size_t i = header + 1; i <= latch; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ ||
        op == IR_OP_JUMP || op == IR_OP_LABEL || op == IR_OP_RETURN) {
      return i;
    }
  }
  return latch;
}

static int re_promote_touches_prefix(const REPromoteSites *sites,
                                     size_t prefix_end) {
  for (size_t k = 0; k < sites->load_count; k++) {
    if (sites->loads[k] < prefix_end) {
      return 1;
    }
  }
  for (size_t k = 0; k < sites->store_count; k++) {
    if (sites->stores[k] < prefix_end) {
      return 1;
    }
  }
  return 0;
}

static void re_promote_build_pieces(const REPromotion *pr,
                                    const REPromoteSites *sites,
                                    const char *type_name,
                                    const char *addr_name, size_t header,
                                    IRInstruction *pieces) {
  const REAddr *region = pr->region;

  memset(pieces, 0, sizeof(*pieces) * 4);
  pieces[0].op = IR_OP_DECLARE_LOCAL;
  pieces[0].dest = ir_operand_symbol(pr->local_name);
  pieces[0].text = mettle_strdup(type_name);
  if (region->is_address_of) {
    pieces[1].op = IR_OP_ADDRESS_OF;
    pieces[1].dest = ir_operand_temp(addr_name);
    pieces[1].lhs = ir_operand_symbol(region->name);
    pieces[2].op = IR_OP_BINARY;
    pieces[2].text = mettle_strdup("+");
    pieces[2].dest = ir_operand_temp(addr_name);
    pieces[2].lhs = ir_operand_temp(addr_name);
    pieces[2].rhs = ir_operand_int(region->offset);
  } else {
    pieces[1].op = IR_OP_BINARY;
    pieces[1].text = mettle_strdup("+");
    pieces[1].dest = ir_operand_temp(addr_name);
    pieces[1].lhs = ir_operand_symbol(region->name);
    pieces[1].rhs = ir_operand_int(region->offset);
    pieces[2].op = IR_OP_NOP;
  }
  pieces[3].op = IR_OP_LOAD;
  pieces[3].dest = ir_operand_symbol(pr->local_name);
  pieces[3].lhs = ir_operand_temp(addr_name);
  pieces[3].rhs = ir_operand_int(pr->size);
  pieces[3].is_unsigned = sites->is_unsigned;
  pieces[3].float_bits = sites->float_bits;
  pieces[3].value_type = sites->value_type;
  pieces[3].alias_class = pr->promoted_class;
  for (int k = 0; k < 4; k++) {
    pieces[k].location = pr->function->instructions[header].location;
  }
}

static int re_promote_insert_pieces(IRFunction *function, size_t header,
                                    IRInstruction *pieces,
                                    size_t *out_inserted) {
  size_t inserted = 0;
  int ok = 1;

  for (int k = 0; k < 4 && ok; k++) {
    if (pieces[k].op == IR_OP_NOP) {
      continue;
    }
    ok = ir_function_insert_instruction(function, header + inserted,
                                        &pieces[k]);
    ir_instruction_destroy_storage(&pieces[k]);
    if (ok) {
      inserted++;
    }
  }
  *out_inserted = inserted;
  return ok;
}

static void re_promote_shift_sites(REPromoteSites *sites, size_t inserted) {
  for (size_t k = 0; k < sites->load_count; k++) {
    sites->loads[k] += inserted;
  }
  for (size_t k = 0; k < sites->store_count; k++) {
    sites->stores[k] += inserted;
  }
  for (size_t k = 0; k < sites->exit_count; k++) {
    sites->exits[k].at += inserted;
  }
}

static void re_promote_rewrite_loads(IRFunction *function,
                                     const REPromoteSites *sites,
                                     const char *local_name) {
  for (size_t k = 0; k < sites->load_count; k++) {
    IRInstruction *ld = &function->instructions[sites->loads[k]];
    IROperand dest = ir_operand_temp(ld->dest.name);
    int keep_unsigned = ld->is_unsigned;
    int keep_float_bits = ld->float_bits;
    MtlcType *keep_type = ld->value_type;

    ir_instruction_destroy_storage(ld);
    memset(ld, 0, sizeof(*ld));
    ld->op = IR_OP_ASSIGN;
    ld->dest = dest;
    ld->lhs = ir_operand_symbol(local_name);
    ld->is_unsigned = keep_unsigned;
    ld->float_bits = keep_float_bits;
    ld->value_type = keep_type;
  }
}

static void re_promote_rewrite_stores(IRFunction *function,
                                     const REPromoteSites *sites,
                                     const char *local_name) {
  for (size_t k = 0; k < sites->store_count; k++) {
    IRInstruction *st = &function->instructions[sites->stores[k]];
    IROperand value = ir_operand_copy(&st->lhs);

    ir_instruction_destroy_storage(st);
    memset(st, 0, sizeof(*st));
    st->op = IR_OP_ASSIGN;
    st->dest = ir_operand_symbol(local_name);
    st->lhs = value;
  }
}

static int re_promote_mirror_stores(IRFunction *function,
                                    REPromoteSites *sites,
                                    const char *local_name) {
  for (size_t k = 0; k < sites->store_count; k++) {
    IRInstruction upd = {0};
    int ok;

    upd.op = IR_OP_ASSIGN;
    upd.dest = ir_operand_symbol(local_name);
    upd.lhs = ir_operand_copy(&function->instructions[sites->stores[k]].lhs);
    upd.location = function->instructions[sites->stores[k]].location;
    ok = ir_function_insert_instruction(function, sites->stores[k] + 1, &upd);
    ir_instruction_destroy_storage(&upd);
    if (!ok) {
      return 0;
    }
    for (size_t m = k + 1; m < sites->store_count; m++) {
      sites->stores[m]++;
    }
  }
  return 1;
}

typedef struct {
  char target[RE_PROMOTE_MAX_EXITS][64];
  char tail[RE_PROMOTE_MAX_EXITS][64];
  size_t count;
} REPromoteTails;

static int re_promote_tails_find(const REPromoteTails *tails,
                                 const char *target) {
  for (size_t m = 0; m < tails->count; m++) {
    if (target && strcmp(tails->target[m], target) == 0) {
      return (int)m;
    }
  }
  return -1;
}

static void re_promote_tails_add(REPromoteTails *tails, const char *target,
                                 const char *tail_name) {
  if (tails->count >= RE_PROMOTE_MAX_EXITS ||
      strlen(target) >= sizeof(tails->target[0])) {
    return;
  }
  snprintf(tails->target[tails->count], sizeof(tails->target[0]), "%s", target);
  snprintf(tails->tail[tails->count], sizeof(tails->tail[0]), "%s", tail_name);
  tails->count++;
}

static int re_promote_return_exit(REPromotion *pr, REPromoteSites *sites,
                                 size_t k, size_t *latch) {
  IRFunction *function = pr->function;
  size_t at = sites->exits[k].at;
  SourceLocation where = function->instructions[at].location;
  size_t inserted = 0;

  if (!re_promote_insert_exit_store(function, at, pr->region->is_address_of,
                                    pr->region_name, pr->region->offset,
                                    pr->local_name, pr->size,
                                    pr->promoted_class, where, pr->id, k,
                                    &inserted)) {
    return 0;
  }
  for (size_t m = 0; m < sites->exit_count; m++) {
    if (sites->exits[m].at >= at) {
      sites->exits[m].at += inserted;
    }
  }
  *latch += inserted;
  return 1;
}

static int re_promote_branch_exit(REPromotion *pr, REPromoteSites *sites,
                                  size_t k, REPromoteTails *tails,
                                  size_t *latch) {
  IRFunction *function = pr->function;
  IRInstruction *br = &function->instructions[sites->exits[k].at];
  SourceLocation where = br->location;
  IRInstruction tail_label = {0};
  IRInstruction tail_jump = {0};
  char tail_name[64];
  char *old_target;
  size_t inserted = 0;
  size_t end;
  int shared = re_promote_tails_find(tails, br->text);
  int ok;

  if (shared >= 0) {
    mettle_free_string(br->text);
    br->text = mettle_strdup(tails->tail[shared]);
    return br->text != NULL;
  }
  snprintf(tail_name, sizeof(tail_name), "__promx_%d_%zu", pr->id, k);
  old_target = mettle_strdup(br->text);
  if (!old_target) {
    return 0;
  }
  re_promote_tails_add(tails, old_target, tail_name);
  mettle_free_string(br->text);
  br->text = mettle_strdup(tail_name);
  if (!br->text) {
    free(old_target);
    return 0;
  }
  tail_label.op = IR_OP_LABEL;
  tail_label.text = mettle_strdup(tail_name);
  tail_jump.op = IR_OP_JUMP;
  tail_jump.text = old_target;
  end = re_promote_exit_block_position(function, old_target,
                                       sites->exits[k].at);
  ok = tail_label.text &&
       ir_function_insert_instruction(function, end, &tail_label) &&
       re_promote_insert_exit_store(function, end + 1,
                                    pr->region->is_address_of, pr->region_name,
                                    pr->region->offset, pr->local_name,
                                    pr->size, pr->promoted_class, where, pr->id,
                                    k, &inserted) &&
       ir_function_insert_instruction(function, end + 1 + inserted, &tail_jump);
  ir_instruction_destroy_storage(&tail_label);
  ir_instruction_destroy_storage(&tail_jump);
  if (!ok) {
    return 0;
  }
  for (size_t m = 0; m < sites->exit_count; m++) {
    if (sites->exits[m].at >= end) {
      sites->exits[m].at += inserted + 2;
    }
  }
  if (*latch >= end) {
    *latch += inserted + 2;
  }
  return 1;
}

static int re_promote_exits(REPromotion *pr, REPromoteSites *sites,
                            size_t *latch) {
  REPromoteTails tails;

  tails.count = 0;
  for (size_t k = 0; k < sites->exit_count; k++) {
    if (sites->exits[k].is_return) {
      if (!re_promote_return_exit(pr, sites, k, latch)) {
        return 0;
      }
      continue;
    }
    if (!re_promote_branch_exit(pr, sites, k, &tails, latch)) {
      return 0;
    }
  }
  return 1;
}

static int re_promote_is_viable(const REPromoteSites *sites,
                                const IRFunction *function, size_t header,
                                size_t latch, int *strong) {
  if (re_prom_trace()) {
    fprintf(stderr,
            "[prom]   viable=%d strong=%d loads=%zu stores=%zu exits=%zu "
            "float=%d\n",
            sites->viable, sites->strong, sites->load_count,
            sites->store_count, sites->exit_count, sites->is_float);
  }
  if (!sites->viable || sites->store_count == 0 || sites->load_count == 0 ||
      sites->is_float) {
    return 0;
  }
  *strong = sites->strong;
  if (sites->exit_count == 0) {
    *strong = 0;
  }
  if (!re_promote_function_ends_in_terminator(function)) {
    if (re_prom_trace()) {
      fprintf(stderr, "[prom]   no-terminator: weak only\n");
    }
    *strong = 0;
  }
  if (!re_promote_touches_prefix(sites,
                                 re_promote_prefix_end(function, header,
                                                       latch))) {
    if (re_prom_trace()) {
      fprintf(stderr, "[prom]   bail: prefix-unsafe\n");
    }
    return 0;
  }
  return 1;
}

static int re_promote_seed(IRFunction *function, const REDefs *defs,
                           const IRTempValueMap *addr_taken, size_t *header,
                           size_t *latch, size_t s, int *counter,
                           int *changed, int *applied) {
  const IRInstruction *seed = &function->instructions[s];
  REAddr region = {0};
  REPromoteSites sites;
  REPromotion pr;
  char region_base[RE_NAME_MAX + 1];
  char region_name[RE_NAME_MAX + 1];
  char local_name[48];
  char addr_name[48];
  const char *type_name;
  IRInstruction pieces[4];
  size_t inserted = 0;
  int strong = 0;

  *applied = 0;
  if (seed->op != IR_OP_STORE || seed->rhs.kind != IR_OPERAND_INT) {
    return 1;
  }
  re_resolve_addr(function, defs, &seed->dest, &region, 0);
  if (re_prom_trace()) {
    fprintf(stderr,
            "[prom] %s store@%zu valid=%d name=%s port=%d off=%lld kind=%d "
            "ao=%d\n",
            function->name ? function->name : "?", s, region.valid,
            region.name ? region.name : "-", region.portable, region.offset,
            (int)region.kind, region.is_address_of);
  }
  if (!re_promote_region_is_usable(&region) ||
      !re_promote_region_names(&region, region_base, sizeof(region_base),
                               region_name, sizeof(region_name))) {
    return 1;
  }
  pr.function = function;
  pr.region = &region;
  pr.region_name = region_name;
  pr.size = seed->rhs.int_value;
  re_promote_collect_sites(function, defs, addr_taken, *header, *latch,
                           region_base, &region, pr.size, seed, &sites);
  if (!re_promote_is_viable(&sites, function, *header, *latch, &strong)) {
    return 1;
  }
  pr.id = (*counter)++;
  pr.promoted_class = sites.promoted_class;
  snprintf(local_name, sizeof(local_name), "__prom_%d", pr.id);
  snprintf(addr_name, sizeof(addr_name), "__proma_%d", pr.id);
  pr.local_name = local_name;
  type_name = pr.size == 8 ? "int64"
                           : (sites.is_unsigned ? "uint32" : "int32");
  if (pr.size != 4 && pr.size != 8) {
    if (re_prom_trace()) {
      fprintf(stderr, "[prom]   bail: size\n");
    }
    return 1;
  }
  re_promote_build_pieces(&pr, &sites, type_name, addr_name, *header, pieces);
  if (!re_promote_insert_pieces(function, *header, pieces, &inserted)) {
    return 0;
  }
  *header += inserted;
  *latch += inserted;
  re_promote_shift_sites(&sites, inserted);
  re_promote_rewrite_loads(function, &sites, local_name);
  if (!strong) {
    if (!re_promote_mirror_stores(function, &sites, local_name)) {
      return 0;
    }
    if (changed) {
      *changed = 1;
    }
    *applied = 1;
    return 1;
  }
  re_promote_rewrite_stores(function, &sites, local_name);
  if (!re_promote_exits(&pr, &sites, latch)) {
    return 0;
  }
  if (changed) {
    *changed = 1;
  }
  *applied = 1;
  return 1;
}

static int re_try_promote_one(IRFunction *function, const REDefs *defs_in,
                              const IRTempValueMap *addr_taken, int *changed) {
  const REDefs defs = *defs_in;
  static int counter;

  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    size_t latch;

    if (label->op != IR_OP_LABEL || !re_label_is_loop_header(label->text)) {
      continue;
    }
    latch = re_loop_latch(function, header);
    if (!latch || !re_preheader_falls_in(function, header)) {
      continue;
    }
    for (size_t s = header + 1; s < latch; s++) {
      int applied = 0;
      if (!re_promote_seed(function, &defs, addr_taken, &header, &latch, s,
                           &counter, changed, &applied)) {
        return 0;
      }
      if (applied) {
        return 1;
      }
    }
  }
  return 0;
}

int ir_promote_loop_memory_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  if (getenv("METTLE_PROM_TRACE")) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ins->op != IR_OP_LOAD && ins->op != IR_OP_STORE) {
        continue;
      }
      fprintf(stderr, "[census] %s @%zu %s cls=%u size=%lld line=%zu:%zu\n",
              function->name ? function->name : "?", i,
              ins->op == IR_OP_LOAD ? "load" : "store",
              (unsigned)ins->alias_class,
              ins->rhs.kind == IR_OPERAND_INT ? ins->rhs.int_value : -1LL,
              ins->location.line, ins->location.column);
    }
  }
  for (;;) {
    REDefs defs = {0};
    IRTempValueMap addr_taken;
    int moved = 0;
    if (!ir_temp_value_map_init(&addr_taken)) {
      return 1;
    }
    defs.function = function;
    defs.addr_taken = &addr_taken;
    if (ir_addr_taken_set_build(function, &addr_taken) &&
        re_collect_defs(function, &defs)) {
      moved = re_try_promote_one(function, &defs, &addr_taken, changed);
    }
    re_map_destroy(&defs.defs);
    re_map_destroy(&defs.def_at);
    ir_temp_value_map_destroy(&addr_taken);
    if (!moved) {
      break;
    }
  }
  return 1;
}

static void re_rewrite_operand_temp_to_symbol(IROperand *operand,
                                              const char *temp,
                                              const char *symbol,
                                              int *changed) {
  if (operand->kind == IR_OPERAND_TEMP && operand->name &&
      strcmp(operand->name, temp) == 0) {
    ir_operand_destroy(operand);
    *operand = ir_operand_symbol(symbol);
    if (changed) {
      *changed = 1;
    }
  }
}

int ir_unify_param_copy_spelling_pass(IRFunction *function, int *changed) {
  REDefs defs = {0};
  IRTempValueMap addr_taken;

  if (!function || function->instruction_count == 0) {
    return 1;
  }
  if (!ir_temp_value_map_init(&addr_taken)) {
    return 1;
  }
  defs.function = function;
  defs.addr_taken = &addr_taken;
  if (ir_addr_taken_set_build(function, &addr_taken) &&
      re_collect_defs(function, &defs)) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *copy = &function->instructions[i];
      if (copy->op != IR_OP_ASSIGN || copy->dest.kind != IR_OPERAND_SYMBOL ||
          !copy->dest.name || copy->lhs.kind != IR_OPERAND_TEMP ||
          !copy->lhs.name) {
        continue;
      }
      if (re_def_count(&defs, IR_OPERAND_SYMBOL, copy->dest.name) != 1 ||
          re_def_count(&defs, IR_OPERAND_TEMP, copy->lhs.name) != 1 ||
          re_symbol_is_aliasable(&defs, copy->dest.name)) {
        continue;
      }
      char symbol[RE_NAME_MAX];
      char temp[RE_NAME_MAX];
      if (snprintf(symbol, sizeof(symbol), "%s", copy->dest.name) >=
              (int)sizeof(symbol) ||
          snprintf(temp, sizeof(temp), "%s", copy->lhs.name) >=
              (int)sizeof(temp)) {
        continue;
      }
      for (size_t j = i + 1; j < function->instruction_count; j++) {
        IRInstruction *ins = &function->instructions[j];
        if (ins->op == IR_OP_NOP) {
          continue;
        }
        re_rewrite_operand_temp_to_symbol(&ins->lhs, temp, symbol, changed);
        re_rewrite_operand_temp_to_symbol(&ins->rhs, temp, symbol, changed);
        if (ins->op == IR_OP_STORE) {
          re_rewrite_operand_temp_to_symbol(&ins->dest, temp, symbol, changed);
        }
        for (size_t a = 0; a < ins->argument_count; a++) {
          re_rewrite_operand_temp_to_symbol(&ins->arguments[a], temp, symbol,
                                            changed);
        }
      }
    }
  }

  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}

#define BP_MAX_LEAVES 4
#define BP_MAX_TERMS 4
#define BP_MAX_PATTERN 32
#define BP_MAX_DEPTH 8

typedef struct {
  const char *terms[BP_MAX_TERMS];
  IROperandKind kinds[BP_MAX_TERMS];
  int count;
  long long konst;
  int ok;
} BPAddr;

typedef struct {
  const IRInstruction *load;
  size_t load_at;
  int shift;
} BPLeaf;

typedef struct {
  BPLeaf leaves[BP_MAX_LEAVES];
  int leaf_count;
  size_t pattern[BP_MAX_PATTERN];
  int pattern_count;
  int ok;
} BPMatch;

static void bp_add_term(BPAddr *out, IROperandKind kind, const char *name) {
  if (!name || out->count >= BP_MAX_TERMS) {
    out->ok = 0;
    return;
  }
  out->terms[out->count] = name;
  out->kinds[out->count] = kind;
  out->count++;
}

static void bp_collect(const IRFunction *function, const REDefs *defs,
                       const IROperand *operand, BPAddr *out, int depth) {
  if (!out->ok) {
    return;
  }
  if (!operand || depth > BP_MAX_DEPTH) {
    out->ok = 0;
    return;
  }
  if (operand->kind == IR_OPERAND_INT) {
    out->konst += operand->int_value;
    return;
  }
  if ((operand->kind != IR_OPERAND_TEMP &&
       operand->kind != IR_OPERAND_SYMBOL) ||
      !operand->name) {
    out->ok = 0;
    return;
  }
  if (operand->kind == IR_OPERAND_SYMBOL &&
      re_symbol_is_aliasable(defs, operand->name)) {
    bp_add_term(out, operand->kind, operand->name);
    return;
  }
  {
    const IRInstruction *def =
        re_unique_def(function, defs, operand->kind, operand->name);
    if (def && def->op == IR_OP_ASSIGN &&
        (def->lhs.kind == IR_OPERAND_TEMP ||
         def->lhs.kind == IR_OPERAND_SYMBOL ||
         def->lhs.kind == IR_OPERAND_INT)) {
      bp_collect(function, defs, &def->lhs, out, depth + 1);
      return;
    }
    if (def && def->op == IR_OP_BINARY && !def->is_float && def->text &&
        strcmp(def->text, "+") == 0) {
      bp_collect(function, defs, &def->lhs, out, depth + 1);
      bp_collect(function, defs, &def->rhs, out, depth + 1);
      return;
    }
  }
  bp_add_term(out, operand->kind, operand->name);
}

static int bp_same_terms(const BPAddr *a, const BPAddr *b) {
  int used[BP_MAX_TERMS];
  if (a->count != b->count) {
    return 0;
  }
  for (int i = 0; i < BP_MAX_TERMS; i++) {
    used[i] = 0;
  }
  for (int i = 0; i < a->count; i++) {
    int found = 0;
    for (int j = 0; j < b->count && !found; j++) {
      if (!used[j] && a->kinds[i] == b->kinds[j] &&
          strcmp(a->terms[i], b->terms[j]) == 0) {
        used[j] = 1;
        found = 1;
      }
    }
    if (!found) {
      return 0;
    }
  }
  return 1;
}

static int bp_note(BPMatch *m, size_t index) {
  for (int i = 0; i < m->pattern_count; i++) {
    if (m->pattern[i] == index) {
      return 1;
    }
  }
  if (m->pattern_count >= BP_MAX_PATTERN) {
    m->ok = 0;
    return 0;
  }
  m->pattern[m->pattern_count++] = index;
  return 1;
}

static long long bp_index_of(const REDefs *defs, IROperandKind kind,
                             const char *name) {
  char key[RE_NAME_MAX];
  if (!re_name_key(key, sizeof(key), kind, name)) {
    return 0;
  }
  return re_map_get(&defs->def_at, key);
}

static const IRInstruction *bp_trace_byte_load(const IRFunction *function,
                                               const REDefs *defs,
                                               const IROperand *operand,
                                               BPMatch *m, size_t *load_at,
                                               int depth) {
  if (!operand || depth > BP_MAX_DEPTH ||
      (operand->kind != IR_OPERAND_TEMP &&
       operand->kind != IR_OPERAND_SYMBOL) ||
      !operand->name) {
    return NULL;
  }
  if (operand->kind == IR_OPERAND_SYMBOL &&
      re_symbol_is_aliasable(defs, operand->name)) {
    return NULL;
  }
  {
    long long at = bp_index_of(defs, operand->kind, operand->name);
    const IRInstruction *def =
        re_unique_def(function, defs, operand->kind, operand->name);
    if (!def || at <= 0) {
      return NULL;
    }
    if (def->op == IR_OP_LOAD) {
      if (def->is_float || !def->is_unsigned ||
          def->rhs.kind != IR_OPERAND_INT || def->rhs.int_value != 1) {
        return NULL;
      }
      if (!bp_note(m, (size_t)(at - 1))) {
        return NULL;
      }
      *load_at = (size_t)(at - 1);
      return def;
    }
    if (def->op == IR_OP_ASSIGN ||
        (def->op == IR_OP_CAST && !def->is_float &&
         subword_target_is_wider(def->text, 1))) {
      if (!bp_note(m, (size_t)(at - 1))) {
        return NULL;
      }
      return bp_trace_byte_load(function, defs, &def->lhs, m, load_at,
                                depth + 1);
    }
  }
  return NULL;
}

static void bp_expand(const IRFunction *function, const REDefs *defs,
                      const IROperand *operand, BPMatch *m, int depth) {
  if (!m->ok) {
    return;
  }
  if (depth > BP_MAX_DEPTH) {
    m->ok = 0;
    return;
  }
  if ((operand->kind == IR_OPERAND_TEMP ||
       operand->kind == IR_OPERAND_SYMBOL) &&
      operand->name &&
      !(operand->kind == IR_OPERAND_SYMBOL &&
        re_symbol_is_aliasable(defs, operand->name))) {
    long long at = bp_index_of(defs, operand->kind, operand->name);
    const IRInstruction *def =
        re_unique_def(function, defs, operand->kind, operand->name);
    if (def && at > 0 && def->op == IR_OP_BINARY && !def->is_float &&
        def->text && strcmp(def->text, "|") == 0) {
      if (!bp_note(m, (size_t)(at - 1))) {
        return;
      }
      bp_expand(function, defs, &def->lhs, m, depth + 1);
      bp_expand(function, defs, &def->rhs, m, depth + 1);
      return;
    }
    if (def && at > 0 && def->op == IR_OP_BINARY && !def->is_float &&
        def->text && strcmp(def->text, "<<") == 0 &&
        def->rhs.kind == IR_OPERAND_INT) {
      long long amount = def->rhs.int_value;
      if (amount <= 0 || amount % 8 != 0 || amount / 8 >= BP_MAX_LEAVES ||
          m->leaf_count >= BP_MAX_LEAVES) {
        m->ok = 0;
        return;
      }
      if (!bp_note(m, (size_t)(at - 1))) {
        return;
      }
      {
        size_t load_at = 0;
        const IRInstruction *load =
            bp_trace_byte_load(function, defs, &def->lhs, m, &load_at, 0);
        if (!load) {
          m->ok = 0;
          return;
        }
        m->leaves[m->leaf_count].load = load;
        m->leaves[m->leaf_count].load_at = load_at;
        m->leaves[m->leaf_count].shift = (int)(amount / 8);
        m->leaf_count++;
      }
      return;
    }
  }
  if (m->leaf_count >= BP_MAX_LEAVES) {
    m->ok = 0;
    return;
  }
  {
    size_t load_at = 0;
    const IRInstruction *load =
        bp_trace_byte_load(function, defs, operand, m, &load_at, 0);
    if (!load) {
      m->ok = 0;
      return;
    }
    m->leaves[m->leaf_count].load = load;
    m->leaves[m->leaf_count].load_at = load_at;
    m->leaves[m->leaf_count].shift = 0;
    m->leaf_count++;
  }
}

static void bp_trace(const char *why, size_t at, int detail) {
  if (getenv("METTLE_BP_TRACE")) {
    fprintf(stderr, "[bp] %zu bail=%s detail=%d\n", at, why, detail);
  }
}

static int bp_in_pattern(const BPMatch *m, size_t index) {
  for (int i = 0; i < m->pattern_count; i++) {
    if (m->pattern[i] == index) {
      return 1;
    }
  }
  return 0;
}

static long long bp_occurrences(const IRFunction *function, const char *name,
                                IROperandKind kind, const BPMatch *m,
                                int inside) {
  long long total = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (bp_in_pattern(m, i) != inside || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->dest.kind == kind && ins->dest.name &&
        strcmp(ins->dest.name, name) == 0) {
      total++;
    }
    if (ins->lhs.kind == kind && ins->lhs.name &&
        strcmp(ins->lhs.name, name) == 0) {
      total++;
    }
    if (ins->rhs.kind == kind && ins->rhs.name &&
        strcmp(ins->rhs.name, name) == 0) {
      total++;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ins->arguments[a].kind == kind && ins->arguments[a].name &&
          strcmp(ins->arguments[a].name, name) == 0) {
        total++;
      }
    }
  }
  return total;
}

static int bp_writes_name(const IRInstruction *ins, const char *name,
                          IROperandKind kind) {
  if (!ir_instruction_writes_destination(ins)) {
    return 0;
  }
  return ins->dest.kind == kind && ins->dest.name &&
         strcmp(ins->dest.name, name) == 0;
}

static int bp_span_is_clean(const IRFunction *function, size_t from, size_t to,
                            const BPAddr *addr) {
  for (size_t i = from; i <= to && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    switch (ins->op) {
    case IR_OP_NOP:
      continue;
    case IR_OP_STORE:
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_RETURN:
      return 0;
    default:
      break;
    }
    if (ir_instruction_has_side_effect(ins)) {
      return 0;
    }
    for (int t = 0; t < addr->count; t++) {
      if (bp_writes_name(ins, addr->terms[t], addr->kinds[t])) {
        return 0;
      }
    }
  }
  return 1;
}

static int bp_build_consumed(const IRFunction *function, REMap *consumed) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *sides[2];
    if (ins->op != IR_OP_BINARY || ins->is_float || !ins->text ||
        (strcmp(ins->text, "|") != 0 && strcmp(ins->text, "<<") != 0)) {
      continue;
    }
    sides[0] = &ins->lhs;
    sides[1] = &ins->rhs;
    for (int s = 0; s < 2; s++) {
      char key[RE_NAME_MAX];
      if ((sides[s]->kind != IR_OPERAND_TEMP &&
           sides[s]->kind != IR_OPERAND_SYMBOL) ||
          !re_name_key(key, sizeof(key), sides[s]->kind, sides[s]->name)) {
        continue;
      }
      if (!re_map_set(consumed, key, 1)) {
        return 0;
      }
    }
  }
  return 1;
}

static int bp_shifts_cover(const BPMatch *m, int width) {
  for (int k = 0; k < width; k++) {
    int seen = 0;
    for (int j = 0; j < width; j++) {
      if (m->leaves[j].shift == k) {
        seen++;
      }
    }
    if (seen != 1) {
      return 0;
    }
  }
  return 1;
}

static const IRInstruction *bp_pack_lowest(const IRFunction *function,
                                           const REDefs *defs,
                                           const BPMatch *m, int width,
                                           BPAddr *base, size_t *lowest_at) {
  for (int j = 0; j < width; j++) {
    if (m->leaves[j].shift != 0) {
      continue;
    }
    base->count = 0;
    base->konst = 0;
    base->ok = 1;
    bp_collect(function, defs, &m->leaves[j].load->lhs, base, 0);
    if (!base->ok) {
      return NULL;
    }
    *lowest_at = m->leaves[j].load_at;
    return m->leaves[j].load;
  }
  return NULL;
}

static int bp_pack_addresses_agree(const IRFunction *function,
                                   const REDefs *defs, const BPMatch *m,
                                   int width, const BPAddr *base,
                                   const IRInstruction *lowest, size_t at) {
  for (int j = 0; j < width; j++) {
    BPAddr here = {0};
    here.ok = 1;
    bp_collect(function, defs, &m->leaves[j].load->lhs, &here, 0);
    if (!here.ok || !bp_same_terms(base, &here) ||
        here.konst != base->konst + m->leaves[j].shift) {
      bp_trace("addr", at, j);
      return 0;
    }
    if (m->leaves[j].load->alias_class != lowest->alias_class) {
      return 0;
    }
  }
  return 1;
}

static void bp_span_bounds(const BPMatch *m, size_t at, size_t *first,
                           size_t *last) {
  *first = at;
  *last = at;
  for (int i = 0; i < m->pattern_count; i++) {
    if (m->pattern[i] < *first) {
      *first = m->pattern[i];
    }
    if (m->pattern[i] > *last) {
      *last = m->pattern[i];
    }
  }
}

static int bp_group_is_retirable(const IRFunction *function, const BPMatch *m,
                                 size_t at, const char *why) {
  for (int i = 0; i < m->pattern_count; i++) {
    const IRInstruction *ins = &function->instructions[m->pattern[i]];
    if (m->pattern[i] == at || ins->op == IR_OP_STORE) {
      continue;
    }
    if (!ir_instruction_writes_destination(ins) ||
        (ins->dest.kind != IR_OPERAND_TEMP &&
         ins->dest.kind != IR_OPERAND_SYMBOL) ||
        !ins->dest.name) {
      return 0;
    }
    if (bp_occurrences(function, ins->dest.name, ins->dest.kind, m, 0) != 0) {
      bp_trace(why, at, (int)m->pattern[i]);
      return 0;
    }
  }
  return 1;
}

static void bp_retire(IRFunction *function, const BPMatch *m, size_t at,
                      int *changed) {
  for (int i = 0; i < m->pattern_count; i++) {
    if (m->pattern[i] == at) {
      continue;
    }
    ir_instruction_destroy_storage(&function->instructions[m->pattern[i]]);
    memset(&function->instructions[m->pattern[i]], 0,
           sizeof(function->instructions[m->pattern[i]]));
    function->instructions[m->pattern[i]].op = IR_OP_NOP;
  }
  if (changed) {
    *changed = 1;
  }
}

static int bp_pack_anchor_is_root(const IRFunction *function,
                                  const REMap *consumed, size_t at) {
  const IRInstruction *terminal = &function->instructions[at];
  char key[RE_NAME_MAX];
  if (terminal->op != IR_OP_BINARY || terminal->is_float || !terminal->text ||
      strcmp(terminal->text, "|") != 0 ||
      terminal->dest.kind != IR_OPERAND_TEMP || !terminal->dest.name) {
    return 0;
  }
  if (!re_name_key(key, sizeof(key), terminal->dest.kind,
                   terminal->dest.name)) {
    return 0;
  }
  return re_map_get(consumed, key) == 0;
}

static int bp_try_at(IRFunction *function, const REDefs *defs,
                     const REMap *consumed, size_t at, int *changed) {
  IRInstruction *terminal = &function->instructions[at];
  BPMatch m = {0};
  BPAddr base = {0};
  const IRInstruction *lowest;
  size_t lowest_at = 0;
  size_t first;
  size_t last;
  int width;

  if (!bp_pack_anchor_is_root(function, consumed, at)) {
    return 0;
  }

  m.ok = 1;
  bp_expand(function, defs, &terminal->lhs, &m, 0);
  bp_expand(function, defs, &terminal->rhs, &m, 0);
  if (!m.ok) {
    bp_trace("expand", at, m.leaf_count);
    return 0;
  }
  width = m.leaf_count;
  if (width != 2 && width != 4) {
    bp_trace("width", at, width);
    return 0;
  }
  if (!bp_shifts_cover(&m, width)) {
    bp_trace("shifts", at, width);
    return 0;
  }

  lowest = bp_pack_lowest(function, defs, &m, width, &base, &lowest_at);
  if (!lowest ||
      !bp_pack_addresses_agree(function, defs, &m, width, &base, lowest, at)) {
    return 0;
  }

  if (!bp_note(&m, at)) {
    return 0;
  }
  bp_span_bounds(&m, at, &first, &last);
  if (last != at || !bp_span_is_clean(function, first, last, &base)) {
    bp_trace("span", at, (int)first);
    return 0;
  }
  if (!bp_group_is_retirable(function, &m, at, "outside-use")) {
    return 0;
  }

  {
    IROperand address = ir_operand_copy(&lowest->lhs);
    IROperand dest = ir_operand_copy(&terminal->dest);
    SourceLocation where = function->instructions[lowest_at].location;
    ir_instruction_destroy_storage(terminal);
    memset(terminal, 0, sizeof(*terminal));
    terminal->op = IR_OP_LOAD;
    terminal->dest = dest;
    terminal->lhs = address;
    terminal->rhs = ir_operand_int(width);
    terminal->is_unsigned = 1;
    terminal->alias_class = IR_ALIAS_CLASS_NONE;
    terminal->location = where;
  }
  bp_retire(function, &m, at, changed);
  return 1;
}

static int bp_is_byte_mask(const IRInstruction *ins) {
  return ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
         strcmp(ins->text, "&") == 0 && ins->rhs.kind == IR_OPERAND_INT &&
         ins->rhs.int_value == 255;
}

static int bp_trace_stored_byte(const IRFunction *function, const REDefs *defs,
                                const IROperand *operand, BPMatch *m,
                                const IROperand **root, int *shift,
                                int depth) {
  if (!operand || depth > BP_MAX_DEPTH) {
    return 0;
  }
  if ((operand->kind != IR_OPERAND_TEMP &&
       operand->kind != IR_OPERAND_SYMBOL) ||
      !operand->name) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL &&
      re_symbol_is_aliasable(defs, operand->name)) {
    *root = operand;
    return 1;
  }
  {
    long long at = bp_index_of(defs, operand->kind, operand->name);
    const IRInstruction *def =
        re_unique_def(function, defs, operand->kind, operand->name);
    if (!def || at <= 0) {
      *root = operand;
      return 1;
    }
    if (def->op == IR_OP_CAST || def->op == IR_OP_ASSIGN ||
        bp_is_byte_mask(def)) {
      if (def->is_float) {
        return 0;
      }
      if (!bp_note(m, (size_t)(at - 1))) {
        return 0;
      }
      return bp_trace_stored_byte(function, defs, &def->lhs, m, root, shift,
                                  depth + 1);
    }
    if (def->op == IR_OP_BINARY && !def->is_float && def->text &&
        strcmp(def->text, ">>") == 0 && def->rhs.kind == IR_OPERAND_INT) {
      long long amount = def->rhs.int_value;
      if (amount <= 0 || amount % 8 != 0 || *shift != 0) {
        return 0;
      }
      if (!bp_note(m, (size_t)(at - 1))) {
        return 0;
      }
      *shift = (int)(amount / 8);
      return bp_trace_stored_byte(function, defs, &def->lhs, m, root, shift,
                                  depth + 1);
    }
  }
  *root = operand;
  return 1;
}

static int bp_same_operand(const IROperand *a, const IROperand *b) {
  return a && b && a->kind == b->kind && a->name && b->name &&
         ir_operand_names_match(a, b);
}

static int bp_store_span_is_clean(const IRFunction *function, size_t from,
                                  size_t to, const BPAddr *addr,
                                  const IROperand *root,
                                  const BPMatch *m) {
  for (size_t i = from + 1; i < to && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (bp_in_pattern(m, i)) {
      continue;
    }
    switch (ins->op) {
    case IR_OP_NOP:
      continue;
    case IR_OP_LOAD:
    case IR_OP_STORE:
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_RETURN:
      return 0;
    default:
      break;
    }
    if (ir_instruction_has_side_effect(ins)) {
      return 0;
    }
    for (int t = 0; t < addr->count; t++) {
      if (bp_writes_name(ins, addr->terms[t], addr->kinds[t])) {
        return 0;
      }
    }
    if (root->name && bp_writes_name(ins, root->name, root->kind)) {
      return 0;
    }
  }
  return 1;
}

static int bp_store_gather(IRFunction *function, const REDefs *defs,
                           size_t at, const BPAddr *base,
                           const IROperand *root, BPMatch *m,
                           size_t *members) {
  const IRInstruction *anchor = &function->instructions[at];
  int width = 1;

  for (size_t i = at + 1;
       i < function->instruction_count && width < BP_MAX_LEAVES; i++) {
    const IRInstruction *ins = &function->instructions[i];
    BPAddr here = {0};
    const IROperand *here_root = NULL;
    int here_shift = 0;
    BPMatch probe;
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op != IR_OP_STORE) {
      if (ir_instruction_has_side_effect(ins) || ins->op == IR_OP_LOAD ||
          ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
          ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ ||
          ins->op == IR_OP_RETURN) {
        break;
      }
      continue;
    }
    if (ins->is_float || ins->rhs.kind != IR_OPERAND_INT ||
        ins->rhs.int_value != 1 || ins->alias_class != anchor->alias_class) {
      break;
    }
    probe = *m;
    if (!bp_trace_stored_byte(function, defs, &ins->lhs, &probe, &here_root,
                              &here_shift, 0) ||
        !here_root || !bp_same_operand(root, here_root) ||
        here_shift != width) {
      break;
    }
    here.ok = 1;
    bp_collect(function, defs, &ins->dest, &here, 0);
    if (!here.ok || !bp_same_terms(base, &here) ||
        here.konst != base->konst + width) {
      break;
    }
    *m = probe;
    if (!bp_note(m, i)) {
      return 0;
    }
    members[width] = i;
    width++;
  }
  return width;
}

static int bp_try_store_at(IRFunction *function, const REDefs *defs, size_t at,
                           int *changed) {
  IRInstruction *anchor = &function->instructions[at];
  BPMatch m = {0};
  BPAddr base = {0};
  const IROperand *root = NULL;
  size_t members[BP_MAX_LEAVES];
  int shift = 0;
  int width;

  if (anchor->op != IR_OP_STORE || anchor->is_float ||
      anchor->rhs.kind != IR_OPERAND_INT || anchor->rhs.int_value != 1) {
    return 0;
  }
  m.ok = 1;
  if (!bp_note(&m, at) ||
      !bp_trace_stored_byte(function, defs, &anchor->lhs, &m, &root, &shift,
                            0) ||
      shift != 0 || !root) {
    bp_trace("st-anchor", at, shift);
    return 0;
  }
  base.ok = 1;
  bp_collect(function, defs, &anchor->dest, &base, 0);
  if (!base.ok) {
    bp_trace("st-addr", at, 0);
    return 0;
  }
  members[0] = at;

  width = bp_store_gather(function, defs, at, &base, root, &m, members);
  if (width != 2 && width != 4) {
    bp_trace("st-width", at, width);
    return 0;
  }
  if (!bp_store_span_is_clean(function, at, members[width - 1], &base, root,
                              &m)) {
    bp_trace("st-span", at, width);
    return 0;
  }
  if (!bp_group_is_retirable(function, &m, at, "st-outside-use")) {
    return 0;
  }

  {
    IROperand value = ir_operand_copy(root);
    IROperand address = ir_operand_copy(&anchor->dest);
    SourceLocation where = anchor->location;
    ir_instruction_destroy_storage(anchor);
    memset(anchor, 0, sizeof(*anchor));
    anchor->op = IR_OP_STORE;
    anchor->dest = address;
    anchor->lhs = value;
    anchor->rhs = ir_operand_int(width);
    anchor->alias_class = IR_ALIAS_CLASS_NONE;
    anchor->location = where;
  }
  bp_retire(function, &m, at, changed);
  return 1;
}

static void bp_run(IRFunction *function, int stores, int *changed) {
  REDefs defs = {0};
  REMap consumed = {0};
  IRTempValueMap addr_taken;

  if (!ir_temp_value_map_init(&addr_taken)) {
    return;
  }
  defs.function = function;
  defs.addr_taken = &addr_taken;
  if (ir_addr_taken_set_build(function, &addr_taken) &&
      re_collect_defs(function, &defs) &&
      bp_build_consumed(function, &consumed)) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (stores) {
        bp_try_store_at(function, &defs, i, changed);
      } else {
        bp_try_at(function, &defs, &consumed, i, changed);
      }
    }
  }

  re_map_destroy(&consumed);
  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
}

static void bp_survey(const IRFunction *function, int *packs,
                      int *splits) {
  int byte_load = 0;
  int byte_store = 0;
  int or_chain = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->is_float) {
      continue;
    }
    if ((ins->op == IR_OP_LOAD || ins->op == IR_OP_STORE) &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == 1) {
      if (ins->op == IR_OP_LOAD) {
        byte_load = 1;
      } else {
        byte_store = 1;
      }
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "|") == 0) {
      or_chain = 1;
    }
  }
  *packs = byte_load && or_chain;
  *splits = byte_store;
}

int ir_widen_byte_pack_pass(IRFunction *function, int *changed) {
  int packs = 0;
  int splits = 0;
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  bp_survey(function, &packs, &splits);
  if (packs) {
    bp_run(function, 0, changed);
  }
  if (splits) {
    bp_run(function, 1, changed);
  }
  return 1;
}

static int re_store_is_const_word(const IRInstruction *ins) {
  return ins->op == IR_OP_STORE && !ins->is_volatile && !ins->is_float &&
         ins->lhs.kind == IR_OPERAND_INT && ins->rhs.kind == IR_OPERAND_INT &&
         ins->rhs.int_value == 4 &&
         ins->lhs.int_value >= -2147483648LL &&
         ins->lhs.int_value <= 4294967295LL;
}

static int re_op_is_pure_between_stores(IROpcode op) {
  return op == IR_OP_NOP || op == IR_OP_BINARY || op == IR_OP_ASSIGN ||
         op == IR_OP_CAST || op == IR_OP_ADDRESS_OF;
}

static int re_merge_store_pair(IRFunction *function, const REDefs *defs,
                               size_t first, int *changed) {
  IRInstruction *lo = &function->instructions[first];
  BPAddr lo_addr = {0};

  if (!re_store_is_const_word(lo)) {
    return 0;
  }
  lo_addr.ok = 1;
  bp_collect(function, defs, &lo->dest, &lo_addr, 0);
  if (!lo_addr.ok || lo_addr.count == 0) {
    return 0;
  }

  for (size_t j = first + 1; j < function->instruction_count; j++) {
    IRInstruction *hi = &function->instructions[j];
    BPAddr hi_addr = {0};
    long long merged;

    if (hi->op != IR_OP_STORE) {
      if (re_op_is_pure_between_stores(hi->op)) {
        continue;
      }
      return 0;
    }
    if (!re_store_is_const_word(hi)) {
      return 0;
    }
    hi_addr.ok = 1;
    bp_collect(function, defs, &hi->dest, &hi_addr, 0);
    if (!hi_addr.ok || !bp_same_terms(&lo_addr, &hi_addr) ||
        hi_addr.konst != lo_addr.konst + 4) {
      return 0;
    }

    merged = (long long)(((unsigned long long)hi->lhs.int_value << 32) |
                         ((unsigned long long)lo->lhs.int_value & 0xffffffffULL));
    /* Widening the pair only removes an instruction while the joined constant
       still fits an immediate the emitters place directly; anything else
       trades two stores for a store plus a materialization. */
    if (merged != (long long)(int)merged) {
      return 0;
    }

    lo->lhs = ir_operand_int(merged);
    lo->rhs = ir_operand_int(8);
    lo->alias_class = IR_ALIAS_CLASS_NONE;
    lo->value_type = NULL;
    lo->is_unsigned = 0;
    ir_instruction_make_nop(hi);
    if (changed) {
      *changed = 1;
    }
    return 1;
  }
  return 0;
}

int ir_merge_adjacent_const_stores_pass(IRFunction *function, int *changed) {
  REDefs defs = {0};
  IRTempValueMap addr_taken;

  if (!function || function->instruction_count == 0) {
    return 1;
  }
  if (!ir_temp_value_map_init(&addr_taken)) {
    return 1;
  }
  defs.function = function;
  defs.addr_taken = &addr_taken;
  if (ir_addr_taken_set_build(function, &addr_taken) &&
      re_collect_defs(function, &defs)) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      while (re_merge_store_pair(function, &defs, i, changed)) {
      }
    }
  }
  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}

typedef struct {
  char *base;
  long long off;
  long long size;
  unsigned char alias_class;
  unsigned char from_load;
  unsigned char is_unsigned;
  unsigned char is_float;
  int float_bits;
  IROperand value;
} FwdFact;

typedef struct {
  FwdFact *items;
  size_t count;
  size_t capacity;
  int top;
} FwdSet;

typedef struct {
  IRFunction *function;
  const REDefs *defs;
  const IRTempValueMap *addr_taken;
  int *changed;
} FwdCtx;

static void fwd_fact_release(FwdFact *fact) {
  free(fact->base);
  fact->base = NULL;
  ir_operand_destroy(&fact->value);
}

static void fwd_set_clear(FwdSet *set) {
  for (size_t i = 0; i < set->count; i++) {
    fwd_fact_release(&set->items[i]);
  }
  set->count = 0;
}

static void fwd_set_destroy(FwdSet *set) {
  fwd_set_clear(set);
  free(set->items);
  set->items = NULL;
  set->capacity = 0;
}

static int fwd_fact_equal(const FwdFact *a, const FwdFact *b) {
  return a->off == b->off && a->size == b->size &&
         a->from_load == b->from_load && a->is_unsigned == b->is_unsigned &&
         a->is_float == b->is_float && a->float_bits == b->float_bits &&
         a->alias_class == b->alias_class && strcmp(a->base, b->base) == 0 &&
         ir_operand_same(&a->value, &b->value);
}

static int fwd_set_contains(const FwdSet *set, const FwdFact *fact) {
  for (size_t i = 0; i < set->count; i++) {
    if (fwd_fact_equal(&set->items[i], fact)) {
      return 1;
    }
  }
  return 0;
}

static const FwdFact *fwd_set_find(const FwdSet *set, const char *base,
                                   long long off, long long size) {
  for (size_t i = set->count; i-- > 0;) {
    const FwdFact *fact = &set->items[i];
    if (fact->off == off && fact->size == size &&
        strcmp(fact->base, base) == 0) {
      return fact;
    }
  }
  return NULL;
}

static int fwd_set_add(FwdSet *set, const FwdFact *fact) {
  if (set->count == set->capacity) {
    size_t capacity = set->capacity ? set->capacity * 2 : 16;
    FwdFact *grown = realloc(set->items, capacity * sizeof(FwdFact));
    if (!grown) {
      return 0;
    }
    set->items = grown;
    set->capacity = capacity;
  }
  FwdFact *slot = &set->items[set->count];
  memset(slot, 0, sizeof(*slot));
  slot->base = mettle_strdup(fact->base);
  if (!slot->base || !ir_operand_clone(&fact->value, &slot->value)) {
    fwd_fact_release(slot);
    return 0;
  }
  slot->off = fact->off;
  slot->size = fact->size;
  slot->alias_class = fact->alias_class;
  slot->from_load = fact->from_load;
  slot->is_unsigned = fact->is_unsigned;
  slot->is_float = fact->is_float;
  slot->float_bits = fact->float_bits;
  set->count++;
  return 1;
}

static int fwd_set_copy(FwdSet *dst, const FwdSet *src) {
  fwd_set_clear(dst);
  dst->top = src->top;
  for (size_t i = 0; i < src->count; i++) {
    if (!fwd_set_add(dst, &src->items[i])) {
      return 0;
    }
  }
  return 1;
}

static int fwd_set_meet(FwdSet *dst, const FwdSet *src) {
  if (src->top) {
    return 1;
  }
  if (dst->top) {
    return fwd_set_copy(dst, src);
  }
  size_t write = 0;
  for (size_t read = 0; read < dst->count; read++) {
    if (!fwd_set_contains(src, &dst->items[read])) {
      fwd_fact_release(&dst->items[read]);
      continue;
    }
    if (write != read) {
      dst->items[write] = dst->items[read];
    }
    write++;
  }
  dst->count = write;
  return 1;
}

static int fwd_set_equal(const FwdSet *a, const FwdSet *b) {
  if (a->top != b->top) {
    return 0;
  }
  if (a->top) {
    return 1;
  }
  if (a->count != b->count) {
    return 0;
  }
  for (size_t i = 0; i < a->count; i++) {
    if (!fwd_set_contains(b, &a->items[i])) {
      return 0;
    }
  }
  return 1;
}

static void fwd_kill_region(FwdSet *set, const IRFunction *function,
                            const char *base, long long off, long long size,
                            unsigned alias_class) {
  REMemRegion region;
  region.base = (char *)base;
  region.off = off;
  region.size = size;
  region.alias_class = (unsigned char)alias_class;
  size_t write = 0;
  for (size_t read = 0; read < set->count; read++) {
    FwdFact *fact = &set->items[read];
    if (re_kill_hits(function, &region, fact->base, fact->off, fact->size,
                     fact->alias_class)) {
      fwd_fact_release(fact);
      continue;
    }
    if (write != read) {
      set->items[write] = *fact;
    }
    write++;
  }
  set->count = write;
}

static void fwd_kill_name(FwdSet *set, IROperandKind kind, const char *name) {
  char prefix = kind == IR_OPERAND_SYMBOL ? 's' : 't';
  if (!name || (kind != IR_OPERAND_SYMBOL && kind != IR_OPERAND_TEMP)) {
    return;
  }
  size_t write = 0;
  for (size_t read = 0; read < set->count; read++) {
    FwdFact *fact = &set->items[read];
    int names_base = fact->base[0] == prefix && strcmp(fact->base + 1, name) == 0;
    int names_value = fact->value.kind == kind && fact->value.name &&
                      strcmp(fact->value.name, name) == 0;
    if (names_base || names_value) {
      fwd_fact_release(fact);
      continue;
    }
    if (write != read) {
      set->items[write] = *fact;
    }
    write++;
  }
  set->count = write;
}

static int fwd_addr_base(const REAddr *addr, char *out, size_t size) {
  return snprintf(out, size, "%c%s",
                  addr->is_address_of
                      ? '&'
                      : (addr->kind == IR_OPERAND_SYMBOL ? 's' : 't'),
                  addr->name) < (int)size;
}

static int fwd_value_is_trackable(const FwdCtx *ctx, const IROperand *value) {
  if (value->float_bits != 0) {
    return 0;
  }
  if (value->kind == IR_OPERAND_INT) {
    return 1;
  }
  if (value->kind == IR_OPERAND_TEMP) {
    return value->name != NULL;
  }
  if (value->kind == IR_OPERAND_SYMBOL) {
    return value->name && !re_symbol_is_aliasable(ctx->defs, value->name);
  }
  return 0;
}

static const char *fwd_cast_name(long long size, int is_unsigned) {
  switch (size) {
  case 1:
    return is_unsigned ? "uint8" : "int8";
  case 2:
    return is_unsigned ? "uint16" : "int16";
  case 4:
    return is_unsigned ? "uint32" : "int32";
  default:
    return NULL;
  }
}

static long long fwd_truncate(long long value, long long size, int is_unsigned) {
  if (size >= 8) {
    return value;
  }
  int bits = (int)size * 8;
  unsigned long long mask = (1ull << bits) - 1ull;
  unsigned long long low = (unsigned long long)value & mask;
  if (is_unsigned || !(low & (1ull << (bits - 1)))) {
    return (long long)low;
  }
  return (long long)(low | ~mask);
}

static int fwd_load_can_use(const IRInstruction *load, long long size,
                            const FwdFact *fact) {
  if (fact->from_load) {
    return fact->is_unsigned == load->is_unsigned &&
           fact->is_float == load->is_float &&
           fact->float_bits == load->float_bits;
  }
  if (load->is_float) {
    return 0;
  }
  return size == 8 || fwd_cast_name(size, load->is_unsigned) != NULL;
}

static int fwd_rewrite_load(FwdCtx *ctx, IRInstruction *ins, long long size,
                            const FwdFact *fact) {
  int keep_unsigned = ins->is_unsigned;
  int keep_float = ins->is_float;
  int keep_bits = ins->float_bits;
  MtlcType *keep_type = ins->value_type;

  if (fact->value.kind == IR_OPERAND_INT) {
    long long value = fact->from_load
                          ? fact->value.int_value
                          : fwd_truncate(fact->value.int_value, size,
                                         ins->is_unsigned);
    if (!ir_rewrite_to_assign_int(ins, value, ctx->changed)) {
      return 0;
    }
    ins->is_unsigned = keep_unsigned;
    ins->is_float = keep_float;
    ins->float_bits = keep_bits;
    ins->value_type = keep_type;
    return 1;
  }

  int self_copy = fact->value.kind == ins->dest.kind && ins->dest.name &&
                  fact->value.name &&
                  ir_operand_names_match(&fact->value, &ins->dest);
  if (fact->from_load || size == 8) {
    if (self_copy) {
      ir_instruction_destroy_storage(ins);
      memset(ins, 0, sizeof(*ins));
      ins->op = IR_OP_NOP;
      if (ctx->changed) {
        *ctx->changed = 1;
      }
      return 1;
    }
    if (!ir_rewrite_to_assign_operand(ins, &fact->value, ctx->changed)) {
      return 0;
    }
    ins->is_unsigned = keep_unsigned;
    ins->is_float = keep_float;
    ins->float_bits = keep_bits;
    ins->value_type = keep_type;
    return 1;
  }

  const char *cast_name = fwd_cast_name(size, ins->is_unsigned);
  IROperand source = ir_operand_none();
  char *text = cast_name ? mettle_strdup(cast_name) : NULL;
  if (!text || !ir_operand_clone(&fact->value, &source)) {
    if (text) {
      mettle_free_string(text);
    }
    return 0;
  }
  ir_operand_destroy(&ins->lhs);
  ir_operand_destroy(&ins->rhs);
  ir_instruction_clear_arguments(ins);
  if (ins->text) {
    mettle_free_string(ins->text);
  }
  ins->op = IR_OP_CAST;
  ins->lhs = source;
  ins->rhs = ir_operand_none();
  ins->text = text;
  ins->is_float = 0;
  ins->float_bits = 0;
  ins->is_unsigned = keep_unsigned;
  ins->value_type = keep_type;
  ins->ast_ref = NULL;
  if (ctx->changed) {
    *ctx->changed = 1;
  }
  return 1;
}

static int fwd_transfer(FwdCtx *ctx, const IRBasicBlock *block, FwdSet *set,
                        int rewrite) {
  for (size_t i = 0; i < block->instruction_count; i++) {
    IRInstruction *ins = &block->instructions[i];
    char wbase[RE_NAME_MAX + 1];
    long long woff = 0;
    long long wsize = RE_MEM_WHOLE;
    unsigned wtype = IR_ALIAS_CLASS_NONE;
    if (ins->op == IR_OP_NOP) {
      continue;
    }

    if (ins->op == IR_OP_STORE) {
      re_instruction_write_region(ctx->function, ctx->defs, ctx->addr_taken,
                                  ins, wbase, sizeof(wbase), &woff, &wsize,
                                  &wtype);
      fwd_kill_region(set, ctx->function, wbase[0] ? wbase : NULL, woff, wsize,
                      wtype);
      if (!ins->is_volatile && !ins->is_float && wbase[0] &&
          ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value > 0 &&
          ins->rhs.int_value <= 8 &&
          fwd_value_is_trackable(ctx, &ins->lhs)) {
        FwdFact fact;
        memset(&fact, 0, sizeof(fact));
        fact.base = wbase;
        fact.off = woff;
        fact.size = wsize;
        fact.alias_class = ins->alias_class;
        fact.value = ins->lhs;
        if (!fwd_set_add(set, &fact)) {
          return 0;
        }
      }
      continue;
    }

    if (ins->op == IR_OP_LOAD) {
      REAddr addr;
      char base[RE_NAME_MAX + 1];
      int have = 0;
      int hit = 0;
      long long size =
          ins->rhs.kind == IR_OPERAND_INT ? ins->rhs.int_value : 0;
      memset(&addr, 0, sizeof(addr));
      if (!ins->is_volatile && size > 0 && size <= 8 && ins->dest.name &&
          (ins->dest.kind == IR_OPERAND_TEMP ||
           ins->dest.kind == IR_OPERAND_SYMBOL)) {
        re_resolve_addr(ctx->function, ctx->defs, &ins->lhs, &addr, 0);
        have = addr.valid && addr.name && fwd_addr_base(&addr, base, sizeof(base));
      }
      if (have) {
        const FwdFact *fact = fwd_set_find(set, base, addr.offset, size);
        if (fact && fwd_load_can_use(ins, size, fact)) {
          hit = 1;
          if (rewrite && !fwd_rewrite_load(ctx, ins, size, fact)) {
            return 0;
          }
        }
      }
      if (ins->op == IR_OP_NOP) {
        continue;
      }
      if (re_instruction_write_region(ctx->function, ctx->defs,
                                      ctx->addr_taken, ins, wbase,
                                      sizeof(wbase), &woff, &wsize, &wtype)) {
        fwd_kill_region(set, ctx->function, wbase[0] ? wbase : NULL, woff,
                        wsize, wtype);
      }
      fwd_kill_name(set, ins->dest.kind, ins->dest.name);
      if (have && !hit && fwd_value_is_trackable(ctx, &ins->dest)) {
        FwdFact fact;
        memset(&fact, 0, sizeof(fact));
        fact.base = base;
        fact.off = addr.offset;
        fact.size = size;
        fact.alias_class = ins->alias_class;
        fact.from_load = 1;
        fact.is_unsigned = (unsigned char)(ins->is_unsigned != 0);
        fact.is_float = (unsigned char)(ins->is_float != 0);
        fact.float_bits = ins->float_bits;
        fact.value = ins->dest;
        if (!fwd_set_add(set, &fact)) {
          return 0;
        }
      }
      continue;
    }

    if (re_instruction_write_region(ctx->function, ctx->defs, ctx->addr_taken,
                                    ins, wbase, sizeof(wbase), &woff, &wsize,
                                    &wtype)) {
      fwd_kill_region(set, ctx->function, wbase[0] ? wbase : NULL, woff, wsize,
                      wtype);
    }
    if (!re_opcode_is_scalar(ins->op)) {
      fwd_set_clear(set);
      continue;
    }
    if (ir_instruction_writes_destination(ins) && ins->dest.name) {
      fwd_kill_name(set, ins->dest.kind, ins->dest.name);
    }
  }
  return 1;
}

static int fwd_block_input(FwdCtx *ctx, const IRBasicBlock *blocks,
                           size_t block, const FwdSet *out, FwdSet *in) {
  fwd_set_clear(in);
  in->top = block != ctx->function->entry_block;
  for (size_t p = 0; p < blocks[block].predecessor_count; p++) {
    size_t pred = blocks[block].predecessors[p];
    if (pred >= ctx->function->block_count) {
      continue;
    }
    if (!fwd_set_meet(in, &out[pred])) {
      return 0;
    }
  }
  if (in->top) {
    in->top = 0;
    fwd_set_clear(in);
  }
  return 1;
}

static size_t fwd_reverse_postorder(const IRBasicBlock *blocks,
                                    size_t block_count, size_t entry,
                                    size_t *order) {
  size_t *stack = malloc(block_count * sizeof(size_t));
  size_t *next_succ = malloc(block_count * sizeof(size_t));
  unsigned char *seen = calloc(block_count, 1);
  size_t post_count = 0;
  if (!stack || !next_succ || !seen) {
    free(stack);
    free(next_succ);
    free(seen);
    return 0;
  }
  size_t depth = 1;
  stack[0] = entry;
  next_succ[0] = 0;
  seen[entry] = 1;
  while (depth > 0) {
    size_t block = stack[depth - 1];
    if (next_succ[depth - 1] < blocks[block].successor_count) {
      size_t successor = blocks[block].successors[next_succ[depth - 1]++];
      if (successor < block_count && !seen[successor]) {
        seen[successor] = 1;
        stack[depth] = successor;
        next_succ[depth] = 0;
        depth++;
      }
      continue;
    }
    order[post_count++] = block;
    depth--;
  }
  for (size_t i = 0; i < post_count / 2; i++) {
    size_t tmp = order[i];
    order[i] = order[post_count - 1 - i];
    order[post_count - 1 - i] = tmp;
  }
  free(stack);
  free(next_succ);
  free(seen);
  return post_count;
}

#define FWD_MAX_ITERATIONS 48

int ir_forward_stored_values_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  ir_function_clear_cfg(function);
  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!blocks || block_count == 0 || function->entry_block >= block_count) {
    return 1;
  }

  REDefs defs = {0};
  IRTempValueMap addr_taken;
  if (!ir_temp_value_map_init(&addr_taken)) {
    return 1;
  }
  defs.function = function;
  defs.addr_taken = &addr_taken;
  int ok = ir_addr_taken_set_build(function, &addr_taken) &&
           re_collect_defs(function, &defs);

  FwdSet *out = calloc(block_count, sizeof(FwdSet));
  size_t *order = malloc(block_count * sizeof(size_t));
  size_t order_count = 0;
  FwdSet in = {0};
  FwdCtx ctx;
  ctx.function = function;
  ctx.defs = &defs;
  ctx.addr_taken = &addr_taken;
  ctx.changed = changed;
  if (!out || !order) {
    ok = 0;
  }
  if (ok) {
    order_count = fwd_reverse_postorder(blocks, block_count,
                                        function->entry_block, order);
    ok = order_count > 0;
  }
  for (size_t b = 0; ok && b < block_count; b++) {
    out[b].top = 1;
  }

  int converged = 0;
  for (int iteration = 0; ok && !converged && iteration < FWD_MAX_ITERATIONS;
       iteration++) {
    converged = 1;
    for (size_t k = 0; ok && k < order_count; k++) {
      size_t b = order[k];
      ok = fwd_block_input(&ctx, blocks, b, out, &in) &&
           fwd_transfer(&ctx, &blocks[b], &in, 0);
      if (ok && !fwd_set_equal(&in, &out[b])) {
        ok = fwd_set_copy(&out[b], &in);
        converged = 0;
      }
    }
  }

  if (getenv("METTLE_FWD_TRACE")) {
    fprintf(stderr, "[fwd] %s: ok=%d converged=%d blocks=%zu\n",
            function->name ? function->name : "?", ok, converged, block_count);
  }
  if (ok && converged) {
    for (size_t k = 0; ok && k < order_count; k++) {
      size_t b = order[k];
      ok = fwd_block_input(&ctx, blocks, b, out, &in) &&
           fwd_transfer(&ctx, &blocks[b], &in, 1);
    }
  }

  fwd_set_destroy(&in);
  if (out) {
    for (size_t b = 0; b < block_count; b++) {
      fwd_set_destroy(&out[b]);
    }
  }
  free(out);
  free(order);
  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}
