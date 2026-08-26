/* Redundancy elimination over the dominator tree.
 *
 * The block-local CSE in ir_optimize_core clears its map at every label, call,
 * jump, branch and return, and never admits a LOAD. Every hot loop in an
 * application-shaped program is branchy, so field reads through a struct
 * pointer survive to codegen one per iteration: skip_ws reloaded p->pos,
 * p->len and p->text for every whitespace character it stepped over.
 *
 * This walks the dominator tree with a scoped table. Two rules keep it sound
 * without an SSA form and without an alias analysis:
 *
 *   - A value crosses a block boundary only when every name it depends on is
 *     one the function never writes (a parameter, or the address of a local).
 *     Nothing can invalidate such an entry, so the walk back up the tree needs
 *     no un-invalidation.
 *   - Every value read from memory carries the region it was read from, and
 *     every write appends the region it wrote to a kill log. A store through
 *     one base invalidates only what it can reach: same base and overlapping
 *     offsets, or a base that might alias. Entering a block with more than one
 *     predecessor accounts for the paths this walk never visits by requiring
 *     survivors to be untouched by ANY store the function makes.
 *
 * Addresses are canonicalized to (base, byte offset) through the `%t = base +
 * k` chains the frontend emits for field access, so two reads of one field
 * match whichever temporaries each spelled it through.
 */

#include "ir_optimize_internal.h"

#define RE_MAX_ADDR_DEPTH 8
#define RE_KEY_MAX 224
#define RE_NAME_MAX 128

/* ---------------------------------------------------------------- name map */

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
    if (strcmp(map->keys[slot], key) == 0) {
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

/* A name lives in one of two spaces; the tag keeps `@i` and `%i` apart. */
static int re_name_key(char *out, size_t size, IROperandKind kind,
                       const char *name) {
  if (!name) {
    return 0;
  }
  char tag = (kind == IR_OPERAND_SYMBOL) ? 's' : 't';
  int written = snprintf(out, size, "%c%s", tag, name);
  return written > 0 && (size_t)written < size;
}

/* ------------------------------------------------------------- def counting */

typedef struct {
  REMap defs;   /* name key -> number of instructions that write it */
  REMap def_at; /* name key -> the writing instruction's index, plus one */
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

/* Count every write. The kernels outside the scalar set can name their
 * outputs in dest, lhs, or rhs, so all three count there: an over-counted
 * name simply stops crossing blocks. */
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
    return 2; /* unnameable: treat as written more than once */
  }
  return re_map_get(&defs->defs, key);
}

/* A symbol a store can reach: a global (any callee may assign it) or a local
 * whose address escaped (a write through the pointer never names it). Its
 * value cannot be cached across anything that writes memory. */
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

/* Stable means nothing in the program can change what this name holds, so an
 * entry built on it needs no invalidation at all. */
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

/* ---------------------------------------------------------- address folding */

typedef struct {
  int is_address_of;   /* base is &@sym, a fixed stack slot */
  IROperandKind kind;  /* when not an address-of: the base operand's kind */
  const char *name;
  long long offset;
  int portable;  /* the base cannot change, so the entry can cross blocks */
  int aliasable; /* the base itself is reachable by a store */
  int valid;
} REAddr;

static void re_resolve_addr(const IRFunction *function, const REDefs *defs,
                            const IROperand *operand, REAddr *out, int depth) {
  out->valid = 0;
  if (!operand || depth > RE_MAX_ADDR_DEPTH) {
    return;
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    /* A local written exactly once is as good as a temp: look through its one
     * definition. Inlining rewrites every pointer parameter into this shape
     * (`@__inl_N_param_p <- %t`), and stopping here made every inlined body
     * opaque to the loop passes. */
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

/* ------------------------------------------------------------------- keying */

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

/* An operand can appear in an entry that outlives its block only when nothing
 * can change what it names. */
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

/* Reading this operand can give a different answer after a memory write. */
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

/* ------------------------------------------------------------- entry tables */

#define RE_MEM_WHOLE (1LL << 40)

/* One byte range of memory, named by the base it is reached through: '&name'
 * is the storage of a variable, 's'/'t' + name is memory addressed by a
 * pointer-valued symbol or temp. Two '&' bases with different names are
 * distinct objects; every other pair may alias. */
typedef struct {
  char *base; /* NULL = every location */
  long long off;
  long long size;
  /* What class of value the access moves, so a store of one class can be told
   * from a slot holding another. 0 when lowering did not record it. */
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

/* Does a write to `kill` invalidate a value read from `mem`? */
static int re_kill_hits(const IRFunction *function, const REMemRegion *kill,
                        const char *mem_base, long long mem_off,
                        long long mem_size, unsigned mem_class) {
  if (!kill->base || !mem_base) {
    return 1;
  }
  if (strcmp(kill->base, mem_base) == 0) {
    return kill->off < mem_off + mem_size && mem_off < kill->off + kill->size;
  }
  if (kill->base[0] == '&' && mem_base[0] == '&') {
    return 0; /* two distinct variables cannot overlap */
  }
  /* Different classes, in a program that never views one address as both. */
  if (ir_alias_classes_distinct(kill->alias_class, mem_class)) {
    return 0;
  }
  /* Two pointers the whole program proves reach different allocations. This is
   * the rule above carried across a call: the caller knew the arguments were
   * distinct variables, and the callee sees only parameters. */
  if (ir_alias_bases_distinct(function, kill->base, mem_base)) {
    return 0;
  }
  return 1;
}

typedef struct {
  char *key;
  char *value;    /* the temp that already holds this value */
  char *dep[2];   /* names the entry reads, for block-local invalidation */
  char *mem_base; /* what the value was read from; NULL = anywhere */
  long long mem_off;
  long long mem_size;
  unsigned char mem_class;
  size_t kill_pos;      /* kill-log length when recorded */
  unsigned merge_epoch; /* merge count when recorded */
  int survives_summary; /* no store anywhere in the function can change it */
  int volatile_value;   /* a memory write can change it: load, aliasable read */
} REEntry;

/* Entries own their strings. A name borrowed from an operand dies the moment
 * this pass rewrites the instruction that held it. */
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

/* A volatile entry still holds its value when nothing written since it was
 * recorded overlaps what it read: not one of the targeted kills behind it in
 * the log, and, if a merge has been crossed (a path this walk never visited
 * joins back in), nothing the whole function can store. */
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

/* Drop the block-local entries that read `name`. Entries in the cross-block
 * table read nothing the function writes, so they never reach here. */
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

/* ------------------------------------------------------------ dominator tree */

typedef struct {
  size_t *idom;
  size_t *rpo_index;
  size_t *order;      /* reverse postorder */
  size_t order_count;
  size_t *child_head; /* first child, or SIZE_MAX */
  size_t *child_next;
} REDom;

static void re_dom_destroy(REDom *dom) {
  free(dom->idom);
  free(dom->rpo_index);
  free(dom->order);
  free(dom->child_head);
  free(dom->child_next);
  memset(dom, 0, sizeof(*dom));
}

static int re_dom_build(const IRBasicBlock *blocks, size_t block_count,
                        size_t entry, REDom *dom) {
  memset(dom, 0, sizeof(*dom));
  if (block_count == 0 || entry >= block_count) {
    return 0;
  }

  dom->idom = malloc(block_count * sizeof(size_t));
  dom->rpo_index = malloc(block_count * sizeof(size_t));
  dom->order = malloc(block_count * sizeof(size_t));
  dom->child_head = malloc(block_count * sizeof(size_t));
  dom->child_next = malloc(block_count * sizeof(size_t));
  size_t *stack = malloc(block_count * sizeof(size_t));
  size_t *next_succ = malloc(block_count * sizeof(size_t));
  unsigned char *seen = calloc(block_count, 1);
  size_t *postorder = malloc(block_count * sizeof(size_t));
  if (!dom->idom || !dom->rpo_index || !dom->order || !dom->child_head ||
      !dom->child_next || !stack || !next_succ || !seen || !postorder) {
    free(stack);
    free(next_succ);
    free(seen);
    free(postorder);
    re_dom_destroy(dom);
    return 0;
  }

  for (size_t i = 0; i < block_count; i++) {
    dom->idom[i] = SIZE_MAX;
    dom->rpo_index[i] = SIZE_MAX;
    dom->child_head[i] = SIZE_MAX;
    dom->child_next[i] = SIZE_MAX;
  }

  /* Iterative DFS postorder from the entry. */
  size_t post_count = 0;
  size_t depth = 0;
  stack[depth] = entry;
  next_succ[depth] = 0;
  seen[entry] = 1;
  depth = 1;
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
    postorder[post_count++] = block;
    depth--;
  }

  dom->order_count = post_count;
  for (size_t i = 0; i < post_count; i++) {
    dom->order[i] = postorder[post_count - 1 - i];
    dom->rpo_index[dom->order[i]] = i;
  }
  free(postorder);
  free(stack);
  free(next_succ);
  free(seen);

  dom->idom[entry] = entry;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t k = 0; k < dom->order_count; k++) {
      size_t block = dom->order[k];
      if (block == entry) {
        continue;
      }
      size_t candidate = SIZE_MAX;
      for (size_t p = 0; p < blocks[block].predecessor_count; p++) {
        size_t pred = blocks[block].predecessors[p];
        if (pred >= block_count || dom->idom[pred] == SIZE_MAX) {
          continue;
        }
        if (candidate == SIZE_MAX) {
          candidate = pred;
          continue;
        }
        size_t a = pred;
        size_t b = candidate;
        while (a != b) {
          while (dom->rpo_index[a] > dom->rpo_index[b]) {
            a = dom->idom[a];
          }
          while (dom->rpo_index[b] > dom->rpo_index[a]) {
            b = dom->idom[b];
          }
        }
        candidate = a;
      }
      if (candidate != SIZE_MAX && dom->idom[block] != candidate) {
        dom->idom[block] = candidate;
        changed = 1;
      }
    }
  }

  /* Children, in reverse so the walk visits them in block order. */
  for (size_t k = dom->order_count; k-- > 0;) {
    size_t block = dom->order[k];
    if (block == entry || dom->idom[block] == SIZE_MAX) {
      continue;
    }
    size_t parent = dom->idom[block];
    dom->child_next[block] = dom->child_head[parent];
    dom->child_head[parent] = block;
  }
  return 1;
}

/* --------------------------------------------------------------- the walker */

typedef struct {
  IRFunction *function;
  const REDefs *defs;
  const IRTempValueMap *addr_taken;
  const IRBasicBlock *blocks;
  size_t block_count;
  RETable global;
  RETable local;
  REKillLog kills;      /* every write the walk has passed, in order */
  REKillLog summary;    /* every targeted write anywhere in the function */
  int summary_unknown;  /* the function has a write no region describes */
  unsigned merge_epoch;
  int *changed;
  int failed;
} REWalk;

/* What this instruction writes: 0 = nothing, 1 = the region in `out`
 * (base NULL when it could be anywhere). */
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
      base[0] = '\0'; /* a store to somewhere: kill everything */
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
  /* A select writes its destination and nothing else, exactly like the
   * binary it replaced. Falling through to the default below told every
   * cached load that memory had changed. */
  case IR_OP_SELECT:
    /* Writing a variable a pointer can reach (a global, or a local whose
     * address escaped) is a store to that variable's storage. */
    if (ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
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
    /* A hint with no architectural effect. It was landing in the default
     * arm and killing every cached load, so the prefetch pass made the loop
     * it was meant to speed up reload each base it had already read: the
     * string-hash loop in json_parse walk fetched p->text twice an
     * iteration, once on each side of the hint. */
    return 0;
  default:
    return 1; /* calls, kernels: anywhere */
  }
}

static int re_load_key(const REWalk *walk, const IRInstruction *ins,
                       char *key, size_t size, REAddr *addr) {
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

/* Rewrite a redundant computation to a copy of the value already in hand. The
 * result flags stay on the copy: an ASSIGN carries the width and signedness
 * the backend needs to pick a register class. */
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

/* Would any store the function makes, anywhere, hit this region? Decides
 * whether an entry may outlive a merge point. */
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
                             const REDom *dom) {
  if (walk->failed) {
    return;
  }
  const IRBasicBlock *block = &walk->blocks[block_index];
  size_t global_mark = walk->global.count;
  re_table_truncate(&walk->local, 0);

  /* A store on a path this walk never took has to reach a merge that dominates
   * the use; the merge is here. Only entries no store in the whole function
   * can touch may cross one. */
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
          /* Only loads cross a block boundary. Address arithmetic is one
           * instruction to recompute and a live register to carry, and
           * carrying it measured slower on the interpreter benchmark. */
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

  for (size_t child = dom->child_head[block_index]; child != SIZE_MAX;
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

  /* Rebuild rather than trust the cached graph: a pass that rewrites an
   * instruction in place leaves cfg_valid set, and the block pointers go
   * stale the moment one of them grows the instruction array. */
  ir_function_clear_cfg(function);
  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!blocks || block_count == 0) {
    return 1;
  }

  REDefs defs = {0};
  REDom dom = {0};
  IRTempValueMap addr_taken;
  int ok = 1;

  if (!ir_temp_value_map_init(&addr_taken)) {
    return 1;
  }
  defs.function = function;
  defs.addr_taken = &addr_taken;
  if (!ir_addr_taken_set_build(function, &addr_taken) ||
      !re_collect_defs(function, &defs) ||
      !re_dom_build(blocks, block_count, function->entry_block, &dom)) {
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
    /* The function-wide write summary: what a value must survive to be
     * trusted across a merge point. */
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
      re_process_block(&walk, function->entry_block, &dom);
    }
    re_table_destroy(&walk.global);
    re_table_destroy(&walk.local);
    re_kills_destroy(&walk.kills);
    re_kills_destroy(&walk.summary);
  }

  re_dom_destroy(&dom);
  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Selecting between two fields of one object                                  */
/*                                                                             */
/* A Huffman decoder walks its tree with                                       */
/*     if (bit != 0) node = t[node].right; else node = t[node].left;           */
/* and the bit is random by construction, so the branch mispredicts about half  */
/* the time. Both arms compute the same address and read the same width; they   */
/* differ in one constant, because the two fields are neighbours. Selecting the */
/* constant in place of the path turns the pair into a single load at           */
/* `base + else_offset + bit * (then_offset - else_offset)`, which is the form  */
/* clang reaches through `bt` and `setb`.                                       */
/*                                                                             */
/* The match is structural: the two arms have to be isomorphic instruction for  */
/* instruction, pure apart from the one value they both produce, and differ in  */
/* exactly one integer operand. Nothing here reads a field offset or a type, so  */
/* it fires on any two-way choice of a single constant.                         */
/* -------------------------------------------------------------------------- */

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

/* Pure, and free of anything that could be observed if the other arm ran. */
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
  /* Division traps, so the arm that would not have run must not divide. */
  if (ins->op == IR_OP_BINARY && ins->text &&
      (strcmp(ins->text, "/") == 0 || strcmp(ins->text, "%") == 0)) {
    return 0;
  }
  return 1;
}

/* Operand equality under a renaming of the temporaries each arm defines. */
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
    if (strcmp(a->name, b->name) == 0) {
      return 1;
    }
    if (!re_name_key(key, sizeof(key), IR_OPERAND_TEMP, b->name)) {
      return 0;
    }
    return re_map_get(rename, key) == 1;
  }
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_STRING:
  case IR_OPERAND_LABEL:
    return a->name && b->name && strcmp(a->name, b->name) == 0;
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

/* True when the condition provably holds 0 or 1, so the select needs no
 * compare of its own. */
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

/* Any other reference to the label means the arm has a second entry. */
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
  SelArm then_arm; /* without its trailing jump */
  SelArm else_arm;
  size_t diff_at; /* position within each arm */
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
  then_arm.count--; /* drop the jump */

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
      /* The one shared result, and it has to be the last thing either arm
       * does. */
      if (!a->dest.name || !b->dest.name ||
          strcmp(a->dest.name, b->dest.name) != 0 || k + 1 != then_arm.count) {
        ok = 0;
      }
    }
  }
  re_map_destroy(&rename);
  if (!ok || diff_count != 1) {
    return 0;
  }

  /* The differing operand has to sit where a temporary can replace it. */
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

  /* Room for the compare, the two instructions that build the selected
   * constant, and the arm itself, all inside the slots the arms occupied. */
  if (else_label - branch < then_arm.count + 3) {
    return 0;
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

static void sel_apply(IRFunction *function, const REDefs *defs,
                      const SelMatch *m, int *changed) {
  static int counter;
  IRInstruction body[SEL_MAX_ARM];
  IRInstruction *br = &function->instructions[m->branch];
  IROperand cond = ir_operand_copy(&br->lhs);
  SourceLocation location = br->location;
  int cond_is_boolean = sel_condition_is_boolean(function, defs, &br->lhs);
  long long delta = m->then_const - m->else_const;
  size_t body_count = m->then_arm.count;
  size_t at;
  char sel_name[48];
  char scaled_name[48];
  char offset_name[48];

  snprintf(sel_name, sizeof(sel_name), "__fsel_%d", counter);
  snprintf(scaled_name, sizeof(scaled_name), "__fselm_%d", counter);
  snprintf(offset_name, sizeof(offset_name), "__fselk_%d", counter);
  counter++;

  /* Move the arm out before its slots are reused. */
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
        sel_apply(function, &defs, &match, changed);
        i = match.end_label;
      }
    }
  }

  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Widening a byte that is already wide                                        */
/*                                                                             */
/* A sub-word load zero-extends into the whole register, so `(int32)buf[i]` on  */
/* a uint8 or uint16 buffer names a value the register already holds. The cast  */
/* still lowered to a movsxd, one instruction per byte in every scanner in the  */
/* suite. Rewriting it to a copy lets copy propagation retire it.               */
/*                                                                             */
/* Only unsigned sources qualify: a signed narrow element has to sign-extend    */
/* from its own width, which is not what the register holds.                    */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/* Hoisting loop-invariant loads                                               */
/*                                                                             */
/* skip_ws reads p->len and p->text every iteration, and no store in the loop   */
/* can change either: the loop's one store goes to p->pos, a different offset   */
/* off the same base. CSE cannot help -- there is only one load instruction,    */
/* executed once per iteration -- so the load moves to the preheader.           */
/*                                                                             */
/* Soundness is the kill rule again: the load's region must survive every       */
/* write between the header and the latch. Execution safety is separate: a      */
/* hoisted load runs even when the loop body would not have, so the base must   */
/* already be dereferenced by an access that runs unconditionally in the        */
/* header's straight-line prefix, and the offset stays within a page of it.     */
/* -------------------------------------------------------------------------- */

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
  size_t latch = 0;
  for (size_t i = header + 1; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_JUMP && ins->text && strcmp(ins->text, label) == 0) {
      latch = i;
    }
  }
  return latch;
}

/* The writes the loop makes, as kill regions. 0 = something unknowable. */
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
    if (!wbase[0] || !re_kills_append(log, wbase, woff, wtype, wsize)) {
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

/* One hoist per scan: an insertion moves every instruction behind it, which
 * stales the def-index map the address resolver walks, so the caller rebuilds
 * and rescans after each success. Returns 1 when a load moved. */

static int re_try_hoist_one_load(IRFunction *function, const REDefs *defs_in,
                                 const IRTempValueMap *addr_taken,
                                 int *changed) {
  const REDefs defs = *defs_in;
  static int counter;

  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    if (label->op != IR_OP_LABEL || !re_label_is_loop_header(label->text)) {
      continue;
    }
    size_t latch = re_loop_latch(function, header);
    if (!latch) {
      continue;
    }
    /* Control has to fall into the header for a preheader to exist. */
    if (header > 0) {
      const IRInstruction *prev = &function->instructions[header - 1];
      size_t p = header - 1;
      while (p > 0 && prev->op == IR_OP_NOP) {
        prev = &function->instructions[--p];
      }
      if (prev->op == IR_OP_JUMP || prev->op == IR_OP_RETURN ||
          prev->op == IR_OP_BRANCH_ZERO || prev->op == IR_OP_BRANCH_EQ) {
        continue;
      }
    }

    REKillLog writes = {0};
    if (!re_collect_loop_writes(function, &defs, addr_taken, header + 1, latch,
                                &writes)) {
      re_kills_destroy(&writes);
      continue;
    }

    /* The header's straight-line prefix runs on every entry to the loop; a
     * base it dereferences is a base a hoisted load may touch. */
    size_t prefix_end = latch;
    for (size_t i = header + 1; i <= latch; i++) {
      IROpcode op = function->instructions[i].op;
      if (op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ ||
          op == IR_OP_JUMP || op == IR_OP_LABEL || op == IR_OP_RETURN) {
        prefix_end = i;
        break;
      }
    }

    for (size_t i = header + 1; i < latch; i++) {
      IRInstruction *load = &function->instructions[i];
      REAddr addr = {0};
      char membuf[RE_NAME_MAX + 1];
      if (load->op != IR_OP_LOAD || load->dest.kind != IR_OPERAND_TEMP ||
          !load->dest.name || load->rhs.kind != IR_OPERAND_INT) {
        continue;
      }
      re_resolve_addr(function, &defs, &load->lhs, &addr, 0);
      if (!addr.valid || !addr.name || !addr.portable || addr.offset < 0 ||
          addr.offset >= 4096 ||
          (addr.kind == IR_OPERAND_TEMP && !addr.is_address_of)) {
        continue;
      }
      if (re_def_count(&defs, IR_OPERAND_TEMP, load->dest.name) != 1) {
        continue;
      }
      if (snprintf(membuf, sizeof(membuf), "%c%s",
                   addr.is_address_of ? '&' : 's',
                   addr.name) >= (int)sizeof(membuf) ||
          !re_region_survives_log(function, &writes, membuf, addr.offset,
                                  load->rhs.int_value, load->alias_class)) {
        continue;
      }

      /* Dereferenceability: the prefix already touches this base, or this
       * load IS in the prefix. */
      long long reach = addr.offset + load->rhs.int_value;
      int safe = i < prefix_end;
      for (size_t k = header + 1; k < prefix_end && !safe; k++) {
        safe = re_access_reaches(function, &defs, &function->instructions[k],
                                 &addr, reach);
      }
      for (size_t k = header; k-- > 0 && !safe;) {
        const IRInstruction *back = &function->instructions[k];
        if (back->op == IR_OP_LABEL || back->op == IR_OP_JUMP ||
            back->op == IR_OP_RETURN || back->op == IR_OP_BRANCH_ZERO ||
            back->op == IR_OP_BRANCH_EQ) {
          break;
        }
        safe = re_access_reaches(function, &defs, back, &addr, reach);
      }
      if (!safe) {
        continue;
      }

      /* Preheader: %addr = base + off ; dest <- *%addr. The original load and
       * its in-loop address chain go to the cleanups behind this pass. */
      char addr_name[48];
      snprintf(addr_name, sizeof(addr_name), "__licm_%d", counter++);
      IRInstruction lead = {0};
      IRInstruction body = {0};
      size_t inserted = 0;
      int failed = 0;

      if (addr.is_address_of) {
        lead.op = IR_OP_ADDRESS_OF;
        lead.dest = ir_operand_temp(addr_name);
        lead.lhs = ir_operand_symbol(addr.name);
      } else {
        lead.op = IR_OP_BINARY;
        lead.text = mettle_strdup("+");
        lead.dest = ir_operand_temp(addr_name);
        lead.lhs = ir_operand_symbol(addr.name);
        lead.rhs = ir_operand_int(0);
      }
      lead.location = load->location;
      if (!ir_function_insert_instruction(function, header, &lead)) {
        failed = 1;
      }
      ir_instruction_destroy_storage(&lead);
      if (!failed) {
        inserted++;
        IRInstruction *moved = &function->instructions[i + inserted];
        body.op = IR_OP_LOAD;
        body.location = moved->location;
        body.dest = ir_operand_temp(moved->dest.name);
        body.lhs = ir_operand_temp(addr_name);
        body.rhs = ir_operand_int(moved->rhs.int_value);
        body.is_float = moved->is_float;
        body.float_bits = moved->float_bits;
        body.is_unsigned = moved->is_unsigned;
        body.value_type = moved->value_type;
        if (addr.offset != 0) {
          /* fold the offset into the lead add */
          IRInstruction *lead_in = &function->instructions[header];
          if (lead_in->op == IR_OP_BINARY) {
            lead_in->rhs.int_value = addr.offset;
          } else {
            /* address-of base: append the offset with a second add */
            IRInstruction add = {0};
            add.op = IR_OP_BINARY;
            add.text = mettle_strdup("+");
            add.dest = ir_operand_temp(addr_name);
            add.lhs = ir_operand_temp(addr_name);
            add.rhs = ir_operand_int(addr.offset);
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
        ir_instruction_make_nop(&function->instructions[i + inserted]);
        if (changed) {
          *changed = 1;
        }
      }
      ir_instruction_destroy_storage(&body);
      re_kills_destroy(&writes);
      return failed ? 0 : 1;
    }
    re_kills_destroy(&writes);
  }
  return 0;
}

int ir_hoist_invariant_loads_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  /* Bounded: every hoist moves one load out of at least one loop, and a load
   * can only move outward as many times as loops enclose it. */
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

/* -------------------------------------------------------------------------- */
/* Promoting a loop-resident memory word to a local                            */
/*                                                                             */
/* Every parser in the suite walks `p->pos` through its loops: load it, test    */
/* it, bump it, store it back, every iteration, because the counter lives in    */
/* the Parser rather than in a register. When one region is the loop's only     */
/* may-aliased traffic, the loop can run on a local instead: load once in the   */
/* preheader, rewrite every load and store of the region to the local, and      */
/* store back once on every exit edge.                                          */
/*                                                                             */
/* Sound when: the base is never reassigned; every write in the loop that       */
/* could alias the region IS a store to exactly the region; no call or kernel   */
/* sits in the loop; and the region is dereferenced unconditionally in the      */
/* header prefix (the preheader load must be safe to execute when the loop      */
/* body would never have run). Exit edges are split so the store-back runs      */
/* once, on leaving, never per iteration.                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
  size_t at;      /* instruction index of the exiting branch/return */
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

/* One promotion per scan, same discipline as the load hoister: insertions
 * stale the def-index map. Returns 1 when something moved. */
static int re_try_promote_one(IRFunction *function, const REDefs *defs_in,
                              const IRTempValueMap *addr_taken, int *changed) {
  const REDefs defs = *defs_in;
  static int counter;

  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    if (label->op != IR_OP_LABEL || !re_label_is_loop_header(label->text)) {
      continue;
    }
    size_t latch = re_loop_latch(function, header);
    if (!latch) {
      continue;
    }
    if (header > 0) {
      const IRInstruction *prev = &function->instructions[header - 1];
      size_t p = header - 1;
      while (p > 0 && prev->op == IR_OP_NOP) {
        prev = &function->instructions[--p];
      }
      if (prev->op == IR_OP_JUMP || prev->op == IR_OP_RETURN ||
          prev->op == IR_OP_BRANCH_ZERO || prev->op == IR_OP_BRANCH_EQ) {
        continue;
      }
    }

    /* Candidate regions are the stores' targets. Try each stored region whose
     * base is a stable symbol. */
    for (size_t s = header + 1; s < latch; s++) {
      const IRInstruction *seed = &function->instructions[s];
      REAddr region = {0};
      if (seed->op != IR_OP_STORE || seed->rhs.kind != IR_OPERAND_INT) {
        continue;
      }
      re_resolve_addr(function, &defs, &seed->dest, &region, 0);
      if (getenv("METTLE_PROM_TRACE")) {
        fprintf(stderr, "[prom] %s store@%zu valid=%d name=%s port=%d off=%lld kind=%d ao=%d\n",
                function->name ? function->name : "?", s, region.valid,
                region.name ? region.name : "-", region.portable,
                region.offset, (int)region.kind, region.is_address_of);
      }
      if (!region.valid || !region.name || !region.portable ||
          region.offset < 0 || region.offset >= 4096 ||
          (region.kind == IR_OPERAND_TEMP && !region.is_address_of)) {
        continue;
      }
      long long size = seed->rhs.int_value;
      char region_base[RE_NAME_MAX + 1];
      if (snprintf(region_base, sizeof(region_base), "%c%s",
                   region.is_address_of ? '&' : 's',
                   region.name) >= (int)sizeof(region_base)) {
        continue;
      }

      /* Sweep the loop: every access that could alias the region must be a
       * load or store of exactly the region; nothing else may write memory in
       * a way that reaches it, and the loop takes no calls at all (a call
       * could read the region through an escaped pointer AND would clobber
       * the promoted local's freshness for it). */
      size_t loads[RE_PROMOTE_MAX_SITES];
      size_t stores[RE_PROMOTE_MAX_SITES];
      size_t load_count = 0;
      size_t store_count = 0;
      REExit exits[RE_PROMOTE_MAX_EXITS];
      size_t exit_count = 0;
      int header_exit = -1;
      int viable = 1;
      int strong = 1;
      int is_float = 0;
      int float_bits = 0;
      int is_unsigned = 0;
      MtlcType *value_type = NULL;

      for (size_t i = header + 1; i < latch && viable; i++) {
        const IRInstruction *ins = &function->instructions[i];
        if (ins->op == IR_OP_NOP) {
          continue;
        }
        if (ins->op == IR_OP_LOAD || ins->op == IR_OP_STORE) {
          REAddr a = {0};
          const IROperand *ao =
              ins->op == IR_OP_LOAD ? &ins->lhs : &ins->dest;
          re_resolve_addr(function, &defs, ao, &a, 0);
          char abase[RE_NAME_MAX + 1];
          int resolvable =
              a.valid && a.name &&
              snprintf(abase, sizeof(abase), "%c%s",
                       a.is_address_of ? '&' : 's',
                       a.name) < (int)sizeof(abase);
          long long asize =
              ins->rhs.kind == IR_OPERAND_INT ? ins->rhs.int_value : RE_MEM_WHOLE;
          REMemRegion probe = {resolvable ? abase : NULL, a.offset, asize,
                               ins->alias_class};
          if (!re_kill_hits(function, &probe, region_base, region.offset, size,
                            seed->alias_class)) {
            continue; /* provably elsewhere */
          }
          /* it may touch the region: it must BE the region, exactly */
          if (!resolvable || strcmp(abase, region_base) != 0 ||
              a.offset != region.offset || asize != size ||
              ins->is_float != (load_count || store_count ? is_float
                                                          : ins->is_float)) {
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
        /* exits */
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
      if (getenv("METTLE_PROM_TRACE")) {
        fprintf(stderr, "[prom]   viable=%d strong=%d loads=%zu stores=%zu exits=%zu float=%d\n",
                viable, strong, load_count, store_count, exit_count, is_float);
      }
      /* One load and one store per iteration already pay: the load leaves
       * the loop entirely and the store becomes a register move. */
      if (!viable || store_count == 0 || load_count == 0 || is_float) {
        continue; /* float promotion left for later; int is the parser case */
      }
      if (exit_count == 0) {
        strong = 0;
      }
      (void)header_exit;
      {
        /* Split edges append their tails at the end of the function, which
         * must therefore be unreachable by fall-through. */
        size_t last = function->instruction_count;
        while (last > 0 && function->instructions[last - 1].op == IR_OP_NOP) {
          last--;
        }
        if (last == 0 ||
            (function->instructions[last - 1].op != IR_OP_RETURN &&
             function->instructions[last - 1].op != IR_OP_JUMP)) {
      if (getenv("METTLE_PROM_TRACE")) {
        fprintf(stderr, "[prom]   no-terminator: weak only\n");
      }
          strong = 0;
        }
      }

      /* Preheader safety: the region itself must be dereferenced
       * unconditionally in the header prefix. */
      size_t prefix_end = latch;
      for (size_t i = header + 1; i <= latch; i++) {
        IROpcode op = function->instructions[i].op;
        if (op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ ||
            op == IR_OP_JUMP || op == IR_OP_LABEL || op == IR_OP_RETURN) {
          prefix_end = i;
          break;
        }
      }
      int safe = 0;
      for (size_t k = 0; k < load_count && !safe; k++) {
        safe = loads[k] < prefix_end;
      }
      for (size_t k = 0; k < store_count && !safe; k++) {
        safe = stores[k] < prefix_end;
      }
      if (!safe) {
      if (getenv("METTLE_PROM_TRACE")) {
        fprintf(stderr, "[prom]   bail: prefix-unsafe\n");
      }
        continue;
      }

      /* Build the pieces. Local + address + initial load before the header;
       * loads and stores in the loop become ASSIGNs; each exit edge gets a
       * store-back (before a RETURN, or on a split edge for a branch). */
      char local_name[48];
      char addr_name[48];
      snprintf(local_name, sizeof(local_name), "__prom_%d", counter);
      snprintf(addr_name, sizeof(addr_name), "__proma_%d", counter);
      counter++;

      const char *type_name =
          size == 8 ? "int64" : (is_unsigned ? "uint32" : "int32");
      if (size != 4 && size != 8) {
      if (getenv("METTLE_PROM_TRACE")) {
        fprintf(stderr, "[prom]   bail: size\n");
      }
        continue;
      }

      IRInstruction pieces[4];
      memset(pieces, 0, sizeof(pieces));
      pieces[0].op = IR_OP_DECLARE_LOCAL;
      pieces[0].dest = ir_operand_symbol(local_name);
      pieces[0].text = mettle_strdup(type_name);
      if (region.is_address_of) {
        pieces[1].op = IR_OP_ADDRESS_OF;
        pieces[1].dest = ir_operand_temp(addr_name);
        pieces[1].lhs = ir_operand_symbol(region.name);
        pieces[2].op = IR_OP_BINARY;
        pieces[2].text = mettle_strdup("+");
        pieces[2].dest = ir_operand_temp(addr_name);
        pieces[2].lhs = ir_operand_temp(addr_name);
        pieces[2].rhs = ir_operand_int(region.offset);
      } else {
        pieces[1].op = IR_OP_BINARY;
        pieces[1].text = mettle_strdup("+");
        pieces[1].dest = ir_operand_temp(addr_name);
        pieces[1].lhs = ir_operand_symbol(region.name);
        pieces[1].rhs = ir_operand_int(region.offset);
        pieces[2].op = IR_OP_NOP;
      }
      pieces[3].op = IR_OP_LOAD;
      pieces[3].dest = ir_operand_symbol(local_name);
      pieces[3].lhs = ir_operand_temp(addr_name);
      pieces[3].rhs = ir_operand_int(size);
      pieces[3].is_unsigned = is_unsigned;
      pieces[3].float_bits = float_bits;
      pieces[3].value_type = value_type;
      for (int k = 0; k < 4; k++) {
        pieces[k].location = function->instructions[header].location;
      }

      size_t inserted = 0;
      int failed = 0;
      for (int k = 0; k < 4 && !failed; k++) {
        if (pieces[k].op == IR_OP_NOP) {
          continue;
        }
        if (!ir_function_insert_instruction(function, header + inserted,
                                            &pieces[k])) {
          failed = 1;
        }
        ir_instruction_destroy_storage(&pieces[k]);
        if (!failed) {
          inserted++;
        }
      }
      if (failed) {
        return 0;
      }
      header += inserted;
      latch += inserted;
      for (size_t k = 0; k < load_count; k++) {
        loads[k] += inserted;
      }
      for (size_t k = 0; k < store_count; k++) {
        stores[k] += inserted;
      }
      for (size_t k = 0; k < exit_count; k++) {
        exits[k].at += inserted;
      }

      /* Rewrite the in-loop accesses. */
      for (size_t k = 0; k < load_count; k++) {
        IRInstruction *ld = &function->instructions[loads[k]];
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
      if (strong) {
        for (size_t k = 0; k < store_count; k++) {
          IRInstruction *st = &function->instructions[stores[k]];
          IROperand value = ir_operand_copy(&st->lhs);
          ir_instruction_destroy_storage(st);
          memset(st, 0, sizeof(*st));
          st->op = IR_OP_ASSIGN;
          st->dest = ir_operand_symbol(local_name);
          st->lhs = value;
        }
      } else {
        for (size_t k = 0; k < store_count; k++) {
          IRInstruction upd = {0};
          upd.op = IR_OP_ASSIGN;
          upd.dest = ir_operand_symbol(local_name);
          upd.lhs = ir_operand_copy(&function->instructions[stores[k]].lhs);
          upd.location = function->instructions[stores[k]].location;
          if (!ir_function_insert_instruction(function, stores[k] + 1, &upd)) {
            ir_instruction_destroy_storage(&upd);
            return 0;
          }
          ir_instruction_destroy_storage(&upd);
          for (size_t m = k + 1; m < store_count; m++) {
            stores[m]++;
          }
        }
        if (changed) {
          *changed = 1;
        }
        return 1;
      }

      /* Store-backs. Returns take theirs inline; branch exits are split: the
       * branch is retargeted to a fresh tail label that stores and jumps on.
       * Inserting the tails at the end never disturbs loop indices. */
      for (size_t k = 0; k < exit_count; k++) {
        IRInstruction st = {0};
        st.op = IR_OP_STORE;
        st.dest = ir_operand_temp(addr_name);
        st.lhs = ir_operand_symbol(local_name);
        st.rhs = ir_operand_int(size);
        st.location = function->instructions[exits[k].at].location;

        if (exits[k].is_return) {
          if (!ir_function_insert_instruction(function, exits[k].at, &st)) {
            failed = 1;
          }
          ir_instruction_destroy_storage(&st);
          if (failed) {
            return 0;
          }
          /* shift every later site */
          for (size_t m = 0; m < exit_count; m++) {
            if (exits[m].at >= exits[k].at) {
              exits[m].at++;
            }
          }
          latch++;
          continue;
        }

        IRInstruction *br = &function->instructions[exits[k].at];
        char tail_name[64];
        snprintf(tail_name, sizeof(tail_name), "__promx_%d_%zu", counter - 1,
                 k);
        char *old_target = mettle_strdup(br->text);
        if (!old_target) {
          ir_instruction_destroy_storage(&st);
          return 0;
        }
        mettle_free_string(br->text);
        br->text = mettle_strdup(tail_name);
        if (!br->text) {
          free(old_target);
          ir_instruction_destroy_storage(&st);
          return 0;
        }

        IRInstruction tail_label = {0};
        IRInstruction tail_jump = {0};
        tail_label.op = IR_OP_LABEL;
        tail_label.text = mettle_strdup(tail_name);
        tail_jump.op = IR_OP_JUMP;
        tail_jump.text = old_target; /* takes ownership */
        size_t end = function->instruction_count;
        if (!tail_label.text ||
            !ir_function_insert_instruction(function, end, &tail_label) ||
            !ir_function_insert_instruction(function, end + 1, &st) ||
            !ir_function_insert_instruction(function, end + 2, &tail_jump)) {
          failed = 1;
        }
        ir_instruction_destroy_storage(&tail_label);
        ir_instruction_destroy_storage(&st);
        ir_instruction_destroy_storage(&tail_jump);
        if (failed) {
          return 0;
        }
      }

      if (changed) {
        *changed = 1;
      }
      return 1;
    }
  }
  return 0;
}

int ir_promote_loop_memory_pass(IRFunction *function, int *changed) {
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

/* -------------------------------------------------------------------------- */
/* One spelling per inlined parameter                                          */
/*                                                                             */
/* The inliner materializes `local @p; @p <- %t`, and block-local copy         */
/* propagation then rewrites the reads of @p that share the entry block back   */
/* to %t while the loop keeps reading @p. The recognizers compare bases by     */
/* name, so the split spelling hides the loop from every one of them: the      */
/* pre-loop `minv <- arr[0]` init reads %t while the body reads @p, and the    */
/* minmax kernel sees two different arrays. Rewriting every later read of %t   */
/* to @p restores one name. Sound because @p holds %t's value from the copy    */
/* onward and neither is ever written again.                                   */
/* -------------------------------------------------------------------------- */

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
