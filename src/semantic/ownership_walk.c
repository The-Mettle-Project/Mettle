#include "semantic/ownership_internal.h"
#include "semantic/type_checker_internal.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  OWN_CTX_READ,
  OWN_CTX_MOVE,
  OWN_CTX_ADDRESS,
  OWN_CTX_DISCARD,
  OWN_CTX_TEMP,
  OWN_CTX_CONSUMED
} OwnCtx;

typedef struct {
  const char *name;
  int binding;
} OwnName;

typedef struct {
  OwnName *names;
  size_t count;
  size_t capacity;
  size_t *marks;
  size_t mark_count;
  size_t mark_capacity;
} OwnNames;

static void own_stmt(OwnWalker *w, ASTNode *node);
static void own_expr(OwnWalker *w, ASTNode *node, OwnCtx ctx);
static void own_prescan(OwnWalker *w, OwnNames *names, ASTNode *node);

static void *own_reserve(void *items, size_t *capacity, size_t count,
                         size_t size) {
  if (count < *capacity) {
    return items;
  }
  size_t next = *capacity ? *capacity * 2 : 8;
  void *grown = realloc(items, next * size);
  if (!grown) {
    return NULL;
  }
  memset((char *)grown + *capacity * size, 0, (next - *capacity) * size);
  *capacity = next;
  return grown;
}

static char *own_dup(const char *text) {
  size_t length = text ? strlen(text) : 0;
  char *copy = malloc(length + 1);
  if (copy) {
    memcpy(copy, text ? text : "", length);
    copy[length] = '\0';
  }
  return copy;
}

static int own_reporting(const OwnWalker *w) {
  return w->mode == OWN_MODE_CHECK && w->silent == 0 && !w->state.dead;
}

static void own_error(OwnWalker *w, const char *code, SourceLocation at,
                      const char *help, const char *format, ...) {
  char message[768];
  va_list args;
  if (!own_reporting(w)) {
    return;
  }
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (help) {
    error_reporter_add_error_with_suggestion(w->reporter, ERROR_SEMANTIC, at,
                                             message, help);
  } else {
    error_reporter_add_error(w->reporter, ERROR_SEMANTIC, at, message);
  }
  error_reporter_set_last_code(w->reporter, code);
  w->errors++;
}

static void own_note_at(OwnWalker *w, SourceLocation at, const char *format,
                        ...) {
  char message[512];
  va_list args;
  if (!own_reporting(w) || at.line == 0) {
    return;
  }
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  error_reporter_add_note_of_span(w->reporter,
                                  source_span_from_location(at, 1), message);
}

static int own_new_binding(OwnWalker *w, const char *name, struct Type *type,
                           OwnBindKind kind, ASTNode *declaration,
                           ASTNode *block, SourceLocation at);

static int own_new_place(OwnWalker *w, int binding, int parent, int field,
                         struct Type *type) {
  OwnPlace *grown = own_reserve(w->places, &w->place_capacity,
                                w->place_count, sizeof(*w->places));
  char path[512];
  if (!grown) {
    return -1;
  }
  w->places = grown;
  OwnPlace *place = &w->places[w->place_count];
  place->binding = binding;
  place->parent = parent;
  place->field = field;
  place->type = type;
  place->bit = -1;
  place->wants_bit = 0;
  place->last_move = NULL;
  place->last_drop = NULL;
  if (parent < 0) {
    snprintf(path, sizeof(path), "%s", w->bindings[binding].name);
  } else {
    struct Type *owner = w->places[parent].type;
    while (owner && owner->refined_base) {
      owner = owner->refined_base;
    }
    snprintf(path, sizeof(path), "%s.%s", w->places[parent].path,
             owner && owner->field_names && field >= 0
                 ? owner->field_names[field]
                 : "?");
  }
  place->path = own_dup(path);
  return (int)w->place_count++;
}

static int own_new_binding(OwnWalker *w, const char *name, struct Type *type,
                           OwnBindKind kind, ASTNode *declaration,
                           ASTNode *block, SourceLocation at) {
  OwnBinding *grown = own_reserve(w->bindings, &w->binding_capacity,
                                  w->binding_count, sizeof(*w->bindings));
  if (!grown) {
    return -1;
  }
  w->bindings = grown;
  int id = (int)w->binding_count++;
  OwnBinding *b = &w->bindings[id];
  memset(b, 0, sizeof(*b));
  b->name = name;
  b->type = type;
  b->kind = kind;
  b->declaration = declaration;
  b->block = block;
  b->location = at;
  b->root = own_new_place(w, id, -1, -1, type);
  return id;
}

static int own_child_place(OwnWalker *w, int parent, int field) {
  for (size_t i = 0; i < w->place_count; i++) {
    if (w->places[i].parent == parent && w->places[i].field == field) {
      return (int)i;
    }
  }
  return -1;
}

static int own_is_ancestor(const OwnWalker *w, int ancestor, int place) {
  for (int p = place; p >= 0; p = w->places[p].parent) {
    if (p == ancestor) {
      return 1;
    }
  }
  return 0;
}

static void own_names_push_mark(OwnNames *names) {
  size_t *grown = own_reserve(names->marks, &names->mark_capacity,
                              names->mark_count, sizeof(*names->marks));
  if (grown) {
    names->marks = grown;
    names->marks[names->mark_count++] = names->count;
  }
}

static void own_names_pop_mark(OwnNames *names) {
  if (names->mark_count > 0) {
    names->count = names->marks[--names->mark_count];
  }
}

static void own_names_add(OwnNames *names, const char *name, int binding) {
  OwnName *grown = own_reserve(names->names, &names->capacity, names->count,
                               sizeof(*names->names));
  if (grown && name) {
    names->names = grown;
    names->names[names->count].name = name;
    names->names[names->count].binding = binding;
    names->count++;
  }
}

static int own_names_find(const OwnNames *names, const char *name) {
  for (size_t i = names->count; i > 0; i--) {
    if (names->names[i - 1].name &&
        strcmp(names->names[i - 1].name, name) == 0) {
      return names->names[i - 1].binding;
    }
  }
  return -1;
}

static struct Type *own_strip_type(struct Type *type) {
  while (type && type->refined_base) {
    type = type->refined_base;
  }
  return type;
}

static int own_node_owns(OwnWalker *w, ASTNode *node) {
  return node && own_type_owns(w->checker, node->resolved_type);
}

static int own_root_binding(OwnWalker *w, ASTNode *node) {
  int binding = -1;
  if (node && node->type == AST_IDENTIFIER &&
      own_map_get(&w->idents, node, &binding)) {
    return binding;
  }
  return -1;
}

static int own_member_chain(ASTNode *node, ASTNode **root, const char **fields,
                            int max_fields) {
  int depth = 0;
  ASTNode *cursor = node;
  const char *reversed[OWN_PATH_MAX];
  while (cursor && cursor->type == AST_MEMBER_ACCESS) {
    MemberAccess *access = (MemberAccess *)cursor->data;
    if (!access || depth >= OWN_PATH_MAX) {
      return -1;
    }
    reversed[depth++] = access->member;
    cursor = access->object;
  }
  if (!cursor || cursor->type != AST_IDENTIFIER || depth > max_fields) {
    return -1;
  }
  *root = cursor;
  for (int i = 0; i < depth; i++) {
    fields[i] = reversed[depth - 1 - i];
  }
  return depth;
}

static int own_resolve_place(OwnWalker *w, ASTNode *node, int *exact) {
  ASTNode *root = NULL;
  const char *fields[OWN_PATH_MAX];
  int depth = own_member_chain(node, &root, fields, OWN_PATH_MAX);
  int binding = -1;
  int place = -1;
  *exact = 0;
  if (depth < 0 || (binding = own_root_binding(w, root)) < 0) {
    return -1;
  }
  place = w->bindings[binding].root;
  for (int i = 0; i < depth; i++) {
    int index = -1;
    struct Type *field_type =
        own_field_type(w->places[place].type, fields[i], &index);
    int child = index >= 0 ? own_child_place(w, place, index) : -1;
    if (child < 0) {
      (void)field_type;
      return place;
    }
    place = child;
  }
  *exact = 1;
  return place;
}

static void own_prescan_member(OwnWalker *w, ASTNode *node) {
  ASTNode *root = NULL;
  const char *fields[OWN_PATH_MAX];
  int depth = own_member_chain(node, &root, fields, OWN_PATH_MAX);
  int binding = -1;
  int place = -1;
  if (depth <= 0 || !own_node_owns(w, node) ||
      (binding = own_root_binding(w, root)) < 0) {
    return;
  }
  place = w->bindings[binding].root;
  for (int i = 0; i < depth; i++) {
    int index = -1;
    struct Type *field_type =
        own_field_type(w->places[place].type, fields[i], &index);
    int child = -1;
    if (index < 0 || !own_type_owns(w->checker, field_type)) {
      return;
    }
    child = own_child_place(w, place, index);
    if (child < 0) {
      child = own_new_place(w, binding, place, index, field_type);
      if (child < 0) {
        return;
      }
    }
    place = child;
  }
}

static struct Type *own_variant_payload(struct Type *type,
                                        const char *variant) {
  type = own_strip_type(type);
  if (!type || type->kind != TYPE_TAGGED_ENUM || !variant) {
    return NULL;
  }
  for (size_t i = 0; i < type->tagged_variant_count; i++) {
    if (type->tagged_variant_names && type->tagged_variant_names[i] &&
        strcmp(type->tagged_variant_names[i], variant) == 0) {
      return type->tagged_variant_payloads
                 ? type->tagged_variant_payloads[i]
                 : NULL;
    }
  }
  return NULL;
}

static void own_prescan_list(OwnWalker *w, OwnNames *names, ASTNode **items,
                             size_t count) {
  for (size_t i = 0; i < count; i++) {
    own_prescan(w, names, items[i]);
  }
}

static void own_prescan_declare(OwnWalker *w, OwnNames *names,
                                ASTNode *node, ASTNode *block, size_t index) {
  VarDeclaration *decl = (VarDeclaration *)node->data;
  struct Type *type = NULL;
  int binding = -1;
  int string_local = 0;
  if (!decl) {
    return;
  }
  own_prescan(w, names, decl->initializer);
  if (decl->is_const) {
    own_names_add(names, decl->name, -1);
    return;
  }
  type = own_declared_type(w->checker, decl);
  if (block && own_is_string_type(type)) {
    int source = decl->initializer &&
                         decl->initializer->type == AST_IDENTIFIER &&
                         decl->initializer->data
                     ? own_names_find(
                           names,
                           ((Identifier *)decl->initializer->data)->name)
                     : -1;
    int handed = source >= 0 && w->bindings[source].is_string;
    string_local = own_string_local_is_tracked(w->checker, block, index, decl,
                                               handed);
  }
  if (own_type_owns(w->checker, type) || string_local) {
    binding = own_new_binding(w, decl->name, type, OWN_BIND_LOCAL, node,
                              block, node->location);
    w->bindings[binding].is_string = string_local;
    own_map_put(&w->declarations, node, binding);
  }
  own_names_add(names, decl->name, binding);
}

static void own_prescan_block(OwnWalker *w, OwnNames *names, ASTNode *node) {
  Program *block = (Program *)node->data;
  own_names_push_mark(names);
  for (size_t i = 0; block && i < block->declaration_count; i++) {
    ASTNode *statement = block->declarations[i];
    if (statement && statement->type == AST_VAR_DECLARATION) {
      own_prescan_declare(w, names, statement, node, i);
    } else {
      own_prescan(w, names, statement);
    }
  }
  own_names_pop_mark(names);
}

static void own_prescan_match(OwnWalker *w, OwnNames *names, ASTNode *node) {
  MatchStatement *match = (MatchStatement *)node->data;
  if (!match) {
    return;
  }
  own_prescan(w, names, match->expression);
  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int binding = -1;
    own_names_push_mark(names);
    if (arm->binding_name && match->expression) {
      struct Type *payload = own_variant_payload(
          match->expression->resolved_type, arm->variant_name);
      if (own_type_owns(w->checker, payload)) {
        binding = own_new_binding(w, arm->binding_name, payload,
                                  OWN_BIND_ARM, arm->body, arm->body,
                                  arm->body ? arm->body->location
                                            : node->location);
        own_map_put(&w->declarations, &match->arms[i], binding);
      }
      own_names_add(names, arm->binding_name, binding);
    }
    own_prescan(w, names, arm->body);
    own_names_pop_mark(names);
  }
}

static void own_prescan_for(OwnWalker *w, OwnNames *names, ASTNode *node) {
  ForStatement *loop = (ForStatement *)node->data;
  if (!loop) {
    return;
  }
  own_names_push_mark(names);
  if (loop->initializer && loop->initializer->type == AST_VAR_DECLARATION) {
    VarDeclaration *decl = (VarDeclaration *)loop->initializer->data;
    own_prescan(w, names, decl ? decl->initializer : NULL);
    own_names_add(names, decl ? decl->name : NULL, -1);
  } else {
    own_prescan(w, names, loop->initializer);
  }
  own_prescan(w, names, loop->condition);
  own_prescan(w, names, loop->increment);
  own_prescan(w, names, loop->body);
  own_names_pop_mark(names);
}

static void own_prescan_identifier(OwnWalker *w, OwnNames *names,
                                   ASTNode *node) {
  Identifier *id = (Identifier *)node->data;
  int binding = id && id->name ? own_names_find(names, id->name) : -1;
  if (binding >= 0) {
    own_map_put(&w->idents, node, binding);
  }
}

static void own_prescan_assignment(OwnWalker *w, OwnNames *names,
                                   ASTNode *node) {
  Assignment *assign = (Assignment *)node->data;
  int binding = -1;
  if (!assign) {
    return;
  }
  own_prescan_list(w, names, node->children, node->child_count);
  if (assign->target || assign->target_count > 0 || !assign->variable_name) {
    return;
  }
  binding = own_names_find(names, assign->variable_name);
  if (binding >= 0) {
    own_map_put(&w->assigns, node, binding);
  }
}

static void own_prescan_if(OwnWalker *w, OwnNames *names, ASTNode *node) {
  IfStatement *branch = (IfStatement *)node->data;
  if (!branch) {
    return;
  }
  own_prescan(w, names, branch->condition);
  own_prescan(w, names, branch->then_branch);
  for (size_t i = 0; i < branch->else_if_count; i++) {
    own_prescan(w, names, branch->else_ifs[i].condition);
    own_prescan(w, names, branch->else_ifs[i].body);
  }
  own_prescan(w, names, branch->else_branch);
}

static void own_prescan_call(OwnWalker *w, OwnNames *names, ASTNode *node) {
  CallExpression *call = (CallExpression *)node->data;
  if (!call) {
    return;
  }
  if (call->function_name && (strcmp(call->function_name, "sizeof") == 0 ||
                              strcmp(call->function_name, "typeof") == 0 ||
                              strcmp(call->function_name, "offsetof") == 0 ||
                              strcmp(call->function_name, "layoutof") == 0 ||
                              strcmp(call->function_name, "fieldof") == 0)) {
    return;
  }
  own_prescan(w, names, call->object);
  own_prescan_list(w, names, call->arguments, call->argument_count);
}

static void own_prescan_switch(OwnWalker *w, OwnNames *names, ASTNode *node) {
  SwitchStatement *sw = (SwitchStatement *)node->data;
  if (!sw) {
    return;
  }
  own_prescan(w, names, sw->expression);
  for (size_t i = 0; i < sw->case_count; i++) {
    CaseClause *clause =
        sw->cases[i] ? (CaseClause *)sw->cases[i]->data : NULL;
    if (clause) {
      own_prescan(w, names, clause->value);
      own_prescan(w, names, clause->value_high);
      own_prescan(w, names, clause->body);
    }
  }
}

static void own_prescan(OwnWalker *w, OwnNames *names, ASTNode *node) {
  if (!node) {
    return;
  }
  switch (node->type) {
  case AST_PROGRAM:
    own_prescan_block(w, names, node);
    return;
  case AST_VAR_DECLARATION:
    own_prescan_declare(w, names, node, NULL, 0);
    return;
  case AST_IDENTIFIER:
    own_prescan_identifier(w, names, node);
    return;
  case AST_MEMBER_ACCESS:
    own_prescan(w, names, ((MemberAccess *)node->data)->object);
    own_prescan_member(w, node);
    return;
  case AST_MATCH_STATEMENT:
    own_prescan_match(w, names, node);
    return;
  case AST_FOR_STATEMENT:
    own_prescan_for(w, names, node);
    return;
  case AST_IF_STATEMENT:
    own_prescan_if(w, names, node);
    return;
  case AST_FUNCTION_CALL:
    own_prescan_call(w, names, node);
    return;
  case AST_SWITCH_STATEMENT:
    own_prescan_switch(w, names, node);
    return;
  case AST_ASSIGNMENT:
    own_prescan_assignment(w, names, node);
    return;
  case AST_VALUE_SCOPE:
    own_prescan(w, names, ((ValueScope *)node->data)->body);
    return;
  case AST_LAMBDA_EXPRESSION:
    return;
  default:
    own_prescan_list(w, names, node->children, node->child_count);
    return;
  }
}

static void own_state_alloc(OwnWalker *w, OwnState *state) {
  state->flags = calloc(w->place_count ? w->place_count : 1, 1);
  state->dead = 0;
}

static void own_state_free(OwnState *state) {
  free(state->flags);
  state->flags = NULL;
}

static void own_state_copy(OwnWalker *w, OwnState *dst, const OwnState *src) {
  if (!dst->flags) {
    own_state_alloc(w, dst);
  }
  memcpy(dst->flags, src->flags, w->place_count);
  dst->dead = src->dead;
}

static OwnState own_state_clone(OwnWalker *w, const OwnState *src) {
  OwnState out = {0};
  own_state_copy(w, &out, src);
  return out;
}

static OwnState own_state_dead(OwnWalker *w) {
  OwnState out = {0};
  own_state_alloc(w, &out);
  out.dead = 1;
  return out;
}

static void own_state_join(OwnWalker *w, OwnState *dst, const OwnState *src) {
  if (src->dead) {
    return;
  }
  if (dst->dead) {
    own_state_copy(w, dst, src);
    return;
  }
  for (size_t i = 0; i < w->place_count; i++) {
    dst->flags[i] |= src->flags[i];
  }
}

static int own_state_equal(OwnWalker *w, const OwnState *a,
                           const OwnState *b) {
  if (a->dead != b->dead) {
    return 0;
  }
  return a->dead || memcmp(a->flags, b->flags, w->place_count) == 0;
}

static void own_set_subtree(OwnWalker *w, OwnState *state, int place,
                            unsigned char flags) {
  for (size_t i = 0; i < w->place_count; i++) {
    if (own_is_ancestor(w, place, (int)i)) {
      state->flags[i] = flags;
    }
  }
}

static int own_effective(OwnWalker *w, const OwnState *state, int place) {
  int maybe_moved = 0;
  unsigned char self = state->flags[place];
  if (self == 0) {
    return 0;
  }
  for (int p = place; p >= 0; p = w->places[p].parent) {
    unsigned char f = state->flags[p];
    if (f == OWN_MOVED) {
      return OWN_MOVED;
    }
    if (f & OWN_MOVED) {
      maybe_moved = 1;
    }
  }
  return (self & OWN_LIVE) | (maybe_moved ? OWN_MOVED : 0);
}

static int own_descendant_moved(OwnWalker *w, const OwnState *state,
                                int place) {
  for (size_t i = 0; i < w->place_count; i++) {
    if ((int)i != place && own_is_ancestor(w, place, (int)i) &&
        (state->flags[i] & OWN_MOVED)) {
      return (int)i;
    }
  }
  return -1;
}

static int own_ancestor_with_destructor(OwnWalker *w, int place) {
  for (int p = w->places[place].parent; p >= 0; p = w->places[p].parent) {
    if (own_type_has_destructor(w->checker, w->places[p].type)) {
      return p;
    }
  }
  return -1;
}

static void own_event_from(OwnWalker *w, int place, int value, int source) {
  if (w->mode != OWN_MODE_CHECK || w->silent > 0 || !w->unit) {
    return;
  }
  OwnEvent *grown = own_reserve(w->events, &w->event_capacity,
                                w->event_count, sizeof(*w->events));
  if (!grown) {
    return;
  }
  w->events = grown;
  w->events[w->event_count].unit = w->unit;
  w->events[w->event_count].place = place;
  w->events[w->event_count].value = value;
  w->events[w->event_count].source = source;
  w->event_count++;
}

static void own_event(OwnWalker *w, int place, int value) {
  own_event_from(w, place, value, -1);
}

static SourceLocation own_site(const ASTNode *node) {
  SourceLocation none = {0, 0, NULL};
  return node ? node->location : none;
}

static void own_report_moved_use(OwnWalker *w, int place, ASTNode *node,
                                 const char *what) {
  OwnPlace *p = &w->places[place];
  ASTNode *site = p->last_drop ? p->last_drop : p->last_move;
  for (int a = p->parent; !site && a >= 0; a = w->places[a].parent) {
    site = w->places[a].last_drop ? w->places[a].last_drop
                                  : w->places[a].last_move;
  }
  int maybe = own_effective(w, &w->state, place) & OWN_LIVE;
  if (site == node) {
    own_error(w, "O0001", node->location,
              "move it before the loop, or put a new value in it before the "
              "next pass",
              "%s of `%s`, which this same line moved on the pass before",
              what, p->path);
    return;
  }
  own_error(w, "O0001", node->location,
            p->last_drop && site == p->last_drop
                ? (strcmp(what, "drop") == 0
                       ? "a value is dropped once: remove this drop or the "
                         "earlier one"
                       : "use the value before the drop, or move the drop "
                         "after this line")
                : "use the value before it moves, or give the receiver a "
                  "pointer with '&' so it borrows",
            "%s of `%s` after it %s%s", what, p->path,
            p->last_drop && site == p->last_drop ? "was dropped"
                                                 : "moved",
            maybe ? " on some paths" : "");
  if (site) {
    own_note_at(w, site->location, "`%s` %s here", p->path,
                site == p->last_drop ? "was dropped" : "moved");
  }
}

static int own_place_is_string(const OwnWalker *w, int place) {
  return place >= 0 && w->bindings[w->places[place].binding].is_string;
}

static void own_use(OwnWalker *w, int place, ASTNode *node, int whole,
                    const char *what) {
  int moved_child = -1;
  if (w->state.dead || place < 0 || own_place_is_string(w, place)) {
    return;
  }
  if (own_effective(w, &w->state, place) & OWN_MOVED) {
    own_report_moved_use(w, place, node, what);
    return;
  }
  if (whole && (moved_child = own_descendant_moved(w, &w->state, place)) >= 0) {
    own_error(w, "O0002", node->location,
              "use the parts that are still there, or put a value back in "
              "first",
              "%s of `%s`, which is partly moved: `%s` moved", what,
              w->places[place].path, w->places[moved_child].path);
    if (w->places[moved_child].last_move) {
      own_note_at(w, w->places[moved_child].last_move->location,
                  "`%s` moved here", w->places[moved_child].path);
    }
  }
}

static void own_record_move(OwnWalker *w, int place, ASTNode *node) {
  char reason[256];
  char note[256];
  if (!own_reporting(w)) {
    return;
  }
  snprintf(note, sizeof(note), "`%s` moves here", w->places[place].path);
  own_note_statement(w->note_statement, note);
  snprintf(reason, sizeof(reason), "`%s` moves here and is not dropped by %s",
           w->places[place].path, own_function_name(w));
  own_add_record(OWN_RECORD_MOVE, node->location,
                 w->bindings[w->places[place].binding].location,
                 own_function_name(w), w->places[place].path,
                 w->places[place].type, NULL, reason, 0, 1);
}

static void own_move(OwnWalker *w, int place, ASTNode *node) {
  int owner = -1;
  if (w->state.dead || place < 0) {
    return;
  }
  own_use(w, place, node, 1, "move");
  if ((owner = own_ancestor_with_destructor(w, place)) >= 0) {
    own_error(w, "O0003", node->location,
              "take it with mem_take(&...) or mem_replace(&..., value), which "
              "leave a value behind for the destructor",
              "cannot move `%s` out of `%s`: %s has a destructor that needs "
              "every field",
              w->places[place].path, w->places[owner].path,
              w->places[owner].type && w->places[owner].type->name
                  ? w->places[owner].type->name
                  : "its type");
  }
  if (w->conditional > 0) {
    own_error(w, "O0004", node->location,
              "move it in a statement of its own, inside an 'if'",
              "`%s` moves inside the right side of '&&' or '||', which runs "
              "only sometimes",
              w->places[place].path);
  }
  if (w->in_errdefer) {
    own_error(w, "O0005", node->location,
              "an owned value is dropped at the end of its scope on every "
              "path, the error return included; remove this",
              "errdefer cannot move or drop `%s`", w->places[place].path);
  }
  own_set_subtree(w, &w->state, place, OWN_MOVED);
  own_event(w, place, 0);
  w->places[place].last_move = node;
  own_record_move(w, place, node);
}

static int own_string_source(OwnWalker *w, ASTNode *node) {
  int binding = node && node->type == AST_IDENTIFIER ? own_root_binding(w, node)
                                                     : -1;
  return binding >= 0 && w->bindings[binding].is_string
             ? w->bindings[binding].root
             : -1;
}

static void own_hand_over(OwnWalker *w, int source, ASTNode *node,
                          const char *into, int kept) {
  char note[300];
  char reason[300];
  if (w->state.dead || source < 0) {
    return;
  }
  own_set_subtree(w, &w->state, source, OWN_MOVED);
  own_event(w, source, 0);
  w->places[source].last_move = node;
  if (!own_reporting(w)) {
    return;
  }
  snprintf(note, sizeof(note), "`%s` moves into `%s` here",
           w->places[source].path, into ? into : "?");
  own_note_statement(w->note_statement, note);
  snprintf(reason, sizeof(reason), "`%s` hands its string to `%s`, %s",
           w->places[source].path, into ? into : "?",
           kept ? "which drops it in turn"
                : "which nothing drops, so the string lives until the "
                  "program ends");
  own_add_record(OWN_RECORD_MOVE, node->location,
                 w->bindings[w->places[source].binding].location,
                 own_function_name(w), w->places[source].path,
                 w->places[source].type, NULL, reason, 0, 1);
}

static void own_take_string(OwnWalker *w, int place, int before, int source) {
  if (w->state.dead) {
    return;
  }
  if (before == OWN_LIVE || before == OWN_MOVED || before == 0) {
    int live = before == OWN_LIVE;
    own_set_subtree(w, &w->state, place, live ? OWN_LIVE : OWN_MOVED);
    own_event(w, place, live);
    return;
  }
  own_set_subtree(w, &w->state, place, OWN_LIVE | OWN_MOVED);
  own_event_from(w, place, 0, source);
}

static void own_reinit(OwnWalker *w, int place, ASTNode *node) {
  if (w->state.dead || place < 0) {
    return;
  }
  for (int p = w->places[place].parent; p >= 0; p = w->places[p].parent) {
    if (own_effective(w, &w->state, p) & OWN_MOVED) {
      own_error(w, "O0006", node->location,
                "assign the whole of `%s` first",
                "cannot assign to `%s`: `%s` has moved",
                w->places[place].path, w->places[p].path);
      return;
    }
  }
  own_set_subtree(w, &w->state, place, OWN_LIVE);
  own_event(w, place, 1);
}

static void own_add_step(OwnDropStep **steps, size_t *count, size_t *capacity,
                         const int *path, int depth, struct Type *type,
                         int guard) {
  OwnDropStep *grown = own_reserve(*steps, capacity, *count, sizeof(**steps));
  if (!grown || depth > OWN_PATH_MAX) {
    return;
  }
  *steps = grown;
  OwnDropStep *step = &(*steps)[(*count)++];
  memset(step, 0, sizeof(*step));
  memcpy(step->path, path, (size_t)depth * sizeof(int));
  step->depth = depth;
  step->type = type;
  step->guard = guard;
}

typedef struct {
  OwnDropStep *steps;
  size_t count;
  size_t capacity;
} OwnSteps;

static void own_plan_place(OwnWalker *w, const OwnState *state, int place,
                           int *path, int depth, OwnSteps *out) {
  int effective = own_effective(w, state, place);
  struct Type *type = own_strip_type(w->places[place].type);
  int guard = -1;
  if (effective == 0 || effective == OWN_MOVED) {
    return;
  }
  if (effective & OWN_MOVED) {
    w->places[place].wants_bit = 1;
    guard = place;
  }
  if (own_descendant_moved(w, state, place) < 0) {
    own_add_step(&out->steps, &out->count, &out->capacity, path, depth,
                 w->places[place].type, guard);
    own_require_glue(w->checker, w->places[place].type,
                     w->bindings[w->places[place].binding].location);
    return;
  }
  for (size_t f = type ? type->field_count : 0; f > 0; f--) {
    int index = (int)(f - 1);
    struct Type *field_type = type->field_types[index];
    int child = own_child_place(w, place, index);
    if (!own_type_owns(w->checker, field_type) || depth >= OWN_PATH_MAX) {
      continue;
    }
    path[depth] = index;
    if (child >= 0) {
      own_plan_place(w, state, child, path, depth + 1, out);
    } else {
      own_add_step(&out->steps, &out->count, &out->capacity, path, depth + 1,
                   field_type, guard);
      own_require_glue(w->checker, field_type,
                       w->bindings[w->places[place].binding].location);
    }
  }
}

static size_t own_plan(OwnWalker *w, const OwnState *state, int place,
                       OwnSteps *out) {
  int path[OWN_PATH_MAX] = {0};
  memset(out, 0, sizeof(*out));
  if (state->dead || place < 0) {
    return 0;
  }
  own_plan_place(w, state, place, path, 0, out);
  return out->count;
}

static void own_store_or_free(OwnWalker *w, ASTNode *call, ASTNode *exit,
                              OwnSteps *steps, int unreachable) {
  OwnPendingPlan *grown = NULL;
  if (w->mode != OWN_MODE_CHECK || w->silent > 0) {
    free(steps->steps);
    return;
  }
  grown = own_reserve(w->pending, &w->pending_capacity, w->pending_count,
                      sizeof(*w->pending));
  if (!grown) {
    free(steps->steps);
    return;
  }
  w->pending = grown;
  w->pending[w->pending_count].call = call;
  w->pending[w->pending_count].exit = exit;
  w->pending[w->pending_count].steps = steps->steps;
  w->pending[w->pending_count].count = steps->count;
  w->pending[w->pending_count].unreachable = unreachable;
  w->pending_count++;
}

static void own_add_obligation(OwnWalker *w, OwnObligationKind kind,
                               int binding, ASTNode *node, ASTNode *root,
                               SourceLocation exit_at) {
  if (w->silent > 0 || w->state.dead) {
    return;
  }
  for (size_t i = 0; i < w->obligation_count; i++) {
    OwnObligation *seen = &w->obligations[i];
    if (seen->kind == kind && seen->binding == binding && seen->node == node) {
      return;
    }
  }
  OwnObligation *grown = own_reserve(w->obligations, &w->obligation_capacity,
                                     w->obligation_count,
                                     sizeof(*w->obligations));
  if (!grown) {
    return;
  }
  w->obligations = grown;
  OwnObligation *ob = &w->obligations[w->obligation_count++];
  memset(ob, 0, sizeof(*ob));
  ob->kind = kind;
  ob->binding = binding;
  ob->node = node;
  ob->root = root;
  ob->statement = w->statement;
  ob->block = w->statement_block;
  ob->root_is_statement = root && root == w->statement;
  ob->exit_at = exit_at;
}

static void own_drop_run_text(OwnWalker *w, OwnDeferEntry *entry,
                              const OwnSteps *steps, char *out, size_t size,
                              int quoted) {
  const char *path = w->places[entry->place].path;
  int guarded = 0;
  int partial = 0;
  for (size_t i = 0; i < steps->count; i++) {
    guarded = guarded || steps->steps[i].guard >= 0;
    partial = partial || steps->steps[i].depth > 0;
  }
  snprintf(out, size, "%s%s%s%s%s", quoted ? "drops " : "drop(",
           partial ? "what is left of " : "", quoted ? "`" : "", path,
           quoted ? "`" : ")");
  if (guarded) {
    size_t used = strlen(out);
    snprintf(out + used, size - used, " if it has not moved");
  }
}

static void own_note_drop_run(OwnWalker *w, OwnDeferEntry *entry,
                              ASTNode *exit, const OwnSteps *steps) {
  char note[300];
  own_drop_run_text(w, entry, steps, note, sizeof(note), 1);
  if (exit && exit->type == AST_PROGRAM) {
    own_note_block_end(exit, note);
  } else if (exit && exit->type != AST_FUNCTION_DECLARATION) {
    own_note_statement(exit, note);
  }
}

static void own_record_drop_run(OwnWalker *w, OwnDeferEntry *entry,
                                ASTNode *exit, const OwnSteps *steps) {
  char reason[300];
  char text[300];
  const char *path = entry->place >= 0 ? w->places[entry->place].path : "?";
  if (!own_reporting(w) || steps->count == 0) {
    return;
  }
  own_note_drop_run(w, entry, exit, steps);
  own_drop_run_text(w, entry, steps, text, sizeof(text), 0);
  snprintf(reason, sizeof(reason), "`%s` is dropped here%s", path,
           ownership_node_is_inserted(entry->defer)
               ? ", by the drop the compiler inserted"
               : "");
  own_add_record(OWN_RECORD_DROP_RUN, own_site(exit),
                 own_site(entry->defer), own_function_name(w), path,
                 entry->place >= 0 ? w->places[entry->place].type : NULL,
                 text, reason, ownership_node_is_inserted(entry->defer), 1);
  own_link_compiled(entry->call);
}

static void own_run_deferred_statement(OwnWalker *w, OwnDeferEntry *entry,
                                       OwnState *state) {
  DeferStatement *defer = (DeferStatement *)entry->defer->data;
  OwnState saved = w->state;
  const void *saved_unit = w->unit;
  w->state = *state;
  w->in_deferred++;
  own_stmt(w, defer->statement);
  w->in_deferred--;
  *state = w->state;
  w->state = saved;
  w->unit = saved_unit;
}

static void own_run_defer(OwnWalker *w, OwnDeferEntry *entry, ASTNode *exit,
                          OwnState *state) {
  OwnSteps steps;
  switch (entry->kind) {
  case OWN_DEFER_DROP:
    own_plan(w, state, entry->place, &steps);
    own_record_drop_run(w, entry, exit, &steps);
    own_store_or_free(w, entry->call, exit, &steps, state->dead);
    if (!state->dead) {
      own_set_subtree(w, state, entry->place, OWN_MOVED);
    }
    return;
  case OWN_DEFER_FORGET:
    if (!state->dead) {
      own_set_subtree(w, state, entry->place, OWN_MOVED);
    }
    return;
  case OWN_DEFER_DROP_UNTRACKED:
    return;
  case OWN_DEFER_STATEMENT: {
    ASTNode *saved_exit = w->current_exit;
    w->current_exit = exit;
    if (!state->dead) {
      own_run_deferred_statement(w, entry, state);
    }
    w->current_exit = saved_exit;
    return;
  }
  }
}

static void own_check_scope_bindings(OwnWalker *w, OwnScope *scope,
                                     ASTNode *exit, OwnState *state) {
  for (size_t i = 0; i < scope->binding_count; i++) {
    int binding = scope->bindings[i];
    OwnSteps steps;
    if (own_plan(w, state, w->bindings[binding].root, &steps) == 0) {
      free(steps.steps);
      continue;
    }
    free(steps.steps);
    own_add_obligation(w, OWN_OB_SCOPE_DROP, binding, NULL, NULL,
                       own_site(exit));
  }
}

static void own_unwind(OwnWalker *w, ASTNode *exit, size_t target,
                       OwnState *state) {
  OwnState saved = w->state;
  int was_dead = state->dead;
  for (size_t s = w->scope_count; s > target; s--) {
    OwnScope *scope = &w->scopes[s - 1];
    for (size_t d = scope->defer_count; d > 0; d--) {
      own_run_defer(w, &scope->defers[d - 1], exit, state);
    }
    if (!state->dead) {
      w->state = *state;
      own_check_scope_bindings(w, scope, exit, state);
      w->state = saved;
    }
  }
  state->dead = was_dead ? 1 : state->dead;
}

static void own_exit_to(OwnWalker *w, ASTNode *exit, size_t target,
                        OwnState *accumulate) {
  OwnState state = own_state_clone(w, &w->state);
  own_unwind(w, exit, target, &state);
  if (accumulate) {
    own_state_join(w, accumulate, &state);
  }
  own_state_free(&state);
  w->state.dead = 1;
}

static OwnScope *own_push_scope(OwnWalker *w, ASTNode *block) {
  OwnScope *grown = own_reserve(w->scopes, &w->scope_capacity,
                                w->scope_count, sizeof(*w->scopes));
  if (!grown) {
    return NULL;
  }
  w->scopes = grown;
  OwnScope *scope = &w->scopes[w->scope_count++];
  memset(scope, 0, sizeof(*scope));
  scope->block = block;
  return scope;
}

static void own_pop_scope(OwnWalker *w) {
  OwnScope *scope = &w->scopes[--w->scope_count];
  free(scope->defers);
  free(scope->bindings);
  memset(scope, 0, sizeof(*scope));
}

static void own_scope_add_binding(OwnWalker *w, int binding) {
  OwnScope *scope = &w->scopes[w->scope_count - 1];
  int *grown = own_reserve(scope->bindings, &scope->binding_capacity,
                           scope->binding_count, sizeof(*scope->bindings));
  if (grown) {
    scope->bindings = grown;
    scope->bindings[scope->binding_count++] = binding;
  }
  w->bindings[binding].scope = w->scope_count - 1;
}

static void own_scope_add_defer(OwnWalker *w, OwnDeferKind kind,
                                ASTNode *defer, ASTNode *call, int place) {
  OwnScope *scope = &w->scopes[w->scope_count - 1];
  OwnDeferEntry *grown = own_reserve(scope->defers, &scope->defer_capacity,
                                     scope->defer_count,
                                     sizeof(*scope->defers));
  if (!grown) {
    return;
  }
  scope->defers = grown;
  OwnDeferEntry *entry = &scope->defers[scope->defer_count++];
  entry->kind = kind;
  entry->defer = defer;
  entry->call = call;
  entry->place = place;
}

static void own_declare(OwnWalker *w, int binding) {
  int root = w->bindings[binding].root;
  own_scope_add_binding(w, binding);
  if (w->state.dead) {
    return;
  }
  own_set_subtree(w, &w->state, root, OWN_LIVE);
  own_event(w, root, 1);
}

static OwnFrame *own_push_frame(OwnWalker *w, OwnFrameKind kind,
                                const char *label, int scope_id,
                                ASTNode *node) {
  OwnFrame *grown = own_reserve(w->frames, &w->frame_capacity,
                                w->frame_count, sizeof(*w->frames));
  if (!grown) {
    return NULL;
  }
  w->frames = grown;
  OwnFrame *frame = &w->frames[w->frame_count++];
  memset(frame, 0, sizeof(*frame));
  frame->kind = kind;
  frame->label = label;
  frame->scope_id = scope_id;
  frame->depth = w->scope_count;
  frame->breaks = own_state_dead(w);
  frame->continues = own_state_dead(w);
  frame->node = node;
  return frame;
}

static void own_pop_frame(OwnWalker *w, OwnState *breaks,
                          OwnState *continues) {
  OwnFrame *frame = &w->frames[--w->frame_count];
  if (breaks) {
    *breaks = frame->breaks;
  } else {
    own_state_free(&frame->breaks);
  }
  if (continues) {
    *continues = frame->continues;
  } else {
    own_state_free(&frame->continues);
  }
}

static OwnFrame *own_find_frame(OwnWalker *w, const char *label,
                                int continuing) {
  for (size_t i = w->frame_count; i > 0; i--) {
    OwnFrame *frame = &w->frames[i - 1];
    if (frame->kind == OWN_FRAME_VALUE) {
      if (label && frame->label && strcmp(label, frame->label) == 0 &&
          !continuing) {
        return frame;
      }
      continue;
    }
    if (continuing && frame->kind != OWN_FRAME_LOOP) {
      continue;
    }
    if (!label || (frame->label && strcmp(label, frame->label) == 0)) {
      return frame;
    }
  }
  return NULL;
}

static OwnFrame *own_find_value_frame(OwnWalker *w, int scope_id) {
  for (size_t i = w->frame_count; i > 0; i--) {
    if (w->frames[i - 1].kind == OWN_FRAME_VALUE &&
        w->frames[i - 1].scope_id == scope_id) {
      return &w->frames[i - 1];
    }
  }
  return NULL;
}

static void own_rvalue(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  int owning = own_node_owns(w, node);
  if (ctx == OWN_CTX_MOVE || ctx == OWN_CTX_CONSUMED ||
      (!owning && !own_string_owned(w->checker, node))) {
    return;
  }
  if (ctx == OWN_CTX_DISCARD) {
    own_add_obligation(w, OWN_OB_DISCARD, -1, node, node, own_site(node));
    return;
  }
  own_add_obligation(w, OWN_OB_TEMPORARY, -1, node, w->root, own_site(node));
}

static OwnCtx own_value_ctx(OwnWalker *w, struct Type *type) {
  if (own_type_owns(w->checker, type)) {
    return OWN_CTX_MOVE;
  }
  return own_is_string_type(type) ? OWN_CTX_CONSUMED : OWN_CTX_READ;
}

static OwnCtx own_node_value_ctx(OwnWalker *w, ASTNode *node) {
  return node ? own_value_ctx(w, node->resolved_type) : OWN_CTX_READ;
}

static int own_is_named_builtin(const char *name) {
  static const char *const names[] = {"sizeof",   "typeof",  "offsetof",
                                      "layoutof", "textof",  "fieldof",
                                      "static_assert"};
  for (size_t i = 0; name && i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(name, names[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int own_place_of(OwnWalker *w, ASTNode *node, int *exact) {
  *exact = 0;
  if (!node) {
    return -1;
  }
  if (node->type == AST_IDENTIFIER) {
    int binding = own_root_binding(w, node);
    *exact = binding >= 0;
    return binding >= 0 ? w->bindings[binding].root : -1;
  }
  if (node->type == AST_MEMBER_ACCESS) {
    return own_resolve_place(w, node, exact);
  }
  return -1;
}

static void own_not_built_string(OwnWalker *w, ASTNode *arg) {
  char subject[200];
  own_error(w, "O0015", arg->location,
            "drop a string where it holds what interpolation, `+` or a "
            "function that builds one returned; a literal or a view of "
            "someone else's string is not yours to free",
            "`%s` holds no string this function built, so there is nothing "
            "to drop",
            own_place_text(arg, subject, sizeof(subject)));
}

static int own_in_generic_instance(const OwnWalker *w) {
  return w->function && w->function->is_generic_instance;
}

static void own_drop_tracked(OwnWalker *w, ASTNode *call, ASTNode *arg,
                             int place, int forget) {
  OwnSteps steps;
  int owner = own_ancestor_with_destructor(w, place);
  if (!w->state.dead &&
      own_effective(w, &w->state, place) == OWN_MOVED) {
    ASTNode *site = w->places[place].last_drop ? w->places[place].last_drop
                                               : w->places[place].last_move;
    if (own_place_is_string(w, place) && (!site || site == call)) {
      own_not_built_string(w, arg);
    } else {
      own_report_moved_use(w, place, arg, forget ? "forget" : "drop");
    }
    return;
  }
  if (owner >= 0) {
    own_error(w, "O0003", arg->location,
              "drop the whole value instead",
              "cannot drop `%s` on its own: %s has a destructor that needs "
              "every field",
              w->places[place].path, w->places[owner].path);
  }
  if (!forget) {
    own_plan(w, &w->state, place, &steps);
    own_store_or_free(w, call, w->in_deferred ? w->current_exit : NULL,
                      &steps, w->state.dead);
  }
  if (w->in_errdefer) {
    own_move(w, place, arg);
    return;
  }
  if (!w->state.dead) {
    own_set_subtree(w, &w->state, place, OWN_MOVED);
    own_event(w, place, 0);
    w->places[place].last_drop = call;
  }
}

static void own_drop_call(OwnWalker *w, ASTNode *node, CallExpression *call,
                          int forget) {
  ASTNode *arg = call->argument_count == 1 ? call->arguments[0] : NULL;
  int exact = 0;
  int place = own_place_of(w, arg, &exact);
  if (!arg) {
    return;
  }
  if (place >= 0 && exact) {
    own_drop_tracked(w, node, arg, place, forget);
    return;
  }
  if (own_is_string_type(arg->resolved_type)) {
    if (own_string_owned(w->checker, arg)) {
      own_expr(w, arg, OWN_CTX_MOVE);
    } else if (own_in_generic_instance(w)) {
      own_expr(w, arg, OWN_CTX_READ);
    } else {
      own_not_built_string(w, arg);
    }
    return;
  }
  if (place >= 0) {
    own_use(w, place, arg, 0, "drop");
    return;
  }
  if (!forget && arg->resolved_type) {
    own_require_glue(w->checker, arg->resolved_type, arg->location);
  }
  if (arg->type == AST_UNARY_EXPRESSION || arg->type == AST_INDEX_EXPRESSION ||
      arg->type == AST_MEMBER_ACCESS || arg->type == AST_IDENTIFIER) {
    own_expr(w, arg, OWN_CTX_ADDRESS);
    return;
  }
  own_expr(w, arg, OWN_CTX_MOVE);
}

static int own_is_interp_call_expr(const CallExpression *call) {
  return call && call->function_name && !call->object &&
         strcmp(call->function_name, "__mtl_interp") == 0;
}

static OwnCtx own_arg_ctx(OwnWalker *w, CallExpression *call, ASTNode *arg,
                          size_t index) {
  if (own_node_owns(w, arg)) {
    return OWN_CTX_MOVE;
  }
  if (!arg || !own_is_string_type(arg->resolved_type)) {
    return OWN_CTX_READ;
  }
  if (w->in_capture) {
    return OWN_CTX_CONSUMED;
  }
  if (call && (own_is_interp_call_expr(call) ||
               own_callee_borrows(w->checker, call, index))) {
    return OWN_CTX_TEMP;
  }
  return OWN_CTX_CONSUMED;
}

static void own_call_args(OwnWalker *w, CallExpression *call, ASTNode **args,
                          size_t count) {
  for (size_t i = 0; i < count; i++) {
    own_expr(w, args[i], own_arg_ctx(w, call, args[i], i));
  }
}

static void own_check_gathered(OwnWalker *w, CallExpression *call) {
  Symbol *callee = call->function_name && !call->is_indirect_call
                       ? symbol_table_lookup_global(w->checker->symbol_table,
                                                    call->function_name)
                       : NULL;
  size_t fixed = 0;
  if (!callee || callee->kind != SYMBOL_FUNCTION ||
      !callee->data.function.is_variadic ||
      callee->data.function.parameter_count == 0) {
    return;
  }
  fixed = callee->data.function.parameter_count - 1;
  for (size_t i = fixed; i < call->argument_count; i++) {
    if (own_node_owns(w, call->arguments[i])) {
      own_error(w, "O0014", call->arguments[i]->location,
                "pass a pointer, or give the function a parameter of the "
                "owning type",
                "`%s` gathers its last arguments into a slice, and a slice "
                "owns nothing, so this value would never be dropped",
                call->function_name);
      return;
    }
  }
}

static void own_call(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  CallExpression *call = (CallExpression *)node->data;
  const char *name = call ? call->function_name : NULL;
  if (!call) {
    return;
  }
  if (name && !call->object &&
      (strcmp(name, "drop") == 0 || strcmp(name, "forget") == 0)) {
    own_drop_call(w, node, call, strcmp(name, "forget") == 0);
    return;
  }
  if (own_is_named_builtin(name)) {
    return;
  }
  if (name && !call->is_indirect_call &&
      ownership_is_destructor_name(w->checker, name) &&
      !own_is_glue_function(w->function)) {
    own_error(w, "O0007", node->location,
              "write drop(value): it runs the destructor and then drops the "
              "fields, and the compiler knows the value is gone",
              "`%s` is a destructor and is not called by name", name);
  }
  if (call->object) {
    own_expr(w, call->object, own_node_owns(w, call->object) ? OWN_CTX_MOVE
                                                             : OWN_CTX_READ);
  }
  own_check_gathered(w, call);
  own_call_args(w, call, call->arguments, call->argument_count);
  own_rvalue(w, node, ctx);
}

static int own_is_global_variable(OwnWalker *w, ASTNode *node) {
  Identifier *id = (Identifier *)node->data;
  Symbol *symbol = id && id->name ? symbol_table_lookup_global(
                                        w->checker->symbol_table, id->name)
                                  : NULL;
  return symbol && symbol->kind == SYMBOL_VARIABLE;
}

static void own_identifier(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  int binding = own_root_binding(w, node);
  if (binding < 0) {
    if (!own_node_owns(w, node)) {
      return;
    }
    if (!own_is_global_variable(w, node)) {
      own_rvalue(w, node, ctx);
      return;
    }
    if (ctx == OWN_CTX_MOVE) {
      Identifier *id = (Identifier *)node->data;
      own_error(w, "O0008", node->location,
                "take it with mem_take(&name) or mem_replace(&name, value)",
                "cannot move `%s` out of a global",
                id && id->name ? id->name : "?");
    }
    return;
  }
  int root = w->bindings[binding].root;
  if (ctx == OWN_CTX_MOVE) {
    own_move(w, root, node);
    return;
  }
  own_use(w, root, node, 1, ctx == OWN_CTX_ADDRESS ? "borrow" : "use");
}

static int own_reaches_through_pointer(ASTNode *node) {
  while (node && node->type == AST_MEMBER_ACCESS) {
    node = ((MemberAccess *)node->data)->object;
  }
  return node && (node->type == AST_UNARY_EXPRESSION ||
                  node->type == AST_INDEX_EXPRESSION);
}

static void own_member(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  MemberAccess *access = (MemberAccess *)node->data;
  int exact = 0;
  int place = own_resolve_place(w, node, &exact);
  if (place >= 0) {
    if (exact && ctx == OWN_CTX_MOVE) {
      own_move(w, place, node);
    } else {
      own_use(w, place, node, exact, ctx == OWN_CTX_ADDRESS ? "borrow"
                                                            : "use");
    }
    return;
  }
  if (ctx == OWN_CTX_MOVE && own_node_owns(w, node)) {
    own_error(w, "O0009", node->location,
              "take it with mem_take(&...) or mem_replace(&..., value), "
              "which leave a value in its place",
              "cannot move a value out of %s",
              own_reaches_through_pointer(node) ? "memory reached through a "
                                                  "pointer or an index"
                                                : "this expression");
  }
  if (!access || !access->object) {
    return;
  }
  if (own_node_owns(w, access->object) &&
      access->object->type != AST_IDENTIFIER &&
      access->object->type != AST_MEMBER_ACCESS &&
      access->object->type != AST_UNARY_EXPRESSION &&
      access->object->type != AST_INDEX_EXPRESSION) {
    own_expr(w, access->object, OWN_CTX_TEMP);
    return;
  }
  own_expr(w, access->object,
           ctx == OWN_CTX_ADDRESS ? OWN_CTX_ADDRESS : OWN_CTX_READ);
}

static void own_unary(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  UnaryExpression *unary = (UnaryExpression *)node->data;
  if (!unary) {
    return;
  }
  if (unary->operator && strcmp(unary->operator, "&") == 0) {
    own_expr(w, unary->operand, OWN_CTX_ADDRESS);
    return;
  }
  if (unary->operator && strcmp(unary->operator, "*") == 0 &&
      ctx == OWN_CTX_MOVE && own_node_owns(w, node)) {
    own_error(w, "O0009", node->location,
              "take it with mem_take(p) or mem_replace(p, value), which leave "
              "a value in its place",
              "cannot move a value out of memory reached through a pointer");
  }
  own_expr(w, unary->operand, OWN_CTX_READ);
}

static OwnCtx own_operand_ctx(int concat, ASTNode *operand) {
  if (concat && operand && own_is_string_type(operand->resolved_type)) {
    return OWN_CTX_CONSUMED;
  }
  return OWN_CTX_READ;
}

static void own_binary(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  BinaryExpression *binary = (BinaryExpression *)node->data;
  ASTNode *saved_root = w->root;
  int concat = 0;
  if (!binary) {
    return;
  }
  if (binary->operator && (strcmp(binary->operator, "&&") == 0 ||
                           strcmp(binary->operator, "||") == 0)) {
    w->root = binary->left;
    own_expr(w, binary->left, OWN_CTX_READ);
    w->root = binary->right;
    w->conditional++;
    own_expr(w, binary->right, OWN_CTX_READ);
    w->conditional--;
    w->root = saved_root;
    return;
  }
  concat = binary->operator && strcmp(binary->operator, "+") == 0 &&
           own_is_string_type(node->resolved_type);
  own_expr(w, binary->left, own_operand_ctx(concat, binary->left));
  own_expr(w, binary->right, own_operand_ctx(concat, binary->right));
  own_rvalue(w, node, ctx);
}

static void own_index(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
  if (!index) {
    return;
  }
  if (ctx == OWN_CTX_MOVE && own_node_owns(w, node)) {
    own_error(w, "O0009", node->location,
              "take it with mem_take(&a[i]) or mem_replace(&a[i], value), "
              "which leave a value in its place",
              "cannot move a value out of an array element");
  }
  if (own_node_owns(w, index->array) &&
      index->array->type == AST_FUNCTION_CALL) {
    own_expr(w, index->array, OWN_CTX_TEMP);
  } else {
    own_expr(w, index->array,
             ctx == OWN_CTX_ADDRESS ? OWN_CTX_ADDRESS : OWN_CTX_READ);
  }
  own_expr(w, index->index, OWN_CTX_READ);
}

static void own_aggregate(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  AggregateLiteral *literal = (AggregateLiteral *)node->data;
  if (!literal) {
    return;
  }
  own_call_args(w, NULL, literal->elements, literal->element_count);
  own_expr(w, literal->repeat_count, OWN_CTX_READ);
  own_rvalue(w, node, ctx);
}

static void own_value_scope(OwnWalker *w, ASTNode *node, OwnCtx ctx);

static void own_children(OwnWalker *w, ASTNode *node) {
  for (size_t i = 0; i < node->child_count; i++) {
    own_expr(w, node->children[i], OWN_CTX_READ);
  }
}

static void own_func_ptr_call(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  FuncPtrCall *call = (FuncPtrCall *)node->data;
  if (!call) {
    return;
  }
  own_expr(w, call->function, OWN_CTX_READ);
  own_call_args(w, NULL, call->arguments, call->argument_count);
  own_rvalue(w, node, ctx);
}

static void own_lambda(OwnWalker *w, ASTNode *node) {
  FunctionDeclaration *lambda = (FunctionDeclaration *)node->data;
  for (size_t i = 0; lambda && i < lambda->captured_count; i++) {
    struct Type *type =
        lambda->captured_types && lambda->captured_types[i]
            ? type_checker_get_type_by_name(w->checker,
                                            lambda->captured_types[i])
            : NULL;
    if (!own_type_owns(w->checker, type)) {
      continue;
    }
    own_error(w, "O0013", node->location,
              "capture a pointer instead: `var held = &name;` before the "
              "lambda, and use `*held` inside it",
              "this closure would copy `%s`, which owns a resource, into an "
              "environment that is never dropped",
              lambda->captured_names && lambda->captured_names[i]
                  ? lambda->captured_names[i]
                  : "?");
  }
}

static void own_expr(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  if (!node) {
    return;
  }
  switch (node->type) {
  case AST_IDENTIFIER:
    own_identifier(w, node, ctx);
    return;
  case AST_MEMBER_ACCESS:
    own_member(w, node, ctx);
    return;
  case AST_UNARY_EXPRESSION:
    own_unary(w, node, ctx);
    return;
  case AST_BINARY_EXPRESSION:
    own_binary(w, node, ctx);
    return;
  case AST_INDEX_EXPRESSION:
    own_index(w, node, ctx);
    return;
  case AST_FUNCTION_CALL:
    own_call(w, node, ctx);
    return;
  case AST_FUNC_PTR_CALL:
    own_func_ptr_call(w, node, ctx);
    return;
  case AST_AGGREGATE_LITERAL:
    own_aggregate(w, node, ctx);
    return;
  case AST_CAST_EXPRESSION:
    own_expr(w, ((CastExpression *)node->data)->operand, OWN_CTX_READ);
    return;
  case AST_VALUE_SCOPE:
    own_value_scope(w, node, ctx);
    return;
  case AST_LAMBDA_EXPRESSION:
    own_lambda(w, node);
    return;
  case AST_NUMBER_LITERAL:
  case AST_STRING_LITERAL:
    return;
  default:
    own_children(w, node);
    return;
  }
}

static void own_set_unit(OwnWalker *w, ASTNode *unit, ASTNode *root) {
  w->unit = unit;
  w->root = root;
}

static void own_var_declaration(OwnWalker *w, ASTNode *node) {
  VarDeclaration *decl = (VarDeclaration *)node->data;
  int binding = -1;
  int source = -1;
  int before = 0;
  int kept = 0;
  struct Type *type = decl ? own_declared_type(w->checker, decl) : NULL;
  own_set_unit(w, node, decl ? decl->initializer : NULL);
  if (!decl) {
    return;
  }
  source = own_is_string_type(type) ? own_string_source(w, decl->initializer)
                                    : -1;
  if (source >= 0) {
    before = w->state.dead ? 0 : own_effective(w, &w->state, source);
  } else {
    own_expr(w, decl->initializer, own_value_ctx(w, type));
  }
  if (own_map_get(&w->declarations, node, &binding) && binding >= 0) {
    if (!w->statement_block && w->mode == OWN_MODE_CHECK) {
      own_error(w, "O0010", node->location,
                "declare it in a block before the loop",
                "`%s` owns a resource and cannot be declared in a loop "
                "header",
                decl->name);
    }
    own_declare(w, binding);
    kept = w->bindings[binding].is_string;
    if (kept && source >= 0) {
      own_take_string(w, w->bindings[binding].root, before, source);
    } else if (kept && !w->state.dead &&
               !own_string_owned(w->checker, decl->initializer)) {
      own_set_subtree(w, &w->state, w->bindings[binding].root, OWN_MOVED);
      own_event(w, w->bindings[binding].root, 0);
    }
  }
  own_hand_over(w, source, decl->initializer, decl->name, kept);
}

static int own_previous_drops(OwnWalker *w, ASTNode *target) {
  long index = -1;
  Program *block = NULL;
  if (!w->statement_block || !w->statement) {
    return 0;
  }
  block = (Program *)w->statement_block->data;
  index = own_block_index(w->statement_block, w->statement);
  if (index <= 0 || !block) {
    return 0;
  }
  ASTNode *previous = block->declarations[index - 1];
  CallExpression *call = previous && previous->type == AST_FUNCTION_CALL
                             ? (CallExpression *)previous->data
                             : NULL;
  return call && call->function_name && !call->object &&
         strcmp(call->function_name, "drop") == 0 &&
         call->argument_count == 1 &&
         own_places_equal(call->arguments[0], target);
}

static void own_assign_tracked(OwnWalker *w, ASTNode *node, ASTNode *target,
                               int place, int source, int before) {
  OwnSteps steps;
  Assignment *assign = (Assignment *)node->data;
  if (own_plan(w, &w->state, place, &steps) > 0) {
    own_add_obligation(w, OWN_OB_ASSIGN_DROP, w->places[place].binding, node,
                       target, node->location);
  }
  free(steps.steps);
  own_reinit(w, place, node);
  if (!own_place_is_string(w, place)) {
    return;
  }
  if (source >= 0) {
    own_take_string(w, place, before, source);
  } else if (!w->state.dead &&
             !own_string_owned(w->checker, assign ? assign->value : NULL)) {
    own_set_subtree(w, &w->state, place, OWN_MOVED);
    own_event(w, place, 0);
  }
}

static void own_assign_untracked(OwnWalker *w, ASTNode *node,
                                 ASTNode *target) {
  size_t count = w->obligation_count;
  if (target->type != AST_IDENTIFIER) {
    own_expr(w, target, OWN_CTX_ADDRESS);
  }
  if (own_previous_drops(w, target)) {
    return;
  }
  own_add_obligation(w, OWN_OB_ASSIGN_DROP, -1, node, target, node->location);
  if (w->obligation_count > count) {
    w->obligations[w->obligation_count - 1].untracked = 1;
  }
}

static int own_assigned_binding(OwnWalker *w, ASTNode *node) {
  int binding = -1;
  if (own_map_get(&w->assigns, node, &binding)) {
    return binding;
  }
  return -1;
}

static void own_assignment(OwnWalker *w, ASTNode *node) {
  Assignment *assign = (Assignment *)node->data;
  ASTNode *target = NULL;
  int exact = 0;
  int place = -1;
  int binding = -1;
  int source = -1;
  int before = 0;
  own_set_unit(w, node, assign ? assign->value : NULL);
  if (!assign) {
    return;
  }
  if (assign->target_count > 0) {
    own_expr(w, assign->value, own_node_value_ctx(w, assign->value));
    for (size_t i = 0; i < assign->target_count; i++) {
      own_expr(w, assign->targets[i], OWN_CTX_ADDRESS);
    }
    return;
  }
  target = assign->target;
  source = target ? -1 : own_string_source(w, assign->value);
  before = source >= 0 && !w->state.dead
               ? own_effective(w, &w->state, source)
               : 0;
  binding = target ? -1 : own_assigned_binding(w, node);
  if (binding >= 0) {
    if (source < 0) {
      own_expr(w, assign->value, OWN_CTX_MOVE);
    }
    own_assign_tracked(w, node, NULL, w->bindings[binding].root, source,
                       before);
    own_hand_over(w, source, assign->value, assign->variable_name,
                  w->bindings[binding].is_string);
    return;
  }
  place = own_place_of(w, target, &exact);
  if (place >= 0 && exact) {
    own_expr(w, assign->value, OWN_CTX_MOVE);
    own_assign_tracked(w, node, target, place, -1, 0);
    return;
  }
  if (!target || (!own_node_owns(w, target) &&
                  !own_node_owns(w, assign->value))) {
    if (source >= 0) {
      own_hand_over(w, source, assign->value, assign->variable_name, 0);
    } else {
      own_expr(w, assign->value, own_node_value_ctx(w, assign->value));
    }
    own_expr(w, target, OWN_CTX_ADDRESS);
    return;
  }
  own_expr(w, assign->value, OWN_CTX_MOVE);
  own_assign_untracked(w, node, target);
}

static void own_return(OwnWalker *w, ASTNode *node) {
  ReturnStatement *ret = (ReturnStatement *)node->data;
  own_set_unit(w, node, ret ? ret->value : NULL);
  if (ret && ret->value_count > 1) {
    for (size_t i = 0; i < ret->value_count; i++) {
      own_expr(w, ret->values[i], own_node_value_ctx(w, ret->values[i]));
    }
  } else if (ret && ret->value) {
    own_expr(w, ret->value, own_node_value_ctx(w, ret->value));
  }
  own_exit_to(w, node, 0, NULL);
}

static void own_condition(OwnWalker *w, ASTNode *condition) {
  own_set_unit(w, condition, condition);
  w->statement = NULL;
  own_expr(w, condition, OWN_CTX_READ);
}

static OwnState own_branch(OwnWalker *w, ASTNode *body, const OwnState *from) {
  OwnState saved = w->state;
  OwnState result;
  w->state = own_state_clone(w, from);
  own_stmt(w, body);
  result = w->state;
  w->state = saved;
  return result;
}

static void own_if(OwnWalker *w, ASTNode *node) {
  IfStatement *branch = (IfStatement *)node->data;
  OwnState joined;
  OwnState arm;
  if (!branch) {
    return;
  }
  own_condition(w, branch->condition);
  joined = own_state_dead(w);
  arm = own_branch(w, branch->then_branch, &w->state);
  own_state_join(w, &joined, &arm);
  own_state_free(&arm);
  for (size_t i = 0; i < branch->else_if_count; i++) {
    own_condition(w, branch->else_ifs[i].condition);
    arm = own_branch(w, branch->else_ifs[i].body, &w->state);
    own_state_join(w, &joined, &arm);
    own_state_free(&arm);
  }
  if (branch->else_branch) {
    arm = own_branch(w, branch->else_branch, &w->state);
    own_state_join(w, &joined, &arm);
    own_state_free(&arm);
  } else {
    own_state_join(w, &joined, &w->state);
  }
  own_state_free(&w->state);
  w->state = joined;
}

typedef struct {
  ASTNode *condition;
  ASTNode *increment;
  ASTNode *body;
  const char *label;
  ASTNode *node;
} OwnLoopShape;

static void own_loop_once(OwnWalker *w, const OwnLoopShape *shape,
                          OwnState *exit_state, OwnState *breaks,
                          OwnState *back) {
  OwnState continues;
  if (shape->condition) {
    own_condition(w, shape->condition);
  }
  *exit_state = shape->condition ? own_state_clone(w, &w->state)
                                 : own_state_dead(w);
  own_push_frame(w, OWN_FRAME_LOOP, shape->label, 0, shape->node);
  own_stmt(w, shape->body);
  own_pop_frame(w, breaks, &continues);
  own_state_join(w, &w->state, &continues);
  own_state_free(&continues);
  if (shape->increment) {
    w->statement = shape->increment;
    w->statement_block = NULL;
    own_stmt(w, shape->increment);
  }
  *back = w->state;
  w->state = own_state_dead(w);
}

static void own_loop(OwnWalker *w, const OwnLoopShape *shape) {
  OwnState entry = own_state_clone(w, &w->state);
  OwnState head = own_state_clone(w, &w->state);
  OwnState exit_state;
  OwnState breaks;
  OwnState back;
  w->silent++;
  for (int round = 0; round < 64; round++) {
    OwnState next = own_state_clone(w, &entry);
    own_state_free(&w->state);
    w->state = own_state_clone(w, &head);
    own_loop_once(w, shape, &exit_state, &breaks, &back);
    own_state_join(w, &next, &back);
    own_state_free(&back);
    own_state_free(&exit_state);
    own_state_free(&breaks);
    int stable = own_state_equal(w, &next, &head);
    own_state_free(&head);
    head = next;
    if (stable) {
      break;
    }
  }
  w->silent--;
  own_state_free(&w->state);
  w->state = own_state_clone(w, &head);
  own_loop_once(w, shape, &exit_state, &breaks, &back);
  own_state_free(&back);
  own_state_join(w, &exit_state, &breaks);
  own_state_free(&breaks);
  own_state_free(&w->state);
  w->state = exit_state;
  own_state_free(&entry);
  own_state_free(&head);
}

static void own_while(OwnWalker *w, ASTNode *node) {
  WhileStatement *loop = (WhileStatement *)node->data;
  OwnLoopShape shape;
  if (!loop) {
    return;
  }
  shape.condition = loop->condition;
  shape.increment = NULL;
  shape.body = loop->body;
  shape.label = loop->label;
  shape.node = node;
  own_loop(w, &shape);
}

static void own_for(OwnWalker *w, ASTNode *node) {
  ForStatement *loop = (ForStatement *)node->data;
  OwnLoopShape shape;
  ASTNode *saved_block = w->statement_block;
  if (!loop) {
    return;
  }
  own_push_scope(w, NULL);
  if (loop->initializer) {
    w->statement = loop->initializer;
    w->statement_block = NULL;
    own_stmt(w, loop->initializer);
  }
  w->statement_block = saved_block;
  shape.condition = loop->condition;
  shape.increment = loop->increment;
  shape.body = loop->body;
  shape.label = loop->label;
  shape.node = node;
  own_loop(w, &shape);
  own_unwind(w, node, w->scope_count - 1, &w->state);
  own_pop_scope(w);
}

static void own_switch(OwnWalker *w, ASTNode *node) {
  SwitchStatement *sw = (SwitchStatement *)node->data;
  OwnState start;
  OwnState joined;
  OwnState breaks;
  OwnState fall;
  int has_default = 0;
  if (!sw) {
    return;
  }
  own_condition(w, sw->expression);
  start = own_state_clone(w, &w->state);
  joined = own_state_dead(w);
  own_push_frame(w, OWN_FRAME_SWITCH, NULL, 0, node);
  fall = own_state_dead(w);
  for (size_t i = 0; i < sw->case_count; i++) {
    CaseClause *clause =
        sw->cases[i] ? (CaseClause *)sw->cases[i]->data : NULL;
    OwnState entry = own_state_clone(w, &start);
    if (!clause) {
      own_state_free(&entry);
      continue;
    }
    has_default = has_default || clause->is_default;
    own_state_join(w, &entry, &fall);
    own_state_free(&fall);
    OwnFrame *frame = &w->frames[w->frame_count - 1];
    own_state_free(&frame->continues);
    frame->continues = own_state_dead(w);
    fall = own_branch(w, clause->body, &entry);
    own_state_join(w, &joined, &fall);
    own_state_free(&fall);
    fall = own_state_clone(w, &w->frames[w->frame_count - 1].continues);
    own_state_free(&entry);
  }
  own_state_free(&fall);
  own_pop_frame(w, &breaks, NULL);
  own_state_join(w, &joined, &breaks);
  own_state_free(&breaks);
  if (!has_default) {
    own_state_join(w, &joined, &start);
  }
  own_state_free(&start);
  own_state_free(&w->state);
  w->state = joined;
}

static int own_match_moves_scrutinee(OwnWalker *w, MatchStatement *match,
                                     MatchArm *arm) {
  struct Type *payload =
      own_variant_payload(match->expression->resolved_type, arm->variant_name);
  return arm->binding_name && own_type_owns(w->checker, payload);
}

static void own_match_arm(OwnWalker *w, MatchStatement *match, size_t i,
                          int scrutinee, const OwnState *start,
                          OwnState *joined) {
  MatchArm *arm = &match->arms[i];
  int binding = -1;
  OwnState saved = w->state;
  w->state = own_state_clone(w, start);
  own_push_scope(w, arm->body);
  if (scrutinee >= 0 && own_match_moves_scrutinee(w, match, arm)) {
    w->unit = arm;
    own_move(w, scrutinee, match->expression);
  }
  if (own_map_get(&w->declarations, arm, &binding) && binding >= 0) {
    w->unit = arm;
    own_scope_add_binding(w, binding);
    if (!w->state.dead) {
      own_set_subtree(w, &w->state, w->bindings[binding].root, OWN_LIVE);
      own_event(w, w->bindings[binding].root, 1);
    }
  }
  own_stmt(w, arm->body);
  own_unwind(w, arm->body, w->scope_count - 1, &w->state);
  own_pop_scope(w);
  own_state_join(w, joined, &w->state);
  own_state_free(&w->state);
  w->state = saved;
}

static void own_match(OwnWalker *w, ASTNode *node) {
  MatchStatement *match = (MatchStatement *)node->data;
  int exact = 0;
  int scrutinee = -1;
  OwnState start;
  OwnState joined;
  if (!match) {
    return;
  }
  own_set_unit(w, match->expression, match->expression);
  scrutinee = own_place_of(w, match->expression, &exact);
  if (scrutinee >= 0 && exact) {
    own_use(w, scrutinee, match->expression, 1, "match on");
  } else if (own_node_owns(w, match->expression) && scrutinee < 0) {
    own_expr(w, match->expression, OWN_CTX_MOVE);
    own_add_obligation(w, OWN_OB_SCRUTINEE, -1, match->expression, node,
                       node->location);
    scrutinee = -1;
  } else {
    own_expr(w, match->expression, OWN_CTX_READ);
    scrutinee = -1;
  }
  start = own_state_clone(w, &w->state);
  joined = own_state_dead(w);
  int has_default = 0;
  for (size_t i = 0; i < match->arm_count; i++) {
    has_default = has_default || match->arms[i].is_default;
    own_match_arm(w, match, i, exact ? scrutinee : -1, &start, &joined);
  }
  if (!has_default && match->arm_count == 0) {
    own_state_join(w, &joined, &start);
  }
  own_state_free(&start);
  own_state_free(&w->state);
  w->state = joined;
}

static int own_defer_captures(const ASTNode *statement) {
  const CallExpression *call = NULL;
  if (!statement || statement->type != AST_FUNCTION_CALL) {
    return 0;
  }
  call = (const CallExpression *)statement->data;
  if (!call || !call->function_name || call->argument_count == 0 ||
      call->object || call->is_indirect_call ||
      strcmp(call->function_name, "sizeof") == 0 ||
      strcmp(call->function_name, "static_assert") == 0 ||
      strcmp(call->function_name, "drop") == 0 ||
      strcmp(call->function_name, "forget") == 0) {
    return 0;
  }
  for (size_t i = 0; i < call->argument_count; i++) {
    if (!call->arguments[i] || !call->arguments[i]->resolved_type ||
        !call->arguments[i]->resolved_type->name) {
      return 0;
    }
  }
  return 1;
}

static void own_defer_drop(OwnWalker *w, ASTNode *node, ASTNode *call_node,
                           int forget) {
  CallExpression *call = (CallExpression *)call_node->data;
  ASTNode *arg = call && call->argument_count == 1 ? call->arguments[0]
                                                   : NULL;
  int exact = 0;
  int place = own_place_of(w, arg, &exact);
  if (place >= 0 && exact) {
    own_scope_add_defer(w, forget ? OWN_DEFER_FORGET : OWN_DEFER_DROP, node,
                        call_node, place);
    return;
  }
  if (arg && own_is_string_type(arg->resolved_type) &&
      !own_in_generic_instance(w)) {
    own_not_built_string(w, arg);
    return;
  }
  own_scope_add_defer(w, OWN_DEFER_DROP_UNTRACKED, node, call_node, -1);
}

static int own_is_drop_call(const ASTNode *node, int *forget) {
  const CallExpression *call = node && node->type == AST_FUNCTION_CALL
                                   ? (const CallExpression *)node->data
                                   : NULL;
  if (!call || !call->function_name || call->object) {
    return 0;
  }
  *forget = strcmp(call->function_name, "forget") == 0;
  return *forget || strcmp(call->function_name, "drop") == 0;
}

static void own_defer(OwnWalker *w, ASTNode *node) {
  DeferStatement *defer = (DeferStatement *)node->data;
  int forget = 0;
  if (!defer || !defer->statement) {
    return;
  }
  own_set_unit(w, node, defer->statement);
  if (node->type == AST_ERRDEFER_STATEMENT) {
    w->in_errdefer++;
    own_stmt(w, defer->statement);
    w->in_errdefer--;
    return;
  }
  if (own_is_drop_call(defer->statement, &forget)) {
    own_defer_drop(w, node, defer->statement, forget);
    return;
  }
  if (own_defer_captures(defer->statement)) {
    w->in_capture++;
    own_expr(w, defer->statement, OWN_CTX_DISCARD);
    w->in_capture--;
    return;
  }
  own_scope_add_defer(w, OWN_DEFER_STATEMENT, node, NULL, -1);
}

static void own_jump(OwnWalker *w, ASTNode *node, int continuing) {
  LoopControlStatement *control = (LoopControlStatement *)node->data;
  OwnFrame *frame = own_find_frame(w, control ? control->target_label : NULL,
                                   continuing);
  own_set_unit(w, node, NULL);
  if (!frame) {
    w->state.dead = 1;
    return;
  }
  own_exit_to(w, node, frame->depth,
              continuing ? &frame->continues : &frame->breaks);
}

static void own_fallthrough(OwnWalker *w, ASTNode *node) {
  OwnFrame *frame = own_find_frame(w, NULL, 0);
  if (!frame || frame->kind != OWN_FRAME_SWITCH) {
    w->state.dead = 1;
    return;
  }
  own_exit_to(w, node, frame->depth, &frame->continues);
}

static void own_yield(OwnWalker *w, ASTNode *node) {
  YieldStatement *yield = (YieldStatement *)node->data;
  OwnFrame *frame = NULL;
  if (!yield) {
    return;
  }
  frame = yield->from_break && yield->label
              ? own_find_frame(w, yield->label, 0)
              : own_find_value_frame(w, yield->scope_id);
  own_set_unit(w, node, yield->value);
  if (yield->value) {
    own_expr(w, yield->value, own_node_value_ctx(w, yield->value));
  }
  if (!frame) {
    w->state.dead = 1;
    return;
  }
  own_exit_to(w, node, frame->depth, &frame->breaks);
}

static void own_value_scope(OwnWalker *w, ASTNode *node, OwnCtx ctx) {
  ValueScope *scope = (ValueScope *)node->data;
  OwnState breaks;
  ASTNode *saved_statement = w->statement;
  ASTNode *saved_block = w->statement_block;
  ASTNode *saved_root = w->root;
  const void *saved_unit = w->unit;
  if (!scope) {
    return;
  }
  if (ctx != OWN_CTX_MOVE && ctx != OWN_CTX_DISCARD && own_node_owns(w, node)) {
    own_rvalue(w, node, ctx);
  }
  own_push_frame(w, OWN_FRAME_VALUE, scope->label, scope->scope_id, node);
  w->statement_block = NULL;
  own_stmt(w, scope->body);
  own_pop_frame(w, &breaks, NULL);
  own_state_join(w, &w->state, &breaks);
  own_state_free(&breaks);
  w->statement = saved_statement;
  w->statement_block = saved_block;
  w->root = saved_root;
  w->unit = saved_unit;
}

static void own_block(OwnWalker *w, ASTNode *node) {
  Program *block = (Program *)node->data;
  ASTNode *saved_block = w->statement_block;
  ASTNode *saved_statement = w->statement;
  own_push_scope(w, node);
  for (size_t i = 0; block && i < block->declaration_count; i++) {
    w->statement_block = node;
    w->statement = block->declarations[i];
    own_stmt(w, block->declarations[i]);
  }
  own_unwind(w, node, w->scope_count - 1, &w->state);
  own_pop_scope(w);
  w->statement_block = saved_block;
  w->statement = saved_statement;
}

static void own_expression_statement(OwnWalker *w, ASTNode *node) {
  own_set_unit(w, node, node);
  own_expr(w, node, OWN_CTX_DISCARD);
}

static void own_stmt_control(OwnWalker *w, ASTNode *node) {
  switch (node->type) {
  case AST_BREAK_STATEMENT:
    own_jump(w, node, 0);
    return;
  case AST_CONTINUE_STATEMENT:
    own_jump(w, node, 1);
    return;
  case AST_FALLTHROUGH_STATEMENT:
    own_fallthrough(w, node);
    return;
  case AST_YIELD_STATEMENT:
    own_yield(w, node);
    return;
  case AST_RETURN_STATEMENT:
    own_return(w, node);
    return;
  default:
    own_expression_statement(w, node);
    return;
  }
}

static void own_stmt(OwnWalker *w, ASTNode *node) {
  if (!node) {
    return;
  }
  if (w->statement != node) {
    w->statement = node;
    w->statement_block = NULL;
  }
  if (node->type != AST_PROGRAM) {
    w->note_statement = node;
  }
  switch (node->type) {
  case AST_PROGRAM:
    own_block(w, node);
    return;
  case AST_VAR_DECLARATION:
    own_var_declaration(w, node);
    return;
  case AST_ASSIGNMENT:
    own_assignment(w, node);
    return;
  case AST_IF_STATEMENT:
    own_if(w, node);
    return;
  case AST_WHILE_STATEMENT:
    own_while(w, node);
    return;
  case AST_FOR_STATEMENT:
    own_for(w, node);
    return;
  case AST_SWITCH_STATEMENT:
    own_switch(w, node);
    return;
  case AST_MATCH_STATEMENT:
    own_match(w, node);
    return;
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT:
    own_defer(w, node);
    return;
  case AST_VALUE_SCOPE:
    own_set_unit(w, node, node);
    own_value_scope(w, node, OWN_CTX_DISCARD);
    return;
  case AST_QUIESCE_STATEMENT:
  case AST_INLINE_ASM:
  case AST_BARRIER_STATEMENT:
    return;
  default:
    own_stmt_control(w, node);
    return;
  }
}

void own_walker_init(OwnWalker *walker, TypeChecker *checker,
                     ErrorReporter *reporter, ASTNode *function_node,
                     OwnMode mode) {
  memset(walker, 0, sizeof(*walker));
  walker->checker = checker;
  walker->reporter = reporter;
  walker->function_node = function_node;
  walker->function = (FunctionDeclaration *)function_node->data;
  walker->mode = mode;
  walker->is_explicit = walker->function && walker->function->is_explicit;
  own_map_init(&walker->idents);
  own_map_init(&walker->declarations);
  own_map_init(&walker->assigns);
}

void own_walker_free(OwnWalker *walker) {
  for (size_t i = 0; i < walker->place_count; i++) {
    free(walker->places[i].path);
  }
  free(walker->places);
  free(walker->bindings);
  own_map_free(&walker->idents);
  own_map_free(&walker->declarations);
  own_map_free(&walker->assigns);
  for (size_t i = 0; i < walker->pending_count; i++) {
    free(walker->pending[i].steps);
  }
  free(walker->pending);
  while (walker->scope_count > 0) {
    own_pop_scope(walker);
  }
  free(walker->scopes);
  for (size_t i = 0; i < walker->frame_count; i++) {
    own_state_free(&walker->frames[i].breaks);
    own_state_free(&walker->frames[i].continues);
  }
  free(walker->frames);
  own_state_free(&walker->state);
  free(walker->obligations);
  free(walker->events);
  memset(walker, 0, sizeof(*walker));
}

static void own_prescan_function(OwnWalker *w) {
  FunctionDeclaration *fd = w->function;
  OwnNames names = {0};
  own_names_push_mark(&names);
  for (size_t i = 0; i < fd->parameter_count; i++) {
    struct Type *type = own_param_type(w->checker, fd, i);
    int binding = -1;
    if (own_type_owns(w->checker, type)) {
      binding = own_new_binding(w, fd->parameter_names[i], type,
                                OWN_BIND_PARAM, w->function_node, fd->body,
                                w->function_node->location);
    }
    own_names_add(&names, fd->parameter_names[i], binding);
  }
  own_prescan(w, &names, fd->body);
  free(names.names);
  free(names.marks);
}

int own_walk_function(OwnWalker *w) {
  FunctionDeclaration *fd = w->function;
  if (!fd || !fd->body) {
    return 1;
  }
  own_prescan_function(w);
  own_state_alloc(w, &w->state);
  own_push_scope(w, NULL);
  for (size_t i = 0; i < w->binding_count; i++) {
    if (w->bindings[i].kind == OWN_BIND_PARAM) {
      w->unit = w->function_node;
      own_declare(w, (int)i);
    }
  }
  w->statement = fd->body;
  w->statement_block = NULL;
  own_stmt(w, fd->body);
  own_unwind(w, w->function_node, 0, &w->state);
  own_pop_scope(w);
  return w->errors == 0;
}

static void own_assign_bits(OwnWalker *w) {
  for (size_t i = 0; i < w->place_count; i++) {
    char note[300];
    OwnBinding *binding = NULL;
    if (!w->places[i].wants_bit || w->places[i].bit >= 0) {
      continue;
    }
    binding = &w->bindings[w->places[i].binding];
    w->places[i].bit = own_new_bit(w->places[i].path, binding->location.line);
    snprintf(note, sizeof(note),
             binding->is_string
                 ? "`%s` holds a string this function built on some paths "
                   "only, so the live bit %s records whether it holds one"
                 : "`%s` moves on some paths only, so the live bit %s "
                   "records whether it still holds a value",
             w->places[i].path, ownership_bit_name(w->places[i].bit));
    own_note_statement(binding->kind == OWN_BIND_LOCAL ? binding->declaration
                                                       : w->function_node,
                       note);
    own_store_function_bit(w->function_node, w->places[i].bit);
    own_add_record(OWN_RECORD_LIVE_BIT,
                   w->bindings[w->places[i].binding].location,
                   w->bindings[w->places[i].binding].location,
                   own_function_name(w), w->places[i].path, w->places[i].type,
                   NULL,
                   binding->is_string
                       ? "holds a string this function built on some paths "
                         "and not others, so a live bit decides at each drop "
                         "whether there is one to free"
                       : "moves on some paths and not others, so a live bit "
                         "decides at each drop whether it still holds a value",
                   0, 0);
  }
}

static void own_copied_bits(OwnWalker *w) {
  for (size_t e = w->event_count; e > 0; e--) {
    OwnEvent *event = &w->events[e - 1];
    if (event->source < 0) {
      continue;
    }
    for (size_t i = 0; i < w->place_count; i++) {
      if (w->places[i].wants_bit &&
          own_is_ancestor(w, event->place, (int)i)) {
        w->places[event->source].wants_bit = 1;
      }
    }
  }
}

void own_commit_outputs(OwnWalker *w) {
  own_copied_bits(w);
  own_assign_bits(w);
  for (size_t e = 0; e < w->event_count; e++) {
    OwnEvent *event = &w->events[e];
    int source = event->source >= 0 ? w->places[event->source].bit : -1;
    for (size_t i = 0; i < w->place_count; i++) {
      if (w->places[i].bit >= 0 &&
          own_is_ancestor(w, event->place, (int)i)) {
        own_store_unit_write(event->unit, w->places[i].bit, event->value,
                             source);
      }
    }
  }
  for (size_t p = 0; p < w->pending_count; p++) {
    OwnPendingPlan *plan = &w->pending[p];
    for (size_t s = 0; s < plan->count; s++) {
      int guard = plan->steps[s].guard;
      plan->steps[s].guard = guard >= 0 ? w->places[guard].bit : -1;
    }
    own_store_plan(plan->call, plan->exit, plan->steps, plan->count,
                   plan->unreachable);
    plan->steps = NULL;
  }
  w->pending_count = 0;
}
