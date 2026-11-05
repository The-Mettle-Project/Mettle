# Platforms

## X11

A client for the core X11 protocol over the local Unix socket (`:N` and
`unix:N` displays; no TCP), with MIT-MAGIC-COOKIE-1 authentication from
`$XAUTHORITY` or `~/.Xauthority`. No libX11 or xcb.

Extensions, each optional:

- XKEYBOARD: detectable auto-repeat, so a held key sends presses only.
  Without it, a release followed at once by a press with the same time is
  treated as repeat.
- RANDR 1.2+: monitors, modes, refresh rates, and change events. Without it
  the whole screen is one monitor.
- XInputExtension 2.0: raw mouse motion for `WINDOW_CURSOR_LOCKED`. Without
  it the pointer is warped back to the centre and the distance reported.
- RENDER: RGBA cursors.

Window manager hints follow ICCCM and EWMH. Without a window manager (a bare
Xvfb, a kiosk) fullscreen and maximize resize the window directly.

The keyboard layout comes from the core keyboard mapping (GetKeyboardMapping
with the XKB group in the event state), so layout switches work. The scale is
Xft.dpi / 96 for every monitor, as X11 has one value for the whole screen.

Not yet: input methods (XIM), sending large clipboard contents in pieces
(INCR send; copies over about 256 KB are cut; receiving INCR works), being a
drag source, exclusive video-mode changes.

## Wayland

A client for the Wayland wire protocol over `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`
(or an absolute `WAYLAND_DISPLAY`), passing file descriptors with SCM_RIGHTS.
No libwayland.

Needs wl_compositor, wl_shm and xdg_wm_base. Uses, when offered: wl_seat,
wl_output, wl_data_device_manager, xdg-decoration (server-side frames),
cursor-shape-v1, fractional-scale-v1 with viewporter, relative-pointer and
pointer-constraints, text-input-v3 and xdg-activation.

The keyboard keymap the compositor sends (XKB text) is read by our own parser,
and key repeat is done in the client with the rate the compositor gives.
Without cursor-shape-v1 a small built-in arrow is used.

Wayland has no global window positions, so `window_position` stays at 0,0 and
`window_set_position` and `window_warp_mouse` do nothing.

Not yet: OpenGL (see [glint.md](glint.md)), client-side frames when the
compositor offers no xdg-decoration, being a drag source.

## Gamepads (Linux)

Read from `/dev/input/event*` (the evdev interface), found by scanning the
directory and watched for hotplug with inotify. A device counts as a gamepad
when it has BTN_GAMEPAD, or a joystick button with X/Y axes. Pads you may not
read (your session decides, usually through udev's uaccess) are skipped.

The default layout is the Linux gamepad specification; SDL_GameControllerDB
mappings (matched by the SDL GUID: bus, vendor, product, version) override it.
Rumble uses force feedback (FF_RUMBLE) when the device allows writing.

## Headless

No display. `window_inject` puts events in the queue as a backend would, and
the library keeps window state from them. One 1920x1080 monitor is reported.
Gamepads still work on Linux.

## Windows

The Win32 API from user32, gdi32, imm32 and shell32, plus shcore and XInput loaded at
run time (so the program still starts where they are missing).

- The process asks for per-monitor DPI awareness v2 (falling back to the
  older calls). Windows report logical sizes like the other backends, follow
  WM_DPICHANGED when dragged between monitors, and size limits are logical.
- Keys: the physical key comes from the scancode (set 1, with the E0 flag),
  the layout key from the virtual-key code, and for punctuation from what the
  layout types (MapVirtualKey). The fake Left Ctrl that comes with AltGr is
  dropped, and Print Screen gets the key-down Windows leaves out.
- Text: WM_CHAR with surrogate pairs joined; dead keys are composed by
  Windows. The IME goes through IMM32: pre-edit text with its cursor, the
  result as text, and the candidate window placed by `window_text_input_rect`.
- `WINDOW_CURSOR_LOCKED` clips the cursor to the window, hides it and reads
  raw mouse motion (WM_INPUT).
- Monitors: EnumDisplayMonitors, with refresh rates from EnumDisplaySettings
  and scale from GetDpiForMonitor.
- The window class has CS_OWNDC, so Glint can make a WGL context on it.
- Gamepads: XInput 1.4 (or 9.1.0), four pads, with rumble.
- Drops: files from Explorer (DragAcceptFiles / WM_DROPFILES) as `WINDOW_EV_DROP_FILES`.

Not yet: dropped text and the enter / move / leave events while dragging
(they need an OLE drop target), transparent windows,
exclusive video-mode changes.

Testing: `mbuild wintest` runs the tests as Windows programs under wine on a
private Xvfb. The debug build passes there; the release build under wine and
a real Windows machine have not been checked yet, and wine is not Windows, so
treat the Win32 backend as needing a run on real Windows before relying on it.
