#ifndef IR_EXPLAIN_MEMORY_H
#define IR_EXPLAIN_MEMORY_H

#include <stddef.h>

/* --explain surfacing of the compile-time memory diagnostics.
 *
 * The memory analyzer (the type checker, src/semantic/type_checker_memory.c)
 * produces use-after-free / leak / dangling-borrow / out-of-bounds diagnostics
 * during type checking -- before the optimizer's --explain machinery runs and
 * in a different layer. To put those facts in the consolidated optimization
 * report (and its `.explain.json` sidecar, which the editor's report panel
 * reads), the analyzer calls `ir_explain_memory_note` for each diagnostic it
 * emits to the user. The explain finalizer renders them as a "memory" section.
 *
 * Decoupled on purpose: this header pulls in nothing from the IR layer, so the
 * semantic layer depends only on these two primitives, not on the IR headers.
 *
 * Collection is off unless `ir_explain_memory_set_collect` enabled it (the
 * driver does so only for `--explain` builds that also optimize, since that is
 * the only path on which the report is produced). When off, the note call is a
 * cheap no-op. */

/* Enable/disable collection. `focus_file`, when non-NULL, limits notes to
 * diagnostics whose source file matches it by basename (mirrors how optimizer
 * remarks are scoped to the main input so imported modules don't flood). */
void ir_explain_memory_set_collect(int enabled, const char *focus_file);

/* Record one emitted memory diagnostic. `severity` is 0 for a warning, 1 for
 * an error. `file` is the diagnostic's source file (may be NULL). `headline`
 * is the message; `fix` is the suggested remedy (may be NULL). Strings are
 * copied. No-op unless collection is enabled and the file passes the focus
 * filter. */
void ir_explain_memory_note(const char *file, int severity, size_t line,
                            const char *headline, const char *fix);

#endif /* IR_EXPLAIN_MEMORY_H */
