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

The `asm` block embeds raw assembly. The contents use NASM syntax. Use with care; the compiler does not validate or optimize inline assembly.

```mettle
function get_rax() -> int64 {
  asm {
    mov rax, 42
  }
}
```
