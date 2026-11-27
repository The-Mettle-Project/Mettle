#include "translate/ctr_emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ctr_body_falls_through(CtrNode *body);

typedef struct Slot {
  int offset;
  int size;
  int align;
  const char *name;
  const char *type;
} Slot;

typedef struct StructPlan {
  CtrStructDecl *decl;
  Slot *slots;
  int count;
  int cap;
} StructPlan;

static void add_slot(StructPlan *p, int offset, int size, int align, const char *name, const char *type) {
  if (p->count == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 8;
    Slot *s = ctr_alloc(sizeof(Slot) * (size_t)p->cap);
    if (p->slots) {
      memcpy(s, p->slots, sizeof(Slot) * (size_t)p->count);
    }
    p->slots = s;
  }
  Slot *s = &p->slots[p->count++];
  s->offset = offset;
  s->size = size;
  s->align = align;
  s->name = name;
  s->type = type;
}

static int align_up(int n, int a) { return a <= 1 ? n : (n + a - 1) / a * a; }

static int natural_align(CtrType *ty) {
  ctr_sync_type(ty);
  if (ty->kind == TY_ARRAY) {
    return natural_align(ty->base);
  }
  return ty->align < 1 ? 1 : ty->align;
}

static int overlaps(StructPlan *p, int off, int size) {
  for (int i = 0; i < p->count; i++) {
    Slot *s = &p->slots[i];
    if (off < s->offset + s->size && s->offset < off + size) {
      return 1;
    }
  }
  return 0;
}

static void plan_union(StructPlan *p, CtrStructDecl *decl) {
  CtrMember *primary = ctr_union_primary(decl);
  for (CtrMember *m = decl->members; m; m = m->next) {
    if (m->is_bitfield) {
      m->emit_kind = CTR_MEMBER_BITFIELD;
      m->unit_is_field = 0;
      m->unit_offset = 0;
      m->unit_size = m->ty->size;
      continue;
    }
    m->emit_kind = m == primary ? CTR_MEMBER_DIRECT : CTR_MEMBER_CAST;
  }
  if (primary) {
    add_slot(p, 0, primary->ty->size, natural_align(primary->ty), ctr_member_name(primary), ctr_type_text(primary->ty));
    return;
  }
  int a = decl->align < 1 ? 1 : decl->align;
  if (a > 8) {
    a = 8;
  }
  int n = decl->size / a;
  if (n > 0) {
    add_slot(p, 0, decl->size, a, "raw", ctr_format("%s[%d]", ctr_unit_type(a), n));
  }
}

static void plan_plain_members(StructPlan *p, CtrStructDecl *decl) {
  for (CtrMember *m = decl->members; m; m = m->next) {
    if (m->is_bitfield) {
      continue;
    }
    ctr_sync_type(m->ty);
    int a = natural_align(m->ty);
    int flexible = decl->is_flexible && !m->next && m->ty->kind == TY_ARRAY;
    if (flexible || m->ty->size == 0) {
      m->emit_kind = CTR_MEMBER_CAST;
      continue;
    }
    if (m->offset % a != 0) {
      m->emit_kind = CTR_MEMBER_CAST;
      add_slot(p, m->offset, m->ty->size, 1, ctr_format("%s_bytes", ctr_member_name(m)),
               ctr_format("uint8[%d]", m->ty->size));
      continue;
    }
    m->emit_kind = CTR_MEMBER_DIRECT;
    add_slot(p, m->offset, m->ty->size, a, ctr_member_name(m), ctr_type_text(m->ty));
  }
}

static void bit_range(CtrStructDecl *decl, int unit_offset, int unit_size, int *lo, int *hi) {
  *lo = 1 << 30;
  *hi = -1;
  for (CtrMember *m = decl->members; m; m = m->next) {
    if (!m->is_bitfield || m->bit_width == 0 || m->unit_offset != unit_offset || m->unit_size != unit_size) {
      continue;
    }
    int first = unit_offset + m->bit_offset / 8;
    int last = unit_offset + (m->bit_offset + m->bit_width - 1) / 8;
    if (first < *lo) {
      *lo = first;
    }
    if (last > *hi) {
      *hi = last;
    }
  }
}

static void plan_bitfields(StructPlan *p, CtrStructDecl *decl) {
  int units = 0;
  for (CtrMember *m = decl->members; m; m = m->next) {
    if (!m->is_bitfield) {
      continue;
    }
    m->emit_kind = CTR_MEMBER_BITFIELD;
    if (m->bit_width == 0) {
      continue;
    }
    if (m->unit_size == 0) {
      m->unit_size = m->ty->size;
    }
    CtrMember *owner = NULL;
    for (CtrMember *q = decl->members; q != m; q = q->next) {
      if (q->is_bitfield && q->bit_width && q->unit_offset == m->unit_offset && q->unit_size == m->unit_size) {
        owner = q;
        break;
      }
    }
    if (owner) {
      m->unit_name = owner->unit_name;
      m->unit_is_field = owner->unit_is_field;
      continue;
    }
    const char *name = ctr_format("bits%d", units++);
    m->unit_name = name;
    if (!overlaps(p, m->unit_offset, m->unit_size) && m->unit_offset % m->unit_size == 0 &&
        m->unit_offset + m->unit_size <= decl->size) {
      m->unit_is_field = 1;
      add_slot(p, m->unit_offset, m->unit_size, m->unit_size, name, ctr_unit_type(m->unit_size));
      continue;
    }
    m->unit_is_field = 0;
    int lo, hi;
    bit_range(decl, m->unit_offset, m->unit_size, &lo, &hi);
    for (int b = lo; b <= hi; b++) {
      if (!overlaps(p, b, 1)) {
        add_slot(p, b, 1, 1, ctr_format("%s_%d", name, b - lo), "uint8");
      }
    }
  }
}

static int slot_cmp(const void *a, const void *b) {
  const Slot *x = a, *y = b;
  return x->offset - y->offset;
}

static StructPlan *plan_struct(CtrStructDecl *decl) {
  StructPlan *p = ctr_alloc(sizeof(StructPlan));
  p->decl = decl;
  if (decl->is_union) {
    plan_union(p, decl);
  } else {
    plan_plain_members(p, decl);
    plan_bitfields(p, decl);
  }
  qsort(p->slots, (size_t)p->count, sizeof(Slot), slot_cmp);
  return p;
}

static CtrMap plans;

static StructPlan *struct_plan(CtrStructDecl *decl) {
  char key[32];
  snprintf(key, sizeof(key), "%p", (void *)decl);
  StructPlan *p = ctr_map_get(&plans, key);
  if (!p) {
    p = plan_struct(decl);
    ctr_map_put(&plans, ctr_strdup(key), p);
  }
  return p;
}

void ctr_emit_struct(CtrBuf *out, CtrStructDecl *decl) {
  StructPlan *p = struct_plan(decl);
  ctr_buf_printf(out, "struct %s {", ctr_struct_name(decl));
  int cur = 0;
  int max_align = 1;
  int pads = 0;
  int any = 0;
  for (int i = 0; i < p->count; i++) {
    Slot *s = &p->slots[i];
    if (s->offset < cur) {
      continue;
    }
    if (align_up(cur, s->align) != s->offset) {
      ctr_buf_printf(out, "\n  pad%d: uint8[%d];", pads++, s->offset - cur);
      cur = s->offset;
    }
    ctr_buf_printf(out, "\n  %s: %s;", s->name, s->type);
    any = 1;
    cur = s->offset + s->size;
    if (s->align > max_align) {
      max_align = s->align;
    }
  }
  int natural = align_up(cur, max_align);
  if (natural < decl->size) {
    int n = decl->size - cur;
    if (align_up(cur + n, max_align) != decl->size) {
      n = decl->size - cur;
    }
    ctr_buf_printf(out, "\n  pad%d: uint8[%d];", pads++, n);
    any = 1;
  }
  if (decl->align > max_align && decl->size > 0) {
    ctr_warn_tok(decl->tok, "'%s' asks for %d-byte alignment; the Mettle struct aligns to %d", ctr_struct_name(decl),
                 decl->align, max_align);
  }
  ctr_buf_put(out, any ? "\n}\n" : " }\n");
}

void ctr_plan_all_structs(CtrProgram *prog) {
  for (int i = 0; i < prog->structs.len; i++) {
    CtrStructDecl *decl = prog->structs.items[i];
    if (decl->complete) {
      struct_plan(decl);
    }
  }
}

static int is_zero_init(CtrInit *init) {
  if (!init) {
    return 1;
  }
  if (init->expr) {
    int64_t v;
    double d;
    CtrNode *e = init->expr;
    if (ctr_try_eval(e, &v)) {
      return v == 0 && !ctr_is_flonum(e->ty);
    }
    (void)d;
    return 0;
  }
  if (!init->children) {
    return 1;
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
    if (!is_zero_init(init->children[i])) {
      return 0;
    }
  }
  return 1;
}

typedef struct {
  int global;
  int ok;
} InitMode;

static char *init_text(CtrInit *init, CtrType *ty, InitMode *mode);

static int is_mettle_const_expr(CtrNode *e);

static int const_list(CtrNode *n) {
  for (; n; n = n->next) {
    if (!is_mettle_const_expr(n)) {
      return 0;
    }
  }
  return 1;
}

static int is_mettle_const_expr(CtrNode *e) {
  int64_t v;
  if (!e) {
    return 1;
  }
  if (ctr_try_eval(e, &v)) {
    return 1;
  }
  switch (e->kind) {
  case ND_NUM:
  case ND_SIZEOF:
  case ND_ALIGNOF:
    return 1;
  case ND_STR:
    return 1;
  case ND_MACRO:
    return is_mettle_const_expr(e->lhs);
  case ND_CAST:
    if (e->ty->kind == TY_PTR && e->lhs->ty->kind == TY_ARRAY) {
      CtrNode *in = e->lhs;
      while (in->kind == ND_MACRO) {
        in = in->lhs;
      }
      return in->kind == ND_STR && in->str_ty->base->size == 1;
    }
    if (e->ty->kind == TY_PTR && e->lhs->ty->kind == TY_FUNC) {
      CtrNode *in = e->lhs;
      return in->kind == ND_VAR;
    }
    if (e->ty->kind == TY_PTR) {
      return 0;
    }
    return ctr_is_numeric(e->ty) && ctr_is_numeric(e->lhs->ty) && is_mettle_const_expr(e->lhs);
  case ND_ADDR: {
    CtrNode *in = e->lhs;
    while (in->kind == ND_MACRO) {
      in = in->lhs;
    }
    return in->kind == ND_VAR && !in->var->is_local && !in->var->mettle_module &&
           ctr_same_mettle_type(e->ty, ctr_pointer_to(in->var->ty));
  }
  case ND_NEG:
  case ND_POS:
  case ND_BITNOT:
    return is_mettle_const_expr(e->lhs);
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
    return e->ty->kind != TY_PTR && is_mettle_const_expr(e->lhs) && is_mettle_const_expr(e->rhs);
  default:
    (void)const_list;
    return 0;
  }
}

static char *scalar_init_text(CtrInit *init, CtrType *ty, InitMode *mode) {
  CtrNode *e = init ? init->expr : NULL;
  if (!e) {
    return NULL;
  }
  if (mode->global) {
    int64_t v;
    if (ctr_is_integer(ty) && ctr_try_eval(e, &v) && !(e->kind == ND_MACRO)) {
      CtrNode num = {0};
      num.kind = ND_NUM;
      num.ival = v;
      num.ty = ty;
      CtrNode *s = e;
      while (s->kind == ND_CAST && s->implicit) {
        s = s->lhs;
      }
      if (s->kind == ND_NUM) {
        num.spelling = s->spelling;
        num.var = s->var;
      }
      return ctr_literal_text(&num, ty);
    }
    if (ctr_is_flonum(ty) && ctr_is_const_expr(e)) {
      return ctr_float_text(ctr_eval_double(e), ty->kind == TY_FLOAT);
    }
    if (!is_mettle_const_expr(e)) {
      mode->ok = 0;
      return NULL;
    }
  }
  return ctr_assign_value_text(e, ty);
}

static int child_count(CtrInit *init) {
  if (init->ty->kind == TY_ARRAY) {
    return (int)init->ty->array_len;
  }
  int n = 0;
  for (CtrMember *m = init->ty->sdecl->members; m; m = m->next) {
    n++;
  }
  return n;
}

static char *array_init_text(CtrInit *init, CtrType *ty, InitMode *mode) {
  if (!init->children) {
    return NULL;
  }
  int n = (int)ty->array_len;
  int last = -1;
  for (int i = 0; i < n; i++) {
    if (!is_zero_init(init->children[i])) {
      last = i;
    }
  }
  if (last < 0) {
    return NULL;
  }
  CtrBuf b = {0};
  ctr_buf_putc(&b, '[');
  for (int i = 0; i <= last; i++) {
    if (i) {
      ctr_buf_put(&b, ", ");
    }
    char *t = init_text(init->children[i], ty->base, mode);
    if (!mode->ok) {
      free(b.data);
      return NULL;
    }
    if (!t) {
      t = ctr_is_flonum(ty->base) ? "0.0" : ty->base->kind == TY_BOOL ? "false"
          : (ty->base->kind == TY_STRUCT || ty->base->kind == TY_UNION) ? "{}"
          : ty->base->kind == TY_ARRAY ? "[]" : "0";
    }
    ctr_buf_put(&b, t);
  }
  ctr_buf_putc(&b, ']');
  return ctr_buf_take(&b);
}

static uint64_t bitfield_bits(CtrMember *m, int64_t v) {
  uint64_t mask = m->bit_width >= 64 ? ~0ull : ((1ull << m->bit_width) - 1);
  return ((uint64_t)v & mask) << m->bit_offset;
}

static int unit_constant(CtrStructDecl *decl, CtrInit *init, CtrMember *first, uint64_t *out, InitMode *mode) {
  uint64_t value = 0;
  for (CtrMember *m = decl->members; m; m = m->next) {
    if (!m->is_bitfield || m->unit_name != first->unit_name) {
      continue;
    }
    CtrInit *c = init->children[m->idx];
    if (!c || !c->expr) {
      continue;
    }
    int64_t v;
    if (!ctr_try_eval(c->expr, &v)) {
      mode->ok = 0;
      return 0;
    }
    value |= bitfield_bits(m, v);
  }
  *out = value;
  return 1;
}

static char *struct_init_text(CtrInit *init, CtrType *ty, InitMode *mode) {
  CtrStructDecl *decl = ty->sdecl;
  if (init->expr) {
    if (mode->global) {
      mode->ok = 0;
      return NULL;
    }
    return ctr_expr_text(init->expr, PREC_LOWEST);
  }
  if (!init->children || is_zero_init(init)) {
    return NULL;
  }
  CtrBuf b = {0};
  int first = 1;
  ctr_buf_putc(&b, '{');
  for (CtrMember *m = decl->members; m; m = m->next) {
    CtrInit *c = init->children[m->idx];
    if (decl->is_union && init->mem && m != init->mem) {
      continue;
    }
    if (is_zero_init(c)) {
      continue;
    }
    if (m->emit_kind == CTR_MEMBER_CAST) {
      mode->ok = 0;
      free(b.data);
      return NULL;
    }
    if (m->emit_kind == CTR_MEMBER_BITFIELD) {
      if (!m->unit_is_field) {
        mode->ok = 0;
        free(b.data);
        return NULL;
      }
      int owner = 1;
      for (CtrMember *q = decl->members; q != m; q = q->next) {
        if (q->is_bitfield && q->unit_name == m->unit_name && !is_zero_init(init->children[q->idx])) {
          owner = 0;
        }
      }
      if (!owner) {
        continue;
      }
      uint64_t bits;
      if (!unit_constant(decl, init, m, &bits, mode)) {
        free(b.data);
        return NULL;
      }
      ctr_buf_printf(&b, "%s %s: 0x%llx", first ? "" : ",", m->unit_name, (unsigned long long)bits);
      first = 0;
      continue;
    }
    char *t = init_text(c, m->ty, mode);
    if (!mode->ok) {
      free(b.data);
      return NULL;
    }
    if (!t) {
      continue;
    }
    ctr_buf_printf(&b, "%s %s: %s", first ? "" : ",", ctr_member_name(m), t);
    first = 0;
  }
  ctr_buf_put(&b, first ? "}" : " }");
  (void)child_count;
  return ctr_buf_take(&b);
}

static char *init_text(CtrInit *init, CtrType *ty, InitMode *mode) {
  if (!init) {
    return NULL;
  }
  ctr_sync_type(ty);
  if (ty->kind == TY_ARRAY) {
    if (init->expr) {
      mode->ok = 0;
      return NULL;
    }
    return array_init_text(init, ty, mode);
  }
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    return struct_init_text(init, ty, mode);
  }
  return scalar_init_text(init, ty, mode);
}

char *ctr_init_literal(CtrInit *init, CtrType *ty, int *ok) {
  InitMode mode = {1, 1};
  char *t = init_text(init, ty, &mode);
  *ok = mode.ok;
  return t;
}

static CtrNode *synth_var(const char *name, CtrType *ty) {
  CtrObj *obj = ctr_alloc(sizeof(CtrObj));
  obj->name = name;
  obj->mettle_name = name;
  obj->ty = ty;
  obj->is_local = 1;
  CtrNode *n = ctr_new_node(ND_VAR, NULL);
  n->var = obj;
  n->ty = ty;
  return n;
}

static void store_init(CtrNode *target, CtrInit *init);

static void store_children(CtrNode *target, CtrInit *init) {
  CtrType *ty = init->ty;
  if (ty->kind == TY_ARRAY) {
    for (int64_t i = 0; i < ty->array_len; i++) {
      if (is_zero_init(init->children[i])) {
        continue;
      }
      CtrNode *idx = ctr_new_node(ND_INDEX, target->tok);
      idx->lhs = target;
      idx->rhs = ctr_new_node(ND_NUM, target->tok);
      idx->rhs->ival = i;
      idx->rhs->ty = ctr_ty_llong;
      idx->ty = ty->base;
      store_init(idx, init->children[i]);
    }
    return;
  }
  for (CtrMember *m = ty->sdecl->members; m; m = m->next) {
    if (ty->kind == TY_UNION && init->mem && m != init->mem) {
      continue;
    }
    CtrInit *c = init->children[m->idx];
    if (is_zero_init(c)) {
      continue;
    }
    CtrNode *mem = ctr_new_node(ND_MEMBER, target->tok);
    mem->lhs = target;
    mem->member = m;
    mem->ty = m->is_bitfield && m->bit_width < 32 && m->ty->size <= 4 ? ctr_ty_int : m->ty;
    store_init(mem, c);
  }
}

static CtrNode *strip_literal(CtrNode *n) {
  while (n->kind == ND_MACRO || (n->kind == ND_CAST && n->implicit)) {
    n = n->lhs;
  }
  return n;
}

static void store_init(CtrNode *target, CtrInit *init) {
  if (!init) {
    return;
  }
  if (target->ty->size == 0 && (!init->expr || strip_literal(init->expr)->kind == ND_COMPOUND_LIT)) {
    return;
  }
  if (init->expr) {
    CtrNode *rhs = init->expr;
    CtrType *tt = target->ty;
    if (tt->kind != TY_STRUCT && tt->kind != TY_UNION && !ctr_same_mettle_type(rhs->ty, tt)) {
      rhs = ctr_new_cast(rhs, tt);
    }
    ctr_emit_assign_stmt(target, rhs, tt);
    return;
  }
  if (init->children) {
    store_children(target, init);
  }
}

static int assigns(CtrNode *n, CtrObj *v) {
  if (!n) {
    return 0;
  }
  int writes = n->kind == ND_ASSIGN || n->kind == ND_OPASSIGN || n->kind == ND_PREINC || n->kind == ND_PREDEC ||
               n->kind == ND_POSTINC || n->kind == ND_POSTDEC;
  CtrNode *target = writes ? strip_literal(n->lhs) : NULL;
  if (target && target->kind == ND_VAR && target->var == v) {
    return 1;
  }
  if (assigns(n->lhs, v) || assigns(n->rhs, v) || assigns(n->cond, v) || assigns(n->then, v) || assigns(n->els, v)) {
    return 1;
  }
  if (n->kind != ND_GOTO && (assigns(n->inc, v) || assigns(n->init, v))) {
    return 1;
  }
  for (CtrNode *b = n->body; b; b = b->next) {
    if (assigns(b, v)) {
      return 1;
    }
  }
  for (CtrNode *a = n->args; a; a = a->next) {
    if (assigns(a, v)) {
      return 1;
    }
  }
  return 0;
}

static int stable_bound(CtrNode *len) {
  int64_t v;
  if (ctr_try_eval(len, &v)) {
    return 1;
  }
  CtrNode *s = strip_literal(len);
  if (s->kind != ND_VAR || !s->var->is_local || s->var->addr_taken || !ctr_em->fn) {
    return 0;
  }
  return !assigns(ctr_em->fn->body, s->var);
}

static void snapshot_bounds(CtrType *ty, CtrObj *var) {
  for (CtrType *t = ty; t->kind == TY_VLA || t->kind == TY_ARRAY || t->kind == TY_PTR; t = t->base) {
    if (t->kind != TY_VLA || stable_bound(t->vla_len)) {
      continue;
    }
    CtrObj *copy = ctr_alloc(sizeof(CtrObj));
    copy->name = ctr_new_temp(ctr_format("%s_len", ctr_mettle_name_of(var)));
    copy->mettle_name = copy->name;
    copy->ty = ctr_ty_llong;
    copy->is_local = 1;
    ctr_line("var %s: int64 = %s;", copy->name, ctr_value_text(t->vla_len, ctr_ty_llong, PREC_LOWEST));
    CtrNode *ref = ctr_new_node(ND_VAR, t->vla_len->tok);
    ref->var = copy;
    ref->ty = ctr_ty_llong;
    t->vla_len = ref;
  }
}

static void emit_vla_decl(CtrNode *node, CtrObj *var) {
  CtrType *ty = var->ty;
  snapshot_bounds(ty, var);
  CtrType *elem = ty;
  CtrBuf count = {0};
  int first = 1;
  while (elem->kind == TY_VLA || elem->kind == TY_ARRAY) {
    char *n = elem->kind == TY_VLA ? ctr_value_text(elem->vla_len, ctr_ty_ullong, PREC_MUL + 1)
                                   : ctr_format("%lld", (long long)elem->array_len);
    ctr_buf_printf(&count, "%s%s", first ? "" : " * ", n);
    first = 0;
    elem = elem->base;
  }
  ctr_em->mod->needs_c = 1;
  const char *et = ctr_type_text(elem);
  ctr_line("var %s: %s* = (%s*)libc.vla_alloc(%s * sizeof(%s));", ctr_mettle_name_of(var), et, et, ctr_buf_take(&count),
           et);
  ctr_line("defer libc.vla_free(%s);", ctr_mettle_name_of(var));
  (void)node;
}

static void emit_local_decl_inner(CtrNode *node, int as_assignment) {
  CtrObj *var = node->var;
  const char *name = ctr_mettle_name_of(var);
  if (var->ty->kind == TY_VLA) {
    emit_vla_decl(node, var);
    return;
  }
  if (var->ty->kind == TY_PTR && ctr_has_vla(var->ty->base)) {
    snapshot_bounds(var->ty, var);
  }
  CtrInit *init = node->decl_init;
  const char *ty = ctr_type_text(var->ty);
  const char *vol = var->ty->is_volatile && ctr_is_scalar(var->ty) && var->ty->kind != TY_PTR ? "volatile " : "";
  if (!init || (is_zero_init(init) && !ctr_is_scalar(var->ty))) {
    if (!as_assignment) {
      ctr_line("var %s: %s%s%s;", name, vol, ty, ctr_zero_suffix(var));
    } else if (init) {
      ctr_line("%s = %s;", name, var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION ? "{}" : var->ty->kind == TY_ARRAY ? "[]" : ctr_is_flonum(var->ty) ? "0.0" : var->ty->kind == TY_BOOL ? "false" : "0");
    }
    return;
  }
  InitMode mode = {0, 1};
  char *lit = init_text(init, var->ty, &mode);
  if (mode.ok && lit) {
    if (as_assignment) {
      ctr_line("%s = %s;", name, lit);
    } else {
      ctr_line("var %s: %s%s = %s;", name, vol, ty, lit);
    }
    return;
  }
  if (!as_assignment) {
    ctr_line("var %s: %s%s;", name, vol, ty);
  }
  store_init(synth_var(name, var->ty), init);
}

int ctr_hoist_begin(CtrNode *expr, void **state);
void ctr_hoist_end(void *state);

void ctr_declare_compound_literals(CtrNode *n);

static void declare_init_literals(CtrInit *init) {
  if (!init) {
    return;
  }
  if (init->expr) {
    ctr_declare_compound_literals(init->expr);
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
    declare_init_literals(init->children[i]);
  }
}

void ctr_emit_local_decl(CtrNode *node, int as_assignment) {
  CtrInit *init = node->decl_init;
  void *state = NULL;
  declare_init_literals(init);
  int hoisted = init && init->expr && ctr_is_scalar(node->var->ty) && ctr_hoist_begin(init->expr, &state);
  emit_local_decl_inner(node, as_assignment);
  if (hoisted) {
    ctr_hoist_end(state);
  }
}

void ctr_emit_global_init_stmts(CtrObj *var) {
  store_init(synth_var(ctr_mettle_name_of(var), var->ty), var->init);
}

static int needs_return_value(CtrObj *fn) { return fn->ty->return_ty->kind != TY_VOID; }

static const char *zero_value(CtrType *ty) {
  if (ctr_is_flonum(ty)) {
    return "0.0";
  }
  if (ty->kind == TY_BOOL) {
    return "false";
  }
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    return NULL;
  }
  return "0";
}

static int is_main(CtrObj *fn) { return strcmp(fn->name, "main") == 0; }

static char *param_list(CtrObj *fn) {
  CtrBuf b = {0};
  int first = 1;
  for (CtrObj *p = fn->params; p; p = p->next) {
    if (!first) {
      ctr_buf_put(&b, ", ");
    }
    first = 0;
    ctr_buf_printf(&b, "%s: %s", ctr_mettle_name_of(p), ctr_type_text(p->ty));
  }
  if (fn->ty->is_variadic) {
    ctr_buf_printf(&b, "%s%s: ..", first ? "" : ", ", fn->va_param ? fn->va_param->mettle_name : "va");
  }
  return ctr_buf_take(&b);
}

static const char *main_signature(CtrObj *fn) {
  CtrObj *argc = fn->params;
  CtrObj *argv = argc ? argc->next : NULL;
  if (!argc) {
    return "fn main() -> int32 {";
  }
  return ctr_format("fn main(%s: int32, %s: cstring*) -> int32 {", ctr_mettle_name_of(argc),
                    argv ? ctr_format("%s_in", ctr_mettle_name_of(argv)) : "argv_in");
}

static void main_prologue(CtrObj *fn) {
  CtrObj *argc = fn->params;
  CtrObj *argv = argc ? argc->next : NULL;
  if (argc && !ctr_same_mettle_type(argc->ty, ctr_ty_int)) {
    ctr_error_tok(argc->tok, "main's first parameter must be int");
  }
  if (argv) {
    ctr_line("var %s: %s = (%s)%s_in;", ctr_mettle_name_of(argv), ctr_type_text(argv->ty), ctr_type_text(argv->ty),
             ctr_mettle_name_of(argv));
  }
}

static int multi_module(void) { return ctr_prog->units.len > 1; }

static const char *export_prefix(CtrObj *obj) { return multi_module() && !obj->is_static ? "export " : ""; }

static void call_global_inits(void) {
  for (int u = 0; u < ctr_prog->units.len; u++) {
    CtrModule *mod = ((CtrUnit *)ctr_prog->units.items[u])->module;
    if (mod->init_name) {
      ctr_use_module(mod);
      ctr_line("%s();", mod->init_name);
    }
  }
}

static const char *fn_signature(CtrObj *fn) {
  if (is_main(fn)) {
    return main_signature(fn);
  }
  const char *exp = export_prefix(fn);
  const char *ret = needs_return_value(fn) ? ctr_format(" -> %s", ctr_return_type_text(fn->ty->return_ty)) : "";
  const char *noret = fn->is_noreturn ? "@noreturn " : "";
  return ctr_format("%s%sfn %s(%s)%s {", noret, exp, ctr_mettle_name_of(fn), param_list(fn), ret);
}

void ctr_emit_function(CtrBuf *out, CtrObj *fn) {
  CtrBuf body = {0};
  ctr_em->out = &body;
  ctr_em->indent = 0;
  ctr_em->fn = fn;
  ctr_em->temps = 0;
  ctr_em->labels = 0;
  ctr_structure_gotos(fn);
  ctr_assign_local_names(fn);
  ctr_find_uninitialized_reads(fn, ctr_needs_cfg(fn));
  ctr_line("%s", fn_signature(fn));
  ctr_em->indent = 1;
  if (is_main(fn)) {
    main_prologue(fn);
    call_global_inits();
  }
  if (ctr_needs_cfg(fn)) {
    ctr_emit_cfg_body(fn);
  } else {
    ctr_emit_block_body(fn->body);
    if (ctr_body_falls_through(fn->body)) {
      if (is_main(fn)) {
        ctr_line("return 0;");
      } else if (needs_return_value(fn) && zero_value(fn->ty->return_ty)) {
        ctr_line("return %s;", zero_value(fn->ty->return_ty));
      } else if (needs_return_value(fn)) {
        ctr_line("var undefined_result: %s;", ctr_type_text(fn->ty->return_ty));
        ctr_line("return undefined_result;");
      }
    }
  }
  ctr_em->indent = 0;
  ctr_line("}");
  ctr_buf_put(out, ctr_buf_take(&body));
  ctr_em->fn = NULL;
}
