#include "translate/ctr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct CondIncl {
  struct CondIncl *next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
  CtrToken *tok;
  int included;
} CondIncl;

typedef struct MacroArg {
  struct MacroArg *next;
  const char *name;
  int is_va_args;
  CtrToken *tok;
} MacroArg;

typedef CtrToken *(*BuiltinFn)(CtrToken *tmpl);

static CtrMap macros;
static CondIncl *cond_incl;
static CtrMap pragma_once;
static CtrMap include_guards;
static CtrMap include_cache;
static int include_next_idx;
static int in_directive_expr;
static int counter_value;
static int mark_ids;
static int pack_stack[64];
static int pack_depth;
static int pack_current;

static CtrToken *preprocess2(CtrToken *tok);

static int is_hash(CtrToken *tok) { return tok->at_bol && ctr_equal(tok, "#"); }

static CtrToken *skip_line(CtrToken *tok) {
  if (tok->at_bol) {
    return tok;
  }
  ctr_warn_tok(tok, "extra token");
  while (!tok->at_bol) {
    tok = tok->next;
  }
  return tok;
}

static CtrToken *new_eof(CtrToken *tok) { return ctr_new_eof(tok); }

static CtrHideset *new_hideset(const char *name) {
  CtrHideset *hs = ctr_alloc(sizeof(CtrHideset));
  hs->name = name;
  return hs;
}

static CtrHideset *hideset_union(CtrHideset *hs1, CtrHideset *hs2) {
  CtrHideset head = {0};
  CtrHideset *cur = &head;
  for (; hs1; hs1 = hs1->next) {
    cur = cur->next = new_hideset(hs1->name);
  }
  cur->next = hs2;
  return head.next;
}

static int hideset_contains(CtrHideset *hs, const char *s, int len) {
  for (; hs; hs = hs->next) {
    if ((int)strlen(hs->name) == len && strncmp(hs->name, s, (size_t)len) == 0) {
      return 1;
    }
  }
  return 0;
}

static CtrHideset *hideset_intersection(CtrHideset *hs1, CtrHideset *hs2) {
  CtrHideset head = {0};
  CtrHideset *cur = &head;
  for (; hs1; hs1 = hs1->next) {
    if (hideset_contains(hs2, hs1->name, (int)strlen(hs1->name))) {
      cur = cur->next = new_hideset(hs1->name);
    }
  }
  return head.next;
}

static CtrToken *add_hideset(CtrToken *tok, CtrHideset *hs) {
  CtrToken head = {0};
  CtrToken *cur = &head;
  for (; tok; tok = tok->next) {
    CtrToken *t = ctr_copy_token(tok);
    t->hideset = hideset_union(t->hideset, hs);
    cur = cur->next = t;
  }
  return head.next;
}

static CtrToken *append(CtrToken *tok1, CtrToken *tok2) {
  if (tok1->kind == TK_EOF) {
    return tok2;
  }
  CtrToken head = {0};
  CtrToken *cur = &head;
  for (; tok1->kind != TK_EOF; tok1 = tok1->next) {
    cur = cur->next = ctr_copy_token(tok1);
  }
  cur->next = tok2;
  return head.next;
}

static CtrToken *skip_cond_incl2(CtrToken *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && (ctr_equal(tok->next, "if") || ctr_equal(tok->next, "ifdef") ||
                         ctr_equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }
    if (is_hash(tok) && ctr_equal(tok->next, "endif")) {
      return tok->next->next;
    }
    tok = tok->next;
  }
  return tok;
}

static CtrToken *skip_cond_incl(CtrToken *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && (ctr_equal(tok->next, "if") || ctr_equal(tok->next, "ifdef") ||
                         ctr_equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }
    if (is_hash(tok) && (ctr_equal(tok->next, "elif") || ctr_equal(tok->next, "else") ||
                         ctr_equal(tok->next, "endif"))) {
      break;
    }
    tok = tok->next;
  }
  return tok;
}

static char *quote_string(const char *str) {
  CtrBuf buf = {0};
  ctr_buf_putc(&buf, '"');
  for (const char *p = str; *p; p++) {
    if (*p == '\\' || *p == '"') {
      ctr_buf_putc(&buf, '\\');
    }
    ctr_buf_putc(&buf, *p);
  }
  ctr_buf_putc(&buf, '"');
  return ctr_buf_take(&buf);
}

static CtrToken *tokenize_text(const char *text, CtrToken *tmpl) {
  CtrFile *file = ctr_new_file(tmpl && tmpl->file ? tmpl->file->name : "<built-in>",
                               tmpl && tmpl->file ? tmpl->file->index : 0, ctr_strdup(text));
  if (tmpl && tmpl->file) {
    file->display = tmpl->file->display;
    file->is_system = tmpl->file->is_system;
  }
  return ctr_tokenize(file);
}

static CtrToken *new_str_token(const char *str, CtrToken *tmpl) {
  CtrToken *t = tokenize_text(quote_string(str), tmpl);
  t->line = tmpl->line;
  t->col = tmpl->col;
  return t;
}

static CtrToken *copy_line(CtrToken **rest, CtrToken *tok) {
  CtrToken head = {0};
  CtrToken *cur = &head;
  for (; !tok->at_bol; tok = tok->next) {
    cur = cur->next = ctr_copy_token(tok);
  }
  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

static CtrToken *new_num_token(int64_t val, CtrToken *tmpl) {
  CtrToken *t = tokenize_text(ctr_format("%lld\n", (long long)val), tmpl);
  t->line = tmpl->line;
  t->col = tmpl->col;
  return t;
}

static int has_include_test(CtrToken **rest, CtrToken *tok, int next);

static CtrToken *read_const_expr(CtrToken **rest, CtrToken *tok) {
  tok = copy_line(rest, tok);
  CtrToken head = {0};
  CtrToken *cur = &head;
  while (tok->kind != TK_EOF) {
    if (ctr_equal(tok, "defined")) {
      CtrToken *start = tok;
      int has_paren = ctr_consume(&tok, tok->next, "(");
      if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) {
        ctr_fatal_tok(start, "macro name must be an identifier");
      }
      CtrMacro *m = ctr_map_get2(&macros, tok->loc, tok->len);
      tok = tok->next;
      if (has_paren) {
        tok = ctr_skip(tok, ")");
      }
      cur = cur->next = new_num_token(m ? 1 : 0, start);
      continue;
    }
    if (ctr_equal(tok, "__has_include") || ctr_equal(tok, "__has_include_next")) {
      CtrToken *start = tok;
      int found = has_include_test(&tok, tok, ctr_equal(tok, "__has_include_next"));
      cur = cur->next = new_num_token(found, start);
      continue;
    }
    cur = cur->next = tok;
    tok = tok->next;
  }
  cur->next = tok;
  return head.next;
}

static const char *known_builtins[] = {
    "__builtin_expect", "__builtin_unreachable", "__builtin_trap", "__builtin_offsetof",
    "__builtin_va_start", "__builtin_va_arg", "__builtin_va_end", "__builtin_va_copy",
    "__builtin_popcount", "__builtin_popcountll", "__builtin_popcountl", "__builtin_clz",
    "__builtin_clzll", "__builtin_clzl", "__builtin_ctz", "__builtin_ctzll", "__builtin_ctzl",
    "__builtin_bswap16", "__builtin_bswap32", "__builtin_bswap64", "__builtin_abs",
    "__builtin_types_compatible_p", "__builtin_constant_p", "__builtin_memcpy",
    "__builtin_memset", "__builtin_strlen", "__builtin_alloca", "__builtin_huge_val",
    "__builtin_inf", "__builtin_inff", "__builtin_nan", "__builtin_nanf", "__builtin_huge_valf",
    "__builtin_add_overflow", "__builtin_sub_overflow", "__builtin_mul_overflow"};

static int is_known_builtin(CtrToken *tok) {
  for (size_t i = 0; i < sizeof(known_builtins) / sizeof(*known_builtins); i++) {
    if (ctr_equal(tok, known_builtins[i])) {
      return 1;
    }
  }
  return 0;
}

static CtrToken *replace_feature_tests(CtrToken *tok) {
  CtrToken head = {0};
  CtrToken *cur = &head;
  while (tok->kind != TK_EOF) {
    int is_test = ctr_equal(tok, "__has_attribute") || ctr_equal(tok, "__has_builtin") ||
                  ctr_equal(tok, "__has_feature") || ctr_equal(tok, "__has_extension") ||
                  ctr_equal(tok, "__has_cpp_attribute") ||
                  ctr_equal(tok, "__has_c_attribute") || ctr_equal(tok, "__has_declspec_attribute");
    if (is_test && ctr_equal(tok->next, "(")) {
      CtrToken *start = tok;
      tok = tok->next->next;
      int value = ctr_equal(start, "__has_builtin") && is_known_builtin(tok);
      int depth = 1;
      while (depth > 0 && tok->kind != TK_EOF) {
        depth += ctr_equal(tok, "(") ? 1 : ctr_equal(tok, ")") ? -1 : 0;
        tok = tok->next;
      }
      cur = cur->next = new_num_token(value, start);
      continue;
    }
    cur = cur->next = tok;
    tok = tok->next;
  }
  cur->next = tok;
  return head.next;
}

static int64_t pp_eval(CtrToken **rest, CtrToken *tok);

static int64_t eval_const_expr(CtrToken **rest, CtrToken *tok) {
  CtrToken *start = tok;
  CtrToken *expr = read_const_expr(rest, tok->next);
  expr = replace_feature_tests(expr);
  in_directive_expr++;
  expr = preprocess2(expr);
  in_directive_expr--;
  if (expr->kind == TK_EOF) {
    ctr_fatal_tok(start, "no expression");
  }
  CtrToken head = {0};
  CtrToken *cur = &head;
  for (CtrToken *t = expr; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_MARK_BEGIN || t->kind == TK_MARK_END) {
      continue;
    }
    CtrToken *c = ctr_copy_token(t);
    if (c->kind == TK_IDENT || c->kind == TK_KEYWORD) {
      c = new_num_token(0, t);
    }
    cur = cur->next = c;
  }
  cur->next = new_eof(start);
  expr = head.next;
  ctr_convert_pp_tokens(expr);
  CtrToken *rest2;
  int64_t val = pp_eval(&rest2, expr);
  if (rest2->kind != TK_EOF) {
    ctr_fatal_tok(rest2, "extra token in preprocessor expression");
  }
  return val;
}

typedef struct {
  int64_t v;
  int is_unsigned;
} PPVal;

static PPVal pp_cond(CtrToken **rest, CtrToken *tok);

static PPVal pp_primary(CtrToken **rest, CtrToken *tok) {
  PPVal r = {0, 0};
  if (ctr_equal(tok, "(")) {
    r = pp_cond(&tok, tok->next);
    *rest = ctr_skip(tok, ")");
    return r;
  }
  if (tok->kind != TK_NUM) {
    ctr_fatal_tok(tok, "invalid token in preprocessor expression");
  }
  r.v = tok->ival;
  r.is_unsigned = tok->ty && tok->ty->is_unsigned && tok->ty->size >= 4;
  if (tok->ty && tok->ty->kind >= TY_FLOAT && tok->ty->kind <= TY_LDOUBLE) {
    ctr_fatal_tok(tok, "floating constant in preprocessor expression");
  }
  *rest = tok->next;
  return r;
}

static PPVal pp_unary(CtrToken **rest, CtrToken *tok) {
  PPVal r;
  if (ctr_equal(tok, "+")) {
    return pp_unary(rest, tok->next);
  }
  if (ctr_equal(tok, "-")) {
    r = pp_unary(rest, tok->next);
    r.v = (int64_t)(0 - (uint64_t)r.v);
    return r;
  }
  if (ctr_equal(tok, "!")) {
    r = pp_unary(rest, tok->next);
    r.v = !r.v;
    r.is_unsigned = 0;
    return r;
  }
  if (ctr_equal(tok, "~")) {
    r = pp_unary(rest, tok->next);
    r.v = ~r.v;
    return r;
  }
  return pp_primary(rest, tok);
}

typedef struct {
  const char *op;
  int prec;
} PPOp;

static const PPOp pp_ops[] = {{"*", 10}, {"/", 10}, {"%", 10}, {"+", 9},   {"-", 9},
                              {"<<", 8}, {">>", 8}, {"<", 7},  {"<=", 7},  {">", 7},
                              {">=", 7}, {"==", 6}, {"!=", 6}, {"&", 5},   {"^", 4},
                              {"|", 3},  {"&&", 2}, {"||", 1}};

static int pp_op_prec(CtrToken *tok, const char **op) {
  for (size_t i = 0; i < sizeof(pp_ops) / sizeof(*pp_ops); i++) {
    if (ctr_equal(tok, pp_ops[i].op)) {
      *op = pp_ops[i].op;
      return pp_ops[i].prec;
    }
  }
  return 0;
}

static int pp_skip;

static int64_t pp_div(CtrToken *tok, PPVal a, PPVal b, int is_mod, int u) {
  if (b.v == 0 && pp_skip) {
    return 0;
  }
  if (b.v == 0) {
    ctr_fatal_tok(tok, "division by zero in preprocessor expression");
  }
  if (u) {
    return is_mod ? (int64_t)((uint64_t)a.v % (uint64_t)b.v) : (int64_t)((uint64_t)a.v / (uint64_t)b.v);
  }
  if (b.v == -1) {
    return is_mod ? 0 : (int64_t)(0 - (uint64_t)a.v);
  }
  return is_mod ? a.v % b.v : a.v / b.v;
}

static int64_t pp_compare(const char *op, PPVal a, PPVal b, int u) {
  int lt = u ? (uint64_t)a.v < (uint64_t)b.v : a.v < b.v;
  int gt = u ? (uint64_t)a.v > (uint64_t)b.v : a.v > b.v;
  if (strcmp(op, "<") == 0) {
    return lt;
  }
  if (strcmp(op, "<=") == 0) {
    return !gt;
  }
  if (strcmp(op, ">") == 0) {
    return gt;
  }
  return !lt;
}

static PPVal pp_apply(CtrToken *tok, const char *op, PPVal a, PPVal b) {
  PPVal r;
  int u = a.is_unsigned || b.is_unsigned;
  r.is_unsigned = u;
  uint64_t ua = (uint64_t)a.v, ub = (uint64_t)b.v;
  if (op[0] == '*') {
    r.v = (int64_t)(ua * ub);
  } else if (op[0] == '/' || op[0] == '%') {
    r.v = pp_div(tok, a, b, op[0] == '%', u);
  } else if (op[0] == '+') {
    r.v = (int64_t)(ua + ub);
  } else if (op[0] == '-') {
    r.v = (int64_t)(ua - ub);
  } else if (strcmp(op, "<<") == 0) {
    r.v = (int64_t)(ua << (ub & 63));
    r.is_unsigned = a.is_unsigned;
  } else if (strcmp(op, ">>") == 0) {
    r.v = a.is_unsigned ? (int64_t)(ua >> (ub & 63)) : a.v >> (ub & 63);
    r.is_unsigned = a.is_unsigned;
  } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
    r.v = (a.v == b.v) == (op[0] == '=');
    r.is_unsigned = 0;
  } else if (op[0] == '<' || op[0] == '>') {
    r.v = pp_compare(op, a, b, u);
    r.is_unsigned = 0;
  } else if (strcmp(op, "&&") == 0) {
    r.v = a.v && b.v;
    r.is_unsigned = 0;
  } else if (strcmp(op, "||") == 0) {
    r.v = a.v || b.v;
    r.is_unsigned = 0;
  } else if (op[0] == '&') {
    r.v = a.v & b.v;
  } else if (op[0] == '^') {
    r.v = a.v ^ b.v;
  } else {
    r.v = a.v | b.v;
  }
  return r;
}

static PPVal pp_binary(CtrToken **rest, CtrToken *tok, int min_prec) {
  PPVal lhs = pp_unary(&tok, tok);
  for (;;) {
    const char *op;
    int prec = pp_op_prec(tok, &op);
    if (prec == 0 || prec < min_prec) {
      break;
    }
    CtrToken *optok = tok;
    int skip = (strcmp(op, "&&") == 0 && !lhs.v) || (strcmp(op, "||") == 0 && lhs.v);
    pp_skip += skip;
    PPVal rhs = pp_binary(&tok, tok->next, prec + 1);
    pp_skip -= skip;
    lhs = pp_apply(optok, op, lhs, rhs);
  }
  *rest = tok;
  return lhs;
}

static PPVal pp_cond(CtrToken **rest, CtrToken *tok) {
  PPVal c = pp_binary(&tok, tok, 1);
  if (!ctr_equal(tok, "?")) {
    *rest = tok;
    return c;
  }
  pp_skip += !c.v;
  PPVal a = pp_cond(&tok, tok->next);
  pp_skip -= !c.v;
  tok = ctr_skip(tok, ":");
  pp_skip += c.v != 0;
  PPVal b = pp_cond(&tok, tok);
  pp_skip -= c.v != 0;
  *rest = tok;
  PPVal r = c.v ? a : b;
  r.is_unsigned = a.is_unsigned || b.is_unsigned;
  return r;
}

static int64_t pp_eval(CtrToken **rest, CtrToken *tok) { return pp_cond(rest, tok).v; }

static CondIncl *push_cond_incl(CtrToken *tok, int included) {
  CondIncl *ci = ctr_alloc(sizeof(CondIncl));
  ci->next = cond_incl;
  ci->ctx = IN_THEN;
  ci->tok = tok;
  ci->included = included;
  cond_incl = ci;
  return ci;
}

static CtrMacro *find_macro(CtrToken *tok) {
  if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) {
    return NULL;
  }
  return ctr_map_get2(&macros, tok->loc, tok->len);
}

static CtrMacro *add_macro(const char *name, int is_objlike, CtrToken *body) {
  CtrMacro *m = ctr_alloc(sizeof(CtrMacro));
  m->name = name;
  m->is_objlike = is_objlike;
  m->body = body;
  ctr_map_put(&macros, name, m);
  if (ctr_prog) {
    ctr_vec_push(&ctr_prog->macros, m);
  }
  return m;
}

static MacroArg *read_macro_params(CtrToken **rest, CtrToken *tok, const char **va_args_name) {
  MacroArg head = {0};
  MacroArg *cur = &head;
  while (!ctr_equal(tok, ")")) {
    if (cur != &head) {
      tok = ctr_skip(tok, ",");
    }
    if (ctr_equal(tok, "...")) {
      *va_args_name = "__VA_ARGS__";
      *rest = ctr_skip(tok->next, ")");
      return head.next;
    }
    if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) {
      ctr_fatal_tok(tok, "expected an identifier");
    }
    if (ctr_equal(tok->next, "...")) {
      *va_args_name = ctr_token_text(tok);
      *rest = ctr_skip(tok->next->next, ")");
      return head.next;
    }
    MacroArg *arg = ctr_alloc(sizeof(MacroArg));
    arg->name = ctr_token_text(tok);
    cur = cur->next = arg;
    tok = tok->next;
  }
  *rest = tok->next;
  return head.next;
}

static void read_macro_definition(CtrToken **rest, CtrToken *tok) {
  if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) {
    ctr_fatal_tok(tok, "macro name must be an identifier");
  }
  char *name = ctr_token_text(tok);
  CtrToken *def_tok = tok;
  tok = tok->next;
  CtrMacro *m;
  if (!tok->has_space && ctr_equal(tok, "(")) {
    const char *va_args_name = NULL;
    MacroArg *params = read_macro_params(&tok, tok->next, &va_args_name);
    m = add_macro(name, 0, copy_line(rest, tok));
    CtrMacroArg head = {0};
    CtrMacroArg *cur = &head;
    for (MacroArg *a = params; a; a = a->next) {
      CtrMacroArg *p = ctr_alloc(sizeof(CtrMacroArg));
      p->name = a->name;
      cur = cur->next = p;
      m->param_count++;
    }
    m->params = head.next;
    m->va_args_name = va_args_name;
  } else {
    m = add_macro(name, 1, copy_line(rest, tok));
  }
  m->def_tok = def_tok;
  m->file = def_tok->file;
}

static MacroArg *read_macro_arg_one(CtrToken **rest, CtrToken *tok, int read_rest) {
  CtrToken head = {0};
  CtrToken *cur = &head;
  int level = 0;
  for (;;) {
    if (level == 0 && ctr_equal(tok, ")")) {
      break;
    }
    if (level == 0 && !read_rest && ctr_equal(tok, ",")) {
      break;
    }
    if (tok->kind == TK_EOF) {
      ctr_fatal_tok(tok, "premature end of input in macro arguments");
    }
    if (ctr_equal(tok, "(")) {
      level++;
    } else if (ctr_equal(tok, ")")) {
      level--;
    }
    cur = cur->next = ctr_copy_token(tok);
    tok = tok->next;
  }
  cur->next = new_eof(tok);
  MacroArg *arg = ctr_alloc(sizeof(MacroArg));
  arg->tok = head.next;
  *rest = tok;
  return arg;
}

static MacroArg *read_macro_args(CtrToken **rest, CtrToken *tok, CtrMacro *m) {
  CtrToken *start = tok;
  tok = tok->next->next;
  MacroArg head = {0};
  MacroArg *cur = &head;
  CtrMacroArg *pp = m->params;
  for (; pp; pp = pp->next) {
    if (cur != &head) {
      tok = ctr_skip(tok, ",");
    }
    cur = cur->next = read_macro_arg_one(&tok, tok, 0);
    cur->name = pp->name;
  }
  if (m->va_args_name) {
    MacroArg *arg;
    if (ctr_equal(tok, ")")) {
      arg = ctr_alloc(sizeof(MacroArg));
      arg->tok = new_eof(tok);
    } else {
      if (m->params) {
        tok = ctr_skip(tok, ",");
      }
      arg = read_macro_arg_one(&tok, tok, 1);
    }
    arg->name = m->va_args_name;
    arg->is_va_args = 1;
    cur = cur->next = arg;
  } else if (m->params == NULL && !ctr_equal(tok, ")")) {
    MacroArg *ignored = read_macro_arg_one(&tok, tok, 1);
    if (ignored->tok->kind != TK_EOF) {
      ctr_fatal_tok(start, "too many arguments to macro '%s'", m->name);
    }
  }
  if (!ctr_equal(tok, ")")) {
    ctr_fatal_tok(start, "too many arguments to macro '%s'", m->name);
  }
  *rest = tok;
  return head.next;
}

static MacroArg *find_arg(MacroArg *args, CtrToken *tok) {
  for (MacroArg *ap = args; ap; ap = ap->next) {
    if (tok->len == (int)strlen(ap->name) && strncmp(tok->loc, ap->name, (size_t)tok->len) == 0) {
      return ap;
    }
  }
  return NULL;
}

static CtrToken *strip_marks(CtrToken *tok) {
  CtrToken head = {0};
  CtrToken *cur = &head;
  for (; tok; tok = tok->next) {
    if (tok->kind == TK_MARK_BEGIN || tok->kind == TK_MARK_END) {
      continue;
    }
    cur = cur->next = ctr_copy_token(tok);
    if (tok->kind == TK_EOF) {
      break;
    }
  }
  return head.next;
}

static char *join_tokens(CtrToken *tok, CtrToken *end) {
  CtrBuf buf = {0};
  for (CtrToken *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_MARK_BEGIN || t->kind == TK_MARK_END) {
      continue;
    }
    if (t != tok && t->has_space) {
      ctr_buf_putc(&buf, ' ');
    }
    ctr_buf_putn(&buf, t->loc, (size_t)t->len);
  }
  return ctr_buf_take(&buf);
}

static CtrToken *stringize(CtrToken *hash, CtrToken *arg) {
  char *s = join_tokens(arg, NULL);
  return new_str_token(s, hash);
}

static CtrToken *paste(CtrToken *lhs, CtrToken *rhs) {
  char *buf = ctr_format("%.*s%.*s\n", lhs->len, lhs->loc, rhs->len, rhs->loc);
  CtrToken *tok = tokenize_text(buf, lhs);
  if (tok->next->kind != TK_EOF) {
    ctr_fatal_tok(lhs, "pasting forms '%s', an invalid token", buf);
  }
  tok->line = lhs->line;
  tok->col = lhs->col;
  tok->origin = lhs->origin;
  tok->hideset = lhs->hideset;
  return tok;
}

static int has_varargs(MacroArg *args) {
  for (MacroArg *ap = args; ap; ap = ap->next) {
    if (ap->is_va_args) {
      return ap->tok->kind != TK_EOF;
    }
  }
  return 0;
}

static CtrToken *expand_arg(MacroArg *arg) {
  return preprocess2(arg->tok);
}

static CtrToken *va_opt_body(CtrToken **rest, CtrToken *tok, MacroArg *args, CtrToken **cur);

static CtrToken *subst(CtrToken *tok, MacroArg *args);

static CtrToken *subst_va_opt(CtrToken **rest, CtrToken *tok, MacroArg *args) {
  tok = ctr_skip(tok->next, "(");
  int level = 0;
  CtrToken head = {0};
  CtrToken *cur = &head;
  while (!(level == 0 && ctr_equal(tok, ")"))) {
    if (tok->kind == TK_EOF) {
      ctr_fatal_tok(tok, "premature end of __VA_OPT__");
    }
    level += ctr_equal(tok, "(") ? 1 : ctr_equal(tok, ")") ? -1 : 0;
    cur = cur->next = ctr_copy_token(tok);
    tok = tok->next;
  }
  cur->next = new_eof(tok);
  *rest = tok->next;
  if (has_varargs(args)) {
    return subst(head.next, args);
  }
  return new_eof(tok);
}

static CtrToken *va_opt_body(CtrToken **rest, CtrToken *tok, MacroArg *args, CtrToken **cur) {
  CtrToken *body = subst_va_opt(rest, tok, args);
  for (CtrToken *t = body; t->kind != TK_EOF; t = t->next) {
    *cur = (*cur)->next = ctr_copy_token(t);
  }
  return *cur;
}

static int subst_hash(CtrToken **tokp, MacroArg *args, CtrToken **cur) {
  CtrToken *tok = *tokp;
  if (!ctr_equal(tok, "#")) {
    return 0;
  }
  MacroArg *arg = find_arg(args, tok->next);
  if (!arg) {
    ctr_fatal_tok(tok->next, "'#' is not followed by a macro parameter");
  }
  *cur = (*cur)->next = stringize(tok, arg->tok);
  *tokp = tok->next->next;
  return 1;
}

static int subst_paste_rhs(CtrToken **tokp, MacroArg *args, CtrToken **cur, CtrToken *head) {
  CtrToken *tok = *tokp;
  if (!ctr_equal(tok, "##")) {
    return 0;
  }
  if (*cur == head) {
    ctr_fatal_tok(tok, "'##' cannot appear at the start of a macro expansion");
  }
  if (tok->next->kind == TK_EOF) {
    ctr_fatal_tok(tok, "'##' cannot appear at the end of a macro expansion");
  }
  MacroArg *arg = find_arg(args, tok->next);
  if (arg) {
    CtrToken *argt = strip_marks(arg->tok);
    if (argt->kind != TK_EOF) {
      **cur = *paste(*cur, argt);
      (*cur)->next = NULL;
      for (CtrToken *t = argt->next; t->kind != TK_EOF; t = t->next) {
        *cur = (*cur)->next = ctr_copy_token(t);
      }
    }
    *tokp = tok->next->next;
    return 1;
  }
  CtrToken *nexttok = tok->next;
  **cur = *paste(*cur, nexttok);
  (*cur)->next = NULL;
  *tokp = nexttok->next;
  return 1;
}

static int subst_arg_paste_lhs(CtrToken **tokp, MacroArg *arg, CtrToken **cur, MacroArg *args) {
  CtrToken *tok = *tokp;
  if (!ctr_equal(tok->next, "##")) {
    return 0;
  }
  CtrToken *rhs = tok->next->next;
  CtrToken *argt = strip_marks(arg->tok);
  if (argt->kind == TK_EOF) {
    MacroArg *arg2 = find_arg(args, rhs);
    if (arg2) {
      CtrToken *a2 = strip_marks(arg2->tok);
      for (CtrToken *t = a2; t->kind != TK_EOF; t = t->next) {
        *cur = (*cur)->next = ctr_copy_token(t);
      }
    } else {
      *cur = (*cur)->next = ctr_copy_token(rhs);
    }
    *tokp = rhs->next;
    return 1;
  }
  for (CtrToken *t = argt; t->kind != TK_EOF; t = t->next) {
    *cur = (*cur)->next = ctr_copy_token(t);
  }
  *tokp = tok->next;
  return 1;
}

static void gnu_comma_paste(CtrToken **cur, CtrToken *head, MacroArg *arg) {
  if (arg->is_va_args && arg->tok->kind == TK_EOF && *cur != head && ctr_equal(*cur, ",")) {
    CtrToken *prev = head;
    while (prev->next != *cur) {
      prev = prev->next;
    }
    prev->next = NULL;
    *cur = prev;
  }
}

static CtrToken *subst(CtrToken *tok, MacroArg *args) {
  CtrToken head = {0};
  CtrToken *cur = &head;
  while (tok->kind != TK_EOF) {
    if (subst_hash(&tok, args, &cur)) {
      continue;
    }
    if (ctr_equal(tok, ",") && ctr_equal(tok->next, "##")) {
      MacroArg *arg = find_arg(args, tok->next->next);
      if (arg && arg->is_va_args) {
        if (arg->tok->kind == TK_EOF) {
          tok = tok->next->next->next;
        } else {
          cur = cur->next = ctr_copy_token(tok);
          tok = tok->next->next;
        }
        continue;
      }
    }
    if (subst_paste_rhs(&tok, args, &cur, &head)) {
      continue;
    }
    MacroArg *arg = find_arg(args, tok);
    if (arg && subst_arg_paste_lhs(&tok, arg, &cur, args)) {
      continue;
    }
    if (arg) {
      gnu_comma_paste(&cur, &head, arg);
      CtrToken *t = expand_arg(arg);
      for (; t->kind != TK_EOF; t = t->next) {
        cur = cur->next = ctr_copy_token(t);
      }
      tok = tok->next;
      continue;
    }
    if (ctr_equal(tok, "__VA_OPT__") && ctr_equal(tok->next, "(")) {
      va_opt_body(&tok, tok, args, &cur);
      continue;
    }
    cur = cur->next = ctr_copy_token(tok);
    tok = tok->next;
  }
  cur->next = tok;
  return head.next;
}

static int balanced(CtrToken *tok) {
  int depth = 0;
  for (; tok && tok->kind != TK_EOF; tok = tok->next) {
    if (ctr_equal(tok, "(") || ctr_equal(tok, "[") || ctr_equal(tok, "{")) {
      depth++;
    } else if (ctr_equal(tok, ")") || ctr_equal(tok, "]") || ctr_equal(tok, "}")) {
      depth--;
      if (depth < 0) {
        return 0;
      }
    }
  }
  return depth == 0;
}

static CtrToken *last_real(CtrToken *tok) {
  CtrToken *last = NULL;
  for (; tok && tok->kind != TK_EOF; tok = tok->next) {
    if (tok->kind != TK_MARK_BEGIN && tok->kind != TK_MARK_END) {
      last = tok;
    }
  }
  return last;
}

static int ends_in_funclike(CtrToken *body) {
  CtrToken *last = last_real(body);
  if (!last) {
    return 1;
  }
  CtrMacro *m = find_macro(last);
  return m && !m->is_objlike;
}

static int ends_with_semicolon_or_brace(CtrToken *body) {
  CtrToken *last = last_real(body);
  return last && (ctr_equal(last, ";") || ctr_equal(last, "}"));
}

static int markable(CtrMacro *m, CtrToken *use, CtrToken *expansion) {
  if (in_directive_expr || use->hideset || m->builtin) {
    return 0;
  }
  if (!ctr_prog || !ctr_prog->options || ctr_prog->options->expand_macros) {
    return 0;
  }
  if (use->file && use->file->is_system) {
    return 0;
  }
  if (!balanced(expansion) || ends_in_funclike(expansion) || ends_with_semicolon_or_brace(expansion)) {
    return 0;
  }
  return 1;
}

static CtrToken *new_mark(CtrTokenKind kind, CtrToken *tmpl, CtrMacroUse *use) {
  CtrToken *t = ctr_copy_token(tmpl);
  t->kind = kind;
  t->len = 0;
  t->mark = use;
  t->hideset = NULL;
  return t;
}

static CtrToken *wrap_marks(CtrToken *body, CtrToken *after, CtrToken *use_tok, CtrMacro *m,
                            MacroArg *args) {
  CtrMacroUse *use = ctr_alloc(sizeof(CtrMacroUse));
  use->macro = m;
  use->use_tok = use_tok;
  use->id = ++mark_ids;
  int n = 0;
  for (MacroArg *a = args; a; a = a->next) {
    n++;
  }
  use->arg_count = n;
  use->args = ctr_alloc(sizeof(CtrToken *) * (size_t)(n + 1));
  n = 0;
  for (MacroArg *a = args; a; a = a->next) {
    use->args[n++] = strip_marks(a->tok);
  }
  CtrToken *begin = new_mark(TK_MARK_BEGIN, use_tok, use);
  CtrToken *end = new_mark(TK_MARK_END, use_tok, use);
  end->next = after;
  begin->next = append(body, end);
  return begin;
}

static CtrToken *set_origin(CtrToken *body, CtrToken *origin) {
  for (CtrToken *t = body; t && t->kind != TK_EOF; t = t->next) {
    t->origin = origin;
  }
  return body;
}

static int expand_objlike(CtrToken **rest, CtrToken *tok, CtrMacro *m) {
  if (m->builtin) {
    BuiltinFn fn = (BuiltinFn)(void *)m->body;
    CtrToken *t = fn(tok);
    t->next = tok->next;
    *rest = t;
    return 1;
  }
  CtrHideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
  CtrToken *body = add_hideset(m->body, hs);
  body = set_origin(body, tok);
  CtrToken *eof = body;
  while (eof->next) {
    eof = eof->next;
  }
  m->use_count++;
  if (markable(m, tok, body)) {
    CtrToken *wrapped = wrap_marks(body, tok->next, tok, m, NULL);
    *rest = wrapped;
  } else {
    *rest = append(body, tok->next);
  }
  if ((*rest)->kind != TK_EOF || tok->next == *rest) {
    (*rest)->at_bol = tok->at_bol;
    (*rest)->has_space = tok->has_space;
  }
  return 1;
}

static CtrToken *next_real(CtrToken *tok) {
  while (tok->kind == TK_MARK_END) {
    tok = tok->next;
  }
  return tok;
}

static int expand_macro(CtrToken **rest, CtrToken *tok) {
  if (hideset_contains(tok->hideset, tok->loc, tok->len)) {
    return 0;
  }
  CtrMacro *m = find_macro(tok);
  if (!m) {
    return 0;
  }
  if (m->is_objlike) {
    return expand_objlike(rest, tok, m);
  }
  if (!ctr_equal(next_real(tok->next), "(")) {
    return 0;
  }
  while (tok->next->kind == TK_MARK_END) {
    tok->next->mark->id = -1;
    tok->next = tok->next->next;
  }
  CtrToken *macro_token = tok;
  MacroArg *args = read_macro_args(&tok, tok, m);
  CtrToken *rparen = tok;
  CtrHideset *hs = hideset_intersection(macro_token->hideset, rparen->hideset);
  hs = hideset_union(hs, new_hideset(m->name));
  CtrToken *body = subst(m->body, args);
  body = add_hideset(body, hs);
  body = set_origin(body, macro_token);
  m->use_count++;
  if (markable(m, macro_token, body)) {
    *rest = wrap_marks(body, tok->next, macro_token, m, args);
  } else {
    *rest = append(body, tok->next);
  }
  (*rest)->at_bol = macro_token->at_bol;
  (*rest)->has_space = macro_token->has_space;
  return 1;
}

static const char *system_dir;
static CtrVec *user_dirs;

static char *try_dir(const char *dir, const char *filename) {
  char *path = ctr_path_join(dir, filename);
  return ctr_file_exists(path) ? path : NULL;
}

const char *ctr_search_include(const char *filename, int quoted, const char *from_dir) {
  if (filename[0] == '/' || (filename[0] && filename[1] == ':')) {
    return ctr_file_exists(filename) ? filename : NULL;
  }
  char *path;
  if (quoted && from_dir && (path = try_dir(from_dir, filename))) {
    return path;
  }
  for (int i = 0; user_dirs && i < user_dirs->len; i++) {
    if ((path = try_dir(user_dirs->items[i], filename))) {
      include_next_idx = i + 1;
      return path;
    }
  }
  if (system_dir && (path = try_dir(system_dir, filename))) {
    include_next_idx = user_dirs ? user_dirs->len + 1 : 1;
    return path;
  }
  return NULL;
}

static const char *search_include_next(const char *filename, int start) {
  for (int i = start; user_dirs && i < user_dirs->len; i++) {
    char *path = try_dir(user_dirs->items[i], filename);
    if (path) {
      include_next_idx = i + 1;
      return path;
    }
  }
  int nuser = user_dirs ? user_dirs->len : 0;
  if (start <= nuser && system_dir) {
    char *path = try_dir(system_dir, filename);
    if (path) {
      include_next_idx = nuser + 1;
      return path;
    }
  }
  return NULL;
}

static char *read_include_filename(CtrToken **rest, CtrToken *tok, int *is_dquote) {
  if (tok->kind == TK_STR) {
    *is_dquote = 1;
    *rest = skip_line(tok->next);
    return ctr_strndup(tok->loc + 1, (size_t)(tok->len - 2));
  }
  if (ctr_equal(tok, "<")) {
    CtrToken *start = tok;
    for (; !ctr_equal(tok, ">"); tok = tok->next) {
      if (tok->at_bol || tok->kind == TK_EOF) {
        ctr_fatal_tok(tok, "expected '>'");
      }
    }
    *is_dquote = 0;
    *rest = skip_line(tok->next);
    return join_tokens(start->next, tok);
  }
  if (tok->kind == TK_IDENT) {
    CtrToken *line = copy_line(rest, tok);
    CtrToken *expanded = strip_marks(preprocess2(line));
    CtrToken *dummy;
    return read_include_filename(&dummy, expanded, is_dquote);
  }
  ctr_fatal_tok(tok, "expected a filename");
  return NULL;
}

static int has_include_test(CtrToken **rest, CtrToken *tok, int next) {
  tok = ctr_skip(tok->next, "(");
  int dq;
  CtrToken *start = tok;
  char *name;
  if (tok->kind == TK_STR) {
    dq = 1;
    name = ctr_strndup(tok->loc + 1, (size_t)(tok->len - 2));
    tok = tok->next;
  } else {
    tok = ctr_skip(tok, "<");
    CtrToken *s = tok;
    while (!ctr_equal(tok, ">")) {
      tok = tok->next;
    }
    name = join_tokens(s, tok);
    dq = 0;
    tok = tok->next;
  }
  *rest = ctr_skip(tok, ")");
  const char *dir = start->file ? ctr_path_dir(start->file->name) : NULL;
  const char *found = next ? search_include_next(name, include_next_idx)
                           : ctr_search_include(name, dq, dir);
  return found != NULL;
}

static CtrToken *detect_include_guard(CtrToken *tok, char **guard) {
  if (!is_hash(tok) || !ctr_equal(tok->next, "ifndef")) {
    return NULL;
  }
  tok = tok->next->next;
  if (tok->kind != TK_IDENT) {
    return NULL;
  }
  char *macro = ctr_token_text(tok);
  tok = tok->next;
  if (!is_hash(tok) || !ctr_equal(tok->next, "define") || !ctr_equal(tok->next->next, macro)) {
    return NULL;
  }
  while (tok->kind != TK_EOF) {
    if (!is_hash(tok)) {
      tok = tok->next;
      continue;
    }
    if (ctr_equal(tok->next, "endif") && tok->next->next->kind == TK_EOF) {
      *guard = macro;
      return tok;
    }
    if (ctr_equal(tok, "if") || ctr_equal(tok, "ifdef") || ctr_equal(tok, "ifndef")) {
      tok = skip_cond_incl(tok->next);
    } else {
      tok = tok->next;
    }
  }
  return NULL;
}

CtrVec *ctr_unit_files;

static void push_unique(CtrVec *vec, CtrFile *file) {
  for (int i = 0; i < vec->len; i++) {
    if (vec->items[i] == file) {
      return;
    }
  }
  ctr_vec_push(vec, file);
}

static void record_include(CtrFile *from, CtrFile *to) {
  if (!to || to->is_system) {
    return;
  }
  if (from) {
    push_unique(&from->includes, to);
  }
  if (ctr_unit_files) {
    push_unique(ctr_unit_files, to);
  }
}

static CtrToken *include_file(CtrToken *tok, const char *path, CtrToken *filename_tok) {
  if (ctr_map_get(&pragma_once, path)) {
    return tok;
  }
  char *guard_name = ctr_map_get(&include_guards, path);
  if (guard_name && ctr_map_get(&macros, guard_name)) {
    return tok;
  }
  CtrToken *tok2 = ctr_map_get(&include_cache, path);
  if (!tok2) {
    int is_system = system_dir && strncmp(path, system_dir, strlen(system_dir)) == 0;
    tok2 = ctr_tokenize_file(path, is_system || (filename_tok->file && filename_tok->file->is_system &&
                                                 strstr(path, "stdlib") != NULL && 0));
    if (!tok2) {
      ctr_fatal_tok(filename_tok, "cannot open %s", path);
    }
    ctr_map_put(&include_cache, path, tok2);
  }
  char *guard = NULL;
  if (detect_include_guard(tok2, &guard)) {
    ctr_map_put(&include_guards, path, guard);
  }
  record_include(filename_tok->file, tok2->file);
  return append(tok2, tok);
}

static void read_line_marker(CtrToken **rest, CtrToken *tok) {
  CtrToken *start = tok;
  tok = preprocess2(copy_line(rest, tok));
  tok = strip_marks(tok);
  if (tok->kind != TK_PPNUM && tok->kind != TK_NUM) {
    ctr_fatal_tok(tok, "invalid line marker");
  }
  ctr_convert_pp_number(tok);
  start->file->line_marker_at = start->line;
  start->file->line_marker_value = (int)tok->ival;
  if (tok->next->kind == TK_STR) {
    start->file->line_marker_name = ctr_strndup(tok->next->str, (size_t)tok->next->str_len - 1);
  }
}

static int presumed_line(CtrToken *tok) {
  CtrFile *f = tok->file;
  if (f && f->line_marker_at && tok->line > f->line_marker_at) {
    return f->line_marker_value + (tok->line - f->line_marker_at - 1);
  }
  return tok->line;
}

static CtrToken *make_pragma_pack(CtrToken *tmpl, int value) {
  CtrToken *t = tokenize_text("__ctr_pragma_pack ( 0 ) ;\n", tmpl);
  CtrToken *num = t->next->next;
  num->kind = TK_NUM;
  num->ival = value;
  num->ty = ctr_ty_int;
  for (CtrToken *x = t; x->kind != TK_EOF; x = x->next) {
    x->line = tmpl->line;
    x->col = tmpl->col;
    x->at_bol = 0;
  }
  t->at_bol = 1;
  return t;
}

static int pragma_pack_value(CtrToken *tok) {
  if (ctr_equal(tok, ")")) {
    return 0;
  }
  ctr_convert_pp_number(tok);
  return (int)tok->ival;
}

static CtrToken *handle_pragma_pack(CtrToken *tok, CtrToken *after) {
  CtrToken *p = tok->next;
  if (!ctr_equal(p, "(")) {
    return after;
  }
  p = p->next;
  if (ctr_equal(p, "push")) {
    if (pack_depth < 64) {
      pack_stack[pack_depth++] = pack_current;
    }
    p = p->next;
    if (ctr_equal(p, ",")) {
      pack_current = pragma_pack_value(p->next);
    }
  } else if (ctr_equal(p, "pop")) {
    pack_current = pack_depth > 0 ? pack_stack[--pack_depth] : 0;
  } else {
    pack_current = pragma_pack_value(p);
  }
  CtrToken *t = make_pragma_pack(tok, pack_current);
  CtrToken *last = t;
  while (last->next->kind != TK_EOF) {
    last = last->next;
  }
  last->next = after;
  return t;
}

typedef struct PushedMacro {
  struct PushedMacro *next;
  const char *name;
  CtrMacro *macro;
} PushedMacro;

static PushedMacro *pushed_macros;

static const char *pragma_macro_name(CtrToken *tok) {
  if (!ctr_equal(tok->next, "(") || tok->next->next->kind != TK_STR) {
    return NULL;
  }
  CtrToken *s = tok->next->next;
  return ctr_strndup(s->loc + 1, (size_t)(s->len - 2));
}

static void push_macro(const char *name) {
  PushedMacro *p = ctr_alloc(sizeof(PushedMacro));
  p->name = name;
  p->macro = ctr_map_get(&macros, name);
  p->next = pushed_macros;
  pushed_macros = p;
}

static void pop_macro(const char *name) {
  for (PushedMacro **pp = &pushed_macros; *pp; pp = &(*pp)->next) {
    if (strcmp((*pp)->name, name) == 0) {
      PushedMacro *p = *pp;
      *pp = p->next;
      if (p->macro) {
        ctr_map_put(&macros, name, p->macro);
      } else {
        ctr_map_delete(&macros, name);
      }
      return;
    }
  }
}

static CtrToken *do_pragma(CtrToken *tok, CtrToken *after) {
  if ((ctr_equal(tok, "push_macro") || ctr_equal(tok, "pop_macro")) && pragma_macro_name(tok)) {
    const char *name = pragma_macro_name(tok);
    if (ctr_equal(tok, "push_macro")) {
      push_macro(name);
    } else {
      pop_macro(name);
    }
    return after;
  }
  if (ctr_equal(tok, "once")) {
    ctr_map_put(&pragma_once, tok->file->name, (void *)1);
    return after;
  }
  if (ctr_equal(tok, "pack")) {
    return handle_pragma_pack(tok, after);
  }
  return after;
}

static CtrToken *directive_include(CtrToken *tok, CtrToken *start, int is_next) {
  int dquote;
  CtrToken *rest;
  char *filename = read_include_filename(&rest, tok->next, &dquote);
  const char *dir = start->file ? ctr_path_dir(start->file->name) : NULL;
  const char *path = is_next ? search_include_next(filename, include_next_idx)
                             : ctr_search_include(filename, dquote, dir);
  if (!path) {
    if (!dquote && !is_next) {
      ctr_fatal_tok(tok->next, "'%s' is not one of the C library headers the translator provides "
                                "(stdlib/c/include)", filename);
    }
    ctr_fatal_tok(tok->next, "cannot find include file '%s'", filename);
  }
  return include_file(rest, path, tok->next);
}

static CtrToken *directive_if(CtrToken *tok, CtrToken *start) {
  int64_t val = eval_const_expr(&tok, tok);
  push_cond_incl(start, val != 0);
  return val ? tok : skip_cond_incl(tok);
}

static CtrToken *directive_ifdef(CtrToken *tok, CtrToken *start, int negate) {
  int defined = find_macro(tok->next) != NULL;
  push_cond_incl(tok, negate ? !defined : defined);
  tok = skip_line(tok->next->next);
  int take = negate ? !defined : defined;
  (void)start;
  return take ? tok : skip_cond_incl(tok);
}

static CtrToken *directive_elif(CtrToken *tok, CtrToken *start) {
  if (!cond_incl || cond_incl->ctx == IN_ELSE) {
    ctr_fatal_tok(start, "stray #elif");
  }
  cond_incl->ctx = IN_ELIF;
  if (!cond_incl->included && eval_const_expr(&tok, tok)) {
    cond_incl->included = 1;
    return tok;
  }
  return skip_cond_incl(tok);
}

static CtrToken *directive_else(CtrToken *tok, CtrToken *start) {
  if (!cond_incl || cond_incl->ctx == IN_ELSE) {
    ctr_fatal_tok(start, "stray #else");
  }
  cond_incl->ctx = IN_ELSE;
  tok = skip_line(tok->next);
  return cond_incl->included ? skip_cond_incl(tok) : tok;
}

static CtrToken *directive_endif(CtrToken *tok, CtrToken *start) {
  if (!cond_incl) {
    ctr_fatal_tok(start, "stray #endif");
  }
  cond_incl = cond_incl->next;
  return skip_line(tok->next);
}

static CtrToken *directive_message(CtrToken *tok, int is_error) {
  CtrToken *rest;
  CtrToken *line = copy_line(&rest, tok->next);
  char *text = join_tokens(line, NULL);
  if (is_error) {
    ctr_fatal_tok(tok, "#error %s", text);
  }
  ctr_warn_tok(tok, "#warning %s", text);
  return rest;
}

static CtrToken *directive(CtrToken *tok) {
  CtrToken *start = tok;
  tok = tok->next;
  if (ctr_equal(tok, "include")) {
    return directive_include(tok, start, 0);
  }
  if (ctr_equal(tok, "include_next")) {
    return directive_include(tok, start, 1);
  }
  if (ctr_equal(tok, "define")) {
    read_macro_definition(&tok, tok->next);
    return tok;
  }
  if (ctr_equal(tok, "undef")) {
    ctr_pp_undef(ctr_token_text(tok->next));
    return skip_line(tok->next->next);
  }
  if (ctr_equal(tok, "if")) {
    return directive_if(tok, start);
  }
  if (ctr_equal(tok, "ifdef")) {
    return directive_ifdef(tok, start, 0);
  }
  if (ctr_equal(tok, "ifndef")) {
    return directive_ifdef(tok, start, 1);
  }
  if (ctr_equal(tok, "elif")) {
    return directive_elif(tok, start);
  }
  if (ctr_equal(tok, "else")) {
    return directive_else(tok, start);
  }
  if (ctr_equal(tok, "endif")) {
    return directive_endif(tok, start);
  }
  if (ctr_equal(tok, "line")) {
    read_line_marker(&tok, tok->next);
    return tok;
  }
  if (tok->kind == TK_PPNUM) {
    read_line_marker(&tok, tok);
    return tok;
  }
  if (ctr_equal(tok, "pragma")) {
    CtrToken *rest;
    CtrToken *line = copy_line(&rest, tok->next);
    return do_pragma(line, rest);
  }
  if (ctr_equal(tok, "error")) {
    return directive_message(tok, 1);
  }
  if (ctr_equal(tok, "warning")) {
    return directive_message(tok, 0);
  }
  if (tok->at_bol) {
    return tok;
  }
  if (ctr_equal(tok, "ident") || ctr_equal(tok, "sccs") || ctr_equal(tok, "assert")) {
    CtrToken *rest;
    copy_line(&rest, tok->next);
    return rest;
  }
  ctr_fatal_tok(tok, "invalid preprocessor directive");
  return tok;
}

static CtrToken *destringize_pragma(CtrToken *tok, CtrToken **rest) {
  CtrToken *start = tok;
  tok = ctr_skip(tok->next, "(");
  if (tok->kind != TK_STR) {
    ctr_fatal_tok(start, "_Pragma expects a string literal");
  }
  CtrToken *str = tok;
  tok = ctr_skip(tok->next, ")");
  CtrBuf buf = {0};
  for (int i = 1; i < str->len - 1; i++) {
    if (str->loc[i] == '\\' && (str->loc[i + 1] == '"' || str->loc[i + 1] == '\\')) {
      i++;
    }
    ctr_buf_putc(&buf, str->loc[i]);
  }
  ctr_buf_putc(&buf, '\n');
  CtrToken *line = tokenize_text(ctr_buf_take(&buf), start);
  CtrToken *eof = line;
  while (eof->next && eof->next->kind != TK_EOF) {
    eof = eof->next;
  }
  *rest = tok;
  return line;
}

static CtrToken *preprocess2(CtrToken *tok) {
  CtrToken head = {0};
  CtrToken *cur = &head;
  while (tok->kind != TK_EOF) {
    if (expand_macro(&tok, tok)) {
      continue;
    }
    if (ctr_equal(tok, "_Pragma") && ctr_equal(tok->next, "(")) {
      CtrToken *rest;
      CtrToken *line = destringize_pragma(tok, &rest);
      tok = do_pragma(line, rest);
      continue;
    }
    if (!is_hash(tok)) {
      tok->line_delta = 0;
      cur = cur->next = tok;
      tok = tok->next;
      continue;
    }
    tok = directive(tok);
  }
  cur->next = tok;
  return head.next;
}

static CtrToken *file_macro(CtrToken *tmpl) {
  while (tmpl->origin) {
    tmpl = tmpl->origin;
  }
  if (tmpl->file && tmpl->file->line_marker_name && tmpl->line > tmpl->file->line_marker_at) {
    return new_str_token(tmpl->file->line_marker_name, tmpl);
  }
  return new_str_token(tmpl->file ? tmpl->file->display : "", tmpl);
}

static CtrToken *line_macro(CtrToken *tmpl) {
  while (tmpl->origin) {
    tmpl = tmpl->origin;
  }
  return new_num_token(presumed_line(tmpl), tmpl);
}

static CtrToken *counter_macro(CtrToken *tmpl) { return new_num_token(counter_value++, tmpl); }

static CtrToken *date_macro(CtrToken *tmpl) { return new_str_token("Nov 27 2026", tmpl); }

static CtrToken *time_macro(CtrToken *tmpl) { return new_str_token("00:00:00", tmpl); }

static void add_builtin(const char *name, BuiltinFn fn) {
  CtrMacro *m = add_macro(name, 1, NULL);
  m->builtin = 1;
  m->body = (CtrToken *)(void *)fn;
}

void ctr_pp_define(const char *name, const char *body) {
  CtrToken *tok = tokenize_text(ctr_format("%s\n", body), NULL);
  CtrMacro *m = add_macro(ctr_strdup(name), 1, tok);
  m->builtin = 0;
  m->file = NULL;
}

void ctr_pp_undef(const char *name) { ctr_map_delete(&macros, name); }

static void define_target_macros(CtrTarget target) {
  int windows = target == CTR_TARGET_WINDOWS_X64;
  int arm = target == CTR_TARGET_LINUX_ARM64 || target == CTR_TARGET_MACOS_ARM64;
  if (windows) {
    ctr_pp_define("_WIN32", "1");
    ctr_pp_define("_WIN64", "1");
    ctr_pp_define("__LLP64__", "1");
  } else if (target == CTR_TARGET_MACOS_ARM64) {
    ctr_pp_define("__APPLE__", "1");
    ctr_pp_define("__MACH__", "1");
    ctr_pp_define("__unix__", "1");
  } else {
    ctr_pp_define("__linux__", "1");
    ctr_pp_define("__linux", "1");
    ctr_pp_define("__unix__", "1");
    ctr_pp_define("__unix", "1");
    ctr_pp_define("__gnu_linux__", "1");
  }
  if (arm) {
    ctr_pp_define("__aarch64__", "1");
    ctr_pp_define("__arm64__", "1");
  } else {
    ctr_pp_define("__x86_64__", "1");
    ctr_pp_define("__x86_64", "1");
    ctr_pp_define("__amd64__", "1");
    ctr_pp_define("__amd64", "1");
  }
  if (!windows) {
    ctr_pp_define("__LP64__", "1");
    ctr_pp_define("_LP64", "1");
  }
  ctr_pp_define("__SIZEOF_LONG__", windows ? "4" : "8");
  ctr_pp_define("__SIZEOF_WCHAR_T__", windows ? "2" : "4");
  ctr_pp_define("__LONG_MAX__", windows ? "2147483647L" : "9223372036854775807L");
  if (target == CTR_TARGET_LINUX_ARM64) {
    ctr_pp_define("__CHAR_UNSIGNED__", "1");
  }
}

static const char *predefined[][2] = {
    {"__STDC__", "1"},
    {"__STDC_VERSION__", "199901L"},
    {"__STDC_HOSTED__", "1"},
    {"__STDC_NO_ATOMICS__", "1"},
    {"__STDC_NO_COMPLEX__", "1"},
    {"__STDC_NO_THREADS__", "1"},
    {"__STDC_NO_VLA__", "1"},
    {"__STDC_UTF_16__", "1"},
    {"__STDC_UTF_32__", "1"},
    {"__METTLE__", "1"},
    {"__mettle_translate__", "1"},
    {"__CHAR_BIT__", "8"},
    {"__SIZEOF_POINTER__", "8"},
    {"__SIZEOF_INT__", "4"},
    {"__SIZEOF_SHORT__", "2"},
    {"__SIZEOF_LONG_LONG__", "8"},
    {"__SIZEOF_FLOAT__", "4"},
    {"__SIZEOF_DOUBLE__", "8"},
    {"__SIZEOF_LONG_DOUBLE__", "8"},
    {"__SIZEOF_SIZE_T__", "8"},
    {"__SIZEOF_PTRDIFF_T__", "8"},
    {"__INT_MAX__", "2147483647"},
    {"__LONG_LONG_MAX__", "9223372036854775807LL"},
    {"__SCHAR_MAX__", "127"},
    {"__SHRT_MAX__", "32767"},
    {"__ORDER_LITTLE_ENDIAN__", "1234"},
    {"__ORDER_BIG_ENDIAN__", "4321"},
    {"__ORDER_PDP_ENDIAN__", "3412"},
    {"__BYTE_ORDER__", "1234"},
    {"__FLOAT_WORD_ORDER__", "1234"},
    {"__func__", "__func__"},
    {"__FUNCTION__", "__func__"},
    {"__PRETTY_FUNCTION__", "__func__"},
    {"__restrict", "restrict"},
    {"__alignof__", "_Alignof"},
    {"__builtin_va_list", "__builtin_va_list"},
    {"__ctr_va_list_tag", "1"},
};

void ctr_pp_init(void) {
  memset(&macros, 0, sizeof(macros));
  memset(&pragma_once, 0, sizeof(pragma_once));
  memset(&include_guards, 0, sizeof(include_guards));
  cond_incl = NULL;
  pack_depth = 0;
  pack_current = 0;
  counter_value = 0;
  for (size_t i = 0; i < sizeof(predefined) / sizeof(*predefined); i++) {
    if (strcmp(predefined[i][0], predefined[i][1]) == 0) {
      continue;
    }
    ctr_pp_define(predefined[i][0], predefined[i][1]);
  }
  define_target_macros(ctr_prog->options->target);
  add_builtin("__FILE__", file_macro);
  add_builtin("__BASE_FILE__", file_macro);
  add_builtin("__LINE__", line_macro);
  add_builtin("__COUNTER__", counter_macro);
  add_builtin("__DATE__", date_macro);
  add_builtin("__TIME__", time_macro);
  add_builtin("__TIMESTAMP__", date_macro);
  system_dir = ctr_prog->options->system_include;
  user_dirs = &ctr_prog->options->include_dirs;
  for (int i = 0; i < ctr_prog->options->defines.len; i++) {
    char *def = ctr_strdup(ctr_prog->options->defines.items[i]);
    char *eq = strchr(def, '=');
    if (eq) {
      *eq = '\0';
      ctr_pp_define(def, eq + 1);
    } else {
      ctr_pp_define(def, "1");
    }
  }
}

static void join_adjacent_string_literals(CtrToken *tok) {
  for (CtrToken *t = tok; t->kind != TK_EOF; t = t->next) {
    if (t->kind != TK_STR || t->next->kind != TK_STR) {
      continue;
    }
    CtrToken *last = t;
    int elem = t->ty->base->size;
    CtrType *basety = t->ty->base;
    for (CtrToken *u = t->next; u->kind == TK_STR; u = u->next) {
      if (u->ty->base->size > elem) {
        elem = u->ty->base->size;
        basety = u->ty->base;
      }
      last = u;
    }
    CtrBuf buf = {0};
    for (CtrToken *u = t; u != last->next; u = u->next) {
      int uel = u->ty->base->size;
      int count = u->str_len / uel - 1;
      for (int i = 0; i < count; i++) {
        uint32_t c = 0;
        memcpy(&c, u->str + i * uel, (size_t)uel);
        for (int b = 0; b < elem; b++) {
          ctr_buf_putc(&buf, (char)((c >> (8 * b)) & 0xFF));
        }
      }
    }
    for (int b = 0; b < elem; b++) {
      ctr_buf_putc(&buf, 0);
    }
    t->str_len = (int)buf.len;
    t->str = ctr_buf_take(&buf);
    t->ty = ctr_array_of(basety, t->str_len / elem);
    t->next = last->next;
  }
}

static CtrToken *attach_marks(CtrToken *tok) {
  CtrToken head = {0};
  CtrToken *cur = &head;
  CtrMacroUse *stack[512];
  int depth = 0;
  CtrMacroUse *pending = NULL;
  CtrToken *last_real_tok = NULL;
  for (CtrToken *t = tok; t; t = t->next) {
    if (t->kind == TK_MARK_BEGIN) {
      if (depth < 512) {
        stack[depth++] = t->mark;
      }
      t->mark->end = NULL;
      t->mark->inner = pending;
      pending = t->mark;
      continue;
    }
    if (t->kind == TK_MARK_END) {
      int at = depth - 1;
      while (at >= 0 && stack[at] != t->mark) {
        at--;
      }
      if (at < 0) {
        continue;
      }
      while (depth > at + 1) {
        stack[--depth]->end = NULL;
      }
      CtrMacroUse *use = stack[--depth];
      use->end = use->id < 0 ? NULL : last_real_tok;
      if (pending == use) {
        pending = use->inner;
        use->end = NULL;
      }
      continue;
    }
    if (pending && t->kind != TK_EOF) {
      t->mark = pending;
      pending = NULL;
    } else if (t->kind != TK_EOF) {
      t->mark = NULL;
    }
    cur = cur->next = t;
    last_real_tok = t;
    if (t->kind == TK_EOF) {
      break;
    }
  }
  return head.next;
}

static int mark_valid(CtrMacroUse *use) { return use && use->end; }

static void prune_marks(CtrToken *tok) {
  for (CtrToken *t = tok; t && t->kind != TK_EOF; t = t->next) {
    while (t->mark && !mark_valid(t->mark)) {
      t->mark = t->mark->inner;
    }
    for (CtrMacroUse *u = t->mark; u; u = u->inner) {
      while (u->inner && !mark_valid(u->inner)) {
        u->inner = u->inner->inner;
      }
    }
  }
}

CtrToken *ctr_preprocess(CtrToken *tok) {
  tok = preprocess2(tok);
  if (cond_incl) {
    ctr_fatal_tok(cond_incl->tok, "unterminated conditional directive");
  }
  tok = attach_marks(tok);
  prune_marks(tok);
  ctr_convert_pp_tokens(tok);
  join_adjacent_string_literals(tok);
  for (CtrToken *t = tok; t->kind != TK_EOF; t = t->next) {
    if (t->origin) {
      CtrToken *o = t->origin;
      while (o->origin) {
        o = o->origin;
      }
      t->line_delta = o->line;
    }
  }
  return tok;
}

