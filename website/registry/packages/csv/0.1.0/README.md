# csv

CSV for Mettle, written in pure Mettle.

- Reads and writes [RFC 4180](https://www.rfc-editor.org/rfc/rfc4180) CSV, and the dialects
  real files use: any delimiter, any quote byte or no quoting, escape bytes, doubled or escaped
  quotes, comment lines, CR, LF or CRLF line ends (or a custom record terminator), headers,
  byte order marks, initial-space skipping and trimming.
- A fast streaming reader. Fields are views into the input whenever possible, so most fields
  are never copied. Special bytes are found 64 at a time with a SWAR bitmap, the way simdcsv
  and zsv do it with vector registers.
- A writer that quotes exactly when needed, matching Python's `csv.writer` byte for byte.
- Typed rows: read records into your structs by header name, and write structs back out.
  Numbers are exact: floats parse correctly rounded and print as the shortest text that reads
  back to the same value.
- Clear errors: byte offset, line, column, record and field. Malformed input never crashes the
  reader. It is repaired the way Python and Rust's csv crate repair it, or, in strict mode,
  reported.

## Use

```bash
mbuild get csv
```

```mettle
import "std/io";
import "csv/csv";

fn main() -> int32 {
  var r: CsvReader* = csv_reader_open("prices.csv", csv_dialect());
  if (r == 0) { return 1; }
  var rec: CsvRecord;
  while (true) {
    var st: int32 = csv_read(r, &rec);
    if (st == CSV_END) { break; }
    if (st == CSV_ERROR) {
      var buf: uint8[200];
      println(csv_error_format(csv_reader_error(r), &buf[0], 200));
      break;
    }
    println("{csv_record_get(&rec, 0)} costs {csv_record_get(&rec, 1)}");
  }
  csv_reader_free(r);
  return 0;
}
```

Rows as structs:

```mettle
struct Trade { symbol: string; qty: int64; price: float64; }

var schema: CsvSchema* = csv_schema_new();
comptime for f in typeof(Trade).fields {
  csv_schema_add(schema, f.name, (int32)f.type.kind, f.offset, f.type.size);
}

var t: Trade;
while (csv_read_row(r, schema, &t) == CSV_RECORD) {
  // columns are matched by header name, in any order
}
```

Writing:

```mettle
var w: CsvWriter* = csv_writer_create("out.csv", csv_dialect());
csv_write_row(w, "name", "note");
csv_write_row(w, "Ann", "said \"hi\", then left");   // quoted and escaped for you
csv_write_i64(w, 42);
csv_write_f64(w, 0.1);
csv_write_end(w);
csv_writer_close(w);
```

See [docs/api.md](docs/api.md) for every function, [docs/design.md](docs/design.md) for how the
reader works, and [docs/references.md](docs/references.md) for the standards and libraries it
follows. The [examples](examples) folder has a streaming file checker (`csvstat`) and a typed
round trip (`trades`).

## Dialects

`csv_dialect()` is RFC 4180 with a header row, lenient repair of bad quoting, any line end
accepted and CRLF written. Change any field of the `CsvDialect` it returns, or start from a
preset: `csv_dialect_rfc4180()` (strict: bad quoting and ragged rows are errors),
`csv_dialect_python()` (reads like Python's `csv.reader`), `csv_dialect_tsv()` and
`csv_dialect_unix()`.

## Tested against

- Every `parses_to!` case in Rust's csv-core reader tests (63 cases).
- The csv-spectrum test files (12 files).
- 10,000 random inputs in random dialects, compared with Python's `csv.reader`.
- 3,000 random row sets written and compared byte for byte with Python's `csv.writer`.
- 10,000 floats: formatting equals Python's `repr`, parsing equals Python's `float`.
- A fuzz test: random and mutated input in random dialects, read from memory and as a stream
  in random chunk sizes (both must agree), written back and read again (must round-trip).
  900,000 inputs ran clean before release; the test suite runs 30,000 each time.

Every test passes in debug and release builds (`mbuild test`, `mbuild test -r`).

## Speed

`mbuild bench` reads and writes an 89 MB file of mixed rows (numbers, words, quoted text with
commas and doubled quotes). Runs alternated with the references on the same shared 16-core
x86-64 Linux machine, which was busy, so each figure is a range over several runs:

| Task | csv (Mettle) | zsv (C, SIMD) | Python 3.13 csv (C) |
|---|---|---|---|
| Read every field | 320 to 790 MB/s | 370 to 950 MB/s (`zsv count`) | 50 to 75 MB/s |
| Write | about 200 MB/s | | 40 to 44 MB/s |

On one pass over the same 5 MB under callgrind, the reader runs about 19 instructions per input
byte, against about 120 for Python's `csv.reader` loop. `tools/bench_ref.sh` runs the reference
numbers.

## License

Apache-2.0. See [LICENSE](LICENSE). Test data derived from other projects is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
