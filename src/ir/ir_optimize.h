#ifndef IR_OPTIMIZE_H
#define IR_OPTIMIZE_H

#include "ir.h"

typedef struct {
  /* Reserved for future IR optimization controls. */
  int preserve_function_boundaries;
  /* --simd-report: emit a note describing what each `@simd` loop became. */
  int simd_report;
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

// When optimization is NOT run (no -O/--release), `@simd` markers are never
// verified. This prints one note saying so (if any are present) and strips the
// markers from the program. Safe to call unconditionally on the debug path.
void ir_note_simd_contracts_unverified(IRProgram *program);

#endif // IR_OPTIMIZE_H
