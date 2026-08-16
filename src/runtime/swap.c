/* Staged code swaps, applied only at a quiesce point.
 * See swap.h for why the binding is a slot rather than a patched instruction. */

#include "swap.h"

/* A fixed table rather than a growing one: this allocates nothing, so a swap
 * cannot fail for want of memory at the moment a program is trying to replace
 * the code that was going wrong. Staging past the limit is refused and said
 * so, which is a better failure than an allocation inside a quiesce point. */
#define METTLE_SWAP_MAX_PENDING 256

typedef struct {
  void **slot;
  void *replacement;
} MettleSwapEntry;

static MettleSwapEntry g_pending[METTLE_SWAP_MAX_PENDING];
static long long g_pending_count = 0;

int mettle_swap_stage(void **slot, void *replacement) {
  if (!slot) {
    return 0;
  }
  /* Restaging a slot replaces the earlier intent. Applying both in order
   * would be the same end state, but it would burn table space and make the
   * pending count disagree with the number of slots that will move. */
  for (long long i = 0; i < g_pending_count; i++) {
    if (g_pending[i].slot == slot) {
      g_pending[i].replacement = replacement;
      return 1;
    }
  }
  if (g_pending_count >= METTLE_SWAP_MAX_PENDING) {
    return 0;
  }
  g_pending[g_pending_count].slot = slot;
  g_pending[g_pending_count].replacement = replacement;
  g_pending_count++;
  return 1;
}

long long mettle_swap_apply(void) {
  long long applied = g_pending_count;
  /* Each store is pointer-sized and aligned, so a caller already inside the
   * old body finishes in the old body and the next call takes the new one.
   * There is no window in which a slot holds anything other than one of the
   * two functions. */
  for (long long i = 0; i < g_pending_count; i++) {
    *g_pending[i].slot = g_pending[i].replacement;
  }
  g_pending_count = 0;
  return applied;
}

long long mettle_swap_pending(void) { return g_pending_count; }

void mettle_swap_discard(void) { g_pending_count = 0; }
