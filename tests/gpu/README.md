# GPU validation tiers

The ordinary test suite proves frontend diagnostics, shared-IR invariants,
PTX/SPIR-V structure, and assembler acceptance. It does not claim device
execution. `hardware_harness.c` is the real CUDA Driver differential gate.

The harness compiles `hardware_kernels.mettle` with `-O`, loads the resulting
PTX through the Driver API, and checks 32 independent contracts against CPU
oracles:

- all XYZ thread, block, block-size, and grid-size indices;
- odd-size guarded SAXPY through a reachable device helper;
- a counted-loop row norm and GPU math intrinsic;
- workgroup/private storage plus a workgroup barrier;
- explicit native 16-byte global-to-workgroup asynchronous staging with a
  commit/wait/publication handoff;
- optimizer-generated staging from ordinary loads/stores, with independent
  scalar work structurally and numerically proved between commit and wait;
- a launch-sized workgroup arena partitioned through two intentionally aliased
  typed views;
- contended global and workgroup u32/u64 add/sub/min/max/and/or/xor/exchange/CAS,
  including returned-old permutations, exactly-one CAS winners, and an actual
  atomic release-store/acquire-load message-passing visibility check;
- subgroup f32/u32 reductions and broadcasts, including a partial data tail;
- subgroup f32/u32 min/max and inclusive/exclusive add scans, including a
  partial final subgroup;
- variable-source u32/f32 shuffle plus word-addressed ballot and any/all votes;
  a partial final warp proves inactive-source fallback and active-mask results;
- numerically stable row softmax with arbitrary runtime width and padded
  input/output strides;
- affine layer normalization with arbitrary runtime width and padded strides;
- numerical f16-input/f32-accumulator cooperative tensor MMA with independent
  padded runtime strides and poisoned padding;
- native mixed E4M3/E5M2 FP8 for direct tiled/transpose layouts, a four-product
  resident chain, and runtime-K residency;
- densely packed E2M1 MXFP4 with UE8M0 block32 scales for a direct tile,
  three-product resident chain, and runtime-K accumulation; every oracle varies
  scale chunks independently and poisons data/scale padding;
- densely packed E2M1 NVFP4 with mantissa-bearing UE4M3 block16 scales through
  the same three residency modes and independent CPU oracles;
- densely packed mixed E3M2/E2M3 FP6 with UE8M0 block32 scales through direct,
  three-product resident-chain, and runtime-K resident-loop kernels; the static
  path uses exact three-byte fragment loads while dynamic strides exercise the
  boundary-safe general bit gather;
- a compiler-formed four-K-tile accumulator chain with distinct, valid padded
  strides; the runner additionally proves one C load, four MMAs, and one D
  store before its numerical oracle executes;
- a double-buffered two-tile f16/f32 pipeline with four native 16-byte copies,
  two pending groups, wait-1/MMA/wait-0 ordering, accumulator residency across
  the final barrier, one C load, two MMAs, one D store, and a distinct per-tile
  CPU oracle;
- a four-stage f16/f32 pipeline with eight native copies, four committed
  groups, wait-3 through wait-0 publication order, one resident accumulator,
  four MMAs, one C load, one D store, and an independent numerical oracle;
- a 2-D, four-output-tile, four-K-tile f16/f32 GEMM with runtime matrix strides;
  the runner proves its runtime loop has one C load, four dynamic MMA
  executions, and one D store, then launches an incomplete K=15 case and proves
  every poisoned D element remains untouched;
- a tail-complete tensor/scalar f16/f32 GEMM that retains the resident WMMA path
  for complete 16x16x16 regions and covers M/N/K edges with typed scalar work;
  19x23x21 simultaneously exercises the tensor interior and all edge classes,
  16x16x7 proves the scalar-only path, and both preserve poisoned padding;
- compiler-native `tensor_matmul` bounded regions for f16, bf16, f64, and i8,
  including block-derived 2-D origins, runtime or padded static strides,
  resident full-K WMMA, exact cooperative M/N/K edges, 64-bit addressing,
  tuple-budget fallback, and explicit unsupported-family/SPIR-V diagnostics;
- natural-width i8/u8/i16/u16/bool launch parameters.

The harness includes no CUDA headers and dynamically loads the Driver API, so
the same C source builds on Windows x86-64 and Linux AArch64. This is deliberate:
host architecture is a test dimension, not a frontend/backend fork.
The ordinary `tests/run_tests.ps1` suite does not load this harness or touch a
GPU; it checks these files as source plus compiler, CPU-oracle, validator, and
offline-assembler evidence only.

## Development GPU

Windows PowerShell:

```powershell
tests/gpu/run_hardware_tests.ps1 -Sanitizer
```

Linux:

```bash
bash tests/gpu/run_hardware_tests.sh --sanitizer
```

`auto` reads the installed device's compute capability and compiles a matching
`sm_NN` profile. Passing on another GPU is useful backend execution evidence,
but is not GB10 evidence.

## DGX Spark release gate

Run natively on the Spark:

```bash
make DGX_SPARK=1
bash tests/gpu/run_hardware_tests.sh --require-gb10 --sanitizer
```

`--require-gb10` refuses to run unless all three facts hold: the harness itself
was compiled for AArch64, CUDA reports compute capability 12.1, and the device
reports integrated memory. It then compiles the exact PTX 8.8 / `sm_121a`
profile. A cross-compiled binary, an x86 Blackwell card, or assembler-only test
cannot accidentally satisfy this gate.

Passing proves the listed correctness and sanitizer cases on that machine. It
does not prove peak performance, every cross-workgroup memory-order litmus,
structured-sparse/TMEM coverage, quarantined native TMA execution, or
competitive AI workload results; those require separate gates.

Structured-2:4 f16/bf16 source coverage is intentionally compiler-only today.
It checks the canonical compressed-A/uint8-mask contract, single,
resident-chain, and runtime-loop lowering, ordered `mma.sp` selection, metadata
guards, register ceilings, and offline `ptxas` acceptance. It is not included
in the ordinary hardware harness, and neither numerical correctness nor GB10
safety is claimed until it passes an independently recoverable device
qualification.

Stable-WMMA logical-grid coverage in `tensor_tiled.mettle` is also
compiler-only today. Native-source and public-builder modules cover f16, bf16,
tf32, f64, i8, and u8 physical subdivision; f16 and f32 results; row/column
A/B/C/D layouts; static and uniform runtime leading dimensions; A/B fragment
reuse; four-subtile chain/loop residency; forced replay; register ceilings; and
offline `ptxas` acceptance. It has no device-numerical or throughput claim yet.

`tensor_matmul.mettle` covers exact bounded whole-matrix regions with resident
non-transposed K chunks plus cooperative M/N/K edges. Its transpose companion
covers independent A/B transpose under mixed stored layouts through a
backend-local opposite-layout native view plus tuple-budget-forced exact replay.
The FP8 companion adds unscaled E4M3/E5M2 exact tails using architectural PTX
conversion plus resident direct-MMA full interiors and tuple-budget-forced exact
replay. `tensor_matmul_scaled.mettle` adds exact mixed FP6, MXFP4, NVFP4, and
scaled/transposed FP8 regions with canonical A/B scale grids, dense-subbyte
alignment guards, resident native runtime-K interiors, and forced exact replay.
`tensor_matmul_sparse.mettle` adds matching f16/bf16 structured-2:4 regions,
including transposed compressed A, compact runtime-K metadata stride, resident
K16 `mma.sp` interiors, and forced exact mask replay. The CPU oracle independently
checks LSB-first FP4/FP6 bytes, all narrow/scale decoders, odd 19x23x67 scaled
and 19x23x19 sparse tails, selected-value order, padded strides, K=0, and no-op
bounds. CUDA 12.9
offline assembly reports 48/48/56/72 registers respectively, zero spills, and
zero stack for the scaled cases; both sparse entries use 48 registers natively
and 40 under forced replay, also with zero stack/spills. These fixtures have
CPU-oracle semantics and offline `sm_121a` assembly only, not device-numerical
or GB10 performance evidence.

`tensor_epilogue.mettle` is likewise compiler/offline-assembler coverage only.
It spans irregular M/N, f16/bf16/f32/f64 storage, both layouts, static/runtime
strides, row/column/matrix bias, alpha/beta, ReLU/clamp, and both collective
scopes. PTX uses synchronized cooperative memory replay and assembles for
`sm_121a` with zero spills.

`tensor_epilogue_fused.mettle` covers fail-closed backend-private residency:
a two-MMA stable-WMMA bias-free chain, a two-MMA FP8 matrix-bias/clamp chain,
intentionally refused stable-WMMA biased and runtime-stride-mismatch cases, and
a staged commit handoff. It also covers a unique loop-exit handoff and proves
that an outer guard reaching the same epilogue forces complete memory replay.
The default and forced-replay variants both assemble for `sm_121a` with zero
spills and an explicit 72-register test ceiling; observed maxima are 58
resident and 66 replay, while both loop forms use 40 registers. This remains
compiler/offline evidence with no
device-numerical, GB10-safety, or performance qualification.

## Experimental TMA quarantine

The neutral rank-1 through rank-5 tensor-transfer contract and its PTX TMA
lowering have offline `ptxas` coverage, but native execution is deliberately
excluded from both ordinary hardware runs and `--require-gb10`. An early
development run exposed an invalid barrier/proxy sequence and may have wedged
the host GPU driver. The missing barrier proxy fence and the non-transitive
shared-store fence have been corrected against the CUDA 12.9 protocol, but
offline assembly is not device qualification.

Do not run the experimental path on a workstation. It is three-way gated for use
only on a disposable or remotely recoverable test host with an independent
watchdog and GPU-reset/reboot access:

```powershell
$env:MTLC_ALLOW_EXPERIMENTAL_TMA = "I_ACCEPT_GPU_RESET_RISK"
$env:MTLC_TMA_RECOVERY_READY = "I_HAVE_OUT_OF_BAND_RECOVERY"
tests/gpu/run_hardware_tests.ps1 -ExperimentalTma
```

```bash
MTLC_ALLOW_EXPERIMENTAL_TMA=I_ACCEPT_GPU_RESET_RISK \
  MTLC_TMA_RECOVERY_READY=I_HAVE_OUT_OF_BAND_RECOVERY \
  bash tests/gpu/run_hardware_tests.sh --experimental-tma
```

Even after those gates pass, the runner completes the ordinary 32-case suite
first and starts the two experimental transfers last in a dedicated process.
The harness rejects direct attempts to mix `--tensor-transfer` with the ordinary
suite; `--tensor-transfer-only` is mandatory. This isolation improves failure
attribution and keeps the established gate separate, but it does not make an
unqualified kernel safe.

Passing that opt-in test on a development GPU still is not GB10 evidence. The
eventual Spark qualification must additionally use `--require-gb10`, native
AArch64, integrated compute 12.1, sanitizer passes, and recorded recovery-safe
execution. Until then, documentation must call native TMA experimental and
unqualified.
