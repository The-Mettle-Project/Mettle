# regex

Regular expressions for Mettle, in pure Mettle. Matching runs in time
linear in the haystack, so no pattern can cause catastrophic backtracking.

- Syntax of Rust's regex crate (close to RE2 and Perl): classes with set
  operations, Unicode properties, named groups, inline flags, lazy and
  counted repetition, word boundaries.
- Unicode 15.1 aware: `\w`, `\d`, `\s`, `\b`, case-insensitive matching with
  simple case folding, scripts, categories and binary properties (UTS #18
  level 1).
- Find, find all, captures, replace (with `$name` expansion), split.
- Fast paths from Rust's regex-automata and RE2: a literal prefilter, forward
  and reverse lazy DFAs, a bounded backtracker and a PikeVM.
- Passes the Rust regex test suite (1078 tests) and RE2's search tests
  (1760 tests) with each engine, and is fuzzed against itself.

## Use

Add it with mbuild, then:

```mettle
import "std/io";
import "std/core";
import "regex/regex";

fn main() -> int32 {
  match (regex_new("(?P<user>[\\w.]+)@(?P<host>[\\w.]+)")) {
    case Ok(re): {
      var h: string = "mail ada@example.org today";
      var c: RegexCaptures = regex_captures_new(re);
      if (regex_captures(re, h, &c)) {
        println("user {regex_group_str(&c, h, 1)} host {regex_group_str(&c, h, 2)}");
      }
      regex_captures_free(&c);
      regex_free(re);
    }
    case Err(e): { println("bad pattern at byte {e.offset}: {e.msg}"); }
  }
  return 0;
}
```

Mettle string literals interpolate `{...}`, so write `{{` for a literal `{`
in a pattern: `"\\d{{4}"` is `\d{4}`.

More in [examples/](examples): `mbuild run validate`, `mbuild run logs`,
`mbuild run rewrite`.

## Documentation

- [docs/syntax.md](docs/syntax.md): pattern syntax and flags
- [docs/api.md](docs/api.md): every public function
- [docs/design.md](docs/design.md): how it works, standards followed, limits
- [docs/references.md](docs/references.md): where the ideas come from
- [docs/benchmarks.md](docs/benchmarks.md): numbers against ripgrep

## Build and test

```bash
mbuild test        # debug
mbuild test -r     # release
mbuild bench FILE  # benchmark on a text file
```

`tests/suite_test.mettle` runs the official suites four times: default
engines, PikeVM only, DFA without prefilter, and DFA with a tiny cache.
`tests/fuzz_test.mettle` compares engines on random patterns and feeds
random bytes to the parser.

## License

Apache-2.0, see [LICENSE](LICENSE). Unicode data and test data come from
third parties under their own licenses, see [THIRD_PARTY.md](THIRD_PARTY.md).
