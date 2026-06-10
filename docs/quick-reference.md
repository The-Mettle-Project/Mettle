# Quick Reference

Short examples for common use cases.

## Minimal Program

```mettle
function main() -> int32 {
  return 0;
}
```

## With Imports

```mettle
import "std/io";

function main() -> int32 {
  println("Hello, Mettle!");
  return 0;
}
```

See [Imports](imports.md) for path resolution and `import_str`.

## With Prelude

```mettle
// Compile with: mettle --prelude main.mettle -o main.s
function main() -> int32 {
  println("Hello");
  return 0;
}
```

## With Extern

```mettle
extern function puts(msg: cstring) -> int32 = "puts";

function main() -> int32 {
  puts("Hello");
  return 0;
}
```

## With Enum and Switch

```mettle
enum Status { Ok = 0, Error = 1 }

function main() -> int32 {
  var s: Status = Ok;
  switch (s) {
    case 0:
      return 0;
    default:
      return 1;
  }
}
```

## With Explicit Casts

```mettle
function main() -> int32 {
  var f: float64 = 3.14;
  var i: int32 = (int32)f;
  
  var p: int32* = (int32*)0;
  var address: int64 = (int64)p;
  
  return i;
}
```

## With Heap Allocation and Structs

Uses `new` for zero-initialized heap allocation. The emitted code calls `calloc(1, n)` directly; no Mettle runtime object is linked unless the program also uses `-d`/`-s` crash tracebacks or `std/thread` atomics. See [Heap Allocator Runtime](heap-allocation.md).

```mettle
struct Point {
  x: int32;
  y: int32;
}

function main() -> int32 {
  var p: Point* = new Point;
  p->x = 10;
  p->y = 20;
  return p->x + p->y;
}
```

## Range-based for and `@simd`

```mettle
function dot(a: int8*, b: int8*, n: int32) -> int32 {
  var s: int32 = 0;
  @simd! for i in 0..n {              // 0..n exclusive; 0..=n inclusive
    s = s + (int32)a[i] * (int32)b[i];
  }
  return s;                           // @simd! = compile error if it can't vectorize
}
```

`@simd` is a best-effort hint (warns if not vectorized); `@simd!` is a hard
contract. Both are checked under `-O`/`--release`; add `--simd-report` to see
what each loop became. See [Control Flow](control-flow.md#vectorization-contracts).

To see what the optimizer decided about **every** loop and call in your file —
no annotations needed — compile with `--explain` (`-O`/`--release`):

```
saxpy (loop @ line 12): vectorized → vfmadd231ps, 8-wide float32 affine map
matvec (loop @ line 38): NOT vectorized
    └ reason: this is a float multiply-accumulate (dot-product shape), but no
      kernel matched its address pattern
    └ fix: hoist invariant index math into a pointer before the loop
main (call to `opaque` @ line 74): NOT inlined
    └ reason: the callee is marked @noinline
```

Nests are summarized (`vectorized inner, scalar outer`), fully unrolled loops
say so, and a backend section reports which functions got the
register-allocating backend (and why the rest fell back).

## Function decorators

```mettle
@inline   function f(x: int32) -> int32 { return x * 3; }   // force inline
@noinline function g() -> int32 { return 1; }               // never inline
@pure @noinline function w(t: int32*, k: int32) -> int32 { /* ... */ }
@simd!    function s(a: int32*, n: int64) -> int64 { /* every body loop must vectorize */ }
```

Prefix a function with `@inline`/`@noinline` (inlining control), `@pure`
(side-effect-free → loop-invariant calls hoisted out of loops), or
`@simd`/`@simd!` (vectorization contract on every body loop). Decorators stack,
take effect under `-O`/`--release`, and apply to functions only. See
[Declarations](declarations.md#function-decorators).

## GPU kernel and dispatch

```mettle
// kernels.mettle  ->  mettle --emit-ptx kernels.mettle -o kernels.ptx
kernel vadd(a: float32*, b: float32*, c: float32*, n: int32) {
  var i: int32 = block.x * block_dim.x + thread.x;
  if (i < n) { c[i] = a[i] + b[i]; }
}
```

```mettle
// host.mettle  ->  mettle --build host.mettle -o host --link-arg .../cuda.lib
import "std/gpu";
// ... gpu_init, gpu_module, gpu_func, gpu_malloc, gpu_to_device ...
dispatch vadd[(n + 255) / 256, 256](da, db, dc, n);
```

See [GPU Offload](gpu.md).

## With Generics

Generic functions and structs with compile-time monomorphization. See [Declarations](declarations.md#generic-functions) and [Types](types.md#generic-type-parameters).

```mettle
struct Pair<A, B> {
  first: A;
  second: B;
}

function swap<T>(a: T*, b: T*) -> void {
  var tmp: T = *a;
  *a = *b;
  *b = tmp;
}

function main() -> int32 {
  var p: Pair<int32, int32>;
  p.first = 10;
  p.second = 20;
  swap<int32>(&p.first, &p.second);
  return p.first + p.second;
}
```
