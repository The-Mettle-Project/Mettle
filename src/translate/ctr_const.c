#include "translate/ctr_parse.h"

static int eval_int(CtrNode *node, int64_t *out);

static int64_t wrap_to(int64_t v, CtrType *ty) {
  if (!ty || !ctr_is_integer(ty)) {
    return v;
  }
  if (ty->kind == TY_BOOL) {
    return v != 0;
  }
  switch (ty->size) {
  case 1:
    return ty->is_unsigned ? (int64_t)(uint8_t)v : (int64_t)(int8_t)v;
  case 2:
    return ty->is_unsigned ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v;
  case 4:
    return ty->is_unsigned ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
  default:
    return v;
  }
}

static int is_unsigned_node(CtrNode *node) { return node->ty && node->ty->is_unsigned; }

static int eval_double(CtrNode *node, double *out);

static int eval_cast(CtrNode *node, int64_t *out) {
  CtrType *ty = node->cast_ty ? node->cast_ty : node->ty;
  CtrNode *src = node->lhs;
  ctr_add_type(src);
  if (ctr_is_flonum(src->ty)) {
    double d;
    if (!eval_double(src, &d)) {
      return 0;
    }
    if (ty->is_unsigned && ty->size == 8) {
      *out = (int64_t)(uint64_t)d;
    } else {
      *out = wrap_to((int64_t)d, ty);
    }
    return 1;
  }
  if (!ctr_is_integer(ty) && ty->kind != TY_PTR) {
    return 0;
  }
  int64_t v;
  if (!eval_int(src, &v)) {
    return 0;
  }
  *out = ty->kind == TY_PTR ? v : wrap_to(v, ty);
  return 1;
}

static int eval_div(CtrNode *node, int64_t a, int64_t b, int64_t *out) {
  if (b == 0) {
    return 0;
  }
  int u = is_unsigned_node(node);
  if (node->kind == ND_DIV) {
    *out = u ? (int64_t)((uint64_t)a / (uint64_t)b) : (b == -1 ? (int64_t)(0 - (uint64_t)a) : a / b);
  } else {
    *out = u ? (int64_t)((uint64_t)a % (uint64_t)b) : (b == -1 ? 0 : a % b);
  }
  *out = wrap_to(*out, node->ty);
  return 1;
}

static int compare_unsigned(CtrNode *node) {
  return node->lhs->ty && node->lhs->ty->is_unsigned && ctr_is_integer(node->lhs->ty);
}

static int eval_compare(CtrNode *node, int64_t a, int64_t b) {
  int u = compare_unsigned(node);
  uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
  switch (node->kind) {
  case ND_EQ:
    return a == b;
  case ND_NE:
    return a != b;
  case ND_LT:
    return u ? ua < ub : a < b;
  case ND_LE:
    return u ? ua <= ub : a <= b;
  case ND_GT:
    return u ? ua > ub : a > b;
  default:
    return u ? ua >= ub : a >= b;
  }
}

static int eval_float_compare(CtrNode *node, int64_t *out) {
  double a, b;
  if (!eval_double(node->lhs, &a) || !eval_double(node->rhs, &b)) {
    return 0;
  }
  switch (node->kind) {
  case ND_EQ:
    *out = a == b;
    break;
  case ND_NE:
    *out = a != b;
    break;
  case ND_LT:
    *out = a < b;
    break;
  case ND_LE:
    *out = a <= b;
    break;
  case ND_GT:
    *out = a > b;
    break;
  default:
    *out = a >= b;
    break;
  }
  return 1;
}

static int eval_shift(CtrNode *node, int64_t a, int64_t b, int64_t *out) {
  if (b < 0 || b >= 64) {
    return 0;
  }
  if (node->kind == ND_SHL) {
    *out = wrap_to((int64_t)((uint64_t)a << b), node->ty);
  } else if (is_unsigned_node(node)) {
    uint64_t ua = (uint64_t)a;
    if (node->ty->size == 4) {
      ua &= 0xffffffffu;
    }
    *out = wrap_to((int64_t)(ua >> b), node->ty);
  } else {
    *out = wrap_to(a >> b, node->ty);
  }
  return 1;
}

static int eval_arith(CtrNode *node, int64_t a, int64_t b, int64_t *out) {
  uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
  switch (node->kind) {
  case ND_ADD:
    *out = (int64_t)(ua + ub);
    break;
  case ND_SUB:
    *out = (int64_t)(ua - ub);
    break;
  case ND_MUL:
    *out = (int64_t)(ua * ub);
    break;
  case ND_BITAND:
    *out = a & b;
    break;
  case ND_BITOR:
    *out = a | b;
    break;
  case ND_BITXOR:
    *out = a ^ b;
    break;
  default:
    return 0;
  }
  *out = wrap_to(*out, node->ty);
  return 1;
}

static int eval_binary(CtrNode *node, int64_t *out) {
  if (node->lhs->ty && ctr_is_flonum(node->lhs->ty) &&
      (node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE ||
       node->kind == ND_GT || node->kind == ND_GE)) {
    return eval_float_compare(node, out);
  }
  if (node->ty && node->ty->kind == TY_PTR) {
    return 0;
  }
  if ((node->lhs->ty && node->lhs->ty->kind == TY_PTR) || (node->rhs->ty && node->rhs->ty->kind == TY_PTR)) {
    return 0;
  }
  int64_t a, b;
  if (!eval_int(node->lhs, &a) || !eval_int(node->rhs, &b)) {
    return 0;
  }
  switch (node->kind) {
  case ND_DIV:
  case ND_MOD:
    return eval_div(node, a, b, out);
  case ND_SHL:
  case ND_SHR:
    return eval_shift(node, a, b, out);
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
    *out = eval_compare(node, a, b);
    return 1;
  default:
    return eval_arith(node, a, b, out);
  }
}

static int eval_logical(CtrNode *node, int64_t *out) {
  int64_t a, b;
  if (!eval_int(node->lhs, &a)) {
    double d;
    if (!eval_double(node->lhs, &d)) {
      return 0;
    }
    a = d != 0;
  }
  if (node->kind == ND_LOGAND && !a) {
    *out = 0;
    return 1;
  }
  if (node->kind == ND_LOGOR && a) {
    *out = 1;
    return 1;
  }
  if (!eval_int(node->rhs, &b)) {
    return 0;
  }
  *out = b != 0;
  return 1;
}

static int eval_unary(CtrNode *node, int64_t *out) {
  int64_t a;
  if (node->kind == ND_NOT && node->lhs->ty && ctr_is_flonum(node->lhs->ty)) {
    double d;
    if (!eval_double(node->lhs, &d)) {
      return 0;
    }
    *out = d == 0;
    return 1;
  }
  if (!eval_int(node->lhs, &a)) {
    return 0;
  }
  switch (node->kind) {
  case ND_NEG:
    *out = wrap_to((int64_t)(0 - (uint64_t)a), node->ty);
    return 1;
  case ND_POS:
    *out = a;
    return 1;
  case ND_BITNOT:
    *out = wrap_to(~a, node->ty);
    return 1;
  default:
    *out = !a;
    return 1;
  }
}

static int eval_cond(CtrNode *node, int64_t *out) {
  int64_t c;
  if (!eval_int(node->cond, &c)) {
    return 0;
  }
  return eval_int(c ? node->then : node->els, out);
}

static int eval_int(CtrNode *node, int64_t *out) {
  ctr_add_type(node);
  switch (node->kind) {
  case ND_NUM:
    if (node->is_float_lit) {
      return 0;
    }
    *out = node->ival;
    return 1;
  case ND_CAST:
    return eval_cast(node, out);
  case ND_MACRO:
    return eval_int(node->lhs, out);
  case ND_SIZEOF:
  case ND_ALIGNOF:
    *out = node->ival;
    return node->ival >= 0;
  case ND_COMMA:
    return eval_int(node->rhs, out);
  case ND_COND:
    return eval_cond(node, out);
  case ND_LOGAND:
  case ND_LOGOR:
    return eval_logical(node, out);
  case ND_NEG:
  case ND_POS:
  case ND_BITNOT:
  case ND_NOT:
    return eval_unary(node, out);
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
    return eval_binary(node, out);
  default:
    return 0;
  }
}

static int eval_double_binary(CtrNode *node, double *out) {
  double a, b;
  if (!eval_double(node->lhs, &a) || !eval_double(node->rhs, &b)) {
    return 0;
  }
  switch (node->kind) {
  case ND_ADD:
    *out = a + b;
    return 1;
  case ND_SUB:
    *out = a - b;
    return 1;
  case ND_MUL:
    *out = a * b;
    return 1;
  case ND_DIV:
    *out = a / b;
    return 1;
  default:
    return 0;
  }
}

static double round_float(double d, CtrType *ty) {
  if (ty && ty->kind == TY_FLOAT) {
    return (double)(float)d;
  }
  return d;
}

static int eval_double(CtrNode *node, double *out) {
  ctr_add_type(node);
  if (node->ty && ctr_is_integer(node->ty)) {
    int64_t v;
    if (!eval_int(node, &v)) {
      return 0;
    }
    *out = node->ty->is_unsigned ? (double)(uint64_t)v : (double)v;
    return 1;
  }
  switch (node->kind) {
  case ND_NUM:
    *out = node->is_float_lit ? node->fval : (double)node->ival;
    return 1;
  case ND_MACRO:
    return eval_double(node->lhs, out);
  case ND_CAST:
    if (!eval_double(node->lhs, out)) {
      return 0;
    }
    *out = round_float(*out, node->ty);
    return 1;
  case ND_NEG:
    if (!eval_double(node->lhs, out)) {
      return 0;
    }
    *out = -*out;
    return 1;
  case ND_POS:
    return eval_double(node->lhs, out);
  case ND_COND: {
    int64_t c;
    if (!eval_int(node->cond, &c)) {
      return 0;
    }
    return eval_double(c ? node->then : node->els, out);
  }
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
    if (!eval_double_binary(node, out)) {
      return 0;
    }
    *out = round_float(*out, node->ty);
    return 1;
  default:
    return 0;
  }
}

int ctr_try_eval(CtrNode *node, int64_t *out) {
  if (!node) {
    return 0;
  }
  return eval_int(node, out);
}

double ctr_eval_double(CtrNode *node) {
  double d = 0;
  eval_double(node, &d);
  return d;
}

int ctr_is_const_expr(CtrNode *node) {
  int64_t v;
  double d;
  return eval_int(node, &v) || eval_double(node, &d);
}

int64_t ctr_eval(CtrNode *node) {
  int64_t v = 0;
  if (!eval_int(node, &v)) {
    ctr_error_tok(node->tok, "not a compile-time constant");
  }
  return v;
}

int64_t ctr_const_expr(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_conditional(rest, tok);
  ctr_add_type(node);
  return ctr_eval(node);
}

static int eval_lvalue_address(CtrNode *node, CtrObj **base, int64_t *offset);

int ctr_eval_address(CtrNode *node, CtrObj **base, int64_t *offset) {
  ctr_add_type(node);
  switch (node->kind) {
  case ND_ADDR:
    return eval_lvalue_address(node->lhs, base, offset);
  case ND_MACRO:
    return ctr_eval_address(node->lhs, base, offset);
  case ND_CAST: {
    if (node->lhs->ty && (node->lhs->ty->kind == TY_ARRAY || node->lhs->ty->kind == TY_FUNC)) {
      return eval_lvalue_address(node->lhs, base, offset);
    }
    if (node->lhs->ty && ctr_is_integer(node->lhs->ty)) {
      int64_t v;
      if (eval_int(node->lhs, &v)) {
        *base = NULL;
        *offset = v;
        return 1;
      }
      return 0;
    }
    return ctr_eval_address(node->lhs, base, offset);
  }
  case ND_ADD:
  case ND_SUB: {
    if (!node->lhs->ty || node->lhs->ty->kind != TY_PTR) {
      return 0;
    }
    int64_t k;
    if (!eval_int(node->rhs, &k) || !ctr_eval_address(node->lhs, base, offset)) {
      return 0;
    }
    int64_t scale = node->lhs->ty->base->size > 0 ? node->lhs->ty->base->size : 1;
    *offset += (node->kind == ND_ADD ? k : -k) * scale;
    return 1;
  }
  case ND_VAR:
    if (node->var->ty->kind == TY_ARRAY || node->var->ty->kind == TY_FUNC) {
      *base = node->var;
      *offset = 0;
      return !node->var->is_local;
    }
    return 0;
  case ND_NUM: {
    int64_t v;
    if (eval_int(node, &v)) {
      *base = NULL;
      *offset = v;
      return 1;
    }
    return 0;
  }
  default:
    return 0;
  }
}

static int eval_lvalue_address(CtrNode *node, CtrObj **base, int64_t *offset) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->is_local) {
      return 0;
    }
    *base = node->var;
    *offset = 0;
    return 1;
  case ND_MACRO:
    return eval_lvalue_address(node->lhs, base, offset);
  case ND_DEREF:
    return ctr_eval_address(node->lhs, base, offset);
  case ND_MEMBER:
    if (node->member->is_bitfield || !eval_lvalue_address(node->lhs, base, offset)) {
      return 0;
    }
    *offset += node->member->offset;
    return 1;
  case ND_INDEX: {
    int64_t k;
    if (!eval_int(node->rhs, &k)) {
      return 0;
    }
    CtrType *bt = node->lhs->ty;
    int ok = bt->kind == TY_ARRAY ? eval_lvalue_address(node->lhs, base, offset)
                                  : ctr_eval_address(node->lhs, base, offset);
    if (!ok) {
      return 0;
    }
    *offset += k * bt->base->size;
    return 1;
  }
  case ND_COMPOUND_LIT:
    if (node->var && !node->var->is_local) {
      *base = node->var;
      *offset = 0;
      return 1;
    }
    return 0;
  default:
    return 0;
  }
}

static int side_effects_list(CtrNode *n) {
  for (; n; n = n->next) {
    if (ctr_has_side_effects(n)) {
      return 1;
    }
  }
  return 0;
}

int ctr_has_side_effects(CtrNode *node) {
  if (!node) {
    return 0;
  }
  switch (node->kind) {
  case ND_ASSIGN:
  case ND_OPASSIGN:
  case ND_PREINC:
  case ND_PREDEC:
  case ND_POSTINC:
  case ND_POSTDEC:
  case ND_FUNCALL:
  case ND_STMT_EXPR:
  case ND_VA_ARG:
  case ND_VA_START:
  case ND_VA_END:
  case ND_VA_COPY:
  case ND_BUILTIN:
  case ND_COMPOUND_LIT:
    return 1;
  case ND_VAR:
    return node->var && node->var->ty && node->var->ty->is_volatile;
  case ND_DEREF:
  case ND_INDEX:
  case ND_MEMBER:
    if (node->ty && node->ty->is_volatile) {
      return 1;
    }
    break;
  default:
    break;
  }
  return ctr_has_side_effects(node->lhs) || ctr_has_side_effects(node->rhs) ||
         ctr_has_side_effects(node->cond) || ctr_has_side_effects(node->then) ||
         ctr_has_side_effects(node->els) || side_effects_list(node->args);
}
