#ifndef IR_SMT_H
#define IR_SMT_H

#include "../proof/smt.h"
#include "ir.h"

typedef struct {
  SmtTerm result;
  SmtTerm trap;
  SmtTerm returned;
  SmtTerm memory;
  char why[192];
} IRSmtOutcome;

typedef struct {
  const char *name;
  SmtTerm term;
} IRSmtShared;

typedef struct {
  const char *callee;
  SmtTerm args[16];
  size_t arg_count;
  SmtTerm reach;
} IRSmtCallSite;

enum {
  IR_SMT_SITE_PROVE = 0,
  IR_SMT_SITE_INVARIANT_ENTRY = 1,
  IR_SMT_SITE_INVARIANT_STEP = 2
};

typedef struct {
  SmtTerm reach;
  SmtTerm trap;
  SmtTerm condition;
  SourceLocation location;
  int kind;
} IRSmtProveSite;

#define IR_SMT_INVARIANT_CALL "__mettle_invariant"

typedef struct {
  SmtCtx *ctx;
  SmtTerm *params;
  size_t param_count;
  IRSmtShared *shared;
  size_t shared_count;
  size_t shared_cap;
  const IRProgram *program;
  unsigned inline_depth;
  int uf_calls;
  IRSmtCallSite *calls;
  size_t call_count;
  size_t call_cap;
  int havoc_loops;
  int record_proves;
  int (*deterministic)(void *context, const char *function);
  void *deterministic_context;
  IRSmtProveSite *proves;
  size_t prove_count;
  size_t prove_cap;
  SmtTerm assume;
  SmtTerm *region_base;
  uint64_t *region_size;
  unsigned char *region_private;
  size_t region_count;
  size_t region_cap;
  SmtTerm *access_addr;
  uint64_t *access_size;
  size_t access_count;
  size_t access_cap;
  SmtTerm *foreign;
  size_t foreign_count;
  size_t foreign_cap;
  int lockstep;
  int chose_nan;
  unsigned encode_index;
  struct IRSmtLoop *loops;
  size_t loop_count;
  struct IRSmtLoopVisit *visits;
  size_t visit_count;
  size_t visit_cap;
} IRSmtFrame;

typedef struct IRSmtLoop {
  const char *label;
  const char **names;
  size_t name_count;
  int memory;
} IRSmtLoop;

typedef struct IRSmtLoopVisit {
  unsigned side;
  size_t loop;
  int back;
  SmtTerm reach;
  SmtTerm memory;
  SmtTerm *values;
} IRSmtLoopVisit;

SmtTerm ir_smt_frame_assumption(IRSmtFrame *frame);

#define IR_SMT_PROVE_CALL "__mettle_prove"

SmtFunc ir_smt_value_func(SmtCtx *ctx, const char *name, size_t arity);
SmtFunc ir_smt_trap_func(SmtCtx *ctx, const char *name, size_t arity);

void ir_smt_frame_init(IRSmtFrame *frame, SmtCtx *ctx, const IRFunction *fn,
                       const IRProgram *program);
void ir_smt_frame_free(IRSmtFrame *frame);

int ir_smt_encode(IRSmtFrame *frame, const IRFunction *fn,
                  const IRInstruction *insns, size_t count, IRSmtOutcome *out);

SmtTerm ir_smt_binary(SmtCtx *ctx, const char *op, int is_unsigned, SmtTerm a,
                      SmtTerm b, SmtTerm *trap);

SmtTerm ir_smt_narrow(SmtCtx *ctx, SmtTerm v, int size, int is_unsigned);
int ir_smt_parse_scalar(const char *type, int *size, int *is_unsigned);

typedef enum { IR_SMT_PROVED, IR_SMT_REFUTED, IR_SMT_UNKNOWN } IRSmtVerdict;

typedef struct {
  IRSmtVerdict verdict;
  long long inputs[16];
  size_t input_count;
  uint64_t clauses;
  uint64_t conflicts;
  uint64_t lemmas;
  double seconds;
  char why[192];
} IRSmtReport;

void ir_smt_check_equivalence(const IRProgram *program, const IRFunction *a,
                              const IRInstruction *a_insns, size_t a_count,
                              const IRFunction *b, const IRInstruction *b_insns,
                              size_t b_count, const IRFunction *guard,
                              IRSmtReport *report);

enum {
  IR_IDPAT_VAR,
  IR_IDPAT_INT,
  IR_IDPAT_NONZERO,
  IR_IDPAT_POW2,
  IR_IDPAT_SAME,
  IR_IDPAT_NONNEG
};

enum {
  IR_IDACT_KEEP,
  IR_IDACT_CONST,
  IR_IDACT_SHL,
  IR_IDACT_SHR,
  IR_IDACT_AND_MASK
};

typedef struct {
  const char *op;
  int a_kind;
  long long a_value;
  int b_kind;
  long long b_value;
  int commutative;
  int act_kind;
  long long act_value;
} IRIdentityRow;

size_t ir_rewrite_identity_count(void);
void ir_rewrite_identity_row(size_t k, IRIdentityRow *row);

int ir_smt_prove_identity_table(FILE *out, int verbose);

#endif
