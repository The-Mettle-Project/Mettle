# References

## Standards followed

| Area | Source |
|---|---|
| Wire protocol, headers, trailers, framing, timeouts | grpc/grpc doc/PROTOCOL-HTTP2.md |
| Status codes and which ones a library may produce | doc/statuscodes.md |
| Status when the HTTP response is not gRPC | doc/http-grpc-status-mapping.md |
| Compression and encoding negotiation | doc/compression.md |
| Service config JSON | doc/service_config.md, gRFC A2 |
| Retries, hedging, throttling, pushback, transparent retry | gRFC A6 |
| Client keepalive, `too_many_pings` | doc/keepalive.md, gRFC A8 |
| Server connection idle/age, ping enforcement | gRFC A9 |
| Reconnect backoff (1 s, x1.6, jitter 0.2, max 120 s) | doc/connection-backoff.md |
| Health service | doc/health-checking.md, grpc/health/v1/health.proto |
| Reflection service | doc/server-reflection.md, grpc/reflection/v1/reflection.proto |
| Rich errors | google/rpc/status.proto, google/rpc/error_details.proto |
| Protobuf JSON durations | protobuf JSON mapping (google.protobuf.Duration) |
| JSON | RFC 8259 |
| Base64 for `-bin` metadata | RFC 4648 (padded and unpadded accepted, unpadded sent) |
| HTTP/2, HPACK | RFC 9113, RFC 7541, through std/http2 and std/hpack |

## Implementations studied

Four implementations were read for behaviour and edge cases; no code was
copied from any of them.

- **grpc-go** (Apache-2.0): the retry state machine (commit on response
  headers or when the retry buffer fills, a 256 KiB per-call buffer),
  transparent retry of refused streams and streams past a GOAWAY's last
  stream id, server ping strikes and the `too_many_pings` GOAWAY, the
  4 MiB default receive limit, `grpc-timeout` written in the finest unit
  that fits in 8 digits and rounded up.
- **grpc-java** (Apache-2.0): INTERNAL for a unary request stream with no
  message or more than one ("half-closed without a request", "too many
  requests"), `deflate` meaning the zlib format, interceptors as an
  ordered list where the first wraps the rest.
- **tonic** (MIT): generated server code as one handler table per service
  with a function per method, and status details carried as
  `grpc-status-details-bin`.
- **grpc-dotnet** (Apache-2.0): hedging as parallel attempts sharing one
  send log, cancelling the losers when one commits; the call-option /
  service-config precedence for wait-for-ready and deadlines.

## Choices

- **Callback server, blocking client.** A server must hold many long-lived
  streams on one thread, so handlers are callbacks and sends never block
  (queued under flow control). A client usually waits for its answer, so
  its calls block by running the channel's event loop, which still lets
  many calls share one channel.
- **The transport is a table of functions.** The call layer knows HTTP/2
  only as streams and events, so it runs unchanged over std/http2 (TCP or
  mtls) and over the in-memory pair used by the tests.
- **Stubs from our own generator on Codec's parser.** Codec already parses
  `.proto` files and generates messages; grpc_gen reuses that and adds the
  stubs and reflection descriptors, so gRPC changes need no Codec release.
- **Decompression is bounded while it runs** (Press's output limit), not
  checked afterwards, so a compressed bomb cannot exhaust memory.

## Testing against references

The FileDescriptorProtos grpc_gen embeds for reflection were checked with
Go's `protodesc.NewFiles` (google.golang.org/protobuf v1.36.11), which
resolves every type reference. The interop cases of
doc/interop-test-descriptions.md pass in both directions against grpc-go
v1.80 (docs/interop.md). `mbuild bench` compares with grpc-go's benchmain.
