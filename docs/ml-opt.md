# ML-driven IR optimization

`--ml-opt` runs a learned optimizer after the classical passes. A graph neural
network reads the IR and points at redundancy and algebra the classical passes
missed. Sound transforms realize each proposal, and every rewrite is
re-executed through the [validation interpreter](translation-validation.md)
and discarded when it diverges.

```bash
mettle --ml-opt --release --build program.mettle
```

`--ml-opt` implies `-O`. It is experimental.

## What it reports

```text
--ml-opt: 1 model proposal: 1 applied (1 validated equivalent, 0 proven-only);
hoisted 0 large constants

-- ml-opt: model-driven IR optimizations --------

  function main  ex.mettle
    \_ line 7    GVN reuse    %.t161  -> %.t159  -1 op

  1 rewrite in 1 function * 1 IR op removed * all validated equivalent by the
  interpreter
```

Each rewrite names the function, the line, the kind, the values involved, and
the instruction count it changed. The last line is the accounting: how many
rewrites, how many instructions, and how many the interpreter confirmed.

Pair it with [`--explain`](compilation.md) to see the ml-opt section beside
the classical decisions.

## The validation gate

The model proposes. It does not decide.

Every rewrite it suggests is applied, then the function is executed before and
after on generated inputs. A rewrite that changes behavior is thrown away and
the IR reverts. That is why the report distinguishes "validated equivalent"
from "proven-only": the first was executed and matched, the second was proved
sound by the transform itself without needing a run.

The gate is the same machinery as [`--verify`](translation-validation.md), so
the same limits apply. A function the interpreter cannot execute cannot have a
model rewrite validated, and an unproven rewrite there is refused.

## Speculative mode

`--ml-opt-speculative` also applies the model's unproven proposals, which are
dead-code deletions. Those stand only when the validator can execute the
function and finds no divergence. It implies `--ml-opt`.

A deletion the validator cannot check is not applied.

## The model

A relational graph neural network over the IR dataflow graph. Nodes are
instructions. Edges are typed: def-use, control, same-expression, and
dominating-same-expression. It classifies each node into one of six actions:
keep, delete, fold, affine, GVN, or collapse.

Inference runs natively in C inside the compiler. Nothing Python is invoked at
compile time.

Three files ship in `bin/mlopt` and the compiler loads them:

| File | Contents |
|------|----------|
| `gnn_genius.bin` | The network weights |
| `bw_lib.txt` | The best `&`, `|`, `^`, `~` form for each truth table up to four inputs |
| `gf2_lib1.txt` | The best `^`, `~`, `<<`, `>>` form for each GF(2) matrix |

The two libraries are exhaustive search results computed offline, so a
collapse the model proposes is realized as a known-optimal form.

## Training and tooling

`tools/mlopt` holds the offline pipeline: the training corpus generators, the
trainer, the exporter, and the superoptimizers that produce the two libraries.
Its README documents rebuilding the model. None of it runs during a compile.

## Environment variables

| Variable | Effect |
|----------|--------|
| `METTLE_ML_MODEL` | Load weights from a different path |
| `METTLE_ML_TRACE` | Trace inference |
| `METTLE_ML_ACTIONS` | Report the raw per-node classifications |
| `METTLE_ML_SPECULATIVE` | Turn speculative mode on without the flag |

## See also

- [Translation validation](translation-validation.md)
- [Compilation](compilation.md)
