#include "translate/ctr.h"

#include <string.h>

CtrType *ctr_ty_void;
CtrType *ctr_ty_bool;
CtrType *ctr_ty_char;
CtrType *ctr_ty_schar;
CtrType *ctr_ty_uchar;
CtrType *ctr_ty_short;
CtrType *ctr_ty_ushort;
CtrType *ctr_ty_int;
CtrType *ctr_ty_uint;
CtrType *ctr_ty_long;
CtrType *ctr_ty_ulong;
CtrType *ctr_ty_llong;
CtrType *ctr_ty_ullong;
CtrType *ctr_ty_float;
CtrType *ctr_ty_double;
CtrType *ctr_ty_ldouble;

static CtrTarget current_target;

CtrType *ctr_new_type(CtrTypeKind kind, int size, int align) {
  CtrType *ty = ctr_alloc(sizeof(CtrType));
  ty->kind = kind;
  ty->size = size;
  ty->align = align;
  return ty;
}

static CtrType *make_int(CtrTypeKind kind, int size, int is_unsigned) {
  CtrType *ty = ctr_new_type(kind, size, size);
  ty->is_unsigned = is_unsigned;
  return ty;
}

void ctr_types_init(CtrTarget target) {
  current_target = target;
  int long_size = target == CTR_TARGET_WINDOWS_X64 ? 4 : 8;
  ctr_ty_void = ctr_new_type(TY_VOID, 1, 1);
  ctr_ty_bool = ctr_new_type(TY_BOOL, 1, 1);
  ctr_ty_bool->is_unsigned = 1;
  ctr_ty_char = make_int(TY_CHAR, 1, target == CTR_TARGET_LINUX_ARM64);
  ctr_ty_char->is_plain_char = 1;
  ctr_ty_schar = make_int(TY_CHAR, 1, 0);
  ctr_ty_uchar = make_int(TY_CHAR, 1, 1);
  ctr_ty_short = make_int(TY_SHORT, 2, 0);
  ctr_ty_ushort = make_int(TY_SHORT, 2, 1);
  ctr_ty_int = make_int(TY_INT, 4, 0);
  ctr_ty_uint = make_int(TY_INT, 4, 1);
  ctr_ty_long = make_int(TY_LONG, long_size, 0);
  ctr_ty_ulong = make_int(TY_LONG, long_size, 1);
  ctr_ty_llong = make_int(TY_LLONG, 8, 0);
  ctr_ty_ullong = make_int(TY_LLONG, 8, 1);
  ctr_ty_float = ctr_new_type(TY_FLOAT, 4, 4);
  ctr_ty_double = ctr_new_type(TY_DOUBLE, 8, 8);
  ctr_ty_ldouble = ctr_new_type(TY_LDOUBLE, 8, 8);
}

CtrType *ctr_size_t_type(void) {
  return current_target == CTR_TARGET_WINDOWS_X64 ? ctr_ty_ullong : ctr_ty_ulong;
}

CtrType *ctr_ptrdiff_type(void) {
  return current_target == CTR_TARGET_WINDOWS_X64 ? ctr_ty_llong : ctr_ty_long;
}

CtrType *ctr_wchar_type(void) {
  return current_target == CTR_TARGET_WINDOWS_X64 ? ctr_ty_ushort : ctr_ty_int;
}

int ctr_is_integer(CtrType *ty) {
  CtrTypeKind k = ty->kind;
  return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT || k == TY_INT || k == TY_LONG ||
         k == TY_LLONG || k == TY_ENUM;
}

int ctr_is_flonum(CtrType *ty) {
  return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE;
}

int ctr_is_numeric(CtrType *ty) { return ctr_is_integer(ty) || ctr_is_flonum(ty); }

int ctr_is_scalar(CtrType *ty) { return ctr_is_numeric(ty) || ty->kind == TY_PTR; }

CtrType *ctr_copy_type(CtrType *ty) {
  CtrType *ret = ctr_alloc(sizeof(CtrType));
  *ret = *ty;
  ret->origin = ty;
  return ret;
}

CtrType *ctr_strip_typedef(CtrType *ty) {
  if (!ty->typedef_name) {
    return ty;
  }
  CtrType *t = ctr_copy_type(ty);
  t->typedef_name = NULL;
  t->typedef_obj = NULL;
  return t;
}

CtrType *ctr_unqual(CtrType *ty) {
  if (!ty->is_const && !ty->is_volatile) {
    return ty;
  }
  CtrType *t = ctr_copy_type(ty);
  t->is_const = 0;
  t->is_volatile = 0;
  return t;
}

CtrType *ctr_pointer_to(CtrType *base) {
  CtrType *ty = ctr_new_type(TY_PTR, 8, 8);
  ty->base = base;
  ty->is_unsigned = 1;
  return ty;
}

CtrType *ctr_func_type(CtrType *return_ty) {
  CtrType *ty = ctr_new_type(TY_FUNC, 1, 1);
  ty->return_ty = return_ty;
  return ty;
}

void ctr_sync_type(CtrType *ty) {
  if (!ty) {
    return;
  }
  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->sdecl && ty->sdecl->complete) {
    ty->size = ty->sdecl->size;
    ty->align = ty->sdecl->align;
    return;
  }
  if (ty->kind == TY_ARRAY && ty->base) {
    ctr_sync_type(ty->base);
    ty->size = (int)(ty->base->size * (ty->array_len < 0 ? 0 : ty->array_len));
    ty->align = ty->base->align;
  }
}

CtrType *ctr_array_of(CtrType *base, int64_t len) {
  ctr_sync_type(base);
  CtrType *ty = ctr_new_type(TY_ARRAY, (int)(base->size * (len < 0 ? 0 : len)), base->align);
  ty->base = base;
  ty->array_len = len;
  return ty;
}

CtrType *ctr_vla_of(CtrType *base, CtrNode *len) {
  CtrType *ty = ctr_new_type(TY_VLA, 8, 8);
  ty->base = base;
  ty->vla_len = len;
  return ty;
}

CtrType *ctr_enum_type(void) {
  CtrType *ty = ctr_new_type(TY_ENUM, 4, 4);
  return ty;
}

CtrType *ctr_struct_type(void) {
  CtrType *ty = ctr_new_type(TY_STRUCT, 0, 1);
  return ty;
}

static CtrType *canonical(CtrType *ty) {
  while (ty->origin) {
    ty = ty->origin;
  }
  return ty;
}

int ctr_same_type(CtrType *a, CtrType *b) {
  if (a == b) {
    return 1;
  }
  if (a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case TY_STRUCT:
  case TY_UNION:
    return a->sdecl == b->sdecl;
  case TY_PTR:
    return ctr_same_type(a->base, b->base);
  case TY_ARRAY:
    return a->array_len == b->array_len && ctr_same_type(a->base, b->base);
  case TY_ENUM:
    return 1;
  case TY_FUNC:
    return ctr_is_compatible(a, b);
  default:
    return a->size == b->size && a->is_unsigned == b->is_unsigned &&
           a->is_plain_char == b->is_plain_char;
  }
}

static int func_compatible(CtrType *t1, CtrType *t2) {
  if (!ctr_is_compatible(t1->return_ty, t2->return_ty)) {
    return 0;
  }
  if (t1->is_variadic != t2->is_variadic) {
    return !t1->has_proto || !t2->has_proto;
  }
  CtrType *p1 = t1->params;
  CtrType *p2 = t2->params;
  for (; p1 && p2; p1 = p1->next, p2 = p2->next) {
    if (!ctr_is_compatible(p1, p2)) {
      return 0;
    }
  }
  return p1 == NULL && p2 == NULL;
}

int ctr_is_compatible(CtrType *t1, CtrType *t2) {
  if (t1 == t2) {
    return 1;
  }
  if (canonical(t1) == canonical(t2)) {
    return 1;
  }
  if (t1->kind != t2->kind) {
    return (t1->kind == TY_ENUM && t2->kind == TY_INT) || (t1->kind == TY_INT && t2->kind == TY_ENUM);
  }
  switch (t1->kind) {
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_LONG:
  case TY_LLONG:
    return t1->is_unsigned == t2->is_unsigned && t1->is_plain_char == t2->is_plain_char;
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
  case TY_BOOL:
  case TY_VOID:
  case TY_ENUM:
    return 1;
  case TY_PTR:
    return ctr_is_compatible(t1->base, t2->base);
  case TY_FUNC:
    return func_compatible(t1, t2);
  case TY_ARRAY:
    return ctr_is_compatible(t1->base, t2->base) &&
           (t1->array_len < 0 || t2->array_len < 0 || t1->array_len == t2->array_len);
  case TY_STRUCT:
  case TY_UNION:
    return t1->sdecl == t2->sdecl;
  default:
    return 0;
  }
}

int ctr_int_rank(CtrType *ty) {
  switch (ty->kind) {
  case TY_BOOL:
    return 0;
  case TY_CHAR:
    return 1;
  case TY_SHORT:
    return 2;
  case TY_INT:
  case TY_ENUM:
    return 3;
  case TY_LONG:
    return 4;
  case TY_LLONG:
    return 5;
  default:
    return 3;
  }
}

CtrType *ctr_promote(CtrType *ty) {
  if (ty->kind == TY_ENUM) {
    return ctr_ty_int;
  }
  if (ctr_is_integer(ty) && ty->size < 4) {
    return ctr_ty_int;
  }
  if (ctr_is_integer(ty)) {
    return ctr_unqual(ctr_strip_typedef(ty));
  }
  return ty;
}

static CtrType *plain_int(CtrType *ty) {
  if (ty->kind == TY_ENUM) {
    return ctr_ty_int;
  }
  switch (ty->kind) {
  case TY_INT:
    return ty->is_unsigned ? ctr_ty_uint : ctr_ty_int;
  case TY_LONG:
    return ty->is_unsigned ? ctr_ty_ulong : ctr_ty_long;
  case TY_LLONG:
    return ty->is_unsigned ? ctr_ty_ullong : ctr_ty_llong;
  default:
    return ty;
  }
}

static CtrType *unsigned_of(CtrType *ty) {
  switch (ty->kind) {
  case TY_INT:
    return ctr_ty_uint;
  case TY_LONG:
    return ctr_ty_ulong;
  default:
    return ctr_ty_ullong;
  }
}

CtrType *ctr_common_type(CtrType *ty1, CtrType *ty2) {
  if (ty1->kind == TY_LDOUBLE || ty2->kind == TY_LDOUBLE) {
    return ctr_ty_ldouble;
  }
  if (ty1->kind == TY_DOUBLE || ty2->kind == TY_DOUBLE) {
    return ctr_ty_double;
  }
  if (ty1->kind == TY_FLOAT || ty2->kind == TY_FLOAT) {
    return ctr_ty_float;
  }
  CtrType *a = plain_int(ctr_promote(ty1));
  CtrType *b = plain_int(ctr_promote(ty2));
  if (a->is_unsigned == b->is_unsigned) {
    return ctr_int_rank(a) >= ctr_int_rank(b) ? a : b;
  }
  CtrType *u = a->is_unsigned ? a : b;
  CtrType *s = a->is_unsigned ? b : a;
  if (ctr_int_rank(u) >= ctr_int_rank(s)) {
    return u;
  }
  if (s->size > u->size) {
    return s;
  }
  return unsigned_of(s);
}

CtrNode *ctr_new_node(CtrNodeKind kind, CtrToken *tok) {
  static int next_id;
  CtrNode *node = ctr_alloc(sizeof(CtrNode));
  node->kind = kind;
  node->tok = tok;
  node->id = ++next_id;
  return node;
}

static int is_null_pointer_constant(CtrNode *node) {
  CtrNode *n = node;
  while (n->kind == ND_CAST && n->cast_ty && n->cast_ty->kind == TY_PTR &&
         n->cast_ty->base->kind == TY_VOID) {
    n = n->lhs;
  }
  int64_t v;
  return n->ty && ctr_is_integer(n->ty) && ctr_try_eval(n, &v) && v == 0;
}

CtrNode *ctr_new_cast(CtrNode *expr, CtrType *ty) {
  ctr_add_type(expr);
  CtrNode *node = ctr_new_node(ND_CAST, expr->tok);
  node->lhs = expr;
  node->ty = ty;
  node->cast_ty = ty;
  node->implicit = 1;
  return node;
}

static CtrNode *decay(CtrNode *node) {
  ctr_add_type(node);
  if (node->ty->kind == TY_ARRAY || node->ty->kind == TY_VLA) {
    return ctr_new_cast(node, ctr_pointer_to(node->ty->base));
  }
  if (node->ty->kind == TY_FUNC) {
    return ctr_new_cast(node, ctr_pointer_to(node->ty));
  }
  return node;
}

static CtrNode *convert(CtrNode *node, CtrType *ty) {
  node = decay(node);
  if (ctr_same_type(node->ty, ty) && node->ty->kind != TY_BOOL) {
    return node;
  }
  if (ctr_same_type(node->ty, ty)) {
    return node;
  }
  return ctr_new_cast(node, ty);
}

static void usual_arith_conv(CtrNode **lhs, CtrNode **rhs) {
  *lhs = decay(*lhs);
  *rhs = decay(*rhs);
  CtrType *ty = ctr_common_type((*lhs)->ty, (*rhs)->ty);
  *lhs = convert(*lhs, ty);
  *rhs = convert(*rhs, ty);
}

static int is_bitfield_member(CtrNode *node) {
  return node->kind == ND_MEMBER && node->member && node->member->is_bitfield;
}

static CtrType *member_type(CtrNode *node) {
  CtrMember *mem = node->member;
  if (mem->is_bitfield && ctr_is_integer(mem->ty) && mem->ty->size <= 4) {
    if (mem->bit_width < 32) {
      return ctr_ty_int;
    }
    return mem->ty->is_unsigned ? ctr_ty_uint : ctr_ty_int;
  }
  return mem->ty;
}

static void type_comparison(CtrNode *node) {
  node->lhs = decay(node->lhs);
  node->rhs = decay(node->rhs);
  CtrType *lt = node->lhs->ty, *rt = node->rhs->ty;
  if (ctr_is_numeric(lt) && ctr_is_numeric(rt)) {
    usual_arith_conv(&node->lhs, &node->rhs);
  } else if (lt->kind == TY_PTR && ctr_is_integer(rt)) {
    node->rhs = ctr_new_cast(node->rhs, lt);
  } else if (rt->kind == TY_PTR && ctr_is_integer(lt)) {
    node->lhs = ctr_new_cast(node->lhs, rt);
  }
  node->ty = ctr_ty_int;
}

static void type_cond(CtrNode *node) {
  node->then = decay(node->then);
  node->els = decay(node->els);
  CtrType *a = node->then->ty, *b = node->els->ty;
  if (a->kind == TY_VOID || b->kind == TY_VOID) {
    node->ty = ctr_ty_void;
  } else if (ctr_is_numeric(a) && ctr_is_numeric(b)) {
    usual_arith_conv(&node->then, &node->els);
    node->ty = node->then->ty;
  } else if (a->kind == TY_PTR && is_null_pointer_constant(node->els)) {
    node->els = convert(node->els, a);
    node->ty = a;
  } else if (b->kind == TY_PTR && is_null_pointer_constant(node->then)) {
    node->then = convert(node->then, b);
    node->ty = b;
  } else if (a->kind == TY_PTR && b->kind == TY_PTR && !ctr_same_type(a, b)) {
    CtrType *t = a->base->kind == TY_VOID ? a : b->base->kind == TY_VOID ? b : a;
    node->then = convert(node->then, t);
    node->els = convert(node->els, t);
    node->ty = t;
  } else {
    node->ty = a;
  }
}

static void type_additive(CtrNode *node) {
  node->lhs = decay(node->lhs);
  node->rhs = decay(node->rhs);
  CtrType *lt = node->lhs->ty, *rt = node->rhs->ty;
  if (ctr_is_numeric(lt) && ctr_is_numeric(rt)) {
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = node->lhs->ty;
    return;
  }
  if (node->kind == ND_SUB && lt->kind == TY_PTR && rt->kind == TY_PTR) {
    node->ty = ctr_ptrdiff_type();
    return;
  }
  if (rt->kind == TY_PTR && lt->kind != TY_PTR) {
    CtrNode *tmp = node->lhs;
    node->lhs = node->rhs;
    node->rhs = tmp;
    node->op = 1;
  }
  node->ty = node->lhs->ty;
}

static void type_shift(CtrNode *node) {
  node->lhs = convert(node->lhs, ctr_promote(decay(node->lhs)->ty));
  node->rhs = convert(node->rhs, ctr_promote(decay(node->rhs)->ty));
  node->ty = node->lhs->ty;
}

static void type_assign(CtrNode *node) {
  CtrType *lt = node->lhs->ty;
  if (lt->kind == TY_ARRAY) {
    ctr_error_tok(node->tok, "not an lvalue");
  }
  if (lt->kind != TY_STRUCT && lt->kind != TY_UNION) {
    node->rhs = convert(node->rhs, ctr_unqual(lt));
  }
  node->ty = lt;
}

static CtrType *opassign_type(CtrNode *node) {
  CtrType *lt = node->lhs->ty;
  CtrNode *rhs = decay(node->rhs);
  node->rhs = rhs;
  if (lt->kind == TY_PTR) {
    return lt;
  }
  if (node->op == ND_SHL || node->op == ND_SHR) {
    node->rhs = convert(node->rhs, ctr_promote(node->rhs->ty));
    return ctr_promote(is_bitfield_member(node->lhs) ? member_type(node->lhs) : lt);
  }
  return ctr_common_type(lt, rhs->ty);
}

static void type_opassign(CtrNode *node) {
  CtrType *op_ty = opassign_type(node);
  node->cast_ty = op_ty;
  if (node->lhs->ty->kind != TY_PTR && node->op != ND_SHL && node->op != ND_SHR) {
    node->rhs = convert(node->rhs, op_ty);
  } else if (node->lhs->ty->kind == TY_PTR) {
    node->rhs = decay(node->rhs);
  }
  node->ty = node->lhs->ty;
}

static void type_unary_arith(CtrNode *node) {
  node->lhs = decay(node->lhs);
  if (ctr_is_numeric(node->lhs->ty)) {
    node->lhs = convert(node->lhs, ctr_promote(node->lhs->ty));
  }
  node->ty = node->lhs->ty;
}

static void type_deref(CtrNode *node) {
  CtrType *t = node->lhs->ty;
  if (t->kind == TY_FUNC) {
    node->ty = t;
    return;
  }
  node->lhs = decay(node->lhs);
  t = node->lhs->ty;
  if (t->kind != TY_PTR) {
    ctr_error_tok(node->tok, "invalid pointer dereference");
    node->ty = ctr_ty_int;
    return;
  }
  node->ty = t->base;
}

static void type_index(CtrNode *node) {
  CtrType *lt = node->lhs->ty;
  CtrType *rt = node->rhs->ty;
  if (ctr_is_integer(lt) && (rt->kind == TY_PTR || rt->kind == TY_ARRAY || rt->kind == TY_VLA)) {
    CtrNode *tmp = node->lhs;
    node->lhs = node->rhs;
    node->rhs = tmp;
    lt = node->lhs->ty;
  }
  if (lt->kind != TY_PTR && lt->kind != TY_ARRAY && lt->kind != TY_VLA) {
    ctr_error_tok(node->tok, "subscripted value is not an array or pointer");
    node->ty = ctr_ty_int;
    return;
  }
  node->rhs = decay(node->rhs);
  node->ty = lt->base;
}

static void type_binary_arith(CtrNode *node) {
  usual_arith_conv(&node->lhs, &node->rhs);
  node->ty = node->lhs->ty;
}

static void type_logical(CtrNode *node) {
  node->lhs = decay(node->lhs);
  if (node->rhs) {
    node->rhs = decay(node->rhs);
  }
  node->ty = ctr_ty_int;
}

static void type_children(CtrNode *node) {
  ctr_add_type(node->lhs);
  ctr_add_type(node->rhs);
  ctr_add_type(node->cond);
  ctr_add_type(node->then);
  ctr_add_type(node->els);
  ctr_add_type(node->init);
  ctr_add_type(node->inc);
  for (CtrNode *n = node->body; n; n = n->next) {
    ctr_add_type(n);
  }
  for (CtrNode *n = node->args; n; n = n->next) {
    ctr_add_type(n);
  }
}

static void type_expr_node(CtrNode *node) {
  switch (node->kind) {
  case ND_NUM:
    if (!node->ty) {
      node->ty = ctr_ty_int;
    }
    return;
  case ND_ADD:
  case ND_SUB:
    type_additive(node);
    return;
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
    type_binary_arith(node);
    return;
  case ND_SHL:
  case ND_SHR:
    type_shift(node);
    return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
    type_comparison(node);
    return;
  case ND_NOT:
  case ND_LOGAND:
  case ND_LOGOR:
    type_logical(node);
    return;
  case ND_NEG:
  case ND_POS:
  case ND_BITNOT:
    type_unary_arith(node);
    return;
  case ND_ASSIGN:
    type_assign(node);
    return;
  case ND_OPASSIGN:
    type_opassign(node);
    return;
  default:
    break;
  }
}

static void type_other_node(CtrNode *node) {
  switch (node->kind) {
  case ND_PREINC:
  case ND_PREDEC:
  case ND_POSTINC:
  case ND_POSTDEC:
    node->ty = node->lhs->ty;
    return;
  case ND_COND:
    type_cond(node);
    return;
  case ND_COMMA:
    node->ty = node->rhs->ty;
    return;
  case ND_MEMBER:
    node->ty = member_type(node);
    return;
  case ND_ADDR:
    node->ty = ctr_pointer_to(node->lhs->ty);
    return;
  case ND_DEREF:
    type_deref(node);
    return;
  case ND_INDEX:
    type_index(node);
    return;
  case ND_VAR:
    node->ty = node->var->ty;
    return;
  case ND_SIZEOF:
  case ND_ALIGNOF:
    node->ty = ctr_size_t_type();
    return;
  case ND_STR:
    node->ty = node->str_ty;
    return;
  default:
    return;
  }
}

void ctr_add_type(CtrNode *node) {
  if (!node || node->ty) {
    return;
  }
  type_children(node);
  type_expr_node(node);
  if (!node->ty) {
    type_other_node(node);
  }
}

static int field_alignment(CtrMember *mem, CtrStructDecl *decl) {
  int align = mem->align;
  if (decl->is_packed) {
    align = 1;
  }
  if (decl->field_align > 0 && align > decl->field_align) {
    align = decl->field_align;
  }
  return align < 1 ? 1 : align;
}

static int align_to(int n, int align) { return (n + align - 1) / align * align; }

static void layout_union(CtrStructDecl *decl) {
  int size = 0;
  int align = 1;
  for (CtrMember *mem = decl->members; mem; mem = mem->next) {
    int a = field_alignment(mem, decl);
    mem->offset = 0;
    if (mem->is_bitfield) {
      mem->unit_offset = 0;
      mem->unit_size = mem->ty->size;
      mem->bit_offset = 0;
    }
    if (a > align) {
      align = a;
    }
    int msize = mem->ty->size < 0 ? 0 : mem->ty->size;
    if (msize > size) {
      size = msize;
    }
  }
  decl->size = align_to(size, align);
  decl->align = align;
}

typedef struct {
  int bits;
  int unit_start;
  int unit_size;
  int open;
} BitState;

static void layout_bitfield_sysv(CtrMember *mem, CtrStructDecl *decl, int *bits, int *align) {
  int sz = mem->ty->size;
  int a = decl->is_packed ? 1 : mem->ty->align;
  if (mem->bit_width == 0) {
    *bits = align_to(*bits, sz * 8);
    return;
  }
  if (!decl->is_packed && *bits / (sz * 8) != (*bits + mem->bit_width - 1) / (sz * 8)) {
    *bits = align_to(*bits, sz * 8);
  }
  mem->unit_size = decl->is_packed ? 0 : sz;
  mem->unit_offset = decl->is_packed ? 0 : (*bits / (sz * 8)) * sz;
  mem->bit_offset = *bits - mem->unit_offset * 8;
  mem->offset = mem->unit_offset;
  *bits += mem->bit_width;
  if (a > *align) {
    *align = a;
  }
}

static void layout_bitfield_ms(CtrMember *mem, CtrStructDecl *decl, int *bits, int *align,
                               BitState *st) {
  int sz = mem->ty->size;
  if (mem->bit_width == 0) {
    if (st->open) {
      *bits = (st->unit_start + st->unit_size) * 8;
    }
    st->open = 0;
    return;
  }
  if (!st->open || st->unit_size != sz || st->bits + mem->bit_width > sz * 8) {
    if (st->open) {
      *bits = (st->unit_start + st->unit_size) * 8;
    }
    int a = field_alignment(mem, decl);
    int start = align_to(*bits / 8 + ((*bits % 8) ? 1 : 0), a);
    st->open = 1;
    st->unit_start = start;
    st->unit_size = sz;
    st->bits = 0;
    if (a > *align) {
      *align = a;
    }
  }
  mem->unit_offset = st->unit_start;
  mem->unit_size = sz;
  mem->bit_offset = st->bits;
  mem->offset = st->unit_start;
  st->bits += mem->bit_width;
  *bits = st->unit_start * 8 + st->unit_size * 8;
}

static void layout_plain_member(CtrMember *mem, CtrStructDecl *decl, int *bits, int *align,
                                BitState *st) {
  int a = field_alignment(mem, decl);
  if (st->open) {
    st->open = 0;
  }
  int off = align_to((*bits + 7) / 8, a);
  mem->offset = off;
  int msize = mem->ty->size < 0 ? 0 : mem->ty->size;
  *bits = (off + msize) * 8;
  if (a > *align) {
    *align = a;
  }
}

static void sync_members(CtrStructDecl *decl) {
  for (CtrMember *mem = decl->members; mem; mem = mem->next) {
    int stale = mem->align == mem->ty->align;
    ctr_sync_type(mem->ty);
    if (stale || mem->align < mem->ty->align) {
      mem->align = mem->ty->align;
    }
  }
}

void ctr_layout_struct(CtrStructDecl *decl, CtrTarget target) {
  sync_members(decl);
  if (decl->is_union) {
    layout_union(decl);
    return;
  }
  int bits = 0;
  int align = 1;
  BitState st = {0};
  int ms = target == CTR_TARGET_WINDOWS_X64;
  for (CtrMember *mem = decl->members; mem; mem = mem->next) {
    if (mem->is_bitfield && ms) {
      layout_bitfield_ms(mem, decl, &bits, &align, &st);
    } else if (mem->is_bitfield) {
      layout_bitfield_sysv(mem, decl, &bits, &align);
    } else {
      layout_plain_member(mem, decl, &bits, &align, &st);
    }
  }
  decl->size = align_to((bits + 7) / 8, align);
  decl->align = align;
}
