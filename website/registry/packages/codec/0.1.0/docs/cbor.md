# CBOR

Specification: RFC 8949, <https://www.rfc-editor.org/rfc/rfc8949>. WebAuthn
and COSE (RFC 9052) data is CBOR.

## Writing

The writer uses the preferred serialization (RFC 8949 section 4.1): every
head in its shortest form, and `codec_cbor_float` picks the shortest of half,
single and double precision that keeps the value. NaN is written as
`0xf97e00`.

| Call | Writes |
|---|---|
| `codec_cbor_uint`, `codec_cbor_int`, `codec_cbor_nint(b, m)` | integers (`nint` is -1 - m, for values below int64) |
| `codec_cbor_bytes(b, p, n)`, `codec_cbor_text(b, s)` | strings |
| `codec_cbor_array(b, n)`, `codec_cbor_map(b, n)` | headers; items follow |
| `codec_cbor_tag(b, n)` | a tag; the tagged item follows |
| `codec_cbor_begin(b, major)`, `codec_cbor_break(b)` | indefinite-length items |
| `codec_cbor_bool`, `_null`, `_undefined`, `_simple` | simple values |
| `codec_cbor_float(b, f)`, `codec_cbor_float32`, `codec_cbor_float64` | floats: shortest, or a fixed width |

## Reading

`codec_cbor_read(r, &v)` reads one item head into a `CodecCborValue`. Arrays
and maps give a count (`n = -1` for indefinite length, closed by a `BREAK`
item); a tag gives its number in `u` and the tagged item comes next; byte and
text strings come back as views into the input. Text is checked as UTF-8.

Typed reads: `codec_cbor_read_int`, `_uint`, `_float`, `_bool`, `_bytes`,
`_text`, `_array`, `_map`. `codec_cbor_skip(r, max_depth)` skips one item and
`codec_cbor_check(p, n, max_depth)` checks that a buffer is exactly one
well-formed item: chunks of an indefinite string must be definite strings of
the same type, `BREAK` may only end an indefinite item, reserved values
(additional information 28 to 30, simple values 24 to 31 in two bytes) are
refused. Nesting, tags included, stops at `CODEC_CBOR_DEPTH` (512).
