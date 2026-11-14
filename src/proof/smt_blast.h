#ifndef METTLE_SMT_BLAST_H
#define METTLE_SMT_BLAST_H

#include "smt_internal.h"
#include "smt_sat.h"

#define BLAST_NONE 0xffffffffu

typedef struct {
  uint32_t kind;
  SatLit a;
  SatLit b;
  SatLit c;
  SatLit out;
} BlastGate;

typedef struct {
  SmtCtx *ctx;
  SatSolver *sat;
  SatLit lit_true;
  SatLit lit_false;
  uint32_t *offset;
  uint32_t offset_cap;
  SatLit *pool;
  size_t pool_count;
  size_t pool_cap;
  BlastGate *gates;
  uint32_t *gate_buckets;
  uint32_t gate_count;
  uint32_t gate_cap;
  uint32_t gate_bucket_cap;
  uint32_t *stack;
  size_t stack_count;
  size_t stack_cap;
  int failed;
  const char *fail_reason;
} Blaster;

void blast_init(Blaster *b, SmtCtx *ctx, SatSolver *sat);
void blast_free(Blaster *b);
int blast_term(Blaster *b, SmtTerm t);
const SatLit *blast_bits(const Blaster *b, SmtTerm t);
void blast_assert(Blaster *b, SmtTerm formula);

#endif
