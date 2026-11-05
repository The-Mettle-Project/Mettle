# @codec_derive: structs to MessagePack and JSON

The compiler writes the mapping code from the struct's own fields; there is
no separate generator to run.

```mettle
import "codec/codec";
import "codec/derive";

@codec_derive struct Config {
  name: string;
  port: int32;
  tags: CodecList<string>;
}
```

gives

| Function | |
|---|---|
| `Config_mp_write(v: Config*, b: CodecBuf*)` | a MessagePack map from field name to value |
| `Config_mp_read(v: Config*, r: CodecReader*) -> bool` | keys in any order, unknown keys skipped, missing keys left as they are |
| `Config_json_write(v: Config*, w: CodecJson*)` | a JSON object, one member per field |

Field types: `int8` to `int64`, `uint8` to `uint64`, `float32`, `float64`,
`bool`, `char`, `string`, another `@codec_derive` struct, a pointer to one
(nil when 0), and `CodecList<T>` or `T[N]` of any of these. Any other field
type stops the build with a message naming the field.

Reading checks that integers fit the field's type (`CODEC_ERR_TYPE`
otherwise), limits nesting through pointers to `CODEC_MP_DEPTH`, and reads
strings as views into the input. Read into a zeroed value: lists are appended
to. Values read into pointer fields and lists are allocated with `malloc`.

Each struct also gets a build-time rule that writes a zero value and reads it
back, so a mapping that does not round-trip stops the build.
