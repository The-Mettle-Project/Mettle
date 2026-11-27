#include "translate/ctr_emit.h"

#include <string.h>

typedef void (*Visit)(CtrNode *n, void *arg);

typedef struct {
  CtrNode **items;
  int len;
} List;

static void visit_tree(CtrNode *n, Visit fn, void *arg);

static void visit_list(CtrNode *n, Visit fn, void *arg) {
  for (; n; n = n->next) {
    visit_tree(n, fn, arg);
  }
}

static void visit_tree(CtrNode *n, Visit fn, void *arg) {
  if (!n) {
    return;
  }
  fn(n, arg);
  visit_tree(n->lhs, fn, arg);
  visit_tree(n->rhs, fn, arg);
  visit_tree(n->cond, fn, arg);
  if (!(n->kind == ND_COND && n->op == 1)) {
    visit_tree(n->then, fn, arg);
  }
  visit_tree(n->els, fn, arg);
  if (n->kind == ND_FOR) {
    visit_list(n->init, fn, arg);
    visit_tree(n->inc, fn, arg);
  } else if (n->kind != ND_GOTO) {
    visit_tree(n->init, fn, arg);
    visit_tree(n->inc, fn, arg);
  }
  visit_list(n->body, fn, arg);
  visit_list(n->args, fn, arg);
}

typedef struct {
  const char *label;
  int count;
} GotoCount;

static void count_visit(CtrNode *n, void *arg) {
  GotoCount *c = arg;
  if (n->kind == ND_GOTO && strcmp(n->label, c->label) == 0) {
    c->count++;
  }
}

static int gotos_to(CtrNode *n, const char *label) {
  GotoCount c = {label, 0};
  visit_tree(n, count_visit, &c);
  return c.count;
}

static void computed_visit(CtrNode *n, void *arg) {
  if (n->kind == ND_GOTO_EXPR || n->kind == ND_LABEL_VAL) {
    *(int *)arg = 1;
  }
}

typedef struct {
  const char *label;
  CtrNodeKind kind;
  CtrNode *loop;
} Retarget;

static void retarget_visit(CtrNode *n, void *arg) {
  Retarget *r = arg;
  if (n->kind == ND_GOTO && strcmp(n->label, r->label) == 0) {
    n->kind = r->kind;
    n->target = r->loop;
    n->inc = NULL;
    n->label = NULL;
  }
}

typedef struct {
  CtrVec switches;
  int escapes;
} SwitchScan;

static void switch_visit(CtrNode *n, void *arg) {
  if (n->kind == ND_SWITCH) {
    ctr_vec_push(&((SwitchScan *)arg)->switches, n);
  }
}

static void escape_visit(CtrNode *n, void *arg) {
  SwitchScan *s = arg;
  if (n->kind != ND_BREAK || !n->target || n->target->kind != ND_SWITCH) {
    return;
  }
  for (int i = 0; i < s->switches.len; i++) {
    if (s->switches.items[i] == n->target) {
      return;
    }
  }
  s->escapes = 1;
}

static CtrNode *unlabeled(CtrNode *n) {
  while (n && n->kind == ND_LABEL) {
    n = n->lhs;
  }
  return n;
}

static int range_ok(List *l, int a, int b) {
  SwitchScan scan = {{0}, 0};
  for (int k = a; k <= b; k++) {
    CtrNode *n = l->items[k];
    CtrNode *s = unlabeled(n);
    if (n->kind == ND_CASE || (s && s->kind == ND_CASE)) {
      return 0;
    }
    if (n->kind == ND_DECL && n->var->ty->kind == TY_VLA) {
      return 0;
    }
    visit_tree(n, switch_visit, &scan);
  }
  for (int k = a; k <= b; k++) {
    visit_tree(l->items[k], escape_visit, &scan);
  }
  return !scan.escapes;
}

typedef struct {
  CtrObj *var;
  int found;
} VarUse;

static void use_visit(CtrNode *n, void *arg) {
  VarUse *u = arg;
  if ((n->kind == ND_VAR || n->kind == ND_DECL) && n->var == u->var) {
    u->found = 1;
  }
}

static int used_after(List *l, int from, CtrObj *var) {
  VarUse u = {var, 0};
  for (int k = from; k < l->len && !u.found; k++) {
    visit_tree(l->items[k], use_visit, &u);
  }
  return u.found;
}

static void push_node(List *out, CtrNode *n) { out->items[out->len++] = n; }

static void hoist_decl(CtrNode *d, List *l, int after, List *out) {
  if (d->kind != ND_DECL || !used_after(l, after, d->var)) {
    return;
  }
  CtrNode *h = ctr_new_node(ND_DECL, d->tok);
  h->var = d->var;
  d->as_assign = 1;
  push_node(out, h);
}

static void hoist_decls(List *l, int a, int b, List *out) {
  for (int k = a; k <= b; k++) {
    CtrNode *n = l->items[k];
    if (n->kind == ND_BLOCK && n->op == 1) {
      for (CtrNode *d = n->body; d; d = d->next) {
        hoist_decl(d, l, b + 1, out);
      }
    } else {
      hoist_decl(n, l, b + 1, out);
    }
  }
}

static CtrNode *wrap(List *l, int a, int b, const char *label) {
  CtrToken *tok = l->items[a]->tok;
  CtrNode *loop = ctr_new_node(ND_WHILE, tok);
  CtrNode *cond = ctr_new_node(ND_NUM, tok);
  cond->ival = 1;
  cond->ty = ctr_ty_bool;
  CtrNode *body = ctr_new_node(ND_BLOCK, tok);
  CtrNode *exit = ctr_new_node(ND_BREAK, tok);
  exit->target = loop;
  CtrNode head = {0};
  CtrNode *cur = &head;
  for (int k = a; k <= b; k++) {
    cur = cur->next = l->items[k];
  }
  cur->next = NULL;
  body->body = head.next;
  if (ctr_body_falls_through(body)) {
    cur->next = exit;
  }
  loop->cond = cond;
  loop->then = body;
  loop->label = label;
  loop->has_break = 1;
  return loop;
}

static void retarget(List *l, int a, int b, const char *label, CtrNodeKind kind, CtrNode *loop) {
  Retarget r = {label, kind, loop};
  for (int k = a; k <= b; k++) {
    visit_tree(l->items[k], retarget_visit, &r);
  }
}

static int decl_count(List *l) {
  int n = 0;
  for (int k = 0; k < l->len; k++) {
    CtrNode *s = l->items[k];
    n += s->kind == ND_DECL;
    if (s->kind == ND_BLOCK && s->op == 1) {
      for (CtrNode *d = s->body; d; d = d->next) {
        n++;
      }
    }
  }
  return n;
}

static int try_label(List *l, int j, CtrObj *fn) {
  const char *name = l->items[j]->label;
  int total = gotos_to(fn->body, name);
  int first = -1, last = -1, inside = 0;
  for (int k = 0; k < l->len; k++) {
    int c = gotos_to(l->items[k], name);
    inside += c;
    if (c && k < j && first < 0) {
      first = k;
    }
    if (c && k >= j) {
      last = k;
    }
  }
  if (total == 0 || inside != total) {
    return 0;
  }
  if ((first >= 0 && !range_ok(l, first, j - 1)) || (last >= 0 && !range_ok(l, j, last))) {
    return 0;
  }
  List out = {ctr_alloc(sizeof(CtrNode *) * (size_t)(l->len + decl_count(l) + 4)), 0};
  int k = 0;
  for (; k < (first >= 0 ? first : j); k++) {
    push_node(&out, l->items[k]);
  }
  if (first >= 0) {
    hoist_decls(l, first, j - 1, &out);
    CtrNode *ahead = wrap(l, first, j - 1, last >= 0 ? ctr_format("to_%s", name) : name);
    retarget(l, first, j - 1, name, ND_BREAK, ahead);
    push_node(&out, ahead);
    k = j;
  }
  if (last >= 0) {
    hoist_decls(l, j, last, &out);
    CtrNode *again = wrap(l, j, last, name);
    again->has_continue = 1;
    retarget(l, j, last, name, ND_CONTINUE, again);
    push_node(&out, again);
    k = last + 1;
  }
  for (; k < l->len; k++) {
    push_node(&out, l->items[k]);
  }
  *l = out;
  return 1;
}

static List to_list(CtrNode *head) {
  int n = 0;
  for (CtrNode *c = head; c; c = c->next) {
    n++;
  }
  List l = {ctr_alloc(sizeof(CtrNode *) * (size_t)(n + 1)), 0};
  for (CtrNode *c = head; c; c = c->next) {
    l.items[l.len++] = c;
  }
  return l;
}

static CtrNode *relink(List *l) {
  for (int k = 0; k < l->len; k++) {
    l->items[k]->next = k + 1 < l->len ? l->items[k + 1] : NULL;
  }
  return l->len ? l->items[0] : NULL;
}

static void structure_stmt(CtrNode *n, CtrObj *fn, CtrMap *tried);

static void structure_list(CtrNode **head, CtrObj *fn, CtrMap *tried) {
  for (int changed = 1; changed;) {
    changed = 0;
    List l = to_list(*head);
    for (int j = 0; j < l.len && !changed; j++) {
      CtrNode *lab = l.items[j];
      if (lab->kind != ND_LABEL || ctr_map_get(tried, lab->label)) {
        continue;
      }
      ctr_map_put(tried, lab->label, lab);
      changed = try_label(&l, j, fn);
    }
    if (changed) {
      *head = relink(&l);
    }
  }
  for (CtrNode *n = *head; n; n = n->next) {
    structure_stmt(n, fn, tried);
  }
}

static void structure_stmt(CtrNode *n, CtrObj *fn, CtrMap *tried) {
  if (!n) {
    return;
  }
  switch (n->kind) {
  case ND_BLOCK:
    structure_list(&n->body, fn, tried);
    return;
  case ND_IF:
    structure_stmt(n->then, fn, tried);
    structure_stmt(n->els, fn, tried);
    return;
  case ND_WHILE:
  case ND_DO:
  case ND_FOR:
  case ND_SWITCH:
    structure_stmt(n->then, fn, tried);
    return;
  case ND_LABEL:
  case ND_CASE:
    structure_stmt(n->lhs, fn, tried);
    return;
  default:
    return;
  }
}

void ctr_structure_gotos(CtrObj *fn) {
  if (!fn->has_goto || !fn->body) {
    return;
  }
  int computed = 0;
  visit_tree(fn->body, computed_visit, &computed);
  if (computed) {
    return;
  }
  CtrMap tried = {0};
  structure_stmt(fn->body, fn, &tried);
}
