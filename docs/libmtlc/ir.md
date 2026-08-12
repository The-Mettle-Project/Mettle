# The libmtlc IR model

This is the contract between a frontend and the backend: what a module is made
of, what each instruction means, the rules control flow must follow, and the
shape each consumer of the IR expects. It is written against the builder API
(`mtlc/build.h`); the internal C structs behind it are not part of the public
surface.

## Shape of a module

A module is:

- an ordered list of **functions**, each a linear instruction stream (there is
  no nesting; basic blocks are implied by labels and terminators);
- a **type registry**: a name-to-descriptor table (`"int64"` maps to the int64
  descriptor, `"float32*"` to the pointer descriptor) that code generators use
  to resolve any type name the IR carries;
- a **module symbol table**: one entry per function and global with its kind,
  type signature, extern flag, link name, and (for globals) the folded
  constant initializer.

`mtlc_builder_finish` populates both tables from what you declared; a frontend
never fills them by hand.

## Values

A function body computes over two kinds of values:

- **Temporaries.** Every value-producing instruction (`mtlc_binary`,
  `mtlc_call`, `mtlc_load`, ...) defines a fresh single-assignment temporary.
  Temps are not storage: you cannot assign to them, and their handle is only
  the name of that one computed value.
- **Storage.** Parameters, locals (`mtlc_local`), and global references
  (`mtlc_global_ref`) name mutable slots. Reading the handle reads the current
  value; `mtlc_assign` writes it. Storage is how values cross basic blocks:
  the IR has no phi nodes, so a value needed after a branch merge must live in
  a local (the optimizer and register allocator recover registers from that
  shape).

Literals (`mtlc_const_int`, `mtlc_const_float`) are values too; they may be
used any number of times.

All handles are scoped to their `MtlcFn`. Nothing value-like crosses function
boundaries; cross-function data flows through calls, globals, and memory.

## The instruction set

What the builder emits, in terms of the semantics codegen implements:

| Builder call | Meaning |
|---|---|
| `mtlc_local(name, T)` | declare a mutable local of type `T` (recorded by type name; resolved via the registry) |
| `mtlc_address_space_alloc(name, E, n, space)` | static kernel storage; returns an exact workgroup/private `E*` |
| `mtlc_dynamic_workgroup_view(name, E)` | unbounded typed `E*` view of the launch-provided workgroup arena; every view in one kernel aliases one base |
| `mtlc_assign(dest, v)` | store `v` into the storage `dest` names |
| `mtlc_binary(op, a, b, T)` | `t = a op b` with result type `T` baked on the instruction |
| `mtlc_unary(op, a, T)` | `t = op a` (`-`, `!`, `~`) |
| `mtlc_call(f, args, n, R)` | `t = f(args...)`; name resolved against the module symbol table |
| `mtlc_intrinsic(id, args, n, R)` | target-neutral semantic GPU operation; backend switches on `id`, not a source name |
| `mtlc_intrinsic_memory(id, ..., space, order, scope)` | memory-bearing semantic operation with an explicit address space and concurrency contract |
| `mtlc_workgroup_barrier(order, regions)` | uniform execution barrier with explicit workgroup/global memory publication |
| `mtlc_async_copy_workgroup(...)` / `commit` / `wait` | balanced per-work-item global-to-workgroup staging groups with explicit transaction width and cache hint |
| `mtlc_tensor_transfer_workgroup(desc, operands)` | uniform rank-1..5 global/workgroup tile movement with exact logical geometry, zero-fill/discard bounds, and optional opaque acceleration view |
| `mtlc_tensor_mma(desc, A, B, C, D)` / `mtlc_tensor_mma_ex(...)` | whole-tile cooperative `D = A*B+C` with exact shape, format, layout, stride, numeric, sparsity, scaling, and collective semantics |
| `mtlc_tensor_matmul(desc, operands)` | exact bounded whole-matrix region with unsigned row/column origins and runtime M/N/K; full native chunks and every M/N/K edge share one neutral operation |
| `mtlc_tensor_epilogue(desc, operands)` | cooperative in-place `activation(alpha*D + beta*bias)` with exact logical layout, broadcast/matrix bias, activation, scalar, stride, and scope semantics |
| `mtlc_gpu_launch(...)` | typed asynchronous host launch with 3-D geometry, shared bytes, stream, and exact kernel argument ABI types |
| `mtlc_cast(v, T)` | `t = (T) v` (width/sign change, int/float, int/pointer) |
| `mtlc_address_of(s, PT)` | `t = &s` for a local or parameter, typed `PT` |
| `mtlc_load(p, E)` | `t = *(E *)p`; element size/floatness/signedness recorded |
| `mtlc_store(p, v, E)` | `*(E *)p = v` |
| `mtlc_label(L)` | define label `L` here |
| `mtlc_jump(L)` | unconditional transfer to `L` |
| `mtlc_branch_if_zero(c, L)` | transfer to `L` when `c == 0`, else fall through |
| `mtlc_return(v)` | return `v` (or nothing with `MTLC_NO_VALUE`) |

Semantics worth spelling out:

- **Result types drive selection.** `mtlc_binary`'s `result_type` decides
  everything the instruction encoder needs: operand width, signed vs unsigned
  `/ % < <= > >=` and `>>`, and float vs integer arithmetic. The backend never
  re-infers types from context, so a wrong `result_type` is a silent
  miscompile, not an error.
- **Comparisons produce 0/1 integers.** `==`,`!=`,`<`,`<=`,`>`,`>=` yield an
  integer 0 or 1 (give them an integer result type). `&&` and `||` here are
  **bitwise** over those 0/1 values, not short-circuit: both operands are
  always evaluated. A frontend that needs C-style short-circuit evaluation
  lowers it with branches (pattern below).
- **Integer overflow wraps.** Arithmetic is native two's complement; there are
  no traps and no undefined-overflow assumptions in the backend.
- **Atomic width is not inferred from the result.** The intrinsic identity
  carries u32/u64 independently of whether the operation returns the old value
  or `void` (store). This keeps load/store/RMW/CAS lowering identical for every
  frontend and prevents a u64 store from being narrowed through a void result.
- **Division by zero** is whatever the target does (a fault on the CPU
  targets). The backend inserts no checks; frontends that promise checks emit
  them as IR.
- **Loads extend by element signedness.** A `uint8/16/32` load zero-extends
  into the 64-bit temp; a signed load sign-extends. That is why `mtlc_load`
  takes the element type rather than a byte count.
- **Pointers are integers with a type.** Pointer arithmetic is ordinary
  integer arithmetic on the address value; scale indexes by the element size
  yourself. `mtlc_cast` converts between pointer and integer freely.
- **Asynchronous staging is replayable and CFG-balanced.**
  `IR_OP_ASYNC_COPY`, `IR_OP_ASYNC_COMMIT`, and `IR_OP_ASYNC_WAIT` carry
  matching scalar types, global/workgroup provenance, element count,
  4/8/16-byte transaction size, cache hint, and pending-group bound. A
  set-valued dataflow verifier tracks every reachable `(pending,
  uncommitted)` state and requires every exit to be fully drained. Native PTX
  may issue `cp.async`; another backend may perform the copies synchronously.
  Completion is per work-item, so cross-item consumption still requires an
  ordered workgroup barrier. Optimizer-generated copies carry provenance only
  for dumps/diagnostics; their semantics are identical to explicit copies.
- **Tensor transfers remain replayable.** `IR_OP_TENSOR_TRANSFER` carries the
  complete rank, direction, element, bounds, scope, global extents/byte strides,
  tile extents, and element strides plus typed raw pointers, signed coordinates,
  and an optional opaque prepared view. It never carries a CUDA tensor-map,
  transaction barrier, or shared bank layout. Shared verification requires
  workgroup-uniform execution and operands. A backend may use the view only as
  an acceleration precondition for the same descriptor; PTX otherwise performs
  cooperative rank-aware replay, while an unsupported SPIR-V profile rejects.
- **Tensor epilogues do not expose fragments.** `IR_OP_TENSOR_EPILOGUE` carries
  a complete logical MxN descriptor plus typed destination/bias pointers,
  selected compute scalars, and runtime strides. Shared verification checks the
  exact operand signature and scope-uniform execution. PTX replays it as an
  ordered cooperative memory loop or consumes a compatible resident MMA/commit
  behind the same semantics. A loop handoff additionally requires its exit jump
  to be the epilogue label's only predecessor; the shared operation never gains
  a fragment field. Current SPIR-V rejects explicitly.
- **Subgroups are implementation-sized.** The semantic intrinsic family carries
  local ID, size, uniform u32/f32 broadcast, varying-source shuffle,
  add/min/max reductions, inclusive/exclusive add scans, word-addressed ballot,
  and any/all votes without naming a warp or wave. Collectives must
  be uniform; the broadcast lane must be uniform and valid. Integer sums wrap
  and floating collective order is not promised to be bit-identical across
  targets. Reductions and votes are subgroup-uniform; shuffle and scan results
  are work-item-varying. Ballot requests a 32-bit word so wider masks are not
  silently narrowed; an unavailable word is zero.
- **Tensor operations are descriptor-exact.** `IR_OP_TENSOR_MMA` carries a
  target-neutral `MtlcTensorMmaDesc`, typed memory operands, and a canonical
  A/B/C/D sequence of any runtime leading dimensions. Its neutral tile count is
  one by default; a count above one concatenates equal-sized operand bundles and
  means the exact sequential chain `D=A0*B0+C; D=A1*B1+D; ...`. Shared
  verification requires one output, later C/D connectivity, compatible
  accumulator/result semantics, and identical output strides. A zero descriptor
  stride means a runtime integer operand; a nonzero stride is an embedded
  constant. It does not carry PTX fragments or vendor opcodes. Every live
  invocation in the chosen scope executes the same whole tile uniformly.
  A backend may subdivide that logical M/N tile into exact physical tiles and
  reuse input fragments, but it must cover the full descriptor shape and honor
  its layouts/strides. PTX does this for stable byte-addressable WMMA grids;
  the grid and A/B traversal never appear in shared IR.
  Structured sparse A is stored at half logical K width, with a uint8
  row-major occupancy mask for each logical group and values in increasing
  selected-index order. This representation is shared semantics; no PTX
  selector or lane packing appears in the descriptor or operands.
  Backends may replay a chain or keep an accumulator resident, but cannot change
  format, layout, leading dimension, transpose, rounding, overflow, sparsity,
  scaling, or scope to make a profile fit. Unsupported combinations are
  compilation errors. Residency metadata has an explicit neutral `LOOP` or
  `PIPELINE` scope. A loop group tags one initial MMA and one connected body
  update, with `IR_OP_TENSOR_COMMIT` inserted only on the loop-exit edge. A
  pipeline group tags an initial MMA and one or more connected straight-line
  updates, with every transition separated by an ordered async wait and
  workgroup publication barrier and commit immediately after the final update.
  This metadata is replayable: a backend may execute the original
  MMAs normally and treat commit as a no-op. A retaining backend may delay D
  only after the shared verifier rechecks unique group membership,
  descriptor/stride connectivity, and the scope-specific CFG or handoff shape.
  The public builder triggers both transforms through ordinary operations; no
  frontend-private annotation is required.
- **Whole-problem regions are exact and separate.** `IR_OP_TENSOR_MATMUL`
  carries one ordinary MMA operand bundle followed by unsigned row origin,
  column origin, and problem M/N/K. The descriptor M/N bound the output region;
  descriptor K is a preferred exact native chunk. In-range outputs cover all
  runtime K, K=0 copies C, and out-of-range M/N coordinates are no-ops. All four
  whole-matrix leading dimensions are explicit. Shared verification checks
  pointer/control types and scope uniformity, while deliberately keeping this
  operation out of single-tile chain/residency rewrites. PTX owns the choice of
  resident stable-WMMA/native scaled full chunks versus exact 64-bit-address
  cooperative edges. The ordinary matrix bundle carries optional scale pointers
  and semantic scale strides or the existing neutral sparse metadata pointer.
  Canonical A-row/B-column scale grids, packed bitstreams, compact sparse
  `[M,ceil(K/4)]` masks, target conversion availability, and guarded contiguous
  loads are backend interpretations rather than new IR fields. For structured
  2:4, A's stride counts two stored values per logical group and metadata stays
  logical under transpose. Unsupported numeric/sparse tails are capability
  errors; no shared field names WMMA, a warp, `mma.sp`, or an NVIDIA fragment.
- **Collective uniformity is scope-sensitive.** Before PTX or SPIR-V emission,
  the shared device call graph classifies values as workgroup-uniform,
  subgroup-uniform, or work-item-varying and marks control-dependent CFG
  regions. A subgroup collective may execute under subgroup-uniform control; a
  workgroup barrier may not. Helper argument ranks are joined from all reachable
  call sites. Broadcast lanes and tensor pointers are checked at their required
  scope, so the guarantee is neither frontend-specific nor limited to direct
  kernel bodies.

## Control flow

The rules the instruction stream must satisfy:

1. **Labels are function-scoped** and must be unique within the function.
   Forward references are fine (jump first, define later).
2. **A label starts a basic block.** Falling into a label from the preceding
   instruction is allowed and means what it looks like.
3. **`mtlc_branch_if_zero` is the only conditional.** Taken on zero, falls
   through on nonzero.
4. **Every path must end at `mtlc_return`.** The builder does not synthesize
   returns, and a body that can run off the end is invalid. The cheap
   guarantee is an unconditional `mtlc_return` at the end of lowering (dead if
   unreachable; the optimizer removes it).

### Lowering patterns

The three structures every frontend needs, in the exact shape the reference
frontend and `examples/calc` emit. `if`/`else`:

```
    branch_if_zero cond -> L_else
    ...then body...
    jump L_end
L_else:
    ...else body...
L_end:
```

`while`:

```
L_top:
    cond = ...recompute condition...
    branch_if_zero cond -> L_end
    ...body...
    jump L_top
L_end:
```

Short-circuit `a && b` as a value (evaluate `b` only when `a` is nonzero):

```
    r = local int
    assign r, 0
    branch_if_zero a -> L_done
    t = (b != 0)
    assign r, t
L_done:
    ...use r...
```

`break`/`continue` are jumps to the loop's `L_end`/`L_top`. Arbitrary CFGs are
legal (the IR does not require structured control flow); reducibility is only
required in practice because every consumer handles what structured lowering
produces, and irreducible graphs are untested territory.

## Module tables

At `mtlc_builder_finish`:

- Every distinct `MtlcType` that passed through the builder is registered in
  the **type registry** under its canonical name (plus all scalar names, always).
  This is how a `DECLARE_LOCAL` carrying `"int64*"` or a parameter list carrying
  `"float32"` resolves to layout during code generation.
- Each declared function becomes a **module symbol** with kind, extern flag,
  return type, and parameter types. Each global becomes a variable symbol with
  its type and folded initializer. Code generators read this table to emit
  global storage, classify call arguments, and decide what is defined here
  versus imported.

Calls resolve by name against this table at code-generation time. A call to a
name with no symbol and no body is an error at emit time, not at build time.

Semantic host launches are different from calls. They survive frontend
lowering as `IR_OP_GPU_LAUNCH`; before host optimization/codegen the backend
materializes naturally aligned scalar cells and an argument-pointer array, then
calls `mtlc_gpu_launch_checked`. Device emitters ignore host functions and
consume kernel IR directly. A runtime provider must define that stable symbol;
`std/gpu` is the bundled CUDA Driver implementation.

## What each consumer requires

**The optimizer** accepts anything the builder produces and preserves these
rules. Use `mtlc_optimize_for` when the target is known. The generic
`mtlc_optimize` takes the conservative kernel-reachable neutral path for a
module containing kernels and the full x86-64 path otherwise.

**x86-64 codegen** consumes optimized or unoptimized IR. This is the only
target with full coverage of the instruction set above, including
`mtlc_address_of` and mixed float/integer bodies.

**ARM64, PTX, and SPIR-V consume either unoptimized IR or their
target-neutral optimized shape.** Call `mtlc_optimize_for` with the eventual
architecture; it excludes x86-specific SIMD/idiom formation. PTX and SPIR-V
also optimize only kernel-reachable device functions after shared call-graph
and collective-uniformity validation. Their subsets:

| | ARM64 | PTX / SPIR-V |
|---|---|---|
| integers, arithmetic, compares, casts | yes | yes |
| labels/jumps/branches, calls, recursion | yes | control flow and non-recursive direct calls to reachable ordinary functions; intrinsics; recursion/indirect/external/kernel calls rejected |
| floats | no | yes (f32/f64) |
| pointers, load/store | no | yes (the kernel model) |
| cooperative tensor MMA | no | stable PTX WMMA plus capability-selected narrow-float and structured-2:4 profiles; current OpenCL 2.0 SPIR-V profile rejects explicitly |
| cooperative tensor epilogue | no | ordered f16/bf16/f32/f64 PTX memory replay plus fail-closed adjacent/unique-loop-exit stable/native resident handoff; current OpenCL 2.0 SPIR-V profile rejects explicitly |
| `mtlc_address_of` | no | no |
| entry identity | linked functions (`main` is the host entry) | only functions marked as kernels become GPU entries |

GPU operations are represented by frontend-neutral `MtlcIntrinsic` identities
documented in [the pipeline reference](pipeline.md#the-code-generators). The
Mettle frontend maps its `gpu_*`, subgroup, math, half-conversion, and atomic
aliases at the IR boundary; a builder frontend supplies the same identities directly.
Pointer descriptors independently carry `MtlcAddressSpace`; atomic instructions
carry `MtlcMemoryOrder`, CAS-only `failure_memory_order`, and
`MtlcMemoryScope`. This separation is intentional:
the pointee's location is a type property, while ordering and visibility are
properties of one operation. IR dumps print the full contract on atomic calls. Legacy
source aliases enter IR as global/relaxed/device rather than relying on emitter
defaults.

`IR_OP_ADDRESS_SPACE_ALLOC` uses a positive `rhs` for a fixed element count. A
zero `rhs` is reserved for a dynamic workgroup view and is illegal for private
storage. The zero extent is not a zero-byte allocation: it means the launch owns
the arena extent. Backends must bind every zero-extent view in a kernel to the
same base and preserve the strongest required element alignment.

## Diagnostics attached to IR

Instructions carry an optional source location (file/line/column) used by
debug info, optimization remarks, and the compile-time interpreter's error
reports. IR built through the public builder currently carries none, so those
features degrade gracefully (remarks without positions); a future builder
revision will expose location tagging.
