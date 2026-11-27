#include "translate/ctr_parse.h"

#include <string.h>

static int direct_block;
static int block_ids;

static CtrNode *stmt(CtrToken **rest, CtrToken *tok);

static int is_label_start(CtrToken *tok) {
  return tok->kind == TK_IDENT && ctr_equal(tok->next, ":") && !ctr_find_typedef(tok);
}

static CtrNode *transparent_block(CtrNode *list, CtrToken *tok) {
  if (list && !list->next) {
    return list;
  }
  CtrNode *b = ctr_new_node(ND_BLOCK, tok);
  b->body = list;
  b->op = 1;
  return b;
}

static CtrObj *static_local(CtrType *ty, CtrToken *name, CtrVarAttr *attr) {
  CtrObj *var = ctr_new_gvar(ctr_get_ident(name), ty, name);
  var->is_static = 1;
  var->owner = ctr_ps.current_fn;
  var->is_definition = 1;
  (void)attr;
  return var;
}

static CtrNode *local_static_decl(CtrToken **rest, CtrToken *tok, CtrType *ty, CtrVarAttr *attr) {
  CtrObj *var = static_local(ty, ty->name, attr);
  if (attr->align > var->ty->align) {
    var->ty = ctr_copy_type(var->ty);
    var->ty->align = attr->align;
  }
  if (ctr_equal(tok, "=")) {
    CtrType *new_ty = NULL;
    var->init = ctr_parse_initializer(&tok, tok->next, var->ty, &new_ty);
    var->ty = new_ty;
  }
  *rest = tok;
  return NULL;
}

static CtrNode *local_extern_decl(CtrToken **rest, CtrToken *tok, CtrType *ty, CtrVarAttr *attr) {
  CtrScope *sc = ctr_ps.scope;
  while (sc->next) {
    sc = sc->next;
  }
  char *name = ctr_get_ident(ty->name);
  CtrVarScope *found = ctr_map_get(&sc->vars, name);
  CtrObj *var;
  if (found && found->var) {
    var = found->var;
  } else {
    CtrScope *saved = ctr_ps.scope;
    ctr_ps.scope = sc;
    var = ctr_new_gvar(name, ty, ty->name);
    ctr_ps.scope = saved;
    var->is_static = 0;
    var->is_extern = attr->is_extern;
    var->is_definition = 0;
    var->is_function = ty->kind == TY_FUNC;
  }
  ctr_push_scope(name)->var = var;
  *rest = tok;
  return NULL;
}

static CtrNode *vla_decl(CtrType *ty, CtrToken *name) {
  CtrObj *var = ctr_new_lvar(ctr_get_ident(name), ty, name);
  CtrNode *n = ctr_new_node(ND_DECL, name);
  n->var = var;
  return n;
}

static CtrNode *local_one(CtrToken **rest, CtrToken *tok, CtrType *basety, CtrVarAttr *attr) {
  CtrType *ty = ctr_declarator(&tok, tok, basety);
  tok = ctr_skip_attributes(tok, attr);
  if (ty->kind == TY_VOID) {
    ctr_fatal_tok(ty->name_pos, "variable declared void");
  }
  if (!ty->name) {
    ctr_fatal_tok(ty->name_pos, "variable name omitted");
  }
  if (attr->is_static) {
    return local_static_decl(rest, tok, ty, attr);
  }
  if (attr->is_extern || ty->kind == TY_FUNC) {
    return local_extern_decl(rest, tok, ty, attr);
  }
  if (ty->kind == TY_VLA) {
    *rest = tok;
    if (ctr_ps.current_fn) {
      ctr_ps.current_fn->uses_va = ctr_ps.current_fn->uses_va;
    }
    return vla_decl(ty, ty->name);
  }
  CtrObj *var = ctr_new_lvar(ctr_get_ident(ty->name), ty, ty->name);
  if (attr->align > var->ty->align) {
    var->ty = ctr_copy_type(var->ty);
    var->ty->align = attr->align;
  }
  CtrNode *n = ctr_new_node(ND_DECL, ty->name);
  n->var = var;
  if (ctr_equal(tok, "=")) {
    CtrType *new_ty = NULL;
    n->decl_init = ctr_parse_initializer(&tok, tok->next, var->ty, &new_ty);
    var->ty = new_ty;
  }
  if (var->ty->size < 0) {
    ctr_error_tok(ty->name, "variable has incomplete type");
  }
  *rest = tok;
  return n;
}

CtrNode *ctr_declaration(CtrToken **rest, CtrToken *tok, CtrType *basety, CtrVarAttr *attr) {
  CtrNode head = {0};
  CtrNode *cur = &head;
  int first = 1;
  CtrToken *start = tok;
  while (!ctr_equal(tok, ";")) {
    if (!first) {
      tok = ctr_skip(tok, ",");
    }
    first = 0;
    CtrNode *n = local_one(&tok, tok, basety, attr);
    if (n) {
      cur = cur->next = n;
    }
  }
  *rest = tok->next;
  if (!head.next) {
    return ctr_new_node(ND_EMPTY, start);
  }
  return transparent_block(head.next, start);
}

static CtrNode *typedef_local(CtrToken **rest, CtrToken *tok, CtrType *basety) {
  int first = 1;
  while (!ctr_consume(&tok, tok, ";")) {
    if (!first) {
      tok = ctr_skip(tok, ",");
    }
    first = 0;
    CtrType *ty = ctr_declarator(&tok, tok, basety);
    if (!ty->name) {
      ctr_fatal_tok(ty->name_pos, "typedef name omitted");
    }
    CtrType *td = ctr_copy_type(ty);
    td->typedef_name = ctr_token_text(ty->name);
    CtrObj *obj = ctr_alloc(sizeof(CtrObj));
    obj->name = td->typedef_name;
    obj->tok = ty->name;
    obj->ty = ty;
    obj->is_typedef = 1;
    obj->file = ty->name->file;
    obj->owner = ctr_ps.current_fn;
    td->typedef_obj = obj;
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->sdecl && !ty->sdecl->tag) {
      ty->sdecl->tag = td->typedef_name;
    }
    CtrVarScope *vs = ctr_push_scope(td->typedef_name);
    vs->type_def = td;
    vs->typedef_obj = obj;
    ctr_vec_push(&ctr_prog->typedefs, obj);
  }
  *rest = tok;
  return ctr_new_node(ND_EMPTY, tok);
}

static CtrNode *block_item(CtrToken **rest, CtrToken *tok, int block_id) {
  if (ctr_equal(tok, "__ctr_pragma_pack")) {
    tok = ctr_skip(tok->next, "(");
    ctr_ps.pack = (int)tok->ival;
    tok = ctr_skip(tok->next, ")");
    *rest = ctr_skip(tok, ";");
    return ctr_new_node(ND_EMPTY, tok);
  }
  if (ctr_equal(tok, "_Static_assert")) {
    tok = ctr_skip(tok->next, "(");
    int64_t v = ctr_const_expr(&tok, tok);
    if (ctr_consume(&tok, tok, ",")) {
      tok = tok->next;
    }
    tok = ctr_skip(tok, ")");
    if (!v) {
      ctr_error_tok(tok, "static assertion failed");
    }
    *rest = ctr_skip(tok, ";");
    return ctr_new_node(ND_EMPTY, tok);
  }
  if (ctr_is_typename(tok) && !is_label_start(tok)) {
    CtrVarAttr attr = {0};
    CtrType *basety = ctr_declspec(&tok, tok, &attr);
    if (attr.is_typedef) {
      return typedef_local(rest, tok, basety);
    }
    if (ctr_equal(tok, ";")) {
      *rest = tok->next;
      return ctr_new_node(ND_EMPTY, tok);
    }
    return ctr_declaration(rest, tok, basety, &attr);
  }
  direct_block = block_id;
  CtrNode *n = stmt(rest, tok);
  ctr_add_type(n);
  return n;
}

CtrNode *ctr_compound_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_BLOCK, tok);
  CtrNode head = {0};
  CtrNode *cur = &head;
  int id = ++block_ids;
  node->ival = id;
  ctr_enter_scope();
  ctr_ps.block_depth++;
  while (!ctr_equal(tok, "}")) {
    if (tok->kind == TK_EOF) {
      ctr_fatal_tok(tok, "expected '}'");
    }
    CtrNode *item = block_item(&tok, tok, id);
    cur = cur->next = item;
  }
  ctr_ps.block_depth--;
  ctr_leave_scope();
  node->body = head.next;
  *rest = tok->next;
  return node;
}

static CtrNode *sub_stmt(CtrToken **rest, CtrToken *tok) {
  direct_block = 0;
  CtrNode *n = stmt(rest, tok);
  ctr_add_type(n);
  return n;
}

static CtrNode *return_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_RETURN, tok);
  if (ctr_consume(rest, tok->next, ";")) {
    return node;
  }
  CtrNode *exp = ctr_expr(&tok, tok->next);
  *rest = ctr_skip(tok, ";");
  ctr_add_type(exp);
  CtrType *rty = ctr_ps.current_fn->ty->return_ty;
  if (rty->kind == TY_VOID) {
    node->lhs = exp;
    node->op = 1;
    return node;
  }
  if (rty->kind != TY_STRUCT && rty->kind != TY_UNION) {
    if (exp->ty->kind == TY_ARRAY || exp->ty->kind == TY_FUNC || !ctr_same_type(exp->ty, rty)) {
      exp = ctr_new_cast(exp, ctr_unqual(rty));
    }
  }
  node->lhs = exp;
  return node;
}

static CtrNode *if_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_IF, tok);
  tok = ctr_skip(tok->next, "(");
  node->cond = ctr_expr(&tok, tok);
  tok = ctr_skip(tok, ")");
  node->then = sub_stmt(&tok, tok);
  if (ctr_equal(tok, "else")) {
    node->els = sub_stmt(&tok, tok->next);
  }
  *rest = tok;
  return node;
}

static CtrNode *switch_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_SWITCH, tok);
  tok = ctr_skip(tok->next, "(");
  node->cond = ctr_expr(&tok, tok);
  ctr_add_type(node->cond);
  if (ctr_is_integer(node->cond->ty)) {
    node->cond = ctr_new_cast(node->cond, ctr_promote(node->cond->ty));
    if (ctr_same_type(node->cond->lhs->ty, node->cond->ty)) {
      node->cond = node->cond->lhs;
    }
  }
  tok = ctr_skip(tok, ")");
  CtrNode *sw = ctr_ps.current_switch;
  CtrNode *brk = ctr_ps.brk_target;
  ctr_ps.current_switch = node;
  ctr_ps.brk_target = node;
  if (ctr_equal(tok, "{")) {
    node->ival = block_ids + 1;
    node->then = ctr_compound_stmt(&tok, tok->next);
  } else {
    node->irregular = 1;
    node->ival = -1;
    node->then = sub_stmt(&tok, tok);
  }
  if (node->irregular) {
    ctr_ps.current_fn->has_irregular_case = 1;
  }
  ctr_ps.current_switch = sw;
  ctr_ps.brk_target = brk;
  *rest = tok;
  return node;
}

static CtrNode *case_stmt(CtrToken **rest, CtrToken *tok, int is_default) {
  CtrNode *sw = ctr_ps.current_switch;
  if (!sw) {
    ctr_fatal_tok(tok, "'%s' statement not in switch statement", is_default ? "default" : "case");
  }
  CtrNode *node = ctr_new_node(ND_CASE, tok);
  int regular = direct_block == sw->ival;
  if (is_default) {
    tok = ctr_skip(tok->next, ":");
    node->is_default = 1;
    sw->default_case = node;
  } else {
    int64_t begin = ctr_const_expr(&tok, tok->next);
    int64_t end = begin;
    if (ctr_equal(tok, "...")) {
      end = ctr_const_expr(&tok, tok->next);
    }
    tok = ctr_skip(tok, ":");
    node->case_begin = begin;
    node->case_end = end;
  }
  if (!regular) {
    sw->irregular = 1;
    ctr_ps.current_fn->has_irregular_case = 1;
  }
  node->target = sw;
  node->case_next = sw->case_next;
  sw->case_next = node;
  int saved = direct_block;
  if (ctr_equal(tok, "}")) {
    node->lhs = ctr_new_node(ND_EMPTY, tok);
    *rest = tok;
    return node;
  }
  direct_block = regular ? saved : 0;
  node->lhs = stmt(rest, tok);
  ctr_add_type(node->lhs);
  return node;
}

static CtrNode *loop_body(CtrToken **rest, CtrToken *tok, CtrNode *node) {
  CtrNode *brk = ctr_ps.brk_target;
  CtrNode *cont = ctr_ps.cont_target;
  ctr_ps.brk_target = node;
  ctr_ps.cont_target = node;
  CtrNode *body = sub_stmt(rest, tok);
  ctr_ps.brk_target = brk;
  ctr_ps.cont_target = cont;
  return body;
}

static CtrNode *for_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_FOR, tok);
  tok = ctr_skip(tok->next, "(");
  ctr_enter_scope();
  ctr_ps.block_depth++;
  if (ctr_is_typename(tok)) {
    CtrVarAttr attr = {0};
    CtrType *basety = ctr_declspec(&tok, tok, &attr);
    node->init = ctr_declaration(&tok, tok, basety, &attr);
  } else if (ctr_equal(tok, ";")) {
    tok = tok->next;
  } else {
    CtrNode *e = ctr_new_node(ND_EXPR_STMT, tok);
    e->lhs = ctr_expr(&tok, tok);
    node->init = e;
    tok = ctr_skip(tok, ";");
  }
  if (!ctr_equal(tok, ";")) {
    node->cond = ctr_expr(&tok, tok);
  }
  tok = ctr_skip(tok, ";");
  if (!ctr_equal(tok, ")")) {
    node->inc = ctr_expr(&tok, tok);
  }
  tok = ctr_skip(tok, ")");
  node->then = loop_body(&tok, tok, node);
  ctr_ps.block_depth--;
  ctr_leave_scope();
  *rest = tok;
  return node;
}

static CtrNode *while_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_WHILE, tok);
  tok = ctr_skip(tok->next, "(");
  node->cond = ctr_expr(&tok, tok);
  tok = ctr_skip(tok, ")");
  node->then = loop_body(rest, tok, node);
  return node;
}

static CtrNode *do_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_DO, tok);
  node->then = loop_body(&tok, tok->next, node);
  tok = ctr_skip(tok, "while");
  tok = ctr_skip(tok, "(");
  node->cond = ctr_expr(&tok, tok);
  tok = ctr_skip(tok, ")");
  *rest = ctr_skip(tok, ";");
  return node;
}

static CtrNode *goto_stmt(CtrToken **rest, CtrToken *tok) {
  if (ctr_equal(tok->next, "*")) {
    CtrNode *node = ctr_new_node(ND_GOTO_EXPR, tok);
    node->lhs = ctr_expr(&tok, tok->next->next);
    *rest = ctr_skip(tok, ";");
    ctr_ps.current_fn->has_goto = 1;
    return node;
  }
  CtrNode *node = ctr_new_node(ND_GOTO, tok);
  node->label = ctr_get_ident(tok->next);
  node->inc = ctr_ps.gotos;
  ctr_ps.gotos = node;
  ctr_ps.current_fn->has_goto = 1;
  *rest = ctr_skip(tok->next->next, ";");
  return node;
}

static CtrNode *jump_stmt(CtrToken **rest, CtrToken *tok, int is_break) {
  CtrNode *target = is_break ? ctr_ps.brk_target : ctr_ps.cont_target;
  if (!target) {
    ctr_fatal_tok(tok, "'%s' statement not in loop%s", is_break ? "break" : "continue",
                  is_break ? " or switch" : "");
  }
  CtrNode *node = ctr_new_node(is_break ? ND_BREAK : ND_CONTINUE, tok);
  node->target = target;
  if (is_break) {
    target->has_break = 1;
  } else {
    target->has_continue = 1;
  }
  *rest = ctr_skip(tok->next, ";");
  return node;
}

static CtrNode *label_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_LABEL, tok);
  node->label = ctr_token_text(tok);
  CtrLabel *l = ctr_alloc(sizeof(CtrLabel));
  l->name = node->label;
  l->node = node;
  l->next = ctr_ps.labels;
  ctr_ps.labels = l;
  tok = ctr_skip_attributes(tok->next->next, NULL);
  if (ctr_equal(tok, "}")) {
    node->lhs = ctr_new_node(ND_EMPTY, tok);
    *rest = tok;
    return node;
  }
  node->lhs = stmt(rest, tok);
  ctr_add_type(node->lhs);
  return node;
}

static CtrNode *asm_stmt(CtrToken **rest, CtrToken *tok) {
  CtrNode *node = ctr_new_node(ND_ASM, tok);
  tok = tok->next;
  while (ctr_equal(tok, "volatile") || ctr_equal(tok, "__volatile__") || ctr_equal(tok, "inline") ||
         ctr_equal(tok, "goto")) {
    tok = tok->next;
  }
  int depth = 0;
  do {
    depth += ctr_equal(tok, "(") ? 1 : ctr_equal(tok, ")") ? -1 : 0;
    tok = tok->next;
  } while (depth > 0 && tok->kind != TK_EOF);
  ctr_error_tok(node->tok, "inline assembly has no translation; rewrite it in Mettle's asm or C");
  *rest = ctr_skip(tok, ";");
  return node;
}

static CtrNode *expr_stmt(CtrToken **rest, CtrToken *tok) {
  if (ctr_equal(tok, ";")) {
    *rest = tok->next;
    return ctr_new_node(ND_EMPTY, tok);
  }
  CtrNode *node = ctr_new_node(ND_EXPR_STMT, tok);
  node->lhs = ctr_expr(&tok, tok);
  *rest = ctr_skip(tok, ";");
  return node;
}

static CtrNode *keyword_stmt(CtrToken **rest, CtrToken *tok, int *ok) {
  *ok = 1;
  if (ctr_equal(tok, "return")) {
    return return_stmt(rest, tok);
  }
  if (ctr_equal(tok, "if")) {
    return if_stmt(rest, tok);
  }
  if (ctr_equal(tok, "switch")) {
    return switch_stmt(rest, tok);
  }
  if (ctr_equal(tok, "case")) {
    return case_stmt(rest, tok, 0);
  }
  if (ctr_equal(tok, "default")) {
    return case_stmt(rest, tok, 1);
  }
  if (ctr_equal(tok, "for")) {
    return for_stmt(rest, tok);
  }
  if (ctr_equal(tok, "while")) {
    return while_stmt(rest, tok);
  }
  if (ctr_equal(tok, "do")) {
    return do_stmt(rest, tok);
  }
  if (ctr_equal(tok, "goto")) {
    return goto_stmt(rest, tok);
  }
  if (ctr_equal(tok, "break")) {
    return jump_stmt(rest, tok, 1);
  }
  if (ctr_equal(tok, "continue")) {
    return jump_stmt(rest, tok, 0);
  }
  if (ctr_equal(tok, "asm") || ctr_equal(tok, "__asm__") || ctr_equal(tok, "__asm")) {
    return asm_stmt(rest, tok);
  }
  *ok = 0;
  return NULL;
}

static CtrNode *stmt(CtrToken **rest, CtrToken *tok) {
  int ok;
  CtrNode *k = keyword_stmt(rest, tok, &ok);
  if (ok) {
    return k;
  }
  if (is_label_start(tok)) {
    return label_stmt(rest, tok);
  }
  if (ctr_equal(tok, "{")) {
    return ctr_compound_stmt(rest, tok->next);
  }
  if (ctr_equal(tok, "__label__")) {
    while (!ctr_equal(tok, ";")) {
      tok = tok->next;
    }
    *rest = tok->next;
    return ctr_new_node(ND_EMPTY, tok);
  }
  return expr_stmt(rest, tok);
}
