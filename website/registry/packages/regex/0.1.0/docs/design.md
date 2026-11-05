# Design

The goal is a regex engine whose worst case is linear in the haystack, with
the speed of a DFA on common patterns. The design follows Rust's
regex-automata and RE2; see [references.md](references.md) for where each
idea comes from.

## Pipeline

```
pattern ──parse──> HIR ──compile──> NFA (forward, with captures)
                    │          └──> NFA (reverse, no captures)
                    └──literals──> prefilter
```

1. **Parser** (`src/syntax/parse.mettle`) turns the pattern into the HIR
   directly, applying flags as it goes (as RE2 does), so later stages never
   see flags. Case-insensitive literals become small classes. Errors carry a
   byte offset. Nesting is limited to 250 levels, counted repetition to
   100000.
2. **HIR and classes** (`src/hir/`) are an array of nodes plus classes held
   as sorted code point ranges, with union, intersection, difference,
   symmetric difference, negation and simple case folding.
3. **Unicode data** (`src/unicode/`) is a 250 KB binary table generated from
   the UCD by `tools/gen_unicode.py` and embedded with `import_str`.
4. **NFA** (`src/nfa/nfa.mettle`) is a Thompson NFA over bytes. Unicode
   classes become UTF-8 automata: ranges are split into UTF-8 byte-range
   sequences, put in a trie, and identical states are shared, which keeps
   classes like `\w` small. `e*` where `e` can match empty is compiled as
   `(e+)?`, as Rust does, which gives the expected group spans for patterns
   like `(a*)*`. A reverse NFA is built from the same HIR for finding match
   starts.
5. **Engines**:
   - `src/literal/prefilter.mettle`: prefix literals are extracted from the
     HIR. A single literal is searched with a rare-byte memmem; up to three
     candidate bytes at the rarest position use memchr2/memchr3. The search
     loops work eight bytes at a time (SWAR). A pattern that is exactly one
     literal never runs an automaton at all.
   - `src/dfa/lazy.mettle`: lazy DFA. States are built during the search
     and cached (2 MiB by default). A state is an ordered list of NFA
     states plus a few bits about the previous byte, so `^`, `$` and `\b`
     are decided when the next byte is seen; matches are reported one byte
     late. Leftmost-first order is kept by dropping lower priority threads
     after a match. When the cache fills, it is cleared; if that keeps
     happening with little progress, the DFA gives up. With Unicode `\b` the
     DFA gives up on non-ASCII bytes. Bytes are grouped into equivalence
     classes to keep transition tables small.
   - `src/nfa/pikevm.mettle`: PikeVM, the always-available engine. It
     tracks capture slots per thread, in priority order.
   - `src/nfa/backtrack.mettle`: bounded backtracker for capture groups over
     short spans. It never revisits a (state, position) pair, so it is
     linear too.
6. **Meta strategy** (`src/regex.mettle`): literal search if the pattern is
   one literal; otherwise forward DFA (with prefilter) for the match end,
   reverse DFA for the start, then the backtracker or PikeVM over just the
   match if groups are wanted. If a DFA gives up, the PikeVM runs the search.

## Standards

- Unicode Technical Standard #18, Unicode Regular Expressions. Level 1 is
  covered: hex notation (RL1.1), properties (RL1.2: general category,
  script, script extensions, binary properties), compatibility properties
  (RL1.2a, Annex C), subtraction and intersection (RL1.3), word boundaries
  (RL1.4, as simple `\b`), simple loose matches with simple case folding
  (RL1.5), line boundaries with `\n` or `\r\n` (RL1.6, partly), and full
  code point range (RL1.7).
- Unicode Standard Annex #44, Unicode Character Database: data files and the
  LM3 loose matching rule for property names.
- Unicode Standard Annex #29: the Grapheme_Cluster_Break, Word_Break and
  Sentence_Break properties are available as classes.
- Unicode Standard, chapter 3 (and RFC 3629): the UTF-8 encoding used for
  byte automata; invalid UTF-8 in a haystack never matches a Unicode class.
- POSIX: bracket expression class names (`[:alpha:]` and so on), ASCII only.

## Limits and safety

- Compile limits: program size (default 10 MiB), nesting depth (250),
  repetition count (100000). Bad patterns return an error and never crash.
- Search memory: two DFA caches (2 MiB each by default), the PikeVM (states
  times capture slots) and the backtracker (at most 256 KB of bits).
- The parser and every engine are fuzzed in `tests/fuzz_test.mettle`.

## Not supported

- Backreferences and look-around: they need backtracking, which breaks the
  linear-time promise.
- Compile-time patterns: a compiled `Regex` lives on the heap, and Mettle's
  compile-time interpreter cannot yet produce heap structures as constants.
- SIMD intrinsics: Mettle has no vector types yet, so the literal search
  uses 64-bit word tricks instead (Teddy, the multi-literal SIMD search of
  Hyperscan and Rust, is not available).
- Regex sets (many patterns at once) and overlapping search.
