# tui API

`import "tui/tui";` gives everything below. Linux only.

Coordinates are cells from (0, 0) at the top left. Sizes are `int32`.
Colours are `uint32`. Text is UTF-8 `string`.

## Session

| Function | What it does |
|---|---|
| `tui_init() -> Tui*` | Full-screen session with mouse and paste on. 0 when there is no terminal. |
| `tui_init_with(opts: int32) -> Tui*` | The same with `TUI_OPT_` flags: `MOUSE`, `MOUSE_MOVE` (every move), `PASTE`, `FOCUS`, `NO_ALT` (stay on the main screen). `TUI_OPT_DEFAULT` is mouse and paste. |
| `tui_end(t)` | Puts the terminal back (modes, main screen, cursor, cooked mode, signal mask) and frees the session. Safe to call twice. |
| `tui_exit(t, code)` | `tui_end`, then end the process. |
| `tui_size(t) -> TuiRect` | The whole screen. |
| `tui_set_depth(t, depth)` | Force `TUI_DEPTH_16`, `TUI_DEPTH_256` or `TUI_DEPTH_TRUE`. By default it comes from `COLORTERM` and `TERM`. |
| `tui_set_ctrl_c_quits(t, on)` | On by default: Ctrl-C arrives as `TUI_EV_QUIT`. Off: as a key. |
| `tui_cursor(t, show, x, y)` | Show the text cursor at (x, y) after each frame, or hide it. |
| `tui_redraw(t)` | Draw every cell again on the next frame. |
| `tui_now(t) -> int64` | Milliseconds on the session clock (monotonic). |

The terminal is always restored by `tui_end`. SIGINT, SIGTERM and SIGHUP
are blocked and arrive as `TUI_EV_QUIT`, so the program can clean up and
call `tui_end`. A crash or SIGKILL cannot be caught; run `reset` then.

## Frames

| Function | What it does |
|---|---|
| `tui_frame(t) -> TuiBuf*` | The buffer for the next frame, cleared. Draw everything into it. |
| `tui_show(t)` | Write the frame in one write: only cells that changed since the last frame, inside synchronized-output brackets (mode 2026). A frame with no changes writes nothing. |
| `t->last_cells`, `t->last_bytes` | Cells and bytes the last `tui_show` wrote. |

## Events

`tui_poll(t, timeout_ms, &ev) -> bool` waits up to `timeout_ms` (-1: no
limit) and returns false if the time ran out first.

`TuiEvent` fields: `kind`, `key`, `ch`, `mods`, `x`, `y`, `button`,
`action`, `w`, `h`, `timer`, `focus`, `text`.

| `kind` | Fields |
|---|---|
| `TUI_EV_KEY` | `key`, `mods`; `ch` (code point) when `key` is `TUI_KEY_CHAR` |
| `TUI_EV_MOUSE` | `x`, `y`, `button`, `action`, `mods` |
| `TUI_EV_PASTE` | `text` (valid until the next poll) |
| `TUI_EV_RESIZE` | `w`, `h`; buffers are already resized |
| `TUI_EV_TIMER` | `timer` (the id) |
| `TUI_EV_FOCUS` | `focus` (true: gained) |
| `TUI_EV_QUIT` | Ctrl-C, SIGINT, SIGTERM, SIGHUP, or the terminal closed |

Keys: `TUI_KEY_CHAR ENTER TAB BACKSPACE ESC UP DOWN LEFT RIGHT HOME END
PGUP PGDN INSERT DELETE`, and `TUI_KEY_F1 + n` for F1 to F12. Ctrl+letter
is `TUI_KEY_CHAR` with the lower-case letter and `TUI_MOD_CTRL`. Shift+Tab
is `TUI_KEY_TAB` with `TUI_MOD_SHIFT`.

Modifiers: `TUI_MOD_SHIFT ALT CTRL META` (bits).

Mouse buttons: `TUI_BTN_LEFT MIDDLE RIGHT NONE WHEEL_UP WHEEL_DOWN
WHEEL_LEFT WHEEL_RIGHT`. Actions: `TUI_MOUSE_PRESS RELEASE DRAG MOVE`.

Timers:

| Function | What it does |
|---|---|
| `tui_timer_every(t, ms) -> int32` | Repeating timer; returns its id (0 if all 32 are used). A late timer fires once, not once per missed period. |
| `tui_timer_once(t, ms) -> int32` | One-shot timer. |
| `tui_timer_stop(t, id)` | Stop a timer. |

## Headless sessions (tests)

| Function | What it does |
|---|---|
| `tui_headless(w, h) -> Tui*` | A session with no terminal, true colour. |
| `tui_feed(t, s)` | Bytes as if typed. `tui_input_feed(t->input, p, n)` for raw bytes. |
| `tui_resize(t, w, h)` | As if the window changed size; the next poll reports it. |
| `tui_output(t) -> string` | The exact bytes the last `tui_show` wrote. |
| `tui_screen(t) -> string` | The screen as text, each row ending in `\n`. Caller frees `.chars`. |
| `tui_screen_buf(t) -> TuiBuf*` | The screen as cells, to check styles. |

Time in a headless session starts at 0 and moves only inside `tui_poll`:
it jumps to the next timer, or by the timeout.

## Colours and styles

| Function / constant | What it is |
|---|---|
| `TUI_DEFAULT` | The terminal's own colour |
| `TUI_BLACK` ... `TUI_WHITE`, `TUI_BRIGHT_BLACK` ... `TUI_BRIGHT_WHITE` | The 16 basic colours |
| `tui_rgb(r, g, b)`, `tui_hex(0xRRGGBB)` | True colour |
| `tui_index(i)` | xterm palette colour 0-255 |
| `tui_mix(a, b, t)` | Blend; `t` from 0 (a) to 1000 (b) |
| `tui_gradient(stops: uint32[], t)` | A colour along several stops, `t` from 0 to 1000 |
| `tui_no_stops() -> uint32[]` | An empty list of stops |
| `tui_rgb_to_256(c)`, `tui_rgb_to_16(c)`, `tui_color_for(c, depth)` | Fallbacks (done for you when drawing) |
| `TuiStyle { fg, bg, attrs }` | `attrs` bits: `TUI_BOLD DIM ITALIC UNDERLINE BLINK REVERSE STRIKE` |
| `tui_style(fg, bg, attrs)`, `tui_style_fg(fg)`, `tui_style_default()` | Making styles |

## Drawing text

| Function | What it does |
|---|---|
| `tui_put(b, x, y, s, st) -> int32` | Write text; returns columns used. |
| `tui_put_clip(b, x, y, s, max_w, st) -> int32` | The same, at most `max_w` columns. |
| `tui_text(b, r, s, st, align, wrap) -> int32` | Text in a rectangle; `\n` breaks lines; wrap at spaces. Returns the total lines. |
| `tui_text_scroll(b, r, s, st, align, wrap, skip)` | The same, starting `skip` lines in. |
| `tui_text_line(b, x, y, w, s, st, align)` | One aligned line. `TUI_LEFT CENTER RIGHT`. |
| `tui_span(text, st)`, `tui_spans(b, x, y, w, spans: TuiSpan[])` | A line with mixed styles. |
| `tui_str_width(s)`, `tui_cp_width(cp)` | Width in columns. |
| `tui_fill(b, r, cp, st)` | Fill a rectangle with one character. |

Text rules: combining marks and variation selectors join the character
before; emoji joined by U+200D share one cell; two regional indicators make
one two-column flag; control characters are skipped; a wide character that
would cross the edge is not drawn. Writing over half of a wide character
blanks the other half.

## Buffers

`TuiBuf { w, h, cells }`, `TuiCell { text, len, width, style }`.
`tui_buf_new(w, h)`, `tui_buf_free`, `tui_buf_clear`, `tui_buf_resize`,
`tui_buf_area(b) -> TuiRect`, `tui_buf_cell(b, x, y) -> TuiCell*` (0 off the
buffer), `tui_buf_set_cp(b, x, y, cp: int32, st)` and `tui_put_cp(b, x, y,
cp: uint32, st)` (one code point, e.g. your own braille), `tui_buf_set_style`,
`tui_buf_blit(dst, src, x, y)`, `tui_buf_text(b)`, `tui_buf_row(b, y)`.

## Layout

| Function | What it does |
|---|---|
| `tui_rect(x, y, w, h)` | A rectangle (negative sizes become 0). |
| `tui_fixed(n)`, `tui_percent(p)`, `tui_fill_len(weight)` | Sizes for `tui_split`. |
| `tui_split(r, dir, lens: TuiLen[], gap, out: TuiRect*) -> int32` | Split into `TUI_ROWS` or `TUI_COLS`. Fixed and percent sizes are given first, in order, while room lasts; fills share the rest by weight. Up to 64 parts. |
| `tui_inset(r, n)`, `tui_inset_xy(r, dx, dy)`, `tui_center(r, w, h)`, `tui_contains_point(r, x, y)` | Rectangle helpers. |

```mettle
var parts: TuiRect[2];
var lens: TuiLen[2] = [tui_fixed(3), tui_fill_len(1)];
tui_split(tui_size(t), TUI_ROWS, lens, 0, &parts[0]);
```

## Widgets

| Function | What it does |
|---|---|
| `tui_border(b, r, kind, st) -> TuiRect` | A border; returns the inside. `TUI_BORDER_NONE PLAIN ROUNDED DOUBLE THICK`. |
| `tui_block(b, r, kind, st, title, title_st) -> TuiRect` | A border with a title on the top left. |
| `tui_border_label(b, r, text, align, bottom, st)` | More labels on the top or bottom edge. |
| `tui_list(b, r, items: string[], s: TuiScroll*, st, sel_st)` | A scroll list; keeps the selected row in view. |
| `tui_table(b, r, headers, cells, widths: TuiLen[], aligns: int32[], s, head_st, st, sel_st)` | A header row, then rows; `cells` is row-major with `headers.length` columns. |
| `tui_scrollbar(b, r, total, offset, visible, track_st, thumb_st)` | A one-column scrollbar. |
| `TuiScroll { selected, offset }`, `tui_scroll_move(s, delta, count)`, `tui_scroll_fit(s, count, visible)` | Scroll state for lists and tables. |

## Charts

| Function | What it does |
|---|---|
| `tui_gauge(b, r, ratio, label, fill, empty, label_fg)` | A bar filled to `ratio` (0 to 1) in eighths of a cell, with a centred label. |
| `tui_meter(b, r, ratio, stops, empty_fg, mark)` | A btop-style row of marks coloured along a gradient; `mark` 0 is ■. |
| `tui_sparkline(b, r, values: float64[], max, fg, stops)` | Bars, newest at the right; `max <= 0` means the largest value; gradient by height when `stops` is not empty. |
| `tui_graph(b, r, series: TuiSeries[], lo, hi, mode)` | Graphs of one or more series. `TUI_GRAPH_BRAILLE` (lines of dots, 2 values per cell), `TUI_GRAPH_BRAILLE \| TUI_GRAPH_FILL` (filled), `TUI_GRAPH_BLOCK` (filled bars, 1 value per cell). |
| `tui_series(values, color) -> TuiSeries` | A series; set `.stops` for a gradient by height. |
| `tui_canvas_new(cw, ch)`, `tui_canvas_dot`, `tui_canvas_line`, `tui_canvas_draw(b, c, x, y, bg)`, `tui_canvas_clear`, `tui_canvas_free` | A braille dot canvas, 2 by 4 dots per cell. |

NaN and out-of-range values are clamped, never a crash.

## Lower level

`TuiInput` with `tui_input_new`, `tui_input_feed`, `tui_input_next(in,
&ev, force)` parses bytes without a terminal (`force` takes a lone ESC or
cut-off sequence as it is). `TuiOut` and `tui_render_diff(o, pen, front,
back)` produce the bytes for a frame.
