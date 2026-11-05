# Using the alph runtime

This guide walks through the runtime in the order you need it. Every function it names is in the [API reference](api.md), with its inputs, outputs and which thread may call it. The longer code comes from [examples/](../examples), which `check.sh` builds and runs, so it stays correct.

## The model

A **node** is one OS process running a set of **schedulers**, one thread per CPU by default. The node runs **actors**. An actor is three things:

- a **state** block, private to it;
- a bounded **mailbox**;
- a **handler**: `fn(ctx: AlphCtx*, m: AlphMsg*) -> int64`.

The handler gets one message, handles it to the end, and returns. That return is the only place a scheduler switches to another actor, which has two consequences:

- **Never block in a handler.** No sleeping, no waiting on a lock, no blocking read. A blocked handler stalls its whole scheduler.
- **Never loop without bound in a handler.** Give it a deadline, `fn handle(...) -> int64 where cycles < 5000`, and the compiler proves it, or tells you why it can't. Work that really is long belongs in a bound actor or a worker pool (below), or should be cut into steps the actor sends to itself.

Actors share nothing. They talk only by messages, and a crash in one handler ends only that actor.

## Starting a node

```mettle
var g_node: AlphNode;

fn main() -> int32 {
  var cfg: AlphConfig;
  alph_config(&cfg);              // defaults: one scheduler per CPU, ...
  if (!alph_node_start(&g_node, &cfg)) {
    return 1;
  }
  alph_node_spawn(&g_node, 0, &root);
  return (int32)alph_node_wait(&g_node);
}
```

`alph_config` fills in defaults. Change the fields you care about before starting:

| Field | Meaning | Default |
|---|---|---|
| `schedulers` | Scheduler threads | One per CPU |
| `max_actors` | Actors alive at once | 65536 |
| `mailbox_size` | Messages a mailbox holds before senders get `ALPH_FULL` | 64 |
| `stack_size` | Stack per scheduler, which is how deep a handler may call | 1 MiB |
| `heap_limit` | Bytes one actor's heap may hold, state included | 0, no limit |
| `max_handles` | IO handles open at once | 65536 |
| `signals` | OS signals the node takes over (see Shutdown and OS signals) | 0, none |
| `spin_us` | How long an idle scheduler keeps checking for work before it sleeps; work arriving meanwhile starts without a wake-up system call | 50 |

`alph_node_wait` returns when a handler calls `alph_halt(ctx, code)`, or a shutdown finishes, and gives back the code.

Name the functions you call from `main` by these rules:
- **Inside a handler,** functions take `ctx`.
- **Outside a handler,** in `main`, another thread or a test, functions start with `alph_node_` and take the node.

## Actors

```mettle
struct Counter {
  total: int64;
}

fn counter(ctx: AlphCtx*, m: AlphMsg*) -> int64 {
  var c: Counter* = (Counter*)ctx->state;
  if (m->tag == TAG_ADD) {
    c->total = c->total + m->arg;
  }
  return ALPH_CONTINUE;
}
```

Every actor's first message is `ALPH_TAG_INIT`, whose `from` is the actor that spawned it. Set up there.

| To spawn | From a handler | From outside |
|---|---|---|
| With no state | `alph_spawn(ctx, flags, &h)` | `alph_node_spawn(node, flags, &h)` |
| With a copy of one `T` | `alph_spawn_value<T>(ctx, flags, &state, &h)` | `alph_node_spawn_value<T>(node, flags, &state, &h)` |
| With raw bytes | `alph_spawn_bytes(ctx, flags, ptr, size, &h)` | `alph_node_spawn_bytes(node, flags, ptr, size, &h)` |

The state is copied, so the original can be a local. Each returns the new pid, or 0 when the node is full or out of memory. `flags` is 0, or any of these combined with `|`:

| Flag | Effect |
|---|---|
| `ALPH_LINK` | Link the new actor to the caller |
| `ALPH_MONITOR` | The caller monitors the new actor |
| `ALPH_BOUND` | The new actor gets a thread of its own and may block (see Bound actors) |

The link and the monitor are set up before the new actor runs, so even a crash in its `INIT` is seen. An actor spawned from outside has no parent, so the `alph_node_spawn` functions accept only `ALPH_BOUND`.

An actor ends when its handler returns `alph_stop(ctx, reason)`, when it crashes, or when an exit signal ends it.

## Messages

A message has a `tag`, the sender's pid in `from`, one word in `arg`, and an optional payload. Pick your tags from `ALPH_TAG_USER` up; the ones below it belong to the runtime.

| To send | Sender writes | Receiver reads |
|---|---|---|
| A tag and one word | `alph_send(ctx, to, TAG, 42)` | `m->arg` |
| A struct | `alph_send_value<Point>(ctx, to, TAG, &p)` | `alph_msg_value<Point>(m, &p)` |
| Bytes of any length | `alph_send_bytes(ctx, to, TAG, ptr, n)` | `alph_msg_bytes(m, &n)` |

`alph_msg_value<T>` returns false when the payload is not exactly one `T`. That usually means a tag was sent with a different type than the receiver expects, and you find out instead of reading garbage. The payload is valid until the handler returns, so copy what you keep.

Sending returns:
- `ALPH_SENT`: delivered.
- `ALPH_FULL`: the receiver's mailbox is full.
- `ALPH_GONE`: the receiver has exited.
- `ALPH_NOMEM`: there was no memory for a large payload.

Nothing is sent unless it returns `ALPH_SENT`. A full mailbox is backpressure: the receiver is behind, and the sender decides whether to retry later, shed the work, or slow down. Messages from one sender arrive in the order they were sent.

## Requests

A request is a message whose answer comes back carrying the same `ref`. From [examples/request.mettle](../examples/request.mettle):

```mettle
fn server(ctx: AlphCtx*, m: AlphMsg*) -> int64 {
  if (m->tag == TAG_SQUARE) {
    alph_reply(ctx, m, TAG_RESULT, m->arg * m->arg);
  }
  return ALPH_CONTINUE;
}

// in the client
c->asking = alph_request(ctx, c->server, TAG_SQUARE, 12, 1000);
...
if (m->tag == TAG_RESULT && m->ref == c->asking) { ... }
if (m->tag == ALPH_TAG_DOWN && m->ref == c->asking) { ... }     // server died
if (m->tag == ALPH_TAG_TIMEOUT && m->ref == c->asking) { ... }  // no answer in time
```

Exactly one of these arrives for each request:
- the reply;
- a `DOWN`, if the server exited or was already gone;
- a `TIMEOUT`, after the given milliseconds.

Anything later for that ref is dropped, so a late reply never surprises you. `alph_request_value<T>` and `alph_reply_value<T>` carry structs, as above, and `_bytes` carries raw data. `alph_reply_full` answers with a word and bytes, for a result code plus data. To stop waiting for a request, call `alph_demonitor(ctx, ref)`: nothing arrives for that ref afterwards.

## Links, monitors and exits

- **Links are two-way.** `alph_link(ctx, pid)`: when either actor exits with a reason other than `ALPH_NORMAL`, the other exits with the same reason. If the other calls `alph_trap_exits(ctx, true)`, it gets an `ALPH_TAG_EXIT` message instead, with `from` the exited actor and `arg` its reason.
- **Monitors are one-way.** `alph_monitor(ctx, pid)` returns a ref. When pid exits, an `ALPH_TAG_DOWN` message arrives with that ref and the reason in `arg`. Monitoring an actor that is already gone gives `DOWN` with `ALPH_NOPROC`.
- **Exit signals.** `alph_exit_signal(ctx, pid, reason)` sends one. `ALPH_KILL` cannot be trapped, so it always ends the target.

Exits and downs arrive after the messages their sender sent before exiting. A worker that sends its result and then stops is seen sending the result first.

Exit reasons:

| Reason | Meaning |
|---|---|
| `ALPH_NORMAL` | A clean exit; links ignore it |
| `ALPH_CRASHED` | A trap or fault in the handler |
| `ALPH_KILLED` | Killed with `ALPH_KILL` |
| `ALPH_SHUTDOWN` | Stopped by a supervisor |
| `ALPH_NOPROC` | The actor was already gone |
| `ALPH_REASON_USER` and up | Your own |

## Timers

- `alph_send_after(ctx, ms, to, tag, arg)` sends a message later.
- `alph_start_timer(ctx, ms, tag, arg)` sends this actor a message later, and returns a ref for `alph_cancel_timer`. A cancelled timer never arrives, even if it was already due.

Timer messages are never dropped for a full mailbox.

## Memory

Every actor has its own heap. Nothing else allocates from it, so allocating takes no lock, and there is no garbage collector.

- **The state** is the first block in the heap, copied in at spawn.
- **`alph_alloc(ctx, size)`** gives 16-byte aligned memory that is not zeroed. Use it for anything the state points to. `alph_free(ctx, ptr)` gives it back, and `alph_realloc(ctx, ptr, size)` resizes it, possibly to a new address.
- **Whatever you leave is freed when the actor exits,** even by a crash, so an actor never leaks past its life.
- **`alph_scratch(ctx, size)`** gives memory that is valid until the handler returns, then freed all at once at no cost. Use it for temporaries such as a read buffer or a parsed request. Do not keep the address, and do not free it.
- **`alph_heap_used(ctx)`** is the bytes the heap holds now, state included.
- **`heap_limit`** in the node config caps one actor's heap, state included. Past it `alph_alloc` and `alph_realloc` return 0, so check them, and a spawn whose state does not fit returns 0. An actor that runs away runs out alone.
- **A medic reset** empties the heap, so pointers kept in the state do not survive it.

Memory belongs to the actor that allocated it. Never send a pointer into your heap to another actor: it is freed when you exit and is not valid on the other side. Send the data.

Message payloads are copied on send, and the receiver's copy is valid until its handler returns. Globals are shared by every scheduler. Avoid them, or guard them with the atomics in `alph/sys/atomic`.

## Passive healing: supervisors

A supervisor restarts actors that exit. From [examples/supervised.mettle](../examples/supervised.mettle):

```mettle
var spec: AlphSupSpec;
alph_sup_init(&spec, ALPH_ONE_FOR_ONE);
alph_sup_add_value<Worker>(&spec, &w, &worker);
alph_spawn_sup(ctx, &spec);
```

`alph_sup_add` returns the child's spec, so you can change it by name:

```mettle
var child: AlphChildSpec* = alph_sup_add_value<Db>(&spec, &db, &db_handler);
child->restart = ALPH_TRANSIENT;   // restart only after a crash, not a clean exit
child->shutdown_ms = 10000;        // time it gets to stop before it is killed
child->sync_start = true;          // the next child waits for its alph_sup_ack
```

Strategies:

| Strategy | When a child exits |
|---|---|
| `ALPH_ONE_FOR_ONE` | Only it restarts |
| `ALPH_ONE_FOR_ALL` | Every child restarts |
| `ALPH_REST_FOR_ONE` | It and the children started after it restart |

- **Restart limit.** More than `spec.max_restarts` restarts within `spec.period_ms` and the supervisor gives up. It exits with `ALPH_SHUTDOWN`, so its own supervisor handles the failure. Nest supervisors to build a tree.
- **Backoff.** Set `spec.backoff_ms` to space out repeated restarts.

A restarted child is a new actor: a new pid, its initial state, and anything it was handling is lost.

## Proactive healing: medics

A medic repairs an actor in place instead. The actor keeps its pid, links, monitors, mailbox and state, and nobody else sees the crash. Based on [examples/medic.mettle](../examples/medic.mettle):

```mettle
fn repair(ctx: AlphCtx*, r: AlphRepair*) -> int64 {
  if (r->message->tag == TAG_SHARE && r->message->arg == 0) {
    return ALPH_HEAL_RESUME;        // drop the bad message, keep going
  }
  return ALPH_HEAL_FAIL;            // let the supervisor deal with it
}

// in the worker's INIT
var spec: AlphMedicSpec;
alph_medic_init(&spec, &repair);
alph_attach_medic(ctx, &spec);
```

The repair function sees:
- the crashed actor's state, which it may fix;
- the message it was handling;
- how many repairs it made recently.

It answers one of:

| Answer | What happens |
|---|---|
| `ALPH_HEAL_RESUME` | Keep the state, drop the message |
| `ALPH_HEAL_RETRY` | Keep the state, handle the message again |
| `ALPH_HEAL_RESET` | Back to the state at attach time; `INIT` runs again |
| `ALPH_HEAL_FAIL` | The actor exits, and passive healing takes over |

Too many repairs within `spec.period_ms` count as a fail. Use both kinds together: a medic for crashes you know how to repair, and a supervisor around the actor for everything else.

## Bound actors

A handler must not block, but some work does: file IO, an fsync, a library with blocking calls. Spawn that actor with `ALPH_BOUND` and it gets a thread of its own. It may block as long as it likes, and no other actor waits for it. In every other way it is an ordinary actor, with a pid, a mailbox, links and monitors.

```mettle
var db: int64 = alph_spawn_value<Db>(ctx, ALPH_BOUND | ALPH_LINK, &db_state, &db_handler);
```

- The thread starts with the actor and stops when it exits.
- A thread costs far more than an actor. Keep bound actors to a handful, such as one per database file. For many short blocking jobs use a worker pool.
- A bound actor stuck for ever in a system call cannot be killed, and it keeps `alph_node_wait` waiting. Give blocking calls timeouts.

`alph_exit_now(reason)` ends the running actor at once, from any call depth, as if its handler had returned `alph_stop(ctx, reason)`. The frames in between are abandoned, so their `defer` blocks do not run. The heap is freed as on any exit. Use it for an error found deep in a call chain, where passing a failure back up is clumsy:

```mettle
const REASON_BAD_INPUT: int64 = ALPH_REASON_USER + 1;

fn parse_digit(c: int64) -> int64 {
  if (c < 48 || c > 57) {
    alph_exit_now(REASON_BAD_INPUT);
  }
  return c - 48;
}
```

Called outside a handler it does nothing. A handler itself should just return `alph_stop`.

## IO and TCP

An IO handle stands for an open file descriptor, owned by one actor. No call on it blocks. When it would have to wait, you arm the handle and a message tells you it is ready.

| Call | What it does |
|---|---|
| `alph_io_open(ctx, fd)` | Take ownership of a descriptor, and make it non-blocking |
| `alph_io_arm(ctx, h, events)` | Ask for one `ALPH_TAG_IO` message when `h` is ready for `ALPH_IO_IN`, `ALPH_IO_OUT` or both |
| `alph_io_read(ctx, h, buf, size)` | Bytes read, 0 at end of stream, `ALPH_IO_AGAIN` when nothing is there, or a negative errno |
| `alph_io_write(ctx, h, buf, size)` | Bytes written, which may be fewer than `size`, `ALPH_IO_AGAIN`, or a negative errno |
| `alph_io_close(ctx, h)` | Close now |
| `alph_io_give(ctx, h, pid)` | Hand the handle to another actor |
| `alph_io_error(ctx, h)` | The pending socket error, 0 for none |

The `ALPH_TAG_IO` message has `ref` set to the handle and `arg` set to what is ready, which may include `ALPH_IO_ERR` or `ALPH_IO_HUP`. It comes once per arm, so arm again after each. A handler that reads once per message is enough: if data is left, arming again reports it at once.

TCP handles come from `alph_tcp_listen(ctx, ip4, port, backlog, flags)`, `alph_tcp_accept` and `alph_tcp_connect`. Pass port 0 to listen and the system picks one, which `alph_tcp_port` reads. With `ALPH_TCP_REUSEPORT` one listener per scheduler can share a port. `alph_tcp_nodelay` turns off Nagle's algorithm. A listener is ready for `ALPH_IO_IN` when a connection waits; accept until `ALPH_IO_AGAIN`, then arm it again. A connect starts at once: arm `ALPH_IO_OUT`, and on `ALPH_TAG_IO` the connection is up when `alph_io_error` returns 0.

From [examples/echo.mettle](../examples/echo.mettle), an acceptor taking one connection:

```mettle
a->listener = alph_tcp_listen(ctx, alph_ip4(127, 0, 0, 1), 0, 16, 0);
alph_io_arm(ctx, a->listener, ALPH_IO_IN);
...
var h: int64 = alph_tcp_accept(ctx, a->listener);
if (h > 0) {
  ...                                        // a connection: give it to an actor
} else {
  alph_io_arm(ctx, a->listener, ALPH_IO_IN); // none waiting: wait for the next
}
```

Ownership rules:
- **A handle belongs to one actor.** Any call on another actor's handle returns -9.
- **The runtime closes it when the owner exits,** a crash included. A crashed connection actor leaves no open socket, and its peer sees the stream end.
- **`alph_io_give` moves it.** The new owner gets `ALPH_TAG_IO_GIVEN` with `ref` the handle and `from` the giver, and arms it itself. If the target exits before taking it, it is closed. If `alph_io_give` returns `ALPH_GONE`, the giver still owns it.
- **Regular files** can be owned but not armed. Read them from a bound actor.
- **`ALPH_TAG_IO`, `ALPH_TAG_IO_GIVEN` and process exits are never dropped,** whatever the mailbox holds.

### Output queues

A write can take only part of the data. An `AlphOutq` in the actor's state keeps the rest, in the actor's heap, so the handler does not track partial writes. From the connection actor in the same example:

```mettle
alph_outq_init(&c->out, 65536);                   // once; the limit is in bytes
...
if (alph_outq_push(ctx, &c->out, buf, n) != ALPH_SENT) {
  return alph_stop(ctx, ALPH_NORMAL);             // ALPH_FULL: the peer reads too slowly
}
if (alph_outq_flush(ctx, &c->out, c->h) < 0) {
  return alph_stop(ctx, ALPH_NORMAL);             // the connection is broken
}
var events: int64 = ALPH_IO_IN;
if (alph_outq_pending(&c->out) > 0) {
  events = events | ALPH_IO_OUT;                  // the socket was full; flush again when writable
}
alph_io_arm(ctx, c->h, events);
```

- `alph_outq_push` copies the data in whole, or queues nothing and returns `ALPH_FULL` when that would pass the limit. A full queue means the peer reads slower than you write. Shed the connection rather than buffer without bound.
- `alph_outq_flush` writes what the socket takes and returns the bytes still waiting, or a negative errno.
- `alph_outq_free` frees the buffer if the actor lives on. When it exits, the heap is freed anyway.

## Names and groups

A name finds an actor. A group reaches several.

| Call | What it does |
|---|---|
| `alph_register(ctx, name)` | Claim `name` for this actor. False when another live actor holds it |
| `alph_unregister(ctx, name)` | Release it |
| `alph_whereis(ctx, name)` | The pid, or 0 |
| `alph_node_whereis(node, name)` | The same, from outside |
| `alph_group_join(ctx, group)` | Join a group, creating it if needed |
| `alph_group_leave(ctx, group)` | Leave it |
| `alph_group_send(ctx, group, tag, arg)` | Send to every member; `_value` and `_bytes` as for `alph_send` |
| `alph_node_group_size(node, group)` | Members now |

```mettle
alph_register(ctx, "log");
alph_group_join(ctx, "room:1");
...
var log: int64 = alph_whereis(ctx, "log");
var reached: int64 = alph_group_send(ctx, "room:1", TAG_SAY, 42);
```

- **Names are copied, and dropped when the actor exits,** a crash included. A restarted actor is a new one, so it registers again in its `INIT`.
- **A pid from `alph_whereis` can be stale.** The actor may exit just after, so a send can still return `ALPH_GONE`.
- **A group send can be dropped per member.** It reaches every member, the sender too if it is one, but skips a member whose mailbox is full or that has exited. It returns how many received it.
- **A group with no members is gone.**

## Child processes

`alph_proc_spawn(ctx, &spec)` starts a program and reports its end as a message. Zero the spec, then set what you need. From [examples/jobs.mettle](../examples/jobs.mettle):

```mettle
var argv: cstring[4];
argv[0] = "sh";
argv[1] = "-c";
argv[2] = "echo hello";
argv[3] = (cstring)0;
var spec: AlphProcSpec;
memset((rawptr)&spec, 0, (int64)sizeof(AlphProcSpec));
spec.argv = &argv[0];
spec.flags = ALPH_PROC_OUT;
var proc: int64 = alph_proc_spawn(ctx, &spec);
alph_io_arm(ctx, spec.stdout_h, ALPH_IO_IN);
```

| Field | Meaning |
|---|---|
| `argv` | The program and its arguments, ending with 0. A name without a slash is looked up in `PATH` |
| `envp` | The environment, ending with 0. 0 means the one the node started with |
| `cwd` | The working directory. 0 means the node's |
| `log` | A file that gets stdout and stderr when they are not piped |
| `flags` | `ALPH_PROC_IN`, `ALPH_PROC_OUT` and `ALPH_PROC_ERR` pipe a stream; `ALPH_PROC_MERGE` sends stderr into the stdout pipe; `ALPH_PROC_QUIET` sends streams that are not piped to /dev/null; `ALPH_PROC_KEEP_GROUP` keeps the child in the node's process group (see below) |
| `stdin_h`, `stdout_h`, `stderr_h` | Out: handles for the piped streams, else 0 |

`alph_proc_spawn` returns the process handle, or a negative errno with nothing started: -2 when the program is not found, -22 when `argv` is empty.

- **The end arrives once** as `ALPH_TAG_PROC_EXIT`, with `ref` the process handle and `arg` the exit code, or 128 plus the signal that ended it. The handle is closed then.
- **Piped streams are ordinary IO handles,** owned by the caller. Read them until `alph_io_read` returns 0, then close them. Output and the exit message arrive in either order, so wait for both.
- **The child's whole process group is killed and reaped** when its owner exits, a crash included, or closes the process handle first.
- **Each child gets a process group of its own** unless started with `ALPH_PROC_KEEP_GROUP`. Use that flag for a program run in the terminal's foreground, so Ctrl-C reaches it and it may read the terminal; it is then signalled and killed alone, not with its own children.
- **`alph_proc_kill(ctx, h, sig)`** sends a signal to the group. A graceful stop is `ALPH_SIGTERM`, a timer, then `ALPH_SIGKILL`. `alph_proc_os_pid` gives the OS process id.

To wait for a started server to accept connections, spawn a port waiter. It tries to connect every `interval_ms`, at most `tries` times, and answers once with `ALPH_TAG_PORT_READY`: `arg` is 1 when a connection was accepted, 0 when it gave up.

```mettle
alph_spawn_port_wait(ctx, alph_ip4(127, 0, 0, 1), port, 50, 100);
...
if (m->tag == ALPH_TAG_PORT_READY && m->arg == 1) {
  ...                                     // the server accepts connections
}
```

## Dynamic supervisors

A supervisor from `alph_sup_add` has a fixed list of children. When children come and go at run time, such as one actor per connection, use a dynamic supervisor. Every child runs the same handler, and each is started on request with its own state.

```mettle
var spec: AlphDynSpec;
alph_dyn_init(&spec, &conn);
spec.restart = ALPH_TEMPORARY;
var sup: int64 = alph_spawn_dyn(ctx, &spec);
...
var ref: int64 = alph_dyn_start_value<Conn>(ctx, sup, &c, 1000);
// later: ALPH_TAG_STARTED with m->ref == ref and m->arg the child's pid
```

- **`alph_dyn_init` sets the defaults.** Change `restart`, `shutdown_ms`, `max_restarts`, `period_ms` and `max_children` by name. The default restart type is `ALPH_PERMANENT`; use `ALPH_TEMPORARY` for connections. `max_children` is 0, no limit.
- **A start is a request.** `alph_dyn_start_value<T>`, `alph_dyn_start_bytes` and `alph_dyn_start` (the state is one word) return a ref. The answer is `ALPH_TAG_STARTED` with `arg` the new pid, or 0 when the supervisor refused (it is full or stopping, or memory ran out). As for any request, `ALPH_TAG_DOWN` or `ALPH_TAG_TIMEOUT` can come instead.
- **`alph_spawn_dyn` links the supervisor to its caller.** If the caller exits, the supervisor stops its children and exits. From outside the node, use `alph_node_spawn_dyn`.
- **Children are restarted one by one,** with the state they were started with. More than `max_restarts` restarts within `period_ms` and the supervisor stops every child and exits with `ALPH_SHUTDOWN`.
- **A handle cannot go in a child's start state,** because ownership has to move. Start the child, wait for `ALPH_TAG_STARTED`, then `alph_io_give` the handle to the new pid. The child takes it on `ALPH_TAG_IO_GIVEN`.

[examples/echo.mettle](../examples/echo.mettle) does this for each accepted connection:

```mettle
if (m->tag == ALPH_TAG_STARTED && m->arg != 0) {
  given = alph_io_give(ctx, a->waiting, m->arg);
}
if (given != ALPH_SENT) {
  alph_io_close(ctx, a->waiting);     // nobody took the connection
}
```

## Worker pools and limiters

A **pool** runs blocking jobs on `ALPH_BOUND` worker threads, so handlers stay short. A **limiter** hands out a fixed number of permits, to cap anything: processes, database connections, jobs.

A pool:

```mettle
fn shout(input: rawptr, size: int64, out: AlphOut*) -> int64 { ... }

var pool: int64 = alph_spawn_pool(ctx, 4, 1024);
var ref: int64 = alph_offload(ctx, pool, (rawptr)"hello", 5, 5000, &shout);
...
if (m->tag == ALPH_TAG_DONE && m->ref == ref) {
  var n: int64 = 0;
  var text: rawptr = alph_msg_bytes(m, &n);   // what shout put in its AlphOut
  // m->arg is what shout returned
}
```

- **`alph_spawn_pool(ctx, workers, queue)`** starts a dispatcher and `workers` threads. `queue` is how many jobs may wait for a free worker. The pool is linked to its caller and exits with it. From outside the node, use `alph_node_spawn_pool`.
- **The work function** is `fn(input: rawptr, size: int64, out: AlphOut*) -> int64`. It runs on a worker thread, where it may block. The input is a copy. It adds result bytes with `alph_out_bytes(out, data, size)`, and must not keep a pointer into its input or `out` after it returns.
- **The answer is a request answer.** `ALPH_TAG_DONE` carries the function's return in `arg` and the bytes as the payload. `ALPH_TAG_FAILED` carries the reason in `arg`: `ALPH_CRASHED`, the reason given to `alph_exit_now`, or `ALPH_FULL` when the queue was full. `ALPH_TAG_DOWN` and `ALPH_TAG_TIMEOUT` can come as for any request.
- **A job that times out still runs to the end,** and its result is dropped. A worker that crashes or exits is replaced.

A limiter, from the `job` actor in [examples/jobs.mettle](../examples/jobs.mettle):

```mettle
var limiter: int64 = alph_spawn_limiter(ctx, 2);       // two permits
...
j->permit = alph_acquire(ctx, j->limiter);             // in INIT
...
if (m->tag == ALPH_TAG_GRANTED && m->ref == j->permit) {
  ...                                                  // start the work
}
...
alph_release(ctx, j->limiter, j->permit);              // when it ends
```

- **Permits go first come, first served.** `ALPH_TAG_GRANTED` arrives with the ref `alph_acquire` returned.
- **A holder that exits gives its permit back,** a crash included.
- **`alph_release` on a request still waiting withdraws it.** There is no timeout on `alph_acquire`; to give up, release.
- **The limiter is linked to its caller.** If it can exit, trap exits in the caller.

## Shutdown and OS signals

`alph_shutdown(ctx, deadline_ms, code)` stops the node in order, and `alph_node_shutdown(node, deadline_ms, code)` does it from outside. Each returns false when a shutdown has already begun.

- **Top-level actors stop newest first.** Those started with `alph_node_spawn` and its variants get an `ALPH_SHUTDOWN` exit signal one at a time, each after the one before has exited. A supervisor therefore stops its children before an older actor goes.
- **An actor that does not trap exits just exits.** One that does gets `ALPH_TAG_EXIT` with `from` 0 and `arg` `ALPH_SHUTDOWN`, and should stop.
- **Then everything left is killed,** at `deadline_ms` (0 means no limit) or when the top-level actors are gone, whichever comes first. The node halts and `alph_node_wait` returns `code`. A killed actor that owns child processes kills them as it exits.
- **Anything not under a top-level actor** gets no warning, only the kill.

A node can take over OS signals. Set the bits in the config, and call `alph_node_start` from `main` before starting other threads, since the node blocks those signals in its calling thread:

```mettle
cfg.signals = ((int64)1 << ALPH_SIGINT) | ((int64)1 << ALPH_SIGTERM);
...
alph_node_signals(&g_node, pid, TAG_SIGNAL);   // optional
```

- **By default the node handles them.** The first signal starts `alph_node_shutdown(node, 5000, 128 + signal)`, and a second halts the node at once.
- **`alph_node_signals` hands them to an actor.** It then gets a message with the tag you gave and `arg` the signal number (`ALPH_SIGINT`, `ALPH_SIGTERM`, `ALPH_SIGHUP`). These are never dropped. Call it with pid 0 to give signals back to the node. If that actor exits, the node handles them again.
- **Child processes start with every signal unblocked.**

## Distribution

Nodes connect over TCP, and an actor on one node can talk to an actor on another as if it were local. From [examples/cluster.mettle](../examples/cluster.mettle), which runs two nodes in one process:

```mettle
import "alph/net/dist";

// in node B: start the manager, then use the port it reports
alph_dist_start(ctx, alph_ip4(0, 0, 0, 0), 4370, COOKIE);
...
if (m->tag == ALPH_TAG_DIST_READY) { ... }       // m->arg is the port it listens on

// in node A
alph_dist_connect(ctx, alph_ip4(10, 0, 0, 2), 4370, 2000);
...
if (m->tag == ALPH_TAG_DIST_UP) {                // m->arg is the connection, or 0
  alph_monitor(ctx, m->arg);                     // DOWN when the connection is lost
  alph_dist_whereis(ctx, m->arg, "counter", 2000);
}
if (m->tag == ALPH_TAG_DIST_FOUND) {             // m->arg is a proxy, or 0
  b->asking = alph_request(ctx, m->arg, TAG_ADD, 1, 2000);
}
```

A **proxy** is a local actor that stands for an actor on another node. Send to it, and the message arrives at the remote actor. The remote actor sees the sender as a proxy for you, so it can answer, and a request and its reply keep their `ref` across the connection. Messages from one sender stay in order.

- **Start a manager first.** `alph_dist_start(ctx, ip4, port, cookie)` starts this node's manager, linked to the caller, and returns its pid, or 0 when the cookie is empty or the node is full. Port 0 picks a free port. When the manager listens, the caller gets `ALPH_TAG_DIST_READY` with the port in `arg`; connect and look up names only after that. If it cannot listen it exits with `ALPH_SHUTDOWN`. `alph_dist_port` asks it for the port again. Trap exits in the caller, or the manager's exit takes it down.
- **`alph_dist_connect(ctx, ip4, port, timeout_ms)`** returns a ref. The answer is `ALPH_TAG_DIST_UP` with `arg` the connection's pid, or 0 when the node could not be reached or holds another cookie. As for any request, `ALPH_TAG_DOWN` or `ALPH_TAG_TIMEOUT` can come instead. It returns `ALPH_GONE` when no manager is running.
- **`alph_dist_whereis(ctx, conn, name, timeout_ms)`** looks up a name on the node at the other end of `conn`. The answer is `ALPH_TAG_DIST_FOUND` with `arg` a proxy, or 0 when the name is not registered there. The same proxy comes back for the same actor.
- **Sending** works as at home: `alph_send`, `alph_request` and the `_value` and `_bytes` forms, with the proxy as the target. A message, payload included, may be up to 16 MiB; a larger one makes the receiving node close the connection.
- **Links, monitors and exit signals** work through a proxy. A proxy lives exactly as long as its actor and exits with the same reason, so a monitor on it gets the remote actor's `DOWN`, and a link brings a remote crash to the linked actor (or an `ALPH_TAG_EXIT`, if it traps exits). Linking is two-way: the remote actor is linked to a proxy for yours, so your crash reaches it too. An exit signal sent to a proxy, `ALPH_KILL` included, goes to the remote actor.
- **`ALPH_NOCONNECTION`** is the reason every proxy exits with when the connection is lost, and the reason the connection's own pid exits with. Monitor the connection pid from `ALPH_TAG_DIST_UP` to hear that the other node is gone. A monitor or link on a proxy sees the same reason.
- **A connection is closed rather than losing a message.** If the receiving actor's mailbox is full, the connection stops reading until it has room, and TCP slows the sender. If a node sends faster than the connection carries and its queue (64 MiB) fills, the connection is closed, and everything over it sees `ALPH_NOCONNECTION`. A message is never dropped on a connection that stays up.

The **cookie** names the cluster, and it is the Noise pre-shared key: its SHA-256 keys the handshake, and it is never sent. Connections are encrypted and authenticated (`Noise_XXpsk3_25519_ChaChaPoly_SHA256`). A node with another cookie cannot connect and cannot read or change traffic, so `alph_dist_connect` answers 0. Whoever has the cookie can join the cluster, so keep it secret, long and random. Each node makes a new static key when its manager starts.

`alph/net/dist` and `alph/net/noise` are opt-in. They are not in `alph/alph`, and a program that never imports them carries no crypto. Import `alph/net/dist` yourself, and map the cryptex package in your app's `mettle.deps` beside alph:

```text
alph=<path>/alph/src
cryptex=<path>/cryptex/src
```

Names and groups are still local to a node, so you find a remote actor with `alph_dist_whereis`, not `alph_whereis`.

## Building

```bash
../Mettle/bin/mettle --build -s --release app.mettle
```

- **Use the dev compiler** from the Mettle repo.
- **Always build with `-s`.** Without it, a failed check (bounds, divide) exits the program before alph can catch it. Faults such as a null pointer are caught either way.
- **`--safe` is not supported yet.**
- **Test in debug and in release.** Release builds have had miscompiles.

## Rules to keep

1. **Handlers return quickly.** No blocking calls, no unbounded loops. Blocking work goes to a bound actor or a worker pool; waiting for IO goes through armed handles.
2. **Don't keep `ctx` or a message** past the handler call. Copy what you need.
3. **Use tags from `ALPH_TAG_USER` up.**
4. **Check what `alph_send` returns** where losing a message matters. A mailbox can refuse a message; signals, timers, IO readiness and handle gifts are never dropped.
5. **Put a supervisor above everything that should come back,** and a medic on actors whose crashes you know how to repair.
6. **Memory and handles belong to one actor.** Don't keep an `alph_scratch` address past the handler, and never send a pointer into your heap; send the data. Move an IO handle with `alph_io_give`, not by sending its number.
7. **An actor that links to something that may exit traps exits.** Otherwise the link takes it down too, and nothing is left to notice.
8. **Give every blocking call in a bound actor a timeout,** so a shutdown can finish.
