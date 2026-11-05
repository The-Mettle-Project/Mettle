# API

```mettle
import "regex/regex";
```

All public names start with `regex_`, `Regex` or `REGEX_`. Internal names
start with `rx_` / `Rx` / `RX_`. Haystacks are `string` values (byte views);
positions are byte offsets.

## Compiling

```mettle
fn regex_new(pattern: string) -> Result<Regex*, RegexError>
fn regex_new_with(pattern: string, flags: int32) -> Result<Regex*, RegexError>
fn regex_new_limits(pattern: string, flags: int32, size_limit: int64, dfa_limit: int64) -> Result<Regex*, RegexError>
fn regex_free(re: Regex*)
```

`flags` is an or of `REGEX_ICASE`, `REGEX_MULTILINE`, `REGEX_DOTALL`,
`REGEX_UNGREEDY`, `REGEX_NO_UNICODE`, `REGEX_VERBOSE`, `REGEX_CRLF` and
`REGEX_BYTES` (see [syntax.md](syntax.md)).

`size_limit` bounds the compiled program (default `REGEX_SIZE_LIMIT`,
10 MiB) and `dfa_limit` each of the two lazy DFA caches (default
`REGEX_DFA_LIMIT`, 2 MiB). A pattern over the size limit fails with
`REGEX_E_SIZE`.

```mettle
struct RegexError {
  code: int32;     // REGEX_E_SYNTAX, REGEX_E_UNSUPPORTED, REGEX_E_SIZE, REGEX_E_NEST
  offset: int64;   // byte offset in the pattern
  msg: string;     // plain-English message (static, do not free)
}
```

A `Regex` keeps search caches inside it. Do not use one `Regex` from two
threads at the same time; compile one per thread.

## Searching

```mettle
struct RegexMatch { start: int64; end: int64; }

fn regex_is_match(re: Regex*, h: string) -> bool
fn regex_find(re: Regex*, h: string, m: RegexMatch*) -> bool
fn regex_find_at(re: Regex*, h: string, start: int64, m: RegexMatch*) -> bool
fn regex_find_in(re: Regex*, h: string, start: int64, end: int64, anchored: bool, m: RegexMatch*) -> bool
```

`regex_find_at` starts searching at `start`, but assertions such as `^` and
`\b` still see the bytes before it. `regex_find_in` also stops at `end`, and
with `anchored` the match must begin exactly at `start`.

## Capture groups

```mettle
struct RegexCaptures { slots: int64*; n: int32; }

fn regex_captures_new(re: Regex*) -> RegexCaptures
fn regex_captures_free(c: RegexCaptures*)
fn regex_captures(re: Regex*, h: string, c: RegexCaptures*) -> bool
fn regex_captures_at(re: Regex*, h: string, start: int64, c: RegexCaptures*) -> bool
fn regex_captures_in(re: Regex*, h: string, start: int64, end: int64, anchored: bool, c: RegexCaptures*) -> bool
fn regex_group(c: RegexCaptures*, i: int32, m: RegexMatch*) -> bool
fn regex_group_str(c: RegexCaptures*, h: string, i: int32) -> string
fn regex_group_count(re: Regex*) -> int32
fn regex_group_index(re: Regex*, name: string) -> int32
fn regex_group_name(re: Regex*, i: int32) -> string
```

Group 0 is the whole match. `regex_group` returns false for a group that did
not take part in the match. `regex_group_str` returns a view into `h` (no
allocation), or `""`.

## Iterating

```mettle
struct RegexIter { ... }

fn regex_iter(re: Regex*, h: string) -> RegexIter
fn regex_next(it: RegexIter*, m: RegexMatch*) -> bool
fn regex_next_captures(it: RegexIter*, c: RegexCaptures*) -> bool
fn regex_find_all(re: Regex*, h: string) -> RegexMatches   // free with regex_matches_free
fn regex_count(re: Regex*, h: string) -> int64
```

Matches never overlap. An empty match right where the previous match ended
is skipped, as in Rust and Python.

## Replacing and splitting

```mettle
fn regex_replace(re: Regex*, h: string, rep: string) -> string
fn regex_replace_all(re: Regex*, h: string, rep: string) -> string
fn regex_replacen(re: Regex*, h: string, limit: int64, rep: string) -> string
fn regex_split(re: Regex*, h: string) -> RegexPieces        // free with regex_pieces_free
fn regex_splitn(re: Regex*, h: string, limit: int64) -> RegexPieces
```

Replacement strings may use `$1`, `$name`, `${name}` (use `${{name}` inside a
Mettle literal) and `$$` for a dollar sign. `$name` takes the longest run of
letters, digits and `_`, so write `${1}a` rather than `$1a`. A group that did
not match, or does not exist, inserts nothing. The returned string is new:
free its `.chars`. Pieces from `regex_split` are views into `h`.

## Other

```mettle
fn regex_escape(s: string) -> string          // quote meta characters; free .chars
fn regex_unicode_version() -> string          // "15.1.0"
```
