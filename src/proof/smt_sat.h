#ifndef METTLE_SMT_SAT_H
#define METTLE_SMT_SAT_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t SatLit;

#define SAT_LIT(var, neg) ((SatLit)(((var) << 1) | ((neg) ? 1u : 0u)))
#define SAT_VAR(lit) ((lit) >> 1)
#define SAT_NEG(lit) ((lit) ^ 1u)
#define SAT_SIGN(lit) ((lit) & 1u)

enum { SAT_RESULT_UNKNOWN = 0, SAT_RESULT_SAT = 10, SAT_RESULT_UNSAT = 20 };

typedef struct SatSolver SatSolver;

typedef struct {
  uint64_t conflicts;
  uint64_t decisions;
  uint64_t propagations;
  uint64_t restarts;
  uint64_t learnt_literals;
  uint64_t minimized_literals;
  uint64_t reductions;
  uint64_t original_clauses;
  uint64_t eliminated;
} SatStats;

void sat_freeze(SatSolver *s, uint32_t var);
void sat_set_preprocess(SatSolver *s, int enabled);
void sat_set_propagation_limit(SatSolver *s, uint64_t limit);
int sat_propagation_limit_reached(const SatSolver *s);

SatSolver *sat_new(void);
void sat_free(SatSolver *s);
uint32_t sat_new_var(SatSolver *s);
uint32_t sat_var_count(const SatSolver *s);
void sat_add_clause(SatSolver *s, const SatLit *lits, unsigned n);
void sat_enable_proof(SatSolver *s);
int sat_solve(SatSolver *s, uint64_t conflict_limit);
int sat_value(const SatSolver *s, uint32_t var);
void sat_get_stats(const SatSolver *s, SatStats *stats);

int sat_refutation_checked(SatSolver *s, uint64_t *lemmas_checked);

enum { SAT_PROOF_ORIGINAL = 0, SAT_PROOF_LEMMA = 1, SAT_PROOF_DELETE = 2 };

typedef struct SatChecker SatChecker;

SatChecker *sat_checker_new(void);
void sat_checker_free(SatChecker *c);
void sat_checker_feed(SatChecker *c, const uint32_t *words, size_t count);
int sat_checker_refuted(const SatChecker *c);
int sat_checker_failed(const SatChecker *c);
uint64_t sat_checker_lemmas(const SatChecker *c);

#endif
