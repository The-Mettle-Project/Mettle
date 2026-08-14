# Checked access: `--safe`

Mettle's bounds and null checks exist only in debug builds. `--release` drops
them, which is why a program that reads past an array there returns whatever
happened to be in the next words rather than saying so. That is the ordinary
bargain in a systems language: you can have the checks or you can have the
speed.

`--safe` refuses the bargain. It checks every memory access at every
optimization level, and then spends the compiler's effort proving the checks
away rather than deleting them unproven. The question stops being "can we
afford to check" and becomes "how much of the checking can we prove is
unnecessary".

```bash
mettle --build --release --safe app.mettle -o app.exe
```

## What it catches

Everything below is caught at `--release`, with the optimizer on:

| | Example |
|---|---|
| Reading or writing past an array | `var a: int32[4]; a[i]` where `i` reaches 4 |
| A negative index | `a[-1]`, including through a computed index |
| Past the end of a heap block | `p = malloc(16); p[16]` |
| Use after free | `free(p); p[0]` |
| Use after free through another pointer | `q = p; free(q); p[0]` |
| A pointer kept across `realloc` | `q = realloc(p, n); p[0]` |
| Running off one allocation into the next | `p` and `q` adjacent, `p[distance_to_q]` |
| A null dereference | `p = 0; p[0]` |
| Past a stack local, through a pointer | `f(&a[0], i)` where `i` leaves `a` |
| Past a global, through a pointer | `f(&TABLE[0], i)` |

Two of those are worth separating out.

**Running off one allocation into the next** is what an address-only check gets
wrong. An access is bounded by *the allocation its pointer came from*, not by
whatever allocation the computed address happens to land in. So the check is
handed the base pointer and the displacement separately, and resolves the base.
A check that only asked "is this address inside something live" would wave it
through.

**Through a pointer** is the case that needs the runtime at all. Indexing a
local or a global directly never reaches it: the size is right there in the
program, so the check is a comparison against a constant, or is proved away.
It is only once a pointer into one is carried somewhere the size does not
travel with it that anything has to be looked up.

## What it costs

Measured against the same programs built without the flag, both timed back to
back and the median of the per-pair ratios taken:

| Benchmark | Overhead | Accesses | Settled at compile time | Checked at run time |
|---|---|---|---|---|
| dot_product | 1.00x | 34 | 8 | 26 |
| crc32 | 1.00x | 31 | 4 | 27 |
| word_count | 1.00x | 31 | 5 | 26 |
| transpose | 1.00x | 35 | 4 | 31 |
| base64_encode | 1.01x | 57 | 15 | 42 |
| binary_search | 1.08x | 33 | 5 | 28 |
| matvec | 1.54x | 37 | 6 | 31 |
| heapsort | 2.00x | 54 | 16 | 38 |

Only a quarter to a third of accesses are settled at compile time, and the
overhead is still mostly nothing at all. That is because the two halves work on
different things. Proving removes a check; where nothing can be proved, the
remaining job is to make the check cheap, and a check that walks the map to ask
which allocation a pointer belongs to costs around a hundred cycles while a
comparison against an answer already in a register costs about three.

The third thing that has to be true is that the checks do not cost the program
its ordinary code quality. A check is a call, and for a while any function
holding one lost register allocation entirely, because the backend defers a
function whose callee it cannot find a signature for and the runtime's entry
points were never declared. Declaring them took `--safe` from compiling the
whole program with the spill-everything backend to compiling essentially all of
it with the register allocator, which is most of what the figures above
changed. A checked build should differ from an unchecked one by its checks and
nothing else.

Being a call costs a second thing, and it is subtler. The allocator has to
assume a call happens, so every value live across one needs a register the call
would not clobber -- and there are seven of those. A nested loop carrying two
pointers, two counters, two resolved spans and two indices has spent them all
before it reaches the value it just loaded, which then goes to the stack on
every iteration; a float accumulator has nowhere to live at all, because no XMM
register survives a call on both calling conventions. So the two runtime calls
the checking machinery puts in loops give some registers back. The check saves
and restores RAX and the volatile XMM lanes around itself, which costs nothing
because it is entered only when the comparison in front of it fails. The span
resolution saves the XMM lanes, which costs eight instructions -- but the
compiler hoisted it in front of the loop, so that is once per loop against once
per element. Where the allocator ends up using none of the registers this hands
back, the saving is dropped again rather than paid for nothing. `transpose` and
`matvec` are the two this is measurable on: 1.5x and 2.9x before it.

So a loop that indexes one pointer resolves that allocation once in front of
itself, and each access inside becomes a subtract, a compare, and a branch that
is never taken. Failing the comparison is not a verdict: it calls the full
check, which is what keeps this exact for an interior pointer reading
backwards, for an allocation that has been freed, and for anything else the
comparison alone cannot judge. heapsort went from 15.3x to 2.5x on that change
alone, without a single extra access being proved.

dot_product is free for a different reason. A loop walking `a[i]` for `i` in
`[0, n)` touches one contiguous range, so one check covers what a check per
element was covering, and the loop body is then empty of calls again and the
vectorizer takes it back. Checks in a hot loop are not merely expensive: they
block the kernel that does the work.

That is why the whole-range check is worth reaching for even where the loop is
not a straight line. The body may branch, so long as it rejoins: an `if/else`
inside the loop does not change how many times the loop runs, so a check
covering the range the header test describes still describes what the loop will
touch. What it may not do is leave early. A loop that can `break`, or an access
the body reaches only on some iterations, keeps its per-access checks, because
one check for the whole range would claim iterations that never happened and
accuse a program that stayed in bounds. word_count is the shape this buys: a
byte loop whose body is a four-way comparison, back to the SIMD scan it had
without the flag.

heapsort is the honest end. Its sift-down indexes by `child` and `swap_idx`,
which no loop bounds, so the checks stay and land in a loop that is already
mostly compare-and-swap.

`--explain` reports where a program sits:

```
-- memory safety: base64_encode.mettle ----------
  57 accesses, 15 settled at compile time (26%), 42 checked at run time
  4 proved in place, 11 folded into a check covering a whole loop
  3 compare against a known extent, 23 against an allocation the loop resolves
  once, 16 ask the runtime which allocation the pointer came from
  line 30 in base64_encode: the object's size is not known here, so the runtime
  is asked which allocation the pointer came from
      30 | var c0: int32 = (int32)(uint8)src[i];
```

Every access lands in exactly one of those buckets and they add up, so a run
that looks wrong can be read rather than guessed at.

`METTLE_SAFETY_TRACE=1` prints why each proof gave up, which from the outside
is otherwise indistinguishable from a limit of the analysis.

## How the checks go away

Five arguments, tried in order. Each is a claim that the access can never leave
its object, and a wrong claim is a miscompile that reads as a safe program, so
anything that cannot be pinned down exactly leaves the check alone.

**A constant index.** `a[3]` against a fixed-size object. This needs a short
walk back through the instructions, because lowering scales every subscript
through a multiply into a temporary, so even `a[3]` arrives as a temporary
rather than as the twelve it obviously is.

**A counted loop over a fixed-size object.** `for i in 0..8` over `int32[8]`
reaches at most offset 28, and the object is 32 bytes. The index has to start
at zero and step by a constant, nothing else in the body may move it, and the
bound has to be a constant.

**An index its own arithmetic bounds.** `alpha[(bits >> 2) & 63]` cannot leave
`[0, 63]` whatever `bits` holds, because a non-negative mask clears every
higher bit including the sign. That is the shape of every table lookup.

**One check for a loop's whole range.** Where the object's size is not known,
a counted loop still touches one contiguous range, so the per-element checks
become a single check in front of the loop. The range must be exactly what the
loop touches: too large accuses a correct program, too small misses a real
overrun. That is why the body has to be straight line, since a conditional
access touches a subset, and why it must contain no calls, since one of them
could free the block partway through.

The loop shapes this reads are wider than the simplest one. The test may carry
arithmetic (`while (i + 3 <= len)`), the index may step by more than one, and
the variable an access indexes by need not be the one the test bounds: a loop
reading three bytes and writing four advances two counters, and the second is
pinned to the first by both starting at zero and both stepping by a constant.

**One check for a bounded index.** A masked index reaches the same range every
iteration, so where the object's size is unknown the check still lifts out of
the loop, with a constant length.

What none of those settle still gets checked, but against an allocation the
enclosing loop resolved once rather than by asking the runtime each time. That
applies wherever the loop cannot release what it is walking and the pointer
either holds still or is a fixed one displaced, which covers most indexing
even when nothing about the index itself can be argued.

## The shape of the implementation

The compiler marks every access during lowering and resolves the marks
immediately afterwards, before the optimizer runs. Nothing downstream ever sees
a safety opcode: the optimizer, the interpreter and all three code generators
work on ordinary IR.

Running before the optimizer is deliberate. Loops still have the canonical
shape the bound proofs read most easily, and a foreign opcode drifting through
a pass schedule full of exact-shape recognizers would be silently mishandled.
The cost is that a check which would become provable only after inlining stays.

There is a related hazard worth naming, because it bit during development. A
loop recognizer scans a body for the pattern it knows, ignores what it does
not, and then replaces the whole body. Handed a loop containing a check, it
matched anyway and erased the check along with everything else, so the mode was
silently absent in exactly the hot loops it exists to cover. Recognizers now
refuse a body holding safety bookkeeping, and one fixture per recognizer family
holds them to it.

At run time, a map from address to owning allocation answers what survives. It
is a three-level table over the address space holding one region id per
16-byte granule, and ids index descriptors carrying each allocation's start and
length. Heap blocks are described as they are allocated, globals once at the
top of `main`, and a stack local for as long as its frame is alive, though only
where a pointer to it actually leaves: every indexed array has its address
taken, and describing on that alone would charge two calls per call to
functions that never needed it.

A described local is also aligned and padded to the map's 16-byte resolution,
because two objects sharing one of those units cannot both be described and the
runtime refuses to guess between them. Without that, the access most worth
catching is the one that goes uncovered: an overrun of a few bytes lands
exactly in the unit an object shares with its neighbour. Freeing does not clear the granules; it marks the descriptor dead and
leaves them naming it, which is what lets a pointer kept across the free be
reported as use-after-free rather than read back as untracked memory. The
descriptor is reclaimed once a later allocation has taken every granule it
held.

Pointers stay ordinary machine pointers. The ABI, struct layouts and every
foreign call are exactly as they were.

## What it does not catch

**Memory Mettle did not allocate.** Anything the runtime was never told about
reads as unowned and is allowed through. A foreign library's pointer is not
something the runtime can judge, and trapping on it would reject correct
programs, which is the one thing this design refuses to do.

**A pointer walked clear of every allocation.** This is the same rule biting a
pointer that did start out valid. `p = p + n` far enough, and `p` no longer
lands inside anything the runtime knows, so the access reads as untracked
rather than as an overrun. What carries provenance here is the pointer's
value, and a value outside every live region carries none: the runtime cannot
tell which allocation it should have belonged to. Indexing (`p[i]`) is
unaffected, since the base is still the pointer the allocation handed out and
only the displacement moves. Catching the walked-off case needs bounds
travelling with the pointer, which is the fat-pointer design this one avoids
in order to leave the ABI alone.



**The allocator itself.** An allocator writes a header below the pointer it
returns, threads its free list through the bodies of released blocks, and
poisons them on the way out. Against the model those read as an overrun and a
use-after-free, and they are neither, so the module defining the heap entry
points is not checked and calls into it are bracketed so work done on its
behalf is skipped as well.

**Reuse.** Once a freed block has been handed out again, a stale pointer to it
resolves to the new owner and is bounds-checked against that. No scheme without
a quarantine can do better, and how long reuse is delayed is the allocator's
policy rather than the checker's.

**Anything that is not a memory access.** Integer overflow, uninitialized
reads, and data races are all out of scope. The compile-time memory analyzer
([docs/borrow-checker.md](borrow-checker.md)) covers some of that ground
statically and reports leaks, which nothing here does.

## Trying it

```bash
mettle --build --release --safe tests/test_safe_use_after_free.mettle -o uaf.exe
./uaf.exe
```

```
Fatal error: use of memory after it was freed: 1 bytes at offset 0 of a 16 byte
allocation (line 10)
```

The fixtures under `tests/test_safe_*.mettle` cover both directions. Each bad
program is built with and without the flag, so the trap is shown to come from
the check rather than from some unrelated change, and each clean program is
required to return the same answer either way. Several are sized so that a
range one byte too large would reject them.
