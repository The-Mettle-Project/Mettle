#include "translate/ctr_emit.h"

#include <stdio.h>
#include <string.h>

CtrEmitter *ctr_em;

static const char *mettle_keywords[] = {
    "fn", "if", "asm", "for", "new", "var", "case", "else", "enum", "impl", "int8", "this",
    "break", "const", "defer", "int16", "int32", "int64", "match", "trait", "uint8", "where",
    "while", "export", "extern", "import", "method", "return", "string", "struct", "switch",
    "uint16", "uint32", "uint64", "barrier", "default", "float32", "float64", "private",
    "continue", "dispatch", "errdefer", "workgroup", "import_str", "kernel", "bool", "char",
    "cstring", "rawptr", "void", "true", "false", "comptime",
    "fallthrough", "sizeof", "typeof", "volatile", "panic", "syscall", "Fn", "Result", "Option",
    "Ok", "Err", "Some", "None", "float16", "bfloat16", "libc", "math"};

static CtrMap keyword_map;

int ctr_is_mettle_keyword(const char *name) {
  if (!keyword_map.entries) {
    for (size_t i = 0; i < sizeof(mettle_keywords) / sizeof(*mettle_keywords); i++) {
      ctr_map_put(&keyword_map, mettle_keywords[i], (void *)1);
    }
  }
  return ctr_map_get(&keyword_map, name) != NULL;
}

static int starts_reserved(const char *name) {
  return strncmp(name, "__mettle", 8) == 0 || strncmp(name, "mettle_", 7) == 0;
}

const char *ctr_safe_ident(const char *name) {
  if (ctr_is_mettle_keyword(name) || starts_reserved(name)) {
    return ctr_format("%s_", name);
  }
  if (name[0] == '$' || strchr(name, '$')) {
    char *copy = ctr_strdup(name);
    for (char *p = copy; *p; p++) {
      if (*p == '$') {
        *p = '_';
      }
    }
    return copy;
  }
  return name;
}

static CtrMap module_names;

static int name_taken(const char *name) { return ctr_map_get(&module_names, name) != NULL; }

static const char *claim_name(const char *base) {
  const char *name = ctr_safe_ident(base);
  if (!name_taken(name)) {
    ctr_map_put(&module_names, name, (void *)1);
    return name;
  }
  for (int i = 2;; i++) {
    const char *alt = ctr_format("%s_%d", name, i);
    if (!name_taken(alt)) {
      ctr_map_put(&module_names, alt, (void *)1);
      return alt;
    }
  }
}

static int typedef_names_struct(CtrObj *td) {
  return (td->ty->kind == TY_STRUCT || td->ty->kind == TY_UNION) && td->ty->sdecl &&
         !td->ty->typedef_name;
}

static const char *struct_base_name(CtrStructDecl *decl) {
  for (int i = 0; i < ctr_prog->typedefs.len; i++) {
    CtrObj *td = ctr_prog->typedefs.items[i];
    if (typedef_names_struct(td) && td->ty->sdecl == decl) {
      return td->name;
    }
  }
  if (decl->tag) {
    return decl->tag;
  }
  for (int i = 0; i < ctr_prog->structs.len; i++) {
    CtrStructDecl *parent = ctr_prog->structs.items[i];
    int anon = 0;
    for (CtrMember *m = parent->members; m; m = m->next) {
      CtrType *t = m->ty;
      while (t->kind == TY_ARRAY) {
        t = t->base;
      }
      const char *field = m->name ? ctr_token_text(m->name) : ctr_format("anon%d", anon);
      anon += !m->name;
      if (parent != decl && t->sdecl == decl) {
        return ctr_format("%s_%s", struct_base_name(parent), field);
      }
    }
  }
  return ctr_format("Anon%d", decl->id);
}

static const char *location_key(CtrToken *tok) {
  if (!tok) {
    return NULL;
  }
  CtrToken *use = tok;
  while (use->origin) {
    use = use->origin;
  }
  return ctr_format("%s:%d:%d:%s:%d:%d", tok->file ? tok->file->key : "?", tok->line, tok->col,
                    use->file ? use->file->key : "?", use->line, use->col);
}

static void type_signature(CtrBuf *b, CtrType *t, int depth) {
  ctr_buf_printf(b, "%d.%d.%d", t->kind, t->size, t->is_unsigned);
  if (t->sdecl && t->sdecl->tag) {
    ctr_buf_printf(b, ".%s", t->sdecl->tag);
  } else if (t->base && depth < 4) {
    ctr_buf_putc(b, '(');
    type_signature(b, t->base, depth + 1);
    ctr_buf_putc(b, ')');
  }
}

static const char *layout_key(CtrStructDecl *decl) {
  CtrBuf b = {0};
  ctr_buf_printf(&b, "%s|%d|%d|%d", struct_base_name(decl), decl->is_union, decl->size, decl->align);
  for (CtrMember *m = decl->members; m; m = m->next) {
    ctr_buf_printf(&b, "|%s:%d:%d:%d:", m->name ? ctr_token_text(m->name) : "", m->offset, m->bit_width,
                   m->bit_offset);
    type_signature(&b, m->ty, 0);
  }
  return ctr_buf_take(&b);
}

static void complete_forward_structs(CtrProgram *prog) {
  CtrMap by_tag = {0};
  for (int i = 0; i < prog->structs.len; i++) {
    CtrStructDecl *decl = prog->structs.items[i];
    const char *key = decl->tag ? ctr_format("%s:%d", decl->tag, decl->is_union) : NULL;
    if (key && decl->complete && !decl->canonical && !ctr_map_get(&by_tag, key)) {
      ctr_map_put(&by_tag, key, decl);
    }
  }
  for (int i = 0; i < prog->structs.len; i++) {
    CtrStructDecl *decl = prog->structs.items[i];
    const char *key = decl->tag ? ctr_format("%s:%d", decl->tag, decl->is_union) : NULL;
    CtrStructDecl *full = key && !decl->complete ? ctr_map_get(&by_tag, key) : NULL;
    if (full) {
      decl->canonical = full;
      decl->mettle_name = full->mettle_name;
    }
  }
}

static void name_structs(CtrProgram *prog) {
  CtrMap seen = {0};
  CtrMap layouts = {0};
  for (int i = 0; i < prog->structs.len; i++) {
    CtrStructDecl *decl = prog->structs.items[i];
    if (!decl->complete) {
      continue;
    }
    const char *key = decl->tok ? ctr_format("%s:%d", location_key(decl->tok), decl->is_union) : NULL;
    const char *layout = layout_key(decl);
    CtrStructDecl *first = key ? ctr_map_get(&seen, key) : NULL;
    if (!first && prog->units.len > 1) {
      first = ctr_map_get(&layouts, layout);
    }
    if (first) {
      decl->canonical = first;
      decl->mettle_name = first->mettle_name;
      continue;
    }
    decl->mettle_name = claim_name(struct_base_name(decl));
    if (key) {
      ctr_map_put(&seen, key, decl);
    }
    ctr_map_put(&layouts, layout, decl);
  }
  if (prog->units.len > 1) {
    complete_forward_structs(prog);
  }
}

static void name_enum_consts(CtrProgram *prog) {
  CtrMap seen = {0};
  for (int i = 0; i < prog->enum_consts.len; i++) {
    CtrObj *ec = prog->enum_consts.items[i];
    if (ec->is_system) {
      continue;
    }
    const char *key = location_key(ec->tok);
    const char *value = ctr_format("enum %s=%lld", ec->name, (long long)ec->enum_value);
    CtrObj *first = key ? ctr_map_get(&seen, key) : NULL;
    if (!first && prog->units.len > 1) {
      first = ctr_map_get(&seen, value);
    }
    if (!first) {
      ctr_map_put(&seen, value, ec);
    }
    if (first) {
      ec->mettle_name = first->mettle_name;
      ec->same_as = first;
      ec->emitted = 1;
      continue;
    }
    ec->mettle_name = claim_name(ec->name);
    if (key) {
      ctr_map_put(&seen, key, ec);
    }
  }
}

static CtrMap external_names;

static const char *external_name(CtrObj *g) {
  const char *known = ctr_map_get(&external_names, g->name);
  if (known) {
    return known;
  }
  const char *name = claim_name(g->name);
  ctr_map_put(&external_names, g->name, (void *)name);
  return name;
}

static int is_user_global(CtrObj *g) { return !g->is_system || g->is_definition; }

static void name_globals(CtrObj *globals) {
  for (CtrObj *g = globals; g; g = g->next) {
    if (g->mettle_module || !is_user_global(g)) {
      continue;
    }
    if (g->owner) {
      continue;
    }
    if (g->is_function && strcmp(g->name, "main") == 0) {
      g->mettle_name = "main";
      ctr_map_put(&module_names, "main", (void *)1);
      continue;
    }
    if (g->name[0] == '.') {
      continue;
    }
    g->mettle_name = g->is_static ? claim_name(g->name) : external_name(g);
  }
}

static void name_statics_and_anon(CtrObj *globals) {
  for (CtrObj *g = globals; g; g = g->next) {
    if (g->mettle_name || g->mettle_module) {
      continue;
    }
    if (g->owner) {
      g->mettle_name = claim_name(ctr_format("%s_%s", g->owner->name, g->name));
    } else if (g->name[0] == '.') {
      g->mettle_name = claim_name("lit");
    } else if (g->is_system && !g->is_definition) {
      g->mettle_name = g->link_name ? g->link_name : g->name;
    }
  }
}

static void name_members(CtrProgram *prog) {
  for (int i = 0; i < prog->structs.len; i++) {
    CtrStructDecl *decl = prog->structs.items[i];
    CtrMap seen = {0};
    int anon = 0;
    for (CtrMember *m = decl->members; m; m = m->next) {
      const char *base = m->name ? ctr_token_text(m->name) : ctr_format("anon%d", anon++);
      const char *name = ctr_safe_ident(base);
      if (ctr_map_get(&seen, name)) {
        name = ctr_format("%s_%d", name, m->idx);
      }
      ctr_map_put(&seen, name, (void *)1);
      m->mettle_name = name;
    }
  }
}

void ctr_prepare_names(CtrProgram *prog) {
  memset(&module_names, 0, sizeof(module_names));
  for (int u = 0; u < prog->units.len; u++) {
    CtrUnit *unit = prog->units.items[u];
    name_globals(unit->globals);
  }
  name_structs(prog);
  name_enum_consts(prog);
  for (int u = 0; u < prog->units.len; u++) {
    CtrUnit *unit = prog->units.items[u];
    name_statics_and_anon(unit->globals);
  }
  name_members(prog);
}

static const char *macro_value_key(CtrMacro *m) {
  CtrNode *v = m->value_expr;
  if (v->kind == ND_STR) {
    CtrBuf b = {0};
    ctr_buf_printf(&b, "macro %s=s", m->name);
    for (int i = 0; i < v->str_len; i++) {
      ctr_buf_printf(&b, "%02x", (unsigned char)v->str[i]);
    }
    return ctr_buf_take(&b);
  }
  if (ctr_is_flonum(m->value_type)) {
    return ctr_format("macro %s=f%d:%a", m->name, m->value_type->size, ctr_eval_double(v));
  }
  int64_t n = 0;
  ctr_try_eval(v, &n);
  return ctr_format("macro %s=i%d%d:%lld", m->name, m->value_type->size, m->value_type->is_unsigned, (long long)n);
}

void ctr_claim_macro_names(CtrProgram *prog) {
  CtrMap seen = {0};
  for (int i = 0; i < prog->macros.len; i++) {
    CtrMacro *m = prog->macros.items[i];
    if (!m->value_expr || m->rejected || !m->is_objlike) {
      continue;
    }
    m->recovered = 1;
    const char *key = m->def_tok ? location_key(m->def_tok) : NULL;
    const char *value = macro_value_key(m);
    CtrMacro *first = key ? ctr_map_get(&seen, key) : NULL;
    if (!first && prog->units.len > 1) {
      first = ctr_map_get(&seen, value);
    }
    if (!first) {
      ctr_map_put(&seen, value, m);
    }
    if (first) {
      m->mettle_name = first->mettle_name;
      m->same_as = first;
      m->emitted = 1;
      continue;
    }
    m->mettle_name = claim_name(m->name);
    if (key) {
      ctr_map_put(&seen, key, m);
    }
  }
}

const char *ctr_unique_label(const char *base) {
  return ctr_format("%s%d", base, ++ctr_em->labels);
}

static int local_taken(const char *name) {
  return name_taken(name) || ctr_map_get(&ctr_em->local_names, name) != NULL;
}

static void claim_local(CtrObj *var) {
  const char *name = ctr_safe_ident(var->name);
  if (strncmp(name, "lit", 3) == 0 && var->tok && var->name[3] >= '0' && var->name[3] <= '9') {
    name = "lit";
  }
  if (local_taken(name)) {
    for (int i = 2;; i++) {
      const char *alt = ctr_format("%s_%d", name, i);
      if (!local_taken(alt)) {
        name = alt;
        break;
      }
    }
  }
  ctr_map_put(&ctr_em->local_names, name, (void *)1);
  var->mettle_name = name;
}

void ctr_assign_local_names(CtrObj *fn) {
  memset(&ctr_em->local_names, 0, sizeof(ctr_em->local_names));
  for (CtrObj *p = fn->params; p; p = p->next) {
    claim_local(p);
  }
  if (fn->va_param) {
    fn->va_param->mettle_name = local_taken("va") ? "va_args" : "va";
    ctr_map_put(&ctr_em->local_names, fn->va_param->mettle_name, (void *)1);
  }
  for (CtrObj *l = fn->locals; l; l = l->next) {
    claim_local(l);
  }
}

const char *ctr_claim_name(const char *base) { return claim_name(base); }

const char *ctr_mettle_name_of(CtrObj *obj) {
  if (!obj->is_local) {
    ctr_use_module(obj->home);
  }
  if (obj->mettle_name) {
    return obj->mettle_name;
  }
  return ctr_safe_ident(obj->name);
}

const char *ctr_struct_name(CtrStructDecl *decl) {
  CtrStructDecl *c = decl->canonical ? decl->canonical : decl;
  c->used = 1;
  ctr_use_module(c->home);
  return decl->mettle_name ? decl->mettle_name : ctr_safe_ident(decl->tag ? decl->tag : "Anon");
}

const char *ctr_member_name(CtrMember *mem) {
  return mem->mettle_name ? mem->mettle_name : (mem->name ? ctr_token_text(mem->name) : "anon");
}

static const char *int_name(int size, int is_unsigned) {
  switch (size) {
  case 1:
    return is_unsigned ? "uint8" : "int8";
  case 2:
    return is_unsigned ? "uint16" : "int16";
  case 4:
    return is_unsigned ? "uint32" : "int32";
  default:
    return is_unsigned ? "uint64" : "int64";
  }
}

const char *ctr_unit_type(int size) { return int_name(size, 1); }

const char *ctr_scalar_type(CtrType *ty) {
  switch (ty->kind) {
  case TY_VOID:
    return "void";
  case TY_BOOL:
    return "bool";
  case TY_FLOAT:
    return "float32";
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return "float64";
  case TY_ENUM:
    return "int32";
  default:
    return int_name(ty->size, ty->is_unsigned);
  }
}

int ctr_is_opaque_struct(CtrType *ty) {
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) {
    return 0;
  }
  CtrStructDecl *decl = ty->sdecl;
  if (decl && !decl->complete && decl->canonical) {
    decl = decl->canonical;
  }
  return !decl || !decl->complete;
}

int ctr_is_va_list(CtrType *ty) {
  return ty && ty->typedef_name &&
         (strcmp(ty->typedef_name, "__builtin_va_list") == 0 || strcmp(ty->typedef_name, "va_list") == 0) &&
         ty->kind == TY_PTR;
}

int ctr_is_bool_type(CtrType *ty) { return ty && ty->kind == TY_BOOL; }

static int needs_group(CtrType *ty) {
  return ty->kind == TY_PTR && ty->base->kind == TY_FUNC;
}

static const char *grouped(CtrType *ty) {
  const char *t = ctr_type_text(ty);
  return needs_group(ty) ? ctr_format("(%s)", t) : t;
}

const char *ctr_return_type_text(CtrType *ty) { return ctr_type_text(ty); }

const char *ctr_fn_type_text(CtrType *fn) {
  CtrBuf buf = {0};
  ctr_buf_put(&buf, "fn(");
  int first = 1;
  for (CtrType *p = fn->params; p; p = p->next) {
    if (!first) {
      ctr_buf_put(&buf, ", ");
    }
    first = 0;
    ctr_buf_put(&buf, ctr_type_text(p));
  }
  if (fn->is_variadic && !first) {
    ctr_buf_put(&buf, ", ..");
  }
  ctr_buf_put(&buf, ") -> ");
  ctr_buf_put(&buf, ctr_return_type_text(fn->return_ty));
  return ctr_buf_take(&buf);
}

CtrType *ctr_vla_element(CtrType *ty) {
  while (ty->kind == TY_VLA || ty->kind == TY_ARRAY) {
    ty = ty->base;
  }
  return ty;
}

int ctr_has_vla(CtrType *ty) {
  for (; ty && (ty->kind == TY_VLA || ty->kind == TY_ARRAY || ty->kind == TY_PTR); ty = ty->base) {
    if (ty->kind == TY_VLA) {
      return 1;
    }
  }
  return 0;
}

static const char *pointer_text(CtrType *ty) {
  CtrType *base = ty->base;
  if (ctr_has_vla(base)) {
    return ctr_format("%s*", grouped(ctr_vla_element(base)));
  }
  if (base->kind == TY_FUNC) {
    return ctr_fn_type_text(base);
  }
  if (base->kind == TY_VOID || ctr_is_opaque_struct(base)) {
    return "rawptr";
  }
  if (ctr_is_va_list(ty)) {
    return "libc.VaList";
  }
  const char *inner = grouped(base);
  if (base->kind == TY_ARRAY) {
    inner = ctr_format("(%s)", ctr_type_text(base));
  }
  if (base->is_volatile && ctr_is_scalar(base)) {
    return ctr_format("volatile %s*", inner);
  }
  return ctr_format("%s*", inner);
}

static const char *array_text(CtrType *ty) {
  CtrBuf dims = {0};
  CtrType *elem = ty;
  while (elem->kind == TY_ARRAY) {
    ctr_buf_printf(&dims, "[%lld]", (long long)(elem->array_len < 0 ? 0 : elem->array_len));
    elem = elem->base;
  }
  return ctr_format("%s%s", grouped(elem), ctr_buf_take(&dims));
}

const char *ctr_type_text(CtrType *ty) {
  if (ctr_is_va_list(ty)) {
    return "libc.VaList";
  }
  switch (ty->kind) {
  case TY_PTR:
    return pointer_text(ty);
  case TY_ARRAY:
    return array_text(ty);
  case TY_VLA:
    return ctr_format("%s*", grouped(ctr_vla_element(ty)));
  case TY_STRUCT:
  case TY_UNION:
    if (ctr_is_opaque_struct(ty)) {
      return "rawptr";
    }
    return ctr_struct_name(ty->sdecl);
  case TY_FUNC:
    return ctr_fn_type_text(ty);
  default:
    return ctr_scalar_type(ty);
  }
}

int ctr_same_mettle_type(CtrType *a, CtrType *b) {
  if (a == b) {
    return 1;
  }
  return strcmp(ctr_type_text(a), ctr_type_text(b)) == 0;
}

CtrMember *ctr_union_primary(CtrStructDecl *decl) {
  for (CtrMember *m = decl->members; m; m = m->next) {
    if (m->is_bitfield || m->ty->kind == TY_ARRAY) {
      continue;
    }
    if (m->ty->size == decl->size && m->ty->align == decl->align) {
      return m;
    }
  }
  return NULL;
}

