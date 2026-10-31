# Live programs

A Mettle program should never have to stop to change. Code, data layout,
representation, the past, and a whole fleet of machines are all things a
running program can take in at a point it names. A crash is a place to stop,
fix, and continue from. This document is the plan for getting there and the
architecture of the first part.

## The ladder

| Level | What changes while the program runs | Milestone |
|---|---|---|
| 1 | Any function body, with no annotation, in an optimized build | M1 |
| 2 | Function signatures; every caller is recompiled into the same patch | M2 |
| 3 | Struct layouts; every live object is found and migrated | M3 |
| 4 | Functions that are on the stack right now, the main loop included | M4 |
| 5 | Representations: an array becomes a map, an array of structs becomes a struct of arrays | M5 |
| 6 | The past: recorded inputs are replayed through the new code and the outcomes are compared | M6 |
| 7 | Speculation: the new code runs on a copy of the state first, then commits or is thrown away | M6 |
| 8 | Crashes: a fault rewinds to the last snapshot and waits for a patch, then continues | M7 |
| 9 | Timelines: one snapshot, K variants of the code, all run, all compared | M7 |
| 10 | A fleet: clients, servers and GPUs swap together at one logical tick | M8 |

Every level keeps the rule from the runtime model: nothing is applied anywhere
except a `quiesce;` the program wrote.

## M1: live builds

`mettle live app.mettle` builds the program, starts it, and watches its
sources. Each save is compiled into a patch, and the running program applies
it at its next `quiesce;`. Nothing in the source is annotated.

### Every function has a slot

A build with `--live` gives every function `f` three symbols:

- `f` is an eight-byte thunk, `jmp [rip + f$slot]`.
- `f$body` is the compiled function.
- `f$slot` is a pointer in `.data`, initialized to `f$body`.

Every call, every `&f`, and every function pointer stored in data resolves to
the thunk, so a swap is one store into `f$slot`. Inlining stays on: a caller
that inlined `f` has different machine code when `f` changes, and the patch
builder sees that difference directly.

### The state file

Linking a live build also writes `app.exe.live`. It records the image base,
the relative address of every defined symbol in the final image, the size of
every global, a hash of every function, the address and size of the spare
arena that new globals are placed in, and the spare thunks that new functions
are given.

A function's hash covers its bytes and every relocation in it. A relocation
is hashed by what it points at: a function or global by name, read-only data
by its content, followed through any relocations inside that content. String
literals are renumbered when unrelated code changes, so hashing them by name
would mark every function that prints anything as changed.

### Building a patch

The patch builder compiles the whole program with the flags the host was
built with, reads the object, and hashes every function the same way. A
function goes into the patch when its hash differs from the live one or it is
new. Everything else the patch refers to resolves to the running image:
functions to their thunks, globals to their storage, imports to their import
address table entries. Read-only data the patch refers to is copied into it.

A new function takes a spare thunk and slot. A new global takes space in the
arena, and its initial bytes travel in the patch. The state file is updated so
the next patch sees the new world.

### Refusals

A patch that the running program cannot take is refused at build time, and the
program keeps running the code it has:

- a global changed size, or a struct used by live state changed layout (M3
  lifts this)
- a function signature changed while a frame that keeps its old code may
  still call it the old way (M4 narrowed this to frames that cannot move)
- the new code needs an import or a runtime component the program never linked
- new thread-local storage
- the spare thunks or the arena are exhausted

### The patch file

```text
"MTLPATCH" version flags
anchor_rva blob_size fixup_count swap_count init_count
blob                      code and read-only data, position independent
fixups   site, kind, target, addend
swaps    slot_rva, body_offset
inits    arena_rva, blob_offset, size
```

A fixup's site is in the blob or in the host image, and its target is in the
blob or in the host image. Kinds are a 64-bit absolute address and a 32-bit
displacement from the end of the field.

### Loading

The loader is written in Mettle (`src/runtime/live.mettle`). `quiesce;` in a
live build calls `mettle_live_quiesce`, which at most every 50 ms looks for the
next patch file, and when it finds one:

1. finds the host image base from `mettle_live_anchor`, whose relative address
   the patch carries;
2. allocates the blob within 2 GB of the image, so 32-bit displacements reach;
3. copies the blob, applies the fixups, copies new globals into the arena, and
   makes the blob read and execute;
4. stages one swap per changed function;
5. applies every staged swap, which is the same pointer store as before.

The old code is never freed. A thread inside an old body finishes there.

## M4: frames move at quiesce

A frame of the function that holds a `quiesce;` moves to the new code at that
point. It is done in the IR, before optimization, so the new code is ordinary
optimized code and no stack maps are needed.

### Sites

In a live build, `quiesce;` in a function `f` lowers to

```text
%flag <- &@f$live_frame
%moved = mettle_live_frame_check(%flag, "f")
branch_zero %moved -> resume
  transfer path
label resume
```

`mettle_live_frame_check` applies pending patches, then reports whether a
patch asked `f` to move, and clears the request. `f$live_frame` is a global
the patch sets through an init.

All cross-statement state in lowered IR lives in named locals (loop
counters, `for` indices, defer captures), so the named locals are the whole
state of the frame at a quiesce point. Each local gets a transfer name: its
source name, with the lowering's numeric suffixes replaced by an ordinal
(`ir_defer_cap#1`, `x#2` for a shadowing `x`), and a key that hashes its type
layout.

The transfer path hands every local declared before the site to the runtime
by address: a scalar through a copy in a `.live` local, so the variable itself
stays in a register; an aggregate, or anything whose address the function
takes, through its own storage. Then it calls `f$resume_k` and returns its
result. It runs no defers; the continuation owns the rest of the function.

### Continuations

`f$resume_k` is generated per site from `f`'s IR. Its entry takes each local
back by name (`mettle_live_frame_take`), and jumps to the point after site
`k`. Entering a loop in the middle would make the control flow irreducible,
which the optimizer's loop passes do not expect. The continuation avoids it
with one copy of the function per enclosing loop level plus one:

- copy `D`, for `D` enclosing loops numbered from the outside, is entered at
  the site, and a back edge of enclosing loop `j` leaves it for copy `j`;
- copy `j` is entered at the header of loop `j` through a landing label that
  falls into it, the same shape a preheader has. Loop `j` and the loops inside
  it keep their back edges, and a back edge of an outer loop `i` leaves for
  copy `i`;
- copy `0` is the whole function in steady state.

Unreachable blocks are dropped. What remains is reducible, every loop is
entered through its header, and the code size is about the loop nest depth
times the function.

A local that the continuation reaches only through its address adopts the
old frame's storage, so pointers the old code saved stay valid. The
continuation declares no storage for it; a local that is new gets zeroed heap
memory. The old frame stays on the stack under the continuation, which is
what keeps adopted storage alive.

The continuation itself has sites, so a later patch moves it again.

### Patches

Continuations are ordinary live functions with thunks and slots, so the diff
treats them like any other. When a continuation of `f` is in a patch, the
patch sets `f$live_frame`, provided the running program has the flag, the
same number of sites, and the same signature for `f`. Otherwise the frame
keeps its old code and the summary says so.

The signature refusal now asks which frames may keep running old code: a
quiesce holder that will not move, or any function that calls into a holder
of another family (it is on the stack while that holder applies the patch).
Only those are checked for calls to a function whose signature changed. A
`main` loop that moves calls the new signature from the new code.

### Runtime

| Call | Does |
|---|---|
| `mettle_live_frame_check(flag, owner)` | apply patches; if the flag is set, clear it and start a record |
| `mettle_live_frame_put(name, key, size, place)` | record where a local's value is |
| `mettle_live_frame_take(name, key, size)` | the place for a matching name and key, or 0 |
| `mettle_live_frame_alloc(size)` | zeroed storage for a new adopted local |

A name that is missing or whose key changed prints one line and the local
starts from its constant initializer, or zero.

## M2 to M8

- M2, signatures: a changed signature puts every caller in the patch. A frame
  on the stack with the old signature keeps running old code; the refusal
  names it if it could ever return into new code.
- M3, layouts: live builds emit a traversal for every type from the declared
  roots. A changed layout runs a generated migration at `quiesce;`, and every
  pointer into moved memory is rewritten. Types reachable only through
  `rawptr` or integers are refused by name.
- M4, the stack: built; see above. Next is moving frames that are on the
  stack without holding a quiesce point, by giving every call site in a live
  build the same transfer path.
- M5, representations: a `migrate fn` from the old type to the new one, run
  once at the swap, with pointers rewritten as in M3.
- M6, time: the allocator tracks dirty pages, so a snapshot every frame is
  cheap. The sources of nondeterminism (clock, input, network, random numbers)
  are recorded. A patch can be replayed against the last N seconds and the
  outcomes compared, or run on a copy of the state for N frames before it is
  committed. Effects decide which calls reach the outside world during a
  speculative run.
- M7, crashes and timelines: the crash handler rewinds to the last snapshot
  and waits for a patch. One snapshot can fan out into K forks, each with a
  different patch, and the results are compared side by side.
- M8, fleets: patches carry a logical tick. Generated wire formats carry their
  own migrations, so old and new nodes can talk during a rollout.
