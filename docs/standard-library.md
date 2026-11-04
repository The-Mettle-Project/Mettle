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

`METTLE_PARALLEL_THREADS` overrides the core count at run time, capped at 16.

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
`sock_connect(fd, &addr)`, `sock_send(fd, buf, n)`, `sock_recv(fd, buf, n)`,
`sock_sendto`, `sock_recvfrom`, `sock_shutdown(fd, NET_SHUT_READ, _WRITE or
_BOTH)`, `sock_local_addr`, `sock_peer_addr`, and `sock_take_error` (the
pending error, after a non-blocking connect).

Sending never raises SIGPIPE; a closed peer gives `-NET_EPIPE`.

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

`http_fetch_to_file(url: cstring, output_path: cstring) -> int32` downloads a
URL to a file.

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

Three C entry points: `exit(code: int32)`, `rand() -> int32`, and
`srand(seed: int32)`.

## std/system

`system(cmd: cstring) -> int32` runs a shell command.

## std/bench

`bench_time_us() -> int64` reads a microsecond clock. Both platforms, with a
`.linux` half.

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
