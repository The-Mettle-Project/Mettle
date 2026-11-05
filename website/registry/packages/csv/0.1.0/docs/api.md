# API

```mettle
import "csv/csv";
```

Every exported name starts with `csv_`, `Csv` or `CSV_`. Fields are byte strings: the library
never changes their encoding. Set `check_utf8` in the dialect to have the reader check UTF-8.

## Dialects

```mettle
struct CsvDialect {
  delimiter: uint8;            // ',' by default
  quote: uint8;                // '"' by default; 0 turns quoting off
  escape: uint8;               // 0 for none, often '\\'
  double_quote: bool;          // "" inside quotes is one quote (true)
  comment: uint8;              // records starting with this byte are skipped; 0 for none
  terminator: uint8;           // CSV_TERM_ANY (CR, LF or CRLF) or one byte
  skip_initial_space: bool;    // drop spaces after a delimiter (false)
  trim: int32;                 // CSV_TRIM_NONE / _HEADERS / _FIELDS / _ALL
  strict: bool;                // report bad quoting instead of repairing it (false)
  flexible: bool;              // allow records of different lengths (false)
  has_header: bool;            // first record is a header (true)
  blank_lines: int32;          // CSV_BLANK_SKIP or CSV_BLANK_EMPTY
  keep_bom: bool;              // keep a leading UTF-8 BOM (false: it is skipped)
  check_utf8: bool;            // report fields that are not UTF-8 (false)
  max_record: int64;           // largest record in bytes, 0 for no limit
  quote_style: int32;          // writer: CSV_QUOTE_MINIMAL / _ALL / _NON_NUMERIC / _NEVER
  line_end: int32;             // writer: CSV_EOL_CRLF / _LF / _CR
}
```

| Function | Gives |
|---|---|
| `csv_dialect()` | The default above |
| `csv_dialect_rfc4180()` | The default, strict |
| `csv_dialect_python()` | Reads like Python's `csv.reader`: no header, ragged rows allowed, empty lines become empty records |
| `csv_dialect_tsv()` | Tab delimiter |
| `csv_dialect_unix()` | Writes like Python's `csv.unix_dialect`: LF line ends, every field quoted |

How bytes are read (Python's rules, which Rust's csv-core shares where both have the feature):

- A field is quoted only when the quote byte is its first byte (after skipped spaces).
- Inside quotes, a doubled quote is one quote (`double_quote`), the escape byte takes the next
  byte as it is, and delimiters and line breaks are data.
- Outside quotes, the escape byte takes the next byte as it is.
- With `CSV_TERM_ANY`, CR, LF and CRLF each end a record. Lines are counted the same way.
- A comment byte only counts as the first byte of a record.
- Lenient repair (the default): a quote inside an unquoted field is kept as data; after a
  closing quote, more bytes before the delimiter are kept as data and the field goes on as
  unquoted (with `double_quote` on, the byte right after the quote is taken as it is, even an
  escape byte); a quoted field still open at the
  end of input ends there.
- Strict mode reports each of those as an error instead, plus an escape byte at the end of
  input.

## Reading

| Function | Does |
|---|---|
| `csv_reader_from_string(data, d) -> CsvReader*` | Reads bytes in memory. No copy: views point into `data` |
| `csv_reader_open(path, d) -> CsvReader*` | Opens and streams a file; 0 if it cannot be opened |
| `csv_reader_from_fd(fd, d) -> CsvReader*` | Streams an open descriptor from offset 0 |
| `csv_reader_from_fn(read, d) -> CsvReader*` | Streams from `read(buf, max) -> int64` (bytes read, 0 at end, negative on failure) |
| `csv_reader_set_buffer(r, size)` | Starting buffer size for a stream (64 KiB); it grows to fit the longest record |
| `csv_read(r, &rec) -> int32` | Next record: `CSV_RECORD`, `CSV_END` or `CSV_ERROR` |
| `csv_reader_headers(r) -> CsvRecord*` | The header (read on first use), or 0 if the dialect has none |
| `csv_reader_error(r) -> CsvError` | The last error |
| `csv_reader_line(r)`, `csv_reader_offset(r)` | Line and byte offset where the next record starts |
| `csv_reader_count(r)` | Records read so far, header included |
| `csv_reader_free(r)` | Frees the reader (and closes a file it opened) |

The header is read and kept by the reader; `csv_read` returns only data records.

```mettle
struct CsvRecord {
  fields: string*;   // views
  n: int32;          // field count
  line: int64;       // line where the record starts, from 1
  byte: int64;       // byte offset where it starts
  index: int64;      // record number from 1, the header counted
  owned: uint8*;
}
```

Field views stay valid until the next `csv_read` on the same reader. Keep a record longer with
`csv_record_clone(&rec)` (one allocation) and free it with `csv_record_free(&copy)`.
`csv_record_get(&rec, i)` returns field `i` or `""`; `csv_record_find(&rec, name)` returns the
index of the first field equal to `name`, or -1.

## Errors

```mettle
struct CsvError {
  kind: int32;       // CSV_OK or CSV_ERR_*
  byte: int64;       // offset in the input
  line: int64;       // from 1
  column: int64;     // from 1, in bytes
  record: int64;     // from 1
  field: int32;      // from 1, 0 when the error is about the whole record
  expected: int32;   // CSV_ERR_FIELD_COUNT
  got: int32;        // CSV_ERR_FIELD_COUNT
}
```

| Kind | Meaning | Reading goes on? |
|---|---|---|
| `CSV_ERR_QUOTE` | quote inside an unquoted field (strict) | no |
| `CSV_ERR_AFTER_QUOTE` | data after a closing quote (strict) | no |
| `CSV_ERR_UNCLOSED` | quoted field not closed (strict); points at the opening quote | no |
| `CSV_ERR_ESCAPE` | escape byte at the end of input (strict) | no |
| `CSV_ERR_FIELD_COUNT` | record length differs from the first record (unless `flexible`) | yes, `rec` is filled |
| `CSV_ERR_UTF8` | field is not UTF-8 (`check_utf8`) | yes, `rec` is filled |
| `CSV_ERR_TOO_LARGE` | record longer than `max_record` | no |
| `CSV_ERR_IO` | read or write failed, or out of memory | no |
| `CSV_ERR_PARSE` | typed rows: a field does not hold the wanted type | yes |
| `CSV_ERR_MISSING` | typed rows: no such column | yes |

`csv_error_text(kind)` gives a short description, and `csv_error_format(e, buf, cap)` writes a
line such as `line 2, column 4 (record 2, field 2): quote inside an unquoted field`.

## Writing

| Function | Does |
|---|---|
| `csv_writer_new(d)` | Writes into memory; `csv_writer_bytes(w)` returns all of it |
| `csv_writer_create(path, d)` | Creates or empties a file; 0 on failure |
| `csv_writer_to_fd(fd, d)` | Writes to an open descriptor from offset 0 |
| `csv_writer_to_fn(write, d)` | Hands bytes to `write(buf, n) -> int64` (n, or negative on failure) |
| `csv_write_field(w, s)` | One field of the current record |
| `csv_write_i64`, `_u64`, `_f64`, `_f32`, `_bool` | Typed fields; floats use the shortest exact text |
| `csv_write_number_text(w, s)` | A field that is already number text (not quoted by `CSV_QUOTE_NON_NUMERIC`) |
| `csv_write_end(w)` | Ends the record |
| `csv_write_row(w, "a", "b", ...)` | A whole record |
| `csv_write_record(w, &rec)` | A record from a reader |
| `csv_writer_flush(w)` | Sends buffered bytes to the file or function |
| `csv_writer_close(w) -> bool` | Flushes, closes a file it created, frees; false if anything failed |
| `csv_writer_free(w)` | Frees without flushing |
| `csv_writer_error(w)` | The last error |

With `CSV_QUOTE_MINIMAL` a field is quoted when it holds the delimiter, the quote, the escape
byte, CR, LF or the terminator, when it is the first field and starts with the comment byte,
when it starts with a space and `skip_initial_space` is on, and when it is the only field of a
record and empty (written `""`, so it does not read back as a blank line). Quotes inside are
doubled, or escaped when `double_quote` is off and an escape byte is set. With quoting off
(`quote` = 0 or `CSV_QUOTE_NEVER`) the escape byte, if any, goes before each special byte.

## Numbers

| Function | Does |
|---|---|
| `csv_parse_i64(s, &v)`, `csv_parse_u64(s, &v)` | Decimal integers with an optional sign; false on overflow |
| `csv_parse_f64(s, &v)`, `csv_parse_f32(s, &v)` | Decimal floats, correctly rounded; also `inf`, `infinity`, `nan` in any case |
| `csv_parse_bool(s, &v)` | `true`/`false` in any case, `1`/`0` |
| `csv_format_f64(v, buf)`, `csv_format_f32(v, buf)` | Shortest text that reads back to `v` (needs 32 bytes); returns the length |

Parsers take the whole string: no spaces, no trailing bytes. Float text looks like Python's
`repr`: `1.0`, `0.0001`, `1e-05`, `1.5e+300`, `inf`, `nan`.

## Typed rows

```mettle
var s: CsvSchema* = csv_schema_new();
comptime for f in typeof(Row).fields {
  csv_schema_add(s, f.name, (int32)f.type.kind, f.offset, f.type.size);
}
```

Supported field types: `bool`, `int8` to `int64`, `uint8` to `uint64`, `float32`, `float64`,
`string`. Fields of other types are skipped.

| Function | Does |
|---|---|
| `csv_read_row(r, s, &row) -> int32` | Reads and decodes the next record. Binds the schema to the header on first use (by position without a header) |
| `csv_schema_bind(s, header)` | Binds by header name (0 for by position); `CSV_ERR_MISSING` if a column is absent |
| `csv_decode(s, &rec, &row) -> CsvError` | Decodes one record |
| `csv_schema_rename(s, field, column)` | Reads `field` from a column with another name |
| `csv_write_header(w, s)`, `csv_write_struct(w, s, &row)` | Writes field names, then structs |
| `csv_schema_free(s)` | Frees the schema |

Schema settings: `s->copy_strings = true` makes decoded strings malloc'd copies (free them with
`csv_decode_free(s, &row)`); by default they are views valid until the next read.
`s->empty_zero = true` decodes an empty field as 0 or false instead of failing.
