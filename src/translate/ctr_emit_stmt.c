#include "translate/ctr_emit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Jump {
  struct Jump *next;
  CtrNode *on_break;
  CtrNode *on_continue_continue;
  CtrNode *on_continue_break;
  int breakable;
  int is_loop;
  int labelable;
  const char *label;
  int label_used;
} Jump;

static Jump *jumps;

static void indent_to(CtrBuf *b, int n) {
  for (int i = 0; i < n; i++) {
    ctr_buf_put(b, "  ");
  }
}

static void vline(CtrBuf *b, int indent, const char *fmt, va_list ap) {
  char tmp[256];
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, copy);
  va_end(copy);
  indent_to(b, indent);
  if (n < (int)sizeof(tmp)) {
    ctr_buf_put(b, tmp);
  } else {
    char *big = ctr_alloc((size_t)n + 1);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    ctr_buf_put(b, big);
  }
  ctr_buf_putc(b, '\n');
}

void ctr_line(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vline(ctr_em->out, ctr_em->indent, fmt, ap);
  va_end(ap);
}

void ctr_open(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vline(ctr_em->out, ctr_em->indent, fmt, ap);
  va_end(ap);
  ctr_em->indent++;
}

void ctr_close(void) {
  ctr_em->indent--;
  ctr_line("}");
}

void ctr_close_with(const char *fmt, ...) {
  ctr_em->indent--;
  va_list ap;
  va_start(ap, fmt);
  vline(ctr_em->out, ctr_em->indent, fmt, ap);
  va_end(ap);
}

static Jump *push(CtrNode *on_break, CtrNode *cc, CtrNode *cb, int is_loop) {
  Jump *j = ctr_alloc(sizeof(Jump));
  j->on_break = on_break;
  j->on_continue_continue = cc;
  j->on_continue_break = cb;
  j->breakable = 1;
  j->is_loop = is_loop;
  j->labelable = is_loop;
  j->next = jumps;
  jumps = j;
  return j;
}

static void pop(void) { jumps = jumps->next; }

static const char *use_label(Jump *j) {
  if (!j->label && j->on_break && j->on_break->kind == ND_WHILE && j->on_break->label) {
    j->label = ctr_safe_ident(j->on_break->label);
  }
  if (!j->label) {
    j->label = ctr_unique_label("loop");
  }
  j->label_used = 1;
  return j->label;
}

char *ctr_break_text(CtrNode *target) {
  int intervened = 0;
  for (Jump *j = jumps; j; j = j->next) {
    if (j->on_break == target && (!intervened || j->labelable)) {
      if (!intervened) {
        return "break;";
      }
      return ctr_format("break %s;", use_label(j));
    }
    if (j->breakable) {
      intervened = 1;
    }
  }
  ctr_error_tok(target ? target->tok : NULL, "break has no enclosing Mettle construct");
  return "break;";
}

char *ctr_continue_text(CtrNode *target) {
  int loop_between = 0;
  int breakable_between = 0;
  for (Jump *j = jumps; j; j = j->next) {
    if (j->on_continue_continue == target) {
      if (!loop_between) {
        return "continue;";
      }
      return ctr_format("continue %s;", use_label(j));
    }
    if (j->on_continue_break == target) {
      if (!breakable_between) {
        return "break;";
      }
      return ctr_format("break %s;", use_label(j));
    }
    if (j->is_loop) {
      loop_between = 1;
    }
    if (j->breakable) {
      breakable_between = 1;
    }
  }
  ctr_error_tok(target ? target->tok : NULL, "continue has no enclosing Mettle loop");
  return "continue;";
}

static CtrNode *strip(CtrNode *n) {
  while (n && n->kind == ND_MACRO) {
    n = n->lhs;
  }
  return n;
}

static int is_bitfield_node(CtrNode *n) {
  n = strip(n);
  return n && n->kind == ND_MEMBER && n->member && n->member->emit_kind == CTR_MEMBER_BITFIELD;
}

static int simple_lvalue(CtrNode *n) {
  n = strip(n);
  switch (n->kind) {
  case ND_VAR:
    return 1;
  case ND_MEMBER:
    return simple_lvalue(n->lhs);
  case ND_DEREF:
    return !ctr_has_side_effects(n->lhs);
  case ND_INDEX:
    return !ctr_has_side_effects(n->lhs) && !ctr_has_side_effects(n->rhs);
  case ND_CAST:
    return n->implicit && simple_lvalue(n->lhs);
  default:
    return 0;
  }
}

static CtrNode *stabilize(CtrNode *lhs) {
  if (simple_lvalue(lhs) || is_bitfield_node(lhs)) {
    return lhs;
  }
  char *t = ctr_new_temp("at");
  CtrObj *obj = ctr_alloc(sizeof(CtrObj));
  obj->name = t;
  obj->mettle_name = t;
  obj->ty = ctr_pointer_to(lhs->ty);
  obj->is_local = 1;
  ctr_line("var %s = &%s;", t, ctr_lvalue_text(lhs, PREC_POSTFIX));
  CtrNode *v = ctr_new_node(ND_VAR, lhs->tok);
  v->var = obj;
  v->ty = obj->ty;
  CtrNode *d = ctr_new_node(ND_DEREF, lhs->tok);
  d->lhs = v;
  d->ty = lhs->ty;
  return d;
}

static const char *mask_text(int width, int unit_bits) {
  uint64_t mask = width >= 64 ? ~0ull : ((1ull << width) - 1);
  (void)unit_bits;
  return ctr_format("0x%llx", (unsigned long long)mask);
}

char *ctr_bitfield_unit(CtrNode *node) {
  CtrMember *mem = node->member;
  CtrNode *base = strip(node->lhs);
  const char *ut = ctr_unit_type(mem->unit_size);
  if (mem->unit_is_field) {
    if (base->kind == ND_DEREF && base->op == 1) {
      return ctr_format("%s->%s", ctr_expr_text(base->lhs, PREC_POSTFIX), mem->unit_name);
    }
    return ctr_format("%s.%s", ctr_lvalue_text(node->lhs, PREC_POSTFIX), mem->unit_name);
  }
  char *addr = base->kind == ND_DEREF ? ctr_expr_text(base->lhs, PREC_UNARY)
                                      : ctr_format("&%s", ctr_lvalue_text(node->lhs, PREC_POSTFIX));
  return ctr_format("(*(%s*)((uint8*)%s + %d))", ut, addr, mem->unit_offset);
}

char *ctr_bitfield_read(CtrNode *node) {
  CtrMember *mem = node->member;
  int w = mem->bit_width;
  int off = mem->bit_offset;
  int bits = mem->unit_size * 8;
  char *unit = ctr_bitfield_unit(node);
  const char *rt = ctr_type_text(node->ty);
  if (mem->ty->is_unsigned || mem->ty->kind == TY_BOOL || (mem->ty->kind == TY_ENUM && mem->ty->enum_unsigned)) {
    char *shifted = off ? ctr_format("(%s >> %d)", unit, off) : unit;
    if (w == bits) {
      return ctr_format("(%s)%s", rt, shifted);
    }
    return ctr_format("(%s)(%s & %s)", rt, shifted, mask_text(w, bits));
  }
  const char *st = bits == 8 ? "int8" : bits == 16 ? "int16" : bits == 32 ? "int32" : "int64";
  int left = bits - off - w;
  char *up = left ? ctr_format("(%s << %d)", unit, left) : unit;
  return ctr_format("(%s)((%s)%s >> %d)", rt, st, up, bits - w);
}

void ctr_emit_bitfield_store(CtrNode *node, const char *value) {
  CtrMember *mem = node->member;
  int w = mem->bit_width;
  int off = mem->bit_offset;
  int bits = mem->unit_size * 8;
  const char *ut = ctr_unit_type(mem->unit_size);
  char *unit = ctr_bitfield_unit(node);
  if (w == bits) {
    ctr_line("%s = (%s)%s;", unit, ut, value);
    return;
  }
  const char *mask = mask_text(w, bits);
  char *placed = off ? ctr_format("(((%s)%s & %s) << %d)", ut, value, mask, off)
                     : ctr_format("((%s)%s & %s)", ut, value, mask);
  char *clear = off ? ctr_format("~((%s)%s << %d)", ut, mask, off) : ctr_format("~(%s)%s", ut, mask);
  ctr_line("%s = (%s & %s) | %s;", unit, unit, clear, placed);
}

static int same_mettle(CtrType *a, CtrType *b) { return ctr_same_mettle_type(a, b); }

static const char *opassign_op(int op) {
  switch (op) {
  case ND_ADD:
    return "+";
  case ND_SUB:
    return "-";
  case ND_MUL:
    return "*";
  case ND_DIV:
    return "/";
  case ND_MOD:
    return "%";
  case ND_BITAND:
    return "&";
  case ND_BITOR:
    return "|";
  case ND_BITXOR:
    return "^";
  case ND_SHL:
    return "<<";
  default:
    return ">>";
  }
}

static char *promoted_read(CtrNode *lhs, CtrType *op_ty) {
  if (is_bitfield_node(lhs)) {
    char *r = ctr_bitfield_read(strip(lhs));
    CtrType *rt = strip(lhs)->ty;
    if (same_mettle(rt, op_ty)) {
      return r;
    }
    return ctr_format("(%s)%s", ctr_type_text(op_ty), r);
  }
  return ctr_value_text(lhs, op_ty, PREC_MUL + 1);
}

static void emit_assign(CtrNode *node) {
  CtrNode *lhs = strip(node->lhs);
  if (is_bitfield_node(lhs)) {
    ctr_emit_bitfield_store(lhs, ctr_value_text(node->rhs, lhs->ty, PREC_UNARY));
    return;
  }
  ctr_line("%s = %s;", ctr_lvalue_text(node->lhs, PREC_LOWEST), ctr_assign_value_text(node->rhs, node->lhs->ty));
}

static void emit_pointer_opassign(CtrNode *node, CtrNode *lhs) {
  const char *op = node->op == ND_ADD ? "+" : "-";
  if (lhs->ty->base->kind == TY_VOID) {
    char *lv = ctr_lvalue_text(lhs, PREC_LOWEST);
    ctr_line("%s = (rawptr)((uint8*)%s %s %s);", lv, lv, op, ctr_expr_text(node->rhs, PREC_ADD + 1));
    return;
  }
  ctr_line("%s %s= %s;", ctr_lvalue_text(lhs, PREC_LOWEST), op, ctr_expr_text(node->rhs, PREC_LOWEST));
}

char *ctr_narrow_text(CtrNode *n, CtrType *to, int prec);

static int narrow_opassign(CtrNode *node, CtrNode *lhs) {
  int op = node->op;
  int wraps = op == ND_ADD || op == ND_SUB || op == ND_MUL || op == ND_BITAND || op == ND_BITOR || op == ND_BITXOR;
  if (!wraps || is_bitfield_node(lhs) || !ctr_is_integer(lhs->ty) || lhs->ty->kind == TY_BOOL) {
    return 0;
  }
  char *rhs = ctr_narrow_text(node->rhs, lhs->ty, PREC_LOWEST);
  if (!rhs) {
    return 0;
  }
  ctr_line("%s %s= %s;", ctr_lvalue_text(lhs, PREC_LOWEST), opassign_op(op), rhs);
  return 1;
}

static void emit_opassign(CtrNode *node) {
  CtrType *op_ty = node->cast_ty;
  int is_shift = node->op == ND_SHL || node->op == ND_SHR;
  int open = !simple_lvalue(node->lhs) && !is_bitfield_node(node->lhs);
  if (open) {
    ctr_open("{");
  }
  CtrNode *lhs = stabilize(node->lhs);
  if (lhs->ty->kind == TY_PTR) {
    emit_pointer_opassign(node, lhs);
  } else if (!is_bitfield_node(lhs) && same_mettle(lhs->ty, op_ty) && lhs->ty->kind != TY_BOOL) {
    CtrType *rt = is_shift ? lhs->ty : op_ty;
    ctr_line("%s %s= %s;", ctr_lvalue_text(lhs, PREC_LOWEST), opassign_op(node->op),
             ctr_value_text(node->rhs, rt, PREC_LOWEST));
  } else if (narrow_opassign(node, lhs)) {
  } else {
    CtrType *rt = is_shift ? op_ty : op_ty;
    char *value = ctr_format("%s %s %s", promoted_read(lhs, op_ty), opassign_op(node->op),
                             ctr_value_text(node->rhs, rt, PREC_MUL + 1));
    CtrType *target = is_bitfield_node(lhs) ? strip(lhs)->ty : lhs->ty;
    char *conv = target->kind == TY_BOOL ? ctr_format("(%s) != 0", value)
                                         : ctr_format("(%s)(%s)", ctr_type_text(target), value);
    if (is_bitfield_node(lhs)) {
      ctr_emit_bitfield_store(strip(lhs), conv);
    } else {
      ctr_line("%s = %s;", ctr_lvalue_text(lhs, PREC_LOWEST), conv);
    }
  }
  if (open) {
    ctr_close();
  }
}

static void emit_incdec(CtrNode *node) {
  int inc = node->kind == ND_PREINC || node->kind == ND_POSTINC;
  CtrNode *lhs = strip(node->lhs);
  if (is_bitfield_node(lhs)) {
    char *v = ctr_format("%s %s 1", ctr_bitfield_read(lhs), inc ? "+" : "-");
    ctr_emit_bitfield_store(lhs, ctr_format("(%s)", v));
    return;
  }
  int open = !simple_lvalue(lhs);
  if (open) {
    ctr_open("{");
  }
  CtrNode *lv = stabilize(lhs);
  char *text = ctr_lvalue_text(lv, PREC_LOWEST);
  CtrType *t = lv->ty;
  if (t->kind == TY_PTR && t->base->kind == TY_VOID) {
    ctr_line("%s = (rawptr)((uint8*)%s %s 1);", text, text, inc ? "+" : "-");
  } else if (ctr_is_integer(t) && t->kind != TY_BOOL) {
    ctr_line("%s%s;", text, inc ? "++" : "--");
  } else if (t->kind == TY_PTR) {
    ctr_line("%s%s;", text, inc ? "++" : "--");
  } else if (t->kind == TY_BOOL) {
    ctr_line(inc ? "%s = true;" : "%s = !%s;", text, text);
  } else {
    ctr_line("%s = %s %s 1.0;", text, text, inc ? "+" : "-");
  }
  if (open) {
    ctr_close();
  }
}

static void emit_cond_stmt(CtrNode *node) {
  int64_t known;
  if (node->op != 1 && !ctr_has_side_effects(node->cond) && ctr_try_eval(node->cond, &known)) {
    ctr_emit_expr_stmt(known ? node->then : node->els);
    return;
  }
  if (node->op == 1) {
    ctr_open("if (%s) {", ctr_negated_cond_text(node->cond));
    ctr_emit_expr_stmt(node->els);
    ctr_close();
    return;
  }
  ctr_open("if (%s) {", ctr_cond_text(node->cond));
  ctr_emit_expr_stmt(node->then);
  if (ctr_has_side_effects(node->els)) {
    ctr_close_with("} else {");
    ctr_em->indent++;
    ctr_emit_expr_stmt(node->els);
  }
  ctr_close();
}

static void emit_va_start(CtrNode *node) {
  ctr_em->mod->needs_c = 1;
  const char *va = ctr_em->fn && ctr_em->fn->va_param ? ctr_em->fn->va_param->mettle_name : "va";
  ctr_line("%s = libc.va_start(%s);", ctr_lvalue_text(node->lhs, PREC_LOWEST), va);
}

static void emit_discard(CtrNode *node) {
  if (!ctr_has_side_effects(node)) {
    return;
  }
  ctr_line("var %s = %s;", ctr_new_temp("unused"), ctr_expr_text(node, PREC_LOWEST));
}

static void emit_builtin_stmt(CtrNode *node) {
  if (node->builtin == 1 || node->builtin == 2) {
    ctr_em->mod->needs_c = 1;
    ctr_line("libc.trap();");
    return;
  }
  if (node->builtin == 28) {
    return;
  }
  emit_discard(node);
}

static void emit_stmt_expr_stmt(CtrNode *node) {
  ctr_open("{");
  for (CtrNode *s = node->body; s; s = s->next) {
    ctr_emit_stmt(s);
  }
  ctr_close();
}

static void emit_expr_stmt_more(CtrNode *node) {
  switch (node->kind) {
  case ND_FUNCALL:
    ctr_line("%s;", ctr_expr_text(node, PREC_LOWEST));
    return;
  case ND_STMT_EXPR:
    emit_stmt_expr_stmt(node);
    return;
  case ND_VA_START:
    emit_va_start(node);
    return;
  case ND_VA_END:
    return;
  case ND_VA_COPY:
    ctr_line("%s = %s;", ctr_lvalue_text(node->lhs, PREC_LOWEST), ctr_lvalue_text(node->rhs, PREC_LOWEST));
    return;
  case ND_BUILTIN:
    emit_builtin_stmt(node);
    return;
  default:
    emit_discard(node);
    return;
  }
}

static void emit_expr_stmt_inner(CtrNode *node) {
  node = strip(node);
  switch (node->kind) {
  case ND_ASSIGN:
    emit_assign(node);
    return;
  case ND_OPASSIGN:
    emit_opassign(node);
    return;
  case ND_PREINC:
  case ND_PREDEC:
  case ND_POSTINC:
  case ND_POSTDEC:
    emit_incdec(node);
    return;
  case ND_COMMA:
    ctr_emit_expr_stmt(node->lhs);
    ctr_emit_expr_stmt(node->rhs);
    return;
  case ND_COND:
    emit_cond_stmt(node);
    return;
  case ND_LOGAND:
  case ND_LOGOR:
    if (!ctr_has_side_effects(node->rhs)) {
      emit_discard(node->lhs);
      return;
    }
    ctr_open("if (%s) {", node->kind == ND_LOGAND ? ctr_cond_text(node->lhs) : ctr_negated_cond_text(node->lhs));
    ctr_emit_expr_stmt(node->rhs);
    ctr_close();
    return;
  case ND_CAST:
    ctr_emit_expr_stmt(node->lhs);
    return;
  default:
    emit_expr_stmt_more(node);
    return;
  }
}

void ctr_emit_assign_stmt(CtrNode *lhs, CtrNode *rhs, CtrType *ty) {
  (void)ty;
  CtrNode tmp = {0};
  tmp.kind = ND_ASSIGN;
  tmp.lhs = lhs;
  tmp.rhs = rhs;
  tmp.ty = lhs->ty;
  emit_assign(&tmp);
}

static int ends_with_jump(CtrNode *n) {
  if (!n) {
    return 0;
  }
  switch (n->kind) {
  case ND_RETURN:
  case ND_BREAK:
  case ND_CONTINUE:
  case ND_GOTO:
    return 1;
  case ND_BLOCK: {
    CtrNode *last = n->body;
    while (last && last->next) {
      last = last->next;
    }
    return ends_with_jump(last);
  }
  case ND_IF:
    return n->els && ends_with_jump(n->then) && ends_with_jump(n->els);
  case ND_LABEL:
  case ND_CASE:
    return ends_with_jump(n->lhs);
  case ND_EXPR_STMT: {
    CtrNode *e = strip(n->lhs);
    if (e->kind == ND_FUNCALL) {
      CtrNode *f = strip(e->lhs);
      while (f->kind == ND_CAST) {
        f = strip(f->lhs);
      }
      return f->kind == ND_VAR && f->var->is_noreturn;
    }
    return 0;
  }
  default:
    return 0;
  }
}

static void emit_body_items(CtrNode *list) {
  for (CtrNode *n = list; n; n = n->next) {
    ctr_emit_stmt(n);
  }
}

static void emit_as_block(CtrNode *n) {
  if (n && n->kind == ND_BLOCK) {
    emit_body_items(n->body);
  } else if (n) {
    ctr_emit_stmt(n);
  }
}

static void emit_if(CtrNode *node) {
  ctr_open("if (%s) {", ctr_cond_text(node->cond));
  emit_as_block(node->then);
  CtrNode *els = node->els;
  while (els && els->kind == ND_IF) {
    ctr_close_with("} else if (%s) {", ctr_cond_text(els->cond));
    ctr_em->indent++;
    emit_as_block(els->then);
    els = els->els;
  }
  if (els && !(els->kind == ND_EMPTY)) {
    ctr_close_with("} else {");
    ctr_em->indent++;
    emit_as_block(els);
  }
  ctr_close();
}

typedef void (*BodyFn)(void *);

static char *capture(BodyFn fn, void *arg, int indent) {
  CtrBuf *saved = ctr_em->out;
  int saved_indent = ctr_em->indent;
  CtrBuf buf = {0};
  ctr_em->out = &buf;
  ctr_em->indent = indent;
  fn(arg);
  ctr_em->out = saved;
  ctr_em->indent = saved_indent;
  return ctr_buf_take(&buf);
}

static void put_raw(const char *text) { ctr_buf_put(ctr_em->out, text); }

static void loop_header(Jump *j, const char *header) {
  if (j->label_used) {
    ctr_line("%s: %s", j->label, header);
  } else {
    ctr_line("%s", header);
  }
}

static void emit_body_fn(void *arg) { emit_as_block(arg); }

typedef struct {
  CtrNode *body;
  const char *tail;
  CtrNode *loop;
} InnerArgs;

static void emit_inc_tail(void *arg) {
  InnerArgs *a = arg;
  ctr_emit_expr_stmt(a->loop->inc);
}

static void emit_continue_pseudo(void *arg) {
  InnerArgs *a = arg;
  Jump *inner = push(NULL, NULL, a->loop, 1);
  char *body = capture(emit_body_fn, a->body, ctr_em->indent + 1);
  pop();
  if (!inner->label) {
    inner->label = ctr_unique_label("body");
  }
  loop_header(inner, "while (true) {");
  put_raw(body);
  if (!ends_with_jump(a->body)) {
    ctr_em->indent++;
    ctr_line("break;");
    ctr_em->indent--;
  }
  ctr_line("}");
}

static void emit_while(CtrNode *node) {
  Jump *j = push(node, node, NULL, 1);
  if (node->label) {
    use_label(j);
  }
  char *body = capture(emit_body_fn, node->then, ctr_em->indent + 1);
  pop();
  loop_header(j, ctr_format("while (%s) {", ctr_cond_text(node->cond)));
  put_raw(body);
  ctr_line("}");
}

static int is_const_false(CtrNode *n) {
  int64_t v;
  return n && ctr_try_eval(n, &v) && v == 0;
}

static void emit_do(CtrNode *node) {
  if (is_const_false(node->cond) && !node->has_continue && !node->has_break) {
    ctr_open("{");
    emit_as_block(node->then);
    ctr_close();
    return;
  }
  Jump *j = push(node, NULL, NULL, 1);
  char *body;
  InnerArgs a = {node->then, NULL, node};
  if (node->has_continue) {
    body = capture(emit_continue_pseudo, &a, ctr_em->indent + 1);
  } else {
    body = capture(emit_body_fn, node->then, ctr_em->indent + 1);
  }
  pop();
  loop_header(j, "while (true) {");
  put_raw(body);
  ctr_em->indent++;
  if (is_const_false(node->cond)) {
    ctr_line("break;");
  } else if (!ends_with_jump(node->then) || node->has_continue) {
    ctr_open("if (%s) {", ctr_negated_cond_text(node->cond));
    ctr_line("break;");
    ctr_close();
  }
  ctr_em->indent--;
  ctr_line("}");
}

static int assigns_var(CtrNode *n, CtrObj *v);

static int list_assigns(CtrNode *n, CtrObj *v) {
  for (; n; n = n->next) {
    if (assigns_var(n, v)) {
      return 1;
    }
  }
  return 0;
}

static int is_var(CtrNode *n, CtrObj *v) {
  n = strip(n);
  while (n && n->kind == ND_CAST && n->implicit) {
    n = strip(n->lhs);
  }
  return n && n->kind == ND_VAR && n->var == v;
}

static int assigns_var(CtrNode *n, CtrObj *v) {
  if (!n) {
    return 0;
  }
  switch (n->kind) {
  case ND_ASSIGN:
  case ND_OPASSIGN:
  case ND_PREINC:
  case ND_PREDEC:
  case ND_POSTINC:
  case ND_POSTDEC:
    if (is_var(n->lhs, v)) {
      return 1;
    }
    break;
  case ND_ADDR:
    if (is_var(n->lhs, v)) {
      return 1;
    }
    break;
  default:
    break;
  }
  return assigns_var(n->lhs, v) || assigns_var(n->rhs, v) || assigns_var(n->cond, v) ||
         assigns_var(n->then, v) || assigns_var(n->els, v) || assigns_var(n->init, v) ||
         assigns_var(n->inc, v) || list_assigns(n->body, v) || list_assigns(n->args, v) ||
         (n->decl_init && 0);
}

typedef struct {
  CtrObj *var;
  CtrNode *from;
  CtrNode *to;
  int inclusive;
} RangeFor;

static int bound_is_stable(CtrNode *bound, CtrNode *body, CtrObj *ivar) {
  CtrNode *b = strip(bound);
  while (b->kind == ND_CAST && b->implicit) {
    b = strip(b->lhs);
  }
  int64_t v;
  if (ctr_try_eval(b, &v)) {
    return 1;
  }
  if (b->kind == ND_VAR && b->var->is_local && !b->var->addr_taken && b->var != ivar &&
      !b->var->ty->is_volatile) {
    return !assigns_var(body, b->var);
  }
  return 0;
}

static int range_for(CtrNode *node, RangeFor *out) {
  CtrNode *init = node->init;
  if (!init || init->kind != ND_DECL || init->next || !init->decl_init || !init->decl_init->expr) {
    return 0;
  }
  CtrObj *v = init->var;
  if (!ctr_is_integer(v->ty) || v->ty->kind == TY_BOOL || v->addr_taken || v->ty->is_volatile) {
    return 0;
  }
  CtrNode *cond = strip(node->cond);
  CtrNode *inc = strip(node->inc);
  if (!cond || !inc || (cond->kind != ND_LT && cond->kind != ND_LE)) {
    return 0;
  }
  if (!is_var(cond->lhs, v) || !ctr_same_mettle_type(cond->lhs->ty, v->ty)) {
    return 0;
  }
  if (inc->kind != ND_POSTINC && inc->kind != ND_PREINC) {
    return 0;
  }
  if (!is_var(inc->lhs, v) || assigns_var(node->then, v)) {
    return 0;
  }
  if (!bound_is_stable(cond->rhs, node->then, v)) {
    return 0;
  }
  out->var = v;
  out->from = init->decl_init->expr;
  out->to = cond->rhs;
  out->inclusive = cond->kind == ND_LE;
  return 1;
}

static void emit_range_for(CtrNode *node, RangeFor *r) {
  Jump *j = push(node, node, NULL, 1);
  char *body = capture(emit_body_fn, node->then, ctr_em->indent + 1);
  pop();
  char *header = ctr_format("for %s: %s in %s..%s%s {", ctr_mettle_name_of(r->var), ctr_type_text(r->var->ty),
                            ctr_value_text(r->from, r->var->ty, PREC_ADD + 1), r->inclusive ? "=" : "",
                            ctr_value_text(r->to, r->var->ty, PREC_ADD + 1));
  loop_header(j, header);
  put_raw(body);
  ctr_line("}");
}

static void emit_for_init(CtrNode *init) {
  if (!init) {
    return;
  }
  if (init->kind == ND_EXPR_STMT) {
    ctr_emit_expr_stmt(init->lhs);
  } else {
    ctr_emit_stmt(init);
  }
}

static int step_is_simple(CtrNode *n) {
  n = strip(n);
  switch (n->kind) {
  case ND_PREINC:
  case ND_PREDEC:
  case ND_POSTINC:
  case ND_POSTDEC:
    return simple_lvalue(n->lhs);
  case ND_ASSIGN:
  case ND_OPASSIGN:
    return simple_lvalue(n->lhs) && !ctr_has_side_effects(n->rhs);
  case ND_COMMA:
    return step_is_simple(n->lhs) && step_is_simple(n->rhs);
  case ND_CAST:
    return step_is_simple(n->lhs);
  default:
    return 0;
  }
}

static int continue_sites(CtrNode *n, CtrNode *loop) {
  if (!n) {
    return 0;
  }
  int c = n->kind == ND_CONTINUE && n->target == loop;
  c += continue_sites(n->then, loop) + continue_sites(n->els, loop) + continue_sites(n->lhs, loop);
  for (CtrNode *b = n->body; b; b = b->next) {
    c += continue_sites(b, loop);
  }
  return c;
}

static int repeatable_step(CtrNode *loop) { return step_is_simple(loop->inc) && continue_sites(loop->then, loop) <= 4; }

static void emit_for(CtrNode *node) {
  RangeFor r;
  if (range_for(node, &r)) {
    emit_range_for(node, &r);
    return;
  }
  int scoped = node->init && node->init->kind != ND_EXPR_STMT;
  if (scoped) {
    ctr_open("{");
  }
  emit_for_init(node->init);
  int pseudo = node->has_continue && node->inc && !repeatable_step(node);
  node->inc_at_continue = node->has_continue && node->inc && !pseudo;
  Jump *j = push(node, node->inc && pseudo ? NULL : node, NULL, 1);
  char *body;
  InnerArgs a = {node->then, NULL, node};
  if (pseudo) {
    body = capture(emit_continue_pseudo, &a, ctr_em->indent + 1);
  } else {
    body = capture(emit_body_fn, node->then, ctr_em->indent + 1);
  }
  char *inc = node->inc ? capture(emit_inc_tail, &a, ctr_em->indent + 1) : "";
  pop();
  loop_header(j, ctr_format("while (%s) {", node->cond ? ctr_cond_text(node->cond) : "true"));
  put_raw(body);
  if (!(node->inc && ends_with_jump(node->then) && !pseudo)) {
    put_raw(inc);
  }
  ctr_line("}");
  if (scoped) {
    ctr_close();
  }
}

static void emit_return(CtrNode *node) {
  if (!node->lhs) {
    if (ctr_em->fn && strcmp(ctr_em->fn->name, "main") == 0 && ctr_em->fn->ty->return_ty->kind == TY_VOID) {
      ctr_line("return 0;");
      return;
    }
    ctr_line("return;");
    return;
  }
  if (node->op == 1) {
    ctr_emit_expr_stmt(node->lhs);
    ctr_line("return;");
    return;
  }
  CtrType *rt = ctr_em->fn->ty->return_ty;
  ctr_line("return %s;", ctr_assign_value_text(node->lhs, rt));
}

void ctr_hoist_decl_names(CtrNode *node, CtrVec *out) {
  for (CtrNode *n = node; n; n = n->next) {
    if (n->kind == ND_DECL) {
      ctr_vec_push(out, n);
    } else if (n->kind == ND_BLOCK && n->op == 1) {
      ctr_hoist_decl_names(n->body, out);
    } else if (n->kind == ND_CASE || n->kind == ND_LABEL) {
      CtrNode *inner = n->lhs;
      CtrNode *save = inner ? inner->next : NULL;
      if (inner) {
        inner->next = NULL;
        ctr_hoist_decl_names(inner, out);
        inner->next = save;
      }
    }
  }
}

static void emit_case_labels(CtrNode *c, CtrNode **body_out) {
  while (c && c->kind == ND_CASE) {
    if (c->lhs && c->lhs->kind == ND_CASE) {
      if (c->is_default) {
        ctr_line("default:");
      } else if (c->case_begin == c->case_end) {
        ctr_line("case %lld:", (long long)c->case_begin);
      } else {
        ctr_line("case %lld..%lld:", (long long)c->case_begin, (long long)c->case_end);
      }
      c = c->lhs;
      continue;
    }
    if (c->is_default) {
      ctr_open("default: {");
    } else if (c->case_begin == c->case_end) {
      ctr_open("case %lld: {", (long long)c->case_begin);
    } else {
      ctr_open("case %lld..%lld: {", (long long)c->case_begin, (long long)c->case_end);
    }
    *body_out = c->lhs;
    return;
  }
}

static int is_break_of(CtrNode *n, CtrNode *sw) { return n && n->kind == ND_BREAK && n->target == sw; }

typedef struct {
  CtrNode *sw;
} SwitchArgs;

static void emit_decl_as_assign(CtrNode *n);

static void emit_group_stmt(CtrNode *n, CtrNode *sw, int last_in_group) {
  if (last_in_group && is_break_of(n, sw)) {
    return;
  }
  if (n->kind == ND_DECL) {
    emit_decl_as_assign(n);
    return;
  }
  if (n->kind == ND_BLOCK && n->op == 1) {
    for (CtrNode *d = n->body; d; d = d->next) {
      if (d->kind == ND_DECL) {
        emit_decl_as_assign(d);
      } else {
        ctr_emit_stmt(d);
      }
    }
    return;
  }
  ctr_emit_stmt(n);
}

static void emit_switch_body(void *arg) {
  SwitchArgs *a = arg;
  CtrNode *sw = a->sw;
  CtrNode *items = sw->then->body;
  CtrNode *n = items;
  while (n && n->kind != ND_CASE) {
    n = n->next;
  }
  while (n) {
    CtrNode *first = NULL;
    emit_case_labels(n, &first);
    CtrNode *last = first;
    CtrNode *next = n->next;
    while (next && next->kind != ND_CASE) {
      last = next;
      next = next->next;
    }
    if (first) {
      emit_group_stmt(first, sw, first == last);
      for (CtrNode *s = n->next; s && s != next; s = s->next) {
        emit_group_stmt(s, sw, s == last);
      }
    }
    if (next && !is_break_of(last, sw) && !ends_with_jump(last)) {
      ctr_line("fallthrough;");
    }
    ctr_close();
    n = next;
  }
}

static void emit_switch(CtrNode *node) {
  CtrVec decls = {0};
  ctr_hoist_decl_names(node->then->body, &decls);
  for (int i = 0; i < decls.len; i++) {
    CtrNode *d = decls.items[i];
    ctr_line("var %s: %s%s;", ctr_mettle_name_of(d->var), ctr_type_text(d->var->ty), ctr_zero_suffix(d->var));
  }
  Jump *wrapper = push(node, NULL, NULL, 1);
  Jump *sw = push(node, NULL, NULL, 0);
  sw->labelable = 0;
  SwitchArgs a = {node};
  char *body = capture(emit_switch_body, &a, ctr_em->indent + (1));
  pop();
  pop();
  char *head = ctr_format("switch (%s) {", ctr_expr_text(node->cond, PREC_LOWEST));
  if (wrapper->label_used) {
    ctr_line("%s: while (true) {", wrapper->label);
    ctr_em->indent++;
    ctr_line("%s", head);
    CtrBuf b = {0};
    for (char *p = body; *p; p++) {
      if (p == body || p[-1] == '\n') {
        ctr_buf_put(&b, "  ");
      }
      ctr_buf_putc(&b, *p);
    }
    put_raw(ctr_buf_take(&b));
    ctr_line("}");
    ctr_line("break;");
    ctr_close();
    return;
  }
  ctr_line("%s", head);
  put_raw(body);
  ctr_line("}");
}

static void emit_block_stmt(CtrNode *node) {
  if (node->op == 1) {
    emit_body_items(node->body);
    return;
  }
  if (!node->body) {
    return;
  }
  ctr_open("{");
  emit_body_items(node->body);
  ctr_close();
}

static void emit_stmt_more(CtrNode *node) {
  switch (node->kind) {
  case ND_LABEL:
    ctr_emit_stmt(node->lhs);
    return;
  case ND_CASE:
    ctr_emit_stmt(node->lhs);
    return;
  case ND_GOTO:
  case ND_GOTO_EXPR:
    ctr_error_tok(node->tok, "goto reached the structured writer");
    return;
  case ND_ASM:
  case ND_EMPTY:
    return;
  default:
    ctr_emit_expr_stmt(node);
    return;
  }
}

static void declare_literals(CtrNode *n);

static void declare_literal_list(CtrNode *n) {
  for (; n; n = n->next) {
    declare_literals(n);
  }
}

static void declare_literals(CtrNode *n) {
  if (!n || n->kind == ND_STMT_EXPR) {
    return;
  }
  if (n->kind == ND_COMPOUND_LIT && n->var && n->var->is_local && !n->mettle_label) {
    n->mettle_label = "declared";
    CtrNode decl = {0};
    decl.kind = ND_DECL;
    decl.var = n->var;
    decl.decl_init = n->decl_init;
    decl.tok = n->tok;
    ctr_emit_local_decl(&decl, ctr_em->cfg_mode);
  }
  declare_literals(n->lhs);
  declare_literals(n->rhs);
  declare_literals(n->cond);
  if (n->kind != ND_IF && n->kind != ND_WHILE && n->kind != ND_FOR && n->kind != ND_DO &&
      n->kind != ND_SWITCH && n->kind != ND_BLOCK) {
    declare_literals(n->then);
    declare_literals(n->els);
  }
  declare_literal_list(n->args);
}

void ctr_declare_compound_literals(CtrNode *n) { declare_literals(n); }

void ctr_emit_stmt(CtrNode *node) {
  if (node->kind == ND_EXPR_STMT || node->kind == ND_RETURN || node->kind == ND_IF ||
      node->kind == ND_SWITCH || node->kind == ND_WHILE) {
    declare_literals(node->kind == ND_EXPR_STMT || node->kind == ND_RETURN ? node->lhs : node->cond);
  }
  switch (node->kind) {
  case ND_BLOCK:
    emit_block_stmt(node);
    return;
  case ND_DECL:
    ctr_emit_local_decl(node, node->as_assign);
    return;
  case ND_EXPR_STMT:
    ctr_emit_expr_stmt(node->lhs);
    return;
  case ND_IF:
    emit_if(node);
    return;
  case ND_WHILE:
    emit_while(node);
    return;
  case ND_DO:
    emit_do(node);
    return;
  case ND_FOR:
    emit_for(node);
    return;
  case ND_SWITCH:
    emit_switch(node);
    return;
  case ND_RETURN:
    emit_return(node);
    return;
  case ND_BREAK:
    ctr_line("%s", ctr_break_text(node->target));
    return;
  case ND_CONTINUE:
    if (node->target && node->target->inc_at_continue) {
      ctr_emit_expr_stmt(node->target->inc);
    }
    ctr_line("%s", ctr_continue_text(node->target));
    return;
  default:
    emit_stmt_more(node);
    return;
  }
}

static void emit_decl_as_assign(CtrNode *n) { ctr_emit_local_decl(n, 1); }

void ctr_emit_block_body(CtrNode *block) { emit_as_block(block); }

int ctr_body_falls_through(CtrNode *body) { return !ends_with_jump(body); }

typedef struct {
  CtrNode *items[32];
  int count;
} IncList;

static void count_uses(CtrNode *n, CtrObj *v, int *uses);

static void count_list(CtrNode *n, CtrObj *v, int *uses) {
  for (; n; n = n->next) {
    count_uses(n, v, uses);
  }
}

static void count_uses(CtrNode *n, CtrObj *v, int *uses) {
  if (!n) {
    return;
  }
  if (n->kind == ND_VAR && n->var == v) {
    (*uses)++;
  }
  count_uses(n->lhs, v, uses);
  count_uses(n->rhs, v, uses);
  count_uses(n->cond, v, uses);
  count_uses(n->then, v, uses);
  count_uses(n->els, v, uses);
  count_list(n->body, v, uses);
  count_list(n->args, v, uses);
}

static int is_incdec(CtrNode *n) {
  return n->kind == ND_PREINC || n->kind == ND_PREDEC || n->kind == ND_POSTINC || n->kind == ND_POSTDEC;
}

static CtrObj *plain_local(CtrNode *n) {
  n = strip(n);
  if (n->kind != ND_VAR) {
    return NULL;
  }
  CtrObj *v = n->var;
  int scalar = ctr_is_integer(v->ty) || v->ty->kind == TY_PTR;
  return v->is_local && !v->addr_taken && !v->ty->is_volatile && scalar && v->ty->kind != TY_BOOL ? v : NULL;
}

static void collect_incdec(CtrNode *n, IncList *list);

static void collect_list(CtrNode *n, IncList *list) {
  for (; n; n = n->next) {
    collect_incdec(n, list);
  }
}

static void collect_incdec(CtrNode *n, IncList *list) {
  if (!n) {
    return;
  }
  if (is_incdec(n)) {
    if (plain_local(n->lhs) && !n->print_plain && list->count < 32) {
      list->items[list->count++] = n;
    }
    return;
  }
  switch (n->kind) {
  case ND_LOGAND:
  case ND_LOGOR:
  case ND_COND:
  case ND_COMMA:
    collect_incdec(n->kind == ND_COND ? n->cond : n->lhs, list);
    return;
  case ND_STMT_EXPR:
  case ND_ASSIGN:
  case ND_OPASSIGN:
    if (n->kind != ND_STMT_EXPR) {
      collect_incdec(n->lhs, list);
      collect_incdec(n->rhs, list);
    }
    return;
  default:
    collect_incdec(n->lhs, list);
    collect_incdec(n->rhs, list);
    collect_list(n->args, list);
    return;
  }
}

static int plan_hoist(CtrNode *root, IncList *list) {
  list->count = 0;
  if (!root || is_incdec(strip(root))) {
    return 0;
  }
  collect_incdec(root, list);
  int kept = 0;
  for (int i = 0; i < list->count; i++) {
    int uses = 0;
    count_uses(root, plain_local(list->items[i]->lhs), &uses);
    if (uses == 1) {
      list->items[kept++] = list->items[i];
    }
  }
  list->count = kept;
  return kept > 0;
}

static void emit_hoisted(IncList *list, int post) {
  for (int i = 0; i < list->count; i++) {
    CtrNode *n = list->items[i];
    int is_post = n->kind == ND_POSTINC || n->kind == ND_POSTDEC;
    if (is_post != post) {
      continue;
    }
    n->print_plain = 0;
    emit_incdec(n);
    n->print_plain = 1;
  }
}

static void set_plain(IncList *list, int on) {
  for (int i = 0; i < list->count; i++) {
    list->items[i]->print_plain = on;
  }
}

void ctr_emit_expr_stmt(CtrNode *node) {
  CtrNode *s = strip(node);
  if (s->kind == ND_COMMA || s->kind == ND_CAST) {
    emit_expr_stmt_inner(s);
    return;
  }
  IncList list;
  if (!plan_hoist(node, &list)) {
    emit_expr_stmt_inner(node);
    return;
  }
  set_plain(&list, 1);
  emit_hoisted(&list, 0);
  emit_expr_stmt_inner(node);
  emit_hoisted(&list, 1);
  set_plain(&list, 0);
}

int ctr_hoist_begin(CtrNode *expr, void **state) {
  IncList *list = ctr_alloc(sizeof(IncList));
  *state = list;
  if (!plan_hoist(expr, list)) {
    return 0;
  }
  set_plain(list, 1);
  emit_hoisted(list, 0);
  return 1;
}

void ctr_hoist_end(void *state) {
  IncList *list = state;
  emit_hoisted(list, 1);
  set_plain(list, 0);
}
