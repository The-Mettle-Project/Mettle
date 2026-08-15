/* Compile-time loop expansion.
 *
 * `comptime for f in typeof(T).fields { ... }` is not a loop the backend ever
 * sees. It is expanded here, during const eval, into one copy of the body per
 * field, each with `f` bound to a different compile-time Field. It has to run
 * before the body is type checked because that is the whole point: `f.type` is
 * a different type in every copy, so one copy can check clean while the next
 * fails, and a single shared check of the body could not tell you which.
 *
 * Every expansion registers a note frame here rather than in a later pass. The
 * attribution is only correct while the expansion that produced the code is
 * the one in progress, so it is captured then -- see
 * error_reporter_push_note_frame.
 *
 * The expanded blocks replace the `comptime for` node in its parent, so nothing
 * after this pass sees an AST_COMPTIME_FOR: lowering, the borrow checker, and
 * the contract checkers all walk ordinary blocks, and hold generated code to
 * exactly the standard hand-written code is held to. */
#include "type_checker_internal.h"

typedef struct {
  ASTNode *block;
  char *note;
  SourceSpan origin;
  const char *binding_name;
  ComptimeValue binding_value;
} ComptimeExpansion;

/* What one `comptime for` site cost, so expansion can be held to a budget
 * rather than quietly becoming the reason builds got slow. Metaprogramming is
 * the most reliable way in the history of programming languages to destroy a
 * build time, and in every case the collapse was gradual and unattributed --
 * so the ledger exists from the first metaprogram, not after the regression. */
typedef struct {
  SourceLocation location;
  const char *type_name;
  size_t iterations;
  size_t nodes;
} ComptimeSiteCost;

/* Open-addressed map from an expanded block to its expansion, so looking one
 * up while checking a block stays O(1) no matter how many were generated. */
struct ComptimeExpansionTable {
  ComptimeExpansion *slots;
  size_t capacity;
  size_t count;
  ComptimeSiteCost *costs;
  size_t cost_count;
  size_t cost_capacity;
};

/* Nodes an expansion added, counted on the clone rather than estimated from
 * the source, so the ledger reports what was actually generated. */
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
      return 0; /* a block is expanded once */
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

/* The ledger. Printed by --report-expansion, and the same numbers a build
 * budget is checked against, so what you are shown is what you are held to. */
void type_checker_report_expansion(const TypeChecker *checker, FILE *out) {
  if (!out) {
    return;
  }
  size_t sites = type_checker_expansion_site_count(checker);
  if (sites == 0) {
    /* An absence worth stating: a program that expanded nothing paid nothing,
     * and saying so is the difference between a cost you avoided and a cost
     * you merely hope you avoided. */
    fprintf(out, "comptime expansion: no sites; nothing generated\n");
    return;
  }

  fprintf(out, "comptime expansion: %zu site%s, %zu nodes generated\n", sites,
          sites == 1 ? "" : "s", type_checker_expansion_total_nodes(checker));
  for (size_t i = 0; i < checker->expansions->cost_count; i++) {
    const ComptimeSiteCost *cost = &checker->expansions->costs[i];
    fprintf(out, "  %s:%zu:%zu  %s  %zu iteration%s, %zu nodes\n",
            cost->location.filename ? cost->location.filename : "<input>",
            cost->location.line, cost->location.column,
            cost->type_name ? cost->type_name : "<anonymous>",
            cost->iterations, cost->iterations == 1 ? "" : "s", cost->nodes);
  }
}

/* A budget is a contract, so exceeding it fails the build and names the site
 * that cost the most -- the same shape as @simd! and @noalloc refusing to
 * under-deliver quietly. */
int type_checker_check_expansion_budget(TypeChecker *checker, size_t budget) {
  size_t total = type_checker_expansion_total_nodes(checker);
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

/* Resolve the `comptime for` sequence to the type whose fields it names. The
 * only sequence that exists today is `<type>.fields`; anything else is refused
 * by name, so the message names what is available rather than what failed. */
static Type *resolve_field_sequence(TypeChecker *checker, ASTNode *sequence) {
  if (!sequence || sequence->type != AST_MEMBER_ACCESS) {
    type_checker_set_error_at_location(
        checker, sequence ? sequence->location : (SourceLocation){0, 0, NULL},
        "'comptime for' iterates a compile-time sequence; the only one is "
        "'<type>.fields'");
    return NULL;
  }

  MemberAccess *member = (MemberAccess *)sequence->data;
  if (!member || !member->member || strcmp(member->member, "fields") != 0) {
    type_checker_set_error_at_location(
        checker, sequence->location,
        "'comptime for' cannot iterate '.%s'; the only compile-time "
        "sequence is '.fields'",
        member && member->member ? member->member : "<unknown>");
    return NULL;
  }

  ComptimeValue owner = comptime_none();
  if (!type_checker_eval_comptime(checker, member->object, &owner) ||
      owner.kind != COMPTIME_TYPE_REF) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "'.fields' needs a compile-time type on its left, for example "
        "'typeof(T).fields'");
    return NULL;
  }

  Type *referred =
      type_checker_type_from_index(checker, owner.as.type_ref.type_index);
  if (!referred) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "'.fields' refers to a type that is not in the type table");
    return NULL;
  }
  if (referred->kind != TYPE_STRUCT && referred->kind != TYPE_STRING) {
    type_checker_set_error_at_location(
        checker, member->object->location,
        "type '%s' has no fields to iterate",
        referred->name ? referred->name : "<anonymous>");
    return NULL;
  }
  return referred;
}

/* Build one iteration: a clone of the body registered with the binding it runs
 * under and the note that attributes it back to the `comptime for`. */
static ASTNode *expand_iteration(TypeChecker *checker,
                                 ComptimeForStatement *directive, Type *owner,
                                 uint32_t owner_index, size_t field_index) {
  TypeField field;
  if (!type_field_at(owner, field_index, &field)) {
    type_checker_set_error_at_location(
        checker, directive->keyword_location,
        "could not read field %zu of '%s' from the type table", field_index,
        owner->name ? owner->name : "<anonymous>");
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

  char note[256];
  snprintf(note, sizeof(note),
           "expanded from comptime-for iteration %zu (field `%s`)",
           field_index + 1, field.name ? field.name : "<anonymous>");

  ComptimeExpansion entry;
  entry.block = clone;
  entry.note = mettle_strdup(note);
  entry.origin = source_span_from_location(directive->keyword_location,
                                           strlen("comptime"));
  entry.binding_name = string_intern(directive->binding_name);
  entry.binding_value = comptime_field_ref(owner_index, (uint32_t)field_index);

  if (!entry.note || !entry.binding_name ||
      !expansion_table_put(checker->expansions, entry)) {
    free(entry.note);
    ast_destroy_node(clone);
    type_checker_set_error_at_location(
        checker, directive->keyword_location,
        "Out of memory recording 'comptime for' iteration %zu",
        field_index + 1);
    return NULL;
  }
  return clone;
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

  Symbol *symbol = symbol_create((char *)entry->binding_name, SYMBOL_CONSTANT,
                                 checker->builtin_field);
  if (!symbol) {
    type_checker_set_error_at_location(
        checker, block->location,
        "Out of memory binding '%s' for this 'comptime for' iteration",
        entry->binding_name);
    return 0;
  }
  symbol->comptime_value = entry->binding_value;
  symbol->is_initialized = 1;
  symbol->is_immutable = 1;
  symbol->decl_line = entry->origin.line;
  symbol->decl_column = entry->origin.column;
  symbol->decl_file = entry->origin.filename;

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

int type_checker_expand_comptime_block(TypeChecker *checker, ASTNode *block) {
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

  /* Build the replacement list first and swap it in only once every directive
   * has expanded. A directive that fails halfway leaves the block exactly as
   * it was, so the AST stays consistent for the rest of the walk and the
   * remaining statements still get checked and reported on. */
  ASTNode **expanded = NULL;
  size_t expanded_count = 0;
  size_t expanded_capacity = 0;
  int ok = 1;

  for (size_t i = 0; i < program->declaration_count && ok; i++) {
    ASTNode *child = program->declarations[i];
    ASTNode *single[1] = {child};
    ASTNode **batch = single;
    ASTNode **owned = NULL;
    size_t incoming = 1;

    if (child && child->type == AST_COMPTIME_FOR) {
      ComptimeForStatement *directive = (ComptimeForStatement *)child->data;
      Type *owner =
          directive ? resolve_field_sequence(checker, directive->sequence)
                    : NULL;
      if (!owner) {
        ok = 0;
        break;
      }
      if (!directive->body || !directive->binding_name) {
        type_checker_set_error_at_location(checker, child->location,
                                           "'comptime for' has no body");
        ok = 0;
        break;
      }

      uint32_t owner_index = type_checker_intern_type(checker, owner);
      /* Zero fields expands to nothing, which is an answer, not an error. */
      incoming = type_field_count(owner);
      if (incoming > 0) {
        owned = calloc(incoming, sizeof(ASTNode *));
        if (!owned) {
          type_checker_set_error_at_location(
              checker, child->location,
              "Out of memory expanding 'comptime for'");
          ok = 0;
          break;
        }
        size_t generated = 0;
        for (size_t f = 0; f < incoming; f++) {
          owned[f] =
              expand_iteration(checker, directive, owner, owner_index, f);
          if (!owned[f]) {
            ok = 0;
            break;
          }
          generated += ast_node_count(owned[f]);
        }
        batch = owned;
        if (ok) {
          expansion_record_cost(checker->expansions,
                                directive->keyword_location, owner->name,
                                incoming, generated);
        }
      } else {
        expansion_record_cost(checker->expansions, directive->keyword_location,
                              owner->name, 0, 0);
      }
    }

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
    /* Clones already handed to the expansion table stay alive: the table keys
     * on their addresses, and a freed address could be handed back out and
     * match a block that was never expanded. */
    free(expanded);
    return 0;
  }

  /* Committed. The `comptime for` nodes are unreachable now, and their bodies
   * were cloned, so retiring them cannot touch an expansion. */
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *child = program->declarations[i];
    if (child && child->type == AST_COMPTIME_FOR) {
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
  return 1;
}
