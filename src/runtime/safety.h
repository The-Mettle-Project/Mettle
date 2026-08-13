#ifndef METTLE_RUNTIME_SAFETY_H
#define METTLE_RUNTIME_SAFETY_H

/* Runtime half of Mettle's checked-access memory safety.
 *
 * The compiler emits a check at every access it cannot prove safe, and the
 * optimizer deletes the ones it can prove. What survives lands here. The
 * runtime answers one question exactly: does the byte range this access wants
 * lie inside the live allocation the pointer came from?
 *
 * Answering it needs a map from an address to the allocation that owns it.
 * That map is a three-level table over the address space, one 32-bit region id
 * per 16-byte granule. Region id zero means no live allocation owns the
 * granule, so a freed pointer and a wild pointer both fail the same way. Ids
 * index a descriptor array holding the allocation's start and length, which is
 * what makes the bounds answer exact rather than approximate: an access that
 * runs off one live allocation into the next is still a violation, because the
 * check compares against the descriptor the BASE pointer resolved to, not
 * against whatever the final address happens to land in.
 *
 * The shadow map costs a quarter of the address space it covers, and it is
 * paid only for memory the program actually registers. That is the price of an
 * exact answer with no allocator surgery and no change to pointer layout: a
 * Mettle pointer under --safe is an ordinary machine pointer, so the ABI, the
 * struct layouts and every foreign call stay exactly as they were.
 *
 * Memory the program never registers reads as unowned, and an access to it is
 * allowed rather than trapped. Foreign libraries hand back pointers Mettle did
 * not allocate and cannot describe; trapping on them would reject correct
 * programs, which is the one thing this design refuses to do. Coverage is
 * therefore complete for memory Mettle owns and silent elsewhere.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of address space described by one shadow entry. Allocations are
 * registered at granule resolution, so two live allocations never share a
 * granule; the allocator's own alignment guarantees it. */
#define METTLE_SAFETY_GRANULE 16u

/* What kind of access failed, reported in the trap message. */
typedef enum {
  METTLE_SAFETY_ACCESS_READ = 0,
  METTLE_SAFETY_ACCESS_WRITE = 1
} MettleSafetyAccessKind;

/* Record `size` bytes at `pointer` as one live allocation. A null pointer or a
 * zero size is ignored. Re-registering an address that is already live
 * replaces the old record, which is what an allocator reusing a block wants. */
void mettle_safety_register(void *pointer, uint64_t size);

/* Retire the allocation starting exactly at `pointer`. Every granule it
 * covered becomes unowned, so a pointer kept across the free traps on its next
 * use. Retiring an address that is not a live allocation start is ignored:
 * freeing foreign memory is not an error the runtime can judge. */
void mettle_safety_unregister(void *pointer);

/* Retire `old_pointer` and register `size` bytes at `new_pointer` as one step,
 * for a reallocation that may or may not have moved the block. */
void mettle_safety_reregister(void *old_pointer, void *new_pointer,
                              uint64_t size);

/* The check itself. `base` is the pointer the access derives from and carries
 * the provenance; `offset` is the signed byte displacement applied to it and
 * `size` the number of bytes touched. Returns when the access is inside the
 * allocation that owns `base`, and does not return otherwise.
 *
 * `what` names the access in the trap message and must outlive the call; the
 * compiler passes a string literal. `line` is the source line of the access. */
void mettle_safety_check(const void *base, int64_t offset, uint64_t size,
                         uint32_t access_kind, const char *what, uint32_t line);

/* Counts for the end-of-run summary and for tests: how many checks ran, and
 * how many allocations are live. Both are advisory and lock-free. */
uint64_t mettle_safety_check_count(void);
uint64_t mettle_safety_live_region_count(void);

/* Distinct descriptor slots ever handed out. A program that allocates and
 * frees in a loop must hold this steady rather than let it climb, which is the
 * observable form of "freed descriptors get recycled once their memory is
 * handed to someone else". */
uint64_t mettle_safety_descriptor_high_water(void);

/* Release every table the shadow map allocated. Only for tests that want to
 * measure a clean run; a program that simply exits does not need it. */
void mettle_safety_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* METTLE_RUNTIME_SAFETY_H */
