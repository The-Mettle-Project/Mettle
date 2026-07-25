# Lexical Structure

This document covers the low-level syntax of Mettle: comments, identifiers, keywords, literals, and operators.

## Comments

Line comments start with `//` and extend to the end of the line. Everything after `//` is ignored by the compiler.

```mettle
// Line comment: everything from // to end of line is ignored
var x: int32 = 42;  // inline comment
```

Block comments are written `/* ... */` and may span multiple lines:

```mettle
/* This is a block comment.
   It can span multiple lines. */
var x: int32 = 42; /* inline block comment */
```

Block comments **nest**, so you can comment out a region that already
contains a block comment:

```mettle
/* outer
   /* inner */
   still commented */
var ready: int32 = 1;
```

An unterminated block comment (missing the final `*/`) is a lexer error.

The sequences `//`, `/*`, and `*/` inside a string literal are not treated as comments; they become part of the string. For example, `"http://example.com"` produces a string containing `http://example.com` with no comment.

## Identifiers

Identifiers name variables, functions, types, and other program elements. They must start with a letter or underscore, followed by any combination of letters, digits, or underscores. Identifiers are case-sensitive. There is no documented length limit; the lexer accepts identifiers until it hits a non-alphanumeric character. Identifiers are strictly ASCII; Unicode identifiers are not supported. The lexer uses `isalpha` and `isalnum`, which treat only ASCII letters and digits as valid. The compiler interns identifier-like names for memory efficiency; see [Compilation](compilation.md#string-interning).

```
my_var
_private
Vector3
```

## Keywords

The following words are reserved and cannot be used as identifiers.

Declarations: `import`, `import_str`, `extern`, `export`, `var`, `const`, `fn`, `kernel`, `struct`, `enum`, `trait`, `impl`, `where`, `method`; GPU storage qualifiers: `workgroup`, `private`. Control flow: `if`, `else`, `while`, `for`, `switch`, `case`, `default`, `match`, `break`, `continue`, `return`, `defer`, `errdefer`, `dispatch`, `barrier`. Other: `asm`, `this`, `new`. Types: `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`, `float32`, `float64`, `string`.

`kernel` declares a GPU entry point, `workgroup var` / `private var` declare static device storage, `barrier(...)` synchronizes a workgroup, and `dispatch` launches a kernel; see [GPU Offload](gpu.md). `in` (used in range-based `for i in lo..hi`) is a *contextual* keyword: it is only special in that position and remains usable as an ordinary identifier elsewhere.

`this` is only valid inside method bodies; it refers to the receiver. Using `this` as a variable name outside a method produces an error. `new` is an expression keyword, not a statement keyword; it appears in expressions like `var p: T* = new T` and cannot start a statement by itself.

Several built-in names are **not** reserved words, so they are ordinary identifiers that happen to have built-in meaning: the type names `bool`, `cstring`, and `void`; the `bool` constants `true` and `false`; the compile-time forms `sizeof`, `assert`, and `assert_eq`; the closure type constructor `Fn`; and the GPU built-ins (`thread`, `block`, `block_dim`, `grid_dim`, and the `subgroup_*`, `atomic_*`, and `tensor_*` families).

x86 mnemonics and register names (`add`, `mov`, `cmp`, `call`, `ret`, `push`, `pop`, `lea`, `jmp`, `eax` through `r15`, and similar) are recognized as distinct tokens for the reserved inline-assembly syntax, matched case-insensitively. They are still accepted wherever an identifier is expected, so a variable named `add` or `cmp` compiles normally.

## Numeric Literals

Decimal literals use digits: `42`, `0`. A leading zero does not select octal: `007` is decimal 7. Hexadecimal: `0x1A`, `0xFF`, `0Xdead`. Binary: `0b1010`, `0B1111`. Floating-point: `3.14`, `0.5`. Invalid literals (e.g. empty hex after `0x`) produce lexical errors.

**Exponent notation is not supported.** A float literal is recognized by the presence of a `.` in the digits, so `1e-3` does not lex as one number; it becomes the literal `1`, the identifier `e`, the operator `-`, and the literal `3`. Write `0.001` instead.

The lexer will not consume a `.` that begins a `..` range, so `1..5` lexes as three tokens rather than as the float `1.` followed by `.5`.

A leading minus is not part of the literal. The expression `-17` is parsed as the unary minus operator applied to the literal `17`. So `var x: int8 = -128` is valid: the literal `128` is negated to `-128`, which fits in `int8`. Integer literals are parsed as decimal strings and must fit within the target type when used; the implementation uses `strtol`/`strtoull` internally. There is no formal maximum; values that overflow the target type may produce implementation-defined behavior.

Underscores in numeric literals (e.g. `1_000_000`) are not supported. The underscore would terminate the number and start an identifier. Use `1000000` instead.

## Character Literals

A character literal is a single character in single quotes, or one escape sequence. The supported escapes are `\n`, `\t`, `\r`, `\\`, `\'`, and `\0`; any other escape is a lexical error.

```mettle
var newline: int32 = '\n';   // 10
var letter:  int32 = 'A';    // 65
```

**A character literal is an integer literal.** The lexer converts it to the numeric value of the byte, so `'A'` and `65` are indistinguishable after lexing, and a character literal is usable anywhere an integer literal is, including in `const` initializers and `switch` case values. There is no distinct character type.

A literal containing more than one character, or a raw newline, is a lexical error. Note that `\"` is not among the character escapes; a double quote inside single quotes is just an ordinary character.

## String Literals

Strings are enclosed in double quotes. The compiler processes escape sequences before storing the value. Supported escapes: `\n` (newline, LF), `\t` (tab), `\r` (carriage return), `\\` (backslash), `\"` (double quote), `\0` (null byte). Unknown escape sequences are preserved literally: the backslash and the following character are both stored. For example, `"\q"` produces the two characters `\` and `q`, not a single character. String literals have type `string` (see [Types](types.md)).

```mettle
var msg: string = "Hello\nWorld\t\"quoted\"";
```

Multiline strings are supported. A newline inside the quotes is stored as a literal newline; the string continues until the closing quote. An unterminated string (no closing quote before end of file) produces a lexical error. There is no documented maximum length; strings are limited by available memory and source file size.

## Operators and Punctuation

Assignment `=`. Compound assignment `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`. Comparison `==`, `!=`, `<`, `>`, `<=`, `>=`. Logical `&&`, `||`. Arithmetic `+`, `-`, `*`, `/`, `%`. Unary `-` (negation), `*` (dereference), `&` (address-of). Member access `.`. Arrow `->`. Range `..` (exclusive) and `..=` (inclusive), used in `for i in lo..hi`. Brackets `( )`, `{ }`, `[ ]`. Delimiters `:`, `;`, `,`. The `@` sigil introduces a loop attribute (`@simd` / `@simd!`); see [Control Flow](control-flow.md#vectorization-contracts).

**Compound assignment:** `target OP= value` is exact syntactic sugar for `target = target OP value`, where `OP` is one of `+ - * / % & | ^ << >>`. The target is evaluated as an ordinary assignment target (identifier, struct field, array element, or pointer dereference) and must be a valid lvalue. For example, `count += 1` is identical to `count = count + 1`, and `mask &= 0xFF` is identical to `mask = mask & 0xFF`. Compound assignment is a statement (also valid as a `for`-loop initializer or increment), not an expression, so it does not produce a value. See [Expressions](expressions.md).

**Operator precedence:** every binary operator is left-associative. From tightest to loosest: multiplicative (`*`, `/`, `%`), additive (`+`, `-`), shifts (`<<`, `>>`), relational (`<`, `<=`, `>`, `>=`), equality (`==`, `!=`), bitwise AND (`&`), bitwise XOR (`^`), bitwise OR (`|`), logical AND (`&&`), logical OR (`||`). Postfix forms (call, member access, indexing), then unary operators, then casts all bind tighter than any binary operator. So `a + b * c` parses as `a + (b * c)`, `a << 1 < b` parses as `(a << 1) < b`, and `a < b == c` parses as `(a < b) == c`. See [Expressions](expressions.md) for the full table. Use parentheses to override.

**Modulo:** The modulo operator `%` returns the remainder of integer division. It requires integer operands. See [Expressions](expressions.md).

**Bitwise operators:** Bitwise AND (`&`), OR (`|`), XOR (`^`), complement (`~`), and shifts (`<<`, `>>`) are supported for integer types. The unary `&` is address-of; the binary `&` is bitwise AND. Context disambiguates.

**Logical operators:** Short-circuit logical AND (`&&`) and OR (`||`) are supported.

**Arrow `->`:** The arrow serves two roles. In function signatures it denotes the return type: `fn f() -> int32`. In expressions it denotes pointer field access: `ptr->field`. Both uses appear in the same program:

```mettle
struct Point { x: int32; y: int32; }

fn get_x(p: Point*) -> int32 {
  return p->x;
}
```


## Lexer Token Model (Implementation Note)

Tokens produced by the lexer carry both:

- `value`: a null-terminated C string used by parser and semantic phases.
- `lexeme`: a string view (`data` pointer + `length`) for length-aware token text handling without `strlen`.

For identifier-like tokens, `value` points to an interned global string (deduplicated across the compilation). This enables fast pointer-based equality checks for names in later phases.
