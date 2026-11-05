# Protocol Buffers

Codec reads and writes the protobuf binary wire format, compiles `.proto`
files into Mettle code, and frames messages for gRPC.

Specifications followed:

- Encoding: <https://protobuf.dev/programming-guides/encoding/>
- proto3 language: <https://protobuf.dev/reference/protobuf/proto3-spec/>
- proto2 language: <https://protobuf.dev/reference/protobuf/proto2-spec/>
- gRPC over HTTP/2: <https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md>

## The compiler: codec_protoc

```text
codec_protoc [-I DIR]... [--out DIR] [--prefix NAME] [--runtime PATH] FILE.proto...
```

File names are import paths found in the `-I` directories (default `.`), the
way protoc takes them. Every file they import is generated too. The output for
`a/b.proto` is `OUT/a/b_pb.mettle`. `--prefix` goes in front of every type
name, which keeps two packages with the same message names apart.
`--runtime` is the import prefix for Codec itself (default `codec`).

The compiler is a program, so build it from a clone of this repository
(`mbuild build -r codec_protoc` puts it in `build/linux/release/`). In a
project that depends on Codec, an `mbuild gen` command can then run it.
Generate before the first build, because your code imports the generated files.

```mettle
fn deps() {
  mb_dep("codec", "v0.1.0");
}

// gen: rebuilds src/gen from proto/*.proto.
fn cmd_gen(a: MbArgs*) -> int32 {
  var protoc: string = "../codec/build/linux/release/codec_protoc";
  return mb_sh(protoc + " -I proto --out src/gen service.proto");
}
```

Then `import "gen/service_pb";` from `src/main.mettle`.

## What is supported

- proto3 and proto2 files: messages, nested messages and enums, all scalar
  types, `repeated` (packed and unpacked, both accepted when reading),
  `map<K, V>`, `oneof`, proto3 `optional`, proto2 `required`, `[default = ...]`,
  `[packed = ...]`, groups, `reserved`, `extensions` ranges, options, `import`
  (including `public` and `weak`), fully qualified type names, services with
  streaming methods.
- Unknown fields are kept and written back unchanged.
- proto3 `string` fields must be valid UTF-8; proto2 strings are not checked,
  as the specification says.
- proto2 enums are closed: an unknown value read into a singular or unpacked
  field is kept as an unknown field. In a packed proto2 enum field it is dropped.
- `extend` blocks are parsed and ignored. Editions files are refused.
  JSON and text format are not part of Codec.

## Generated code

For `message Person` the generator writes:

| Name | What it does |
|---|---|
| `struct Person` | one field per proto field, `has_x` flags for fields with presence, `x_case` for each oneof, `_unknown` for unknown fields |
| `Person_FIELD_x` | the field number of `x` (used for oneof cases) |
| `Person_encode(m, w)` | appends `m` to a `CodecRev` |
| `Person_serialize(m, w) -> string` | empties `w`, encodes, returns the bytes (a view into `w`) |
| `Person_decode(m, data) -> int32` | merges the message in `data` into `m`; `CODEC_OK` or an error code |
| `Person_read(m, r, depth, group)` | the same from a `CodecReader`, used for nesting |
| `Person_check(m) -> bool` | every proto2 `required` field is set, nested messages included |
| `Person_init(m)` | zeroes `m` and sets proto2 default values |
| `Person_free(m)` | frees what decoding allocated (nested messages and lists) |

For an enum `Color`: a constant per value (`Color_RED`), `Color_valid(v)` and
`Color_name(v)`.

Types map like this:

| proto | Mettle |
|---|---|
| `double`, `float` | `float64`, `float32` |
| `int32`, `sint32`, `sfixed32` | `int32` |
| `int64`, `sint64`, `sfixed64` | `int64` |
| `uint32`, `fixed32` | `uint32` |
| `uint64`, `fixed64` | `uint64` |
| `bool` | `bool` |
| `string`, `bytes` | `string` |
| enum | `int32` |
| message (singular) | `T*`, 0 when absent |
| `repeated T` | `CodecList<T>` (messages by value) |
| `map<K, V>` | `CodecList<Msg_FieldEntry>` with `key` and `value` |

Nested names are joined with `_`: `Person.PhoneNumber` is `Person_PhoneNumber`.
A field named like a Mettle keyword gets a `_` at the end (`type_`, `default_`).

Decoded strings and bytes point into the input buffer, so keep it alive while
you use the message. A decoded `map` keeps the last value for a repeated key.

## Encoding

Messages are written back to front into a `CodecRev`, last field first, as upb
does. A nested message is written before its length, so the length is known
when it is written and no size pass is needed.

```mettle
var w: CodecRev;
codec_rev_init(&w, 256);
var bytes: string = Person_serialize(&p, &w);
var back: Person;
if (Person_decode(&back, bytes) != CODEC_OK) { ... }
Person_free(&back);
```

## gRPC

For each method `rpc SayHello (HelloRequest) returns (HelloReply)` in
`service Greeter`:

| Name | Side |
|---|---|
| `Greeter_SayHello_PATH` | `"/pkg.Greeter/SayHello"` |
| `Greeter_SayHello_frame_request(req, w) -> string` | client: the 5-byte prefix and the message |
| `Greeter_SayHello_read_response(msg, out) -> int32` | client: decode an `H2GrpcMessage` |
| `Greeter_SayHello_read_request(msg, req) -> int32` | server |
| `Greeter_SayHello_frame_response(resp, w) -> string` | server |

`H2GrpcMessage` and `h2_grpc_read`, which splits incoming DATA into messages,
come from the standard library's `std/http2`. Compressed messages are refused
with `CODEC_ERR_INVALID`; decompress them first. Frame each message of a stream
on its own, because `CodecRev` grows toward the front.

## Wire helpers

`codec/proto/wire` holds the functions generated code calls, usable by hand:
`codec_pb_put_int32(w, field, v)` and the like for each type,
`codec_pb_put_len(w, field, mark)` to close a length-delimited field,
`codec_pb_read_tag`, `codec_pb_read_sub`, `codec_pb_read_bytes`,
`codec_pb_skip` (groups included) and `codec_pb_keep_unknown`.

## Limits

Nesting stops at 100 levels (`CODEC_PB_DEPTH`) with `CODEC_ERR_DEPTH`, as in
the reference implementations. Lengths are checked against the bytes left
before anything is read.
