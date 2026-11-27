#include "translate/ctr_emit.h"

#include <string.h>

typedef void (*NodeVisit)(CtrNode *node, void *arg);

static void walk(CtrNode *node, NodeVisit visit, void *arg);

static void walk_init(CtrInit *init, NodeVisit visit, void *arg) {
  if (!init) {
    return;
  }
  if (init->expr) {
    walk(init->expr, visit, arg);
  }
  if (!init->children) {
    return;
  }
  int n = 0;
  if (init->ty->kind == TY_ARRAY) {
    n = (int)init->ty->array_len;
  } else if (init->ty->sdecl) {
    for (CtrMember *m = init->ty->sdecl->members; m; m = m->next) {
      n++;
    }
  }
  for (int i = 0; i < n; i++) {
    walk_init(init->children[i], visit, arg);
  }
}

static void walk_list(CtrNode *n, NodeVisit visit, void *arg) {
  for (; n; n = n->next) {
    walk(n, visit, arg);
  }
}

static void walk(CtrNode *node, NodeVisit visit, void *arg) {
  if (!node) {
    return;
  }
  visit(node, arg);
  walk(node->lhs, visit, arg);
  walk(node->rhs, visit, arg);
  walk(node->cond, visit, arg);
  if (!(node->kind == ND_COND && node->op == 1)) {
    walk(node->then, visit, arg);
  }
  walk(node->els, visit, arg);
  if (node->kind == ND_FOR) {
    walk_list(node->init, visit, arg);
  } else {
    walk(node->init, visit, arg);
  }
  walk(node->inc, visit, arg);
  walk_list(node->body, visit, arg);
  walk_list(node->args, visit, arg);
  walk_init(node->decl_init, visit, arg);
}

static int user_macro(CtrMacro *m) {
  return m && m->is_objlike && !m->builtin && m->file && !m->file->is_system && !m->rejected &&
         strcmp(ctr_safe_ident(m->name), m->name) == 0;
}

static int is_string_value(CtrNode *n) { return n->kind == ND_STR && n->str_ty->base->size == 1; }

static int same_value(CtrMacro *m, CtrNode *inner) {
  CtrNode *prev = m->value_expr;
  if (is_string_value(prev) || is_string_value(inner)) {
    return is_string_value(prev) && is_string_value(inner) && prev->str_len == inner->str_len &&
           memcmp(prev->str, inner->str, (size_t)prev->str_len) == 0;
  }
  if (!ctr_same_type(prev->ty, inner->ty)) {
    return 0;
  }
  int64_t a, b;
  if (ctr_try_eval(prev, &a) && ctr_try_eval(inner, &b)) {
    return a == b;
  }
  if (ctr_is_flonum(prev->ty) && ctr_is_const_expr(prev) && ctr_is_const_expr(inner)) {
    return ctr_eval_double(prev) == ctr_eval_double(inner);
  }
  return 0;
}

static int constant_value(CtrNode *inner) {
  int64_t v;
  if (is_string_value(inner)) {
    return 1;
  }
  if (inner->ty && ctr_is_integer(inner->ty) && ctr_try_eval(inner, &v)) {
    return inner->ty->kind != TY_BOOL;
  }
  return inner->ty && ctr_is_flonum(inner->ty) && ctr_is_const_expr(inner);
}

static void visit_macro(CtrNode *node, void *arg) {
  (void)arg;
  if (node->kind != ND_MACRO || !node->macro) {
    return;
  }
  CtrMacro *m = node->macro->macro;
  if (!user_macro(m)) {
    return;
  }
  CtrNode *inner = node->lhs;
  while (inner->kind == ND_MACRO) {
    inner = inner->lhs;
  }
  if (!constant_value(inner)) {
    m->rejected = 1;
    return;
  }
  if (m->value_expr && !same_value(m, inner)) {
    m->rejected = 1;
    return;
  }
  m->value_expr = inner;
  m->value_type = inner->ty;
}

static void scan_globals(CtrObj *globals) {
  for (CtrObj *g = globals; g; g = g->next) {
    if (g->is_function && g->body) {
      walk(g->body, visit_macro, NULL);
    } else if (g->init) {
      walk_init(g->init, visit_macro, NULL);
    }
  }
}

void ctr_claim_macro_names(CtrProgram *prog);

void ctr_recover_macros(CtrProgram *prog) {
  if (prog->options->expand_macros) {
    return;
  }
  for (int u = 0; u < prog->units.len; u++) {
    CtrUnit *unit = prog->units.items[u];
    scan_globals(unit->globals);
  }
  ctr_claim_macro_names(prog);
}

static int needs_runtime_init(CtrObj *g) {
  if (g->is_function || !g->init || g->mettle_module || g->is_system) {
    return 0;
  }
  int ok = 1;
  ctr_init_literal(g->init, g->ty, &ok);
  return !ok;
}

int ctr_needs_global_init(CtrObj *globals) {
  for (CtrObj *g = globals; g; g = g->next) {
    if (needs_runtime_init(g)) {
      return 1;
    }
  }
  return 0;
}

static void visit_goto(CtrNode *node, void *arg) {
  int *found = arg;
  if (node->kind == ND_GOTO || node->kind == ND_GOTO_EXPR || node->kind == ND_LABEL_VAL) {
    *found = 1;
  }
  if (node->kind == ND_SWITCH && node->irregular) {
    *found = 1;
  }
}

int ctr_needs_cfg(CtrObj *fn) {
  if (!fn->has_goto && !fn->has_irregular_case) {
    return 0;
  }
  int found = 0;
  walk(fn->body, visit_goto, &found);
  return found;
}
