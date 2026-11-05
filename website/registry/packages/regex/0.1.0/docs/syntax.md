# Pattern syntax

The syntax is the one of Rust's regex crate, which is close to RE2 and to
Perl without backreferences or look-around. Those two features are refused
with `REGEX_E_UNSUPPORTED`, because no engine can run them in linear time.

**Mettle string literals interpolate `{...}`.** Write `{{` for a literal `{`
in a pattern: `"\\d{{3}-\\d{{4}"` is the pattern `\d{3}-\d{4}`. A lone `}`
needs no escape.

## Characters

| Pattern | Matches |
|---|---|
| `x` | the character x |
| `.` | any character except `\n` (with `s`: any character; with `R`: not `\r` either) |
| `\.` | a literal `.`; any ASCII punctuation can be escaped this way |
| `\t \n \r \f \v \a` | tab, newline, carriage return, form feed, vertical tab, bell |
| `\x7F` | hex code (exactly two digits) |
| `\x{10FFFF}` | any hex code point (1 to 8 digits) |
| `\u007F`, `\u{7F}` | four-digit or braced code point |
| `\U0000007F`, `\U{7F}` | eight-digit or braced code point |

With Unicode mode off (`(?-u)`), `\x80` to `\xFF` match single raw bytes.

## Classes

| Pattern | Matches |
|---|---|
| `[xyz]` | any of x, y, z |
| `[^xyz]` | anything except x, y, z |
| `[a-z]` | a range |
| `[[:alpha:]]` | ASCII class (also `[[:^alpha:]]`) |
| `[x[^xyz]]` | nested class (union) |
| `[a-y&&xyz]` | intersection |
| `[a-z--aeiou]` | difference |
| `[a-g~~b-h]` | symmetric difference |
| `[\[\]]` | escaped brackets |

Set operators have equal precedence and run left to right. Negation applies
last: `[^a-z&&b]` is `[^[a-z&&b]]`. A `]` or `-` right after `[` (or `[^`) is
literal.

ASCII classes: `alnum alpha ascii blank cntrl digit graph lower print punct
space upper word xdigit`.

## Perl classes

| Pattern | Unicode mode (default) | `(?-u)` |
|---|---|---|
| `\d` `\D` | `\p{Nd}` | `[0-9]` |
| `\s` `\S` | `\p{White_Space}` | `[\t\n\v\f\r ]` |
| `\w` `\W` | `[\p{Alphabetic}\p{M}\p{Nd}\p{Pc}\p{Join_Control}]` | `[0-9A-Za-z_]` |

These follow Unicode Technical Standard #18, Annex C.

## Unicode properties

| Pattern | Matches |
|---|---|
| `\pN` | one-letter name |
| `\p{Greek}` | a general category, script or binary property |
| `\PN`, `\P{Greek}`, `\p{^Greek}` | the negation |
| `\p{gc=Lu}`, `\p{sc:Greek}`, `\p{scx=Grek}` | explicit property |
| `\p{gcb=Extend}`, `\p{wb=ALetter}`, `\p{sb=Close}` | break properties (UAX #29) |
| `\p{sc!=Greek}` | negated value |

Names are matched loosely (UAX #44 rule LM3): case, spaces, `_` and `-` are
ignored, and a leading `is` is dropped. A bare name is looked up as a binary
property, then a general category, then a script, as in Rust. Supported:
every general category and group, every script and script extension, the
binary properties of `PropList.txt`, `DerivedCoreProperties.txt` and
`emoji-data.txt`, the three break properties, and `Any`, `ASCII`,
`Assigned`. Data is Unicode 15.1.0.

## Assertions

| Pattern | Holds |
|---|---|
| `^` | at the start of the text (with `m`: also after `\n`) |
| `$` | at the end of the text (with `m`: also before `\n`) |
| `\A`, `\z` | start, end of the text (always) |
| `\b`, `\B` | at a Unicode word boundary, or not |
| `\b{start}`, `\<` | start of a word |
| `\b{end}`, `\>` | end of a word |
| `\b{start-half}`, `\b{end-half}` | no word character before, after |

In Unicode mode `\B` never holds next to bytes that are not valid UTF-8. In
`(?-u)` mode the word assertions use ASCII word characters.

## Groups and flags

| Pattern | Meaning |
|---|---|
| `(exp)` | numbered capture group |
| `(?P<name>exp)`, `(?<name>exp)` | named capture group |
| `(?:exp)` | group without capture |
| `(?flags)` | set flags until the end of the current group |
| `(?flags:exp)` | set flags for exp only |

Flags: `i` case-insensitive (Unicode simple case folding), `m` multi-line,
`s` dot matches `\n`, `U` swap greedy and lazy, `u` Unicode (on by default),
`x` verbose (ignore spaces, `#` starts a comment), `R` CRLF mode (`^`, `$`
and `.` treat `\r\n` as the line end). `(?i-s)` sets i and clears s.

The same flags can be given when compiling: `REGEX_ICASE`,
`REGEX_MULTILINE`, `REGEX_DOTALL`, `REGEX_UNGREEDY`, `REGEX_NO_UNICODE`,
`REGEX_VERBOSE`, `REGEX_CRLF`. `REGEX_BYTES` lets empty matches split a UTF-8
sequence (useful when the haystack is not text).

## Repetition

| Pattern | Matches |
|---|---|
| `x*` `x+` `x?` | zero or more, one or more, zero or one |
| `x{n}` `x{n,}` `x{n,m}` | exactly n, at least n, n to m (n ≤ m ≤ 100000) |
| `x*?` `x+?` `x??` `x{n,m}?` | the same, preferring fewer |

## Semantics

Searches are leftmost-first, like Perl, RE2 and Rust: among matches that
start at the leftmost position, the one a backtracking engine would find
first wins. Iteration skips an empty match that starts where the previous
match ended. In UTF-8 mode (the default) an empty match never splits a UTF-8
sequence.
