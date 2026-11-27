#include "translate/ctr_parse.h"

#include <stdlib.h>
#include <string.h>

CtrParser ctr_ps;

void ctr_enter_scope(void) {
  CtrScope *sc = ctr_alloc(sizeof(CtrScope));
  sc->next = ctr_ps.scope;
  sc->depth = ctr_ps.scope ? ctr_ps.scope->depth + 1 : 0;
  ctr_ps.scope = sc;
}

void ctr_leave_scope(void) { ctr_ps.scope = ctr_ps.scope->next; }

CtrVarScope *ctr_push_scope(const char *name) {
  CtrVarScope *sc = ctr_alloc(sizeof(CtrVarScope));
  ctr_map_put(&ctr_ps.scope->vars, name, sc);
  return sc;
}

CtrVarScope *ctr_find_var(CtrToken *tok) {
  for (CtrScope *sc = ctr_ps.scope; sc; sc = sc->next) {
    CtrVarScope *vs = ctr_map_get2(&sc->vars, tok->loc, tok->len);
    if (vs) {
      return vs;
    }
  }
  return NULL;
}

static CtrType *find_tag(CtrToken *tok) {
  for (CtrScope *sc = ctr_ps.scope; sc; sc = sc->next) {
    CtrType *ty = ctr_map_get2(&sc->tags, tok->loc, tok->len);
    if (ty) {
      return ty;
    }
  }
  return NULL;
}

static void push_tag_scope(CtrToken *tok, CtrType *ty) {
  ctr_map_put2(&ctr_ps.scope->tags, ctr_strndup(tok->loc, (size_t)tok->len), tok->len, ty);
}

CtrType *ctr_find_typedef(CtrToken *tok) {
  if (tok->kind == TK_IDENT) {
    CtrVarScope *sc = ctr_find_var(tok);
    if (sc) {
      return sc->type_def;
    }
  }
  return NULL;
}

static CtrObj *new_var(const char *name, CtrType *ty, CtrToken *tok) {
  CtrObj *var = ctr_alloc(sizeof(CtrObj));
  var->name = name;
  var->ty = ty;
  var->tok = tok;
  var->id = ++ctr_ps.obj_ids;
  var->file = tok ? tok->file : ctr_ps.unit_file;
  ctr_push_scope(name)->var = var;
  return var;
}

CtrObj *ctr_new_lvar(const char *name, CtrType *ty, CtrToken *tok) {
  CtrObj *var = new_var(name, ty, tok);
  var->is_local = 1;
  var->next = ctr_ps.locals;
  var->owner = ctr_ps.current_fn;
  var->scope_depth = ctr_ps.block_depth;
  ctr_ps.locals = var;
  return var;
}

CtrObj *ctr_new_gvar(const char *name, CtrType *ty, CtrToken *tok) {
  CtrObj *var = new_var(name, ty, tok);
  var->next = ctr_ps.globals;
  var->is_static = 1;
  var->is_definition = 1;
  ctr_ps.globals = var;
  return var;
}

CtrObj *ctr_new_anon_gvar(CtrType *ty, CtrToken *tok) {
  CtrObj *var = ctr_new_gvar(ctr_format(".anon.%d", ctr_ps.anon_count++), ty, tok);
  var->is_static = 1;
  return var;
}

char *ctr_get_ident(CtrToken *tok) {
  if (tok->kind != TK_IDENT) {
    ctr_fatal_tok(tok, "expected an identifier");
  }
  return ctr_token_text(tok);
}

static const char *typename_keywords[] = {
    "void", "_Bool", "char", "short", "int", "long", "struct", "union", "typedef", "enum",
    "static", "extern", "_Alignas", "signed", "unsigned", "const", "volatile", "auto",
    "register", "restrict", "__restrict", "__restrict__", "_Noreturn", "float", "double",
    "typeof", "inline", "_Thread_local", "__thread", "_Atomic", "__attribute__", "__attribute",
    "__inline", "__inline__", "__volatile__", "__volatile", "__const", "__const__", "__signed__",
    "__signed", "__typeof__", "__typeof", "__extension__", "__declspec", "__cdecl", "__stdcall",
    "__fastcall", "_Complex", "__int64", "__int32", "__int16", "__int8", "__w64",
    "__forceinline", "_Float32", "_Float64", "__builtin_va_list", "__unaligned", "__ptr64",
    "__ptr32", "__auto_type"};

static CtrMap typename_map;

int ctr_is_typename(CtrToken *tok) {
  if (!typename_map.entries) {
    for (size_t i = 0; i < sizeof(typename_keywords) / sizeof(*typename_keywords); i++) {
      ctr_map_put(&typename_map, typename_keywords[i], (void *)1);
    }
  }
  if (tok->kind == TK_KEYWORD) {
    return ctr_map_get2(&typename_map, tok->loc, tok->len) != NULL;
  }
  return ctr_find_typedef(tok) != NULL;
}

static CtrToken *skip_balanced(CtrToken *tok) {
  int depth = 0;
  do {
    if (ctr_equal(tok, "(")) {
      depth++;
    } else if (ctr_equal(tok, ")")) {
      depth--;
    }
    if (tok->kind == TK_EOF) {
      ctr_fatal_tok(tok, "unterminated attribute");
    }
    tok = tok->next;
  } while (depth > 0);
  return tok;
}

static int attr_name_is(CtrToken *tok, const char *name) {
  if (ctr_equal(tok, name)) {
    return 1;
  }
  char *wrapped = ctr_format("__%s__", name);
  return ctr_equal(tok, wrapped);
}

static CtrToken *read_mettle_attr(CtrToken *tok, CtrVarAttr *attr) {
  tok = ctr_skip(tok, "(");
  if (tok->kind == TK_STR && attr) {
    attr->mettle_name = ctr_strndup(tok->str, (size_t)tok->str_len - 1);
  }
  tok = tok->next;
  if (ctr_equal(tok, ",")) {
    tok = tok->next;
    if (tok->kind == TK_STR && attr) {
      attr->mettle_module = ctr_strndup(tok->str, (size_t)tok->str_len - 1);
    }
    tok = tok->next;
  }
  if (ctr_equal(tok, ",")) {
    tok = tok->next;
    if (tok->kind == TK_STR && attr) {
      attr->mettle_alias = ctr_strndup(tok->str, (size_t)tok->str_len - 1);
    }
    tok = tok->next;
  }
  return ctr_skip(tok, ")");
}

static CtrToken *read_one_attribute(CtrToken *tok, CtrVarAttr *attr) {
  if (attr_name_is(tok, "packed")) {
    if (attr) {
      attr->is_packed = 1;
    }
    return tok->next;
  }
  if (attr_name_is(tok, "noreturn")) {
    if (attr) {
      attr->is_noreturn = 1;
    }
    return tok->next;
  }
  if (attr_name_is(tok, "aligned") && ctr_equal(tok->next, "(")) {
    CtrToken *p = tok->next->next;
    int64_t a = ctr_const_expr(&p, p);
    if (attr && a > attr->align) {
      attr->align = (int)a;
    }
    return ctr_skip(p, ")");
  }
  if (attr_name_is(tok, "aligned")) {
    if (attr && attr->align < 16) {
      attr->align = 16;
    }
    return tok->next;
  }
  if (attr_name_is(tok, "mettle")) {
    return read_mettle_attr(tok->next, attr);
  }
  if (attr_name_is(tok, "mettle_builtin")) {
    if (attr) {
      attr->is_builtin = 1;
    }
    return tok->next;
  }
  tok = tok->next;
  if (ctr_equal(tok, "(")) {
    tok = skip_balanced(tok);
  }
  return tok;
}

static CtrToken *gnu_attribute(CtrToken *tok, CtrVarAttr *attr) {
  tok = ctr_skip(tok->next, "(");
  tok = ctr_skip(tok, "(");
  while (!ctr_equal(tok, ")")) {
    if (ctr_equal(tok, ",")) {
      tok = tok->next;
      continue;
    }
    tok = read_one_attribute(tok, attr);
  }
  tok = ctr_skip(tok, ")");
  return ctr_skip(tok, ")");
}

static CtrToken *declspec_attribute(CtrToken *tok, CtrVarAttr *attr) {
  tok = ctr_skip(tok->next, "(");
  while (!ctr_equal(tok, ")")) {
    if (ctr_equal(tok, "align") && ctr_equal(tok->next, "(")) {
      CtrToken *p = tok->next->next;
      int64_t a = ctr_const_expr(&p, p);
      if (attr && a > attr->align) {
        attr->align = (int)a;
      }
      tok = ctr_skip(p, ")");
      continue;
    }
    if (ctr_equal(tok, "noreturn") && attr) {
      attr->is_noreturn = 1;
    }
    tok = tok->next;
    if (ctr_equal(tok, "(")) {
      tok = skip_balanced(tok);
    }
  }
  return ctr_skip(tok, ")");
}

CtrToken *ctr_skip_attributes(CtrToken *tok, CtrVarAttr *attr) {
  for (;;) {
    if (ctr_equal(tok, "__attribute__") || ctr_equal(tok, "__attribute")) {
      tok = gnu_attribute(tok, attr);
      continue;
    }
    if (ctr_equal(tok, "__declspec")) {
      tok = declspec_attribute(tok, attr);
      continue;
    }
    if (ctr_equal(tok, "__asm__") || ctr_equal(tok, "__asm") || ctr_equal(tok, "asm")) {
      if (ctr_equal(tok->next, "(")) {
        tok = skip_balanced(tok->next);
        continue;
      }
    }
    if (ctr_equal(tok, "__cdecl") || ctr_equal(tok, "__stdcall") || ctr_equal(tok, "__fastcall") ||
        ctr_equal(tok, "__w64") || ctr_equal(tok, "__ptr64") || ctr_equal(tok, "__ptr32") ||
        ctr_equal(tok, "__unaligned") || ctr_equal(tok, "__extension__")) {
      tok = tok->next;
      continue;
    }
    return tok;
  }
}

enum {
  K_VOID = 1 << 0,
  K_BOOL = 1 << 2,
  K_CHAR = 1 << 4,
  K_SHORT = 1 << 6,
  K_INT = 1 << 8,
  K_LONG = 1 << 10,
  K_FLOAT = 1 << 12,
  K_DOUBLE = 1 << 14,
  K_OTHER = 1 << 16,
  K_SIGNED = 1 << 17,
  K_UNSIGNED = 1 << 18,
  K_INT64 = 1 << 19
};

typedef struct {
  int counter;
  CtrType *ty;
} SpecState;

static int storage_spec(CtrToken *tok, CtrVarAttr *attr) {
  if (ctr_equal(tok, "typedef")) {
    attr->is_typedef = 1;
  } else if (ctr_equal(tok, "static")) {
    attr->is_static = 1;
  } else if (ctr_equal(tok, "extern")) {
    attr->is_extern = 1;
  } else if (ctr_equal(tok, "inline") || ctr_equal(tok, "__inline") ||
             ctr_equal(tok, "__inline__") || ctr_equal(tok, "__forceinline")) {
    attr->is_inline = 1;
  } else if (ctr_equal(tok, "_Thread_local") || ctr_equal(tok, "__thread")) {
    attr->is_tls = 1;
  } else if (ctr_equal(tok, "_Noreturn")) {
    attr->is_noreturn = 1;
  } else if (ctr_equal(tok, "register") || ctr_equal(tok, "auto")) {
  } else {
    return 0;
  }
  return 1;
}

static int qualifier_spec(CtrToken *tok, int *is_const, int *is_volatile) {
  if (ctr_equal(tok, "const") || ctr_equal(tok, "__const") || ctr_equal(tok, "__const__")) {
    *is_const = 1;
    return 1;
  }
  if (ctr_equal(tok, "volatile") || ctr_equal(tok, "__volatile") || ctr_equal(tok, "__volatile__")) {
    *is_volatile = 1;
    return 1;
  }
  return ctr_equal(tok, "restrict") || ctr_equal(tok, "__restrict") ||
         ctr_equal(tok, "__restrict__") || ctr_equal(tok, "_Atomic");
}

static int keyword_bits(CtrToken *tok) {
  static const struct {
    const char *name;
    int bits;
  } table[] = {{"void", K_VOID},       {"_Bool", K_BOOL},      {"char", K_CHAR},
               {"short", K_SHORT},     {"int", K_INT},         {"long", K_LONG},
               {"float", K_FLOAT},     {"double", K_DOUBLE},   {"signed", K_SIGNED},
               {"__signed", K_SIGNED}, {"__signed__", K_SIGNED}, {"unsigned", K_UNSIGNED},
               {"__int64", K_INT64},   {"__int32", K_INT},     {"__int16", K_SHORT},
               {"__int8", K_CHAR},     {"_Float32", K_FLOAT},  {"_Float64", K_DOUBLE}};
  for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++) {
    if (ctr_equal(tok, table[i].name)) {
      return table[i].bits;
    }
  }
  return 0;
}

static CtrType *type_for_counter(int counter, CtrToken *tok) {
  int u = (counter & K_UNSIGNED) != 0;
  int base = counter & ~(K_SIGNED | K_UNSIGNED);
  int s = (counter & K_SIGNED) != 0;
  switch (base) {
  case K_VOID:
    return ctr_ty_void;
  case K_BOOL:
    return ctr_ty_bool;
  case K_CHAR:
    return u ? ctr_ty_uchar : s ? ctr_ty_schar : ctr_ty_char;
  case K_SHORT:
  case K_SHORT + K_INT:
    return u ? ctr_ty_ushort : ctr_ty_short;
  case 0:
  case K_INT:
    return u ? ctr_ty_uint : ctr_ty_int;
  case K_LONG:
  case K_LONG + K_INT:
    return u ? ctr_ty_ulong : ctr_ty_long;
  case K_LONG + K_LONG:
  case K_LONG + K_LONG + K_INT:
  case K_INT64:
    return u ? ctr_ty_ullong : ctr_ty_llong;
  case K_FLOAT:
    return ctr_ty_float;
  case K_DOUBLE:
    return ctr_ty_double;
  case K_LONG + K_DOUBLE:
    return ctr_ty_ldouble;
  default:
    ctr_fatal_tok(tok, "invalid type");
    return NULL;
  }
}

static CtrType *typeof_specifier(CtrToken **rest, CtrToken *tok) {
  tok = ctr_skip(tok, "(");
  CtrType *ty;
  if (ctr_is_typename(tok)) {
    ty = ctr_typename(&tok, tok);
  } else {
    CtrNode *node = ctr_expr(&tok, tok);
    ctr_add_type(node);
    ty = node->ty;
  }
  *rest = ctr_skip(tok, ")");
  return ty;
}

static CtrType *va_list_type(void) {
  static CtrType *ty;
  if (!ty) {
    ty = ctr_copy_type(ctr_pointer_to(ctr_ty_char));
    ty->typedef_name = "__builtin_va_list";
  }
  return ty;
}

static CtrType *apply_qualifiers(CtrType *ty, int is_const, int is_volatile) {
  if (!is_const && !is_volatile) {
    return ty;
  }
  CtrType *t = ctr_copy_type(ty);
  t->is_const = is_const;
  t->is_volatile = is_volatile;
  return t;
}

static int other_type_spec(CtrToken **tokp, SpecState *st, CtrVarAttr *attr) {
  CtrToken *tok = *tokp;
  CtrType *ty2 = ctr_find_typedef(tok);
  if (ctr_equal(tok, "struct") || ctr_equal(tok, "union") || ctr_equal(tok, "enum") ||
      ctr_equal(tok, "typeof") || ctr_equal(tok, "__typeof__") || ctr_equal(tok, "__typeof") ||
      ctr_equal(tok, "__builtin_va_list") || ty2) {
    if (st->counter) {
      return 0;
    }
    if (ctr_equal(tok, "struct")) {
      st->ty = ctr_struct_union_decl(&tok, tok->next, 0);
    } else if (ctr_equal(tok, "union")) {
      st->ty = ctr_struct_union_decl(&tok, tok->next, 1);
    } else if (ctr_equal(tok, "enum")) {
      st->ty = ctr_enum_specifier(&tok, tok->next);
    } else if (ctr_equal(tok, "__builtin_va_list")) {
      st->ty = va_list_type();
      tok = tok->next;
    } else if (ty2) {
      ctr_sync_type(ty2);
      st->ty = ty2;
      tok = tok->next;
    } else {
      st->ty = typeof_specifier(&tok, tok->next);
    }
    st->counter += K_OTHER;
    *tokp = tok;
    return 1;
  }
  (void)attr;
  return 0;
}

static CtrToken *alignas_spec(CtrToken *tok, CtrVarAttr *attr) {
  tok = ctr_skip(tok->next, "(");
  int64_t a;
  if (ctr_is_typename(tok)) {
    CtrType *t = ctr_typename(&tok, tok);
    a = t->align;
  } else {
    a = ctr_const_expr(&tok, tok);
  }
  if (attr && a > attr->align) {
    attr->align = (int)a;
  }
  return ctr_skip(tok, ")");
}

CtrType *ctr_declspec(CtrToken **rest, CtrToken *tok, CtrVarAttr *attr) {
  SpecState st = {0, ctr_ty_int};
  int is_const = 0, is_volatile = 0;
  CtrVarAttr dummy = {0};
  if (!attr) {
    attr = &dummy;
  }
  while (ctr_is_typename(tok) || ctr_equal(tok, "__ctr_pragma_pack")) {
    tok = ctr_skip_attributes(tok, attr);
    if (!ctr_is_typename(tok)) {
      break;
    }
    if (storage_spec(tok, attr)) {
      if (attr == &dummy && (ctr_equal(tok, "typedef") || ctr_equal(tok, "static") ||
                             ctr_equal(tok, "extern"))) {
        ctr_fatal_tok(tok, "storage class specifier is not allowed in this context");
      }
      tok = tok->next;
      continue;
    }
    if (qualifier_spec(tok, &is_const, &is_volatile)) {
      tok = tok->next;
      continue;
    }
    if (ctr_equal(tok, "_Alignas")) {
      tok = alignas_spec(tok, attr);
      continue;
    }
    if (ctr_equal(tok, "_Complex")) {
      ctr_fatal_tok(tok, "_Complex has no Mettle counterpart");
    }
    if (ctr_equal(tok, "__auto_type")) {
      ctr_fatal_tok(tok, "__auto_type is not supported");
    }
    if (other_type_spec(&tok, &st, attr)) {
      continue;
    }
    int bits = keyword_bits(tok);
    if (!bits || (st.counter & K_OTHER)) {
      break;
    }
    st.counter += bits;
    st.ty = type_for_counter(st.counter, tok);
    tok = tok->next;
  }
  tok = ctr_skip_attributes(tok, attr);
  *rest = tok;
  return apply_qualifiers(st.ty, is_const, is_volatile);
}

static CtrType *func_params(CtrToken **rest, CtrToken *tok, CtrType *ty);
static CtrType *type_suffix(CtrToken **rest, CtrToken *tok, CtrType *ty);

static CtrType *param_one(CtrToken **rest, CtrToken *tok) {
  CtrVarAttr attr = {0};
  CtrType *ty2 = ctr_declspec(&tok, tok, &attr);
  ty2 = ctr_declarator(&tok, tok, ty2);
  CtrToken *name = ty2->name;
  if (ty2->kind == TY_ARRAY || ty2->kind == TY_VLA) {
    ty2 = ctr_pointer_to(ty2->base);
    ty2->name = name;
  } else if (ty2->kind == TY_FUNC) {
    ty2 = ctr_pointer_to(ty2);
    ty2->name = name;
  }
  *rest = tok;
  return ctr_copy_type(ty2);
}

static CtrObj *param_object(CtrType *param) {
  CtrObj *var = ctr_alloc(sizeof(CtrObj));
  var->name = param->name ? ctr_get_ident(param->name) : ctr_format("unnamed%d", ctr_ps.anon_count++);
  var->ty = param;
  var->tok = param->name ? param->name : param->name_pos;
  var->id = ++ctr_ps.obj_ids;
  var->file = var->tok ? var->tok->file : ctr_ps.unit_file;
  var->is_local = 1;
  var->is_param = 1;
  if (param->name) {
    ctr_push_scope(var->name)->var = var;
  }
  return var;
}

static CtrType *knr_params(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  CtrType head = {0};
  CtrType *cur = &head;
  while (!ctr_equal(tok, ")")) {
    if (cur != &head) {
      tok = ctr_skip(tok, ",");
    }
    CtrType *p = ctr_copy_type(ctr_ty_int);
    p->name = tok;
    tok = tok->next;
    cur = cur->next = p;
  }
  ty = ctr_func_type(ty);
  ty->params = head.next;
  ty->has_proto = 0;
  *rest = tok->next;
  return ty;
}

static CtrType *func_params(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  if (ctr_equal(tok, "void") && ctr_equal(tok->next, ")")) {
    *rest = tok->next->next;
    CtrType *f = ctr_func_type(ty);
    f->has_proto = 1;
    return f;
  }
  if (ctr_equal(tok, ")")) {
    *rest = tok->next;
    CtrType *f = ctr_func_type(ty);
    f->has_proto = 0;
    return f;
  }
  if (tok->kind == TK_IDENT && !ctr_is_typename(tok) &&
      (ctr_equal(tok->next, ",") || ctr_equal(tok->next, ")"))) {
    return knr_params(rest, tok, ty);
  }
  CtrType head = {0};
  CtrType *cur = &head;
  int is_variadic = 0;
  ctr_enter_scope();
  while (!ctr_equal(tok, ")")) {
    if (cur != &head) {
      tok = ctr_skip(tok, ",");
    }
    if (ctr_equal(tok, "...")) {
      is_variadic = 1;
      tok = tok->next;
      ctr_skip(tok, ")");
      break;
    }
    cur = cur->next = param_one(&tok, tok);
    cur->param_obj = param_object(cur);
  }
  ctr_leave_scope();
  ty = ctr_func_type(ty);
  ty->params = head.next;
  ty->is_variadic = is_variadic;
  ty->has_proto = 1;
  *rest = tok->next;
  return ty;
}

static CtrToken *array_qualifiers(CtrToken *tok) {
  while (ctr_equal(tok, "static") || ctr_equal(tok, "restrict") || ctr_equal(tok, "const") ||
         ctr_equal(tok, "volatile") || ctr_equal(tok, "__restrict") || ctr_equal(tok, "__restrict__")) {
    tok = tok->next;
  }
  return tok;
}

static CtrType *array_dimensions(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  tok = array_qualifiers(tok);
  if (ctr_equal(tok, "]")) {
    ty = type_suffix(rest, tok->next, ty);
    return ctr_array_of(ty, -1);
  }
  if (ctr_equal(tok, "*") && ctr_equal(tok->next, "]")) {
    ty = type_suffix(rest, tok->next->next, ty);
    return ctr_array_of(ty, -1);
  }
  CtrNode *expr = ctr_conditional(&tok, tok);
  tok = ctr_skip(tok, "]");
  ty = type_suffix(rest, tok, ty);
  ctr_add_type(expr);
  int64_t len;
  if (ty->kind != TY_VLA && ctr_try_eval(expr, &len)) {
    return ctr_array_of(ty, len);
  }
  return ctr_vla_of(ty, expr);
}

static CtrType *type_suffix(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  if (ctr_equal(tok, "(")) {
    return func_params(rest, tok->next, ty);
  }
  if (ctr_equal(tok, "[")) {
    return array_dimensions(rest, tok->next, ty);
  }
  *rest = tok;
  return ty;
}

static CtrType *pointers(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  while (ctr_consume(&tok, tok, "*")) {
    ty = ctr_pointer_to(ty);
    for (;;) {
      tok = ctr_skip_attributes(tok, NULL);
      int c = 0, v = 0;
      if (qualifier_spec(tok, &c, &v)) {
        if (c || v) {
          ty = apply_qualifiers(ty, c || ty->is_const, v || ty->is_volatile);
        }
        tok = tok->next;
        continue;
      }
      break;
    }
  }
  *rest = tok;
  return ty;
}

static int is_nested_declarator(CtrToken *tok) {
  tok = ctr_skip_attributes(tok, NULL);
  return !ctr_is_typename(tok) && !ctr_equal(tok, ")");
}

CtrType *ctr_declarator(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  tok = ctr_skip_attributes(tok, NULL);
  ty = pointers(&tok, tok, ty);
  tok = ctr_skip_attributes(tok, NULL);
  if (ctr_equal(tok, "(") && is_nested_declarator(tok->next)) {
    CtrToken *start = tok;
    CtrType dummy = {0};
    ctr_declarator(&tok, start->next, &dummy);
    tok = ctr_skip(tok, ")");
    ty = type_suffix(rest, tok, ty);
    return ctr_declarator(&tok, start->next, ty);
  }
  CtrToken *name = NULL;
  CtrToken *name_pos = tok;
  if (tok->kind == TK_IDENT) {
    name = tok;
    tok = tok->next;
  }
  tok = ctr_skip_attributes(tok, &ctr_ps.trailing);
  ty = type_suffix(&tok, tok, ty);
  tok = ctr_skip_attributes(tok, &ctr_ps.trailing);
  ty = ctr_copy_type(ty);
  ty->origin = ty->origin;
  ty->name = name;
  ty->name_pos = name_pos;
  *rest = tok;
  return ty;
}

static CtrType *declarator_with_attrs(CtrToken **rest, CtrToken *tok, CtrType *ty, CtrVarAttr *attr) {
  memset(&ctr_ps.trailing, 0, sizeof(ctr_ps.trailing));
  CtrType *out = ctr_declarator(rest, tok, ty);
  CtrVarAttr *t = &ctr_ps.trailing;
  if (t->mettle_name) {
    attr->mettle_name = t->mettle_name;
  }
  if (t->mettle_module) {
    attr->mettle_module = t->mettle_module;
  }
  if (t->mettle_alias) {
    attr->mettle_alias = t->mettle_alias;
  }
  attr->is_noreturn |= t->is_noreturn;
  attr->is_packed |= t->is_packed;
  attr->is_builtin |= t->is_builtin;
  if (t->align > attr->align) {
    attr->align = t->align;
  }
  return out;
}

CtrType *ctr_abstract_declarator(CtrToken **rest, CtrToken *tok, CtrType *ty) {
  tok = ctr_skip_attributes(tok, NULL);
  ty = pointers(&tok, tok, ty);
  tok = ctr_skip_attributes(tok, NULL);
  if (ctr_equal(tok, "(") && is_nested_declarator(tok->next)) {
    CtrToken *start = tok;
    CtrType dummy = {0};
    ctr_abstract_declarator(&tok, start->next, &dummy);
    tok = ctr_skip(tok, ")");
    ty = type_suffix(rest, tok, ty);
    return ctr_abstract_declarator(&tok, start->next, ty);
  }
  return type_suffix(rest, tok, ty);
}

CtrType *ctr_typename(CtrToken **rest, CtrToken *tok) {
  CtrType *ty = ctr_declspec(&tok, tok, NULL);
  return ctr_abstract_declarator(rest, tok, ty);
}

int ctr_is_end(CtrToken *tok) {
  return ctr_equal(tok, "}") || (ctr_equal(tok, ",") && ctr_equal(tok->next, "}"));
}

static int consume_end(CtrToken **rest, CtrToken *tok) {
  if (ctr_equal(tok, "}")) {
    *rest = tok->next;
    return 1;
  }
  if (ctr_equal(tok, ",") && ctr_equal(tok->next, "}")) {
    *rest = tok->next->next;
    return 1;
  }
  return 0;
}

static CtrObj *new_enum_const(CtrToken *name, CtrType *ty, int64_t val) {
  CtrObj *obj = ctr_alloc(sizeof(CtrObj));
  obj->name = ctr_token_text(name);
  obj->tok = name;
  obj->is_enum_const = 1;
  obj->enum_value = val;
  obj->enum_type = ty;
  obj->ty = ctr_ty_int;
  obj->file = name->file;
  obj->id = ++ctr_ps.obj_ids;
  obj->is_system = name->file && name->file->is_system;
  ctr_push_scope(obj->name)->enum_obj = obj;
  if (ctr_prog) {
    ctr_vec_push(&ctr_prog->enum_consts, obj);
  }
  return obj;
}

CtrType *ctr_enum_specifier(CtrToken **rest, CtrToken *tok) {
  CtrType *ty = ctr_enum_type();
  tok = ctr_skip_attributes(tok, NULL);
  CtrToken *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }
  if (tag && !ctr_equal(tok, "{")) {
    CtrType *found = find_tag(tag);
    if (!found) {
      push_tag_scope(tag, ty);
      ty->enum_name = ctr_token_text(tag);
      ty->enum_tok = tag;
      *rest = tok;
      return ty;
    }
    *rest = tok;
    return found;
  }
  tok = ctr_skip(tok, "{");
  int64_t val = 0;
  int i = 0;
  ty->enum_unsigned = 1;
  while (!consume_end(rest, tok)) {
    if (i++ > 0) {
      tok = ctr_skip(tok, ",");
    }
    CtrToken *name = tok;
    tok = ctr_skip_attributes(tok->next, NULL);
    if (ctr_equal(tok, "=")) {
      val = ctr_const_expr(&tok, tok->next);
    }
    new_enum_const(name, ty, val);
    ty->enum_unsigned &= val >= 0;
    val++;
  }
  *rest = ctr_skip_attributes(*rest, NULL);
  if (tag) {
    push_tag_scope(tag, ty);
    ty->enum_name = ctr_token_text(tag);
    ty->enum_tok = tag;
  }
  return ty;
}

static CtrMember *member_one(CtrToken **rest, CtrToken *tok, CtrType *basety, CtrVarAttr *attr,
                             int idx) {
  CtrMember *mem = ctr_alloc(sizeof(CtrMember));
  mem->ty = ctr_declarator(&tok, tok, basety);
  mem->name = mem->ty->name;
  mem->idx = idx;
  mem->align = attr->align ? attr->align : mem->ty->align;
  tok = ctr_skip_attributes(tok, attr);
  if (attr->align && attr->align > mem->align) {
    mem->align = attr->align;
  }
  if (ctr_consume(&tok, tok, ":")) {
    mem->is_bitfield = 1;
    mem->bit_width = (int)ctr_const_expr(&tok, tok);
  }
  mem->tok = mem->name ? mem->name : tok;
  *rest = ctr_skip_attributes(tok, NULL);
  return mem;
}

static CtrMember *struct_members(CtrToken **rest, CtrToken *tok, CtrStructDecl *decl) {
  CtrMember head = {0};
  CtrMember *cur = &head;
  int idx = 0;
  while (!ctr_equal(tok, "}")) {
    if (ctr_equal(tok, "__ctr_pragma_pack")) {
      tok = ctr_skip(ctr_skip(tok->next, "(")->next, ")");
      tok = ctr_skip(tok, ";");
      continue;
    }
    if (ctr_equal(tok, ";")) {
      tok = tok->next;
      continue;
    }
    CtrVarAttr attr = {0};
    CtrType *basety = ctr_declspec(&tok, tok, &attr);
    int first = 1;
    if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION) && ctr_consume(&tok, tok, ";")) {
      CtrMember *mem = ctr_alloc(sizeof(CtrMember));
      mem->ty = basety;
      mem->idx = idx++;
      mem->align = attr.align ? attr.align : basety->align;
      mem->anon = 1;
      mem->tok = tok;
      cur = cur->next = mem;
      continue;
    }
    while (!ctr_consume(&tok, tok, ";")) {
      if (!first) {
        tok = ctr_skip(tok, ",");
      }
      first = 0;
      cur = cur->next = member_one(&tok, tok, basety, &attr, idx++);
    }
  }
  if (cur != &head && cur->ty->kind == TY_ARRAY && cur->ty->array_len < 0) {
    cur->ty = ctr_array_of(cur->ty->base, 0);
    decl->is_flexible = 1;
  }
  *rest = tok->next;
  return head.next;
}

void ctr_finish_struct(CtrStructDecl *decl) {
  ctr_layout_struct(decl, ctr_prog->options->target);
  decl->complete = 1;
  if (decl->type) {
    decl->type->size = decl->size;
    decl->type->align = decl->align;
  }
}

static CtrType *struct_type_for(CtrStructDecl *decl) {
  CtrType *ty = ctr_struct_type();
  ty->kind = decl->is_union ? TY_UNION : TY_STRUCT;
  ty->sdecl = decl;
  ty->size = decl->complete ? decl->size : -1;
  ty->align = decl->complete ? decl->align : 1;
  return ty;
}

static CtrStructDecl *new_struct_decl(CtrToken *tag, int is_union, CtrToken *tok) {
  CtrStructDecl *decl = ctr_alloc(sizeof(CtrStructDecl));
  decl->tag = tag ? ctr_token_text(tag) : NULL;
  decl->is_union = is_union;
  decl->tok = tag ? tag : tok;
  decl->file = decl->tok->file;
  decl->id = ctr_prog->structs.len + 1;
  ctr_vec_push(&ctr_prog->structs, decl);
  return decl;
}

static CtrType *struct_reference(CtrToken *tag, int is_union, CtrToken *tok) {
  CtrType *found = find_tag(tag);
  if (found) {
    return found;
  }
  CtrStructDecl *decl = new_struct_decl(tag, is_union, tok);
  CtrType *ty = struct_type_for(decl);
  decl->type = ty;
  push_tag_scope(tag, ty);
  return ty;
}

static CtrStructDecl *struct_for_definition(CtrToken *tag, int is_union, CtrToken *tok) {
  if (tag) {
    CtrType *found = ctr_map_get2(&ctr_ps.scope->tags, tag->loc, tag->len);
    if (found && found->sdecl && !found->sdecl->complete) {
      return found->sdecl;
    }
  }
  CtrStructDecl *decl = new_struct_decl(tag, is_union, tok);
  CtrType *ty = struct_type_for(decl);
  decl->type = ty;
  if (tag) {
    push_tag_scope(tag, ty);
  }
  return decl;
}

CtrType *ctr_struct_union_decl(CtrToken **rest, CtrToken *tok, int is_union) {
  CtrVarAttr attr = {0};
  tok = ctr_skip_attributes(tok, &attr);
  CtrToken *tag = NULL;
  CtrToken *start = tok;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }
  if (tag && !ctr_equal(tok, "{")) {
    *rest = tok;
    return struct_reference(tag, is_union, start);
  }
  tok = ctr_skip(tok, "{");
  CtrStructDecl *decl = struct_for_definition(tag, is_union, start);
  decl->field_align = ctr_ps.pack;
  decl->members = struct_members(&tok, tok, decl);
  tok = ctr_skip_attributes(tok, &attr);
  decl->is_packed = attr.is_packed;
  if (attr.align) {
    for (CtrMember *m = decl->members; m; m = m->next) {
      (void)m;
    }
  }
  ctr_finish_struct(decl);
  if (attr.align > decl->align) {
    decl->align = attr.align;
    decl->size = (decl->size + attr.align - 1) / attr.align * attr.align;
    decl->type->align = decl->align;
    decl->type->size = decl->size;
  }
  *rest = tok;
  return decl->type;
}

static void handle_pragma_pack(CtrToken **rest, CtrToken *tok) {
  tok = ctr_skip(tok->next, "(");
  ctr_ps.pack = (int)tok->ival;
  tok = ctr_skip(tok->next, ")");
  *rest = ctr_skip(tok, ";");
}

static CtrObj *find_func(const char *name) {
  CtrScope *sc = ctr_ps.scope;
  while (sc->next) {
    sc = sc->next;
  }
  CtrVarScope *vs = ctr_map_get(&sc->vars, name);
  if (vs && vs->var && vs->var->is_function) {
    return vs->var;
  }
  return NULL;
}

static void typedef_one(CtrType *ty, CtrToken *name_tok) {
  if (!name_tok) {
    ctr_fatal_tok(ty->name_pos, "typedef name omitted");
  }
  CtrType *td = ctr_copy_type(ty);
  td->typedef_name = ctr_token_text(name_tok);
  CtrObj *obj = ctr_alloc(sizeof(CtrObj));
  obj->name = td->typedef_name;
  obj->tok = name_tok;
  obj->ty = ty;
  obj->is_typedef = 1;
  obj->file = name_tok->file;
  obj->id = ++ctr_ps.obj_ids;
  obj->is_system = name_tok->file && name_tok->file->is_system;
  td->typedef_obj = obj;
  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->sdecl && !ty->sdecl->tag) {
    ty->sdecl->tag = td->typedef_name;
  }
  if (ty->kind == TY_ENUM && !ty->enum_name) {
    ty->enum_name = td->typedef_name;
  }
  CtrVarScope *vs = ctr_push_scope(td->typedef_name);
  vs->type_def = td;
  vs->typedef_obj = obj;
  ctr_vec_push(&ctr_prog->typedefs, obj);
}

static CtrToken *parse_typedef(CtrToken *tok, CtrType *basety) {
  int first = 1;
  while (!ctr_consume(&tok, tok, ";")) {
    if (!first) {
      tok = ctr_skip(tok, ",");
    }
    first = 0;
    CtrType *ty = ctr_declarator(&tok, tok, basety);
    typedef_one(ty, ty->name);
  }
  return tok;
}

static int is_function_decl(CtrToken *tok) {
  if (ctr_equal(tok, ";")) {
    return 0;
  }
  CtrType dummy = {0};
  CtrType *ty = ctr_declarator(&tok, tok, &dummy);
  return ty->kind == TY_FUNC;
}

static void apply_attr_to_fn(CtrObj *fn, CtrVarAttr *attr) {
  if (attr->mettle_name) {
    fn->mettle_name = attr->mettle_name;
    fn->link_name = attr->mettle_name;
  }
  if (attr->mettle_module) {
    fn->mettle_module = attr->mettle_module;
  }
  if (attr->mettle_alias) {
    fn->mettle_alias = attr->mettle_alias;
  }
  if (attr->is_noreturn) {
    fn->is_noreturn = 1;
  }
  if (attr->is_builtin) {
    fn->is_builtin = 1;
  }
}

static CtrObj *declare_function(CtrType *ty, CtrVarAttr *attr, CtrToken *name_tok) {
  char *name = ctr_get_ident(name_tok);
  CtrObj *fn = find_func(name);
  if (fn) {
    if (!fn->ty->has_proto && ty->has_proto) {
      fn->ty = ty;
    }
    if (!fn->is_static && attr->is_static) {
      fn->is_static = 1;
    }
  } else {
    CtrScope *saved = ctr_ps.scope;
    while (ctr_ps.scope->next) {
      ctr_ps.scope = ctr_ps.scope->next;
    }
    fn = ctr_new_gvar(name, ty, name_tok);
    ctr_ps.scope = saved;
    fn->is_function = 1;
    fn->is_definition = 0;
    fn->is_static = attr->is_static;
    fn->is_inline = attr->is_inline;
    fn->is_system = name_tok->file && name_tok->file->is_system;
    CtrObj *known = fn->is_system ? NULL : ctr_map_get(&ctr_libc_mappings, name);
    if (known) {
      fn->mettle_name = known->mettle_name;
      fn->mettle_module = known->mettle_module;
    }
  }
  apply_attr_to_fn(fn, attr);
  return fn;
}

static void create_param_lvars(CtrType *param) {
  if (!param) {
    return;
  }
  create_param_lvars(param->next);
  CtrObj *v = param->param_obj;
  if (!v) {
    v = ctr_new_lvar(param->name ? ctr_get_ident(param->name) : ctr_format("unnamed%d", ctr_ps.anon_count++),
                     param, param->name ? param->name : param->name_pos);
    v->is_param = 1;
    return;
  }
  v->ty = param;
  v->owner = ctr_ps.current_fn;
  v->next = ctr_ps.locals;
  ctr_ps.locals = v;
  if (param->name) {
    ctr_push_scope(v->name)->var = v;
  }
}

static CtrToken *knr_declarations(CtrToken *tok, CtrType *fnty) {
  while (!ctr_equal(tok, "{")) {
    CtrVarAttr attr = {0};
    CtrType *basety = ctr_declspec(&tok, tok, &attr);
    int first = 1;
    while (!ctr_consume(&tok, tok, ";")) {
      if (!first) {
        tok = ctr_skip(tok, ",");
      }
      first = 0;
      CtrType *ty = ctr_declarator(&tok, tok, basety);
      if (ty->kind == TY_ARRAY) {
        CtrToken *n = ty->name;
        ty = ctr_pointer_to(ty->base);
        ty->name = n;
      }
      for (CtrType *p = fnty->params; p; p = p->next) {
        if (p->name && ty->name && p->name->len == ty->name->len &&
            strncmp(p->name->loc, ty->name->loc, (size_t)p->name->len) == 0) {
          CtrType *next = p->next;
          CtrToken *pname = p->name;
          *p = *ctr_copy_type(ty);
          p->next = next;
          p->name = pname;
        }
      }
    }
  }
  return tok;
}

static void resolve_gotos(CtrObj *fn) {
  for (CtrNode *x = ctr_ps.gotos; x; x = x->inc) {
    CtrLabel *found = NULL;
    for (CtrLabel *l = ctr_ps.labels; l; l = l->next) {
      if (strcmp(l->name, x->label) == 0) {
        found = l;
        break;
      }
    }
    if (!found) {
      ctr_error_tok(x->tok, "use of undeclared label '%s'", x->label);
      continue;
    }
    found->used = 1;
    x->label_ref = found;
    x->target = found->node;
  }
  fn->labels = ctr_ps.labels;
  ctr_ps.gotos = NULL;
  ctr_ps.labels = NULL;
}

static CtrObj *reverse_locals(CtrObj *list) {
  CtrObj *out = NULL;
  while (list) {
    CtrObj *next = list->next;
    list->next = out;
    out = list;
    list = next;
  }
  return out;
}

CtrToken *ctr_function(CtrToken *tok, CtrType *basety, CtrVarAttr *attr) {
  CtrType *ty = declarator_with_attrs(&tok, tok, basety, attr);
  if (!ty->name) {
    ctr_fatal_tok(ty->name_pos, "function name omitted");
  }
  tok = ctr_skip_attributes(tok, attr);
  CtrObj *fn = declare_function(ty, attr, ty->name);
  if (ctr_consume(&tok, tok, ";")) {
    return tok;
  }
  if (ctr_equal(tok, ",")) {
    return ctr_global_variable(tok->next, basety, attr);
  }
  if (!ty->has_proto && ty->params) {
    tok = knr_declarations(tok, ty);
    fn->ty = ty;
  }
  if (fn->is_definition && fn->body) {
    ctr_error_tok(ty->name, "redefinition of '%s'", fn->name);
  }
  fn->is_definition = 1;
  fn->mettle_module = NULL;
  fn->mettle_name = NULL;
  fn->link_name = NULL;
  fn->ty = ty;
  fn->tok = ty->name;
  fn->file = ty->name->file;
  fn->is_static = fn->is_static || attr->is_static;
  fn->is_inline = attr->is_inline;
  ctr_ps.current_fn = fn;
  ctr_ps.locals = NULL;
  ctr_ps.block_depth = 0;
  ctr_enter_scope();
  create_param_lvars(ty->params);
  fn->params = ctr_ps.locals;
  ctr_ps.locals = NULL;
  if (ty->is_variadic) {
    fn->va_param = ctr_new_lvar("__va_area__", ctr_pointer_to(ctr_ty_char), ty->name);
    fn->va_param->is_param = 1;
    ctr_ps.locals = ctr_ps.locals->next;
    fn->va_param->next = NULL;
  }
  CtrToken *body_start = tok;
  tok = ctr_skip(tok, "{");
  fn->body = ctr_compound_stmt(&tok, tok);
  (void)body_start;
  fn->locals = reverse_locals(ctr_ps.locals);
  ctr_leave_scope();
  resolve_gotos(fn);
  ctr_ps.current_fn = NULL;
  return tok;
}

static void global_init(CtrToken **rest, CtrToken *tok, CtrObj *var) {
  CtrType *new_ty = NULL;
  var->init = ctr_parse_initializer(rest, tok, var->ty, &new_ty);
  var->ty = new_ty;
}

static CtrObj *find_global_obj(const char *name) {
  CtrScope *sc = ctr_ps.scope;
  while (sc->next) {
    sc = sc->next;
  }
  CtrVarScope *vs = ctr_map_get(&sc->vars, name);
  return vs ? vs->var : NULL;
}

static CtrObj *global_one(CtrToken **rest, CtrToken *tok, CtrType *basety, CtrVarAttr *attr) {
  CtrType *ty = declarator_with_attrs(&tok, tok, basety, attr);
  if (!ty->name) {
    ctr_fatal_tok(ty->name_pos, "variable name omitted");
  }
  tok = ctr_skip_attributes(tok, attr);
  if (ty->kind == TY_FUNC) {
    *rest = tok;
    return declare_function(ty, attr, ty->name);
  }
  char *name = ctr_get_ident(ty->name);
  CtrObj *prev = find_global_obj(name);
  CtrObj *var;
  if (prev && !prev->is_function) {
    var = prev;
    if (ty->kind == TY_ARRAY && ty->array_len >= 0) {
      var->ty = ty;
    }
    if (!attr->is_extern) {
      var->is_extern = 0;
      var->is_definition = 1;
    }
  } else {
    var = ctr_new_gvar(name, ty, ty->name);
    var->is_static = attr->is_static;
    var->is_extern = attr->is_extern;
    var->is_definition = !attr->is_extern;
    var->is_system = ty->name->file && ty->name->file->is_system;
  }
  if (attr->mettle_name) {
    var->mettle_name = attr->mettle_name;
    var->link_name = attr->mettle_name;
  }
  if (attr->mettle_module) {
    var->mettle_module = attr->mettle_module;
  }
  if (attr->is_tls) {
    ctr_warn_tok(ty->name, "thread-local storage has no Mettle counterpart; '%s' becomes an ordinary global", name);
  }
  if (attr->align && attr->align > var->ty->align) {
    var->ty = ctr_copy_type(var->ty);
    var->ty->align = attr->align;
  }
  if (ctr_equal(tok, "=")) {
    var->is_definition = 1;
    var->is_extern = 0;
    var->tok = ty->name;
    var->file = ty->name->file;
    global_init(&tok, tok->next, var);
  } else if (!attr->is_extern) {
    var->is_tentative = var->init == NULL;
  }
  *rest = tok;
  return var;
}

CtrToken *ctr_global_variable(CtrToken *tok, CtrType *basety, CtrVarAttr *attr) {
  int first = 1;
  while (!ctr_consume(&tok, tok, ";")) {
    if (!first) {
      tok = ctr_skip(tok, ",");
    }
    first = 0;
    global_one(&tok, tok, basety, attr);
  }
  return tok;
}

static CtrToken *static_assert_decl(CtrToken *tok) {
  tok = ctr_skip(tok->next, "(");
  int64_t v = ctr_const_expr(&tok, tok);
  if (ctr_consume(&tok, tok, ",")) {
    tok = tok->next;
  }
  tok = ctr_skip(tok, ")");
  if (!v) {
    ctr_error_tok(tok, "static assertion failed");
  }
  return ctr_skip(tok, ";");
}

static CtrToken *toplevel_one(CtrToken *tok) {
  if (ctr_equal(tok, "__ctr_pragma_pack")) {
    handle_pragma_pack(&tok, tok);
    return tok;
  }
  if (ctr_equal(tok, "_Static_assert")) {
    return static_assert_decl(tok);
  }
  if (ctr_equal(tok, ";")) {
    return tok->next;
  }
  if ((ctr_equal(tok, "asm") || ctr_equal(tok, "__asm__") || ctr_equal(tok, "__asm")) &&
      ctr_equal(tok->next, "(")) {
    ctr_warn_tok(tok, "top-level asm is dropped");
    tok = skip_balanced(tok->next);
    return ctr_skip(tok, ";");
  }
  CtrVarAttr attr = {0};
  CtrType *basety = ctr_declspec(&tok, tok, &attr);
  if (attr.is_typedef) {
    return parse_typedef(tok, basety);
  }
  if (is_function_decl(tok)) {
    return ctr_function(tok, basety, &attr);
  }
  return ctr_global_variable(tok, basety, &attr);
}

CtrObj *ctr_parse_unit(CtrToken *tok, CtrFile *file) {
  memset(&ctr_ps, 0, sizeof(ctr_ps));
  ctr_ps.unit_file = file;
  ctr_enter_scope();
  while (tok->kind != TK_EOF) {
    tok = toplevel_one(tok);
  }
  CtrObj *list = NULL;
  for (CtrObj *v = ctr_ps.globals; v;) {
    CtrObj *next = v->next;
    v->next = list;
    list = v;
    v = next;
  }
  return list;
}
