#include "type_checker_internal.h"
#include <ctype.h>

typedef struct {
  const char *binding_name;
  ComptimeValue binding_value;
  Type *binding_type;
  const char *note;
  SourceSpan origin;
} ComptimeFrame;

typedef struct {
  ASTNode *block;
  char *note;
  SourceSpan origin;
  const char *binding_name;
  ComptimeValue binding_value;
  Type *binding_type;
  ComptimeFrame *inherited;
  size_t inherited_count;
} ComptimeExpansion;

struct ComptimeBindingSlot {
  Symbol *symbol;
  const char *note;
  SourceSpan origin;
};

#define COMPTIME_MAX_ROUNDS 4096

static int binding_push(TypeChecker *checker, const char *name,
                        ComptimeValue value, Type *declared, SourceSpan origin,
                        const char *note);
static void binding_pop(TypeChecker *checker);
static int resolve_composed_names(TypeChecker *checker, ASTNode *node);
static int capture_inherited(TypeChecker *checker, ComptimeExpansion *entry,
                             size_t count);

typedef struct {
  SourceLocation location;
  const char *type_name;
  size_t iterations;
  size_t nodes;
} ComptimeSiteCost;

struct ComptimeExpansionTable {
  ComptimeExpansion *slots;
  size_t capacity;
  size_t count;
  ComptimeSiteCost *costs;
  size_t cost_count;
  size_t cost_capacity;
};

static size_t ast_node_count(const ASTNode *node) {
  if (!node) {
    return 0;
  }
  size_t total = 1;
  for (size_t i = 0; i < node->child_count; i++) {
    total += ast_node_count(node->children[i]);
  }
  return total;
}

static void expansion_record_cost(ComptimeExpansionTable *table,
                                  SourceLocation location,
                                  const char *type_name, size_t iterations,
                                  size_t nodes) {
  if (!table) {
    return;
  }
  if (table->cost_count == table->cost_capacity) {
    size_t next = table->cost_capacity ? table->cost_capacity * 2 : 8;
    ComptimeSiteCost *grown =
        realloc(table->costs, next * sizeof(ComptimeSiteCost));
    if (!grown) {
      return;
    }
    table->costs = grown;
    table->cost_capacity = next;
  }
  table->costs[table->cost_count].location = location;
  table->costs[table->cost_count].type_name = type_name;
  table->costs[table->cost_count].iterations = iterations;
  table->costs[table->cost_count].nodes = nodes;
  table->cost_count++;
}

static size_t expansion_hash(const ASTNode *block, size_t capacity) {
  uintptr_t bits = (uintptr_t)block;
  bits ^= bits >> 33;
  bits *= (uintptr_t)0xff51afd7ed558ccdULL;
  bits ^= bits >> 29;
  return (size_t)bits & (capacity - 1);
}

static int expansion_table_grow(ComptimeExpansionTable *table) {
  size_t next = table->capacity ? table->capacity * 2 : 16;
  ComptimeExpansion *slots = calloc(next, sizeof(ComptimeExpansion));
  if (!slots) {
    return 0;
  }
  for (size_t i = 0; i < table->capacity; i++) {
    if (!table->slots[i].block) {
      continue;
    }
    size_t at = expansion_hash(table->slots[i].block, next);
    while (slots[at].block) {
      at = (at + 1) & (next - 1);
    }
    slots[at] = table->slots[i];
  }
  free(table->slots);
  table->slots = slots;
  table->capacity = next;
  return 1;
}

static int expansion_table_put(ComptimeExpansionTable *table,
                               ComptimeExpansion entry) {
  if (!table->capacity || (table->count + 1) * 4 >= table->capacity * 3) {
    if (!expansion_table_grow(table)) {
      return 0;
    }
  }
  size_t at = expansion_hash(entry.block, table->capacity);
  while (table->slots[at].block) {
    if (table->slots[at].block == entry.block) {
      return 0;
    }
    at = (at + 1) & (table->capacity - 1);
  }
  table->slots[at] = entry;
  table->count++;
  return 1;
}

static const ComptimeExpansion *
expansion_table_get(const ComptimeExpansionTable *table, const ASTNode *block) {
  if (!table || !table->capacity || !block) {
    return NULL;
  }
  size_t at = expansion_hash(block, table->capacity);
  while (table->slots[at].block) {
    if (table->slots[at].block == block) {
      return &table->slots[at];
    }
    at = (at + 1) & (table->capacity - 1);
  }
  return NULL;
}

void type_checker_expansions_destroy(ComptimeExpansionTable *table) {
  if (!table) {
    return;
  }
  for (size_t i = 0; i < table->capacity; i++) {
    free(table->slots[i].note);
    free(table->slots[i].inherited);
  }
  free(table->slots);
  free(table->costs);
  free(table);
}

size_t type_checker_expansion_site_count(const TypeChecker *checker) {
  return checker && checker->expansions ? checker->expansions->cost_count : 0;
}

size_t type_checker_expansion_total_nodes(const TypeChecker *checker) {
  if (!checker || !checker->expansions) {
    return 0;
  }
  size_t total = 0;
  for (size_t i = 0; i < checker->expansions->cost_count; i++) {
    total += checker->expansions->costs[i].nodes;
  }
  return total;
}

void type_checker_report_expansion(const TypeChecker *checker, FILE *out) {
  if (!out) {
    return;
  }
  size_t sites = type_checker_expansion_site_count(checker);
  if (checker->schedule_stats.schedules > 0) {
    fprintf(out,
            "schedules: %zu read as data, %zu phases, %zu functions "
            "generated\n",
            checker->schedule_stats.schedules, checker->schedule_stats.phases,
            checker->schedule_stats.generated);
  } else {
    fprintf(out, "schedules: none; nothing generated\n");
  }
  if (checker->comptime_text_bytes > 0) {
    fprintf(out, "comptime text: %zu bytes built while compiling\n",
            checker->comptime_text_bytes);
  }
  if (sites == 0) {
    fprintf(out, "comptime expansion: no sites; nothing generated\n");
    return;
  }

  fprintf(out,
          "comptime expansion: %zu site%s, %zu nodes generated, %zu round%s to "
          "settle\n",
          sites, sites == 1 ? "" : "s",
          type_checker_expansion_total_nodes(checker),
          checker->expansion_rounds,
          checker->expansion_rounds == 1 ? "" : "s");
  for (size_t i = 0; i < checker->expansions->cost_count; i++) {
    const ComptimeSiteCost *cost = &checker->expansions->costs[i];
    fprintf(out, "  %s:%zu:%zu  %s  %zu iteration%s, %zu nodes\n",
            cost->location.filename ? cost->location.filename : "<input>",
            cost->location.line, cost->location.column,
            cost->type_name ? cost->type_name : "<anonymous>",
            cost->iterations, cost->iterations == 1 ? "" : "s", cost->nodes);
  }
}

int type_checker_check_expansion_budget(TypeChecker *checker, size_t budget) {
  size_t total = type_checker_expansion_total_nodes(checker) +
                 checker->comptime_text_bytes;
  if (total <= budget) {
    return 1;
  }

  const ComptimeSiteCost *worst = NULL;
  for (size_t i = 0; i < checker->expansions->cost_count; i++) {
    if (!worst || checker->expansions->costs[i].nodes > worst->nodes) {
      worst = &checker->expansions->costs[i];
    }
  }
  if (worst) {
    type_checker_set_error_at_location(
        checker, worst->location,
        "comptime expansion generated %zu nodes, over the budget of %zu; this "
        "site generated %zu of them across %zu iterations",
        total, budget, worst->nodes, worst->iterations);
  }
  return 0;
}

typedef struct {
  int is_table;
  int is_range;
  long long range_start;
  Type *range_type;
  Type *owner;
  uint32_t owner_index;
  const char *label;
  size_t count;
  AggregateLiteral *table;
} ComptimeSource;

#define COMPTIME_RANGE_MAX 65536

static int resolve_range_sequence(TypeChecker *checker,
                                  ComptimeForStatement *directive,
                                  ComptimeSource *out) {
  ComptimeValue low = comptime_none(), high = comptime_none();
  long long count;
  Type *bound_type;
  if (!type_checker_eval_comptime(checker, directive->sequence, &low) ||
      low.kind != COMPTIME_INT) {
    type_checker_set_error_at_location(
        checker, directive->sequence->location,
        "a 'comptime for' range starts at a compile-time integer");
    return 0;
  }
  if (!type_checker_eval_comptime(checker, directive->range_end, &high) ||
      high.kind != COMPTIME_INT) {
    type_checker_set_error_at_location(
        checker, directive->range_end->location,
        "a 'comptime for' range ends at a compile-time integer");
    return 0;
  }
  count = high.as.int_value - low.as.int_value +
          (directive->range_inclusive ? 1 : 0);
  if (count < 0) {
    count = 0;
  }
  if (count > COMPTIME_RANGE_MAX) {
    type_checker_set_error_at_location(
        checker, directive->range_end->location,
        "a 'comptime for' range of %lld values is over the limit of %d",
        count, COMPTIME_RANGE_MAX);
    return 0;
  }
  bound_type = type_checker_infer_type(checker, directive->range_end);
  out->is_range = 1;
  out->range_start = low.as.int_value;
  out->range_type = bound_type && type_checker_is_integer_type(bound_type)
                        ? bound_type
                        : checker->builtin_int32;
  out->label = "range";
  out->count = (size_t)count;
  return 1;
}

static ASTNode *module_const_initializer(TypeChecker *checker,
                                         const char *name, Type **out_type) {
  Program *module = NULL;
  size_t i;
  if (!checker || !checker->module_program || !name) {
    return NULL;
  }
  module = (Program *)checker->module_program->data;
  if (!module) {
    return NULL;
  }
  for (i = 0; i < module->declaration_count; i++) {
    ASTNode *declaration = module->declarations[i];
    VarDeclaration *var = NULL;
    if (!declaration || declaration->type != AST_VAR_DECLARATION) {
      continue;
    }
    var = (VarDeclaration *)declaration->data;
    if (!var || !var->is_const || !var->name || strcmp(var->name, name) != 0) {
      continue;
    }
    if (!var->initializer ||
        var->initializer->type != AST_AGGREGATE_LITERAL || !var->type_name) {
      return NULL;
    }
    *out_type = type_checker_get_type_by_name(checker, var->type_name);
    return *out_type ? var->initializer : NULL;
  }
  return NULL;
}

static int resolve_table_sequence(TypeChecker *checker, ASTNode *sequence,
                                  ComptimeSource *out) {
  MemberAccess *member =
      sequence && sequence->type == AST_MEMBER_ACCESS
          ? (MemberAccess *)sequence->data
          : NULL;
  Identifier *named = NULL;
  Symbol *symbol = NULL;
  AggregateLiteral *literal = NULL;
  ASTNode *initializer = NULL;
  Type *table_type = NULL;

  if (!member || !member->member || strcmp(member->member, "rows") != 0) {
    return 0;
  }
  if (!member->object || member->object->type != AST_IDENTIFIER) {
    type_checker_set_error_at_location(
        checker,
        member->object ? member->object->location : sequence->location,
        "'.rows' needs a constant table on its left, named where it was "
        "declared");
    return -1;
  }
  named = (Identifier *)member->object->data;
  symbol = named ? type_checker_resolve_identifier(checker, named) : NULL;
  if (symbol && symbol->constant_initializer &&
      symbol->constant_initializer->type == AST_AGGREGATE_LITERAL &&
      symbol->type) {
    table_type = symbol->type;
    initializer = symbol->constant_initializer;
  } else {
    initializer = module_const_initializer(checker, named ? named->name : NULL,
                                           &table_type);
  }
  if (!initializer || !table_type) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "'%s' is not a constant table; a table is a 'const' holding an array "
        "literal",
        named && named->name ? named->name : "<unknown>");
    return -1;
  }
  if (table_type->kind != TYPE_ARRAY || !table_type->base_type) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "'%s' is a constant, and a table has to be an array of them",
        named->name ? named->name : "<unknown>");
    return -1;
  }
  literal = (AggregateLiteral *)initializer->data;
  if (!literal || literal->is_struct) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "'%s' is not written as an array literal",
        named->name ? named->name : "<unknown>");
    return -1;
  }

  out->is_table = 1;
  out->owner = table_type->base_type;
  out->owner_index = type_checker_intern_type(checker, out->owner);
  out->label = named->name;
  out->table = literal;
  out->count = literal->element_count;
  return 1;
}

static int resolve_sequence(TypeChecker *checker, ASTNode *sequence,
                            ComptimeSource *out) {
  int table = 0;
  memset(out, 0, sizeof(*out));
  if (!sequence || sequence->type != AST_MEMBER_ACCESS) {
    type_checker_set_error_at_location(
        checker, sequence ? sequence->location : (SourceLocation){0, 0, NULL},
        "'comptime for' iterates a compile-time sequence: '<type>.fields', "
        "'<table>.rows' for a constant table, or a range 'lo..hi'");
    return 0;
  }

  table = resolve_table_sequence(checker, sequence, out);
  if (table != 0) {
    return table > 0;
  }

  MemberAccess *member = (MemberAccess *)sequence->data;
  if (!member || !member->member || strcmp(member->member, "fields") != 0) {
    type_checker_set_error_at_location(
        checker, sequence->location,
        "'comptime for' cannot iterate '.%s'; the compile-time sequences are "
        "'.fields', '.rows' and a range 'lo..hi'",
        member && member->member ? member->member : "<unknown>");
    return 0;
  }

  ComptimeValue owner = comptime_none();
  if (!type_checker_eval_comptime(checker, member->object, &owner) ||
      owner.kind != COMPTIME_TYPE_REF) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "'.fields' needs a compile-time type on its left, for example "
        "'typeof(T).fields'");
    return 0;
  }

  Type *referred =
      type_checker_type_from_index(checker, owner.as.type_ref.type_index);
  if (!referred) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "'.fields' refers to a type that is not in the type table");
    return 0;
  }
  if (referred->kind != TYPE_STRUCT && referred->kind != TYPE_STRING) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "type '%s' has no fields to iterate",
        referred->name ? referred->name : "<anonymous>");
    return 0;
  }
  out->is_table = 0;
  out->owner = referred;
  out->owner_index = type_checker_intern_type(checker, referred);
  out->label = referred->name;
  out->count = type_field_count(referred);
  return 1;
}

static void iteration_note(char *out, size_t capacity,
                           const ComptimeSource *source, const TypeField *field,
                           size_t index) {
  if (source && source->is_range) {
    snprintf(out, capacity, "expanded from comptime-for iteration %zu (value %lld)",
             index + 1, source->range_start + (long long)index);
    return;
  }
  if (source && source->is_table) {
    snprintf(out, capacity, "expanded from comptime-for iteration %zu (row %zu of `%s`)",
             index + 1, index + 1, source->label ? source->label : "<table>");
    return;
  }
  snprintf(out, capacity, "expanded from comptime-for iteration %zu (field `%s`)",
           index + 1, field && field->name ? field->name : "<anonymous>");
}

static Type *binding_declared_type(const ComptimeSource *source) {
  if (source && source->is_range) {
    return source->range_type;
  }
  if (!source || !source->is_table || !source->owner) {
    return NULL;
  }
  return source->owner->kind == TYPE_STRUCT ? NULL : source->owner;
}

static ComptimeValue iteration_value(TypeChecker *checker,
                                     const ComptimeSource *source,
                                     size_t index) {
  if (source->is_range) {
    return comptime_int(source->range_start + (long long)index);
  }
  if (source->is_table) {
    ASTNode *row = source->table && index < source->table->element_count
                       ? source->table->elements[index]
                       : NULL;
    if (row && row->type == AST_AGGREGATE_LITERAL) {
      return comptime_row(row->data, source->owner_index, (uint32_t)index);
    }
    {
      ComptimeValue scalar = comptime_none();
      if (row && type_checker_eval_comptime(checker, row, &scalar)) {
        return scalar;
      }
    }
    return comptime_row(NULL, source->owner_index, (uint32_t)index);
  }
  return comptime_field_ref(source->owner_index, (uint32_t)index);
}

static int register_expansion(TypeChecker *checker,
                              ComptimeForStatement *directive, ASTNode *clone,
                              const ComptimeSource *source,
                              const TypeField *field, size_t field_index,
                              size_t inherit_count) {
  char note[256];
  iteration_note(note, sizeof(note), source, field, field_index);

  ComptimeExpansion entry;
  entry.block = clone;
  entry.note = mettle_strdup(note);
  entry.origin = source_span_from_location(directive->keyword_location,
                                           strlen("comptime"));
  entry.binding_name = string_intern(directive->binding_name);
  entry.binding_value = iteration_value(checker, source, field_index);
  entry.binding_type = binding_declared_type(source);
  entry.inherited = NULL;
  entry.inherited_count = 0;

  int captured = capture_inherited(checker, &entry, inherit_count);

  if (!captured || !entry.note || !entry.binding_name ||
      !expansion_table_put(checker->expansions, entry)) {
    free(entry.inherited);
    free(entry.note);
    type_checker_set_error_at_location(
        checker, directive->keyword_location,
        "Out of memory recording 'comptime for' iteration %zu",
        field_index + 1);
    return 0;
  }
  return 1;
}

static int read_field(TypeChecker *checker, ComptimeForStatement *directive,
                      Type *owner, size_t field_index, TypeField *out_field) {
  if (type_field_at(owner, field_index, out_field)) {
    return 1;
  }
  type_checker_set_error_at_location(
      checker, directive->keyword_location,
      "could not read field %zu of '%s' from the type table", field_index,
      owner->name ? owner->name : "<anonymous>");
  return 0;
}

static ASTNode *expand_iteration(TypeChecker *checker,
                                 ComptimeForStatement *directive,
                                 const ComptimeSource *source,
                                 size_t field_index) {
  TypeField field;
  memset(&field, 0, sizeof(field));
  if (!source->is_table && !source->is_range &&
      !read_field(checker, directive, source->owner, field_index, &field)) {
    return NULL;
  }

  ASTNode *clone = ast_clone_node(directive->body);
  if (!clone) {
    type_checker_set_error_at_location(
        checker, directive->keyword_location,
        "Out of memory expanding 'comptime for' iteration %zu",
        field_index + 1);
    return NULL;
  }

  SourceSpan origin = source_span_from_location(directive->keyword_location,
                                                strlen("comptime"));
  size_t inherit_count = checker->comptime_binding_count;
  char note[256];
  iteration_note(note, sizeof(note), source, &field, field_index);
  if (!binding_push(checker, string_intern(directive->binding_name),
                    iteration_value(checker, source, field_index),
                    binding_declared_type(source), origin, note)) {
    type_checker_set_error_at_location(
        checker, directive->keyword_location,
        "Out of memory binding '%s' for iteration %zu", directive->binding_name,
        field_index + 1);
    ast_destroy_node(clone);
    return NULL;
  }
  int resolved = resolve_composed_names(checker, clone);
  binding_pop(checker);

  if (!resolved ||
      !register_expansion(checker, directive, clone, source, &field,
                          field_index, inherit_count)) {
    ast_destroy_node(clone);
    return NULL;
  }
  return clone;
}

static const char *declaration_name(const ASTNode *node) {
  switch (node->type) {
  case AST_FUNCTION_DECLARATION: {
    const FunctionDeclaration *decl = (const FunctionDeclaration *)node->data;
    return decl ? decl->name : NULL;
  }
  case AST_STRUCT_DECLARATION: {
    const StructDeclaration *decl = (const StructDeclaration *)node->data;
    return decl ? decl->name : NULL;
  }
  case AST_VAR_DECLARATION: {
    const VarDeclaration *decl = (const VarDeclaration *)node->data;
    return decl ? decl->name : NULL;
  }
  case AST_ENUM_DECLARATION: {
    const EnumDeclaration *decl = (const EnumDeclaration *)node->data;
    return decl ? decl->name : NULL;
  }
  case AST_TYPE_DECLARATION: {
    const TypeDeclaration *decl = (const TypeDeclaration *)node->data;
    return decl ? decl->name : NULL;
  }
  default:
    return NULL;
  }
}

static int expand_declaration_iteration(TypeChecker *checker,
                                        ComptimeForStatement *directive,
                                        const ComptimeSource *source,
                                        size_t field_index, ASTNode **out,
                                        size_t *out_count) {
  TypeField field;
  memset(&field, 0, sizeof(field));
  if (!source->is_table && !source->is_range &&
      !read_field(checker, directive, source->owner, field_index, &field)) {
    return 0;
  }

  Program *body = (Program *)directive->body->data;
  size_t body_count = body ? body->declaration_count : 0;

  SourceSpan origin = source_span_from_location(directive->keyword_location,
                                                strlen("comptime"));
  size_t inherit_count = checker->comptime_binding_count;
  char note[256];
  iteration_note(note, sizeof(note), source, &field, field_index);
  if (!binding_push(checker, string_intern(directive->binding_name),
                    iteration_value(checker, source, field_index),
                    binding_declared_type(source), origin, note)) {
    type_checker_set_error_at_location(
        checker, directive->keyword_location,
        "Out of memory binding '%s' for iteration %zu",
        directive->binding_name, field_index + 1);
    return 0;
  }

  int ok = 1;
  for (size_t i = 0; i < body_count && ok; i++) {
    ASTNode *clone = ast_clone_node(body->declarations[i]);
    if (!clone) {
      type_checker_set_error_at_location(
          checker, directive->keyword_location,
          "Out of memory expanding 'comptime for' iteration %zu",
          field_index + 1);
      ok = 0;
      break;
    }
    if (!resolve_composed_names(checker, clone) ||
        !register_expansion(checker, directive, clone, source, &field,
                            field_index, inherit_count)) {
      ast_destroy_node(clone);
      ok = 0;
      break;
    }
    out[(*out_count)++] = clone;
  }

  binding_pop(checker);
  return ok;
}

static int check_generated_names(TypeChecker *checker,
                                 ComptimeForStatement *directive,
                                 ASTNode **generated, size_t count) {
  for (size_t i = 0; i < count; i++) {
    const char *name = declaration_name(generated[i]);
    if (!name) {
      continue;
    }
    for (size_t j = 0; j < i; j++) {
      const char *other = declaration_name(generated[j]);
      if (other && strcmp(name, other) == 0) {
        type_checker_set_error_at_location(
            checker, directive->keyword_location,
            "this 'comptime for' generated two declarations named '%s'; "
            "compose the name from the binding so each iteration differs, for "
            "example 'ident(\"%s_\", %s.name)'",
            name, name, directive->binding_name);
        return 0;
      }
    }
  }
  return 1;
}

static Symbol *binding_symbol(TypeChecker *checker, const char *name,
                              ComptimeValue value, Type *declared,
                              SourceSpan origin) {
  Type *binding_type = declared ? declared : checker->builtin_field;
  Symbol *symbol = NULL;
  if (value.kind == COMPTIME_ROW) {
    binding_type = checker->builtin_row;
  } else if (!declared) {
    switch (value.kind) {
    case COMPTIME_STRING:
      binding_type = checker->builtin_string;
      break;
    case COMPTIME_INT:
      binding_type = checker->builtin_int64;
      break;
    case COMPTIME_FLOAT:
      binding_type = checker->builtin_float64;
      break;
    default:
      break;
    }
  }
  symbol = symbol_create((char *)name, SYMBOL_CONSTANT, binding_type);
  if (!symbol) {
    return NULL;
  }
  symbol->comptime_value = value;
  symbol->is_comptime_binding = 1;
  if (value.kind == COMPTIME_INT) {
    symbol->has_constant_value = 1;
    symbol->constant_integer_value = value.as.int_value;
    symbol->data.constant.value = value.as.int_value;
  } else if (value.kind == COMPTIME_FLOAT) {
    symbol->has_constant_value = 1;
    symbol->constant_is_float = 1;
    symbol->constant_float_value = value.as.float_value;
  }
  symbol->is_initialized = 1;
  symbol->is_immutable = 1;
  symbol->decl_line = origin.line;
  symbol->decl_column = origin.column;
  symbol->decl_file = origin.filename;
  return symbol;
}

static int binding_push(TypeChecker *checker, const char *name,
                        ComptimeValue value, Type *declared, SourceSpan origin,
                        const char *note) {
  if (checker->comptime_binding_count == checker->comptime_binding_capacity) {
    size_t next = checker->comptime_binding_capacity
                      ? checker->comptime_binding_capacity * 2
                      : 4;
    struct ComptimeBindingSlot *grown =
        realloc(checker->comptime_bindings,
                next * sizeof(struct ComptimeBindingSlot));
    if (!grown) {
      return 0;
    }
    checker->comptime_bindings = grown;
    checker->comptime_binding_capacity = next;
  }
  Symbol *symbol = binding_symbol(checker, name, value, declared, origin);
  if (!symbol) {
    return 0;
  }
  struct ComptimeBindingSlot *slot =
      &checker->comptime_bindings[checker->comptime_binding_count++];
  slot->symbol = symbol;
  slot->note = note;
  slot->origin = origin;
  return 1;
}

static void binding_pop(TypeChecker *checker) {
  if (checker->comptime_binding_count > 0) {
    symbol_destroy(
        checker->comptime_bindings[--checker->comptime_binding_count].symbol);
  }
}

Symbol *type_checker_lookup_expansion_binding(const TypeChecker *checker,
                                              const char *name) {
  if (!checker || !name) {
    return NULL;
  }
  for (size_t i = checker->comptime_binding_count; i > 0; i--) {
    Symbol *binding = checker->comptime_bindings[i - 1].symbol;
    if (binding && binding->name && strcmp(binding->name, name) == 0) {
      return binding;
    }
  }
  return NULL;
}

static int capture_inherited(TypeChecker *checker, ComptimeExpansion *entry,
                             size_t count) {
  entry->inherited = NULL;
  entry->inherited_count =
      count <= checker->comptime_binding_count ? count
                                               : checker->comptime_binding_count;
  if (entry->inherited_count == 0) {
    return 1;
  }
  entry->inherited = calloc(entry->inherited_count, sizeof(ComptimeFrame));
  if (!entry->inherited) {
    entry->inherited_count = 0;
    return 0;
  }
  for (size_t i = 0; i < entry->inherited_count; i++) {
    const struct ComptimeBindingSlot *slot = &checker->comptime_bindings[i];
    entry->inherited[i].binding_name = slot->symbol ? slot->symbol->name : NULL;
    entry->inherited[i].binding_value =
        slot->symbol ? slot->symbol->comptime_value : comptime_none();
    entry->inherited[i].binding_type = slot->symbol ? slot->symbol->type : NULL;
    entry->inherited[i].note = slot->note;
    entry->inherited[i].origin = slot->origin;
  }
  return 1;
}

static void push_frame(TypeChecker *checker, const char *binding_name,
                       ComptimeValue binding_value, Type *binding_type,
                       SourceSpan origin, const char *note,
                       ComptimeDeclScope *scope) {
  if (binding_push(checker, binding_name, binding_value, binding_type, origin,
                   note)) {
    scope->bindings_pushed++;
  }
  if (note && checker->error_reporter &&
      error_reporter_push_note_frame(checker->error_reporter, origin, note)) {
    scope->notes_pushed++;
  }
}

static int resolve_directive_sequence(TypeChecker *checker,
                                      ComptimeForStatement *directive,
                                      ComptimeSource *source) {
  memset(source, 0, sizeof(*source));
  return directive->range_end
             ? resolve_range_sequence(checker, directive, source)
             : resolve_sequence(checker, directive->sequence, source);
}

static void push_origin(TypeChecker *checker,
                        const ComptimeExpansionOrigin *origin,
                        ComptimeDeclScope *scope) {
  ComptimeForStatement directive;
  ComptimeSource source;
  TypeField field;
  char note[256];
  if (origin->parent) {
    push_origin(checker, origin->parent, scope);
  }
  memset(&directive, 0, sizeof(directive));
  directive.binding_name = (char *)origin->binding_name;
  directive.sequence = ast_clone_node(origin->sequence);
  directive.range_end = ast_clone_node(origin->range_end);
  directive.range_inclusive = origin->range_inclusive;
  directive.keyword_location = origin->keyword_location;
  if (!resolve_directive_sequence(checker, &directive, &source) ||
      origin->index >= source.count) {
    return;
  }
  memset(&field, 0, sizeof(field));
  if (!source.is_table && !source.is_range &&
      !read_field(checker, &directive, source.owner, origin->index, &field)) {
    return;
  }
  iteration_note(note, sizeof(note), &source, &field, origin->index);
  push_frame(checker, string_intern(origin->binding_name),
             iteration_value(checker, &source, origin->index),
             binding_declared_type(&source),
             source_span_from_location(origin->keyword_location,
                                       strlen("comptime")),
             string_intern(note), scope);
}

void type_checker_enter_expansion_decl(TypeChecker *checker,
                                       const ASTNode *declaration,
                                       ComptimeDeclScope *scope) {
  scope->bindings_pushed = 0;
  scope->notes_pushed = 0;
  if (!checker || !declaration) {
    return;
  }
  const ComptimeExpansion *entry =
      expansion_table_get(checker->expansions, declaration);
  if (!entry) {
    if (declaration->expansion_origin) {
      push_origin(checker, declaration->expansion_origin, scope);
    }
    return;
  }
  for (size_t i = 0; i < entry->inherited_count; i++) {
    push_frame(checker, entry->inherited[i].binding_name,
               entry->inherited[i].binding_value,
               entry->inherited[i].binding_type, entry->inherited[i].origin,
               entry->inherited[i].note, scope);
  }
  push_frame(checker, entry->binding_name, entry->binding_value,
             entry->binding_type, entry->origin, entry->note, scope);
}

void type_checker_leave_expansion_decl(TypeChecker *checker,
                                       ComptimeDeclScope *scope) {
  if (!checker || !scope) {
    return;
  }
  while (scope->notes_pushed > 0) {
    error_reporter_pop_note_frame(checker->error_reporter);
    scope->notes_pushed--;
  }
  while (scope->bindings_pushed > 0) {
    binding_pop(checker);
    scope->bindings_pushed--;
  }
}

static int is_identifier_text(const char *text) {
  if (!text || !*text) {
    return 0;
  }
  if (!isalpha((unsigned char)text[0]) && text[0] != '_') {
    return 0;
  }
  for (const char *p = text + 1; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '_') {
      return 0;
    }
  }
  return 1;
}

static const char *compose_name(TypeChecker *checker, ASTNode *composed) {
  char joined[512];
  size_t length = 0;
  joined[0] = '\0';

  for (size_t i = 0; i < composed->child_count; i++) {
    ComptimeValue part = comptime_none();
    if (!type_checker_eval_comptime(checker, composed->children[i], &part) ||
        part.kind != COMPTIME_STRING || !part.as.string.value) {
      type_checker_set_error_at_location(
          checker, composed->children[i]->location,
          "'ident(...)' joins compile-time strings; part %zu is not one "
          "(a string literal or a '.name' query is)",
          i + 1);
      return NULL;
    }
    size_t part_length = strlen(part.as.string.value);
    if (length + part_length + 1 > sizeof(joined)) {
      type_checker_set_error_at_location(checker, composed->location,
                                         "'ident(...)' composed a name longer "
                                         "than %zu characters",
                                         sizeof(joined) - 1);
      return NULL;
    }
    memcpy(joined + length, part.as.string.value, part_length + 1);
    length += part_length;
  }

  if (!is_identifier_text(joined)) {
    type_checker_set_error_at_location(
        checker, composed->location,
        "'ident(...)' composed \"%s\", which is not a name a program could "
        "have written",
        joined);
    return NULL;
  }
  return string_intern(joined);
}

static int resolve_composed_names(TypeChecker *checker, ASTNode *node) {
  if (!node) {
    return 1;
  }

  if (node->type == AST_COMPTIME_FOR) {
    return 1;
  }

  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call && call->function_name &&
        strcmp(call->function_name, "ident") == 0) {
      const char *name = compose_name(checker, node);
      if (!name || !ast_fold_call_to_identifier(node, name)) {
        if (name) {
          type_checker_set_error_at_location(
              checker, node->location,
              "Out of memory resolving 'ident(...)'");
        }
        return 0;
      }
      return 1;
    }
  }

  ASTNode **slot = NULL;
  char **name_slot = NULL;
  switch (node->type) {
  case AST_FUNCTION_DECLARATION:
  case AST_LAMBDA_EXPRESSION: {
    FunctionDeclaration *decl = (FunctionDeclaration *)node->data;
    if (decl) {
      slot = &decl->composed_name;
      name_slot = &decl->name;
    }
    break;
  }
  case AST_STRUCT_DECLARATION: {
    StructDeclaration *decl = (StructDeclaration *)node->data;
    if (decl) {
      slot = &decl->composed_name;
      name_slot = &decl->name;
    }
    break;
  }
  case AST_TYPE_DECLARATION: {
    TypeDeclaration *decl = (TypeDeclaration *)node->data;
    if (decl) {
      slot = &decl->composed_name;
      name_slot = &decl->name;
    }
    break;
  }
  case AST_VAR_DECLARATION: {
    VarDeclaration *decl = (VarDeclaration *)node->data;
    if (decl) {
      slot = &decl->composed_name;
      name_slot = &decl->name;
    }
    break;
  }
  default:
    break;
  }

  if (slot && *slot) {
    const char *name = compose_name(checker, *slot);
    if (!name) {
      return 0;
    }
    *name_slot = (char *)name;
    ast_destroy_node(*slot);
    *slot = NULL;
  }

  for (size_t i = 0; i < node->child_count; i++) {
    if (!resolve_composed_names(checker, node->children[i])) {
      return 0;
    }
  }
  return 1;
}

int type_checker_check_composed_names(TypeChecker *checker, ASTNode *node) {
  if (!checker || !node) {
    return 1;
  }
  if (node->type == AST_COMPTIME_FOR) {
    return 1;
  }
  const ASTNode *composed = NULL;
  switch (node->type) {
  case AST_FUNCTION_DECLARATION:
  case AST_LAMBDA_EXPRESSION: {
    const FunctionDeclaration *decl = (const FunctionDeclaration *)node->data;
    composed = decl ? decl->composed_name : NULL;
    break;
  }
  case AST_STRUCT_DECLARATION: {
    const StructDeclaration *decl = (const StructDeclaration *)node->data;
    composed = decl ? decl->composed_name : NULL;
    break;
  }
  case AST_TYPE_DECLARATION: {
    const TypeDeclaration *decl = (const TypeDeclaration *)node->data;
    composed = decl ? decl->composed_name : NULL;
    break;
  }
  case AST_VAR_DECLARATION: {
    const VarDeclaration *decl = (const VarDeclaration *)node->data;
    composed = decl ? decl->composed_name : NULL;
    break;
  }
  default:
    break;
  }

  int ok = 1;
  if (composed) {
    type_checker_set_error_at_location(
        checker, node->location,
        "'ident(...)' composes a name for generated code; it needs a "
        "'comptime for' to generate it");
    ok = 0;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!type_checker_check_composed_names(checker, node->children[i])) {
      ok = 0;
    }
  }
  return ok;
}

const char *type_checker_expansion_note(TypeChecker *checker,
                                        const ASTNode *block,
                                        SourceSpan *out_origin) {
  if (!checker) {
    return NULL;
  }
  const ComptimeExpansion *entry =
      expansion_table_get(checker->expansions, block);
  if (!entry) {
    return NULL;
  }
  if (out_origin) {
    *out_origin = entry->origin;
  }
  return entry->note;
}

int type_checker_declare_expansion_binding(TypeChecker *checker,
                                           const ASTNode *block) {
  if (!checker) {
    return 1;
  }
  const ComptimeExpansion *entry =
      expansion_table_get(checker->expansions, block);
  if (!entry) {
    return 1;
  }

  Symbol *symbol =
      binding_symbol(checker, entry->binding_name, entry->binding_value,
                     entry->binding_type, entry->origin);
  if (!symbol) {
    type_checker_set_error_at_location(
        checker, block->location,
        "Out of memory binding '%s' for this 'comptime for' iteration",
        entry->binding_name);
    return 0;
  }

  if (!symbol_table_declare(checker->symbol_table, symbol)) {
    symbol_destroy(symbol);
    type_checker_set_error_at_location(
        checker, block->location,
        "'%s' is already declared in this 'comptime for' body",
        entry->binding_name);
    return 0;
  }
  return 1;
}

static int expand_one_round(TypeChecker *checker, ASTNode *block,
                            int module_scope, int one_directive) {
  if (!checker || !block || block->type != AST_PROGRAM) {
    return 1;
  }
  Program *program = (Program *)block->data;
  if (!program) {
    return 1;
  }

  size_t directive_count = 0;
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *child = program->declarations[i];
    if (child && child->type == AST_COMPTIME_FOR) {
      directive_count++;
    }
  }
  if (directive_count == 0) {
    return 1;
  }

  if (!checker->expansions) {
    checker->expansions = calloc(1, sizeof(ComptimeExpansionTable));
    if (!checker->expansions) {
      type_checker_set_error_at_location(
          checker, block->location, "Out of memory expanding 'comptime for'");
      return 0;
    }
  }

  ASTNode **expanded = NULL;
  size_t expanded_count = 0;
  size_t expanded_capacity = 0;
  size_t retired = 0;
  int ok = 1;

  for (size_t i = 0; i < program->declaration_count && ok; i++) {
    ASTNode *child = program->declarations[i];
    ASTNode *single[1] = {child};
    ASTNode **batch = single;
    ASTNode **owned = NULL;
    size_t incoming = 1;

    ComptimeDeclScope outer = {0, 0};

    if (child && child->type == AST_COMPTIME_FOR &&
        !(one_directive && retired > 0)) {
      retired++;
      type_checker_enter_expansion_decl(checker, child, &outer);

      ComptimeForStatement *directive = (ComptimeForStatement *)child->data;
      ComptimeSource source;
      memset(&source, 0, sizeof(source));
      size_t errors_before = checker->error_reporter
                                 ? (size_t)checker->error_reporter->count
                                 : 0;
      if (!directive ||
          !(directive->range_end
                ? resolve_range_sequence(checker, directive, &source)
                : resolve_sequence(checker, directive->sequence, &source))) {
        type_checker_leave_expansion_decl(checker, &outer);
        if (directive && module_scope && checker->defer_unresolved_comptime) {
          if (checker->error_reporter) {
            checker->error_reporter->count = errors_before;
          }
          checker->has_error = 0;
          continue;
        }
        ok = 0;
        break;
      }
      if (!directive->body || !directive->binding_name) {
        type_checker_set_error_at_location(checker, child->location,
                                           "'comptime for' has no body");
        ok = 0;
        type_checker_leave_expansion_decl(checker, &outer);
        break;
      }

      size_t iterations = source.count;
      Program *body = (Program *)directive->body->data;
      size_t per_iteration =
          module_scope ? (body ? body->declaration_count : 0) : 1;
      incoming = iterations * per_iteration;

      if (incoming > 0) {
        owned = calloc(incoming, sizeof(ASTNode *));
        if (!owned) {
          type_checker_set_error_at_location(
              checker, child->location,
              "Out of memory expanding 'comptime for'");
          ok = 0;
          type_checker_leave_expansion_decl(checker, &outer);
          break;
        }
        size_t produced = 0;
        for (size_t f = 0; f < iterations && ok; f++) {
          if (module_scope) {
            ok = expand_declaration_iteration(checker, directive, &source, f,
                                              owned, &produced);
          } else {
            owned[produced] = expand_iteration(checker, directive, &source, f);
            ok = owned[produced] != NULL;
            produced += ok ? 1 : 0;
          }
        }
        if (ok && module_scope) {
          ok = check_generated_names(checker, directive, owned, produced);
        }
        size_t generated = 0;
        for (size_t k = 0; k < produced; k++) {
          generated += ast_node_count(owned[k]);
        }
        incoming = produced;
        batch = owned;
        expansion_record_cost(checker->expansions, directive->keyword_location,
                              source.label, iterations, generated);
      } else {
        expansion_record_cost(checker->expansions, directive->keyword_location,
                              source.label, iterations, 0);
      }
    }
    type_checker_leave_expansion_decl(checker, &outer);

    if (ok && expanded_count + incoming > expanded_capacity) {
      size_t next = expanded_capacity ? expanded_capacity * 2 : 8;
      while (next < expanded_count + incoming) {
        next *= 2;
      }
      ASTNode **grown = realloc(expanded, next * sizeof(ASTNode *));
      if (!grown) {
        type_checker_set_error_at_location(
          checker, block->location,
          "Out of memory expanding 'comptime for'");
        ok = 0;
      } else {
        expanded = grown;
        expanded_capacity = next;
      }
    }
    if (ok) {
      for (size_t k = 0; k < incoming; k++) {
        expanded[expanded_count++] = batch[k];
      }
    }
    free(owned);
  }

  ASTNode **children = NULL;
  if (ok) {
    children = malloc(expanded_count ? expanded_count * sizeof(ASTNode *)
                                     : sizeof(ASTNode *));
    if (!children) {
      type_checker_set_error_at_location(
          checker, block->location, "Out of memory expanding 'comptime for'");
      ok = 0;
    }
  }
  if (!ok) {
    free(expanded);
    return 0;
  }

  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *child = program->declarations[i];
    int carried = 0;
    if (!child || child->type != AST_COMPTIME_FOR) {
      continue;
    }
    for (size_t k = 0; k < expanded_count && !carried; k++) {
      if (expanded[k] == child) {
        carried = 1;
      }
    }
    if (!carried) {
      ast_destroy_node(child);
    }
  }

  free(program->declarations);
  free(block->children);
  program->declarations = expanded;
  program->declaration_count = expanded_count;
  memcpy(children, expanded, expanded_count * sizeof(ASTNode *));
  block->children = children;
  block->child_count = expanded_count;
  block->child_capacity = 0;
  block->child_capacity_owner = NULL;
  return 1;
}

static const ComptimeExpansionOrigin *
origin_create(const ComptimeForStatement *directive,
              const ComptimeExpansionOrigin *parent, size_t index) {
  ComptimeExpansionOrigin *origin = calloc(1, sizeof(*origin));
  if (origin) {
    origin->binding_name = string_intern(directive->binding_name);
    origin->sequence = ast_clone_node(directive->sequence);
    origin->range_end = ast_clone_node(directive->range_end);
    origin->range_inclusive = directive->range_inclusive;
    origin->index = index;
    origin->keyword_location = directive->keyword_location;
    origin->parent = parent;
  }
  return origin;
}

int type_checker_expand_directive_now(TypeChecker *checker, ASTNode *directive,
                                      ASTNode ***out, size_t *out_count) {
  ASTNode *probe = ast_clone_node(directive);
  ComptimeForStatement *statement =
      probe ? (ComptimeForStatement *)probe->data : NULL;
  ComptimeDeclScope outer = {0, 0};
  ComptimeSource source;
  size_t errors_before =
      checker->error_reporter ? (size_t)checker->error_reporter->count : 0;
  ASTNode **made = NULL;
  size_t produced = 0;
  int ok = 0;
  *out = NULL;
  *out_count = 0;
  if (!statement || !statement->body) {
    return 0;
  }
  if (!checker->expansions) {
    checker->expansions = calloc(1, sizeof(ComptimeExpansionTable));
    if (!checker->expansions) {
      return 0;
    }
  }
  type_checker_enter_expansion_decl(checker, directive, &outer);
  if (resolve_directive_sequence(checker, statement, &source)) {
    Program *body = (Program *)statement->body->data;
    size_t per_iteration = body ? body->declaration_count : 0;
    made = calloc(source.count * per_iteration + 1, sizeof(ASTNode *));
    ok = made != NULL;
    for (size_t f = 0; ok && f < source.count; f++) {
      size_t first = produced;
      ok = expand_declaration_iteration(checker, statement, &source, f, made,
                                        &produced);
      if (ok) {
        const ComptimeExpansionOrigin *origin =
            origin_create(statement, directive->expansion_origin, f);
        ok = origin != NULL;
        for (size_t k = first; ok && k < produced; k++) {
          made[k]->expansion_origin = origin;
        }
      }
    }
    if (ok) {
      ok = check_generated_names(checker, statement, made, produced);
    }
  }
  type_checker_leave_expansion_decl(checker, &outer);
  if (!ok) {
    if (checker->error_reporter) {
      checker->error_reporter->count = errors_before;
    }
    checker->has_error = 0;
    free(made);
    return 0;
  }
  *out = made;
  *out_count = produced;
  return 1;
}

int type_checker_expand_comptime_block(TypeChecker *checker, ASTNode *block,
                                       int module_scope) {
  if (!module_scope) {
    return expand_one_round(checker, block, 0, 0);
  }

  for (;;) {
    Program *program;
    size_t remaining = 0;
    if (!expand_one_round(checker, block, 1, 1)) {
      return 0;
    }
    checker->expansion_rounds++;
    if (!type_checker_register_generated_types(checker, block)) {
      return 0;
    }
    program = (Program *)block->data;
    for (size_t i = 0; program && i < program->declaration_count; i++) {
      ASTNode *child = program->declarations[i];
      if (child && child->type == AST_COMPTIME_FOR) {
        remaining++;
      }
    }
    if (remaining == 0) {
      return 1;
    }
    if (checker->expansion_rounds > COMPTIME_MAX_ROUNDS) {
      type_checker_set_error_at_location(
          checker, block->location,
          "'comptime for' expansion did not settle after %d rounds; a "
          "directive that generates a directive has to reach an end",
          COMPTIME_MAX_ROUNDS);
      return 0;
    }
  }
}
