# raize_telemetry

Named events, handlers and spans for Mettle programs, in the style of Elixir's `:telemetry`. It is pure Mettle with no dependencies. `tm_execute` is lock-free and returns almost immediately when nothing is listening.

The mbuild package is `raize_telemetry` and the public prefix is `tm_`. The API is specified in [docs/SPEC.md](docs/SPEC.md).

**v0.1** has events, handlers, spans and the clock. Metrics (counters, histograms) come in v0.2. It runs on Linux x86-64 only, because it uses asm atomics and a raw `clock_gettime`.

```mettle
import "raize_telemetry/raize_telemetry";

var g_req_stop: int32 = 0;

fn log_request(ev: TmEvent*, cfg: rawptr) {
  var d: TmKV* = tm_kv_find(ev->meas, ev->n_meas, "duration_ns");
  // ... copy anything you keep: meas/meta are borrowed for this call only
}

fn main() -> int32 {
  tm_init();                                       // once, before threads start
  g_req_stop = tm_event("app.request.stop");       // register ids at startup
  tm_attach("logger", "app.request", &log_request, (rawptr)0);

  if (tm_has_handlers(g_req_stop)) {               // skip building KVs when unused
    var meas: TmKV[1];
    meas[0] = tm_int("duration_ns", 1234);
    var meta: TmKV[1];
    meta[0] = tm_str("route", "/users/:id");
    tm_execute(g_req_stop, &meas[0], 1, &meta[0], 1);
  }
  tm_shutdown();
  return 0;
}
```

Rules worth knowing:
- **Prefixes match whole segments.** `app.request` matches `app.request.stop` but not `app.requests`.
- **`cfg` must outlive `tm_detach`.** Keep it valid until `tm_shutdown`, because a handler can still run once on another thread after it is detached.
- **Handlers run on the caller's thread.** They must be thread-safe and must not fail.

## Build and test

```bash
export METTLE=../Mettle/bin/mettle
../mbuild/build/linux/release/mbuild test-programs    # debug + release suites, benchmark
```

License: Apache 2.0.
