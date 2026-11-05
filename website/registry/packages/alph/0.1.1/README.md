# alph runtime

Erlang-style actors, supervision and self-healing for [Mettle](https://github.com/The-Mettle-Project/Mettle), written in Mettle.

An actor is a state block, a bounded mailbox and a handler that runs one message to completion. A crash inside a handler ends only that actor, and its links and monitors see it.

Healing comes in two kinds:
- **Passive:** supervisors restart failed actors the OTP way.
- **Proactive:** a medic repairs a crashed actor in place, on the same pid, without anyone else seeing the crash.

Around that: a heap per actor, bound actors that may block, non-blocking IO and TCP, names and groups, child processes, dynamic supervisors, a worker pool and a limiter, and an ordered shutdown that handles OS signals.

Nodes can also connect: an actor on another node is reached through a proxy, with remote requests, links, monitors and exit signals, over a Noise-encrypted connection. This is opt-in (`alph/net/dist`) and not part of `alph/alph`.

It runs on x86-64 Linux.

## Example

```mettle
import "std/io";
import "alph/alph";

const TAG_ADD: int64 = ALPH_TAG_USER;

struct Counter {
  total: int64;
}

fn counter(ctx: AlphCtx*, m: AlphMsg*) -> int64 {
  var c: Counter* = (Counter*)ctx->state;
  if (m->tag == TAG_ADD) {
    c->total = c->total + m->arg;
    if (c->total >= 6) {
      println("total {c->total}");
      alph_halt(ctx, 0);
    }
  }
  return ALPH_CONTINUE;
}

var g_node: AlphNode;

fn main() -> int32 {
  var cfg: AlphConfig;
  alph_config(&cfg);
  if (!alph_node_start(&g_node, &cfg)) {
    return 1;
  }
  var c: Counter;
  c.total = 0;
  var pid: int64 = alph_node_spawn_value<Counter>(&g_node, 0, &c, &counter);
  for i: int64 in 1..4 {
    alph_node_send(&g_node, pid, TAG_ADD, i);
  }
  return (int32)alph_node_wait(&g_node);
}
```

This is [examples/counter.mettle](examples/counter.mettle). The other examples show [requests](examples/request.mettle), [a supervisor](examples/supervised.mettle), [a medic](examples/medic.mettle), [a TCP echo server](examples/echo.mettle), [a limiter with child processes](examples/jobs.mettle) and [two nodes in one process](examples/cluster.mettle).

## Use

Map the runtime in your app's `mettle.deps`, then `import "alph/alph"`:

```text
alph=<path to alph>/src
```

Build with the dev compiler from the Mettle repo, always with `-s`:

```bash
../Mettle/bin/mettle --build -s --release app.mettle
```

Without `-s`, the compiler turns failed checks (bounds, divide) into an inline exit that alph cannot catch.

To use distribution, also `import "alph/net/dist"` and map the cryptex package in `mettle.deps` (`cryptex=<path>/cryptex/src`).

- [Guide](docs/guide.md): how to use the runtime, from starting a node to healing, IO, child processes, pools, shutdown and distribution.
- [API reference](docs/api.md): every function, type and constant, with its inputs, outputs and which thread may call it.

## Test

```bash
./check.sh
```

It checks that `docs/api.md` matches the source, then builds every test and example in debug and release and runs it. After changing a doc comment, run `tools/apidoc.sh` to rebuild the reference.

## Layout

```text
src/alph.mettle             the runtime in one import
src/core/node.mettle        the node: actors, schedulers, messages, links, monitors, requests, timers,
                            names and groups, IO and TCP, child processes, shutdown and signals
src/core/mailbox.mettle     messages and the bounded mailbox
src/core/queue.mettle       intrusive multi-producer queue
src/core/heap.mettle        per-actor heaps and scratch memory
src/heal/supervisor.mettle  supervisors: passive healing
src/heal/medic.mettle       medics: proactive healing
src/heal/dynamic.mettle     dynamic supervisors: children started at run time
src/work/pool.mettle        worker pool: blocking jobs on bound threads
src/work/limiter.mettle     limiter: a counting semaphore as an actor
src/net/port.mettle         port waiter: tells you when a TCP port accepts connections
src/net/dist.mettle         distribution (opt-in): proxies for remote actors, remote send, links and monitors
src/net/noise.mettle        the Noise_XXpsk3 handshake and transport ciphers dist uses (opt-in)
src/sys/linux.mettle        syscall numbers and flags
src/sys/atomic.mettle       64-bit atomics
src/sys/clock.mettle        monotonic time and futex
src/sys/thread.mettle       threads with a guard page and their own signal stack; CPU count
src/sys/recover.mettle      recovery points over the runtime's crash hook
src/sys/fd.mettle           read, write, close, epoll, eventfd and socket wrappers
docs/                       guide and API reference
examples/                   small programs, run by check.sh
tests/                      self-checking test programs
tools/                      apidoc.sh, which builds docs/api.md
```

## License

Apache License 2.0. See [LICENSE](LICENSE).
