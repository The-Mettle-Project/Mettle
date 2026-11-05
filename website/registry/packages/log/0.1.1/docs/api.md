# API

`import "log/log";` brings in everything below. Exported names start with
`log_`, `Log` or `LOG_`; names starting with `log__` are internal.

## Levels (`log/level`)

| Constant | Value | Syslog severity |
|---|---|---|
| `LOG_TRACE` | 1 | 7 debug |
| `LOG_DEBUG` | 5 | 7 debug |
| `LOG_INFO` | 9 | 6 informational |
| `LOG_WARN` | 13 | 4 warning |
| `LOG_ERROR` | 17 | 3 error |
| `LOG_FATAL` | 21 | 2 critical |
| `LOG_OFF` | 25 | nothing passes |

The values are OpenTelemetry SeverityNumbers. Each named level starts a range
of four (9..12 are all INFO), so any number from 1 to 24 is a valid level.

| Call | |
|---|---|
| `log_level_name(l) -> string` | `"INFO"` |
| `log_level_key(l) -> string` | `"info"` |
| `log_level_base(l) -> int32` | the named level that l falls in |
| `log_level_parse(s) -> int32` | `trace`, `debug`, `info`, `warn`/`warning`, `error`/`err`, `fatal`/`critical`/`crit`, `off`/`none` (any case) or `1`..`25`; -1 otherwise |
| `log_level_syslog(l) -> int32` | the RFC 5424 severity |

## Fields (`log/field`)

```mettle
struct LogValue { kind: int32; i: int64; f: float64; s: string; }
struct LogField { key: string; val: LogValue; }
```

| Call | kind |
|---|---|
| `log_int(key, int64)` | `LOG_INT` (0) |
| `log_f64(key, float64)` | `LOG_F64` (1) |
| `log_bool(key, bool)` | `LOG_BOOL` (2) |
| `log_str(key, string)` | `LOG_STR` (3) |
| `log_uint(key, uint64)` | `LOG_UINT` (4) |
| `log_dur(key, ns)` | `LOG_DUR` (5): text shows `1.5ms`, JSON the nanoseconds |
| `log_err(msg)` | `log_str("error", msg)` |

Kinds 0 to 3 and the layout match telemetry's `TmKV`, so a `TmKV*` array can
be passed to `log_emit` with a cast.

Fields are borrowed for the call. A logger made with `log_with` copies its
fields.

`LogRecord` is what sinks see: `level`, `target`, `msg`, `ctx`/`n_ctx` (the
logger's fields), `fields`/`n_fields` (the call's), `time_ns` (unix
nanoseconds) and `thread`.

## Loggers (`log/logger`)

| Call | |
|---|---|
| `log_logger(target) -> LogLogger*` | a logger for a target such as `"net/http"`; keep it, it is never freed |
| `log_root() -> LogLogger*` | the logger with no target |
| `log_with(lg, fields...) -> LogLogger*` | a child that adds fields to every record |
| `log_on(lg, level) -> bool` | whether a record would be kept |
| `log_trace`, `log_debug`, `log_info`, `log_warn`, `log_error`, `log_fatal` `(lg, msg, fields...)` | log at that level |
| `log_at(lg, level, msg, fields...)` | log at any level |
| `log_emit(lg, level, msg, fields*, n)` | send to the sinks without checking the filter |

`log_fatal` waits until async sinks have written the record. It does not
exit the program.

## Setup

| Call | |
|---|---|
| `log_init()` | set up from the environment now (otherwise the first call does it) |
| `log_set_filter(spec) -> int32` | replace the filter; returns how many directives were unreadable |
| `log_add_sink(s) -> bool` | add a sink (up to `LOG_MAX_SINKS`, 16); the first one replaces the default stderr sink |
| `log_flush()` | wait until async sinks have written everything so far |
| `log_shutdown()` | flush and close every sink; call it last |

Setup from the environment reads `METTLE_LOG` (filter, default `info`),
`METTLE_LOG_FORMAT` (`text`, `logfmt`, `json`) and `METTLE_LOG_STYLE`
(`auto`, `always`, `never`), and adds a stderr sink unless one was added.

## Filters (`log/filter`)

A spec is a comma-separated list of `LEVEL`, `TARGET` or `TARGET=LEVEL`.
`LEVEL` alone sets the default. `TARGET` alone allows every level. A
directive covers a target equal to it or starting with it followed by `/`,
`.` or `:`. The longest matching directive wins.

| Call | |
|---|---|
| `log_filter_parse(spec, f*)` | read a spec; bad directives are counted in `f.bad` and skipped |
| `log_filter_level(f*, target) -> int32` | the lowest level that passes for target |
| `log_filter_free(f*)` | |
| `log_target_matches(directive, target) -> bool` | |

## Formats (`log/format`)

| Kind | Output |
|---|---|
| `LOG_FMT_TEXT` | `2026-11-05T10:53:01.123456Z  INFO net/http: connected peer=10.0.0.2` |
| `LOG_FMT_LOGFMT` | `time=2026-11-05T10:53:01.123456Z level=info target=net/http msg=connected peer=10.0.0.2` |
| `LOG_FMT_JSON` | `{"time":"2026-11-05T10:53:01.123456Z","level":"info","target":"net/http","msg":"connected","peer":"10.0.0.2"}` |
| `LOG_FMT_SYSLOG` | `<14>app[123]: connected peer=10.0.0.2` |
| `LOG_FMT_RFC5424` | `<14>1 2026-11-05T10:53:01.123456Z host app 123 net/http - connected peer=10.0.0.2` |
| `LOG_FMT_JOURNAL` | `PRIORITY=6`, `SYSLOG_IDENTIFIER`, `TARGET`, `MESSAGE` and one upper-cased field per key |

| Flag | |
|---|---|
| `LOG_COLOR` | ANSI colours in text |
| `LOG_NO_TIME` | no timestamp |
| `LOG_THREAD` | add the thread id |
| `LOG_NO_TARGET` | no target |

Times are UTC, RFC 3339, with microseconds. `log_format(fmt*, record*, buf*)`
formats one record into a Codec `CodecBuf`, and `log_put_time(buf*, ns)`
writes a timestamp.

Every format escapes control characters, so one record is always one line.
logfmt and text quote a value when it is empty or holds a space, `=`, `"` or
a control character. Keys with those bytes get `_` in their place.

## Sinks (`log/sink`)

| Call | |
|---|---|
| `log_sink_stderr(kind, flags)` | stderr; adds `LOG_COLOR` to text when `log_color_auto(2)` |
| `log_sink_fd(fd, kind, flags)` | any descriptor |
| `log_sink_file(path, kind, flags, max_bytes, keep)` | append; rotate when a record would pass max_bytes (0: never) |
| `log_sink_file_daily(path, kind, flags)` | rotate at UTC midnight to `path.YYYY-MM-DD` |
| `log_sink_syslog(tag, facility)` | `/dev/log`; facility `LOG_FAC_USER`, `LOG_FAC_DAEMON`, `LOG_FAC_LOCAL0`.. (any 0..23) |
| `log_sink_journal(tag)` | `/run/systemd/journal/socket` |
| `log_sink_unix(path, kind, tag)` | datagrams to a Unix socket |
| `log_sink_memory(kind, flags)` | in memory; `log_sink_text(s)` and `log_sink_clear(s)` |
| `log_sink_callback(fn(rawptr, LogRecord*) -> int32 with alloc, syscall, ctx)` | records, unformatted |
| `log_sink_new(kind, flags)` | a bare sink: set `write` and `ctx` for your own writer |
| `log_sink_level(s, level)` | this sink skips records below level |
| `log_sink_async(s, capacity, policy) -> bool` | write from a background thread; `LOG_BLOCK` or `LOG_DROP` |
| `log_sink_flush(s, timeout_ms) -> bool` | wait for an async sink to catch up |
| `log_sink_dropped(s) -> int32` | drops not yet reported |
| `log_sink_close(s)` | flush, stop the thread, close the file or socket |
| `log_color_auto(fd) -> bool` | the colour decision for fd |

File and socket sinks return 0 when they cannot be opened.

A writer is `fn(ctx: rawptr, bytes: uint8*, n: int64) -> int64 with alloc,
syscall`, a callback `fn(rawptr, LogRecord*) -> int32 with alloc, syscall`.
These effect clauses close the calls, so every log call can run inside code
that declares `with alloc, syscall` (alph actors and offload jobs). A writer
or callback may not use `asm` or any other effect. A writer must be
safe to call from several threads at once unless the sink is async (then
only the writer thread calls it).
