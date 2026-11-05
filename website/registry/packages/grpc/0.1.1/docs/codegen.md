# Generated code

```
grpc_gen [-I DIR]... [--out DIR] [--prefix NAME] [--no-pb] FILE.proto...
```

`grpc_gen` reads `.proto` files with Codec's parser. For `a/b.proto` it
writes `OUT/a/b_pb.mettle` (the messages, exactly as `codec_protoc` would;
see Codec's docs/protobuf.md) and, if the file has services,
`OUT/a/b_grpc.mettle`. `--prefix` goes in front of every generated type
name, which keeps names from clashing across packages. `--no-pb` skips
the message files.

For `service Greeter` with `rpc SayHello (HelloRequest) returns (HelloReply)`:

## Client

| Kind | Functions |
|---|---|
| unary | `Greeter_SayHello(ch, &req, &resp, &buf, &o) -> GrpcStatus` |
| server streaming | `_start(ch, &req, &o) -> GrpcCall*`, `_recv(c, &resp) -> bool` |
| client streaming | `_start(ch, &o)`, `_write(c, &req)`, `_finish(c, &resp) -> GrpcStatus` |
| bidirectional | `_start(ch, &o)`, `_write(c, &req)`, `_recv(c, &resp)`, then `grpc_call_close_send(c)` |

Then `grpc_call_finish(c)` and `grpc_call_free(c)` for the streaming kinds.

Decoded strings point into the message they came from. For the unary
function that is `buf` (free `buf.data` when done with `resp`); for
`_recv` it is the call, until the next `_recv`; for `_finish` until
`grpc_call_free`. Init `resp` once before the first `_recv`. The status a
unary function returns owns its strings (`grpc_status_free`).

## Server

```mettle
export struct Greeter_Handlers {
  user: rawptr;
  on_cancel: fn(rawptr, GrpcCall*) -> void;
  SayHello: fn(rawptr, GrpcCall*, HelloRequest*, HelloReply*) -> GrpcStatus;   // unary
  // server streaming:  M: fn(rawptr, GrpcCall*, Req*) -> GrpcStatus
  // client streaming:  M: fn(rawptr, GrpcCall*, Req*) -> void, M_done: fn(rawptr, GrpcCall*, Resp*) -> GrpcStatus
  // bidirectional:     M: fn(rawptr, GrpcCall*, Req*) -> void, M_done: fn(rawptr, GrpcCall*) -> GrpcStatus
}
```

`Greeter_register(&srv, &h)` adds the methods; `h` must outlive the
server. A method left 0 answers UNIMPLEMENTED. Streaming handlers send
with `Greeter_M_reply(c, &resp)`. The status a handler returns ends the
call (a unary response is sent first when it is OK); return
`grpc_status(GRPC_PENDING, "")` to keep the call and finish it later with
`grpc_srv_finish`. A request is valid only during the callback. Nested
messages put into a unary or client-streaming response must be allocated
with `new`, because the stub frees the response afterwards.

Without `on_cancel` a cancelled call is finished for you; with it, your
callback must call `grpc_srv_finish`.

## Reflection

`Greeter_file_descriptors(out, cap)` gives the `FileDescriptorProto` of
the file and of everything it imports, base64, for `grpc_reflection_add`.
`Greeter_NAME` is the full service name, for health checks.
