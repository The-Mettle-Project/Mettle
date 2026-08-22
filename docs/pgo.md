# Profile-guided optimization

`--pgo` measures which functions your program calls most, then tells the
optimizer. It does that by running `main()` at compile time. There is no
instrumented build and no training run.

```bash
mettle --pgo --release --build program.mettle
```

`--pgo` implies `-O`.

## What it does

The compiler interprets `main()` in the same
[compile-time interpreter](testing.md) that runs `mettle test`, counting calls
as it goes. Any function whose count clears the threshold is treated as hot,
and a hot callee bypasses the inliner's static size budget the same way an
explicit `@inline` does.

The interpretation is deterministic and sandboxed. It reads no files and
touches no devices.

## What it prints

```text
pgo: interpreted main() at compile time - 17053 steps (ran to completion), 9
functions touched, hot threshold 1024 calls
pgo:   helper: 1000 calls
pgo:   print: 1 calls
pgo:   get_stdout: 1 calls
pgo:   fwrite: 1 calls
pgo:   println: 1 calls
```

The first line says how far it got. "ran to completion" means the whole of
`main()` was interpreted. A program that reaches something the interpreter
cannot model stops there, and the counts up to that point are still used.

## The threshold

`METTLE_PGO_HOT` sets the call count that makes a function hot:

```bash
METTLE_PGO_HOT=100 mettle --pgo --release --build program.mettle
```

Lower it to inline more, raise it to inline less.

## Seeing the effect

Pair it with [`--explain`](compilation.md) to watch inlining decisions change:

```bash
mettle --pgo --release --explain --build program.mettle
```

```text
main (call to `helper` @ line 5): inlined  [inlined]
```

Build the same file without `--pgo` and compare. A callee that was refused for
size and is now inlined is what `--pgo` bought.

## When it helps

It helps when the hot path runs through a callee just over the inliner's size
budget, which the static heuristic has no way to know is hot. It does nothing
when `main()` does not exercise the real workload, since the counts come from
that one interpreted run.

A program whose real hot path depends on input the interpreter never sees will
measure the wrong thing. Give `main()` a representative default path if you
want `--pgo` to be worth turning on.

## See also

- [Compile-time execution](testing.md)
- [Compilation](compilation.md)
- [Translation validation](translation-validation.md)
