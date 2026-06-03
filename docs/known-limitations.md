# Known Limitations

This document lists current limitations of the Mettle language, compiler, and runtime. For supported behavior, see the [language reference](LANGUAGE.md).

## Table of Contents

1. [Language & Types](#language--types)
2. [Compiler & Optimizations](#compiler--optimizations)
3. [Memory, Pointers & Safety](#memory-pointers--safety)
4. [Control Flow & Error Handling](#control-flow--error-handling)
5. [Modules & Platform](#modules--platform)

---

## Language & Types

### Constants

- `const NAME [: type] = <expr>;` declares a compile-time integer constant. At top level the value is folded at every use site; a local `const` is an immutable variable (reassignment is a compile error). Initializers must be compile-time constant integer expressions (literals, `sizeof`, other constants, and arithmetic/bitwise/comparison operators over them). Float, string, and aggregate constants are not yet supported, and constants must be declared before use.

### Traits & Generics

Traits and constrained generics support inline bounds, multiple bounds, trailing `where` clauses on functions and structs, explicit impls, and trait method declarations with concrete impl method bodies. **Limitation:** generic trait-method calls on named values are monomorphized to concrete impl functions rather than resolved dynamically.

### Pattern Matching

- `**match` on tagged enums** supports both a statement form (arm bodies are `{ ... }` blocks) and an expression form that yields a value. In expression form, each arm body must be a single value-yielding expression (for example, `match (o) { case Some(v): v + 1, default: 0 }`). All arm types must unify, and the match must be exhaustive (`default:` or all variants covered) because it must always produce a value.
- **Tagged-enum constructors** are function-like. Payload variants use `Some(x)`; payloadless variants use empty call syntax such as `None()`.

### Switch

- `switch` case values must be compile-time constant integer expressions. Inclusive range cases (`case lo..hi:`) are supported; both bounds must also be compile-time constant integers.

---

## Compiler & Optimizations

- Optimization passes are limited. The `-O` flag enables some optimizations but does not cover the full space of possible improvements.
- Unreachable-code analysis is block-local and conservative; some dead paths in complex control flow may not be diagnosed yet.

---

## Memory, Pointers & Safety

### Struct-by-Value Function Arguments

Structs work normally as **locals**: field access, whole-struct assignment, and `&s` all use the full laid-out size (assignment copies every byte, not just the first machine word).

**Struct-by-value parameters and returns** follow the Microsoft x64 ABI's aggregate rule on Windows. A struct whose size is exactly 1, 2, 4, or 8 bytes is passed directly in one integer register. Other aggregate sizes, including structs larger than 8 bytes and odd-sized small structs such as 3-byte values, are passed and returned by **hidden pointer**:

- **Arguments:** the caller copies the source struct into a per-call stack temp and passes the temp's address in the normal argument register/slot. The callee dereferences the pointer to access fields. By-value semantics are preserved; mutations inside the callee affect only the temp copy, not the caller's original.
- **Returns:** the caller allocates a slot in its own frame and passes its address as a hidden first integer argument (Win64: `rcx`). The callee writes the result through that pointer and returns the pointer in `rax`. The caller materializes the returned struct from that frame slot, so the value outlives the call's stack teardown.

**Remaining limitations:**


| Scenario                                                              | Behavior                                                                                                                                                                                       |
| --------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Parameter `fn(s: Big)` with `sizeof(Big) > 8`                         | Supported in text-asm and `--emit-obj` modes.                                                                                                                                                  |
| Returning `-> Big` with `sizeof(Big) > 8`                             | Supported in text-asm and `--emit-obj` modes. Hidden out-pointer; result lives in the caller's frame.                                                                                          |
| Chained pattern `f(g())` where both are struct-by-value               | Supported. The returned struct survives passage into the next call.                                                                                                                            |
| Mettle calling C functions with struct-by-value args/returns          | Supported on Windows when the C object uses the Microsoft x64 ABI and the final link uses Mettle's internal linker.                                                                            |
| C calling exported Mettle functions with struct-by-value args/returns | Not yet covered by tests or documented as supported.                                                                                                                                           |
| Float-typed return values from Mettle-to-Mettle calls                 | Supported in text-asm and `--emit-obj` modes. Callees return through `xmm0` per the Win64 ABI; the text IR path still mirrors the bits into `rax` after calls for its internal value pipeline. |


**Practical guidance:**

- Struct-by-value **arguments and returns** are safe in the text-asm backend and in `--emit-obj` mode.
- For C interop, the backend follows the platform C ABI: Microsoft x64 on Windows (COFF) and System V AMD64 on Linux (ELF). Scalar and pointer arguments, return values, register-and-stack argument passing, and the hidden struct-return pointer all match the target convention. Struct-by-value passing and returning is covered for the Mettle-calls-C direction. See [C Interoperability - Passing Structs to C](c-interop.md).
- With `--linker internal`, raw COFF `.o` / `.obj` files can be supplied through `--link-arg`; the final executable link remains inside Mettle.

Arrays follow the same rule as in [Types - Array Types](types.md#array-types): they are not passed by value; use `&arr[0]` or a `T`* parameter.

### Null & Bounds Checks

- **Null dereference:** constant nulls such as `*0` are diagnosed at compile time. Runtime null checks are emitted for dynamic dereference and pointer-based indexing in normal builds, but are disabled in `--release`. Pointers originating from C or inline assembly can still be invalid in ways the compiler cannot prove.
- **Array indexing:** fixed-size array indexing is checked at compile time for constant indices and guarded at runtime for dynamic indices in normal builds; those runtime guards are disabled in `--release`. Pointer indexing remains unchecked for bounds because the compiler does not know the pointee extent.

### Heap

- There is no garbage collector and no heap manager. `new` and string concatenation emit direct `calloc(1, size)` calls; allocations are reclaimed by the OS at process exit unless user code manages them explicitly.
- String concatenation via `+` allocates through the C runtime and does not require a Mettle heap runtime object.

### C Interoperability

- Pointers that cross into C remain an ownership hazard. C code that takes ownership of manually allocated buffers must follow the C library's allocation/free contract; `new` allocations are released only when the process exits.

---

## Control Flow & Error Handling

### Deferred Calls

- A deferred direct call `defer fn(args...)` captures its **argument values at the defer point** (by value); the snapshots are replayed at scope exit. Deferred **method calls** (`defer obj.m(...)`) and **indirect/function-pointer calls** still re-evaluate their operands at scope exit (by reference); snapshot into a local first if you need the defer-point value.

### Error Defer

- `errdefer` is function-only and convention-based. It is valid only inside functions, and any non-zero explicit return value is treated as an error.

---

## Modules & Platform

### Imports

- Imports may carry a platform guard: `import "..." if windows;` or `import "..." if linux;`. A guarded import is included only when its platform matches the build target (the compiler targets its host), and an off-target guarded module is never looked up. Unguarded imports are unconditional. The guard predicate is limited to `windows` and `linux`.

### Platform Support

- `std/net` and the web server example are Windows-only (Winsock2). Use POSIX socket externs for networking on Linux.

