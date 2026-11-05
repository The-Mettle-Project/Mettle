# tui

Terminal user interfaces for Mettle, in pure Mettle. No C, no ncurses.

- Full-screen sessions: raw mode, the alternate screen, a hidden cursor,
  and the terminal put back as it was on exit, Ctrl-C, SIGTERM and SIGHUP.
- Resizing (SIGWINCH) and signals arrive as events through a signal
  descriptor from std/term, in
  the same wait as the keyboard, with no signal handlers.
- A double-buffered cell grid: each frame only writes the cells that
  changed, inside synchronized-output brackets, so screens do not flicker.
- True colour with automatic 256 and 16 colour fallbacks, bold, dim,
  italic, underline, blink, reverse and strike.
- Correct widths for UTF-8 text: East Asian wide characters, combining
  marks, emoji, skin tones, ZWJ sequences and flags.
- Input: keys with Shift, Alt and Ctrl, function keys, kitty's CSI u keys,
  SGR mouse (clicks, wheel, drags, moves), bracketed paste and focus.
- Layout: split rows and columns by fixed size, percentage or weighted fill.
- Widgets: boxes with plain, rounded, double and thick borders and titles,
  text with wrapping and alignment, styled spans, scroll lists, tables,
  scrollbars, gauges, btop-style meters, sparklines, a braille canvas and
  line graphs in braille or block characters, with colour gradients.
- An event loop with timers for live dashboards.
- A headless backend for tests: render to memory, feed keys, check exact
  screens and exact output bytes, with a clock that only moves when asked.

Linux only for now: the terminal parts come from std/term, which also
runs on macOS, but the input wait still uses Linux ppoll.

## Example

```mettle
import "tui/tui";

fn main() -> int32 {
  var t: Tui* = tui_init();
  if (t == 0) { return 1; }            // not a terminal
  tui_timer_every(t, 500);
  var ev: TuiEvent;
  var n: int32 = 0;
  var running: bool = true;
  while (running) {
    var b: TuiBuf* = tui_frame(t);
    var inner: TuiRect = tui_block(b, tui_size(t), TUI_BORDER_ROUNDED,
        tui_style_fg(TUI_CYAN), "hello", tui_style_default());
    tui_put(b, inner.x, inner.y, "ticks: {n}  (q quits)", tui_style_default());
    tui_show(t);
    if (!tui_poll(t, -1, &ev)) { continue; }
    if (ev.kind == TUI_EV_QUIT) { running = false; }
    if (ev.kind == TUI_EV_KEY && ev.ch == 113) { running = false; }
    if (ev.kind == TUI_EV_TIMER) { n = n + 1; }
  }
  tui_end(t);
  return 0;
}
```

## Use it

Add it to your project with mbuild, then `import "tui/tui";`. All names
start with `tui_`, `Tui` or `TUI_`.

## Build and test

```bash
mbuild test          # five test programs, debug
mbuild test -r       # the same, release
mbuild run demo      # a live dashboard
mbuild run keys      # shows every input event
mbuild bench         # frame time at 200x60
```

On this machine a 200x60 dashboard frame (a filled braille graph and 18
lines of text) takes about 0.3 ms to draw, diff and encode in release.

## Docs

- [docs/api.md](docs/api.md): every function and type.
- [docs/references.md](docs/references.md): the standards followed and
  the libraries whose ideas were used.

## License

Apache-2.0. See [LICENSE](LICENSE).
