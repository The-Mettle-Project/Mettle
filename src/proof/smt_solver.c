#include "smt_blast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  SmtTerm array;
  SmtTerm index;
  SmtTerm value;
} ReadEntry;

typedef struct {
  SmtFunc func;
  uint32_t args;
  SmtTerm value;
} AppEntry;

typedef struct {
  SmtCtx *ctx;
  SmtMap memo;
  ReadEntry *reads;
  uint32_t read_count;
  uint32_t read_cap;
  AppEntry *apps;
  uint32_t app_count;
  uint32_t app_cap;
  SmtTerm *app_args;
  uint32_t app_arg_count;
  uint32_t app_arg_cap;
  int abstracted;
  int failed;
  const char *why;
} Lower;

struct SmtSolver {
  SmtCtx *ctx;
  SmtTerm *asserts;
  uint32_t count;
  uint32_t cap;
  int certify;
  int normalize;
  uint64_t conflict_limit;
  const char *reason;
  SmtModel model;
  int has_model;
  SmtStats stats;
  SmtResult last;
};

static void lower_fail(Lower *l, const char *why) {
  if (!l->failed) {
    l->failed = 1;
    l->why = why;
  }
}

static SmtTerm lower(Lower *l, SmtTerm t);

static SmtTerm lower_read(Lower *l, SmtTerm array, SmtTerm index) {
  SmtCtx *ctx = l->ctx;
  const SmtNode *n = &ctx->nodes[array];
  switch (n->kind) {
  case SMT_K_STORE: {
    SmtTerm j = lower(l, smt_arg(ctx, array, 1));
    SmtTerm v = lower(l, smt_arg(ctx, array, 2));
    SmtTerm hit = smt_eq(ctx, index, j);
    if (hit == ctx->bool_true) {
      return v;
    }
    SmtTerm rest = lower_read(l, smt_arg(ctx, array, 0), index);
    return smt_ite(ctx, hit, v, rest);
  }
  case SMT_K_ITE: {
    SmtTerm c = lower(l, smt_arg(ctx, array, 0));
    SmtTerm x = lower_read(l, smt_arg(ctx, array, 1), index);
    SmtTerm y = lower_read(l, smt_arg(ctx, array, 2), index);
    return smt_ite(ctx, c, x, y);
  }
  case SMT_K_VAR: {
    for (uint32_t i = 0; i < l->read_count; i++) {
      if (l->reads[i].array == array && l->reads[i].index == index) {
        return l->reads[i].value;
      }
    }
    SmtSort elem = ctx->sorts[n->sort].b;
    if (smt_sort_is_array(ctx, elem)) {
      lower_fail(l, "nested arrays are not supported");
      return ctx->bool_false;
    }
    if (l->read_count == l->read_cap) {
      l->read_cap = l->read_cap ? l->read_cap * 2u : 64u;
      l->reads = realloc(l->reads, l->read_cap * sizeof(ReadEntry));
    }
    ReadEntry *e = &l->reads[l->read_count++];
    e->array = array;
    e->index = index;
    e->value = smt_fresh(ctx, "read", elem);
    return e->value;
  }
  default:
    lower_fail(l, "array term of this kind is not supported");
    return ctx->bool_false;
  }
}

static SmtTerm lower_apply(Lower *l, SmtTerm t) {
  SmtCtx *ctx = l->ctx;
  SmtTerm args[64];
  unsigned na = ctx->nodes[t].nargs;
  SmtFunc func = ctx->nodes[t].p0;
  SmtSort sort = ctx->nodes[t].sort;
  if (na > 64u) {
    lower_fail(l, "function arity above 64");
    return ctx->bool_false;
  }
  if (smt_sort_is_array(ctx, sort)) {
    lower_fail(l, "array-valued functions are not supported");
    return ctx->bool_false;
  }
  for (unsigned i = 0; i < na; i++) {
    args[i] = lower(l, smt_arg(ctx, t, i));
  }
  for (uint32_t i = 0; i < l->app_count; i++) {
    AppEntry *e = &l->apps[i];
    if (e->func == func &&
        memcmp(&l->app_args[e->args], args, na * sizeof(SmtTerm)) == 0) {
      return e->value;
    }
  }
  if (l->app_count == l->app_cap) {
    l->app_cap = l->app_cap ? l->app_cap * 2u : 64u;
    l->apps = realloc(l->apps, l->app_cap * sizeof(AppEntry));
  }
  if (l->app_arg_count + na > l->app_arg_cap) {
    while (l->app_arg_count + na > l->app_arg_cap) {
      l->app_arg_cap = l->app_arg_cap ? l->app_arg_cap * 2u : 256u;
    }
    l->app_args = realloc(l->app_args, l->app_arg_cap * sizeof(SmtTerm));
  }
  AppEntry *e = &l->apps[l->app_count++];
  e->func = func;
  e->args = l->app_arg_count;
  memcpy(&l->app_args[l->app_arg_count], args, na * sizeof(SmtTerm));
  l->app_arg_count += na;
  e->value = smt_fresh(ctx, smt_func_name(ctx, func), sort);
  return e->value;
}

static SmtTerm lower(Lower *l, SmtTerm t) {
  SmtCtx *ctx = l->ctx;
  const SmtNode *n = &ctx->nodes[t];
  if (n->nargs == 0) {
    return t;
  }
  SmtTerm cached = smt_map_get(&l->memo, t);
  if (cached) {
    return cached;
  }
  SmtTerm out;
  switch (n->kind) {
  case SMT_K_SELECT: {
    SmtTerm index = lower(l, smt_arg(ctx, t, 1));
    out = lower_read(l, smt_arg(ctx, t, 0), index);
    break;
  }
  case SMT_K_STORE:
    out = t;
    break;
  case SMT_K_APPLY:
    out = lower_apply(l, t);
    break;
  case SMT_K_FORALL:
  case SMT_K_EXISTS:
    l->abstracted = 1;
    out = smt_fresh(ctx, "quant", ctx->sort_bool);
    break;
  default: {
    if (smt_sort_is_array(ctx, n->sort)) {
      out = t;
      break;
    }
    if (n->kind == SMT_K_EQ &&
        smt_sort_is_array(ctx, ctx->nodes[smt_arg(ctx, t, 0)].sort)) {
      lower_fail(l, "equality between arrays is not supported");
      out = ctx->bool_false;
      break;
    }
    SmtTerm args[4];
    int changed = 0;
    SmtKind kind = (SmtKind)n->kind;
    unsigned nargs = n->nargs < 4u ? n->nargs : 4u;
    uint32_t p0 = n->p0, p1 = n->p1;
    for (unsigned i = 0; i < nargs; i++) {
      SmtTerm a = smt_arg(ctx, t, i);
      args[i] = lower(l, a);
      changed |= args[i] != a;
    }
    if (!changed) {
      out = t;
    } else if (kind == SMT_K_EXTRACT) {
      out = smt_extract(ctx, args[0], p0, p1);
    } else if (kind == SMT_K_ZEXT) {
      out = smt_zext(ctx, args[0], p0);
    } else if (kind == SMT_K_SEXT) {
      out = smt_sext(ctx, args[0], p0);
    } else {
      out = smt_mk(ctx, kind, nargs, args);
    }
    break;
  }
  }
  smt_map_put(&l->memo, t, out);
  return out;
}

static int needs_lowering_rec(SmtCtx *ctx, SmtTerm t, uint32_t stamp) {
  if (smt_mark_test_set(ctx, t, stamp)) {
    return 0;
  }
  const SmtNode *n = &ctx->nodes[t];
  if (n->kind == SMT_K_SELECT || n->kind == SMT_K_APPLY ||
      n->kind == SMT_K_FORALL || n->kind == SMT_K_EXISTS ||
      n->kind == SMT_K_STORE) {
    return 1;
  }
  for (unsigned i = 0; i < n->nargs; i++) {
    if (needs_lowering_rec(ctx, smt_arg(ctx, t, i), stamp)) {
      return 1;
    }
  }
  return 0;
}

SmtSolver *smt_solver_new(SmtCtx *ctx) {
  SmtSolver *s = calloc(1, sizeof(SmtSolver));
  s->ctx = ctx;
  s->certify = 1;
  s->normalize = 1;
  smt_model_init(&s->model);
  return s;
}

void smt_solver_set_normalize(SmtSolver *s, int enabled) {
  s->normalize = enabled;
}

void smt_solver_free(SmtSolver *s) {
  if (!s) {
    return;
  }
  smt_model_free(&s->model);
  free(s->asserts);
  free(s);
}

void smt_solver_assert(SmtSolver *s, SmtTerm f) {
  if (s->count == s->cap) {
    s->cap = s->cap ? s->cap * 2u : 16u;
    s->asserts = realloc(s->asserts, s->cap * sizeof(SmtTerm));
  }
  s->asserts[s->count++] = f;
}

void smt_solver_set_certify(SmtSolver *s, int enabled) {
  s->certify = enabled;
}

void smt_solver_set_conflict_limit(SmtSolver *s, uint64_t limit) {
  s->conflict_limit = limit;
}

const char *smt_solver_reason(const SmtSolver *s) { return s->reason; }

void smt_solver_stats(const SmtSolver *s, SmtStats *stats) {
  *stats = s->stats;
}

static void build_model(SmtSolver *s, Blaster *b, SatSolver *sat, Lower *l) {
  SmtCtx *ctx = s->ctx;
  smt_model_free(&s->model);
  smt_model_init(&s->model);
  for (uint32_t t = 1; t < ctx->node_count && t < b->offset_cap; t++) {
    if (ctx->nodes[t].kind != SMT_K_VAR || b->offset[t] == BLAST_NONE) {
      continue;
    }
    unsigned w = smt_sort_is_bv(ctx, ctx->nodes[t].sort)
                     ? ctx->sorts[ctx->nodes[t].sort].width
                     : 1u;
    SmtBits v;
    smt_bits_zero(&v);
    for (unsigned i = 0; i < w; i++) {
      SatLit lit = b->pool[b->offset[t] + i];
      int bit = sat_value(sat, SAT_VAR(lit)) ^ (int)SAT_SIGN(lit);
      if (bit) {
        smt_bits_set(&v, i, 1);
      }
    }
    smt_model_set(&s->model, t, &v);
  }
  SmtEval e;
  smt_eval_init(&e, ctx, &s->model);
  for (uint32_t i = 0; i < l->read_count; i++) {
    SmtBits idx, val;
    smt_eval(&e, l->reads[i].index, &idx);
    smt_eval(&e, l->reads[i].value, &val);
    smt_model_array_put(smt_model_array(&s->model, l->reads[i].array), &idx,
                        &val);
  }
  for (uint32_t i = 0; i < l->app_count; i++) {
    AppEntry *a = &l->apps[i];
    unsigned na = smt_func_arity(ctx, a->func);
    SmtBits args[64], val;
    for (unsigned k = 0; k < na; k++) {
      smt_eval(&e, l->app_args[a->args + k], &args[k]);
    }
    smt_eval(&e, a->value, &val);
    smt_model_func_put(smt_model_func(&s->model, a->func), na, args, &val);
  }
  smt_eval_free(&e);
  s->has_model = 1;
}

static int validate_model(SmtSolver *s) {
  SmtEval e;
  smt_eval_init(&e, s->ctx, &s->model);
  int ok = 1;
  for (uint32_t i = 0; i < s->count && ok; i++) {
    SmtBits v;
    if (!smt_eval(&e, s->asserts[i], &v) || !(v.w[0] & 1u)) {
      ok = 0;
    }
  }
  smt_eval_free(&e);
  return ok;
}

SmtResult smt_solver_check(SmtSolver *s) {
  SmtCtx *ctx = s->ctx;
  s->reason = NULL;
  s->has_model = 0;
  memset(&s->stats, 0, sizeof(s->stats));
  double t0 = smt_now_seconds();
  const char *ctx_error;
  if (smt_ctx_error(ctx, &ctx_error)) {
    s->reason = ctx_error;
    return s->last = SMT_UNKNOWN;
  }
  SmtTerm formula = ctx->bool_true;
  for (uint32_t i = 0; i < s->count; i++) {
    formula = smt_and(ctx, formula, s->asserts[i]);
  }
  Lower l;
  memset(&l, 0, sizeof(l));
  l.ctx = ctx;
  smt_map_init(&l.memo);
  uint32_t stamp = smt_mark_begin(ctx);
  SmtTerm lowered = formula;
  SmtTerm constraints = ctx->bool_true;
  if (needs_lowering_rec(ctx, formula, stamp)) {
    lowered = lower(&l, formula);
    for (uint32_t i = 0; i < l.read_count; i++) {
      for (uint32_t j = i + 1u; j < l.read_count; j++) {
        if (l.reads[i].array != l.reads[j].array) {
          continue;
        }
        SmtTerm same = smt_eq(ctx, l.reads[i].index, l.reads[j].index);
        if (same == ctx->bool_false) {
          continue;
        }
        constraints = smt_and(
            ctx, constraints,
            smt_implies(ctx, same,
                        smt_eq(ctx, l.reads[i].value, l.reads[j].value)));
      }
    }
    for (uint32_t i = 0; i < l.app_count; i++) {
      for (uint32_t j = i + 1u; j < l.app_count; j++) {
        AppEntry *a = &l.apps[i], *c = &l.apps[j];
        if (a->func != c->func) {
          continue;
        }
        SmtTerm same = ctx->bool_true;
        unsigned na = smt_func_arity(ctx, a->func);
        for (unsigned k = 0; k < na; k++) {
          same = smt_and(ctx, same,
                         smt_eq(ctx, l.app_args[a->args + k],
                                l.app_args[c->args + k]));
        }
        if (same == ctx->bool_false) {
          continue;
        }
        constraints = smt_and(
            ctx, constraints,
            smt_implies(ctx, same, smt_eq(ctx, a->value, c->value)));
      }
    }
  }
  SmtResult result = SMT_UNKNOWN;
  if (l.failed) {
    s->reason = l.why;
    goto out;
  }
  SatSolver *sat = sat_new();
  if (s->certify) {
    sat_enable_proof(sat);
  }
  Blaster b;
  blast_init(&b, ctx, sat);
  if (s->normalize) {
    lowered = smt_normalize(ctx, lowered);
    constraints = smt_normalize(ctx, constraints);
  }
  blast_assert(&b, lowered);
  blast_assert(&b, constraints);
  double t1 = smt_now_seconds();
  s->stats.blast_seconds = t1 - t0;
  s->stats.gates = b.gate_count;
  if (b.failed) {
    s->reason = b.fail_reason;
    blast_free(&b);
    sat_free(sat);
    goto out;
  }
  int r = sat_solve(sat, s->conflict_limit);
  double t2 = smt_now_seconds();
  s->stats.sat_seconds = t2 - t1;
  SatStats ss;
  sat_get_stats(sat, &ss);
  s->stats.variables = sat_var_count(sat);
  s->stats.clauses = ss.original_clauses;
  s->stats.conflicts = ss.conflicts;
  s->stats.decisions = ss.decisions;
  s->stats.propagations = ss.propagations;
  s->stats.restarts = ss.restarts;
  if (r == SAT_RESULT_SAT) {
    build_model(s, &b, sat, &l);
    if (l.abstracted) {
      s->reason = "the formula holds quantifiers, which the solver treats as "
                  "opaque atoms";
      result = SMT_UNKNOWN;
    } else if (!validate_model(s)) {
      s->reason = "internal error: the model does not satisfy the assertions";
      result = SMT_UNKNOWN;
    } else {
      result = SMT_SAT;
    }
  } else if (r == SAT_RESULT_UNSAT) {
    result = SMT_UNSAT;
    if (s->certify) {
      SatProofView view;
      sat_proof_view(sat, &view);
      uint64_t checked = 0;
      int ok = sat_drup_check(&view, &checked);
      s->stats.lemmas_checked = checked;
      if (!ok) {
        s->reason = "internal error: the refutation failed proof checking";
        result = SMT_UNKNOWN;
      }
    }
    s->stats.check_seconds = smt_now_seconds() - t2;
  } else {
    s->reason = "conflict limit reached";
  }
  blast_free(&b);
  sat_free(sat);
out:
  s->stats.terms = ctx->node_count;
  smt_map_free(&l.memo);
  free(l.reads);
  free(l.apps);
  free(l.app_args);
  return s->last = result;
}

int smt_solver_value(SmtSolver *s, SmtTerm t, uint64_t *words) {
  if (!s->has_model) {
    return 0;
  }
  SmtEval e;
  smt_eval_init(&e, s->ctx, &s->model);
  SmtBits v;
  int ok = smt_eval(&e, t, &v);
  smt_eval_free(&e);
  if (ok) {
    memcpy(words, v.w, sizeof(v.w));
  }
  return ok;
}

void smt_solver_print_model(SmtSolver *s, FILE *out) {
  SmtCtx *ctx = s->ctx;
  if (!s->has_model) {
    return;
  }
  SmtTerm vars[512];
  unsigned total = 0;
  for (uint32_t i = 0; i < s->count; i++) {
    SmtTerm found[512];
    unsigned n = smt_term_free_vars(ctx, s->asserts[i], found, 512);
    for (unsigned k = 0; k < n && k < 512u; k++) {
      int dup = 0;
      for (unsigned j = 0; j < total && !dup; j++) {
        dup = vars[j] == found[k];
      }
      if (!dup && total < 512u) {
        vars[total++] = found[k];
      }
    }
  }
  fputs("(\n", out);
  for (unsigned i = 0; i < total; i++) {
    SmtTerm v = vars[i];
    SmtSort sort = ctx->nodes[v].sort;
    fprintf(out, "  (define-fun %s () ", smt_term_name(ctx, v));
    smt_sort_print(ctx, sort, out);
    fputc(' ', out);
    if (smt_sort_is_array(ctx, sort)) {
      SmtArrayModel *a = smt_model_array(&s->model, v);
      unsigned iw = ctx->sorts[ctx->sorts[sort].a].width;
      unsigned ew = ctx->sorts[ctx->sorts[sort].b].width;
      for (uint32_t k = 0; k < a->count; k++) {
        fputs("(store ", out);
      }
      fputs("((as const ", out);
      smt_sort_print(ctx, sort, out);
      fputs(") ", out);
      SmtBits zero;
      smt_bits_zero(&zero);
      smt_bits_print_hex(&zero, ew, out);
      fputc(')', out);
      for (uint32_t k = 0; k < a->count; k++) {
        fputc(' ', out);
        smt_bits_print_hex(&a->entries[k].index, iw, out);
        fputc(' ', out);
        smt_bits_print_hex(&a->entries[k].value, ew, out);
        fputc(')', out);
      }
    } else {
      uint64_t w[SMT_WORDS];
      smt_solver_value(s, v, w);
      if (smt_sort_is_bool(ctx, sort)) {
        fputs(w[0] ? "true" : "false", out);
      } else {
        SmtBits bits;
        memcpy(bits.w, w, sizeof(bits.w));
        smt_bits_print_hex(&bits, ctx->sorts[sort].width, out);
      }
    }
    fputs(")\n", out);
  }
  fputs(")\n", out);
}

SmtResult smt_prove(SmtCtx *ctx, SmtTerm formula, int certify,
                    uint64_t conflict_limit, SmtSolver **keep) {
  SmtSolver *s = smt_solver_new(ctx);
  smt_solver_set_certify(s, certify);
  smt_solver_set_conflict_limit(s, conflict_limit);
  smt_solver_assert(s, smt_not(ctx, formula));
  SmtResult r = smt_solver_check(s);
  if (keep) {
    *keep = s;
  } else {
    smt_solver_free(s);
  }
  return r;
}
