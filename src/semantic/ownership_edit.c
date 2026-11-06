#include "semantic/ownership_internal.h"
#include "semantic/type_checker_internal.h"
#include "parser/ast_print.h"
#include "string_intern.h"
#include <stdlib.h>
#include <string.h>

static int g_own_scope_serial = 0x40000000;
static int g_own_temp_serial = 0;

ASTNode *own_make_identifier(const char *name, struct Type *type,
                             SourceLocation at) {
  ASTNode *node = ast_create_identifier(name, at);
  if (node) {
    node->resolved_type = type;
  }
  return node;
}

ASTNode *own_make_call(const char *name, ASTNode **args, size_t count,
                       struct Type *result, SourceLocation at) {
  ASTNode *node = ast_create_call_expression(name, args, count, at);
  if (node) {
    node->resolved_type = result;
  }
  return node;
}

ASTNode *own_make_defer(ASTNode *statement, SourceLocation at) {
  return ast_create_defer_statement(statement, at);
}

ASTNode *own_make_block(SourceLocation at) {
  ASTNode *block = ast_create_program();
  if (block) {
    block->location = at;
  }
  return block;
}

static long own_child_index(ASTNode *parent, const ASTNode *child) {
  for (size_t i = 0; i < parent->child_count; i++) {
    if (parent->children[i] == child) {
      return (long)i;
    }
  }
  return -1;
}

static void own_children_insert(ASTNode *parent, long at, ASTNode *child) {
  if (at < 0 || (size_t)at >= parent->child_count) {
    ast_add_child(parent, child);
    return;
  }
  ast_add_child(parent, child);
  memmove(&parent->children[at + 1], &parent->children[at],
          (parent->child_count - 1 - (size_t)at) * sizeof(ASTNode *));
  parent->children[at] = child;
}

void own_block_insert(ASTNode *block, size_t index, ASTNode *statement) {
  Program *program = (Program *)block->data;
  ASTNode **grown = NULL;
  long child_at = -1;
  if (!program || !statement) {
    return;
  }
  if (index > program->declaration_count) {
    index = program->declaration_count;
  }
  if (index < program->declaration_count) {
    child_at = own_child_index(block, program->declarations[index]);
  }
  grown = realloc(program->declarations,
                  (program->declaration_count + 1) * sizeof(ASTNode *));
  if (!grown) {
    return;
  }
  program->declarations = grown;
  memmove(&program->declarations[index + 1], &program->declarations[index],
          (program->declaration_count - index) * sizeof(ASTNode *));
  program->declarations[index] = statement;
  program->declaration_count++;
  own_children_insert(block, child_at, statement);
}

long own_block_index(ASTNode *block, const ASTNode *statement) {
  Program *program = block ? (Program *)block->data : NULL;
  for (size_t i = 0; program && i < program->declaration_count; i++) {
    if (program->declarations[i] == statement) {
      return (long)i;
    }
  }
  return -1;
}

ASTNode *own_node_move_out(ASTNode *node) {
  ASTNode *moved = malloc(sizeof(ASTNode));
  if (!moved) {
    return NULL;
  }
  *moved = *node;
  node->children = NULL;
  node->child_count = 0;
  node->child_capacity = 0;
  node->child_capacity_owner = NULL;
  node->data = NULL;
  node->resolved_type = NULL;
  node->proven_refinement = NULL;
  node->proven_predicate = NULL;
  node->proven_binding = NULL;
  return moved;
}

static void own_become_identifier(ASTNode *node, const char *name,
                                  struct Type *type) {
  ASTNode *fresh = own_make_identifier(name, type, node->location);
  if (!fresh) {
    return;
  }
  *node = *fresh;
  free(fresh);
}

static void own_become(ASTNode *node, ASTNode *replacement) {
  SourceLocation at = node->location;
  *node = *replacement;
  node->location = at;
  free(replacement);
}

ASTNode *own_clone_place(ASTNode *place) {
  ASTNode *copy = NULL;
  if (!place) {
    return NULL;
  }
  switch (place->type) {
  case AST_IDENTIFIER:
    copy = own_make_identifier(((Identifier *)place->data)->name,
                               place->resolved_type, place->location);
    break;
  case AST_MEMBER_ACCESS:
    copy = ast_create_member_access(
        own_clone_place(((MemberAccess *)place->data)->object),
        ((MemberAccess *)place->data)->member, place->location);
    break;
  case AST_UNARY_EXPRESSION:
    copy = ast_create_unary_expression(
        ((UnaryExpression *)place->data)->operator,
        own_clone_place(((UnaryExpression *)place->data)->operand),
        place->location);
    break;
  case AST_INDEX_EXPRESSION:
    copy = ast_create_array_index_expression(
        own_clone_place(((ArrayIndexExpression *)place->data)->array),
        own_clone_place(((ArrayIndexExpression *)place->data)->index),
        place->location);
    break;
  case AST_NUMBER_LITERAL:
    copy = ast_clone_node(place);
    break;
  default:
    return NULL;
  }
  if (copy) {
    copy->resolved_type = place->resolved_type;
  }
  return copy;
}

int own_places_equal(const ASTNode *a, const ASTNode *b) {
  if (!a || !b || a->type != b->type) {
    return a == b;
  }
  switch (a->type) {
  case AST_IDENTIFIER:
    return strcmp(((Identifier *)a->data)->name,
                  ((Identifier *)b->data)->name) == 0;
  case AST_MEMBER_ACCESS:
    return strcmp(((MemberAccess *)a->data)->member,
                  ((MemberAccess *)b->data)->member) == 0 &&
           own_places_equal(((MemberAccess *)a->data)->object,
                            ((MemberAccess *)b->data)->object);
  case AST_UNARY_EXPRESSION:
    return strcmp(((UnaryExpression *)a->data)->operator,
                  ((UnaryExpression *)b->data)->operator) == 0 &&
           own_places_equal(((UnaryExpression *)a->data)->operand,
                            ((UnaryExpression *)b->data)->operand);
  case AST_INDEX_EXPRESSION:
    return own_places_equal(((ArrayIndexExpression *)a->data)->array,
                            ((ArrayIndexExpression *)b->data)->array) &&
           own_places_equal(((ArrayIndexExpression *)a->data)->index,
                            ((ArrayIndexExpression *)b->data)->index);
  case AST_NUMBER_LITERAL:
    return ((NumberLiteral *)a->data)->int_value ==
               ((NumberLiteral *)b->data)->int_value &&
           ((NumberLiteral *)a->data)->is_float ==
               ((NumberLiteral *)b->data)->is_float;
  default:
    return 0;
  }
}

static size_t own_place_print(const ASTNode *place, char *out, size_t size,
                              size_t used) {
  if (!place || used + 1 >= size) {
    return used;
  }
  switch (place->type) {
  case AST_IDENTIFIER:
    return used + (size_t)snprintf(out + used, size - used, "%s",
                                   ((Identifier *)place->data)->name);
  case AST_MEMBER_ACCESS: {
    const MemberAccess *access = (const MemberAccess *)place->data;
    const ASTNode *object = access->object;
    if (object && object->type == AST_UNARY_EXPRESSION &&
        strcmp(((UnaryExpression *)object->data)->operator, "*") == 0) {
      used = own_place_print(((UnaryExpression *)object->data)->operand, out,
                             size, used);
      return used + (size_t)snprintf(out + used, size - used, "->%s",
                                     access->member);
    }
    used = own_place_print(object, out, size, used);
    return used +
           (size_t)snprintf(out + used, size - used, ".%s", access->member);
  }
  case AST_UNARY_EXPRESSION:
    used += (size_t)snprintf(out + used, size - used, "%s",
                             ((UnaryExpression *)place->data)->operator);
    return own_place_print(((UnaryExpression *)place->data)->operand, out,
                           size, used);
  case AST_INDEX_EXPRESSION:
    used = own_place_print(((ArrayIndexExpression *)place->data)->array, out,
                           size, used);
    used += (size_t)snprintf(out + used, size - used, "[");
    used = own_place_print(((ArrayIndexExpression *)place->data)->index, out,
                           size, used);
    return used + (size_t)snprintf(out + used, size - used, "]");
  case AST_NUMBER_LITERAL:
    return used + (size_t)snprintf(out + used, size - used, "%lld",
                                   ((NumberLiteral *)place->data)->int_value);
  case AST_FUNCTION_CALL:
    return used + (size_t)snprintf(out + used, size - used, "%s(...)",
                                   ((CallExpression *)place->data)
                                           ->function_name
                                       ? ((CallExpression *)place->data)
                                             ->function_name
                                       : "?");
  default:
    return used + (size_t)snprintf(out + used, size - used, "...");
  }
}

const char *own_place_text(const ASTNode *place, char *out, size_t size) {
  out[0] = '\0';
  own_place_print(place, out, size, 0);
  return out;
}

static char *own_temp_name(const char *prefix) {
  char name[64];
  snprintf(name, sizeof(name), "%s%d", prefix, ++g_own_temp_serial);
  return (char *)string_intern(name);
}

static ASTNode *own_make_var(const char *name, ASTNode *initializer) {
  ASTNode *decl = ast_create_var_declaration(name, NULL, initializer,
                                             initializer->location);
  if (decl && decl->data) {
    ((VarDeclaration *)decl->data)->type_is_inferred = 1;
  }
  return decl;
}

static ASTNode *own_make_drop(OwnWalker *w, ASTNode *place, SourceLocation at) {
  ASTNode *args[1] = {place};
  return own_make_call("drop", args, 1, own_void_type(w->checker), at);
}

static void own_record_inserted(OwnWalker *w, ASTNode *node,
                                OwnRecordKind kind, const char *subject,
                                struct Type *type, SourceLocation at,
                                SourceLocation origin, const char *text,
                                const char *reason) {
  ASTNode *drop_call = node;
  own_attach_note(node, kind, subject, type, at, origin, reason,
                  own_function_name(w));
  own_add_record(kind, at, origin, own_function_name(w), subject, type, text,
                 reason, 1, 0);
  if (node && node->type == AST_DEFER_STATEMENT && node->data) {
    drop_call = ((DeferStatement *)node->data)->statement;
  }
  if (drop_call && drop_call->type == AST_FUNCTION_CALL) {
    own_link_compiled(drop_call);
  }
}

static int own_insert_scope_drop(OwnWalker *w, OwnBinding *binding) {
  char reason[400];
  char text[300];
  ASTNode *ident = own_make_identifier(binding->name, binding->type,
                                       binding->location);
  ASTNode *drop = own_make_drop(w, ident, binding->location);
  ASTNode *defer = own_make_defer(drop, binding->location);
  long index = 0;
  OwnRecordKind kind = OWN_RECORD_SCOPE_DROP;
  if (!defer) {
    return 0;
  }
  snprintf(text, sizeof(text), "defer drop(%s);", binding->name);
  if (binding->kind == OWN_BIND_LOCAL) {
    index = own_block_index(binding->block, binding->declaration);
    if (index < 0) {
      return 0;
    }
    snprintf(reason, sizeof(reason),
             "`%s` owns a resource, so it is dropped when its scope ends, "
             "unless it has moved",
             binding->name);
    own_block_insert(binding->block, (size_t)index + 1, defer);
  } else if (binding->kind == OWN_BIND_PARAM) {
    Program *body = (Program *)binding->block->data;
    kind = OWN_RECORD_PARAM_DROP;
    index = 0;
    while (body && index < (long)body->declaration_count &&
           body->declarations[index]->type == AST_DEFER_STATEMENT &&
           ownership_node_is_inserted(body->declarations[index])) {
      index++;
    }
    snprintf(reason, sizeof(reason),
             "`%s` is passed by value, so %s owns it and drops it when it "
             "returns, unless it has moved",
             binding->name, own_function_name(w));
    own_block_insert(binding->block, (size_t)index, defer);
  } else {
    kind = OWN_RECORD_ARM_DROP;
    if (binding->block->type != AST_PROGRAM) {
      ASTNode *body = own_node_move_out(binding->block);
      ASTNode *wrapper = own_make_block(binding->block->location);
      own_block_insert(wrapper, 0, body);
      own_become(binding->block, wrapper);
    }
    snprintf(reason, sizeof(reason),
             "`%s` takes the payload out of the matched value, so this arm "
             "owns it and drops it when the arm ends, unless it has moved",
             binding->name);
    own_block_insert(binding->block, 0, defer);
  }
  own_record_inserted(w, defer, kind, binding->name, binding->type,
                      binding->location, binding->location, text, reason);
  return 1;
}

static int own_contains_call_or(const ASTNode *node, const char *name) {
  if (!node) {
    return 0;
  }
  if (node->type == AST_FUNCTION_CALL || node->type == AST_FUNC_PTR_CALL ||
      node->type == AST_VALUE_SCOPE || node->type == AST_NEW_EXPRESSION) {
    return 1;
  }
  if (name && node->type == AST_IDENTIFIER &&
      strcmp(((Identifier *)node->data)->name, name) == 0) {
    return 1;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (own_contains_call_or(node->children[i], name)) {
      return 1;
    }
  }
  return 0;
}

static const char *own_root_name(const ASTNode *place) {
  while (place) {
    if (place->type == AST_IDENTIFIER) {
      return ((Identifier *)place->data)->name;
    }
    if (place->type == AST_MEMBER_ACCESS) {
      place = ((MemberAccess *)place->data)->object;
    } else if (place->type == AST_UNARY_EXPRESSION) {
      place = ((UnaryExpression *)place->data)->operand;
    } else if (place->type == AST_INDEX_EXPRESSION) {
      place = ((ArrayIndexExpression *)place->data)->array;
    } else {
      return NULL;
    }
  }
  return NULL;
}

static ASTNode *own_assign_target_place(OwnWalker *w, OwnObligation *ob) {
  Assignment *assign = (Assignment *)ob->node->data;
  if (ob->root) {
    return own_clone_place(ob->root);
  }
  if (assign && assign->variable_name && ob->binding >= 0) {
    return own_make_identifier(assign->variable_name,
                               w->bindings[ob->binding].type,
                               ob->node->location);
  }
  return NULL;
}

static void own_wrap_in_block(ASTNode *statement, ASTNode **prefix,
                              size_t count) {
  ASTNode *original = own_node_move_out(statement);
  ASTNode *block = own_make_block(statement->location);
  for (size_t i = 0; i < count; i++) {
    own_block_insert(block, i, prefix[i]);
  }
  own_block_insert(block, count, original);
  own_become(statement, block);
}

static int own_insert_assign_drop(OwnWalker *w, OwnObligation *ob) {
  Assignment *assign = (Assignment *)ob->node->data;
  ASTNode *place = own_assign_target_place(w, ob);
  char text[300];
  char subject[200];
  char reason[400];
  const char *root = place ? own_root_name(place) : NULL;
  ASTNode *drop = NULL;
  if (!place || !assign) {
    return 0;
  }
  own_place_text(place, subject, sizeof(subject));
  drop = own_make_drop(w, place, ob->node->location);
  snprintf(text, sizeof(text), "drop(%s);", subject);
  snprintf(reason, sizeof(reason),
           "the value `%s` held is dropped before the assignment replaces "
           "it",
           subject);
  own_record_inserted(w, drop, OWN_RECORD_ASSIGN_DROP, subject,
                      place->resolved_type, ob->node->location,
                      ob->node->location, text, reason);
  if (own_contains_call_or(assign->value, root)) {
    char *name = own_temp_name("__next");
    ASTNode *value = own_node_move_out(assign->value);
    ASTNode *var = own_make_var(name, value);
    ASTNode *prefix[2] = {var, drop};
    own_become_identifier(assign->value, name, value->resolved_type);
    own_wrap_in_block(ob->node, prefix, 2);
    return 1;
  }
  if (ob->block && own_block_index(ob->block, ob->node) >= 0) {
    own_block_insert(ob->block, (size_t)own_block_index(ob->block, ob->node),
                     drop);
    return 1;
  }
  ASTNode *prefix[1] = {drop};
  own_wrap_in_block(ob->node, prefix, 1);
  return 1;
}

static int own_insert_discard_drop(OwnWalker *w, OwnObligation *ob) {
  char subject[200];
  char text[300];
  char reason[400];
  ASTNode *value = own_node_move_out(ob->node);
  ASTNode *args[1] = {value};
  ASTNode *call = own_make_call("drop", args, 1, own_void_type(w->checker),
                                ob->node->location);
  if (!value || !call) {
    return 0;
  }
  own_place_text(value, subject, sizeof(subject));
  own_become(ob->node, call);
  snprintf(text, sizeof(text), "drop(%s);", subject);
  snprintf(reason, sizeof(reason),
           "the value %s returns is not kept, so it is dropped at once",
           subject);
  own_record_inserted(w, ob->node, OWN_RECORD_DISCARD_DROP, subject,
                      value->resolved_type, ob->node->location,
                      ob->node->location, text, reason);
  return 1;
}

static size_t own_eval_children(ASTNode *node, ASTNode **out, size_t max) {
  size_t count = 0;
  if (!node) {
    return 0;
  }
  switch (node->type) {
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)node->data;
    if (call->object && count < max) {
      out[count++] = call->object;
    }
    for (size_t i = 0; i < call->argument_count && count < max; i++) {
      out[count++] = call->arguments[i];
    }
    return count;
  }
  case AST_BINARY_EXPRESSION:
    out[count++] = ((BinaryExpression *)node->data)->left;
    out[count++] = ((BinaryExpression *)node->data)->right;
    return count;
  case AST_INDEX_EXPRESSION:
    out[count++] = ((ArrayIndexExpression *)node->data)->array;
    out[count++] = ((ArrayIndexExpression *)node->data)->index;
    return count;
  case AST_AGGREGATE_LITERAL: {
    AggregateLiteral *literal = (AggregateLiteral *)node->data;
    for (size_t i = 0; i < literal->element_count && count < max; i++) {
      out[count++] = literal->elements[i];
    }
    return count;
  }
  default:
    for (size_t i = 0; i < node->child_count && count < max; i++) {
      out[count++] = node->children[i];
    }
    return count;
  }
}

static int own_find_path(ASTNode *from, ASTNode *target, ASTNode **path,
                         size_t *depth, size_t max) {
  ASTNode *children[64];
  size_t count = 0;
  if (*depth >= max) {
    return 0;
  }
  path[(*depth)++] = from;
  if (from == target) {
    return 1;
  }
  count = own_eval_children(from, children, 64);
  for (size_t i = 0; i < count; i++) {
    if (children[i] && own_find_path(children[i], target, path, depth, max)) {
      return 1;
    }
  }
  (*depth)--;
  return 0;
}

static int own_is_trivial(const ASTNode *node) {
  if (!node) {
    return 1;
  }
  if (node->type == AST_NUMBER_LITERAL || node->type == AST_STRING_LITERAL ||
      node->type == AST_IDENTIFIER) {
    return 1;
  }
  if (node->type == AST_UNARY_EXPRESSION &&
      strcmp(((UnaryExpression *)node->data)->operator, "&") == 0) {
    return own_is_trivial(((UnaryExpression *)node->data)->operand);
  }
  if (node->type == AST_MEMBER_ACCESS) {
    return own_is_trivial(((MemberAccess *)node->data)->object);
  }
  return 0;
}

static size_t own_collect_hoists(ASTNode *root, ASTNode *target,
                                 ASTNode **hoist, size_t max) {
  ASTNode *path[256];
  size_t depth = 0;
  size_t count = 0;
  if (!own_find_path(root, target, path, &depth, 256)) {
    return 0;
  }
  for (size_t level = 0; level + 1 < depth; level++) {
    ASTNode *children[64];
    size_t n = own_eval_children(path[level], children, 64);
    for (size_t i = 0; i < n && children[i] != path[level + 1]; i++) {
      if (!own_is_trivial(children[i]) && children[i]->resolved_type &&
          children[i]->resolved_type->kind != TYPE_VOID && count < max) {
        hoist[count++] = children[i];
      }
    }
  }
  return count;
}

static ASTNode *own_make_value_scope(ASTNode **statements, size_t count,
                                     ASTNode *tail, struct Type *type,
                                     SourceLocation at) {
  int scope_id = g_own_scope_serial++;
  ASTNode *body = own_make_block(at);
  ASTNode *yield = ast_create_yield_statement(tail, scope_id, at);
  ASTNode *scope = NULL;
  for (size_t i = 0; i < count; i++) {
    own_block_insert(body, i, statements[i]);
  }
  own_block_insert(body, count, yield);
  scope = ast_create_value_scope(body, scope_id, VALUE_SCOPE_BLOCK, NULL, at);
  if (scope) {
    scope->resolved_type = type;
  }
  return scope;
}

static int own_name_temporary(OwnWalker *w, OwnObligation *ob) {
  ASTNode *root = ob->root;
  ASTNode *hoist[16];
  ASTNode *prefix[17];
  size_t hoists = 0;
  char *name = own_temp_name("__tmp");
  char subject[200];
  char text[300];
  char reason[400];
  if (!root) {
    return 0;
  }
  own_place_text(ob->node, subject, sizeof(subject));
  hoists = own_collect_hoists(root, ob->node, hoist, 16);
  for (size_t i = 0; i < hoists; i++) {
    char *held = own_temp_name("__arg");
    ASTNode *value = own_node_move_out(hoist[i]);
    prefix[i] = own_make_var(held, value);
    own_become_identifier(hoist[i], held, value->resolved_type);
  }
  ASTNode *value = own_node_move_out(ob->node);
  prefix[hoists] = own_make_var(name, value);
  own_become_identifier(ob->node, name, value->resolved_type);
  snprintf(text, sizeof(text), "var %s = %s;", name, subject);
  snprintf(reason, sizeof(reason),
           "the value %s returns is not kept, so it is held in `%s` until "
           "the statement ends and dropped there",
           subject, name);
  own_record_inserted(w, prefix[hoists], OWN_RECORD_TEMPORARY, subject,
                      value->resolved_type, ob->node->location,
                      ob->node->location, text, reason);
  if (ob->root_is_statement || !root->resolved_type ||
      root->resolved_type->kind == TYPE_VOID) {
    own_wrap_in_block(root, prefix, hoists + 1);
    return 1;
  }
  ASTNode *inner = own_node_move_out(root);
  ASTNode *scope = own_make_value_scope(prefix, hoists + 1, inner,
                                        inner->resolved_type, root->location);
  own_become(root, scope);
  return 1;
}

static int own_name_scrutinee(OwnWalker *w, OwnObligation *ob) {
  MatchStatement *match = (MatchStatement *)ob->root->data;
  char *name = own_temp_name("__matched");
  char subject[200];
  char text[300];
  char reason[400];
  ASTNode *value = own_node_move_out(match->expression);
  ASTNode *var = own_make_var(name, value);
  ASTNode *prefix[1] = {var};
  own_place_text(value, subject, sizeof(subject));
  own_become_identifier(match->expression, name, value->resolved_type);
  snprintf(text, sizeof(text), "var %s = %s;", name, subject);
  snprintf(reason, sizeof(reason),
           "the value matched on is held in `%s` so the arms can take its "
           "payload; what no arm takes is dropped when the match ends",
           name);
  own_record_inserted(w, var, OWN_RECORD_TEMPORARY, subject,
                      value->resolved_type, ob->root->location,
                      ob->root->location, text, reason);
  own_wrap_in_block(ob->root, prefix, 1);
  return 1;
}

static int own_touched(ASTNode **touched, size_t count, const ASTNode *node) {
  for (size_t i = 0; i < count; i++) {
    if (touched[i] == node) {
      return 1;
    }
  }
  return 0;
}

static int own_apply_structural(OwnWalker *w, OwnObligation *ob,
                                ASTNode **touched, size_t *touched_count) {
  ASTNode *key = ob->root ? ob->root : ob->node;
  if (own_touched(touched, *touched_count, key) ||
      own_touched(touched, *touched_count, ob->statement)) {
    return 0;
  }
  if (*touched_count < 256) {
    touched[(*touched_count)++] = key;
  }
  if (ob->statement && *touched_count < 256) {
    touched[(*touched_count)++] = ob->statement;
  }
  switch (ob->kind) {
  case OWN_OB_DISCARD:
    return own_insert_discard_drop(w, ob);
  case OWN_OB_TEMPORARY:
    return own_name_temporary(w, ob);
  case OWN_OB_SCRUTINEE:
    return own_name_scrutinee(w, ob);
  case OWN_OB_ASSIGN_DROP:
    return own_insert_assign_drop(w, ob);
  default:
    return 0;
  }
}

int own_apply_obligations(OwnWalker *w) {
  ASTNode *touched[256];
  size_t touched_count = 0;
  int applied = 0;
  for (size_t i = 0; i < w->obligation_count; i++) {
    OwnObligation *ob = &w->obligations[i];
    if (ob->kind != OWN_OB_SCOPE_DROP) {
      applied += own_apply_structural(w, ob, touched, &touched_count);
    }
  }
  if (applied > 0) {
    return applied;
  }
  for (size_t i = 0; i < w->obligation_count; i++) {
    OwnObligation *ob = &w->obligations[i];
    if (ob->kind == OWN_OB_SCOPE_DROP && ob->binding >= 0 &&
        !w->bindings[ob->binding].needs_drop) {
      w->bindings[ob->binding].needs_drop = 1;
      applied += own_insert_scope_drop(w, &w->bindings[ob->binding]);
    }
  }
  return applied;
}

static SourceLocation own_first_statement(ASTNode *block) {
  Program *program = block && block->type == AST_PROGRAM
                         ? (Program *)block->data
                         : NULL;
  SourceLocation none = {0, 0, NULL};
  if (program && program->declaration_count > 0 &&
      program->declarations[0]) {
    return program->declarations[0]->location;
  }
  return block ? block->location : none;
}

static void own_report_explicit(OwnWalker *w, OwnObligation *ob,
                                const char *code, const char *message,
                                const char *help, OwnFixKind fix,
                                SourceLocation at, const char *text,
                                ASTNode *node) {
  error_reporter_add_error_with_suggestion(w->reporter, ERROR_SEMANTIC, at,
                                           message, help);
  error_reporter_set_last_code(w->reporter, code);
  if (text || node) {
    own_add_fix(fix, at, text, node);
  }
  (void)ob;
  w->errors++;
}

static void own_explicit_one(OwnWalker *w, OwnObligation *ob) {
  char message[600];
  char help[600];
  char subject[200];
  char text[300];
  switch (ob->kind) {
  case OWN_OB_SCOPE_DROP: {
    OwnBinding *binding = &w->bindings[ob->binding];
    if (binding->reported) {
      return;
    }
    binding->reported = 1;
    if (ob->exit_at.line == 0) {
      ob->exit_at = binding->location;
    }
    snprintf(text, sizeof(text), "defer drop(%s);", binding->name);
    snprintf(message, sizeof(message),
             "`%s` still owns a resource when its scope ends here, and "
             "`@explicit` code drops nothing on its own",
             binding->name);
    if (binding->kind == OWN_BIND_LOCAL) {
      snprintf(help, sizeof(help),
               "write `%s` after line %zu, or drop or move `%s` on every "
               "path; --fix writes it",
               text, binding->location.line, binding->name);
      own_report_explicit(w, ob, "O0020", message, help, OWN_FIX_INSERT_AFTER,
                          ob->exit_at, NULL, NULL);
      own_add_fix(OWN_FIX_INSERT_AFTER, binding->declaration->location, text,
                  NULL);
      return;
    }
    snprintf(help, sizeof(help),
             "write `%s` as the first statement of the %s; --fix writes it",
             text, binding->kind == OWN_BIND_PARAM ? "function" : "arm");
    own_report_explicit(w, ob, "O0020", message, help, OWN_FIX_INSERT_BEFORE,
                        ob->exit_at, NULL, NULL);
    own_add_fix(OWN_FIX_INSERT_BEFORE, own_first_statement(binding->block),
                text, NULL);
    return;
  }
  case OWN_OB_ASSIGN_DROP: {
    ASTNode *place = own_assign_target_place(w, ob);
    Assignment *assign = (Assignment *)ob->node->data;
    own_place_text(place, subject, sizeof(subject));
    snprintf(message, sizeof(message),
             "this assignment replaces the value in `%s` without dropping "
             "it, and `@explicit` code drops nothing on its own",
             subject);
    if (assign && own_contains_call_or(assign->value, own_root_name(place))) {
      char *holder = own_temp_name("next_");
      snprintf(help, sizeof(help),
               "compute the new value first, then `drop(%s);`, then assign: "
               "the order implicit code uses; --fix writes it",
               subject);
      own_report_explicit(w, ob, "O0021", message, help, OWN_FIX_REPLACE,
                          ob->node->location, NULL, NULL);
      own_add_statement_fix(OWN_FIX_ASSIGN_HOLD, ob->node->location,
                            ob->node->location, 0, holder);
      return;
    }
    snprintf(text, sizeof(text), "drop(%s);", subject);
    snprintf(help, sizeof(help),
             "write `%s` before it, or move the old value out first; --fix "
             "writes it",
             text);
    own_report_explicit(w, ob, "O0021", message, help, OWN_FIX_INSERT_BEFORE,
                        ob->node->location, text, NULL);
    return;
  }
  case OWN_OB_DISCARD:
    own_place_text(ob->node, subject, sizeof(subject));
    snprintf(message, sizeof(message),
             "the value %s returns owns a resource and is thrown away, and "
             "`@explicit` code drops nothing on its own",
             subject);
    snprintf(help, sizeof(help),
             "write `drop(%s);`, or keep the value; --fix writes the drop",
             subject);
    own_report_explicit(w, ob, "O0022", message, help, OWN_FIX_WRAP_DROP,
                        ob->node->location, "", NULL);
    return;
  default: {
    int statement_level = ob->kind == OWN_OB_TEMPORARY && ob->statement &&
                          ob->node->type == AST_FUNCTION_CALL;
    char *holder = own_temp_name("held_");
    own_place_text(ob->node, subject, sizeof(subject));
    snprintf(message, sizeof(message),
             "the value %s returns owns a resource and is used without a "
             "name, and `@explicit` code drops nothing on its own",
             subject);
    snprintf(help, sizeof(help),
             "put it in a variable first, as `var %s = %s;`, and drop it "
             "when the statement is done%s",
             holder, subject, statement_level ? "; --fix writes it" : "");
    own_report_explicit(w, ob, "O0023", message, help, OWN_FIX_REPLACE,
                        ob->node->location, NULL, NULL);
    if (statement_level) {
      own_add_statement_fix(OWN_FIX_TEMPORARY, ob->node->location,
                            ob->statement->location,
                            ob->statement->type == AST_RETURN_STATEMENT,
                            holder);
    }
    return;
  }
  }
}

int own_report_obligations(OwnWalker *w) {
  int reported = 0;
  for (size_t i = 0; i < w->obligation_count; i++) {
    if (w->is_explicit) {
      own_explicit_one(w, &w->obligations[i]);
      reported++;
      continue;
    }
    SourceLocation at = w->obligations[i].exit_at;
    if (at.line == 0 && w->obligations[i].binding >= 0) {
      at = w->bindings[w->obligations[i].binding].location;
    }
    if (at.line == 0) {
      at = w->function_node->location;
    }
    error_reporter_add_error(
        w->reporter, ERROR_INTERNAL, at,
        "internal error: the drops the compiler inserted in this function "
        "do not account for every owned value; this is a compiler bug");
    error_reporter_set_last_code(w->reporter, "O0099");
    w->errors++;
    reported++;
  }
  return reported;
}
