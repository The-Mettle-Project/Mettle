# Compile-time execution

The compiler carries an interpreter for its own IR. Two subcommands put it in
your hands: `mettle test` runs your tests without building anything, and
`mettle trace` shows you what a function did, line by line.

Neither generates code and neither links, so both answer instantly.

## Writing a test

Mark a function `@test`. It takes no arguments and returns `int32`, where 0
means it passed.

```mettle
@test fn adds() -> int32 {
  if (1 + 1 != 2) { return 1; }
  return 0;
}

@test fn slices() -> int32 {
  var s: string = "hello";
  if (s.length != 5) { return 1; }
  return 0;
}
```

## Running them

```bash
mettle test program.mettle
```

```text
running 2 tests (compile-time interpreter, no codegen)
test adds ... ok
test slices ... ok

2 passed (program.mettle)
```

A failure names the value that came back:

```text
test fails ... FAILED (returned 3; a test must return 0)

1 passed, 1 failed (program.mettle)
```

`--filter=S` runs the tests whose names contain `S`:

```bash
mettle test program.mettle --filter=add
```

```text
running 1 test (compile-time interpreter, no codegen)
test adds ... ok
```

Tests are stripped from a normal build, so `@test` functions cost nothing in
the program you ship.

## Tracing a function

`mettle trace <file> <fn> [args...]` interprets one function with the
arguments you give and prints the source with the values beside it:

```bash
mettle trace program.mettle total 4
```

```text
trace: total(n=4)

   1 | fn total(n: int32) -> int32 {
   2 |   var sum: int32 = 0;                <- sum = 0
   3 |   var i: int32 = 0;                  <- i = 0
   4 |   while (i < n) {
   5 |     sum = sum + i;                   <- sum = 0, 1, 3, 6 (4x)
   6 |     i = i + 1;                       <- i = 1, 2, 3, 4 (4x)
   7 |   }
   8 |   return sum;

returns 6
```

A line inside a loop shows the sequence of values it took and how many times
it ran, so a wrong recurrence shows up as the wrong series rather than as a
wrong final answer.

## What the interpreter can run

It models real memory, so most code runs:

- Integers, floats, `bool`, `char`
- Strings with their actual bytes, including literals and interpolation
- Structs and arrays, passed and returned by value
- Globals, with their initializers
- Heap allocation and the modeled string externs
- Closures, called through function-address tokens
- Tagged enums, `match`, `defer`

It stops at what it cannot model: a call into a foreign library whose behavior
it does not know, inline assembly, and anything that reads the operating
system. A function that reaches one of those reports an unsupported construct
rather than guessing.

## The same interpreter elsewhere

Three other features run on it, which is why its coverage matters:

- [`--pgo`](pgo.md) interprets `main()` to measure call frequencies.
- [`--verify`](translation-validation.md) executes each function before and
  after every optimizer pass and compares.
- `comptime for` expands over compile-time values.

## See also

- [Declarations](declarations.md)
- [Translation validation](translation-validation.md)
- [Profile-guided optimization](pgo.md)
