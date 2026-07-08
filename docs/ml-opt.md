# ML-Driven IR Optimization

The `--ml-opt` flag runs a learned optimization pass on Mettle's IR after the classical optimizer, behind a translation-validation gate: **the model proposes, the validator disposes**. A graph neural network flags spots where a cheaper but equivalent form exists, a sound transform realizes each proposal, and then every applied rewrite is executed through the compiler's reference interpreter (the same differential harness as [`--verify`](translation-validation.md)) against the pre-rewrite IR. A proposal that changes observable behavior is rejected with a printed counterexample and the function keeps its validated IR. The model is never trusted, only checked.

The pass is experimental and opt-in. Default builds never run it and ship no model. Inference runs in native C; there is no Python or PyTorch at compile time.

```bash
mettle --ml-opt --release app.mettle -o app.exe       # apply, validate each rewrite
mettle --ml-opt --explain --release app.mettle        # ...and report each rewrite + verdict
mettle --ml-opt-speculative --release app.mettle      # also apply unproven model deletes
```

`--ml-opt` implies `-O`: it runs the classical optimizer first and operates on the result.

## Architecture

```
classical optimizer -> GNN (gnn_genius) -> sound transform -> apply -> interpreter differential
                       "where to act"      "realize it"               "reject on divergence"
```

The pass runs in four steps:

1. The compiler dumps the post-classical IR to `_mlopt.ir`.
2. `src/ir/ml_gnn.c` builds the typed-edge dataflow graph the model was trained on (def-use, control, same-expr, and dominating-same-expr edges, with dead `nop` nodes stripped), runs the GNN forward pass from the bundled weights, and produces a per-instruction action.
3. For each flagged instruction the matching sound transform produces a disposition or declines. `src/ir/ml_opt.c` applies each function's dispositions.
4. Every changed function is re-executed through the reference interpreter against its pre-rewrite snapshot on generated inputs, comparing the return value, final buffer bytes, extern-call trace, and touched globals (`ir_verify_check_rewrite`). A validated group stands. On divergence the function is rolled back and the dispositions are re-applied one at a time, each against a fresh snapshot, so exactly the offending proposal is named and discarded while the innocent ones stand.

The model is a six-class relational GNN with the actions `KEEP`, `DELETE`, `FOLD`, `AFFINE`, `GVN`, and `COLLAPSE`. It has roughly 10.8M parameters, a hidden dimension of 384, and 8 layers. Its weights live in `gnn_genius.bin`.

A rejection prints like a `--verify` catch:

```
ml-opt: PROPOSAL REJECTED: model rewrite 'NOP ir#6' changed the observable
        behavior of function 'touch'
  counterexample (input set 0) touch(<buf:33 elems>)
  divergence: return value was 42, is now 0
  action: proposal discarded; 'touch' keeps its validated IR
```

and the compile summary counts every fate:

```
--ml-opt: 7 model proposals: 6 applied (6 validated equivalent, 0 proven-only),
          1 REJECTED by the validator; hoisted 0 large constants
```

When the interpreter cannot execute a function (unsupported parameter type, unsupported construct, no usable inputs), proposals backed by a construction-time proof still apply - marked `proven-only` - and speculative proposals are dropped. The gate itself costs about a millisecond per rewritten function; the visible `--ml-opt` compile time is GNN inference.

## Speculative mode: --ml-opt-speculative

The model's `DELETE` action (dead code) was historically never applied: it carries no construction-time proof, and the text-level liveness used during training is not sound on real IR. The validation gate changes that calculus. Under `--ml-opt-speculative`, `DELETE` proposals on non-control-flow instructions (stores, pure defs, calls - never labels, branches, returns, or locals) are applied as bare `NOP` dispositions **on the validator's word alone**: they stand only when the interpreter can execute the function and finds no divergence, and are dropped whenever the function is unverifiable.

This is the interesting research surface: an unproven, learned rewrite class that is unusable without a gate becomes usable with one. In practice the model's delete head is aggressive - it reliably finds genuinely dead stores the classical pipeline missed (it has no dead-store elimination), and just as reliably proposes deleting live stores in `mem_copy`-shaped code, which the validator rejects one by one with trap-divergence counterexamples. Across the 49-program benchmark suite, speculative mode produces 1,190 proposals: 348 validate and apply, 537 are rejected with counterexamples, and the rest are skipped (unverifiable functions or declined appliers). Every resulting binary matches its baseline's observable behavior at runtime - a 45% rejection rate is exactly why an unproven learned action is unusable without the gate and safe with it.

The honest caveat: interpreter differential on 6 generated input sets is strong evidence, not proof. A speculative rewrite whose wrongness needs an exotic input could in principle survive. That is why speculative mode is a separate opt-in flag rather than part of `--ml-opt`, whose non-speculative transforms remain sound by construction *and* validated.

Speculative mode has already earned its keep as a red team: on its first day it found and exploited four weaknesses in the validation harness itself, each fixed the same day:

1. extern calls' pointed-to memory was never compared - only the pointer value, identical in both runs - so corrupting a locally-built `print_int` digit buffer was invisible (fixed: per-argument byte capture at call time),
2. the interpreter zero-filled locals that native code leaves as stack garbage, hiding deleted initializing stores (fixed: deterministic `0xA5` poison),
3. the interpreter's `IR_OP_LOWER_BOUND_I32` kernel semantics hardcoded `lo = 0` while native codegen seeds it from the destination's prior value - an undocumented in/out contract the kernel emitters relied on (fixed: the interpreter now reads the destination, and `ir.h` documents the contract),
4. the three original input sets never exercised `sift_down(start, end)`-shaped loop bodies, so deleting the loop-carried update validated cleanly (fixed: six input sets probing index-pair relationships).

Every one of those weaknesses also blinded `--verify` for classical passes. An adversarial rewrite generator turns out to be a fuzzer for your validator - arguably the most valuable output of the whole exercise.

## Self-test: METTLE_ML_SABOTAGE

`METTLE_ML_SABOTAGE=1` deliberately corrupts the first `COPY`/`CONST` disposition of the compile into a wrong constant, proving the whole detect -> reject -> heal loop end to end - the ml-opt twin of `METTLE_VERIFY_BREAK`. The regression suite runs it on every build (`ml_opt_sabotage_caught`), alongside `ml_opt_gate`, which also injects a wrong disposition via `METTLE_ML_DISP` and checks the binary still matches the baseline.

## Transforms and Soundness

Every default-mode transform is sound by proof or by construction before it ever reaches the interpreter gate.

| Action | What it does | Soundness |
|--------|--------------|-----------|
| `GVN` | reuse a dominating temp computing the same pure expression | available-expressions dataflow plus dominance: the reused temp is SSA and computed on every path |
| `AFFINE` | collapse linear-form cancellation such as `(a + b) - b` to `a` | exact integer affine forms in Z/2^64 with SSA-versioned bases (opt-in via `METTLE_ML_AFFINE`) |
| `COLLAPSE` | a tangled value equals a single in-scope leaf or a constant | the value matches a fixed leaf or constant over hundreds of random 64-bit vectors, which a non-trivial function cannot |
| bitwise superopt | rewrite a pure AND/OR/XOR/NOT tangle to its global optimum | exact truth table: leaves set to 2^k column constants make one evaluation yield the full truth table, since bits are position-independent |
| xor-shift superopt | rewrite a `^`, `~`, `<<`, `>>` expression to its GF(2) optimum | exact: such expressions are affine over GF(2), of the form `f(v) = Mv + b`, fully determined by 64 basis evaluations |

These proofs are the transforms' *first* line of defense; the interpreter differential still re-checks every applied rewrite (defense in depth: the applier itself could be buggy). The `DELETE` action has no proof of its own and is applied only under `--ml-opt-speculative`, where the validator is the sole gate.

## When It Helps

The transforms target different kinds of redundancy:

- `GVN` is the one that fires on ordinary code: loop-invariant and cross-block recomputation that the classical optimizer's block-local CSE misses.
- `AFFINE`, `COLLAPSE`, and the superoptimizers target redundancy that mostly does not survive a competent classical optimizer in everyday programs. They bite where their slack actually lives: bit-mixing, hashes, PRNGs, checksums, crypto, obfuscated or machine-generated code, and the small-expression frontier that `gcc -O3` itself leaves suboptimal.

On the example benchmark suite most programs see only `GVN` dispositions. The superoptimizers verify correct and fire zero times because the tangles are not present. This is expected: a superoptimizer can only remove slack the code actually contains.

## The --explain Report

With `--explain`, the pass prints every model-driven rewrite as a per-function report (colors and tree glyphs on a UTF-8 terminal, ASCII otherwise). Each entry shows the source-level expression before, the optimal form after, and the number of IR ops removed.

```
── ml-opt: model-driven IR optimizations ────────
  function mix  hash.mettle
    └ line 31  GVN reuse              (p * p)                         → %.t70           -1 op
  function mix_bits  hash.mettle
    └ line 44  bitwise superoptimize  (((x | y) ^ (x & y)) | z)       → ((x ^ y) | z)   -2 ops
  function scramble  rng.mettle
    └ line 9   xor-shift superoptimize ((x ^ (x << 13)) ^ (x << 13))  → x               -3 ops

  3 rewrites in 3 functions · 6 IR ops removed · all validated equivalent by the interpreter
```

Each row carries its validation verdict: rejected proposals render in red with `REJECTED: counterexample found`, and rewrites that stood on their construction-time proof alone (unverifiable function) are tagged `(proven)`.

Each rewrite is identified by function, source file, and line, resolved from the IR before any rewrite shifts indices, with the original and optimal expressions reconstructed at source level. If a line is unavailable for a lowered instruction, the report falls back to the IR index (`ir#N`).

[`examples/explain_demo/explain_demo.mettle`](../examples/explain_demo/explain_demo.mettle) is a runnable program that exercises every `--explain` section: vectorized and non-vectorized loops, inlined and non-inlined calls, `GVN` reuse, and the bitwise superoptimizer.

```bash
mettle --release --explain --ml-opt examples/explain_demo/explain_demo.mettle
```

## Model and Library Files

The pass loads three files, resolved next to the executable (`bin/mlopt/`, bundled by `build.bat`) or from `tools/mlopt/` in a development tree:

- `gnn_genius.bin` - the GNN weights (`METTLE_ML_MODEL`)
- `bw_lib.txt` - optimal bitwise forms per truth table (`METTLE_ML_BWLIB`)
- `gf2_lib1.txt` - optimal GF(2) forms per matrix (`METTLE_ML_GF2LIB`)

Each path can be overridden with the environment variable shown in parentheses. If a file is missing, the corresponding transform is skipped and the build still succeeds.

Two further toggles control optional behavior. `METTLE_ML_AFFINE=1` enables the affine action, and `METTLE_ML_COLLAPSE_ALL=1` runs the collapse verifier on every root as a diagnostic.

## Internals and Retraining

See [`tools/mlopt/README.md`](../tools/mlopt/README.md) for the model architecture, the offline training and export pipeline, and how to rebuild `gnn_genius.bin`, `bw_lib.txt`, and `gf2_lib1.txt`. The fuller design history is in [`ml-ir-optimization-design.md`](ml-ir-optimization-design.md).
