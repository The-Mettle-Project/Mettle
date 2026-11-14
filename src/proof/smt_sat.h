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
} SatStats;

SatSolver *sat_new(void);
void sat_free(SatSolver *s);
uint32_t sat_new_var(SatSolver *s);
uint32_t sat_var_count(const SatSolver *s);
void sat_add_clause(SatSolver *s, const SatLit *lits, unsigned n);
void sat_enable_proof(SatSolver *s);
int sat_solve(SatSolver *s, uint64_t conflict_limit);
int sat_value(const SatSolver *s, uint32_t var);
void sat_get_stats(const SatSolver *s, SatStats *stats);

typedef struct {
  const uint32_t *originals;
  size_t original_words;
  const uint32_t *proof;
  size_t proof_words;
  uint32_t vars;
} SatProofView;

void sat_proof_view(const SatSolver *s, SatProofView *view);

int sat_drup_check(const SatProofView *view, uint64_t *lemmas_checked);

#endif
