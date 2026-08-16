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

- `const NAME [: type] = <expr>;` declares an immutable binding; reassignment is a compile error. Constants must be declared before use.
  - **Top-level (global) numeric `const`** values must have compile-time constant initializers. Integer values are folded at each use site, while float values get storage when code may take their address. A numeric `const` may omit its type annotation. Other top-level `const` values get storage in the object file initialized to their value.
  - **Global aggregate `const`** values must use an aggregate literal whose elements are compile-time constants, since global bytes are laid out at compile time and there is no module initializer to run a call in.
  - **Local (function-scope) `const`** may have **any** type. It is registered as an immutable local variable initialized to its value (not folded), so the initializer follows the same rules as any local variable initializer, including calls.

### Aggregate Literals

`[a, b, c]`, `[value; count]`, and `{ field: value, ... }` initialize arrays and structs. The literal takes the type of what it initializes, so it is only legal where that type is written down: a `var`/`const` initializer, or the right-hand side of an assignment.

- **Elements must be compile-time constants** (literals, other constants, `sizeof`, arithmetic over those, `&some_function`, `&some_global`, `0`, string literals, nested literals). A call is rejected at the element. There is no run-time aggregate literal.
- **Omitted struct fields and short array literals leave the rest zero.** Extra array elements, unknown field names, and repeated field names are all errors.
- A **closure** cannot appear in a literal: its environment is built at run time.

### Global Variables

Top-level `var` supports scalars, strings, aggregates, and pointers:

- **Aggregates** (arrays and structs) may carry an aggregate literal, which is laid out in `.data`, or be left uninitialized, which reserves zeroed `.bss`. Indexing, field access, `&g[i]`, and pointer traversal all behave as they do for locals.
- **Function pointers** may be left uninitialized, set to `0` (a null function pointer, as for any pointer), or initialized with `&some_function`.
- **Address initializers** (`var p: int32* = &g_other;`, `var f: fn(int32) -> int32 = &handler;`) resolve at link time rather than folding to a constant, so the backend emits a relocation. The same applies to a `&function` or string element inside an aggregate literal. A function referenced only this way is kept alive by dead-function elimination.
- **An array of function pointers cannot be spelled directly**: `fn(int32) -> int32[2]` parses `int32[2]` as the return type. Wrap the entries in a struct, which is the usual shape for a dispatch table anyway.

### Traits & Generics

Traits and constrained generics support inline bounds, multiple bounds, trailing `where` clauses on functions and structs, explicit impls, and trait method declarations with concrete impl method bodies. **Limitation:** generic trait-method calls on named values are monomorphized to concrete impl functions rather than resolved dynamically.

### Anonymous Functions & Closures

Anonymous functions are written `fn(params) -> ret { body }` in expression position. A non-capturing lambda is a first-class function pointer (storable, callable, passable as a higher-order argument, usable as a C callback). A lambda that references an enclosing variable is a *capturing closure*: captures are by value (snapshotted at creation), and the closure value is an 8-byte pointer to a heap environment. A closure type is spelled with a capital `Fn(...)->R` (distinct from the thin, C-compatible `fn(...)->R`); use it to return a closure, pass one to a higher-order function, or store one in a struct field. A capturing closure and a thin function pointer are not interchangeable (mixing them across a boundary is a compile error, not a miscompile).

A closure (or plain function pointer) stored in a struct field can be called through `obj.field(args)`, including through a pointer-to-struct receiver. Captures are by value, but the closure's copy lives in its heap environment and persists across calls, so closures can hold mutable state (counters, accumulators) without affecting the outer variable.

A non-capturing lambda or `&func` can be passed anywhere an `Fn(...)` closure is expected - as a call argument to a plain function, a variable declaration, or a return value - and the compiler transparently wraps it in a generated adapter so it dispatches through the closure calling convention. **Limitations:** (1) Adaptation covers a *literal* `&func` or lambda at the boundary; a thin value already sitting in a variable (`var g: fn(...)->R = &func; use(g);`) is not yet adapted - write `use(g)` as `use(&func)` directly, or re-spell `g`'s declared type as `Fn(...)`. (2) Assigning a thin value to an existing `Fn(...)`-typed struct field (`obj.field = &func;`) is not yet adapted; assign a real (possibly non-capturing) lambda literal instead. (3) The captured variables must have an explicit type (closures cannot capture a variable whose type was inferred). (4) The heap environment (of both closures and adapters) is not freed automatically (consistent with Mettle's manual-heap model).

### Pattern Matching

- `**match` on tagged enums** supports both a statement form (arm bodies are `{ ... }` blocks) and an expression form that yields a value. In expression form, each arm body must be a single value-yielding expression (for example, `match (o) { case Some(v): v + 1, default: 0 }`). All arm types must unify, and the match must be exhaustive (`default:` or all variants covered) because it must always produce a value.
- **Tagged-enum constructors** are function-like. Payload variants use `Some(x)`. Payloadless variants may be written either bare (`None`) or with empty call syntax (`None()`); both forms construct the same value.

### Switch

- `switch` case values must be compile-time constant integer expressions. Inclusive range cases (`case lo..hi:`) are supported; both bounds must also be compile-time constant integers.

---

## Compiler & Optimizations

- Unreachable-code analysis is block-local and conservative; some dead paths in complex control flow may not be diagnosed yet.

### Vectorization

`--explain` reports what became of every loop and why, so treat it as the
authority; what follows is the shape of the remaining gaps.

- **An `if` that only chooses a value vectorizes over int32 elements.** A clamp, a
  floor, a ReLU, a running extremum, an if/else nest, and the early-return chain
  an inlined helper leaves behind all become a lane minimum, maximum, or select.
  Arms that store, call, or write a second name do not: a masked lane cannot
  carry that effect.
- **Counting under a condition vectorizes.** `if (a[i] > t) { c = c + 1; }` and
  `if (a[i] < 0) { s = s + a[i]; }` are made unconditional by multiplying in the
  comparison. Two shapes are not: an else arm that also writes the accumulator,
  and a guard that is not a comparison, since `if (x & 6)` fires on 2, 4 and 6
  alike. Write that one as `(x & 6) != 0` and it converts.
- **A comparison is a value.** `c + (a[i] > t)` and `a[i] * (a[i] != 0)` become a
  lane mask narrowed to 0 or 1, for all six operators.
- **A scan may be seeded from its first element.** `var m = a[0]; var i = 1;` is
  reset to start at 0, since iteration 0 would compare the seed with itself.
- **Reductions over `^`, `&` and `|` have no kernel** and are reported as serial
  recurrences, though they are associative. Only `+`, minimum and maximum
  reduce.
- **Float elements have no select kernel.** A clamp over `float32` or `float64`
  stays scalar. Minimum and maximum on floats are not the same operation as the
  branch when a lane holds NaN, so the kernel needs an ordered compare that is
  not written yet.
- **Depth is bounded by registers.** An int32 map has six ymm registers for its
  value stack. Three levels of `if`/`else` over distinct expressions needs more
  than that and stays scalar; splitting the deepest arm into its own loop fits.
- **Reductions over a byte array need a 64-bit accumulator.** `acc: int32` summing
  `uint8` elements is refused because the kernel widens to int64.
- **Global arrays vectorize.** Their base is hoisted above the loop, so a program
  that keeps its buffers at file scope reaches the same kernels as one passing
  pointers.

---

## Memory, Pointers & Safety

### Struct-by-Value Function Arguments

Structs work normally as **locals**: field access, whole-struct assignment, and `&s` all use the full laid-out size (assignment copies every byte, not just the first machine word). The same holds when the destination is a struct field, a nested field, an array element, or reached through a pointer, including when the value comes from a function returning an aggregate, which arrives through a hidden pointer.

**Struct-by-value parameters and returns** follow the Microsoft x64 ABI's aggregate rule on Windows. A struct whose size is exactly 1, 2, 4, or 8 bytes is passed directly in one integer register. Other aggregate sizes, including structs larger than 8 bytes and odd-sized small structs such as 3-byte values, are passed and returned by **hidden pointer**:

- **Arguments:** the caller copies the source struct into a per-call stack temp and passes the temp's address in the normal argument register/slot. The callee dereferences the pointer to access fields. By-value semantics are preserved; mutations inside the callee affect only the temp copy, not the caller's original.
- **Returns:** the caller allocates a slot in its own frame and passes its address as a hidden first integer argument (Win64: `rcx`). The callee writes the result through that pointer and returns the pointer in `rax`. The caller materializes the returned struct from that frame slot, so the value outlives the call's stack teardown.

**Remaining limitations:**


| Scenario                                                              | Behavior                                                                                                                                                                                       |
| --------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Parameter `fn(s: Big)` with `sizeof(Big) > 8`                         | Supported in the native object backend.                                                                                                                                                        |
| Returning `-> Big` with `sizeof(Big) > 8`                             | Supported in the native object backend. Hidden out-pointer; result lives in the caller's frame.                                                                                                |
| Chained pattern `f(g())` where both are struct-by-value               | Supported. The returned struct survives passage into the next call.                                                                                                                            |
| Mettle calling C functions with struct-by-value args/returns          | Supported on Windows when the C object uses the Microsoft x64 ABI and the final link uses Mettle's internal linker.                                                                            |
| C calling exported Mettle functions with struct-by-value args/returns | Not yet covered by tests or documented as supported.                                                                                                                                           |
| Float-typed return values from Mettle-to-Mettle calls                 | Supported in the native object backend. Callees return through `xmm0` per the Win64 ABI.                                                                                                      |


**Practical guidance:**

- Struct-by-value **arguments and returns** are safe in the native object backend.
- For C interop, the backend follows the platform C ABI: Microsoft x64 on Windows (COFF) and System V AMD64 on Linux (ELF). Scalar and pointer arguments, return values, register-and-stack argument passing, and the hidden struct-return pointer all match the target convention. Struct-by-value passing and returning is covered for the Mettle-calls-C direction. See [C Interoperability - Passing Structs to C](c-interop.md).
- With `--linker internal`, raw COFF `.o` / `.obj` files can be supplied through `--link-arg`; the final executable link remains inside Mettle.

Arrays follow the same rule as in [Types - Array Types](types.md#array-types): they are not passed by value; use `&arr[0]` or a `T`* parameter.

### Null & Bounds Checks

- **Null dereference:** constant nulls such as `*0` are diagnosed at compile time. Runtime null checks are emitted for dynamic dereference and pointer-based indexing in normal builds, but are disabled in `--release`. Pointers originating from C or inline assembly can still be invalid in ways the compiler cannot prove.
- **Array indexing:** fixed-size array indexing is checked at compile time for constant indices and guarded at runtime for dynamic indices in normal builds; those runtime guards are disabled in `--release`. Pointer indexing remains unchecked for bounds because the compiler does not know the pointee extent.

### Borrow lifetimes

- The compiler tracks *borrows* (a pointer derived from another object via `&x[i]`) and reports, as warnings, the cases it can prove are dangling: a borrow into a stack local used after that local's `{ }` block exits; and an interior pointer into a heap buffer used after the buffer is `realloc`'d or `free`'d. Analysis is conservative and intra-function: borrows are only tracked along a function's straight-line spine (a borrow taken inside an `if`/`while`/`for` body is not tracked), and a borrow handed across a function-call boundary is not yet followed. There is no ownership/borrow *syntax*; the checker is pure inference, so it never rejects a program. It only points at provable mistakes.

### Heap

- There is no garbage collector and no heap manager. `new` and string concatenation emit direct `calloc(1, size)` calls; allocations are reclaimed by the OS at process exit unless user code manages them explicitly.
- String concatenation via `+` allocates through Mettle's owned heap.

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

- `std/net` works on Windows and Linux from one import. Windows binds Winsock2.
  Linux selects `std/net.linux` and uses owned socket system calls. No thread or
  C library is added.
