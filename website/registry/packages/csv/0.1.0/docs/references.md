# References

## Standards

- **RFC 4180**, Common Format and MIME Type for Comma-Separated Values (CSV) Files, 2005.
  <https://www.rfc-editor.org/rfc/rfc4180>. The default dialect and `csv_dialect_rfc4180()`
  follow its grammar: comma delimiter, double quote, doubled quotes inside quoted fields, CRLF
  record ends (LF and CR are also accepted when reading, as section 2 allows in practice and
  every major reader does), an optional header row, and every record the same length.
- **RFC 3629**, UTF-8. `check_utf8` uses its well-formed byte sequences (no overlongs, no
  surrogates, nothing above U+10FFFF), as in table 3-7 of the Unicode Standard.
- **IEEE 754-2019** binary64 and binary32, round half to even, for number parsing and printing.

## Implementations studied

| Library | License | What was taken |
|---|---|---|
| Python `csv` module (`Modules/_csv.c`, CPython 3.13) | PSF | The reader's states and its repair rules for malformed input; escape bytes in and out of quotes; `skipinitialspace`; empty lines as empty records; the writer's QUOTE_MINIMAL/ALL/NONNUMERIC/NONE rules and the quoted lone empty field. Python is also the reference both test generators compare against. |
| Rust `csv` and `csv-core` (BurntSushi) | MIT / Unlicense | Behaviour of a reader that skips blank lines; comments only at record start; custom terminators; `flexible`, `has_headers` and `trim` settings; unequal-length errors that let reading go on; the BOM rule; byte-record and position (byte, line, record) reporting. Its 63 reader test cases are part of our suite. |
| simdcsv (Geoff Langdale, Daniel Lemire) | BSD-3 | The idea of a structural bitmap of special bytes built a block at a time and walked by bit scanning, instead of a byte-at-a-time state machine. |
| zsv (liquidaty) | MIT | The same idea without the quote-parity trick: flag every candidate byte, then let a small state machine decide at each flagged byte. Our reader follows this design, with SWAR in place of vector registers. Also our speed reference. |
| David Gay, "Correctly Rounded Binary-Decimal and Decimal-Binary Conversions" (1990); William Clinger, "How to Read Floating Point Numbers Accurately" (1990) | paper | The exact fast path for short decimals, and exact big-integer conversion for the rest. |
| Robert Burger, R. Kent Dybvig, "Printing Floating-Point Numbers Quickly and Accurately" (1996) | paper | Shortest float to text. |
| Sean Anderson, "Bit Twiddling Hacks"; Alan Mycroft (1987) | public domain | The zero-byte test `(x - 0x01..) & ~x & 0x80..`; de Bruijn trailing-zero count (Leiserson, Prokop, Randall, 1998). |

No code was copied from any of these; the Mettle code is new. Test cases taken from csv-core and
csv-spectrum are data, listed in [THIRD_PARTY.md](../THIRD_PARTY.md).

## Choices

- **Repair by default, strict on request.** Real files have stray quotes; Python and Rust both
  repair, and agree with each other on how. Strict mode exists for validation, and points at
  the byte that broke the rule.
- **Python's escape rules over csv-core's.** csv-core honours the escape byte only inside
  quotes; Python honours it everywhere. Python's rule reads every file csv-core's does the same
  way (csv-core's tests pass), and also reads files written with `QUOTE_NONE` and an escape.
- **Blank lines skipped by default** (Rust), with Python's empty records as an option.
- **SWAR bitmap rather than a byte table DFA.** csv-core runs a table-driven DFA at about one
  byte per step. The bitmap skips plain bytes eight at a time and costs per special byte
  instead, which wins on real data where fields are several bytes long.
- **Shortest float text, in Python's layout.** It reads back exactly, is the shortest such
  text, and is the format Python users expect in CSV files.

## Test suites

- csv-core reader tests: <https://github.com/BurntSushi/rust-csv/blob/master/csv-core/src/reader.rs>
- csv-spectrum: <https://github.com/max-mapper/csv-spectrum>. Its `location_coordinates.json`
  has a different phone number than its CSV file; the test uses the CSV's value.
- Python 3.13's `csv` and `float`, through `tools/gen_tests.py`.
