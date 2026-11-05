# Codec

Serialization for [Mettle](https://github.com/The-Mettle-Project/Mettle), in pure Mettle.

| Format | What you get | Checked against |
|---|---|---|
| Protocol Buffers | a `.proto` compiler (proto2 and proto3), generated encode and decode, gRPC framing on `std/http2` | Google's conformance suite: all 1368 binary tests pass |
| MessagePack | the full spec, extension types and timestamps | the msgpack-test-suite |
| JSON | a streaming writer with exact, shortest-form numbers | RFC 8259; a million-value float round trip |
| CBOR | reader and writer, preferred serialization, half floats | the RFC 8949 Appendix A examples |
| Structs | `@codec_derive`: MessagePack and JSON for your own structs, written by the compiler | round-trip rule checked at build time |

Every decoder checks lengths before it reads, limits nesting, and is fuzzed
with inputs placed against a no-access page. Strings and bytes are read as
views into the input, with no copy. Protobuf encoding and decoding run faster
than C++ protobuf on the benchmark message ([docs/testing.md](docs/testing.md)).

## Use it

In your `mbuild.mettle`:

```mettle
fn deps() {
  mb_dep("codec", "v0.1.0");
}
```

`import "codec/codec";` brings in everything except `@codec_derive`
(`import "codec/derive";`) and the `.proto` compiler. Import one part, such as
`"codec/msgpack/read"`, to keep builds small.

### Protocol Buffers

```text
codec_protoc -I proto --out src/gen greeter.proto
```

```mettle
var w: CodecRev;
codec_rev_init(&w, 256);
var req: HelloRequest;
req.name = "Ada";
var bytes: string = HelloRequest_serialize(&req, &w);

var back: HelloRequest;
if (HelloRequest_decode(&back, bytes) != CODEC_OK) { ... }
```

[docs/protobuf.md](docs/protobuf.md) shows how to run the compiler through
`mbuild gen`, the generated API and gRPC.

### MessagePack and JSON from a struct

```mettle
import "codec/codec";
import "codec/derive";

@codec_derive struct Job {
  id: uint64;
  name: string;
  tags: CodecList<string>;
}

var b: CodecBuf;
codec_buf_init(&b, 256);
Job_mp_write(&job, &b);

var r: CodecReader;
codec_rd_init(&r, b.data, b.len);
var back: Job;
if (!Job_mp_read(&back, &r)) { println(codec_error_name(r.err)); }
```

### JSON

```mettle
var w: CodecJson;
codec_json_init(&w, &b);
codec_json_object(&w);
codec_json_field_string(&w, "status", "ok");
codec_json_field_float(&w, "load", 0.37);
codec_json_end(&w);
if (codec_json_finish(&w) == CODEC_OK) { println(codec_buf_view(&b)); }
```

### CBOR

```mettle
codec_cbor_map(&b, 1);
codec_cbor_int(&b, 3);      // alg
codec_cbor_int(&b, -7);     // ES256
```

## Errors

Readers and writers keep the first error and ignore later calls, so you check
once at the end: `r.err`, `b.err` or `codec_json_finish`. The codes are in
`src/core/status.mettle`; `codec_error_name(code)` gives a short name.

## Docs

- [Protocol Buffers and gRPC](docs/protobuf.md)
- [MessagePack](docs/msgpack.md)
- [JSON writer](docs/json.md)
- [CBOR](docs/cbor.md)
- [@codec_derive](docs/derive.md)
- [Testing, conformance and speed](docs/testing.md)
- [Specifications and references](docs/references.md)

## Develop

```bash
mbuild test            # debug
mbuild test -r         # release
mbuild gen             # rebuild generated protobuf code for tests and examples
mbuild fuzz 1000000    # a long fuzzing run
mbuild conformance     # Google's protobuf conformance suite (see docs/testing.md)
```

Examples are in `examples/` (`mbuild run example_protobuf`, `example_msgpack`,
`example_json`, `example_cbor`).

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
