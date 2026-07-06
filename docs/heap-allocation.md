# Heap Allocation

Mettle has no garbage collector and no heap manager. `new`, array literals that need heap storage, and string concatenation lower to a direct C runtime call:

```asm
calloc(1, size)
```

There is no tracing, root scanning, safepoint, shutdown sweep, or Mettle-side allocator state. The operating system reclaims the memory at process exit unless user code explicitly manages a buffer through `std/mem`.

For the wider picture (helper objects, what `--build` links, the runtime-model story), see [Runtime Model](runtime-model.md). This page covers only the language semantics of `new` and where allocation happens.

## When to use `new`

```mettle
struct Point { x: int32; y: int32; }

fn main() -> int32 {
  var p: Point* = new Point;
  p.x = 10;
  p.y = 20;
  return p.x + p.y;
}
```

- Returns a pointer to a zero-initialized region of memory sized for the type.
- The memory comes from libc `calloc`; failure semantics follow `calloc` (returns `null`, no exception).
- Lifetime is "until process exit" unless `free` is called explicitly through `std/mem`.

## When to use `std/mem` instead

Use `std/mem` (`malloc`/`calloc`/`realloc`/`free`) for:

- Buffers with explicit lifetime.
- C interop where ownership crosses the boundary.
- Long-running ownership where leaks matter (servers, long-lived processes).

## String concatenation

`string + string` allocates a new buffer through the same `calloc(1, n)` call and copies both sides into it. This is a normal heap allocation; no separate string runtime exists.

```mettle
var greeting: string = "Hello, " + name;
```

## Linking impact

`new` and string concatenation **do not link any Mettle helper objects**. The emitted code calls libc `calloc` directly. A program that uses `new` heavily but does not enable crash tracebacks (`-d`/`-s`/`-g`) or `std/thread` atomics links zero Mettle runtime objects.

If you build manually:

```bash
mettle main.mettle -o main.obj
gcc -nostartfiles main.obj -o main.exe -lkernel32
```

For the rules around the two opt-in helper objects, see [Runtime Model — Helper objects](runtime-model.md#helper-objects).
