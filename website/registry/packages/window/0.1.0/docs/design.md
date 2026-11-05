# Design

```
src/
  window.mettle            the public API; picks a backend, runs the event loop
  plat_linux.mettle        Linux: chooses X11, Wayland or headless and forwards calls
  plat_windows.mettle      Windows: chooses Win32 or headless
  core/
    keys.mettle            key codes (USB HID), keysym and evdev tables, dead keys
    types.mettle           public types, the event queue, per-window input state
  headless/headless.mettle no display; events come from window_inject
  x11/
    wire.mettle            the X11 connection: socket, auth, setup, requests, replies
    backend.mettle         windows, events, cursors, fullscreen, drawing
    select.mettle          clipboard (ICCCM) and drag and drop (XDND)
    ext.mettle             XKB, RandR, XInput 2, RENDER
  wayland/
    wire.mettle            the Wayland wire format, object table, fd passing
    xkb.mettle             XKB keymap text reader and keysym lookup
    keysyms.txt            keysym names (made by tools/gen_keysyms.py)
    backend.mettle         registry, xdg-shell windows, seat, outputs, data device, IME
  win32/
    api.mettle             the Windows API declarations and structures
    backend.mettle         window procedure, windows, input, IME, monitors, clipboard
  gamepad/
    gamepad.mettle         the pad model, SDL_GameControllerDB parser, public calls
    evdev.mettle           Linux evdev devices, hotplug, rumble
    xinput.mettle          Windows XInput pads
```

## How an event travels

1. A backend reads from its socket (or device) and turns each message into a
   `WindowEvent`.
2. It hands the event to `window_push_event`, which updates the window's
   state (keys held, mouse position, size, focus), drops duplicates (a key-up
   with no key-down, a resize to the same size), joins mouse moves that follow
   each other, counts clicks, and queues it.
3. `window_poll` / `window_wait` take events from the queue. When it is empty
   they ask the backend for more, waiting on the display socket and the wake
   eventfd with poll(2).

So `window_key_down` and the other state calls give the same answers on every
backend, and backends stay small.

## Rules the code keeps

- Every length and offset read from a server, compositor, device or file is
  checked before use. Bad input ends the connection or is skipped; it never
  ends the program.
- Requests are written straight to the socket; there is no output buffer to
  forget to flush.
- No libc on Linux: raw system calls only. On Windows, Win32 calls only.
- Every export starts with `window_`, `Window` or `WINDOW_` (Mettle exports
  are global across the whole program).

## Speed

`bench/bench.mettle` (`mbuild build -r`, then `build/linux/release/bench :97`
with an Xvfb on :97). On the development machine (x86-64, Linux 6.12):

| Path | Result |
|---|---|
| Event queue (push, state update, take) | 3.6 million events/s |
| X11 MotionNotify to WindowEvent | 3.7 million events/s |
| XKB keymap parse (German, 64 KB) | 0.23 ms |
| XKB key lookup | 95 million/s |
| X11 connect (Xvfb, with setup, atoms and extensions) | 0.9 ms |
| X11 window created and shown | 0.07 ms |

Input arrives at a few thousand events a second at most, so none of these
paths limits a program. GLFW and SDL were not installed on that machine, so
there are no side-by-side numbers yet.
