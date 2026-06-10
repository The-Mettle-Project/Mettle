#ifndef IR_OPTIMIZE_H
#define IR_OPTIMIZE_H

#include "ir.h"

typedef struct {
  /* Reserved for future IR optimization controls. */
  int preserve_function_boundaries;
  /* --simd-report: emit a note describing what each `@simd` loop became. */
  int simd_report;
  /* --explain: report every optimization decision (loop vectorization, call
   * inlining) as a note, with a reason whenever the optimizer declined. */
  int explain;
  /* When set, --explain remarks are limited to source locations in this file
   * (the main input), so imported stdlib modules don't flood the report. */
  const char *explain_focus_file;
} IROptimizeOptions;

// Runs optimization passes on the generated IR program.
// Currently implements:
// - Small-function inlining (including control flow, no calls in callee)
// - Copy/constant propagation for temporaries and stack locals
// - Fibonacci-style rotate_add fusion and loop-body rotate fusion
// - Small constant-bound counted-loop unrolling (<= 64 trips)
// - Integer constant/algebraic folding and strength reduction
// - CSE, dead temp elimination, branch/jump CFG cleanup
// Returns 1 on success, 0 on error.
int ir_optimize_program(IRProgram *program,
                        const IROptimizeOptions *options);

// True when the most recent ir_optimize_program() failed because a `@simd!`
// vectorization contract was violated (a user error already reported to
// stderr), as opposed to an internal compiler error. Lets the driver skip the
// generic ICE report in that case.
int ir_optimize_had_user_error(void);

/* --explain (see ir_optimize_explain.c). The codegen MIR gate records, per
 * focus-file function, whether it got the register-allocating backend; the
 * driver flushes that section after codegen. No-ops unless --explain is on. */
int ir_explain_enabled(void);
void ir_explain_backend_function(const char *function_name,
                                 const char *filename, int ok,
                                 const char *detail);
void ir_explain_backend_flush(void);

// When optimization is NOT run (no -O/--release), `@simd` markers are never
// verified. This prints one note saying so (if any are present) and strips the
// markers from the program. Safe to call unconditionally on the debug path.
void ir_note_simd_contracts_unverified(IRProgram *program);

#endif // IR_OPTIMIZE_H
