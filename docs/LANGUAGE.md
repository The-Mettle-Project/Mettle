# Mettle Language Reference

Mettle is the **reference frontend** for [libmtlc](../README.md): a typed,
assembly-inspired systems language. Its parser and semantic analysis lower
Mettle source into the backend IR, which libmtlc optimizes and compiles to
native machine code (x86-64 / ARM64) or GPU targets (PTX / SPIR-V). There is no
external assembler and no assembly text anywhere in the pipeline. This reference
documents the Mettle *language*; to drive the backend from a different frontend,
see [Writing a frontend for libmtlc](embedding.md).

## Table of Contents

1. [Lexical Structure](lexical-structure.md)
2. [Types](types.md)
3. [Declarations](declarations.md)
4. [Expressions](expressions.md)
5. [Control Flow](control-flow.md)
6. [Modules](modules.md)
7. [Imports](imports.md)
8. [Standard Library](standard-library.md)
9. [Heap Allocation](heap-allocation.md)
10. [GPU Offload](gpu.md)
11. [Runtime Model](runtime-model.md)
12. [C Interoperability](c-interop.md)
13. [Compilation](compilation.md)
14. [Quick Reference](quick-reference.md)
15. [Known Limitations](known-limitations.md)

For the backend itself (the IR, optimizers, code generators, linker, and the
public C API any frontend can drive) see the [documentation index](README.md).
