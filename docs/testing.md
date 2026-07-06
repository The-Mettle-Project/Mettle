# Compile-time execution: `mettle test` and `mettle trace`

Mettle's compiler contains a reference interpreter for its own IR. Two
everyday workflows run on it - no codegen, no linker, no process spawn, so
feedback is effectively instant:

## `mettle test` - tests that live inside the compiler

```mettle
fn fib(n: int64) -> int64 {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}

@test fn test_fib() -> int64 {
    assert_eq(fib(0), 0);
    assert_eq(fib(10), 55);
    assert(fib(12) > fib(11));
    return 0;
}
```

```
$ mettle test app.mettle

running 1 test (compile-time interpreter, no codegen)
test test_fib ... ok

1 passed (app.mettle)
```

- `@test` functions take no parameters and return `int64` (0 = pass). They
  are **type-checked in every build** - broken tests fail a normal compile -
  but their code is **compiled out of normal binaries**, so tests cost
  nothing at runtime and need no separate build target.
- `assert(cond)` and `assert_eq(left, right)` are test builtins the
  interpreter implements natively. A failure renders as a full compiler
  diagnostic with the source snippet, a caret on the assertion, and the
  actual values:

  ```
  error[E0003]: assertion failed in test 'test_fib_wrong'
    --> app.mettle:24:5
  24 |     assert_eq(fib(10), 54);
     |     ^^^^^^^^^ left: 55, right: 54
  ```

  Calling them outside a `@test` function is a compile error.
- **Every test doubles as a memory sanitizer.** The interpreter owns the
  heap, so an allocation a test never frees is reported with its allocation
  line - sanitizer findings without ever running a binary:

  ```
  test test_leaky ... ok, but LEAKED
  warning[E0003]: test 'test_leaky' leaked 24 bytes: this allocation is never freed
    --> app.mettle:37:1
  ```

  Null dereferences and out-of-bounds accesses fail the test the same way.
- `--filter=SUBSTR` runs matching tests only. Add `-O`/`--release` to test
  the optimized IR instead of the debug shape.
- A test using constructs outside the interpretable subset (strings,
  closures, real I/O) is reported `skipped` with the reason - run those
  through a normal `--build`.

Exit code is nonzero when any test fails, so `mettle test` slots straight
into CI.

## `mettle trace` - see your function run, line by line

```
$ mettle trace app.mettle sum_range 0 10

trace: sum_range(lo=0, hi=10)

   6 | fn sum_range(lo: int64, hi: int64) -> int64 {
   7 |     var total: int64 = 0;                            <- total = 0
   8 |     var i: int64 = lo;                               <- i = 0
   9 |     while (i < hi) {
  10 |         total = total + i;                           <- total = 0, 1, 3, 6, ..., 45 (10x)
  11 |         i = i + 1;                                   <- i = 1, 2, 3, 4, ..., 10 (10x)
  12 |     }
  13 |     return total;

returns 45
```

Print-debugging without prints: the function is interpreted on the given
arguments and its source is printed with the values every line produced -
loop iterations are compressed to first samples, the last value, and a
count. Int and float parameters take the CLI values in order; pointer
parameters get a synthesized 33-element seeded buffer (shown as
`<buf:33 x int64>`). Crashes report the guard trap instead of a value.

## How this differs from other languages

`zig test` / `cargo test` / `go test` compile, link, and execute a test
binary. Mettle interprets the IR inside the compiler process: there is no
artifact, feedback scales with test size rather than program size, heap
misuse is caught by construction, and assertion values come back through
the same diagnostic pipeline as compile errors. The same interpreter powers
`--verify` (translation validation of the optimizer itself, see
[translation-validation.md](translation-validation.md)), so the semantics
your tests run on are the semantics the optimizer is held to.
