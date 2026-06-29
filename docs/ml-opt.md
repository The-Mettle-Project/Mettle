# ML-Driven IR Optimization

The `--ml-opt` flag runs a learned, verifier-gated optimization pass on Mettle's IR after the classical optimizer. A graph neural network flags spots where a cheaper but equivalent form exists, and a sound transform then proves the equivalence and applies it. The model decides where to look; the transform decides whether the rewrite is correct. Nothing is applied on the model's word alone.

The pass is experimental and opt-in. Default builds never run it and ship no model. Inference runs in native C; there is no Python or PyTorch at compile time.

```bash
mettle --ml-opt --release app.mettle -o app.exe       # apply
mettle --ml-opt --explain --release app.mettle        # apply and report each rewrite
```

`--ml-opt` implies `-O`: it runs the classical optimizer first and operates on the result.

## Architecture

```
classical optimizer  ->  GNN (gnn_genius)  ->  sound transform  ->  verifier  ->  apply
                         "where to act"        "realize it"         "is it equal?"
```

The pass runs in three steps:

1. The compiler dumps the post-classical IR to `_mlopt.ir`.
2. `src/ir/ml_gnn.c` builds the typed-edge dataflow graph the model was trained on (def-use, control, same-expr, and dominating-same-expr edges, with dead `nop` nodes stripped), runs the GNN forward pass from the bundled weights, and produces a per-instruction action.
3. For each flagged instruction the matching sound transform produces a disposition or declines. `src/ir/ml_opt.c` applies the dispositions to the IR.

The model is a six-class relational GNN with the actions `KEEP`, `DELETE`, `FOLD`, `AFFINE`, `GVN`, and `COLLAPSE`. It has roughly 10.8M parameters, a hidden dimension of 384, and 8 layers. Its weights live in `gnn_genius.bin`.

## Transforms and Soundness

Every applied transform is sound by proof or by construction, never by input sampling.

| Action | What it does | Soundness |
|--------|--------------|-----------|
| `GVN` | reuse a dominating temp computing the same pure expression | available-expressions dataflow plus dominance: the reused temp is SSA and computed on every path |
| `AFFINE` | collapse linear-form cancellation such as `(a + b) - b` to `a` | exact integer affine forms in Z/2^64 with SSA-versioned bases (opt-in via `METTLE_ML_AFFINE`) |
| `COLLAPSE` | a tangled value equals a single in-scope leaf or a constant | the value matches a fixed leaf or constant over hundreds of random 64-bit vectors, which a non-trivial function cannot |
| bitwise superopt | rewrite a pure AND/OR/XOR/NOT tangle to its global optimum | exact truth table: leaves set to 2^k column constants make one evaluation yield the full truth table, since bits are position-independent |
| xor-shift superopt | rewrite a `^`, `~`, `<<`, `>>` expression to its GF(2) optimum | exact: such expressions are affine over GF(2), of the form `f(v) = Mv + b`, fully determined by 64 basis evaluations |

The `DELETE` action (dead code) is not applied here. The classical optimizer already performs sound dead-code elimination, and the text-level liveness used to produce the labels is not sound on all real IR shapes.

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

  3 rewrites in 3 functions · 6 IR ops removed, all verified equivalent
```

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
