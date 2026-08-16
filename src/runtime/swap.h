#ifndef METTLE_RUNTIME_SWAP_H
#define METTLE_RUNTIME_SWAP_H

/* Runtime half of code swapping.
 *
 * A swap replaces a function in a running process. The hard half is not the
 * code, it is deciding *when*: a replacement that lands while the old body is
 * mid-execution, or between two operations that were meant to be one, is a
 * miscompile that happens to occur at run time.
 *
 * Mettle answers that by refusing to choose. The program names the moment with
 * `quiesce;`, and nothing is applied anywhere else. Staging a swap records an
 * intent; the intent takes effect at the next point the programmer wrote, and
 * at no other. That is the same rule the rest of the language follows: nothing
 * runs at a point you did not author.
 *
 * The binding is a slot, not a patched instruction. A swappable call reads a
 * function pointer from a slot and calls through it, so applying a swap is a
 * single pointer-sized store. Two consequences matter:
 *
 *  - It is sound without stopping the world. A thread already inside the old
 *    body keeps running the old body, which is still resident; only later
 *    calls see the new one. Rewriting instructions under a running thread
 *    would need every other thread halted, and halting them is exactly the
 *    unauthored control flow this design refuses.
 *  - Nothing is executable-page-writable. The process never needs W^X toggled,
 *    because no code is ever modified.
 *
 * The cost is one indirection per call, paid only by functions that opted in,
 * and a program that stages nothing never references these symbols and does
 * not link this file.
 */

#include <stddef.h>

/* Record that `slot` should point at `replacement` from the next quiesce
 * point onward. Staging the same slot twice keeps the later replacement.
 * Returns 1 when recorded, 0 when the pending table is full. */
int mettle_swap_stage(void **slot, void *replacement);

/* Apply every staged swap. This is what `quiesce;` lowers to, so it runs only
 * where the program said it may. Returns how many slots were rebound. */
long long mettle_swap_apply(void);

/* How many swaps are staged and not yet applied. */
long long mettle_swap_pending(void);

/* Drop staged swaps without applying them. */
void mettle_swap_discard(void);

#endif /* METTLE_RUNTIME_SWAP_H */
