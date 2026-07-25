# Expressions

Expressions produce values. They appear in initializers, assignments, function arguments, and control flow conditions.

**Operator precedence** (highest first):

Three tiers bind tighter than every binary operator. From tightest:

| Tier | Forms | Example |
|------|-------|---------|
| Postfix | call, generic call, member access `.` and `->`, indexing `[]` | `a.b[i].c(x)` |
| Unary | `-`, `+`, `*`, `&`, `~`, `!` | `-x`, `!y`, `*p`, `&v`, `~mask` |
| Cast | `(Type)expr` | `(int64)x` |

Postfix forms chain left to right; unary operators are right-associative. A cast binds tighter than any binary operator but looser than postfix, so `(int64)p->len` casts the field, not the pointer.

Binary operators, all **left-associative**, from tightest to loosest:

| Precedence | Operators | Example |
|------------|-----------|---------|
| 1 | Multiplicative `*`, `/`, `%` | `a * b`, `a % b` |
| 2 | Additive `+`, `-` | `a + b` |
| 3 | Shifts `<<`, `>>` | `a << 1` |
| 4 | Relational `<`, `<=`, `>`, `>=` | `a < b` |
| 5 | Equality `==`, `!=` | `a == b` |
| 6 | Bitwise AND `&` | `a & b` |
| 7 | Bitwise XOR `^` | `a ^ b` |
| 8 | Bitwise OR `\|` | `a \| b` |
| 9 | Logical AND `&&` | `a && b` |
| 10 | Logical OR `\|\|` | `a \|\| b` |

The shift level sits between additive and relational, as in C: `a << 1 < b` parses as `(a << 1) < b`, and `a + b << c` parses as `(a + b) << c`. Comparisons do not chain specially, so `a < b == c` parses as `(a < b) == c`, comparing a 0/1 result against `c`. Use parentheses to clarify or override.

## Literals

Numeric literals: decimal (`42`), hexadecimal (`0xFF`), binary (`0b1010`), floating-point (`3.14`). String literals: `"hello"`. The null pointer: `0` (for pointer types).

**Negative literals:** A leading minus is not part of the literal. The expression `-17` is parsed as the unary minus operator applied to the literal `17`. This matters for boundary values: `var x: int8 = -128` is valid because the literal `128` is negated to `-128`, which fits in `int8`. If `-128` were a literal, some implementations might reject it.

**Literal default types:** A bare integer literal like `42` has type `int32` when the context does not require a specific type. Floating-point literals default to `float64`. In expressions like `var x: int64 = 42`, the literal is implicitly converted to the expected type. See [Types](types.md) for conversion rules.

## Identifiers and Member Access

An identifier denotes a variable, parameter, or function. Member access uses `.` for struct fields and string fields. Pointer field access uses `->`.

```mettle
x
obj.field
ptr->field
s.chars
s.length
```

## Arithmetic and Comparison

Arithmetic: `+`, `-`, `*`, `/`, `%`. Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`. Operands must have compatible types. Integer division truncates toward zero. Modulo `%` returns the remainder and requires integer operands.

```mettle
a + b
a - b
a * b
a / b
a % b
a == b
a != b
a < b
a <= b
a > b
a >= b
```

**Bitwise operators:** Bitwise AND (`&`), OR (`|`), XOR (`^`), complement (`~`), and shifts (`<<`, `>>`) are supported for integer types. Unary `&` is address-of; binary `&` is bitwise AND. Context disambiguates.

**Logical operators:** Short-circuit logical AND (`&&`) and OR (`||`) are supported.

**Division by zero:** Integer division by zero produces undefined behavior. On x86-64, `idiv` raises a divide exception (#DE), typically resulting in a crash. The compiler does not insert runtime checks. Floating-point division by zero produces infinity or NaN per IEEE 754.

## Unary Operators

Negation `-x`. Logical NOT `!x` (returns 1 if x is 0, otherwise 0). Dereference `*p` (loads the value at the pointer). Address-of `&x` (produces a pointer to x). Address-of requires an assignable expression (lvalue).

```mettle
-x       // negation
!x       // logical NOT
*p       // dereference
&x       // address-of
```

**Null dereference:** In normal builds, the compiler emits runtime null checks for dynamic pointer dereference/indexing and traps with a fatal message on null. In `--release`, those generated checks are disabled; dereferencing a null pointer is undefined behavior and typically crashes. See [Types](types.md#pointer-types).

**Address-of on non-lvalues:** Taking the address of a temporary or non-assignable expression is a compile error. For example, `&(x + 1)` and `&42` are invalid—the operand must be a variable, struct field, array element, or dereferenced pointer. The error message is "Address-of operator requires an assignable expression".

## Indexing

Arrays and pointers support indexing. The index must be an integer. The expression `arr[i]` or `ptr[i]` computes the address of the element and loads or stores as appropriate in context.

**Element size:** Indexing advances by the size of the element type, not by bytes. For `int32* p`, the expression `p[1]` accesses the next `int32` (4 bytes). For `uint8*` or `cstring`, `ptr[i]` advances by 1 byte. This matches C semantics.

To pass an array to a function that expects a pointer, use `&arr[0]` or `&buf[0]`. The function parameter should have type `T*`:

```mettle
fn sum(buf: int32*, len: int32) -> int32 {
  var total: int32 = 0;
  var i: int32 = 0;
  while (i < len) {
    total = total + buf[i];
    i = i + 1;
  }
  return total;
}

var data: int32[10];
// ...
var result: int32 = sum(&data[0], 10);
```

## Function and Method Calls

Function calls: `name(args)`. Method calls: `obj.method(args)`. Arguments are evaluated left to right. The number and types must match the declaration.

```mettle
add(1, 2)
puts("hello")
obj.method(args)
```

**Argument type mismatches:** Argument types must be assignable to the parameter types. Incompatible types (e.g. passing `float64` where `int32` is expected) produce a compile error. Implicit conversions (e.g. `int32` to `int64`) are applied when the type checker allows them. See [Types](types.md#type-conversions).

**Function pointers:** Use the `fn(param_types) -> return_type` type to store and pass function addresses. Take the address with `&func` and call like a normal function: `fp(args)`. See [Types](types.md#function-pointer-type) for details.

## Allocation

The `new` expression allocates a zero-initialized value with a direct `calloc(1, size)` call and returns a pointer. Mettle does not link a heap runtime for this. See [Heap Allocation](heap-allocation.md) for details.

```mettle
var p: MyStruct* = new MyStruct;
```

**Initialization:** `new` allocates memory that is **zeroed**. All bytes of the allocated object are set to zero before the pointer is returned.

**Allocation failure:** `new` uses the platform C runtime `calloc`. If allocation fails, the result is null.

## Expression Evaluation Order

**Function arguments** are evaluated left to right. The first argument is fully evaluated before the second, and so on.

**Binary operands** (e.g. `a + b`, `x == y`) are evaluated in an implementation-defined order. Do not rely on the order of evaluation for side effects; use separate statements if the order matters.

## Cast Expressions

Explicit type casting is supported using the `(Type)expression` syntax. This allows explicit conversions between different numeric types, pointer types, and between integers and pointers.

```mettle
var f: float64 = 3.14;
var i: int64 = (int64)f;

var ptr: int32* = (int32*)0;
var addr: int64 = (int64)ptr;
```

Valid cast conversions include:
- Any numeric type (integer or float) to any other numeric type.
- Any pointer type to any other pointer type.
- Any integer type to any pointer type, and vice versa.
- Function pointers to other function pointers, or to/from regular pointers and integers.

Casting across different sizes might result in zero-extension, sign-extension, or truncation, depending on the target type and the sign of the source type. Floating-point to integer conversions truncate towards zero.

## Boolean Context

In control flow conditions (`if`, `while`, `for`), the condition must be a numeric type (integer or floating-point). Zero is false; non-zero is true. Pointers are not valid as conditions—use `ptr != 0` for null checks.

Comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`) produce `int32` with value 0 (false) or 1 (true). These values can be used directly in conditions. See [Control Flow](control-flow.md).

## String Expressions

**Concatenation:** The `+` operator concatenates two `string` values. Both operands must be `string`; the result is a heap-backed string whose `.chars` points to a freshly allocated buffer and whose `.length` is the sum of the operand lengths. The allocation is emitted as a direct `calloc(1, size)` call.

**Indexing:** Use `s.chars[i]` to access the i-th byte of a string. The `.chars` field is a pointer; indexing advances by 1 byte (element size of `uint8`). Pointer indexing is not bounds-checked; ensure `i < s.length` to avoid undefined behavior.
