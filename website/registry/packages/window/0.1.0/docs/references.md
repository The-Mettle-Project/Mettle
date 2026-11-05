# Standards and references

## Standards followed

| Area | Specification |
|---|---|
| X11 wire protocol, events, keyboard mapping | X Window System Protocol, Version 11 (X.Org, X11R7) |
| Authentication file | Xau file format (libXau `AuthRead`), MIT-MAGIC-COOKIE-1 |
| Window manager hints, selections | ICCCM 2.0 (Inter-Client Communication Conventions Manual) |
| Window state, fullscreen, ping, icons | freedesktop.org Extended Window Manager Hints 1.5 |
| Drag and drop on X11 | XDND protocol, version 5 (freedesktop.org) |
| Auto-repeat, keyboard groups | The X Keyboard Extension: Protocol Specification |
| Monitors on X11 | The X Resize, Rotate and Reflect Extension (RandR) 1.5 |
| Raw mouse motion on X11 | The X Input Extension, Version 2 |
| RGBA cursors on X11 | The X Rendering Extension 0.11 |
| Wayland wire format | The Wayland Protocol, "Wire Format" chapter; wayland.xml |
| Wayland windows and extras | xdg-shell, xdg-decoration-unstable-v1, cursor-shape-v1, fractional-scale-v1, viewporter, relative-pointer-unstable-v1, pointer-constraints-unstable-v1, text-input-unstable-v3, xdg-activation-v1 (wayland-protocols) |
| Keymaps on Wayland | XKB keymap text format v1, as written by libxkbcommon |
| Key codes | USB HID Usage Tables, Keyboard/Keypad page (0x07) |
| Keysyms | X.Org keysymdef.h and XF86keysym.h |
| Linux input | Linux kernel Documentation/input: input.rst, event-codes.rst, gamepad.rst, ff.rst |
| Gamepad mappings | SDL_GameControllerDB mapping format |
| File lists in drops | RFC 2483 (text/uri-list), RFC 8089 (file URIs) |
| Windows | Win32 API documentation (learn.microsoft.com): window procedures, keyboard and mouse input, raw input, High DPI, Input Method Manager, clipboard, XInput |
| Scancodes on Windows | Keyboard Scan Code Specification (Microsoft), set 1 |

## References studied

No code was copied from these projects. Their licenses are GLFW: zlib;
SDL: zlib; winit: Apache-2.0 / MIT; libxkbcommon: MIT; MythicUI: the author's
own, Apache-2.0.

| Idea | From |
|---|---|
| Small flat API, window hints in one config struct, a library that starts itself | GLFW |
| Physical scancode and layout key as two codes, USB HID numbering | SDL3 (SDL_Scancode / SDL_Keycode) |
| One event queue for every window, timestamps on events, gamepad model with buttons named by position | SDL3 |
| Wait vs poll loops, a wake call for other threads, redraw requests that coalesce | winit (ControlFlow::Wait, EventLoopProxy, request_redraw) |
| Logical vs physical sizes, scale-factor events | winit |
| IME pre-edit events with a cursor offset | winit (Ime::Preedit), SDL3 (SDL_EVENT_TEXT_EDITING) |
| XKB detectable auto-repeat on X11, hidden helper window owning the clipboard | GLFW (x11_init.c, x11_window.c) |
| Selection ownership with CurrentTime | GLFW, SDL |
| Raw motion through XInput 2 for disabled cursors, warp-to-centre fallback | GLFW |
| Release-press pairs with equal time as key repeat (no XKB) | GLFW, MythicUI |
| No-libX11 client: connection, authentication, setup parsing, keysym table | MythicUI (`mythic_plat_x11.mettle`) |
| SDL GUID layout and the order of raw buttons and axes on Linux | SDL3 (SDL_sysjoystick.c) |
| Z/RZ as triggers when RX exists, else as the right stick | SDL3, the Linux gamepad specification |
| Automatic key types for keys without a type, Caps Lock capitalization | libxkbcommon |
| Client-side key repeat on Wayland with the compositor's rate | GLFW, SDL3, winit |
| Dropping the fake Left Ctrl that comes with AltGr; a key-down for Print Screen | GLFW (win32_window.c) |
| Per-monitor DPI v2 with WM_DPICHANGED's suggested rectangle | GLFW, SDL3, winit |
| Win32 window procedure, UTF-16 conversion, DIB drawing | MythicUI (`mythic_plat_win32.mettle`) |
| XInput dead zones (7849 / 8689) and trigger threshold (30) | XInput documentation, SDL3 |

## Tests that check against real implementations

- X11: every test runs against a real X server (Xvfb) and input goes through
  the XTEST extension. The clipboard is exchanged with `xclip`, a separate
  X client using libX11.
- Keymaps: the parser reads keymaps compiled by `xkbcomp` from
  xkeyboard-config (US and German), and is checked key by key.
- Gamepads: a real Xbox controller is detected on the development machine;
  the test pad goes through the same evdev code with made-up input.
- Wayland: a scripted compositor in the test (no compositor was available on
  the build machine). The protocol details were taken from the XML.
