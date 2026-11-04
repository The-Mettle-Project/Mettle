# Standard library

The modules under `stdlib/std`. Import one by path, `import "std/io";`, or
pass [`--prelude`](compilation.md) to get the common ones without asking.

Signatures below are copied from the source. A module lists only what it
exports; a module with no `export` at all is entirely public.

Several modules ship a `.linux.mettle` sibling. A `std/` import with no
extension picks the platform half on its own, so `import "std/io"` is right on
both systems.

## std/core

`Result` and `Option`, the two tagged enums the rest of the library returns.

```mettle
export enum Result<T, E> { Ok(T), Err(E) }
export enum Option<T> { Some(T), None }
```

`Result` is for a call that could not do what it was asked, and the error arm
carries why. `Option` is for a value that may not be there. Read either with
[`match`](control-flow.md).

## std/io

Console and file input and output. Both platforms.

| Function | Effect |
|----------|--------|
| `print(msg: string)` | Write to stdout |
| `println(msg: string)` | Write to stdout with a newline |
| `print_err(msg: string)` | Write to stderr |
| `println_err(msg: string)` | Write to stderr with a newline |
| `newline()` | Write one newline |
| `print_cstr(s: cstring)` | Write nul-terminated bytes |
| `println_cstr(s: cstring)` | The same with a newline |

Values reach the output through [interpolation](expressions.md), so there is
one print function and it takes a `string`:

```mettle
var n: int32 = 42;
println("answer is {n}");
```

That line takes one block and gives it back: `print` and `println` only read
what they are handed, which the compiler works out rather than being told, so
the string built for the call is released when it returns. See
[what interpolation costs](expressions.md).

Files come through the C standard library, declared here so you can call them
directly:

| Function | Effect |
|----------|--------|
| `fopen(filename: cstring, mode: cstring) -> cstring` | Open a file |
| `fclose(fp: cstring) -> int32` | Close it |
| `fread(buf: cstring, size: int64, count: int64, fp: cstring) -> int64` | Read |
| `fwrite(buf: cstring, size: int64, count: int64, fp: cstring) -> int64` | Write |
| `fgets(buf: cstring, size: int32, fp: cstring) -> cstring` | Read a line |
| `get_stdin()`, `get_stdout()`, `get_stderr()` | The standard streams |

Two wrappers make line reading pleasant, returning a `string` view of the
buffer you hand them:

| Function | Effect |
|----------|--------|
| `read_line(buf: cstring, cap: int32, fp: cstring) -> string` | One line from a file |
| `read_line_stdin(buf: cstring, cap: int32) -> string` | One line from stdin |

`cstr(s: string, alloc: fn(int64) -> rawptr) -> cstring` copies a `string`
into nul-terminated memory from the allocator you pass, for handing to C.

## std/conv

Character tests, string slicing and searching, and number conversion. Both
platforms.

Character tests take and return [`char`](types.md):

```text
is_digit  is_upper  is_lower  is_alpha  is_alnum  is_space
to_lower  to_upper  digit_to_char  char_to_digit
```

Building a `string`:

| Function | Effect |
|----------|--------|
| `str_from_bytes(p: cstring, n: int64) -> string` | View n bytes |
| `str_from_cstr(s: cstring) -> string` | View up to the nul |
| `str_slice(s: string, start: int64, len: int64) -> string` | A sub-view |

Searching and trimming:

| Function | Effect |
|----------|--------|
| `str_find(s: string, needle: string) -> Option<int64>` | Byte offset of the first match |
| `str_find_byte(s: string, c: int32) -> Option<int64>` | Offset of a byte |
| `str_contains(s: string, needle: string) -> int32` | 1 when present |
| `str_compare(a: string, b: string) -> int32` | Unsigned-byte lexicographic ordering: -1, 0 or 1 |
| `str_count(s: string, needle: string) -> int64` | Count non-overlapping matches; an empty needle matches at every boundary |
| `str_starts_with(s: string, prefix: string) -> int32` | 1 when it does |
| `str_ends_with(s: string, suffix: string) -> int32` | 1 when it does |
| `str_eq_at(s: string, offset: int64, needle: string) -> int32` | Compare at an offset |
| `str_trim(s: string) -> string` | Both ends |
| `str_trim_start(s: string) -> string` | Leading space |
| `str_trim_end(s: string) -> string` | Trailing space |
| `str_split_once(s: string, sep: string) -> Option<StrSplit>` | First split |

`StrSplit` is a struct holding the two halves.

Numbers:

| Function | Effect |
|----------|--------|
| `str_to_i64(s: string) -> Result<int64, string>` | Parse, with a reason on failure |
| `str_to_i64_or(s: string, fallback: int64) -> int64` | Parse or fall back |
| `i64_to_str(n: int64, buf: uint8*, buf_len: int64) -> string` | Format into your buffer |
| `int_to_dec(buf: uint8*, n: int64) -> int32` | Digits written |
| `format_i64(buf: uint8*, buf_len: int32, pattern: string, value: int64) -> int32` | Pattern format |

`cstr_len`, `streq`, and `cstr_ncmp` work on `cstring` for the C boundary.

## std/encoding

Strict hexadecimal and RFC 4648 Base64 over arbitrary bytes. Both platforms.
A `string` is a byte view here, so embedded nuls are ordinary input; use
`str_from_bytes` to make one from a pointer and length.

Size queries return `Result<int64, string>`:

```text
hex_encoded_len(src_len)       hex_decoded_len(src)
base64_encoded_len(src_len)    base64_decoded_len(src)
```

The codecs take caller-owned output storage and return an `Ok(string)` view of
the bytes they wrote. They allocate nothing and add no nul terminator:

| Function | Effect |
|----------|--------|
| `hex_encode(src: string, dst: cstring, dst_cap: int64) -> Result<string, string>` | Lowercase hexadecimal |
| `hex_decode(src: string, dst: cstring, dst_cap: int64) -> Result<string, string>` | Decode either letter case |
| `base64_encode(src: string, dst: cstring, dst_cap: int64) -> Result<string, string>` | Standard alphabet with padding |
| `base64_decode(src: string, dst: cstring, dst_cap: int64) -> Result<string, string>` | Strict padded Base64 |

Hex rejects odd lengths and non-hex digits. Base64 rejects whitespace, the
URL-safe alphabet, misplaced padding, lengths not divisible by four, and
nonzero unused trailing bits. Empty input succeeds with a null destination and
zero capacity. For nonempty output, the destination must be large enough and
must not overlap the source. `Err` carries a static reason string.

## std/math

Floating-point maths in Mettle source, no libm on the link line. Both
platforms.

Constants are functions, so `PI()` and `E()`:

```text
PI  TAU  HALF_PI  QUARTER_PI  E  SQRT2  SQRT1_2
LN2  LN10  LOG2E  LOG10E  LOG10_2  INF  NAN
EPSILON  F32_EPSILON  MAX_FINITE  MIN_POSITIVE
DEG_PER_RAD  RAD_PER_DEG
```

Classification: `is_nan`, `is_inf`, `is_finite`, `signbit`.

Sign and range: `fabs`, `copysign`, `fsign`, `fmin`, `fmax`, `fclamp`,
`saturate`.

Rounding: `trunc`, `floor`, `ceil`, `round`, `fract`, `fmod`.

Float to integer with the mode named, rather than the one a cast picks:
`floor_i32`, `ceil_i32`, `trunc_i32`, `round_i32`, and their `_i64` siblings.
All are `@inline`.

Powers and roots: `sqrt`, `rsqrt`, `hypot`, `cbrt`, `pow`, `ldexp`, `frexp`.

Exponential and logarithm: `exp`, `exp2`, `expm1`, `log`, `log2`, `log10`,
`log1p`.

Trigonometry: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`.

Bit access: `f64_bits(x) -> int64`, `f64_from_bits(b) -> float64`,
`f64_raw_exponent(x) -> int32`.

Every one takes and returns `float64`.

Accuracy: the kernels carry enough terms that their truncation error sits below
one unit in the last place, so results are accurate to about 1 ulp for arguments
in the normal range. Trigonometric argument reduction uses a two-part split of
pi/2, which holds full precision for `|x|` up to roughly 1e8; past that the
reduction, and so the result, degrades.

The constants are functions rather than `const` bindings because top-level
`const` is restricted to integers (see [known limitations](known-limitations.md)).
They fold to a single load, so `PI()` costs nothing over a literal.

## std/mem

Raw memory moves. Both platforms.

| Function | Effect |
|----------|--------|
| `mem_zero(p: cstring, n: int64) -> cstring` | Fill with zero |
| `mem_fill(p: cstring, value: int32, n: int64) -> cstring` | Fill with a byte |
| `mem_copy(dst: cstring, src: cstring, n: int64) -> cstring` | Copy, no overlap |
| `mem_move(dst: cstring, src: cstring, n: int64) -> cstring` | Copy, overlap allowed |
| `mem_equal(a: cstring, b: cstring, n: int64) -> int32` | 1 when equal |
| `mem_compare(a: cstring, b: cstring, n: int64) -> int32` | Ordering |
| `mem_find_byte(p: cstring, value: int32, n: int64) -> int64` | Offset or -1 |
| `alloc_zeroed(n: int64) -> rawptr` | Allocate and zero |
| `buf_dup(src: rawptr, len: int64) -> rawptr` | Allocate a copy |

`mem_copy` requires that the ranges do not overlap; reach for `mem_move` when
they might.

`memory_region` describes a borrowed region from foreign code so Mettle can
dereference it under [`--safe`](memory-safety.md). The caller must know the
whole region is live. Ending the region retires its identity; the foreign owner
still releases the bytes.

## std/alloc

Mettle's own thread-safe heap, written in Mettle. Both platforms.
[Heap allocation](heap-allocation.md) covers when to reach for it.

The short names allocate from a shared default heap:

| Function | Effect |
|----------|--------|
| `mx_alloc(n: int64) -> rawptr` | Allocate |
| `mx_calloc(count: int64, size: int64) -> rawptr` | Allocate and zero |
| `mx_realloc(p: rawptr, newsize: int64) -> rawptr` | Resize |
| `mx_free(p: rawptr)` | Release |
| `mx_stats() -> MemStats` | Counters |

For a heap of your own:

| Function | Effect |
|----------|--------|
| `mem_heap_create() -> MemHeap*` | New heap |
| `mem_alloc(h: MemHeap*, n: int64) -> rawptr` | Allocate from it |
| `mem_calloc(h: MemHeap*, count: int64, size: int64) -> rawptr` | Allocate and zero |
| `mem_realloc(h: MemHeap*, p: rawptr, newsize: int64) -> rawptr` | Resize |
| `mem_free(h: MemHeap*, p: rawptr)` | Release |
| `mem_heap_stats(h: MemHeap*) -> MemStats` | Counters |
| `mem_heap_destroy(h: MemHeap*)` | Tear down |
| `mem_default_heap() -> MemHeap*` | The heap the `mx_` names use |

The `mettle_heap_*` names are the hooks
[`--native-heap`](heap-allocation.md) redirects the language's own allocation
through.

A null result is the integer `0`:

```mettle
var buf: rawptr = mx_alloc(1024);
if (buf == 0) { return 1; }
```

Thread safety: every public heap operation takes the heap's spinlock, so a
single `MemHeap`, the process-global one behind the `mx_` names included, is
safe to share across threads. It is a short-critical-section spinlock, so
heavily contended workloads do better with a `MemHeap` per thread than with the
shared one.

A block must be freed to the heap that allocated it. Freeing `0` is a no-op.
`mem_heap_destroy` does not walk large blocks, so those must already have been
freed individually. `mem_calloc` and `mx_calloc` reject a `count * size` that
would overflow `int64`, so a wrapped-around small request can never
under-allocate behind a large logical size. Freed small blocks are not zeroed;
the `calloc` names zero explicitly.

## std/arena

Bump allocation with a whole-region reset. Both platforms. Good for work with
one clear end, a request, a frame, a parse.

| Function | Effect |
|----------|--------|
| `arena_init(default_chunk_size: int64) -> Arena*` | New arena |
| `arena_alloc(a: Arena*, n: int64) -> rawptr` | Bump |
| `arena_alloc_zeroed(a: Arena*, n: int64) -> rawptr` | Bump and zero |
| `arena_alloc_aligned(a: Arena*, n: int64, align: int64) -> rawptr` | Bump to an alignment |
| `arena_save(a: Arena*) -> ArenaSave` | Mark a point |
| `arena_restore(a: Arena*, save: ArenaSave)` | Roll back to it |
| `arena_reset(a: Arena*)` | Release everything, keep the chunks |
| `arena_free(a: Arena*)` | Release everything |
| `arena_stats(a: Arena*) -> ArenaStats` | Counters |

## std/strbuf

A growable byte buffer over an [arena](#stdarena). Both platforms.

| Function | Effect |
|----------|--------|
| `strbuf_new(arena: Arena*, initial_cap: int64) -> StrBuf*` | New buffer |
| `strbuf_len(b: StrBuf*) -> int64` | Bytes written |
| `strbuf_append_byte(b: StrBuf*, c: uint8) -> int32` | One byte |
| `strbuf_append_bytes(b: StrBuf*, src: cstring, n: int64) -> int32` | n bytes |
| `strbuf_append_cstr(b: StrBuf*, s: cstring) -> int32` | Up to the nul |
| `strbuf_append_string(b: StrBuf*, s: string) -> int32` | A string view |
| `strbuf_append_int(b: StrBuf*, n: int64) -> int32` | Signed decimal |
| `strbuf_append_uint(b: StrBuf*, n: uint64) -> int32` | Unsigned decimal |
| `strbuf_append_hex(b: StrBuf*, n: uint64) -> int32` | Hexadecimal |
| `strbuf_finish_string(b: StrBuf*) -> string` | Borrowed view; does not terminate |
| `strbuf_finish_cstr(b: StrBuf*) -> cstring` | Terminate and hand back |

The returned view or cstring remains owned by the arena and must not be freed
separately. Take a fresh view after a later append, which can move the buffer's
current storage.

## std/collections

Generic owning collections. Both platforms. Import `std/core` as well when
matching their `Result` and `Option` returns.

`List<T>` is a growable contiguous array. A zero-initialized value and
`list_init<T>()` are both empty and ready to use:

| Function | Effect |
|----------|--------|
| `list_reserve<T>(items: List<T>*, minimum: int64) -> Result<int32, string>` | Ensure capacity |
| `list_push<T>(items: List<T>*, value: T) -> Result<int32, string>` | Append |
| `list_pop<T>(items: List<T>*) -> Option<T>` | Remove and return the last element |
| `list_get<T>(items: List<T>*, index: int64) -> Option<T>` | Checked copy |
| `list_get_ptr<T>(items: List<T>*, index: int64) -> T*` | Checked borrowed pointer, or null |
| `list_set<T>(items: List<T>*, index: int64, value: T) -> bool` | Checked replacement |
| `list_remove<T>(items: List<T>*, index: int64) -> Option<T>` | Ordered removal, shifting later elements |
| `list_clear<T>(items: List<T>*)` | Keep storage, reset length |
| `list_free<T>(items: List<T>*)` | Release storage and reset the list |

`HashMap<K, V>` is an open-addressed table with cached hashes and tombstones.
`hash_map_init<K, V>(hash_fn, equal_fn)` takes plain function pointers of types
`fn(rawptr) -> uint64` and `fn(rawptr, rawptr) -> bool`; the arguments point to
keys. `std/hash` supplies ready-made callbacks for `string` and `int64` keys.

| Function | Effect |
|----------|--------|
| `hash_map_put<K, V>(map: HashMap<K, V>*, key: K, value: V) -> Result<int32, string>` | Insert or replace |
| `hash_map_get<K, V>(map: HashMap<K, V>*, key: K) -> Option<V>` | Checked copy |
| `hash_map_get_ptr<K, V>(map: HashMap<K, V>*, key: K) -> V*` | Borrowed value pointer, or null |
| `hash_map_contains<K, V>(map: HashMap<K, V>*, key: K) -> bool` | Membership |
| `hash_map_remove<K, V>(map: HashMap<K, V>*, key: K) -> Option<V>` | Remove and return a value |
| `hash_map_clear<K, V>(map: HashMap<K, V>*)` | Keep storage, remove every entry |
| `hash_map_free<K, V>(map: HashMap<K, V>*)` | Release the slot array |

Both collections shallow-copy their elements. They do not clone strings, free
pointees, or run destructors, and copying the collection itself does not clone
its storage. Pass the owning value by pointer and call its `free` exactly once.
Pointers from `list_get_ptr` and `hash_map_get_ptr` are invalidated by growth or
rehashing. A failed growth leaves the original collection usable.

## std/hash

Portable 64-bit hashing and key callbacks for `std/collections`:

| Function | Effect |
|----------|--------|
| `hash_bytes(data: cstring, length: int64) -> uint64` | FNV-1a over bytes |
| `hash_string(value: string) -> uint64` | FNV-1a over a string view |
| `hash_string_ptr(value: rawptr) -> uint64` | Hash callback for a `string*` |
| `string_equal_ptr(a: rawptr, b: rawptr) -> bool` | Equality callback for two `string*` |
| `hash_int64_ptr(value: rawptr) -> uint64` | Mixing hash callback for an `int64*` |
| `int64_equal_ptr(a: rawptr, b: rawptr) -> bool` | Equality callback for two `int64*` |

These hashes are for hash tables, not passwords, signatures, or adversarial
network input.

## std/utf8

Code points over the byte view a [`string`](types.md) gives you. Both
platforms.

| Function | Effect |
|----------|--------|
| `utf8_at(s: string, i: int64) -> int32` | Code point at a byte offset |
| `utf8_span(s: string, i: int64) -> int64` | Bytes in the character at i |
| `utf8_count(s: string) -> int64` | Characters, not bytes |
| `utf8_offset(s: string, index: int64) -> int64` | Byte offset of character n |
| `utf8_valid(s: string) -> bool` | Well-formed |
| `utf8_len(cp: int32) -> int64` | Bytes a code point needs |
| `utf8_encode(buf: cstring, cp: int32) -> int64` | Write one, return the length |
| `utf8_string(buf: cstring, cp: int32) -> string` | Write one, return a view |

Malformed input never stops the program and never stalls it. A bad byte decodes
to U+FFFD and spans exactly one byte, so a walk always advances and garbage
reads as garbage rather than being invented into text. Over-long encodings,
surrogate halves and out-of-range code points are refused the same way.

`utf8_offset` is O(length): finding the n'th character in UTF-8 means counting
the ones before it, which is also why `s[i]` indexes bytes and there is no
character index.

## std/osmem

Pages from the operating system, under the heap. Both platforms, with a
`.linux` half.

| Function | Effect |
|----------|--------|
| `os_page_size() -> int64` | Page size |
| `os_mem_map(n: int64) -> cstring` | Map n bytes |
| `os_mem_unmap(p: cstring, n: int64)` | Unmap |
| `os_mem_protect_noaccess(p: cstring, n: int64)` | Make a range unreadable |

On Linux these make raw system calls, so they also work in a self-contained
ELF (`--emit-arm64`), which links no libc.

## std/thread

Threads, mutexes, and atomics. Both platforms, with a `.linux` half and a
`thread_posix` variant.

Threads: `thread_join(handle: int64, timeout_ms: uint32) -> uint32`,
`thread_join_infinite`, `thread_detach`, `thread_close`,
`thread_sleep_ms(milliseconds: uint32)`.

Mutexes: `mutex_create`, `mutex_create_owned`, `mutex_lock(mutex, timeout_ms)`,
`mutex_lock_infinite`, `mutex_unlock`, `mutex_close`.

Atomics on an `int32*`: `atomic_compare_exchange_i32`, `atomic_exchange_i32`,
`atomic_inc_i32`, `atomic_dec_i32`.

Spin locks on an `int32*`: `spin_try_lock`, `spin_lock`, `spin_unlock`. The
lock variable must be a shared `int32` initialised to 0.

Wait results come back as `WAIT_OBJECT_0()`, `WAIT_TIMEOUT()`, or
`WAIT_FAILED()`, and `INFINITE()` is the timeout that never expires.

The externs that perform nothing carry `with none`: the atomics, the waits,
the handle close, and the yield a spin lock backs off with. That is what lets
a real-time path declare `forbids alloc` and hold it across taking a lock. The
ones that do take a resource, `CreateThread` and `CreateMutexA`, declare
nothing and are read as performing anything, which is what an unannotated
extern means.

## std/sync

Locks and condition variables for threads: `SyncMutex`, `SyncCond`,
`SyncRwLock` and `SyncSem`. Linux, macOS and Windows. Each is a plain struct
whose zero value is ready to use, so `var m: SyncMutex;` or a zeroed field
needs no setup, and none of them allocates. Share them by pointer, never by
copy.

Every timed call takes milliseconds and keeps one deadline for the whole call,
so a waiter that is woken and loses the race waits only for the time that is
left. A negative timeout waits forever. The timed calls return false on
timeout.

| Function | Effect |
|----------|--------|
| `sync_mutex_lock(&m)`, `sync_mutex_lock_timeout(&m, ms) -> bool`, `sync_mutex_try_lock(&m) -> bool`, `sync_mutex_unlock(&m)` | A mutex. An unlock with nobody waiting makes no system call |
| `sync_cond_wait(&c, &m)`, `sync_cond_wait_timeout(&c, &m, ms) -> bool` | Unlock `m`, wait, and lock `m` again. It can wake for no reason, so wait in a loop that re-checks |
| `sync_cond_signal(&c)`, `sync_cond_broadcast(&c)` | Wake one waiter, or all |
| `sync_rwlock_read(&l)`, `sync_rwlock_read_timeout`, `sync_rwlock_try_read` | Shared hold |
| `sync_rwlock_write(&l)`, `sync_rwlock_write_timeout`, `sync_rwlock_try_write` | Exclusive hold. A waiting writer keeps new readers out |
| `sync_rwlock_unlock(&l)` | Release either kind |
| `sync_sem_acquire(&s)`, `sync_sem_acquire_timeout`, `sync_sem_try_acquire`, `sync_sem_release(&s)` | A counting semaphore; set `s.count` before sharing it |

They sit on the platform's wait-on-address call, which `std/sync_os` exports:
`sync_os_wait(addr, expected, timeout_ns)`, `sync_os_wake(addr, all)` and
`sync_os_now_ns()`. That is futex on Linux and `__ulock_wait` on macOS. On
Windows it is built from kernel32's SRW locks and condition variables.

## std/parallel

Runs one body over a range of indexes across the machine's cores. Both
platforms: Windows threads, pthreads elsewhere.

| Function | Effect |
|----------|--------|
| `parallel_range(body, ctx: rawptr, lo: int64, hi: int64, min_chunk: int64)` | Split `[lo,hi)` across threads |
| `parallel_fill_bytes(destination: rawptr, count: int64, value: int64)` | Fill bytes |
| `parallel_fill_sized(destination: rawptr, count: int64, value: int64, element_size: int32)` | Fill 1, 2, 4 or 8-byte elements |
| `parallel_fill_i32(destination: int32*, count: int64, value: int32)` | Fill `int32` |
| `parallel_fill_i64(destination: int64*, count: int64, value: int64)` | Fill `int64` |
| `parallel_zero(destination: rawptr, bytes: int64)` | Zero bytes |
| `parallel_copy(destination: rawptr, source: rawptr, bytes: int64)` | Copy, moving correctly when the buffers overlap |

The body has type `fn(rawptr, int64, int64) -> int32` and is called once per
chunk with the context and that chunk's half-open range. It must be safe to
run chunks at the same time: read what you like, write only to the indexes you
were handed.

```mettle
struct Work { src: float64*; dst: float64*; }

@noinline fn scale(ctx: rawptr, lo: int64, hi: int64) -> int32 {
  var w: Work* = (Work*)ctx;
  var i: int64 = lo;
  while (i < hi) { w->dst[i] = w->src[i] * 2.0; i += 1; }
  return 0;
}

parallel_range(&scale, (rawptr)w, 0, n, 1024);
```

The context must be heap-allocated. Handing a stack address to the body is a
compile error, because the body outlives the frame as far as the compiler can
see, even though `parallel_range` joins before it returns.

Marking a loop `@parallel` writes all of this for you: the compiler outlines the
body, builds the context and calls `parallel_range` itself. Reach for these
functions directly when the work does not fit a counted loop.
[Declarations](declarations.md) covers the decorator.

`min_chunk` is the smallest range worth a thread. Below `min_chunk * 2` the
body runs once on the calling thread, so a short range costs one call and no
thread. The caller always runs the last chunk itself.

`METTLE_PARALLEL_THREADS` overrides the core count at run time, capped at 64.

How much this buys depends on what the loop waits for. A body that computes
scales close to the core count; one that streams memory saturates the memory
bus first. On a loop doing 200 dependent rounds per element it measured 7.2x,
and on a 64MB fill 1.8x.

## std/machine

One type, `MachineInsn`, and a `const` of it is an instruction set: `name`,
`encoding`, `operands`, `reads`, `writes` and `semantics` per row. The
compiler reads the const, refuses one that cannot be decoded back,
assembles `const PROGRAM: string[N]` into that machine's bytes, and runs the
result by calling each instruction's semantics function in the compile-time
interpreter. `mettle machine <file>` prints the description and
`mettle emulate <file>` runs it. [Bare metal](bare-metal.md) covers it, and
[`examples/machine/`](../examples/machine/) is a worked one.

## std/schedule

One type, `Schedule`, and a `const` of it is a frame written down: `phase`,
`effect`, `entry`, `thread` and `joins` per row. The compiler reads the const, checks
it, and generates one wrapper per phase and one dispatcher per thread with a
`quiesce` at every phase boundary. Because a wrapper provides only its own
phase's effect, a call across a phase boundary is refused by the effect pass.
[The runtime model](runtime-model.md) covers it, and `mettle expand` prints
what was generated.

## std/rule

The records a `@rule fn` reads and returns: `Program`, `Function`, `TypeInfo`,
`FieldInfo`, `EffectInfo`, `Site` and `Verdict`, with `verdict_pass()`,
`verdict_fail(site, message)` and `verdict_gap(site, message)`. A `Function`
carries its inferred `effects` and `requires` and its declared `forbids` and
`provides`; `function_performs`, `function_requires`, `function_forbids`,
`function_provides` and `program_effect_index` read them.

Verdicts about the program as a whole, for a complaint no line owns:
`verdict_fail_program(message)` and `verdict_gap_program(message)`.

Queries over a program: `function_calls(f, callee) -> bool`,
`function_matches(f, owner, variant) -> bool`,
`program_function_index(p, qualified) -> int64`,
`program_type_index(p, qualified) -> int64`, and
`function_reaches(p, f, callee) -> bool`, which follows direct calls
transitively. [Rules](rules.md) covers what a rule is and what it sees.

## std/target

`TargetDesc`, the record a target description is written as: `name`, `arch`,
`os`, `format`, `pointer_bits`, `stack_alignment`, `shadow_space`,
`red_zone`, `int_args`, `float_args`, `indirect_return`, `separate_classes`,
`widths`, `vector_width`, `address_spaces`, and the ten `cost_` fields that
say what an instruction of each kind costs on the machine. `mettle target
<triple>` prints one for every built-in target and `--target desc.mettle`
reads one back. [Bare metal](bare-metal.md) covers what a description may
change, and [Types](types.md) covers what the cost fields are read for.

## std/random

Random numbers from the operating system, good for keys, tokens and ids:
`getrandom` on Linux, `getentropy` on macOS, `RtlGenRandom` on Windows. There
is no seed and no shared state, so it is safe from any thread.

| Function | Effect |
|----------|--------|
| `random_bytes(buf: uint8*, n: int64) -> int64` | Fill `buf`. Returns 0, or a negative code if the OS has no randomness to give |
| `random_u64() -> uint64`, `random_u32() -> uint32` | One value. The program stops if the OS refuses |
| `random_below(n: uint64) -> uint64` | Uniform in `0..n-1`, with no bias |
| `random_range(lo: int64, hi: int64) -> int64` | Uniform in `lo..hi`, both included |
| `random_float64() -> float64` | Uniform in `[0, 1)`, from 53 bits |

Each call asks the OS. For fast simulation noise, seed your own generator from
`random_u64()`.

## std/env

Environment variables and program arguments. Linux, macOS and Windows.

| Function | Effect |
|----------|--------|
| `env_get(name) -> Option<string>` | A variable's value. The view lasts until the variable is set again |
| `env_get_or(name, fallback) -> string`, `env_has(name) -> bool` | The same, shorter |
| `env_set(name, value) -> int64` | Set a variable. 0, or -22 for a name with `=` or a NUL |
| `env_list(buf, cap, prefix) -> int64` | Every variable as `NAME=value` lines, or only names starting with `prefix` |
| `env_args(out: string*, max: int32) -> int32` | The program's arguments, `argv[0]` first. Returns how many there are in all |

On Linux no libc is linked, so the runtime keeps the environment: it reads the
block the kernel passed in, and keeps changes in a table of its own. The
arguments come from `/proc/self/cmdline` on Linux, `_NSGetArgv` on macOS, and
`GetCommandLineA` on Windows, split with the MSVCRT rules (at most 256).

## std/term

Terminal basics, for prompts. Linux, macOS and Windows. fd 0 is standard
input, 1 output and 2 error.

| Function | Effect |
|----------|--------|
| `term_is_tty(fd) -> bool` | Is `fd` a terminal (a console on Windows) |
| `term_set_echo(fd, on: bool) -> int64` | Turn echo on or off. Returns 1 or 0 for whether it was on before, or a negative code when `fd` is not a terminal |
| `term_read_secret(prompt, buf, cap) -> int64` | Write `prompt` to standard error, read one line from standard input with echo off, and turn echo back on. Returns the length, without the line ending |

When standard input is a pipe or a file, `term_read_secret` reads the line as
it is, so scripts can feed a password in.

## std/socket

TCP and UDP over IPv4 and IPv6, with timeouts, polling and host-name lookup.
Linux, macOS and Windows. Use it for new code; `std/net` below is the older
Winsock-style API.

`import "std/socket"` brings in four modules:

| Module | Holds |
|--------|-------|
| `std/netaddr` | `NetAddr`, parsing and formatting, and the shared constants. No OS calls |
| `std/socket_os` | The raw calls, one file per platform |
| `std/dns` | `net_resolve` and the DNS message codec |
| `std/socket` | Helpers: connect by name, listen, send all, wait |

The raw calls return a count or descriptor, or a negative error code:
`-NET_EAGAIN`, `-NET_ECONNREFUSED`, `-NET_ETIMEDOUT`, `-NET_ECONNRESET`,
`-NET_EPIPE`, `-NET_EINPROGRESS`, `-NET_EINTR` and others. The codes are the
platform's own (errno, or Winsock's), named the same everywhere. So a receive
timeout (`-NET_EAGAIN`) is easy to tell from a closed peer (0). On Linux the
calls are raw system calls, with no libc.

Sockets are plain descriptors, so any event loop can watch them, alph's epoll
reactor included.

**Addresses.** `NetAddr` has `family` (`NET_IPV4` or `NET_IPV6`), `port`,
`ip` (16 bytes; IPv4 uses the first 4) and `scope_id`.

| Function | Effect |
|----------|--------|
| `net_addr_parse(s) -> Result<NetAddr, string>` | `192.0.2.1`, `2001:db8::1`, `::ffff:192.0.2.1`, `fe80::1%3` |
| `net_addr_v4(a, b, c, d, port)`, `net_addr_loopback(family, port)`, `net_addr_any(family, port)` | Build one |
| `net_addr_format(a, buf, cap) -> int64` | `192.0.2.1:80`, `[2001:db8::1]:443` |
| `net_addr_format_ip(a, buf, cap) -> int64` | The address alone, in RFC 5952 form |
| `net_addr_equal(a, b)`, `net_addr_is_loopback(a)` | Compare |
| `net_split_host_port(s, default_port) -> Result<NetHostPort, string>` | Split `host:port` or `[v6]:port`, with no lookup |

**Raw calls.** `sock_open(family, NET_TCP or NET_UDP)`, `sock_close`,
`sock_bind(fd, &addr)`, `sock_listen(fd, backlog)`, `sock_accept(fd, &peer)`,
`sock_accept_nonblocking(fd, &peer)`, `sock_connect(fd, &addr)`,
`sock_send(fd, buf, n)`, `sock_recv(fd, buf, n)`,
`sock_sendto`, `sock_recvfrom`, `sock_shutdown(fd, NET_SHUT_READ, _WRITE or
_BOTH)`, `sock_local_addr`, `sock_peer_addr`, and `sock_take_error` (the
pending error, after a non-blocking connect).

Sending never raises SIGPIPE; a closed peer gives `-NET_EPIPE`. Every socket
is close-on-exec. On a non-blocking socket, `sock_connect` returns
`-NET_EINPROGRESS`; wait for `NET_POLL_OUT`, then read `sock_take_error`.

| Function | Effect |
|----------|--------|
| `sock_set_option(fd, opt, 1 or 0)` | `NET_OPT_REUSEADDR`, `_REUSEPORT` (not Windows), `_NODELAY`, `_KEEPALIVE`, `_V6ONLY`, `_BROADCAST` |
| `sock_set_nonblocking(fd, on: bool)` | Non-blocking mode |
| `sock_set_timeout(fd, recv_ms, send_ms)` | Blocking calls give up after this long. 0 waits forever, -1 leaves that side alone |
| `sock_poll(fds: NetPollFd*, n, timeout_ms) -> int64` | Up to 64 sockets. `NET_POLL_IN`, `_OUT`, `_ERR`, `_HUP`, `_NVAL` are the same on every platform |
| `net_now_ms() -> int64` | A monotonic clock in ms, for deadlines |

On Linux there is also epoll: `net_epoll_create()`, `net_epoll_ctl(ep,
NET_EPOLL_ADD, _MOD or _DEL, fd, events, data)` and `net_epoll_wait(ep, out:
NetEpollEvent*, max, timeout_ms)`. `NET_EPOLL_ET` asks for edge-triggered
events.

**Helpers.** These return `Result<int64, NetError>`, where `NetError` has a
positive `code` and a `message`. `net_error_text(code)` describes a code.

| Function | Effect |
|----------|--------|
| `tcp_connect(host, port, timeout_ms)` | Resolve, then try each address in turn within the timeout. Gives a blocking socket |
| `tcp_connect_addr(&addr, timeout_ms) -> int64` | Connect to one address |
| `tcp_listen(&addr, backlog)` | Bind with `SO_REUSEADDR` and listen. Port 0 picks a free port |
| `udp_bind(&addr)` | A bound UDP socket |
| `sock_send_all(fd, buf, n) -> int64` | Send everything, through short writes |
| `sock_recv_exact(fd, buf, n) -> int64` | Receive exactly `n` bytes; 0 if the peer closed first |
| `sock_wait(fd, events, timeout_ms) -> int64` | Wait for one socket |

**Names.** `net_resolve(host, port, want, out: NetAddr*, max, timeout_ms) ->
Result<int32, DnsError>` returns how many addresses it wrote. `want` is
`DNS_WANT_ANY`, `_V4` or `_V6`. It tries, in order: an IP literal,
`localhost` and names under `.localhost`, `/etc/hosts`, then DNS over UDP to
the servers in `/etc/resolv.conf`, falling back to TCP when a reply is
truncated. IPv4 addresses come first. `search` domains are not applied. On
Windows, names past `localhost` go to the system resolver. `DnsError.code` is
one of `DNS_ERR_NOT_FOUND`, `_TIMEOUT`, `_BAD_NAME`, `_NO_SERVERS` or
`_SERVER`.

The pieces are exported too: `dns_query(&server, name, DNS_TYPE_A or
DNS_TYPE_AAAA, port, out, max, timeout_ms)` asks one server,
`dns_build_query` and `dns_parse_response` are the message codec, and
`dns_hosts_lookup` and `dns_parse_resolv_conf` read the two files' text.
To resolve on your own event loop, take `dns_system_servers(out, max)`, build
a query with `dns_new_id` and `dns_build_query`, send it on your own
non-blocking UDP socket, and read the reply with `dns_parse_response`.

## std/net

TCP and UDP sockets. Windows, over Winsock. On Linux import
`std/net_posix` instead; `std/prelude` leaves both out for that reason.

Setup: `net_init() -> Result<int32, int32>`, `net_cleanup()`,
`net_is_initialized()`, `net_last_error()`.

Sockets: `socket_tcp() -> Result<int64, int32>`,
`socket_udp() -> Result<int64, int32>`.

Addresses: `sockaddr_in(ip: cstring, port: int32) -> Result<cstring, int32>`,
`sockaddr_in_any(port: int32) -> Result<cstring, int32>`.

Options: `set_reuseaddr`, `set_nonblocking`, `set_nodelay`.

Transfer: `send_all(sock: int64, buf: cstring, len: int32) -> Result<int32, int32>`.
A short write leaves the loop carrying the platform error code, so a partial
send can never be read as a whole one.

The `Ok` arm of `sockaddr_in` and `sockaddr_in_any` is a 16-byte buffer from
the heap; the caller frees it.

`net_init` and `net_cleanup` are reference counted and safe to call from more
than one thread.

The Winsock constants are functions: `AF_INET()`, `SOCK_STREAM()`,
`SOCK_DGRAM()`, `IPPROTO_TCP()`, `IPPROTO_UDP()`, `SOL_SOCKET()`,
`SO_REUSEADDR()`, `SD_RECEIVE()`, `SD_SEND()`, `SD_BOTH()`, `INADDR_ANY()`,
`INVALID_SOCKET()`, `SOCKET_ERROR()`, `FIONBIO()`, `TCP_NODELAY()`.

## std/http

HTTP/1.1 (RFC 9110, RFC 9112): head parsers, body framing, a chunked decoder,
URLs and queries, writers, and a small blocking client. Importing it also
brings in `std/socket`. `http_fetch_to_file(url, path)` is the older helper
that runs `curl`; it is unchanged.

The parsers work on a buffer you own. They allocate nothing, and every string
they give back is a view into that buffer, so keep the buffer alive while you
use the result.

### Parsing a head

`http_parse_request(buf: string, req: HttpRequest*) -> int64` and
`http_parse_response(buf: string, res: HttpResponse*) -> int64` return the
bytes of head used (through the blank line), `0` if the head is not complete
yet, or a negative `HTTP_ERR_*`. After a `0`, read more bytes and call again.
`req.verb`, `req.target`, `req.minor` (the 1 in `HTTP/1.1`) and `req.headers`
(up to `HTTP_MAX_HEADERS`, 64, as `HttpHeader { name; value }` with the value
already trimmed) are filled in. A response has `status`, `reason`, `minor` and
the same headers. `head_len` is set too. The body starts at `buf[head_len]`.

The parsers are strict, for untrusted input. They refuse:

- a method that is not a token, a target with a space, a control byte or a
  byte over 0x7E, and any version but `HTTP/1.0` and `HTTP/1.1`;
- a header name that is not a token (so `Host : a` fails), and a header line
  that starts with a space or tab (obsolete folding);
- a bare CR or a bare LF, and NUL or other control bytes. Bare LF line ends
  are not accepted. A bad byte fails at once, before the head is complete;
- a head over `HTTP_MAX_HEAD` (64 KiB) and more than 64 headers;
- `Content-Length` together with `Transfer-Encoding`, `Content-Length` values
  that are not plain digits or that differ (repeated equal values are fine),
  and a request whose `Transfer-Encoding` does not end in `chunked`, names it
  twice, or is on HTTP/1.0 (a response with another last coding just runs
  until close);
- an HTTP/1.1 request without exactly one non-empty `Host`.

`http_find_head_end(p: uint8*, n) -> int64` does the first scan alone. Every
call scans from the start, so feed it large reads rather than single bytes.

### Headers and body framing

| Function | Effect |
|----------|--------|
| `http_header(h: HttpHeader*, count, name) -> Option<string>` | First header with that name, any case |
| `http_header_find(h, count, name, from) -> int32` | Index of the next match at or after `from`, or -1 |
| `http_header_has_token(value, token) -> bool` | Is `token` in a comma list such as `keep-alive, Upgrade` |
| `http_header_list_has(h, count, name, token) -> bool` | The same across every header of that name |
| `http_keep_alive(minor, h, count) -> bool` | Does the connection stay open: `Connection: close` and HTTP/1.0 rules |
| `http_request_body_kind(req) -> HttpBody` | How the request body is framed |
| `http_response_body_kind(res, head_request: bool) -> HttpBody` | The same for a response |

`HttpBody` has a `kind` and a `length`. The kinds are `HTTP_BODY_NONE`,
`HTTP_BODY_LENGTH` (`length` bytes), `HTTP_BODY_CHUNKED`, `HTTP_BODY_CLOSE`
(read until the peer closes, responses only) and `HTTP_BODY_INVALID` (the
headers contradict each other, and `length` is the `HTTP_ERR_*`). A response to
HEAD, any 1xx, 204 and 304 has no body whatever the headers say. A request
with neither header has none.

### Chunked bodies

```mettle
var d: HttpChunkDecoder;
var f: HttpChunkFeed;
http_chunk_init(&d, 1048576);                 // body limit; 0 for none
var status: int64 = http_chunk_feed(&d, src, n, dst, cap, &f);
```

`http_chunk_feed` takes `n` bytes of chunked framing and writes the body bytes
it finds to `dst`. `f.consumed` and `f.produced` say how much of each was
used. You can split the input anywhere, even inside a chunk size. The result is:

- `HTTP_CHUNK_MORE`: all input was used, feed more.
- `HTTP_CHUNK_DONE`: the body is complete. `f.consumed` stops at the end of
  the trailers, so what follows belongs to the next message.
- `HTTP_CHUNK_FULL`: `dst` is full. Use `f.produced` bytes, then call again
  with the unused part of the input.
- `HTTP_ERR_CHUNK` for a bad size, bad framing or a bare CR or LF, and
  `HTTP_ERR_BODY_TOO_LARGE` over the limit. An error is final.

`dst` may be the same buffer as `src`, which decodes in place. Chunk
extensions and trailers are checked for control bytes and skipped.

### URLs and queries

`http_url_parse(url: string, out: HttpUrl*) -> int64` returns 0 or
`HTTP_ERR_URL`. `HttpUrl` holds views: `scheme`, `userinfo`, `host` (no
brackets for IPv6, and `ipv6` is 1), `port` (80 for http and ws, 443 for https
and wss, if the URL has none), `path` (`"/"` if empty), `query`, `fragment`,
and `tls`. Only those four schemes parse. The URL must be ASCII with no spaces
or control bytes, and port 0 is refused.

| Function | Effect |
|----------|--------|
| `http_percent_decode(src, dst: uint8*, cap, plus_is_space: bool) -> int64` | Decodes `%XX` into `dst`; the length, `HTTP_ERR_ESCAPE` or `HTTP_ERR_TOO_SMALL` |
| `http_percent_encode(b: StrBuf*, s) -> int64` | Appends `s`, escaping all but letters, digits and `-._~` |
| `http_query_find(query, name) -> Option<string>` | Raw value of the first parameter called `name`; the name is decoded before the match |
| `http_query_get(query, name, dst, cap) -> int64` | The same, decoded (`+` is a space); `HTTP_ERR_ABSENT` if missing |
| `http_query_next(query, &pos, &name, &value) -> bool` | Steps through the pairs; start with `pos = 0` |

### Writers

All write into a `StrBuf` and return the bytes appended or a negative error.
Names must be tokens, and values and reason phrases must have no CR, LF, NUL or
other control byte, so a caller cannot smuggle in a second header or message.
The inputs are checked first, and on an error nothing is appended.

| Function | Effect |
|----------|--------|
| `http_write_request_head(b, verb, target, h: HttpHeader*, n) -> int64` | `METHOD target HTTP/1.1`, the headers, a blank line. Adds no headers itself |
| `http_write_response_head(b, status, reason, h, n) -> int64` | Status line and headers; an empty `reason` uses `http_status_text` |
| `http_write_header(b, name, value) -> int64` | One header line |
| `http_write_chunk(b, data: uint8*, n) -> int64` | One chunk; `n <= 0` writes nothing, since an empty chunk ends the body |
| `http_write_last_chunk(b)`, `http_write_last_chunk_trailers(b, h, n)` | The end of a chunked body |
| `http_status_text(code) -> string` | `"Not Found"` for 404; `"Unknown"` if not listed |
| `http_valid_token(s)`, `http_valid_field_value(s)` | The checks the writers use |
| `http_error_text(code) -> string` | A short message for any `HTTP_ERR_*` |

### Client and server pieces

`http_request(arena, verb, url, headers, nheaders, body, timeout_ms) ->
Result<HttpClientResponse*, string>` makes one blocking request. Only `http://`
works; `https://` gives the error `TLS is not supported yet`. It sends `Host`,
`Content-Length` (for a body, or for POST, PUT and PATCH) and `Connection:
close`, and reads the whole reply: `Content-Length`, chunked, or until close.
1xx replies are skipped. Redirects are not followed. A `Host` header of yours
replaces the default one; passing `Content-Length`, `Transfer-Encoding` or
`Connection` is an error. `timeout_ms` covers connect, send and the whole
reply (0 or less: no limit). A body over `HTTP_CLIENT_MAX_BODY` (64 MiB) fails.

`HttpClientResponse` has `status`, `reason`, `headers`/`nheaders` and `body`,
all in the arena. `http_get(arena, url, timeout_ms)` is the short form.

```mettle
match (http_get(arena, "http://127.0.0.1:8080/items?id=7", 3000)) {
  case Ok(r): { println("{r->status} {r->body}"); }
  case Err(e): { println("failed: {e}"); }
}
```

`http_read_head(fd, buf, cap, have, timeout_ms, &head_end) -> int64` is for
servers. It reads until `buf` holds a whole head and returns the bytes now in
`buf`, with `head_end` set to where the head ends. `have` is how many bytes are
already in `buf` (left over on a keep-alive connection, or 0). `timeout_ms` is
for the whole head, so a client that trickles bytes cannot hold the connection.
Then parse `buf[0..head_end]` with `http_parse_request`. It returns
`HTTP_ERR_CLOSED`, `HTTP_ERR_TIMEOUT`, `HTTP_ERR_HEAD_TOO_LARGE` or a parse
error for a bad byte. After an error, drop the connection.

## std/websocket

RFC 6455 WebSocket, as pure protocol code. It reads and writes no socket and
makes no OS call, so a server or a client can drive it from any event loop.
Linux, macOS and Windows, and it runs in the compile-time interpreter. No extensions are
negotiated, so the RSV bits must be zero.

Roles: `WS_ROLE_SERVER` parses frames from a client, which must be masked.
`WS_ROLE_CLIENT` parses frames from a server, which must not be.

### Handshake

`ws_accept_key(client_key, out: uint8*, cap) -> int64` writes the
Sec-WebSocket-Accept value (28 bytes) and returns its length, or -1 for an
invalid key (it must decode to 16 bytes). It uses a private SHA-1, exported as
`ws_sha1(p, n, out20)`. SHA-1 is here for the handshake only.

Server side:

| Function | Effect |
|----------|--------|
| `ws_check_request(verb, upgrade, connection, key, version) -> int32` | `WS_UPGRADE_OK` (0), or the status to refuse with: 405, 426 or 400. Pass `""` for a missing header |
| `ws_check_request_head(head) -> int32` | The same, from the request text. Needs HTTP/1.1 and one key |
| `ws_select_protocol(offered, supported) -> string` | The first offered subprotocol the server supports, or `""` |
| `ws_write_101(buf: StrBuf*, client_key, protocol) -> int32` | Appends the 101 response. `protocol` may be `""`. Returns 1, or 0 and the buffer is unchanged |
| `ws_write_refusal(buf, status) -> int32` | Appends a 400, 405 or 426 response (426 names version 13) |

Client side. The stdlib has no random source, so you draw 16 random bytes and
pass them in. Keep them: the answer is checked against them.

| Function | Effect |
|----------|--------|
| `ws_client_key(key16: uint8*, out, cap) -> int64` | The 24-byte Sec-WebSocket-Key text |
| `ws_write_request(buf, host, path, key16, protocols, origin) -> int32` | Appends the upgrade request. `protocols` and `origin` may be `""`. Refuses a control byte in a value, so a header cannot be injected |
| `ws_check_response(head, client_key, offered) -> int32` | 0, or `WS_HS_BAD_STATUS`, `_UPGRADE`, `_CONNECTION`, `_ACCEPT`, `_EXTENSION` or `_PROTOCOL` |

Helpers: `ws_find_head_end(buf, n)` gives the index past `\r\n\r\n`, or -1.
`ws_header_value(head, name)`, `ws_head_has_token(head, name, token)`,
`ws_has_token(list, token)` (case-insensitive, comma list) and
`ws_status_code(head)` read a head.

### Frames

`ws_parse_header(buf, n, role, max_payload, &h) -> int32` reads one frame
header from the front of `buf`. It returns `WS_NEED_MORE` (keep the bytes, read
more, call again from the same start), `WS_HEADER` or `WS_ERROR`. `WsHeader`
has `fin`, `opcode`, `masked`, `mask[4]`, `payload_len` and `header_len`. After
`WS_ERROR`, `error_code` is the close code to answer with and `error` says why.

It refuses: RSV bits set, unknown opcodes, a control frame that is fragmented
or longer than 125 bytes, a wrong mask bit for the role, a 64-bit length with
the top bit set, and a length that is not in its shortest form. Those are 1002.
A data frame over `max_payload` (0 means no limit) is 1009. Errors show up as
soon as the bytes that show them have arrived.

`ws_unmask(data, n, mask, offset)` XORs in place. `offset` is where `data`
sits in the payload, so a payload that arrives in pieces is unmasked piece by
piece.

`ws_header_write(out, cap, fin, opcode, len, mask) -> int64` writes a header
(`mask` is 0 for none, else four bytes). The writers `ws_write_frame(buf, fin,
opcode, payload, n, mask)`, `ws_write_text`, `ws_write_binary`, `ws_write_ping`
and `ws_write_pong` append a whole frame to a `StrBuf` and mask the payload when
`mask` is set. They return the bytes added, or -1 and the buffer is unchanged.
`ws_write_text` refuses text that is not UTF-8. For a fragmented message, send
the first piece with the text or binary opcode and `fin` false, then
`WS_OP_CONT` pieces, with `fin` true on the last.

### Close frames

`ws_close_code_valid(code)` is true for 1000-1003, 1007-1014 and 3000-4999.
`ws_close_encode(code, reason, out, cap)` builds a payload (reason up to 123
bytes of UTF-8). `ws_close_parse(p, n, &close)` reads one and returns 0, or the
code to answer with: 1002 for a one-byte payload or a bad code, 1007 for a
reason that is not UTF-8. `WsClose` has `code` (1005 when empty) and `reason`.
`ws_write_close(buf, code, reason, mask)` appends the frame.

### Messages

`WsAssembler` follows a stream of frames. It refuses a new data frame inside a
fragmented message and a continuation with nothing to continue, lets control
frames come between fragments, caps the size of a message, and checks text as
UTF-8 as the bytes arrive, even when a character is split across fragments. It
also refuses any frame after a close.

```mettle
var a: WsAssembler;
ws_assembler_init(&a, WS_ROLE_SERVER, 1048576, sink);   // sink: a StrBuf*
var used: int64 = 0;
var ev: int32 = ws_decode(&a, buf, n, &used);
```

`ws_decode` stops at each event and returns it: `WS_EV_MESSAGE`,
`WS_EV_PING`, `WS_EV_PONG` or `WS_EV_CLOSE`. It returns `WS_EV_NONE` when it
needs more bytes, and `WS_ERROR` on a protocol error (`a.error_code` is the
close code). `used` is how many bytes it took. Call it again with the bytes
after those, until none are left. A header is taken only when it is whole, so
keep the unused tail and put it in front of the next read. Payload bytes are
unmasked in place in `buf`.

The bytes of a data message collect in the sink, which is emptied when a
message starts. After `WS_EV_MESSAGE`, read `ws_assembler_message(&a)` and
`a.message_type` (`WS_OP_TEXT` or `WS_OP_BINARY`) before the next decode. After
a ping or pong, `ws_assembler_control(&a)` has its data (answer a ping with a
pong that carries it). After a close, `a.close` has the code and reason.

To handle one frame at a time, use `ws_parse_header` and then
`ws_assembler_begin(&a, &h)`, `ws_assembler_feed(&a, data, n)` for each piece
and `ws_assembler_end(&a)`, which returns the event. With a null sink, `feed`
is how you see the payload.

`WsUtf8` is the streaming validator behind this: `ws_utf8_reset`,
`ws_utf8_feed(&v, p, n)`, `ws_utf8_done(&v)`, and `ws_utf8_valid(p, n)` for one
buffer. It refuses overlong forms, surrogates and anything past U+10FFFF.

Not done: extensions such as permessage-deflate, and the connection state
machine around the close handshake (sending your own close and waiting for the
peer's). A caller does that with the pieces above.

## std/hpack

HPACK (RFC 7541), the header compression of HTTP/2. Linux, macOS and
Windows, no sockets, and it runs in the compile-time interpreter.

Errors are negative `int32` codes: `HPACK_ERR_TRUNCATED`, `_INTEGER`,
`_HUFFMAN`, `_INDEX`, `_SIZE_UPDATE`, `_TOO_LARGE` and `_NOSPACE`.
`hpack_error_name(code)` gives text. After a decode error the dynamic table is
out of step with the peer, so close the connection (HTTP/2 calls it
`COMPRESSION_ERROR`).

The building blocks work on raw bytes:

| Function | Effect |
|----------|--------|
| `hpack_int_encode(value: int64, prefix_bits: int32, flags: int32, out: uint8*, cap: int64) -> int64` | Prefix integer; bytes written, or -1 |
| `hpack_int_decode(src: uint8*, n: int64, prefix_bits: int32, value: int64*) -> int64` | Bytes used, or an error; values above `HPACK_INT_MAX` (2^32-1) are refused |
| `hpack_huffman_encode(src, n, dst, cap) -> int64` | Bytes written, or -1 |
| `hpack_huffman_encoded_len(src, n) -> int64` | Size of the Huffman form |
| `hpack_huffman_decode(src, n, dst, cap) -> int64` | Decoded length, or an error; with `dst` null it only checks |
| `hpack_string_encode(s: string, huffman: bool, out, cap) -> int64` | A string literal |
| `hpack_string_decode(src, n, dst, cap, out_len: int64*) -> int64` | Bytes used |

The Huffman decoder refuses EOS in the data, padding of more than 7 bits, and
padding that is not all ones.

`HpackTable` is the dynamic table, with a byte limit, eviction and size
updates. `hpack_table_init(&t, arena, capacity)` takes its storage from the
arena once; `capacity` is the most it can ever hold, and the limit starts at
`min(capacity, 4096)`. `hpack_table_get(&t, index, &field)` reads index 1..61
from the static table and 62 and up from the dynamic one, newest first. For a
dynamic entry the strings stay valid only until the next change.
`hpack_table_add`, `hpack_table_set_max` and `hpack_table_len` do the rest.

### Decoding

```mettle
var d: HpackDecoder;
hpack_decoder_init(&d, tables_arena, 4096, 16384);   // capacity, max list size
var h: HpackHeaders;
hpack_headers_init(&h, request_arena);
var err: int32 = hpack_decode(&d, block, block_len, &h);
for i in 0..h.count {
  // h.items[i].name, .value, .never_index
}
```

`hpack_decode` takes one complete header block (a HEADERS or PUSH_PROMISE
block with its CONTINUATIONs joined). Every string and the array of fields go
into the arena given to `hpack_headers_init`, so reset that arena between
blocks. Static-table strings are not copied.

The list is capped at `max_list_size` (name + value + 32 bytes for each field,
the way `SETTINGS_MAX_HEADER_LIST_SIZE` counts); zero picks 65536. A string
that would pass the cap is refused before it is copied. A size update is
accepted only before the first field and only up to the decoder's limit.
`hpack_decoder_set_limit(&d, n)` moves that limit when a new
`SETTINGS_HEADER_TABLE_SIZE` takes effect.

### Encoding

```mettle
var e: HpackEncoder;
hpack_encoder_init(&e, tables_arena, 4096, true);     // capacity, use Huffman
var o: HpackOut;
hpack_out_init(&o, buf, cap);
hpack_encode_begin(&e, &o);                           // queued size updates
hpack_encode_field(&e, &o, ":method", "GET", HPACK_ENC_INDEX);
hpack_encode_field(&e, &o, "authorization", token, HPACK_ENC_NEVER);
// o.len bytes of buf are the block
```

`hpack_encode_block(&e, &o, list, count)` does the begin and all the fields
for an array of `HpackHeader`. A field that matches a table entry exactly is
written as an index. Otherwise it is a literal, added to the table in
`HPACK_ENC_INDEX` mode, not added in `HPACK_ENC_NO_INDEX`, and in
`HPACK_ENC_NEVER` mode (for secrets) marked so that proxies do not add it
either; that mode never uses a full index. A field bigger than the whole table
is sent without indexing. Huffman is used for a string when it is no longer
than the raw form.

When the peer's `SETTINGS_HEADER_TABLE_SIZE` arrives, call
`hpack_encoder_set_table_size(&e, n)`. The size update is written by the next
`hpack_encode_begin`: the smallest size first if the limit dipped and came
back, then the last one.

If the buffer is full, `hpack_encode_field` returns `HPACK_ERR_NOSPACE` and
writes nothing; the table is not changed either. `hpack_encode_block` can fail
half way, with the table already moved, so give it room or drop the
connection.

## std/http2

HTTP/2 framing (RFC 9113) and gRPC message framing, over `std/hpack`. Linux, macOS and
Windows, no sockets, and it runs in the compile-time interpreter. It does not
track streams or connection state; it cuts frames out of bytes and builds them.

Builders (`h2_build_*`) write a whole frame into a buffer and return its size,
or -1 if it does not fit or an argument is out of range. Parsers (`h2_parse_*`)
take the frame header and the payload, fill a struct whose pointers point into
the payload (nothing is copied), and return 0 or an error code: `H2_NO_ERROR`,
`H2_PROTOCOL_ERROR`, `H2_FLOW_CONTROL_ERROR`, `H2_FRAME_SIZE_ERROR`,
`H2_COMPRESSION_ERROR` and the rest of RFC 9113 section 7. `h2_error_name` and
`h2_frame_type_name` give text.

Constants: `H2_PREFACE` (24 bytes), the frame types `H2_FRAME_DATA` to
`H2_FRAME_CONTINUATION`, flags `H2_FLAG_END_STREAM`, `_ACK`, `_END_HEADERS`,
`_PADDED` and `_PRIORITY`, and `H2_SETTINGS_*` identifiers.

### Reading frames

`h2_reader_next(&r, buf, n, &frame) -> int32` looks at the front of a buffer:

| Result | Meaning |
|--------|---------|
| `H2_READ_MORE` | Not enough bytes; `frame.need` is the least the buffer must hold |
| `H2_READ_FRAME` | A frame is ready: `frame.header` (`length`, `type`, `flags`, `stream_id`), `frame.payload`, and `frame.consumed` to drop |
| `H2_READ_PREFACE` | The client preface was read; drop `frame.consumed` |
| `H2_READ_ERROR` | A connection error; send GOAWAY with `frame.error` |

`h2_reader_init(&r, max_frame_size, expect_preface)` sets it up; a server
passes true. Asking again after `MORE` or `ERROR` is safe. As soon as the 9
header bytes are in, it checks the length against `max_frame_size`, that
stream 0 is used only by SETTINGS, PING and GOAWAY and never by DATA, HEADERS,
PRIORITY, RST_STREAM, PUSH_PROMISE or CONTINUATION, and that a header block is
followed only by CONTINUATION on the same stream. Unknown frame types come
through for you to ignore.

### Frames

| Function | Effect |
|----------|--------|
| `h2_frame_header_read(src) -> H2FrameHeader`, `h2_frame_header_write(h, out)` | The 9-byte header; the reserved bit is dropped |
| `h2_build_data(out, cap, stream, data, n, end_stream, pad)` / `h2_parse_data(h, payload, &H2Data)` | `pad` is -1 for none, or 0..255 |
| `h2_build_headers(out, cap, stream, block, n, end_stream, end_headers, pad, &H2Priority or 0)` / `h2_parse_headers` | Padding and priority |
| `h2_build_priority` / `h2_parse_priority` | A stream that depends on itself is an error |
| `h2_build_rst_stream` / `h2_parse_rst_stream` | |
| `h2_build_settings(out, cap, list, count)`, `h2_build_settings_ack` | |
| `h2_build_push_promise` / `h2_parse_push_promise` | |
| `h2_build_ping(out, cap, data8, ack)` / `h2_parse_ping` | |
| `h2_build_goaway` / `h2_parse_goaway` | Last stream, error code, debug data |
| `h2_build_window_update` / `h2_parse_window_update` | A zero increment is `H2_PROTOCOL_ERROR` |
| `h2_build_continuation` / `h2_parse_continuation` | |

`h2_is_stream_error(h, err)` says whether an error from a parser is a stream
error (reset that stream with `RST_STREAM`) or a connection error (GOAWAY).
One case it cannot see: a HEADERS frame whose priority depends on its own
stream is a stream error, so compare `H2Headers.priority.dependency` with the
stream id yourself.

`h2_check_frame_length(h, max_frame_size)` is the size check on its own.
Builders do not check the peer's `SETTINGS_MAX_FRAME_SIZE`; split big payloads
yourself.

### Settings and flow control

`H2Settings` holds the six settings, and `h2_settings_defaults(&s)` fills in
the protocol defaults. `h2_settings_apply_frame(&s, h, payload)` checks a whole
SETTINGS frame and then stores it, all or nothing. It returns `H2_FRAME_SIZE_ERROR`
for a length that is not a multiple of 6 or an ACK with a payload,
`H2_PROTOCOL_ERROR` for `ENABLE_PUSH` above 1 or `MAX_FRAME_SIZE` outside
16384..16777215, and `H2_FLOW_CONTROL_ERROR` for `INITIAL_WINDOW_SIZE` above
2^31-1. Unknown settings are ignored. `h2_setting_check(id, value)` and
`h2_settings_entry(payload, i)` work one setting at a time.

`H2Window` is a flow-control window:

| Function | Effect |
|----------|--------|
| `h2_window_init(&w, size)` | Start (`H2_DEFAULT_WINDOW` is 65535) |
| `h2_window_credit(&w, n) -> int32` | Add a WINDOW_UPDATE; past 2^31-1 is `H2_FLOW_CONTROL_ERROR` |
| `h2_window_debit(&w, n) -> int32` | Take bytes out; more than it holds is `H2_FLOW_CONTROL_ERROR` |
| `h2_window_adjust(&w, delta) -> int32` | A `SETTINGS_INITIAL_WINDOW_SIZE` change; may go below zero |
| `h2_window_available(&w) -> int64` | Bytes that may be sent now |

A failed call leaves the window as it was.

### gRPC

A gRPC message is a 5-byte prefix (a compressed flag and a 32-bit big-endian
length) and the message. `h2_grpc_write(out, cap, msg, n, compressed)` writes
both. `h2_grpc_read(buf, n, max_message, &m)` returns `H2_GRPC_READ_MORE`
(with `m.need`), `H2_GRPC_READ_MESSAGE` (with `m.data`, `m.length`,
`m.compressed`, `m.consumed`) or `H2_GRPC_READ_ERROR`. A flag byte other than
0 or 1 gives `m.status = H2_GRPC_INTERNAL`, and a length over `max_message`
(`H2_GRPC_DEFAULT_MAX_MESSAGE` is 4 MiB) gives `H2_GRPC_RESOURCE_EXHAUSTED`,
as soon as the prefix is in.

`h2_grpc_timeout_parse("100m") -> int64` reads a `grpc-timeout` value into
milliseconds, rounded up, or -1 if it is not valid. `h2_grpc_timeout_format(ms,
out, cap) -> int64` writes one, using the coarsest unit that is exact (`5S`,
`2M`, `100m`) and moving up a unit, rounded up, when it would pass 8 digits.

`H2_GRPC_OK` to `H2_GRPC_UNAUTHENTICATED` are the `grpc-status` codes.
`h2_grpc_status_name(code)`, `h2_grpc_status_parse(text)` (-1 if invalid) and
`h2_grpc_status_from_http(http_status)` (the mapping for a response with no
`grpc-status`) round them out.

## std/file

Files by descriptor, for programs that manage their own storage: reads and
writes at an offset, sync, truncate, rename, remove and locks. Linux, macOS
and Windows. Every call returns a count, size or descriptor, or a negative
error code (`-FILE_ENOENT`, `-FILE_EACCES`, `-FILE_EEXIST`,
`-FILE_EWOULDBLOCK`, `-FILE_EINTR`, or another platform code). On Linux the
calls are raw system calls.

`file_open(path: string, flags: int32) -> int64` takes `FILE_READ`,
`FILE_WRITE`, `FILE_CREATE`, `FILE_TRUNCATE`, `FILE_APPEND` and `FILE_NEW`
(with `FILE_CREATE`, fail if the file exists). New files get mode 0644, and
the descriptor is close-on-exec. On Windows the result is a `HANDLE`, and
`FILE_APPEND` has no effect.

| Function | Effect |
|----------|--------|
| `file_read_at(fd, buf: uint8*, n: int64, offset: int64) -> int64` | Read at an offset without moving the file position. 0 is end of file |
| `file_write_at(fd, buf, n, offset) -> int64` | Write at an offset |
| `file_sync(fd)` | Flush data and metadata to the device (`F_FULLFSYNC` on macOS) |
| `file_sync_data(fd)` | Flush data (`fdatasync` on Linux) |
| `file_truncate(fd, size)`, `file_size(fd)` | Set or read the length |
| `file_rename(from, to)` | Rename, replacing `to` |
| `file_remove(path)` | Delete |
| `file_lock(fd, FILE_LOCK_SHARED or FILE_LOCK_EXCLUSIVE, wait: bool)` | Lock the whole file. With `wait` false, a conflict fails at once with `-FILE_EWOULDBLOCK` |
| `file_unlock(fd)`, `file_close(fd)` | Release |

Locks are `flock` locks on Linux and macOS: advisory, and held per open, so a
second `file_open` of the same file in the same process conflicts too. On
Windows they are `LockFileEx` locks, which are mandatory.

**Memory maps.** `file_map(fd, offset, length, flags, &m: FileMap) -> int64`
maps part of an open file. `flags` is `FILE_MAP_READ`, plus `FILE_MAP_WRITE`
for writes that reach the file, plus `FILE_MAP_PRIVATE` to keep those writes in
this process instead. Any offset works: `m.data` points at the byte asked for,
and `m.length` is the length. The file must already be long enough, so grow it
with `file_truncate` first. `file_map_sync(&m)` writes a shared map's changes
to the file; on Windows it does not wait for the device, so call `file_sync`
as well. `file_unmap(&m)` releases it, and a second call does nothing.

## std/dir

Paths and directories. Windows.

| Function | Effect |
|----------|--------|
| `dir_exists(path: cstring) -> int32` | 1 when it does |
| `dir_create(path: cstring) -> int32` | Create one |
| `file_exists(path: cstring) -> int32` | 1 when it does |
| `getcwd(buf: cstring, size: int32) -> int32` | Working directory |
| `dir_list_md_files(root_dir, paths_buf, paths_size, max_files) -> int32` | List `.md` files under a root |

## std/process

Start programs, wait for them, kill them, and capture their output. Linux,
macOS and Windows. Every call returns 0, a count, a flag, or a negative error
code (`-PROC_ENOENT`, `-PROC_EACCES`, `-PROC_EINVAL`, `-PROC_ETIMEDOUT`, or
another platform code). On Linux the calls are raw system calls.

`exit(code: int32)`, `rand() -> int32` and `srand(seed: int32)` are still
here, and `std/prelude` still takes them from this module.

### Starting a program

Fill a `ProcOptions` and call `proc_spawn`.

```mettle
var argv: string[2] = ["/bin/echo", "hello"];
var o: ProcOptions;
proc_options_init(&o, argv);
o.stdout = PROC_PIPE;
var p: Proc;
if (proc_spawn(&o, &p) != 0) { return 1; }
// read p.stdout_fd with proc_read ...
var st: ProcStatus;
proc_wait(&p, &st);
proc_close(&p);
```

| Field | Meaning |
|-------|---------|
| `argv: string[]` | `argv[0]` is the program. With no `/` in it, `PATH` is searched |
| `env: string[]`, `has_env: int32` | `"NAME=value"` entries. With `has_env` 0 (the default) the child inherits the environment. With 1 it gets `env` alone, and an empty `env` means an empty environment |
| `cwd: string` | The child's working directory. Empty keeps ours |
| `stdin`, `stdout`, `stderr: int32` | `PROC_INHERIT` (default), `PROC_PIPE`, or `PROC_NULL`. `stderr` may also be `PROC_TO_STDOUT` |

`proc_options_init(o, argv)` zeroes the options and sets `argv`.

`proc_spawn(opts, out: Proc*) -> int64` returns 0 or a negative code. If the
program does not exist it returns `-PROC_ENOENT`, and if it cannot be run
`-PROC_EACCES`. On Linux and macOS the child reports the real `errno`
through a close-on-exec pipe, so a failed `execve` is seen by the parent
before `proc_spawn` returns. A bad `cwd` fails the same way.

`Proc` has `pid`, and `stdin_fd`, `stdout_fd`, `stderr_fd`: our ends of the
pipes, or -1. Pipe ends are close-on-exec. The child's signal mask is
cleared, so signals blocked by `proc_signal_fd` do not reach it.

On Linux the inherited environment is the one the process started with
(`/proc/self/environ`); variables set later with `putenv` are not passed on.

### Waiting and killing

`ProcStatus` has `signaled` (1 if a signal killed the process), `code` (the
exit code when `signaled` is 0) and `signal` (the signal number when
`signaled` is 1).

| Function | Effect |
|----------|--------|
| `proc_wait(p, st) -> int64` | Block until the child ends. Returns 0 and fills `st` (which may be 0) |
| `proc_try_wait(p, st) -> int64` | Do not block. 1 if it ended (`st` filled), 0 if still running |
| `proc_wait_timeout(p, ms, st) -> int64` | Wait up to `ms` (negative waits forever). 1 if it ended, 0 on timeout. It does not kill the child. On Linux and macOS it checks every 1 to 10 ms |
| `proc_kill(pid: int64, sig: int32) -> int64` | Send a signal. Signal 0 only checks that the process exists |
| `proc_send(p, sig) -> int64` | The same for a `Proc`. Gives `-PROC_ESRCH` once the child was reaped, so a reused pid is never hit |
| `proc_pid()`, `proc_parent_pid()` | Our ids |

A wait reaps the child and the answer is kept in `Proc`, so waiting again
returns the same status. The signal constants are `PROC_SIGHUP`, `PROC_SIGINT`,
`PROC_SIGKILL`, `PROC_SIGUSR1`, `PROC_SIGUSR2`, `PROC_SIGPIPE`, `PROC_SIGTERM`
and `PROC_SIGCHLD`; their values follow the platform.

### Pipes

| Function | Effect |
|----------|--------|
| `proc_read(fd, buf: uint8*, n) -> int64` | One read. 0 is end of file |
| `proc_write(fd, buf, n) -> int64` | One write. `-PROC_EPIPE` if the child closed its end. It does not kill your program with `SIGPIPE` |
| `proc_close_stdin(p)` | Close the child's stdin, so it sees end of file |
| `proc_close(p)` | Close every pipe end still open |

### Running to completion

`proc_run(argv: string[], stdin_text: string, out: StrBuf*, err: StrBuf*,
timeout_ms: int64, st: ProcStatus*) -> int64` starts the program with all
three streams piped, feeds `stdin_text` and closes stdin, and appends stdout
to `out` and stderr to `err` (either may be 0 to discard). It polls all the
pipes together, so a large output cannot stall the child. It returns 0 when
the program ended, a spawn error, or `-PROC_ETIMEDOUT` if `timeout_ms`
(negative: no limit) passed. On timeout the child is killed with `SIGKILL`
and reaped, and `st` says so. A non-zero exit code is not an error: look at
`st`.

`proc_communicate(p, stdin_text, out, err, timeout_ms, st)` does the same for
a `Proc` you spawned yourself. It uses only the pipes that exist, and closes
them all before it returns.

### Signals as readiness

There are no handlers. `proc_signal_fd(signals: int32[..]) -> int64` returns a
descriptor that becomes readable when one of the signals arrives. Poll it with
`sock_poll`, then call `proc_signal_read(fd) -> int32` for the signal number
(it blocks if none is waiting; a negative result is an error).
`proc_signal_release(fd, signals...)` closes the descriptor and gives the
signals back.

```mettle
var fd: int64 = proc_signal_fd(PROC_SIGINT, PROC_SIGTERM);
// poll fd for NET_POLL_IN in the event loop, then:
var sig: int32 = proc_signal_read(fd);
```

On Linux the signals are blocked in the calling thread and the descriptor is a
`signalfd`. Call it before starting threads, so every thread blocks them.

### macOS

The calls are libc calls (`fork`, `execve`, `waitpid`). `pipe` has no
close-on-exec flag, so the flag is set right after; a `fork` on another
thread in between could leak an end. `proc_write` ignores `SIGPIPE` for the
duration of the call, for the whole process.

Signals use a self-pipe: a handler writes the signal number as one byte, and
the read end is the descriptor. There is one pipe for the process, so every
`proc_signal_fd` call returns the same descriptor, and `proc_signal_release`
frees it for all signals.

### Windows

The calls are Kernel32 calls (`CreateProcessA`, `CreatePipe`,
`WaitForSingleObject`). The command line is built from `argv` with the MSVCRT
quoting rules, so an argument with spaces or quotes arrives intact; Windows
searches `PATH` and adds `.exe`. Pipe ends are `HANDLE`s.

There are no signals. `proc_kill` and `proc_send` end the process at once
whatever the signal is (signal 0 only checks that it runs), and the exit
code is 1: `signaled` is never 1. `proc_signal_fd` and `proc_signal_read`
return `-PROC_ENOSYS`. `proc_communicate` feeds stdin from a helper thread and
checks the output pipes every millisecond. A `PROC_INHERIT` stream is shared
only if the parent's own handle is inheritable.

## std/system

`system(cmd: cstring) -> int32` runs a shell command.

## std/bench

`bench_time_us() -> int64` reads a microsecond clock. Both platforms, with a
`.linux` half.

## std/time

Clocks, durations and dates. Linux, macOS and Windows.

`std/time` holds the clocks and sleep, and imports `std/datetime`, which holds
everything else. `std/datetime` reads no clock, so code that must run in the
compile-time interpreter can import it alone. The OS calls live in
`std/time_os`.

A duration is an `int64` count of nanoseconds. Build one from the units
`TIME_NANOSECOND`, `TIME_MICROSECOND`, `TIME_MILLISECOND`, `TIME_SECOND`,
`TIME_MINUTE`, `TIME_HOUR` and `TIME_DAY`: `(int64)5 * TIME_SECOND`.

| Function | Effect |
|----------|--------|
| `time_unix_ns() -> int64` | Wall clock, nanoseconds since 1970-01-01 UTC |
| `time_unix()`, `time_unix_ms()`, `time_unix_us()` | The same in seconds, ms and µs |
| `time_mono_ns() -> int64` | Monotonic clock; only differences mean anything |
| `time_mono_us()`, `time_mono_ms()` | The same in µs and ms |
| `time_since_ns(start: int64) -> int64` | Nanoseconds since a `time_mono_ns()` reading |
| `time_sleep_ns(ns: int64)` | Sleep at least `ns`; Windows rounds up to whole ms |
| `time_sleep_us(us: int64)`, `time_sleep_ms(ms: int64)` | The same in µs and ms |

Dates use the proleptic Gregorian calendar. `DateTime` holds `year`, `month`
(1-12), `day`, `hour`, `minute`, `second`, `nanos`, `weekday` (0 is Sunday),
`yearday` (1-366) and `offset_min`, the zone's offset from UTC in minutes. The
other fields are local to that offset.

| Function | Effect |
|----------|--------|
| `time_utc(unix_ns: int64) -> DateTime` | Break a time into UTC fields |
| `time_at_offset(unix_ns: int64, offset_min: int32) -> DateTime` | The same at a fixed offset |
| `time_now_utc() -> DateTime` | The current UTC date and time (`std/time`) |
| `time_date(year, month, day, hour, minute, second) -> DateTime` | Build a UTC `DateTime`; out-of-range parts roll over |
| `time_to_unix(dt) -> int64`, `time_to_unix_ns(dt) -> int64` | Back to unix seconds or nanoseconds |
| `time_days_from_civil(year: int64, month: int32, day: int32) -> int64` | Days since 1970-01-01 |
| `time_weekday_of_days(days: int64) -> int32` | Weekday of a day count |
| `time_is_leap_year(year: int64) -> bool` | Gregorian leap year |
| `time_days_in_month(year: int64, month: int32) -> int32` | 28 to 31, or 0 for a bad month |
| `time_floor_div(a, b)`, `time_floor_mod(a, b)` | Division that rounds down, for times before 1970 |

The formatters write into a caller buffer, add a nul, and return the length,
or -1 if the buffer is too small or the year is outside 0-9999.
`TIME_FORMAT_MAX` (48) bytes is always enough.

| Function | Output |
|----------|--------|
| `time_format_iso8601(dt, frac_digits: int32, buf: uint8*, cap: int64) -> int64` | `2026-11-05T09:30:15.123Z`, or `+01:00` for a non-zero offset |
| `time_format_date(dt, buf, cap) -> int64` | `2026-11-05` |
| `time_format_clock(dt, buf, cap) -> int64` | `09:30:15` |
| `time_format_http(unix_ns, buf, cap) -> int64` | `Thu, 05 Nov 2026 09:30:15 GMT` |
| `time_format_duration(ns, buf, cap) -> int64` | `1h2m3.5s`, `1.5ms`, `250us`, `0s` |

`time_append_iso8601(b, dt, frac_digits)`, `time_append_http(b, unix_ns)` and
`time_append_duration(b, ns)` append the same text to a `StrBuf*` and return 1,
or 0 on failure.

`time_parse_iso8601(s: string) -> Result<DateTime, string>` reads a date
(`2026-11-05`) or a date-time with optional seconds, fraction and zone
(`Z`, `+01:00`, `+0100`, `+01`). A space may stand in for the `T`, and no zone
means UTC. `time_parse_http(s: string) -> Result<int64, string>` reads an HTTP
date and returns unix nanoseconds.

## std/gpu

Device memory, streams, and launches for the [GPU targets](gpu.md).

## std/tracy

Zones and frame marks for the Tracy profiler. Needs
[`--tracy`](compilation.md) at build time.

## std/ui

Win32 window and message-loop helpers. Windows.

## std/win32

Raw Win32 declarations the other Windows modules build on.

## std/live

`live_tick(tick: int64)` tells a [live build](live.md) which tick it is on. A
patch built with `--live-at-tick N` waits until the tick reaches `N`.

## std/prelude

Imports `std/core`, `std/io`, `std/math`, `std/conv`, `std/mem`, and
`std/process`, and re-exports them. [`--prelude`](compilation.md) imports it
for you. Networking stays out, because it is platform-split.

## See also

- [Heap allocation](heap-allocation.md)
- [C interoperability](c-interop.md)
- [Modules and imports](modules.md)
