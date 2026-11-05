# OpenGL with Glint

`window` makes the window and handles input; Glint makes the GL context on it.
[examples/glint_triangle](../examples/glint_triangle) is a full program.

```mettle
var gcfg: GlintConfig = glint_config();
var cfg: WindowConfig;
window_config_default(&cfg);
// X11: the window must use a visual the GL driver can draw to.
var visual: int64 = 0;
var depth: int32 = 0;
if (glint_x11_visual(&gcfg, &visual, &depth)) { cfg.gl_visual = visual; cfg.gl_depth = depth; }
var win: Window* = window_create(&cfg);

var n: WindowNative;
window_native(win, &n);
var handle: int64 = n.x11_window;
if (n.backend == WINDOW_BACKEND_WIN32) { handle = n.hwnd; }
var ctx: GlintContext* = glint_open_existing(handle, &gcfg);
```

Then each frame: poll events, `glViewport(0, 0, fb_w, fb_h)` with the size
from `WINDOW_EV_RESIZE` (or `window_framebuffer_size`), draw, `glint_swap(ctx)`.
At the end, `glint_close(ctx)` before `window_destroy(win)`.

Glint opens its own connection to the X server named by `DISPLAY`. When you
use `window_set_display`, give Glint the same display.

## Wayland

Mesa's EGL takes a libwayland `wl_display*` and a `wl_egl_window`. This
library speaks the Wayland protocol itself and has no libwayland objects, so
GL on Wayland goes through XWayland for now: the AUTO backend picks X11 when
`DISPLAY` is set, which it is on every desktop Wayland compositor that runs
XWayland. Software drawing (`window_blit`) works on native Wayland.
