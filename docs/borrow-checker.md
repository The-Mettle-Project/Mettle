# The Mettle borrow checker

Mettle has a borrow checker. It is not Rust's.

Rust's borrow checker is a **sound, mandatory gate**: it proves the *absence* of
a class of memory bugs in safe code, and to do so it rejects every program it
cannot prove safe. That guarantee is real and valuable — and it comes with two
well-known costs. It rejects correct programs (the "fighting the borrow checker"
tax), and it goes completely dark the moment you reach for a raw pointer: inside
`unsafe`, dereferencing a dangling `*mut T` is not checked at all.

Mettle makes the opposite trade, on purpose. Its borrow checker is an
**advisory analysis with a zero-false-positive budget**: it never rejects a
valid program, it warns only when it can *prove* a bug, and it does this on the
raw pointers a systems language actually uses. Where Rust says "I cannot prove
this safe, so no," Mettle says "I can prove this is a bug, here is the line and
the fix" — and stays quiet otherwise.

For a language with manual memory and raw pointers, that is the better trade,
and along the axes that make borrow checkers painful — false positives, the
`unsafe` blind spot, diagnostics, and forced rewrites — it is strictly better.
This document makes that claim concrete, and is honest about where Rust's
guarantee is still stronger.

## What it checks

All of these are inferred with no annotations required. They run during
type-checking and are surfaced in `--explain` (prose + the `.explain.json`
`memory` array, rendered in the editor's Memory tab).

| Class | Example | Rust analogue |
|-------|---------|---------------|
| Use-after-free (direct) | `free(p); p[0]` | ownership / `Drop` |
| Double free | `free(p); free(p)` | ownership |
| Use-after-free through an alias | `q = p; free(q); p[0]` | move semantics |
| Use-after-free across a call (inferred) | `consume(p); p[0]` where `consume` frees `p` | move semantics |
| Borrow outliving its stack scope | `{ var x; g = &x[0]; } use(g)` | lifetimes (`'a`) |
| Interior pointer after `realloc` | `q = &buf[i]; realloc(buf,..); q[0]` | iterator invalidation |
| Interior pointer after `free` | `q = &buf[i]; free(buf); q[0]` | dangling reference |
| Returning the address of a stack local | `return &local` | `fn() -> &T` lifetime error |
| Leak (no owner on any path) | `var p = malloc(n); return 0` | (Rust frees via `Drop`) |

Plus the conservative siblings that share the same walker: constant
out-of-bounds indexes, loop final-iteration overruns, null/“wild” constant
dereferences, division-by-zero, over-width shifts, and memcpy/memset writes that
overflow a fixed destination.

The design rule is uniform: a definite-bug fact is only set on a function's
straight-line *spine*. Anything inside a branch or loop demotes to "maybe" and
stays silent, and reassignment clears stale facts. That is what buys the
zero-false-positive budget.

## Two ways it improves on Rust's checker

### 1. It never rejects a valid program

The borrow checker you fight is the one that says no to code you know is
correct. Mettle never does. The canonical example is two mutable interior
pointers into disjoint elements of the same array:

```mettle
var a: int32[4];
a[0] = 1;
a[1] = 2;
var x: int32* = &a[0];
var y: int32* = &a[1];   // a second mutable interior pointer — fine
x[0] = x[0] + 10;
y[0] = y[0] + 20;
return a[0] + a[1];
```

Mettle compiles this **silently** — it is not a bug. The equivalent Rust is
rejected:

```rust
let mut a = [1, 2, 3, 4];
let x = &mut a[0];
let y = &mut a[1];       // error[E0499]: cannot borrow `a` as mutable
                         //               more than once at a time
*x += 10;
*y += 20;
```

Rust cannot see that the two borrows are disjoint, so it refuses the program;
you reach for `split_at_mut` or indices. Mettle only warns when it can prove the
pointer is actually dangling, so disjoint borrows, self-referential structures,
conditional borrows, and the other shapes that need `unsafe`, `RefCell`, or a
rewrite in Rust all compile without a fight.

### 2. It checks raw pointers — where Rust's checker is off

This is the big one. In Rust, manual memory means `unsafe` and raw pointers, and
inside `unsafe` the borrow checker provides **no** use-after-free or
use-after-move checking on raw-pointer dereferences. Mettle checks exactly that
code:

```mettle
fn uaf_through_alias() -> int32 {
    var buf: cstring = malloc(16);
    buf[0] = 7;
    var q: cstring = buf;     // q and buf name the same block
    free(q);
    return (int32)buf[0];     // use-after-free through the other name
}
```

```
warning[E0003]: Use of `buf` after the block it shares with `q` was freed at
line 8; freeing one alias frees the block both names point to, so this is
use-after-free
  = help: `buf` and `q` name the same block; free it once, after the last use
          of either
```

The Rust transliteration uses `*mut`, `Box::from_raw`, and `drop`; the
read-back of the freed pointer is a real use-after-free that Rust's borrow
checker does not flag, because raw-pointer dereferences are the programmer's
responsibility inside `unsafe`. Mettle tracks ownership *through the pointer
copy* — the same single-owner discipline Rust enforces in its type system — and
reports it. Freeing the original and reading the alias (`free(buf); q[0]`),
double-freeing through two names (`free(a); free(b)`), and reading an interior
pointer after the block is `realloc`'d are all caught the same way — and none of
it asks you to annotate a single pointer. Where Rust makes you write `&`, `&mut`,
`T`, and lifetime parameters to express ownership, Mettle's analysis is entirely
inference-based: ordinary C-style code, checked as written.

## What Rust still does that Mettle does not

This would not be an honest comparison without it. Rust's checker is *complete*
within safe code in a way Mettle's deliberately is not:

- **It is a soundness guarantee.** Rust *proves the absence* of these bugs in
  safe code; Mettle proves the *presence* of a subset of them. Mettle has false
  negatives by design — a use-after-free buried inside a data-dependent branch,
  or laundered through memory the analysis does not model, will compile clean.
  Mettle narrows the bug surface and explains what it finds; it does not certify
  a program bug-free.
- **Aliasing-as-UB and data races.** Rust's `&mut` exclusivity also underpins
  `Send`/`Sync` thread-safety and the no-aliasing optimizations LLVM relies on.
  Mettle's raw pointers may freely alias; it checks *lifetime* and *ownership*
  bugs, not aliasing-induced UB or data races across threads.
- **Whole-program lifetime inference.** Mettle's interprocedural reach is
  ownership summaries inferred to a fixpoint over the call graph (who frees, who
  stores, who returns fresh). It does not infer general lifetime relationships
  the way Rust's region inference does.

The summary: if you want a machine-checked proof that a *safe* program has no
use-after-free, Rust's gate is stronger. If you are writing a systems language
with raw pointers and manual memory and you want a checker that catches real
bugs there, never rejects correct code, and never makes you fight it — Mettle's
is better, and it is better precisely where Rust's stops looking.

## Trying it

Every diagnostic above is reproduced by a test fixture:

- `tests/warn_use_after_move.mettle` — alias use-after-free / double-free
- `tests/warn_borrow_scope.mettle` / `_realloc` / `_free` — borrow lifetimes
- `tests/warn_mem_diagnostics.mettle`, `tests/warn_mem_interproc.mettle` — the
  use-after-free / double-free / leak family
- `tests/no_warn_borrow_clean.mettle` — the zero-false-positive guard

Compile any of them with `--explain --explain-json` to see the same findings in
the optimization report and the JSON sidecar.
