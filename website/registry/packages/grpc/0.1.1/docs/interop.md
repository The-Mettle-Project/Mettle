# Interop

`tools/interop` holds the gRPC interop test server and client
(doc/interop-test-descriptions.md), on the generated `grpc.testing` stubs.

```bash
mbuild build interop_server interop_client
build/linux/debug/interop_server --port=50051
build/linux/debug/interop_client --server_host=127.0.0.1 --server_port=50051 --test_case=large_unary
```

`--probe=false` skips the first step of the client_compressed cases, for
servers that do not check `expect_compressed`.

## Results against grpc-go v1.80.0 (2026-11-05)

grpc-go's interop client and server come from
`google.golang.org/grpc/interop/client` and `.../interop/server`.

| Case | grpc-go client, Mettle server | Mettle client, grpc-go server |
|---|---|---|
| empty_unary, large_unary | pass | pass |
| client_streaming, server_streaming | pass | pass |
| ping_pong, empty_stream | pass | pass |
| timeout_on_sleeping_server | pass | pass |
| cancel_after_begin, cancel_after_first_response | pass | pass |
| status_code_and_message, special_status_message | pass | pass |
| custom_metadata | pass | pass |
| unimplemented_method, unimplemented_service | pass | pass |
| client_compressed_unary, client_compressed_streaming | see below | pass (1) |
| server_compressed_unary, server_compressed_streaming | see below | pass |

(1) grpc-go's stock interop server has no gzip decompressor and ignores
`expect_compressed`, so these ran against the same test service with
`encoding/gzip` imported, with `--probe=false`.

grpc-go's interop client does not offer the compressed cases, so a small
grpc-go client covered them against the Mettle server: a gzip-compressed
unary request with a compressed 314159-byte response, an uncompressed
request flagged `expect_compressed` refused with INVALID_ARGUMENT, and a
gzip-compressed client stream. All pass.
