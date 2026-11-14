# Proofs

Mettle carries a theorem prover and an SMT solver inside the compiler. A
`theorem` states a claim about your functions, a proof script guides the
prover, and the build fails when a proof does not go through. The same
solver proves the optimizer's own rewrites and checks your `rewrite` rules
and the passes `--verify` watches.

```mettle
fn mul_by_shifts(x: uint64, c: uint64) -> uint64 {
    if (c == 0) { return 0; }
    var rest: uint64 = mul_by_shifts(x << 1, c >> 1);
    if ((c & 1) == 1) { return rest + x; }
    return rest;
}

theorem mul_splits_low_bit(x: uint64, c: uint64) {
    x * c == (x << 1) * (c >> 1) + x * (c & 1)
} by smt;

theorem mul_by_shifts_is_mul(x: uint64, c: uint64) {
    mul_by_shifts(x, c) == x * c
} proof {
    induction c;
    use mul_splits_low_bit(x, c);
    auto;
}
```

The second theorem says the shift-and-add multiplier a code generator would
emit computes `x * c` for every pair of 64-bit inputs. The proof takes 0.13
seconds.

## Theorems

```mettle
theorem name(parameters) where condition {
    claim
} by tactic;
```

The claim is a `bool` expression over the parameters. The `where` clause is
optional and restricts the parameters the claim has to hold for. A theorem
holds when, for every value of every parameter that satisfies `where`, the
claim evaluates to `true` without trapping.

A proof is either one tactic after `by` or a block of tactics after `proof`.
A theorem with neither is proved by `auto`.

The claim, the `where` clause and every expression a tactic names become
ordinary functions before type checking. They are type checked and lowered
like the rest of the program, and the prover reads the lowered IR. A theorem
therefore talks about the code the compiler generates, with the semantics the
compile-time interpreter gives it: integers are 64-bit values narrowed when
they are stored in a typed variable, shift counts are masked to 63, division
by zero traps and `INT64_MIN / -1` wraps.

Theorems are checked on every build, with or without `-O`. They are dropped
before code generation and cost nothing at run time.

## How the prover sees a function

A call stays a call until a tactic unfolds it. Unfolding replaces the call
with the callee's body, which may itself contain calls. A function can be
unfolded when the prover can give it a definition:

```mermaid
flowchart TD
  F[function] --> E{body encodes?}
  E -- loops, memory, floats --> O[opaque]
  E -- yes --> R{reads only its parameters?}
  R -- globals, uninitialized locals --> O
  R -- yes --> C{recursive?}
  C -- no --> D[defined]
  C -- directly --> M{a parameter decreases at every recursive call?}
  M -- yes --> D
  M -- no --> O
  C -- through another function --> O
```

The decrease is proved by the solver, in the signed or unsigned order of the
parameter's type. Both orders are well founded on a fixed-width integer, so a
definition with a decreasing parameter terminates and its equation is sound to
use. An opaque function can still appear in a theorem. The prover knows
nothing about it except that equal arguments give equal results.

When a proof fails, the error lists every function the proof touched that has
no definition, with the reason.

## Tactics

A proof works on a list of goals. Each goal has hypotheses and a conclusion,
all over the theorem's parameters. Tactics act on the first goal.

- `smt` closes the goal if the solver proves the hypotheses imply the
  conclusion.
- `auto` unfolds defined functions that do not recurse, tries `smt`, then
  unfolds recursive functions one level and tries again.
- `unfold f, g` replaces every call to the named functions with their bodies,
  one level. With no names it unfolds every defined function.
- `induction n` adds the strong induction hypothesis on parameter `n`: the
  claim holds for every smaller `n` in the order of `n`'s type, with every
  other parameter free.
- `cases e` splits the goal in two, one with `e` as a hypothesis and one with
  `!e`.
- `assert e` makes `e` the next goal and adds it as a hypothesis to the goal
  after it.
- `use t(a, b)` adds an earlier proved theorem `t`, instantiated at the given
  arguments, as a hypothesis.
- `simp` rewrites the goal into the normal form the solver uses.
- `show` prints the goals.

An induction hypothesis is used by `smt` and `auto` through its calls: every
call in the goal that matches a call in the hypothesis yields an instance. In
`mul_by_shifts_is_mul`, unfolding `mul_by_shifts(x, c)` exposes
`mul_by_shifts(x << 1, c >> 1)`, and the hypothesis instantiated there states
that call equals `(x << 1) * (c >> 1)`.

A proof that ends with goals left is an error, and so is a tactic that fails.

## Failed proofs

A failing `smt` or `auto` reports the goal it could not close and, when the
solver found one, the parameter values where the goal fails:

```text
abs.mettle:8:6: error: theorem `abs_is_nonnegative`: auto could not close the goal; with the definitions unfolded so far it fails at x = -2147483648
  remaining goals:
    goal 1 of 1 (x: int32)
      |- (0 <= ((x < 0) ? (int32)(-x) : x))
```

The values are exact when every function in the goal has been unfolded. A
call that is still folded can make the failure spurious, which is why the
message says "with the definitions unfolded so far".

## mettle prove

```bash
mettle prove program.mettle
mettle prove program.mettle --goals
mettle prove program.mettle --interactive gauss
```

`mettle prove` checks every theorem and prints one line per theorem with the
tactics run, the solver calls made and the time taken. `--goals` prints the
goals after every tactic. `--interactive` runs the named theorem's script and
then reads more tactics from standard input, printing the goals after each one,
until no goals remain or a blank line ends the session.

## The solver

The solver decides formulas over Booleans, bit-vectors up to 256 bits, arrays
and uninterpreted functions:

```mermaid
flowchart LR
  T[terms] --> S[simplify while building]
  S --> A[remove arrays and functions by Ackermann expansion]
  A --> N[normalize arithmetic to polynomials]
  N --> B[bit-blast with shared gates]
  B --> C[CDCL SAT]
  C -- unsat --> P[replay the refutation in a DRUP checker]
  C -- sat --> V[evaluate the assertions on the model]
```

Terms are hash-consed, so a subterm built twice is one node. The simplifier
folds constants, applies Boolean and bit-vector identities, and turns division
by a power of two into shifts and masks.

The normalizer rewrites sums, differences, negations, shifts by constants and
products into polynomials over Z/2^w with sorted monomials. An equation
becomes one polynomial set to zero, with negative terms moved to the other
side. Reassociated and strength-reduced arithmetic meets in one form, and the
SAT solver never sees it. Inside a negated equation it also writes `v` as
`2^k * (v >> k) + (v & (2^k - 1))` when both pieces appear, which is how
`mul_splits_low_bit` is proved without a multiplier circuit.

The SAT solver uses two watched literals with blocking literals, VSIDS
decisions with phase saving, first-UIP learning with recursive minimization,
restarts driven by the moving average of clause quality, and clause deletion
by literal block distance.

Every `unsat` answer is replayed through an independent DRUP checker that
shares no code with the SAT solver. A refutation that fails the replay turns
the answer into `unknown`. Every `sat` answer is checked by evaluating the
original assertions on the model. A wrong answer from the search therefore
cannot reach a proof. The bit-blaster, the simplifier and the normalizer are
checked by a differential fuzzer that compares each against the evaluator.

Quantified formulas are treated as opaque atoms. That is sound for proofs,
and a `sat` answer on a formula that contained one is reported as `unknown`.

## mettle smt

```bash
mettle smt problem.smt2
mettle smt --stats problem.smt2
```

`mettle smt` reads SMT-LIB 2: `declare-const`, `declare-fun`, `define-fun`,
`assert`, `push`, `pop`, `check-sat`, `get-model`, `get-value` and `echo`, with
the `QF_ABV` operators, indexed operators, `let` and `ite`. `--stats` prints
the term, gate, clause and conflict counts and the time spent blasting,
searching and checking. `--no-certify` skips the refutation check.

## The compiler proves its own rewrites

`mettle prove-compiler` proves every row of the optimizer's identity table in
`src/ir/optimizer/ir_optimize_rewrite.c`, for signed and unsigned operands, in
both operand orders where a row is commutative, and for every shift a
power-of-two pattern allows:

```text
  proved   x>=0 % 2^k  =>  x & (2^k - 1)
  proved   x || c!=0  =>  1              (both orders)
30 identities, 570 obligations over 64-bit signed and unsigned operands, 0 refuted, 15.0 ms
```

A row the solver refutes is printed with the input that breaks it.

It then runs the x86-64 backend's own division lowering,
`mir_emit_const_divmod` in `src/codegen/binary/mir_lower.c`, for every
power-of-two divisor from 1 to 2^62, their negations, `-1` and `INT64_MIN`,
signed and unsigned, quotient and remainder. It executes the MIR instructions
that come out symbolically and proves each sequence computes the IR's `/` or
`%` for every 64-bit dividend:

```text
382 constant divisions lowered by mir_emit_const_divmod: 382 proved, 0 refuted, 0 not decided, 1048.7 ms
```

With the rounding bias removed from the signed power-of-two sequence, 250 of
the 382 are refuted, each with a dividend that shows the error.

Division by other constants uses a multiply-high by a magic number. Proving
those for all 64-bit dividends is beyond bit-blasting: the solver does not
finish `x / 10` in 100 seconds, even for 32-bit dividends. They are checked by
`--verify` on generated inputs.

[Rewrite rules](rewrite-rules.md) are proved for every input before the
compiler applies them, and fall back to generated inputs only when the solver
cannot model a side, such as one that computes with floats.
[Translation validation](translation-validation.md) proves each pass
application on a loop-free integer function before it runs inputs.
