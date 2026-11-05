typedef struct {
  const IRModuleSymbol *symbol;
  const IRFunction *scope;
  const IROperand *root;
  int rejected;
} SafetyPlainArray;

typedef struct {
  IRProgram *program;
  SafetyPlainArray *arrays;
  size_t count;
  unsigned taint_epoch;
} SafetyPlainStorage;

typedef struct {
  const char *name;
  int kind;
  int first_def;
  int def_count;
  SafetyPlainArray *array;
  int root_known;
  SafetyPlainArray *root;
} SafetyPlainName;

typedef struct {
  const IRFunction *fn;
  SafetyPlainName *names;
  size_t mask;
  int *next_def;
  unsigned char *tainted;
  unsigned tainted_epoch;
} SafetyPlainIndex;

static SafetyPlainIndex *g_safety_plain_index = NULL;
static size_t g_safety_plain_index_size = 0;

static void safety_plain_index_drop(void) {
  for (size_t w = 0; w < g_safety_plain_index_size; w++) {
    SafetyPlainIndex *index = &g_safety_plain_index[w];
    for (size_t n = 0; index->names && n <= index->mask; n++) {
      free((char *)index->names[n].name);
    }
    free(index->names);
    free(index->next_def);
    free(index->tainted);
  }
  free(g_safety_plain_index);
  g_safety_plain_index = NULL;
  g_safety_plain_index_size = 0;
}

static SafetyPlainName *safety_plain_name_slot(const SafetyPlainIndex *index,
                                               int kind, const char *name) {
  size_t at = mettle_fnv1a_hash(name) & index->mask;
  while (index->names[at].name &&
         !(index->names[at].kind == kind && !strcmp(index->names[at].name, name))) {
    at = (at + 1) & index->mask;
  }
  return &index->names[at];
}

static const SafetyPlainIndex *safety_plain_index_of(SafetyPlainStorage *state,
                                                     const IRFunction *fn) {
  SafetyPlainIndex *index;
  size_t size = 16;
  size_t arrays_here = 0;
  if (!g_safety_plain_index) {
    size_t ways = 16;
    while (ways < (state->program->function_count + 1) * 2) ways *= 2;
    g_safety_plain_index = calloc(ways, sizeof(*g_safety_plain_index));
    if (!g_safety_plain_index) return NULL;
    g_safety_plain_index_size = ways;
  }
  {
    size_t at = (((uintptr_t)fn) >> 4) & (g_safety_plain_index_size - 1);
    while (g_safety_plain_index[at].fn && g_safety_plain_index[at].fn != fn) {
      at = (at + 1) & (g_safety_plain_index_size - 1);
    }
    index = &g_safety_plain_index[at];
  }
  if (index->fn == fn) return index;
  for (size_t a = 0; a < state->count; a++) arrays_here += state->arrays[a].scope == fn;
  while (size < (fn->instruction_count + fn->parameter_count + arrays_here + 1) * 2) size *= 2;
  index->names = calloc(size, sizeof(*index->names));
  index->next_def = malloc((fn->instruction_count + 1) * sizeof(*index->next_def));
  if (!index->names || !index->next_def) {
    free(index->names);
    free(index->next_def);
    memset(index, 0, sizeof(*index));
    return NULL;
  }
  index->mask = size - 1;
  for (size_t i = fn->instruction_count; i-- > 0;) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL || !ir_instruction_writes_destination(in) ||
        !in->dest.name || (in->dest.kind != IR_OPERAND_TEMP &&
                           in->dest.kind != IR_OPERAND_SYMBOL)) continue;
    SafetyPlainName *slot = safety_plain_name_slot(index, in->dest.kind, in->dest.name);
    if (!slot->name) {
      slot->name = mettle_strdup(in->dest.name);
      slot->kind = in->dest.kind;
      slot->first_def = -1;
    }
    index->next_def[i] = slot->first_def;
    slot->first_def = (int)i;
    slot->def_count++;
  }
  for (size_t a = 0; a < state->count; a++) {
    const IROperand *root = state->arrays[a].root;
    if (state->arrays[a].scope != fn || !root || !root->name) continue;
    SafetyPlainName *slot = safety_plain_name_slot(index, root->kind, root->name);
    if (!slot->name) {
      slot->name = mettle_strdup(root->name);
      slot->kind = root->kind;
      slot->first_def = -1;
    }
    if (!slot->array) slot->array = &state->arrays[a];
  }
  index->fn = fn;
  return index;
}

static const SafetyPlainName *safety_plain_lookup(SafetyPlainStorage *state,
                                                  const IRFunction *fn,
                                                  const IROperand *value,
                                                  const SafetyPlainIndex **out) {
  const SafetyPlainIndex *index = safety_plain_index_of(state, fn);
  const SafetyPlainName *slot;
  *out = index;
  if (!index || !value->name ||
      (value->kind != IR_OPERAND_TEMP && value->kind != IR_OPERAND_SYMBOL)) {
    return NULL;
  }
  slot = safety_plain_name_slot(index, value->kind, value->name);
  return slot->name ? slot : NULL;
}

static const IRInstruction *safety_plain_definition(SafetyPlainStorage *state,
                                                   const IRFunction *fn,
                                                   const IROperand *value) {
  const SafetyPlainIndex *index = NULL;
  const SafetyPlainName *slot = safety_plain_lookup(state, fn, value, &index);
  if (!slot || slot->def_count != 1) return NULL;
  return &fn->instructions[slot->first_def];
}

static SafetyPlainArray *safety_plain_root(SafetyPlainStorage *state,
    const IRFunction *fn, const IROperand *value, int depth);

static SafetyPlainArray *safety_plain_parameter_root(SafetyPlainStorage *state,
    const IRFunction *fn, const IROperand *value, size_t p, int depth) {
  const IRModuleSymbol *decl = ir_program_lookup_symbol(state->program, fn->name);
  if (decl && decl->is_exported) return NULL;
  for (size_t s = 0; s < state->program->module_symbol_count; s++) {
    const IRModuleSymbol *symbol = &state->program->module_symbols[s];
    if (symbol->init_symbol_ref && !strcmp(symbol->init_symbol_ref, fn->name))
      return NULL;
    for (size_t r = 0; r < symbol->init_reloc_count; r++)
      if (symbol->init_relocs[r].symbol &&
          !strcmp(symbol->init_relocs[r].symbol, fn->name)) return NULL;
  }
  SafetyPlainArray *result = NULL;
  for (size_t f = 0; f < state->program->function_count; f++) {
    const IRFunction *caller = state->program->functions[f];
    for (size_t i = 0; i < caller->instruction_count; i++) {
      const IRInstruction *call = &caller->instructions[i];
      if (call->op == IR_OP_ADDRESS_OF && call->lhs.name &&
          !strcmp(call->lhs.name, fn->name) &&
          (!call->dest.name || strncmp(call->dest.name, ".safe", 5))) return NULL;
      if (call->op != IR_OP_CALL || !call->text || strcmp(call->text, fn->name))
        continue;
      if (p >= call->argument_count) return NULL;
      SafetyPlainArray *argument = safety_plain_root(state, caller,
          &call->arguments[p], depth + 1);
      if (!argument || (result && result != argument)) return NULL;
      result = argument;
    }
  }
  (void)value;
  return result;
}

static SafetyPlainArray *safety_plain_root(SafetyPlainStorage *state,
    const IRFunction *fn, const IROperand *value, int depth) {
  const SafetyPlainIndex *index = NULL;
  const SafetyPlainName *known = NULL;
  if (depth > 32) return NULL;
  known = safety_plain_lookup(state, fn, value, &index);
  if (known && known->array) return known->array;
  if (value->kind == IR_OPERAND_SYMBOL && value->name) {
    for (size_t p = 0; p < fn->parameter_count; p++) {
      if (strcmp(fn->parameter_names[p], value->name)) continue;
      if (known && known->def_count > 0) return NULL;
      {
        SafetyPlainName *slot = index ? safety_plain_name_slot(index, value->kind,
                                                               value->name)
                                      : NULL;
        SafetyPlainArray *found;
        if (slot && slot->name && slot->root_known) return slot->root;
        found = safety_plain_parameter_root(state, fn, value, p, depth);
        if (slot && !slot->name) {
          slot->name = mettle_strdup(value->name);
          slot->kind = value->kind;
          slot->first_def = -1;
        }
        if (slot) {
          slot->root_known = 1;
          slot->root = found;
        }
        return found;
      }
      const IRModuleSymbol *decl = ir_program_lookup_symbol(state->program, fn->name);
    }
    if (!ir_function_find_declaration(fn, value->name, 1))
      for (size_t a = 0; a < state->count; a++)
        if (state->arrays[a].symbol &&
            !strcmp(value->name, state->arrays[a].symbol->name)) return &state->arrays[a];
  }
  const IRInstruction *in = safety_plain_definition(state, fn, value);
  if (!in) return NULL;
  if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL)
    return safety_plain_root(state, fn, &in->lhs, depth + 1);
  if (in->op == IR_OP_ASSIGN || in->op == IR_OP_CAST ||
      (in->op == IR_OP_BINARY && in->text && !strcmp(in->text, "+")))
    return safety_plain_root(state, fn, &in->lhs, depth + 1);
  return NULL;
}

static int safety_plain_reads_value_only(const IRInstruction *in) {
  if (in->op == IR_OP_BRANCH_ZERO || in->op == IR_OP_BRANCH_EQ) return 1;
  return in->op == IR_OP_BINARY && in->text &&
      (!strcmp(in->text, "==") || !strcmp(in->text, "!=") ||
       !strcmp(in->text, "<") || !strcmp(in->text, ">") ||
       !strcmp(in->text, "<=") || !strcmp(in->text, ">=") ||
       !strcmp(in->text, "&&") || !strcmp(in->text, "||"));
}

static int safety_plain_metadata_pairs(SafetyPlainStorage *state,
    const IRFunction *fn, SafetyPlainArray *array, const char *name,
    size_t arity) {
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_CALL && in->text && !strcmp(in->text, name) &&
        in->argument_count == arity && in->arguments &&
        safety_plain_root(state, fn, &in->arguments[0], 0) == array) return 1;
  }
  return 0;
}

static int safety_plain_literal_bytes(SafetyPlainStorage *state,
    const IRFunction *fn, const IROperand *value, int depth) {
  if (!value || depth > 32) return 0;
  if (value->kind == IR_OPERAND_STRING) return 1;
  const IRInstruction *in = safety_plain_definition(state, fn, value);
  if (!in) return 0;
  if (in->op == IR_OP_ASSIGN || in->op == IR_OP_CAST ||
      (in->op == IR_OP_BINARY && in->text && !strcmp(in->text, "+")))
    return safety_plain_literal_bytes(state, fn, &in->lhs, depth + 1);
  if (in->op != IR_OP_LOAD || in->rhs.kind != IR_OPERAND_INT ||
      in->rhs.int_value != 8 || in->lhs.kind != IR_OPERAND_SYMBOL) return 0;
  const IRInstruction *source = safety_plain_definition(state, fn, &in->lhs);
  if (source) return source->op == IR_OP_ASSIGN &&
      source->lhs.kind == IR_OPERAND_STRING;
  const IRModuleSymbol *symbol =
      ir_program_lookup_symbol(state->program, in->lhs.name);
  if (!symbol || symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
      symbol->is_exported || symbol->is_volatile || !symbol->init_string ||
      symbol->init_reloc_count) return 0;
  for (size_t f = 0; f < state->program->function_count; f++) {
    const IRFunction *other = state->program->functions[f];
    for (size_t i = 0; i < other->instruction_count; i++) {
      const IRInstruction *write = &other->instructions[i];
      if (write->op != IR_OP_DECLARE_LOCAL &&
          ir_instruction_writes_destination(write) &&
          write->dest.kind == IR_OPERAND_SYMBOL && write->dest.name &&
          !strcmp(write->dest.name, in->lhs.name)) return 0;
    }
  }
  return 1;
}

static int safety_plain_copy_source_is_clean(SafetyPlainStorage *state,
    const IRFunction *fn, const IROperand *value) {
  if (safety_plain_literal_bytes(state, fn, value, 0)) return 1;
  SafetyPlainArray *array = safety_plain_root(state, fn, value, 0);
  return array && !array->rejected;
}

static int safety_plain_allowed_use(SafetyPlainStorage *state,
    SafetyPlainArray *array, const IRFunction *fn,
    const IRInstruction *in, int position) {
  if (safety_plain_reads_value_only(in)) return 1;
  if (in->op == IR_OP_ADDRESS_OF) return position == 1 &&
      safety_plain_definition(state, fn, &in->dest) == in;
  if (in->op == IR_OP_SAFETY_CHECK) return position == 3;
  if (in->op == IR_OP_LOAD) return position == 1;
  if (in->op == IR_OP_STORE) return position == 0 &&
      in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 0 &&
      in->rhs.int_value <= 8;
  if ((in->op == IR_OP_ASSIGN || in->op == IR_OP_CAST ||
       (in->op == IR_OP_BINARY && in->text && !strcmp(in->text, "+"))) &&
      position == 1) return safety_plain_definition(state, fn, &in->dest) == in;
  if (in->op != IR_OP_CALL || !in->text || position < 3) return 0;
  IRFunction *callee = ir_program_find_function(state->program, in->text);
  if (callee && (size_t)(position - 3) < callee->parameter_count) {
    IROperand parameter = {0};
    parameter.kind = IR_OPERAND_SYMBOL;
    parameter.name = callee->parameter_names[position - 3];
    return safety_plain_root(state, callee, &parameter, 0) == array;
  }
  if (!strcmp(in->text, "mettle_safety_buffer_check") ||
      !strcmp(in->text, "mettle_safety_value_clear")) return 1;
  if (!strcmp(in->text, "mettle_safety_value_copy")) return 1;
  if (!strcmp(in->text, "memcpy") || !strcmp(in->text, "memmove")) {
    return position != 3 ||
        safety_plain_metadata_pairs(state, fn, array, "mettle_safety_value_copy", 3);
  }
  if (!strcmp(in->text, "memset")) {
    return position != 3 ||
        safety_plain_metadata_pairs(state, fn, array, "mettle_safety_value_clear", 2);
  }
  if (!strcmp(in->text, "mettle_safety_call_param_value")) return 1;
  if (position != 3) return 0;
  return !strcmp(in->text, "mettle_safety_identity") ||
         !strcmp(in->text, "mettle_safety_value_load") ||
         !strcmp(in->text, "mettle_safety_value_store") ||
         !strcmp(in->text, "mettle_safety_register") ||
         !strcmp(in->text, "mettle_safety_register_static") ||
         !strcmp(in->text, "mettle_safety_unregister") ||
         !strcmp(in->text, "mettle_safety_free_identity") ||
         !strcmp(in->text, "free") ||
         !strcmp(in->text, "mettle_heap_free") ||
         !strcmp(in->text, "mettle_safety_check_identity") ||
         !strcmp(in->text, "mettle_safety_span_identity");
}

enum { SAFETY_TAINT_CLEAN, SAFETY_TAINT_SET, SAFETY_TAINT_EDGE };

typedef struct {
  unsigned char *tainted;
  int *head;
  int *next;
  size_t *to;
  size_t edges;
  size_t *stack;
  size_t depth;
} SafetyTaintGraph;

static int safety_plain_operand_taint(const SafetyPlainIndex *index,
    const IROperand *value, size_t *slot) {
  if (value->kind == IR_OPERAND_INT)
    return value->int_value != 0 ? SAFETY_TAINT_SET : SAFETY_TAINT_CLEAN;
  if (!value->name ||
      (value->kind != IR_OPERAND_TEMP && value->kind != IR_OPERAND_SYMBOL))
    return SAFETY_TAINT_SET;
  const SafetyPlainName *name = safety_plain_name_slot(index, value->kind, value->name);
  if (!name->name || name->first_def < 0) return SAFETY_TAINT_SET;
  *slot = (size_t)(name - index->names);
  return SAFETY_TAINT_EDGE;
}

static void safety_taint_mark(SafetyTaintGraph *graph, size_t slot) {
  if (graph->tainted[slot]) return;
  graph->tainted[slot] = 1;
  graph->stack[graph->depth++] = slot;
}

static void safety_taint_operand(SafetyTaintGraph *graph,
    const SafetyPlainIndex *index, const IROperand *value, size_t node) {
  size_t from = 0;
  int kind = safety_plain_operand_taint(index, value, &from);
  if (kind == SAFETY_TAINT_SET) {
    safety_taint_mark(graph, node);
  } else if (kind == SAFETY_TAINT_EDGE) {
    graph->to[graph->edges] = node;
    graph->next[graph->edges] = graph->head[from];
    graph->head[from] = (int)graph->edges++;
  }
}

static void safety_taint_definition(SafetyPlainStorage *state, const IRFunction *fn,
    SafetyTaintGraph *graph, const SafetyPlainIndex *index,
    const IRInstruction *in, size_t node) {
  if (in->op == IR_OP_ASSIGN) {
    safety_taint_operand(graph, index, &in->lhs, node);
  } else if (in->op == IR_OP_CALL && in->text &&
             !strcmp(in->text, "mettle_safety_value_load") &&
             in->argument_count == 3) {
    SafetyPlainArray *array = safety_plain_root(state, fn, &in->arguments[0], 0);
    if (!array || array->rejected) safety_taint_mark(graph, node);
  } else if (in->op == IR_OP_CALL && in->text &&
             (!strcmp(in->text, "mettle_safety_merge_identity") ||
              !strcmp(in->text, "mettle_safety_subtract_identity")) &&
             in->argument_count == 2) {
    safety_taint_operand(graph, index, &in->arguments[0], node);
    safety_taint_operand(graph, index, &in->arguments[1], node);
  } else {
    safety_taint_mark(graph, node);
  }
}

static int safety_plain_solve_taint(SafetyPlainStorage *state, const IRFunction *fn,
    SafetyPlainIndex *index) {
  size_t slots = index->mask + 1;
  size_t edge_room = fn->instruction_count * 2 + 1;
  SafetyTaintGraph graph = {0};
  if (!index->tainted) index->tainted = malloc(slots);
  graph.tainted = index->tainted;
  graph.head = malloc(slots * sizeof(*graph.head));
  graph.next = malloc(edge_room * sizeof(*graph.next));
  graph.to = malloc(edge_room * sizeof(*graph.to));
  graph.stack = malloc(slots * sizeof(*graph.stack));
  int ok = graph.tainted && graph.head && graph.next && graph.to && graph.stack;
  if (ok) {
    memset(graph.tainted, 0, slots);
    for (size_t s = 0; s < slots; s++) graph.head[s] = -1;
    for (size_t s = 0; s < slots; s++) {
      const SafetyPlainName *name = &index->names[s];
      if (!name->name) continue;
      if (name->first_def < 0) safety_taint_mark(&graph, s);
      for (int at = name->first_def; at >= 0; at = index->next_def[at])
        safety_taint_definition(state, fn, &graph, index, &fn->instructions[at], s);
    }
    while (graph.depth) {
      size_t from = graph.stack[--graph.depth];
      for (int e = graph.head[from]; e >= 0; e = graph.next[e])
        safety_taint_mark(&graph, graph.to[e]);
    }
    index->tainted_epoch = state->taint_epoch + 1;
  }
  free(graph.head);
  free(graph.next);
  free(graph.to);
  free(graph.stack);
  return ok;
}

static int safety_plain_value_tainted(SafetyPlainStorage *state,
    const IRFunction *fn, const IROperand *value) {
  if (value->kind == IR_OPERAND_INT) return value->int_value != 0;
  SafetyPlainIndex *index = (SafetyPlainIndex *)safety_plain_index_of(state, fn);
  if (!index) return 1;
  if (index->tainted_epoch != state->taint_epoch + 1 &&
      !safety_plain_solve_taint(state, fn, index)) return 1;
  size_t slot = 0;
  int kind = safety_plain_operand_taint(index, value, &slot);
  return kind == SAFETY_TAINT_EDGE ? index->tainted[slot] : kind == SAFETY_TAINT_SET;
}

static int safety_plain_read_intrinsic(const IRInstruction *in) {
  return in->op == IR_OP_CALL && in->text &&
      (!strcmp(in->text, "mettle_safety_identity") ||
       !strcmp(in->text, "mettle_safety_value_load") ||
       !strcmp(in->text, "mettle_safety_merge_identity") ||
       !strcmp(in->text, "mettle_safety_subtract_identity"));
}

typedef struct {
  const char *name;
  int kind;
  int uses;
  int writes;
} SafetyPlainCount;

static SafetyPlainCount *safety_plain_count_slot(SafetyPlainCount *table, size_t mask,
                                                 const IROperand *value) {
  size_t at;
  if (!value->name ||
      (value->kind != IR_OPERAND_TEMP && value->kind != IR_OPERAND_SYMBOL)) {
    return NULL;
  }
  at = mettle_fnv1a_hash(value->name) & mask;
  while (table[at].name && !(table[at].kind == (int)value->kind &&
                             !strcmp(table[at].name, value->name))) {
    at = (at + 1) & mask;
  }
  if (!table[at].name) {
    table[at].name = mettle_strdup(value->name);
    table[at].kind = value->kind;
  }
  return &table[at];
}

static void safety_plain_count_reads(SafetyPlainCount *table, size_t mask,
                                     const IRInstruction *in, int delta) {
  const IROperand *fixed[3] = {&in->lhs, &in->rhs,
                               in->op == IR_OP_STORE ? &in->dest : NULL};
  for (int k = 0; k < 3; k++) {
    SafetyPlainCount *slot =
        fixed[k] ? safety_plain_count_slot(table, mask, fixed[k]) : NULL;
    if (slot) slot->uses += delta;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    SafetyPlainCount *slot = safety_plain_count_slot(table, mask, &in->arguments[a]);
    if (slot) slot->uses += delta;
  }
}

static int safety_plain_drop_dead_origins(IRFunction *fn) {
  size_t size = 16;
  while (size < (fn->instruction_count + 1) * 8) size *= 2;
  SafetyPlainCount *table = calloc(size, sizeof(*table));
  size_t mask = size - 1;
  int changed;
  if (!table) return 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_NOP) continue;
    safety_plain_count_reads(table, mask, in, 1);
    if (in->op != IR_OP_DECLARE_LOCAL && ir_instruction_writes_destination(in)) {
      SafetyPlainCount *slot = safety_plain_count_slot(table, mask, &in->dest);
      if (slot) slot->writes++;
    }
  }
  do {
    changed = 0;
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      SafetyPlainCount *slot;
      if (!in->dest.name || strncmp(in->dest.name, ".safe", 5)) continue;
      if (in->op != IR_OP_ASSIGN && in->op != IR_OP_DECLARE_LOCAL &&
          !safety_plain_read_intrinsic(in)) continue;
      slot = safety_plain_count_slot(table, mask, &in->dest);
      if (!slot || slot->uses != 0 ||
          (in->op == IR_OP_DECLARE_LOCAL && slot->writes != 0)) continue;
      safety_plain_count_reads(table, mask, in, -1);
      if (in->op != IR_OP_DECLARE_LOCAL) slot->writes--;
      ir_instruction_make_nop(in);
      changed = 1;
    }
  } while (changed);
  for (size_t n = 0; n < size; n++) {
    free((char *)table[n].name);
  }
  free(table);
  return 1;
}

static int safety_fold_identity_arithmetic(IRFunction *fn) {
  int folded = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    IRInstruction *in = &fn->instructions[i];
    if (in->op != IR_OP_CALL || !in->text || in->argument_count != 2 ||
        !in->arguments || in->dest.kind == IR_OPERAND_NONE) {
      continue;
    }
    int merge = strcmp(in->text, "mettle_safety_merge_identity") == 0;
    int subtract = strcmp(in->text, "mettle_safety_subtract_identity") == 0;
    if (!merge && !subtract) {
      continue;
    }
    const IROperand *left = &in->arguments[0];
    const IROperand *right = &in->arguments[1];
    int left_nothing = left->kind == IR_OPERAND_INT && left->int_value == 0;
    int right_nothing = right->kind == IR_OPERAND_INT && right->int_value == 0;
    int same = safety_operand_same(left, right);
    const IROperand *result = NULL;
    IROperand nothing = ir_operand_int(0);
    if (right_nothing || (merge && same)) {
      result = left;
    } else if (left_nothing) {
      result = right;
    } else if (subtract && same) {
      result = &nothing;
    }
    if (!result) {
      continue;
    }
    IROperand dest = ir_operand_none();
    IROperand source = ir_operand_none();
    if (!ir_operand_clone(&in->dest, &dest) ||
        !ir_operand_clone(result, &source)) {
      ir_operand_destroy(&dest);
      ir_operand_destroy(&source);
      return -1;
    }
    ir_instruction_make_nop(in);
    in->op = IR_OP_ASSIGN;
    in->dest = dest;
    in->lhs = source;
    folded++;
  }
  return folded;
}

static int safety_simplify_plain_storage(IRProgram *program) {
  SafetyPlainStorage state = {0};
  state.program = program;
  size_t capacity = program->module_symbol_count;
  for (size_t f = 0; f < program->function_count; f++)
    capacity += program->functions[f]->instruction_count;
  state.arrays = calloc(capacity, sizeof(*state.arrays));
  if (!state.arrays && capacity) return 0;
  for (size_t s = 0; s < program->module_symbol_count; s++) {
    const IRModuleSymbol *symbol = &program->module_symbols[s];
    if (symbol->kind == IR_MODSYM_VARIABLE && symbol->type &&
        symbol->type->kind == MTLC_TYPE_ARRAY && !symbol->is_exported &&
        !symbol->is_extern && !symbol->is_volatile && !symbol->init_reloc_count)
      state.arrays[state.count++].symbol = symbol;
  }
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *fn = program->functions[f];
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      int local = in->op == IR_OP_DECLARE_LOCAL && in->value_type &&
                  in->value_type->kind == MTLC_TYPE_ARRAY;
      int allocation = in->op == IR_OP_CALL && in->text &&
          (!strcmp(in->text, "malloc") || !strcmp(in->text, "calloc") ||
           !strcmp(in->text, "mettle_heap_alloc") ||
           !strcmp(in->text, "mettle_heap_calloc")) &&
          safety_plain_definition(&state, fn, &in->dest) == in;
      if (local || allocation) {
        state.arrays[state.count].scope = fn;
        state.arrays[state.count++].root = &in->dest;
      }
    }
  }
  safety_plain_index_drop();
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *fn = program->functions[f];
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (in->op == IR_OP_INLINE_ASM) {
        safety_trace("plain storage analysis keeps origins around assembly", in->location.line);
        safety_plain_index_drop(); free(state.arrays);
        return 1;
      }
      const IROperand *values[3] = {&in->dest, &in->lhs, &in->rhs};
      for (int v = 0; v < 3; v++) {
        if (v == 0 && (in->op == IR_OP_DECLARE_LOCAL ||
                       ir_instruction_writes_destination(in))) continue;
        SafetyPlainArray *array = safety_plain_root(&state, fn, values[v], 0);
        if (array && !safety_plain_allowed_use(&state, array, fn, in, v)) {
          if (safety_trace_enabled()) fprintf(stderr, "safety: plain storage escape in %s: %s operand %d\n", fn->name, ir_opcode_name(in->op), v);
          array->rejected = 1;
        }
      }
      for (size_t a = 0; a < in->argument_count; a++) {
        SafetyPlainArray *array = safety_plain_root(&state, fn, &in->arguments[a], 0);
        if (array && !safety_plain_allowed_use(&state, array, fn, in, (int)a + 3)) {
          if (safety_trace_enabled()) fprintf(stderr, "safety: plain storage argument escape in %s: %s argument %zu\n", fn->name, in->text ? in->text : "?", a);
          array->rejected = 1;
        }
      }
    }
  }
  for (size_t s = 0; s < program->module_symbol_count; s++) {
    const IRModuleSymbol *symbol = &program->module_symbols[s];
    for (size_t a = 0; a < state.count; a++) {
      if (!state.arrays[a].symbol) continue;
      const char *name = state.arrays[a].symbol->name;
      if (symbol->init_symbol_ref && !strcmp(symbol->init_symbol_ref, name))
        state.arrays[a].rejected = 1;
      for (size_t r = 0; r < symbol->init_reloc_count; r++)
        if (symbol->init_relocs[r].symbol && !strcmp(symbol->init_relocs[r].symbol, name))
          state.arrays[a].rejected = 1;
    }
  }
  int changed;
  do {
    changed = 0;
    for (size_t f = 0; f < program->function_count; f++) {
      const IRFunction *fn = program->functions[f];
      for (size_t i = 0; i < fn->instruction_count; i++) {
        const IRInstruction *in = &fn->instructions[i];
        if (in->op != IR_OP_CALL || !in->text ||
            strcmp(in->text, "mettle_safety_value_store") || in->argument_count != 4)
          continue;
        SafetyPlainArray *array = safety_plain_root(&state, fn, &in->arguments[0], 0);
        if (array && !array->rejected &&
            safety_plain_value_tainted(&state, fn, &in->arguments[2])) {
          array->rejected = 1;
          state.taint_epoch++;
          changed = 1;
        }
      }
      for (size_t i = 0; i < fn->instruction_count; i++) {
        const IRInstruction *in = &fn->instructions[i];
        if (in->op != IR_OP_CALL || !in->text || !in->arguments ||
            strcmp(in->text, "mettle_safety_value_copy") || in->argument_count != 3)
          continue;
        SafetyPlainArray *array = safety_plain_root(&state, fn, &in->arguments[0], 0);
        if (array && !array->rejected &&
            !safety_plain_copy_source_is_clean(&state, fn, &in->arguments[1])) {
          if (safety_trace_enabled())
            fprintf(stderr, "safety: plain storage copy source in %s can hold origins\n",
                    fn->name ? fn->name : "?");
          array->rejected = 1;
          state.taint_epoch++;
          changed = 1;
        }
      }
    }
  } while (changed);
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      if (in->op != IR_OP_CALL || !in->text || !in->argument_count) continue;
      int load = !strcmp(in->text, "mettle_safety_value_load");
      int store = !strcmp(in->text, "mettle_safety_value_store");
      if (!load && !store) continue;
      SafetyPlainArray *array = safety_plain_root(&state, fn, &in->arguments[0], 0);
      if (!array || array->rejected) continue;
      IROperand dest = ir_operand_none();
      if (load && !ir_operand_clone(&in->dest, &dest)) { safety_plain_index_drop(); free(state.arrays); return 0; }
      ir_instruction_make_nop(in);
      if (load) {
        in->op = IR_OP_ASSIGN;
        in->dest = dest;
        in->lhs = ir_operand_int(0);
      }
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      if (!safety_plain_read_intrinsic(in) ||
          safety_plain_value_tainted(&state, fn, &in->dest)) continue;
      IROperand dest = ir_operand_none();
      if (!ir_operand_clone(&in->dest, &dest)) { safety_plain_index_drop(); free(state.arrays); return 0; }
      ir_instruction_make_nop(in);
      in->op = IR_OP_ASSIGN;
      in->dest = dest;
      in->lhs = ir_operand_int(0);
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *in = &fn->instructions[i];
      for (size_t a = 0; a < in->argument_count + 2; a++) {
        IROperand *value = a == 0 ? &in->lhs : a == 1 ? &in->rhs :
                                                    &in->arguments[a - 2];
        if (value->name && !strncmp(value->name, ".safe_identity_", 15) &&
            !safety_plain_value_tainted(&state, fn, value)) {
          ir_operand_destroy(value);
          *value = ir_operand_int(0);
        }
      }
    }
    if (safety_fold_identity_arithmetic(fn) < 0) {
      safety_plain_index_drop(); free(state.arrays);
      return 0;
    }
    safety_plain_index_drop();

    if (!safety_plain_drop_dead_origins(fn)) {
      safety_plain_index_drop(); free(state.arrays);
      return 0;
    }
  }
  safety_plain_index_drop(); free(state.arrays);
  return 1;
}
