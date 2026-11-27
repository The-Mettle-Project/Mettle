#include "translate/ctr_emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ctr_char_literal_ok(CtrNode *node);

static char *paren(char *text, int need, int have) {
  if (have < need) {
    return ctr_format("(%s)", text);
  }
  return text;
}

char *ctr_new_temp(const char *base) {
  for (;;) {
    char *name = ctr_format("%s%d", base, ++ctr_em->temps);
    if (!ctr_map_get(&ctr_em->local_names, name)) {
      ctr_map_put(&ctr_em->local_names, name, (void *)1);
      return name;
    }
  }
}

static CtrNode *strip_macro(CtrNode *n) {
  while (n && n->kind == ND_MACRO && !(n->macro && n->macro->macro && n->macro->macro->recovered)) {
    n = n->lhs;
  }
  return n;
}

int ctr_has_vla(CtrType *ty);

static int vla_row(CtrType *t) { return t->kind == TY_VLA || (t->kind == TY_ARRAY && ctr_has_vla(t)); }

static char *vla_stride(CtrType *row);

static int is_bool_node(CtrNode *n) {
  n = strip_macro(n);
  switch (n->kind) {
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
  case ND_LOGAND:
  case ND_LOGOR:
  case ND_NOT:
    return 1;
  default:
    return 0;
  }
}

static int fits(int64_t v, CtrType *ty) {
  if (!ctr_is_integer(ty) || ty->kind == TY_BOOL) {
    return 0;
  }
  int bits = ty->size * 8;
  if (ty->is_unsigned) {
    return bits == 64 || (v >= 0 && (uint64_t)v < (1ull << bits));
  }
  if (bits == 64) {
    return 1;
  }
  return v >= -(1ll << (bits - 1)) && v <= (1ll << (bits - 1)) - 1;
}

static const char *binop_text(CtrNodeKind k) {
  static const struct {
    CtrNodeKind kind;
    const char *text;
  } ops[] = {{ND_ADD, "+"},  {ND_SUB, "-"},   {ND_MUL, "*"},     {ND_DIV, "/"},     {ND_MOD, "%"},
             {ND_BITAND, "&"}, {ND_BITOR, "|"}, {ND_BITXOR, "^"}, {ND_SHL, "<<"},   {ND_SHR, ">>"},
             {ND_EQ, "=="}, {ND_NE, "!="},   {ND_LT, "<"},      {ND_LE, "<="},     {ND_GT, ">"},
             {ND_GE, ">="}, {ND_LOGAND, "&&"}, {ND_LOGOR, "||"}};
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); i++) {
    if (ops[i].kind == k) {
      return ops[i].text;
    }
  }
  return "?";
}

static int binop_prec(CtrNodeKind k) {
  switch (k) {
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
    return PREC_MUL;
  case ND_ADD:
  case ND_SUB:
    return PREC_ADD;
  case ND_SHL:
  case ND_SHR:
    return PREC_SHIFT;
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
    return PREC_REL;
  case ND_EQ:
  case ND_NE:
    return PREC_EQ;
  case ND_BITAND:
    return PREC_BITAND;
  case ND_BITXOR:
    return PREC_BITXOR;
  case ND_BITOR:
    return PREC_BITOR;
  case ND_LOGAND:
    return PREC_LOGAND;
  default:
    return PREC_LOGOR;
  }
}

static char *stmt_block_text(void (*emit)(void *), void *arg, const char *tail);

static char *expr(CtrNode *node, int prec);
static int comparison_operands(CtrNode *node, CtrNode **lhs, CtrNode **rhs);
static int binop_prec(CtrNodeKind k);
static const char *binop_text(CtrNodeKind k);

static int wrap_op(CtrNodeKind k) {
  return k == ND_ADD || k == ND_SUB || k == ND_MUL || k == ND_BITAND || k == ND_BITOR || k == ND_BITXOR;
}

static CtrNode *narrow_leaf(CtrNode *n) {
  while (n->kind == ND_CAST && n->implicit && ctr_is_integer(n->lhs->ty) && n->lhs->ty->kind != TY_BOOL &&
         n->lhs->ty->size <= n->ty->size && !is_bool_node(n->lhs)) {
    n = n->lhs;
  }
  return n;
}

static int wrap_safe(CtrNode *n, CtrType *to, int depth) {
  CtrNode *s = strip_macro(n);
  if (depth > 32) {
    return 0;
  }
  if (s->kind == ND_NUM && !s->is_float_lit) {
    return 1;
  }
  CtrNode *leaf = narrow_leaf(s);
  if (leaf != s) {
    return wrap_safe(leaf, to, depth + 1);
  }
  if (wrap_op(s->kind) && ctr_is_integer(s->ty) && !(s->lhs->ty->kind == TY_PTR)) {
    return wrap_safe(s->lhs, to, depth + 1) && wrap_safe(s->rhs, to, depth + 1);
  }
  if ((s->kind == ND_NEG || s->kind == ND_BITNOT) && ctr_is_integer(s->ty)) {
    return wrap_safe(s->lhs, to, depth + 1);
  }
  return ctr_is_integer(s->ty) && s->ty->kind != TY_BOOL && !is_bool_node(s) &&
         ctr_same_mettle_type(s->ty, to);
}

static int64_t wrap_value(int64_t v, CtrType *to) {
  int bits = to->size * 8;
  if (bits >= 64) {
    return v;
  }
  uint64_t mask = (1ull << bits) - 1;
  uint64_t u = (uint64_t)v & mask;
  if (!to->is_unsigned && (u >> (bits - 1))) {
    return (int64_t)(u | ~mask);
  }
  return (int64_t)u;
}

static char *narrow_text(CtrNode *n, CtrType *to, int prec) {
  CtrNode *s = strip_macro(n);
  if (s->kind == ND_NUM && !s->is_float_lit) {
    CtrNode num = *s;
    num.ival = wrap_value(s->ival, to);
    num.ty = to;
    if (num.ival != s->ival) {
      num.spelling = NULL;
    }
    char *t = ctr_literal_text(&num, to);
    return num.ival < 0 ? paren(t, prec, PREC_UNARY + 1) : t;
  }
  CtrNode *leaf = narrow_leaf(s);
  if (leaf != s) {
    return narrow_text(leaf, to, prec);
  }
  if (wrap_op(s->kind) && ctr_is_integer(s->ty)) {
    int p = binop_prec(s->kind);
    return paren(ctr_format("%s %s %s", narrow_text(s->lhs, to, p), binop_text(s->kind),
                            narrow_text(s->rhs, to, p + 1)),
                 prec, p);
  }
  if (s->kind == ND_NEG && to->is_unsigned) {
    return paren(ctr_format("0 - %s", narrow_text(s->lhs, to, PREC_ADD + 1)), prec, PREC_ADD);
  }
  if (s->kind == ND_NEG || s->kind == ND_BITNOT) {
    return paren(ctr_format("%s%s", s->kind == ND_NEG ? "-" : "~", narrow_text(s->lhs, to, PREC_UNARY)), prec,
                 PREC_UNARY);
  }
  return expr(n, prec);
}

char *ctr_narrow_text(CtrNode *n, CtrType *to, int prec) {
  if (!ctr_is_integer(to) || to->kind == TY_BOOL || !wrap_safe(n, to, 0)) {
    return NULL;
  }
  return narrow_text(n, to, prec);
}

static char *function_address(CtrObj *f) {
  if (f->mettle_module) {
    return ctr_format("&%s%s", ctr_module_prefix(f->mettle_module), f->mettle_name);
  }
  return ctr_format("&%s", ctr_mettle_name_of(f));
}

static char *decay_text(CtrNode *inner, CtrType *to, int bare) {
  CtrNode *s = strip_macro(inner);
  if (s->kind == ND_STR && s->str_ty->base->size == 1) {
    char *lit = ctr_string_literal_text(s->str, s->str_len, 1);
    if (inner != s && s->str_len > 0) {
      lit = expr(inner, PREC_POSTFIX);
    }
    return bare ? lit : ctr_format("(%s)%s", ctr_type_text(to), lit);
  }
  if (s->kind == ND_VAR && s->var->is_function) {
    return function_address(s->var);
  }
  if (inner->ty->kind == TY_FUNC) {
    return expr(inner, PREC_UNARY);
  }
  return ctr_format("&%s[0]", ctr_lvalue_text(inner, PREC_POSTFIX));
}

char *ctr_assign_value_text(CtrNode *node, CtrType *ty) {
  CtrNode *saved = ctr_em->bare_node;
  ctr_em->bare_node = node;
  char *t = ctr_value_text(node, ty, PREC_LOWEST);
  ctr_em->bare_node = saved;
  return t;
}

static int is_null_constant(CtrNode *n) {
  n = strip_macro(n);
  while (n->kind == ND_CAST) {
    n = strip_macro(n->lhs);
  }
  return n->kind == ND_NUM && !n->is_float_lit && n->ival == 0;
}

static char *to_bool_text(CtrNode *inner) {
  if (is_bool_node(inner) || inner->ty->kind == TY_BOOL) {
    return ctr_cond_text(inner);
  }
  if (ctr_is_flonum(inner->ty)) {
    return ctr_format("%s != 0.0", expr(inner, PREC_EQ + 1));
  }
  return ctr_format("%s != 0", expr(inner, PREC_EQ + 1));
}

static int is_pure_literal(CtrNode *n) {
  if (n->kind == ND_NUM) {
    return !n->is_float_lit && !n->var;
  }
  if (n->kind == ND_NEG || n->kind == ND_POS || n->kind == ND_BITNOT) {
    return is_pure_literal(n->lhs);
  }
  if (n->kind == ND_CAST && n->implicit) {
    return is_pure_literal(n->lhs);
  }
  return 0;
}

static int cast_literal(CtrNode *n) {
  CtrNode *s = strip_macro(n);
  while (s->kind == ND_CAST && ctr_is_integer(s->ty)) {
    s = strip_macro(s->lhs);
  }
  return is_pure_literal(s);
}

static char *numeric_cast(CtrNode *inner, CtrType *to, int prec) {
  CtrNode *s = strip_macro(inner);
  int64_t folded;
  if (ctr_is_integer(to) && to->kind != TY_BOOL && s->kind != ND_NUM && is_pure_literal(s) &&
      ctr_try_eval(s, &folded)) {
    CtrNode num = {0};
    num.kind = ND_NUM;
    num.ival = wrap_value(folded, to);
    num.ty = to;
    char *t = ctr_literal_text(&num, to);
    return num.ival < 0 ? paren(t, prec, PREC_UNARY + 1) : t;
  }
  if (ctr_is_integer(to) && ctr_is_integer(inner->ty) && to->size < inner->ty->size &&
      (wrap_op(s->kind) || s->kind == ND_NEG || s->kind == ND_BITNOT)) {
    char *narrow = ctr_narrow_text(inner, to, prec);
    if (narrow) {
      return narrow;
    }
  }
  if (s->kind == ND_NUM && !s->is_float_lit && ctr_is_integer(to) && fits(s->ival, to)) {
    if (s->var && s->var->is_enum_const && s->var->mettle_name && ctr_same_mettle_type(to, ctr_ty_int)) {
      return (char *)ctr_mettle_name_of(s->var);
    }
    if (inner->kind == ND_MACRO && inner != s) {
      char *t = expr(inner, PREC_POSTFIX);
      if (ctr_same_mettle_type(to, inner->ty)) {
        return t;
      }
      return paren(ctr_format("(%s)%s", ctr_type_text(to), t), prec, PREC_UNARY);
    }
    return ctr_literal_text(s, to);
  }
  if (s->kind == ND_NUM && !s->is_float_lit && ctr_is_integer(to) && to->kind != TY_BOOL) {
    CtrNode num = *s;
    num.ival = wrap_value(s->ival, to);
    num.ty = to;
    num.spelling = NULL;
    num.var = NULL;
    char *t = ctr_literal_text(&num, to);
    return num.ival < 0 ? paren(t, prec, PREC_UNARY + 1) : t;
  }
  if (s->kind == ND_NUM && ctr_is_flonum(to) && inner == s) {
    return ctr_literal_text(s, to);
  }
  if (ctr_is_bool_type(inner->ty) || is_bool_node(inner)) {
    return paren(ctr_format("(%s)(%s)", ctr_type_text(to), ctr_cond_text(inner)), prec, PREC_UNARY);
  }
  if (ctr_same_mettle_type(inner->ty, to)) {
    return expr(inner, prec);
  }
  return paren(ctr_format("(%s)%s", ctr_type_text(to), expr(inner, PREC_UNARY)), prec, PREC_UNARY);
}

static char *pointer_cast(CtrNode *node, CtrNode *inner, CtrType *to, int prec) {
  if (is_null_constant(inner) && ctr_is_integer(strip_macro(inner)->ty ? strip_macro(inner)->ty : ctr_ty_int)) {
    return "0";
  }
  if (inner->ty->kind == TY_ARRAY || inner->ty->kind == TY_VLA || inner->ty->kind == TY_FUNC) {
    char *t = decay_text(inner, to, node == ctr_em->bare_node);
    CtrType *natural = ctr_pointer_to(inner->ty->kind == TY_FUNC ? inner->ty : inner->ty->base);
    int same = strip_macro(inner)->kind == ND_STR || ctr_same_mettle_type(natural, to);
    if (same || (strcmp(ctr_type_text(to), "rawptr") == 0 && node->implicit)) {
      return paren(t, prec, t[0] == '"' ? PREC_POSTFIX : PREC_UNARY);
    }
    return paren(ctr_format("(%s)%s", ctr_type_text(to), paren(t, PREC_UNARY, PREC_UNARY)), prec, PREC_UNARY);
  }
  if (ctr_same_mettle_type(inner->ty, to)) {
    return expr(inner, prec);
  }
  const char *from = ctr_type_text(inner->ty);
  if (node->implicit && (strcmp(from, "rawptr") == 0 || strcmp(ctr_type_text(to), "rawptr") == 0) &&
      inner->ty->kind == TY_PTR) {
    return expr(inner, prec);
  }
  return paren(ctr_format("(%s)%s", ctr_type_text(to), expr(inner, PREC_UNARY)), prec, PREC_UNARY);
}

static char *cast_text(CtrNode *node, int prec) {
  CtrNode *inner = node->lhs;
  CtrType *to = node->ty;
  if (to->kind == TY_VOID) {
    return expr(inner, prec);
  }
  if (to->kind == TY_BOOL) {
    char *b = to_bool_text(inner);
    return paren(b, prec, is_bool_node(inner) || inner->ty->kind == TY_BOOL ? PREC_LOGOR : PREC_EQ);
  }
  if (to->kind == TY_PTR) {
    return pointer_cast(node, inner, to, prec);
  }
  if (to->kind == TY_STRUCT || to->kind == TY_UNION) {
    return expr(inner, prec);
  }
  if (inner->ty->kind == TY_PTR || inner->ty->kind == TY_ARRAY || inner->ty->kind == TY_FUNC) {
    char *src = inner->ty->kind == TY_PTR ? expr(inner, PREC_UNARY)
                                          : decay_text(inner, ctr_pointer_to(ctr_ty_char), 0);
    return paren(ctr_format("(%s)%s", ctr_type_text(to), paren(src, PREC_UNARY, PREC_UNARY)), prec, PREC_UNARY);
  }
  return numeric_cast(inner, to, prec);
}

char *ctr_value_text(CtrNode *node, CtrType *ty, int prec) {
  if (!ty || ctr_same_mettle_type(node->ty, ty) || ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    return expr(node, prec);
  }
  CtrNode tmp = {0};
  tmp.kind = ND_CAST;
  tmp.lhs = node;
  tmp.ty = ty;
  tmp.cast_ty = ty;
  tmp.implicit = 1;
  tmp.tok = node->tok;
  return cast_text(&tmp, prec);
}

char *ctr_cond_text(CtrNode *node) {
  CtrNode *s = strip_macro(node);
  switch (s->kind) {
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE: {
    int p = binop_prec(s->kind);
    CtrNode *a = s->lhs, *b = s->rhs;
    comparison_operands(s, &a, &b);
    char *l = a == s->lhs ? ctr_value_text(a, s->lhs->ty, p) : expr(a, p);
    char *r = b == s->rhs ? ctr_value_text(b, s->rhs->ty, p + 1) : expr(b, p + 1);
    if (s->lhs->ty->kind == TY_PTR && is_null_constant(s->rhs)) {
      r = "0";
    }
    int ordering = s->kind != ND_EQ && s->kind != ND_NE;
    if (ordering && s->lhs->ty->kind == TY_PTR) {
      l = ctr_format("(uint64)%s", expr(s->lhs, PREC_UNARY));
      r = ctr_format("(uint64)%s", expr(s->rhs, PREC_UNARY));
    }
    return ctr_format("%s %s %s", l, binop_text(s->kind), r);
  }
  case ND_LOGAND:
  case ND_LOGOR: {
    int p = binop_prec(s->kind);
    char *l = paren(ctr_cond_text(s->lhs), p, s->lhs && is_bool_node(s->lhs) ? binop_prec(strip_macro(s->lhs)->kind) : PREC_EQ);
    char *r = paren(ctr_cond_text(s->rhs), p + 1, s->rhs && is_bool_node(s->rhs) ? binop_prec(strip_macro(s->rhs)->kind) : PREC_EQ);
    return ctr_format("%s %s %s", l, binop_text(s->kind), r);
  }
  case ND_NOT: {
    CtrNode *in = s->lhs;
    if (is_bool_node(in) || in->ty->kind == TY_BOOL) {
      return ctr_format("!%s", paren(ctr_cond_text(in), PREC_UNARY, is_bool_node(in) ? binop_prec(strip_macro(in)->kind) : PREC_POSTFIX));
    }
    if (ctr_is_flonum(in->ty)) {
      return ctr_format("%s == 0.0", expr(in, PREC_EQ + 1));
    }
    return ctr_format("%s == 0", expr(in, PREC_EQ + 1));
  }
  default:
    break;
  }
  if (node->ty->kind == TY_BOOL) {
    return expr(node, PREC_LOWEST);
  }
  return to_bool_text(node);
}

static int bool_prec(CtrNode *node) {
  CtrNode *s = strip_macro(node);
  if (is_bool_node(s)) {
    return s->kind == ND_NOT ? PREC_EQ : binop_prec(s->kind);
  }
  return PREC_EQ;
}

static char *bool_as_int(CtrNode *node, int prec) {
  char *c = ctr_cond_text(node);
  (void)bool_prec;
  return paren(ctr_format("(%s)(%s)", ctr_type_text(node->ty), c), prec, PREC_UNARY);
}

static int is_void_ptr(CtrType *t) { return t->kind == TY_PTR && t->base->kind == TY_VOID; }

static char *pointer_add(CtrNode *node, int prec) {
  CtrNode *p = node->lhs;
  CtrNode *n = node->rhs;
  const char *op = node->kind == ND_ADD ? "+" : "-";
  if (is_void_ptr(p->ty)) {
    char *t = ctr_format("(rawptr)((uint8*)%s %s %s)", expr(p, PREC_UNARY), op, expr(n, PREC_ADD + 1));
    return paren(t, prec, PREC_UNARY);
  }
  if (p->ty->kind == TY_PTR && vla_row(p->ty->base)) {
    char *t = ctr_format("%s %s %s * %s", expr(p, PREC_ADD), op, expr(n, PREC_MUL), vla_stride(p->ty->base));
    return paren(t, prec, PREC_ADD);
  }
  char *t = ctr_format("%s %s %s", expr(p, PREC_ADD), op, expr(n, PREC_ADD + 1));
  return paren(t, prec, PREC_ADD);
}

static char *pointer_diff(CtrNode *node, int prec) {
  CtrType *pt = node->lhs->ty;
  char *t;
  if (is_void_ptr(pt)) {
    t = ctr_format("(uint8*)%s - (uint8*)%s", expr(node->lhs, PREC_UNARY), expr(node->rhs, PREC_UNARY));
  } else {
    t = ctr_format("%s - %s", expr(node->lhs, PREC_ADD), expr(node->rhs, PREC_ADD + 1));
  }
  if (pt->kind == TY_PTR && vla_row(pt->base)) {
    t = ctr_format("(%s) / %s", t, vla_stride(pt->base));
  }
  if (ctr_same_mettle_type(node->ty, ctr_ty_llong)) {
    return paren(t, prec, PREC_ADD);
  }
  return paren(ctr_format("(%s)(%s)", ctr_type_text(node->ty), t), prec, PREC_UNARY);
}

static int is_literal(CtrNode *n) {
  n = strip_macro(n);
  return n->kind == ND_NUM && !n->is_float_lit && !(n->var && n->var->is_enum_const);
}

static CtrNode *widening_source(CtrNode *n, CtrType *t) {
  if (n->kind != ND_CAST || !n->implicit || !ctr_is_integer(t) || t->kind == TY_BOOL) {
    return n;
  }
  CtrNode *in = n->lhs;
  CtrType *from = in->ty;
  if (!ctr_is_integer(from) || from->kind == TY_BOOL || is_bool_node(in)) {
    return n;
  }
  if (from->size < t->size && (from->is_unsigned || !t->is_unsigned)) {
    return in;
  }
  if (from->size == t->size) {
    return in;
  }
  return n;
}

static int anchors(CtrNode *n, CtrType *t) {
  return !is_literal(n) && ctr_same_mettle_type(n->ty, t);
}

static int literal_fits(CtrNode *lit, CtrNode *other) {
  CtrNode *s = strip_macro(lit);
  return is_literal(lit) && ctr_is_integer(other->ty) && other->ty->kind != TY_BOOL && fits(s->ival, other->ty);
}

static int unsigned_mix_ok(CtrNode *a, CtrNode *b, CtrType *t) {
  if (!t->is_unsigned) {
    return 1;
  }
  return ctr_same_mettle_type(a->ty, t) || ctr_same_mettle_type(b->ty, t);
}

static void operand_pair(CtrType *t, CtrNode **lhs, CtrNode **rhs) {
  CtrNode *a = widening_source(*lhs, t);
  CtrNode *b = widening_source(*rhs, t);
  if ((anchors(a, t) || anchors(b, t)) && unsigned_mix_ok(a, b, t) && !is_literal(a) && !is_literal(b)) {
    *lhs = a;
    *rhs = b;
    return;
  }
  if (anchors(a, t) && is_literal(b)) {
    *lhs = a;
    return;
  }
  if (anchors(b, t) && is_literal(a)) {
    *rhs = b;
    return;
  }
  if (!is_literal(a) && a != *lhs) {
    *rhs = b != *rhs && !is_literal(b) ? b : *rhs;
    return;
  }
  if (!is_literal(b) && b != *rhs) {
    *lhs = a != *lhs && !is_literal(a) ? a : *lhs;
  }
}

static int comparison_operands(CtrNode *node, CtrNode **lhs, CtrNode **rhs) {
  CtrNode *a = widening_source(node->lhs, node->lhs->ty);
  CtrNode *b = widening_source(node->rhs, node->rhs->ty);
  if (!ctr_is_integer(node->lhs->ty)) {
    return 0;
  }
  if (a != node->lhs && literal_fits(node->rhs, a)) {
    *lhs = a;
    *rhs = node->rhs;
    return 1;
  }
  if (b != node->rhs && literal_fits(node->lhs, b)) {
    *lhs = node->lhs;
    *rhs = b;
    return 1;
  }
  CtrNode *pa = node->lhs, *pb = node->rhs;
  operand_pair(node->lhs->ty, &pa, &pb);
  *lhs = pa;
  *rhs = pb;
  return pa != node->lhs || pb != node->rhs;
}

static char *binary_text(CtrNode *node, int prec) {
  if (is_bool_node(node)) {
    return bool_as_int(node, prec);
  }
  if ((node->kind == ND_ADD || node->kind == ND_SUB) && node->lhs->ty->kind == TY_PTR) {
    if (node->kind == ND_SUB && node->rhs->ty->kind == TY_PTR) {
      return pointer_diff(node, prec);
    }
    return pointer_add(node, prec);
  }
  int p = binop_prec(node->kind);
  CtrNode *a = node->lhs;
  CtrNode *b = node->rhs;
  if (ctr_is_integer(node->ty)) {
    if (node->kind == ND_SHL || node->kind == ND_SHR) {
      b = widening_source(b, b->ty);
    } else {
      operand_pair(node->ty, &a, &b);
    }
  }
  char *l = a == node->lhs ? ctr_value_text(a, node->ty, p) : expr(a, p);
  char *r = b == node->rhs ? ctr_value_text(b, node->ty, p + 1) : expr(b, p + 1);
  int shift = node->kind == ND_SHL || node->kind == ND_SHR;
  int64_t folded;
  int typed = ctr_is_integer(node->ty) && !ctr_same_mettle_type(node->ty, ctr_ty_int);
  if (typed && shift && !is_literal(a) && cast_literal(a) && ctr_try_eval(a, &folded)) {
    CtrNode num = {0};
    num.kind = ND_NUM;
    num.ival = folded;
    num.ty = node->ty;
    l = ctr_format("(%s)%s", ctr_type_text(node->ty), paren(ctr_literal_text(&num, node->ty), PREC_UNARY, PREC_UNARY + 1));
  } else if (typed && is_literal(a) && (shift || is_literal(b))) {
    l = ctr_format("(%s)%s", ctr_type_text(node->ty), paren(l, PREC_UNARY, PREC_UNARY + 1));
  }
  return paren(ctr_format("%s %s %s", l, binop_text(node->kind), r), prec, p);
}

static char *unary_text(CtrNode *node, int prec) {
  char *in = expr(node->lhs, PREC_UNARY);
  switch (node->kind) {
  case ND_NEG:
    if (ctr_is_integer(node->ty) && is_pure_literal(node)) {
      int64_t folded;
      ctr_try_eval(node, &folded);
      CtrNode num = {0};
      num.kind = ND_NUM;
      num.ival = folded;
      num.ty = node->ty;
      char *t = ctr_literal_text(&num, node->ty);
      return folded < 0 ? paren(t, prec, PREC_UNARY + 1) : t;
    }
    if (node->ty->is_unsigned && ctr_is_integer(node->ty)) {
      return paren(ctr_format("0 - %s", expr(node->lhs, PREC_ADD + 1)), prec, PREC_ADD);
    }
    return paren(ctr_format(in[0] == '-' ? "-(%s)" : "-%s", in), prec, PREC_UNARY);
  case ND_BITNOT:
    if (ctr_is_integer(node->ty) && node->ty->is_unsigned && is_pure_literal(node)) {
      int64_t folded;
      ctr_try_eval(node, &folded);
      CtrNode num = {0};
      num.kind = ND_NUM;
      num.ival = wrap_value(folded, node->ty);
      num.ty = node->ty;
      num.spelling = ctr_format("0x%llX", (unsigned long long)num.ival);
      return ctr_literal_text(&num, node->ty);
    }
    return paren(ctr_format("~%s", in), prec, PREC_UNARY);
  default:
    return expr(node->lhs, prec);
  }
}

static int lvalue_is_simple(CtrNode *n) {
  n = strip_macro(n);
  if (n->kind == ND_VAR) {
    return 1;
  }
  if (n->kind == ND_MEMBER) {
    return lvalue_is_simple(n->lhs);
  }
  if (n->kind == ND_DEREF) {
    return !ctr_has_side_effects(n->lhs);
  }
  if (n->kind == ND_INDEX) {
    return !ctr_has_side_effects(n->lhs) && !ctr_has_side_effects(n->rhs);
  }
  return 0;
}

static int is_bitfield(CtrNode *n) {
  n = strip_macro(n);
  return n->kind == ND_MEMBER && n->member && n->member->emit_kind == CTR_MEMBER_BITFIELD;
}

typedef struct {
  CtrNode *node;
  int want_value;
} SideEffectArg;

static void emit_side_effect(void *arg) {
  SideEffectArg *a = arg;
  ctr_emit_expr_stmt(a->node);
}

static char *read_back(CtrNode *lhs, CtrType *ty) {
  if (is_bitfield(lhs)) {
    return ctr_bitfield_read(strip_macro(lhs));
  }
  return ctr_value_text(lhs, ty, PREC_LOWEST);
}

typedef struct {
  CtrNode *node;
  const char *pointer;
  CtrNode *through;
} AssignThrough;

static void emit_assign_through(void *arg) {
  AssignThrough *a = arg;
  ctr_line("var %s = &%s;", a->pointer, ctr_lvalue_text(a->node->lhs, PREC_POSTFIX));
  ctr_emit_expr_stmt(a->through);
}

static char *assign_value(CtrNode *node, int prec) {
  CtrNode *lhs = node->lhs;
  if (lvalue_is_simple(lhs) || is_bitfield(lhs)) {
    SideEffectArg a = {node, 1};
    return paren(stmt_block_text(emit_side_effect, &a, read_back(lhs, node->ty)), prec, PREC_POSTFIX);
  }
  CtrObj *pointer = ctr_alloc(sizeof(CtrObj));
  pointer->name = ctr_new_temp("at");
  pointer->mettle_name = pointer->name;
  pointer->ty = ctr_pointer_to(lhs->ty);
  pointer->is_local = 1;
  CtrNode *ref = ctr_new_node(ND_VAR, node->tok);
  ref->var = pointer;
  ref->ty = pointer->ty;
  CtrNode *target = ctr_new_node(ND_DEREF, node->tok);
  target->lhs = ref;
  target->ty = lhs->ty;
  CtrNode *through = ctr_alloc(sizeof(CtrNode));
  *through = *node;
  through->lhs = target;
  AssignThrough a = {node, pointer->name, through};
  return paren(stmt_block_text(emit_assign_through, &a, read_back(target, node->ty)), prec, PREC_POSTFIX);
}

static char *incdec_value(CtrNode *node, int prec) {
  CtrNode *lhs = node->lhs;
  int post = node->kind == ND_POSTINC || node->kind == ND_POSTDEC;
  SideEffectArg a = {node, 1};
  if (!post) {
    return paren(stmt_block_text(emit_side_effect, &a, read_back(lhs, lhs->ty)), prec, PREC_POSTFIX);
  }
  char *t = ctr_new_temp("old");
  CtrBuf b = {0};
  ctr_buf_printf(&b, "{ var %s: %s = %s; ", t, ctr_type_text(lhs->ty), read_back(lhs, lhs->ty));
  char *stmt = stmt_block_text(emit_side_effect, &a, NULL);
  ctr_buf_put(&b, stmt);
  ctr_buf_printf(&b, " %s }", t);
  return paren(ctr_buf_take(&b), prec, PREC_POSTFIX);
}

static char *cond_expr_text(CtrNode *node, int prec) {
  int64_t known;
  if (node->op != 1 && node->ty->kind != TY_VOID && !ctr_has_side_effects(node->cond) &&
      ctr_try_eval(node->cond, &known)) {
    return ctr_value_text(known ? node->then : node->els, node->ty, prec);
  }
  if (node->ty->kind == TY_VOID) {
    SideEffectArg a = {node, 0};
    return paren(stmt_block_text(emit_side_effect, &a, NULL), prec, PREC_POSTFIX);
  }
  if (node->op == 1) {
    char *t = ctr_new_temp("cv");
    char *v = ctr_value_text(node->cond, node->ty, PREC_LOWEST);
    char *e = ctr_value_text(node->els, node->ty, PREC_LOWEST);
    char *test = ctr_format(ctr_is_flonum(node->ty) ? "%s != 0.0" : "%s != 0", t);
    return paren(ctr_format("{ var %s: %s = %s; if (%s) %s else %s }", t, ctr_type_text(node->ty), v, test, t, e),
                 prec, PREC_POSTFIX);
  }
  char *c = ctr_cond_text(node->cond);
  char *a = ctr_value_text(node->then, node->ty, PREC_LOWEST);
  char *b = ctr_value_text(node->els, node->ty, PREC_LOWEST);
  return paren(ctr_format("if (%s) %s else %s", c, a, b), prec, PREC_IFEXPR + 1);
}

static char *comma_text(CtrNode *node, int prec) {
  SideEffectArg a = {node->lhs, 0};
  char *tail = ctr_value_text(node->rhs, node->ty, PREC_LOWEST);
  if (node->ty->kind == TY_VOID) {
    CtrBuf b = {0};
    ctr_buf_put(&b, stmt_block_text(emit_side_effect, &a, NULL));
    SideEffectArg r = {node->rhs, 0};
    ctr_buf_put(&b, stmt_block_text(emit_side_effect, &r, NULL));
    return ctr_buf_take(&b);
  }
  return paren(stmt_block_text(emit_side_effect, &a, tail), prec, PREC_POSTFIX);
}

static int is_union_member(CtrNode *n) {
  return n->kind == ND_MEMBER && n->member && n->member->emit_kind == CTR_MEMBER_CAST;
}

static char *member_base_address(CtrNode *base) {
  CtrNode *s = strip_macro(base);
  if (s->kind == ND_DEREF) {
    return expr(s->lhs, PREC_UNARY);
  }
  return ctr_format("&%s", ctr_lvalue_text(base, PREC_POSTFIX));
}

static char *union_member_text(CtrNode *node, int prec) {
  CtrMember *mem = node->member;
  char *addr = member_base_address(node->lhs);
  if (mem->ty->kind == TY_ARRAY) {
    const char *elem = ctr_type_text(ctr_pointer_to(mem->ty->base));
    char *t = mem->offset ? ctr_format("((%s)((uint8*)%s + %d))", elem, paren(addr, PREC_UNARY, PREC_UNARY), mem->offset)
                          : ctr_format("((%s)%s)", elem, paren(addr, PREC_UNARY, PREC_UNARY));
    return paren(t, prec, PREC_POSTFIX);
  }
  char *t;
  if (mem->offset) {
    t = ctr_format("(*(%s*)((uint8*)%s + %d))", ctr_type_text(mem->ty), paren(addr, PREC_UNARY, PREC_UNARY), mem->offset);
  } else {
    t = ctr_format("(*(%s*)%s)", ctr_type_text(mem->ty), paren(addr, PREC_UNARY, PREC_UNARY));
  }
  return paren(t, prec, PREC_POSTFIX);
}

static CtrNode *decayed_array(CtrNode *n) {
  CtrNode *s = strip_macro(n);
  while (s->kind == ND_CAST && s->implicit && s->lhs->ty->kind == TY_ARRAY) {
    s = strip_macro(s->lhs);
  }
  return s->ty->kind == TY_ARRAY ? s : NULL;
}

static char *member_text(CtrNode *node, int prec) {
  if (is_bitfield(node)) {
    return paren(ctr_bitfield_read(node), prec, PREC_UNARY);
  }
  if (is_union_member(node)) {
    return union_member_text(node, prec);
  }
  CtrNode *base = strip_macro(node->lhs);
  if (base->kind == ND_DEREF && decayed_array(base->lhs)) {
    return ctr_format("%s[0].%s", ctr_lvalue_text(decayed_array(base->lhs), PREC_POSTFIX), ctr_member_name(node->member));
  }
  if (base->kind == ND_DEREF && base->op == 1) {
    return ctr_format("%s->%s", expr(base->lhs, PREC_POSTFIX), ctr_member_name(node->member));
  }
  return ctr_format("%s.%s", ctr_lvalue_text(node->lhs, PREC_POSTFIX), ctr_member_name(node->member));
}

char *ctr_lvalue_text(CtrNode *node, int prec) {
  CtrNode *s = strip_macro(node);
  switch (s->kind) {
  case ND_VAR:
    return (char *)ctr_mettle_name_of(s->var);
  case ND_DEREF:
    if (vla_row(s->ty)) {
      return expr(s->lhs, prec);
    }
    return paren(ctr_format("*%s", expr(s->lhs, PREC_UNARY)), prec, PREC_UNARY);
  case ND_CAST:
    if (s->implicit && ctr_same_mettle_type(s->ty, s->lhs->ty)) {
      return ctr_lvalue_text(s->lhs, prec);
    }
    return expr(s, prec);
  case ND_COMPOUND_LIT:
    return (char *)ctr_mettle_name_of(s->var);
  default:
    return expr(node, prec);
  }
}

static char *addr_text(CtrNode *node, int prec) {
  CtrNode *in = strip_macro(node->lhs);
  if (in->kind == ND_DEREF) {
    return ctr_value_text(in->lhs, node->ty, prec);
  }
  if (in->kind == ND_VAR && in->var->is_function) {
    return function_address(in->var);
  }
  if (vla_row(in->ty)) {
    return expr(in, prec);
  }
  if (is_union_member(in)) {
    char *addr = member_base_address(in->lhs);
    char *t = in->member->offset ? ctr_format("(%s)((uint8*)%s + %d)", ctr_type_text(node->ty), paren(addr, PREC_UNARY, PREC_UNARY), in->member->offset)
                                 : ctr_format("(%s)%s", ctr_type_text(node->ty), paren(addr, PREC_UNARY, PREC_UNARY));
    return paren(t, prec, PREC_UNARY);
  }
  if (in->kind == ND_STR) {
    return ctr_string_literal_text(in->str, in->str_len, in->str_ty->base->size);
  }
  return paren(ctr_format("&%s", ctr_lvalue_text(in, PREC_POSTFIX)), prec, PREC_UNARY);
}

CtrType *ctr_vla_element(CtrType *ty);
int ctr_has_vla(CtrType *ty);

static char *vla_count(CtrType *row, CtrType *as) {
  CtrBuf b = {0};
  int first = 1;
  for (CtrType *t = row; t->kind == TY_VLA || t->kind == TY_ARRAY; t = t->base) {
    char *n = t->kind == TY_VLA ? ctr_value_text(t->vla_len, as, PREC_MUL + 1)
                                : ctr_format("%lld", (long long)t->array_len);
    ctr_buf_printf(&b, "%s%s", first ? "" : " * ", n);
    first = 0;
  }
  return ctr_buf_take(&b);
}

static char *vla_stride(CtrType *row) { return vla_count(row, ctr_ty_llong); }

static int vla_indexed(CtrNode *base) {
  CtrType *t = base->ty;
  return t->kind == TY_VLA || (t->kind == TY_PTR && ctr_has_vla(t->base)) ||
         (t->kind == TY_ARRAY && ctr_has_vla(t));
}

static int indexes_past_trailing_array(CtrNode *base, CtrNode *index) {
  CtrNode *s = strip_macro(base);
  if (s->kind != ND_MEMBER || !s->member || s->member->next || s->ty->kind != TY_ARRAY || s->ty->array_len > 1) {
    return 0;
  }
  if (s->member->emit_kind == CTR_MEMBER_CAST) {
    return 0;
  }
  int64_t v;
  return !ctr_try_eval(index, &v) || v != 0;
}

static char *index_text(CtrNode *node, int prec) {
  (void)prec;
  CtrNode *base = node->lhs;
  char *idx = expr(node->rhs, PREC_LOWEST);
  if (vla_indexed(base)) {
    char *flat = expr(base, PREC_ADD);
    if (node->ty->kind == TY_VLA || node->ty->kind == TY_ARRAY) {
      int64_t row;
      if (ctr_try_eval(node->rhs, &row) && row == 0) {
        return expr(base, prec);
      }
      if (ctr_try_eval(node->rhs, &row) && row == 1) {
        return ctr_format("(%s + %s)", flat, vla_stride(node->ty));
      }
      return ctr_format("(%s + %s * %s)", flat, expr(node->rhs, PREC_MUL), vla_stride(node->ty));
    }
    return ctr_format("%s[%s]", strchr(flat, ' ') && flat[0] != '(' ? ctr_format("(%s)", flat) : flat, idx);
  }
  if (indexes_past_trailing_array(base, node->rhs)) {
    return ctr_format("(&%s[0])[%s]", ctr_lvalue_text(base, PREC_POSTFIX), idx);
  }
  if (base->ty->kind == TY_ARRAY) {
    return ctr_format("%s[%s]", ctr_lvalue_text(base, PREC_POSTFIX), idx);
  }
  return ctr_format("%s[%s]", expr(base, PREC_POSTFIX), idx);
}

static char *sizeof_text(CtrNode *node, int prec) {
  CtrType *t = node->cast_ty;
  if (node->ival < 0) {
    CtrType *elem = t;
    while (elem->kind == TY_VLA || elem->kind == TY_ARRAY) {
      elem = elem->base;
    }
    return paren(ctr_format("%s * sizeof(%s)", vla_count(t, ctr_ty_ullong), ctr_type_text(elem)), prec, PREC_MUL);
  }
  int named = t && (t->kind == TY_STRUCT || t->kind == TY_UNION || ctr_is_scalar(t)) && !ctr_is_opaque_struct(t) &&
              t->kind != TY_ENUM;
  if (named && t->kind != TY_STRUCT && t->kind != TY_UNION) {
    return paren(ctr_format("sizeof(%s)", ctr_type_text(t)), prec, PREC_POSTFIX);
  }
  if (named) {
    return paren(ctr_format("sizeof(%s)", ctr_type_text(t)), prec, PREC_POSTFIX);
  }
  return ctr_format("%lld", (long long)node->ival);
}

const char *ctr_module_prefix(const char *module) {
  if (strcmp(module, "std/c") == 0) {
    ctr_em->mod->needs_c = 1;
    return "libc.";
  }
  if (strcmp(module, "std/math") == 0) {
    ctr_em->mod->needs_math = 1;
    return "math.";
  }
  return "";
}

const char *ctr_callee_text(CtrNode *fn_node, CtrType *fnty, int *is_mettle_variadic, CtrObj **callee) {
  CtrNode *s = strip_macro(fn_node);
  while (s->kind == ND_CAST && s->implicit) {
    s = strip_macro(s->lhs);
  }
  while (s->kind == ND_DEREF && s->ty->kind == TY_FUNC) {
    s = strip_macro(s->lhs);
    while (s->kind == ND_CAST && s->implicit) {
      s = strip_macro(s->lhs);
    }
  }
  *callee = NULL;
  *is_mettle_variadic = fnty->is_variadic;
  if (s->kind == ND_VAR && s->var->is_function) {
    CtrObj *f = s->var;
    *callee = f;
    if (f->mettle_module) {
      return ctr_format("%s%s", ctr_module_prefix(f->mettle_module), f->mettle_name);
    }
    return ctr_mettle_name_of(f);
  }
  if (s->kind == ND_CAST && is_null_constant(s->lhs)) {
    return ctr_format("((%s)(rawptr)0)", ctr_type_text(s->ty));
  }
  return expr(s, PREC_POSTFIX);
}


static int is_default_promotion(CtrNode *cast) {
  if (cast->kind != ND_CAST || !cast->implicit) {
    return 0;
  }
  CtrType *from = cast->lhs->ty;
  CtrType *to = cast->ty;
  if (from->kind == TY_FLOAT && to->kind == TY_DOUBLE) {
    return 1;
  }
  return ctr_is_integer(from) && (from->size < 4 || from->kind == TY_ENUM) && to->kind == TY_INT;
}

static char *c_vararg_text(CtrNode *arg) {
  if (is_default_promotion(arg)) {
    CtrNode *inner = arg->lhs;
    if (is_bool_node(inner)) {
      return ctr_cond_text(inner);
    }
    return expr(inner, PREC_LOWEST);
  }
  CtrNode *saved = ctr_em->bare_node;
  ctr_em->bare_node = arg;
  char *t = expr(arg, PREC_LOWEST);
  ctr_em->bare_node = saved;
  return t;
}

static int param_is_va_list(CtrType *p) { return p && ctr_is_va_list(p); }

static int is_aggregate(CtrType *t) { return t->kind == TY_STRUCT || t->kind == TY_UNION; }

static char *call_site_type(CtrNode *call) {
  CtrBuf b = {0};
  ctr_buf_put(&b, "fn(");
  for (CtrNode *arg = call->args; arg; arg = arg->next) {
    ctr_buf_printf(&b, "%s%s", arg == call->args ? "" : ", ", ctr_type_text(arg->ty));
  }
  CtrType *rt = call->func_ty->return_ty;
  ctr_buf_printf(&b, ") -> %s", rt->kind == TY_VOID ? "void" : ctr_return_type_text(rt));
  return ctr_buf_take(&b);
}

static char *call_text(CtrNode *node, int prec) {
  int mettle_variadic;
  CtrObj *callee;
  const char *fn = ctr_callee_text(node->lhs, node->func_ty, &mettle_variadic, &callee);
  if (!callee && !node->func_ty->has_proto && node->args) {
    fn = ctr_format("((%s)%s)", call_site_type(node), fn);
  }
  CtrBuf b = {0};
  ctr_buf_printf(&b, "%s(", fn);
  CtrType *param = node->func_ty->params;
  int first = 1;
  int extern_c = callee && !callee->is_definition && !callee->mettle_module && !callee->skip_emit;
  for (CtrNode *arg = node->args; arg; arg = arg->next) {
    if (!first) {
      ctr_buf_put(&b, ", ");
    }
    first = 0;
    if (param) {
      if (extern_c && param_is_va_list(param)) {
        ctr_em->mod->needs_c = 1;
        ctr_buf_printf(&b, "libc.va_native(&%s)", ctr_lvalue_text(arg, PREC_POSTFIX));
      } else {
        ctr_buf_put(&b, ctr_assign_value_text(arg, param));
      }
      param = param->next;
    } else if (mettle_variadic && callee && !extern_c && is_aggregate(arg->ty)) {
      ctr_buf_printf(&b, "&%s", ctr_lvalue_text(arg, PREC_POSTFIX));
    } else if (mettle_variadic) {
      ctr_buf_put(&b, c_vararg_text(arg));
    } else {
      ctr_buf_put(&b, expr(arg, PREC_LOWEST));
    }
  }
  ctr_buf_putc(&b, ')');
  (void)prec;
  return ctr_buf_take(&b);
}

static char *va_text(CtrNode *node, int prec) {
  ctr_em->mod->needs_c = 1;
  char *ap = ctr_lvalue_text(node->lhs, PREC_POSTFIX);
  CtrType *t = node->ty;
  if (ctr_is_flonum(t)) {
    char *d = ctr_format("libc.va_double(&%s)", ap);
    if (t->kind == TY_FLOAT) {
      return paren(ctr_format("(float32)%s", d), prec, PREC_UNARY);
    }
    return d;
  }
  if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
    return ctr_format("*(%s*)libc.va_next(&%s)", ctr_type_text(t), ap);
  }
  if (ctr_same_mettle_type(t, ctr_ty_ullong)) {
    return ctr_format("libc.va_next(&%s)", ap);
  }
  return paren(ctr_format("(%s)libc.va_next(&%s)", ctr_type_text(t), ap), prec, PREC_UNARY);
}

static const char *long_variant(const char *base) {
  return ctr_ty_long->size == 8 ? ctr_format("%s64", base) : ctr_format("%s32", base);
}

static const char *builtin_target(const char *name, CtrType **arg_ty) {
  static const struct {
    const char *c_name;
    const char *base;
    int width;
  } table[] = {{"popcount", "popcount", 32}, {"popcountl", "popcount", 0}, {"popcountll", "popcount", 64},
               {"clz", "clz", 32},           {"clzl", "clz", 0},           {"clzll", "clz", 64},
               {"ctz", "ctz", 32},           {"ctzl", "ctz", 0},           {"ctzll", "ctz", 64},
               {"parity", "parity", 32},     {"ffs", "ffs", 32},           {"abs", "abs", 32}};
  for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++) {
    if (strcmp(name, table[i].c_name) != 0) {
      continue;
    }
    int width = table[i].width ? table[i].width : ctr_ty_long->size * 8;
    int is_signed = strcmp(table[i].base, "abs") == 0 || strcmp(table[i].base, "ffs") == 0;
    *arg_ty = width == 64 ? (is_signed ? ctr_ty_llong : ctr_ty_ullong) : (is_signed ? ctr_ty_int : ctr_ty_uint);
    return table[i].width ? ctr_format("%s%d", table[i].base, width) : long_variant(table[i].base);
  }
  if (strcmp(name, "bswap16") == 0 || strcmp(name, "bswap32") == 0 || strcmp(name, "bswap64") == 0) {
    *arg_ty = name[5] == '1' ? ctr_ty_ushort : name[5] == '3' ? ctr_ty_uint : ctr_ty_ullong;
    return name;
  }
  if (strcmp(name, "alloca") == 0) {
    *arg_ty = ctr_ty_ullong;
    return "vla_alloc";
  }
  return NULL;
}

static char *builtin_text(CtrNode *node, int prec) {
  ctr_em->mod->needs_c = 1;
  const char *name = node->spelling + strlen("__builtin_");
  CtrBuf b = {0};
  if (node->builtin == 16 || node->builtin == 17) {
    ctr_em->mod->needs_math = 1;
    return "math.INF()";
  }
  if (node->builtin == 18 || node->builtin == 19) {
    ctr_em->mod->needs_math = 1;
    return "(float32)math.INF()";
  }
  if (node->builtin == 20) {
    ctr_em->mod->needs_math = 1;
    return "math.NAN()";
  }
  if (node->builtin == 21) {
    ctr_em->mod->needs_math = 1;
    return "(float32)math.NAN()";
  }
  if (node->builtin == 29) {
    return expr(node->args, prec);
  }
  CtrType *arg_ty = NULL;
  const char *target = builtin_target(name, &arg_ty);
  if (!target) {
    ctr_error_tok(node->tok, "'%s' has no Mettle translation", node->spelling);
    return "0";
  }
  ctr_buf_printf(&b, "libc.%s(", target);
  int first = 1;
  for (CtrNode *a = node->args; a; a = a->next) {
    if (!first) {
      ctr_buf_put(&b, ", ");
    }
    first = 0;
    ctr_buf_put(&b, arg_ty ? ctr_value_text(a, arg_ty, PREC_LOWEST) : expr(a, PREC_LOWEST));
  }
  ctr_buf_putc(&b, ')');
  return ctr_buf_take(&b);
}

static void emit_stmt_expr_body(void *arg) {
  CtrNode *node = arg;
  CtrNode *last = node->body;
  while (last && last->next) {
    last = last->next;
  }
  for (CtrNode *s = node->body; s && s != last; s = s->next) {
    ctr_emit_stmt(s);
  }
  if (last && !(last->kind == ND_EXPR_STMT && node->ty->kind != TY_VOID)) {
    ctr_emit_stmt(last);
  }
}

static char *stmt_expr_text(CtrNode *node, int prec) {
  CtrNode *last = node->body;
  while (last && last->next) {
    last = last->next;
  }
  char *tail = NULL;
  if (node->ty->kind != TY_VOID && last && last->kind == ND_EXPR_STMT) {
    tail = expr(last->lhs, PREC_LOWEST);
  }
  return paren(stmt_block_text(emit_stmt_expr_body, node, tail), prec, PREC_POSTFIX);
}

static char *macro_text(CtrNode *node, int prec) {
  CtrMacro *m = node->macro && node->macro->macro ? node->macro->macro : NULL;
  if (!m || !m->recovered) {
    return expr(node->lhs, prec);
  }
  ctr_use_module(m->home);
  return (char *)m->mettle_name;
}

static char *var_text(CtrNode *node) {
  CtrObj *v = node->var;
  if (v->is_function) {
    return function_address(v);
  }
  if (v->mettle_module) {
    return ctr_format("%s%s", ctr_module_prefix(v->mettle_module), v->mettle_name);
  }
  return (char *)ctr_mettle_name_of(v);
}

static char *expr_more(CtrNode *node, int prec) {
  switch (node->kind) {
  case ND_FUNCALL:
    return call_text(node, prec);
  case ND_VA_ARG:
    return va_text(node, prec);
  case ND_BUILTIN:
    return builtin_text(node, prec);
  case ND_STMT_EXPR:
    return stmt_expr_text(node, prec);
  case ND_COMPOUND_LIT:
    return (char *)ctr_mettle_name_of(node->var);
  case ND_ASSIGN:
    return assign_value(node, prec);
  case ND_OPASSIGN:
    return assign_value(node, prec);
  case ND_PREINC:
  case ND_PREDEC:
  case ND_POSTINC:
  case ND_POSTDEC:
    return node->print_plain ? ctr_lvalue_text(node->lhs, prec) : incdec_value(node, prec);
  case ND_COND:
    return cond_expr_text(node, prec);
  case ND_COMMA:
    return comma_text(node, prec);
  case ND_LABEL_VAL:
    return ctr_format("(rawptr)%d", node->label_ref && node->label_ref->node ? node->label_ref->node->flow_index : 0);
  case ND_VA_START:
  case ND_VA_END:
  case ND_VA_COPY: {
    SideEffectArg a = {node, 0};
    return stmt_block_text(emit_side_effect, &a, NULL);
  }
  default:
    ctr_error_tok(node->tok, "this expression has no Mettle translation");
    return "0";
  }
}

static char *expr(CtrNode *node, int prec) {
  switch (node->kind) {
  case ND_NUM:
    return ctr_literal_text(node, node->ty);
  case ND_STR:
    if (node->str_ty->base->size != 1) {
      return (char *)ctr_wide_string_name(node);
    }
    return ctr_string_literal_text(node->str, node->str_len, node->str_ty->base->size);
  case ND_VAR:
    return var_text(node);
  case ND_MACRO:
    return macro_text(node, prec);
  case ND_CAST:
    return cast_text(node, prec);
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR:
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
  case ND_LOGAND:
  case ND_LOGOR:
    return binary_text(node, prec);
  case ND_NOT:
    return bool_as_int(node, prec);
  case ND_NEG:
  case ND_POS:
  case ND_BITNOT:
    return unary_text(node, prec);
  case ND_MEMBER:
    return member_text(node, prec);
  case ND_ADDR:
    return addr_text(node, prec);
  case ND_DEREF:
    if (node->ty->kind == TY_FUNC || vla_row(node->ty)) {
      return expr(node->lhs, prec);
    }
    return paren(ctr_format("*%s", expr(node->lhs, PREC_UNARY)), prec, PREC_UNARY);
  case ND_INDEX:
    return index_text(node, prec);
  case ND_SIZEOF:
  case ND_ALIGNOF:
    return node->kind == ND_SIZEOF ? sizeof_text(node, prec) : ctr_format("%lld", (long long)node->ival);
  default:
    return expr_more(node, prec);
  }
}

char *ctr_expr_text(CtrNode *node, int prec) { return expr(node, prec); }

static char *stmt_block_text(void (*emit)(void *), void *arg, const char *tail) {
  CtrBuf *saved = ctr_em->out;
  int saved_indent = ctr_em->indent;
  CtrBuf buf = {0};
  ctr_em->out = &buf;
  ctr_em->indent = 0;
  emit(arg);
  ctr_em->out = saved;
  ctr_em->indent = saved_indent;
  char *body = ctr_buf_take(&buf);
  CtrBuf out = {0};
  ctr_buf_put(&out, "{ ");
  for (char *p = body; *p; p++) {
    if (*p == '\n') {
      if (p[1]) {
        ctr_buf_putc(&out, ' ');
      }
      continue;
    }
    ctr_buf_putc(&out, *p);
  }
  if (tail) {
    if (out.len > 2) {
      ctr_buf_putc(&out, ' ');
    }
    ctr_buf_put(&out, tail);
  }
  ctr_buf_put(&out, out.data[out.len - 1] == ' ' ? "}" : " }");
  return ctr_buf_take(&out);
}

