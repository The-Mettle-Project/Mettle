# Diagnostics

Mettle reports compile problems with rustc-style rich diagnostics: a stable
error code, the source snippet with the offending range underlined, an inline
label saying what was expected, attached notes pointing at related code, and a
concrete `help:` suggestion.

```
error[E0003]: Function 'add' expects 2 arguments, got 3
  --> app.mettle:6:20
  |
5 | fn main() -> int64 {
6 |     var r: int64 = add(1, 2, 3);
  |                    ^^^ expected 2 arguments, got 3
7 |     var s: float64 = add(1, 2);
note: function 'add' defined here
  --> app.mettle:1:4
  |
1 | fn add(a: int64, b: int64) -> int64 {
  |    ^^^

error: could not compile `app.mettle` due to 1 previous error
help: for more about this error, run `mettle explain E0003`
```

## Behavior

- **All errors in one compile.** The type checker recovers after a bad
  statement or declaration and keeps checking, so a file with four mistakes
  reports four errors, not one per rebuild. A variable whose initializer
  fails is still registered with its declared type, so later uses of it do
  not cascade into bogus `Undefined variable` errors.
- **No parser cascades.** After a syntax error the parser resynchronizes at
  the next statement boundary, and repeated errors at the same location are
  suppressed. Contextual messages ("Expected '(' after 'if'") replace the
  generic token mismatch instead of stacking on top of it.
- **Typo suggestions.** Undefined names get a Levenshtein-based
  "did you mean 'count'?" over every symbol in scope.
- **A summary footer** counts errors and warnings and names the `explain`
  code for the first error.

## Warnings

- `unused variable 'x'` - a local was never read. Prefix the name with `_`
  to opt out (`var _x: ...`). Only the main compile unit is checked;
  imported modules stay quiet.
- `Unreachable code` - a statement follows a `return`/terminator.
- Memory-safety warnings and errors (`M0101`..`M0112`): use-after-free,
  double free, leaks, out-of-bounds constant indexing, escaping stack
  addresses, and borrow-checker lifetime findings. See
  [borrow-checker.md](borrow-checker.md).

## `mettle explain <CODE>`

Extended documentation for any diagnostic code, with an example and how to
fix it:

```
$ mettle explain E0004
$ mettle explain M0103
$ mettle explain list      # index of every code
```

Codes are stable across compiler versions:

| Code | Meaning |
|------|---------|
| E0001 | Lexical error |
| E0002 | Syntax error |
| E0003 | Semantic error (undefined name, duplicate, bad call, ...) |
| E0004 | Type mismatch |
| E0005 | Scope error |
| E0006 | I/O error |
| E0007 | Internal compiler error |
| M0101..M0112 | Memory-safety findings (`mettle explain list`) |

## Machine-readable output: `--error-format=json`

For editors and CI, `--error-format=json` prints one JSON object per
diagnostic on stderr (NDJSON):

```json
{"severity":"error","code":"E0004","message":"Type mismatch: expected 'int64', found 'string'",
 "file":"app.mettle","line":2,"column":20,"length":5,
 "label":"expected 'int64', found 'string'",
 "help":"use numeric literal without quotes: 42",
 "notes":[{"message":"...","file":"...","line":1,"column":4,"length":3}]}
```

Fields: `severity` (`error`|`warning`|`note`), stable `code`, `message`,
1-based `line`/`column`, `length` (characters to underline), optional
`label` (inline caret text) and `help` (suggestion), and `notes` (related
locations such as "previous declaration here").

## Color

ANSI color is used when stderr is a terminal. Overrides: `NO_COLOR` (off),
`CLICOLOR=0` (off), `CLICOLOR_FORCE=1` (force on), `TERM=dumb` (off).
Windows consoles get VT sequences enabled automatically.

## Related tooling

- `--explain` / `--explain-json` - optimization decision report (what
  vectorized/inlined and why not).
- `--annotate-asm`, `--annotate-lines=A-B` - codegen provenance.
- Compile-time memory diagnostics run automatically; disable interprocedural
  analysis with `METTLE_NO_MEM_INTERPROC=1`.
