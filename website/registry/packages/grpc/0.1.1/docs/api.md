# API

Every export starts with `grpc_`, `Grpc` or `GRPC_`. `import "grpc/grpc"`
brings in all of it; single modules (`grpc/status`, `grpc/client`...) work too.

## Memory

The library allocates from `GrpcArena`, a bump allocator on malloc:
`grpc_arena_new()`, `grpc_arena_alloc(a, n)`, `grpc_arena_reset(a)`,
`grpc_arena_free(a)`. Functions that copy strings for you (metadata,
service configs, JSON) take one. Each call has its own, freed with the call.

## Status

`GRPC_OK` .. `GRPC_UNAUTHENTICATED` are the 17 codes of doc/statuscodes.md.
`GrpcStatus { code, message, details }`; `details` holds the raw bytes of a
`google.rpc.Status` (see Rich errors).

| Function | |
|---|---|
| `grpc_status(code, message)` | make one |
| `grpc_code_name(code)`, `grpc_code_parse(name)` | `"UNAVAILABLE"` and back |
| `grpc_code_from_http(status)` | for a response without `grpc-status` |
| `grpc_status_free(&st)` | free the strings of a status that owns them (from `grpc_unary`, unary stubs, `grpc_status_rich`) |
| `GRPC_PENDING` | a generated handler returns it to finish the call later |

## Metadata

`GrpcMetadata` is a list of key/value pairs in an arena. Keys are
lowercase `[0-9a-z_.-]`; keys ending `-bin` hold any bytes (base64 on the
wire, raw in the API); other values are printable ASCII. `grpc-*` keys,
pseudo-headers and HTTP/2 connection headers are reserved
(`GRPC_MD_E_RESERVED`).

`grpc_md_add`, `grpc_md_set`, `grpc_md_get`, `grpc_md_get_nth`,
`grpc_md_count`, `grpc_md_remove`, `grpc_md_append`.

On a call: `grpc_call_md(c)` is what you send (client: request metadata;
server: response headers), `grpc_call_trailer_md(c)` the server's
trailers, `grpc_call_headers_md(c)` what came in (client: response
headers; server: request metadata), `grpc_call_trailers_md(c)` the
client's view of the trailers.

## Deadlines and cancellation

Times are monotonic nanoseconds (`grpc_now_ns`). `GrpcCallOptions.timeout_ns`
or `.deadline` sets a call's deadline; the server sees it as `grpc-timeout`
and gets the same deadline in `grpc_call_context(c)->deadline`. Pass a
server call's context as `GrpcCallOptions.parent` to give an outgoing call
the same deadline and cancel it when the incoming one is cancelled.

`grpc_ctx_err(ctx)` is `GRPC_OK`, `GRPC_CANCELLED` or
`GRPC_DEADLINE_EXCEEDED`. `grpc_timeout_encode` / `grpc_timeout_decode`
handle the header form.

## Client

```mettle
var ch: GrpcChannel;
var copts: GrpcChannelOptions;          // zero is a fine start
grpc_channel_init(&ch, transport, &copts);
```

`GrpcChannelOptions`: `authority`, `scheme`, `user_agent`, `max_send`,
`max_recv` (default 4 MiB), `send_enc` (`GRPC_ENC_GZIP`...),
`retry_buffer` (256 KiB per call), `keepalive_time_ns`,
`keepalive_timeout_ns`, `keepalive_without_calls`, `service_config`,
`interceptors` + `ninterceptors`, and `connect` + `connect_user` to make a
new transport after the old one closes (with the backoff of
doc/connection-backoff.md).

A channel belongs to one thread. Its calls block by running the channel's
event loop, so many calls can be open at once.

| Function | |
|---|---|
| `grpc_call_options()` | defaults: no deadline, wait_for_ready from config |
| `grpc_call_start(ch, path, kind, &o) -> GrpcCall*` | always returns a call; if it could not start it is already done |
| `grpc_call_send(c, bytes)` | one message; returns `GRPC_OK` or the call's final code |
| `grpc_call_close_send(c)` | half-close |
| `grpc_call_wait_headers(c)` | wait for response headers |
| `grpc_call_recv(c, &bytes) -> bool` | next message, valid until the next recv; false at the end |
| `grpc_call_finish(c) -> GrpcStatus` | wait for the status |
| `grpc_call_cancel(c)` | RST_STREAM CANCEL, status CANCELLED |
| `grpc_call_free(c)` | cancels if still running |
| `grpc_unary(ch, path, req, &out, &o)` | the whole unary call; `out.data` is malloc'd |
| `grpc_channel_poll(ch)` | handle what is ready, for outside event loops |
| `grpc_channel_close(ch)` | fails open calls with UNAVAILABLE |

`GrpcCallOptions.md` is request metadata, copied when the call starts;
it is how to send metadata with `grpc_unary` and the unary stubs.
`GrpcCallOptions.compress` compresses requests with the channel's
`send_enc`. Responses are decompressed whatever the server picks.

### Retries and hedging

Set `copts.service_config` to a parsed config:

```mettle
var cfg: GrpcServiceConfig;
var err: string = grpc_svc_config_parse(grpc_arena_new(), json_text, &cfg);   // "" when fine
```

The JSON is the one of doc/service_config.md. A call follows its method's
`retryPolicy` or `hedgingPolicy` (gRFC A6): requests are kept in a send
log and replayed on a new stream until the call commits, which happens
when response headers arrive or the log passes `retry_buffer`. Retries
wait a jittered exponential backoff, or the server's
`grpc-retry-pushback-ms`; `retryThrottling` stops retries when too many
fail. Streams the server refused, or that a GOAWAY says it never saw, are
retried at once without counting as an attempt. The server sees
`grpc-previous-rpc-attempts` as `c->prev_attempts`.

## Server

```mettle
var srv: GrpcServer;
var sopts: GrpcServerOptions;
grpc_server_init(&srv, &sopts);
Greeter_register(&srv, &handlers);      // generated, or grpc_server_add(&srv, &desc)
var conn: GrpcServerConn;
grpc_server_conn_init(&conn, &srv, transport);
grpc_server_conn_serve(&conn);          // or grpc_server_conn_step / _poll in your own loop
```

`GrpcServerOptions`: `max_recv`, `max_send`, `send_enc` (compress
responses when the client accepts it), `interceptors`, `keepalive_time_ns`,
`keepalive_timeout_ns`, `min_ping_interval_ns` (5 min),
`permit_without_calls`, `max_conn_idle_ns`, `max_conn_age_ns` (with 10%
jitter), `max_conn_age_grace_ns`.

Handlers are callbacks (`GrpcMethodDesc`: `on_start`, `on_message`,
`on_half_close`, `on_cancel`), so one thread serves many streams. Unary
and server-streaming methods get their single request at half-close.
Inside a handler:

| Function | |
|---|---|
| `grpc_srv_send(c, bytes)` | queue a response; never blocks |
| `grpc_srv_send_headers(c)` | send response headers now |
| `grpc_srv_pending(c)` | bytes not yet taken by flow control |
| `grpc_srv_finish(c, status)` | end the call; do not touch it afterwards |

Every call a handler has seen must be finished exactly once, also after
`on_cancel`. A request for an unknown method gets UNIMPLEMENTED; a
request that is not gRPC (wrong method or content type) gets HTTP 405 or
415. `grpc_server_conn_shutdown` sends GOAWAY and closes once the running
calls end. Pings closer than `min_ping_interval_ns` count as strikes; the
third gets GOAWAY `ENHANCE_YOUR_CALM` "too_many_pings".

## Interceptors

`GrpcInterceptor { user, on_start, on_send, on_headers, on_recv, on_close }`,
any of them 0. `on_start`, `on_send` and `on_recv` return false to fail
the call with the status they fill in. The first interceptor in the list
is the outermost: it sees `on_start` and `on_send` first and `on_recv`
and `on_close` last. On the server, `on_start` sees the request metadata
and is the place for authentication.

## Compression

`GRPC_ENC_IDENTITY`, `GRPC_ENC_GZIP`, `GRPC_ENC_DEFLATE` (zlib format, as
grpc-go and grpc-java send it). Decompressed messages are held to
`max_recv` while they inflate, so a small compressed bomb fails with
RESOURCE_EXHAUSTED instead of using memory.

## Rich errors

`grpc_status_rich(code, message, details, n)` builds a status carrying a
`google.rpc.Status` whose details are `GrpcPbAny` values made with
`grpc_any_pack(&any, "google.rpc.ErrorInfo", bytes)`. The client reads them
with `grpc_status_details(&st, &pb_status)` and `grpc_any_is`. The
messages of google/rpc/error_details.proto are generated as `GrpcPb*`
(`GrpcPbErrorInfo`, `GrpcPbRetryInfo`, `GrpcPbBadRequest`...).

## Health

```mettle
var health: GrpcHealth;
grpc_health_init(&health);                  // "" (the server) is SERVING
grpc_health_register(&health, &srv);
grpc_health_set(&health, "pkg.Service", GRPC_HEALTH_SERVING);
grpc_health_shutdown(&health);              // all NOT_SERVING, before stopping
```

Check, Watch (streams every change) and List are all served.

## Reflection

```mettle
var refl: GrpcReflection;
grpc_reflection_init(&refl);
grpc_reflection_add(&refl, &Greeter_file_descriptors);   // generated
grpc_reflection_register(&refl, &srv);
```

Answers list_services, file_by_filename and file_containing_symbol on
`grpc.reflection.v1` and `grpc.reflection.v1alpha`.

## Transports

A call runs on a `GrpcTransport` (src/transport.mettle): a table of
functions over one HTTP/2 connection, seen as streams and events.

| | |
|---|---|
| `grpc_h2_dial(&t, host, port, timeout_ms)` | client over TCP (h2c, prior knowledge) |
| `grpc_h2_init(&t, net_stream, is_server, cfg)` | any connected `NetStream`, such as an accepted socket (`net_stream_socket(fd)`) |
| `grpc_h2_transport(&t)` | the table for `grpc_channel_init` / `grpc_server_conn_init` |
| `grpc_h2_free(&t)` | close and free |
| `grpc_mem_pipe_init`, `grpc_mem_transport` | a client and server pair in memory |

HTTP/2 is std/h2conn. One `GrpcH2` is one connection; a server accepts
sockets and runs one `GrpcServerConn` per connection (one thread each, or
its own loop with `grpc_server_conn_step`).

## TLS

`import "grpc/tls"` (it brings in mtls and cryptex):

```mettle
var roots: MtlsRoots;
mtls_roots_init(&roots);
mtls_roots_load_system(&roots);             // or mtls_roots_add_file(&roots, "ca.pem")
var cfg: MtlsConfig;
mtls_config_init(&cfg);
cfg.roots = &roots;
var s: MtlsStream;
var t: GrpcH2;
grpc_tls_dial(&t, &s, "example.com", 443, &cfg, 5000);   // GRPC_TLS_OK
copts.scheme = "https";
```

A server calls `grpc_tls_accept(&t, &s, fd, &cfg)` on each accepted
socket, with `mtls_config_load_cert_files(&cfg, chain, key)`. For mutual
TLS set `cfg.roots` and `cfg.client_auth = MTLS_CLIENT_AUTH_REQUIRE` on
the server and load a client certificate on the client. ALPN `h2` is
added to the config and required of the peer.
