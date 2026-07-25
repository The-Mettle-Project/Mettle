/* calc - a tiny C-like language compiled to a native executable through
 * libmtlc, using ONLY the public API in <mtlc/...>. It is deliberately NOT the
 * Mettle frontend: a self-contained lexer + recursive-descent parser that
 * lowers straight into the libmtlc IR builder, then drives the backend
 * (optimize -> native codegen -> link) to a runnable binary. This is the
 * frontend-agnostic proof for libmtlc.
 *
 *   calc <input.calc> <output-exe>
 *
 * The language: 64-bit integers only. Functions, parameters, `var` locals,
 * assignment, `if`/`else`, `while`, `return`, calls (including recursion), and
 * the usual arithmetic/relational/logical operators. `main` is the entry point;
 * its return value becomes the process exit code.
 *
 *   fn fact(n) {
 *     if (n < 2) { return 1; }
 *     return n * fact(n - 1);
 *   }
 *   fn main() { return fact(5); }   // exits 120
 */
#include <mtlc/build.h>
#include <mtlc/mtlc.h>
#include <mtlc/pipeline.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- lexer */

typedef enum {
  T_EOF, T_INT, T_IDENT,
  T_FN, T_VAR, T_IF, T_ELSE, T_WHILE, T_RETURN,
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_SEMI, T_COMMA,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_ASSIGN, T_NOT,
  T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_AND, T_OR
} TokType;

typedef struct {
  TokType type;
  char text[64];
  long long ival;
  int line;
} Token;

typedef struct {
  Token *toks;
  size_t count, cap;
} TokenList;

static void tok_push(TokenList *l, Token t) {
  if (l->count == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 128;
    l->toks = realloc(l->toks, l->cap * sizeof(Token));
  }
  l->toks[l->count++] = t;
}

/* Return 1 on success, 0 on a lex error (message printed). */
static int lex(const char *src, TokenList *out) {
  int line = 1;
  const char *p = src;
  while (*p) {
    if (*p == '\n') { line++; p++; continue; }
    if (isspace((unsigned char)*p)) { p++; continue; }
    if (p[0] == '/' && p[1] == '/') { /* line comment */
      while (*p && *p != '\n') p++;
      continue;
    }
    Token t = {0};
    t.line = line;
    if (isdigit((unsigned char)*p)) {
      t.type = T_INT;
      t.ival = strtoll(p, (char **)&p, 10);
      tok_push(out, t);
      continue;
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
      size_t n = 0;
      while ((isalnum((unsigned char)*p) || *p == '_') && n < sizeof(t.text) - 1)
        t.text[n++] = *p++;
      t.text[n] = 0;
      if (!strcmp(t.text, "fn")) t.type = T_FN;
      else if (!strcmp(t.text, "var")) t.type = T_VAR;
      else if (!strcmp(t.text, "if")) t.type = T_IF;
      else if (!strcmp(t.text, "else")) t.type = T_ELSE;
      else if (!strcmp(t.text, "while")) t.type = T_WHILE;
      else if (!strcmp(t.text, "return")) t.type = T_RETURN;
      else t.type = T_IDENT;
      tok_push(out, t);
      continue;
    }
    /* two-character operators first */
    if (p[0] == '=' && p[1] == '=') { t.type = T_EQ; p += 2; tok_push(out, t); continue; }
    if (p[0] == '!' && p[1] == '=') { t.type = T_NE; p += 2; tok_push(out, t); continue; }
    if (p[0] == '<' && p[1] == '=') { t.type = T_LE; p += 2; tok_push(out, t); continue; }
    if (p[0] == '>' && p[1] == '=') { t.type = T_GE; p += 2; tok_push(out, t); continue; }
    if (p[0] == '&' && p[1] == '&') { t.type = T_AND; p += 2; tok_push(out, t); continue; }
    if (p[0] == '|' && p[1] == '|') { t.type = T_OR; p += 2; tok_push(out, t); continue; }
    switch (*p) {
    case '(': t.type = T_LPAREN; break;
    case ')': t.type = T_RPAREN; break;
    case '{': t.type = T_LBRACE; break;
    case '}': t.type = T_RBRACE; break;
    case ';': t.type = T_SEMI; break;
    case ',': t.type = T_COMMA; break;
    case '+': t.type = T_PLUS; break;
    case '-': t.type = T_MINUS; break;
    case '*': t.type = T_STAR; break;
    case '/': t.type = T_SLASH; break;
    case '%': t.type = T_PERCENT; break;
    case '=': t.type = T_ASSIGN; break;
    case '<': t.type = T_LT; break;
    case '>': t.type = T_GT; break;
    case '!': t.type = T_NOT; break;
    default:
      fprintf(stderr, "calc: line %d: unexpected character '%c'\n", line, *p);
      return 0;
    }
    p++;
    tok_push(out, t);
  }
  Token eof = {0};
  eof.type = T_EOF;
  eof.line = line;
  tok_push(out, eof);
  return 1;
}

/* ------------------------------------------------------------------ parser */

typedef struct {
  char name[64];
  MtlcValue value;
} Local;

typedef struct {
  TokenList *toks;
  size_t pos;
  MtlcBuilder *builder;
  const MtlcType *i64;
  MtlcFn *fn;      /* current function being lowered */
  Local locals[128];
  int local_count;
  int error;
} Parser;

static Token *peek(Parser *p) { return &p->toks->toks[p->pos]; }
static Token *advance(Parser *p) { return &p->toks->toks[p->pos++]; }

static void perr(Parser *p, const char *what) {
  if (!p->error) {
    fprintf(stderr, "calc: line %d: expected %s, found token type %d\n",
            peek(p)->line, what, peek(p)->type);
    p->error = 1;
  }
}

static int accept(Parser *p, TokType t) {
  if (peek(p)->type == t) {
    p->pos++;
    return 1;
  }
  return 0;
}
static int expect(Parser *p, TokType t, const char *what) {
  if (accept(p, t)) return 1;
  perr(p, what);
  return 0;
}

static MtlcValue lookup_local(Parser *p, const char *name) {
  for (int i = p->local_count - 1; i >= 0; i--) {
    if (strcmp(p->locals[i].name, name) == 0) return p->locals[i].value;
  }
  return MTLC_NO_VALUE;
}
static void bind_local(Parser *p, const char *name, MtlcValue v) {
  if (p->local_count < (int)(sizeof(p->locals) / sizeof(p->locals[0]))) {
    snprintf(p->locals[p->local_count].name,
             sizeof(p->locals[p->local_count].name), "%s", name);
    p->locals[p->local_count].value = v;
    p->local_count++;
  }
}

static MtlcValue parse_expr(Parser *p);
static void parse_block(Parser *p);

/* primary := INT | IDENT | IDENT '(' args ')' | '(' expr ')' | '-' primary | '!' primary */
static MtlcValue parse_primary(Parser *p) {
  Token *t = peek(p);
  if (t->type == T_INT) {
    advance(p);
    return mtlc_const_int(p->fn, p->i64, t->ival);
  }
  if (t->type == T_MINUS) {
    advance(p);
    return mtlc_unary_op(p->fn, MTLC_UNOP_NEGATE, parse_primary(p), p->i64);
  }
  if (t->type == T_NOT) {
    advance(p);
    return mtlc_unary_op(p->fn, MTLC_UNOP_LOGICAL_NOT, parse_primary(p),
                         p->i64);
  }
  if (t->type == T_LPAREN) {
    advance(p);
    MtlcValue v = parse_expr(p);
    expect(p, T_RPAREN, "')'");
    return v;
  }
  if (t->type == T_IDENT) {
    char name[64];
    snprintf(name, sizeof(name), "%s", t->text);
    advance(p);
    if (accept(p, T_LPAREN)) { /* call */
      MtlcValue args[16];
      size_t argc = 0;
      if (peek(p)->type != T_RPAREN) {
        do {
          if (argc < 16) args[argc] = parse_expr(p);
          else parse_expr(p);
          argc++;
        } while (accept(p, T_COMMA));
      }
      expect(p, T_RPAREN, "')'");
      return mtlc_call(p->fn, name, args, argc, p->i64);
    }
    MtlcValue v = lookup_local(p, name);
    if (v == MTLC_NO_VALUE) {
      fprintf(stderr, "calc: line %d: unknown name '%s'\n", t->line, name);
      p->error = 1;
    }
    return v;
  }
  perr(p, "an expression");
  return MTLC_NO_VALUE;
}

/* Precedence-climbing over the binary operators. The backend's operator enum
 * means a token that is not an operator cannot silently become bad IR. */
static int binop_of(TokType t, MtlcBinaryOp *out) {
  switch (t) {
  case T_STAR: *out = MTLC_BINOP_MUL; return 1;
  case T_SLASH: *out = MTLC_BINOP_DIV; return 1;
  case T_PERCENT: *out = MTLC_BINOP_REM; return 1;
  case T_PLUS: *out = MTLC_BINOP_ADD; return 1;
  case T_MINUS: *out = MTLC_BINOP_SUB; return 1;
  case T_LT: *out = MTLC_BINOP_LT; return 1;
  case T_LE: *out = MTLC_BINOP_LE; return 1;
  case T_GT: *out = MTLC_BINOP_GT; return 1;
  case T_GE: *out = MTLC_BINOP_GE; return 1;
  case T_EQ: *out = MTLC_BINOP_EQ; return 1;
  case T_NE: *out = MTLC_BINOP_NE; return 1;
  case T_AND: *out = MTLC_BINOP_LOGICAL_AND; return 1;
  case T_OR: *out = MTLC_BINOP_LOGICAL_OR; return 1;
  default: return 0;
  }
}
static int binop_prec(TokType t) {
  switch (t) {
  case T_STAR: case T_SLASH: case T_PERCENT: return 6;
  case T_PLUS: case T_MINUS: return 5;
  case T_LT: case T_LE: case T_GT: case T_GE: return 4;
  case T_EQ: case T_NE: return 3;
  case T_AND: return 2;
  case T_OR: return 1;
  default: return 0;
  }
}
static MtlcValue parse_binary(Parser *p, int min_prec) {
  MtlcValue left = parse_primary(p);
  for (;;) {
    TokType t = peek(p)->type;
    int prec = binop_prec(t);
    if (prec == 0 || prec < min_prec) break;
    MtlcBinaryOp op;
    if (!binop_of(t, &op)) break;
    advance(p);
    MtlcValue right = parse_binary(p, prec + 1); /* left-associative */
    left = mtlc_binary_op(p->fn, op, left, right, p->i64);
  }
  return left;
}
static MtlcValue parse_expr(Parser *p) { return parse_binary(p, 1); }

static void parse_statement(Parser *p) {
  Token *t = peek(p);
  if (t->type == T_VAR) {
    advance(p);
    Token *name = peek(p);
    if (!expect(p, T_IDENT, "a variable name")) return;
    expect(p, T_ASSIGN, "'='");
    MtlcValue init = parse_expr(p);
    expect(p, T_SEMI, "';'");
    MtlcValue slot = mtlc_local(p->fn, name->text, p->i64);
    bind_local(p, name->text, slot);
    mtlc_assign(p->fn, slot, init);
    return;
  }
  if (t->type == T_RETURN) {
    advance(p);
    MtlcValue v = parse_expr(p);
    expect(p, T_SEMI, "';'");
    mtlc_return(p->fn, v);
    return;
  }
  if (t->type == T_IF) {
    advance(p);
    /* The builder hands out unique labels, so the frontend never composes
     * label strings -- and a target it forgets to place is caught at finish. */
    MtlcLabel lelse = mtlc_label_new(p->fn, "else");
    MtlcLabel lend = mtlc_label_new(p->fn, "endif");
    expect(p, T_LPAREN, "'('");
    MtlcValue cond = parse_expr(p);
    expect(p, T_RPAREN, "')'");
    mtlc_branch_if_zero_to(p->fn, cond, lelse);
    parse_block(p);
    mtlc_jump_to(p->fn, lend);
    mtlc_label_here(p->fn, lelse);
    if (accept(p, T_ELSE)) parse_block(p);
    mtlc_label_here(p->fn, lend);
    return;
  }
  if (t->type == T_WHILE) {
    advance(p);
    MtlcLabel ltop = mtlc_label_new(p->fn, "loop");
    MtlcLabel lend = mtlc_label_new(p->fn, "endloop");
    expect(p, T_LPAREN, "'('");
    mtlc_label_here(p->fn, ltop);
    MtlcValue cond = parse_expr(p);
    expect(p, T_RPAREN, "')'");
    mtlc_branch_if_zero_to(p->fn, cond, lend);
    parse_block(p);
    mtlc_jump_to(p->fn, ltop);
    mtlc_label_here(p->fn, lend);
    return;
  }
  if (t->type == T_IDENT && p->toks->toks[p->pos + 1].type == T_ASSIGN) {
    char name[64];
    snprintf(name, sizeof(name), "%s", t->text);
    advance(p); /* name */
    advance(p); /* '=' */
    MtlcValue v = parse_expr(p);
    expect(p, T_SEMI, "';'");
    MtlcValue slot = lookup_local(p, name);
    if (slot == MTLC_NO_VALUE) {
      fprintf(stderr, "calc: line %d: assignment to unknown name '%s'\n",
              t->line, name);
      p->error = 1;
      return;
    }
    mtlc_assign(p->fn, slot, v);
    return;
  }
  /* bare expression statement (e.g. a call for its effect) */
  parse_expr(p);
  expect(p, T_SEMI, "';'");
}

static void parse_block(Parser *p) {
  if (!expect(p, T_LBRACE, "'{'")) return;
  while (peek(p)->type != T_RBRACE && peek(p)->type != T_EOF && !p->error) {
    parse_statement(p);
  }
  expect(p, T_RBRACE, "'}'");
}

static void parse_function(Parser *p) {
  expect(p, T_FN, "'fn'");
  Token *name = peek(p);
  if (!expect(p, T_IDENT, "a function name")) return;

  char pnames[16][64];
  const char *pname_ptrs[16];
  const MtlcType *ptypes[16];
  size_t pcount = 0;
  expect(p, T_LPAREN, "'('");
  if (peek(p)->type != T_RPAREN) {
    do {
      Token *pt = peek(p);
      if (!expect(p, T_IDENT, "a parameter name")) return;
      if (pcount < 16) {
        snprintf(pnames[pcount], sizeof(pnames[pcount]), "%s", pt->text);
        pname_ptrs[pcount] = pnames[pcount];
        ptypes[pcount] = p->i64;
        pcount++;
      }
    } while (accept(p, T_COMMA));
  }
  expect(p, T_RPAREN, "')'");
  if (p->error) return;

  p->fn = mtlc_builder_function(p->builder, name->text, p->i64, pname_ptrs,
                                ptypes, pcount, 0);
  if (!p->fn) {
    p->error = 1;
    return;
  }
  p->local_count = 0;
  for (size_t i = 0; i < pcount; i++) {
    bind_local(p, pnames[i], mtlc_fn_param(p->fn, i));
  }
  parse_block(p);
  /* Guarantee a terminator: a trailing `return 0` (dead if the body already
   * returned on every path). */
  mtlc_return(p->fn, mtlc_const_int(p->fn, p->i64, 0));
}

/* --------------------------------------------------------------------- main */

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (buf) {
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
  }
  fclose(f);
  return buf;
}

/* Both the builder and the pipeline report through this, so a backend failure
 * is reported in calc's own voice instead of appearing on stderr from
 * somewhere inside libmtlc. */
static void on_backend_diagnostic(void *user_data, MtlcDiagSeverity severity,
                                  const char *message) {
  (void)user_data;
  fprintf(stderr, "calc: backend %s: %s\n", mtlc_diag_severity_name(severity),
          message);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <input.calc> <output-exe>\n", argv[0]);
    return 2;
  }
  char *src = read_file(argv[1]);
  if (!src) {
    fprintf(stderr, "calc: cannot read '%s'\n", argv[1]);
    return 2;
  }

  TokenList toks = {0};
  if (!lex(src, &toks)) { free(src); free(toks.toks); return 1; }

  Parser p = {0};
  p.toks = &toks;
  p.builder = mtlc_builder_create();
  mtlc_builder_set_diagnostic_handler(p.builder, on_backend_diagnostic, NULL);
  p.i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  while (peek(&p)->type != T_EOF && !p.error) {
    parse_function(&p);
  }
  free(src);
  free(toks.toks);
  /* Builder errors are sticky, so one check after lowering the whole program
   * covers every emit call above. */
  if (p.error || !mtlc_builder_ok(p.builder)) {
    if (!p.error) {
      fprintf(stderr, "calc: IR construction failed: %s\n",
              mtlc_builder_error(p.builder));
    }
    mtlc_builder_destroy(p.builder);
    return 1;
  }

  MtlcModule *module = mtlc_builder_finish(p.builder);
  if (!module) {
    /* finish consumed the builder, so its message already went to the handler. */
    fprintf(stderr, "calc: IR construction failed\n");
    return 1;
  }

  /* Drive the backend: optimize, then compile+link to a native executable. */
  MtlcContext *ctx = mtlc_context_create();
  mtlc_context_set_diagnostic_handler(ctx, on_backend_diagnostic, NULL);
  mtlc_context_set_opt_level(ctx, 1);
  mtlc_context_set_whole_program(ctx, 1);
  int ok = mtlc_optimize(ctx, module) &&
           mtlc_build_executable(ctx, module, argv[2]);
  if (ok) {
    printf("calc: wrote %s (%zu functions) via libmtlc %s\n", argv[2],
           mtlc_module_function_count(module), mtlc_version());
  } else {
    const char *why = mtlc_context_last_error(ctx);
    fprintf(stderr, "calc: backend failed: %s\n", why ? why : "unknown error");
  }
  mtlc_module_destroy(module);
  mtlc_context_destroy(ctx);
  return ok ? 0 : 1;
}
