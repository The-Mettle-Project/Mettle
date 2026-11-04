# json

A pull reader. You ask for the next event and read the token it stands on.
Nothing is allocated and the document is never copied.

```mettle
import "json";

fn port_of(document: string) -> int64 {
  var reader: JsonReader;
  json_begin(&reader, document);
  var event: int32 = json_next(&reader);
  while (event != JSON_END && event != JSON_ERROR) {
    if (event == JSON_KEY && json_text_is(&reader, "port")) {
      var port: int64 = 0;
      json_next(&reader);
      json_read_int64(&reader, &port);
      return port;
    }
    event = json_next(&reader);
  }
  return -1;
}
```

## Events

`json_next` returns one of `JSON_OBJECT_START`, `JSON_OBJECT_END`,
`JSON_ARRAY_START`, `JSON_ARRAY_END`, `JSON_KEY`, `JSON_STRING`, `JSON_NUMBER`,
`JSON_TRUE`, `JSON_FALSE`, `JSON_NULL`, `JSON_END` or `JSON_ERROR`. After an
error it keeps returning `JSON_ERROR`, and `json_error_offset` gives the byte
where the document went wrong.

## Reading a token

- `json_string_length` and `json_string_copy` decode escapes, including
  `\uXXXX` and surrogate pairs, into UTF-8.
- `json_text_is` compares a key or string against text, escapes decoded.
- `json_read_int64` refuses a number with a fraction, an exponent or a value
  outside the range of `int64`.
- `json_read_float64` is correctly rounded for every input. It takes a fast path
  when the digits and exponent allow one, and falls back to exact big integer
  arithmetic.
- `json_skip` jumps past the object or array the reader stands on.

## Limits

Nesting stops at `JSON_MAX_DEPTH`, 128 levels. The reader checks the grammar and
does not check that the bytes in a string are valid UTF-8.
