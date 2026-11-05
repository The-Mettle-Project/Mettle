# Non-blocking use

`MtlsConn` never touches a socket. It holds two queues: bytes that came
from the network and bytes that must go to it. Your event loop moves
bytes between those queues and the socket; mtls does the rest.

| Step | Call |
|---|---|
| What does the connection need? | `mtls_conn_wants(c)`: `MTLS_WANT_READ`, `MTLS_WANT_WRITE`, both, or 0 |
| Socket readable | `mtls_conn_in_space(c, &p)` gives room; read into it; `mtls_conn_in_commit(c, n)` |
| Socket writable | `mtls_conn_out_peek(c, &p)` gives bytes; write some; `mtls_conn_out_done(c, n)` |
| Handshake finished? | `mtls_conn_handshaking(c)` is false and `mtls_conn_error(c)` is 0 |
| Application data in | `mtls_conn_read(c, buf, cap)`: count, 0 at a clean end, `MTLS_E_WANT_IO` for "wait" |
| Application data out | `mtls_conn_write(c, p, n)`, then flush when writable |
| Close | `mtls_conn_close(c)`, flush, `mtls_conn_free(c)` |

`mtls_conn_feed(c, p, n)` and `mtls_conn_take_out(c, buf, cap)` are
copying versions of the in/out calls, handy for tests and pipes.

A connection that holds unread application data stops taking input until
you read it. That is backpressure, not an error: read, then feed again.

## MtlsNb: a non-blocking socket, ready-made

`MtlsNb` (src/tls/nb.mettle) does the moving for you on a non-blocking
socket. Nothing in it waits: each call does what it can and returns.

| Call | Meaning |
|---|---|
| `mtls_nb_client(&t, fd, &cfg, host)`, `mtls_nb_server(&t, fd, &cfg)` | Start (the socket is made non-blocking) |
| `mtls_nb_pump(&t)` | On every readiness event: send what is queued, read and process what came, send any answer. The handshake moves forward here |
| `mtls_nb_handshaking(&t)` | Still shaking hands |
| `mtls_nb_read(&t, buf, cap)` | Data, 0 at a clean end, `MTLS_E_WANT_IO` (nothing yet), `MTLS_E_EOF` (closed without close_notify) |
| `mtls_nb_write(&t, p, n)` | Queue and send what fits; the rest goes on later pumps |
| `mtls_nb_wants(&t)` | `MTLS_NB_READ` / `MTLS_NB_WRITE` to watch for; 0 means finished |
| `mtls_nb_close(&t)` | close_notify, free, close the socket if `t.owns_fd`. Safe twice, and from an exit hook |

`MTLS_NB_READ` is 1 and `MTLS_NB_WRITE` is 4, the same as `ALPH_IO_IN` /
`ALPH_IO_OUT`, `ALPH_RX_READ` / `ALPH_RX_WRITE` and `EPOLLIN` / `EPOLLOUT`,
so the mask goes straight into those APIs. An `MtlsNb` belongs to one
thread or one actor at a time.

## With alph actors

Run each connection in its own session actor. The handshake then never
blocks a scheduler thread: it advances one readiness message at a time.
Set an exit hook so a session that crashes or stops early frees its TLS
state. alph owns the socket (`owns_fd` stays false) and closes it after
the hook. [examples/alph_echo](../examples/alph_echo) is the complete,
tested version.

```mettle
struct Session { h: int64; tls: MtlsNb; started: bool; }

fn session_exit(ctx: AlphCtx*, reason: int64) -> int64 {
  var s: Session* = (Session*)ctx->state;
  if (s->started) { mtls_nb_close(&s->tls); }   // runs on every exit, crash included
  return 0;
}

fn session(ctx: AlphCtx*, m: AlphMsg*) -> int64 {
  var s: Session* = (Session*)ctx->state;
  if (m->tag == ALPH_TAG_IO_GIVEN) {
    s->h = m->ref;
    alph_on_exit(ctx, &session_exit);
    if (mtls_nb_server(&s->tls, alph_io_fd(ctx, s->h), &g_cfg) != MTLS_OK) { return alph_stop(ctx, ALPH_NORMAL); }
    s->started = true;
    alph_io_arm(ctx, s->h, mtls_nb_wants(&s->tls));
    return ALPH_CONTINUE;
  }
  if (m->tag != ALPH_TAG_IO) { return ALPH_CONTINUE; }
  if (mtls_nb_pump(&s->tls) != MTLS_OK) { return alph_stop(ctx, ALPH_NORMAL); }
  if (!mtls_nb_handshaking(&s->tls)) {
    // mtls_nb_read until MTLS_E_WANT_IO; answer with mtls_nb_write
  }
  var ev: int64 = mtls_nb_wants(&s->tls);
  if (ev == 0) { return alph_stop(ctx, ALPH_NORMAL); }
  alph_io_arm(ctx, s->h, ev);
  return ALPH_CONTINUE;
}
```

## With alph/sched/exec tasks

A task waits for its socket with `alph_task_on_ready` and continues in a
new task when it is ready:

```mettle
fn tls_step(tc: AlphTaskCtx*, arg: int64) -> int64 {
  var s: Session* = (Session*)arg;
  if (mtls_nb_pump(&s->tls) == MTLS_OK) {
    // read and write as above
    var ev: int64 = mtls_nb_wants(&s->tls);
    if (ev != 0) {
      alph_task_on_ready(tc, s->tls.fd, ev, arg, &tls_step);
      return 0;
    }
  }
  alph_rx_remove(alph_task_reactor(tc), s->tls.fd);   // before the socket closes
  s->tls.owns_fd = true;
  mtls_nb_close(&s->tls);
  free(s);
  return 0;
}
```

mtls calls do only what alph's handler and task types allow (`with alloc,
syscall, asm`), so they can be called from handlers, tasks and exit hooks.

## Threads

`MtlsConn` and `MtlsNb` belong to one thread at a time. `MtlsStream` (the
blocking wrapper) may be shared after the handshake: one thread reads
while any number of threads write. Each `mtls_stream_write` call goes out
whole, reads are serialized, and key updates and alerts from the reading
side are sent under the same write lock. To stop, call
`mtls_stream_shutdown` from any thread (it wakes a blocked reader), join
the others, then `mtls_stream_close`.

## Blocking

`MtlsStream` (src/tls/stream.mettle) wraps the same calls over a blocking
std/socket descriptor: `mtls_dial`, `mtls_stream_client`,
`mtls_stream_server`, `mtls_stream_read`, `mtls_stream_write`,
`mtls_stream_close`.
