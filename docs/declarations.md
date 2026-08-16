# Declarations

Declarations introduce variables, functions, types, and other program elements. All declarations appear at top level or within struct bodies (for methods). Declarations are processed in order; a symbol must be declared before use, except for forward declarations.

## Variables

Variables are declared with `var`, a name, a type, and an optional initializer. **Mettle never infers binding types**: every `var` (and every local `const`) must state its type explicitly - a declaration with an initializer but no type annotation is a compile error. The value `0` is a valid initializer for pointers (null).

```mettle
var x: int32;
var y: int32 = 42;
var msg: string = "hello";
var buf: uint8[1024];
var table: int32[4] = [10, 20, 30, 40];
var origin: Pt = { x: 1.0, y: 2.0 };
var z = 1 + 2;              // ERROR: 'z' requires an explicit type
```

Arrays and structs may be initialized with an [aggregate literal](expressions.md#aggregate-literals), or left uninitialized, in which case they start zeroed.

(Only two kinds of binding get their type structurally rather than by annotation: a range-`for` loop counter takes the type of its bound, and a top-level `const` takes its initializer's type. Neither is inferred from an arbitrary expression.)

## Constants

Constants are declared with `const`, a name, an optional type, and a required initializer. Assigning to a `const` is a compile error, and a constant must be declared before it is used.

```mettle
const MAX: int32 = 100;
const STEP = 4;            // top-level const gets its initializer's type
const BOUND = MAX - STEP;  // may reference earlier constants

fn main() -> int32 {
    const RATE: float64 = 1.5;   // local consts require an explicit type
    const LABEL: string = "ready";
    return MAX;
}
```

A numeric `const`, with integer or float type, must have a compile-time constant initializer. It may use literals, earlier numeric constants, `sizeof`, and operators that the type checker can evaluate. A top-level integer `const` is folded directly into machine code at each use and occupies no storage. Float constants still get normal storage when code may take their address. A top-level numeric `const` may omit its type annotation. A local `const` must state its type, like any local binding.

A `const` of type `Type` or `Field` is a compile-time reflection binding: it holds a TypeRef or FieldRef and occupies no storage. `Type` and `Field` cannot be used as `var` types, function parameters, return types, or struct fields.

A top-level `const` of any other type gets normal storage in the object file, initialized to its value. Float, string, and **aggregate** constants all work at global scope:

```mettle
const RATE: float64 = 1.5;
const LABEL: string = "ready";
const TABLE: int32[4] = [10, 20, 30, 40];
const ORIGIN: Pt = { x: 1.0, y: 2.0 };
```

Because a global's storage is laid out at compile time, an aggregate one must be initialized with an [aggregate literal](expressions.md#aggregate-literals) whose elements are all compile-time constants. A call has nothing to lay out and is rejected with a source location. There is no module initializer that could run one.

A **local** (function-scope) `const` may have any type. Numeric local constants need a compile-time initializer. Other local constants follow the same rules as local variables, so they may use a call.

## Functions

Functions are declared with `fn`, a name, parameters in parentheses, an optional return type, and a body. The return type can use `->` or `:`. Omitting the return type indicates a void function (no return value).

A local variable cannot use the name of a function parameter. A local variable may still use the name of a module global.

```mettle
fn add(a: int32, b: int32) -> int32 {
  return a + b;
}

fn greet() {  // void return
  // ...
}
```

A function can return more than one value by listing the types in parentheses.
The caller assigns the values to a matching list of names.

```mettle
fn solve() -> (float64, float64, float64) {
  return (0.8, 1.2, 2.4);
}

fn main() -> int32 {
  var x: float64 = 0.0;
  var y: float64 = 0.0;
  var z: float64 = 0.0;
  (x, y, z) = solve();
  return 0;
}
```

Each returned value must match its target type. The list form supports scalar
values and names declared in the current scope.

A function named `main` with signature `() -> int32` serves as the program entry point when present. The compiler emits `_start` which calls `main` and passes its return value to the runtime.

## Function decorators

A function declaration may be prefixed with one or more `@` decorators that
steer the optimizer. Decorators stack and may appear in any order; they attach
to the `fn` (or `export fn`) that follows.

```mettle
@inline fn fast(x: int32) -> int32 { return x * 3 + 1; }
@pure @noinline fn hash(p: int32*, n: int64) -> int64 { /* ... */ }
```

| Decorator | Meaning |
|-----------|---------|
| `@inline` | Force the function past the inliner's size, parameter-count, and call-count heuristics, the built-in benchmark denylist, and the caller-size budget (an over-budget caller normally only accepts tiny call-free callees). Structural blockers still prevent inlining, inline assembly above all. |
| `@inline!` | **Contract**: every call to this function must inline, or compilation fails at each surviving call site with the inliner's reason (recursion, a structural guard). Implies `@inline`. |
| `@noinline` | Never inline this function. This is the user-facing way to keep a hot helper as its own call. |
| `@pure` | Assert the function is free of side effects **and** safe to evaluate speculatively (it neither writes observable state nor traps in a way that depends on being reached). The optimizer may then evaluate a call once before a loop and reuse the result. See below. |
| `@noalloc` | **Contract**: the function, and everything it can reach through direct calls, performs zero heap allocations, or compilation fails pointing at the allocation. This is a proof, not a lint: `new`, string `+` concatenation, allocator calls (`malloc`/`calloc`/...), calls to externs not known to be allocation-free, and calls through function pointers anywhere in the reachable graph all violate it. Known-clean libm/memory externs (`sqrtf`, `memcpy`, ...) are allowed. |
| `@simd` / `@simd!` | Apply a vectorization contract to every counted loop in the body. See [Vectorization contracts](control-flow.md#vectorization-contracts). |
| `@test` | Mark a compile-time unit test. The function takes no parameters, returns `int64`, and treats `0` as pass. It is type-checked in every build but compiled out of normal binaries, and `mettle test` runs it in the compiler's IR interpreter. See [Testing](testing.md). |

`@inline` and `@noinline` are mutually exclusive. Applying `@inline`,
`@noinline`, `@pure`, or `@noalloc` to a loop, a struct, or an `extern`
function is a compile error. Only `@simd` and `@simd!`
may be attached to a statement, and only to a `for` or `while` loop. When a
declaration is both decorated and exported, the decorators come first
(`@inline export fn f()`, never `export @inline fn f()`). Decorators have
effect only under `-O` / `--release` (a note reminds you when contracts go
unverified in a debug build); `@test` is the exception, since it changes what
is compiled in every build.

### `@pure` and loop-invariant call hoisting

When a `@pure` function is called inside a loop with arguments that do not
change across iterations, the optimizer hoists the call into the loop preheader
and reuses the single result:

```mettle
@pure @noinline fn weight(table: int32*, k: int32) -> int32 { /* ... */ }

fn score(table: int32*, k: int32, items: int32*, n: int64) -> int64 {
  var total: int64 = 0;
  for i in 0..n {
    total = total + (int64)(items[i] * weight(table, k));  // weight(table,k) hoisted
  }
  return total;
}
```

Hoisting is conservative: it fires only when every argument is loop-invariant
**and** the loop body performs no memory store (a pure callee may read memory
through a pointer argument, so a store in the loop could change what it reads).
`@pure` is a *trusted* contract. The compiler does not verify purity, exactly as
`@simd!` trusts the vectorizability claim. Marking an impure or
non-speculation-safe function `@pure` is a program error.

## Generic Functions

Functions can declare type parameters in angle brackets before the parameter list. A generic function is declared with `fn`, like any other; there is no separate `function` keyword. Call sites must provide type arguments: `f<T>(args)` or `f<int32>(args)`.

```mettle
fn swap<T>(a: T*, b: T*) -> void {
  var tmp: T = *a;
  *a = *b;
  *b = tmp;
}

fn main() -> int32 {
  var x: int32 = 10;
  var y: int32 = 20;
  swap<int32>(&x, &y);
  return x + y;
}
```

The compiler monomorphizes each unique instantiation before type checking. Type parameters can appear in parameter types, return type, and local variable types. See [Types](types.md#generic-type-parameters) for instantiation syntax.

## Forward Declarations

Functions can be declared before definition. The forward declaration ends with a semicolon. The definition must match the forward declaration (same name, parameter types, return type).

```mettle
fn add(a: int32, b: int32) -> int32;

fn add(a: int32, b: int32) -> int32 {
  return a + b;
}
```

## Extern Functions

Extern functions are implemented in C or another language. They are declared with `extern fn` and an optional link name after `=`. If the link name is omitted, the Mettle name is used. Parameters and return types must match the C ABI. Use `cstring` for C `char*` and `rawptr` for `void*`.

```mettle
extern fn puts(msg: cstring) -> int32 = "puts";
extern fn malloc(size: int64) -> rawptr = "malloc";
extern fn my_func(x: int32) -> int32;  // link name = my_func
```

## Extern Variables

Extern variables refer to C globals. They must have an explicit type and cannot have an initializer. The link name is optional.

```mettle
extern var errno_value: int32 = "errno";
```

## Generic Structs

Structs can declare type parameters in angle brackets. Use the struct with type arguments when declaring variables: `Pair<int32, int32>`, `List<float64>`.

```mettle
struct Pair<A, B> {
  first: A;
  second: B;
}

struct List<T> {
  data: T*;
  length: int32;
  capacity: int32;
}

fn main() -> int32 {
  var p: Pair<int32, int32>;
  p.first = 10;
  p.second = 20;
  return p.first + p.second;
}
```

The compiler monomorphizes each unique struct instantiation. Generic structs can have multiple type parameters. See [Types](types.md#generic-type-parameters).

## Structs and Enums

Functions, variables, structs, and enums can be prefixed with `export` to make them visible to modules that import this file.

```mettle
export enum Status {
  Ok = 0,
  Error = 1
}
```

## Methods

Structs can define methods. The receiver is implicit (`this`). Methods are called with `obj.method(args)`. When the receiver is a struct value, the compiler passes it by value as the first argument; when it is a pointer, the pointer is passed.

```mettle
struct Vector3 {
  x: int32;
  y: int32;
  z: int32;

  method magnitude() -> float64 {
    return 0.0;  // placeholder
  }
}

fn main() -> int32 {
  var v: Vector3;
  var m: float64 = v.magnitude();
  return 0;
}
```

## Inline Assembly

The `asm` block syntax is reserved, but native object code generation does not currently support inline assembly.

```mettle
fn get_rax() -> int64 {
  asm {
    mov rax, 42
  }
}
```
