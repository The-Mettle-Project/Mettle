# References

What tui follows, and where its ideas come from. No code was copied; the
designs below were reimplemented in Mettle.

## Standards

- **ECMA-48** (5th edition, 1991), Control Functions for Coded Character
  Sets: CSI syntax (parameter, intermediate and final bytes), CUP, CUF,
  ED and SGR. The input parser follows its byte classes, so unknown
  sequences are skipped whole.
- **xterm Control Sequences** (Thomas Dickey, ctlseqs): SGR 38;2 and 38;5
  colours, bright colours 90-97, DEC private modes 1049 (alternate
  screen), 25 (cursor), 7 (auto-wrap), 1000/1002/1003 (mouse), 1006 (SGR
  mouse reports), 1004 (focus), 2004 (bracketed paste), modifier
  parameters ("CSI 1;5A"), and the "CSI n ~" function keys.
- **Synchronized output**, mode 2026 (the terminal-wg proposal used by
  kitty, WezTerm, foot, iTerm2 and others): frames are bracketed so the
  terminal shows them whole. Terminals that do not know it ignore it.
- **Kitty keyboard protocol**: "CSI code ; mods u" keys are understood.
- **Unicode UAX #11** (East Asian Width) and the General Category from
  the Unicode Character Database (15.1 via Python's unicodedata) for
  widths; tools/gen_width.py makes src/width_tables.mettle. UTF-8 decoding
  follows Unicode chapter 3 (no overlongs, no surrogates) and replaces bad
  bytes with U+FFFD one maximal subpart at a time.
- **Linux**: termios raw mode (cfmakeraw flags), TIOCGWINSZ and
  signalfd(2), through std/term; ppoll(2) through std/linux.

## Libraries studied

| Library | License | What tui took |
|---|---|---|
| ratatui (Rust) | MIT | Double buffering with a cell diff; the layout model of fixed, percentage and fill sizes; the set of widgets (block, list, table, gauge, sparkline, canvas). |
| crossterm (Rust) | MIT | The event model (key, mouse, paste, resize, focus) and opening /dev/tty so input works when stdin is a pipe. |
| tcell (Go) | Apache-2.0 | Moving the cursor the short way and sending only changed style attributes; resetting SGR only when an attribute must be turned off. |
| notcurses (C) | Apache-2.0 | Synchronized output around frames; braille as a 2 by 4 pixel grid; colour depth from COLORTERM. |
| termbox2 (C) | MIT | A small, flat API; the ESC-wait timeout for a lone Escape key. |
| FTXUI (C++) | MIT | Braille canvas drawing and the gauge with eighth-cell blocks. |
| btop (C++) | Apache-2.0 | The look of meters and graphs: gradient colours by height and filled braille graphs. |
| tmux | ISC | Picking the nearest 256-colour entry from the 6x6x6 cube or the grey ramp (colour_find_rgb). |
| glibc wcwidth / Markus Kuhn's wcwidth | LGPL / public domain | Width rules: zero for marks and format characters, two for W and F. Only the rules were used, rebuilt from the Unicode data. |

## Choices

- **A signal descriptor, not signal handlers.** SIGWINCH, SIGINT, SIGTERM
  and SIGHUP are blocked and read from std/term's signal descriptor in the
  same ppoll as input, so
  there are no async-signal-safety problems and resizes come in order with
  keys.
- **Ctrl-C as an event.** Raw mode turns off ISIG, so Ctrl-C is a byte;
  it becomes `TUI_EV_QUIT` so programs restore the terminal on their own
  path (turn it off with `tui_set_ctrl_c_quits`).
- **Auto-wrap off** while running, so writing the last cell never scrolls
  the screen; after the last column the cursor is treated as unknown.
- **Colour downgrading at write time.** Widgets always use the colours
  asked for; the renderer brings them down to 256 or 16 colours, so the
  same drawing code works everywhere.
- **Grapheme handling** is the common terminal subset: marks join, ZWJ
  joins, flags pair. Full UAX #29 segmentation is not done; terminals do
  not agree on it either.
- **std first.** Raw mode, the window size and the signal descriptor come
  from std/term (term_raw_begin/end, term_size, term_signal_fd/next/end);
  opening /dev/tty, read, write, ppoll and exit go through std/linux.
  tui has no system call numbers of its own.
- **Nothing to write, nothing written.** A frame with no changed cells
  and the same cursor makes no write() at all, so a once-a-second redraw
  of a still screen costs only the cell comparison.
- **Headless clock.** Tests never sleep: time moves to the next timer.

## Testing

Five test programs (about 330 checks) run in debug and release: widths
and UTF-8 decoding, exact output bytes of the renderer, exact screens of
every widget, the event loop and timers, and a 20,000-round fuzz of the
input parser with bytes biased toward ESC, '[', digits and ';'. The demo
was also run on a real pseudo-terminal to check raw mode, resize, Ctrl-C,
SIGTERM and that the terminal settings come back unchanged.
