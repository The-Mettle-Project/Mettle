# Zero-run profile-guided optimization: `--pgo`

Classic PGO demands three steps: build instrumented, run a training
workload, rebuild with the profile. Almost nobody does it. Mettle collapses
the loop to one flag: the compiler **interprets your program's own `main()`
while compiling it** (deterministic, sandboxed, externs modeled as pure,
fuel-capped at 64M steps) and feeds the measured execution frequencies
straight into the optimizer.

```
$ mettle --release --pgo app.mettle -o app.obj
pgo: interpreted main() at compile time - 63785953 steps (partial (fuel cap 64M steps)),
     2 functions touched, hot threshold 1024 calls
pgo:   keyed_step: 214046 calls  [hot]
```

## What the profile drives

**Hot-callee inlining.** The inliner's static caps exist because, without
evidence, inlining a large callee is a code-size gamble. A measured-hot
callee (>= `METTLE_PGO_HOT` calls, default 1024) IS the evidence: it gets
the same budget override an explicit `@inline` grants - the 128-instruction
budget widens to 512 and the 2-call glue cap to 6. The denylist and
structural guards still apply, and a cold or unprofiled callee keeps the
static heuristics, so `--pgo` can only unlock inlining, never lose it.

Inlining is the gateway optimization: once the call boundary dissolves,
constant arguments propagate into the callee's body. Combined with the
constant-folding upgrade below, a hot helper called with a compile-time key

```mettle
fn keyed_step(x: int64, k: int64) -> int64 {
    var m: int64 = k;
    m = m ^ (m >> 13);  m = m * 31;  m = m + 7;
    /* ... 40 rounds ... */
    return (x << 1) ^ m;
}
```

collapses to a single precomputed constant per call site: measured **2.1x**
end to end on the 30M-iteration driver (830ms -> 395ms, identical output),
from one flag.

## The constant-folding upgrade underneath

Landing PGO exposed a general optimizer gap: constant propagation knew a
variable's value but only *invalidated* it on arithmetic writes instead of
*computing* the new one, so `m = m ^ K; m = m * P; ...` chains never
folded. The pass now evaluates an integer `dest = a op b` the moment its
operands are proven constant, narrowing the result to the destination's
declared width (the canonical-homes contract: temps compute at 64-bit,
typed homes wrap on write). Operands are consulted, never rewritten, so
partially-constant expressions keep the shapes the SIMD recognizers
pattern-match. This fires in every `-O` build, not just under `--pgo`.

Both features were validated by `--verify` (translation validation) across
the example corpus and all of MettleWarband - 7225 pass applications, zero
divergences.

## Semantics and limits

- The interpreted run is approximate where the program touches the outside
  world: unknown externs return 0 and write nothing. Compute-bound code
  profiles faithfully; I/O-driven control flow profiles as "cold", which
  only means static heuristics apply there.
- Fuel exhaustion yields a partial profile - the first 64M steps of a
  program are usually exactly its hot loops.
- No main(), or main() outside the interpretable subset: the profile is
  absent and `--pgo` is a no-op (reported, never silent).
- The profile is deterministic, so builds stay reproducible.

`METTLE_PGO_HOT=<n>` adjusts the hot threshold. See also
[translation-validation.md](translation-validation.md) (the `--verify`
machinery this shares its interpreter with) and [testing.md](testing.md)
(`mettle test` / `mettle trace`).
