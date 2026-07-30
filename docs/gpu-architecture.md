# GPU architecture and acceptance contract

This document is the engineering contract for making Mettle/libmtlc a serious
GPU and AI toolchain. It deliberately separates what works today from what is
required. A checked box means a test proves the property; prose or a demo is not
evidence.

## Target platform

The primary hardware target is NVIDIA DGX Spark (GB10): Linux on a 20-core
ARM64 CPU, an integrated Blackwell GPU (compute capability 12.1), 128 GB of
unified memory, and 273 GB/s memory bandwidth. The compiler profile is PTX 8.8
with architecture-specific `.target sm_121a`. The portability profile is PTX 6.4 with
`.target compute_75`; SPIR-V/OpenCL remains the non-NVIDIA portability path.

The host matrix is:

| Host | GPU target | Role |
|---|---|---|
| Linux AArch64 / GB10 | PTX `sm_121a` | primary release and performance gate |
| Linux AArch64 / any | portable PTX + SPIR-V | native compiler/ABI gate |
| Linux/Windows x86-64 | PTX + SPIR-V | development and cross-generation |

GB10 is not an x86 machine with a GPU attached. Correctness includes AAPCS64,
the relaxed Arm memory model, 64-bit pointers and `size_t`, cache-line sharing,
and CUDA's ordering rules on unified memory.

## Layer boundary (non-negotiable)

The implementation has four layers. A feature is rejected if it shortcuts this
boundary for convenience.

1. **Frontend:** parses Mettle syntax and diagnoses language/ABI errors. It may
   identify a function as a kernel, but must not know PTX opcodes, SPIR-V IDs,
   CUDA versions, or GB10 instruction encodings.
2. **Public IR/libmtlc:** carries target-neutral kernel identity, address spaces,
   launch geometry, synchronization scope/order, and typed GPU operations. Any
   frontend must be able to construct the same module through `include/mtlc`.
3. **Code generators:** map neutral IR onto PTX or SPIR-V. Target profiles and
   capability checks live here, not in the parser or type checker.
4. **Runtime providers:** implement allocation, module loading, streams, events,
   launches, and graphs. `std/gpu` currently provides CUDA Driver bindings; the
   language and IR contracts must permit HIP/OpenCL providers without changing
   source semantics.

The public-API test is the boundary gate. Without including a Mettle frontend
header, it builds direct device calls, an unreachable ordinary function,
kernels, a typed host launch, address-space/barrier/subgroup operations, and a
broad cooperative-tensor module including explicit/automatic staging and a
four-stage resident pipeline. It verifies entry/reachability rules, checked
provider-ABI lowering, frontend-neutral memory/tensor construction, backend
capability rejection, and PTX, SPIR-V, x86-64, and AArch64 products.

## Honest status (July 2026)

| Capability | Status | Evidence / missing work |
|---|---|---|
| Explicit kernel identity | working foundation | `kernel` survives AST -> IR; public builders can mark kernels; ordinary functions are not exported as entries |
| PTX GB10 profile | assembler-validated | PTX 8.8 / `sm_121a`; the CLI auto-detects the local GPU when no `--gpu-arch` is given and falls back to this profile with no visible device; `ptxas -arch=sm_121a` CI/developer gate when supported; raw `sm_121` cannot silently opt into architecture-specific tensor features |
| Portable PTX profile | assembler-validated | PTX 6.4 / `compute_75`; raw `sm_NN` and `compute_NN` configuration is exposed by CLI and context |
| SPIR-V kernel modules | structurally validated | OpenCL 2.0 module generation; `spirv-val` when installed |
| Kernel scalar/pointer ABI | compile-time checked | aggregate/closure/string/nested-pointer parameters rejected; narrow scalar PTX parameters use natural widths |
| Typed index/math/atomic intrinsics | working foundation | public semantic enum is carried on IR calls; native Mettle `thread`/`block`/`block_dim`/`grid_dim` XYZ access and type-directed atomics need no declaration; PTX/SPIR-V no longer infer GPU behavior from call-name strings |
| Neutral typed host-launch IR | working foundation | compact source launch remains compatible, while named `dispatch` now exposes reorderable 3-D grid/block, dynamic shared bytes, and stream; public builders carry the identical operation and exact argument types; host lowering alone marshals provider parameters; CPU stubs validate every control and natural-width argument, cross-host AArch64 object emission covers nontrivial values, and enqueue failures are checked |
| Streams, events, async copies/allocations, managed memory, 3-D launch runtime | API foundation | CUDA Driver bindings and neutral `gpu_*` wrappers exist; hardware integration tests are still required |
| Native build and GPU module emission on AArch64 | CI-gated | compiler builds on `ubuntu-24.04-arm`; native ELF64 objects link/run with libc while PTX/SPIR-V and the static smoke product emit natively |
| Native Mettle CUDA host ABI on AArch64 | CI-gated foundation | compact and full named source launches run through AAPCS64 `cuLaunchKernel` linkage against a hardware-free provider that checks all 11 arguments, nontrivial XYZ geometry, dynamic shared bytes, and stream; real `libcuda`/GB10 execution remains a DGX gate |
| Explicit pointer address spaces and device storage | assembler-, validator-, and device-validated foundation | public types distinguish generic/global/workgroup/constant/private; fixed Mettle arrays lower to neutral static workgroup/private allocations; a `workgroup var arena: T*` lowers to a typed view of the launch-provided dynamic arena, with multiple views intentionally aliasing one base; PTX uses one module-scope external shared array and SPIR-V one hidden Workgroup pointer kernel argument; ptxas/spirv-val pass and a partitioned two-view CUDA launch passes memcheck/racecheck on compute 12.0; OpenCL host binding is not yet execution-tested |
| Neutral asynchronous workgroup staging | assembler-, validator-, and device-validated foundation | source and public APIs emit balanced global-to-workgroup copy/commit/wait groups with 4/8/16-byte transactions and explicit cache hints; CFG verification rejects unbalanced exits; PTX sm_80+ uses native `cp.async`, portable PTX/SPIR-V replay typed synchronous copies, and compiler-owned PTX workgroup storage is at least 32-byte aligned; explicit and optimizer-generated overlap pass numerical hardware tests on compute 12.0, while GB10 execution remains unproven |
| Configurable workgroup barriers | working foundation | Mettle syntax and the public builder carry acquire/release/acq_rel/seq_cst plus workgroup/global memory-region masks; SPIR-V encodes exact semantics and PTX safely strengthens to `bar.sync`; hardware litmus tests remain pending |
| Scoped/ordered atomics | assembler-, validator-, and device-validated foundation | native and public u32/u64 load/store/add/sub/min/max/and/or/xor/exchange/CAS carry global/workgroup provenance, 64-bit element indices, exact legal C/C++ orders, work-item through system scope, and distinct CAS failure order; PTX 6.4/8.8 assembles, SPIR-V opcodes/semantics validate (u64 requires the standard optional OpenCL extensions), and contended global/workgroup plus actual release-store/acquire-load litmus tests pass memcheck/racecheck on compute 12.0; GB10/OpenCL device execution remains unproven |
| Direct device functions | working foundation | a shared IR call graph emits transitively reachable ordinary functions as PTX `.func` / SPIR-V `OpFunctionCall`, omits unrelated host functions, and rejects recursion, indirect/external calls, host launches, and direct kernel calls; scalar/pointer ABI is assembler-tested, but separate compilation and dynamic dispatch are not implemented |
| Subgroup collectives | assembler-, validator-, and device-validated foundation | neutral ID/size, u32/f32 broadcast and varying-source shuffle, add/min/max reductions, inclusive/exclusive add scans, word-addressed ballot, and any/all votes lower to PTX active-mask `shfl.sync`/`vote.sync`; SPIR-V 1.0 uses native `Groups` plus KHR ballot/vote extensions and explicitly rejects non-uniform shuffle in the current OpenCL 2.0 profile; exact opcode/capability gates pass, and a partial-warp numerical oracle validates inactive-source fallback and mask/vote results on compute 12.0 |
| Collective uniformity verification | shared-IR foundation | workgroup/subgroup/work-item lattice tracks control dependence, varying-trip loops, broadcast lanes, tensor pointers, and helper arguments propagated from every reachable call site; divergent barrier/subgroup/tensor cases are compile failures for both backends; memory loads and opaque results remain deliberately conservative |
| Neutral cooperative-tensor contract and stable PTX WMMA | assembler- and device-validated foundation | native named Mettle syntax and public `MtlcTensorMmaDesc` cover independent formats, shape, four layouts, logical dense-subbyte packing, static or uniform runtime data strides, canonical scale matrices/strides, transpose, rounding, overflow, sparsity, and scope without leaking fragments; public and optimizer-formed neutral chains plus verified loop/pipeline residency groups represent exact sequential accumulation without a vendor fragment ABI; PTX lowers the documented f16/bf16/tf32/f64/i8/u8/i4/u4/b1 stable WMMA family and now subdivides larger byte-addressable logical M/N grids with A/B fragment reuse and multi-subtile residency accounting; CUDA 12.9 `ptxas -arch=sm_121a` accepts every tested profile/grid with zero spills; numerical execution evidence covers the earlier single-physical-tile f16/f32 MMA, resident chains/loops, tail-complete tensor/scalar GEMMs, and two- and four-stage async-copy/tensor pipelines on compute 12.0, while generated grids and GB10 execution remain unqualified |
| Neutral bounded whole-matrix regions | offline-assembler + CPU-oracle foundation | source `tensor_matmul` and public `mtlc_tensor_matmul` carry ordinary whole-matrix pointers, an existing neutral descriptor, optional metadata/scale pointers and strides, unsigned row/column origins, runtime M/N/K, and explicit data leading dimensions in a distinct shared-IR operation; PTX keeps full in-bounds stable-WMMA, unscaled-FP8, scaled `mxf8f6f4`/MXFP4/NVFP4, or structured-2:4 f16/bf16 `mma.sp` chunks resident and computes partial M/N, K tails, K below one chunk, partial warps, misaligned packed origins/strides, and tuple-budget fallback cooperatively with 64-bit addressing; dense subgroup f16/bf16-to-f32, f64, i8/u8-to-i32-wrap, independent A/B transpose, unscaled E4M3/E5M2-to-f32, block-scaled FP8/FP6/FP4-to-f32, and matching f16/bf16 structured-2:4-to-f32 are exact today; TF32/reduced-precision/saturating/other-sparse tails reject; sixteen `sm_121a` fixtures assemble at zero spills/stack with 40-register stable-WMMA, 64-register unscaled-FP8, 48/48/56/72-register scaled ceilings, and 48-register native/40-register replay sparse ceilings, while CPU oracles cover 19x23x21 unscaled, 19x23x67 scaled, and 19x23x19 sparse tails, all narrow/scale decoders, exact LSB-first packing, compact mask/selected-value order, transpose, layouts, padded strides, wrap, K=0, and no-op bounds; no new operation has numerical device, GB10, or performance evidence |
| Neutral cooperative tensor epilogue | offline-assembler foundation | source and public builder APIs represent arbitrary M/N in-place `activation(alpha*D + beta*bias)` with f16/bf16/f32/f64 storage, row/column D layout, static/runtime D stride, absent/per-row/per-column/matrix bias, independent matrix-bias layout/stride, identity/ReLU/clamp, and subgroup/workgroup scope; shared IR verifies exact operands and collective uniformity without fragment concepts; PTX has ordered cooperative memory replay and fail-closed adjacent or unique-loop-exit resident handoff for bias-free stable WMMA, fully mapped native MMA bias modes, and staged commits; an outer guard/bypass forces replay; `sm_121a` assembly reports zero spills at 14--21 standalone and 31--58 resident registers, with forced replay peaking at 66; SPIR-V rejects explicitly, while opaque stable-WMMA bias residency, numerical execution, GB10 qualification, and performance evidence remain open |
| Neutral multidimensional tensor transfer | experimental native path, portable PTX foundation | source and public builder APIs carry rank 1-5 extents, byte strides, tile extents, per-dimension element strides, signed coordinates, zero-fill/discard bounds, direction, and an optional opaque acceleration view without CUDA/PTX concepts in frontend or shared IR; portable PTX performs exact cooperative replay, while encodable PTX 8.8/sm_121a descriptors select load/store TMA with tensor-map acquire, runtime view/alignment fallback, proxy fences, transaction barriers, and bulk-group completion; all paths assemble with zero spills and exact ordering gates, but native TMA execution is quarantined after an invalid early barrier/proxy sequence may have wedged a development driver, so there is intentionally no device-validation claim |
| Blackwell-native tensor selection | working native foundation | neutral descriptors select mixed E4M3/E5M2 FP8, every documented block-scaled `mxf8f6f4` FP8/FP6/FP4 A/B pair, packed E2M1 MXFP4/UE8M0-block32, and NVFP4/UE4M3-block16; PTX owns m16n8 subdivision, lane fragments, bitstream unpacking, scale selectors, guarded direct packed loads, backend-local GB10-safe edge decoders, and register-pressure-based direct/chain/runtime-loop/whole-matrix residency; all source/public-builder forms and the complete 5x5 mixed-type matrix for both whole-tile and whole-matrix operations assemble for `sm_121a` with zero spills, while mixed E3M2/E2M3 direct/chain/runtime numerics pass on compute 12.0; TMA is now an offline-qualified experimental path, while generated quantization/tiling, throughput tuning, other-architecture TMEM/tcgen work, and actual GB10 execution remain open |
| Structured-sparse tensor selection | CPU-semantic + offline-assembler foundation | the neutral contract stores compressed A plus uint8 occupancy masks for 1:2, 2:4, and 4:8 without exposing vendor metadata; PTX capability-selects f16/bf16 structured 2:4 `mma.sp`, sanitizes dynamic masks before the instruction, hoists A/metadata across adjacent N subtiles, and now composes resident K16 interiors with exact bounded-region sparse edges; source and public-builder `sm_121a` whole-tile modules assemble at 40 registers and bounded modules at 48, all with zero spills, while a CPU oracle covers compact masks, selected-value order, transpose, odd K, K=0, and no-op bounds; A-fragment and metadata-bit placement match the PTX 8.8 equations/Figure 119; GPU numerics, other ratios/types, malformed-input behavior beyond defensive clamping, performance, and device execution are not yet qualified |
| Target-neutral GPU optimization | working foundation | CLI `-O` and public `mtlc_optimize_for` run a shared scalar/CFG fixpoint over kernel-reachable device functions; exact legality passes form adjacent accumulator chains, single-update runtime-loop residency, automatic load/store staging, and explicit N-stage async tensor residency; the shared verifier rechecks every group connection, CFG shape, and ordered wait/publication handoff; PTX separately chooses native/replay and resident/replay emission; native and public frontends trigger the same IR transforms; `--dump-ir` and `--explain` expose each neutral decision and backend boundary; x86 SIMD and ML rewrites are excluded |
| Real-device differential/sanitizer harness | working on development NVIDIA hardware | one cross-host CUDA Driver harness checks 3-D indices, odd tails, device calls, loops/math, static/dynamic workgroup memory, explicit and optimizer-generated async staging, contended u32/u64 atomics and release/acquire visibility, barriers, subgroup reductions/scans/exchange/ballot/votes, arbitrary-width row softmax and affine layer normalization, poisoned-padding f16/f32 tensor MMA, native FP8, direct/chain/runtime MXFP4, NVFP4, and mixed dense FP6 with independent scale chunks, a four-K-tile resident chain, two- and four-stage staged tensor pipelines, a 32x32x64 runtime-K resident GEMM plus incomplete-K commit bypass, a 19x23x21 tensor/scalar tail-complete GEMM plus scalar-only K=7 path, and natural-width parameters; all 32 cases pass on compute 12.0 with memcheck at 0 errors and racecheck at 0 hazards; strict Spark mode requires native AArch64 + integrated compute 12.1 and has not yet been recorded here |
| Source-expressed AI correctness primitives | working baseline | arbitrary-width/padded-stride f32 softmax and affine layer normalization; padded multi-tile and tail-complete tensor/scalar f16/f32 GEMMs; native FP8, mixed FP6, MXFP4, and NVFP4 direct/chain/runtime accumulation; and explicit two- and four-stage async-copy/tensor pipelines pass CPU numerical oracles on compute 12.0; they prove exact expressiveness and correctness, not general fusion, compiler-generated tiling/pipeline rotation, GB10 tuning, or competitive throughput |
| GPU fusion, compiler-generated tiling/layout, memory planning, cost model, autotuning | early partial foundation | exact adjacent, single-update loop, automatic copy staging, explicit N-stage staged-tensor accumulation, a separate exact epilogue, and fail-closed backend-private MMA/epilogue residency (including unique loop exits) have neutral legality, native/public construction, negative gates, verifier rechecks, inspectable PTX decisions, and offline assembly; PTX generates physical grids for larger stable byte-addressable logical tiles, chooses A/B fragment reuse, prices every resident output subtile plus epilogue temporaries, and now selects native full-K versus cooperative M/N/K edges inside each bounded whole-matrix region, including backend-local opposite-layout normalization, resident direct-MMA unscaled E4M3/E5M2 interiors, and resident block-scaled FP8/FP6/FP4 interiors with exact cooperative tails and guarded packed loads; a backend-only tuple budget produces reproducible resident/replay variants, an offline profiler records hashed resource and static-instruction evidence, and a device-free selector computes explicit-model occupancy/Pareto results or consumes hash-bound paired timings with corrected significance/effect gates without inventing a winner; automatic launch-grid/batch generation, physical layout transforms, buffer rotation, multi-exit/graph-wide fusion, whole-kernel memory planning, on-device measurement collection/cache integration, and GB10 performance evidence are not implemented |
| Graph capture/replay and multi-device execution | **not implemented** | runtime and neutral launch-graph IR needed |
| Real GB10 differential/performance result | **not yet recorded** | the self-hosted Spark workflow and strict correctness/sanitizer gate exist, but a passing compute-12.0 development GPU is not substituted for AArch64/GB10 evidence; competitive performance workloads remain pending |

This is a foundation, not parity with CUDA, HIP, SYCL, or Triton. Claiming
otherwise before the acceptance gates below pass would be false.

## Delivery order and hard gates

### 1. Correct portable compute model

- Done for atomic load/store/RMW/compare-exchange with explicit order/scope;
  standalone fences and ordered non-atomic memory operations remain pending.
- Done for scalar storage: neutral address spaces are exposed as fixed Mettle
  `workgroup var` / `private var` arrays, launch-sized aliased workgroup pointer
  views, and public static/dynamic builders. Richer aggregate element types and
  execution of the SPIR-V host-binding path remain pending.
- Extend the direct device-call foundation with generic specialization,
  cross-module device linking, inlining/cost modeling, and richer aggregate
  calling conventions without weakening the portable call-graph rules.
- Done for the complete source launch controls: named `dispatch` exposes 3-D
  geometry, dynamic-arena bytes, and stream without changing neutral IR.
  Recoverable status remains the explicit `gpu_launch_3d` API; add neutral
  launch attributes without coupling them to one provider.
- Define atomics by operation, width, memory order, and scope; lower them to PTX
  and SPIR-V with litmus tests.

Gate: the same public-IR module validates and executes against CPU references on
PTX and SPIR-V for indexing, aliasing, barriers, every atomic order/scope, and
edge launch shapes.

### 2. AArch64 host completeness

- Done: relocatable AArch64 ELF objects, AAPCS64 register/stack calls,
  function/global/string relocations, libc/CUDA extern symbols, and the same
  `gcc -no-pie` linker contract as x86-64 Linux.
- Pending: native AArch64 DWARF/debug tables and broader aggregate/SIMD IR.
- Run the full standard-library and runtime suite natively on AArch64.
- Audit all host atomics and lock-free structures under the Arm memory model.
- Add DGX builds using `-march=armv9.2-a`; prefer `-mcpu=gb10` with GCC 15 or
  LLVM 21, while retaining a compatible AArch64 release build.

Gate: build and run the Mettle GPU host application against the real CUDA
Driver on DGX Spark without an x86 cross-product, emulator, provider stub, or
hidden frontend special case.

### 3. GB10-native AI operations

- First-class f16, bf16, FP8, FP6, and FP4 tensor storage/arithmetic formats.
- Done as a backend-neutral foundation: one cooperative tensor descriptor spans
  arbitrary positive m/n/k, independent A/B/accumulator/result formats, four
  layouts and static or uniform runtime leading dimensions, transpose, math
  mode, rounding, overflow,
  structured sparsity, scale modes, and subgroup/workgroup scope. Stable PTX
  WMMA profiles assemble for `sm_121a`; unsupported profiles fail capability
  selection rather than narrowing semantics.
- Source-level 2-D output tiling, uniform K-tile accumulation, and a combined
  tensor-interior/scalar-edge strategy are numerically validated with padded
  runtime strides. PTX now subdivides one exact byte-addressable logical tensor
  tile into stable physical grids with operand reuse. The separate neutral
  bounded-region operation now makes full-K residency and exact M/N/K edge
  partitioning backend-generated for dense f16/bf16-to-f32, f64,
  i8/u8-to-i32-wrap, unscaled FP8, and block-scaled FP8/FP6/FP4. Automatic 2-D
  launch geometry, batch slicing, physical layout transforms, sparse/other
  unsupported dtype tails, and region-size tuning remain open work, not hidden
  restrictions.
- Done for straight-line, one exact runtime-loop, and explicit N-stage
  asynchronous accumulation: the neutral optimizer fuses adjacent tiles,
  proves a single-update loop, or proves every ordered wait/publication handoff;
  the shared verifier rechecks the scope-specific shape and PTX retains the
  accumulator when its explicit tuple budget permits. Public frontends trigger
  the same transforms.
- Done for explicit and narrowly automatic global-to-workgroup staging:
  balanced neutral copy groups lower to native `cp.async` on sm_80+ and typed
  synchronous replay elsewhere. Arbitrary N-stage generation/rotation,
  multiple-update loops, compiler-generated unrolling, and tiling remain open.
- Native mixed FP8, the complete block-scaled FP8/FP6/FP4 type matrix, MXFP4,
  and NVFP4 selection is implemented for direct, chain-resident,
  runtime-loop-resident, and exact bounded whole-matrix forms. Canonical
  f16/bf16 structured 2:4 selection is
  offline-assembler validated; extend it with safe numerical qualification,
  other sparse ratios/types, tensor maps/TMEM/tcgen, generated
  quantization/tiling, and cluster launch attributes.
- Layout-aware matrix/tensor IR whose semantics are backend-neutral; PTX selects
  Blackwell instructions and SPIR-V selects the best valid implementation.

Gate: numerically validated GEMM, fused softmax, layer normalization, attention,
quantized matmul, and reduction kernels across adversarial shapes and dtypes.

Current evidence satisfies baseline and tail-complete f16/f32 GEMM, unfused row
softmax, affine layer normalization, exact static/loop tensor residency, native
FP8/FP6/MXFP4/NVFP4 tiles, and exact two- and four-stage async tensor pipelines on
the development GPU. Structured 2:4 has offline assembly evidence only. General fusion, attention, generated quantized matmul,
adversarial dtype coverage, and the actual GB10 run remain open gates.

### 4. Compiler advantage

- GPU-aware legality analysis, fusion, tiling, vectorization, memory planning,
  occupancy/register-pressure modeling, and profile-guided autotuning.
- Persistent kernel cache keyed by IR, target capabilities, launch shape, and
  compiler version; reproducible selection with inspectable reasons.
- Source-level debug locations, sanitizer hooks, profiler ranges, and useful
  diagnostics for divergence, races, spills, and occupancy loss.

Gate: a published, reproducible GB10 suite compares correctness, compile time,
latency, throughput, peak memory, and power against CUDA C++/cuBLAS, Triton, and
representative SYCL/HIP paths. “Overcome” requires statistically defensible wins
on named workloads, not a favorable microbenchmark.

## Required test tiers

1. **Every commit:** frontend diagnostics, public-IR boundary test, PTX/SPIR-V
   structural checks, `ptxas` when available, native AArch64 object/link/ABI.
2. **GPU runner:** execute every kernel against a deterministic CPU oracle under
   compute-sanitizer, including odd sizes, misalignment, aliasing, NaN/Inf,
   integer boundaries, and launch failures.
3. **DGX Spark:** repeat correctness on GB10, then record ptxas registers/spills,
   Nsight metrics, warm/cold timings, memory use, and thermally steady results.
4. **Release:** no performance claim without stored inputs, baselines, raw data,
   compiler/toolkit versions, and confidence intervals.

## Primary references

- [DGX Spark hardware](https://docs.nvidia.com/dgx/dgx-spark/hardware.html)
- [DGX Spark porting guide](https://docs.nvidia.com/dgx/dgx-spark-porting-guide/overview.html)
- [DGX Spark optimization guidance](https://docs.nvidia.com/dgx/dgx-spark-porting-guide/optimization.html)
- [CUDA Driver execution API](https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__EXEC.html)
- [PTX ISA](https://docs.nvidia.com/cuda/parallel-thread-execution/)
- [SYCL 2020 specification](https://registry.khronos.org/SYCL/specs/sycl-2020/html/sycl-2020.html)
- [HIP Driver API](https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/)
- [Triton language API](https://triton-lang.org/main/python-api/triton.language.html)
