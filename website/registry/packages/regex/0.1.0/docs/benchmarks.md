# Benchmarks

`mbuild bench FILE` counts the matches of nine patterns in FILE (best of
three runs, release build). The reference is ripgrep 14.1.1, which uses
Rust's regex crate, run as `rg --no-mmap -j1 --count-matches PATTERN FILE`
(its time includes reading the file and starting the process, about 7 ms).

Haystack: 28.9 MB, eight copies of `UnicodeData.txt` and `NamesList.txt`
from the UCD. Linux x86-64. Both tools report the same match counts for
every pattern.

| Pattern | Matches | regex (ms) | ripgrep (ms) |
|---|---:|---:|---:|
| `LATIN` | 27704 | 14 | 9 |
| `(?i)latin` | 37432 | 29 | 16 |
| `zqzqzq` (no match) | 0 | 2 | 7 |
| `LATIN\|GREEK\|CYRILLIC` | 45440 | 44 | 13 |
| `[A-Z]+ LETTER` | 179960 | 77 | 46 |
| `\w+` | 4853936 | 226 | 296 |
| `\b[0-9A-F]{4,6}\b` | 849792 | 106 | 142 |
| `(?m)^00[0-9A-F]{2};[^;]*` | 2048 | 15 | 9 |
| `([0-9A-F]{4});([^;\n]*)` with groups | 350368 | 320 | 115 (no groups) |

Notes:

- ripgrep only counts matches; it never extracts groups, so the last row
  compares unlike work. Capture extraction here uses the bounded backtracker
  over each match.
- Alternations of literals are where Rust leads most: it uses Teddy, a SIMD
  multi-literal search. Mettle has no vector types yet, so the prefilter here
  is a 64-bit word-at-a-time memchr2/memchr3.
- Catastrophic patterns stay linear: `(a*)*b` and `(?:a|aa)+$x` over 200 KB
  of `a` run in milliseconds (see `tests/fuzz_test.mettle`).
