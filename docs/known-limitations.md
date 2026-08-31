# Known limitations

What does not work yet, verified against the compiler in this checkout. Each
entry is something a reader would otherwise hit and have to work out.

## Language

A tagged-enum variant carries at most one payload. `Circle(float64)` works;
`Pair(int32, int32)` does not parse. Wrap two values in a struct.

`switch` cases fall through into the next case. End each one with `break`
unless you mean the fall-through.

A generic call names its type arguments: `id<int64>(7)`. Nothing is inferred
from the argument.

An array is one-dimensional. `int64[3][4]` does not parse; index a flat
`int64[12]` yourself.

Two types may point at each other. `struct A { b: B*; }` above
`struct B { a: A*; }` compiles, as does a longer cycle and an enum whose
payload points at a struct that stores the enum by value. What has no size is a
cycle that stores values rather than pointers, and that is reported:

```text
error[E0003]: 'A' and 'B' each store a value of the other, so neither has a
size. Hold one of them by pointer: 'B*'
```

## Aggregate literals

Elements must be compile-time constants: literals, other constants, `sizeof`,
arithmetic over those, `&function`, `&global`, `0`, string literals, and nested
literals. A call in an element is rejected:

```text
error[E0003]: aggregate literal elements must be compile-time constants
```

There is no run-time aggregate literal.

A closure cannot appear in one, because its environment is built at run time.
Writing a lambda where a `Fn` field is expected inside a literal reports a
mismatch against `fn(...)`.

Omitted struct fields and short array literals leave the rest zero. Extra
elements, unknown field names, and repeated field names are errors.

## Compile-time expansion

`typeof(T).fields` is the only compile-time sequence. `comptime for` iterates a
struct's fields and nothing else:

```text
error[E0003]: 'comptime for' iterates a compile-time sequence; the only one is
'<type>.fields'
```

There is no way to declare a table of data and generate from it.

Compile-time strings compare and nothing else. `==` and `!=` fold, which is
enough to check that two declarations agree by name. There is no
concatenation, ordering, length, or substring. `ident(...)` composes
declaration names under rules the compiler checks.

`ident(...)` composes a declaration's name, not a type. A generated type can be
named where it is used, and not from inside the iteration that generated it.

A `comptime for` reflects on the types the program wrote. Module-scope
expansion runs after those are registered and before the generated ones are, so
`typeof(GeneratedStruct).fields` reports an unknown type.

The binding cannot appear where a type goes. `f.type` answers `f.type.size` and
`f.type.kind`, and cannot be written as a parameter, return, or field type.

## Closures

A plain function value already sitting in a variable is not adapted to a
closure type. Take the address at the point you need it, `&twice`, which is
accepted at a declaration, an argument, a return, and an assignment.

## Borrow analysis

Analysis is conservative and stays within one function. Borrows are tracked
along a function's straight-line spine, so a borrow taken inside an `if`,
`while`, or `for` body is not tracked, and a borrow handed across a call
boundary is not followed.

There is no ownership syntax, so it never rejects a program. It points only at
what it can prove. See [Borrow checker](borrow-checker.md).

## Null and bounds checks

Constant null dereferences are diagnosed while compiling. Run-time null checks
are emitted for dynamic dereferences in normal builds and dropped under
`--release`.

Fixed-size array indexing is checked at compile time for constant indices and
guarded at run time in normal builds, and those guards are dropped under
`--release`. Use [`--safe`](memory-safety.md) to keep them, including under
`--release`.

Pointer indexing is never bounds-checked, because the compiler does not know
the pointee's extent. Pointers arriving from C or from inline assembly can be
invalid in ways nothing can prove.

## Vectorization

Reductions over `^`, `&`, and `|` have no kernel and are reported as serial.

Float elements have no select kernel, so a clamp over `float32` or `float64`
stays scalar. The same clamp over `int32` vectorizes.

A reduction over a byte array needs a 64-bit accumulator. An `int32`
accumulator summing bytes is reported rather than vectorized.

Depth is bounded by registers. An int32 map has six ymm registers for its
expression, and a deeper one falls back.

`--explain` names the reason for every loop it leaves alone, and
`mettle explain <code>` expands any of them.

## Struct ABI

Struct-by-value arguments and returns work in both directions on both
platforms, under the Microsoft x64 rule on Windows and System V's eightbyte
classification on Linux. See
[C interoperability](c-interop.md).

## Deferred calls

A deferred direct call, `defer f(args)`, captures its argument values at the
defer point and replays them at scope exit.

A deferred method call, `defer obj.m(...)`, and a deferred call through a
function pointer re-evaluate their operands at scope exit. Snapshot into a
local first when you need the value from the defer point.

`errdefer` is function-only and convention-based: any non-zero explicit return
is treated as an error.

## Platform

Linux links no shared library, ever. The ELF writer refuses a `PT_INTERP`, so
a `.so` cannot be used and libraries such as raylib are out of reach. Static
archives work against the owned subset.

`std/ui`, the Win32 window and control helpers, is Windows-only. It has no
Linux counterpart.

External Tracy needs a C++ runtime, so `--tracy` fails under the owned runtime
rule. `--profile-runtime` is the built-in alternative.

`--musl` is rejected, because linking musl would break the owned-runtime rule.

## Narrow targets

The 16- and 32-bit targets compute in one register's worth of value. A value
wider than a word -- `int32` in 16-bit code, `int64` or a float in either -- is
a compile error naming the declaration. A struct or an array is a frame region
reached by address, so both work; strings do not travel through these targets
at all. Pointers are near.

Neither target has an object format that carries its relocations, so
`--emit-flat` is their only product; asking for an object or an executable is
an error rather than a file whose code is the wrong width for its header.

Real mode pushes no error code, so a 16-bit `@interrupt` handler takes no
parameters or the interrupt frame alone. `bits 16` and `bits 32` are allowed
inside a `@naked` function, which is the only place the compiler contributes no
bytes of its own.

A freestanding target has no runtime, so it emits no runtime checks and refuses
`--safe`. `--build` needs a linker and a runtime belonging to the machine being
built for, so it is refused for a foreign or freestanding target.
Cross-compiling means emitting the object here and linking it there.

## Compiler

An expression may nest 4096 levels deep, and blocks may nest 4096 deep. Past
either the compiler reports it:

```text
error[E0002]: Expression nests more than 4096 levels deep
```

Nesting is what counts, not length: `a + b + c + ...` folds in a loop and costs
one level however long it runs. The ceiling exists because each level is a
frame in the recursive descent and in every pass that walks the tree
afterwards, and without it deep enough input exhausted the stack and killed the
process with no diagnostic. It sits an order of magnitude below where that
happened.

Unreachable-code analysis is block-local and conservative. Some dead paths in
complex control flow are not diagnosed.

## See also

- [Memory safety](memory-safety.md)
- [Compilation](compilation.md)
- [Runtime model](runtime-model.md)
