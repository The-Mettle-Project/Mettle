# Documentation

This repository is **libmtlc**, a reusable native compiler backend, plus
**Mettle**, the reference frontend that drives it. The docs are organized the
same way.

## The backend: libmtlc

libmtlc owns the IR, the optimizers, code generation for four targets (x86-64
with AVX2, ARM64, NVIDIA PTX, SPIR-V), and linking. Any frontend can build IR
and drive it through the public C API in [`include/mtlc/`](../include/mtlc/).

**The [libmtlc reference](libmtlc/README.md)** is the backend's own
documentation set:

- [Getting started](embedding.md): the tutorial (build IR, optimize, emit,
  link) with a complete non-Mettle example.
- [API reference](libmtlc/api.md): every public function, with ownership,
  lifetime, error, and thread-safety contracts.
- [The IR model](libmtlc/ir.md): values, instructions, control-flow rules,
  module tables, and per-consumer IR shape requirements.
- [The type system](libmtlc/types.md): `MtlcType` kinds, layout, canonical
  constructors, and the immortality contract.
- [The pipeline](libmtlc/pipeline.md): the optimizer pass families, the
  ML-opt validation gate, each code generator's product and limits, linking.
- [Internals](libmtlc/internals.md): source layout, self-containment
  invariants, and how to extend the backend.

Driver-facing views of the same machinery:

- [Compilation](compilation.md): the `mettle` driver and its options.
- [Linker and build pipelines](linker-build-pipelines.md): which linker runs
  for each `--build` combination.
- [GPU offload](gpu.md): the PTX and SPIR-V targets from Mettle source.
- [GPU architecture and acceptance contract](gpu-architecture.md): the GB10 /
  AArch64 target matrix, frontend/backend boundary, honest gap table, and the
  gates required before making parity or performance claims.
- [ML-driven IR optimization](ml-opt.md), [Translation
  validation](translation-validation.md), [Profile-guided
  optimization](pgo.md): `--ml-opt`, `--verify`, `--pgo`. The successor model
  under development is described in [gnn_oracle](ml-opt-oracle.md).
- [Runtime model](runtime-model.md): what emitted programs assume of the OS.
- [Diagnostics](diagnostics.md): the frontend-neutral diagnostics reporter.
- [The `--explain-json` schema](explain-json.md): the machine-readable
  optimization report editors and analysis tools read.

A second, non-Mettle frontend that exercises the whole public API lives in
[`examples/calc`](../examples/calc).

## The reference frontend: the Mettle language

Mettle is a typed, assembly-inspired systems language. These document the
*language* the reference frontend implements (see the
[language reference index](LANGUAGE.md)):

- [Lexical structure](lexical-structure.md), [Types](types.md),
  [Declarations](declarations.md), [Expressions](expressions.md),
  [Control flow](control-flow.md)
- [Modules](modules.md), [Imports](imports.md),
  [Standard library](standard-library.md)
- [Heap allocation](heap-allocation.md), [Borrow checker](borrow-checker.md),
  [C interoperability](c-interop.md)
- [Memory safety](memory-safety.md): `--safe`, what it checks, what it proves
  away, and what it does not cover.
- [Compile-time execution](testing.md) (`mettle test` / `trace`),
  [Quick reference](quick-reference.md), [Known limitations](known-limitations.md)

## Design and rationale

- [Ideology](ideology.md): the rules the language holds itself to, and what each
  one costs.
- [Architecture](ARCHITECTURE.md): the full pipeline, subsystem by subsystem.
- [Mettle and libmtlc](mettle-and-libmtlc.md): where the frontend ends and the
  backend begins, and how to build each half.

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the build/test workflow and the
rules that keep the backend frontend-agnostic.
