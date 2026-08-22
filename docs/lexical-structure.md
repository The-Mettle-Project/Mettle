# Lexical structure

How the compiler turns Mettle source text into tokens: comments, names,
literals, and the words you cannot use as names.

## Source text

Source files are UTF-8 and end in `.mettle`. A byte-order mark at the start of
a file is skipped. Line endings may be LF or CRLF.

Outside string literals, spaces, tabs, and newlines separate tokens and carry
no other meaning. A statement ends at a semicolon or at the end of a line, so
both of these are one statement:

```mettle
var a: int32 = 1;
var b: int32 = 2
```

An expression may span lines. The compiler keeps reading while the statement is
plainly unfinished, so a long call or a long arithmetic chain can be broken
across lines without a continuation marker.

## Comments

`//` starts a comment that runs to the end of the line. `/*` starts a comment
that ends at the matching `*/`. An unterminated block comment is a lexical
error.

```mettle
// this line is a comment
var n: int32 = 0;  /* so is this */
```

## Identifiers

An identifier starts with a letter or `_` and continues with letters, digits,
and `_`. Identifiers are case sensitive.

A local you declare and never read draws a warning. Rename it with a leading
underscore to keep it on purpose:

```text
warning[E0003]: unused variable 'r'
   = help: remove it, or rename it to '_r' to keep it intentionally
```

## Keywords

These words are reserved and cannot name anything:

```text
asm       barrier   break     case      const     continue  default
defer     dispatch  else      enum      errdefer  export    extern
fn        for       if        impl      import    import_str
kernel    match     method    new       private   return    struct
switch    this      trait     var       where     while     workgroup
```

The built-in type names are also reserved: `int8`, `int16`, `int32`, `int64`,
`uint8`, `uint16`, `uint32`, `uint64`, `float32`, `float64`, `string`.

Some names are recognized by position and stay usable elsewhere: `bool`,
`char`, `cstring`, `rawptr`, `comptime`, `in`, `sizeof`, `typeof`, `Fn`,
`Type`, `Field`. Writing one where a type belongs gets you that type. Writing
one as a variable name works.

Inside an `asm` block the x86-64 mnemonics and register names are recognized
too, and they match without regard to case. They mean nothing outside a block.

## Integer literals

Decimal, hexadecimal with `0x`, and binary with `0b`. Hex digits and the `x`
or `b` marker may be upper or lower case.

```mettle
var d: int32 = 1000;
var h: int32 = 0xFF;
var b: int32 = 0b1010;
```

An integer literal with no other context is `int32`. Assigning it to a wider
type converts it in place, so `var n: int64 = 42` is exact. A literal too big
for its destination is rejected by [M0118](diagnostics.md).

## Float literals

A run of digits with a `.` in it is `float64`. To get a `float32`, name the
type or cast.

```mettle
var d: float64 = 3.14;
var f: float32 = (float32)3.14;
```

## Character literals

A single character in single quotes has type `char`, which is one byte. The
escapes are `\n`, `\t`, `\r`, `\`, `\'`, and `\0`. Any other escape is an
error.

```mettle
var c: char = 'h';
var nl: char = '\n';
```

## String literals

Text in double quotes has type `string`. The escapes are `\n`, `\t`, `\r`,
`\`, `\"`, and `\0`. An unrecognized escape is kept as the backslash followed
by the character.

Every string literal is scanned for interpolation. `{expr}` splices the value
of `expr` into the text, and `{{` writes one literal `{`:

```mettle
var n: int32 = 7;
println("n is {n}, twice is {n * 2}, a brace is {{");
```

```text
n is 7, twice is 14, a brace is {
```

[Expressions](expressions.md#string-interpolation) covers what may appear
inside the braces and which types print how.

## Operators and punctuation

```text
+   -   *   /   %   ~   !   &   |   ^   <<  >>
&&  ||  ==  !=  <   >   <=  >=
=   +=  -=  *=  /=  %=  &=  |=  ^=  <<= >>=
++  --  ->  .   ..  ,   ;   :   @
(   )   {   }   [   ]
```

[Expressions](expressions.md#precedence) gives the precedence table.

## See also

- [Types](types.md)
- [Declarations](declarations.md)
