#ifndef METTLE_SMT_H
#define METTLE_SMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct SmtCtx SmtCtx;
typedef struct SmtSolver SmtSolver;
typedef uint32_t SmtTerm;
typedef uint32_t SmtSort;
typedef uint32_t SmtFunc;

#define SMT_MAX_WIDTH 256u

typedef enum {
  SMT_K_INVALID = 0,
  SMT_K_TRUE,
  SMT_K_FALSE,
  SMT_K_CONST,
  SMT_K_VAR,
  SMT_K_NOT,
  SMT_K_AND,
  SMT_K_OR,
  SMT_K_XOR,
  SMT_K_ITE,
  SMT_K_EQ,
  SMT_K_BVNOT,
  SMT_K_BVNEG,
  SMT_K_BVAND,
  SMT_K_BVOR,
  SMT_K_BVXOR,
  SMT_K_BVADD,
  SMT_K_BVSUB,
  SMT_K_BVMUL,
  SMT_K_BVUDIV,
  SMT_K_BVUREM,
  SMT_K_BVSDIV,
  SMT_K_BVSREM,
  SMT_K_BVSMOD,
  SMT_K_BVSHL,
  SMT_K_BVLSHR,
  SMT_K_BVASHR,
  SMT_K_CONCAT,
  SMT_K_EXTRACT,
  SMT_K_ZEXT,
  SMT_K_SEXT,
  SMT_K_BVULT,
  SMT_K_BVULE,
  SMT_K_BVSLT,
  SMT_K_BVSLE,
  SMT_K_SELECT,
  SMT_K_STORE,
  SMT_K_APPLY,
  SMT_K_FORALL,
  SMT_K_EXISTS,
  SMT_K_COUNT
} SmtKind;

typedef enum { SMT_UNKNOWN = 0, SMT_SAT = 1, SMT_UNSAT = 2 } SmtResult;

typedef struct {
  uint64_t variables;
  uint64_t clauses;
  uint64_t conflicts;
  uint64_t decisions;
  uint64_t propagations;
  uint64_t restarts;
  uint64_t lemmas_checked;
  uint64_t terms;
  uint64_t gates;
  double blast_seconds;
  double sat_seconds;
  double check_seconds;
} SmtStats;

SmtCtx *smt_ctx_new(void);
void smt_ctx_free(SmtCtx *ctx);
void smt_ctx_set_simplify(SmtCtx *ctx, int enabled);
int smt_ctx_error(const SmtCtx *ctx, const char **message);
void smt_ctx_clear_error(SmtCtx *ctx);
uint32_t smt_ctx_term_count(const SmtCtx *ctx);

SmtSort smt_bool_sort(SmtCtx *ctx);
SmtSort smt_bv_sort(SmtCtx *ctx, unsigned width);
SmtSort smt_array_sort(SmtCtx *ctx, SmtSort index, SmtSort element);
int smt_sort_is_bool(const SmtCtx *ctx, SmtSort sort);
int smt_sort_is_bv(const SmtCtx *ctx, SmtSort sort);
int smt_sort_is_array(const SmtCtx *ctx, SmtSort sort);
unsigned smt_sort_width(const SmtCtx *ctx, SmtSort sort);
SmtSort smt_sort_index(const SmtCtx *ctx, SmtSort sort);
SmtSort smt_sort_element(const SmtCtx *ctx, SmtSort sort);

SmtTerm smt_true(SmtCtx *ctx);
SmtTerm smt_false(SmtCtx *ctx);
SmtTerm smt_bool(SmtCtx *ctx, int value);
SmtTerm smt_bv_u64(SmtCtx *ctx, unsigned width, uint64_t value);
SmtTerm smt_bv_i64(SmtCtx *ctx, unsigned width, int64_t value);
SmtTerm smt_bv_words(SmtCtx *ctx, unsigned width, const uint64_t *words);
SmtTerm smt_var(SmtCtx *ctx, const char *name, SmtSort sort);
SmtTerm smt_fresh(SmtCtx *ctx, const char *prefix, SmtSort sort);

SmtTerm smt_mk(SmtCtx *ctx, SmtKind kind, unsigned nargs,
               const SmtTerm *args);
SmtTerm smt_mk1(SmtCtx *ctx, SmtKind kind, SmtTerm a);
SmtTerm smt_mk2(SmtCtx *ctx, SmtKind kind, SmtTerm a, SmtTerm b);
SmtTerm smt_not(SmtCtx *ctx, SmtTerm a);
SmtTerm smt_and(SmtCtx *ctx, SmtTerm a, SmtTerm b);
SmtTerm smt_or(SmtCtx *ctx, SmtTerm a, SmtTerm b);
SmtTerm smt_xor(SmtCtx *ctx, SmtTerm a, SmtTerm b);
SmtTerm smt_implies(SmtCtx *ctx, SmtTerm a, SmtTerm b);
SmtTerm smt_iff(SmtCtx *ctx, SmtTerm a, SmtTerm b);
SmtTerm smt_eq(SmtCtx *ctx, SmtTerm a, SmtTerm b);
SmtTerm smt_ite(SmtCtx *ctx, SmtTerm c, SmtTerm a, SmtTerm b);
SmtTerm smt_extract(SmtCtx *ctx, SmtTerm a, unsigned hi, unsigned lo);
SmtTerm smt_zext(SmtCtx *ctx, SmtTerm a, unsigned extra);
SmtTerm smt_sext(SmtCtx *ctx, SmtTerm a, unsigned extra);
SmtTerm smt_resize(SmtCtx *ctx, SmtTerm a, unsigned width, int is_signed);
SmtTerm smt_select(SmtCtx *ctx, SmtTerm array, SmtTerm index);
SmtTerm smt_store(SmtCtx *ctx, SmtTerm array, SmtTerm index, SmtTerm value);
SmtFunc smt_func(SmtCtx *ctx, const char *name, unsigned nargs,
                 const SmtSort *domain, SmtSort range);
SmtTerm smt_apply(SmtCtx *ctx, SmtFunc func, unsigned nargs,
                  const SmtTerm *args);
SmtTerm smt_forall(SmtCtx *ctx, SmtTerm var, SmtTerm body);
SmtTerm smt_exists(SmtCtx *ctx, SmtTerm var, SmtTerm body);

SmtKind smt_term_kind(const SmtCtx *ctx, SmtTerm t);
SmtSort smt_term_sort(const SmtCtx *ctx, SmtTerm t);
unsigned smt_term_nargs(const SmtCtx *ctx, SmtTerm t);
SmtTerm smt_term_arg(const SmtCtx *ctx, SmtTerm t, unsigned i);
uint32_t smt_term_param(const SmtCtx *ctx, SmtTerm t, unsigned i);
const char *smt_term_name(const SmtCtx *ctx, SmtTerm t);
int smt_term_const_words(const SmtCtx *ctx, SmtTerm t, uint64_t *words);
int smt_term_is_const(const SmtCtx *ctx, SmtTerm t);
const char *smt_func_name(const SmtCtx *ctx, SmtFunc f);
unsigned smt_func_arity(const SmtCtx *ctx, SmtFunc f);
SmtSort smt_func_range(const SmtCtx *ctx, SmtFunc f);
SmtSort smt_func_domain(const SmtCtx *ctx, SmtFunc f, unsigned i);
SmtFunc smt_func_lookup(const SmtCtx *ctx, const char *name);

SmtTerm smt_subst(SmtCtx *ctx, SmtTerm t, unsigned n, const SmtTerm *vars,
                  const SmtTerm *values);
int smt_term_contains(SmtCtx *ctx, SmtTerm t, SmtTerm sub);
int smt_term_has_quantifier(SmtCtx *ctx, SmtTerm t);
unsigned smt_term_free_vars(SmtCtx *ctx, SmtTerm t, SmtTerm *out,
                            unsigned capacity);

void smt_term_print(const SmtCtx *ctx, SmtTerm t, FILE *out);
size_t smt_term_to_string(const SmtCtx *ctx, SmtTerm t, char *buf,
                          size_t capacity);
void smt_sort_print(const SmtCtx *ctx, SmtSort s, FILE *out);

SmtSolver *smt_solver_new(SmtCtx *ctx);
void smt_solver_free(SmtSolver *solver);
void smt_solver_assert(SmtSolver *solver, SmtTerm formula);
void smt_solver_set_certify(SmtSolver *solver, int enabled);
void smt_solver_set_conflict_limit(SmtSolver *solver, uint64_t limit);
SmtResult smt_solver_check(SmtSolver *solver);
const char *smt_solver_reason(const SmtSolver *solver);
int smt_solver_value(SmtSolver *solver, SmtTerm t, uint64_t *words);
void smt_solver_stats(const SmtSolver *solver, SmtStats *stats);
void smt_solver_print_model(SmtSolver *solver, FILE *out);

SmtResult smt_prove(SmtCtx *ctx, SmtTerm formula, int certify,
                    uint64_t conflict_limit, SmtSolver **keep);

int smtlib_run(const char *source, const char *path, FILE *out, int certify,
               int show_stats);
int smtlib_main(int argc, char **argv);

#endif
