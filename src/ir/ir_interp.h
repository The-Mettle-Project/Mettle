#ifndef IR_INTERP_H
#define IR_INTERP_H

/* In-compiler reference interpreter for optimized IR.
 *
 * This is the semantic arbiter behind --verify translation validation: it
 * executes an IRFunction directly over the in-memory instruction array with a
 * bounds-checked synthetic address space, and its behavior for every opcode -
 * including the SIMD kernel ops - is the documented scalar semantics from
 * ir.h. Two runs of the same machine configuration are bit-deterministic, so
 * "interpret the function before pass P and after pass P on identical inputs
 * and compare observations" is a sound equivalence check: any divergence means
 * the pass changed observable behavior.
 *
 * Observables: the return value, the final bytes of every registered buffer,
 * the ordered trace of extern calls (unknown externs are modeled as pure
 * "return 0" but traced, so deleting or reordering one is still visible), and
 * the final values of touched globals.
 */

#include "ir.h"
#include <stdint.h>

typedef enum {
  IR_INTERP_OK = 0,
  IR_INTERP_UNSUPPORTED, /* opcode/type outside the interpretable subset */
  IR_INTERP_TRAP,        /* div by zero, out-of-bounds, use-after-free */
  IR_INTERP_FUEL,        /* step budget exhausted */
  IR_INTERP_DEPTH,       /* call depth cap exceeded */
  /* The program called its own runtime guard (mettle_crash_trap*): a clean,
   * deliberate abort. Two runs that both guard-trap are considered
   * equivalent regardless of the exact crash point, matching how debug
   * checks behave under optimization. */
  IR_INTERP_GUARD_TRAP
} IRInterpStatus;

typedef struct {
  long long i;
  double f;
  int is_float;
} IRInterpValue;

typedef struct {
  char name[64];
  IRInterpValue args[8];
  size_t arg_count;
} IRInterpExternCall;

typedef struct IRInterpMachine IRInterpMachine;

IRInterpMachine *ir_interp_create(IRProgram *program);
void ir_interp_destroy(IRInterpMachine *machine);

/* Route calls to `name` to `fn` instead of the program's copy (used to run a
 * self-recursive function's BEFORE snapshot against itself). */
void ir_interp_set_override(IRInterpMachine *machine, const char *name,
                            IRFunction *fn);

/* Register an input buffer; returns its synthetic address (or 0 on failure).
 * `init` may be NULL for a zeroed buffer. Buffers registered in the same
 * order with the same contents give two machines an identical address space. */
unsigned long long ir_interp_add_buffer(IRInterpMachine *machine,
                                        const void *init, long long size);

IRInterpStatus ir_interp_run(IRInterpMachine *machine, IRFunction *function,
                             const IRInterpValue *args, size_t arg_count,
                             IRInterpValue *result, long long fuel);

/* Observation accessors (valid after ir_interp_run). */
size_t ir_interp_buffer_count(const IRInterpMachine *machine);
const unsigned char *ir_interp_buffer_data(const IRInterpMachine *machine,
                                           size_t index, long long *size);
size_t ir_interp_extern_trace_count(const IRInterpMachine *machine);
const IRInterpExternCall *ir_interp_extern_trace(const IRInterpMachine *machine,
                                                 size_t index);
/* Iterate final global values: returns count; name/value for index. */
size_t ir_interp_global_count(const IRInterpMachine *machine);
const char *ir_interp_global_name(const IRInterpMachine *machine, size_t index);
IRInterpValue ir_interp_global_value(const IRInterpMachine *machine,
                                     size_t index);

/* Human-readable reason for the last non-OK status ("call_indirect",
 * "extern trace overflow", "local type 'string'", ...). */
const char *ir_interp_status_detail(const IRInterpMachine *machine);

#endif /* IR_INTERP_H */
