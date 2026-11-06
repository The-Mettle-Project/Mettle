#include "semantic/ownership_internal.h"
#include "semantic/type_checker_internal.h"
#include "ir/ir_explain_ownership.h"
#include "common.h"
#include "string_intern.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const ASTNode *call;
  const ASTNode *exit;
  OwnDropPlan plan;
  int next;
} OwnPlanEntry;

typedef struct {
  const void *unit;
  OwnBitWrite *writes;
  size_t count;
  size_t capacity;
} OwnUnitEntry;

typedef struct {
  const ASTNode *declaration;
  int *bits;
  size_t count;
  size_t capacity;
} OwnBirthEntry;

typedef struct {
  char *name;
  int state;
  char *destructor;
  char *glue;
  struct Type *type;
} OwnTypeEntry;

typedef struct {
  OwnPlanEntry *plans;
  size_t plan_count;
  size_t plan_capacity;
  OwnNodeMap plan_heads;
  OwnNodeMap planned_calls;
  OwnUnitEntry *units;
  size_t unit_count;
  size_t unit_capacity;
  OwnNodeMap unit_index;
  char **notes;
  size_t note_count;
  size_t note_capacity;
  OwnNodeMap note_index;
  OwnNodeMap block_end_index;
  OwnNodeMap inserted_index;
  OwnFix *fixes;
  size_t fix_count;
  size_t fix_capacity;
  OwnRecord *records;
  size_t record_count;
  size_t record_capacity;
  OwnNodeMap compiled_index;
  char **compiled;
  size_t compiled_count;
  size_t compiled_capacity;
  int bit_count;
  char **bit_names;
  OwnNodeMap bit_functions;
  OwnBirthEntry *function_bits;
  size_t function_bit_count;
  size_t function_bit_capacity;
  OwnTypeEntry *types;
  size_t type_count;
  size_t type_capacity;
  struct Type **glue_queue;
  SourceLocation *glue_sites;
  size_t glue_site_capacity;
  size_t glue_count;
  size_t glue_capacity;
  size_t glue_done;
  ASTNode *program;
  TypeChecker *checker;
  int apply_fixes;
} OwnGlobals;

static OwnGlobals g_own;

static size_t own_hash_pointer(const void *pointer) {
  uint64_t x = (uint64_t)(uintptr_t)pointer;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)x;
}

void own_map_init(OwnNodeMap *map) { memset(map, 0, sizeof(*map)); }

void own_map_free(OwnNodeMap *map) {
  free(map->keys);
  free(map->values);
  memset(map, 0, sizeof(*map));
}

static int own_map_insert_raw(OwnNodeMap *map, const void *key, int value) {
  size_t mask = map->slots - 1;
  size_t at = own_hash_pointer(key) & mask;
  while (map->keys[at] && map->keys[at] != key) {
    at = (at + 1) & mask;
  }
  if (!map->keys[at]) {
    map->used++;
  }
  map->keys[at] = key;
  map->values[at] = value;
  return 1;
}

static int own_map_grow(OwnNodeMap *map) {
  size_t slots = map->slots ? map->slots * 2 : 64;
  const void **old_keys = map->keys;
  int *old_values = map->values;
  size_t old_slots = map->slots;
  map->keys = calloc(slots, sizeof(*map->keys));
  map->values = calloc(slots, sizeof(*map->values));
  if (!map->keys || !map->values) {
    free(map->keys);
    free(map->values);
    map->keys = old_keys;
    map->values = old_values;
    return 0;
  }
  map->slots = slots;
  map->used = 0;
  for (size_t i = 0; i < old_slots; i++) {
    if (old_keys[i]) {
      own_map_insert_raw(map, old_keys[i], old_values[i]);
    }
  }
  free(old_keys);
  free(old_values);
  return 1;
}

int own_map_put(OwnNodeMap *map, const void *key, int value) {
  if (!key) {
    return 0;
  }
  if ((map->used + 1) * 2 > map->slots && !own_map_grow(map)) {
    return 0;
  }
  return own_map_insert_raw(map, key, value);
}

int own_map_get(const OwnNodeMap *map, const void *key, int *value) {
  if (!map->slots || !key) {
    return 0;
  }
  size_t mask = map->slots - 1;
  size_t at = own_hash_pointer(key) & mask;
  while (map->keys[at]) {
    if (map->keys[at] == key) {
      if (value) {
        *value = map->values[at];
      }
      return 1;
    }
    at = (at + 1) & mask;
  }
  return 0;
}

static void *own_grow(void *items, size_t *capacity, size_t count,
                      size_t size) {
  if (count < *capacity) {
    return items;
  }
  size_t next = *capacity ? *capacity * 2 : 16;
  void *grown = realloc(items, next * size);
  if (!grown) {
    return NULL;
  }
  memset((char *)grown + *capacity * size, 0, (next - *capacity) * size);
  *capacity = next;
  return grown;
}

static char *own_strdup(const char *text) {
  if (!text) {
    return NULL;
  }
  size_t length = strlen(text);
  char *copy = malloc(length + 1);
  if (copy) {
    memcpy(copy, text, length + 1);
  }
  return copy;
}

static void own_free_records(void) {
  for (size_t i = 0; i < g_own.record_count; i++) {
    OwnRecord *r = &g_own.records[i];
    free(r->function);
    free(r->subject);
    free(r->type_name);
    free(r->text);
    free(r->reason);
    free(r->compiled_to);
  }
  free(g_own.records);
}

static void own_free_tables(void) {
  for (size_t i = 0; i < g_own.plan_count; i++) {
    free(g_own.plans[i].plan.steps);
  }
  free(g_own.plans);
  for (size_t i = 0; i < g_own.unit_count; i++) {
    free(g_own.units[i].writes);
  }
  free(g_own.units);
  for (size_t i = 0; i < g_own.function_bit_count; i++) {
    free(g_own.function_bits[i].bits);
  }
  free(g_own.function_bits);
  for (size_t i = 0; i < g_own.note_count; i++) {
    free(g_own.notes[i]);
  }
  free(g_own.notes);
  for (size_t i = 0; i < g_own.fix_count; i++) {
    free(g_own.fixes[i].file);
    free(g_own.fixes[i].text);
  }
  free(g_own.fixes);
  for (size_t i = 0; i < g_own.compiled_count; i++) {
    free(g_own.compiled[i]);
  }
  free(g_own.compiled);
  for (int i = 0; i < g_own.bit_count; i++) {
    free(g_own.bit_names[i]);
  }
  free(g_own.bit_names);
  for (size_t i = 0; i < g_own.type_count; i++) {
    free(g_own.types[i].name);
    free(g_own.types[i].destructor);
    free(g_own.types[i].glue);
  }
  free(g_own.types);
  free(g_own.glue_queue);
  free(g_own.glue_sites);
}

void ownership_reset(void) {
  int apply = g_own.apply_fixes;
  own_free_records();
  own_free_tables();
  own_map_free(&g_own.plan_heads);
  own_map_free(&g_own.planned_calls);
  own_map_free(&g_own.unit_index);
  own_map_free(&g_own.note_index);
  own_map_free(&g_own.block_end_index);
  own_map_free(&g_own.inserted_index);
  own_map_free(&g_own.compiled_index);
  own_map_free(&g_own.bit_functions);
  memset(&g_own, 0, sizeof(g_own));
  g_own.apply_fixes = apply;
}

void ownership_set_apply_fixes(int enabled) { g_own.apply_fixes = enabled; }

int own_new_bit(const char *path, size_t line) {
  size_t capacity = (size_t)g_own.bit_count;
  char **grown = realloc(g_own.bit_names,
                         (capacity + 1) * sizeof(*g_own.bit_names));
  char name[300];
  size_t used = 0;
  if (!grown) {
    return -1;
  }
  g_own.bit_names = grown;
  used = (size_t)snprintf(name, sizeof(name), "own$live$");
  for (const char *p = path ? path : "value"; *p && used + 24 < sizeof(name);
       p++) {
    name[used++] = *p == '.' ? '$' : *p;
  }
  snprintf(name + used, sizeof(name) - used, "$%zu", line);
  used = strlen(name);
  for (int suffix = 2, clash = 1; clash; suffix++) {
    clash = 0;
    for (int i = 0; i < g_own.bit_count && !clash; i++) {
      clash = strcmp(g_own.bit_names[i], name) == 0;
    }
    if (clash) {
      snprintf(name + used, sizeof(name) - used, "$%d", suffix);
    }
  }
  g_own.bit_names[g_own.bit_count] = own_strdup(name);
  return g_own.bit_count++;
}

const char *ownership_bit_name(int bit) {
  return bit >= 0 && bit < g_own.bit_count ? g_own.bit_names[bit] : NULL;
}

int ownership_bit_count(void) { return g_own.bit_count; }

static void own_bit_list_add(OwnBirthEntry **entries, size_t *count,
                             size_t *capacity, OwnNodeMap *index_map,
                             const ASTNode *key, int bit) {
  int index = 0;
  OwnBirthEntry *entry = NULL;
  if (!own_map_get(index_map, key, &index)) {
    OwnBirthEntry *grown = own_grow(*entries, capacity, *count,
                                    sizeof(**entries));
    if (!grown) {
      return;
    }
    *entries = grown;
    index = (int)(*count)++;
    (*entries)[index].declaration = key;
    own_map_put(index_map, key, index);
  }
  entry = &(*entries)[index];
  for (size_t i = 0; i < entry->count; i++) {
    if (entry->bits[i] == bit) {
      return;
    }
  }
  int *bits = own_grow(entry->bits, &entry->capacity, entry->count,
                       sizeof(*entry->bits));
  if (!bits) {
    return;
  }
  entry->bits = bits;
  entry->bits[entry->count++] = bit;
}

void own_store_function_bit(const ASTNode *function, int bit) {
  own_bit_list_add(&g_own.function_bits, &g_own.function_bit_count,
                   &g_own.function_bit_capacity, &g_own.bit_functions,
                   function, bit);
}

const int *ownership_function_bits(const ASTNode *function, size_t *count) {
  int index = 0;
  *count = 0;
  if (!function || !own_map_get(&g_own.bit_functions, function, &index)) {
    return NULL;
  }
  *count = g_own.function_bits[index].count;
  return g_own.function_bits[index].bits;
}

static OwnPlanEntry *own_find_plan(const ASTNode *drop_call,
                                   const ASTNode *exit) {
  int at = -1;
  if (!own_map_get(&g_own.plan_heads, drop_call, &at)) {
    return NULL;
  }
  while (at >= 0) {
    if (g_own.plans[at].exit == exit) {
      return &g_own.plans[at];
    }
    at = g_own.plans[at].next;
  }
  return NULL;
}

void own_store_plan(const ASTNode *drop_call, const ASTNode *exit,
                    OwnDropStep *steps, size_t count, int unreachable) {
  OwnPlanEntry *existing = own_find_plan(drop_call, exit);
  OwnPlanEntry *grown = NULL;
  int head = -1;
  if (existing) {
    free(existing->plan.steps);
    existing->plan.steps = steps;
    existing->plan.step_count = count;
    existing->plan.unreachable = unreachable;
    return;
  }
  grown = own_grow(g_own.plans, &g_own.plan_capacity, g_own.plan_count,
                   sizeof(*g_own.plans));
  if (!grown) {
    free(steps);
    return;
  }
  g_own.plans = grown;
  if (!own_map_get(&g_own.plan_heads, drop_call, &head)) {
    head = -1;
  }
  g_own.plans[g_own.plan_count].call = drop_call;
  g_own.plans[g_own.plan_count].exit = exit;
  g_own.plans[g_own.plan_count].plan.steps = steps;
  g_own.plans[g_own.plan_count].plan.step_count = count;
  g_own.plans[g_own.plan_count].plan.unreachable = unreachable;
  g_own.plans[g_own.plan_count].next = head;
  own_map_put(&g_own.planned_calls, drop_call, 1);
  own_map_put(&g_own.plan_heads, drop_call, (int)g_own.plan_count);
  g_own.plan_count++;
}

const OwnDropPlan *ownership_drop_plan(const ASTNode *drop_call,
                                       const ASTNode *exit) {
  OwnPlanEntry *entry = own_find_plan(drop_call, exit);
  return entry ? &entry->plan : NULL;
}

int ownership_call_is_planned(const ASTNode *drop_call) {
  return own_map_get(&g_own.planned_calls, drop_call, NULL);
}

void own_store_unit_write(const void *unit, int bit, int value) {
  int index = 0;
  OwnUnitEntry *entry = NULL;
  if (!own_map_get(&g_own.unit_index, unit, &index)) {
    OwnUnitEntry *grown = own_grow(g_own.units, &g_own.unit_capacity,
                                   g_own.unit_count, sizeof(*g_own.units));
    if (!grown) {
      return;
    }
    g_own.units = grown;
    index = (int)g_own.unit_count++;
    g_own.units[index].unit = unit;
    own_map_put(&g_own.unit_index, unit, index);
  }
  entry = &g_own.units[index];
  for (size_t i = entry->count; i > 0; i--) {
    if (entry->writes[i - 1].bit == bit) {
      if (entry->writes[i - 1].value == value) {
        return;
      }
      break;
    }
  }
  OwnBitWrite *writes =
      own_grow(entry->writes, &entry->capacity, entry->count,
               sizeof(*entry->writes));
  if (!writes) {
    return;
  }
  entry->writes = writes;
  entry->writes[entry->count].bit = bit;
  entry->writes[entry->count].value = value;
  entry->count++;
}

const OwnBitWrite *ownership_unit_writes(const void *unit, size_t *count) {
  int index = 0;
  *count = 0;
  if (!unit || !own_map_get(&g_own.unit_index, unit, &index)) {
    return NULL;
  }
  *count = g_own.units[index].count;
  return g_own.units[index].writes;
}

void own_add_record(OwnRecordKind kind, SourceLocation at,
                    SourceLocation origin, const char *function,
                    const char *subject, struct Type *type, const char *text,
                    const char *reason, int inserted, int written) {
  OwnRecord *grown = own_grow(g_own.records, &g_own.record_capacity,
                              g_own.record_count, sizeof(*g_own.records));
  OwnRecord *r = NULL;
  if (!grown) {
    return;
  }
  g_own.records = grown;
  r = &g_own.records[g_own.record_count++];
  r->kind = kind;
  r->at = at;
  r->origin = origin;
  r->function = own_strdup(function);
  r->subject = own_strdup(subject);
  r->type_name = type ? own_type_text(type) : NULL;
  r->text = own_strdup(text);
  r->reason = own_strdup(reason);
  r->compiled_to = NULL;
  r->inserted = inserted;
  r->written = written;
}

size_t ownership_record_count(void) { return g_own.record_count; }

const OwnRecord *ownership_record_at(size_t index) {
  return index < g_own.record_count ? &g_own.records[index] : NULL;
}

static void own_note_append(OwnNodeMap *index_map, const void *node,
                            const char *text) {
  int index = 0;
  char **grown = NULL;
  if (!node || !text) {
    return;
  }
  if (own_map_get(index_map, node, &index)) {
    char *old = g_own.notes[index];
    if (strstr(old, text)) {
      return;
    }
    size_t length = strlen(old) + strlen(text) + 3;
    char *joined = malloc(length);
    if (!joined) {
      return;
    }
    snprintf(joined, length, "%s; %s", old, text);
    free(old);
    g_own.notes[index] = joined;
    return;
  }
  grown = own_grow(g_own.notes, &g_own.note_capacity, g_own.note_count,
                   sizeof(*g_own.notes));
  if (!grown) {
    return;
  }
  g_own.notes = grown;
  g_own.notes[g_own.note_count] = own_strdup(text);
  own_map_put(index_map, node, (int)g_own.note_count);
  g_own.note_count++;
}

void own_attach_note(ASTNode *node, OwnRecordKind kind, const char *subject,
                     struct Type *type, SourceLocation at,
                     SourceLocation origin, const char *reason,
                     const char *function) {
  char text[512];
  (void)kind;
  (void)subject;
  (void)type;
  (void)at;
  (void)origin;
  (void)function;
  snprintf(text, sizeof(text), "inserted: %s", reason ? reason : "");
  own_note_append(&g_own.note_index, node, text);
  own_map_put(&g_own.inserted_index, node, 1);
}

void own_note_statement(ASTNode *statement, const char *text) {
  own_note_append(&g_own.note_index, statement, text);
}

void own_note_block_end(ASTNode *block, const char *text) {
  own_note_append(&g_own.block_end_index, block, text);
}

const char *ownership_note(const ASTNode *node) {
  int index = 0;
  if (!node || !own_map_get(&g_own.note_index, node, &index)) {
    return NULL;
  }
  return g_own.notes[index];
}

const char *ownership_block_end_note(const ASTNode *block) {
  int index = 0;
  if (!block || !own_map_get(&g_own.block_end_index, block, &index)) {
    return NULL;
  }
  return g_own.notes[index];
}

int ownership_node_is_inserted(const ASTNode *node) {
  return node && own_map_get(&g_own.inserted_index, node, NULL);
}

void own_add_fix(OwnFixKind kind, SourceLocation at, const char *text,
                 ASTNode *node) {
  OwnFix *grown = own_grow(g_own.fixes, &g_own.fix_capacity, g_own.fix_count,
                           sizeof(*g_own.fixes));
  if (!grown || !at.filename || at.line == 0) {
    return;
  }
  g_own.fixes = grown;
  OwnFix *fix = &g_own.fixes[g_own.fix_count++];
  fix->kind = kind;
  fix->file = own_strdup(at.filename);
  fix->line = at.line;
  fix->column = at.column;
  fix->statement_line = at.line;
  fix->statement_column = at.column;
  fix->statement_returns = 0;
  fix->text = own_strdup(text);
  fix->node = node;
}

void own_add_statement_fix(OwnFixKind kind, SourceLocation at,
                           SourceLocation statement, int returns,
                           const char *text) {
  size_t before = g_own.fix_count;
  own_add_fix(kind, at, text, NULL);
  if (g_own.fix_count > before) {
    OwnFix *fix = &g_own.fixes[g_own.fix_count - 1];
    fix->statement_line = statement.line;
    fix->statement_column = statement.column;
    fix->statement_returns = returns;
  }
}

size_t own_fix_count(void) { return g_own.fix_count; }

OwnFix *own_fix_at(size_t index) {
  return index < g_own.fix_count ? &g_own.fixes[index] : NULL;
}

int ownership_apply_fixes_enabled(void) { return g_own.apply_fixes; }

void ownership_note_compiled(const ASTNode *drop_call, const char *what) {
  int index = 0;
  char **grown = NULL;
  if (!drop_call || !what) {
    return;
  }
  if (own_map_get(&g_own.compiled_index, drop_call, &index)) {
    char *old = g_own.compiled[index];
    if (strstr(old, what)) {
      return;
    }
    size_t length = strlen(old) + strlen(what) + 3;
    char *joined = malloc(length);
    if (!joined) {
      return;
    }
    snprintf(joined, length, "%s; %s", old, what);
    free(old);
    g_own.compiled[index] = joined;
    return;
  }
  grown = own_grow(g_own.compiled, &g_own.compiled_capacity,
                   g_own.compiled_count, sizeof(*g_own.compiled));
  if (!grown) {
    return;
  }
  g_own.compiled = grown;
  g_own.compiled[g_own.compiled_count] = own_strdup(what);
  own_map_put(&g_own.compiled_index, drop_call, (int)g_own.compiled_count);
  g_own.compiled_count++;
}

static const char *own_compiled_text(const void *key) {
  int index = 0;
  return key && own_map_get(&g_own.compiled_index, key, &index)
             ? g_own.compiled[index]
             : NULL;
}

void own_link_compiled(const ASTNode *drop_call) {
  if (g_own.record_count > 0) {
    g_own.records[g_own.record_count - 1].key = drop_call;
  }
}

static const char *own_record_kind_word(OwnRecordKind kind) {
  static const char *const words[OWN_RECORD_KIND_COUNT] = {
      "scope-drop", "parameter-drop", "arm-drop", "assign-drop",
      "discard-drop", "temporary", "glue", "move", "live-bit", "drop-runs"};
  return kind < OWN_RECORD_KIND_COUNT ? words[kind] : "?";
}

void ownership_publish_explain(void) {
  for (size_t i = 0; i < g_own.record_count; i++) {
    const OwnRecord *r = &g_own.records[i];
    IRExplainOwnershipRecord out;
    memset(&out, 0, sizeof(out));
    out.file = r->at.filename;
    out.line = r->at.line;
    out.column = r->at.column;
    out.kind = own_record_kind_word(r->kind);
    out.function = r->function;
    out.subject = r->subject;
    out.type = r->type_name;
    out.text = r->text;
    out.reason = r->reason;
    out.lowered = own_compiled_text(r->key);
    out.inserted = r->inserted;
    out.key = r->key;
    ir_explain_ownership_note(&out);
  }
}

static OwnTypeEntry *own_type_entry(const char *name) {
  for (size_t i = 0; i < g_own.type_count; i++) {
    if (strcmp(g_own.types[i].name, name) == 0) {
      return &g_own.types[i];
    }
  }
  OwnTypeEntry *grown = own_grow(g_own.types, &g_own.type_capacity,
                                 g_own.type_count, sizeof(*g_own.types));
  if (!grown) {
    return NULL;
  }
  g_own.types = grown;
  OwnTypeEntry *entry = &g_own.types[g_own.type_count++];
  entry->name = own_strdup(name);
  entry->state = 0;
  return entry;
}

static StructDeclaration *own_struct_declaration(const char *name) {
  Program *program = g_own.program && g_own.program->data
                         ? (Program *)g_own.program->data
                         : NULL;
  for (size_t i = 0; program && i < program->declaration_count; i++) {
    ASTNode *decl = program->declarations[i];
    if (decl && decl->type == AST_STRUCT_DECLARATION && decl->data &&
        ((StructDeclaration *)decl->data)->name &&
        strcmp(((StructDeclaration *)decl->data)->name, name) == 0) {
      return (StructDeclaration *)decl->data;
    }
  }
  return NULL;
}

static struct Type *own_strip(struct Type *type) {
  while (type && type->refined_base) {
    type = type->refined_base;
  }
  return type;
}

static Symbol *own_destructor_symbol(TypeChecker *checker,
                                     struct Type *type) {
  char name[512];
  Symbol *symbol = NULL;
  StructDeclaration *sd = NULL;
  if (!checker || !type || !type->name) {
    return NULL;
  }
  snprintf(name, sizeof(name), "%s_drop__ptr", type->name);
  symbol = symbol_table_lookup_global(checker->symbol_table, name);
  if (symbol && symbol->kind == SYMBOL_FUNCTION) {
    return symbol;
  }
  sd = own_struct_declaration(type->name);
  if (sd && sd->drop_function) {
    symbol = symbol_table_lookup_global(checker->symbol_table,
                                        sd->drop_function);
    if (symbol && symbol->kind == SYMBOL_FUNCTION) {
      return symbol;
    }
  }
  return NULL;
}

int own_type_has_destructor(TypeChecker *checker, struct Type *type) {
  type = own_strip(type);
  return type && type->kind == TYPE_STRUCT &&
         own_destructor_symbol(checker, type) != NULL;
}

const char *own_destructor_name(TypeChecker *checker, struct Type *type) {
  Symbol *symbol = NULL;
  type = own_strip(type);
  if (!type || type->kind != TYPE_STRUCT) {
    return NULL;
  }
  symbol = own_destructor_symbol(checker, type);
  return symbol ? symbol->name : NULL;
}

static int own_struct_fields_own(TypeChecker *checker, struct Type *type) {
  for (size_t i = 0; i < type->field_count; i++) {
    if (type->field_types && own_type_owns(checker, type->field_types[i])) {
      return 1;
    }
  }
  return 0;
}

static int own_enum_payloads_own(TypeChecker *checker, struct Type *type) {
  for (size_t i = 0; i < type->tagged_variant_count; i++) {
    if (type->tagged_variant_payloads &&
        own_type_owns(checker, type->tagged_variant_payloads[i])) {
      return 1;
    }
  }
  return 0;
}

int own_type_owns(TypeChecker *checker, struct Type *type) {
  OwnTypeEntry *entry = NULL;
  int owns = 0;
  type = own_strip(type);
  if (!type) {
    return 0;
  }
  if (type->kind == TYPE_ARRAY) {
    return type->array_size > 0 && own_type_owns(checker, type->base_type);
  }
  if ((type->kind != TYPE_STRUCT && type->kind != TYPE_TAGGED_ENUM) ||
      !type->name) {
    return 0;
  }
  entry = own_type_entry(type->name);
  if (!entry) {
    return 0;
  }
  if (entry->state == 1 || entry->state == 2) {
    return entry->state == 1;
  }
  if (entry->state == 3) {
    return 0;
  }
  entry->state = 3;
  entry->type = type;
  if (type->kind == TYPE_STRUCT) {
    owns = own_destructor_symbol(checker, type) != NULL ||
           own_struct_fields_own(checker, type);
  } else {
    owns = own_enum_payloads_own(checker, type);
  }
  entry->state = owns ? 1 : 2;
  return owns;
}

int ownership_type_owns(TypeChecker *checker, struct Type *type) {
  if (!g_own.program && checker) {
    g_own.program = checker->module_program;
  }
  return own_type_owns(checker, type);
}

struct Type *own_field_type(struct Type *type, const char *field, int *index) {
  type = own_strip(type);
  if (index) {
    *index = -1;
  }
  if (!type || type->kind != TYPE_STRUCT || !field) {
    return NULL;
  }
  for (size_t i = 0; i < type->field_count; i++) {
    if (type->field_names && type->field_names[i] &&
        strcmp(type->field_names[i], field) == 0) {
      if (index) {
        *index = (int)i;
      }
      return type->field_types ? type->field_types[i] : NULL;
    }
  }
  return NULL;
}

struct Type *own_declared_type(TypeChecker *checker, VarDeclaration *decl) {
  struct Type *type = NULL;
  if (!decl) {
    return NULL;
  }
  if (decl->type_name) {
    type = type_checker_get_type_by_name(checker, decl->type_name);
  }
  if (!type && decl->initializer) {
    type = decl->initializer->resolved_type;
  }
  return type;
}

struct Type *own_param_type(TypeChecker *checker, FunctionDeclaration *fd,
                            size_t index) {
  if (!fd || index >= fd->parameter_count || !fd->parameter_types ||
      !fd->parameter_types[index]) {
    return NULL;
  }
  return type_checker_get_type_by_name(checker, fd->parameter_types[index]);
}

char *own_type_text(struct Type *type) {
  char room[256];
  const char *text = NULL;
  if (!type) {
    return NULL;
  }
  text = type_checker_spell_type(type, room, sizeof(room));
  return own_strdup(text ? text : type->name);
}

static void own_sanitize(const char *in, char *out, size_t size) {
  size_t used = 0;
  for (const char *p = in; p && *p && used + 5 < size; p++) {
    if (*p == '*') {
      memcpy(out + used, "_ptr", 4);
      used += 4;
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_') {
      out[used++] = *p;
    } else {
      out[used++] = '_';
    }
  }
  out[used] = '\0';
}

static int own_type_needs_glue(TypeChecker *checker, struct Type *type) {
  type = own_strip(type);
  if (!type || !own_type_owns(checker, type)) {
    return 0;
  }
  if (type->kind == TYPE_ARRAY || type->kind == TYPE_TAGGED_ENUM) {
    return 1;
  }
  return own_struct_fields_own(checker, type);
}

static char *own_glue_name(struct Type *type) {
  char base[300];
  char name[400];
  type = own_strip(type);
  if (type->kind == TYPE_ARRAY) {
    char *inner = own_glue_name(type->base_type);
    snprintf(name, sizeof(name), "%s_x%zu", inner ? inner : "__drop",
             type->array_size);
    free(inner);
    return own_strdup(name);
  }
  own_sanitize(type->name, base, sizeof(base));
  snprintf(name, sizeof(name), "__drop_%s", base);
  return own_strdup(name);
}

void own_require_glue(TypeChecker *checker, struct Type *type,
                      SourceLocation site) {
  type = own_strip(type);
  if (!own_type_needs_glue(checker, type)) {
    return;
  }
  for (size_t i = 0; i < g_own.glue_count; i++) {
    if (g_own.glue_queue[i] == type) {
      return;
    }
  }
  struct Type **grown = own_grow(g_own.glue_queue, &g_own.glue_capacity,
                                 g_own.glue_count, sizeof(*g_own.glue_queue));
  if (!grown) {
    return;
  }
  g_own.glue_queue = grown;
  SourceLocation *sites = own_grow(g_own.glue_sites,
                                   &g_own.glue_site_capacity,
                                   g_own.glue_count,
                                   sizeof(*g_own.glue_sites));
  if (!sites) {
    return;
  }
  g_own.glue_sites = sites;
  g_own.glue_sites[g_own.glue_count] = site;
  g_own.glue_queue[g_own.glue_count++] = type;
}

const char *ownership_drop_callee(TypeChecker *checker, struct Type *type) {
  static char name[400];
  char *glue = NULL;
  type = own_strip(type);
  if (!type || !own_type_owns(checker, type)) {
    return NULL;
  }
  if (!own_type_needs_glue(checker, type)) {
    return own_destructor_name(checker, type);
  }
  glue = own_glue_name(type);
  snprintf(name, sizeof(name), "%s", glue ? glue : "");
  free(glue);
  return name;
}

int ownership_is_destructor_name(TypeChecker *checker, const char *name) {
  size_t length = name ? strlen(name) : 0;
  static const char suffix[] = "_drop__ptr";
  if (length <= sizeof(suffix) - 1 ||
      strcmp(name + length - (sizeof(suffix) - 1), suffix) != 0) {
    return 0;
  }
  char type_name[512];
  snprintf(type_name, sizeof(type_name), "%.*s",
           (int)(length - (sizeof(suffix) - 1)), name);
  struct Type *type = type_checker_get_type_by_name(checker, type_name);
  return type && type->kind == TYPE_STRUCT;
}

int own_is_glue_function(const FunctionDeclaration *fd) {
  return fd && fd->ownership_role == 1;
}

const char *own_function_name(const OwnWalker *walker) {
  return walker && walker->function && walker->function->name
             ? walker->function->name
             : "?";
}

struct Type *own_void_type(TypeChecker *checker) {
  return checker ? checker->builtin_void : NULL;
}

static void own_program_append(ASTNode *program, ASTNode *decl) {
  Program *prog = (Program *)program->data;
  ASTNode **grown = realloc(prog->declarations,
                            (prog->declaration_count + 1) * sizeof(ASTNode *));
  if (!grown) {
    return;
  }
  prog->declarations = grown;
  prog->declarations[prog->declaration_count++] = decl;
  ast_add_child(program, decl);
}

static SourceLocation own_type_location(struct Type *type) {
  SourceLocation none = {0, 0, NULL};
  Program *program = g_own.program ? (Program *)g_own.program->data : NULL;
  for (size_t i = 0; type && type->name && program &&
                     i < program->declaration_count;
       i++) {
    ASTNode *decl = program->declarations[i];
    const char *name = NULL;
    if (decl && decl->type == AST_STRUCT_DECLARATION && decl->data) {
      name = ((StructDeclaration *)decl->data)->name;
    } else if (decl && decl->type == AST_ENUM_DECLARATION && decl->data) {
      name = ((EnumDeclaration *)decl->data)->name;
    }
    if (name && strcmp(name, type->name) == 0) {
      return decl->location;
    }
  }
  return none;
}

static ASTNode *own_self_field(const char *field, SourceLocation at) {
  ASTNode *self = ast_create_identifier("self", at);
  ASTNode *deref = ast_create_unary_expression("*", self, at);
  return ast_create_member_access(deref, field, at);
}

static ASTNode *own_drop_of(ASTNode *place, SourceLocation at) {
  ASTNode *args[1] = {place};
  return ast_create_call_expression("drop", args, 1, at);
}

static void own_glue_struct_body(TypeChecker *checker, struct Type *type,
                                 ASTNode *body, SourceLocation at) {
  const char *destructor = own_destructor_name(checker, type);
  size_t index = 0;
  if (destructor) {
    ASTNode *args[1] = {ast_create_identifier("self", at)};
    own_block_insert(body, index++,
                     ast_create_call_expression(destructor, args, 1, at));
  }
  for (size_t f = type->field_count; f > 0; f--) {
    struct Type *field_type = type->field_types[f - 1];
    if (!own_type_owns(checker, field_type)) {
      continue;
    }
    own_require_glue(checker, field_type, at);
    own_block_insert(
        body, index++,
        own_drop_of(own_self_field(type->field_names[f - 1], at), at));
  }
}

static void own_glue_array_body(TypeChecker *checker, struct Type *type,
                                ASTNode *body, SourceLocation at) {
  ASTNode *count = ast_create_number_literal((long long)type->array_size, at,
                                             10);
  ASTNode *zero = ast_create_number_literal(0, at, 10);
  ASTNode *one = ast_create_number_literal(1, at, 10);
  ASTNode *index_decl = ast_create_var_declaration("i", "int64", count, at);
  ASTNode *condition = ast_create_binary_expression(
      ast_create_identifier("i", at), ">", zero, at);
  ASTNode *step = ast_create_assignment(
      "i",
      ast_create_binary_expression(ast_create_identifier("i", at), "-", one,
                                   at),
      at);
  ASTNode *element = ast_create_array_index_expression(
      ast_create_identifier("self", at), ast_create_identifier("i", at), at);
  ASTNode *loop_body = own_make_block(at);
  ASTNode *loop = ast_create_node(AST_WHILE_STATEMENT, at);
  WhileStatement *data = calloc(1, sizeof(WhileStatement));
  own_require_glue(checker, type->base_type, at);
  own_block_insert(loop_body, 0, step);
  own_block_insert(loop_body, 1, own_drop_of(element, at));
  data->condition = condition;
  data->body = loop_body;
  loop->data = data;
  ast_add_child(loop, condition);
  ast_add_child(loop, loop_body);
  own_block_insert(body, 0, index_decl);
  own_block_insert(body, 1, loop);
}

static void own_glue_enum_body(TypeChecker *checker, struct Type *type,
                               ASTNode *body, SourceLocation at) {
  MatchArm *arms = calloc(type->tagged_variant_count + 1, sizeof(MatchArm));
  size_t arm_count = 0;
  int needs_default = 0;
  ASTNode *scrutinee = ast_create_unary_expression(
      "*", ast_create_identifier("self", at), at);
  if (!arms) {
    return;
  }
  for (size_t v = 0; v < type->tagged_variant_count; v++) {
    struct Type *payload = type->tagged_variant_payloads
                               ? type->tagged_variant_payloads[v]
                               : NULL;
    if (!own_type_owns(checker, payload)) {
      needs_default = 1;
      continue;
    }
    ASTNode *arm_body = own_make_block(at);
    own_require_glue(checker, payload, at);
    own_block_insert(arm_body, 0,
                     own_drop_of(ast_create_identifier("payload", at), at));
    arms[arm_count].variant_name = type->tagged_variant_names[v];
    arms[arm_count].binding_name = "payload";
    arms[arm_count].body = arm_body;
    arm_count++;
  }
  if (needs_default) {
    arms[arm_count].variant_name = "default";
    arms[arm_count].is_default = 1;
    arms[arm_count].body = own_make_block(at);
    arm_count++;
  }
  own_block_insert(body, 0,
                   ast_create_match_statement(scrutinee, arms, arm_count, at));
  free(arms);
}

static ASTNode *own_build_glue(TypeChecker *checker, struct Type *type,
                               SourceLocation site) {
  char *name = own_glue_name(type);
  char *pointee = own_type_text(type->kind == TYPE_ARRAY ? type->base_type
                                                         : type);
  char param_type[600];
  SourceLocation at = own_type_location(
      type->kind == TYPE_ARRAY ? type->base_type : type);
  if (at.line == 0) {
    at = site;
  }
  ASTNode *body = own_make_block(at);
  char *names[1] = {"self"};
  char *types[1] = {param_type};
  ASTNode *fn = NULL;
  snprintf(param_type, sizeof(param_type), "%s*", pointee ? pointee : "?");
  if (type->kind == TYPE_ARRAY) {
    own_glue_array_body(checker, type, body, at);
  } else if (type->kind == TYPE_TAGGED_ENUM) {
    own_glue_enum_body(checker, type, body, at);
  } else {
    own_glue_struct_body(checker, type, body, at);
  }
  fn = ast_create_function_declaration(name, names, types, 1, NULL, body, at);
  if (fn && fn->data) {
    ((FunctionDeclaration *)fn->data)->ownership_role = 1;
  }
  free(name);
  free(pointee);
  return fn;
}

static void own_glue_record(TypeChecker *checker, struct Type *type,
                            ASTNode *fn, SourceLocation site) {
  char reason[400];
  char *text = own_type_text(type);
  FunctionDeclaration *fd = (FunctionDeclaration *)fn->data;
  const char *what = "each owning field, last to first";
  SourceLocation at = fn->location.line > 0 ? fn->location : site;
  if (type->kind == TYPE_ARRAY) {
    what = "each element, last to first";
  } else if (type->kind == TYPE_TAGGED_ENUM) {
    what = "the payload of the variant it holds";
  } else if (own_destructor_name(checker, type)) {
    what = "its own drop method, then each owning field, last to first";
  }
  snprintf(reason, sizeof(reason), "dropping a %s runs %s",
           text ? text : "value", what);
  own_attach_note(fn, OWN_RECORD_GLUE, fd->name, type, at, at, reason,
                  fd->name);
  own_add_record(OWN_RECORD_GLUE, at, at, fd->name, fd->name, type, NULL,
                 reason, 1, 0);
  free(text);
}

static int own_generate_glue(TypeChecker *checker, ASTNode *program) {
  int ok = 1;
  while (g_own.glue_done < g_own.glue_count) {
    SourceLocation site = g_own.glue_sites[g_own.glue_done];
    struct Type *type = g_own.glue_queue[g_own.glue_done++];
    ASTNode *fn = own_build_glue(checker, type, site);
    if (!fn) {
      return 0;
    }
    own_program_append(program, fn);
    own_glue_record(checker, type, fn, site);
    if (!type_checker_register_function_signature(checker, fn) ||
        !type_checker_process_declaration(checker, fn)) {
      ok = 0;
    }
  }
  return ok;
}

static int own_node_involves_ownership(TypeChecker *checker,
                                       const ASTNode *node) {
  if (!node) {
    return 0;
  }
  if (node->resolved_type && own_type_owns(checker, node->resolved_type)) {
    return 1;
  }
  if (node->type == AST_VAR_DECLARATION && node->data &&
      own_type_owns(checker,
                    own_declared_type(checker, (VarDeclaration *)node->data))) {
    return 1;
  }
  if (node->type == AST_FUNCTION_CALL && node->data) {
    const char *name = ((CallExpression *)node->data)->function_name;
    if (name && (strcmp(name, "drop") == 0 || strcmp(name, "forget") == 0 ||
                 ownership_is_destructor_name(checker, name))) {
      return 1;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (own_node_involves_ownership(checker, node->children[i])) {
      return 1;
    }
  }
  return 0;
}

static int own_function_needs_walk(TypeChecker *checker, ASTNode *decl) {
  FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
  if (!fd || !fd->body || fd->is_extern || own_is_glue_function(fd) ||
      fd->meta_role != META_ROLE_NONE || fd->type_param_count > 0) {
    return 0;
  }
  for (size_t i = 0; i < fd->parameter_count; i++) {
    if (own_type_owns(checker, own_param_type(checker, fd, i))) {
      return 1;
    }
  }
  return own_node_involves_ownership(checker, fd->body);
}

static void own_elaborate(TypeChecker *checker, ErrorReporter *reporter,
                          ASTNode *decl) {
  for (int round = 0; round < 12; round++) {
    OwnWalker walker;
    int applied = 0;
    own_walker_init(&walker, checker, reporter, decl, OWN_MODE_DISCOVER);
    own_walk_function(&walker);
    if (walker.obligation_count > 0) {
      applied = own_apply_obligations(&walker);
    }
    own_walker_free(&walker);
    if (applied == 0) {
      return;
    }
  }
}

static int own_run_function(TypeChecker *checker, ErrorReporter *reporter,
                            ASTNode *decl) {
  FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
  OwnWalker walker;
  int errors = 0;
  if (!fd->is_explicit) {
    own_elaborate(checker, reporter, decl);
  }
  own_walker_init(&walker, checker, reporter, decl, OWN_MODE_CHECK);
  own_walk_function(&walker);
  errors = walker.errors;
  if (walker.obligation_count > 0) {
    errors += own_report_obligations(&walker);
  }
  if (errors == 0) {
    own_commit_outputs(&walker);
  }
  own_walker_free(&walker);
  return errors;
}

static int own_check_destructor(TypeChecker *checker, ErrorReporter *reporter,
                                ASTNode *decl) {
  FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
  size_t length = fd && fd->name ? strlen(fd->name) : 0;
  char type_name[512];
  char expected[600];
  if (length > 0 && !fd->is_extern &&
      (strcmp(fd->name, "drop") == 0 || strcmp(fd->name, "forget") == 0)) {
    error_reporter_add_error_with_suggestion(
        reporter, ERROR_SEMANTIC, decl->location,
        "`drop` and `forget` are built in, so a function cannot take either "
        "name",
        "rename this function; `drop(x)` ends the life of an owned value "
        "and `forget(x)` gives it up without dropping it");
    error_reporter_set_last_code(reporter, "O0012");
    return 1;
  }
  if (length > 5 && strcmp(fd->name + length - 5, "_drop") == 0) {
    snprintf(type_name, sizeof(type_name), "%.*s", (int)(length - 5),
             fd->name);
    struct Type *type = type_checker_get_type_by_name(checker, type_name);
    if (type && type->kind == TYPE_STRUCT) {
      error_reporter_add_error_with_suggestion(
          reporter, ERROR_SEMANTIC, decl->location,
          "a destructor takes its value through a pointer",
          "name it the pointer-receiver way and take a pointer, as "
          "`fn T_drop__ptr(self: T*)`, or as `method drop()` inside the "
          "struct");
      error_reporter_set_last_code(reporter, "O0011");
      return 1;
    }
  }
  if (!fd || !ownership_is_destructor_name(checker, fd->name)) {
    return 0;
  }
  snprintf(type_name, sizeof(type_name), "%.*s", (int)(length - 10),
           fd->name);
  snprintf(expected, sizeof(expected), "%s*", type_name);
  if (fd->parameter_count != 1 || !fd->parameter_types ||
      strcmp(fd->parameter_types[0], expected) != 0 ||
      (fd->return_type && strcmp(fd->return_type, "void") != 0)) {
    error_reporter_add_error_with_suggestion(
        reporter, ERROR_SEMANTIC, decl->location,
        "a destructor takes one pointer to its type and returns nothing",
        "write it as `fn T_drop__ptr(self: T*)`");
    error_reporter_set_last_code(reporter, "O0011");
    return 1;
  }
  return 0;
}

int ownership_run(TypeChecker *checker, ASTNode *program,
                  ErrorReporter *reporter) {
  Program *prog = program ? (Program *)program->data : NULL;
  size_t count = prog ? prog->declaration_count : 0;
  int errors = 0;
  g_own.program = program;
  g_own.checker = checker;
  for (size_t i = 0; i < count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_FUNCTION_DECLARATION && decl->data) {
      errors += own_check_destructor(checker, reporter, decl);
    }
  }
  if (errors > 0) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_FUNCTION_DECLARATION &&
        own_function_needs_walk(checker, decl)) {
      errors += own_run_function(checker, reporter, decl);
    }
  }
  if (errors == 0 && !own_generate_glue(checker, program)) {
    errors++;
  }
  return errors == 0;
}
