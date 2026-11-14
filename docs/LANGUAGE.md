# Mettle language reference

Mettle is a typed, assembly-inspired systems language. Its frontend lowers
Mettle source into the [libmtlc](../README.md) IR, which the backend optimizes
and compiles to native machine code, x86-64 or ARM64, or to a GPU target, PTX
or SPIR-V. There is no external assembler and no assembly text anywhere in the
pipeline.

This index covers the language. To drive the backend from a different
frontend, see [Writing a frontend](embedding.md).

## The language

1. [Lexical structure](lexical-structure.md)
2. [Types](types.md), including declared types and deadlines
3. [Declarations](declarations.md)
4. [Expressions](expressions.md)
5. [Control flow](control-flow.md)
6. [Modules and imports](modules.md)
7. [Standard library](standard-library.md)

## Memory

8. [Ownership](ownership.md): drops, moves, `@explicit`, and how to see each
9. [Heap allocation](heap-allocation.md)
10. [Memory safety](memory-safety.md)
11. [Borrow checker](borrow-checker.md), including what a task may be handed

## Building

12. [Getting started](getting-started.md)
13. [Compilation](compilation.md)
14. [Diagnostics](diagnostics.md)
15. [Compile-time execution](testing.md)
16. [C interoperability](c-interop.md)
17. [Runtime model](runtime-model.md), including schedules as data
18. [Rewrite rules](rewrite-rules.md)
19. [Proofs](proofs.md): `theorem`, tactics, and the SMT solver
20. [Rules](rules.md)
21. [Effects](effects.md), including which globals two threads share

## Beyond the CPU

22. [GPU offload](gpu.md)

## Reference

23. [Quick reference](quick-reference.md)
24. [Known limitations](known-limitations.md)

Every mechanism above reports what it spent and what it assumed: `--explain`
carries sections for the types proven, the effects held, the rules run, and
the beliefs the build rested on, and each has its own `--report-*` flag and
`--*-budget=N` contract. [Compilation](compilation.md) lists them, along with
`mettle why`, which explains a fact that held in the same words a refusal
would have used.

For the backend itself, the IR, the optimizers, the code generators, the
linker, and the public C API, see the [documentation index](README.md).
