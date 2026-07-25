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
```

**Out-of-bounds indexing:** The compiler rejects constant out-of-bounds indexes for fixed-size arrays (for example `arr[10]` on `int32[10]`). For dynamic indices on fixed-size arrays, normal builds emit runtime bounds checks. In `--release`, those generated bounds checks are disabled. Pointer indexing is never bounds-checked because pointee extent is unknown.

**Use before initialization:** Local scalar and pointer variables must be assigned before first read. A use like `var x: int32; return x;` is a compile error.

**Passing arrays to functions:** Arrays are not passed by value (they can be large). Pass a pointer to the first element: `&arr[0]` or `&buf[0]`. The function parameter should have type `T*` (e.g. `int32*`, `uint8*`). Taking the address of an array with `&arr` yields a pointer to the whole array; for function calls expecting `T*`, use `&arr[0]`.

## Built-in Alias Types

`cstring` is an alias for `uint8*`. It is used for C interop: null-terminated strings, `void*`, and opaque pointers. `cstring` and `uint8*` are interchangeable. Use `cstring` when calling C functions that expect `char*` or `void*`.

`string` is a built-in struct with two fields: `.chars` (pointer to the character data) and `.length` (uint64, byte count). String literals have type `string`. The `string` type is distinct from `cstring`; use `s.chars` or the `cstr` helper from `std/io` to obtain a `cstring` for C calls.

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

function identity<T>(x: T) -> T {
  return x;
}

var p: Pair<int32, int32>;           // struct instantiation
var n: int32 = identity<int32>(42);  // function call with type args
```

The compiler performs **monomorphization** before type checking: each unique instantiation becomes a concrete type or function. There is no runtime generics; all type parameters are resolved at compile time.

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

function bump<T>(x: T) -> T where T: Incrementable {
  return x.next_value();
}
```

## Type Conversions

Widening conversions (e.g. `int32` to `int64`, `float32` to `float64`) are implicit. Narrowing conversions (e.g. `int32` to `int16`, `int64` to `int8`) are allowed implicitly for integer-to-integer and float-to-float. There is no implicit conversion between integers and floats, or between pointers and integers, except that `0` is valid as a null pointer initializer and in pointer comparisons.

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
