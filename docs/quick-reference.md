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

## String Interpolation

```mettle
import "std/io";

fn main() -> int32 {
  var n: int32 = 42;
  var who: string = "world";
  println("hello {who}, n={n}, twice={n * 2}");
  return 0;
}
```

`{expr}` in any string literal splices integers, `bool`, floats, and strings;
`{{` is a literal `{`. See [Expressions](expressions.md#string-interpolation).

## With Prelude

```mettle
// Compile with: mettle --prelude --build main.mettle -o main.exe
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

## With Constant Tables

Arrays and structs are written with an [aggregate literal](expressions.md#aggregate-literals). At global scope the value is laid out in the object file's data, so a lookup table costs nothing at startup. `[value; count]` repeats one value.

```mettle
struct Pt {
  x: float64;
  y: float64;
}

struct Ops {
  fast: fn(int32) -> int32;
  slow: fn(int32) -> int32;
}

fn triple(v: int32) -> int32 { return v * 3; }
fn quadruple(v: int32) -> int32 { return v * 4; }

const SHIFTS: int32[8] = [7, 12, 17, 22, 5, 9, 14, 20];
const SCRATCH: uint8[64] = [0; 64];
const CORNERS: Pt[2] = [{ x: 0.0, y: 0.0 }, { x: 1.0, y: 1.0 }];
const DISPATCH: Ops = { fast: &triple, slow: &quadruple };

fn main() -> int32 {
  var local: int32[4] = [1, 2, 3, 4];  // copied in from the same constant
  return SHIFTS[0] + (int32)CORNERS[1].x + DISPATCH.fast(1) + local[3];
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

To see what the optimizer decided about **every** loop and call in your file,
with no annotations needed, compile with `--explain` (`-O`/`--release`):

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
instructions, with the fallbacks grouped by cause, largest functions first,
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

A fix the compiler can prove carries a `verified:` line. It applies the change
to an internal clone, re-runs its own optimizer on it, and prints the line only
when a kernel claimed the loop,

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
dot-product row-pointer hoist, the single-region fill, the body-local hoist,
the single float width, and call-in-the-loop fixes. For the last of those the
compiler pretend-applies the decorator change, re-runs its own **inliner** on
a clone of the caller, and proves lines like `simulated removing @noinline
from damp ... this loop then vectorizes → vfmadd231ps`. When a simulation
proves the standard advice is *unwritable* (e.g. the index math genuinely
changes every iteration, a real non-unit-stride access), the advice is
replaced with that finding rather than printed. When the change applies and
the loop still stays scalar, the advice is tagged `(first step only)` and a
`still blocked:` line names what surfaces next.

The diagnoses outside that library carry advice the compiler cannot simulate.
It is checked a different way: `tests/test_explain_fix_advice.mettle` holds
each suggestion beside the loop it is given for and beside that loop with the
suggestion carried out, and the suite fails if the second half stops
vectorizing. Advice that names no edit to the loop in front of it, such as the
checklist printed when no cause could be identified, appears as `note:` and
stays out of "where to start".

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
budget). `@noalloc` proves the function, and everything it can reach through
direct calls, performs zero heap allocations: `new`, string `+`, allocator
calls, unprovable externs, and function-pointer calls inside the reachable
graph are all compile errors pointing at the offending site. Verified
`@noalloc` functions are reported in `--explain`.

## GPU kernel and dispatch

```mettle
// kernels.mettle  ->  mettle --emit-ptx kernels.mettle -o kernels.ptx
kernel vadd(a: float32*, b: float32*, c: float32*, n: int32) {
  workgroup var tile: float32[256];
  private var scratch: int32[4];
  var i: int32 = block.x * block_dim.x + thread.x;
  if (thread.x < 256 && i < n) { tile[thread.x] = a[i] + b[i]; }
  barrier(workgroup, acq_rel);
  if (thread.x < 256 && i < n) { c[i] = tile[thread.x]; }
}
```

```mettle
// host.mettle  ->  mettle --build host.mettle -o host --link-arg .../cuda.lib
import "std/gpu";
extern kernel(block = 256) vadd(a: float32*, b: float32*, c: float32*, n: int32);

if (gpu_open("kernels.ptx") == 0) { return 1; }
// ... gpu_malloc, gpu_to_device ...
dispatch vadd[work: n](da, db, dc, n);

dispatch gemm[grid: (tiles_n, tiles_m, batch),
              block: (32, 1, 1),
              shared: arena_bytes,
              stream: compute_stream](a, b, c, d, m, n, k);
```

`mettle --gpu-info` reports the local card, the driver, and the `.target`
`--emit-ptx` picks for it; `--gpu-arch=native` demands that card rather than
falling back to the cross-compile default.

Declared with `extern kernel`, `vadd` is checked against the declaration and
resolved by name; written as a plain `int64` it is the runtime handle
`gpu_func` returns and nothing is checked. `dispatch` preserves
a typed, target-neutral launch in IR; the host backend alone marshals it for
the selected runtime provider. The compact form supplies 1-D grid/block sizes.
The named form requires `grid: (x,y,z)` and `block: (x,y,z)` and optionally
accepts `shared:` dynamic-arena bytes plus `stream:`; controls may be reordered.
libmtlc's `mtlc_gpu_launch` builds the identical operation.

`workgroup var name: T[N]` is fixed storage shared by a workgroup;
`private var name: T[N]` is fixed storage per work-item. A pointer-shaped
`workgroup var arena: T*` is an unbounded view of launch-sized workgroup memory;
multiple typed views alias one base and are partitioned with aligned offsets.
All are kernel-only. `barrier(workgroup, global,
acq_rel)` accepts one or both memory regions and an acquire/release/acq-rel/
seq-cst order; all live work-items must reach it uniformly.

Native subgroup built-ins expose `subgroup_local_id`, `subgroup_size`, typed
`subgroup_broadcast(value, lane)`, add/min/max reductions, and inclusive or
exclusive add scans for u32/f32, variable-source `subgroup_shuffle`,
word-addressed `subgroup_ballot`, and boolean `subgroup_any` / `subgroup_all`.
No declaration is required. Collectives must be uniform, and a broadcast source
lane must be uniform and valid. PTX uses a 32-lane warp; SPIR-V keeps the
implementation-defined OpenCL subgroup size and uses KHR ballot/vote extensions.

Native atomic built-ins are type-directed for `uint32*`, `uint64*`, and matching
workgroup arrays. Loads and value-returning operations need no extern; stores
return `void`:

```mettle
var ticket: uint32 = atomic_fetch_add(counter, index, 1,
                                      order: acq_rel, scope: device);
var ready: uint32 = atomic_load(flags, index,
                                order: acquire, scope: device);
atomic_store(flags, index, 1, order: release, scope: device);
var observed: uint64 = atomic_compare_exchange(
    state, index, expected, desired,
    success_order: seq_cst, failure_order: acquire, scope: system);
```

The full family is `atomic_load`, `atomic_store`,
`atomic_fetch_add/sub/min/max/and/or/xor`, `atomic_exchange`, and
`atomic_compare_exchange`. Storage is inferred as global or workgroup;
`space:` may state it explicitly. Defaults are seq-cst and device scope for
global storage or workgroup scope for workgroup storage. Loads accept
relaxed/acquire/seq-cst; stores accept relaxed/release/seq-cst. CAS failure
order is distinct and cannot be release/acq-rel or stronger than success.

Native cooperative tensor operations use named, compile-time semantics rather
than backend fragments or opcodes:

```mettle
kernel tile(a: uint16*, b: uint16*, c: float32*, d: float32*) {
  tensor_mma(a, b, c, d,
             shape: m16n16k16,
             input_type: f16, output_type: f32,
             a_layout: row, b_layout: col);
}

kernel gemm_region(a: uint16*, b: uint16*, c: float32*, d: float32*,
                   m: uint32, n: uint32, k: uint32,
                   lda: uint32, ldb: uint32, ldc: uint32, ldd: uint32) {
  tensor_matmul(a, b, c, d,
                (uint32)block.y * (uint32)16,
                (uint32)block.x * (uint32)16,
                m, n, k,
                shape: m16n16k16,
                input_type: f16, output_type: f32,
                lda: lda, ldb: ldb, ldc: ldc, ldd: ldd);
}

kernel finish(d: float32*, bias: float32*, alpha: float32, beta: float32) {
  tensor_epilogue(d, shape: m32n16, element_type: f32,
                  bias_mode: column, bias: bias,
                  alpha: alpha, beta: beta, activation: relu);
}
```

`shape` can be replaced by `m`/`n`/`k`; independent operand/result formats,
all four layouts/leading dimensions, transpose, rounding, overflow, math,
sparsity, scale operands, and subgroup/workgroup scope are represented. Leading
dimensions may be compile-time integers or uniform runtime integer expressions.
`tensor_matmul(A,B,C,D,row,column,M,N,K,...)` computes one exact bounded
whole-matrix region. Its five controls are unsigned and uniform; all four
whole-matrix leading dimensions are explicit. PTX keeps full runtime-K chunks
resident when legal and cooperatively computes every M/N/K edge. PTX can view
independently transposed A/B through the opposite backend-local layout; forced
fallback still replays transpose exactly, and neither path creates a frontend
transpose. Unscaled E4M3/E5M2 inputs have exact architectural conversion and
cooperative edge replay plus resident direct-MMA full interiors. Matched
block32/UE8M0 FP8/FP6/FP4 descriptors and packed block32/UE8M0 or
block16/UE4M3 E2M1 descriptors likewise keep scaled native interiors resident
and replay every M/N/K edge exactly. `scale_A` is logical row-major
`[M,ceil(K/block)]`; `scale_B` is logical column-major
`[ceil(K/block),N]`, independently of operand transpose. Dense FP6/FP4 is an
LSB-first logical bitstream. Matching f16/bf16 structured-2:4 regions use two
stored A values and one uint8 mask per logical four-wide K group. Metadata is
compact row-major `[M,ceil(K/4)]`, does not transpose with A, and a partial final
group still owns two stored values; PTX keeps complete K16 `mma.sp` chunks
resident and replays every edge from the neutral masks. Unsupported numeric or
sparse tail families reject rather than round, pad, or silently narrow.
`tensor_epilogue` is a separate exact `activation(alpha*D + beta*bias)`
collective with row/column D layout, absent/row/column/matrix bias,
identity/ReLU/clamp, f16/bf16/f32/f64 storage, and subgroup/workgroup scope.
ReLU and clamp use ordered comparisons and preserve unordered values.
PTX always has ordered cooperative memory replay and may consume an adjacent
compatible MMA/commit directly. It may also carry a verified loop accumulator
across its exit jump when that jump is the epilogue label's only predecessor;
bypass edges force replay. Stable opaque WMMA permits bias-free scalar post-ops;
direct mapped MMA permits every bias mode. No fragment enters shared IR.

The PTX backend supports the stable WMMA family and rejects unsupported profiles;
the current OpenCL 2.0 SPIR-V profile explicitly rejects unsupported tensor
MMA and epilogue capabilities.

See [GPU Offload](gpu.md).

## With Generics

Generic functions and structs with compile-time monomorphization. See [Declarations](declarations.md#generic-functions) and [Types](types.md#generic-type-parameters).

```mettle
struct Pair<A, B> {
  first: A;
  second: B;
}

fn swap<T>(a: T*, b: T*) -> void {
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
