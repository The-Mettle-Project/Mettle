#include "translate/ctr_parse.h"

#include <stdlib.h>
#include <string.h>

static CtrToken *stop_at;

static CtrNode *cast_expr(CtrToken **rest, CtrToken *tok);
static CtrNode *unary(CtrToken **rest, CtrToken *tok);
static CtrNode *postfix(CtrToken **rest, CtrToken *tok);
static CtrNode *primary(CtrToken **rest, CtrToken *tok);

CtrNode *ctr_new_num(int64_t val, CtrType *ty, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_NUM, tok);
  node->ival = val;
  node->ty = ty;
  return node;
}

CtrNode *ctr_new_var_node(CtrObj *var, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_VAR, tok);
  node->var = var;
  return node;
}

CtrNode *ctr_new_binary(CtrNodeKind kind, CtrNode *lhs, CtrNode *rhs, CtrToken *tok) {
  CtrNode *node = ctr_new_node(kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

CtrNode *ctr_new_unary(CtrNodeKind kind, CtrNode *expr, CtrToken *tok) {
  CtrNode *node = ctr_new_node(kind, tok);
  node->lhs = expr;
  return node;
}

CtrNode *ctr_expr(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_assign(&tok, tok);
  if (ctr_equal(tok, ",")) {
    CtrToken *op = tok;
    CtrNode *rhs = ctr_expr(rest, tok->next);
    CtrNode *n = ctr_new_binary(ND_COMMA, node, rhs, op);
    ctr_add_type(n);
    return n;
  }
  *rest = tok;
  ctr_add_type(node);
  return node;
}

static const struct {
  const char *op;
  CtrNodeKind kind;
} assign_ops[] = {{"+=", ND_ADD},     {"-=", ND_SUB},    {"*=", ND_MUL},    {"/=", ND_DIV},
                  {"%=", ND_MOD},     {"&=", ND_BITAND}, {"|=", ND_BITOR},  {"^=", ND_BITXOR},
                  {"<<=", ND_SHL},    {">>=", ND_SHR}};

static void check_lvalue(CtrNode *node) {
  switch (node->kind) {
  case ND_VAR:
  case ND_DEREF:
  case ND_INDEX:
  case ND_MEMBER:
  case ND_COMPOUND_LIT:
  case ND_MACRO:
    return;
  case ND_CAST:
    if (node->implicit) {
      check_lvalue(node->lhs);
      return;
    }
    break;
  default:
    break;
  }
  ctr_error_tok(node->tok, "expression is not assignable");
}

CtrNode *ctr_assign(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_conditional(&tok, tok);
  if (ctr_equal(tok, "=")) {
    CtrToken *op = tok;
    CtrNode *rhs = ctr_assign(rest, tok->next);
    ctr_add_type(node);
    check_lvalue(node);
    CtrNode *n = ctr_new_binary(ND_ASSIGN, node, rhs, op);
    ctr_add_type(n);
    return n;
  }
  for (size_t i = 0; i < sizeof(assign_ops) / sizeof(*assign_ops); i++) {
    if (ctr_equal(tok, assign_ops[i].op)) {
      CtrToken *op = tok;
      CtrNode *rhs = ctr_assign(rest, tok->next);
      ctr_add_type(node);
      check_lvalue(node);
      CtrNode *n = ctr_new_binary(ND_OPASSIGN, node, rhs, op);
      n->op = assign_ops[i].kind;
      ctr_add_type(n);
      return n;
    }
  }
  *rest = tok;
  return node;
}

static CtrNode *logor(CtrToken **rest, CtrToken *tok);

CtrNode *ctr_conditional(CtrToken **rest, CtrToken *tok) {
  CtrNode *cond = logor(&tok, tok);
  if (!ctr_equal(tok, "?")) {
    *rest = tok;
    return cond;
  }
  CtrToken *qtok = tok;
  if (ctr_equal(tok->next, ":")) {
    CtrNode *els = ctr_conditional(rest, tok->next->next);
    CtrNode *node = ctr_new_node(ND_COND, qtok);
    node->cond = cond;
    node->then = cond;
    node->els = els;
    node->op = 1;
    ctr_add_type(node);
    return node;
  }
  CtrNode *node = ctr_new_node(ND_COND, qtok);
  node->cond = cond;
  node->then = ctr_expr(&tok, tok->next);
  tok = ctr_skip(tok, ":");
  node->els = ctr_conditional(rest, tok);
  ctr_add_type(node);
  return node;
}

typedef struct {
  const char *op;
  CtrNodeKind kind;
  int prec;
} BinOp;

static const BinOp bin_ops[] = {
    {"||", ND_LOGOR, 1},  {"&&", ND_LOGAND, 2}, {"|", ND_BITOR, 3}, {"^", ND_BITXOR, 4},
    {"&", ND_BITAND, 5},  {"==", ND_EQ, 6},     {"!=", ND_NE, 6},   {"<", ND_LT, 7},
    {"<=", ND_LE, 7},     {">", ND_GT, 7},      {">=", ND_GE, 7},   {"<<", ND_SHL, 8},
    {">>", ND_SHR, 8},    {"+", ND_ADD, 9},     {"-", ND_SUB, 9},   {"*", ND_MUL, 10},
    {"/", ND_DIV, 10},    {"%", ND_MOD, 10}};

static const BinOp *find_binop(CtrToken *tok) {
  if (tok->kind != TK_PUNCT) {
    return NULL;
  }
  for (size_t i = 0; i < sizeof(bin_ops) / sizeof(*bin_ops); i++) {
    if (ctr_equal(tok, bin_ops[i].op)) {
      return &bin_ops[i];
    }
  }
  return NULL;
}

static CtrNode *binary_prec(CtrToken **rest, CtrToken *tok, int min_prec) {
  CtrNode *lhs = cast_expr(&tok, tok);
  for (;;) {
    if (stop_at && tok == stop_at) {
      break;
    }
    const BinOp *op = find_binop(tok);
    if (!op || op->prec < min_prec) {
      break;
    }
    CtrToken *optok = tok;
    CtrNode *rhs = binary_prec(&tok, tok->next, op->prec + 1);
    lhs = ctr_new_binary(op->kind, lhs, rhs, optok);
    ctr_add_type(lhs);
  }
  *rest = tok;
  return lhs;
}

static CtrNode *logor(CtrToken **rest, CtrToken *tok) { return binary_prec(rest, tok, 1); }

static int is_postfix_op(CtrToken *tok) {
  return ctr_equal(tok, "[") || ctr_equal(tok, "(") || ctr_equal(tok, ".") ||
         ctr_equal(tok, "->") || ctr_equal(tok, "++") || ctr_equal(tok, "--");
}

static CtrToken *matching_close(CtrToken *open) {
  int depth = 0;
  for (CtrToken *t = open; t->kind != TK_EOF; t = t->next) {
    if (ctr_equal(t, "(") || ctr_equal(t, "[")) {
      depth++;
    } else if (ctr_equal(t, ")") || ctr_equal(t, "]")) {
      depth--;
      if (depth == 0) {
        return t;
      }
    }
  }
  return NULL;
}

static int single_atom(CtrToken *tok) {
  return tok->kind == TK_NUM || tok->kind == TK_STR || tok->kind == TK_IDENT;
}

typedef enum { SHAPE_NONE, SHAPE_PRIMARY, SHAPE_POSTFIX, SHAPE_UNARY } MacroShape;

static MacroShape operand_shape(CtrToken *tok, CtrToken *end) {
  if (tok == end && single_atom(tok)) {
    return SHAPE_PRIMARY;
  }
  if (ctr_equal(tok, "(") && matching_close(tok) == end) {
    return SHAPE_PRIMARY;
  }
  return SHAPE_NONE;
}

static MacroShape macro_shape(CtrMacroUse *use, CtrToken *tok) {
  CtrToken *end = use->end;
  if (!end) {
    return SHAPE_NONE;
  }
  MacroShape s = operand_shape(tok, end);
  if (s != SHAPE_NONE) {
    return s;
  }
  if (tok->kind == TK_IDENT && ctr_equal(tok->next, "(") && matching_close(tok->next) == end) {
    return SHAPE_POSTFIX;
  }
  int unary_op = ctr_equal(tok, "-") || ctr_equal(tok, "+") || ctr_equal(tok, "~") || ctr_equal(tok, "!");
  if (unary_op && !is_postfix_op(end->next) && operand_shape(tok->next, end) != SHAPE_NONE) {
    return SHAPE_UNARY;
  }
  return SHAPE_NONE;
}

static CtrNode *wrap_macro(CtrMacroUse *use, CtrNode *inner, CtrToken *tok) {
  ctr_add_type(inner);
  CtrNode *node = ctr_new_node(ND_MACRO, tok);
  node->macro = use;
  node->lhs = inner;
  node->ty = inner->ty;
  return node;
}

static CtrNode *try_macro(CtrToken **rest, CtrToken *tok, int *handled, int want_unary) {
  CtrMacroUse *chain[64];
  int n = 0;
  for (CtrMacroUse *u = tok->mark; u && n < 64; u = u->inner) {
    chain[n++] = u;
  }
  for (int i = n - 1; i >= 0; i--) {
    MacroShape shape = macro_shape(chain[i], tok);
    if (shape == SHAPE_NONE || (shape == SHAPE_UNARY) != want_unary) {
      continue;
    }
    CtrMacroUse *use = chain[i];
    CtrMacroUse *saved_mark = tok->mark;
    tok->mark = i > 0 ? chain[0] : NULL;
    if (i > 0) {
      chain[i - 1]->inner = NULL;
    }
    CtrToken *saved_stop = stop_at;
    stop_at = use->end->next;
    CtrNode *inner = shape == SHAPE_UNARY ? unary(rest, tok) : postfix(rest, tok);
    stop_at = saved_stop;
    if (i > 0) {
      chain[i - 1]->inner = chain[i];
    }
    tok->mark = saved_mark;
    *handled = 1;
    if (*rest != use->end->next) {
      return inner;
    }
    return wrap_macro(use, inner, use->use_tok);
  }
  *handled = 0;
  return NULL;
}

static CtrNode *new_cast_explicit(CtrNode *expr, CtrType *ty, CtrToken *tok) {
  ctr_add_type(expr);
  CtrNode *node = ctr_new_node(ND_CAST, tok);
  node->lhs = expr;
  node->ty = ty;
  node->cast_ty = ty;
  return node;
}

static CtrNode *cast_expr(CtrToken **rest, CtrToken *tok) {
  if (tok->mark) {
    int handled;
    CtrNode *m = try_macro(rest, tok, &handled, 1);
    if (handled) {
      return m;
    }
  }
  if (ctr_equal(tok, "(") && ctr_is_typename(tok->next)) {
    CtrToken *start = tok;
    CtrType *ty = ctr_typename(&tok, tok->next);
    tok = ctr_skip(tok, ")");
    if (ctr_equal(tok, "{")) {
      return unary(rest, start);
    }
    CtrNode *operand = cast_expr(rest, tok);
    CtrNode *node = new_cast_explicit(operand, ty, start);
    return node;
  }
  return unary(rest, tok);
}

static CtrNode *new_incdec(CtrNodeKind kind, CtrNode *lhs, CtrToken *tok) {
  ctr_add_type(lhs);
  check_lvalue(lhs);
  CtrNode *n = ctr_new_unary(kind, lhs, tok);
  ctr_add_type(n);
  return n;
}

static CtrNode *sizeof_type(CtrType *ty, CtrToken *tok) {
  CtrNode *n = ctr_new_node(ND_SIZEOF, tok);
  n->cast_ty = ty;
  if (ty->kind == TY_VLA) {
    n->ival = -1;
  } else {
    n->ival = ty->size;
  }
  n->ty = ctr_size_t_type();
  if (ty->size < 0) {
    ctr_error_tok(tok, "sizeof of an incomplete type");
  }
  return n;
}

static CtrNode *sizeof_expr(CtrToken **rest, CtrToken *tok) {
  CtrToken *start = tok;
  if (ctr_equal(tok->next, "(") && ctr_is_typename(tok->next->next)) {
    CtrType *ty = ctr_typename(&tok, tok->next->next);
    tok = ctr_skip(tok, ")");
    if (ctr_equal(tok, "{")) {
      CtrNode *lit = ctr_compound_literal(&tok, tok, ty, start);
      ctr_add_type(lit);
      *rest = tok;
      CtrNode *n = sizeof_type(lit->ty, start);
      return n;
    }
    *rest = tok;
    return sizeof_type(ty, start);
  }
  CtrNode *e = unary(rest, tok->next);
  ctr_add_type(e);
  CtrNode *n = sizeof_type(e->ty, start);
  n->lhs = e;
  return n;
}

static CtrNode *alignof_expr(CtrToken **rest, CtrToken *tok) {
  CtrToken *start = tok;
  CtrType *ty;
  if (ctr_equal(tok->next, "(") && ctr_is_typename(tok->next->next)) {
    ty = ctr_typename(&tok, tok->next->next);
    *rest = ctr_skip(tok, ")");
  } else {
    CtrNode *e = unary(rest, tok->next);
    ctr_add_type(e);
    ty = e->ty;
  }
  CtrNode *n = ctr_new_node(ND_ALIGNOF, start);
  n->ival = ty->align;
  n->cast_ty = ty;
  n->ty = ctr_size_t_type();
  return n;
}

static CtrNode *unary_op(CtrNodeKind kind, CtrToken **rest, CtrToken *tok) {
  CtrNode *e = cast_expr(rest, tok->next);
  CtrNode *n = ctr_new_unary(kind, e, tok);
  ctr_add_type(n);
  return n;
}

static CtrNode *addr_of(CtrToken **rest, CtrToken *tok) {
  CtrNode *e = cast_expr(rest, tok->next);
  ctr_add_type(e);
  if (e->kind == ND_MEMBER && e->member && e->member->is_bitfield) {
    ctr_error_tok(tok, "cannot take the address of a bit-field");
  }
  CtrNode *n = ctr_new_unary(ND_ADDR, e, tok);
  ctr_add_type(n);
  if (e->kind == ND_VAR && e->var) {
    e->var->addr_taken = 1;
  }
  return n;
}

static CtrNode *label_address(CtrToken **rest, CtrToken *tok) {
  CtrNode *n = ctr_new_node(ND_LABEL_VAL, tok);
  n->label = ctr_get_ident(tok->next);
  n->ty = ctr_pointer_to(ctr_ty_void);
  n->inc = ctr_ps.gotos;
  ctr_ps.gotos = n;
  if (ctr_ps.current_fn) {
    ctr_ps.current_fn->has_goto = 1;
  }
  *rest = tok->next->next;
  return n;
}

static CtrNode *unary(CtrToken **rest, CtrToken *tok) {
  if (ctr_equal(tok, "+")) {
    return unary_op(ND_POS, rest, tok);
  }
  if (ctr_equal(tok, "-")) {
    return unary_op(ND_NEG, rest, tok);
  }
  if (ctr_equal(tok, "!")) {
    return unary_op(ND_NOT, rest, tok);
  }
  if (ctr_equal(tok, "~")) {
    return unary_op(ND_BITNOT, rest, tok);
  }
  if (ctr_equal(tok, "&")) {
    return addr_of(rest, tok);
  }
  if (ctr_equal(tok, "*")) {
    CtrNode *e = cast_expr(rest, tok->next);
    ctr_add_type(e);
    if (e->ty->kind == TY_FUNC) {
      return e;
    }
    CtrNode *n = ctr_new_unary(ND_DEREF, e, tok);
    ctr_add_type(n);
    return n;
  }
  if (ctr_equal(tok, "++")) {
    return new_incdec(ND_PREINC, unary(rest, tok->next), tok);
  }
  if (ctr_equal(tok, "--")) {
    return new_incdec(ND_PREDEC, unary(rest, tok->next), tok);
  }
  if (ctr_equal(tok, "&&")) {
    return label_address(rest, tok);
  }
  if (ctr_equal(tok, "sizeof")) {
    return sizeof_expr(rest, tok);
  }
  if (ctr_equal(tok, "_Alignof") || ctr_equal(tok, "__alignof")) {
    return alignof_expr(rest, tok);
  }
  if (ctr_equal(tok, "__extension__")) {
    return cast_expr(rest, tok->next);
  }
  return postfix(rest, tok);
}

static CtrMember *get_struct_member(CtrType *ty, CtrToken *tok) {
  if (!ty->sdecl) {
    return NULL;
  }
  for (CtrMember *mem = ty->sdecl->members; mem; mem = mem->next) {
    if (mem->anon && (mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION)) {
      if (get_struct_member(mem->ty, tok)) {
        return mem;
      }
      continue;
    }
    if (mem->name && mem->name->len == tok->len &&
        strncmp(mem->name->loc, tok->loc, (size_t)tok->len) == 0) {
      return mem;
    }
  }
  return NULL;
}

static CtrNode *struct_ref(CtrNode *node, CtrToken *tok) {
  ctr_add_type(node);
  CtrType *ty = node->ty;
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) {
    ctr_fatal_tok(node->tok, "member reference base is not a struct or union");
  }
  if (!ty->sdecl || !ty->sdecl->complete) {
    ctr_fatal_tok(tok, "member access into an incomplete type");
  }
  for (;;) {
    CtrMember *mem = get_struct_member(ty, tok);
    if (!mem) {
      ctr_fatal_tok(tok, "no member named '%.*s'", tok->len, tok->loc);
    }
    node = ctr_new_unary(ND_MEMBER, node, tok);
    node->member = mem;
    ctr_add_type(node);
    if (mem->name) {
      break;
    }
    ty = mem->ty;
  }
  return node;
}

static CtrNode *deref_for_arrow(CtrNode *node, CtrToken *tok) {
  ctr_add_type(node);
  CtrNode *d = ctr_new_unary(ND_DEREF, node, tok);
  d->op = 1;
  ctr_add_type(d);
  return d;
}

static CtrNode *funcall(CtrToken **rest, CtrToken *tok, CtrNode *fn);

static CtrNode *postfix_compound(CtrToken **rest, CtrToken *tok, int *ok) {
  *ok = 0;
  if (ctr_equal(tok, "(") && ctr_is_typename(tok->next)) {
    CtrToken *start = tok;
    CtrType *ty = ctr_typename(&tok, tok->next);
    tok = ctr_skip(tok, ")");
    if (ctr_equal(tok, "{")) {
      *ok = 1;
      return ctr_compound_literal(rest, tok, ty, start);
    }
  }
  return NULL;
}

static CtrNode *postfix(CtrToken **rest, CtrToken *tok) {
  int ok = 0;
  CtrNode *node = NULL;
  if (tok->mark) {
    node = try_macro(&tok, tok, &ok, 0);
  }
  if (!ok) {
    node = postfix_compound(&tok, tok, &ok);
  }
  if (!ok) {
    node = primary(&tok, tok);
  }
  for (;;) {
    if (stop_at && tok == stop_at) {
      break;
    }
    if (ctr_equal(tok, "(")) {
      node = funcall(&tok, tok->next, node);
      continue;
    }
    if (ctr_equal(tok, "[")) {
      CtrToken *start = tok;
      CtrToken *saved = stop_at;
      stop_at = NULL;
      CtrNode *idx = ctr_expr(&tok, tok->next);
      stop_at = saved;
      tok = ctr_skip(tok, "]");
      ctr_add_type(node);
      node = ctr_new_binary(ND_INDEX, node, idx, start);
      ctr_add_type(node);
      continue;
    }
    if (ctr_equal(tok, ".")) {
      node = struct_ref(node, tok->next);
      tok = tok->next->next;
      continue;
    }
    if (ctr_equal(tok, "->")) {
      CtrToken *arrow = tok;
      CtrNode *d = deref_for_arrow(node, arrow);
      node = struct_ref(d, tok->next);
      tok = tok->next->next;
      continue;
    }
    if (ctr_equal(tok, "++")) {
      node = new_incdec(ND_POSTINC, node, tok);
      tok = tok->next;
      continue;
    }
    if (ctr_equal(tok, "--")) {
      node = new_incdec(ND_POSTDEC, node, tok);
      tok = tok->next;
      continue;
    }
    break;
  }
  *rest = tok;
  return node;
}

static CtrNode *default_promote_arg(CtrNode *arg) {
  ctr_add_type(arg);
  CtrType *t = arg->ty;
  if (t->kind == TY_FLOAT) {
    return ctr_new_cast(arg, ctr_ty_double);
  }
  if (t->kind == TY_ARRAY || t->kind == TY_VLA) {
    return ctr_new_cast(arg, ctr_pointer_to(t->base));
  }
  if (t->kind == TY_FUNC) {
    return ctr_new_cast(arg, ctr_pointer_to(t));
  }
  if (ctr_is_integer(t) && (t->size < 4 || t->kind == TY_ENUM || t->kind == TY_BOOL)) {
    return ctr_new_cast(arg, ctr_ty_int);
  }
  return arg;
}

static CtrNode *convert_arg(CtrNode *arg, CtrType *param) {
  ctr_add_type(arg);
  if (param->kind == TY_STRUCT || param->kind == TY_UNION) {
    return arg;
  }
  CtrType *t = arg->ty;
  if (t->kind == TY_ARRAY || t->kind == TY_VLA) {
    arg = ctr_new_cast(arg, ctr_pointer_to(t->base));
    t = arg->ty;
  } else if (t->kind == TY_FUNC) {
    arg = ctr_new_cast(arg, ctr_pointer_to(t));
    t = arg->ty;
  }
  if (ctr_same_type(t, param)) {
    return arg;
  }
  return ctr_new_cast(arg, ctr_unqual(param));
}

static CtrType *callee_type(CtrNode *fn) {
  ctr_add_type(fn);
  CtrType *t = fn->ty;
  if (t->kind == TY_PTR && t->base->kind == TY_FUNC) {
    return t->base;
  }
  if (t->kind == TY_FUNC) {
    return t;
  }
  ctr_fatal_tok(fn->tok, "called object is not a function");
  return NULL;
}

static CtrNode *funcall(CtrToken **rest, CtrToken *tok, CtrNode *fn) {
  CtrType *ty = callee_type(fn);
  CtrType *param_ty = ty->params;
  CtrNode head = {0};
  CtrNode *cur = &head;
  CtrToken *saved = stop_at;
  stop_at = NULL;
  while (!ctr_equal(tok, ")")) {
    if (cur != &head) {
      tok = ctr_skip(tok, ",");
    }
    CtrNode *arg = ctr_assign(&tok, tok);
    if (param_ty && ty->has_proto) {
      arg = convert_arg(arg, param_ty);
      param_ty = param_ty->next;
    } else {
      if (ty->has_proto && !ty->is_variadic) {
        ctr_error_tok(arg->tok, "too many arguments");
      }
      arg = default_promote_arg(arg);
    }
    cur = cur->next = arg;
  }
  stop_at = saved;
  if (param_ty && ty->has_proto) {
    ctr_error_tok(tok, "too few arguments");
  }
  *rest = ctr_skip(tok, ")");
  CtrNode *node = ctr_new_node(ND_FUNCALL, fn->tok);
  node->lhs = fn;
  node->func_ty = ty;
  node->ty = ty->return_ty;
  node->args = head.next;
  if (fn->kind == ND_VAR && fn->var) {
    fn->var->referenced = 1;
  }
  return node;
}

static int same_qualifiers(CtrType *a, CtrType *b) {
  return a->is_const == b->is_const && a->is_volatile == b->is_volatile;
}

static int generic_matches(CtrType *ctrl, CtrType *assoc) {
  if (!same_qualifiers(ctrl, assoc) || !ctr_is_compatible(ctrl, assoc)) {
    return 0;
  }
  while (ctrl->kind == TY_PTR && assoc->kind == TY_PTR) {
    ctrl = ctrl->base;
    assoc = assoc->base;
    if (!same_qualifiers(ctrl, assoc)) {
      return 0;
    }
  }
  return 1;
}

static CtrNode *generic_selection(CtrToken **rest, CtrToken *tok) {
  CtrToken *start = tok;
  tok = ctr_skip(tok->next, "(");
  CtrNode *ctrl = ctr_assign(&tok, tok);
  ctr_add_type(ctrl);
  CtrType *t1 = ctrl->ty;
  if (t1->kind == TY_FUNC) {
    t1 = ctr_pointer_to(t1);
  } else if (t1->kind == TY_ARRAY) {
    t1 = ctr_pointer_to(t1->base);
  }
  CtrNode *ret = NULL;
  while (!ctr_consume(rest, tok, ")")) {
    tok = ctr_skip(tok, ",");
    if (ctr_equal(tok, "default")) {
      tok = ctr_skip(tok->next, ":");
      CtrNode *n = ctr_assign(&tok, tok);
      if (!ret) {
        ret = n;
      }
      continue;
    }
    CtrType *t2 = ctr_typename(&tok, tok);
    tok = ctr_skip(tok, ":");
    CtrNode *n = ctr_assign(&tok, tok);
    if (generic_matches(ctr_unqual(t1), t2)) {
      ret = n;
    }
  }
  if (!ret) {
    ctr_fatal_tok(start, "no _Generic association matches the controlling expression");
  }
  return ret;
}

static CtrNode *va_builtin(CtrToken **rest, CtrToken *tok, CtrNodeKind kind) {
  CtrNode *node = ctr_new_node(kind, tok);
  tok = ctr_skip(tok->next, "(");
  node->lhs = ctr_assign(&tok, tok);
  ctr_add_type(node->lhs);
  if (kind == ND_VA_ARG) {
    tok = ctr_skip(tok, ",");
    node->ty = ctr_typename(&tok, tok);
  } else if (kind == ND_VA_COPY) {
    tok = ctr_skip(tok, ",");
    node->rhs = ctr_assign(&tok, tok);
    ctr_add_type(node->rhs);
    node->ty = ctr_ty_void;
  } else if (kind == ND_VA_START) {
    if (ctr_consume(&tok, tok, ",")) {
      CtrNode *last = ctr_assign(&tok, tok);
      (void)last;
    }
    node->ty = ctr_ty_void;
    if (ctr_ps.current_fn) {
      ctr_ps.current_fn->uses_va = 1;
    }
  } else {
    node->ty = ctr_ty_void;
  }
  *rest = ctr_skip(tok, ")");
  return node;
}

static int64_t offsetof_designator(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  int64_t off = 0;
  for (;;) {
    if (tok->kind == TK_IDENT) {
      CtrMember *mem = get_struct_member(ty, tok);
      if (!mem) {
        ctr_fatal_tok(tok, "no member named '%.*s'", tok->len, tok->loc);
      }
      off += mem->offset;
      ty = mem->ty;
      if (mem->anon) {
        continue;
      }
      tok = tok->next;
    } else if (ctr_equal(tok, ".")) {
      tok = tok->next;
    } else if (ctr_equal(tok, "[")) {
      int64_t i = ctr_const_expr(&tok, tok->next);
      tok = ctr_skip(tok, "]");
      off += i * ty->base->size;
      ty = ty->base;
    } else {
      break;
    }
  }
  *rest = tok;
  return off;
}

static CtrNode *builtin_offsetof(CtrToken **rest, CtrToken *tok) {
  CtrToken *start = tok;
  tok = ctr_skip(tok->next, "(");
  CtrType *ty = ctr_typename(&tok, tok);
  tok = ctr_skip(tok, ",");
  int64_t off = offsetof_designator(&tok, tok, ty);
  *rest = ctr_skip(tok, ")");
  CtrNode *n = ctr_new_num(off, ctr_size_t_type(), start);
  n->spelling = NULL;
  return n;
}

static CtrNode *builtin_types_compatible(CtrToken **rest, CtrToken *tok) {
  CtrToken *start = tok;
  tok = ctr_skip(tok->next, "(");
  CtrType *t1 = ctr_typename(&tok, tok);
  tok = ctr_skip(tok, ",");
  CtrType *t2 = ctr_typename(&tok, tok);
  *rest = ctr_skip(tok, ")");
  return ctr_new_num(ctr_is_compatible(ctr_unqual(t1), ctr_unqual(t2)), ctr_ty_int, start);
}

typedef struct {
  const char *name;
  int id;
  int argc;
  int result;
} BuiltinSpec;

enum {
  BR_INT,
  BR_UINT,
  BR_ULL,
  BR_USHORT,
  BR_VOID,
  BR_PTR,
  BR_DOUBLE,
  BR_FLOAT,
  BR_SIZE,
  BR_ARG0,
  BR_BOOL
};

static const BuiltinSpec builtin_specs[] = {
    {"__builtin_unreachable", 1, 0, BR_VOID}, {"__builtin_trap", 2, 0, BR_VOID},
    {"__builtin_popcount", 3, 1, BR_INT},     {"__builtin_popcountl", 4, 1, BR_INT},
    {"__builtin_popcountll", 5, 1, BR_INT},   {"__builtin_clz", 6, 1, BR_INT},
    {"__builtin_clzl", 7, 1, BR_INT},         {"__builtin_clzll", 8, 1, BR_INT},
    {"__builtin_ctz", 9, 1, BR_INT},          {"__builtin_ctzl", 10, 1, BR_INT},
    {"__builtin_ctzll", 11, 1, BR_INT},       {"__builtin_bswap16", 12, 1, BR_USHORT},
    {"__builtin_bswap32", 13, 1, BR_UINT},    {"__builtin_bswap64", 14, 1, BR_ULL},
    {"__builtin_alloca", 15, 1, BR_PTR},      {"__builtin_huge_val", 16, 0, BR_DOUBLE},
    {"__builtin_inf", 17, 0, BR_DOUBLE},      {"__builtin_inff", 18, 0, BR_FLOAT},
    {"__builtin_huge_valf", 19, 0, BR_FLOAT}, {"__builtin_nan", 20, 1, BR_DOUBLE},
    {"__builtin_nanf", 21, 1, BR_FLOAT},      {"__builtin_add_overflow", 22, 3, BR_BOOL},
    {"__builtin_sub_overflow", 23, 3, BR_BOOL}, {"__builtin_mul_overflow", 24, 3, BR_BOOL},
    {"__builtin_abs", 25, 1, BR_INT},         {"__builtin_ffs", 26, 1, BR_INT},
    {"__builtin_parity", 27, 1, BR_INT},      {"__builtin_prefetch", 28, -1, BR_VOID},
    {"__builtin_assume_aligned", 29, -1, BR_ARG0}, {"__builtin_frame_address", 30, 1, BR_PTR},
    {"__builtin_return_address", 31, 1, BR_PTR}};

static CtrType *builtin_result(int r, CtrNode *args) {
  switch (r) {
  case BR_INT:
    return ctr_ty_int;
  case BR_UINT:
    return ctr_ty_uint;
  case BR_ULL:
    return ctr_ty_ullong;
  case BR_USHORT:
    return ctr_ty_ushort;
  case BR_VOID:
    return ctr_ty_void;
  case BR_PTR:
    return ctr_pointer_to(ctr_ty_void);
  case BR_DOUBLE:
    return ctr_ty_double;
  case BR_FLOAT:
    return ctr_ty_float;
  case BR_BOOL:
    return ctr_ty_bool;
  case BR_ARG0:
    return args ? args->ty : ctr_ty_void;
  default:
    return ctr_size_t_type();
  }
}

static const BuiltinSpec *find_builtin(CtrToken *tok) {
  for (size_t i = 0; i < sizeof(builtin_specs) / sizeof(*builtin_specs); i++) {
    if (ctr_equal(tok, builtin_specs[i].name)) {
      return &builtin_specs[i];
    }
  }
  return NULL;
}

static CtrNode *builtin_call(CtrToken **rest, CtrToken *tok, const BuiltinSpec *spec) {
  CtrNode *node = ctr_new_node(ND_BUILTIN, tok);
  node->builtin = spec->id;
  node->spelling = spec->name;
  tok = ctr_skip(tok->next, "(");
  CtrNode head = {0};
  CtrNode *cur = &head;
  while (!ctr_equal(tok, ")")) {
    if (cur != &head) {
      tok = ctr_skip(tok, ",");
    }
    CtrNode *arg = ctr_assign(&tok, tok);
    ctr_add_type(arg);
    cur = cur->next = arg;
  }
  *rest = ctr_skip(tok, ")");
  node->args = head.next;
  node->ty = builtin_result(spec->result, node->args);
  return node;
}

static CtrNode *builtin_expect(CtrToken **rest, CtrToken *tok) {
  tok = ctr_skip(tok->next, "(");
  CtrNode *e = ctr_assign(&tok, tok);
  tok = ctr_skip(tok, ",");
  ctr_assign(&tok, tok);
  *rest = ctr_skip(tok, ")");
  return e;
}

static CtrNode *builtin_constant_p(CtrToken **rest, CtrToken *tok) {
  CtrToken *start = tok;
  tok = ctr_skip(tok->next, "(");
  CtrNode *e = ctr_assign(&tok, tok);
  ctr_add_type(e);
  *rest = ctr_skip(tok, ")");
  int64_t v;
  return ctr_new_num(ctr_try_eval(e, &v), ctr_ty_int, start);
}

static CtrNode *try_builtin(CtrToken **rest, CtrToken *tok, int *ok) {
  *ok = 1;
  if (ctr_equal(tok, "__builtin_va_start")) {
    return va_builtin(rest, tok, ND_VA_START);
  }
  if (ctr_equal(tok, "__builtin_va_arg")) {
    return va_builtin(rest, tok, ND_VA_ARG);
  }
  if (ctr_equal(tok, "__builtin_va_end")) {
    return va_builtin(rest, tok, ND_VA_END);
  }
  if (ctr_equal(tok, "__builtin_va_copy")) {
    return va_builtin(rest, tok, ND_VA_COPY);
  }
  if (ctr_equal(tok, "__builtin_offsetof")) {
    return builtin_offsetof(rest, tok);
  }
  if (ctr_equal(tok, "__builtin_types_compatible_p")) {
    return builtin_types_compatible(rest, tok);
  }
  if (ctr_equal(tok, "__builtin_expect")) {
    return builtin_expect(rest, tok);
  }
  if (ctr_equal(tok, "__builtin_constant_p")) {
    return builtin_constant_p(rest, tok);
  }
  if (ctr_equal(tok, "_Generic")) {
    return generic_selection(rest, tok);
  }
  const BuiltinSpec *spec = ctr_equal(tok->next, "(") ? find_builtin(tok) : NULL;
  if (spec) {
    return builtin_call(rest, tok, spec);
  }
  *ok = 0;
  return NULL;
}

static CtrNode *stmt_expr(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_STMT_EXPR, tok);
  node->body = ctr_compound_stmt(&tok, tok->next->next)->body;
  *rest = ctr_skip(tok, ")");
  CtrNode *last = node->body;
  while (last && last->next) {
    last = last->next;
  }
  if (last && last->kind == ND_EXPR_STMT) {
    ctr_add_type(last->lhs);
    node->ty = last->lhs->ty;
  } else {
    node->ty = ctr_ty_void;
  }
  return node;
}

static CtrNode *func_name_literal(CtrToken *tok) {
  const char *name = ctr_ps.current_fn ? ctr_ps.current_fn->name : "";
  CtrNode *n = ctr_new_node(ND_STR, tok);
  n->str = ctr_strdup(name);
  n->str_len = (int)strlen(name) + 1;
  n->str_ty = ctr_array_of(ctr_ty_char, n->str_len);
  n->ty = n->str_ty;
  n->op = 1;
  return n;
}

static CtrNode *implicit_function(CtrToken *tok) {
  ctr_warn_tok(tok, "implicit declaration of function '%.*s'", tok->len, tok->loc);
  CtrScope *saved = ctr_ps.scope;
  while (ctr_ps.scope->next) {
    ctr_ps.scope = ctr_ps.scope->next;
  }
  CtrType *ty = ctr_func_type(ctr_ty_int);
  CtrObj *fn = ctr_new_gvar(ctr_token_text(tok), ty, tok);
  ctr_ps.scope = saved;
  fn->is_function = 1;
  fn->is_definition = 0;
  fn->is_static = 0;
  return ctr_new_var_node(fn, tok);
}

static CtrNode *identifier(CtrToken **rest, CtrToken *tok) {
  CtrVarScope *sc = ctr_find_var(tok);
  *rest = tok->next;
  if (sc && sc->var) {
    sc->var->referenced = 1;
    return ctr_new_var_node(sc->var, tok);
  }
  if (sc && sc->enum_obj) {
    CtrNode *n = ctr_new_num(sc->enum_obj->enum_value, ctr_ty_int, tok);
    n->var = sc->enum_obj;
    sc->enum_obj->referenced = 1;
    return n;
  }
  if (ctr_equal(tok, "__func__")) {
    return func_name_literal(tok);
  }
  if (ctr_equal(tok->next, "(")) {
    return implicit_function(tok);
  }
  ctr_fatal_tok(tok, "use of undeclared identifier '%.*s'", tok->len, tok->loc);
  return NULL;
}

static CtrNode *number_literal(CtrToken **rest, CtrToken *tok) {
  CtrNode *node;
  if (ctr_is_flonum(tok->ty)) {
    node = ctr_new_node(ND_NUM, tok);
    node->fval = tok->fval;
    node->ty = tok->ty;
    node->is_float_lit = 1;
  } else {
    node = ctr_new_num(tok->ival, tok->ty, tok);
  }
  node->spelling = ctr_token_text(tok);
  *rest = tok->next;
  return node;
}

static CtrNode *primary(CtrToken **rest, CtrToken *tok) {
  if (ctr_equal(tok, "(") && ctr_equal(tok->next, "{")) {
    return stmt_expr(rest, tok);
  }
  if (ctr_equal(tok, "(")) {
    CtrToken *saved = stop_at;
    stop_at = NULL;
    CtrNode *node = ctr_expr(&tok, tok->next);
    stop_at = saved;
    *rest = ctr_skip(tok, ")");
    return node;
  }
  int ok;
  CtrNode *b = try_builtin(rest, tok, &ok);
  if (ok) {
    return b;
  }
  if (tok->kind == TK_IDENT) {
    return identifier(rest, tok);
  }
  if (tok->kind == TK_STR) {
    CtrNode *n = ctr_new_node(ND_STR, tok);
    n->str = tok->str;
    n->str_len = tok->str_len;
    n->str_ty = tok->ty;
    n->ty = tok->ty;
    *rest = tok->next;
    return n;
  }
  if (tok->kind == TK_NUM) {
    return number_literal(rest, tok);
  }
  ctr_fatal_tok(tok, "expected an expression");
  return NULL;
}
