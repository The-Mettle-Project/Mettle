# Testing, conformance and speed

## Tests

`mbuild test` runs every test program in debug; `mbuild test -r` in release.
Both must pass.

| Test | What it checks |
|---|---|
| `core` | varints, zigzag, fixed-width, reader bounds, both buffers |
| `msgpack` | all 233 encodings of the msgpack-test-suite, decoded and re-encoded in the smallest form; errors; depth |
| `json` | escaping, UTF-8 handling, numbers, nesting, pretty output, misuse |
| `proto` | generated code: known bytes, round trips, merging, oneofs, unknown fields, maps, bad input, depth, gRPC framing |
| `cbor` | the RFC 8949 Appendix A examples, decoded and re-encoded; errors; depth |
| `derive` | `@codec_derive` round trips, key order, ranges, depth |
| `fuzz` | every decoder, as below |

`mbuild gen` rebuilds the generated code in `tests/gen` and the example.

## Protobuf conformance

`mbuild conformance` runs Google's conformance suite against
`tests/conformance/testee.mettle`. It needs a protobuf source tree with the
runner built:

```bash
mkdir -p ~/.cache/codec-conformance && cd ~/.cache/codec-conformance
git clone --depth 1 --branch v29.3 --recurse-submodules --shallow-submodules https://github.com/protocolbuffers/protobuf.git src
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_CONFORMANCE=ON -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_INSTALL=OFF
ninja -C build conformance_test_runner protoc
```

Set `PROTOBUF_DIR` to use another place. Result with protobuf 29.3, debug and
release (`-r`):

```text
CONFORMANCE SUITE PASSED: 1368 successes, 1307 skipped, 0 expected failures, 0 unexpected failures.
```

Every binary test passes, with `--enforce_recommended`. The skipped tests are
JSON and text format input or output, which Codec does not parse.

## Fuzzing

`tests/fuzz_test.mettle` feeds mutated and random input to the MessagePack
reader and typed reads, `@codec_derive` reads, generated protobuf decoding,
the CBOR reader, the varint reader, the `.proto` parser and the JSON writer.
Each input is copied so its last byte sits just before a page with no access,
so reading even one byte past the end crashes the test. What a decoder accepts
must re-encode to something it accepts again, and for protobuf, encode the
same way twice. `mbuild test` runs 3,000 rounds per target; `mbuild fuzz N`
runs N in release. 2,000,000 rounds per target ran clean.

The float formatter is checked separately: `tools/float_check.py` sends a
million random float64 and float32 values through `floatdump` and reads each
result back in Python. None failed to round-trip.

## Speed

`mbuild build -r && build/linux/release/codec_bench`. On an x86-64 Linux
machine (one core, mettle v0.18.0):

| Test | Codec | Reference |
|---|---|---|
| protobuf encode, 199-byte message | 1535 MB/s, 123 ns | C++ protobuf 29.3, `-O2`: 1340 MB/s, 142 ns |
| protobuf decode, same message | 1084 MB/s, 174 ns | C++ protobuf 29.3, `-O2`: 843 MB/s, 225 ns |
| MessagePack encode, 89-byte struct | 945 MB/s, 89 ns | Python msgpack (C extension): 394 ns |
| MessagePack decode, same struct | 846 MB/s, 100 ns | Python msgpack (C extension): 386 ns |
| JSON write, 114-byte object | 482 MB/s, 225 ns | Python json (C encoder): 1851 ns |
| JSON float64 | 103 ns per value | |
| JSON string escaping, 1 MiB | 1675 MB/s | |

Notes on fairness: C++ protobuf copies strings into `std::string` when it
decodes, while Codec's decoded strings point into the input. The Python
numbers include the cost of calling from Python and are there for scale only.
To reproduce the C++ numbers, generate C++ code for `tests/proto/basic.proto`
with the `protoc` built above and compile `tools/cpp_bench.cc` against
`libprotobuf.a` and the Abseil libraries in the same build directory.
`tools/bench_ref.py` gives the Python numbers.
