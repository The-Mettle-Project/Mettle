# window

Windows and input for [Mettle](https://github.com/Lex-Dellimere), in pure Mettle.

`window` opens and manages windows and reads the keyboard, mouse and gamepads.
It has one event loop that suits both games (poll every frame) and desktop
apps (sleep until something happens), and it gives out the native handles a
graphics library needs: [Glint](https://github.com/Lex-Dellimere/Glint) makes
OpenGL contexts on its windows.

There is no C and no system library underneath on Linux: the X11 and Wayland
clients speak the protocols directly over Unix sockets, and gamepads are read
from the kernel's evdev interface. On Windows it calls the Win32 API directly.

```mettle
import "window/window";

fn main() -> int32 {
  var cfg: WindowConfig;
  window_config_default(&cfg);
  cfg.title = "Hello";
  cfg.w = 800; cfg.h = 600;
  var win: Window* = window_create(&cfg);
  if (win == 0) { return 1; }
  var ev: WindowEvent;
  while (!window_should_close(win)) {
    while (window_poll(&ev)) {
      if (ev.kind == WINDOW_EV_KEY_DOWN && ev.key == WINDOW_KEY_ESCAPE) { window_set_should_close(win, true); }
    }
    // draw here (Glint, or window_blit for software pixels)
  }
  window_destroy(win);
  window_shutdown();
  return 0;
}
```

## What it does

| | X11 | Wayland | Win32 | Headless |
|---|---|---|---|---|
| Windows: title, size, limits, show / hide, focus, attention | yes | yes (no positions: Wayland has none) | yes | yes |
| Minimize, maximize, restore, borderless fullscreen per monitor | yes (EWMH, or by hand without a window manager) | yes | yes | state only |
| Monitors: position, size, work area, refresh, scale | yes (RandR) | yes (wl_output) | yes | one fake monitor |
| HiDPI: logical vs pixel size, scale changes | Xft.dpi | integer and fractional scale | per-monitor v2 | - |
| Keyboard: physical and layout keys, modifiers, repeat flag | yes (XKB detectable repeat) | yes (own XKB keymap reader, client-side repeat) | yes | injected |
| Text: UTF-8, dead keys | yes | yes | yes | injected |
| Input method (IME) with pre-edit text | not yet | yes (text-input-v3) | yes (IMM32) | - |
| Mouse: buttons, double clicks, wheel, enter / leave | yes | yes | yes | injected |
| Cursors: standard shapes, RGBA image, hidden | yes (cursor font, RENDER) | yes (cursor-shape-v1, shm) | yes | - |
| Pointer lock with raw motion, confine | yes (XInput 2) | yes (pointer-constraints, relative-pointer) | yes (raw input) | - |
| Clipboard (UTF-8 text) | yes (ICCCM, INCR receive) | yes | yes | in memory |
| Drag and drop in (files, text) | yes (XDND 5) | yes | files (shell32) | - |
| Software drawing (`window_blit`) | yes (PutImage) | yes (wl_shm) | yes (GDI) | accepted |
| OpenGL through Glint | yes | through XWayland (see [docs/glint.md](docs/glint.md)) | yes (WGL) | - |
| Gamepads with hotplug and rumble | evdev | evdev | XInput | evdev on Linux |

## Build and test

```bash
mbuild build
```

```bash
mbuild test
```

```bash
mbuild test -r
```

The tests start their own Xvfb (from display :97 up) and their own small
Wayland compositor, so they never touch your screen. They need `Xvfb`; the
clipboard test also uses `xclip` when it is there. `mbuild wintest` runs the
same tests as Windows programs under wine (set `MBUILD_METTLE_WINDOWS` to a
Windows build of the Mettle compiler).

Examples: `build/linux/debug/hello` (a window drawn in software that prints
every event), `build/linux/debug/gamepad` (prints gamepad input) and
[examples/glint_triangle](examples/glint_triangle) (OpenGL through Glint).

## Docs

- [docs/guide.md](docs/guide.md): how to use it.
- [docs/api.md](docs/api.md): every function, type and constant.
- [docs/platforms.md](docs/platforms.md): what each backend does and does not do.
- [docs/glint.md](docs/glint.md): making a GL context with Glint.
- [docs/design.md](docs/design.md): how the code is laid out.
- [docs/references.md](docs/references.md): the standards followed and where the ideas came from.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
