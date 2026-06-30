# Quick Reference

Short examples for common use cases.

## Minimal Program

```mettle
fn main() -> int32 {
  return 0;
}
```

## With Imports

```mettle
import "std/io";

fn main() -> int32 {
  println("Hello, Mettle!");
  return 0;
}
```

See [Imports](imports.md) for path resolution and `import_str`.

## With Prelude

```mettle
// Compile with: mettle --prelude main.mettle -o main.s
fn main() -> int32 {
  println("Hello");
  return 0;
}
```

## With Extern

```mettle
extern fn puts(msg: cstring) -> int32 = "puts";

fn main() -> int32 {
  puts("Hello");
  return 0;
}
```

## With Enum and Switch

```mettle
enum Status { Ok = 0, Error = 1 }

fn main() -> int32 {
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
fn main() -> int32 {
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

fn main() -> int32 {
  var p: Point* = new Point;
  p->x = 10;
  p->y = 20;
  return p->x + p->y;
}
```

## Range-based for and `@simd`

```mettle
fn dot(a: int8*, b: int8*, n: int32) -> int32 {
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
say so, and a backend section reports register-allocation coverage weighted by
instructions — with the fallbacks grouped by cause, largest functions first,
and a `consequence:`/`fix:` line per cause (e.g. a function containing a SIMD
kernel still runs the kernel at full vector speed; only its scalar code
spills):

```
  1742/1850 functions reaching codegen compiled with the register-allocating backend
  97.2% of the program's 215,012 optimized IR instructions are in register-allocated code

  contains a call form the register allocator doesn't support yet (39 functions, 8,120 instructions):
      └ consequence: every value in the function is kept on the stack instead of in registers
      └ largest: editor_tick (1480), vk_scene_build (912) ...
```

Reports past ~200 lines (real applications) are written to
`<output-stem>.explain.txt` next to the output binary, with a five-line digest
on stderr (`METTLE_EXPLAIN_REPORT_LINES` overrides the threshold; `0` never
diverts).

Fix suggestions are **verified, not guessed**, where the compiler can prove
them: it applies the suggested change to an internal clone, re-runs its own
optimizer on it, and only then prints a `verified:` line —

```
sum_bytes (loop @ line 27): NOT vectorized
    └ reason: this is a byte-sum loop, but the vpsadbw kernel accumulates
      into int64 and this loop's accumulator is narrower
    └ fix: declare the accumulator as int64
    └ verified: simulated that fix and re-ran the optimizer: this loop
      then vectorizes → vpsadbw, 32-wide byte sum (AVX2)
```

The same applies to inlining advice: "mark it @inline" is re-checked with the
decorator pretend-applied, and when a hidden structural guard means it would
NOT help, the report says that instead of giving advice that won't work.

The verified library covers element-width fixes (int16/int64 → int32),
accumulator widening (int32 and byte sums need an int64 accumulator), the
dot-product row-pointer hoist, and call-in-the-loop fixes — for those the
compiler pretend-applies the decorator change, re-runs its own **inliner** on
a clone of the caller, and proves lines like `simulated removing @noinline
from damp ... this loop then vectorizes → vfmadd231ps`. And when a simulation
proves the standard advice is *unwritable* (e.g. the index math genuinely
changes every iteration — a real non-unit-stride access), the advice is
replaced with that finding rather than printed.

## Function decorators

```mettle
@inline   fn f(x: int32) -> int32 { return x * 3; }   // force inline
@inline!  fn l(a: float32, b: float32) -> float32 {   // CONTRACT: every call
  return a + b;                                             // inlines, or compile error
}
@noinline fn g() -> int32 { return 1; }               // never inline
@pure @noinline fn w(t: int32*, k: int32) -> int32 { /* ... */ }
@noalloc  fn hot(x: float32) -> float32 {             // CONTRACT: proven
  return x * x;                                             // allocation-free
}
@simd!    fn s(a: int32*, n: int64) -> int64 { /* every body loop must vectorize */ }
```

Prefix a function with `@inline`/`@noinline` (inlining control), `@pure`
(side-effect-free → loop-invariant calls hoisted out of loops), or
`@simd`/`@simd!` (vectorization contract on every body loop). Decorators stack,
take effect under `-O`/`--release`, and apply to functions only. See
[Declarations](declarations.md#function-decorators).

The `!` decorators are **contracts**: the compiler either delivers the
optimization or fails the build with the precise reason. `@inline!` errors on
any call site that survives inlining (e.g. recursion, the caller's growth
budget). `@noalloc` proves the function — and everything it can reach through
direct calls — performs zero heap allocations: `new`, string `+`, allocator
calls, unprovable externs, and function-pointer calls inside the reachable
graph are all compile errors pointing at the offending site. Verified
`@noalloc` functions are reported in `--explain`.

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

fn main() -> int32 {
  var p: Pair<int32, int32>;
  p.first = 10;
  p.second = 20;
  swap<int32>(&p.first, &p.second);
  return p.first + p.second;
}
```
