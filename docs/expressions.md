# Expressions

Operators, precedence, casts, calls, and string interpolation.

## Precedence

Tightest first. Every binary operator groups left to right.

| Level | Operators |
|-------|-----------|
| 13 | `.` member access |
| 11 | `*` `/` `%` |
| 10 | `+` `-` |
| 9 | `<<` `>>` |
| 8 | `<` `<=` `>` `>=` |
| 7 | `==` `!=` |
| 6 | `&` |
| 5 | `^` |
| 4 | `|` |
| 3 | `&&` |
| 2 | `||` |

Two of these catch people out. Addition binds tighter than shift, so
`1 << 2 + 1` is `1 << 3`, which is 8. The bitwise operators bind looser than
comparison, so `a & b == c` is `a & (b == c)`. Parenthesize when you mean the
other thing.

Unary `-`, `!`, `~`, `&`, and a cast bind tighter than any binary operator.

## Arithmetic

`+`, `-`, `*`, `/`, `%`, and unary `-`. Integer `/` truncates toward zero and
`%` takes the sign of the left operand.

Overflow wraps. The compiler emits the machine's own instructions and adds no
check.

Dividing by a constant zero fails the build, as [M0116](diagnostics.md):

```text
error[M0116]: Division by a constant zero; this traps the moment it executes
```

## Bitwise and shifts

`&`, `|`, `^`, `~`, `<<`, `>>`. A right shift of a signed value is
arithmetic and of an unsigned value is logical.

Shifting by a constant at or past the operand's width draws
[M0115](diagnostics.md), a warning, because the hardware masks the count:

```text
warning[M0115]: Shift by 32 on a 32-bit value (`int32`); the hardware masks
the shift count, so this does not produce the zero the code reads as
```

## Comparison and logic

`<`, `<=`, `>`, `>=`, `==`, `!=` produce a `bool`. `&&` and `||` produce a
`bool` and stop as soon as the answer is known, so the right side is not
evaluated when the left settles it.

`!` negates, and produces a `bool` like the rest of them:

```mettle
println("{!(a > 3)}");
```

```text
false
```

A `bool` converts to the integer types with no cast, so `var n: int32 = !flag;`
still gives 1 or 0.

## Assignment

`=` assigns. The compound forms `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`,
`^=`, `<<=`, `>>=` read, apply, and write back.

`++` and `--` are statements that add or subtract one:

```mettle
var i: int32 = 0;
i++;
i--;
```

## Casts

`(T)expr` converts. It moves between integer widths, between integers and
floats, between `char` and integers, and between pointer types.

```mettle
var c: char = 'h';
var code: int32 = c;
var back: char = (char)(code + 1);
```

Widening needs no cast. Narrowing one integer into a smaller one does, and the
compiler says which conversion it wanted:

```text
error[M0119]: Narrowing conversion from 'int64' to 'int8' needs a cast
```

## Member access and indexing

`a.b` reads a field of a struct value. `p->b` reads a field through a pointer.
`a[i]` indexes an array, a pointer, or a `string`.

```mettle
var p: Point = { x: 3, y: 4 };
var q: Point* = &p;
println("{p.x} {q->y}");
```

Indexing a `string` yields a `char`. Indexing a pointer walks by the pointee's
size, so `b[i].x` on a `Body*` reads the i-th element's field.

`s.length` is the byte count of a `string` and `s.chars` is the `uint8*`
behind it.

## Address-of

`&x` takes the address of a variable, a field, or an element. `&f` takes the
address of a named function, which is how a plain function reaches a `Fn`
parameter or field.

The compiler reports an address that would outlive what it points at, as
[M0103](diagnostics.md) for a returned stack local and
[M0104](diagnostics.md) for one stored in a global.

## Calls

```mettle
var n: int32 = add(2, 3);
```

Arguments pass by value, structs included. A generic call names its type
arguments: `id<int64>(7)`.

A method call is `value.method(args)` or `pointer->method(args)`.
[Declarations](declarations.md) covers how the compiler finds the function
behind it.

Writing a function's name with no parentheses gives you the function itself,
which has a function-pointer type. It does not call it.

## sizeof and typeof

`sizeof(T)` is the size of a type in bytes, settled at compile time.
`typeof(T)` is a compile-time `Type` value, for use with
[`comptime for`](control-flow.md).

## Lambdas

A lambda is an expression:

```mettle
var dbl: fn(int32) -> int32 = fn(x: int32) -> int32 { return x * 2; };
```

Stored in a `Fn(...)` it may capture. [Declarations](declarations.md) covers
the capture rules.

## String interpolation

Every string literal is scanned for `{expr}`. The expression is evaluated and
its text spliced in. `{{` writes one literal `{`.

```mettle
var i: int32 = 7;
var d: float64 = 2.5;
var c: char = 'z';
var s: string = "txt";
println("{i} {d} {c} {s} {i * 2 + 1} {(int64)i}");
```

```text
7 2.5 z txt 15 7
```

Any expression may appear inside the braces: arithmetic, a cast, a field
access, an index, a call. Every scalar type prints in its own way. A `char`
prints as the character and a `uint8` as the number. A `bool` prints `true` or
`false`.

The braces are part of the literal, so interpolation works in any string, and
[`print`](standard-library.md) and `println` are ordinary functions taking one
`string`.

## See also

- [Types](types.md)
- [Control flow](control-flow.md)
- [Declarations](declarations.md)
