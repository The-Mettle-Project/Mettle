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
 *   - Loads carry a memory generation. Anything that might write memory bumps
 *     it, and so does entering a block with more than one predecessor: a store
 *     on a path this walk never visits has to reach a merge that dominates the
 *     use, and the merge is where it gets accounted for.
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

/* Count every write. The kernels and calls outside the scalar set can name
 * their outputs in dest, lhs, or rhs, so all three count there: an
 * over-counted name simply stops crossing blocks. */
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

typedef struct {
  char *key;
  char *value;         /* the temp that already holds this value */
  char *dep[2];        /* names the entry reads, for block-local invalidation */
  unsigned generation; /* memory generation this entry was recorded at */
  int volatile_value;  /* a memory write can change it: load, or aliasable read */
} REEntry;

/* Entries own their strings. A name borrowed from an operand dies the moment
 * this pass rewrites the instruction that held it. */
static void re_entry_release(REEntry *entry) {
  free(entry->key);
  free(entry->value);
  free(entry->dep[0]);
  free(entry->dep[1]);
  entry->key = NULL;
  entry->value = NULL;
  entry->dep[0] = NULL;
  entry->dep[1] = NULL;
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
                         const char *dep0, const char *dep1,
                         unsigned generation, int volatile_value) {
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
  if (!entry->key || !entry->value || (dep0 && !entry->dep[0]) ||
      (dep1 && !entry->dep[1])) {
    re_entry_release(entry);
    return 0;
  }
  entry->generation = generation;
  entry->volatile_value = volatile_value;
  table->count++;
  return 1;
}

static const char *re_table_lookup(const RETable *table, const char *key,
                                   unsigned generation) {
  for (size_t i = table->count; i-- > 0;) {
    const REEntry *entry = &table->items[i];
    if (entry->volatile_value && entry->generation != generation) {
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
  unsigned generation;
  int *changed;
  int failed;
} REWalk;

static int re_writes_memory(const REWalk *walk, const IRInstruction *ins) {
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
    break;
  default:
    return 1;
  }
  /* Writing a local whose address escaped is a memory write a load can see. */
  if (ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
      ir_temp_value_map_lookup(walk->addr_taken, ins->dest.name)) {
    return 1;
  }
  return 0;
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

static void re_process_block(REWalk *walk, size_t block_index,
                             const REDom *dom) {
  if (walk->failed) {
    return;
  }
  const IRBasicBlock *block = &walk->blocks[block_index];
  size_t global_mark = walk->global.count;
  re_table_truncate(&walk->local, 0);

  /* A store on a path this walk never took has to reach a merge that dominates
   * the use; the merge is here. */
  if (block->predecessor_count != 1) {
    walk->generation++;
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
        } else {
          volatile_value = re_operand_aliasable(walk->defs, &ins->lhs) ||
                           re_operand_aliasable(walk->defs, &ins->rhs);
        }
        const char *hit = re_table_lookup(&walk->local, key, walk->generation);
        if (!hit) {
          hit = re_table_lookup(&walk->global, key, walk->generation);
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
          if (!re_table_push(&walk->local, key, ins->dest.name, dep0, dep1,
                             walk->generation, volatile_value)) {
            walk->failed = 1;
            return;
          }
          /* Only loads cross a block boundary. Address arithmetic is one
           * instruction to recompute and a live register to carry, and
           * carrying it measured slower on the interpreter benchmark. */
          if (is_load && portable &&
              re_def_count(walk->defs, IR_OPERAND_TEMP, ins->dest.name) == 1 &&
              !re_table_push(&walk->global, key, ins->dest.name, NULL, NULL,
                             walk->generation, volatile_value)) {
            walk->failed = 1;
            return;
          }
        }
      }
    }

    if (re_writes_memory(walk, ins)) {
      walk->generation++;
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
    re_process_block(&walk, function->entry_block, &dom);
    re_table_destroy(&walk.global);
    re_table_destroy(&walk.local);
  }

  re_dom_destroy(&dom);
  re_map_destroy(&defs.defs);
  re_map_destroy(&defs.def_at);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}
