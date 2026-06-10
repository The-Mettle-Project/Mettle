# Declarations

Declarations introduce variables, functions, types, and other program elements. All declarations appear at top level or within struct bodies (for methods). Declarations are processed in order; a symbol must be declared before use, except for forward declarations.

## Variables

Variables are declared with `var`, a name, a type, and an optional initializer. Variables require an explicit type. Initializers are optional for locals; globals can have initializers. The value `0` is a valid initializer for pointers (null).

```mettle
var x: int32;
var y: int32 = 42;
var msg: string = "hello";
var buf: uint8[1024];
```

## Constants

Constants are declared with `const`, a name, an optional type, and a required initializer that is a compile-time constant integer expression. A top-level `const` is folded directly into the machine code at every use site and occupies no storage. A local `const` is an immutable binding; assigning to it is a compile error.

```mettle
const MAX: int32 = 100;
const STEP = 4;            // type inferred as int32
const BOUND = MAX - STEP;  // may reference earlier constants
```

Initializers may use integer literals, `sizeof`, other constants, and arithmetic, bitwise, and comparison operators over them. Float, string, and aggregate constants are not yet supported, and a constant must be declared before it is used.

## Functions

Functions are declared with `function` (or the shorthand `fn`), a name, parameters in parentheses, an optional return type, and a body. The return type can use `->` or `:`. Omitting the return type indicates a void function (no return value).

```mettle
function add(a: int32, b: int32) -> int32 {
  return a + b;
}

fn greet() {  // void return
  // ...
}
```

A function named `main` with signature `() -> int32` serves as the program entry point when present. The compiler emits `_start` which calls `main` and passes its return value to the runtime.

## Function decorators

A function declaration may be prefixed with one or more `@` decorators that
steer the optimizer. Decorators stack and may appear in any order; they attach
to the `function` (or `export function`) that follows.

```mettle
@inline function fast(x: int32) -> int32 { return x * 3 + 1; }
@pure @noinline function hash(p: int32*, n: int64) -> int64 { /* ... */ }
```

| Decorator | Meaning |
|-----------|---------|
| `@inline` | Force the function past the inliner's size, parameter-count, and call-count heuristics, the built-in benchmark denylist, and the caller-size budget (an over-budget caller normally only accepts tiny call-free callees). Structural blockers — most importantly inline assembly — still prevent inlining. |
| `@inline!` | **Contract**: every call to this function must inline, or compilation fails at each surviving call site with the inliner's reason (recursion, a structural guard). Implies `@inline`. |
| `@noinline` | Never inline this function. This is the user-facing way to keep a hot helper as its own call. |
| `@pure` | Assert the function is free of side effects **and** safe to evaluate speculatively (it neither writes observable state nor traps in a way that depends on being reached). The optimizer may then evaluate a call once before a loop and reuse the result — see below. |
| `@noalloc` | **Contract**: the function — and everything it can reach through direct calls — performs zero heap allocations, or compilation fails pointing at the allocation. This is a proof, not a lint: `new`, string `+` concatenation, allocator calls (`malloc`/`calloc`/...), calls to externs not known to be allocation-free, and calls through function pointers anywhere in the reachable graph all violate it. Known-clean libm/memory externs (`sqrtf`, `memcpy`, ...) are allowed. |
| `@simd` / `@simd!` | Apply a vectorization contract to every counted loop in the body — see [Vectorization contracts](control-flow.md#vectorization-contracts). |

`@inline` and `@noinline` are mutually exclusive. Applying `@inline`,
`@noinline`, `@pure`, or `@noalloc` to anything other than a function — a loop,
a struct, an `extern` function — is a compile error. Decorators have effect
only under `-O` / `--release` (a note reminds you when contracts go
unverified in a debug build).

### `@pure` and loop-invariant call hoisting

When a `@pure` function is called inside a loop with arguments that do not
change across iterations, the optimizer hoists the call into the loop preheader
and reuses the single result:

```mettle
@pure @noinline function weight(table: int32*, k: int32) -> int32 { /* ... */ }

function score(table: int32*, k: int32, items: int32*, n: int64) -> int64 {
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
`@pure` is a *trusted* contract — the compiler does not verify purity, exactly as
`@simd!` trusts the vectorizability claim. Marking an impure or
non-speculation-safe function `@pure` is a program error.

## Generic Functions

Functions can declare type parameters in angle brackets before the parameter list. Call sites must provide type arguments: `f<T>(args)` or `f<int32>(args)`.

```mettle
function swap<T>(a: T*, b: T*) -> void {
  var tmp: T = *a;
  *a = *b;
  *b = tmp;
}

function main() -> int32 {
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
function add(a: int32, b: int32) -> int32;

function add(a: int32, b: int32) -> int32 {
  return a + b;
}
```

## Extern Functions

Extern functions are implemented in C or another language. They are declared with `extern function` and an optional link name after `=`. If the link name is omitted, the Mettle name is used. Parameters and return types must match the C ABI. Use `cstring` for C `char*` or `void*`.

```mettle
extern function puts(msg: cstring) -> int32 = "puts";
extern function malloc(size: int64) -> cstring = "malloc";
extern function my_func(x: int32) -> int32;  // link name = my_func
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

function main() -> int32 {
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

var v: Vector3;
v.magnitude();
```

## Inline Assembly

The `asm` block syntax is reserved, but native object code generation does not currently support inline assembly.

```mettle
function get_rax() -> int64 {
  asm {
    mov rax, 42
  }
}
```
