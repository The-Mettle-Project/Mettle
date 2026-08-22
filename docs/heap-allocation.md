# Heap allocation

How to put things on the heap and get them back off it. Mettle has no garbage
collector: memory you take, you return.

Every example here compiles and runs.

## The short answer

One value of a struct type:

```mettle
var n: Node* = new Node;
free(n);
```

Anything else, including arrays of any type:

```mettle
var xs: int32* = malloc(count * sizeof(int32));
free(xs);
```

`new` and `free` come from nowhere; `malloc` and its family come from
[`std/mem`](standard-library.md). Both release with `free`. There is no
`delete`.

## One struct

`new T` allocates one zeroed `T` and gives you a `T*`:

```mettle
import "std/io";
import "std/mem";

struct Node { value: int32; next: Node*; }

fn main() -> int32 {
  var n: Node* = new Node;
  n->value = 7;
  println("{n->value} next_is_null={n->next == 0}");
  free(n);
  return 0;
}
```

```text
7 next_is_null=true
```

The memory arrives zeroed, which is why `n->next` is already a null pointer.
`new` takes a struct type only: `new int32[8]` is rejected. Arrays go through
`malloc`.

## An array

Ask for the element count times the element size. `sizeof(T)` gives you the
second half:

```mettle
import "std/io";
import "std/mem";

fn main() -> int32 {
  var count: int64 = 8;
  var xs: int32* = malloc(count * sizeof(int32));
  if (xs == 0) { return 1; }
  defer free(xs);

  for i in 0..8 { xs[i] = i * i; }
  println("{xs[7]}");
  return 0;
}
```

```text
49
```

The pointer is typed, so `xs[i]` steps by the element size and needs no cast in
either direction. Sizes are `int64`; see [Sizes](#sizes) for the one cast that
is needed.

## A zeroed array

`calloc(count, size)` allocates and zeros in one step, which is what you want
for an array of structs you are about to fill in part:

```mettle
import "std/io";
import "std/mem";

struct Point { x: int32; y: int32; }

fn main() -> int32 {
  var n: int64 = 4;
  var pts: Point* = calloc(n, sizeof(Point));
  if (pts == 0) { return 1; }
  defer free(pts);

  for i in 0..4 { pts[i].x = i; pts[i].y = i * 10; }
  println("{pts[3].x},{pts[3].y} first_was_zero={pts[0].x == 0}");
  return 0;
}
```

```text
3,30 first_was_zero=true
```

`malloc` does not zero. `calloc` and `new` do.

## Growing one

`realloc` resizes, and may move the block. Assign its result to a new variable
first, because on failure it returns 0 and leaves the original alive: writing
`xs = realloc(xs, ...)` would lose the only pointer you had to it.

```mettle
import "std/io";
import "std/mem";

fn main() -> int32 {
  var cap: int64 = 2;
  var xs: int32* = malloc(cap * sizeof(int32));
  if (xs == 0) { return 1; }
  xs[0] = 1;
  xs[1] = 2;

  cap = cap * 4;
  var grown: int32* = realloc(xs, cap * sizeof(int32));
  if (grown == 0) { free(xs); return 1; }
  xs = grown;
  defer free(xs);

  for i in 2..8 { xs[i] = i; }
  println("{xs[0]} {xs[7]}");
  return 0;
}
```

```text
1 7
```

After a successful `realloc` the old pointer is stale. Any pointer you took
into the old block, `&xs[4]` for instance, is stale too, and using one is
reported as [M0111](memory-safety.md). Re-derive them from the new base.

## Releasing it

`defer free(p)` is the reliable form: it runs on every path out of the scope,
including an early `return` and a `break`.

```mettle
var xs: int32* = malloc(n * sizeof(int32));
if (xs == 0) { return 1; }
defer free(xs);
```

Put the `defer` immediately after the null check, so every path below it is
covered and none above it needs to be.

Free a buffer with the API that made it. A `malloc`, a `calloc`, a `realloc`,
or a `new` pairs with `free`. A `mem_alloc(h, ...)` pairs with
`mem_free(h, ...)`. An arena allocation is released with the whole arena. Do
not free stack storage, and do not free the same pointer twice.

## Structures that link

A list, a tree, or a graph is `new` per node and a walk to release them. Take
the next pointer before freeing the node holding it:

```mettle
import "std/io";
import "std/mem";

struct Node { value: int32; next: Node*; }

fn main() -> int32 {
  var head: Node* = 0;
  for i in 0..4 {
    var n: Node* = new Node;
    n->value = i;
    n->next = head;
    head = n;
  }

  var walk: Node* = head;
  while (walk != 0) {
    print("{walk->value} ");
    walk = walk->next;
  }
  println("");

  while (head != 0) {
    var next: Node* = head->next;
    free(head);
    head = next;
  }
  return 0;
}
```

```text
3 2 1 0
```

A null pointer is the integer `0`, so `head = 0` starts an empty list and
`walk != 0` ends the traversal.

## Strings

Concatenation allocates, and the result is an ordinary `string`:

```mettle
var greeting: string = "hello, " + name;
```

Building text piece by piece is better served by
[`std/strbuf`](standard-library.md) over an arena, which grows one buffer
instead of allocating per join.

Handing a string to C needs a nul-terminated copy, and `cstr` takes the
allocator to make it from, so the free is yours:

```mettle
var c: cstring = cstr(greeting, &malloc);
defer free(c);
```

[C interoperability](c-interop.md) covers that boundary.

## Sizes

Sizes are `int64`. Literals, `sizeof(T)`, arithmetic over them, and every
integer type up to `uint32` pass with no cast, because those widen. `uint64`
needs one, since the conversion can change the value and Mettle
[narrows loudly](types.md). In practice that means `string.length`:

```mettle
var copy: rawptr = malloc((int64)s.length + 1);
```

## Failure is a null pointer

Every allocator returns `0` when it cannot satisfy the request. Test before
you use the result:

```mettle
var buf: rawptr = malloc(1024);
if (buf == 0) { return 1; }
```

The compiler reports a dereference of a pointer it can prove is null, as
[M0113](memory-safety.md), but it cannot prove anything about a value that
depends on how much memory the machine had. That check is yours.

## Choosing an allocator

Four surfaces, in the order you should reach for them.

| Use | When |
|-----|------|
| `new T` | One struct, and you want it zeroed |
| `malloc` / `calloc` / `realloc` / `free` | Arrays, and anything whose lifetime you manage individually |
| [`std/arena`](standard-library.md) | Many allocations with one common end |
| [`std/alloc`](standard-library.md) | You need a separate heap, or its statistics |

### An arena

An arena bump-allocates and releases everything at once, which suits work with
one clear end: a request, a frame, a parse. Nothing is freed individually.

```mettle
import "std/io";
import "std/arena";

struct Point { x: int32; y: int32; }

fn main() -> int32 {
  var a: Arena* = arena_init(4096);
  defer arena_free(a);

  var pts: Point* = arena_alloc(a, 64 * sizeof(Point));
  if (pts == 0) { return 1; }
  for i in 0..64 { pts[i].x = i; }

  var mark: ArenaSave = arena_save(a);
  var scratch: uint8* = arena_alloc(a, 1024);
  if (scratch == 0) { return 1; }
  arena_restore(a, mark);

  println("{pts[63].x}");
  return 0;
}
```

```text
63
```

`arena_save` and `arena_restore` roll back to a point, which is how a
temporary working set costs nothing to release. `arena_reset` empties the
arena and keeps its chunks for reuse; `arena_free` returns everything.

### A heap of your own

`std/alloc` is Mettle's own thread-safe allocator, written in Mettle. Reach
for it when one heap for the whole program is the wrong shape, or when you
want the counters:

```mettle
import "std/io";
import "std/alloc";

fn main() -> int32 {
  var h: MemHeap* = mem_heap_create();
  if (h == 0) { return 1; }
  defer mem_heap_destroy(h);

  var buf: rawptr = mem_alloc(h, 4096);
  if (buf == 0) { return 1; }
  mem_free(h, buf);

  println("heap ok");
  return 0;
}
```

The `mx_` names, `mx_alloc` and friends, are the same allocator over one
shared default heap, for when you want it without carrying a handle.

## Where the memory comes from

On Windows the runtime asks the process heap. On Linux it maps anonymous pages
and manages the blocks itself, because a Linux build links no libc.

`--native-heap` routes `new` and the `malloc` family through
[`std/alloc`](standard-library.md) in place of the runtime's own path.

## What the compiler checks

Freeing twice, using a pointer after it is freed, leaking an allocation that
never escapes its function, and using a pointer into a block that was freed or
moved are all reported while compiling. [Memory safety](memory-safety.md)
lists the codes; [Borrow checker](borrow-checker.md) covers the derived-pointer
cases.

Add [`--safe`](memory-safety.md) for bounds checks at run time on the accesses
the compiler could not settle statically.

## Closures

A capturing closure allocates an environment where the lambda is written, and
it lives as long as the closure value. [Declarations](declarations.md) covers
capture.

## See also

- [Memory safety](memory-safety.md)
- [Standard library](standard-library.md)
- [C interoperability](c-interop.md)
