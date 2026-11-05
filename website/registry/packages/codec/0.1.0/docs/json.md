# JSON writer

Codec writes JSON (RFC 8259, <https://www.rfc-editor.org/rfc/rfc8259>). The
registry `json` package reads it; the two are meant to be used together.

```mettle
var b: CodecBuf;
codec_buf_init(&b, 256);
var w: CodecJson;
codec_json_init(&w, &b);
codec_json_object(&w);
codec_json_field_int(&w, "id", 7);
codec_json_key(&w, "tags");
codec_json_array(&w);
codec_json_string(&w, "a");
codec_json_end(&w);
codec_json_end(&w);
if (codec_json_finish(&w) == CODEC_OK) { println(codec_buf_view(&b)); }   // {"id":7,"tags":["a"]}
```

| Call | Writes |
|---|---|
| `codec_json_object(w)`, `codec_json_array(w)`, `codec_json_end(w)` | brackets |
| `codec_json_key(w, k)` | a member name; its value goes next |
| `codec_json_string`, `_int`, `_uint`, `_float`, `_float32`, `_bool`, `_null` | values |
| `codec_json_bytes_as_string(w, p, n)` | a string from raw bytes |
| `codec_json_raw(w, text)` | text that is already JSON, unchecked |
| `codec_json_field_string(w, k, v)` and the other `field_` calls | a key and a value |
| `codec_json_pretty(w, spaces)` | indented output |
| `codec_json_finish(w)` | `CODEC_OK`, or the first error |

Commas, colons and indentation are put in for you. Mistakes, such as a value
where a key belongs, a key in an array, an unclosed object or a second
top-level value, give `CODEC_ERR_STATE`. Nesting stops at 256 levels.

## Strings

Quotes, backslashes and control characters are escaped (`\n`, `\t`, ... or
`\u00XX`). The text is checked as UTF-8: invalid bytes give `CODEC_ERR_UTF8`.
Flags for `codec_json_init_flags`:

| Flag | Effect |
|---|---|
| `CODEC_JSON_ASCII` | write non-ASCII as `\uXXXX` (with surrogate pairs) |
| `CODEC_JSON_LOSSY_UTF8` | replace invalid UTF-8 with U+FFFD instead of failing |
| `CODEC_JSON_ESCAPE_SLASH` | write `/` as `\/`, safe inside an HTML `<script>` |
| `CODEC_JSON_NAN_ERROR` | NaN and infinity are an error instead of `null` |

Plain runs are found 8 bytes at a time and copied in one go.

## Numbers

Integers are exact over the whole `int64` and `uint64` range. Floats use
Grisu2 (Florian Loitsch, "Printing Floating-Point Numbers Quickly and
Accurately with Integers", PLDI 2010), as RapidJSON does: the digits always
read back as the same float, and they are the shortest such digits for all
but about 0.1% of values, which get one digit more. `codec_json_float32`
does the same for `float32`. The layout follows JavaScript: `1`, `0.5`,
`1e+21`, `1.5e-7`. `-0.0` is written as `-0`. NaN and the infinities have no
JSON form and are written as `null` unless `CODEC_JSON_NAN_ERROR` is set.

`codec_format_f64`, `codec_format_f32`, `codec_format_i64` and
`codec_format_u64` write the same text into a plain buffer.

## Streaming

To send output in pieces, take `codec_buf_view(&b)`, send it, and set
`b.len = 0` before writing more; the writer keeps its nesting state.
A buffer from `codec_buf_fixed` never allocates: running out of room gives
`CODEC_ERR_LIMIT`.
