# Types

Mettle is statically typed. Every variable and function parameter has an explicit type. This document describes the type system.

## Type Sizes and Alignment

The following sizes and alignments apply on x86-64. Use these when laying out structs for C interop or manual memory management.

| Type | Size | Alignment |
|------|------|-----------|
| `int8`, `uint8`, `bool` | 1 | 1 |
| `int16`, `uint16` | 2 | 2 |
| `int32`, `uint32`, `float32` | 4 | 4 |
| `int64`, `uint64`, `float64`, pointers, plain enums | 8 | 8 |
| `string` | 16 | 8 |
| `Type`, `Field` | none | none |

Struct and array sizes are derived from their fields and element types. Pointers and plain integer-valued enums are 8 bytes. Tagged enums are sized from their tag and largest payload.

## Primitive Types

Signed integers: `int8`, `int16`, `int32`, `int64` (1, 2, 4, 8 bytes). Unsigned integers: `uint8`, `uint16`, `uint32`, `uint64` (1, 2, 4, 8 bytes). Floating-point: `float32`, `float64` (4, 8 bytes, IEEE 754). Sizes and representations follow the target platform (x86-64).

**Integer overflow:** The compiler emits native x86-64 arithmetic instructions. Signed integer overflow wraps (two's complement); there is no trap or runtime check. Unsigned overflow wraps modulo 2^n. Assembly programmers can rely on wrap-around behavior.

**Integer literal default type:** When the context does not disambiguate, integer literals default to `int32`. Floating-point literals default to `float64`. Examples: `42` has type `int32`; `3.14` has type `float64`. In expressions like `var x: int64 = 42`, the literal is implicitly converted to the expected type.

**Boolean:** `bool` is a built-in 1-byte type with the two built-in constants `true` and `false`. It is distinct from `uint8`: a `switch` over a `bool` must cover both `true` and `false` unless it has a `default`. Note that comparison operators do not produce `bool`; they produce an `int32` that is 0 or 1, and conditions in `if`, `while`, and `for` accept any numeric type rather than requiring `bool`.

```mettle
var ready: bool = true;
switch (ready) {
  case true:  return 1;
  case false: return 0;
}
```

## Pointer Types

Pointers use the `*` suffix. A pointer holds the address of a value of the base type. Multi-level pointers are supported.

```mettle
var p: int32*;        // pointer to int32
var pp: int32**;      // pointer to pointer to int32
var sp: MyStruct*;    // pointer to struct
```

The null pointer is written `0`. Pointers and `0` are comparable for equality.

**Pointer arithmetic:** `ptr + n` and `ptr - n` are supported when one operand is a pointer and the other is an integer; the offset scales by the pointed-to type size (C semantics). `ptr1 - ptr2` is supported when both pointers have the same type and yields a byte offset as `int64`. Indexing `ptr[i]` is equivalent to `*(ptr + i)`. For byte-level stepping, use `uint8*` or `cstring`.

**Null dereference:** In normal builds, the compiler emits runtime null checks for dynamic pointer dereference/indexing and traps with a fatal message on null. In `--release`, these generated checks are disabled; null dereference is undefined behavior and typically crashes.

## Function Pointer Types

Function pointers are first-class values that can be stored, passed as arguments, and called indirectly. They enable callbacks and function references.

### Function Pointer Type Syntax

Function pointer types use the `fn` keyword with parameter types and return type:

```mettle
var fp: fn(int32, int32) -> int32;  // pointer to function taking (int32, int32) returning int32
var void_fn: fn() -> void;           // pointer to function taking nothing returning nothing
```

### Taking Function Addresses

Use the address-of operator `&` to create a function pointer:

```mettle
fn add(a: int32, b: int32) -> int32 {
  return a + b;
}

var fp: fn(int32, int32) -> int32;
fp = &add;  // & takes the address of a function
```

### Calling Through Function Pointers

Call a function pointer like a regular function:

```mettle
var result: int32 = fp(3, 4);  // calls the function pointed to by fp
```

### Function Pointer Use Cases

Function pointers are useful for callbacks, strategy patterns, and C interop:

```mettle
// Callback pattern
fn add(a: int32, b: int32) -> int32 {
  return a + b;
}

fn apply(op: fn(int32, int32) -> int32, a: int32, b: int32) -> int32 {
  return op(a, b);
}

fn main() -> int32 {
  return apply(&add, 5, 3);  // passes add as callback
}
```

**Type equality:** Two function pointer types are equal if they have the same parameter types and return type. `fn(int32) -> int32` is compatible with `fn(int32) -> int32` but not with `fn(int32, int32) -> int32`.

### Anonymous Functions (Lambdas)

`fn` may also be written in expression position to produce an anonymous function value, without naming it at the top level:

```mettle
var add: fn(int32, int32) -> int32 = fn(x: int32, y: int32) -> int32 {
  return x + y;
};
var seven: int32 = add(3, 4);

// Inline as a higher-order argument:
var product: int32 = apply(fn(x: int32, y: int32) -> int32 { return x * y; }, 6, 7);
```

A non-capturing lambda has the same `fn(params) -> ret` type as a named function and is a plain function pointer, so it is usable anywhere a function pointer is (including C callbacks). The body is a normal block. The return type may be omitted, in which case it defaults to `void`; write it explicitly whenever the lambda returns a value, since an omitted type silently makes the lambda `void` rather than inferring from the `return` statement.

### Closures

A lambda that references a variable from an enclosing scope *captures* it, becoming a closure that carries its captured state:

```mettle
import "std/io"

fn main() -> int32 {
  var base: int32 = 10;
  var add: Fn(int32) -> int32 = fn(x: int32) -> int32 { return x + base; };  // captures base
  print_int(add(5));    // 15
  return 0;
}
```

Captures are **by value**: each captured variable's value is snapshotted when the closure is created, so changing the original afterwards does not change what the closure sees. A closure value is an 8-byte pointer to a heap-allocated environment holding the code pointer and the captured values. The closure's own copy is **mutable and persists across calls**, so a closure can carry state:

```mettle
fn counter(start: int32) -> Fn() -> int32 {
  return fn() -> int32 { start = start + 1; return start; };
}
var next: Fn() -> int32 = counter(0);
println_int(next());   // 1
println_int(next());   // 2 - state persists in the closure's environment
```

Because a closure carries state, its type is distinct from a plain function pointer. A closure type is written with a capital **`Fn`**: `Fn(int32) -> int32`. A plain `fn(...)->R` stays a thin, C-compatible function pointer; `Fn(...)->R` is a stateful closure.

Use `Fn(...)->R` to carry closures across function boundaries - returned from a factory, passed to a higher-order function, or stored in a struct field:

```mettle
import "std/io"

fn make_adder(n: int32) -> Fn(int32) -> int32 {
  return fn(x: int32) -> int32 { return x + n; };   // closure capturing n
}

fn apply_twice(f: Fn(int32) -> int32, v: int32) -> int32 {
  return f(f(v));
}

fn main() -> int32 {
  var add: Fn(int32) -> int32 = make_adder(10);
  println_int(add(5));            // 15
  println_int(apply_twice(add, 0)); // 20
  println_int(make_adder(3)(5));  // 8 - the returned closure is called directly
  return 0;
}
```

A closure (or plain function pointer) stored in a struct field is called through the field, including via a pointer-to-struct receiver:

```mettle
import "std/io"

struct Handler { on_event: Fn(int32) -> int32; }

fn main() -> int32 {
  var weight: int32 = 2;
  var h: Handler;
  h.on_event = fn(ev: int32) -> int32 { return ev * weight; };
  println_int(h.on_event(21));   // 42
  var hp: Handler* = &h;
  println_int(hp.on_event(5));   // 10
  return 0;
}
```

A capturing closure and a thin `fn(...)->R` are not directly interchangeable (a thin pointer cannot carry an environment, and a closure call site reads a code pointer a thin value does not have) - but a plain function or non-capturing lambda can still be passed anywhere an `Fn(...)` is expected. The compiler transparently wraps it in a generated adapter so it dispatches through the closure calling convention:

```mettle
import "std/io"

fn apply_twice(f: Fn(int32) -> int32, v: int32) -> int32 { return f(f(v)); }

fn plus_one(x: int32) -> int32 { return x + 1; }

fn main() -> int32 {
  println_int(apply_twice(&plus_one, 5));   // 7 - a plain function, adapted
  var f: Fn(int32) -> int32 = &plus_one;    // var-decl adaptation
  println_int(f(10));                       // 11
  return 0;
}
```

Adaptation applies at the point a plain function or lambda literal is directly written into an `Fn(...)` boundary (a call argument, a `var` declaration, or a `return`). A thin value already sitting in a variable, or assigned into an `Fn(...)`-typed struct field, is not yet adapted; write `&func` (or the lambda literal) directly at the boundary. Like every binding in Mettle, a local holding a closure states its type explicitly - `var f: Fn(int32) -> int32 = ...`. See [known limitations](known-limitations.md).

## Array Types

Fixed-size arrays use `[N]` where N is a constant. Arrays are value types; the elements are laid out contiguously. Indexing is zero-based.

```mettle
var arr: int32[10];
var buf: uint8[256];
var table: int32[4] = [10, 20, 30, 40];
const ZEROED: uint8[256] = [0; 256];
```

An array may be initialized with an [aggregate literal](expressions.md#aggregate-literals); left uninitialized, it starts zeroed.

**Out-of-bounds indexing:** The compiler rejects constant out-of-bounds indexes for fixed-size arrays (for example `arr[10]` on `int32[10]`). For dynamic indices on fixed-size arrays, normal builds emit runtime bounds checks. In `--release`, those generated bounds checks are disabled. Pointer indexing is never bounds-checked because pointee extent is unknown.

**Use before initialization:** Local scalar and pointer variables must be assigned before first read. A use like `var x: int32; return x;` is a compile error.

**Passing arrays to functions:** Arrays are not passed by value (they can be large). Pass a pointer to the first element: `&arr[0]` or `&buf[0]`. The function parameter should have type `T*` (e.g. `int32*`, `uint8*`). Taking the address of an array with `&arr` yields a pointer to the whole array; for function calls expecting `T*`, use `&arr[0]`.

## Built-in Alias Types

`cstring` is an alias for `uint8*`: a pointer to bytes a C function will read
up to a NUL. `cstring` and `uint8*` are interchangeable. Use `cstring` when
calling C functions that expect `char*`.

`rawptr` is an address with no element type: what an allocator hands out and
what a deallocator takes. It converts to and from every pointer type in both
directions, so

```mettle
var a: int32* = malloc(n * 4);
free(a);
```

needs no cast either way. Because it names no element, it cannot be indexed,
dereferenced, or offset, give the address a type first. Use `rawptr` for an
opaque pointer or a `void*` at a C boundary.

`string` is a built-in struct with two fields: `.chars` (pointer to the
character data) and `.length` (uint64, byte count). It is a borrowed view: a
pointer, a length, and **no terminator**. String literals have type `string`,
and `print`/`println` take one, so `println("done")` writes `length` bytes and
never scans for a NUL.

NUL-termination is a property of the C boundary, not of the type. A string
*literal* already sits in rodata with a terminator the compiler emitted, so it
flows straight into a `cstring` parameter, `fopen("data.txt", "rb")`
allocates nothing. For a string built at run time, `cstr(s, alloc)` from
`std/io` produces the terminated copy, and takes the allocator it comes from so
the copy is visible in the signature:

```mettle
var path: cstring = cstr(name, &malloc);
defer free(path);
var fp: cstring = fopen(path, "rb");
```

**Creating strings at runtime:** There is no built-in constructor. To build a `string` from a `cstring` and length, assign the fields: `s.chars = ptr; s.length = len`. The `string` does not own the buffer; the caller is responsible for the lifetime of the data pointed to by `.chars`.

**String assignment:** Assigning one `string` to another copies the 16-byte struct (the `.chars` pointer and `.length`). Both values then refer to the same underlying buffer. No deep copy of the character data occurs. To share a buffer, assignment is sufficient; to copy contents, allocate a new buffer and copy bytes (e.g. via `malloc` and `memcpy` from `std/mem`).

## Struct Types

Structs group named fields. Fields are laid out in declaration order with appropriate alignment for the target. Structs can define methods (see [Declarations](declarations.md)).

```mettle
struct Point {
  x: int32;
  y: int32;
}

struct SockAddrIn {
  sin_family: int16;
  sin_port: uint16;
  sin_addr: uint32;
  sin_zero: uint8[8];
}
```

A struct value is written with an [aggregate literal](expressions.md#aggregate-literals): `var p: Point = { x: 1, y: 2 };`. Fields may appear in any order, and any field left out stays zero.

For C interop, match the C struct layout exactly (field order, types, padding).

## Enum Types

Enums define a named type and a set of variants, each with an integer value. Variants without an explicit value continue from the previous variant (0 if first). Variant names are in scope after the enum is defined; use them directly (e.g. `North`, not `Direction.North`).

```mettle
enum Direction {
  North,        // 0
  East = 2,     // 2
  South,        // 3 (previous + 1)
  West = -5     // -5
}

var a: Direction = North;
var b: Direction = East;
```

**Underlying type:** Enums use `int64` as the underlying representation. This affects struct layout and C interop: a struct field of enum type is 8 bytes, aligned to 8.

**Casting integers to enums:** Implicit narrowing allows assigning an integer to an enum variable when the types are compatible (e.g. `var d: Direction = 2`). For values read from C APIs or switch results, assign directly when the integer type narrows to the enum or use an explicit cast (e.g. `(Direction)val`) to force the conversion.

Enums can be compared with integers and used in `switch` cases. They can be exported for use in other modules (see [Declarations](declarations.md)).

## Tagged Enum Types

Tagged enums associate a payload type with some variants. They are useful for values such as `Option`, `Result`, or message unions where each variant may carry different data.

```mettle
enum Option {
  Some(int32),
  None
}

var a: Option = Some(42);
var b: Option = None();
```

**Constructors:** Each variant is constructed with function-call syntax. Payload variants take one argument, such as `Some(42)`. Payloadless variants are currently written with empty call syntax, such as `None()`.

**Payload binding:** Use `match` to branch on a tagged enum and bind the payload from a specific variant. See [Control Flow](control-flow.md#match).

**Representation:** A tagged enum stores a discriminant tag plus storage for the largest payload among its variants. Its size is not fixed like a plain enum, so avoid assuming it is 8 bytes in C interop or manual layout code.

Tagged enums can also be generic:

```mettle
enum Result<T> {
  Ok(T),
  Err
}
```

## Generic Type Parameters

Functions and structs can be generic. Type parameters are declared in angle brackets: `fn f<T>(...)` or `struct S<T> { ... }`. Instantiation uses the same syntax: `f<int32>(args)` or `var x: Pair<int32, float64>`.

```mettle
struct Pair<A, B> {
  first: A;
  second: B;
}

fn identity<T>(x: T) -> T {
  return x;
}

fn main() -> int32 {
  var p: Pair<int32, int32>;           // struct instantiation
  var n: int32 = identity<int32>(42);  // function call with type args
  p.first = n;
  return p.first;
}
```

The compiler performs **monomorphization** before type checking: each unique instantiation becomes a concrete type or function. There is no runtime generics; all type parameters are resolved at compile time. A generic struct's methods are part of its instantiation: each concrete type carries its own copies, with the type parameters substituted through their signatures and bodies.

Struct layout is computed in the frontend type table (declaration order,
natural alignment, C padding rules), including byte offset, and for a bitfield
the bit offset and width. `offsetof(Point.x)` folds that byte offset
during const eval. Enum types record variant names and values on the same
table. Pointers answer their pointee; arrays answer element type and length;
slices (when present) answer element type and a runtime length.

## Compile-time `Type` and `Field`

Reflection values are compile-time values, not a second type system. A type name in value position is a `Type` value (a TypeRef: an index into the compiler's type table). A field of such a value is a `Field` value (a FieldRef: `{type_index, field_index}`).

```mettle
struct Point {
  x: int32;
  y: int32;
}

const T: Type = int32;           // or typeof(int32), or typeof(n)
const F: Field = Point.x;        // or typeof(Point).x, or fieldof(Point, "x")
```

`Type` and `Field` have **no runtime representation**. They cannot be stored in a `var`, appear as a function parameter or return type, sit in a struct field, or otherwise escape into runtime code. Bind them with `const` and use them only at compile time. `sizeof(Type)` is rejected for the same reason.

`typeof` is a compile-time builtin. A type name is taken as that type; any other argument is typed as an expression and `typeof` yields that expression's type.

### `fieldof`

`Point.x` and `typeof(Point).x` both need the field name spelled in the source. `fieldof(T, name)` takes the name as a compile-time **string**, so a metaprogram can compose the name it looks up:

```mettle
const F: Field = fieldof(Point, "x");
static_assert(fieldof(Point, "y").offset == 8);
```

The first argument is a type, either named directly or held in a `const T: Type`. The second is any compile-time string, which includes a `.name` read back out of the field table, so a walk can look each field up again without the name appearing in source:

```mettle
comptime for f in typeof(Packet).fields {
  total = total + (int32)fieldof(Packet, f.name).type.size;
}
```

The result is an ordinary `Field` and answers every `Field` query, so it composes with `offsetof`:

```mettle
static_assert(offsetof(fieldof(Packet, "stamp")) == 8);
```

A name that no field carries is a compile error that lists the names the type does have:

```
error[E0003]: 'Point' has no field named 'z'; it has x, y
```

Like `typeof` and `offsetof`, `fieldof` is spelled with an identifier and call syntax and adds no punctuation the lexer did not already read.

## Reflection queries

A `Type` and a `Field` are asked about themselves with ordinary member access. Every query folds during const eval, so all of them are visible to `static_assert` and none reaches the backend.

| On a `Type` | Answers |
|---|---|
| `.kind` | a `Kind` (see below) |
| `.name` | module-qualified name, e.g. `"std/net.Point"` |
| `.size`, `.align` | `int64` bytes |
| `.fields` | a compile-time sequence of `Field` |
| `.pointee` | the pointed-to `Type` (pointers only) |
| `.element` | the element `Type` (arrays and slices) |
| `.len` | element count (sized arrays only) |

| On a `Field` | Answers |
|---|---|
| `.name` | the field's own name, unqualified |
| `.type` | the field's `Type` |
| `.offset` | byte offset within its owner |
| `.index` | declaration position |

A `Field` answers nothing about its type directly: ask `.type` and compose, as in `f.type.size` or `f.type.kind`. A declared field always wins over a query of the same name, so a struct with a field called `size` is unaffected by the reflection surface.

```mettle
static_assert(typeof(Point).kind == Kind.Struct);
static_assert(typeof(Point).fields.len == 2);
static_assert(typeof(Point).fields[1].type.size == 4);
static_assert(typeof(Point*).pointee.size == 8);
static_assert(typeof(int32[4]).len == 4);
```

`typeof` accepts a type or an expression, including types spelled with `*` or `[N]`: `typeof(Point*)` and `typeof(n)` both work.

### Names are module-qualified

`.name` on a user-declared type reports the module it was **defined** in, whatever import path reached it: `std/arena.Arena`, not `arena.Arena` or a filesystem path. A type declared in the root program is qualified by its file stem. Builtins and structural types answer their own already-unambiguous spelling (`int32`, `Point*`).

This is deliberate and worth understanding before the name reaches a wire format. A bare name cannot distinguish two modules that each declare `Point`, and compile-time strings compare but do not concatenate or slice, so a bare name could never be turned back into a qualified one. The cost is the mirror image: a qualified name is coupled to file layout, so **moving a module renames its types**. A generator that writes these names into a wire format should pin them rather than assume `.name` is stable across a refactor.

### Comparing names

Compile-time strings answer `==` and `!=`, and the result folds like any other compile-time integer, so it is available to `static_assert`:

```mettle
static_assert(typeof(Point).fields[0].name == "x");
static_assert(typeof(Point).fields[0].name != typeof(Point).fields[1].name);
```

Reading a name and comparing two names are different capabilities, and the second is what lets a contract that spans two declarations be stated where it belongs. Checking that two structs stay field-for-field identical is the shape that motivates it:

```mettle
comptime for f in typeof(WireIn).fields {
  static_assert(f.name   == fieldof(WireOut, f.name).name);
  static_assert(f.offset == fieldof(WireOut, f.name).offset);
}
```

If the two drift apart, the build fails and the diagnostic names the iteration that caught it. A field name the other struct does not carry is caught by `fieldof` first, which lists the names it does have.

Equality is the whole surface. There is no concatenation, ordering, length, or substring, because each of those is a way to compute a name the program never wrote, and `ident(...)` already composes names under rules the compiler can check.

### `layoutof`

`layoutof(T)` folds to a compile-time integer digest of everything a stored value of `T` depends on: its kind, size and alignment, and for each field the name, byte offset, bit offset and width, and that field's own layout. A pointer field contributes that it is a pointer and its width, never its pointee's layout, so a self-referential struct terminates.

The digest exists to be pinned:

```mettle
static_assert(layoutof(Player) == 7833040735491835555);
```

Once pinned, any change to the layout fails the build. That includes changes the size does not show:

```mettle
static_assert(layoutof(Player) != layoutof(Renamed));    // field renamed, same size
static_assert(layoutof(Player) != layoutof(Reordered));  // fields swapped
```

A rename moves the digest even when every offset and width is unchanged, because the field's name is part of the contract: reading `health` where `hp` was written is the same defect as reading the wrong offset.

This is what makes a stored or transmitted layout checkable rather than assumed. A value written by one build and read by another (a save file, a wire packet, a process that outlived a code change) is only sound while the layouts agree, and the compiler is the only thing that knows both. Pinning the digest turns "these agree" from a comment into a build failure.

The digest is derived only from declared facts, so it is the same for the same declaration on every host and in every build mode. It is not stable across compiler versions that change the layout algorithm, which is the point: if the layout moves, the pin should break.

### `Kind`

`.kind` answers with `Kind`, an enum the compiler registers itself, so reflection needs no import and no `--prelude`. Its variants are reachable **only** qualified, as `Kind.Struct` and never as a bare `Struct`, precisely so that reflection does not claim common names like `Struct`, `Array`, or `Bool` out of every program's namespace.

`Kind` covers `Void`, `Bool`, `Int8`...`Int64`, `Uint8`...`Uint64`, `Float32`, `Float64`, `String`, `Pointer`, `Array`, `Slice`, `Struct`, `Enum`, `TaggedEnum`, and `FunctionPointer`. Integer and float widths stay distinct, because telling `int32` from `int64` is exactly what a wire-format generator needs. It deliberately has no `Type` or `Field` variant: those exist only at compile time, so no value you can reflect on ever has one.

### Inspecting and budgeting expansion

Generated code that cannot be read cannot be reviewed, so expansion is inspectable and its cost is on the record.

`mettle expand <file>` prints the program as Mettle source after expansion, with each generated block carrying the same note a diagnostic raised inside it would carry:

```
// expanded from comptime-for iteration 2 (field `seq`)
{
    total = (((total + 4) + (4 * 100)) + 1);
}
```

Where a node has no faithful source spelling the printer says so inline, as a marked comment, and reports on stderr that the output is not a complete program. It does not guess: a printer that silently misrepresents generated code is worse than none, because it is believed.

`--report-expansion` prints what each site cost, and `--expansion-budget=N` makes that a contract: over budget fails the build and names the site responsible:

```
comptime expansion: 2 sites, 84 nodes generated
  packet.mettle:18:3  Packet  3 iterations, 84 nodes
  packet.mettle:25:3  Empty   0 iterations, 0 nodes
```

A program that expands nothing reports `no sites; nothing generated`. That absence is stated rather than implied, for the same reason `Kind` is registered only on first mention: a cost you cannot confirm you avoided is one you are still paying in doubt.

### Sequences

`.fields` is a real compile-time value, not a special form only legal in `comptime for`. It answers `.len` and `[i]`, and nothing else: a program can observe a sequence but never hold one, and a `Sequence` has no runtime representation, exactly like `Type` and `Field`. The subscript must be a compile-time constant in range, since there is nothing to index at run time.

## `comptime for`

`comptime for` iterates a compile-time sequence and expands to one copy of its body per element, before any of the body is type checked:

```mettle
struct Packet {
  kind: uint8;
  seq: uint32;
  payload: int64;
}

fn main() -> int32 {
  var total: int64 = 0;
  comptime for f in typeof(Packet).fields {
    static_assert(f.type.size > 0);
    total = total + f.offset + f.type.size;
  }
  return (int32)total;
}
```

`comptime` is contextual, not a reserved word: only `comptime` immediately followed by `for` starts one of these, so `comptime` remains usable as an ordinary identifier. `typeof(T).fields` is the only compile-time sequence today. A type with no fields expands to nothing, which is not an error.

Expansion happens **before** the body is type checked, and each copy is checked separately. That is the point of the construct rather than an implementation detail: `f.offset`, `f.type`, and `f.type.size` differ per iteration, so a body can be valid for one field and rejected for the next.

Because the copies are checked separately, a diagnostic inside one has to say which copy it came from. Every error and warning raised while checking an expansion carries a note naming the iteration and the field, with the caret on the `comptime for` the programmer wrote:

```
error[E0003]: static_assert failed
  --> mixed.mettle:9:18
   |
 8 |   comptime for f in typeof(Mixed).fields {
 9 |     static_assert(f.type.size == 8);
   |                  ^
10 |     total = total + f.offset;
note: expanded from comptime-for iteration 2 (field `small`)
  --> mixed.mettle:8:3
  |
8 |   comptime for f in typeof(Mixed).fields {
  |   ^^^^^^^^
```

Nested expansions report the whole chain, outermost first.

The binding is a `Field`, and it is scoped to its own expansion, so it cannot be seen by surrounding code and cannot shadow anything the programmer wrote. It is a compile-time value like any other `Type` or `Field`: it cannot be cast, stored, or otherwise leaked into runtime code. Generated code is checked exactly as hand-written code is, contracts included.

### At module scope: generating declarations

A `comptime for` written between declarations rather than inside a function generates declarations. The directive is the same one; what differs is the list its expansions are spliced into.

```mettle
struct Packet {
  kind: uint8;
  seq: uint32;
  payload: int64;
}

comptime for f in typeof(Packet).fields {
  const ident("OFFSET_", f.name): int64 = f.offset;

  fn ident("end_of_", f.name)(base: int64) -> int64 {
    return base + f.offset + f.type.size;
  }
}

fn main() -> int32 {
  return (int32)(end_of_seq(0) + OFFSET_payload);
}
```

That generates `OFFSET_kind`, `OFFSET_seq`, `OFFSET_payload` and the three `end_of_*` functions. Structs, enums and globals work the same way. This is what a table shredded across a switch is for: declare the shape once, generate the accessors, and adding a field adds its accessor rather than leaving one behind.

Every iteration needs a name of its own, and `ident(...)` composes one from compile-time strings:

```mettle
ident("end_of_", f.name)     // end_of_kind, end_of_seq, end_of_payload
```

It is written the way `typeof` and `offsetof` are, out of an identifier and call syntax, and it adds no punctuation the lexer did not already read. Like `comptime`, it is contextual: `ident(...)` composes a name only inside a `comptime for` body, so a function the programmer happens to call `ident` is unaffected. Each part must evaluate to a compile-time string, either a string literal or a `.name` query, and the result must be a name the program could have been written with.

`ident(...)` also stands where a value does, so an iteration can refer to what it generated:

```mettle
comptime for f in typeof(Packet).fields {
  const ident("WIDTH_", f.name): int64 = f.type.size;
  fn ident("width_of_", f.name)() -> int64 {
    return ident("WIDTH_", f.name);
  }
}
```

Two boundaries are worth stating, because both are refusals rather than gaps waiting to be filled in quietly:

- **`ident(...)` composes a declaration's name, not a type.** A type annotation is a name the checker resolves, and that happens before the binding has a value. Write a generated type's name out where you use it. Naming a generated type from inside the iteration that generated it is not expressible today, and the compiler says so rather than failing further along.
- **Two iterations that compose the same name is an error.** A body whose declaration name does not come from the binding produces one name for every field, and all but one of those declarations would not be in the program. A generator that quietly drops half its output is the under-delivery contracts exist to prevent, so it fails the build and suggests composing the name.

A directive that generates a directive is expanded too; module scope keeps expanding until nothing is left. A nested expansion carries the bindings of every directive that produced it, so an inner body can read the outer field and a diagnostic prints every step of the chain.

Nothing after expansion can tell a generated declaration from a written one. `@noalloc` on a generated function is proven or fails the build, naming the generated function. `@test` on one runs under `mettle test`. `mettle expand` prints it as source with the iteration that produced it, and `--report-expansion` counts what it cost.

**Constraints:** Trait bounds are supported. Declare a trait, satisfy it with `impl Trait for Type`, and constrain generic parameters with inline bounds such as `T: Name`, multiple inline bounds such as `T: Addable + SignedNumber`, or a trailing `where` clause.

```mettle
trait Incrementable {
  fn next_value(self: Self) -> Self;
}

impl Incrementable for int32 {
  fn next_value(self: Self) -> Self {
    return self + 1;
  }
}

fn bump<T>(x: T) -> T where T: Incrementable {
  return x.next_value();
}
```

## Type Conversions

**Widen silently. Narrow loudly.** An integer conversion happens on its own
only when every value of the source type is representable in the destination:
`int32` to `int64`, `uint32` to `int64`, `bool` to any integer. A conversion
that can change the value (`int64` to `int32`, `uint64` to `int64`, `int32`
to `uint32`) is a narrowing and needs a cast at the site, where a reader can
see it. Narrowing without one is [M0119](diagnostics.md).

```mettle
fn f(n: int64) {
  var wide:   int64 = 1;
  var narrow: int32 = n;          // M0119
  var meant:  int32 = (int32)n;   // says the wrap is intended
}
```

A compile-time constant is checked rather than refused, because its value is
known: `var b: uint8 = 200;` is fine, and `var h: int32 = 2654435761;` is
[M0118](diagnostics.md), naming the value and the range it missed. Two
destinations sit outside the rule because they are not range conversions:
`bool` is a truth coercion (a comparison yields `int32`, and
`var b: bool = x > y;` stores it), and an enum names a set. An enum flowing the
other way is checked exactly, it converts implicitly when every declared
member fits.

Floating-point conversions (`float32` to `float64` and back) remain implicit in
both directions. There is no implicit conversion between integers and floats, or
between pointers and integers, except that `0` is valid as a null pointer
initializer and in pointer comparisons.

**Explicit casts:** Mettle provides an explicit cast syntax `(Type)expr`. This can be used to convert between numeric types, pointer types, and between integers and pointers. It is especially useful for pointer reinterpretation (e.g. treating `int32*` as `uint8*` for byte access) or converting floats to integers:

```mettle
var p: int32*;
var bytes: uint8* = (uint8*)p;

var f: float64 = 3.14;
var i: int32 = (int32)f;
```

Valid cast conversions include:

- Any numeric type (integer or float) to any other numeric type.
- Any pointer type to any other pointer type.
- Any integer type to any pointer type, and vice versa.
- Function pointers to other function pointers, or to/from regular pointers and integers.

Casting across different sizes might result in zero-extension, sign-extension, or truncation, depending on the target type and the sign of the source type. Floating-point to integer conversions truncate towards zero.

See [Expressions](expressions.md) for more details on cast conversions and evaluation behavior.
