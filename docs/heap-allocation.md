# Heap allocation

Mettle has no garbage collector. Memory you take from the heap you give back,
and the compiler tells you when it can see that you did not.

## The allocators

Three surfaces reach the heap, and they layer on each other.

`new T` allocates one zeroed `T` and gives you a `T*`:

```mettle
struct P { x: int32; }
```

```mettle
var p: P* = new P;
p->x = 3;
```

`std/mem` exports the C-shaped names, backed by the owned runtime:

```mettle
import "std/mem";
```

```mettle
var p: int32* = malloc(16);
p[0] = 7;
free(p);
```

`malloc`, `calloc`, `realloc`, and `free` are declared there.

[`std/alloc`](standard-library.md) is the allocator itself, written in Mettle.
It adds explicit heaps, size classes, and statistics. Use it when one heap for
the whole program is the wrong shape:

```mettle
import "std/alloc";
```

```mettle
var h: MemHeap* = mem_heap_create();
var buf: rawptr = mem_alloc(h, 4096);
mem_free(h, buf);
mem_heap_destroy(h);
```

[`std/arena`](standard-library.md) bump-allocates and frees a whole region at
once, which suits work with one clear end: a request, a frame, a parse.

## Where the memory comes from

On Windows the runtime asks the process heap for raw storage. On Linux it maps
anonymous pages and manages the blocks itself. Neither path calls a C
allocator, because a Linux build links no libc.

`--native-heap` routes `new`, `malloc`, `calloc`, `realloc`, and `free`
through [`std/alloc`](standard-library.md) in place of the runtime's own
path.

## Failure is a null pointer

Allocation failure gives back `0`. Test before you use it:

```mettle
var buf: rawptr = mx_alloc(1024);
if (buf == 0) { return 1; }
```

## Types and sizes

Both surfaces speak `rawptr`: an allocator hands out an address with no
element type and a deallocator takes one. `rawptr` converts to and from every
pointer type, so neither `var a: int32* = malloc(n)` nor `free(a)` needs a
cast.

Sizes are `int64`. Literals, `sizeof(T)`, arithmetic over them, and every
integer type up to `uint32` pass with no cast, because those widen. `uint64`
needs one, since the conversion can change the value and Mettle
[narrows loudly](types.md). In practice that means `string.length`:

```mettle
var copy: rawptr = malloc((int64)s.length + 1);
```

## Zeroed by default

`new` gives zeroed memory. So does a local declared without an initializer,
array or struct alike. `calloc` and `mx_calloc` zero what they hand out;
`malloc` and `mx_alloc` do not.

## Pairing

Free a buffer with the API that made it. A `malloc` pairs with `free`, a
`mem_alloc(h, ...)` with `mem_free(h, ...)`, and an arena allocation with
`arena_reset` or `arena_free` over the whole arena. Do not free stack storage
or an OS handle.

`defer` is the natural place to put the release, because it runs on every path
out of the scope:

```mettle
var p: int32* = malloc(16);
defer free(p);
```

## What the compiler checks

Freeing twice, or reading after a free, is reported while compiling. See
[Memory safety](memory-safety.md) for the full list and for `--safe`, which
adds run-time bounds checks.

Closures allocate an environment where the lambda is written, and it lives as
long as the closure value. [Declarations](declarations.md) covers capture.

## See also

- [Memory safety](memory-safety.md)
- [Borrow checker](borrow-checker.md)
- [Standard library](standard-library.md)
