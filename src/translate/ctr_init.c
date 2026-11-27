#include "translate/ctr_parse.h"

#include <string.h>

static void initializer2(CtrToken **rest, CtrToken *tok, CtrInit *init);

static CtrInit *new_initializer(CtrType *ty, int is_flexible) {
  CtrInit *init = ctr_alloc(sizeof(CtrInit));
  init->ty = ty;
  if (ty->kind == TY_ARRAY) {
    if (is_flexible && ty->size < 0) {
      init->is_flexible = 1;
      return init;
    }
    if (ty->array_len < 0) {
      init->is_flexible = 1;
      return init;
    }
    init->children = ctr_alloc(sizeof(CtrInit *) * (size_t)(ty->array_len + 1));
    for (int64_t i = 0; i < ty->array_len; i++) {
      init->children[i] = new_initializer(ty->base, 0);
    }
    return init;
  }
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    int len = 0;
    for (CtrMember *mem = ty->sdecl->members; mem; mem = mem->next) {
      len++;
    }
    init->children = ctr_alloc(sizeof(CtrInit *) * (size_t)(len + 1));
    for (CtrMember *mem = ty->sdecl->members; mem; mem = mem->next) {
      if (is_flexible && ty->sdecl->is_flexible && !mem->next) {
        CtrInit *child = ctr_alloc(sizeof(CtrInit));
        child->ty = mem->ty;
        child->is_flexible = 1;
        init->children[mem->idx] = child;
      } else {
        init->children[mem->idx] = new_initializer(mem->ty, 0);
      }
    }
    return init;
  }
  return init;
}

static CtrToken *skip_excess_element(CtrToken *tok) {
  if (ctr_equal(tok, "{")) {
    tok = skip_excess_element(tok->next);
    return ctr_skip(tok, "}");
  }
  ctr_assign(&tok, tok);
  return tok;
}

static void string_initializer(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  if (init->is_flexible) {
    *init = *new_initializer(ctr_array_of(init->ty->base, tok->ty->array_len), 0);
  }
  int64_t len = init->ty->array_len < tok->ty->array_len ? init->ty->array_len : tok->ty->array_len;
  int elem = init->ty->base->size;
  int src_elem = tok->ty->base->size;
  for (int64_t i = 0; i < len; i++) {
    int64_t v = 0;
    memcpy(&v, tok->str + i * src_elem, (size_t)src_elem);
    if (elem == 1 && !init->ty->base->is_unsigned) {
      v = (signed char)v;
    }
    init->children[i]->expr = ctr_new_num(v, init->ty->base, tok);
    init->children[i]->expr->op = 2;
  }
  init->tok = tok;
  *rest = tok->next;
}

static int is_string_init(CtrToken *tok, CtrType *ty) {
  return ty->kind == TY_ARRAY && tok->kind == TK_STR && ctr_is_integer(ty->base) &&
         ty->base->size == tok->ty->base->size;
}

static int64_t count_array_init_elements(CtrToken *tok, CtrType *ty) {
  int first = 1;
  CtrInit *dummy = new_initializer(ty->base, 1);
  int64_t i = 0, max = 0;
  while (!ctr_is_end(tok)) {
    if (!first) {
      tok = ctr_skip(tok, ",");
    }
    first = 0;
    if (ctr_equal(tok, "[")) {
      i = ctr_const_expr(&tok, tok->next);
      if (ctr_equal(tok, "...")) {
        i = ctr_const_expr(&tok, tok->next);
      }
      tok = ctr_skip(tok, "]");
      if (ctr_equal(tok, "=")) {
        tok = tok->next;
      }
      initializer2(&tok, tok, dummy);
    } else {
      initializer2(&tok, tok, dummy);
    }
    i++;
    if (i > max) {
      max = i;
    }
  }
  return max;
}

static int64_t array_designator(CtrToken **rest, CtrToken *tok, CtrType *ty, int64_t *end) {
  int64_t begin = ctr_const_expr(&tok, tok->next);
  if (begin >= ty->array_len) {
    ctr_error_tok(tok, "array designator index exceeds array bounds");
  }
  if (ctr_equal(tok, "...")) {
    *end = ctr_const_expr(&tok, tok->next);
  } else {
    *end = begin;
  }
  *rest = ctr_skip(tok, "]");
  return begin;
}

static CtrMember *struct_designator(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  CtrToken *start = tok;
  tok = ctr_skip(tok, ".");
  for (CtrMember *mem = ty->sdecl->members; mem; mem = mem->next) {
    if (mem->anon && (mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION)) {
      for (CtrMember *m2 = mem->ty->sdecl->members; m2; m2 = m2->next) {
        if (m2->name && m2->name->len == tok->len &&
            strncmp(m2->name->loc, tok->loc, (size_t)tok->len) == 0) {
          *rest = start;
          return mem;
        }
      }
      continue;
    }
    if (mem->name && mem->name->len == tok->len &&
        strncmp(mem->name->loc, tok->loc, (size_t)tok->len) == 0) {
      *rest = tok->next;
      return mem;
    }
  }
  ctr_fatal_tok(tok, "struct has no member named '%.*s'", tok->len, tok->loc);
  return NULL;
}

static void designation(CtrToken **rest, CtrToken *tok, CtrInit *init);

static CtrMember *next_named(CtrMember *mem) {
  while (mem && mem->is_bitfield && !mem->name) {
    mem = mem->next;
  }
  return mem;
}

static void struct_initializer2(CtrToken **rest, CtrToken *tok, CtrInit *init, CtrMember *mem, int lead);
static void array_initializer2(CtrToken **rest, CtrToken *tok, CtrInit *init, int64_t i);

static void designation_array(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  int64_t end;
  int64_t begin = array_designator(&tok, tok, init->ty, &end);
  CtrToken *start = tok;
  for (int64_t i = begin; i <= end && i < init->ty->array_len; i++) {
    designation(&tok, start, init->children[i]);
  }
  array_initializer2(rest, tok, init, end + 1);
}

static void designation_struct(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  CtrMember *mem = struct_designator(&tok, tok, init->ty);
  designation(&tok, tok, init->children[mem->idx]);
  init->expr = NULL;
  struct_initializer2(rest, tok, init, mem->next, 1);
}

static void designation_union(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  CtrMember *mem = struct_designator(&tok, tok, init->ty);
  init->mem = mem;
  designation(rest, tok, init->children[mem->idx]);
}

static void designation(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  if (ctr_equal(tok, "[")) {
    if (init->ty->kind != TY_ARRAY) {
      ctr_fatal_tok(tok, "array index in non-array initializer");
    }
    designation_array(rest, tok, init);
    return;
  }
  if (ctr_equal(tok, ".") && init->ty->kind == TY_STRUCT) {
    designation_struct(rest, tok, init);
    return;
  }
  if (ctr_equal(tok, ".") && init->ty->kind == TY_UNION) {
    designation_union(rest, tok, init);
    return;
  }
  if (ctr_equal(tok, ".")) {
    ctr_fatal_tok(tok, "field name not in struct or union initializer");
  }
  if (ctr_equal(tok, "=")) {
    tok = tok->next;
  }
  initializer2(rest, tok, init);
}

static void array_initializer1(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  tok = ctr_skip(tok, "{");
  if (init->is_flexible) {
    int64_t len = count_array_init_elements(tok, init->ty);
    *init = *new_initializer(ctr_array_of(init->ty->base, len), 0);
  }
  int first = 1;
  for (int64_t i = 0; !ctr_consume(rest, tok, "}"); i++) {
    if (!first) {
      tok = ctr_skip(tok, ",");
    }
    first = 0;
    if (ctr_equal(tok, "}")) {
      continue;
    }
    if (ctr_equal(tok, "[")) {
      int64_t end;
      int64_t begin = array_designator(&tok, tok, init->ty, &end);
      CtrToken *start = tok;
      for (int64_t j = begin; j <= end && j < init->ty->array_len; j++) {
        designation(&tok, start, init->children[j]);
      }
      i = end;
      continue;
    }
    if (i < init->ty->array_len) {
      initializer2(&tok, tok, init->children[i]);
    } else {
      tok = skip_excess_element(tok);
    }
  }
}

static void array_initializer2(CtrToken **rest, CtrToken *tok, CtrInit *init, int64_t i) {
  if (init->is_flexible) {
    int64_t len = count_array_init_elements(tok, init->ty);
    *init = *new_initializer(ctr_array_of(init->ty->base, len), 0);
  }
  for (; i < init->ty->array_len && !ctr_is_end(tok); i++) {
    CtrToken *start = tok;
    if (i > 0) {
      tok = ctr_skip(tok, ",");
    }
    if (ctr_equal(tok, "[") || ctr_equal(tok, ".")) {
      *rest = start;
      return;
    }
    initializer2(&tok, tok, init->children[i]);
  }
  *rest = tok;
}

static void struct_initializer1(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  tok = ctr_skip(tok, "{");
  CtrMember *mem = next_named(init->ty->sdecl->members);
  int first = 1;
  while (!ctr_consume(rest, tok, "}")) {
    if (!first) {
      tok = ctr_skip(tok, ",");
    }
    first = 0;
    if (ctr_equal(tok, "}")) {
      continue;
    }
    if (ctr_equal(tok, ".")) {
      mem = struct_designator(&tok, tok, init->ty);
      designation(&tok, tok, init->children[mem->idx]);
      mem = next_named(mem->next);
      continue;
    }
    if (tok->kind == TK_IDENT && ctr_equal(tok->next, ":")) {
      for (CtrMember *m = init->ty->sdecl->members; m; m = m->next) {
        if (m->name && m->name->len == tok->len && strncmp(m->name->loc, tok->loc, (size_t)tok->len) == 0) {
          mem = m;
        }
      }
      tok = tok->next->next;
    }
    if (mem) {
      initializer2(&tok, tok, init->children[mem->idx]);
      mem = next_named(mem->next);
    } else {
      tok = skip_excess_element(tok);
    }
  }
}

static void struct_initializer2(CtrToken **rest, CtrToken *tok, CtrInit *init, CtrMember *mem, int lead) {
  int first = !lead;
  mem = next_named(mem);
  for (; mem && !ctr_is_end(tok); mem = next_named(mem->next)) {
    CtrToken *start = tok;
    if (!first) {
      tok = ctr_skip(tok, ",");
    }
    first = 0;
    if (ctr_equal(tok, "[") || ctr_equal(tok, ".")) {
      *rest = start;
      return;
    }
    initializer2(&tok, tok, init->children[mem->idx]);
  }
  *rest = tok;
}

static void union_initializer(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  CtrMember *first = next_named(init->ty->sdecl->members);
  if (ctr_equal(tok, "{") && ctr_equal(tok->next, ".")) {
    tok = tok->next;
    while (!ctr_equal(tok, "}")) {
      CtrMember *mem = struct_designator(&tok, tok, init->ty);
      init->mem = mem;
      designation(&tok, tok, init->children[mem->idx]);
      if (!ctr_consume(&tok, tok, ",")) {
        break;
      }
    }
    *rest = ctr_skip(tok, "}");
    return;
  }
  init->mem = first;
  if (!first) {
    *rest = skip_excess_element(tok);
    return;
  }
  if (ctr_equal(tok, "{")) {
    initializer2(&tok, tok->next, init->children[first->idx]);
    ctr_consume(&tok, tok, ",");
    *rest = ctr_skip(tok, "}");
  } else {
    initializer2(rest, tok, init->children[first->idx]);
  }
}

static void struct_expr_initializer(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  CtrNode *expr = ctr_assign(rest, tok);
  ctr_add_type(expr);
  if (expr->ty->kind == TY_STRUCT && expr->ty->sdecl == init->ty->sdecl) {
    init->expr = expr;
    return;
  }
  CtrToken *dummy;
  struct_initializer2(&dummy, tok, init, init->ty->sdecl->members, 0);
  *rest = dummy;
}

static void scalar_initializer(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  if (ctr_equal(tok, "{")) {
    initializer2(&tok, tok->next, init);
    ctr_consume(&tok, tok, ",");
    while (!ctr_equal(tok, "}")) {
      tok = skip_excess_element(tok);
      ctr_consume(&tok, tok, ",");
    }
    *rest = ctr_skip(tok, "}");
    return;
  }
  CtrNode *expr = ctr_assign(rest, tok);
  ctr_add_type(expr);
  init->expr = expr;
}

static void initializer2(CtrToken **rest, CtrToken *tok, CtrInit *init) {
  init->tok = tok;
  CtrType *ty = init->ty;
  if (ty->kind == TY_ARRAY && is_string_init(tok, ty)) {
    string_initializer(rest, tok, init);
    return;
  }
  if (ty->kind == TY_ARRAY && ctr_equal(tok, "{") && is_string_init(tok->next, ty) &&
      ctr_equal(tok->next->next, "}")) {
    string_initializer(&tok, tok->next, init);
    *rest = ctr_skip(tok, "}");
    return;
  }
  if (ty->kind == TY_ARRAY) {
    if (ctr_equal(tok, "{")) {
      array_initializer1(rest, tok, init);
    } else {
      array_initializer2(rest, tok, init, 0);
    }
    return;
  }
  if (ty->kind == TY_STRUCT) {
    if (ctr_equal(tok, "{")) {
      struct_initializer1(rest, tok, init);
    } else {
      struct_expr_initializer(rest, tok, init);
    }
    return;
  }
  if (ty->kind == TY_UNION) {
    if (!ctr_equal(tok, "{")) {
      CtrNode *expr = ctr_assign(&tok, tok);
      ctr_add_type(expr);
      if (expr->ty->kind == TY_UNION) {
        init->expr = expr;
        *rest = tok;
        return;
      }
      CtrMember *first = next_named(init->ty->sdecl->members);
      init->mem = first;
      init->children[first->idx]->expr = ctr_new_cast(expr, first->ty);
      *rest = tok;
      return;
    }
    union_initializer(rest, tok, init);
    return;
  }
  scalar_initializer(rest, tok, init);
}

static CtrType *copy_struct_type(CtrType *ty) {
  ty = ctr_copy_type(ty);
  return ty;
}

static void convert_scalars(CtrInit *init) {
  if (!init) {
    return;
  }
  if (init->expr && init->ty->kind != TY_STRUCT && init->ty->kind != TY_UNION &&
      init->ty->kind != TY_ARRAY) {
    CtrNode *e = init->expr;
    ctr_add_type(e);
    if (!ctr_same_type(e->ty, init->ty) || e->ty->kind == TY_ARRAY || e->ty->kind == TY_FUNC) {
      init->expr = ctr_new_cast(e, ctr_unqual(init->ty));
    }
  }
  if (init->children) {
    int n = 0;
    if (init->ty->kind == TY_ARRAY) {
      n = (int)init->ty->array_len;
    } else if (init->ty->sdecl) {
      for (CtrMember *m = init->ty->sdecl->members; m; m = m->next) {
        n++;
      }
    }
    for (int i = 0; i < n; i++) {
      convert_scalars(init->children[i]);
    }
  }
}

CtrInit *ctr_parse_initializer(CtrToken **rest, CtrToken *tok, CtrType *ty, CtrType **new_ty) {
  CtrInit *init = new_initializer(ty, 1);
  initializer2(rest, tok, init);
  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->sdecl->is_flexible) {
    ty = copy_struct_type(ty);
    CtrMember *mem = ty->sdecl->members;
    while (mem->next) {
      mem = mem->next;
    }
    CtrInit *flex = init->children[mem->idx];
    if (flex && flex->ty->kind == TY_ARRAY && flex->ty->array_len > 0) {
      ty->size = mem->offset + flex->ty->size;
    }
    *new_ty = ty;
    convert_scalars(init);
    return init;
  }
  *new_ty = init->ty;
  convert_scalars(init);
  return init;
}

CtrNode *ctr_compound_literal(CtrToken **rest, CtrToken *tok, CtrType *ty, CtrToken *start) {
  CtrNode *node = ctr_new_node(ND_COMPOUND_LIT, start);
  if (ctr_ps.scope->next == NULL || !ctr_ps.current_fn) {
    CtrObj *var = ctr_new_anon_gvar(ty, start);
    CtrType *new_ty = NULL;
    var->init = ctr_parse_initializer(rest, tok, ty, &new_ty);
    var->ty = new_ty;
    node->var = var;
    node->ty = var->ty;
    return node;
  }
  CtrObj *var = ctr_new_lvar(ctr_format("lit%d", ctr_ps.anon_count++), ty, start);
  CtrType *new_ty = NULL;
  node->decl_init = ctr_parse_initializer(rest, tok, ty, &new_ty);
  var->ty = new_ty;
  node->var = var;
  node->ty = var->ty;
  return node;
}
