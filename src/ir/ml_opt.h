#ifndef ML_OPT_H
#define ML_OPT_H

/* --ml-opt: model-driven IR rewriting behind the translation-validation gate.
 *
 * The GNN proposes rewrites; every proposal that can be checked is executed
 * through the reference-interpreter differential (ir_verify_check_rewrite)
 * before it is allowed to stand. A proposal that changes observable behavior
 * is discarded with a printed counterexample - the model is never trusted,
 * only checked. Proposals backed by a construction-time proof (GVN dataflow,
 * truth-table/GF(2) superoptimization) may stand unvalidated when the
 * interpreter cannot run the function; speculative proposals (model-flagged
 * dead code under --ml-opt-speculative) exist ONLY on the validator's word
 * and are dropped when the function is unverifiable. */

#include "ir.h"

typedef struct {
  int proposals; /* disposition lines the model produced */
  int validated; /* applied, interpreter-differential validated */
  int proven;    /* applied on construction-time proof only (unverifiable fn) */
  int rejected;  /* discarded: validator found a counterexample */
  int skipped;   /* declined applier, missing target, or unverifiable speculative */
} MLOptStats;

int ir_apply_ml_opt(IRProgram *program, MLOptStats *stats);
int ir_hoist_constants(IRProgram *program);

#endif /* ML_OPT_H */
