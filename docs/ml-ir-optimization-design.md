# ML IR-to-IR Optimization — Design

Status: **draft for argument**. Nothing is built yet. Captures the architecture
agreed so far and flags the calls still open.

## Goal

A model that maps **IR dump → optimized IR**, used as an experimental,
opt-in optimization path. The model proposes; trusted infrastructure verifies;
the classical optimizer is always the fallback. The default build ships no ML.

This supersedes the earlier phase-ordering framing, but reuses its key pieces:
the offline search becomes the **label generator**, and the fuzzer corpus
becomes the **training corpus**.

## The one invariant: the model never has the last word

A generative IR→IR model produces (a) invalid IR — cheap to catch — and, worse,
(b) valid-but-wrong IR: a silent miscompile with no diagnostic. A compiler
cannot ship that. So the architecture is fixed:

```
IR ──▶ [model proposes] ──▶ [verifier: equivalent to input?]
                                 │
                  accept ◀───────┴───────▶ reject
                     │                       │
               use proposal          fall back to classical optimizer
```

The model is a proposal engine in front of a verifier. The default compiler is
untouched; the model is an opt-in flag (working name `--ml-opt`) with frozen
weights, so it stays deterministic and differential-fuzzable.

Recurring theme: **the ML is the easy part; the correctness infrastructure is
the hard part.** For this project that infrastructure is the verifier.

## Resolved decisions

### D1 — Staged output representation: rewrite actions first, raw IR second

- **Stage 1 — verified rewrite actions.** The model emits a sequence of
  proven-correct rewrite operations; trusted C applies them to the IR. Output is
  **correct by construction** — no equivalence checker needed, because each
  rewrite in the library is individually sound. IR text is *input only*; no IR
  parser required. This proves the end-to-end pipeline (tokenize → model →
  apply → measure) with correctness for free.
- **Stage 2 — raw IR generation.** The model emits the optimized IR dump
  directly. Maximally flexible, but now correctness rests entirely on the
  verifier, and the compiler must *parse* generated IR back in. Built only after
  Stage 1 works.

### D2 — Training external, inference in Mettle

Train in PyTorch/JAX (mature autodiff, the boring path). Run inference in
Mettle's existing LLM engine under `--ml-opt`. Training is not codegen, so this
doesn't violate the from-scratch-codegen ethos; building autodiff in Mettle is a
possible later dogfooding project, explicitly out of scope for v1.

### D3 — Verifier: probabilistic now, sound later

- **Probabilistic (have the seed).** `tools/fuzz/irexec.py` already parses and
  interprets the `--dump-ir` format. Differentially execute model output vs the
  −O0 reference on random inputs; any mismatch → reject. Catches most wrongness,
  never proves equivalence. Sufficient to gate Stage 1 experiments and to mine
  Stage 2 training data safely.
- **Sound (the real centerpiece).** Translation validation / equivalence
  checking that *proves* the proposal computes the same function. This is the
  research effort that makes Stage 2 trustworthy rather than a toy.

### D4 — Labels = best-discovered IR, not default-optimizer output

Training the model to mimic the *default* optimizer caps it at parity — an
expensive, fallible copy of what we already have. The model can only *beat* the
optimizer if its targets are better than default. Those targets come from
**offline search** over schedules / rewrite sequences (the work argued in the
prior design round), scored by the proxy cost model and validated by real
timings. Search finds the better IR; the model learns to emit it in one shot.

- Stage 1 label = the action sequence that reproduces the best-discovered IR.
- Stage 2 label = the best-discovered IR dump text.

### D5 — Corpus = fuzzer genprog, loop/array-biased

`examples/` (~50 kernels) overfits instantly. Use the differential fuzzer's
genprog as the corpus generator, biased toward loop/array/reduction shapes so
training data exercises real optimization surface. The fuzzer's −O0 oracle
doubles as the correctness backstop.

## What already exists vs. what we build

Exists (seeds to wire up):
- `--dump-ir` textual IR sidecar — model input, and Stage-2 target text
  (`ir_program_dump`, `src/ir/ir.c`).
- `tools/fuzz/irexec.py` — IR-dump parser + reference interpreter = the
  probabilistic verifier seed.
- Declarative rewrite engine (table-driven sound IR identities) — the natural
  basis for the **Stage-1 action library**.
- `ir_optimize_function_revectorize` clone harness — the no-commit rollout
  sandbox for search.
- Fuzzer genprog + −O0 differential oracle — corpus + correctness backstop.
- Mettle LLM engine — the inference runtime.

Must build:
- A stable, model-friendly IR **tokenization** over the dump format.
- The Stage-1 rewrite-action vocabulary + a trusted applier (extending the
  declarative rewrite engine).
- The offline **search → label** data pipeline (corpus → search → export pairs).
- External training harness; weight export/load into the Mettle LLM engine.
- `--ml-opt` flag wiring: dump → tokenize → infer → apply/parse → verify →
  use-or-fallback.
- Stage 2 only: a **C-side IR parser** (text → IR) and the sound verifier.

## Milestones

1. **IR round-trip + data pipeline (no ML).** Confirm `--dump-ir` is stable and
   tokenizable; extend `irexec.py` coverage to the corpus; build corpus →
   optimizer/search → `(IR dump, action-sequence, optimized IR)` export. The
   go/no-go: can we mine a clean, verified pair dataset at scale? Nothing
   downstream works without it.
2. **Stage 1 model.** Train IR→action-sequence externally; load into Mettle;
   apply actions under `--ml-opt`; measure proxy + real timings vs classical.
   Correct by construction, so this isolates "does the model learn useful
   transforms" from "is the output safe."
3. **Sound verifier.** Translation-validation/equivalence checker strong enough
   to gate untrusted IR.
4. **Stage 2 model.** IR→IR raw generation; C-side IR parser; every output
   through the sound verifier with classical fallback; fuzz the `--ml-opt` path.

## Open questions

- **OQ1 (search algo for labels):** greedy/beam search-then-distill vs on-policy
  RL to discover the best-IR labels. Leaning search; revisit if sequential
  rewrite interactions need credit assignment.
- **OQ2 (tokenization):** raw dump text vs a structured/canonicalized IR
  serialization designed for the model (SSA value numbering, normalized
  temp names). Structured almost certainly wins; degree is open.
- **OQ3 (Stage-1 action granularity):** coarse (whole-pass invocations) vs fine
  (individual algebraic rewrites). Fine is more expressive but longer sequences.
- **OQ4 (verifier strength vs reject rate):** how strong must the sound verifier
  be before Stage 2 reject rates are tolerable? Unknown until Stage 1 data shows
  how far model output strays from verifiable equivalence.
- **OQ5 (proxy fidelity):** does a hand-built static proxy track real time well
  enough to rank search candidates, or is an early learned cost model forced?
