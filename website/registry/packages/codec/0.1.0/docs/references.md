# References

Specifications Codec follows, and the implementations whose techniques it
uses. No code was copied from any of them; the ideas below were rewritten in
Mettle. All of them use permissive licenses (none is GPL).

## Specifications

| Format | Specification |
|---|---|
| Protocol Buffers | Encoding: <https://protobuf.dev/programming-guides/encoding/>; language: the proto2 and proto3 specs at <https://protobuf.dev/reference/protobuf/> |
| gRPC framing | <https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md> |
| MessagePack | <https://github.com/msgpack/msgpack/blob/master/spec.md> |
| JSON | RFC 8259, <https://www.rfc-editor.org/rfc/rfc8259> |
| CBOR | RFC 8949, <https://www.rfc-editor.org/rfc/rfc8949> |
| UTF-8 | RFC 3629, <https://www.rfc-editor.org/rfc/rfc3629> |

## Implementations studied

| Implementation | License | What Codec took from it |
|---|---|---|
| upb (<https://github.com/protocolbuffers/protobuf/tree/main/upb>) | BSD-3-Clause | Encoding back to front, so a nested message's length is known when it is written and no size pass is needed (`CodecRev`). The default nesting limit of 100. Keeping unknown fields as raw spans of the input. |
| protobuf-c (<https://github.com/protobuf-c/protobuf-c>) | BSD-2-Clause | The shape of generated code: plain structs, `has_` flags for fields with presence, a case field per oneof, a free function per message. |
| protobuf C++ (<https://github.com/protocolbuffers/protobuf>) | BSD-3-Clause | Merge semantics on decode, last-value-wins for repeated map keys, accepting packed and unpacked repeated fields alike, closed proto2 enums. Its conformance suite is how Codec is checked. |
| msgpack-c (<https://github.com/msgpack/msgpack-c>) | BSL-1.0 | Picking the smallest form for each value, and writing non-negative integers in the unsigned forms. |
| rmp, Rust MessagePack (<https://github.com/3Hren/msgpack-rust>) | MIT | Typed reads that fail on the wrong kind, and a peek that does not consume. |
| RapidJSON (<https://github.com/Tencent/rapidjson>) | MIT | Grisu2 with a table of cached powers of ten, the boundary rounding step, and laying digits out the way JavaScript does. |
| yyjson (<https://github.com/ibireme/yyjson>) and simdjson (<https://github.com/simdjson/simdjson>) | MIT, Apache-2.0 | Finding the bytes that need escaping 8 at a time with word-wide bit tricks (SWAR), and printing integers two digits at a time from a table. |
| TinyCBOR (<https://github.com/intel/tinycbor>) | MIT | A pull reader over item heads with indefinite-length items closed by BREAK, and validation as a separate pass. |

## Papers

- Florian Loitsch, "Printing Floating-Point Numbers Quickly and Accurately
  with Integers", PLDI 2010. Grisu2, used by the JSON writer. Ryu (Ulf Adams,
  PLDI 2018) is always shortest but needs much larger tables; Grisu2's digits
  always read back exactly and are one digit longer for about 0.1% of values,
  which is the trade RapidJSON also makes.

## Test data

- msgpack-test-suite by Yusuke Kawasaki (<https://github.com/kawanet/msgpack-test-suite>, MIT),
  in `tests/data/msgpack-test-suite/` with its license.
- RFC 8949 Appendix A examples, as hex in `tests/data/cbor_vectors.txt`.
- Google's protobuf conformance suite, run from a protobuf source tree by
  `mbuild conformance` (not stored here).
