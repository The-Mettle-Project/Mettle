#include "translate/ctr_emit.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  char *set;
  int n;
} Assigned;

static Assigned copy_state(const Assigned *s) {
  Assigned c = {ctr_alloc((size_t)s->n + 1), s->n};
  memcpy(c.set, s->set, (size_t)s->n);
  return c;
}

static void intersect(Assigned *into, const Assigned *a, const Assigned *b) {
  for (int i = 0; i < into->n; i++) {
    into->set[i] = a->set[i] && b->set[i];
  }
}

static void fill(Assigned *s) { memset(s->set, 1, (size_t)s->n); }

static int tracked(CtrObj *v) {
  return v && v->is_local && !v->is_param && v->local_index > 0 && !v->is_static;
}

static void mark_set(Assigned *s, CtrObj *v) {
  if (tracked(v)) {
    s->set[v->local_index - 1] = 1;
  }
}

static void read_var(Assigned *s, CtrObj *v) {
  if (tracked(v) && !s->set[v->local_index - 1]) {
    v->needs_zero = 1;
    s->set[v->local_index - 1] = 1;
  }
}

static CtrNode *strip(CtrNode *n) {
  while (n && (n->kind == ND_MACRO || (n->kind == ND_CAST && n->implicit && n->lhs->kind == ND_VAR))) {
    n = n->lhs;
  }
  return n;
}

static void expr(CtrNode *n, Assigned *s);
static void stmt(CtrNode *n, Assigned *s);

static void expr_list(CtrNode *n, Assigned *s) {
  for (; n; n = n->next) {
    expr(n, s);
  }
}

static void init_tree(CtrInit *init, Assigned *s) {
  if (!init) {
    return;
  }
  if (init->expr) {
    expr(init->expr, s);
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
    init_tree(init->children[i], s);
  }
}

static void assign_expr(CtrNode *n, Assigned *s) {
  CtrNode *lhs = strip(n->lhs);
  expr(n->rhs, s);
  if (lhs->kind == ND_VAR) {
    if (n->kind == ND_OPASSIGN) {
      read_var(s, lhs->var);
    }
    mark_set(s, lhs->var);
    return;
  }
  expr(n->lhs, s);
}

static void branch_expr(CtrNode *n, Assigned *s) {
  expr(n->cond, s);
  Assigned a = copy_state(s), b = copy_state(s);
  expr(n->then, &a);
  expr(n->els, &b);
  intersect(s, &a, &b);
}

static void short_circuit(CtrNode *n, Assigned *s) {
  expr(n->lhs, s);
  Assigned r = copy_state(s);
  expr(n->rhs, &r);
}

static void expr(CtrNode *n, Assigned *s) {
  if (!n) {
    return;
  }
  switch (n->kind) {
  case ND_VAR:
    read_var(s, n->var);
    return;
  case ND_ASSIGN:
  case ND_OPASSIGN:
    assign_expr(n, s);
    return;
  case ND_ADDR: {
    CtrNode *in = strip(n->lhs);
    if (in->kind == ND_VAR) {
      mark_set(s, in->var);
      return;
    }
    expr(n->lhs, s);
    return;
  }
  case ND_COND:
    if (n->op == 1) {
      expr(n->cond, s);
      Assigned b = copy_state(s);
      expr(n->els, &b);
      return;
    }
    branch_expr(n, s);
    return;
  case ND_LOGAND:
  case ND_LOGOR:
    short_circuit(n, s);
    return;
  case ND_STMT_EXPR:
    for (CtrNode *b = n->body; b; b = b->next) {
      stmt(b, s);
    }
    return;
  case ND_COMPOUND_LIT:
    init_tree(n->decl_init, s);
    mark_set(s, n->var);
    return;
  case ND_SIZEOF:
  case ND_ALIGNOF:
    return;
  case ND_VA_START:
  case ND_VA_COPY: {
    CtrNode *dest = strip(n->lhs);
    expr(n->rhs, s);
    if (dest->kind == ND_VAR) {
      mark_set(s, dest->var);
    } else {
      expr(n->lhs, s);
    }
    return;
  }
  default:
    expr(n->lhs, s);
    expr(n->rhs, s);
    expr(n->cond, s);
    expr(n->then, s);
    expr(n->els, s);
    expr_list(n->args, s);
    return;
  }
}

static int jumps_away(CtrNode *n) {
  if (!n) {
    return 0;
  }
  switch (n->kind) {
  case ND_RETURN:
  case ND_BREAK:
  case ND_CONTINUE:
  case ND_GOTO:
    return 1;
  case ND_BLOCK: {
    CtrNode *last = n->body;
    while (last && last->next) {
      last = last->next;
    }
    return jumps_away(last);
  }
  default:
    return 0;
  }
}

static void if_stmt(CtrNode *n, Assigned *s) {
  expr(n->cond, s);
  Assigned a = copy_state(s), b = copy_state(s);
  stmt(n->then, &a);
  if (n->els) {
    stmt(n->els, &b);
  }
  if (jumps_away(n->then)) {
    memcpy(s->set, b.set, (size_t)s->n);
  } else if (n->els && jumps_away(n->els)) {
    memcpy(s->set, a.set, (size_t)s->n);
  } else {
    intersect(s, &a, &b);
  }
}

static void loop_stmt(CtrNode *n, Assigned *s) {
  if (n->kind == ND_FOR) {
    if (n->init && n->init->kind == ND_EXPR_STMT) {
      expr(n->init->lhs, s);
    } else if (n->init) {
      stmt(n->init, s);
    }
  }
  if (n->kind != ND_DO) {
    expr(n->cond, s);
  }
  Assigned b = copy_state(s);
  stmt(n->then, &b);
  expr(n->inc, &b);
  if (n->kind == ND_DO) {
    expr(n->cond, &b);
  }
}

static void decl_stmt(CtrNode *n, Assigned *s) {
  if (n->decl_init) {
    init_tree(n->decl_init, s);
    mark_set(s, n->var);
    return;
  }
  if (tracked(n->var)) {
    int aggregate = n->var->ty->kind == TY_ARRAY || n->var->ty->kind == TY_STRUCT ||
                    n->var->ty->kind == TY_UNION || n->var->ty->kind == TY_VLA;
    s->set[n->var->local_index - 1] = (char)aggregate;
  }
}

static void stmt(CtrNode *n, Assigned *s) {
  if (!n) {
    return;
  }
  switch (n->kind) {
  case ND_BLOCK:
    for (CtrNode *b = n->body; b; b = b->next) {
      stmt(b, s);
    }
    return;
  case ND_DECL:
    decl_stmt(n, s);
    return;
  case ND_EXPR_STMT:
    expr(n->lhs, s);
    return;
  case ND_IF:
    if_stmt(n, s);
    return;
  case ND_WHILE:
  case ND_FOR:
  case ND_DO:
    loop_stmt(n, s);
    return;
  case ND_SWITCH: {
    expr(n->cond, s);
    Assigned b = copy_state(s);
    stmt(n->then, &b);
    return;
  }
  case ND_RETURN:
    expr(n->lhs, s);
    fill(s);
    return;
  case ND_BREAK:
  case ND_CONTINUE:
  case ND_GOTO:
    fill(s);
    return;
  case ND_LABEL:
  case ND_CASE:
    stmt(n->lhs, s);
    return;
  default:
    return;
  }
}

const char *ctr_zero_suffix(CtrObj *var) {
  CtrType *t = var->ty;
  if (!var->needs_zero || !ctr_is_scalar(t) || ctr_is_va_list(t)) {
    return "";
  }
  if (ctr_is_flonum(t)) {
    return " = 0.0";
  }
  return t->kind == TY_BOOL ? " = false" : " = 0";
}

void ctr_find_uninitialized_reads(CtrObj *fn, int whole_function) {
  int n = 0;
  for (CtrObj *v = fn->locals; v; v = v->next) {
    v->local_index = ++n;
    v->needs_zero = 0;
  }
  if (whole_function) {
    for (CtrObj *v = fn->locals; v; v = v->next) {
      v->needs_zero = 1;
    }
    return;
  }
  Assigned s = {ctr_alloc((size_t)n + 1), n};
  stmt(fn->body, &s);
}
