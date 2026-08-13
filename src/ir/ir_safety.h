#ifndef IR_SAFETY_H
#define IR_SAFETY_H

/* `--safe`: turning unproven accesses into either nothing or a check.
 *
 * Lowering marks every memory access with an IR_OP_SAFETY_CHECK. This step
 * runs immediately afterwards, before the optimizer, and leaves none of them
 * behind: a check it can prove cannot fail is deleted, and one it cannot is
 * rewritten into ordinary IR, either a comparison against a constant extent or
 * a call into the runtime shadow map.
 *
 * Running before the optimizer is deliberate. Loops still have the canonical
 * shape the bound proofs read most easily, and every later pass, recognizer,
 * code generator and the interpreter see only ordinary IR. The cost is that a
 * check which would become provable after inlining stays; that is a smaller
 * loss than it sounds, because the proofs that matter (a constant index, an
 * induction variable the loop already bounds, a repeat of an access proved
 * earlier) are all available at this point.
 */

#include "ir.h"
#include <stddef.h>

typedef struct {
  size_t emitted;      /* accesses lowering marked */
  size_t proved;       /* deleted: the access cannot be out of bounds */
  size_t exempt;       /* dropped: inside the allocator, which is not checked */
  size_t extent_tests; /* survivors that compile to a compare and branch */
  size_t region_calls; /* survivors that have to ask the runtime */
} IRSafetyStats;

/* Resolve every check in the program. Returns zero only on allocation
 * failure, having left the program unchanged. `stats` may be NULL. */
int ir_safety_resolve_program(IRProgram *program, IRSafetyStats *stats);

/* Tell the runtime where the heap is: register what each allocation call
 * returns, and retire it before the matching free. Without this the shadow map
 * describes nothing, every region check finds unowned memory, and pointer
 * accesses pass unexamined.
 *
 * Runs after --native-heap has chosen the allocator, so it sees whichever
 * names the calls ended up with, and before the optimizer, so the bookkeeping
 * inlines and moves like any other call. Returns zero only on allocation
 * failure. */
int ir_safety_register_allocations(IRProgram *program);

#endif /* IR_SAFETY_H */
