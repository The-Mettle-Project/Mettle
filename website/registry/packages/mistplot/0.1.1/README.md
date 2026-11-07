# MistPlot

Charts for [Mettle](https://github.com/suidvandiewereld/Mettle): line, area, bar
and scatter, static or live. One `MpChart` object holds the data and the style.
A frame pipeline turns it into a scene (rects, lines, paths, markers and text),
and small renderers turn the scene into SVG, an HTML `<figure>`, terminal text
or MythicUI paint calls. Output is deterministic: the same chart renders the
same bytes.

```
src/core/        buffers, colors, scales, ticks, number format, render targets
src/data/        sample columns, min/max summaries, decimation, smoothing
src/scene/       the scene (display list), paths, pixel snapping
src/chart/       the chart object, setters, frame pipeline, picking
src/render/      SVG, HTML, terminal, files
src/sync/        mp_chart_share (locking for other threads)
integrations/    raize (RaizeWeb), mythic (MythicUI), meodb (MeoDB)
cmd/golden/      prints the tests/golden/ fixtures (tools/golden.sh --update rewrites them)
docs/API.md      every export: inputs, outputs, errors, ownership (tools/apidoc.sh)
```

## Build

MistPlot needs the head Mettle compiler (`../Mettle/bin/mettle`), not the
v0.17.0 release.

```bash
./check.sh
```

`check.sh` checks the doc comments and `docs/API.md`, runs every test in the
interpreter, builds and runs each test in debug and release, compares
`cmd/golden` with every file in `tests/golden/`, and runs the RaizeWeb, MythicUI
and MeoDB integration builds. It prints `PASS`. `MEODB_DIR` picks the MeoDB
checkout (default `../meodb`); an integration whose library isn't on disk is
skipped.

To use MistPlot from an app, add it to the app's `mbuild.mettle`:

```mettle
mb_dep_path("mistplot", "../mistplot");
```

and `import "mistplot/mistplot";`. The integrations live outside `src/`, so an
app imports one by path (for example `import "../mistplot/integrations/raize/mp_raize";`)
and maps its host library itself.

## Ownership

1. A chart copies everything you give it. `mp_chart_free` releases all of it.
2. An `MpBuf` you pass in belongs to you; each call clears it and fills it.
3. Pointers returned from a chart (`mp_chart_scene`, `mp_chart_svg_view`) are
   borrowed: valid only while you hold `mp_chart_lock`, until the next render.
4. An integration registration borrows the chart. Unregister it before you free the chart.
5. Returned strings are views of a buffer you passed, or static text.

Every call returns `MP_OK` or an `MP_ERR_*` code; the chart also keeps the first
error (`mp_chart_error`). Size, scale and text measurement are not chart state:
you pass them per render, so one chart can draw as an SVG, a terminal and a
HiDPI window. Call `mp_chart_share(c)` before other threads touch a chart.

## Static chart in RaizeWeb

`integrations/raize/mp_raize.mettle`. Your app maps RaizeWeb's `raize_core`,
`raize_realtime`, `raize_app` and `raize_template`.

```mettle
import "raize_core/context/context";
import "raize_app/app";
import "mistplot/mistplot";
import "../mistplot/integrations/raize/mp_raize";

var g_sales: MpChart* = 0;            // keep module-scope charts behind a pointer

fn h_sales(ctx: RzContext*) -> int32 { return mp_raize_svg(ctx, g_sales, 640.0, 360.0); }

fn sales_app(app: RzApp*) {
  g_sales = mp_chart_new(MP_BAR);
  mp_chart_set_title(g_sales, "Requests by region");
  mp_chart_set_y_format(g_sales, MP_FMT_SI);
  var s: int32 = mp_chart_series(g_sales, "requests");
  mp_chart_set_value(g_sales, s, "north", 420.0);
  mp_chart_set_value(g_sales, s, "south", 310.0);
  mp_chart_set_value(g_sales, s, "east", 515.0);
  if (mp_chart_error(g_sales) == MP_OK) { rz_get(app, "/sales.svg", &h_sales); }
}
```

Every helper returns the HTTP status it built, as RaizeWeb's router requires.
`mp_raize_svg` sends an `ETag` of `<generation>-<seq>`, so an unchanged
chart costs a 304 and no render. `mp_raize_html` and `mp_raize_embed` give an
HTML fragment; `mp_raize_tpl_figure` and `mp_raize_tpl_embed` fill a template
variable, so a page can hold `{{{chart}}}`.

For a live page, `mp_raize_channel(app, c, topic, socket_path)` registers the
chart on a channel topic (and shares it). After a batch of pushes,
`mp_raize_notify(c)` sends the new tag to joined pages, which fetch the frame
through the same ETag route. While the socket is down the page polls.

Example: `integrations/raize/examples/dashboard.mettle` (port 8088,
`--run-for N` to stop on its own).

## Live chart in MythicUI

`integrations/mythic/mp_mythic.mettle`. Your app maps MythicUI as `mythic`.

```mettle
import "mythic/mythic";
import "mistplot/mistplot";
import "../mistplot/integrations/mythic/mp_mythic";

struct App { cpu: MpChart*; }

fn frame(ui: MythicUi*, ud: rawptr) {
  var app: App* = (App*)ud;
  if (ui->pass == 0) {                  // MythicUI may run a frame 3 times
    var t: float64 = (float64)mythic_time_us(ui) / 1000000.0;
    mp_chart_push(app->cpu, 0, t, read_cpu());
    mp_chart_set_now(app->cpu, t);      // scroll smoothly between samples
  }
  mp_mythic_chart(ui, app->cpu, mythic_grow(), mythic_fixed(320));
}

// setup
app->cpu = mp_chart_new(MP_AREA);
mp_chart_set_title(app->cpu, "CPU");
mp_chart_set_x_format(app->cpu, MP_FMT_TIME);
mp_chart_set_y_range(app->cpu, 0.0, 100.0);
mp_chart_keep(app->cpu, 100000);      // a ring of 100000 samples
mp_chart_window(app->cpu, 60.0);      // show the last 60 x units
mp_chart_series(app->cpu, "cpu %");
```

`mp_mythic_chart` follows the MythicUI theme (unless you called
`mp_chart_set_theme`), measures text with MythicUI's fonts, draws at the
window's device scale, shows a hover readout and asks for the next frame while
the chart is live. Push on pass 0 only; call it on every pass.
`mp_mythic_save_png(c, w, h, scale, path)` writes a HiDPI PNG.

Call `mp_chart_set_now(c, t)` every frame with your clock in x units (the chart
never reads one): the window becomes exactly `[t - span, t]`, so it scrolls
continuously instead of stepping at each sample, and an automatic y axis eases
towards the data plus 15% (fast up, slow down) instead of snapping to nice ticks.
`mp_chart_clear_now` and `mp_chart_set_y_ease(c, false)` turn these off.

A frame costs O(plot width) however many samples a sorted series holds: each
pixel column keeps its first, min, max and last sample, so peaks survive.

Example: `integrations/mythic/examples/live_dashboard.mettle`.

## Live chart in the terminal

```mettle
import "std/io";
import "std/math";
import "std/thread";
import "std/bench";
import "mistplot/mistplot";

fn main() -> int32 {
  var c: MpChart* = mp_chart_new(MP_LINE);
  mp_chart_set_title(c, "sin(t)");
  mp_chart_set_x_format(c, MP_FMT_TIME);
  mp_chart_keep(c, 10000);
  mp_chart_window(c, 30.0);
  var s: int32 = mp_chart_series(c, "signal");
  var out: MpBuf* = mp_buf_new(65536);
  var t0: int64 = bench_time_us();
  var i: int32 = 0;
  while (i < 600) {
    var t: float64 = (float64)(bench_time_us() - t0) / 1000000.0;
    mp_chart_push(c, s, t, sin(t));
    if (mp_chart_term(c, 100, 28, MP_TERM_BRAILLE | MP_TERM_HOME, out) == MP_OK) {
      print(mp_buf_str(out));           // MP_TERM_HOME redraws in place
    }
    thread_sleep_ms(50);
    i = i + 1;
  }
  mp_buf_free(out);
  mp_chart_free(c);
  return 0;
}
```

Modes are `MP_TERM_BRAILLE`, `MP_TERM_BLOCKS` and `MP_TERM_ASCII`
(`MP_TERM_AUTO` picks blocks for bar charts); add `MP_TERM_PLAIN` for output
with no escape codes. To write a file instead: `mp_chart_svg(c, w, h, out)`
fills a buffer and `mp_chart_save_svg(c, w, h, path)` saves it.

## MeoDB

`integrations/meodb/mp_meo.mettle`, on MeoDB's committed-change feed. Your app
maps MeoDB as `meodb`. A chart follows a table: either you reload it on a change
(`mp_meo_watch_table`), or you map columns with `mp_meo_watch_xy` and committed
inserts go straight in.

```mettle
var g_chart: MpChart* = 0;
var g_watch: MpMeoWatch* = 0;

fn reload_readings(ctx: rawptr, table_id: uint32, c: MpChart*) -> int32 {
  mp_chart_clear(c);                    // runs under the chart lock: no half-drawn frame
  // re-scan the table and mp_chart_push each row
  return mp_meo_set_subtitle(g_watch, table_id, c);
}

// setup
g_chart = mp_chart_new(MP_LINE);
mp_chart_window(g_chart, 30.0);
g_watch = mp_meo_watch_new(feed);
mp_meo_watch_table(g_watch, READINGS, g_chart, &reload_readings, (rawptr)app);

// poll thread
mp_meo_poll(g_watch);                   // apply the committed changes
mp_raize_notify(g_chart);               // tell joined pages, if the chart is on a channel
```

Only committed changes count: a rolled-back transaction reloads nothing, and a
reader that fell behind the feed reloads everything. `mp_meo_table_stats` says
when a table last changed and at which commit. When the writer is another
process, `mp_meo_probe_check` watches the file's commit counter instead.

Example: `integrations/meodb/examples/meo_live_chart.mettle`.

## Compiler issues

Known head-compiler bugs MistPlot works around:

- `--release`: assigning a struct returned by value to a module-scope variable
  fails (`global_access`) when it nests a struct from another module. Keep such
  globals on the heap, and fill `MpChartInfo` through an out-pointer.
- Debug: an exported function with a `string` parameter, called through a
  function pointer, receives a garbage string. Callbacks are therefore never
  exported (`tools/lint.sh` checks it).
- A literal `0` can't be passed for a function-pointer parameter.

## License

Apache License 2.0. See [LICENSE](LICENSE).
