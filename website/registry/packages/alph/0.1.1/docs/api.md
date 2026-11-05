# alph runtime API

Every exported function, type and constant, with its doc comment. Built by `tools/apidoc.sh` from `src/`; do not edit by hand.

Functions are grouped by module. Names that take `ctx` run inside a handler; names starting `alph_node_` take the node and work from any thread.

## alph

The alph runtime in one import.

```text
import "alph/alph";
```

Imports are visible through the whole import graph, so this brings in
everything an application uses: the node and its actors, messages,
supervisors and medics, and the standalone event loop alph/io/reactor,
which also works on its own. The x86-64 Linux layer under alph/sys comes along
too; most programs never call it directly.

## alph/io/reactor

An event loop for one thread, with no actors and no
threads of its own. Usable alone: a terminal UI, a monitor, a client or a
small server can run on just this, and alph's schedulers build on it.

A reactor watches file descriptors (sockets, pipes, terminals, anything
epoll takes), keeps timers with nanosecond deadlines, and can be woken
from any thread. alph_rx_wait sleeps until something happens and fills an
array of events; nothing spins, so an idle reactor costs no CPU.

```text
var r: AlphReactor;
alph_rx_init(&r);
alph_rx_add(&r, sock, ALPH_RX_READ, ALPH_RX_ONESHOT, 7);
var t: int64 = alph_rx_timer(&r, 250 * 1000000, 1);  // 250 ms
var ev: AlphRxEvent[64];
while (true) {
  var n: int64 = alph_rx_wait(&r, &ev[0], 64, -1);
  for i: int64 in 0..n {
    if (ev[i].kind == ALPH_RX_FD && ev[i].data == 7) { ... read; alph_rx_modify(...) to re-arm }
    if (ev[i].kind == ALPH_RX_TIMER) { ... ev[i].data == 1 }
  }
}
```

Only alph_rx_wake may be called from another thread; everything else
belongs to the thread that runs the reactor. For many threads, run one
reactor per thread.

Readiness bits, for alph_rx_add and AlphRxEvent.events.

```mettle
export const ALPH_RX_READ: int64 = 1;           // data to read, or a peer to accept
export const ALPH_RX_WRITE: int64 = 4;          // room to write, or a connect finished
export const ALPH_RX_ERROR: int64 = 8;          // an error is pending (always reported)
export const ALPH_RX_HANGUP: int64 = 16;        // hung up (always reported)
export const ALPH_RX_PEER_CLOSED: int64 = 8192; // the peer closed its writing side
```

Arming modes, for alph_rx_add and alph_rx_modify.

```text
ALPH_RX_LEVEL    reported on every wait while ready (simplest)
ALPH_RX_ONESHOT  reported once, then quiet until alph_rx_modify arms it
                 again: one owner handles each readiness exactly once
ALPH_RX_EDGE     reported when it becomes ready; read or write until
                 ALPH_IO_AGAIN before waiting again
```

```mettle
export const ALPH_RX_LEVEL: int64 = 0;
export const ALPH_RX_ONESHOT: int64 = 1;
export const ALPH_RX_EDGE: int64 = 2;
```

Event kinds, in AlphRxEvent.kind.

```mettle
export const ALPH_RX_FD: int64 = 1;      // data = the fd's data; events = what is ready
export const ALPH_RX_TIMER: int64 = 2;   // data = the timer's data; events = its id
export const ALPH_RX_WAKE: int64 = 3;    // alph_rx_wake was called, once or more
export const ALPH_RX_SIGNAL: int64 = 4;  // data = the signal number
```

### `AlphRxEvent`

```mettle
export struct AlphRxEvent {
  kind: int64;
  data: int64;
  events: int64;
}
```

One event from alph_rx_wait.

### `AlphReactor`

```mettle
export struct AlphReactor {
  epfd: int64;
  wakefd: int64;
  timerfd: int64;
  sigfd: int64;
  woken: int64;      // a wake is pending in wakefd
  buf: int64;        // epoll_event[ALPH_RX_BATCH]
  slots: int64;      // AlphRxTimer array
  slot_cap: int64;
  free_top: int64;   // free slot indexes, a stack in free_list
  free_list: int64;
  heap: int64;       // slot indexes, ordered by deadline
  heap_len: int64;
  armed: int64;      // the deadline timerfd is set to, or 0 when disarmed
  pwait2: int64;     // epoll_pwait2: 0 not tried yet, 1 works, -1 missing (the timerfd is used)
}
```

A reactor. Declare one, start it with alph_rx_init, and keep it in place;
its fields are its own.

### `alph_rx_init`

```mettle
export fn alph_rx_init(r: AlphReactor*) -> bool
```

Starts a reactor. Returns false when the kernel refuses an epoll set, an
eventfd or a timerfd, or memory runs out. Thread: any, before use.

### `alph_rx_free`

```mettle
export fn alph_rx_free(r: AlphReactor*)
```

Closes the reactor's own descriptors and frees its memory. The
descriptors it watched stay open; close them yourself. Thread: its owner.

### `alph_rx_add`

```mettle
export fn alph_rx_add(r: AlphReactor*, fd: int64, events: int64, mode: int64, data: int64) -> int64
```

Starts watching fd for events (ALPH_RX_READ, ALPH_RX_WRITE,
ALPH_RX_PEER_CLOSED; errors and hang-ups always come), armed by mode.
data comes back in each of its events and must not be negative. The fd
should be non-blocking (alph_sys_nonblock).

Returns 0, or a negative errno: -17 when fd is already watched, -1 (EPERM)
for a regular file, which epoll cannot watch. Thread: its owner.

### `alph_rx_modify`

```mettle
export fn alph_rx_modify(r: AlphReactor*, fd: int64, events: int64, mode: int64, data: int64) -> int64
```

Changes what fd is watched for, its mode or its data; for a one-shot fd
this arms it again. Returns 0 or a negative errno. Thread: its owner.

### `alph_rx_remove`

```mettle
export fn alph_rx_remove(r: AlphReactor*, fd: int64) -> int64
```

Stops watching fd; call it before closing fd. Returns 0 or a negative
errno. Thread: its owner.

### `alph_rx_timer_at`

```mettle
export fn alph_rx_timer_at(r: AlphReactor*, deadline_ns: int64, data: int64) -> int64
```

Starts a timer that fires once at deadline_ns (alph_now_ns time), with
data. Returns its id (above 0), to cancel it, or 0 when out of memory.
Thread: its owner.

### `alph_rx_timer`

```mettle
export fn alph_rx_timer(r: AlphReactor*, after_ns: int64, data: int64) -> int64
```

Starts a timer that fires once after after_ns nanoseconds. Otherwise as
alph_rx_timer_at. Thread: its owner.

### `alph_rx_cancel`

```mettle
export fn alph_rx_cancel(r: AlphReactor*, id: int64) -> bool
```

Cancels timer id. Returns false when it already fired or was cancelled.
Thread: its owner.

### `alph_rx_cancel_data`

```mettle
export fn alph_rx_cancel_data(r: AlphReactor*, id: int64, data: int64*) -> bool
```

Cancels timer id as alph_rx_cancel, and stores its data in *data, so a
caller that keeps an allocation in the data can free it. Returns false,
leaving *data alone, when it already fired or was cancelled.
Thread: its owner.

### `alph_rx_timers`

```mettle
export fn alph_rx_timers(r: AlphReactor*) -> int64
```

Timers pending. Thread: its owner.

### `alph_rx_wake`

```mettle
export fn alph_rx_wake(r: AlphReactor*)
```

Wakes the reactor: its current or next alph_rx_wait returns an
ALPH_RX_WAKE event. Many wakes before a wait make one event.
Thread: any; this is the one call another thread may make.

### `alph_rx_signals`

```mettle
export fn alph_rx_signals(r: AlphReactor*, mask: int64) -> bool
```

Delivers signals (a mask: bit n for signal n, such as (1 << 28) for
SIGWINCH) as ALPH_RX_SIGNAL events instead of handlers: they are blocked
for the calling thread, so call this before starting other threads, which
inherit the block. Returns false when the kernel refuses. Thread: its
owner.

### `alph_rx_wait`

```mettle
export fn alph_rx_wait(r: AlphReactor*, out: AlphRxEvent*, cap: int64, timeout_ns: int64) -> int64
```

Waits until something happens or timeout_ns passes (-1: no limit; 0:
just look), then fills out with up to cap events: ready fds, timers that
came due, a wake, signals. Timers fire with nanosecond precision, not
epoll's milliseconds: the wait itself takes the deadline (epoll_pwait2,
with the calling thread's timer slack set to 1 ns on its first wait), or
on kernels before 5.11 the timerfd is armed for it. Returns how many events, 0 on a
timeout, or a negative errno (-4 when a signal handler interrupted the
wait and there was nothing to report). Thread: its owner.

## alph/sched/exec

A work-stealing task executor, with no actors.

Worker threads run plain tasks: a function and one word, run to the end.
Each worker keeps its own deque of tasks (Chase-Lev): it pushes and pops
at one end, and idle workers steal from the other, so a busy worker's
backlog spreads without a shared queue. Tasks from outside threads go
through one injection queue. Each worker also owns an alph/io reactor, so
a task can wait for a socket or a timer without blocking its thread: the
reactor runs the next task when the fd is ready or the time comes. Idle
workers spin briefly, then sleep in their reactor; nothing busy-waits.

For compilers, build tools and batch work, groups give fork and join: a
thread waits for a group with alph_group_wait, and a task that needs its
subtasks' results helps run tasks until they are done (alph_task_help).

```text
fn compile(tc: AlphTaskCtx*, unit: int64) -> int64 ... { ...; return 0; }
var ex: AlphExec;
var cfg: AlphExecConfig;
alph_exec_config(&cfg);
alph_exec_start(&ex, &cfg);
var g: AlphGroup;
alph_group_init(&g);
for u: int64 in 0..units { alph_exec_spawn_in(&ex, &g, u, &compile); }
alph_group_wait(&g);
alph_exec_stop(&ex);
```

Tasks are not isolated like actors: a crash in a task ends the process.
Use alph/core/node when work must survive its own failures.

### `AlphExecConfig`

```mettle
export struct AlphExecConfig {
  threads: int64;
  spin_us: int64;
  stack_size: int64;
}
```

How to run an executor. Fill it with alph_exec_config, then change what
you need:

```text
threads     worker threads; default one per CPU
spin_us     how long an idle worker looks for work before it sleeps;
            default 50
stack_size  bytes of stack per worker; default 1 MiB
```

### `AlphTaskCtx`

```mettle
export struct AlphTaskCtx {
  exec: AlphExec*;
  worker: int64;
  events: int64;
  self: int64;
}
```

What a task gets besides its word. Read these fields:

```text
exec    the executor, for alph_exec_ functions
worker  the index of the worker running it
events  for a task started by alph_task_on_ready: what was ready
        (ALPH_RX_READ, ALPH_RX_WRITE, ...); else 0
```

The rest is the runtime's own. ctx is valid until the task returns.

### `AlphGroup`

```mettle
export struct AlphGroup {
  pending: int64;
}
```

A group of tasks to wait for. Start it with alph_group_init; tasks join it
when spawned with an _in function and leave when they return.

### `AlphExec`

```mettle
export struct AlphExec {
  workers: int64;    // AlphWorker array
  count: int64;
  spin_ns: int64;
  stop: int64;
  sleepers: int64;   // workers asleep now
  pending: int64;    // tasks queued or running, for alph_exec_wait_idle
  inj_lock: int64;
  inj: int64;        // AlphJob ring, grown under inj_lock
  inj_head: int64;
  inj_len: int64;
  inj_cap: int64;
}
```

An executor. Declare one, start it with alph_exec_start, and keep it in
place until alph_exec_stop returns. Its fields are the runtime's own.

### `alph_exec_config`

```mettle
export fn alph_exec_config(cfg: AlphExecConfig*)
```

Fills cfg with the defaults listed at AlphExecConfig. Thread: any.

### `alph_exec_start`

```mettle
export fn alph_exec_start(ex: AlphExec*, cfg: AlphExecConfig*) -> bool
```

Starts an executor with cfg->threads workers. Returns false when threads,
reactors or memory cannot be had. Thread: any.

### `alph_exec_spawn`

```mettle
export fn alph_exec_spawn(ex: AlphExec*, arg: int64,
                          f: fn(AlphTaskCtx*, int64) -> int64 with alloc, syscall, asm) -> bool
```

Queues a task from any thread: f(ctx, arg) runs on some worker. Returns
false when memory runs out. Thread: any.

### `alph_exec_spawn_in`

```mettle
export fn alph_exec_spawn_in(ex: AlphExec*, g: AlphGroup*, arg: int64,
                             f: fn(AlphTaskCtx*, int64) -> int64 with alloc, syscall, asm) -> bool
```

As alph_exec_spawn, and the task joins group g (0 for none) until it
returns. Thread: any.

### `alph_task_spawn_in`

```mettle
export fn alph_task_spawn_in(tc: AlphTaskCtx*, g: AlphGroup*, arg: int64,
                             f: fn(AlphTaskCtx*, int64) -> int64 with alloc, syscall, asm) -> bool
```

Queues a subtask from inside a task, on this worker's own deque, where it
runs next unless another worker steals it first (work-first, as Cilk and
Tokio do). It joins group g (0 for none). Returns false when memory runs
out. Thread: a task.

### `alph_task_spawn`

```mettle
export fn alph_task_spawn(tc: AlphTaskCtx*, arg: int64,
                          f: fn(AlphTaskCtx*, int64) -> int64 with alloc, syscall, asm) -> bool
```

As alph_task_spawn_in with no group. Thread: a task.

### `alph_group_init`

```mettle
export fn alph_group_init(g: AlphGroup*)
```

Starts group g empty. Thread: any.

### `alph_group_pending`

```mettle
export fn alph_group_pending(g: AlphGroup*) -> int64
```

Tasks in g not yet finished. Thread: any.

### `alph_group_wait`

```mettle
export fn alph_group_wait(g: AlphGroup*)
```

Blocks the calling thread until every task in g has returned. Not for a
task: a worker blocked here runs nothing; use alph_task_help.
Thread: any thread that is not a worker.

### `alph_task_help`

```mettle
export fn alph_task_help(tc: AlphTaskCtx*, g: AlphGroup*)
```

From inside a task: runs other tasks until every task in g has returned,
so a task can wait for its subtasks without holding up its worker.
Thread: a task.

### `alph_exec_wait_idle`

```mettle
export fn alph_exec_wait_idle(ex: AlphExec*)
```

Blocks the calling thread until no task is queued or running (tasks
waiting on fds or timers do not count). Thread: any thread that is not a
worker.

### `alph_task_on_ready`

```mettle
export fn alph_task_on_ready(tc: AlphTaskCtx*, fd: int64, events: int64, arg: int64,
                             f: fn(AlphTaskCtx*, int64) -> int64 with alloc, syscall, asm) -> int64
```

From inside a task: runs f(ctx, arg) on this worker once fd is ready for
events (ALPH_RX_READ, ALPH_RX_WRITE), with ctx->events saying what is
ready. One shot: call again to wait again, but only after the previous
wait on fd has run (a wait replaced before it runs is lost, with its
memory). fd must be non-blocking and
stay with this worker's tasks while it is watched; alph_rx_remove it from
alph_task_reactor(tc) before closing it. Returns 0, or a negative errno.
Thread: a task.

### `alph_task_after`

```mettle
export fn alph_task_after(tc: AlphTaskCtx*, ns: int64, arg: int64,
                          f: fn(AlphTaskCtx*, int64) -> int64 with alloc, syscall, asm) -> int64
```

From inside a task: runs f(ctx, arg) on this worker after ns nanoseconds.
Returns the timer's id for alph_task_cancel, or 0 when memory runs out.
Thread: a task.

### `alph_task_cancel`

```mettle
export fn alph_task_cancel(tc: AlphTaskCtx*, id: int64) -> bool
```

Cancels a timer from alph_task_after, on the worker that started it.
Returns false when it already ran or was cancelled. Thread: a task on the
same worker.

### `alph_task_reactor`

```mettle
export fn alph_task_reactor(tc: AlphTaskCtx*) -> AlphReactor*
```

This worker's reactor, to add or remove fds directly. Thread: a task.

### `alph_exec_stop`

```mettle
export fn alph_exec_stop(ex: AlphExec*)
```

Stops the executor: workers finish the task they are running and exit,
and this returns once all have. Queued tasks are dropped; call
alph_exec_wait_idle first to let them run. Thread: any thread that is not
a worker.

## alph/core/node

Actors and the node that runs them.

A node is one OS process running a set of schedulers, one thread each. An
actor is a state block, a bounded mailbox, a signal queue and a handler:

```text
fn handler(ctx: AlphCtx*, m: AlphMsg*) -> int64
```

The handler runs one message to completion and returns ALPH_CONTINUE, or
`alph_stop(ctx, reason)` to exit. That return is the only point where a
scheduler switches actors, so a handler must not block or loop without
bound: give it a `where cycles < N` deadline to have the compiler prove
it. The first message every actor gets is ALPH_TAG_INIT.

A crash inside a handler (a failed check, a null or wild pointer, a divide
by zero, a stack overflow) ends only that actor, with ALPH_CRASHED, and its
links and monitors see it as on BEAM. Programs must be built with -s for
failed checks to be caught.

Signals (link, unlink, exit, monitor, demonitor, down, timer) travel on a
separate unbounded queue and are never dropped. Exits and downs keep their
order with messages: each records how far the receiver's mailbox was
filled when it was sent, and waits until those messages are handled. So a
result sent just before an exit is seen before the exit.

Function families: names that take ctx are called from a handler; names
that start alph_node_ take the node and work from any thread. A bare name
sends a tag and one word, _value<T> sends one T, _bytes sends raw bytes.

Exit reasons, carried in m->arg of EXIT and DOWN. Your own reasons start
at ALPH_REASON_USER.

```mettle
export const ALPH_NORMAL: int64 = 0;    // a clean exit; links ignore it
export const ALPH_KILL: int64 = 1;      // send with alph_exit_signal to kill; cannot be trapped
export const ALPH_KILLED: int64 = 2;    // how an actor that was killed exits
export const ALPH_NOPROC: int64 = 3;    // the actor was already gone
export const ALPH_CRASHED: int64 = 4;   // a trap or fault inside the handler
export const ALPH_SHUTDOWN: int64 = 5;  // an orderly stop, from a supervisor
export const ALPH_NOCONNECTION: int64 = 6;  // the actor was on another node, and the connection to it is gone
export const ALPH_REASON_USER: int64 = 100;
```

Tags the runtime delivers. Tags below ALPH_TAG_USER belong to the runtime;
your own tags start at ALPH_TAG_USER.

```mettle
export const ALPH_TAG_INIT: int64 = 1;     // the first message; from = parent (0 if none); arg = restarts so far under a supervisor, else 0
export const ALPH_TAG_BUSY: int64 = 60;
export const ALPH_TAG_EXIT: int64 = 2;     // a linked actor exited; only with alph_trap_exits on. from = it, arg = reason
export const ALPH_TAG_DOWN: int64 = 3;     // a monitored actor exited. from = it, ref = the monitor, arg = reason
export const ALPH_TAG_TIMEOUT: int64 = 4;  // a request got no reply in time. ref = the request, arg = the server
export const ALPH_TAG_CRASHED: int64 = 6;  // to a medic: its patient crashed. from = it, arg = reason
export const ALPH_TAG_IO: int64 = 7;       // an armed handle is ready. ref = the handle, arg = ALPH_IO_ bits
export const ALPH_TAG_IO_GIVEN: int64 = 8; // a handle was handed to this actor. ref = the handle, from = the giver
export const ALPH_TAG_PROC_EXIT: int64 = 9; // a child process ended. ref = its handle, arg = exit code, or 128 + signal
export const ALPH_TAG_LINKED: int64 = 26;   // to a proxy: from linked to it
export const ALPH_TAG_UNLINKED: int64 = 27; // to a proxy: from unlinked from it
export const ALPH_TAG_USER: int64 = 100;
```

What a handler returns.

```mettle
export const ALPH_CONTINUE: int64 = 0;  // keep running
export const ALPH_STOP: int64 = 1;      // exit; return alph_stop(ctx, reason) rather than this
```

What sending returns.

```mettle
export const ALPH_SENT: int64 = 0;   // in the receiver's mailbox
export const ALPH_FULL: int64 = -1;  // the receiver's mailbox is full; nothing was sent
export const ALPH_GONE: int64 = -2;  // the receiver has exited; nothing was sent
export const ALPH_NOMEM: int64 = -3; // no memory for a large payload; nothing was sent
export const ALPH_BUSY: int64 = -4;  // alph_request only: the server sheds load (alph_mailbox_shed); nothing was sent
```

What a handle is ready for, in alph_io_arm and in m->arg of ALPH_TAG_IO.

```mettle
export const ALPH_IO_IN: int64 = 1;    // readable, or a listener has a connection waiting
export const ALPH_IO_OUT: int64 = 4;   // writable, or a connect finished (check alph_io_error)
export const ALPH_IO_ERR: int64 = 8;   // an error is pending; only reported, never armed
export const ALPH_IO_HUP: int64 = 16;  // the other end hung up; only reported, never armed
```

What alph_io_read, alph_io_write and alph_tcp_accept return when they
would have to wait: arm the handle and try again on ALPH_TAG_IO.

```mettle
export const ALPH_IO_AGAIN: int64 = -11;
```

Flags for alph_tcp_listen.

```mettle
export const ALPH_TCP_REUSEPORT: int64 = 1;  // several listeners share the port; the kernel spreads connections
```

Spawn flags; combine with |.

```mettle
export const ALPH_LINK: int64 = 1;     // link the new actor to the caller
export const ALPH_MONITOR: int64 = 2;  // the caller monitors the new actor
export const ALPH_BOUND: int64 = 4;    // the new actor gets a thread of its own and may block
export const ALPH_PROXY: int64 = 8;
```

alph_outq_send writes before the end of the run once this much is held.

```mettle
export const ALPH_OUTQ_HOLD_BYTES: int64 = 16384;
export const ALPH_OUTQ_HOLD_SENDS: int64 = 32;
```

Registry kinds, for alph_node_reg_new.

```mettle
export const ALPH_REG_UNIQUE: int64 = 0;     // a key has at most one holder
export const ALPH_REG_DUPLICATE: int64 = 1;  // a key has any number of holders
```

### `AlphRegEntry`

```mettle
export struct AlphRegEntry {
  pid: int64;
  value: int64;
}
```

One holder of a registry key, as alph_reg_lookup returns it.

### `AlphConfig`

```mettle
export struct AlphConfig {
  schedulers: int64;
  max_actors: int64;
  mailbox_size: int64;
  stack_size: int64;
  heap_limit: int64;
  max_handles: int64;
  signals: int64;
  spin_us: int64;
  crash_reports: int64;
}
```

How to run a node. Fill it with alph_config, then change what you need.

```text
schedulers    scheduler threads; default: one per CPU
max_actors    actors alive at once; default 65536
mailbox_size  messages a mailbox holds before senders get ALPH_FULL,
              unless the actor sets its own with alph_mailbox_limit;
              default 64. Memory grows with what is queued, not with this
stack_size    bytes of stack per scheduler, which is how deep a handler
              may call; default 1 MiB
heap_limit    bytes one actor's heap may hold, state included; past it
              alph_alloc returns 0 and spawning fails. 0 (default) is
              no limit
max_handles   IO handles open at once; default 65536
signals       OS signals the node takes over, as bits (1 << ALPH_SIGINT)
              and so on; default 0. See alph_node_signals
spin_us       how long an idle scheduler keeps checking for work before
              it sleeps; default 2. Work that arrives meanwhile starts
              without a wake-up system call. 0 sleeps at once
crash_reports 1 prints the runtime's full report (stack trace) for every
              actor crash, as a debugging aid; default 0, quiet, because
              a report costs about 60 us of its scheduler's time. Crashes
              are still counted (alph_node_stats) and reach links,
              monitors, medics and supervisors either way
```

### `AlphStats`

```mettle
export struct AlphStats {
  live: int64;
  spawned: int64;
  crashed: int64;
  healed: int64;
  shed: int64;
  rehomed: int64;
  rescued: int64;
  io_demoted: int64;
  active: int64;
  refused: int64;
  yields: int64;
  isolated: int64;
}
```

Counters from alph_node_stats.

```text
live     actors alive now
spawned  actors started since the node started
crashed  crashes that ended an actor
healed   crashes a medic repaired, unseen by anyone else
shed     messages dropped by alph_mailbox_shed for waiting too long
rehomed  wakes that moved an actor to its sender's scheduler, as its
         own was asleep
rescued  schedulers the watchdog woke, or activated, for work stuck
         behind a long handler
io_demoted  handles watched one-shot since their fd was taken with
         alph_io_fd
active   schedulers taking work now (compaction leaves the rest idle)
refused  requests their sender refused at once with ALPH_BUSY, as the
         server was shedding
yields   actor runs cut short after ALPH_RUN_NS, with mail left
isolated  actors cut short twice in a row and so moved to a scheduler
         of their own, away from the work queued behind them
```

### `AlphNode`

```mettle
export struct AlphNode {
  procs: int64;
  capacity: int64;
  mailbox_size: int64;
  free_stack: int64;
  free_top: int64;
  free_lock: int64;
  scheds: int64;
  sched_count: int64;
  next_sched: int64;
  next_ref: int64;
  live: int64;
  spawned: int64;
  crashed: int64;
  healed: int64;
  halted: int64;
  halt_code: int64;
  heap_limit: int64;
  stack_size: int64;
  bound_list: int64;    // every bound scheduler, for alph_node_wait
  sysmon: AlphThread;   // the watchdog, alph_sysmon_main
  sysmon_handle: int64;
  sysmon_word: int64;   // futex word it sleeps on; bumped to wake it
  sysmon_idle: int64;   // 1 while it sleeps long because nothing ran: work starting wakes it
  bound_lock: int64;
  ios: int64;           // AlphIoSlot table
  io_capacity: int64;
  io_free_stack: int64;
  io_free_top: int64;
  io_lock: int64;
  names: int64;         // AlphReg: the default registry, for alph_register
  groups: int64;        // AlphTable of process groups
  names_lock: int64;    // guards groups and every group's members
  env: int64;           // the environment the process started with, for children
  env_lock: int64;
  roots: int64;         // AlphVec of top-level pids, in spawn order
  roots_lock: int64;
  shutting: int64;      // alph_node_shutdown has begun
  sigfd: int64;         // signalfd for cfg.signals, on scheduler 0, or -1
  sig_pid: int64;       // who gets OS signals, or 0 to shut down
  sig_tag: int64;
  spin_ns: int64;       // AlphConfig.spin_us, in nanoseconds
  hungry: int64;        // schedulers with hungry set
  spinners: int64;      // schedulers spinning now
  max_spinners: int64;  // at most this many spin at once
  active: int64;        // schedulers 0..active-1 take new work; the rest drain and sleep
  balance_at: int64;    // when the next look at the load is due (ns)
  balance_lock: int64;
  balanced: int64;      // when the last look was (ns)
  low_streak: int64;    // looks in a row that asked for fewer active schedulers
  low_max: int64;       //   and the most any of them asked for
  rise_at: int64;       // when a busy scheduler last activated another (ns)
  shed: int64;          // messages dropped by alph_mailbox_shed
  rehomed: int64;       // wakes that moved an actor to its sender's scheduler
  rescued: int64;       // schedulers the watchdog woke for work stuck behind a long run
  refused: int64;       // requests refused at once with ALPH_BUSY by their senders
  sysmon_looks: int64;  // times the watchdog woke to look
  io_demoted: int64;    // handles turned one-shot because their fd was taken (alph_io_fd)
  rt_offset: int64;     // CLOCK_REALTIME minus CLOCK_MONOTONIC (ns), refreshed by the watchdog
  yields: int64;        // runs cut short by ALPH_RUN_NS
  isolated: int64;      // CPU-bound actors moved off a busy scheduler after such cuts
  epoch: int64;         // bumped by every retire
  retired: AlphVec;     // pairs: block (bit 0 set: a plain block, else an AlphEntry*), epoch
  retire_lock: int64;
}
```

A node. Declare one, start it with alph_node_start, and keep it in place
until alph_node_wait returns. Its fields are the runtime's own.

### `AlphCtx`

```mettle
export struct AlphCtx {
  self: int64;
  state: rawptr;
  state_size: int64;
  node: AlphNode*;
  proc: int64;
  sched: int64;
  stop_reason: int64;
}
```

What a handler gets besides the message. Read these fields:

```text
self        this actor's pid
state       this actor's state block; cast it to your state type
state_size  its size in bytes
node        the node, for the alph_node_ functions
```

The other fields are the runtime's own. ctx is valid until the handler
returns; do not keep it.

### `alph_send`

```mettle
export fn alph_send(ctx: AlphCtx*, to: int64, tag: int64, arg: int64) -> int64
```

Sends a tag and one word to another actor.

```text
to   the receiver's pid
tag  what the message means; your tags start at ALPH_TAG_USER
arg  one word of data, read as m->arg
```

Returns ALPH_SENT, ALPH_FULL when the receiver's mailbox is full, or
ALPH_GONE when it has exited. Nothing is sent unless it returns ALPH_SENT.
Messages from one sender arrive in the order they were sent.
Thread: a handler. Outside the node, use alph_node_send.

### `alph_send_value`

```mettle
export fn alph_send_value<T>(ctx: AlphCtx*, to: int64, tag: int64, value: T*) -> int64
```

Sends one T, copied, as the payload. The receiver reads it with
alph_msg_value<T>. Returns as alph_send, plus ALPH_NOMEM when a payload
over ALPH_INLINE bytes could not be allocated.
Thread: a handler. Outside the node, use alph_node_send_value.

### `alph_send_bytes`

```mettle
export fn alph_send_bytes(ctx: AlphCtx*, to: int64, tag: int64, data: rawptr, size: int64) -> int64
```

Sends size bytes at data, copied, as the payload: for data whose length is
only known at run time. The receiver reads it with alph_msg_bytes.
Returns as alph_send_value.
Thread: a handler. Outside the node, use alph_node_send_bytes.

### `alph_node_send`

```mettle
export fn alph_node_send(node: AlphNode*, to: int64, tag: int64, arg: int64) -> int64
```

Sends a tag and one word from outside the node; m->from is 0.
Returns as alph_send. Thread: any.

### `alph_node_send_value`

```mettle
export fn alph_node_send_value<T>(node: AlphNode*, to: int64, tag: int64, value: T*) -> int64
```

Sends one T from outside the node. Returns as alph_send_value.
Thread: any.

### `alph_node_send_bytes`

```mettle
export fn alph_node_send_bytes(node: AlphNode*, to: int64, tag: int64, data: rawptr,
                               size: int64) -> int64
```

Sends size bytes from outside the node. Returns as alph_send_value.
Thread: any.

### `alph_backlog`

```mettle
export fn alph_backlog(ctx: AlphCtx*) -> int64
```

How many messages wait in this actor's mailbox, not counting the one
being handled: a handler can batch work (one write for many answers)
while more is queued, and finish it with the last. Thread: a handler.

### `alph_request`

```mettle
export fn alph_request(ctx: AlphCtx*, to: int64, tag: int64, arg: int64, timeout_ms: int64) -> int64
```

Sends a request: a message whose answer comes back with the same ref.

```text
to          the server's pid
tag, arg    as alph_send
timeout_ms  how long to wait for the reply, or 0 to wait for ever
```

Returns the request's ref (above 0), or ALPH_FULL or ALPH_NOMEM when the
request was not sent, or ALPH_BUSY when the server sheds load
(alph_mailbox_shed) and refusing it here costs the server nothing:
answer your own client at once, or try later or elsewhere. After a ref,
exactly one of these arrives, carrying it:

```text
the reply           tag and payload as the server sent them
ALPH_TAG_DOWN       the server exited first, or was already gone;
                    arg = its exit reason
ALPH_TAG_TIMEOUT    no reply within timeout_ms; arg = the server
ALPH_TAG_BUSY       the server shed it unhandled for waiting too long
                    (alph_mailbox_shed): overloaded, try later
```

Anything later for that ref is dropped, so a late reply never surprises
the requester. alph_demonitor(ctx, ref) gives up on a request: nothing
for that ref arrives after it. The server answers with alph_reply.
Thread: a handler.

### `alph_request_at`

```mettle
export fn alph_request_at(ctx: AlphCtx*, to: int64, tag: int64, arg: int64, timeout_ms: int64,
                          sent_at: int64) -> int64
```

As alph_request, for a request that really arrived at sent_at (monotonic
ns, as alph_now_ns; from alph_io_recv for one read from a socket): a
server that sheds load (alph_mailbox_shed) counts its wait from then,
so time spent in the kernel's buffers or the caller's queue counts too.
Thread: a handler.

### `alph_request_value`

```mettle
export fn alph_request_value<T>(ctx: AlphCtx*, to: int64, tag: int64, value: T*,
                                timeout_ms: int64) -> int64
```

Sends a request carrying one T. Otherwise as alph_request.
Thread: a handler.

### `alph_request_bytes`

```mettle
export fn alph_request_bytes(ctx: AlphCtx*, to: int64, tag: int64, data: rawptr, size: int64,
                             timeout_ms: int64) -> int64
```

Sends a request carrying size bytes. Otherwise as alph_request.
Thread: a handler.

### `alph_reply`

```mettle
export fn alph_reply(ctx: AlphCtx*, request: AlphMsg*, tag: int64, arg: int64) -> int64
```

Answers a request with a tag and one word.

```text
request  the request message as the handler received it
```

The reply goes to the requester with the request's ref. Returns as
alph_send, except that a full mailbox never refuses a reply: the
requester's own outstanding requests bound how many it can get, so
replies get through even when it is flooded with other work. A requester
that already timed out or exited drops it. Thread: a handler.

### `alph_reply_value`

```mettle
export fn alph_reply_value<T>(ctx: AlphCtx*, request: AlphMsg*, tag: int64, value: T*) -> int64
```

Answers a request with one T. Otherwise as alph_reply.
Thread: a handler.

### `alph_reply_bytes`

```mettle
export fn alph_reply_bytes(ctx: AlphCtx*, request: AlphMsg*, tag: int64, data: rawptr,
                           size: int64) -> int64
```

Answers a request with size bytes. Otherwise as alph_reply.
Thread: a handler.

### `alph_reply_full`

```mettle
export fn alph_reply_full(ctx: AlphCtx*, request: AlphMsg*, tag: int64, arg: int64, data: rawptr,
                          size: int64) -> int64
```

Answers a request with a tag, one word and size bytes, for replies that
carry both a result code and data. Otherwise as alph_reply.
Thread: a handler.

### `alph_node_forward`

```mettle
export fn alph_node_forward(node: AlphNode*, to: int64, from: int64, tag: int64, arg: int64,
                            ref: int64, flags: int64, data: rawptr, size: int64) -> int64
```

Posts a message as if from actor from, with the request or reply marks
of another node's message: flags is ALPH_MSG_FLAG_REQUEST,
ALPH_MSG_FLAG_REPLY or 0, and ref is that message's ref. For
alph/net/dist, which delivers messages that crossed a connection; other
code uses alph_send and friends. Returns as alph_send_bytes.
Thread: any.

The request and reply marks of a message, for m->flags and
alph_node_forward.

```mettle
export const ALPH_MSG_FLAG_REQUEST: int64 = 1;
export const ALPH_MSG_FLAG_REPLY: int64 = 2;
```

### `alph_link`

```mettle
export fn alph_link(ctx: AlphCtx*, pid: int64)
```

Links this actor with pid, both ways: when either exits with a reason
other than ALPH_NORMAL, the other exits with the same reason, or gets an
ALPH_TAG_EXIT message if it traps exits. Linking to a gone actor acts as
if it had just exited with ALPH_NOPROC. Linking twice is one link.
Thread: a handler.

### `alph_unlink`

```mettle
export fn alph_unlink(ctx: AlphCtx*, pid: int64)
```

Removes the link with pid, both ways. Does nothing if there is none.
Thread: a handler.

### `alph_monitor`

```mettle
export fn alph_monitor(ctx: AlphCtx*, pid: int64) -> int64
```

Watches pid, one way. When it exits, an ALPH_TAG_DOWN message arrives with
the returned ref and its exit reason in arg; if it is already gone, DOWN
arrives with ALPH_NOPROC. Each call is a separate monitor.
Returns the monitor's ref. Thread: a handler.

### `alph_demonitor`

```mettle
export fn alph_demonitor(ctx: AlphCtx*, ref: int64)
```

Stops a monitor. A DOWN for it that is already on its way is dropped.

```text
ref  from alph_monitor
```

Thread: a handler.

### `alph_trap_exits`

```mettle
export fn alph_trap_exits(ctx: AlphCtx*, on: bool)
```

With on, exits of linked actors arrive as ALPH_TAG_EXIT messages (from =
the linked actor, arg = its reason) instead of ending this actor. Kills
still end it. Off by default. Thread: a handler.

### `alph_exit_signal`

```mettle
export fn alph_exit_signal(ctx: AlphCtx*, pid: int64, reason: int64)
```

Sends an exit signal to pid, as erlang:exit/2. The target exits with
reason unless it traps exits, in which case it gets an ALPH_TAG_EXIT
message. ALPH_KILL cannot be trapped: the target exits with ALPH_KILLED.
ALPH_NORMAL is ignored by an actor that does not trap exits.
Thread: a handler. Outside the node, use alph_node_exit_signal.

### `alph_node_exit_signal`

```mettle
export fn alph_node_exit_signal(node: AlphNode*, pid: int64, reason: int64)
```

Sends an exit signal from outside the node, as alph_exit_signal; the
message's from is 0. Thread: any.

### `alph_stop`

```mettle
export fn alph_stop(ctx: AlphCtx*, reason: int64) -> int64
```

Ends this actor once the handler returns: `return alph_stop(ctx, reason);`
Its links and monitors see reason. Returns ALPH_STOP. Thread: a handler.

### `alph_exit_now`

```mettle
export fn alph_exit_now(reason: int64)
```

Ends the running actor at once, from any call depth inside its handler,
as if the handler had returned alph_stop(reason). Frames between here and
the handler are abandoned: their defers do not run. The actor's heap is
freed as on any exit. Use it for errors found deep in a call chain; a
handler can just return alph_stop. Does nothing, and returns, when called
outside a handler. Thread: a handler, at any depth.

### `alph_parent`

```mettle
export fn alph_parent(ctx: AlphCtx*) -> int64
```

The pid of the actor that spawned this one, or 0 when the node did.
Thread: a handler.

### `alph_mailbox_limit`

```mettle
export fn alph_mailbox_limit(ctx: AlphCtx*, limit: int64)
```

Sets how many messages this actor's mailbox holds before senders get
ALPH_FULL, in place of AlphConfig.mailbox_size. An actor many others talk
to, such as one that owns a store, raises it; memory grows only with what
is actually queued. Messages already queued stay.

```text
limit  at least 1
```

Thread: a handler.

### `alph_on_exit`

```mettle
export fn alph_on_exit(ctx: AlphCtx*, hook: fn(AlphCtx*, int64) -> int64 with alloc, syscall, asm)
```

Sets a hook the runtime calls once when this actor exits, before its
links and monitors are told and its handles close: for state tied to the
actor's life, such as counters, files or a lease to give back. It runs
on every exit, crash included, except a kill (ALPH_KILL), which stays the
hard stop. It gets a ctx like a handler's, with the actor's state, and
the exit reason; it may send, but its own return value is ignored. A
crash inside it ends only the hook. Not run when a medic gives up on its
patient. Setting it again replaces it.

```text
hook  fn(ctx, reason) -> int64
```

Thread: a handler.

### `alph_continue`

```mettle
export fn alph_continue(ctx: AlphCtx*, tag: int64, arg: int64)
```

Delivers tag and arg to this actor before any message already in its
mailbox, as OTP's handle_continue: INIT can answer quickly and finish its
setup next, before other work gets in. Signals (exits, timers) may still
come first. Thread: a handler.

### `alph_mailbox_shed`

```mettle
export fn alph_mailbox_shed(ctx: AlphCtx*, target_us: int64, interval_us: int64)
```

Sheds requests by waiting time, as Facebook's adaptive CoDel does for RPC
queues: once the requests this actor takes have waited longer than
target_us continuously for interval_us, with the queue never emptying,
it drops every one still above the target unhandled, so its queueing
delay stays near target_us and its time goes to work that can still
succeed. A dropped request is answered at once with ALPH_TAG_BUSY (ref =
the request's, arg = how long it waited in us), and every drop counts in
alph_node_stats' shed. While it drops, or while its waits are above the
target and it has taken nothing for longer than that, alph_request to it
returns ALPH_BUSY at once without sending (stats: refused), so turning
requests away costs the overloaded actor nothing. Only requests are shed: plain sends need
alph_mailbox_shed_casts, and replies, DOWN, TIMEOUT and EXIT messages
never are. Senders stamp their messages with the time while this is on:
one clock read per send. target_us 0 turns it off.
For example alph_mailbox_shed(ctx, 5000, 100000): 5 ms, 100 ms.
Thread: a handler.

### `alph_mailbox_shed_casts`

```mettle
export fn alph_mailbox_shed_casts(ctx: AlphCtx*, on: bool)
```

With on, alph_mailbox_shed also drops plain user messages (alph_send and
friends) that waited too long, silently: a sender learns nothing, so use
it only where losing a message is fine, such as telemetry or gossip.
Off by default; alph_mailbox_shed(ctx, 0, 0) turns it off too.
Thread: a handler.

### `alph_send_after`

```mettle
export fn alph_send_after(ctx: AlphCtx*, ms: int64, to: int64, tag: int64, arg: int64)
```

Sends a tag and one word to `to` after ms milliseconds. It cannot be
cancelled; use alph_start_timer for that. Like every signal it is never
dropped for a full mailbox. Thread: a handler.

### `alph_start_timer`

```mettle
export fn alph_start_timer(ctx: AlphCtx*, ms: int64, tag: int64, arg: int64) -> int64
```

Starts a timer for this actor: after ms milliseconds a message with tag
and arg arrives, its ref set to the returned value, unless
alph_cancel_timer is called first. Returns the timer's ref.
Thread: a handler.

### `alph_cancel_timer`

```mettle
export fn alph_cancel_timer(ctx: AlphCtx*, ref: int64)
```

Cancels a timer from alph_start_timer. Its message never arrives, even if
it is already due. Cancelling twice, or after it arrived, does nothing.
Thread: a handler.

### `alph_alloc`

```mettle
export fn alph_alloc(ctx: AlphCtx*, size: int64) -> rawptr
```

Allocates size bytes from this actor's own heap: 16-byte aligned, not
zeroed, and only this actor's to use. No other actor shares the heap, so
no lock is taken. Free it with alph_free, or leave it: everything is freed
when the actor exits, even by a crash. Returns 0 when the heap limit
(AlphConfig.heap_limit) would be passed or memory ran out.
Thread: a handler.

### `alph_free`

```mettle
export fn alph_free(ctx: AlphCtx*, ptr: rawptr)
```

Frees memory from alph_alloc or alph_realloc on this actor's heap.
Freeing 0 does nothing. Thread: a handler.

### `alph_realloc`

```mettle
export fn alph_realloc(ctx: AlphCtx*, ptr: rawptr, size: int64) -> rawptr
```

Resizes memory from alph_alloc, keeping its contents up to the smaller of
the two sizes. ptr 0 allocates. Returns the new address, which may differ,
or 0 when there is no room, in which case ptr is left as it was.
Thread: a handler.

### `alph_scratch`

```mettle
export fn alph_scratch(ctx: AlphCtx*, size: int64) -> rawptr
```

Allocates size bytes that are valid until this handler call returns, then
freed all at once at no cost. Use it for temporaries: parsing a request,
building a reply. Do not keep the address. Returns 0 when memory ran out.
Thread: a handler.

### `alph_heap_used`

```mettle
export fn alph_heap_used(ctx: AlphCtx*) -> int64
```

Bytes this actor's heap holds now, its state included.
Thread: a handler.

### `alph_spawn`

```mettle
export fn alph_spawn(ctx: AlphCtx*, flags: int64,
                     handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Spawns an actor with no state.

```text
flags    0, or any of ALPH_LINK, ALPH_MONITOR and ALPH_BOUND. A link
         and a monitor are set up before the new actor runs, so even a
         crash in its INIT reaches the caller; with ALPH_MONITOR its
         DOWN arrives with from = the new pid. ALPH_BOUND gives the new
         actor a thread of its own, so its handler may block (file IO,
         fsync, a long computation) without holding up other actors.
handler  the new actor's handler; its first message is ALPH_TAG_INIT
```

Returns the new pid, or 0 when the node is full (max_actors) or out of
memory. Thread: a handler. Outside the node, use alph_node_spawn.

### `alph_spawn_value`

```mettle
export fn alph_spawn_value<T>(ctx: AlphCtx*, flags: int64, state: T*,
                              handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Spawns an actor whose state starts as a copy of *state. The handler reads
it as (T*)ctx->state. Otherwise as alph_spawn. Thread: a handler.

### `alph_spawn_bytes`

```mettle
export fn alph_spawn_bytes(ctx: AlphCtx*, flags: int64, state: rawptr, size: int64,
                           handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Spawns an actor whose state starts as a copy of size bytes at state.
Otherwise as alph_spawn. Thread: a handler.

### `alph_spawn_bytes_arg`

```mettle
export fn alph_spawn_bytes_arg(ctx: AlphCtx*, flags: int64, state: rawptr, size: int64, arg: int64,
                               handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Spawns an actor whose INIT message carries arg, so it can tell how it was
started. Supervisors pass the number of times the child was started
before: 0 the first time, more on a restart. Otherwise as
alph_spawn_bytes. Thread: a handler.

### `alph_node_spawn`

```mettle
export fn alph_node_spawn(node: AlphNode*, flags: int64,
                          handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Spawns an actor with no state from outside the node. It has no parent: its
INIT comes from 0 and nothing is linked to it.

```text
flags  0, or ALPH_BOUND for an actor with a thread of its own
```

Returns the new pid, or 0. Thread: any.

### `alph_node_spawn_value`

```mettle
export fn alph_node_spawn_value<T>(node: AlphNode*, flags: int64, state: T*,
                                   handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Spawns an actor whose state starts as a copy of *state, from outside the
node. Otherwise as alph_node_spawn. Thread: any.

### `alph_node_spawn_bytes`

```mettle
export fn alph_node_spawn_bytes(node: AlphNode*, flags: int64, state: rawptr, size: int64,
                                handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Spawns an actor whose state starts as a copy of size bytes at state, from
outside the node. Otherwise as alph_node_spawn. Thread: any.

### `alph_node_reg_new`

```mettle
export fn alph_node_reg_new(node: AlphNode*, kind: int64) -> int64
```

Makes a registry: a table of keys held by actors, each holder with a
value, like Elixir's Registry. Lookups from handlers take no lock and
never wait for a writer; a holder's keys are dropped when it exits, crash
included, before its links and monitors are told.

```text
kind  ALPH_REG_UNIQUE: a key has at most one holder (names, sessions);
      ALPH_REG_DUPLICATE: any number (subscribers, roles)
```

Returns the registry, to pass to the alph_reg_ functions, or 0 when out
of memory. A registry lives as long as the node. Thread: any.

### `alph_reg_register`

```mettle
export fn alph_reg_register(ctx: AlphCtx*, reg: int64, key: string, value: int64) -> bool
```

Registers key for the calling actor with value, in registry reg. The key
is copied. In a unique registry it fails when another live actor holds
key, and succeeds without change when the caller already does; in a
duplicate one every call adds an entry, so an actor may hold a key twice.

Returns true when registered, false when taken, key is empty, or memory
runs out. Thread: a handler.

### `alph_reg_unregister`

```mettle
export fn alph_reg_unregister(ctx: AlphCtx*, reg: int64, key: string)
```

Drops every entry the calling actor holds for key in reg; does nothing
when it holds none. Thread: a handler.

### `alph_reg_update`

```mettle
export fn alph_reg_update(ctx: AlphCtx*, reg: int64, key: string, value: int64) -> bool
```

Sets the value of every entry the calling actor holds for key in reg.
Returns false when it holds none. Thread: a handler.

### `alph_reg_lookup`

```mettle
export fn alph_reg_lookup(ctx: AlphCtx*, reg: int64, key: string, out: AlphRegEntry*, cap: int64) -> int64
```

The holders of key in reg: copies up to cap of them into out and returns
how many there are, which may be more than cap. Each may exit at any
time after. Takes no lock. Thread: a handler.

### `alph_reg_whereis`

```mettle
export fn alph_reg_whereis(ctx: AlphCtx*, reg: int64, key: string) -> int64
```

The holder of key in reg (the first one, in a duplicate registry), or 0.
Takes no lock. Thread: a handler.

### `alph_reg_send`

```mettle
export fn alph_reg_send(ctx: AlphCtx*, reg: int64, key: string, tag: int64, arg: int64) -> int64
```

Sends a tag and one word to every holder of key in reg, each as
alph_send, with no lock held while sending. Returns how many got it, or
ALPH_GONE when key has no holder. Thread: a handler.

### `alph_reg_dispatch`

```mettle
export fn alph_reg_dispatch(ctx: AlphCtx*, reg: int64, key: string, mask: int64, tag: int64,
                            arg: int64) -> int64
```

As alph_reg_send, but only to holders whose value has a bit of mask set,
so a value can carry roles or interests (Elixir's dispatch). mask -1
reaches every holder. Returns how many got it, or ALPH_GONE when none
matched. Thread: a handler.

### `alph_node_reg_count`

```mettle
export fn alph_node_reg_count(node: AlphNode*, reg: int64) -> int64
```

Entries in reg: one per holder per key, roughly while actors come and
go. Thread: any.

### `alph_register`

```mettle
export fn alph_register(ctx: AlphCtx*, name: string) -> bool
```

Registers name for the calling actor, so others can find it with
alph_whereis. An actor may hold several names. The name is copied; it is
dropped when the actor exits, crash included, or by alph_unregister.

Returns true when the name is now the caller's (also when it already
was), false when another live actor holds it, name is empty, or memory
runs out. Thread: a handler.

### `alph_unregister`

```mettle
export fn alph_unregister(ctx: AlphCtx*, name: string)
```

Drops name if the calling actor holds it; otherwise does nothing.
Thread: a handler.

### `alph_node_whereis`

```mettle
export fn alph_node_whereis(node: AlphNode*, name: string) -> int64
```

The pid registered as name, or 0 when there is none. The actor may exit
at any time after, so a send to the pid can still return ALPH_GONE.
Thread: any.

### `alph_whereis`

```mettle
export fn alph_whereis(ctx: AlphCtx*, name: string) -> int64
```

The pid registered as name, or 0; as alph_node_whereis, but from a
handler it takes no lock. Thread: a handler.

### `alph_group_join`

```mettle
export fn alph_group_join(ctx: AlphCtx*, group: string) -> bool
```

Adds the calling actor to the process group called group, creating the
group if needed. The name is copied. The actor leaves when it exits,
crash included, or by alph_group_leave; a group with no members is gone.

Returns true when the caller is a member (also when it already was),
false when group is empty or memory runs out. Thread: a handler.

### `alph_group_leave`

```mettle
export fn alph_group_leave(ctx: AlphCtx*, group: string)
```

Takes the calling actor out of group; does nothing if it is not a
member. Thread: a handler.

### `alph_node_group_size`

```mettle
export fn alph_node_group_size(node: AlphNode*, group: string) -> int64
```

How many actors are in group now; 0 when there is no such group.
Thread: any.

### `alph_node_group_send`

```mettle
export fn alph_node_group_send(node: AlphNode*, group: string, tag: int64, arg: int64) -> int64
```

Sends a tag and one word to every member of group from outside the node,
with from = 0, each as alph_node_send. Returns how many members got it; a
full mailbox or an exited member is skipped. Thread: any.

### `alph_group_send`

```mettle
export fn alph_group_send(ctx: AlphCtx*, group: string, tag: int64, arg: int64) -> int64
```

Sends a tag and one word to every member of group, the caller too if it
is one, each as alph_send. Returns how many members got it; a full
mailbox or an exited member is skipped. Thread: a handler.

### `alph_group_send_value`

```mettle
export fn alph_group_send_value<T>(ctx: AlphCtx*, group: string, tag: int64, value: T*) -> int64
```

Sends one T, copied for each member, to every member of group, as
alph_send_value. Returns as alph_group_send. Thread: a handler.

### `alph_group_send_bytes`

```mettle
export fn alph_group_send_bytes(ctx: AlphCtx*, group: string, tag: int64, data: rawptr,
                                size: int64) -> int64
```

Sends size bytes at data, copied for each member, to every member of
group, as alph_send_bytes. Returns as alph_group_send. Thread: a handler.

### `alph_io_open`

```mettle
export fn alph_io_open(ctx: AlphCtx*, fd: int64) -> int64
```

Makes the calling actor the owner of fd: it is made non-blocking, put on
this scheduler's epoll set, and closed when the actor exits, crash
included, unless it is closed or given away first. Any descriptor works;
one epoll cannot watch (a regular file) is still owned and closed, but
alph_io_arm refuses it.

```text
fd  an open file descriptor; the handle owns it from now on
```

Returns the handle (above 0), or a negative errno when fd is invalid, or
ALPH_NOMEM when max_handles are open. Thread: a handler.

### `alph_io_arm`

```mettle
export fn alph_io_arm(ctx: AlphCtx*, h: int64, events: int64) -> int64
```

Asks for one ALPH_TAG_IO message the next time handle h is ready for
events (ALPH_IO_IN, ALPH_IO_OUT, or both). The message's ref is h and its
arg says what is ready, which may include ALPH_IO_ERR or ALPH_IO_HUP. Arm
again after each message to hear about the next time: one message
disarms every direction, as an epoll one-shot does.

Arming costs no system call: the handle stays on its epoll set,
edge-triggered, and what became ready while unarmed is kept and reported
at once. After a read through alph_io_read that left data behind (or a
write through alph_io_write that wrote everything), it is reported at
once as well, so reading part of what is there and arming again works as
with a level. A message may then find nothing after all: the read returns
ALPH_IO_AGAIN, and arming again waits for more.

Returns 0, or a negative errno: -9 when this actor does not own h, -1
when h cannot be watched (a regular file). Thread: a handler.

### `alph_io_read`

```mettle
export fn alph_io_read(ctx: AlphCtx*, h: int64, buf: rawptr, size: int64) -> int64
```

Reads up to size bytes from handle h into buf, without blocking.
Returns the count, 0 at end of stream (the peer closed), ALPH_IO_AGAIN
when nothing is there yet (arm ALPH_IO_IN and try again), or a negative
errno (-9 when this actor does not own h). Thread: a handler.

### `alph_io_stamps`

```mettle
export fn alph_io_stamps(ctx: AlphCtx*, h: int64, on: bool) -> int64
```

Turns on (or off) kernel arrival stamps for handle h, a socket, for
alph_io_recv. Turning them on for any socket makes the kernel stamp every
packet the host receives (a clock read each): small, but host-wide, so it
is off unless asked for. Returns 0 or a negative errno. Thread: a handler.

### `alph_io_recv`

```mettle
export fn alph_io_recv(ctx: AlphCtx*, h: int64, buf: rawptr, size: int64, from_ns: int64*,
                       to_ns: int64*) -> int64
```

As alph_io_read, and says when what it read arrived: every byte arrived
at or after *from_ns and at or before *to_ns (monotonic ns, as
alph_now_ns). For a stream the kernel stamps the last packet read
(*to_ns); bytes before it may be older, back to *from_ns: the end of the
last read when that one emptied the socket, else the last packet it
read. A batch of requests can be given times between the two, the oldest
first. Without stamps (alph_io_stamps) both are the time of this call.
Thread: a handler.

### `alph_io_write`

```mettle
export fn alph_io_write(ctx: AlphCtx*, h: int64, buf: rawptr, size: int64) -> int64
```

Writes up to size bytes from buf to handle h, without blocking. Returns
the count written, which may be less than size, ALPH_IO_AGAIN when none
fit (arm ALPH_IO_OUT and try again), or a negative errno. For whole
messages, an AlphOutq does the bookkeeping. Thread: a handler.

### `alph_io_close`

```mettle
export fn alph_io_close(ctx: AlphCtx*, h: int64)
```

Closes handle h and its descriptor now. Does nothing when this actor does
not own h. Thread: a handler.

### `alph_io_fd`

```mettle
export fn alph_io_fd(ctx: AlphCtx*, h: int64) -> int64
```

The descriptor behind handle h, for calls alph has no wrapper for, or -9
when this actor does not own h. Do not close it yourself; use
alph_io_close. From then on h is watched one-shot, re-armed by a system
call on each alph_io_arm, since reads and writes alph does not see would
break its edge bookkeeping (alph_node_stats counts these as io_demoted).
Thread: a handler.

### `alph_io_give`

```mettle
export fn alph_io_give(ctx: AlphCtx*, h: int64, to: int64) -> int64
```

Hands handle h to the actor `to`, which becomes its owner and gets an
ALPH_TAG_IO_GIVEN message (ref = h, from = this actor). Any arming is
dropped; the new owner arms it. If `to` exits before taking it, it is
closed. Use it to pass an accepted connection to the actor that serves it.

Returns ALPH_SENT, ALPH_GONE when `to` has exited (h stays with this
actor), or -9 when this actor does not own h. Thread: a handler.

### `alph_io_error`

```mettle
export fn alph_io_error(ctx: AlphCtx*, h: int64) -> int64
```

The pending error on a socket handle, 0 when there is none: after a
connect reports ALPH_IO_OUT, 0 means it is connected. Returns a positive
errno, or -9 when this actor does not own h. Thread: a handler.

### `alph_ip4`

```mettle
export fn alph_ip4(a: int64, b: int64, c: int64, d: int64) -> int64
```

An IPv4 address in host order, for the alph_tcp_ functions:
alph_ip4(127, 0, 0, 1). Thread: any.

### `alph_tcp_listen`

```mettle
export fn alph_tcp_listen(ctx: AlphCtx*, ip4: int64, port: int64, backlog: int64, flags: int64) -> int64
```

Opens a TCP listener owned by the calling actor.

```text
ip4      the address to listen on: alph_ip4(0, 0, 0, 0) for all
port     the port, or 0 to let the system pick (see alph_tcp_port)
backlog  connections the kernel queues before accept
flags    0, or ALPH_TCP_REUSEPORT so several actors (one per
         scheduler) can each listen on the same port
```

Arm it with ALPH_IO_IN; on ALPH_TAG_IO, call alph_tcp_accept until it
returns ALPH_IO_AGAIN, then arm again. Returns the handle, or a negative
errno (-98 when the address is in use). Thread: a handler.

### `alph_tcp_accept`

```mettle
export fn alph_tcp_accept(ctx: AlphCtx*, listener: int64) -> int64
```

Accepts one waiting connection on a listener handle. The new connection
is a handle owned by the calling actor; give it to another actor with
alph_io_give. Returns the handle, ALPH_IO_AGAIN when none is waiting, or
a negative errno. Thread: a handler.

### `alph_tcp_connect`

```mettle
export fn alph_tcp_connect(ctx: AlphCtx*, ip4: int64, port: int64) -> int64
```

Starts connecting to ip4:port. Returns the handle at once, still
connecting: arm it with ALPH_IO_OUT, and on ALPH_TAG_IO check
alph_io_error, which is 0 once connected. Returns a negative errno when
it fails at once. Thread: a handler.

### `alph_tcp_port`

```mettle
export fn alph_tcp_port(ctx: AlphCtx*, h: int64) -> int64
```

The local port of a socket handle: the port a listener opened with port
0 got. Returns a negative errno on failure. Thread: a handler.

### `alph_tcp_peer`

```mettle
export fn alph_tcp_peer(ctx: AlphCtx*, h: int64, ip4: int64*, port: int64*) -> int64
```

The address of the other end of a connected TCP handle: *ip4 in host
order (alph_ip4(127, 0, 0, 1) for loopback) and *port. For logs and
access rules. Returns 0, or a negative errno: -107 (ENOTCONN) when it is
not connected, -9 when this actor does not own h. Thread: a handler.

### `alph_tcp_local`

```mettle
export fn alph_tcp_local(ctx: AlphCtx*, h: int64, ip4: int64*, port: int64*) -> int64
```

The local address of a TCP handle, as alph_tcp_peer: the address and port
a connection arrived on, or a listener listens on. Returns 0 or a
negative errno. Thread: a handler.

### `alph_tcp_nodelay`

```mettle
export fn alph_tcp_nodelay(ctx: AlphCtx*, h: int64, on: bool) -> int64
```

Turns Nagle's algorithm off (on true) for a TCP handle, so small writes
go out at once. Returns 0 or a negative errno. Thread: a handler.

### `AlphOutq`

```mettle
export struct AlphOutq {
  data: rawptr;
  len: int64;    // bytes in data
  sent: int64;   // bytes of data already written
  cap: int64;
  limit: int64;
  h: int64;      // alph_outq_send: the handle it writes to at the end of the run
  held: int64;   //   sends since the last write
  listed: int64; //   1 while on its actor's list to write at the end of the run
  mark: int64;   //   len when the current handler started: after a crash only bytes before it are trusted
}
```

An output queue: bytes waiting to be written to a handle, kept in the
actor's heap. It takes whole messages and writes what the socket accepts,
so a handler never has to track partial writes. Put it in the actor's
state and start it with alph_outq_init.

```text
limit  bytes it may hold; past it alph_outq_push returns ALPH_FULL,
       which is the signal that the peer reads too slowly
```

### `alph_outq_init`

```mettle
export fn alph_outq_init(q: AlphOutq*, limit: int64)
```

Starts q empty, holding at most limit bytes. Thread: any.

### `alph_outq_send`

```mettle
export fn alph_outq_send(ctx: AlphCtx*, q: AlphOutq*, h: int64, data: rawptr, size: int64) -> int64
```

Queues size bytes at data on q for handle h, and writes them when this
run of the actor ends, together with everything else sent through q
meanwhile: many answers to one connection go out in one write, as Go's
buffered writers and the BEAM's iolists do. It writes early once
ALPH_OUTQ_HOLD_BYTES or ALPH_OUTQ_HOLD_SENDS are held, and before h is
closed or given away; when the actor exits, what it queued is still
written, and when a handler crashes, what earlier handlers queued is, but
not what the crashing one did (it may have been corrupted; a medic retry
sends it again). Use alph_outq_push and alph_outq_flush to
write at once instead. Returns ALPH_SENT, ALPH_FULL (nothing queued) or
ALPH_NOMEM, or a negative errno from an early write.
Thread: the handler of the actor whose state holds q.

### `alph_outq_pending`

```mettle
export fn alph_outq_pending(q: AlphOutq*) -> int64
```

Bytes waiting in q. Thread: any.

### `alph_outq_push`

```mettle
export fn alph_outq_push(ctx: AlphCtx*, q: AlphOutq*, data: rawptr, size: int64) -> int64
```

Copies size bytes at data to the end of q. Returns ALPH_SENT, ALPH_FULL
when that would pass the limit (nothing is queued), or ALPH_NOMEM.
Thread: the handler of the actor whose state holds q.

### `alph_outq_flush`

```mettle
export fn alph_outq_flush(ctx: AlphCtx*, q: AlphOutq*, h: int64) -> int64
```

Writes as much of q to handle h as the socket accepts now. When bytes are
left it arms h for ALPH_IO_OUT, so the next ALPH_TAG_IO with ALPH_IO_OUT
is the time to flush again. Returns the bytes still waiting (0 when all
went out), or a negative errno when the write failed (the connection is
broken). Thread: the handler of the actor whose state holds q.

### `alph_outq_free`

```mettle
export fn alph_outq_free(ctx: AlphCtx*, q: AlphOutq*)
```

Frees q's buffer; q is empty afterwards. Thread: the handler of the actor
whose state holds q.

Flags for AlphProcSpec.flags. A stream that is not piped goes to
AlphProcSpec.log when set, else to /dev/null with ALPH_PROC_QUIET, else it
is inherited from the node. stdin is never sent to the log.

```mettle
export const ALPH_PROC_IN: int64 = 1;     // pipe the child's stdin; write to spec.stdin_h
export const ALPH_PROC_OUT: int64 = 2;    // pipe the child's stdout; read from spec.stdout_h
export const ALPH_PROC_ERR: int64 = 4;    // pipe the child's stderr; read from spec.stderr_h
export const ALPH_PROC_MERGE: int64 = 8;  // send stderr into the stdout pipe; needs ALPH_PROC_OUT
export const ALPH_PROC_QUIET: int64 = 16; // streams that are not piped go to /dev/null
export const ALPH_PROC_KEEP_GROUP: int64 = 32; // stay in the node's process group; see alph_proc_spawn
```

### `AlphProcSpec`

```mettle
export struct AlphProcSpec {
  argv: cstring*;
  envp: cstring*;
  cwd: cstring;
  log: cstring;
  flags: int64;
  stdin_h: int64;
  stdout_h: int64;
  stderr_h: int64;
}
```

What to run, for alph_proc_spawn. Set the inputs; alph_proc_spawn fills
the three handles.

```text
argv      the program and its arguments, ending with 0. A program
          name without a slash is looked up in PATH
envp      the environment, ending with 0; 0 means the environment
          this process started with
cwd       the working directory; 0 means the node's
log       a file for stdout and stderr when they are not piped,
          created or emptied (mode 0644); 0 means none
flags     ALPH_PROC_ bits
stdin_h   out: a handle to write the child's stdin, or 0
stdout_h  out: a handle to read the child's stdout, or 0
stderr_h  out: a handle to read the child's stderr, or 0
```

### `alph_proc_spawn`

```mettle
export fn alph_proc_spawn(ctx: AlphCtx*, spec: AlphProcSpec*) -> int64
```

Starts a child process owned by the calling actor, in a process group of
its own; with ALPH_PROC_KEEP_GROUP it stays in the node's group instead,
so a terminal's Ctrl-C and foreground reads reach it (for an app run in
the foreground), and only the child itself, not its own children, is
signalled and killed. The child's end is reported once as ALPH_TAG_PROC_EXIT, ref =
the returned handle, arg = its exit code or 128 + the signal that ended
it (127 when the program could not be run); the handle is closed then.
Piped streams are ordinary IO handles owned by the caller: arm and read
them like sockets, and close them when done.

When the owner exits before the child ends (crash included) or closes the
handle with alph_io_close, the child's whole process group is killed and
the child reaped. The handle can be moved with alph_io_give.

Returns the process handle, or a negative errno with nothing started:
-2 when the program is not found, -22 when argv is empty, -12 when
memory or handles run out, or the errno of opening the log.
Thread: a handler.

### `alph_proc_kill`

```mettle
export fn alph_proc_kill(ctx: AlphCtx*, h: int64, sig: int64) -> int64
```

Sends OS signal sig (15 for SIGTERM, 9 for SIGKILL, ...) to the process
group of the child behind process handle h, or to the child alone when it
was started with ALPH_PROC_KEEP_GROUP. The end still arrives as
ALPH_TAG_PROC_EXIT. A graceful stop is SIGTERM, then a timer, then
SIGKILL.

Returns 0, -9 when the caller does not own h, -3 when the child has
already ended, or another negative errno. Thread: a handler.

### `alph_proc_os_pid`

```mettle
export fn alph_proc_os_pid(ctx: AlphCtx*, h: int64) -> int64
```

The OS process id of the child behind process handle h, or 0 when the
caller does not own h or the child has been reaped. Thread: a handler.

### `alph_spawn_medic`

```mettle
export fn alph_spawn_medic(ctx: AlphCtx*, state: rawptr, size: int64,
                           handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Spawns a medic for the calling actor, which becomes its patient. When the
patient crashes, the medic gets ALPH_TAG_CRASHED (from = the patient,
arg = the reason) and must answer with one of alph_patient_resume, _reset
or _fail. If a live medic is attached already it is kept, since a reset
patient runs INIT again.

```text
state, size  the medic's initial state, copied
handler      the medic's handler
```

Returns the medic's pid, or 0. Thread: the patient's handler.

### `alph_patient_state`

```mettle
export fn alph_patient_state(ctx: AlphCtx*, pid: int64, size: int64*) -> rawptr
```

For a medic: the crashed patient's state as the crash left it, which the
medic may change. *size receives its size. Valid until the patient is
resumed, reset or failed. Returns 0 when pid is not this medic's crashed
patient. Thread: the medic's handler.

### `alph_patient_message`

```mettle
export fn alph_patient_message(ctx: AlphCtx*, pid: int64) -> AlphMsg*
```

For a medic: the message the patient was handling when it crashed. Valid
until the patient is resumed, reset or failed. Returns 0 when pid is not
this medic's crashed patient. Thread: the medic's handler.

### `alph_patient_resume`

```mettle
export fn alph_patient_resume(ctx: AlphCtx*, pid: int64, retry: bool) -> bool
```

For a medic: lets the patient go on with its state as it is now. With
retry, the message it crashed on is handled again first; without, that
message is dropped. Returns false when pid is not this medic's crashed
patient. Thread: the medic's handler.

### `alph_patient_reset`

```mettle
export fn alph_patient_reset(ctx: AlphCtx*, pid: int64, state: rawptr, size: int64) -> bool
```

For a medic: starts the patient over from a copy of size bytes at state.
Its heap is emptied, exits are no longer trapped, and it gets INIT again.
Its pid, links, monitors and mailbox stay. If even the fresh state does
not fit its heap limit, it exits with ALPH_CRASHED instead. Returns false
when pid is not this medic's crashed patient. Thread: the medic's handler.

### `alph_patient_fail`

```mettle
export fn alph_patient_fail(ctx: AlphCtx*, pid: int64, reason: int64) -> bool
```

For a medic: gives up. The patient exits with reason, which its links,
monitors and supervisor see; passive healing takes over. Returns false
when pid is not this medic's crashed patient. Thread: the medic's handler.

### `alph_node_shutdown`

```mettle
export fn alph_node_shutdown(node: AlphNode*, deadline_ms: int64, code: int64) -> bool
```

Stops the node in order. Top-level actors (those from alph_node_spawn and
friends) get ALPH_SHUTDOWN one at a time, newest first, each after the
one before has exited, so a supervisor stops its children before an older
root goes. At deadline_ms, or once the roots are gone, every actor still
alive is killed; then the node halts and alph_node_wait returns code.
Killed actors that own child processes kill and reap them as they exit.
A bound actor blocked for ever in a system call cannot be killed and
keeps alph_node_wait waiting; give blocking calls timeouts.

```text
deadline_ms  time for the orderly part; 0 means no limit
code         what alph_node_wait returns
```

Returns false when a shutdown has already begun. Thread: any.

### `alph_shutdown`

```mettle
export fn alph_shutdown(ctx: AlphCtx*, deadline_ms: int64, code: int64) -> bool
```

Stops the node in order, as alph_node_shutdown. Thread: a handler.

### `alph_node_signals`

```mettle
export fn alph_node_signals(node: AlphNode*, pid: int64, tag: int64)
```

Sends the OS signals the node took over (AlphConfig.signals) to actor
pid as messages: tag, arg = the signal number (ALPH_SIGINT, ALPH_SIGTERM,
ALPH_SIGHUP, ...). They are never dropped. pid 0, the default, or a pid
that has exited, means the node handles them: the first starts
alph_node_shutdown(node, 5000, 128 + signal), a second halts at once.

A node that takes over signals resets each to its default action (a
shell may start a program with SIGINT ignored) and blocks them in the
thread that calls alph_node_start, so call that from main before
starting other threads.
Child processes start with every signal unblocked. Thread: any.

### `alph_config`

```mettle
export fn alph_config(cfg: AlphConfig*)
```

Fills cfg with the defaults: one scheduler per CPU, 65536 actors, 64
messages per mailbox, 1 MiB of stack per scheduler, no heap limit, 65536
IO handles.
Thread: any.

### `alph_node_start`

```mettle
export fn alph_node_start(node: AlphNode*, cfg: AlphConfig*) -> bool
```

Starts a node: its schedulers begin running and actors can be spawned.

```text
node  storage for the node; keep it in place until alph_node_wait returns
cfg   from alph_config, changed as needed; read here and not kept
```

Returns false when cfg is out of range (no schedulers, no actors) or
memory or threads run out.
Thread: any, once per node.

### `alph_node_halt`

```mettle
export fn alph_node_halt(node: AlphNode*, code: int64)
```

Stops the node: each scheduler finishes the actor it is running and
exits, and alph_node_wait returns code. Actors still alive are not told.
Thread: any. From a handler, alph_halt is the same.

### `alph_halt`

```mettle
export fn alph_halt(ctx: AlphCtx*, code: int64)
```

Stops the node, as alph_node_halt. The calling handler still returns
normally. Thread: a handler.

### `alph_node_wait`

```mettle
export fn alph_node_wait(node: AlphNode*) -> int64
```

Waits until the node is halted and its schedulers have stopped. Returns
the code given to alph_halt or alph_node_halt.
Thread: any thread that is not one of the node's schedulers, typically
main, once.

### `alph_node_stats`

```mettle
export fn alph_node_stats(node: AlphNode*, stats: AlphStats*)
```

Copies the node's counters into *stats. Counters from different threads
are read one by one, so they may be a moment apart. Thread: any.

## alph/core/mailbox

Messages, and the bounded mailbox that holds them.

The mailbox is a chain of small segments, as in crossbeam's SegQueue:
senders claim a position with one CAS, the single receiver reads in order,
and a segment is linked when the last one fills. An idle actor holds one
segment of ALPH_SEG cells; a busy one reuses a spare instead of allocating.
Messages from one sender arrive in the order they were sent. A mailbox
holding its limit refuses the message instead of growing, so overload
shows up at the sender as ALPH_FULL rather than as memory growth.

Payloads up to this many bytes travel inside the message. Larger ones cost
one allocation and are freed after the handler returns.

```mettle
export const ALPH_INLINE: int64 = 40;
```

### `AlphMsg`

```mettle
export struct AlphMsg {
  tag: int64;
  from: int64;
  to: int64;
  ref: int64;
  arg: int64;
  flags: int64;
  size: int64;
  heap: int64;      // payload address when size > ALPH_INLINE, else 0
  data: uint8[40];  // payload when size <= ALPH_INLINE
}
```

A message, as a handler receives it. Read the fields directly:

```text
tag    what the message means. Tags below ALPH_TAG_USER (100) belong to
       the runtime; pick your own from ALPH_TAG_USER up
from   the sender's pid, or 0 when sent from outside the node
to     the receiver's pid
ref    the request this message answers (replies, DOWN, TIMEOUT), else 0
arg    one word of data; the exit reason on EXIT and DOWN
size   payload bytes, 0 when there is none
```

The payload is read with alph_msg_value or alph_msg_bytes. It is valid
until the handler returns; copy what you keep.

### `alph_msg_value`

```mettle
export fn alph_msg_value<T>(m: AlphMsg*, out: T*) -> bool
```

Copies the payload into *out when it holds exactly one T.

```text
m    the message
out  where the T goes
```

Returns false, leaving *out alone, when the payload is not sizeof(T)
bytes: usually a tag sent with a different type than the receiver expects.
Thread: the handler that received m.

### `alph_msg_bytes`

```mettle
export fn alph_msg_bytes(m: AlphMsg*, size: int64*) -> rawptr
```

The payload's address. If size is not 0, *size receives its length in
bytes. Returns the address even for an empty payload; check the size.
Thread: the handler that received m.

### `AlphMailbox`

```mettle
export struct AlphMailbox {
  head: int64;   // the segment the receiver reads
  limit: int64;  // messages it holds before senders are refused
  spare: int64;  // an emptied segment kept for reuse, or 0
  stamp: int64;  // 1: senders note when each message was sent (alph_mailbox_stamp)
  // enq and tail are written by every sender, deq by the receiver for every
  // message. On one cache line, each write would take the line from the
  // other side; the padding keeps them 64 bytes apart.
  pad_a: int64[8];
  enq: int64;    // next position to claim; senders
  tail: int64;   // the segment holding position enq; senders
  pad_b: int64[8];
  deq: int64;    // next position to read; the receiver
  pad_c: int64[8];
}
```

A bounded multi-producer, single-consumer queue of messages. Internal to
the runtime; actors never touch it.

Positions only grow. Position pos is cell pos % ALPH_LAP of its segment;
the last position of each lap is an end mark no message uses: a sender
that finds enq there waits until the segment that claimed the last cell
has linked the next one.

### `alph_mailbox_init`

```mettle
export fn alph_mailbox_init(mb: AlphMailbox*, limit: int64) -> bool
```

Prepares an empty mailbox that holds up to limit messages. Returns false
when out of memory. Thread: any, before the mailbox is used.

### `alph_mailbox_set_limit`

```mettle
export fn alph_mailbox_set_limit(mb: AlphMailbox*, limit: int64)
```

Sets how many messages the mailbox holds before senders are refused.
Messages already in it stay. Thread: any.

### `alph_mailbox_count`

```mettle
export fn alph_mailbox_count(mb: AlphMailbox*) -> int64
```

Messages waiting, roughly: senders may be mid-way. Thread: any.

### `alph_mailbox_push`

```mettle
export fn alph_mailbox_push(mb: AlphMailbox*, m: AlphMsg*) -> bool
```

Copies *m into the mailbox. Returns false when it holds its limit, or
when a new segment was needed and memory ran out. Thread: any.

### `alph_mailbox_push_at`

```mettle
export fn alph_mailbox_push_at(mb: AlphMailbox*, m: AlphMsg*, at: int64) -> bool
```

As alph_mailbox_push, noting at (monotonic ns, above 0) instead of now as
the time m was sent, while the mailbox stamps: the time its request
really arrived, before it waited elsewhere. Thread: any.

### `alph_mailbox_push_any`

```mettle
export fn alph_mailbox_push_any(mb: AlphMailbox*, m: AlphMsg*) -> bool
```

As alph_mailbox_push, but past the limit too: for messages the receiver
asked for, such as replies, which its own outstanding requests already
bound. Returns false only when memory runs out. Thread: any.

### `alph_mailbox_stamp`

```mettle
export fn alph_mailbox_stamp(mb: AlphMailbox*, on: bool)
```

Makes senders note when each message was sent (on true), so the receiver
can see how long it waited (alph_mailbox_pop_at). Costs a clock read per
send. Thread: any.

### `alph_mailbox_pop`

```mettle
export fn alph_mailbox_pop(mb: AlphMailbox*, out: AlphMsg*) -> bool
```

Moves the oldest message into *out. Returns false when there is none, or
when the oldest is still being written by its sender.
Thread: the mailbox's one consumer.

### `alph_mailbox_pop_at`

```mettle
export fn alph_mailbox_pop_at(mb: AlphMailbox*, out: AlphMsg*, sent_at: int64*) -> bool
```

As alph_mailbox_pop, and stores in *sent_at when the message was sent (ns), or
0 when the mailbox was not stamping then. Thread: the mailbox's one
consumer.

## alph/heal/supervisor

Passive healing, the OTP way.

A supervisor is an actor that starts its children in order and restarts
them when they exit, by strategy:

```text
ALPH_ONE_FOR_ONE   restart only the child that exited
ALPH_ONE_FOR_ALL   stop every other child, then restart all of them
ALPH_REST_FOR_ONE  stop the children started after it, then restart it
                   and them
```

and by each child's restart type:

```text
ALPH_PERMANENT  always restarted
ALPH_TRANSIENT  restarted unless it exited with ALPH_NORMAL or ALPH_SHUTDOWN
ALPH_TEMPORARY  never restarted
```

A restarted child is a new actor with a new pid and its initial state;
its INIT message's arg counts the starts before it (0 the first time).
More than max_restarts restarts within period_ms and the supervisor gives
up: it stops every child and exits with ALPH_SHUTDOWN, so its own
supervisor sees the failure. Children stop in reverse start order: an
ALPH_SHUTDOWN exit signal, then ALPH_KILL after the child's shutdown_ms.

Unlike OTP, restarts can back off: consecutive restarts wait backoff_ms,
doubling up to backoff_max_ms, and a quiet period_ms resets the streak.

```text
var spec: AlphSupSpec;
alph_sup_init(&spec, ALPH_ONE_FOR_ONE);
alph_sup_add_value<Cache>(&spec, &cache, &cache_handler);
var db: AlphChildSpec* = alph_sup_add(&spec, &db_handler);
db->restart = ALPH_TRANSIENT;
alph_spawn_sup(ctx, &spec);
```

Restart strategies, for AlphSupSpec.strategy; described at the top of
this module.

```mettle
export const ALPH_ONE_FOR_ONE: int64 = 0;
export const ALPH_ONE_FOR_ALL: int64 = 1;
export const ALPH_REST_FOR_ONE: int64 = 2;
```

Restart types, for AlphChildSpec.restart; described at the top of this
module.

```mettle
export const ALPH_PERMANENT: int64 = 0;
export const ALPH_TRANSIENT: int64 = 1;
export const ALPH_TEMPORARY: int64 = 2;
```

Children one supervisor can hold. Nest supervisors for more, or use a
dynamic supervisor (alph/heal/dynamic) for many alike.

```mettle
export const ALPH_SUP_MAX_CHILDREN: int64 = 64;
```

AlphChildSpec.shutdown_ms: wait for the child to stop, however long.

```mettle
export const ALPH_SUP_WAIT: int64 = -1;
```

The tag a sync_start child acknowledges with, through alph_sup_ack.

```mettle
export const ALPH_TAG_ACK: int64 = 10;
```

The request alph_sup_which sends; both supervisor kinds answer it.

```mettle
export const ALPH_TAG_SUP_WHICH: int64 = 44;
```

The request alph_sup_terminate sends: arg = the child's pid.

```mettle
export const ALPH_TAG_SUP_STOP: int64 = 45;
```

The request alph_sup_restart sends: arg = the child's index.

```mettle
export const ALPH_TAG_SUP_RESTART: int64 = 46;
```

A supervisor's note to itself: a child it is stopping is past its
shutdown_ms; arg = the child's pid.

```mettle
export const ALPH_TAG_SUP_KILL: int64 = 47;
```

The answer to alph_sup_which: arg = how many children, payload = that many
AlphChildInfo.

```mettle
export const ALPH_TAG_CHILDREN: int64 = 48;
```

The answer to alph_sup_terminate and alph_sup_restart: arg = 1 (stopped)
or the new pid (restarted), 0 when the supervisor could not do it.

```mettle
export const ALPH_TAG_SUP_ANSWER: int64 = 49;
```

### `AlphChildInfo`

```mettle
export struct AlphChildInfo {
  pid: int64;
  index: int64;
  starts: int64;
}
```

One child, as alph_sup_which lists it.

```text
pid     its pid, 0 while it is not running (stopped by
        alph_sup_terminate, waiting to restart, or a finished temporary
        child of a static supervisor)
index   its place in a static supervisor's spec; -1 in a dynamic one
starts  how many times it was started
```

### `AlphChildSpec`

```mettle
export struct AlphChildSpec {
  restart: int64;
  shutdown_ms: int64;
  sync_start: bool;
  flags: int64;
  sup: int64;  // the AlphSupSpec of a nested supervisor, until spawned, or 0
  state: rawptr;
  state_size: int64;
  state_at: int64;
  handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm;
}
```

One child of a supervisor, as alph_sup_add returns it. Change these:

```text
restart      ALPH_PERMANENT (default), ALPH_TRANSIENT or ALPH_TEMPORARY
shutdown_ms  how long it gets to stop before it is killed; 0 kills it at
             once; ALPH_SUP_WAIT waits as long as it takes (the default
             for a nested supervisor, so it stops its own children).
             once. Default 5000
sync_start   when true, the next child starts only after this one calls
             alph_sup_ack from its INIT handler. Default false
flags        spawn flags besides ALPH_LINK, such as ALPH_BOUND for a
             child that may block. Default 0
```

The other fields are set by alph_sup_add and alph_sup_add_sup.

### `AlphSupSpec`

```mettle
export struct AlphSupSpec {
  strategy: int64;
  max_restarts: int64;
  period_ms: int64;
  backoff_ms: int64;
  backoff_max_ms: int64;
  jitter_pct: int64;
  start_timeout_ms: int64;
  count: int64;
  children: AlphChildSpec[64];
}
```

A supervisor's definition. Start it with alph_sup_init, add children with
alph_sup_add, change these if needed, then spawn it:

```text
strategy          ALPH_ONE_FOR_ONE, ALPH_ONE_FOR_ALL or ALPH_REST_FOR_ONE
max_restarts      restarts allowed within period_ms before giving up;
                  default 1, as OTP (at most 63)
period_ms         default 5000; 0 means no limit, never give up
backoff_ms        wait before the first of consecutive restarts; 0
                  (default) restarts at once
backoff_max_ms    cap on the doubling backoff; 0 means no cap
jitter_pct        each backoff is shortened by a random 0..jitter_pct
                  percent, so supervisors hit by one failure do not all
                  restart in step; 0 (default) is none, 20 is typical
start_timeout_ms  how long a sync_start child has to ack; default 5000
```

count and children are filled by alph_sup_add.

### `alph_sup_init`

```mettle
export fn alph_sup_init(spec: AlphSupSpec*, strategy: int64)
```

Starts a supervisor definition with no children and OTP's defaults: at
most 1 restart in 5000 ms, no backoff, 5000 ms for a child to ack.
Thread: any.

### `alph_sup_add_bytes`

```mettle
export fn alph_sup_add_bytes(spec: AlphSupSpec*, state: rawptr, size: int64,
                             handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> AlphChildSpec*
```

Adds a child whose state starts as a copy of size bytes at state on every
start. The bytes are copied when the supervisor is spawned, so they need
not outlive that call. Returns the child spec, with the defaults
described at AlphChildSpec, or 0 when the supervisor already has
ALPH_SUP_MAX_CHILDREN children. Children start in the order added.
Thread: any.

### `alph_sup_add_value`

```mettle
export fn alph_sup_add_value<T>(spec: AlphSupSpec*, state: T*,
                                handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> AlphChildSpec*
```

Adds a child whose state starts as a copy of *state. Otherwise as
alph_sup_add_bytes. Thread: any.

### `alph_sup_add`

```mettle
export fn alph_sup_add(spec: AlphSupSpec*,
                       handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> AlphChildSpec*
```

Adds a child with no state. Otherwise as alph_sup_add_bytes. Thread: any.

### `alph_sup_add_sup`

```mettle
export fn alph_sup_add_sup(spec: AlphSupSpec*, child: AlphSupSpec*) -> AlphChildSpec*
```

Adds a supervisor as a child, so trees nest as in OTP. The child
supervisor is built from child, which, like a child's initial state, is
read when the parent is spawned: keep it, and the states its children
copy, alive until then. Its shutdown_ms is ALPH_SUP_WAIT, OTP's infinity
for supervisors. Returns the child spec, or 0 when spec is full.
Thread: any.

### `alph_sup_ack`

```mettle
export fn alph_sup_ack(ctx: AlphCtx*, init: AlphMsg*)
```

Tells the supervisor this child finished starting. Only for children with
sync_start; call it from the INIT handler, passing the INIT message.
Thread: the child's handler.

### `alph_sup_which`

```mettle
export fn alph_sup_which(ctx: AlphCtx*, sup: int64, timeout_ms: int64) -> int64
```

Asks supervisor sup, static or dynamic, for its children. The answer is
ALPH_TAG_CHILDREN: arg = how many, payload = that many AlphChildInfo (read
with alph_msg_bytes); as for any request, ALPH_TAG_DOWN or
ALPH_TAG_TIMEOUT may come instead. Each carries the returned ref.
Returns as alph_request. Thread: a handler.

### `alph_sup_terminate`

```mettle
export fn alph_sup_terminate(ctx: AlphCtx*, sup: int64, pid: int64, timeout_ms: int64) -> int64
```

Asks supervisor sup, static or dynamic, to stop its child pid and not
restart it: ALPH_SHUTDOWN, then a kill after the child's shutdown_ms. The
answer, ALPH_TAG_SUP_ANSWER, comes once the child has exited (arg 1), or
at once with arg 0 when pid is not its child or it is busy restarting.
A static supervisor keeps the child's spec, so alph_sup_restart can start
it again; a dynamic one forgets the child. Returns as alph_request.
Thread: a handler.

### `alph_sup_restart`

```mettle
export fn alph_sup_restart(ctx: AlphCtx*, sup: int64, index: int64, timeout_ms: int64) -> int64
```

Asks static supervisor sup to start again child index (its place in the
spec), stopped by alph_sup_terminate. The answer is ALPH_TAG_SUP_ANSWER
with the new pid, or 0 when that child is not stopped, the index is
wrong, or sup is a dynamic supervisor. Returns as alph_request.
Thread: a handler.

### `alph_sup_handle`

```mettle
export fn alph_sup_handle(ctx: AlphCtx*, m: AlphMsg*) -> int64
```

A supervisor's handler, for starting one where a handler is asked for,
with a state from alph_sup_bytes. Thread: the runtime calls it.

### `alph_sup_bytes`

```mettle
export fn alph_sup_bytes(spec: AlphSupSpec*, size: int64*) -> rawptr
```

A supervisor's initial state, built from spec, for starting it where a
state and handler are asked for, such as alph_dyn_start_with with
alph_sup_handle. Returns a malloc'd block, its size in *size; free it
once it has been copied. Returns 0 when out of memory. Thread: any.

### `alph_spawn_sup`

```mettle
export fn alph_spawn_sup(ctx: AlphCtx*, spec: AlphSupSpec*) -> int64
```

Spawns a supervisor linked to the calling actor, which becomes its parent:
if the parent exits, the supervisor stops its children and exits too.

```text
spec  the definition; copied, along with every child's initial state
```

Returns the supervisor's pid, or 0 when the node is full or out of
memory. Thread: a handler. Outside the node, use alph_node_spawn_sup.

### `alph_node_spawn_sup`

```mettle
export fn alph_node_spawn_sup(node: AlphNode*, spec: AlphSupSpec*) -> int64
```

Spawns a top-level supervisor with no parent. Stop it with
alph_node_exit_signal(node, pid, ALPH_SHUTDOWN). Otherwise as
alph_spawn_sup. Thread: any.

## alph/heal/medic

Proactive healing.

Passive healing is a supervisor: the actor crashes, exits, and is started
again with a new pid. Proactive healing is a medic: a small actor attached
to a bigger one. When the bigger one crashes, the runtime holds it in place
(same pid, links, monitors and mailbox; new messages still queue) and the
medic's repair function sees the crash reason, the state as the crash left
it, and the message being handled. It answers with one of:

```text
ALPH_HEAL_RESUME  go on with the state as repaired; drop that message
ALPH_HEAL_RETRY   go on, and handle that message again
ALPH_HEAL_RESET   start over from the state the actor had when the medic
                  was attached; INIT runs again
ALPH_HEAL_FAIL    give up: the actor exits and its supervisor takes over
```

Nobody else sees a repaired crash. More than max_repairs within period_ms
counts as ALPH_HEAL_FAIL. If the medic itself dies, the actor runs on
without one, and its next crash is handled passively.

Attach from the actor's INIT handler, so a restarted actor gets a medic
too:

```text
fn repair(ctx: AlphCtx*, r: AlphRepair*) -> int64 {
  var a: Account* = (Account*)r->state;
  if (a->balance < 0) { a->balance = a->last_good; return ALPH_HEAL_RESUME; }
  return ALPH_HEAL_FAIL;
}
```

```text
var spec: AlphMedicSpec;
alph_medic_init(&spec, &repair);
alph_attach_medic(ctx, &spec);
```

What a repair function returns.

```mettle
export const ALPH_HEAL_RESUME: int64 = 0;
export const ALPH_HEAL_RETRY: int64 = 1;
export const ALPH_HEAL_RESET: int64 = 2;
export const ALPH_HEAL_FAIL: int64 = 3;
```

### `AlphRepair`

```mettle
export struct AlphRepair {
  patient: int64;
  reason: int64;
  state: rawptr;
  state_size: int64;
  message: AlphMsg*;
  repairs: int64;
}
```

What a repair function is given about one crash.

```text
patient     the crashed actor's pid
reason      why it crashed; ALPH_CRASHED
state       its state block as the crash left it; repair may change it
state_size  the block's size
message     the message it was handling (tag, from, arg, payload);
            read the payload with alph_msg_value or alph_msg_bytes
repairs     repairs within the current period, this one included
```

Everything here is valid only during the repair call.

### `AlphMedicSpec`

```mettle
export struct AlphMedicSpec {
  max_repairs: int64;
  period_ms: int64;
  repair: fn(AlphCtx*, AlphRepair*) -> int64 with alloc, syscall, asm;
}
```

A medic's definition. Fill it with alph_medic_init, then change:

```text
max_repairs  repairs allowed within period_ms; one more counts as
             ALPH_HEAL_FAIL. Default 3 (at most 63)
period_ms    default 10000
```

repair is the function the medic calls on each crash. It runs in the
medic, so it may use ctx to send messages; it must return one of the
ALPH_HEAL_ values, and anything else counts as ALPH_HEAL_FAIL.

### `alph_medic_init`

```mettle
export fn alph_medic_init(spec: AlphMedicSpec*,
                          repair: fn(AlphCtx*, AlphRepair*) -> int64 with alloc, syscall, asm)
```

Fills spec with the defaults (3 repairs per 10000 ms) and the repair
function. Thread: any.

### `alph_attach_medic`

```mettle
export fn alph_attach_medic(ctx: AlphCtx*, spec: AlphMedicSpec*) -> int64
```

Attaches a medic to the calling actor. The actor's state right now becomes
the state ALPH_HEAL_RESET returns to, so attach at the end of INIT. While
a medic is alive, attaching again keeps it: a reset actor runs INIT again
without gaining a second medic.

```text
spec  from alph_medic_init; copied
```

Returns the medic's pid, or 0 when the node is full or out of memory.
Thread: the actor's own handler.

## alph/heal/dynamic

Supervisors whose children are added at run time.

A dynamic supervisor starts with no children. Every child runs the same
handler and is started on request with its own initial state, as OTP's
DynamicSupervisor: one connection or session per child. There is no
fixed limit on children; max_children sets one.

Each child is restarted alone (one for one) by the spec's restart type:

```text
ALPH_PERMANENT  always restarted, from the initial state it was started with
ALPH_TRANSIENT  restarted unless it exited with ALPH_NORMAL or ALPH_SHUTDOWN
ALPH_TEMPORARY  never restarted; the usual choice for connections
```

More than max_restarts restarts within period_ms and the supervisor gives
up: it stops every child and exits with ALPH_SHUTDOWN. When it stops,
for that reason or because its parent exited or sent it an exit signal,
every child gets an ALPH_SHUTDOWN exit signal at once, and ALPH_KILL
after shutdown_ms.

```text
var spec: AlphDynSpec;
alph_dyn_init(&spec, &conn_handler);
spec.restart = ALPH_TEMPORARY;
var sup: int64 = alph_spawn_dyn(ctx, &spec);
...
var ref: int64 = alph_dyn_start_value<Conn>(ctx, sup, &conn, 1000);
// later: ALPH_TAG_STARTED with m->ref == ref and m->arg = the child's pid
```

The reply to a start request: m->ref is the request, m->arg the new
child's pid, or 0 when it was not started (the supervisor is full,
stopping, or out of memory, or the node is full).

```mettle
export const ALPH_TAG_STARTED: int64 = 12;
```

### `AlphDynSpec`

```mettle
export struct AlphDynSpec {
  restart: int64;
  shutdown_ms: int64;
  max_restarts: int64;
  period_ms: int64;
  max_children: int64;
  backoff_ms: int64;
  backoff_max_ms: int64;
  jitter_pct: int64;
  handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm;
}
```

A dynamic supervisor's definition. Start it with alph_dyn_init, change
these if needed, then spawn it with alph_spawn_dyn:

```text
restart       restart type of every child; default ALPH_PERMANENT, as OTP
shutdown_ms   how long children get to stop before they are killed; 0
              kills them at once. Default 5000
max_restarts  restarts allowed within period_ms before giving up;
              default 3 (at most 63)
period_ms     default 5000; 0 means no limit: no restart ever counts as
              recent, so the supervisor never gives up
max_children  children at once; 0 (default) is no limit
backoff_ms    wait before restarting a child that keeps failing,
              doubling up to backoff_max_ms; reset once it has run for
              period_ms. 0 (default) restarts at once
backoff_max_ms  cap on the doubling backoff; 0 means no cap
jitter_pct    each backoff is shortened by a random 0..jitter_pct
              percent; 0 (default) is none
handler       the children's handler, set by alph_dyn_init
```

restart and shutdown_ms are defaults: a child started with
alph_dyn_start_opts has its own.

### `AlphDynOpts`

```mettle
export struct AlphDynOpts {
  restart: int64;
  shutdown_ms: int64;
  flags: int64;
}
```

Options for one child, for alph_dyn_start_opts. Fill with alph_dyn_opts,
then change what you need:

```text
restart      ALPH_PERMANENT (default), ALPH_TRANSIENT or ALPH_TEMPORARY
shutdown_ms  how long it gets to stop; 0 kills at once; ALPH_SUP_WAIT
             waits as long as it takes. Default 5000
flags        spawn flags besides ALPH_LINK, such as ALPH_BOUND. Default 0
```

### `AlphDyn`

```mettle
export struct AlphDyn {
  spec: AlphDynSpec;
  children: int64;      // AlphDynChild array in the supervisor's heap
  count: int64;
  cap: int64;
  stopping: int64;
  exit_reason: int64;
  timer: int64;
  restarts: int64[64];  // times of recent restarts, a ring
  restart_count: int64;
  next_id: int64;
  rng: int64;           // random state for jitter
}
```

A dynamic supervisor's state, for alph_sup_add_dyn. Its fields are the
runtime's own.

### `alph_dyn_init`

```mettle
export fn alph_dyn_init(spec: AlphDynSpec*, handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm)
```

Starts a dynamic supervisor definition for children that run handler,
with the defaults listed at AlphDynSpec. Thread: any.

### `alph_dyn_handle`

```mettle
export fn alph_dyn_handle(ctx: AlphCtx*, m: AlphMsg*) -> int64
```

A dynamic supervisor's handler, for starting one where a handler is asked
for; alph_sup_add_dyn uses it. Thread: the runtime calls it.

### `alph_spawn_dyn`

```mettle
export fn alph_spawn_dyn(ctx: AlphCtx*, spec: AlphDynSpec*) -> int64
```

Spawns a dynamic supervisor linked to the calling actor, which becomes
its parent: if the parent exits, the supervisor stops its children and
exits too.

```text
spec  the definition; copied
```

Returns the supervisor's pid, or 0 when the node is full or out of
memory. Thread: a handler. Outside the node, use alph_node_spawn_dyn.

### `alph_node_spawn_dyn`

```mettle
export fn alph_node_spawn_dyn(node: AlphNode*, spec: AlphDynSpec*) -> int64
```

Spawns a top-level dynamic supervisor with no parent. Stop it with
alph_node_exit_signal(node, pid, ALPH_SHUTDOWN). Otherwise as
alph_spawn_dyn. Thread: any.

### `alph_dyn_start_bytes`

```mettle
export fn alph_dyn_start_bytes(ctx: AlphCtx*, sup: int64, data: rawptr, size: int64,
                               timeout_ms: int64) -> int64
```

Asks dynamic supervisor sup to start a child whose state is size bytes
at data, copied. The answer is ALPH_TAG_STARTED (arg = the new pid, or 0)
or, as for any request, ALPH_TAG_DOWN or ALPH_TAG_TIMEOUT; each carries
the returned ref. Returns as alph_request. Thread: a handler.

### `alph_dyn_start_value`

```mettle
export fn alph_dyn_start_value<T>(ctx: AlphCtx*, sup: int64, state: T*, timeout_ms: int64) -> int64
```

Asks sup to start a child whose state is one T, copied. Otherwise as
alph_dyn_start_bytes. Thread: a handler.

### `alph_dyn_start`

```mettle
export fn alph_dyn_start(ctx: AlphCtx*, sup: int64, arg: int64, timeout_ms: int64) -> int64
```

Asks sup to start a child whose state is one int64 holding arg (a
handle, an index). Otherwise as alph_dyn_start_bytes. Thread: a handler.

### `alph_dyn_start_with`

```mettle
export fn alph_dyn_start_with(ctx: AlphCtx*, sup: int64, data: rawptr, size: int64, timeout_ms: int64,
                              handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Asks sup to start a child that runs handler instead of the one in its
AlphDynSpec, with size bytes at data as its state, copied. A restart
runs the same handler. Starting a supervisor under sup:

```text
var size: int64 = 0;
var st: rawptr = alph_sup_bytes(&child_spec, &size);
alph_dyn_start_with(ctx, sup, st, size, &alph_sup_handle, 1000);
free(st);
```

Otherwise as alph_dyn_start_bytes. Thread: a handler.

### `alph_dyn_opts`

```mettle
export fn alph_dyn_opts(opts: AlphDynOpts*)
```

Fills opts with the defaults listed at AlphDynOpts. Thread: any.

### `alph_dyn_start_opts`

```mettle
export fn alph_dyn_start_opts(ctx: AlphCtx*, sup: int64, data: rawptr, size: int64, opts: AlphDynOpts*,
                              timeout_ms: int64,
                              handler: fn(AlphCtx*, AlphMsg*) -> int64 with alloc, syscall, asm) -> int64
```

Asks sup to start a child that runs handler, with its own restart type,
shutdown_ms and spawn flags from opts instead of the spec's: one
supervisor can then hold permanent workers and temporary jobs, or
children on their own thread (ALPH_BOUND). Otherwise as
alph_dyn_start_with. Thread: a handler.

### `alph_node_dyn_start_bytes`

```mettle
export fn alph_node_dyn_start_bytes(node: AlphNode*, sup: int64, data: rawptr, size: int64) -> int64
```

Starts a child from outside the node: as alph_dyn_start_bytes with the
spec's handler, but nobody hears the answer. Returns ALPH_SENT, or as
alph_node_send. Thread: any.

### `alph_sup_add_dyn`

```mettle
export fn alph_sup_add_dyn(spec: AlphSupSpec*, dyn: AlphDyn*, dyn_spec: AlphDynSpec*) -> AlphChildSpec*
```

Adds a dynamic supervisor as a child of the static supervisor spec. dyn
receives its initial state, built from dyn_spec; like any child's initial
state it is copied when the parent is spawned, so keep dyn alive until
then. Its shutdown_ms is ALPH_SUP_WAIT, so it stops its own children
first. Returns the child spec, or 0 when spec is full. Thread: any.

## alph/work/pool

A pool of threads for blocking work.

A handler must not block, so blocking work (file IO, a blocking client,
a long hash) goes to a pool: a dispatcher actor and N ALPH_BOUND worker
actors, each on a thread of its own. alph_offload sends one job, a
function and its input; the answer comes back as a message, like the
reply to a request:

```text
ALPH_TAG_DONE    arg = the work function's return, payload = the bytes
                 it put in its AlphOut
ALPH_TAG_FAILED  arg = the reason the job ended without a result:
                 ALPH_CRASHED for a trap, the reason given to
                 alph_exit_now, or ALPH_FULL when the pool's queue was
                 full
```

or, as for any request, ALPH_TAG_DOWN (the pool is gone) or
ALPH_TAG_TIMEOUT. A job that times out still runs to the end; its result
is dropped. A worker that crashes or exits is replaced.

```text
fn hash_file(input: rawptr, size: int64, out: AlphOut*) -> int64 { ... }
var pool: int64 = alph_spawn_pool(ctx, 4, 1024);
var ref: int64 = alph_offload(ctx, pool, path, path_len, 5000, &hash_file);
```

The answers to alph_offload, described at the top of this module.

```mettle
export const ALPH_TAG_DONE: int64 = 15;
export const ALPH_TAG_FAILED: int64 = 16;
```

### `AlphOut`

```mettle
export struct AlphOut {
  ctx: AlphCtx*;
  data: int64;
  len: int64;
  cap: int64;
}
```

Where a work function puts its result bytes. Fill it with alph_out_bytes;
its fields are the runtime's own.

### `alph_out_bytes`

```mettle
export fn alph_out_bytes(out: AlphOut*, data: rawptr, size: int64) -> bool
```

Appends size bytes at data to out. Returns false when the worker's heap
is full (heap_limit); out keeps what it had. Thread: the work function
that got out.

### `alph_spawn_pool`

```mettle
export fn alph_spawn_pool(ctx: AlphCtx*, workers: int64, queue: int64) -> int64
```

Spawns a pool linked to the calling actor, which becomes its parent: if
the parent exits, the pool and its workers exit too.

```text
workers  threads, each running one job at a time (at least 1)
queue    jobs that may wait for a free worker; past it a job fails
         with ALPH_FULL (at least 1)
```

Returns the pool's pid, or 0 when the arguments are out of range or the
node is full. Thread: a handler. Outside the node, use
alph_node_spawn_pool.

### `alph_node_spawn_pool`

```mettle
export fn alph_node_spawn_pool(node: AlphNode*, workers: int64, queue: int64) -> int64
```

Spawns a top-level pool with no parent. Stop it with
alph_node_exit_signal(node, pid, ALPH_SHUTDOWN). Otherwise as
alph_spawn_pool. Thread: any.

### `alph_offload`

```mettle
export fn alph_offload(ctx: AlphCtx*, pool: int64, input: rawptr, size: int64, timeout_ms: int64,
                       work: fn(rawptr, int64, AlphOut*) -> int64 with alloc, syscall, asm) -> int64
```

Sends a job to pool: work(copy of input, size, out) runs on a worker
thread, where it may block. The input is size bytes, copied. The answer
carries the returned ref; it is described at the top of this module.
work must not keep pointers into its input or out after it returns.

Returns the ref, or ALPH_FULL or ALPH_NOMEM when the job was not sent
(the pool's mailbox is full, or no memory). Thread: a handler.

## alph/work/limiter

A counting semaphore as an actor.

A limiter holds a number of permits, such as the -j of a build or the
connections a database allows. alph_acquire asks for one; the answer
ALPH_TAG_GRANTED comes when one is free, first come first served. The
holder gives it back with alph_release, and a holder that exits gives it
back by its exit. alph_release on a request still waiting withdraws it.

```text
var cpu: int64 = alph_spawn_limiter(ctx, 8);
var ref: int64 = alph_acquire(ctx, cpu);
... ALPH_TAG_GRANTED with m->ref == ref: start the job
alph_release(ctx, cpu, ref);
```

The answer to alph_acquire: a permit is now the caller's. m->ref is the
ref alph_acquire returned.

```mettle
export const ALPH_TAG_GRANTED: int64 = 19;
```

### `alph_spawn_limiter`

```mettle
export fn alph_spawn_limiter(ctx: AlphCtx*, permits: int64) -> int64
```

Spawns a limiter with permits permits (at least 1), linked to the calling
actor. Returns its pid, or 0. Thread: a handler. Outside the node, use
alph_node_spawn_limiter.

### `alph_node_spawn_limiter`

```mettle
export fn alph_node_spawn_limiter(node: AlphNode*, permits: int64) -> int64
```

Spawns a top-level limiter with no parent. Otherwise as
alph_spawn_limiter. Thread: any.

### `alph_acquire`

```mettle
export fn alph_acquire(ctx: AlphCtx*, limiter: int64) -> int64
```

Asks limiter for one permit. ALPH_TAG_GRANTED with the returned ref comes
when it is the caller's; ALPH_TAG_DOWN with that ref if the limiter is
gone. There is no timeout: to give up waiting, use alph_release.

Returns the ref, or ALPH_FULL or ALPH_NOMEM when the request was not
sent. Thread: a handler.

### `alph_release`

```mettle
export fn alph_release(ctx: AlphCtx*, limiter: int64, ref: int64) -> int64
```

Gives back the permit named by ref, or withdraws the request if it is
still waiting; a GRANTED for ref already on its way is dropped. Returns
as alph_send; on ALPH_FULL try again later, or exit, which also gives it
back. Thread: a handler.

## alph/work/task

One-shot tasks and a bounded parallel map, like Elixir's
Task and Task.async_stream.

A task is a short-lived actor that runs one function and answers. The
caller does not block: it gets a ref, and the answer arrives as a message
with that ref, so "await" is handling ALPH_TAG_TASK_DONE. A task that
crashes answers with ALPH_TAG_DOWN, and one that runs too long with
ALPH_TAG_TIMEOUT, as for any alph_request; its late answer is dropped.

```text
fn square(ctx: AlphCtx*, data: rawptr, size: int64) -> int64 ... {
  return *(int64*)data * *(int64*)data;
}
var ref: int64 = alph_task(ctx, (rawptr)&n, 8, 1000, &square);
... ALPH_TAG_TASK_DONE with m->ref == ref: m->arg is the square
```

alph_task_map runs one function over many items with at most a given
number running at once, and answers once with every result in order.

The answer to alph_task: arg = what the function returned; ref = the ref
alph_task returned.

```mettle
export const ALPH_TAG_TASK_DONE: int64 = 51;
```

The answer to alph_task_map: arg = how many items failed; the payload is
one AlphTaskResult per item, in item order. ref = the ref alph_task_map
returned.

```mettle
export const ALPH_TAG_TASK_ALL: int64 = 52;
```

### `AlphTaskResult`

```mettle
export struct AlphTaskResult {
  ok: int64;
  value: int64;
}
```

One item's outcome in an ALPH_TAG_TASK_ALL answer.

```text
ok     1 when the function returned, 0 when it crashed or timed out
value  what it returned, 0 when it failed
```

### `alph_task`

```mettle
export fn alph_task(ctx: AlphCtx*, data: rawptr, size: int64, timeout_ms: int64,
                    work: fn(AlphCtx*, rawptr, int64) -> int64 with alloc, syscall, asm) -> int64
```

Starts a task running work on a copy of size bytes at data, with
timeout_ms to answer (0 waits for ever). work gets the task's own ctx, so
it may allocate, send and spawn like any handler; it must not block.

Returns the ref the answer carries (ALPH_TAG_TASK_DONE, or ALPH_TAG_DOWN
when the task crashed, or ALPH_TAG_TIMEOUT), or 0 when the node is full,
or ALPH_FULL or ALPH_NOMEM as alph_request. Thread: a handler.

### `alph_task_map`

```mettle
export fn alph_task_map(ctx: AlphCtx*, items: rawptr, count: int64, item_size: int64, max_tasks: int64,
                        item_timeout_ms: int64, timeout_ms: int64,
                        work: fn(AlphCtx*, rawptr, int64) -> int64 with alloc, syscall, asm) -> int64
```

Runs work over count items of item_size bytes each, at items, with at
most max_tasks tasks at once, each given item_timeout_ms (0: no limit).
The items are copied. The answer, once every item is done, is
ALPH_TAG_TASK_ALL: arg = how many failed, payload = an AlphTaskResult per
item in order. A coordinator actor does the work, so the caller stays
free; if it crashes the answer is ALPH_TAG_DOWN, and timeout_ms bounds
the whole map (0: no limit).

Returns the ref the answer carries, 0 when the node is full or out of
memory, or as alph_request. Thread: a handler.

## alph/work/agent

State behind an actor, like Elixir's Agent.

An agent holds one block of state. Others read it with alph_agent_get, or
change it by sending a function that runs inside the agent, one at a
time, so no lock is needed. Put it under a supervisor with
alph_sup_add_bytes(spec, state, size, &alph_agent_handle).

```text
fn add(state: rawptr, size: int64, arg: int64) -> int64 ... {
  *(int64*)state = *(int64*)state + arg;
  return *(int64*)state;
}
var counter: int64 = alph_spawn_agent(ctx, ALPH_LINK, (rawptr)&zero, 8);
alph_agent_update(ctx, counter, 5, 1000, &add);
... ALPH_TAG_AGENT_DONE: m->arg is the new count
```

The answer to alph_agent_get: the payload is a copy of the state.

```mettle
export const ALPH_TAG_AGENT_STATE: int64 = 55;
```

The answer to alph_agent_update: arg = what the function returned.

```mettle
export const ALPH_TAG_AGENT_DONE: int64 = 56;
```

### `alph_agent_handle`

```mettle
export fn alph_agent_handle(ctx: AlphCtx*, m: AlphMsg*) -> int64
```

An agent's handler, for starting one where a handler is asked for, such
as under a supervisor; its state is the agent's state.
Thread: the runtime calls it.

### `alph_spawn_agent`

```mettle
export fn alph_spawn_agent(ctx: AlphCtx*, flags: int64, state: rawptr, size: int64) -> int64
```

Spawns an agent whose state starts as a copy of size bytes at state.

```text
flags  as alph_spawn: ALPH_LINK to stop it with the caller
```

Returns its pid, or 0 when the node is full or out of memory.
Thread: a handler.

### `alph_agent_get`

```mettle
export fn alph_agent_get(ctx: AlphCtx*, agent: int64, timeout_ms: int64) -> int64
```

Asks agent for a copy of its state. The answer is ALPH_TAG_AGENT_STATE
with the copy as payload. Returns as alph_request. Thread: a handler.

### `alph_agent_update`

```mettle
export fn alph_agent_update(ctx: AlphCtx*, agent: int64, arg: int64, timeout_ms: int64,
                            f: fn(rawptr, int64, int64) -> int64 with alloc, syscall, asm) -> int64
```

Runs f(state, size, arg) inside agent; f may change the state in place.
The answer is ALPH_TAG_AGENT_DONE with what f returned. If f crashes, the
agent crashes and the answer is ALPH_TAG_DOWN. Returns as alph_request.
Thread: a handler.

### `alph_agent_cast`

```mettle
export fn alph_agent_cast(ctx: AlphCtx*, agent: int64, arg: int64,
                          f: fn(rawptr, int64, int64) -> int64 with alloc, syscall, asm) -> int64
```

As alph_agent_update with no answer. Returns as alph_send.
Thread: a handler.

## alph/net/port

Wait until a TCP port accepts connections.

After starting a server as a child process, a caller often has to wait
until it listens. alph_spawn_port_wait starts an actor that tries to
connect every interval_ms and tells its parent once:

```text
ALPH_TAG_PORT_READY  from = the waiter, arg = 1 when a connection was
                     accepted, 0 after tries failed attempts
```

The waiter closes its test connection and exits after telling. To stop it
early, send it an exit signal.

The waiter's answer, described at the top of this module.

```mettle
export const ALPH_TAG_PORT_READY: int64 = 24;
```

### `alph_spawn_port_wait`

```mettle
export fn alph_spawn_port_wait(ctx: AlphCtx*, ip4: int64, port: int64, interval_ms: int64,
                               tries: int64) -> int64
```

Spawns an actor that waits for ip4:port (alph_ip4) to accept TCP
connections, trying every interval_ms at most tries times (each at least
1), and answers the caller with ALPH_TAG_PORT_READY. It is linked to the
caller, so it stops if the caller exits; its own normal exit does not
reach the caller.

Returns its pid, or 0 when an argument is out of range or the node is
full. Thread: a handler.

## alph/net/dist

Actors on other nodes, as on BEAM.

Nodes connect over TCP. An actor on another node is reached through a
proxy: a local actor that stands for it. Send to the proxy and the
message arrives at the remote actor, from a proxy that stands for you
there, so it can answer. Requests and replies work across nodes, with
their refs.

A proxy lives exactly as long as the actor it stands for. When that actor
exits, the proxy exits with the same reason, so a local link or monitor on
the proxy sees the remote exit. When the connection is lost, every proxy
over it exits with ALPH_NOCONNECTION, as on BEAM. Linking to a proxy links
both ways: the remote actor is linked to a proxy for the local one, so an
exit on either side reaches the other. An exit signal sent to a proxy, kill
included, is passed to the remote actor.

Messages are never dropped while the connection holds. When a receiver's
mailbox is full, the connection stops reading until it has room, which
slows the sender through TCP. When a node sends faster than its
connection can carry and the connection's queue fills, the connection is
closed rather than losing a message, and everything over it sees
ALPH_NOCONNECTION.

Each node runs one dist manager, started with alph_dist_start:

```text
alph_dist_start(ctx, alph_ip4(0, 0, 0, 0), 4370, "cookie");
... ALPH_TAG_DIST_READY, arg = the port: the manager listens
var ref: int64 = alph_dist_connect(ctx, alph_ip4(10, 0, 0, 2), 4370, 5000);
... ALPH_TAG_DIST_UP, arg = the connection, or 0 when it failed
var ref2: int64 = alph_dist_whereis(ctx, conn, "store", 5000);
... ALPH_TAG_DIST_FOUND, arg = a proxy for the remote "store", or 0
```

Monitor the connection pid to hear when the other node goes away.

Connections are encrypted and authenticated with Noise
(Noise_XXpsk3_25519_ChaChaPoly_SHA256, alph/net/noise). The pre-shared key
is the SHA-256 of the cluster cookie, which never crosses the network: a
node without the same cookie cannot complete the handshake or read or
change any traffic. Each node makes a new static key when its manager
starts. Treat the cookie as a secret, and make it long and random.

The answer to alph_dist_connect: arg is the connection's pid, or 0.

```mettle
export const ALPH_TAG_DIST_UP: int64 = 30;
export const ALPH_TAG_DIST_FOUND: int64 = 31;
export const ALPH_TAG_DIST_PORT: int64 = 41;
export const ALPH_TAG_DIST_READY: int64 = 42;
```

### `alph_dist_start`

```mettle
export fn alph_dist_start(ctx: AlphCtx*, ip4: int64, port: int64, cookie: string) -> int64
```

Starts this node's dist manager, linked to the caller, listening on
ip4:port (port 0 picks a free port). It registers itself as "alph.dist"
and then sends the caller ALPH_TAG_DIST_READY with the port it listens
on; connect and look up names only after that. Only nodes started with
the same cookie can connect.

Returns the manager's pid, or 0 when the cookie is empty, the system has
no randomness for the node's key, or the node is full. If listening fails, the manager exits at once with
ALPH_SHUTDOWN, which the caller sees through the link. Thread: a handler.

### `alph_dist_connect`

```mettle
export fn alph_dist_connect(ctx: AlphCtx*, ip4: int64, port: int64, timeout_ms: int64) -> int64
```

Connects to the node listening on ip4:port. The answer is
ALPH_TAG_DIST_UP with arg = the connection's pid, or 0 when the node
could not be reached or holds another cookie; or, as for any request,
ALPH_TAG_DOWN or ALPH_TAG_TIMEOUT. Each carries the returned ref.

Returns the ref, or ALPH_GONE when no dist manager runs, or ALPH_FULL.
Thread: a handler.

### `alph_dist_whereis`

```mettle
export fn alph_dist_whereis(ctx: AlphCtx*, conn: int64, name: string, timeout_ms: int64) -> int64
```

Looks up name in the registry of the node at the other end of conn. The
answer is ALPH_TAG_DIST_FOUND with arg = a local proxy for that actor,
or 0 when the name is not registered there; or ALPH_TAG_DOWN or
ALPH_TAG_TIMEOUT. Returns the request's ref, or as alph_request.
Thread: a handler.

### `alph_dist_port`

```mettle
export fn alph_dist_port(ctx: AlphCtx*, timeout_ms: int64) -> int64
```

Asks this node's dist manager which port it listens on, for a manager
started with port 0. The answer is ALPH_TAG_DIST_PORT with arg = the
port. Returns the request's ref, or ALPH_GONE when no manager runs.
Thread: a handler.

## alph/net/noise

The Noise_XXpsk3_25519_ChaChaPoly_SHA256 handshake and
transport ciphers (noiseprotocol.org, revision 34), on the cryptex package.

```text
-> e
<- e, ee, s, es
-> s, se, psk
```

Both sides prove knowledge of a pre-shared key (alph uses the SHA-256 of
the cluster cookie) without sending it, and each learns the other's static
public key. Handshake payloads are empty. After the third message,
alph_noise_split gives each side a sending and a receiving cipher. The
initiator only knows the responder holds the key once a transport message
from it decrypts.

Plain data, no actors: alph/net/dist drives it. Thread: whoever owns the
state.

The longest Noise message, and the most plaintext one transport message
can carry.

```mettle
export const ALPH_NOISE_MAX: int64 = 65535;
export const ALPH_NOISE_PLAIN_MAX: int64 = 65519;
```

### `AlphNoiseCipher`

```mettle
export struct AlphNoiseCipher {
  k: CrKey32;
  n: int64;
}
```

One direction of an established session.

### `AlphNoise`

```mettle
export struct AlphNoise {
  initiator: int64;
  step: int64;         // handshake messages written or read so far
  h: uint8[32];
  ck: uint8[32];
  c: AlphNoiseCipher;
  has_key: int64;
  s_sk: CrX25519Secret;
  s_pk: CrX25519Public;
  e_sk: CrX25519Secret;
  e_pk: CrX25519Public;
  fixed_e: int64;      // alph_noise_set_ephemeral was called
  re: CrX25519Public;
  rs: CrX25519Public;
  psk: uint8[32];
}
```

A handshake in progress. Fill it with alph_noise_init; its fields are
this module's own.

### `alph_noise_init`

```mettle
export fn alph_noise_init(n: AlphNoise*, initiator: bool, s_sk: CrX25519Secret*, psk: uint8*) -> bool
```

Starts a handshake.

```text
n          the state to fill
initiator  true on the side that connects
s_sk       this node's static secret key
psk        32 bytes both sides share
```

Returns false when the cryptex package fails. Thread: any.

### `alph_noise_set_ephemeral`

```mettle
export fn alph_noise_set_ephemeral(n: AlphNoise*, e_sk: CrX25519Secret*) -> bool
```

Uses e_sk as the next ephemeral key instead of a random one. For tests
against fixed vectors only; a real handshake must never reuse one.

### `alph_noise_write`

```mettle
export fn alph_noise_write(n: AlphNoise*, out: uint8*, cap: int64, out_len: int64*) -> bool
```

Writes this side's next handshake message to out (cap at least 96
bytes) and its length to *out_len. Returns false when it is not this
side's turn or the cryptex package fails. Thread: the state's owner.

### `alph_noise_read`

```mettle
export fn alph_noise_read(n: AlphNoise*, msg: uint8*, len: int64) -> bool
```

Reads the other side's next handshake message. Returns false when it is
not the other side's turn, the message has the wrong length, or it does
not authenticate (a wrong pre-shared key shows up here, on the
responder's side of the third message). Thread: the state's owner.

### `alph_noise_done`

```mettle
export fn alph_noise_done(n: AlphNoise*) -> bool
```

True once all three handshake messages are done. Thread: the owner.

### `alph_noise_peer`

```mettle
export fn alph_noise_peer(n: AlphNoise*, out: CrX25519Public*)
```

The other side's static public key, valid once the handshake is done.
Thread: the state's owner.

### `alph_noise_split`

```mettle
export fn alph_noise_split(n: AlphNoise*, send: AlphNoiseCipher*, recv: AlphNoiseCipher*) -> bool
```

Ends a finished handshake: fills the sending and receiving ciphers and
wipes the handshake's secrets. Returns false when the handshake is not
done. Thread: the state's owner.

### `alph_noise_seal`

```mettle
export fn alph_noise_seal(c: AlphNoiseCipher*, out: uint8*, pt: uint8*, len: int64) -> bool
```

Encrypts len bytes at pt (at most ALPH_NOISE_PLAIN_MAX) to out, which
gets len + 16 bytes. Returns false when len is too long or the nonce is
exhausted. Thread: the cipher's owner.

### `alph_noise_open`

```mettle
export fn alph_noise_open(c: AlphNoiseCipher*, out: uint8*, ct: uint8*, len: int64) -> bool
```

Decrypts a transport message of len bytes at ct to out, which gets
len - 16 bytes. Returns false when it does not authenticate.
Thread: the cipher's owner.

## alph/sys/atomic

64-bit atomics on x86-64.

Each operation is a @naked export function, so it runs exactly the
instructions written here, and the optimizer sees an opaque call that it
cannot move memory accesses across. x86-64 is TSO: a plain load already has
acquire order and a plain store has release order. Sequential consistency
needs a locked instruction or mfence, which store_sc, swap, cas and add use.
Pointers are passed as int64.

### `alph_atomic_load`

```mettle
export fn alph_atomic_load(p: int64*) -> int64
```

Reads *p. Acquire order: later reads and writes stay after it.
Thread: any.

### `alph_atomic_store`

```mettle
export fn alph_atomic_store(p: int64*, v: int64)
```

Writes v to *p. Release order: earlier reads and writes stay before it.
A later load may still pass it; use alph_atomic_store_sc when that matters.
Thread: any.

### `alph_atomic_store_sc`

```mettle
export fn alph_atomic_store_sc(p: int64*, v: int64)
```

Writes v to *p, sequentially consistent: no later load can be reordered
before it. Thread: any.

### `alph_atomic_swap`

```mettle
export fn alph_atomic_swap(p: int64*, v: int64) -> int64
```

Stores v in *p and returns the value it replaced. Sequentially consistent.
Thread: any.

### `alph_atomic_cas`

```mettle
export fn alph_atomic_cas(p: int64*, expected: int64, desired: int64) -> int64
```

Compare and swap: stores desired in *p if *p equals expected. Returns the
value *p held before, so the swap happened exactly when the result equals
expected. Sequentially consistent. Thread: any.

### `alph_atomic_add`

```mettle
export fn alph_atomic_add(p: int64*, delta: int64) -> int64
```

Adds delta to *p and returns the value from before the add. Sequentially
consistent. Thread: any.

### `alph_atomic_fence`

```mettle
export fn alph_atomic_fence()
```

A full barrier: no load or store moves across it in either direction.
Thread: any.

### `alph_cpu_relax`

```mettle
export fn alph_cpu_relax()
```

A hint inside spin loops: lets the sibling hyperthread run and saves
power. It has no effect on memory. Thread: any.

## alph/sys/clock

Monotonic time and futex parking.

### `alph_now_ns`

```mettle
export fn alph_now_ns() -> int64
```

Monotonic time in nanoseconds. Only differences between two readings mean
anything; it never goes backwards. It reads the clock through the kernel's
vDSO, about 20 ns, and falls back to the system call (about 150-190 ns)
where there is none; the kernel picks the clock source either way.
Thread: any.

### `alph_realtime_ns`

```mettle
export fn alph_realtime_ns() -> int64
```

Wall-clock time (CLOCK_REALTIME) in nanoseconds since 1970, read as
alph_now_ns reads the monotonic clock. It may jump when the system's time
is set; use it only to convert kernel timestamps, which are in it.
Thread: any.

### `alph_now_ms`

```mettle
export fn alph_now_ms() -> int64
```

Monotonic time in milliseconds, as alph_now_ns.
Thread: any.

### `alph_futex_wait`

```mettle
export fn alph_futex_wait(word: int64*, expected: int64, timeout_ns: int64)
```

Sleeps while the low 32 bits of *word equal expected.

```text
word        the futex word; only its low 32 bits are compared
expected    the value that means "keep sleeping"
timeout_ns  longest sleep in nanoseconds, or a negative number for none
```

Returns on a wake, a timeout, a signal, or because *word already differs,
so callers re-check their condition in a loop.
Thread: any. Used by the schedulers to park.

### `alph_futex_wake`

```mettle
export fn alph_futex_wake(word: int64*, count: int64)
```

Wakes up to count threads sleeping in alph_futex_wait on word.
Thread: any.

## alph/sys/thread

Threads that survive their own crashes.

Each thread gets a guard page at the bottom of its stack, its own alternate
signal stack, and a recovery registration covering its stack. A stack
overflow then faults on the guard page, the fault handler runs on the
alternate stack, and alph/sys/recover can resume the thread. The node runs
one of these per scheduler.

### `AlphThread`

```mettle
export struct AlphThread {
  body: fn(int64) -> int64;
  arg: int64;
  stack_size: int64;
  guard: int64;
  altstack: int64;
  result: int64;
  recover: AlphRecover;
}
```

A thread started by alph_thread_start. Keep it alive and in place until
alph_thread_join returns; the thread reads and writes it.

```text
result   what body returned, valid after alph_thread_join
recover  the thread's recovery registration, for alph_recover_save
```

### `alph_thread_start`

```mettle
export fn alph_thread_start(t: AlphThread*, body: fn(int64) -> int64, arg: int64,
                            stack_size: int64) -> int64
```

Starts body(arg) on a new thread.

```text
t           storage for the thread; it belongs to the thread until joined
body        the thread's work; its return value lands in t->result
arg         passed to body
stack_size  bytes of stack, at least 64 KiB (smaller values are raised)
```

Returns the thread's handle, or 0 when it could not start. Keep the handle:
the thread owns t, so the handle is what the caller holds on to.
Thread: any.

### `alph_thread_join`

```mettle
export fn alph_thread_join(handle: int64, t: AlphThread*) -> int64
```

Waits for a thread to finish and returns what its body returned.

```text
handle  from alph_thread_start
t       the same storage given to alph_thread_start
```

Thread: any thread except the one being joined.

### `alph_cpu_count`

```mettle
export fn alph_cpu_count() -> int64
```

The number of CPUs this process may run on, at least 1. The node uses it
as the default scheduler count.
Thread: any.

## alph/sys/recover

Recovery points that survive traps and faults (x86-64 Linux).

Mettle's runtime asks a hook where to resume before it ends the process on
a trap (bounds, divide, null) or a fault signal (SIGSEGV, SIGFPE, ...). The
hook is mettle_crash_set_live_recovery, built for `mettle live`. alph
registers its own callbacks there, so a crash inside an actor resumes the
scheduler instead of killing the node.

Use, in the frame that must survive (the scheduler loop):

```text
alph_recover_arm(r);
if (alph_recover_save(&r->regs[0]) != 0) {
  // a crash below resumed here; locals kept in registers may be stale,
  // so read state back from memory
} else {
  run_the_handler();
}
alph_recover_disarm(r);
```

Programs must be built with -s: without it the compiler emits inserted
traps as an inline exit that never reaches the hook. --safe is not
supported: the jump does not unwind its shadow call stack.

### `AlphRecover`

```mettle
export struct AlphRecover {
  // rbx, rbp, (unused), (unused), r12, r13, r14, r15, rsp, rip: the layout
  // the runtime's signal path restores (crash_handler.c).
  regs: int64[10];
  stack_lo: int64;
  stack_hi: int64;
  armed: int64;
  crashes: int64;
  owner: int64;
}
```

One thread's recovery point. Register it with alph_recover_register,
then save into regs with alph_recover_save.

```text
crashes  how many crashes resumed this point so far
owner    free for the registrant: the node keeps its scheduler here
```

### `alph_recover_save`

```mettle
export fn alph_recover_save(regs: int64*) -> int64
```

Saves the callee-saved registers, the stack pointer and the return address
into regs (an AlphRecover's regs). Returns 0 now, and returns 1 a second
time when a crash resumes here. Arm the AlphRecover first, and keep the
calling frame alive while the protected code runs.
Thread: the thread that owns regs.

### `alph_recover_resume`

```mettle
export fn alph_recover_resume(regs: int64)
```

Jumps back to the point saved in regs, where alph_recover_save returns 1.
Called by alph_recover_hook_jump; do not call it with a frame that has
returned. Thread: the thread that saved regs.

### `alph_recover_quiet`

```mettle
export fn alph_recover_quiet(on: bool)
```

With on true, a crash the hook recovers prints nothing: the runtime asks
the hook first, and skips the stack trace and report it would otherwise
build and write, which cost about 60 us per crash. A crash nobody
recovers is reported as always. Thread: any; it sets a process-wide flag.

### `alph_recover_hook_point`

```mettle
export fn alph_recover_hook_point(stack: int64) -> int64
```

Called by the Mettle runtime on a crash, not by programs. Returns the armed
recovery point of the thread whose stack holds `stack`, or 0 to let the
crash end the process. It runs inside a signal handler, so it only reads.

### `alph_recover_current`

```mettle
export fn alph_recover_current() -> AlphRecover*
```

The armed recovery point of the calling thread, or 0 when the thread has
none armed. Thread: any.

### `alph_recover_hook_wait`

```mettle
export fn alph_recover_hook_wait(slot: int64) -> int64
```

Called by the Mettle runtime, not by programs: decides to resume. alph
always resumes at once, disarming the point and counting the crash. regs
is the first field, so the slot is the AlphRecover itself.

### `alph_recover_hook_jump`

```mettle
export fn alph_recover_hook_jump(slot: int64)
```

Called by the Mettle runtime on the trap path, not by programs: jumps to
the recovery point. The signal path restores the registers itself.

### `alph_recover_register`

```mettle
export fn alph_recover_register(r: AlphRecover*, lo: int64, hi: int64) -> bool
```

Registers r as the recovery point for the thread whose stack is [lo, hi),
and installs alph's crash hook on first use.

```text
r       stays registered until alph_recover_unregister; keep it in place
lo, hi  the stack range, lowest and one past highest address
```

Returns false when 256 threads are registered at once. A slot freed by
alph_recover_unregister is reused, so threads may come and go for ever.
Thread: any.

### `alph_recover_register_here`

```mettle
export fn alph_recover_register_here(r: AlphRecover*, below: int64, above: int64) -> bool
```

Registers the calling thread when alph did not create it, such as the
main thread. The stack range is taken around the caller's frame:

```text
below  bytes of stack below the caller's frame (how deep it may grow)
above  bytes above it (the frames that called it)
```

Returns false when the table is full. Thread: the thread being registered.

### `alph_recover_unregister`

```mettle
export fn alph_recover_unregister(r: AlphRecover*)
```

Stops the crash hook from matching r and frees its table slot for the
next thread. Thread: any.

### `alph_recover_arm`

```mettle
export fn alph_recover_arm(r: AlphRecover*)
```

Arms r: a crash on its thread now resumes at the last alph_recover_save.
Arm before saving. Thread: the thread that owns r.

### `alph_recover_disarm`

```mettle
export fn alph_recover_disarm(r: AlphRecover*)
```

Disarms r: a crash on its thread ends the process again. A resumed crash
disarms it already. Thread: the thread that owns r.

## alph/sys/fd

Thin wrappers over the file descriptor, epoll and socket
system calls the IO layer uses. Each returns what the kernel returns: a
result, or a negative errno. They never block on their own; whether a
read blocks depends on the descriptor, and alph opens every descriptor it
manages non-blocking. Programs use alph_io_* and alph_tcp_* from
alph/core/node instead.

### `alph_sys_read`

```mettle
export fn alph_sys_read(fd: int64, buf: rawptr, size: int64) -> int64
```

Reads up to size bytes into buf. Returns the count, 0 at end of file, or
a negative errno (-ALPH_EAGAIN when a non-blocking descriptor is empty).
Thread: any.

### `alph_sys_write`

```mettle
export fn alph_sys_write(fd: int64, buf: rawptr, size: int64) -> int64
```

Writes up to size bytes from buf. Returns the count written, which may
be less than size, or a negative errno. Thread: any.

### `alph_sys_close`

```mettle
export fn alph_sys_close(fd: int64) -> int64
```

Closes fd. Returns 0 or a negative errno. Thread: any.

### `alph_sys_nonblock`

```mettle
export fn alph_sys_nonblock(fd: int64) -> int64
```

Makes fd non-blocking. Returns 0 or a negative errno. Thread: any.

### `alph_sys_epoll`

```mettle
export fn alph_sys_epoll() -> int64
```

A new epoll instance, closed on exec, or a negative errno. Thread: any.

### `alph_sys_epoll_ctl`

```mettle
export fn alph_sys_epoll_ctl(ep: int64, op: int64, fd: int64, events: int64, data: int64) -> int64
```

Adds, changes or removes fd in the epoll set ep.

```text
op      ALPH_EPOLL_CTL_ADD, _MOD or _DEL
events  ALPH_EPOLLIN and friends
data    the word epoll hands back with each event for fd
```

Returns 0 or a negative errno. Thread: any.

### `alph_sys_epoll_wait`

```mettle
export fn alph_sys_epoll_wait(ep: int64, events: rawptr, max: int64, timeout_ms: int64) -> int64
```

Waits up to timeout_ms (negative: without limit) for events on ep and
stores up to max of them in events, which must hold 12 bytes per event.
Returns how many, or a negative errno (-4 when a signal interrupted it).
Thread: any.

### `alph_sys_epoll_wait_ns`

```mettle
export fn alph_sys_epoll_wait_ns(ep: int64, events: rawptr, max: int64, timeout_ns: int64) -> int64
```

epoll_pwait2 (Linux 5.11): as alph_sys_epoll_wait with the timeout in
nanoseconds (negative: no limit). Returns -38 (ENOSYS) on older kernels.

### `alph_sys_event_mask`

```mettle
export fn alph_sys_event_mask(events: rawptr, i: int64) -> int64
```

The events mask of event i in an array filled by alph_sys_epoll_wait.

### `alph_sys_event_data`

```mettle
export fn alph_sys_event_data(events: rawptr, i: int64) -> int64
```

The data word of event i in an array filled by alph_sys_epoll_wait.

### `alph_sys_eventfd`

```mettle
export fn alph_sys_eventfd() -> int64
```

A new eventfd, non-blocking and closed on exec, or a negative errno.
Writing 8 bytes to it makes it readable until it is read. Thread: any.

### `alph_sys_sockaddr`

```mettle
export fn alph_sys_sockaddr(addr: uint8*, ip4: int64, port: int64)
```

Fills a 16-byte struct sockaddr_in for ip4 (host order, as from alph_ip4)
and port.

### `alph_sys_tcp_socket`

```mettle
export fn alph_sys_tcp_socket() -> int64
```

A new non-blocking TCP socket, closed on exec, or a negative errno.
Thread: any.

### `alph_sys_setsockopt`

```mettle
export fn alph_sys_setsockopt(fd: int64, level: int64, name: int64, value: int64) -> int64
```

Sets an integer socket option. Returns 0 or a negative errno.
Thread: any.

## alph/sys/linux

The Linux x86-64 system call numbers and flags alph uses.

Every raw syscall in the runtime goes through these names, so a port to
another platform starts here.

System call numbers.

```mettle
export const ALPH_SYS_READ: int64 = 0;
export const ALPH_SYS_WRITE: int64 = 1;
export const ALPH_SYS_CLOSE: int64 = 3;
export const ALPH_SYS_MMAP: int64 = 9;
export const ALPH_SYS_MPROTECT: int64 = 10;
export const ALPH_SYS_MUNMAP: int64 = 11;
export const ALPH_SYS_SOCKET: int64 = 41;
export const ALPH_SYS_CONNECT: int64 = 42;
export const ALPH_SYS_SHUTDOWN: int64 = 48;
export const ALPH_SYS_BIND: int64 = 49;
export const ALPH_SYS_LISTEN: int64 = 50;
export const ALPH_SYS_GETSOCKNAME: int64 = 51;
export const ALPH_SYS_SETSOCKOPT: int64 = 54;
export const ALPH_SYS_RECVMSG: int64 = 47;
export const ALPH_SYS_GETSOCKOPT: int64 = 55;
export const ALPH_SYS_FCNTL: int64 = 72;
export const ALPH_SYS_SIGALTSTACK: int64 = 131;
export const ALPH_SYS_FUTEX: int64 = 202;
export const ALPH_SYS_SCHED_GETAFFINITY: int64 = 204;
export const ALPH_SYS_CLOCK_GETTIME: int64 = 228;
export const ALPH_SYS_EPOLL_WAIT: int64 = 232;
export const ALPH_SYS_EPOLL_PWAIT2: int64 = 441;
export const ALPH_SYS_PRCTL: int64 = 157;
export const ALPH_PR_SET_TIMERSLACK: int64 = 29;
export const ALPH_SYS_EPOLL_CTL: int64 = 233;
export const ALPH_SYS_ACCEPT4: int64 = 288;
export const ALPH_SYS_EVENTFD2: int64 = 290;
export const ALPH_SYS_EPOLL_CREATE1: int64 = 291;
export const ALPH_SYS_OPEN: int64 = 2;
export const ALPH_SYS_RT_SIGPROCMASK: int64 = 14;
export const ALPH_SYS_RT_SIGACTION: int64 = 13;
export const ALPH_SYS_ACCESS: int64 = 21;
export const ALPH_SYS_DUP2: int64 = 33;
export const ALPH_SYS_CLONE: int64 = 56;
export const ALPH_SYS_EXECVE: int64 = 59;
export const ALPH_SYS_KILL: int64 = 62;
export const ALPH_SYS_CHDIR: int64 = 80;
export const ALPH_SYS_SETPGID: int64 = 109;
export const ALPH_SYS_EXIT_GROUP: int64 = 231;
export const ALPH_SYS_WAITID: int64 = 247;
export const ALPH_SYS_PIPE2: int64 = 293;
export const ALPH_SYS_PIDFD_OPEN: int64 = 434;
export const ALPH_SYS_SIGNALFD4: int64 = 289;
export const ALPH_SYS_GETPID: int64 = 39;
export const ALPH_SYS_GETPGID: int64 = 121;
```

Flags for those calls.

```mettle
export const ALPH_PROT_NONE: int64 = 0;
export const ALPH_PROT_READ_WRITE: int64 = 3;
export const ALPH_MAP_PRIVATE_ANONYMOUS: int64 = 0x22;
export const ALPH_SS_DISABLE: int64 = 2;
export const ALPH_CLOCK_MONOTONIC: int64 = 1;
export const ALPH_FUTEX_WAIT_PRIVATE: int64 = 128;
export const ALPH_FUTEX_WAKE_PRIVATE: int64 = 129;
export const ALPH_O_NONBLOCK: int64 = 2048;
export const ALPH_O_CLOEXEC: int64 = 524288;
export const ALPH_F_GETFL: int64 = 3;
export const ALPH_F_SETFL: int64 = 4;
export const ALPH_EPOLL_CTL_ADD: int64 = 1;
export const ALPH_EPOLL_CTL_DEL: int64 = 2;
export const ALPH_EPOLL_CTL_MOD: int64 = 3;
export const ALPH_EPOLLIN: int64 = 1;
export const ALPH_EPOLLOUT: int64 = 4;
export const ALPH_EPOLLERR: int64 = 8;
export const ALPH_EPOLLHUP: int64 = 16;
export const ALPH_EPOLLRDHUP: int64 = 8192;
export const ALPH_EPOLLONESHOT: int64 = 1073741824;
export const ALPH_AF_INET: int64 = 2;
export const ALPH_SOCK_STREAM: int64 = 1;
export const ALPH_SOCK_NONBLOCK: int64 = 2048;
export const ALPH_SOCK_CLOEXEC: int64 = 524288;
export const ALPH_SOL_SOCKET: int64 = 1;
export const ALPH_SO_TIMESTAMPING: int64 = 37;   // SO_TIMESTAMPING_OLD: timespecs, 64-bit on x86-64
export const ALPH_SOF_RX_SOFTWARE: int64 = 8;
export const ALPH_SOF_SOFTWARE: int64 = 16;
export const ALPH_SO_REUSEADDR: int64 = 2;
export const ALPH_SO_ERROR: int64 = 4;
export const ALPH_SO_REUSEPORT: int64 = 15;
export const ALPH_IPPROTO_TCP: int64 = 6;
export const ALPH_TCP_NODELAY: int64 = 1;
export const ALPH_EAGAIN: int64 = 11;
export const ALPH_EINPROGRESS: int64 = 115;
export const ALPH_ENOENT: int64 = 2;
export const ALPH_O_RDONLY: int64 = 0;
export const ALPH_O_RDWR: int64 = 2;
export const ALPH_O_WRONLY: int64 = 1;
export const ALPH_O_CREAT: int64 = 64;
export const ALPH_O_TRUNC: int64 = 512;
export const ALPH_X_OK: int64 = 1;
export const ALPH_SIGCHLD: int64 = 17;
export const ALPH_SIGKILL: int64 = 9;
export const ALPH_SIGHUP: int64 = 1;
export const ALPH_SIGINT: int64 = 2;
export const ALPH_SIGTERM: int64 = 15;
export const ALPH_SIG_BLOCK: int64 = 0;
export const ALPH_SIG_SETMASK: int64 = 2;
export const ALPH_IDTYPE_PID: int64 = 1;
export const ALPH_WNOHANG: int64 = 1;
export const ALPH_WEXITED: int64 = 4;
export const ALPH_CLD_EXITED: int64 = 1;
```

The memory page size, for guard pages.

```mettle
export const ALPH_PAGE_SIZE: int64 = 4096;
```

## alph/core/heap

Per-actor heaps, per-scheduler chunk pools and scratch.

Every actor owns an AlphHeap. Only that actor allocates from it, so
allocation takes no lock: blocks come from size-class free lists or are
carved from the heap's current chunk. When the actor exits, its chunks go
back to a pool as a whole. Nothing is collected and nothing ever pauses
other actors.

Chunks come from an AlphPool, one per scheduler, which carves them from
large mmap'd slabs and keeps freed ones for reuse. A pool has a spin lock,
but only its scheduler uses it in the normal course, so it is uncontended.
Blocks over 32 KiB bypass chunks and are mapped on their own.

Internal to the runtime: actors use alph_alloc, alph_free, alph_realloc
and alph_scratch from alph/core/node.

### `AlphPool`

```mettle
export struct AlphPool {
  lock: int64;
  free: int64[3];  // a list per chunk size
  slab: int64;     // the slab being carved
  slab_end: int64;
}
```

Free chunks of each size, ready for reuse. One per scheduler.

### `AlphHeap`

```mettle
export struct AlphHeap {
  free: int64[11];  // a free list per size class
  cur: int64;       // bump pointer in the current chunk
  end: int64;
  chunks: int64;    // every chunk the heap owns, linked through their headers
  next_size: int64; // index of the next chunk size to take
  large: int64;     // blocks mapped on their own, a doubly linked list
  used: int64;      // bytes in blocks handed out, headers included
  limit: int64;     // 0 for none
}
```

One actor's memory.

### `AlphScratch`

```mettle
export struct AlphScratch {
  first: int64;     // the chunk kept across resets
  cur: int64;
  end: int64;
  extra: int64;     // further chunks, returned on reset
}
```

Memory valid until the scratch is reset: after every handler call.

### `alph_pool_take`

```mettle
export fn alph_pool_take(pool: AlphPool*, index: int64) -> int64
```

Hands out a chunk of the given size index, or 0 when memory ran out.
Thread: any; the pool's lock is taken.

### `alph_pool_give`

```mettle
export fn alph_pool_give(pool: AlphPool*, chunks: int64)
```

Gives back a list of chunks linked through their first word.
Thread: any; the pool's lock is taken.

### `alph_heap_init`

```mettle
export fn alph_heap_init(heap: AlphHeap*, limit: int64)
```

Makes heap empty with the given byte limit (0 for none). It owns nothing.

### `alph_heap_alloc`

```mettle
export fn alph_heap_alloc(heap: AlphHeap*, pool: AlphPool*, size: int64) -> rawptr
```

Allocates size bytes, 16-byte aligned and not zeroed. Returns 0 when the
heap's limit would be passed or memory ran out.
Thread: the heap's owner.

### `alph_heap_size`

```mettle
export fn alph_heap_size(ptr: rawptr) -> int64
```

The usable size of a block from alph_heap_alloc.

### `alph_heap_free`

```mettle
export fn alph_heap_free(heap: AlphHeap*, ptr: rawptr)
```

Returns a block to the heap. ptr must come from this heap; 0 is ignored.
Thread: the heap's owner.

### `alph_heap_release`

```mettle
export fn alph_heap_release(heap: AlphHeap*, pool: AlphPool*)
```

Releases everything the heap owns: chunks go back to pool, mapped blocks
to the system. The heap is empty afterwards, with the same limit.
Thread: any thread that is the only one touching heap.

### `alph_scratch_alloc`

```mettle
export fn alph_scratch_alloc(s: AlphScratch*, pool: AlphPool*, size: int64) -> rawptr
```

Allocates size bytes valid until the next alph_scratch_reset, 16-byte
aligned and not zeroed. Returns 0 when memory ran out.
Thread: the scratch's owner.

### `alph_scratch_reset`

```mettle
export fn alph_scratch_reset(s: AlphScratch*, pool: AlphPool*)
```

Frees everything allocated since the last reset, keeping the first chunk.
Thread: the scratch's owner.

## alph/core/queue

Intrusive multi-producer, single-consumer queue (Vyukov).

A node is any struct whose first field is `next: int64`. Push is wait-free
and may run on any thread. Pop runs only on the queue's one consumer. The
node uses it for run queues and signal queues; actors never touch it.

### `AlphQueue`

```mettle
export struct AlphQueue {
  head: int64;  // last pushed node; producers swap it
  tail: int64;  // next node to pop; consumer only
  stub: int64;  // a node made of just a next field
}
```

A queue holds the address of its own stub, so it must not move after
alph_queue_init.

### `alph_queue_init`

```mettle
export fn alph_queue_init(q: AlphQueue*)
```

Makes q empty. Thread: any, before q is used.

### `alph_queue_push`

```mettle
export fn alph_queue_push(q: AlphQueue*, node: rawptr)
```

Appends node, whose first field must be `next: int64`. The queue keeps the
node until it is popped; do not push it again before then. Thread: any.

### `alph_queue_pop`

```mettle
export fn alph_queue_pop(q: AlphQueue*) -> int64
```

Removes and returns the oldest node, or 0 when the queue is empty. It also
returns 0 while a push is between its two steps; every pusher wakes the
consumer after it finishes, so treating that 0 as empty loses nothing.
Thread: the queue's one consumer.
