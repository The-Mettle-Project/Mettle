# MessagePack

Specification: <https://github.com/msgpack/msgpack/blob/master/spec.md>,
including extension types and the timestamp extension (type -1).

## Writing

Each call appends one value, or one array or map header, to a `CodecBuf`, in
the smallest form the specification allows. Non-negative integers use the
unsigned forms whatever their Mettle type, as msgpack-c does.

| Call | Writes |
|---|---|
| `codec_mp_nil(b)`, `codec_mp_bool(b, v)` | nil, bool |
| `codec_mp_int(b, v)`, `codec_mp_uint(b, v)` | integers |
| `codec_mp_float32(b, v)`, `codec_mp_float64(b, v)` | floats |
| `codec_mp_str(b, s)`, `codec_mp_bin(b, p, n)` | strings and binary |
| `codec_mp_str_head(b, n)`, `codec_mp_bin_head(b, n)` | headers only; write the bytes next |
| `codec_mp_array(b, n)`, `codec_mp_map(b, n)` | headers; write the n items (or n key, value pairs) next |
| `codec_mp_ext(b, type, p, n)`, `codec_mp_ext_head(b, type, n)` | extensions |
| `codec_mp_timestamp(b, sec, nsec)` | a timestamp in the 4, 8 or 12 byte form, whichever fits |

## Reading

Values are read one at a time from a `CodecReader`. Strings, binary and
extension data come back as views into the input, with no copy.

- `codec_mp_read(r, &v)` reads any value into a `CodecMpValue` (`kind`, and
  `i`, `u`, `f`, `b`, `data`, `n` or `ext_type` by kind). It returns false at
  the end or on an error; `r->err` says which.
- `codec_mp_peek(r)` gives the kind of the next value without reading it.
- Typed reads: `codec_mp_read_int`, `_uint`, `_float` (any number),
  `_bool`, `_str`, `_bin`, `_array`, `_map`, `_ext`, `_timestamp`,
  and `codec_mp_read_nil` (reads nil only if it is next). A value of
  another kind fails the reader with `CODEC_ERR_TYPE`.
- `codec_mp_skip(r, max_depth)` skips one whole value.
- `codec_mp_check(p, n, max_depth)` checks that a buffer is one well-formed value.

Errors are sticky: after the first one every read returns zero, so check
`r->err` once at the end.

## Safety

Every length is checked against the bytes left. An array or map cannot claim
more items than there are bytes left (`CODEC_ERR_LIMIT`), so a tiny input
cannot make a caller reserve huge lists. `codec_mp_skip` and `codec_mp_check`
walk nesting with an explicit stack and a depth limit (`CODEC_MP_DEPTH`, 512),
never recursion. Strings are not checked for UTF-8 by the reader; use
`codec_utf8_valid_string` where it matters.

For structs, see [derive.md](derive.md).
