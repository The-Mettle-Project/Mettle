# Documentation

This repository is **Mettle**: the language, its compiler frontend, the driver,
and the standard library. The backend it compiles against is
[**libmtlc**](https://github.com/The-Mettle-Project/libmtlc), a separate
project; [Mettle and libmtlc](mettle-and-libmtlc.md) draws that line and
explains how the dependency is fetched and pinned.

## The language

Start at the [language reference index](LANGUAGE.md). The individual chapters:

- [Lexical structure](lexical-structure.md), [Types](types.md),
  [Declarations](declarations.md), [Expressions](expressions.md),
  [Control flow](control-flow.md)
- [Modules](modules.md), [Imports](imports.md),
  [Standard library](standard-library.md)
- [Heap allocation](heap-allocation.md), [Borrow checker](borrow-checker.md),
  [C interoperability](c-interop.md)
- [Compile-time execution](testing.md) (`mettle test` / `trace`),
  [Quick reference](quick-reference.md), [Known limitations](known-limitations.md)

## The compiler

- [Compilation](compilation.md): the `mettle` driver and its options.
- [Diagnostics](diagnostics.md): how errors and warnings are reported.
- [Linker and build pipelines](linker-build-pipelines.md): which linker runs
  for each `--build` combination.
- [Runtime model](runtime-model.md): what emitted programs assume of the OS.
- [Mettle and libmtlc](mettle-and-libmtlc.md): what this repository owns, what
  the backend owns, and how to build and sync across the two.

## Optimization and GPU offload

These are driver-facing views of machinery that lives in libmtlc. The flags are
Mettle's; the passes behind them are the backend's.

- [ML-driven IR optimization](ml-opt.md) and [the ML-opt
  oracle](ml-opt-oracle.md): `--ml-opt`.
- [Translation validation](translation-validation.md): `--verify`.
- [Profile-guided optimization](pgo.md): `--pgo`.
- [The `--explain-json` schema](explain-json.md): the machine-readable
  optimization report editors and analysis tools read.
- [GPU offload](gpu.md): the PTX and SPIR-V targets from Mettle source.
- [GPU architecture and acceptance contract](gpu-architecture.md): the GB10 /
  AArch64 target matrix, the frontend/backend boundary, an honest gap table,
  and the gates required before making parity or performance claims.

## The backend

libmtlc has its own documentation set, in its own repository: the [API
reference](https://github.com/The-Mettle-Project/libmtlc/blob/main/docs/libmtlc/api.md),
[the IR model](https://github.com/The-Mettle-Project/libmtlc/blob/main/docs/libmtlc/ir.md),
[the type system](https://github.com/The-Mettle-Project/libmtlc/blob/main/docs/libmtlc/types.md),
[the pipeline and per-target
limits](https://github.com/The-Mettle-Project/libmtlc/blob/main/docs/libmtlc/pipeline.md),
and [internals](https://github.com/The-Mettle-Project/libmtlc/blob/main/docs/libmtlc/internals.md).
[Writing a frontend for
libmtlc](https://github.com/The-Mettle-Project/libmtlc/blob/main/docs/embedding.md)
is the tutorial for driving the backend from a language that is not Mettle, and
[the end-to-end
architecture](https://github.com/The-Mettle-Project/libmtlc/blob/main/docs/ARCHITECTURE.md)
is the single-document deep dive across both halves.

Those links track libmtlc's `main`, while
[`libmtlc.version`](../libmtlc.version) pins a specific commit — so read them at
that commit when the difference matters. A fetched dependency carries the same
docs at `libmtlc/docs/`, always at the pinned revision.

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the build and test workflow.
Backend changes belong upstream in libmtlc, not here.
