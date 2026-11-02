# Live programs

A Mettle program should never have to stop to change. Code, data layout,
representation, the past, and a whole fleet of machines are all things a
running program can take in at a point it names. A crash is a place to stop,
fix, and continue from. This document is the plan for getting there and the
architecture of the parts that are built.

## The ladder

1. Any function body, with no annotation, in an optimized build. **M1, built.**
2. Function signatures; callers are recompiled into the same patch. **M2,
   built.**
3. Layouts: module-scope variables, frame locals and heap objects convert to
   their new types. **M3, built.**
4. Every function on the stack, the main loop included. **M4, built.**
5. Representations: an array becomes a map, an array of structs becomes a
   struct of arrays. M5.
6. The past: recorded inputs replayed through the new code, outcomes compared.
   M6.
7. Speculation: the new code runs on a copy of the state first, then commits
   or is thrown away. M6.
8. Crashes: a fault waits for a patch, then continues from the last quiesce
   point. **M7, built.** Rewinding to a snapshot is next.
9. Timelines: one snapshot, K variants of the code, all run, all compared. M7.
10. A fleet: clients, servers and GPUs swap together at one logical tick. M8.

Every level keeps the rule from the runtime model: nothing is applied anywhere
except a `quiesce;` the program wrote.

```mermaid
flowchart LR
    save[save a .mettle file] --> compile[compile the whole program<br/>with the host's flags]
    compile --> diff[hash every function<br/>against app.exe.live]
    diff --> patch[write NNNNNN.mpatch]
    patch --> poll[running program polls<br/>at quiesce]
    poll --> load[place blob within 2 GB<br/>fix addresses, bind imports]
    load --> stop[stop other threads]
    stop --> convert[convert variables<br/>to new layouts]
    convert --> swap[jump over each<br/>replaced entry]
    swap --> resume[resume threads]
    resume --> move[frames move at their<br/>next site]
```

## M1: live builds

`mettle live app.mettle` builds the program, starts it, and watches every file
the compile read, imports and stdlib included (`app.exe.live.files`). Each save
is compiled into a patch, and the running program applies it at its next
`quiesce;`.

### Function entries

In a build with `--live`, every function `f` is laid out as

```text
offset  bytes
-16     signature hash of f's parameters and result (8 bytes)
 -8     f, f$body: 0F 1F 84 00 00 00 00 00   eight-byte no-op
  0     the compiled code, aligned to 16 or 32 as in any build
```

Every call, every `&f`, and every function pointer stored in data resolves to
`f`, so calls are direct and pay one no-op. A patch replaces `f` by storing
`E9 rel32` over the first five bytes of the no-op with one aligned eight-byte
write. The code after the entry keeps its alignment, so loop alignment inside
it is unchanged.

Functions the patch creates have no entry in the host. They take one of 4096
spare slots in the host's `.data`, and patch code calls them through a
blob-local `jmp [rip + slot]`. A later patch that replaces one of them stores
into the slot.

A live build runs within 0.4% of a normal build: geometric mean over eight
Suite 3 programs at `-O`, against 3.5% when every call went through a thunk
and a slot. Live builds also keep `var` globals in memory: the read-only
global fold is off, so a variable the program never assigns still takes the
value the running program has.

### The state file

Linking a live build writes `app.exe.live`: the image base, the relative
address of every defined symbol in the final image, the size of every global,
the layout of every module-scope variable, a hash of every function, the
address and size of the spare arena that new globals are placed in, and the
spare slots. Patches append to it, and each patch saves a snapshot of the
state before it, for undo.

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
functions to their entries, globals to their storage, imports to their import
address table entries. Read-only data the patch refers to is copied into it,
deduplicated by content so every copy of a table in one patch is the same
address.

The builder indexes symbols by name and each section's relocations by offset,
so hashing and copying stay linear in the size of the object. Warriors of
Servitude (24,000 lines, a 1 MB `main` with its continuation) patches in
10 s, 6 of them the compile; an unchanged save reports "No function changed"
in 9 s.

A new function takes a spare slot. A new global takes space in the arena, and
its initial bytes travel in the patch. A function whose line rows the host
recorded gets rows in the patch, so crash traces name patched code.

### Refusals

A patch that the running program cannot take is refused at build time, and the
program keeps running the code it has:

- a function signature changed while a frame that keeps its old code may
  still call it the old way
- the new code needs a runtime component the program never linked
- the new code uses a kind of relocation the loader cannot apply
- the spare slots or the arena are exhausted

### The patch file

```text
offset  field
 0      "MTLPATCH"
 8      version 2
12      patch index
16      anchor_rva           relative address of mettle_live_anchor
24      blob_size
32      fixup count          24 bytes each: site, kind, target, addend
36      swap count           16 bytes each: target rva, body offset
40      init count           24 bytes each: arena rva, blob offset, size
44      function count
48      debug functions offset, 52 count
56      debug locations offset, 60 count
64      import count, 68 import table size
72      undo target          nonzero: undo that patch, nothing else
76      migration count      32 bytes each, see M3
80      blob, fixups, swaps, inits, imports, migrations
```

A fixup's site is in the blob or in the host image (`0x100`), and its target
is in the blob or the host image (`0x200`). Kinds are a 64-bit absolute
address, a 32-bit displacement from the end of the field, and a 32-bit
absolute address (Linux non-PIE). Kind flag `0x400` marks a store into host
data that undo must restore; those apply after the swaps.

A swap whose target rva has bit 63 set is a function entry: the loader writes
a jump there. Otherwise it is a slot.

### Loading

The loader is written in Mettle: `src/runtime/live.mettle` is portable, and
`live_os_windows.mettle` and `live_os_linux.mettle` implement one port: write,
lock, unlock, ticks, alloc, free, alloc near, seal, code store, exe path, read,
bind, suspend others, resume others, thread id and sleep, plus three naked
functions for M7 (the site trampoline, the leave hook and the resume jump). `quiesce;` in a live build
calls `mettle_live_quiesce`, which at most every 50 ms looks for the next
patch file, and when it finds one:

1. finds the host image base from `mettle_live_anchor`;
2. allocates the blob within 2 GB of the image, so 32-bit displacements reach;
3. copies the blob, copies new globals into the arena, applies the fixups,
   binds imports, and makes the blob read and execute;
4. registers the blob's debug rows with the crash handler;
5. stops every other thread (Windows), converts migrated variables, writes the
   entry jumps and slots, stores the frame tables, and resumes the threads.

Every store in step 5 is logged with the value it replaced. The old code is
never freed. A thread inside an old body finishes there.

## M2: signatures

A changed signature puts every caller in the patch. The refusal asks which
frames may keep running old code: a function that holds a quiesce point and
cannot move, or a function on the stack while another family's quiesce point
applies the patch. Only those are checked for calls to a function whose
signature changed.

## M3: layouts

Lowering records a layout descriptor for every module-scope variable and for
every local a frame carries:

```text
i<n> u<n> b<n> e<n> f<n>     integer, unsigned, bool, enum, float of n bytes
p8 s16                       pointer, string
A<count>[elem]               array
S<size>{name:offset:type;…}  struct
x<n>                         anything else, n opaque bytes
```

The state file keeps the descriptor of every variable (`t` lines). When the
patch builder resolves a reference to a variable whose descriptor changed, it
puts both descriptors in the blob and adds a migration entry (old rva, new
rva, size, old descriptor, new descriptor). The variable converts in place
when the new layout fits the room the host left after it (see below);
otherwise it moves to new storage in the arena, and every function that
refers to it joins the patch, so no code is left reading the old storage.

The loader zeroes the new storage and converts with every other thread
stopped. Conversion walks the new descriptor: struct fields match by name at
any depth, arrays convert the elements both have, integers and floats convert
by value, a bool takes nonzero as true, and pointers, strings and opaque bytes
copy when kind and size match. Anything else stays zero. Undo converts the
current value back into the old storage the same way.

Frames use the same conversion. `mettle_live_frame_put` records each local's
descriptor with its value, and `mettle_live_frame_take` converts into the new
local when the descriptors differ.

Heap objects convert in place. In a live build, lowering turns `new T` and
`new T[n]` of a struct type into `mettle_live_new(bytes, element size, count,
name, descriptor)`. The runtime reserves 64 GB of address space (top-down on
Windows, so the 2 GB window around the image stays free for patch code) and
commits it in 64 KB pages. Each page holds blocks of one size class, from 32
bytes to 64 KB, and each block starts with a 16-byte header: a pointer to the
type record (name and descriptor) and the element count. An object gets
`bytes + bytes / 4 + 16` bytes of room. Freed blocks go on a free list per
class. The owned allocator's `free` and `realloc` call into the live runtime
first, which claims any pointer inside the arena with a single range check;
objects too large for a class fall back to `calloc` and an address table.

Lowering records each tracked type's descriptor; the state file keeps them
(`y` lines), and a patch whose type descriptor differs carries a type
migration (kind 1 in the migration table: name, old descriptor, new
descriptor). The loader takes the heap lock before it stops the other threads,
walks the arena page by page, checks that every object of the type has room
(`count * new size <= room`), and refuses the whole patch if one does not.
Otherwise it converts each object through a scratch copy and points its
header at the record for the new descriptor. Each object converts from the
descriptor it was allocated with, so objects that old code made after an
earlier patch still convert. Undo converts back.

Two million `new` and `free` pairs of a 16-byte struct take 65 ms in a live
build and 71 ms in a normal one on Windows, 49 ms and 26 ms on Linux.

Module-scope variables get the same treatment through slack: a live build
emits zero bytes after every variable (8 after a scalar, `size / 2 + 64` after
anything larger, at most 64 KB), so the
state file's size for it is its capacity. When the new layout fits, the patch
carries a migration whose old and new address are the same, and the loader
converts through a scratch copy. Only a variable that outgrows its room moves
to the arena, and pointers into its old storage then keep the old value;
rewriting them needs pointer types in descriptors (`p8` carries no pointee).

## M4: every frame moves

Moving frames is done in the IR, before optimization, so the new code is
ordinary optimized code and no stack maps are needed.

### Sites

Lowering wraps every simple statement in begin and end markers. The begin
marker carries a hash of the statement's source text with whitespace and
comments removed. After every function is lowered, a program pass finds which
functions may reach a quiesce point: those that hold one, make an indirect
call, or call one that may. Then each function gets sites:

- every `quiesce;`
- every statement that calls a function that may reach a quiesce point

A site's key mixes the statement's hash, the previous statement's hash, and
the source text of the headers of the loops around it, plus an occurrence
count for repeats. Keys come from source text so an edit elsewhere, or a
struct whose fields moved, leaves them the same.

Each function with sites gets a version table, `MTLV`, a signature hash and
its keys, in read-only data. `f$live_frame` holds a pointer to the newest
table. At a site, `f` calls `mettle_live_frame_check` (quiesce) or
`mettle_live_frame_moved` (call sites), which compare the newest table with
the function's own. The frame moves when the tables differ, belong to
different blobs, share the signature, and the newest one has this site's key.

### Continuations

Each function with sites gets one continuation, `f$resume(key)`. Its entry
takes every local back by name, then dispatches on the key to the point after
that site. Entering a loop in the middle would make the control flow
irreducible, so the continuation has one copy of the function per loop that
holds a site, plus the root:

- the copy for loop `L` is entered at the site and runs the rest of `L`'s
  current iteration;
- a back edge of `L` or of a loop around it goes to a landing label before
  that loop's header in the copy one level out;
- the root copy is the whole function in steady state.

What remains is reducible, every loop is entered through its header, and the
size is about the number of site-holding loops times the function.

The transfer path at the bottom of `f` puts every live local with
`mettle_live_frame_put`, then loops: call `f$resume(key)`, and when
`mettle_live_frame_again` says the continuation moved again, call it again
with `mettle_live_frame_last_key`. A continuation that moves hands its locals
back with `mettle_live_frame_hand_back(key)` and returns. The stack stays two
frames deep however many patches land.

A local that the continuation reaches only through its address adopts the
old frame's storage, so pointers the old code saved stay valid; the old frame
stays on the stack under the continuation, which keeps that storage alive.

### Runtime

- `mettle_live_frame_check(flag, table, key, owner)` applies patches, then
  reports whether this frame moves.
- `mettle_live_frame_moved(flag, table, key, owner)` reports it without
  applying.
- `mettle_live_frame_put(name, key, size, place, keep, descriptor)` records a
  local; `keep` leaves the value in place, otherwise it is copied now.
- `mettle_live_frame_take(name, key, size, want_storage, descriptor)` returns
  the place of a matching local, converted if its descriptor changed, or 0.
- `mettle_live_frame_end`, `mettle_live_frame_hand_back`,
  `mettle_live_frame_again`, `mettle_live_frame_last_key` run the transfer
  loop.

A local that is missing prints one line and starts from its constant
initializer, or zero.

## M7: crashes continue

A crash under `mettle live` waits for a patch and continues from the last
quiesce point the thread passed, in a frame that is still on its stack.

In a live build, `quiesce;` calls `mettle_live_site_check`, a naked trampoline
in the port layer. It stores the callee-saved registers (and `xmm6` to `xmm15`
on Windows), its caller's stack pointer and its return address into a
per-thread stack of eight resume points, then jumps to
`mettle_live_frame_check`, which records the frame's flag, key, table and
owner beside them. Entries are keyed by stack pointer: a site in the same frame
reuses its entry, a deeper frame pushes one, and every return from a function
with quiesce sites calls `mettle_live_site_leave_here`, which pops the entries
at or below the returning frame. The top entry is always a live frame.

Live builds send every trap through the crash handler, including the checks a
build without `-s` would turn into `puts` and `exit`. The Windows exception
filter, the Linux signal handler and the trap reporter all end the same way:
print the report, take the top resume point above the faulting stack pointer,
and call `mettle_live_recover_wait`. That returns at once unless
`app.exe.live.watched` exists, which `mettle live` writes before it starts the
program and a `--live` link deletes. Otherwise it polls for patches every
100 ms, and once one applies whose table still has the site's key and
signature, it starts a frame move and returns. The handler then loads the
resume point into the faulting context (or jumps to it from a trap) with
`rax = 1`, so the quiesce site sees `mettle_live_frame_check` report a move.
The function's transfer path hands its locals to the newest continuation, as
for any move.

The frames above the resume point are abandoned without running their defers.
A stack overflow is not recovered: its guard page is gone.

## Undo

`mettle --live-undo app.exe` restores the state snapshot taken before the
latest patch and writes a patch whose header names the patch to undo. The
loader replays that patch's log backwards with other threads stopped:
variables convert back into their old storage, entries get their old bytes,
slots and frame tables their old values. Frames then move back to the old code
at their next site, the same way they moved forward.

## Threads

A patch applies on the thread that reaches `quiesce;`. On Windows the loader
suspends every other thread of the process while it converts and swaps. On
Linux the other threads run on; each entry is one aligned eight-byte store,
and a migration on Linux is only safe when the other threads do not touch the
migrated variables while it runs. A frame moves only on its own thread, at its
own next site.

## Linux

The ELF backend emits the same entries. The state file records the addresses
the ELF emitter assigns. Patches use the same fixups, with explicit addends
and 32-bit absolute addresses for the non-PIE image. The loader reads
`/proc/self/exe`, allocates near the image with `mmap`, and makes code
writable for an entry store with `mprotect`. `mettle live` on Linux spawns the
program and polls source modification times.

## M5 to M8

- M5, representations: a `migrate fn` from the old type to the new one, run
  once at the swap, with pointers rewritten as in M3.
- M6, time: the allocator tracks dirty pages, so a snapshot every frame is
  cheap. The sources of nondeterminism (clock, input, network, random numbers)
  are recorded. A patch can be replayed against the last N seconds and the
  outcomes compared, or run on a copy of the state for N frames before it is
  committed. Effects decide which calls reach the outside world during a
  speculative run.
- M7, rewind and timelines: with M6 snapshots, a crash rewinds memory to the
  last snapshot before it continues. One snapshot can fan out into K forks,
  each with a different patch, and the results are compared side by side.
- GPU kernels: programs load PTX and keep `gpu_func` handles themselves, so a
  patch cannot reach them. Launches by kernel name through a runtime table,
  which a patch refills from freshly loaded PTX, would let kernels change with
  the host code.
- M8, fleets: patches carry a logical tick. Generated wire formats carry their
  own migrations, so old and new nodes can talk during a rollout.
