#include "translate/ctr_emit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Item {
  int file_order;
  int line;
  int col;
  int seq;
  char *text;
} Item;

static CtrEmitter program_em;
static CtrMap modules_by_key;
static CtrMap module_names;
static CtrMap file_orders;
static CtrModule *system_mod;
static const char *out_dir;
static int item_seq;

static int multi_module(void) { return ctr_prog->units.len > 1; }

static CtrModule *first_module(void) { return ((CtrUnit *)ctr_prog->units.items[0])->module; }

void ctr_use_module(CtrModule *home) {
  if (!home || !ctr_em || !ctr_em->mod || home == ctr_em->mod) {
    return;
  }
  CtrModule *mod = ctr_em->mod;
  for (int i = 0; i < mod->imports.len; i++) {
    if (mod->imports.items[i] == home) {
      return;
    }
  }
  ctr_vec_push(&mod->imports, home);
}

static const char *folded(const char *name) {
  char *s = ctr_strdup(name);
  for (char *p = s; *p; p++) {
    if (*p >= 'A' && *p <= 'Z') {
      *p = (char)(*p + 32);
    }
  }
  return s;
}

static const char *unique_module_name(const char *base) {
  const char *name = base;
  for (int i = 2; ctr_map_get(&module_names, folded(name)); i++) {
    name = ctr_format("%s_%d", base, i);
  }
  ctr_map_put(&module_names, folded(name), (void *)1);
  return name;
}

static CtrModule *new_module(const char *base, CtrFile *file) {
  CtrModule *mod = ctr_alloc(sizeof(CtrModule));
  mod->file = file;
  mod->name = unique_module_name(base);
  mod->out_path = out_dir ? ctr_path_join(out_dir, ctr_format("%s.mettle", mod->name)) : NULL;
  mod->index = ctr_prog->modules.len;
  ctr_vec_push(&ctr_prog->modules, mod);
  return mod;
}

static CtrModule *system_module(void) {
  if (!multi_module()) {
    return first_module();
  }
  if (!system_mod) {
    system_mod = new_module("c_decls", NULL);
  }
  return system_mod;
}

static CtrModule *module_of_file(CtrFile *file) {
  if (!file) {
    return first_module();
  }
  if (file->is_system) {
    return system_module();
  }
  CtrModule *mod = file->key ? ctr_map_get(&modules_by_key, file->key) : NULL;
  return mod ? mod : first_module();
}

static CtrModule *module_of_token(CtrToken *tok) {
  while (tok && tok->origin) {
    tok = tok->origin;
  }
  return module_of_file(tok ? tok->file : NULL);
}

static const char *single_output_path(CtrProgram *prog, CtrUnit *unit) {
  if (prog->options->output) {
    return prog->options->output;
  }
  return ctr_path_join(ctr_path_dir(unit->path), ctr_format("%s.mettle", ctr_path_stem(unit->path)));
}

static void create_unit_modules(CtrProgram *prog) {
  for (int u = 0; u < prog->units.len; u++) {
    CtrUnit *unit = prog->units.items[u];
    unit->module = new_module(ctr_path_stem(unit->path), unit->file);
    unit->module->is_unit = 1;
    if (!out_dir) {
      unit->module->out_path = single_output_path(prog, unit);
    }
    ctr_map_put(&modules_by_key, unit->file->key, unit->module);
  }
}

static void create_header_modules(CtrProgram *prog) {
  for (int u = 0; u < prog->units.len; u++) {
    CtrUnit *unit = prog->units.items[u];
    for (int i = 0; i < unit->files.len; i++) {
      CtrFile *f = unit->files.items[i];
      if (f->is_system || ctr_map_get(&modules_by_key, f->key)) {
        continue;
      }
      CtrModule *mod = multi_module() ? new_module(ctr_format("%s_h", ctr_path_stem(f->name)), f) : unit->module;
      ctr_map_put(&modules_by_key, f->key, mod);
    }
  }
}

static void create_modules(CtrProgram *prog) {
  memset(&modules_by_key, 0, sizeof(modules_by_key));
  memset(&module_names, 0, sizeof(module_names));
  memset(&file_orders, 0, sizeof(file_orders));
  system_mod = NULL;
  out_dir = NULL;
  if (multi_module()) {
    CtrUnit *first = prog->units.items[0];
    out_dir = prog->options->output ? prog->options->output : ctr_path_dir(first->path);
  }
  create_unit_modules(prog);
  create_header_modules(prog);
}

static int shares_symbol(CtrObj *g) {
  return !g->is_static && !g->is_typedef && !g->mettle_module && !g->owner && g->name[0] != '.';
}

static int defines_symbol(CtrObj *g) {
  if (g->is_function) {
    return g->is_definition && g->body;
  }
  return g->is_definition || !g->is_extern;
}

static int better_definition(CtrObj *prev, CtrObj *g) {
  return !prev || (!prev->is_function && !prev->init && g->init);
}

static void collect_definitions(CtrProgram *prog, CtrMap *defs) {
  for (int u = 0; u < prog->units.len; u++) {
    CtrUnit *unit = prog->units.items[u];
    for (CtrObj *g = unit->globals; g; g = g->next) {
      g->home = unit->module;
      if (shares_symbol(g) && defines_symbol(g) && better_definition(ctr_map_get(defs, g->name), g)) {
        ctr_map_put(defs, g->name, g);
      }
    }
  }
}

static void link_declarations(CtrProgram *prog, CtrMap *defs) {
  for (int u = 0; u < prog->units.len; u++) {
    CtrUnit *unit = prog->units.items[u];
    for (CtrObj *g = unit->globals; g; g = g->next) {
      if (!shares_symbol(g)) {
        continue;
      }
      CtrObj *def = ctr_map_get(defs, g->name);
      if (def && def != g) {
        g->home = def->home;
        g->skip_emit = 1;
      } else if (!def) {
        g->home = system_module();
      }
    }
  }
}

static void assign_homes(CtrProgram *prog) {
  CtrMap defs = {0};
  collect_definitions(prog, &defs);
  if (multi_module()) {
    link_declarations(prog, &defs);
  }
  for (int i = 0; i < prog->structs.len; i++) {
    CtrStructDecl *decl = prog->structs.items[i];
    CtrStructDecl *c = decl->canonical ? decl->canonical : decl;
    if (!c->home) {
      c->home = c->tok ? module_of_token(c->tok) : module_of_file(c->file);
    }
    decl->home = c->home;
  }
  for (int i = 0; i < prog->enum_consts.len; i++) {
    CtrObj *ec = prog->enum_consts.items[i];
    if (!ec->is_system) {
      ec->home = module_of_token(ec->tok);
    }
  }
  for (int i = 0; i < prog->macros.len; i++) {
    CtrMacro *m = prog->macros.items[i];
    if (m->recovered) {
      m->home = module_of_file(m->file);
    }
  }
}

static const char *init_base_name(CtrModule *mod) {
  if (!multi_module()) {
    return "init_globals";
  }
  CtrBuf b = {0};
  ctr_buf_put(&b, "init_");
  for (const char *p = mod->name; *p; p++) {
    char c = *p;
    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    ctr_buf_putc(&b, ok ? c : '_');
  }
  return ctr_buf_take(&b);
}

static void name_global_inits(CtrProgram *prog) {
  for (int u = 0; u < prog->units.len; u++) {
    CtrUnit *unit = prog->units.items[u];
    if (ctr_needs_global_init(unit->globals)) {
      unit->module->init_name = ctr_claim_name(init_base_name(unit->module));
    }
  }
}

static int item_cmp(const void *a, const void *b) {
  const Item *x = *(const Item *const *)a, *y = *(const Item *const *)b;
  if (x->file_order != y->file_order) {
    return x->file_order - y->file_order;
  }
  if (x->line != y->line) {
    return x->line - y->line;
  }
  if (x->col != y->col) {
    return x->col - y->col;
  }
  return x->seq - y->seq;
}

static int file_order(CtrModule *mod, CtrFile *f) {
  if (!f) {
    return -1;
  }
  if (mod->file && strcmp(mod->file->key, f->key) == 0) {
    return 1 << 20;
  }
  void *known = ctr_map_get(&file_orders, f->key);
  if (known) {
    return (int)(intptr_t)known;
  }
  for (int i = 0; i < ctr_prog->files.len; i++) {
    CtrFile *g = ctr_prog->files.items[i];
    if (strcmp(g->key, f->key) == 0) {
      ctr_map_put(&file_orders, f->key, (void *)(intptr_t)(i + 1));
      return i + 1;
    }
  }
  return 0;
}

static void add_item(CtrModule *mod, CtrToken *tok, char *text) {
  Item *it = ctr_alloc(sizeof(Item));
  CtrToken *t = tok;
  while (t && t->origin) {
    t = t->origin;
  }
  it->file_order = t ? file_order(mod, t->file) : -1;
  it->line = t ? t->line : 0;
  it->col = t ? t->col : 0;
  it->seq = item_seq++;
  it->text = text;
  ctr_vec_push(&mod->items, it);
}

static CtrMap wide_strings;

static int64_t wide_unit(const unsigned char *s, int elem) {
  uint64_t v = 0;
  for (int k = elem - 1; k >= 0; k--) {
    v = (v << 8) | s[k];
  }
  return (int64_t)v;
}

static const char *wide_unit_text(int64_t v) {
  if (v >= 32 && v < 127 && v != '\'' && v != '\\') {
    return ctr_format("'%c'", (int)v);
  }
  return ctr_format("%lld", (long long)v);
}

static const char *wide_base_name(const unsigned char *s, int count, int elem) {
  CtrBuf b = {0};
  ctr_buf_put(&b, "wide_");
  for (int i = 0; i < count && i < 24; i++) {
    int64_t v = wide_unit(s + i * elem, elem);
    int word = (v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9') || v == '_';
    if (!word) {
      free(b.data);
      return "wide";
    }
    ctr_buf_putc(&b, (char)v);
  }
  return count ? ctr_buf_take(&b) : "wide_empty";
}

const char *ctr_wide_string_name(CtrNode *str) {
  int elem = str->str_ty->base->size;
  int count = str->str_len / elem;
  const unsigned char *s = (const unsigned char *)str->str;
  CtrBuf key = {0};
  ctr_buf_printf(&key, "%d:%d:", ctr_em->mod->index, elem);
  for (int i = 0; i < str->str_len; i++) {
    ctr_buf_printf(&key, "%02x", s[i]);
  }
  if (ctr_em->mod->index < 0) {
    free(key.data);
    return "wide";
  }
  const char *known = ctr_map_get(&wide_strings, key.data);
  if (known) {
    return known;
  }
  const char *name = ctr_claim_name(wide_base_name(s, count - 1, elem));
  ctr_map_put(&wide_strings, ctr_buf_take(&key), (void *)name);
  CtrBuf text = {0};
  ctr_buf_printf(&text, "var %s: %s[%d] = [", name, ctr_type_text(str->str_ty->base), count);
  for (int i = 0; i < count; i++) {
    ctr_buf_printf(&text, "%s%s", i ? ", " : "", wide_unit_text(wide_unit(s + i * elem, elem)));
  }
  ctr_buf_put(&text, "];\n");
  add_item(ctr_em->mod, str->tok, ctr_buf_take(&text));
  return name;
}

static const char *volatile_prefix(CtrObj *var) {
  return var->ty->is_volatile && ctr_is_scalar(var->ty) && var->ty->kind != TY_PTR ? "volatile " : "";
}

static void emit_extern_var(CtrObj *var) {
  CtrModule *mod = var->home;
  if (ctr_map_get(&mod->externs, var->name)) {
    return;
  }
  ctr_map_put(&mod->externs, var->name, var);
  ctr_em->mod = mod;
  add_item(mod, var->tok,
           ctr_format("extern var %s: %s%s;\n", ctr_mettle_name_of(var), volatile_prefix(var), ctr_type_text(var->ty)));
}

static void emit_global_var(CtrModule *mod, CtrObj *var, CtrBuf *init_fn) {
  if (!var->is_definition && var->is_extern) {
    emit_extern_var(var);
    return;
  }
  ctr_em->mod = mod;
  const char *name = ctr_mettle_name_of(var);
  const char *ty = ctr_type_text(var->ty);
  const char *vol = volatile_prefix(var);
  const char *exp = multi_module() && !var->is_static ? "export " : "";
  int ok = 1;
  char *lit = var->init ? ctr_init_literal(var->init, var->ty, &ok) : NULL;
  if (ok && lit) {
    add_item(mod, var->tok, ctr_format("%svar %s: %s%s = %s;\n", exp, name, vol, ty, lit));
    return;
  }
  add_item(mod, var->tok, ctr_format("%svar %s: %s%s;\n", exp, name, vol, ty));
  if (ok) {
    return;
  }
  CtrBuf *saved = ctr_em->out;
  int saved_indent = ctr_em->indent;
  ctr_em->out = init_fn;
  ctr_em->indent = 1;
  ctr_emit_global_init_stmts(var);
  ctr_em->out = saved;
  ctr_em->indent = saved_indent;
}

static void emit_unit(CtrUnit *unit) {
  CtrModule *mod = unit->module;
  CtrBuf init_fn = {0};
  for (CtrObj *g = unit->globals; g; g = g->next) {
    if (g->is_function || g->is_typedef || g->mettle_module || g->is_system || g->skip_emit) {
      continue;
    }
    emit_global_var(mod, g, &init_fn);
  }
  for (CtrObj *g = unit->globals; g; g = g->next) {
    if (g->is_function && g->is_definition && g->body && !g->skip_emit) {
      ctr_em->mod = mod;
      CtrBuf b = {0};
      ctr_emit_function(&b, g);
      add_item(mod, g->tok, ctr_buf_take(&b));
    }
  }
  if (init_fn.len) {
    const char *exp = multi_module() ? "export " : "";
    add_item(mod, NULL, ctr_format("%sfn %s() {\n%s}\n", exp, mod->init_name, ctr_buf_take(&init_fn)));
  }
}

static const char *extern_params(CtrObj *g) {
  CtrBuf params = {0};
  int i = 0;
  for (CtrType *p = g->ty->params; p; p = p->next, i++) {
    const char *pname = p->name ? ctr_safe_ident(ctr_token_text(p->name)) : ctr_format("a%d", i);
    const char *pt = ctr_is_va_list(p) ? "rawptr" : ctr_type_text(p);
    ctr_buf_printf(&params, "%s%s: %s", i ? ", " : "", pname, pt);
  }
  if (g->ty->is_variadic && i) {
    ctr_buf_put(&params, ", ..");
  }
  return params.data ? ctr_buf_take(&params) : "";
}

static void emit_extern_fn(CtrObj *g) {
  CtrModule *mod = g->home;
  if (ctr_map_get(&mod->externs, g->name)) {
    return;
  }
  ctr_map_put(&mod->externs, g->name, g);
  ctr_em->mod = mod;
  const char *name = ctr_mettle_name_of(g);
  const char *params = extern_params(g);
  CtrType *rt = g->ty->return_ty;
  const char *ret = rt->kind == TY_VOID ? "" : ctr_format(" -> %s", ctr_return_type_text(rt));
  const char *link = strcmp(name, g->name) != 0 ? ctr_format(" = \"%s\"", g->name) : "";
  ctr_buf_printf(&mod->extern_text, "extern fn %s(%s)%s%s;\n", name, params, ret, link);
}

static void emit_externs(CtrUnit *unit) {
  for (CtrObj *g = unit->globals; g; g = g->next) {
    if (g->is_function && !g->is_definition && !g->mettle_module && g->referenced && !g->skip_emit) {
      emit_extern_fn(g);
    }
  }
}

static const char *macro_value_text(CtrMacro *m) {
  CtrType *ty = m->value_type;
  if (m->value_expr->kind == ND_STR) {
    return ctr_string_literal_text(m->value_expr->str, m->value_expr->str_len, 1);
  }
  if (ctr_is_flonum(ty)) {
    return ctr_float_text(ctr_eval_double(m->value_expr), ty->kind == TY_FLOAT);
  }
  int64_t v = 0;
  ctr_try_eval(m->value_expr, &v);
  CtrNode num = {0};
  num.kind = ND_NUM;
  num.ival = v;
  num.ty = ty;
  CtrNode *s = m->value_expr;
  while (s->kind == ND_CAST || s->kind == ND_MACRO) {
    s = s->lhs;
  }
  if (s->kind == ND_NUM && !s->is_float_lit) {
    num.spelling = s->spelling;
  }
  return ctr_literal_text(&num, ty);
}

static void emit_macro_consts(CtrProgram *prog) {
  for (int i = 0; i < prog->macros.len; i++) {
    CtrMacro *m = prog->macros.items[i];
    if (!m->recovered || !m->is_objlike || m->emitted) {
      continue;
    }
    m->emitted = 1;
    ctr_em->mod = m->home;
    const char *t = m->value_expr->kind == ND_STR ? "string" : ctr_type_text(m->value_type);
    add_item(m->home, m->def_tok, ctr_format("const %s: %s = %s;\n", m->mettle_name, t, macro_value_text(m)));
  }
}

static void emit_enum_consts(CtrProgram *prog) {
  for (int i = 0; i < prog->enum_consts.len; i++) {
    CtrObj *ec = prog->enum_consts.items[i];
    if (ec->is_system || !ec->mettle_name || ec->emitted) {
      continue;
    }
    add_item(ec->home, ec->tok, ctr_format("const %s: int32 = %lld;\n", ec->mettle_name, (long long)ec->enum_value));
  }
}

static int struct_pending(CtrStructDecl *decl) {
  if (!decl->complete || decl->canonical || decl->emitted) {
    return 0;
  }
  return !(decl->file && decl->file->is_system) || decl->used;
}

static void emit_structs(CtrProgram *prog) {
  for (int changed = 1; changed;) {
    changed = 0;
    for (int i = 0; i < prog->structs.len; i++) {
      CtrStructDecl *decl = prog->structs.items[i];
      if (!struct_pending(decl)) {
        continue;
      }
      decl->emitted = 1;
      changed = 1;
      ctr_em->mod = decl->home;
      CtrBuf b = {0};
      ctr_emit_struct(&b, decl);
      add_item(decl->home, decl->tok, ctr_buf_take(&b));
    }
  }
}

static int module_cmp(const void *a, const void *b) {
  const CtrModule *x = *(const CtrModule *const *)a, *y = *(const CtrModule *const *)b;
  return x->index - y->index;
}

static void separate(CtrBuf *out) {
  if (out->len) {
    ctr_buf_putc(out, '\n');
  }
}

static char *module_text(CtrModule *mod) {
  CtrBuf out = {0};
  if (mod->needs_c) {
    ctr_buf_put(&out, "import \"std/c\" as libc;\n");
  }
  if (mod->needs_math) {
    ctr_buf_put(&out, "import \"std/math\" as math;\n");
  }
  qsort(mod->imports.items, (size_t)mod->imports.len, sizeof(void *), module_cmp);
  for (int i = 0; i < mod->imports.len; i++) {
    CtrModule *imp = mod->imports.items[i];
    ctr_buf_printf(&out, "import \"%s\";\n", imp->name);
  }
  if (mod->extern_text.len) {
    separate(&out);
    ctr_buf_put(&out, mod->extern_text.data);
  }
  qsort(mod->items.items, (size_t)mod->items.len, sizeof(void *), item_cmp);
  for (int i = 0; i < mod->items.len; i++) {
    Item *it = mod->items.items[i];
    separate(&out);
    ctr_buf_put(&out, it->text);
  }
  return out.data ? ctr_buf_take(&out) : ctr_strdup("");
}

static void write_module(CtrModule *mod) {
  if (!mod->is_unit && !mod->items.len && !mod->extern_text.len) {
    return;
  }
  char *text = module_text(mod);
  if (!ctr_write_file(mod->out_path, text, strlen(text))) {
    ctr_fatal("cannot write '%s'", mod->out_path);
  }
  if (ctr_prog->options->verbose) {
    ctr_note("wrote %s", mod->out_path);
  }
}

void ctr_emit_program(CtrProgram *prog) {
  static CtrModule scratch;
  memset(&program_em, 0, sizeof(program_em));
  memset(&scratch, 0, sizeof(scratch));
  scratch.index = -1;
  memset(&wide_strings, 0, sizeof(wide_strings));
  ctr_em = &program_em;
  program_em.mod = &scratch;
  item_seq = 0;
  create_modules(prog);
  ctr_prepare_names(prog);
  ctr_recover_macros(prog);
  ctr_plan_all_structs(prog);
  assign_homes(prog);
  name_global_inits(prog);
  for (int u = 0; u < prog->units.len; u++) {
    emit_unit(prog->units.items[u]);
  }
  for (int u = 0; u < prog->units.len; u++) {
    emit_externs(prog->units.items[u]);
  }
  emit_macro_consts(prog);
  emit_enum_consts(prog);
  emit_structs(prog);
  if (out_dir && !ctr_make_dirs(out_dir)) {
    ctr_fatal("cannot create '%s'", out_dir);
  }
  for (int i = 0; i < prog->modules.len; i++) {
    write_module(prog->modules.items[i]);
  }
}
