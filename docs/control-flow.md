# Control Flow

Mettle provides structured control flow: conditionals, loops, and switches. All control structures use braces for the body.

## Assignment

Assignment uses `=`. The left side must be an lvalue (variable, struct field, array element, or dereferenced pointer). Assignment is a statement; it does not produce a value for use in larger expressions.

```mettle
x = 42;
ptr->field = value;
arr[i] = x;
```

**Compound assignment** (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) is syntactic sugar for `target = target OP value`, where `OP` is the corresponding binary operator. The left side must be the same kind of lvalue as for plain assignment. Compound assignment is a statement. It produces no value for use in a larger expression. It is valid in `for`-loop initializers and increments.

```mettle
count += 1;
arr[i] *= 2;
for (var i: int32 = 0; i < 10; i += 1) {
  // ...
}
```

See [Lexical Structure](lexical-structure.md#operators-and-punctuation) for the full operator list.

**Type mismatches** produce a compile error. Assigning a value of incompatible type (e.g. `x = 3.14` where `x` is `int32`) is rejected; the compiler does not silently truncate.

## If and Else

The `if` statement evaluates a condition. If true, the then branch runs. The optional `else` branch runs when the condition is false. The condition must be a **numeric type** (integer or floating-point); zero is false, non-zero is true. A pointer is not a valid condition. Compare it: write `if (ptr != 0)` to test for non-null.

```mettle
if (x > 0) {
  // ...
} else if (x < 0) {
  // `else if` is parsed as part of the if statement
} else {
  // ...
}
```

`else if` chaining is fully supported as a contiguous sequence of conditions, avoiding deep AST nesting. There is no separate `elseif` keyword.

## While

The `while` loop evaluates the condition. If true, the body runs and the condition is evaluated again. The loop exits when the condition is false.

```mettle
while (condition) {
  // ...
}
```

Common patterns:

```mettle
// Iterate over an array
var i: int32 = 0;
while (i < len) {
  arr[i] = arr[i] * 2;
  i = i + 1;
}

// Infinite loop (e.g. accept loop in a server)
while (1) {
  // ...
}
```

An infinite loop is written `while (1)`; the condition is always true.

## For

The `for` loop has an initializer, condition, and increment. The initializer runs once. The condition is evaluated before each iteration; if false, the loop exits. The increment runs after each iteration. The initializer can declare a variable. Condition and increment are optional, so `for (;;)` is a valid infinite loop.

```mettle
for (var i: int32 = 0; i < 10; i = i + 1) {
  // ...
}
```

**Scope:** A variable declared in the initializer (e.g. `var i`) is scoped to the loop. It is not accessible after the loop exits.

**Infinite loop:** Use `for (;;)` when all three parts are omitted. This is idiomatic in systems code.

### Range-based for

`for i in lo..hi { ... }` iterates `i` over a half-open range. `lo..hi` is
**exclusive** of `hi`; `lo..=hi` is **inclusive**:

```mettle
for i in 0..n      { sum = sum + a[i]; }   // i = 0, 1, ..., n-1
for i in 0..=n     { /* i = 0, 1, ..., n */ }
for i: int64 in 0..count { /* loop variable typed explicitly */ }
```

The loop variable's type is inferred from the start bound, or you may annotate
it (`for i: int64 in ...`). A range-based `for` desugars at parse time into the
ordinary counted `for` above: the start bound is evaluated once, the end bound
is re-evaluated each iteration (so hoist a call-valued bound yourself if that
matters). Labels work as usual: `outer: for i in 0..n { ... }`.

> The `..`/`..=` distinction here is exclusive/inclusive. Note that switch-case
> ranges (`case lo..hi:`) use `..` as **inclusive**, a historical inconsistency
> to be aware of.

## Vectorization contracts

A counted loop may carry a `@simd` attribute that asks the optimizer to
vectorize it. This only has effect under `-O` / `--release` (the auto-vectorizer
runs only when optimizing); plain debug builds print one note that the contracts
were not checked.

```mettle
@simd  for i in 0..n { c[i] = a[i] + b[i]; }   // best-effort: warn if not vectorized
@simd! for i in 0..n { c[i] = a[i] + b[i]; }   // contract: compile ERROR if not vectorized
```

- **`@simd`** is a hint. If the loop vectorizes, nothing is printed; if it does
  not, the compiler emits a *warning* explaining why and keeps the scalar loop.
- **`@simd!`** is a hard contract. If the loop does not vectorize, compilation
  **fails** with an error and a precise reason; the performance guarantee
  cannot silently regress.

Both attributes also apply to `while` loops. The diagnostic names the cause when
it can determine it: a function call in the body, control flow (a nested loop or
data-dependent branch), an unsupported element width (16- or 64-bit integers
have no kernel), a loop-carried serial recurrence (a scalar computed from its
own previous value through a non-reassociable operation (`*`, `/`, a shift, or
a bitwise or xor op), so the iterations form a dependency chain, e.g. a hash, an
RNG, or an IIR filter), or, when none of those apply, that no vectorizer
recognized the loop's shape. The recurrence cause is found by backward
data-flow analysis, and `+`/`-` reductions are excluded from it, because those
reassociate and vectorize.

`@simd` may also sit on a **function**, where it becomes the default contract
for *every* counted loop in the body that does not carry its own `@simd`:

```mettle
@simd! fn sum(a: int32*, n: int64) -> int64 {
  var s: int64 = 0;
  var i: int64 = 0;
  while (i < n) { s = s + (int64)a[i]; i = i + 1; }   // inherits @simd! from the function
  return s;
}
```

A per-loop attribute always wins over the function default, so you can place a
function-wide `@simd` and still relax (or tighten) an individual loop. Note that
`@simd!` on a function is a hard contract on *all* its counted loops. If the
body mixes vectorizable and non-vectorizable loops, annotate the loops
individually instead. See [Function decorators](declarations.md#function-decorators).

### `--simd-report`

Pass `--simd-report` (with `-O`/`--release`) to have the compiler report what
each `@simd` loop became:

```
kernels.mettle:10:10: note: @simd loop vectorized (simd_dot_i8)
kernels.mettle:21:9:  warning: @simd loop was not vectorized: the loop body contains a function call
```

This makes the optimizer's decision legible instead of a black box: you can see
exactly which kernel a loop lowered to, or why it stayed scalar.

## Switch

The `switch` statement evaluates an expression and compares it to each `case` value. Case values must be compile-time constant integer expressions (including enum variants and `true`/`false`). When a case matches, its body runs. Use `break` to exit the switch. Use `continue` inside a loop that contains the switch to continue the loop. Only one `default` clause is allowed.

**Range cases:** A case may match an inclusive interval with `case lo..hi:`, where both bounds are compile-time constant integer expressions and `lo <= hi`. The case runs when the switch value is in `[lo, hi]`. Cases are tested top to bottom and the first match wins, so a single-value case listed before an overlapping range still takes precedence.

**Fall-through:** Unlike some languages, Mettle does not enforce `break`. If you omit it, execution falls through to the next case (C-style behavior). To avoid accidental bugs, always end each case with `break` explicitly unless you intend fall-through.

**Exhaustiveness:** `switch` over raw integers may omit matching cases and continue after the statement if no case matches. `switch` over `enum` or `bool` must be exhaustive unless a `default` clause is present.

```mettle
switch (expr) {
  case 1:
    // ...
    break;
  case 2:
    // ...
    break;
  case 3..9:        // inclusive range: matches 3 through 9
    // ...
    break;
  default:
    // ...
}
```

## Match

The `match` statement branches on a tagged enum and optionally binds the payload of a variant. The subject expression must have a tagged-enum type.

```mettle
match (value) {
  case Some(v): {
    return v;
  }
  case None: {
    return 0;
  }
}
```

**Arms:** Each `case` arm has a variant name and a block body. Use `case VariantName(binding):` when that variant carries a payload and you want to bind it to a local name. Use `case VariantName:` for payloadless variants.

**Default arm:** `default:` is allowed. Without `default`, the match must cover every variant of the tagged enum.

**No fall-through:** `match` arms do not fall through. Once an arm matches, its block runs and control continues after the `match`.

### match as an expression

`match` also exists in an expression form that yields a value. Each arm body is a single value-yielding expression rather than a block, and arms may be separated by commas, newlines, or semicolons:

```mettle
fn unwrap_or(o: Option, fallback: int32) -> int32 {
  return match (o) {
    case Some(value): value
    case None: fallback
  };
}

var doubled: int32 = match (Some(10)) {
  case Some(v): v
  default: 0
} * 2;
```

All arm bodies must have a compatible type, and because the expression has to produce a value, it must be exhaustive: cover every variant or supply a `default`. The statement form above is the right choice when the arms run several statements or diverge; the expression form is the right choice when each arm is a single value.

## Break and Continue

`break` exits the innermost loop or switch. `continue` skips to the next iteration of the innermost loop. Both are context-checked; they are valid only inside loops or switches. Using them elsewhere is a compile error.

**Important:** a bare `break` or `continue` targets the **innermost** enclosing loop or switch. Inside nested loops, `break` exits the inner loop and leaves the outer one running. Inside a `switch` that sits in a loop, `break` exits the switch and the loop keeps going; write `continue` to skip to the next loop iteration. To reach further out, label the loop and name it (see below).

```mettle
while (1) {
  switch (cmd) {
    case 0:
      break;      // exits switch only, loop continues
    case 1:
      continue;   // skips to next loop iteration (exits switch and continues loop)
    case 2:
      break;      // exits switch
  }
  // ...
}
```

### Labeled break and continue

A `while` or `for` loop may carry a label, written `name:` immediately before
the loop keyword. `break name` then exits that labeled loop, and
`continue name` jumps to the next iteration of that labeled loop, regardless of
how deeply nested the statement is:

```mettle
outer: for (var i: int32 = 0; i < n; i = i + 1) {
  for (var j: int32 = 0; j < m; j = j + 1) {
    if (grid[i][j] == target) {
      break outer;     // exits BOTH loops
    }
    if (skip[j]) {
      continue outer;  // next i, abandoning the rest of the j loop
    }
  }
}
```

Rules and limits:

- Labels attach only to `while` and `for` loops. Writing `name:` before any
  other statement is a compile error.
- The label in `break name` / `continue name` must match the label of an
  enclosing loop; an unknown label is a compile error
  (`'break NAME' has no matching labeled loop`).
- `continue name` requires the target to be a loop (every labeled loop is, so
  this always holds for valid labels).
- Unlabeled `break`/`continue` still target the innermost loop or switch as
  before.
- Labels live in their own namespace and do not collide with variable or
  function names.
- The jump skips every deferred statement between it and the labeled loop,
  including the labeled loop's own. See
  [What defer does not cover](#what-defer-does-not-cover).

## Return

`return` exits the current function. A function with a return type must provide a value: `return value`. A void function uses `return` with no value.

```mettle
return;
return value;
```

## Short-Circuit Evaluation

Logical operators `&&` and `||` support short-circuit evaluation. For pointer checks like `ptr != 0 && ptr->field > 0`, a single condition is safe:

```mettle
if (ptr != 0 && ptr->field > 0) {
  // ...
}
```

## Defer and Errdefer

`defer` runs a statement when the current scope exits. `errdefer` runs one when
the function returns a non-zero value. Deferred statements run in reverse order
of declaration, so the last one written runs first.

Every example below was compiled and run, and the output shown is what it
printed.

### Forms

```mettle
defer cleanup();          // a call
defer count = count + 1;  // an assignment
defer {                   // a block
  flush();
  close(handle);
}
errdefer rollback();      // a call, on a non-zero return
```

A deferred direct call copies its argument values where the `defer` is written.
In a loop, `defer print_int(i)` records `i` as it stands on that iteration, so
the calls print `0`, `1`, `2`. Method calls and calls through a function pointer
read their operands at scope exit, so copy the value into a local first if you
need the one from the defer point:

```mettle
var current: int32 = i;
defer obj.m(current);
```

### Order

```mettle
fn lifo() -> int32 {
  defer println("first");
  defer println("second");
  defer println("third");
  println("body");
  return 0;
}
```

```
body
third
second
first
```

### Scope

A block runs its deferred statements when the block ends. The function runs its
own when it returns.

```mettle
fn nested() -> int32 {
  defer println("outer cleanup");
  {
    defer println("inner cleanup");
    println("inner body");
  }
  println("after block");
  return 0;
}
```

```
inner body
inner cleanup
after block
outer cleanup
```

A loop body is a block, so a `defer` written inside one runs at the end of each
iteration:

```mettle
fn each() -> int32 {
  var i: int32 = 0;
  while (i < 3) {
    defer println("iteration cleanup");
    println("iteration start");
    i = i + 1;
  }
  return 0;
}
```

```
iteration start
iteration cleanup
iteration start
iteration cleanup
iteration start
iteration cleanup
```

### Errdefer

`errdefer` is valid only inside a function. The rule is a convention on the
return value: zero means success, and every other value means an error.

```mettle
fn work(n: int32) -> int32 {
  defer println("defer always");
  errdefer println("errdefer only");
  if (n == 1) {
    return 42;
  }
  return 0;
}
```

`work(0)` prints `defer always`. `work(1)` prints `errdefer only`, then
`defer always`. The same split applies when a function body ends without a
`return`.

### What defer does not cover

Four ways out of a scope skip the deferred statements. Free the resource by
hand on these paths.

**`break` skips the loop body's deferred statements.**

```mettle
fn br() -> int32 {
  var i: int32 = 0;
  while (i < 3) {
    defer println("iter defer");
    println("iter body");
    i = i + 1;
    if (i == 1) { break; }
  }
  return 0;
}
```

```
iter body
```

**`continue` skips them for the iteration it leaves.** Later iterations that
reach the end of the body run them as usual.

**`break name` and `continue name` skip every deferred statement between the
jump and the labeled loop,** including the labeled loop's own.

**A `switch` case body never runs its deferred statements.** A `defer` written
inside a case is dropped.

```mettle
fn sw() -> int32 {
  switch (1) {
    case 1: {
      defer println("case defer");   // never runs
      println("case 1");
    }
  }
  return 0;
}
```

```
case 1
```

These four gaps are recorded in [Known Limitations](known-limitations.md).

### Cost

Each `defer` copies its arguments and adds the deferred statement to the exit
path of its scope. A function with `errdefer` compiles two exit paths and picks
one by testing the return value. Clean up by hand in a hot loop where that
matters.

## Unreachable Code
The compiler emits a warning for unreachable statements that appear after an unconditional `return`, `break`, or `continue` in the same block.
