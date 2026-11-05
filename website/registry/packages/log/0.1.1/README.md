# log

Structured logging for [Mettle](https://github.com/Lex-Dellimere/Mettle), in pure Mettle.

- Levels on OpenTelemetry's severity numbers: TRACE, DEBUG, INFO, WARN, ERROR, FATAL.
- Key-value fields: strings, integers, floats, booleans, durations.
- Per-target filters from the environment, written like `RUST_LOG`.
- A level that is off costs one compare (`log_on` is about 0.3 ns).
- Safe from any number of threads and alph actors: records are formatted on
  the caller's stack, one write per record, no shared buffer.
- Async sinks with a bounded queue and a choice of back-pressure: wait, or
  drop and report how many were dropped.
- Output to stderr (coloured text), files with size or daily rotation, JSON
  lines, logfmt, local syslog and journald.

## Use it

In `mbuild.mettle`:

```mettle
fn deps() {
  mb_dep_path("log", "../log");
}
```

```mettle
import "log/log";

var g_log: LogLogger* = (LogLogger*)0;

fn main() -> int32 {
  g_log = log_logger("app/server");
  log_info(g_log, "listening", log_str("addr", "0.0.0.0"), log_int("port", 8080));

  var req: LogLogger* = log_with(g_log, log_str("request_id", "r-17"));
  log_warn(req, "slow handler", log_dur("took", (int64)250000000));

  if (log_on(g_log, LOG_DEBUG)) {
    // work that only a debug record needs
  }
  log_shutdown();
  return 0;
}
```

```text
2026-11-05T10:53:01.123456Z  INFO app/server: listening addr=0.0.0.0 port=8080
2026-11-05T10:53:01.123502Z  WARN app/server: slow handler request_id=r-17 took=250ms
```

With no setup, the first log call reads the environment and writes to stderr.

| Variable | Meaning |
|---|---|
| `METTLE_LOG` | the filter, e.g. `warn,app=debug,app/db=trace,noisy=off` (default `info`) |
| `METTLE_LOG_FORMAT` | `text` (default), `logfmt` or `json` |
| `METTLE_LOG_STYLE` | colour: `auto` (default), `always` or `never` |
| `NO_COLOR`, `CLICOLOR_FORCE`, `TERM=dumb` | honoured when the style is `auto` |

## Sinks

```mettle
log_set_filter("info,db=debug");
log_add_sink(log_sink_stderr(LOG_FMT_TEXT, 0));

var f: LogSink* = log_sink_file("app.log", LOG_FMT_JSON, 0, (int64)10485760, 5);  // 10 MiB, keep 5
log_sink_async(f, 4096, LOG_BLOCK);
log_add_sink(f);

log_add_sink(log_sink_journal("app"));
```

| Sink | |
|---|---|
| `log_sink_stderr(kind, flags)`, `log_sink_fd(fd, kind, flags)` | a descriptor |
| `log_sink_file(path, kind, flags, max_bytes, keep)` | appends; rotates to `path.1` .. `path.keep` |
| `log_sink_file_daily(path, kind, flags)` | a new file each UTC day; old ones are `path.YYYY-MM-DD` |
| `log_sink_syslog(tag, facility)` | the local syslog socket `/dev/log` |
| `log_sink_journal(tag)` | journald's native socket, fields kept as journal fields |
| `log_sink_unix(path, kind, tag)` | datagrams to any Unix socket |
| `log_sink_memory(kind, flags)` | keeps output in memory (tests) |
| `log_sink_callback(fn, ctx)` | gets each `LogRecord` unformatted |

Formats: `LOG_FMT_TEXT`, `LOG_FMT_LOGFMT`, `LOG_FMT_JSON`, `LOG_FMT_SYSLOG`,
`LOG_FMT_RFC5424`, `LOG_FMT_JOURNAL`. Flags: `LOG_COLOR`, `LOG_NO_TIME`,
`LOG_THREAD`, `LOG_NO_TARGET`.

## Docs

- [docs/api.md](docs/api.md): every call.
- [docs/design.md](docs/design.md): how it works, threads, the queue, costs.
- [docs/references.md](docs/references.md): standards followed and where the ideas came from.

## Build and test

```bash
mbuild test
mbuild test -r
mbuild bench
```

The tests cover levels, filters, exact output of every format, escaping,
loggers and context fields, eight threads through blocking and dropping
queues, file rotation, and a fuzz run over filters and all formats with a
JSON validity check.

## Platforms

Tested on Linux x86-64. The stderr, file, memory and callback sinks use
only portable stdlib calls; the syslog and journald sinks are Linux only and
return 0 elsewhere.

## License

Apache-2.0. See [LICENSE](LICENSE).
