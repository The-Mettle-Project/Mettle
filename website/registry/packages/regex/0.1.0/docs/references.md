# References

No code was copied from these projects. Ideas and algorithms came from them
as listed. Test data from Rust regex and RE2 is used under their licenses
(see `THIRD_PARTY.md`).

## Rust regex, regex-syntax, regex-automata, memchr

https://github.com/rust-lang/regex, https://github.com/BurntSushi/memchr
(MIT or Apache-2.0)

- Pattern syntax, flags, error cases and semantics, including `\b{start}`,
  `\<`, nested class set operations and the lookup order of bare property
  names.
- Perl class definitions per UTS #18 Annex C.
- HIR as a flag-free intermediate form, and literal extraction into prefix
  sets with exact and inexact literals.
- Byte-based Thompson NFA with UTF-8 automata for classes; the UTF-8 range
  splitting algorithm (utf8-ranges).
- `e*` compiled as `(e+)?` when `e` can match empty.
- The meta strategy: prefilter, forward lazy DFA for the end, reverse lazy
  DFA for the start, then a capture engine over the match only; fallback to
  the PikeVM.
- Lazy (hybrid) DFA cache clearing with a give-up rule, and giving up on
  non-ASCII bytes when Unicode `\b` is present.
- Bounded backtracker with a visited bitset for captures on short spans.
- Unicode `\B` never matching next to invalid UTF-8.
- Iterator rule for empty matches and UTF-8 empty-match splitting.
- Replacement syntax (`$name`, `${name}`, `$$`).
- memchr family: word-at-a-time fallback search and rare-byte selection for
  substring search.
- The test suite in `testdata/` (TOML).

## RE2

https://github.com/google/re2 (BSD 3-Clause)

- Applying flags during parsing; case-insensitive literals as folded
  classes.
- Lazy DFA built during search, with matches reported one byte late,
  empty-width flags decided at the next byte, and leftmost-first order kept
  by cutting lower priority threads after a match.
- PikeVM with threads in priority order and capture copying; BitState (the
  bounded backtracker idea).
- Reverse program for finding where a match starts.
- The search test log `re2-search.txt`.

## Hyperscan

https://github.com/intel/hyperscan (BSD 3-Clause)

- Literal-first matching: find required literals with fast scans and run the
  automaton only around candidates. Teddy (SIMD multi-literal search) is the
  Hyperscan technique Rust adopted; it is planned here once Mettle has
  vector types.

## Papers and articles

- Ken Thompson, "Regular Expression Search Algorithm", CACM 1968: the NFA
  construction.
- Rob Pike's PikeVM, as described by Russ Cox, "Regular Expression Matching:
  the Virtual Machine Approach" (2009).
- Russ Cox, "Regular Expression Matching Can Be Simple And Fast" (2007) and
  "Regular Expression Matching in the Wild" (2010): lazy DFA design, reverse
  search for match starts.
- Jan Daciuk et al., "Incremental Construction of Minimal Acyclic Finite-State
  Automata" (2000): sharing identical states in the UTF-8 automata.

## Standards

- Unicode Technical Standard #18, Unicode Regular Expressions.
- Unicode Standard Annex #44, Unicode Character Database (version 15.1.0).
- Unicode Standard Annex #29, Unicode Text Segmentation (break properties).
- The Unicode Standard, chapter 3, and RFC 3629 (UTF-8).
- POSIX.1-2017, Base Definitions, 9.3.5 (bracket expressions, class names).
