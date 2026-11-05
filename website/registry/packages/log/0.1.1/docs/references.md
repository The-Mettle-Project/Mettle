# References

## Standards followed

| What | Standard | Where |
|---|---|---|
| Level numbers | OpenTelemetry Logs Data Model, SeverityNumber, <https://opentelemetry.io/docs/specs/otel/logs/data-model/#field-severitynumber> | `log/level` |
| Syslog severities, facilities, RFC 5424 message | RFC 5424, The Syslog Protocol, <https://www.rfc-editor.org/rfc/rfc5424> (sections 6.1, 6.2) | `log/level`, `LOG_FMT_RFC5424` |
| Local syslog message | RFC 3164 section 4.1 and glibc `syslog(3)`'s `<PRI>TAG[PID]: MSG`; the timestamp is left out because rsyslog's imuxsock (`IgnoreTimestamp`, on by default) and journald stamp messages on arrival | `LOG_FMT_SYSLOG` |
| Journald | systemd, Native Journal Protocol, <https://systemd.io/JOURNAL_NATIVE_PROTOCOL/>: `KEY=value` lines, the binary form for values with newlines, field name rules | `LOG_FMT_JOURNAL` |
| JSON | RFC 8259, through Codec's writer; one object per line as in JSON Lines, <https://jsonlines.org> | `LOG_FMT_JSON` |
| logfmt | Brandur Leach, "logfmt", <https://brandur.org/logfmt>, and the go-logfmt encoder's quoting rules | `LOG_FMT_LOGFMT` |
| Timestamps | RFC 3339, <https://www.rfc-editor.org/rfc/rfc3339> | `log_put_time` |
| Colour | ECMA-48 SGR escapes; NO_COLOR, <https://no-color.org>; CLICOLOR_FORCE, <https://bixense.com/clicolors/> | `log/sink` |
| Log injection | CWE-117, <https://cwe.mitre.org/data/definitions/117.html> | every format |

## Ideas taken, and from where

| Idea | Source |
|---|---|
| Filter syntax `target=level,...`, a bare target means all levels, the longest match wins | Rust `env_logger` (MIT/Apache-2.0) |
| A cheap global "max level" check before anything else, then a cached per-callsite answer that is redone when the filter changes | Rust `log` (`max_level`) and `tracing` (callsite `Interest` cache) (MIT) |
| Child loggers with fields (`log_with`), the key names `time`, `level`, `msg`, `error` | Go `log/slog` (BSD-3-Clause) and `zap`'s `With` (MIT) |
| Fields as a typed array rather than format strings; no allocation on the hot path; check the level first | `zap` and `zerolog` (MIT) |
| Size rotation numbering (`path.1` newest, `path.N` removed), sinks with their own levels, an async logger with a bounded queue and block or drop policies | `spdlog` (MIT) |
| Overload protection: drop under load but count and report the drops with a record of their own; flush on the most severe levels | Erlang/OTP `logger` (Apache-2.0) |
| Level colours (trace magenta, debug blue, info green, warn yellow, error red) | `env_logger` and `tracing-subscriber` |
| Date from a day count | Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms", <https://howardhinnant.github.io/date_algorithms.html> (public domain) |

No code was copied from any of these; only the ideas above.

## How it is checked

- Exact output for every format, including escaping, special floats,
  journald's binary form and field-name rules.
- Journald and syslog delivery were checked by hand with `journalctl -o
  verbose` (fields `PRIORITY`, `TARGET`, `MESSAGE` and custom fields arrive).
- Eight threads through a blocking queue (every record arrives exactly once,
  lines whole), through a dropping queue (written plus reported drops equal
  sent), and through a synchronous sink.
- File rotation sizes and order.
- A fuzz run: 20,000 random filter specs and random records in every format;
  text and logfmt always one line with no raw control bytes, JSON always valid.
- The fast time formatter against std/datetime from year 1 to 9999.
- All tests pass in debug and release.
