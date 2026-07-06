# Translation validation: `--verify`

Mettle can prove its own optimizer honest. With `--verify`, the compiler
validates **every optimization pass, on every function, during the compile**:
whenever a pass changes a function's IR, the before-IR and after-IR are both
executed by a built-in reference interpreter on identical generated inputs,
and every observable is compared. If a pass changed behavior, the compiler:

1. **names the exact pass and function**,
2. **prints a concrete counterexample** (the inputs and the differing
   observation), and
3. **quarantines the pass for that function and recompiles from the
   validated pre-pass IR** - the emitted binary is always built from IR that
   passed validation.

```
$ mettle --verify --build app.mettle -o app.exe

verify: MISCOMPILE CAUGHT: pass 'constant_and_branch_simplify' changed the
        observable behavior of function 'scale'
  counterexample (input set 0): scale(<buf:33 elems>, 8, 11)
  divergence: return value was -7316370850897153118, is now -4463642014434965641
  action: pre-pass IR restored; 'constant_and_branch_simplify' quarantined
          for 'scale'; compilation continues from validated IR

translation validation: 1 MISCOMPILE CAUGHT & QUARANTINED (64 validated)
```

A clean compile ends with:

```
translation validation: OK - 6761 pass applications validated on 3 input sets each
```

The nearest comparable tool is Alive2 for LLVM - an external research
harness. Mettle's validation is built into the compiler, runs per function at
compile time, produces runnable counterexamples, and self-heals.

## What is compared

Each function runs on 3 generated input sets (boundary-ish ints, floats,
seeded buffers for pointer parameters, NUL-terminated text for `cstring`).
The observation of a run is:

- the **return value** (integers bit-exact; floats within a small relative
  tolerance, because vectorized `+` reductions legitimately reassociate),
- the **final bytes of every buffer** (pointer arguments and heap
  allocations),
- the **ordered trace of extern calls** with their arguments - an unknown
  extern is modeled as pure and returns 0, but it is traced, so a pass that
  deletes, duplicates, or reorders an `fwrite` still diverges,
- the **final values of touched globals**.

Runtime guard traps (`mettle_crash_trap*`, the compiler's own null/bounds
checks) abort a run cleanly. Two runs that both guard-trap are equivalent
even if the crash point moved (the usual debug-checks contract); a pass that
makes a *completing* program trap - or a trapping program complete - is a
divergence.

## What is validated

All per-function passes: the fixpoint scalar pipeline (CSE, constant folding,
strength reduction, loop unrolling, SROA, ...) and every SIMD
recognizer/vectorizer. The interpreter implements the documented scalar
semantics of each SIMD kernel op from `ir.h`, so a vectorizer that emits a
kernel whose semantics differ from the loop it replaced is caught - the
`iv-start-zero` class of historical bugs is exactly this shape.

Not validated (reported per function in the summary):

- functions with `string`/struct-typed parameters or locals the input
  generator cannot synthesize,
- functions using `call_indirect` (closures) or inline asm,
- program-level passes (the inliner, pure-call hoisting),
- functions whose runs trap or exhaust fuel on all three input sets.

A skipped function is compiled normally; skipping is loud, never silent.

## Soundness of the net itself

A **false positive** (the input generator provoking a difference that could
not occur in a real execution) costs only optimization: the function keeps
its validated pre-pass IR. It can never miscompile. A **false negative** is
possible in principle - three input sets are not a proof - but the inputs
exercise loop bodies, boundaries, and aliased memory, and every historical
Mettle miscompile class (wrong constant, wrong sign, wrong first index,
clobbered accumulator) diverges on almost any input.

## Findings to date

On its first run over MettleWarband (~20k lines), `--verify` caught two real
latent soundness bugs in `null_check_licm`, both now fixed:

- hoisting a null-check trap above a loop with **no proof the loop runs**:
  a legal state (null pointer + zero-trip loop) began trapping. Fixed by
  guarding the hoisted check with a re-evaluation of the loop entry
  condition.
- hoisting a null-check that sat **under a condition inside the loop body**
  (`if (keep) { buf[i] = x; }`): iterations that never reached the check
  began trapping. Fixed by requiring a straight control-line from loop entry
  to the check.

## Self-test

`METTLE_VERIFY_BREAK="pass_name[:function]"` deliberately corrupts one IR
constant right after the named pass, proving the whole detect -> report ->
quarantine -> heal loop end to end. The regression suite runs this on every
build (`verify_sabotage_caught`), alongside `verify_clean` and
`verify_nullcheck_zerotrip`.

## Cost

Roughly 3-6x compile time on large programs (MettleWarband: ~7s -> ~37s);
small programs are near-instant. Use it in CI, before releases, or whenever
a release-mode result looks suspicious. `--verify` implies `-O`.
