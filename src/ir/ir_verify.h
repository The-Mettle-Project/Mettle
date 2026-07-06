#ifndef IR_VERIFY_H
#define IR_VERIFY_H

/* --verify: per-pass translation validation.
 *
 * When enabled, the pass driver snapshots a function's IR before every
 * optimization pass. If the pass reports a change, the BEFORE and AFTER IR
 * are executed by the reference interpreter (ir_interp.c) on identical
 * generated inputs and their observations compared: return value, final
 * buffer bytes, extern-call trace, touched globals. On divergence the
 * compiler prints the offending pass, function, and a concrete
 * counterexample, RESTORES the pre-pass IR, quarantines that (pass,
 * function) pair for the rest of the compile, and carries on - the emitted
 * binary is always built from validated IR.
 *
 * A wrong quarantine (a false positive from the input generator) costs only
 * optimization, never correctness.
 */

#include "ir.h"

void ir_verify_set_enabled(int enabled);
int ir_verify_enabled(void);

/* Called at the start/end of ir_optimize_program_pipeline. end prints the
 * validation summary to stderr and resets state. */
void ir_verify_begin_program(IRProgram *program);
void ir_verify_end_program(void);

/* True when a previous divergence quarantined `pass_name` for `function`;
 * the driver must skip the pass. */
int ir_verify_pass_quarantined(const IRFunction *function,
                               const char *pass_name);

typedef struct IRVerifySnapshot IRVerifySnapshot;

/* Deep-copy the function's instruction stream. Returns NULL when verification
 * is disabled/not begun (callers treat NULL as "skip validation"). */
IRVerifySnapshot *ir_verify_snapshot_take(IRFunction *function);
void ir_verify_snapshot_free(IRVerifySnapshot *snapshot);

/* METTLE_VERIFY_BREAK="pass_name[:function_name]": deliberately corrupt the
 * function's IR right after the named pass runs (once per compile), to prove
 * end-to-end that validation detects and quarantines a miscompiling pass.
 * Sets *changed when it fires. */
void ir_verify_maybe_sabotage(IRFunction *function, const char *pass_name,
                              int *changed);

/* Validate the pass application. Returns 1 when the pass is kept (validated,
 * or unverifiable and given the benefit of the doubt), 0 when a divergence
 * was found: the function's IR has been restored from the snapshot, the
 * (pass, function) pair quarantined, and *changed cleared. */
int ir_verify_check_pass(IRFunction *function, IRVerifySnapshot *snapshot,
                         const char *pass_name, int *changed);

/* Exit code contribution: number of divergences found this compile. */
int ir_verify_divergence_count(void);

#endif /* IR_VERIFY_H */
