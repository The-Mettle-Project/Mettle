#ifndef IR_PGO_H
#define IR_PGO_H

/* Zero-run profile-guided optimization.
 *
 * `--pgo` interprets the program's own main() inside the compiler (bounded
 * fuel, deterministic, externs modeled as pure) right after IR lowering and
 * harvests real execution frequencies: how many times each function was
 * called, how many interpreter steps ran in each function body, and how often
 * each source-keyed IR site executed. The optimizer then consumes the numbers
 * - most importantly the inliner, which lets a measured-hot callee bypass its
 * static size budget the same way an explicit @inline does, and declines to
 * bloat callers with measured-cold glue.
 *
 * Classic PGO needs an instrumented build plus a training run plus a
 * rebuild; this needs one flag. Fuel exhaustion is fine - a partial profile
 * of the first N million steps is still a real profile. When main() can't
 * be interpreted at all, the profile is simply absent and every consumer
 * falls back to its static heuristic.
 */

#include "ir.h"

/* Interpret main() and build the profile tables. Returns 1 when a profile
 * was collected (main found and at least some instructions executed). */
int ir_pgo_profile_program(IRProgram *program);

int ir_pgo_enabled(void);
void ir_pgo_reset(void);

/* Total interpreted calls of `name` across every call site. -1 = unknown
 * function / no profile. */
long long ir_pgo_callee_calls(const char *name);

/* Total interpreted instruction executions inside `name`. This catches hot
 * entry points such as main() whose call count is zero because they were
 * entered directly by the interpreter. -1 = unknown function / no profile. */
long long ir_pgo_function_body_steps(const char *name);

/* Execution count for instructions in `function_name` carrying this exact
 * source location. The table is source-keyed instead of instruction-indexed so
 * it survives ordinary early optimizer mutations. -1 = no profile / unusable
 * location. */
long long ir_pgo_site_count(const char *function_name,
                            SourceLocation location);

/* Shared hot threshold for function calls and source/block-like site counts.
 * METTLE_PGO_HOT overrides it; default is 1024. */
long long ir_pgo_hot_threshold(void);

/* Measured-hot: called at least METTLE_PGO_HOT (default 1024) times. */
int ir_pgo_function_is_hot(const char *name);

/* Print the profile summary (steps interpreted, hottest functions). */
void ir_pgo_print_summary(void);

#endif /* IR_PGO_H */
