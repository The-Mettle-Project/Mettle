#include "ir_theorem.h"
#include "ir_smt.h"
#include "../common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THM_MAX_VARS 16u
#define THM_MAX_CANDIDATES 96u
#define THM_SMT_LIMIT 200000u

enum { DEF_UNBUILT, DEF_BUILDING, DEF_DEFINED, DEF_OPAQUE };

typedef struct {
  char *name;
  const IRFunction *fn;
  int state;
  size_t arity;
  SmtTerm params[16];
  SmtTerm value;
  SmtTerm trap;
  int recursive;
  int measure;
  char **callees;
  size_t ncallees;
  char why[200];
} ThmDef;

typedef struct {
  SmtTerm *hyps;
  size_t nhyps;
  size_t cap;
  SmtTerm concl;
} ThmGoal;

typedef struct {
  const char *name;
  const IRFunction *claim;
  const IRFunction *where;
  size_t nvars;
  SmtTerm vars[THM_MAX_VARS];
  const char *var_types[THM_MAX_VARS];
  SmtTerm assume;
  SmtTerm statement;
  int proved;
} ThmEntry;

typedef struct {
  IRProgram *program;
  SmtCtx *ctx;
  ThmDef *defs;
  size_t ndefs;
  size_t capdefs;
  ThmEntry *thms;
  size_t nthms;
  size_t capthms;
  ThmGoal *goals;
  size_t ngoals;
  size_t capgoals;
  ThmEntry *current;
  const char *file;
  size_t line;
  size_t column;
  int failed;
  int errors;
  int tactic_count;
  uint64_t smt_calls;
  uint64_t lemmas;
} Prover;

static IRTheoremOptions g_options;

void ir_theorem_set_options(const IRTheoremOptions *options) {
  if (options) {
    g_options = *options;
  } else {
    memset(&g_options, 0, sizeof(g_options));
  }
}

int ir_theorem_stop_after(void) { return g_options.stop_after; }

int ir_program_has_theorems(const IRProgram *program) {
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *f = program->functions[i];
    if (f && f->rewrite_role == IR_THEOREM_ROLE_CLAIM) {
      return 1;
    }
  }
  return 0;
}

static const IRFunction *thm_find_function(const IRProgram *program,
                                           const char *name) {
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *f = program->functions[i];
    if (f && f->name && strcmp(f->name, name) == 0) {
      return f;
    }
  }
  return NULL;
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} ThmText;

static void tt_put(ThmText *t, const char *s) {
  size_t n = strlen(s);
  if (t->len + n + 1 > t->cap) {
    size_t cap = t->cap ? t->cap * 2 : 256;
    while (cap < t->len + n + 1) {
      cap *= 2;
    }
    t->buf = realloc(t->buf, cap);
    t->cap = cap;
  }
  memcpy(t->buf + t->len, s, n + 1);
  t->len += n;
}

static void tt_printf(ThmText *t, const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  tt_put(t, tmp);
}

static int thm_narrow_of(Prover *p, SmtTerm t, SmtTerm *inner,
                         const char **cast) {
  SmtCtx *ctx = p->ctx;
  SmtKind k = smt_term_kind(ctx, t);
  if (k != SMT_K_SEXT && k != SMT_K_ZEXT) {
    return 0;
  }
  SmtTerm e = smt_term_arg(ctx, t, 0);
  unsigned w = smt_sort_width(ctx, smt_term_sort(ctx, e));
  if (smt_term_kind(ctx, e) != SMT_K_EXTRACT || smt_term_param(ctx, e, 1)) {
    return 0;
  }
  *inner = smt_term_arg(ctx, e, 0);
  static const char *signed_names[] = {"int8", "int16", "int32"};
  static const char *unsigned_names[] = {"uint8", "uint16", "uint32"};
  int idx = w == 8 ? 0 : w == 16 ? 1 : w == 32 ? 2 : -1;
  if (idx < 0) {
    *cast = k == SMT_K_SEXT ? "signed" : "unsigned";
  } else {
    *cast = k == SMT_K_SEXT ? signed_names[idx] : unsigned_names[idx];
  }
  return 1;
}

static int thm_is_bool_word(Prover *p, SmtTerm t, SmtTerm *cond, int *neg) {
  SmtCtx *ctx = p->ctx;
  if (smt_term_kind(ctx, t) != SMT_K_ITE) {
    return 0;
  }
  uint64_t a[4], b[4];
  if (!smt_term_const_words(ctx, smt_term_arg(ctx, t, 1), a) ||
      !smt_term_const_words(ctx, smt_term_arg(ctx, t, 2), b)) {
    return 0;
  }
  if (a[0] == 1 && b[0] == 0 && !a[1] && !b[1]) {
    *cond = smt_term_arg(ctx, t, 0);
    *neg = 0;
    return 1;
  }
  if (a[0] == 0 && b[0] == 1 && !a[1] && !b[1]) {
    *cond = smt_term_arg(ctx, t, 0);
    *neg = 1;
    return 1;
  }
  return 0;
}

static const char *thm_display_name(const char *name, char *buf, size_t cap) {
  const char *hash = strchr(name, '#');
  if (hash) {
    snprintf(buf, cap, "%s", hash + 1);
    return buf;
  }
  if (strncmp(name, IR_THEOREM_CLAIM_PREFIX, strlen(IR_THEOREM_CLAIM_PREFIX)) ==
      0) {
    return name + strlen(IR_THEOREM_CLAIM_PREFIX);
  }
  return name;
}

static void thm_print(Prover *p, SmtTerm t, ThmText *out, int depth) {
  SmtCtx *ctx = p->ctx;
  SmtKind k = smt_term_kind(ctx, t);
  char nb[160];
  if (out->len > 4000 || depth > 60) {
    tt_put(out, "...");
    return;
  }
  SmtTerm inner;
  const char *cast;
  int neg;
  switch (k) {
  case SMT_K_TRUE:
    tt_put(out, "true");
    return;
  case SMT_K_FALSE:
    tt_put(out, "false");
    return;
  case SMT_K_CONST: {
    uint64_t w[4];
    smt_term_const_words(ctx, t, w);
    unsigned width = smt_sort_width(ctx, smt_term_sort(ctx, t));
    if (width == 64) {
      tt_printf(out, "%lld", (long long)w[0]);
    } else {
      tt_printf(out, "%llu", (unsigned long long)w[0]);
    }
    return;
  }
  case SMT_K_VAR:
    tt_put(out, thm_display_name(smt_term_name(ctx, t), nb, sizeof(nb)));
    return;
  case SMT_K_SEXT:
  case SMT_K_ZEXT:
    if (thm_narrow_of(p, t, &inner, &cast)) {
      if (smt_term_kind(ctx, inner) == SMT_K_VAR) {
        thm_print(p, inner, out, depth + 1);
      } else {
        tt_printf(out, "(%s)(", cast);
        thm_print(p, inner, out, depth + 1);
        tt_put(out, ")");
      }
      return;
    }
    tt_put(out, k == SMT_K_SEXT ? "sext(" : "zext(");
    thm_print(p, smt_term_arg(ctx, t, 0), out, depth + 1);
    tt_put(out, ")");
    return;
  case SMT_K_EXTRACT:
    tt_put(out, "(");
    thm_print(p, smt_term_arg(ctx, t, 0), out, depth + 1);
    tt_printf(out, ")[%u:%u]", smt_term_param(ctx, t, 0),
              smt_term_param(ctx, t, 1));
    return;
  case SMT_K_ITE:
    if (thm_is_bool_word(p, t, &inner, &neg)) {
      if (neg) {
        tt_put(out, "!(");
        thm_print(p, inner, out, depth + 1);
        tt_put(out, ")");
      } else {
        thm_print(p, inner, out, depth + 1);
      }
      return;
    }
    tt_put(out, "(");
    thm_print(p, smt_term_arg(ctx, t, 0), out, depth + 1);
    tt_put(out, " ? ");
    thm_print(p, smt_term_arg(ctx, t, 1), out, depth + 1);
    tt_put(out, " : ");
    thm_print(p, smt_term_arg(ctx, t, 2), out, depth + 1);
    tt_put(out, ")");
    return;
  case SMT_K_NOT: {
    SmtTerm a = smt_term_arg(ctx, t, 0);
    if (smt_term_kind(ctx, a) == SMT_K_EQ) {
      SmtTerm x = smt_term_arg(ctx, a, 0), y = smt_term_arg(ctx, a, 1);
      uint64_t w[4];
      if (smt_term_const_words(ctx, y, w) && !w[0] && !w[1] &&
          thm_is_bool_word(p, x, &inner, &neg)) {
        if (neg) {
          tt_put(out, "!(");
          thm_print(p, inner, out, depth + 1);
          tt_put(out, ")");
        } else {
          thm_print(p, inner, out, depth + 1);
        }
        return;
      }
      tt_put(out, "(");
      thm_print(p, x, out, depth + 1);
      tt_put(out, " != ");
      thm_print(p, y, out, depth + 1);
      tt_put(out, ")");
      return;
    }
    tt_put(out, "!");
    thm_print(p, a, out, depth + 1);
    return;
  }
  case SMT_K_APPLY: {
    const char *name = smt_term_name(ctx, t);
    char fname[200];
    snprintf(fname, sizeof(fname), "%s", name);
    char *bang = strstr(fname, "!trap");
    if (bang) {
      *bang = 0;
      tt_put(out, "traps(");
    }
    tt_put(out, thm_display_name(fname, nb, sizeof(nb)));
    tt_put(out, "(");
    for (unsigned i = 0; i < smt_term_nargs(ctx, t); i++) {
      if (i) {
        tt_put(out, ", ");
      }
      thm_print(p, smt_term_arg(ctx, t, i), out, depth + 1);
    }
    tt_put(out, bang ? "))" : ")");
    return;
  }
  case SMT_K_FORALL:
  case SMT_K_EXISTS:
    tt_put(out, k == SMT_K_FORALL ? "forall " : "exists ");
    thm_print(p, smt_term_arg(ctx, t, 0), out, depth + 1);
    tt_put(out, ": ");
    thm_print(p, smt_term_arg(ctx, t, 1), out, depth + 1);
    return;
  default:
    break;
  }
  if (k == SMT_K_EQ) {
    SmtTerm x = smt_term_arg(ctx, t, 0), y = smt_term_arg(ctx, t, 1);
    uint64_t w[4];
    if (smt_term_const_words(ctx, x, w) && !w[0] && !w[1] &&
        thm_is_bool_word(p, y, &inner, &neg)) {
      tt_put(out, neg ? "" : "!(");
      thm_print(p, inner, out, depth + 1);
      tt_put(out, neg ? "" : ")");
      return;
    }
  }
  const char *op = "?";
  switch (k) {
  case SMT_K_AND: op = "&&"; break;
  case SMT_K_OR: op = "||"; break;
  case SMT_K_XOR: op = "^^"; break;
  case SMT_K_EQ: op = "=="; break;
  case SMT_K_BVAND: op = "&"; break;
  case SMT_K_BVOR: op = "|"; break;
  case SMT_K_BVXOR: op = "^"; break;
  case SMT_K_BVADD: op = "+"; break;
  case SMT_K_BVSUB: op = "-"; break;
  case SMT_K_BVMUL: op = "*"; break;
  case SMT_K_BVUDIV: op = "/u"; break;
  case SMT_K_BVUREM: op = "%u"; break;
  case SMT_K_BVSDIV: op = "/"; break;
  case SMT_K_BVSREM: op = "%"; break;
  case SMT_K_BVSMOD: op = "mod"; break;
  case SMT_K_BVSHL: op = "<<"; break;
  case SMT_K_BVLSHR: op = ">>u"; break;
  case SMT_K_BVASHR: op = ">>"; break;
  case SMT_K_BVULT: op = "<u"; break;
  case SMT_K_BVULE: op = "<=u"; break;
  case SMT_K_BVSLT: op = "<"; break;
  case SMT_K_BVSLE: op = "<="; break;
  case SMT_K_CONCAT: op = "++"; break;
  case SMT_K_BVNOT:
    tt_put(out, "~");
    thm_print(p, smt_term_arg(ctx, t, 0), out, depth + 1);
    return;
  case SMT_K_BVNEG:
    tt_put(out, "-");
    thm_print(p, smt_term_arg(ctx, t, 0), out, depth + 1);
    return;
  default:
    break;
  }
  tt_put(out, "(");
  thm_print(p, smt_term_arg(ctx, t, 0), out, depth + 1);
  tt_printf(out, " %s ", op);
  thm_print(p, smt_term_arg(ctx, t, 1), out, depth + 1);
  tt_put(out, ")");
}

static char *thm_render(Prover *p, SmtTerm t) {
  ThmText text = {NULL, 0, 0};
  thm_print(p, t, &text, 0);
  return text.buf ? text.buf : mettle_strdup("");
}

static void goal_init(ThmGoal *g, SmtTerm concl) {
  memset(g, 0, sizeof(*g));
  g->concl = concl;
}

static void goal_add(ThmGoal *g, SmtTerm h) {
  for (size_t i = 0; i < g->nhyps; i++) {
    if (g->hyps[i] == h) {
      return;
    }
  }
  if (g->nhyps == g->cap) {
    g->cap = g->cap ? g->cap * 2 : 8;
    g->hyps = realloc(g->hyps, g->cap * sizeof(SmtTerm));
  }
  g->hyps[g->nhyps++] = h;
}

static void goal_copy(ThmGoal *dst, const ThmGoal *src) {
  goal_init(dst, src->concl);
  for (size_t i = 0; i < src->nhyps; i++) {
    goal_add(dst, src->hyps[i]);
  }
}

static void goal_free(ThmGoal *g) {
  free(g->hyps);
  memset(g, 0, sizeof(*g));
}

static void prover_print_goals(Prover *p, FILE *out) {
  if (p->ngoals == 0) {
    fprintf(out, "    no goals left\n");
    return;
  }
  for (size_t i = 0; i < p->ngoals; i++) {
    ThmGoal *g = &p->goals[i];
    fprintf(out, "    goal %zu of %zu", i + 1, p->ngoals);
    if (p->current && p->current->nvars) {
      fprintf(out, " (");
      for (size_t v = 0; v < p->current->nvars; v++) {
        fprintf(out, "%s%s: %s", v ? ", " : "",
                smt_term_name(p->ctx, p->current->vars[v]),
                p->current->var_types[v] ? p->current->var_types[v] : "int64");
      }
      fprintf(out, ")");
    }
    fprintf(out, "\n");
    for (size_t h = 0; h < g->nhyps; h++) {
      char *s = thm_render(p, g->hyps[h]);
      fprintf(out, "      h%zu: %s\n", h + 1, s);
      free(s);
    }
    char *s = thm_render(p, g->concl);
    fprintf(out, "      |- %s\n", s);
    free(s);
  }
}

static void prover_error(Prover *p, const char *fmt, ...) {
  char msg[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  fprintf(stderr, "%s:%zu:%zu: error: theorem `%s`: %s\n",
          p->file ? p->file : "?", p->line, p->column,
          p->current ? p->current->name : "?", msg);
  p->failed = 1;
}

static ThmDef *def_find(Prover *p, const char *name) {
  for (size_t i = 0; i < p->ndefs; i++) {
    if (strcmp(p->defs[i].name, name) == 0) {
      return &p->defs[i];
    }
  }
  return NULL;
}

static ThmDef *def_get(Prover *p, const char *name);

static int def_reaches(Prover *p, const char *from, const char *target,
                       unsigned depth) {
  if (depth > 32) {
    return 1;
  }
  ThmDef *d = def_get(p, from);
  if (!d || (d->state != DEF_DEFINED && d->state != DEF_BUILDING)) {
    return 0;
  }
  for (size_t i = 0; i < d->ncallees; i++) {
    if (strcmp(d->callees[i], from) == 0) {
      continue;
    }
    if (strcmp(d->callees[i], target) == 0) {
      return 1;
    }
    if (def_reaches(p, d->callees[i], target, depth + 1)) {
      return 1;
    }
  }
  return 0;
}

static void def_opaque(ThmDef *d, const char *why, const char *detail) {
  d->state = DEF_OPAQUE;
  snprintf(d->why, sizeof(d->why), "%s%s%s", why, detail ? " " : "",
           detail ? detail : "");
}

static SmtTerm thm_lt(SmtCtx *ctx, SmtTerm a, SmtTerm b, int size,
                      int is_unsigned) {
  a = ir_smt_narrow(ctx, a, size, is_unsigned);
  b = ir_smt_narrow(ctx, b, size, is_unsigned);
  return smt_mk2(ctx, is_unsigned ? SMT_K_BVULT : SMT_K_BVSLT, a, b);
}

static void def_build(Prover *p, ThmDef *d) {
  SmtCtx *ctx = p->ctx;
  const IRFunction *fn = thm_find_function(p->program, d->name);
  d->fn = fn;
  if (!fn) {
    def_opaque(d, "it is not in the program", NULL);
    return;
  }
  if (fn->instruction_count == 0) {
    def_opaque(d, "it has no body", NULL);
    return;
  }
  d->arity = fn->parameter_count;
  if (d->arity > 16) {
    def_opaque(d, "it takes more than 16 parameters", NULL);
    return;
  }
  IRSmtFrame frame;
  memset(&frame, 0, sizeof(frame));
  frame.ctx = ctx;
  frame.program = p->program;
  frame.uf_calls = 1;
  frame.param_count = d->arity;
  frame.params = calloc(d->arity ? d->arity : 1, sizeof(SmtTerm));
  for (size_t i = 0; i < d->arity; i++) {
    char name[256];
    snprintf(name, sizeof(name), "%s#%s", d->name,
             fn->parameter_names[i] ? fn->parameter_names[i] : "p");
    frame.params[i] = smt_var(ctx, name, smt_bv_sort(ctx, 64));
    d->params[i] = frame.params[i];
  }
  IRSmtOutcome out;
  if (!ir_smt_encode(&frame, fn, fn->instructions, fn->instruction_count,
                     &out)) {
    def_opaque(d, "its body cannot be encoded:", out.why);
    ir_smt_frame_free(&frame);
    return;
  }
  SmtTerm free_vars[128];
  unsigned nfree = smt_term_free_vars(ctx, out.result, free_vars, 64);
  if (nfree > 64) {
    nfree = 64;
  }
  nfree += smt_term_free_vars(ctx, out.trap, free_vars + nfree, 64);
  for (unsigned i = 0; i < nfree && i < 128; i++) {
    int ok = 0;
    for (size_t k = 0; k < d->arity; k++) {
      ok |= free_vars[i] == d->params[k];
    }
    if (!ok) {
      def_opaque(d, "its result depends on", smt_term_name(ctx, free_vars[i]));
      ir_smt_frame_free(&frame);
      return;
    }
  }
  d->value = out.result;
  d->trap = out.trap;
  for (size_t i = 0; i < frame.call_count; i++) {
    const char *callee = frame.calls[i].callee;
    int seen = 0;
    for (size_t k = 0; k < d->ncallees; k++) {
      seen |= strcmp(d->callees[k], callee) == 0;
    }
    if (!seen) {
      d->callees = realloc(d->callees, (d->ncallees + 1) * sizeof(char *));
      d->callees[d->ncallees++] = mettle_strdup(callee);
    }
    if (strcmp(callee, d->name) == 0) {
      d->recursive = 1;
    }
  }
  if (d->recursive) {
    d->measure = -1;
    for (size_t i = 0; i < d->arity && d->measure < 0; i++) {
      int size = 8, is_unsigned = 0;
      if (fn->parameter_types && fn->parameter_types[i] &&
          !ir_smt_parse_scalar(fn->parameter_types[i], &size, &is_unsigned)) {
        continue;
      }
      SmtTerm ob = smt_true(ctx);
      for (size_t c = 0; c < frame.call_count; c++) {
        IRSmtCallSite *site = &frame.calls[c];
        if (strcmp(site->callee, d->name) != 0 || site->arg_count != d->arity) {
          continue;
        }
        ob = smt_and(ctx, ob,
                     smt_implies(ctx, site->reach,
                                 thm_lt(ctx, site->args[i], d->params[i], size,
                                        is_unsigned)));
      }
      SmtResult r = smt_prove(ctx, ob, 1, 20000, NULL);
      p->smt_calls++;
      if (r == SMT_UNSAT) {
        d->measure = (int)i;
      }
    }
    if (d->measure < 0) {
      def_opaque(d, "no parameter decreases at every recursive call, so the "
                    "definition is not known to terminate",
                 NULL);
      ir_smt_frame_free(&frame);
      return;
    }
  }
  ir_smt_frame_free(&frame);
  d->state = DEF_BUILDING;
  for (size_t i = 0; i < d->ncallees; i++) {
    if (strcmp(d->callees[i], d->name) == 0) {
      continue;
    }
    if (def_reaches(p, d->callees[i], d->name, 0)) {
      def_opaque(d, "it is mutually recursive with", d->callees[i]);
      return;
    }
  }
  d->state = DEF_DEFINED;
}

static ThmDef *def_get(Prover *p, const char *name) {
  ThmDef *d = def_find(p, name);
  if (d) {
    return d;
  }
  if (p->ndefs == p->capdefs) {
    p->capdefs = p->capdefs ? p->capdefs * 2 : 32;
    p->defs = realloc(p->defs, p->capdefs * sizeof(ThmDef));
  }
  size_t index = p->ndefs++;
  memset(&p->defs[index], 0, sizeof(ThmDef));
  p->defs[index].name = mettle_strdup(name);
  p->defs[index].state = DEF_BUILDING;
  def_build(p, &p->defs[index]);
  return &p->defs[index];
}

typedef struct {
  SmtTerm *items;
  size_t count;
  size_t cap;
} TermList;

static void tl_push(TermList *l, SmtTerm t) {
  for (size_t i = 0; i < l->count; i++) {
    if (l->items[i] == t) {
      return;
    }
  }
  if (l->count == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 32;
    l->items = realloc(l->items, l->cap * sizeof(SmtTerm));
  }
  l->items[l->count++] = t;
}

static void collect_apps(Prover *p, SmtTerm t, TermList *apps, uint32_t *seen,
                         size_t seen_cap) {
  SmtCtx *ctx = p->ctx;
  if (t < seen_cap) {
    if (seen[t]) {
      return;
    }
    seen[t] = 1;
  }
  if (smt_term_kind(ctx, t) == SMT_K_APPLY) {
    tl_push(apps, t);
  }
  for (unsigned i = 0; i < smt_term_nargs(ctx, t); i++) {
    collect_apps(p, smt_term_arg(ctx, t, i), apps, seen, seen_cap);
  }
}

static void goal_apps(Prover *p, const ThmGoal *g, TermList *apps,
                      int include_quantified) {
  size_t cap = smt_ctx_term_count(p->ctx) + 1;
  uint32_t *seen = calloc(cap, sizeof(uint32_t));
  collect_apps(p, g->concl, apps, seen, cap);
  for (size_t i = 0; i < g->nhyps; i++) {
    if (!include_quantified &&
        smt_term_kind(p->ctx, g->hyps[i]) == SMT_K_FORALL) {
      continue;
    }
    collect_apps(p, g->hyps[i], apps, seen, cap);
  }
  free(seen);
}

static int app_def_name(Prover *p, SmtTerm app, char *name, size_t cap,
                        int *is_trap) {
  const char *full = smt_term_name(p->ctx, app);
  if (!full) {
    return 0;
  }
  snprintf(name, cap, "%s", full);
  char *bang = strstr(name, "!trap");
  *is_trap = bang != NULL;
  if (bang) {
    *bang = 0;
  }
  return 1;
}

static int name_listed(const char *name, char **names, size_t count) {
  if (count == 0) {
    return 1;
  }
  for (size_t i = 0; i < count; i++) {
    if (strcmp(names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static size_t goal_unfold(Prover *p, ThmGoal *g, char **names, size_t count,
                          int recursive) {
  SmtCtx *ctx = p->ctx;
  TermList apps = {NULL, 0, 0};
  goal_apps(p, g, &apps, 0);
  SmtTerm *from = malloc((apps.count + 1) * sizeof(SmtTerm));
  SmtTerm *to = malloc((apps.count + 1) * sizeof(SmtTerm));
  size_t n = 0;
  for (size_t i = 0; i < apps.count; i++) {
    char name[256];
    int is_trap;
    if (!app_def_name(p, apps.items[i], name, sizeof(name), &is_trap) ||
        !name_listed(name, names, count)) {
      continue;
    }
    ThmDef *d = def_get(p, name);
    if (d->state != DEF_DEFINED || (d->recursive && recursive == 0) ||
        (!d->recursive && recursive == 2)) {
      continue;
    }
    unsigned na = smt_term_nargs(ctx, apps.items[i]);
    if (na != d->arity) {
      continue;
    }
    SmtTerm args[16];
    for (unsigned k = 0; k < na; k++) {
      args[k] = smt_term_arg(ctx, apps.items[i], k);
    }
    from[n] = apps.items[i];
    to[n] = smt_subst(ctx, is_trap ? d->trap : d->value, na, d->params, args);
    n++;
  }
  if (n) {
    g->concl = smt_replace(ctx, g->concl, (unsigned)n, from, to);
    for (size_t h = 0; h < g->nhyps; h++) {
      if (smt_term_kind(ctx, g->hyps[h]) == SMT_K_FORALL) {
        continue;
      }
      g->hyps[h] = smt_replace(ctx, g->hyps[h], (unsigned)n, from, to);
    }
  }
  free(from);
  free(to);
  free(apps.items);
  return n;
}

static void collect_candidates(Prover *p, const ThmGoal *g, TermList *out) {
  SmtCtx *ctx = p->ctx;
  TermList apps = {NULL, 0, 0};
  goal_apps(p, g, &apps, 0);
  for (size_t i = 0; i < apps.count && out->count < THM_MAX_CANDIDATES; i++) {
    for (unsigned k = 0; k < smt_term_nargs(ctx, apps.items[i]); k++) {
      SmtTerm a = smt_term_arg(ctx, apps.items[i], k);
      if (smt_sort_is_bv(ctx, smt_term_sort(ctx, a)) &&
          smt_sort_width(ctx, smt_term_sort(ctx, a)) == 64) {
        tl_push(out, a);
      }
    }
  }
  free(apps.items);
}

typedef struct {
  SmtResult result;
  char detail[400];
} SmtAttempt;

static int pattern_match(Prover *p, SmtTerm pat, SmtTerm ground,
                         const SmtTerm *vars, unsigned nv, SmtTerm *bind) {
  SmtCtx *ctx = p->ctx;
  for (unsigned i = 0; i < nv; i++) {
    if (pat == vars[i]) {
      if (!bind[i]) {
        bind[i] = ground;
        return 1;
      }
      return bind[i] == ground;
    }
  }
  int has_var = 0;
  for (unsigned i = 0; i < nv && !has_var; i++) {
    has_var = smt_term_contains(ctx, pat, vars[i]);
  }
  if (!has_var) {
    return pat == ground;
  }
  if (smt_term_kind(ctx, pat) != smt_term_kind(ctx, ground) ||
      smt_term_nargs(ctx, pat) != smt_term_nargs(ctx, ground) ||
      smt_term_sort(ctx, pat) != smt_term_sort(ctx, ground) ||
      smt_term_param(ctx, pat, 0) != smt_term_param(ctx, ground, 0) ||
      smt_term_param(ctx, pat, 1) != smt_term_param(ctx, ground, 1)) {
    return 0;
  }
  for (unsigned i = 0; i < smt_term_nargs(ctx, pat); i++) {
    if (!pattern_match(p, smt_term_arg(ctx, pat, i),
                       smt_term_arg(ctx, ground, i), vars, nv, bind)) {
      return 0;
    }
  }
  return 1;
}

static SmtTerm instantiate(Prover *p, SmtTerm forall, const TermList *ground,
                           const TermList *cands) {
  SmtCtx *ctx = p->ctx;
  SmtTerm vars[THM_MAX_VARS];
  unsigned nv = 0;
  SmtTerm body = forall;
  while (smt_term_kind(ctx, body) == SMT_K_FORALL && nv < THM_MAX_VARS) {
    vars[nv++] = smt_term_arg(ctx, body, 0);
    body = smt_term_arg(ctx, body, 1);
  }
  TermList triggers = {NULL, 0, 0};
  size_t cap = smt_ctx_term_count(ctx) + 1;
  uint32_t *seen = calloc(cap, sizeof(uint32_t));
  collect_apps(p, body, &triggers, seen, cap);
  free(seen);
  SmtTerm out = smt_true(ctx);
  int any = 0;
  for (size_t t = 0; t < triggers.count; t++) {
    SmtTerm trig = triggers.items[t];
    int mentions = 0;
    for (unsigned i = 0; i < nv && !mentions; i++) {
      mentions = smt_term_contains(ctx, trig, vars[i]);
    }
    if (!mentions) {
      continue;
    }
    for (size_t gi = 0; gi < ground->count; gi++) {
      SmtTerm bind[THM_MAX_VARS];
      memset(bind, 0, sizeof(bind));
      if (!pattern_match(p, trig, ground->items[gi], vars, nv, bind)) {
        continue;
      }
      int complete = 1;
      for (unsigned i = 0; i < nv; i++) {
        complete &= bind[i] != 0;
      }
      if (!complete) {
        continue;
      }
      out = smt_and(ctx, out, smt_subst(ctx, body, nv, vars, bind));
      any = 1;
    }
  }
  free(triggers.items);
  if (!any && nv == 1) {
    for (size_t c = 0; c < cands->count; c++) {
      out = smt_and(ctx, out, smt_subst(ctx, body, 1, vars, &cands->items[c]));
    }
  }
  return out;
}

static void goal_smt(Prover *p, const ThmGoal *g, uint64_t limit,
                     SmtAttempt *attempt) {
  SmtCtx *ctx = p->ctx;
  TermList cands = {NULL, 0, 0};
  TermList ground = {NULL, 0, 0};
  collect_candidates(p, g, &cands);
  goal_apps(p, g, &ground, 0);
  SmtTerm premise = smt_true(ctx);
  for (size_t i = 0; i < g->nhyps; i++) {
    SmtTerm h = g->hyps[i];
    if (smt_term_kind(ctx, h) == SMT_K_FORALL) {
      premise = smt_and(ctx, premise, instantiate(p, h, &ground, &cands));
      continue;
    }
    premise = smt_and(ctx, premise, h);
  }
  free(cands.items);
  free(ground.items);
  SmtSolver *solver = NULL;
  SmtTerm claim = smt_implies(ctx, premise, g->concl);
  attempt->result = smt_prove(ctx, claim, 1, limit, &solver);
  p->smt_calls++;
  SmtStats st;
  smt_solver_stats(solver, &st);
  p->lemmas += st.lemmas_checked;
  attempt->detail[0] = 0;
  if (attempt->result == SMT_SAT && p->current) {
    size_t at = 0;
    for (size_t v = 0; v < p->current->nvars; v++) {
      uint64_t w[4];
      int size = 8, is_unsigned = 0;
      ir_smt_parse_scalar(p->current->var_types[v], &size, &is_unsigned);
      SmtTerm nv = ir_smt_narrow(ctx, p->current->vars[v], size, is_unsigned);
      if (!smt_solver_value(solver, nv, w)) {
        continue;
      }
      int n = is_unsigned
                  ? snprintf(attempt->detail + at, sizeof(attempt->detail) - at,
                             "%s%s = %llu", v ? ", " : "",
                             smt_term_name(ctx, p->current->vars[v]),
                             (unsigned long long)w[0])
                  : snprintf(attempt->detail + at, sizeof(attempt->detail) - at,
                             "%s%s = %lld", v ? ", " : "",
                             smt_term_name(ctx, p->current->vars[v]),
                             (long long)w[0]);
      if (n < 0 || (size_t)n >= sizeof(attempt->detail) - at) {
        break;
      }
      at += (size_t)n;
    }
  } else if (attempt->result == SMT_UNKNOWN) {
    snprintf(attempt->detail, sizeof(attempt->detail), "%s",
             smt_solver_reason(solver) ? smt_solver_reason(solver)
                                       : "the solver gave up");
  }
  smt_solver_free(solver);
}

static void goals_replace_head(Prover *p, ThmGoal *with, size_t count) {
  goal_free(&p->goals[0]);
  size_t rest = p->ngoals - 1;
  size_t need = rest + count;
  if (need > p->capgoals) {
    p->capgoals = need + 8;
    p->goals = realloc(p->goals, p->capgoals * sizeof(ThmGoal));
  }
  memmove(&p->goals[count], &p->goals[1], rest * sizeof(ThmGoal));
  for (size_t i = 0; i < count; i++) {
    p->goals[i] = with[i];
  }
  p->ngoals = need;
}

static int encode_term_function(Prover *p, const char *fname,
                                IRSmtOutcome *out) {
  const IRFunction *fn = thm_find_function(p->program, fname);
  if (!fn) {
    prover_error(p, "internal error: `%s` is missing", fname);
    return 0;
  }
  IRSmtFrame frame;
  memset(&frame, 0, sizeof(frame));
  frame.ctx = p->ctx;
  frame.program = p->program;
  frame.uf_calls = 1;
  frame.param_count = p->current->nvars;
  frame.params = calloc(frame.param_count ? frame.param_count : 1,
                        sizeof(SmtTerm));
  memcpy(frame.params, p->current->vars,
         p->current->nvars * sizeof(SmtTerm));
  int ok = ir_smt_encode(&frame, fn, fn->instructions, fn->instruction_count,
                         out);
  ir_smt_frame_free(&frame);
  if (!ok) {
    prover_error(p, "an expression in the proof cannot be encoded: %s",
                 out->why);
  }
  return ok;
}

static SmtTerm truth_of(SmtCtx *ctx, const IRSmtOutcome *o) {
  return smt_and(ctx, smt_not(ctx, o->trap),
                 smt_not(ctx, smt_eq(ctx, o->result, smt_bv_u64(ctx, 64, 0))));
}

static ThmEntry *thm_find(Prover *p, const char *name) {
  for (size_t i = 0; i < p->nthms; i++) {
    if (strcmp(p->thms[i].name, name) == 0) {
      return &p->thms[i];
    }
  }
  return NULL;
}

static int tactic_auto(Prover *p, char **names, size_t count, int quiet) {
  ThmGoal *g = &p->goals[0];
  SmtAttempt attempt;
  attempt.result = SMT_UNKNOWN;
  attempt.detail[0] = 0;
  for (int round = 0; round < 2; round++) {
    for (int k = 0; k < 8; k++) {
      if (!goal_unfold(p, g, names, count, 0)) {
        break;
      }
    }
    goal_smt(p, g, THM_SMT_LIMIT, &attempt);
    if (attempt.result == SMT_UNSAT) {
      goal_free(g);
      memmove(&p->goals[0], &p->goals[1], (p->ngoals - 1) * sizeof(ThmGoal));
      p->ngoals--;
      return 1;
    }
    if (round == 1 || !goal_unfold(p, g, names, count, 2)) {
      break;
    }
  }
  if (!quiet) {
    if (attempt.result == SMT_SAT) {
      prover_error(p, "auto could not close the goal; with the definitions "
                      "unfolded so far it fails at %s",
                   attempt.detail);
    } else {
      prover_error(p, "auto could not close the goal: %s", attempt.detail);
    }
  }
  return 0;
}

static char *next_token(char **cursor, const char *delims) {
  char *s = *cursor;
  while (*s && strchr(delims, *s)) {
    s++;
  }
  if (!*s) {
    *cursor = s;
    return NULL;
  }
  char *start = s;
  while (*s && !strchr(delims, *s)) {
    s++;
  }
  if (*s) {
    *s++ = 0;
  }
  *cursor = s;
  return start;
}

static int run_tactic(Prover *p, char *line) {
  SmtCtx *ctx = p->ctx;
  char *words[64];
  size_t nw = 0;
  char *cursor = line;
  for (char *tok = next_token(&cursor, " \t"); tok && nw < 64;
       tok = next_token(&cursor, " \t")) {
    words[nw++] = tok;
  }
  if (nw < 2) {
    return 1;
  }
  sscanf(words[0], "%zu:%zu", &p->line, &p->column);
  const char *tactic = words[1];
  char **args = &words[2];
  size_t nargs = nw - 2;
  p->tactic_count++;
  if (strcmp(tactic, "show") == 0) {
    fprintf(stdout, "  %s:%zu: goals of `%s`\n", p->file, p->line,
            p->current->name);
    prover_print_goals(p, stdout);
    return 1;
  }
  if (p->ngoals == 0) {
    prover_error(p, "`%s` has no goal to work on; the proof is already done",
                 tactic);
    return 0;
  }
  ThmGoal *g = &p->goals[0];
  if (strcmp(tactic, "smt") == 0) {
    SmtAttempt attempt;
    goal_smt(p, g, THM_SMT_LIMIT, &attempt);
    if (attempt.result == SMT_UNSAT) {
      goal_free(g);
      memmove(&p->goals[0], &p->goals[1], (p->ngoals - 1) * sizeof(ThmGoal));
      p->ngoals--;
      return 1;
    }
    if (attempt.result == SMT_SAT) {
      prover_error(p, "smt could not close the goal; it fails at %s",
                   attempt.detail);
    } else {
      prover_error(p, "smt could not close the goal: %s", attempt.detail);
    }
    return 0;
  }
  if (strcmp(tactic, "auto") == 0) {
    return tactic_auto(p, args, nargs, 0);
  }
  if (strcmp(tactic, "unfold") == 0) {
    if (!goal_unfold(p, g, args, nargs, 1)) {
      prover_error(p, "unfold found no call it could expand");
      return 0;
    }
    return 1;
  }
  if (strcmp(tactic, "simp") == 0) {
    g->concl = smt_normalize(ctx, g->concl);
    for (size_t h = 0; h < g->nhyps; h++) {
      g->hyps[h] = smt_normalize(ctx, g->hyps[h]);
    }
    return 1;
  }
  if (strcmp(tactic, "induction") == 0) {
    if (nargs != 1) {
      prover_error(p, "induction takes one parameter");
      return 0;
    }
    ThmEntry *t = p->current;
    size_t idx = t->nvars;
    for (size_t v = 0; v < t->nvars; v++) {
      if (strcmp(smt_term_name(ctx, t->vars[v]), args[0]) == 0) {
        idx = v;
      }
    }
    if (idx == t->nvars) {
      prover_error(p, "`%s` is not a parameter of the theorem", args[0]);
      return 0;
    }
    int size = 8, is_unsigned = 0;
    if (!ir_smt_parse_scalar(t->var_types[idx], &size, &is_unsigned)) {
      prover_error(p, "induction needs an integer parameter, and `%s` has "
                      "type %s",
                   args[0], t->var_types[idx]);
      return 0;
    }
    SmtTerm fresh[THM_MAX_VARS];
    for (size_t v = 0; v < t->nvars; v++) {
      char prefix[160];
      snprintf(prefix, sizeof(prefix), "%s'", smt_term_name(ctx, t->vars[v]));
      fresh[v] = smt_fresh(ctx, prefix, smt_bv_sort(ctx, 64));
    }
    unsigned nv = (unsigned)t->nvars;
    SmtTerm premise = thm_lt(ctx, fresh[idx], t->vars[idx], size, is_unsigned);
    for (size_t h = 0; h < g->nhyps; h++) {
      if (smt_term_kind(ctx, g->hyps[h]) == SMT_K_FORALL) {
        continue;
      }
      premise = smt_and(ctx, premise,
                        smt_subst(ctx, g->hyps[h], nv, t->vars, fresh));
    }
    SmtTerm body = smt_implies(ctx, premise,
                               smt_subst(ctx, g->concl, nv, t->vars, fresh));
    for (size_t v = t->nvars; v-- > 0;) {
      body = smt_forall(ctx, fresh[v], body);
    }
    goal_add(g, body);
    return 1;
  }
  if (strcmp(tactic, "cases") == 0 || strcmp(tactic, "assert") == 0) {
    if (nargs != 1) {
      prover_error(p, "%s takes one condition", tactic);
      return 0;
    }
    IRSmtOutcome o;
    if (!encode_term_function(p, args[0], &o)) {
      return 0;
    }
    SmtTerm c = truth_of(ctx, &o);
    ThmGoal two[2];
    if (strcmp(tactic, "cases") == 0) {
      goal_copy(&two[0], g);
      goal_add(&two[0], c);
      goal_copy(&two[1], g);
      goal_add(&two[1], smt_not(ctx, c));
    } else {
      goal_copy(&two[0], g);
      two[0].concl = c;
      goal_copy(&two[1], g);
      goal_add(&two[1], c);
    }
    goals_replace_head(p, two, 2);
    return 1;
  }
  if (strcmp(tactic, "use") == 0) {
    if (nargs < 1) {
      prover_error(p, "use needs a theorem");
      return 0;
    }
    ThmEntry *lemma = thm_find(p, args[0]);
    if (!lemma) {
      prover_error(p, "there is no theorem `%s`", args[0]);
      return 0;
    }
    if (lemma == p->current) {
      prover_error(p, "a theorem cannot use itself; use induction");
      return 0;
    }
    if (!lemma->proved) {
      prover_error(p, "`%s` is not proved, so it cannot be used", args[0]);
      return 0;
    }
    if (nargs - 1 != lemma->nvars) {
      prover_error(p, "`%s` takes %zu arguments, given %zu", args[0],
                   lemma->nvars, nargs - 1);
      return 0;
    }
    SmtTerm values[THM_MAX_VARS];
    for (size_t i = 0; i < lemma->nvars; i++) {
      IRSmtOutcome o;
      if (!encode_term_function(p, args[1 + i], &o)) {
        return 0;
      }
      values[i] = o.result;
    }
    SmtTerm fact = smt_implies(ctx, lemma->assume, lemma->statement);
    goal_add(g, smt_subst(ctx, fact, (unsigned)lemma->nvars, lemma->vars,
                          values));
    return 1;
  }
  prover_error(p, "unknown tactic `%s`", tactic);
  return 0;
}

static int prove_theorem(Prover *p, ThmEntry *t, const char *script) {
  SmtCtx *ctx = p->ctx;
  p->current = t;
  p->failed = 0;
  p->tactic_count = 0;
  p->file = t->claim->location.filename;
  p->line = t->claim->location.line;
  p->column = t->claim->location.column;
  IRSmtOutcome claim;
  if (!encode_term_function(p, t->claim->name, &claim)) {
    return 0;
  }
  t->statement = truth_of(ctx, &claim);
  t->assume = smt_true(ctx);
  if (t->where) {
    IRSmtOutcome w;
    if (!encode_term_function(p, t->where->name, &w)) {
      return 0;
    }
    t->assume = truth_of(ctx, &w);
  }
  for (size_t i = 0; i < p->ngoals; i++) {
    goal_free(&p->goals[i]);
  }
  p->ngoals = 0;
  if (p->capgoals == 0) {
    p->capgoals = 8;
    p->goals = malloc(p->capgoals * sizeof(ThmGoal));
  }
  goal_init(&p->goals[0], t->statement);
  if (t->where) {
    goal_add(&p->goals[0], t->assume);
  }
  p->ngoals = 1;
  char *copy = mettle_strdup(script ? script : "");
  char *cursor = copy;
  int ok = 1;
  for (char *line = next_token(&cursor, "\n"); line && ok;
       line = next_token(&cursor, "\n")) {
    char *line_copy = mettle_strdup(line);
    char shown[256];
    snprintf(shown, sizeof(shown), "%s", line);
    ok = run_tactic(p, line_copy);
    free(line_copy);
    if (g_options.show_goals && ok) {
      char *space = strchr(shown, ' ');
      fprintf(stdout, "  after %s:\n", space ? space + 1 : shown);
      prover_print_goals(p, stdout);
    }
  }
  free(copy);
  if (g_options.interactive && strcmp(g_options.interactive, t->name) == 0) {
    FILE *in = g_options.input ? g_options.input : stdin;
    char buf[512];
    fprintf(stdout, "theorem %s, interactive; enter tactics (smt, auto, "
                    "unfold [f], induction x, simp, show), a blank line "
                    "ends\n",
            t->name);
    prover_print_goals(p, stdout);
    while (p->ngoals > 0 && fgets(buf, sizeof(buf), in)) {
      size_t n = strlen(buf);
      while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
      }
      if (n == 0) {
        break;
      }
      char line[600];
      snprintf(line, sizeof(line), "%zu:%zu %s", p->line, p->column, buf);
      p->failed = 0;
      if (run_tactic(p, line)) {
        prover_print_goals(p, stdout);
      }
      ok = 1;
    }
  }
  if (ok && p->ngoals > 0) {
    p->line = t->claim->location.line;
    p->column = t->claim->location.column;
    prover_error(p, "the proof ends with %zu goal%s left", p->ngoals,
                 p->ngoals == 1 ? "" : "s");
    ok = 0;
  }
  if (!ok) {
    if (p->ngoals) {
      fprintf(stderr, "  remaining goals:\n");
      prover_print_goals(p, stderr);
    }
    for (size_t i = 0; i < p->ndefs; i++) {
      if (p->defs[i].state == DEF_OPAQUE && p->defs[i].fn &&
          strncmp(p->defs[i].name, "__theorem", 9) != 0) {
        fprintf(stderr, "  note: `%s` has no definition the prover can use: "
                        "%s\n",
                p->defs[i].name, p->defs[i].why);
      }
    }
    return 0;
  }
  t->proved = 1;
  return 1;
}

int ir_theorems_check(IRProgram *program) {
  if (!ir_program_has_theorems(program)) {
    return 1;
  }
  Prover p;
  memset(&p, 0, sizeof(p));
  p.program = program;
  p.ctx = smt_ctx_new();
  int proved = 0, total = 0;
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *f = program->functions[i];
    if (!f || f->rewrite_role != IR_THEOREM_ROLE_CLAIM) {
      continue;
    }
    if (p.nthms == p.capthms) {
      p.capthms = p.capthms ? p.capthms * 2 : 16;
      p.thms = realloc(p.thms, p.capthms * sizeof(ThmEntry));
    }
    ThmEntry *t = &p.thms[p.nthms++];
    memset(t, 0, sizeof(*t));
    t->name = f->name + strlen(IR_THEOREM_CLAIM_PREFIX);
    t->claim = f;
    char where_name[300];
    snprintf(where_name, sizeof(where_name), "%s%s", IR_THEOREM_WHERE_PREFIX,
             t->name);
    t->where = thm_find_function(program, where_name);
    t->nvars = f->parameter_count < THM_MAX_VARS ? f->parameter_count
                                                 : THM_MAX_VARS;
    for (size_t v = 0; v < t->nvars; v++) {
      t->vars[v] = smt_var(p.ctx, f->parameter_names[v], smt_bv_sort(p.ctx, 64));
      t->var_types[v] = f->parameter_types ? f->parameter_types[v] : "int64";
    }
  }
  for (size_t i = 0; i < p.nthms; i++) {
    ThmEntry *t = &p.thms[i];
    double t0 = mettle_now_ms();
    uint64_t calls_before = p.smt_calls;
    total++;
    if (prove_theorem(&p, t, t->claim->theorem_script)) {
      proved++;
      if (g_options.report) {
        fprintf(stdout, "theorem %s: proved (%d tactic%s, %llu solver "
                        "call%s, %.1f ms)\n",
                t->name, p.tactic_count, p.tactic_count == 1 ? "" : "s",
                (unsigned long long)(p.smt_calls - calls_before),
                p.smt_calls - calls_before == 1 ? "" : "s",
                mettle_now_ms() - t0);
      }
    } else {
      p.errors++;
    }
  }
  if (g_options.report) {
    fprintf(stdout, "%d of %d theorem%s proved; %llu refutation lemmas "
                    "checked\n",
            proved, total, total == 1 ? "" : "s",
            (unsigned long long)p.lemmas);
  }
  for (size_t i = 0; i < p.ngoals; i++) {
    goal_free(&p.goals[i]);
  }
  free(p.goals);
  for (size_t i = 0; i < p.ndefs; i++) {
    free(p.defs[i].name);
    for (size_t k = 0; k < p.defs[i].ncallees; k++) {
      free(p.defs[i].callees[k]);
    }
    free(p.defs[i].callees);
  }
  free(p.defs);
  free(p.thms);
  smt_ctx_free(p.ctx);
  return p.errors == 0;
}
