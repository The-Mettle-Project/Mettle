# Guide

## Starting

`window_create` starts the library the first time you call it. To choose the
backend yourself, call `window_init_backend` first:

| Backend | When |
|---|---|
| `WINDOW_BACKEND_AUTO` | Win32 on Windows. On Linux, X11 when `DISPLAY` is set, else Wayland. |
| `WINDOW_BACKEND_WIN32` | Always Win32 (Windows). |
| `WINDOW_BACKEND_X11` | Always X11. |
| `WINDOW_BACKEND_WAYLAND` | Always Wayland. |
| `WINDOW_BACKEND_HEADLESS` | No display: windows live in memory, events come from `window_inject`. Good for tests and servers that only want gamepads. |

The environment variable `WINDOW_BACKEND` (`x11`, `wayland` or `headless`)
overrides AUTO. `window_set_display(":1")` or `window_set_display("wayland-1")`
names the display instead of `DISPLAY` / `WAYLAND_DISPLAY`.

AUTO picks X11 on a Wayland desktop that also runs XWayland because GL through
Glint needs X11 today (see [glint.md](glint.md)).

Anything that can fail returns 0 or false, and `window_error()` says why.
Nothing in the library ends your program.

## Windows

Fill a `WindowConfig` with `window_config_default` and change what you need:
`title`, `w`, `h`, `x`, `y` (or `WINDOW_POS_CENTER`), `resizable`, `decorated`,
`visible`, `floating`, `transparent`, `maximized`, `fullscreen`, `monitor`,
`min_w` / `min_h` / `max_w` / `max_h`, `app_id`, and on X11 `gl_visual` /
`gl_depth` for GL.

Sizes are logical: on a screen with scale 2, an 800x600 window covers 1600x1200
pixels. `window_size` gives the logical size, `window_framebuffer_size` the
pixel size (use it for `glViewport`), `window_scale` the ratio. Mouse
positions are logical too. Set `hidpi = false` to ask for pixels on X11.

## The event loop

Games poll every frame:

```mettle
while (!window_should_close(win)) {
  while (window_poll(&ev)) { handle(&ev); }
  draw();
}
```

Apps sleep until something happens:

```mettle
while (!window_should_close(win)) {
  if (window_wait(&ev, -1)) { handle(&ev); }
}
```

`window_wait(&ev, ms)` waits at most `ms` milliseconds (use it for timers).
`window_wake()` wakes a waiting loop from another thread; the loop gets
`WINDOW_EV_WAKE`. `window_request_redraw(win)` asks for one
`WINDOW_EV_REDRAW` on the next turn, however often you call it. Redraw events
also come when the window is first shown and when it is uncovered.

Every event has `kind`, `win` (0 for gamepad, monitor and wake events) and
`time_us`. The other fields depend on the kind; see [api.md](api.md).

## Keyboard

Key events carry two codes, both `WINDOW_KEY_*` (USB HID numbering):

- `scancode`: where the key sits. Use it for game controls, so WASD stays in
  place on an AZERTY keyboard.
- `key`: what the layout says the key is. Use it for shortcuts like Ctrl+Z.

`repeat` is true for auto-repeat. `mods` has `WINDOW_MOD_*` bits.
`window_key_down(win, scancode)` asks whether a key is held right now.
When a window loses focus, every held key gets a key-up event, so nothing
stays stuck.

Typed text comes as `WINDOW_EV_TEXT` with UTF-8 in `text` / `text_len`, with
dead keys combined (´ then e gives é). Ctrl and Alt shortcuts make no text.
With an input method (IME), `WINDOW_EV_PREEDIT` carries the text being
composed (`cursor` is a byte offset into it); an empty pre-edit means it is
done. Tell the IME where your text cursor is with `window_text_input_rect`,
and turn text input off where you do not want it (`window_text_input`).

## Mouse

`WINDOW_EV_MOUSE_MOVE` (x, y and the change dx, dy), `MOUSE_DOWN` / `MOUSE_UP`
(`button`, and `clicks` 1, 2 or 3), `SCROLL` (dx, dy in wheel steps; dy > 0 is
away from you), `MOUSE_ENTER` / `MOUSE_LEAVE`.

Cursor modes (`window_set_cursor_mode`):

- `WINDOW_CURSOR_NORMAL`
- `WINDOW_CURSOR_HIDDEN`: hidden while over the window.
- `WINDOW_CURSOR_LOCKED`: hidden, held in the window, and `WINDOW_EV_RAW_MOTION`
  reports unaccelerated motion. For mouse-look.
- `WINDOW_CURSOR_CONFINED`: visible but cannot leave the window.

Shapes: `window_set_cursor(win, WINDOW_CURSOR_TEXT)` and the rest, or your own
RGBA image with `window_set_cursor_image`.

## Gamepads

Pads are numbered 0 to 15 and keep their number while connected.
`WINDOW_EV_GAMEPAD_ADDED` / `REMOVED` tell you when they come and go (pads
already plugged in at start arrive as ADDED too). Buttons (`WINDOW_PAD_*`)
are named by position: `SOUTH` is A on an Xbox pad and Cross on a PlayStation
pad. Sticks go from -1 to 1 with down and right positive; triggers go from 0
to 1. Read the state any time with `window_gamepad_button` and
`window_gamepad_axis`, or follow the events.

Pads that follow the Linux gamepad layout (most do) work as they are. For
others, load mappings in SDL_GameControllerDB format:

```mettle
window_gamepad_add_mappings(import_str "gamecontrollerdb.txt");
```

`window_gamepad_rumble(pad, low, high, ms)` shakes the pad.

## Clipboard, drag and drop

`window_clipboard_set(win, "text")` and `window_clipboard_get(win)` (free the
result with `free(s.chars)`). What you copy stays available until you copy
something else or the program ends. `WINDOW_EV_CLIPBOARD` says another program
took the clipboard.

Drops arrive as `WINDOW_EV_DROP_ENTER` / `DROP_MOVE` / `DROP_LEAVE` while
something is dragged over the window, then `WINDOW_EV_DROP_FILES` (`data`
holds local paths, one per line) or `WINDOW_EV_DROP_TEXT`. `data` stays valid
until the next `window_poll` or `window_wait`.

## Monitors and fullscreen

`window_monitor_count` and `window_monitor(i, &m)` describe each monitor:
position, size, work area (without panels), physical size in millimetres,
refresh rate in millihertz, scale, name and which one is primary.
`WINDOW_EV_MONITORS` says the list changed.

`window_set_fullscreen(win, WINDOW_FULLSCREEN, i)` covers monitor `i`
(-1 for the one the window is on) with a borderless window, the way modern
games do; `WINDOW_WINDOWED` goes back.

## Drawing without a GPU

`window_blit(win, rgba, w, h, stride)` shows RGBA pixels (top row first). Draw
at the framebuffer size for sharp results on HiDPI screens.

## Graphics APIs

`window_native(win, &n)` fills a `WindowNative`: on X11 the window id, visual
and display name; on Wayland the surface's object id; on Windows the HWND.
See [glint.md](glint.md) for OpenGL.
