# Design

## A log call

1. `log_info(lg, ...)` first compares the level with a global floor: the most
   verbose level that any target allows. When the filter is `info` with no
   directives, a debug call stops here.
2. Past the floor, the logger's cached level decides. Each logger keeps the
   level its target gets from the filter and the filter generation it was
   worked out for. When `log_set_filter` installs a new filter, the
   generation moves on and each logger works its level out again on its next
   call. This is the callsite interest cache of Rust's `tracing`, kept per
   logger since Mettle has no macros to make a static per call site.
3. A record is built on the stack: level, target, message, the logger's
   fields, the call's fields, the time (std/time `time_unix_ns`) and the
   thread id.
4. For each sink at or below the record's level, the record is formatted
   into a 768-byte buffer on the stack (a bigger record moves to the heap)
   and handed to the sink's writer, or to its queue if it is async.

No lock is taken on this path, and no memory is allocated unless a record
is larger than the stack buffer. Formatting on the calling thread's stack is
the per-thread buffering: threads and alph actors never share a buffer.

Field arguments are evaluated before the call, as with any function. With
no fields a disabled call costs about 1.5 ns; each field adds a few ns of
building. Put costly fields behind `log_on`, which costs about 0.3 ns.

## Writing

A synchronous sink writes each record with one call. For stderr that is one
`write`, so records from different threads never interleave. File sinks
take a mutex because rotation must not race with writes.

## Async sinks and back-pressure

`log_sink_async` gives a sink a bounded queue and a writer thread.

- The queue is a ring of slots (a power of two). Two counting semaphores
  track free slots and filled slots. A producer takes a free slot with the
  semaphore, claims a slot index with an atomic increment, copies its bytes
  into the slot's own buffer (which grows once and is reused), marks the slot
  ready and signals. No lock is held, and in a steady state nothing is
  allocated.
- The writer thread takes every filled slot it can, in order, joins them
  into one buffer of up to 64 KiB and makes one write. Datagram sinks
  (syslog, journald) get one write per record instead.
- `LOG_BLOCK`: a full queue makes the producer wait for space. Nothing is
  lost.
- `LOG_DROP`: a full queue drops the record and counts it, unless the record
  is ERROR or FATAL, which wait. The writer reports the count with a WARN
  record of its own (`target=log msg="queue full, records dropped"
  dropped=N`), in the sink's format, as Erlang's logger does.
- `log_flush` waits until the writer has written everything queued before
  the call. `log_fatal` flushes, so a FATAL record is on disk before the
  program goes on to exit.

## Filters

Directives are the `env_logger` syntax. A target is a path such as
`alph/actor` or `net::tcp`; a directive covers its own target and
everything below it at a `/`, `.` or `:` boundary. The longest directive
wins.

A replaced filter is not freed, because another thread may be reading it
at that moment. Each `log_set_filter` keeps a few dozen bytes; set the
filter at start-up and on rare changes, not in a loop.

## Formats

All text output escapes control characters (CWE-117: one record is one
line, and a log line cannot be forged from input). JSON goes through Codec's
writer with lossy UTF-8, so invalid UTF-8 becomes U+FFFD instead of failing.

Timestamps are RFC 3339 UTC with microseconds, written with Howard
Hinnant's `civil_from_days`; the test compares it with std/datetime across
years 1 to 9999.

## Shared with telemetry

`LogField` has the same layout and kind numbers as telemetry's `TmKV`, and
the levels are OpenTelemetry severity numbers, so log records and telemetry
events can carry the same fields, and an exporter can map levels to
OpenTelemetry without a table. Log is for records people read; telemetry is
for measurements and spans that code consumes.

## Costs (release, Linux x86-64, loaded 16-core machine)

| Case | ns per call |
|---|---|
| `log_on` with the level off | 0.3 |
| disabled call, no fields | 1.5 |
| disabled call, one field | 10 |
| enabled, logfmt, 3 fields, to a writer that discards | 450 |
| enabled, JSON, 3 fields, to a writer that discards | 580 |
| enabled, logfmt, async to /dev/null, 4 threads (wall time per record) | 260 |

Of the enabled cost, about 175 ns is reading the wall clock: std/time makes
a `clock_gettime` system call on Linux. Formatting three fields as logfmt
takes about 115 ns. `mbuild bench` runs these.
