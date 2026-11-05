# API

`import "window/window";` brings in everything below. Every name starts with
`window_`, `Window` or `WINDOW_`.

## Start and stop

| Function | Does |
|---|---|
| `window_init() -> bool` | Starts with `WINDOW_BACKEND_AUTO` (`window_create` calls it). |
| `window_init_backend(backend: int32) -> bool` | Starts with a chosen backend. |
| `window_set_display(name: string)` | The display to use (":1", "wayland-1" or a socket path), before starting. |
| `window_backend() -> int32` | The running backend. |
| `window_shutdown()` | Closes every window and the display connection. |
| `window_error() -> string` | Why the last failing call failed. |
| `window_now_us() -> int64` | Monotonic clock, microseconds. |
| `window_sleep_ms(ms: int32)` | Sleeps. |

## Windows

| Function | Does |
|---|---|
| `window_config_default(cfg: WindowConfig*)` | Defaults: 800x600, centred, resizable, decorated, visible, focused, hidpi. |
| `window_create(cfg: WindowConfig*) -> Window*` | Opens a window (0 on failure). `cfg` may be 0. |
| `window_destroy(w: Window*)` | Closes it. Its queued events are dropped. |
| `window_should_close(w) -> bool`, `window_set_should_close(w, v)` | Set when the user asks to close. |
| `window_set_title(w, title: string)` | UTF-8 title. |
| `window_size(w, &w, &h)`, `window_framebuffer_size(w, &w, &h)` | Logical and pixel size. |
| `window_set_size(w, width, height)` | Logical size. |
| `window_position(w, &x, &y)`, `window_set_position(w, x, y)` | Desktop position (X11, Win32). |
| `window_set_size_limits(w, min_w, min_h, max_w, max_h)` | 0 = no limit. |
| `window_scale(w) -> float32` | Content scale (1.0 at 96 DPI). |
| `window_show(w)`, `window_hide(w)` | |
| `window_minimize(w)`, `window_maximize(w)`, `window_restore(w)` | |
| `window_focus(w)` | Raise and focus (the system may refuse). |
| `window_request_attention(w)` | Flash / urgency hint. |
| `window_set_fullscreen(w, mode, monitor)` | `WINDOW_FULLSCREEN` or `WINDOW_WINDOWED`; monitor -1 = current. |
| `window_focused`, `window_visible`, `window_minimized`, `window_maximized`, `window_hovered`, `window_is_fullscreen` | State, `-> bool`. |
| `window_set_icon(w, rgba: uint8*, width, height)` | Up to 1024x1024. |
| `window_blit(w, rgba: uint8*, width, height, stride) -> bool` | Show RGBA pixels. |
| `window_native(w, out: WindowNative*) -> bool` | Native handles. |
| `window_set_user(w, p: rawptr)`, `window_user(w) -> rawptr` | Your own pointer. |

## Events

| Function | Does |
|---|---|
| `window_poll(ev: WindowEvent*) -> bool` | Next event without waiting. |
| `window_wait(ev, timeout_ms: int32) -> bool` | Waits up to timeout_ms (-1 = forever). False on timeout. |
| `window_wake()` | Wakes a waiting loop (any thread); gives `WINDOW_EV_WAKE`. |
| `window_request_redraw(w)` | One `WINDOW_EV_REDRAW` on the next turn. |
| `window_inject(ev: WindowEvent*)` | Queue an event yourself (tests, replays). |

`WindowEvent` fields: `kind`, `win`, `time_us`, `key`, `scancode`, `mods`,
`repeat`, `button`, `clicks`, `x`, `y`, `dx`, `dy`, `w`, `h`, `fb_w`, `fb_h`,
`scale`, `text[64]`, `text_len`, `cursor`, `pad`, `axis`, `value`, `data`,
`data_len`.

| Kind | Fields |
|---|---|
| `WINDOW_EV_CLOSE` | the user asked to close |
| `WINDOW_EV_RESIZE` | `w`, `h` (logical), `fb_w`, `fb_h` (pixels) |
| `WINDOW_EV_MOVE` | `x`, `y` |
| `WINDOW_EV_FOCUS_IN`, `WINDOW_EV_FOCUS_OUT` | |
| `WINDOW_EV_REDRAW` | |
| `WINDOW_EV_SCALE` | `scale` |
| `WINDOW_EV_MINIMIZE`, `WINDOW_EV_MAXIMIZE`, `WINDOW_EV_RESTORE`, `WINDOW_EV_SHOWN`, `WINDOW_EV_HIDDEN` | |
| `WINDOW_EV_KEY_DOWN`, `WINDOW_EV_KEY_UP` | `key`, `scancode`, `mods`, `repeat` |
| `WINDOW_EV_TEXT` | `text`, `text_len` (UTF-8) |
| `WINDOW_EV_PREEDIT` | `text`, `text_len`, `cursor` |
| `WINDOW_EV_MOUSE_MOVE` | `x`, `y`, `dx`, `dy` |
| `WINDOW_EV_MOUSE_DOWN`, `WINDOW_EV_MOUSE_UP` | `button`, `clicks`, `x`, `y` |
| `WINDOW_EV_SCROLL` | `dx`, `dy` (wheel steps) |
| `WINDOW_EV_MOUSE_ENTER`, `WINDOW_EV_MOUSE_LEAVE` | |
| `WINDOW_EV_RAW_MOTION` | `dx`, `dy` (locked cursor) |
| `WINDOW_EV_DROP_ENTER`, `WINDOW_EV_DROP_MOVE`, `WINDOW_EV_DROP_LEAVE` | `x`, `y` |
| `WINDOW_EV_DROP_FILES`, `WINDOW_EV_DROP_TEXT` | `data`, `data_len` |
| `WINDOW_EV_CLIPBOARD` | another program took the clipboard |
| `WINDOW_EV_GAMEPAD_ADDED`, `WINDOW_EV_GAMEPAD_REMOVED` | `pad` |
| `WINDOW_EV_GAMEPAD_DOWN`, `WINDOW_EV_GAMEPAD_UP` | `pad`, `button` |
| `WINDOW_EV_GAMEPAD_AXIS` | `pad`, `axis`, `value` |
| `WINDOW_EV_MONITORS` | the monitor list changed |
| `WINDOW_EV_WAKE` | `window_wake` was called |
| `WINDOW_EV_QUIT` | the display connection was lost (every window also gets CLOSE) |

## Keyboard

| Function | Does |
|---|---|
| `window_key_down(w, scancode) -> bool` | Physical key held. |
| `window_key_down_layout(w, key) -> bool` | Layout key held. |
| `window_mods(w) -> int32` | `WINDOW_MOD_SHIFT`, `_CTRL`, `_ALT`, `_SUPER`, `_CAPS_LOCK`, `_NUM_LOCK`, `_ALTGR`. |
| `window_text_input(w, on: bool)` | Text and IME on or off (on by default). |
| `window_text_input_rect(w, x, y, width, height)` | Where the text cursor is, for the IME. |
| `window_key_name(key) -> string` | "A", "Enter", "Left Shift". |

Key codes: `WINDOW_KEY_A` .. `WINDOW_KEY_Z`, `WINDOW_KEY_0` .. `WINDOW_KEY_9`,
`WINDOW_KEY_F1` .. `WINDOW_KEY_F12`, `WINDOW_KEY_F13` .. `WINDOW_KEY_F24`
(104..115), `ENTER`, `ESCAPE`, `BACKSPACE`, `TAB`, `SPACE`, `MINUS`, `EQUAL`,
`LEFT_BRACKET`, `RIGHT_BRACKET`, `BACKSLASH`, `NONUS_HASH`, `SEMICOLON`,
`APOSTROPHE`, `GRAVE`, `COMMA`, `PERIOD`, `SLASH`, `CAPS_LOCK`, `PRINT_SCREEN`,
`SCROLL_LOCK`, `PAUSE`, `INSERT`, `HOME`, `PAGE_UP`, `DELETE`, `END`,
`PAGE_DOWN`, `RIGHT`, `LEFT`, `DOWN`, `UP`, `NUM_LOCK`, `KP_DIVIDE`,
`KP_MULTIPLY`, `KP_MINUS`, `KP_PLUS`, `KP_ENTER`, `KP_0` .. `KP_9`,
`KP_PERIOD`, `KP_EQUAL`, `KP_COMMA`, `NONUS_BACKSLASH`, `MENU`, `POWER`,
`MUTE`, `VOLUME_UP`, `VOLUME_DOWN`, `INTL1` .. `INTL6`, `LANG1` .. `LANG5`,
`LEFT_CTRL`, `LEFT_SHIFT`, `LEFT_ALT`, `LEFT_SUPER`, `RIGHT_CTRL`,
`RIGHT_SHIFT`, `RIGHT_ALT`, `RIGHT_SUPER`. The values are USB HID usages.

Helpers for backends and tools: `window_evdev_to_key`, `window_keysym_to_key`,
`window_keysym_to_ucs`, `window_ucs_to_keysym`, `window_keysym_is_dead`,
`window_dead_compose`, `window_dead_spacing`.

## Mouse and cursor

| Function | Does |
|---|---|
| `window_mouse_position(w, &x, &y)` | Logical position. |
| `window_mouse_down(w, button) -> bool` | `WINDOW_BUTTON_LEFT`, `_MIDDLE`, `_RIGHT`, `_X1`, `_X2`. |
| `window_warp_mouse(w, x, y)` | Move the pointer (X11, Win32). |
| `window_set_cursor(w, shape)` | `WINDOW_CURSOR_ARROW`, `_TEXT`, `_HAND`, `_CROSSHAIR`, `_WAIT`, `_RESIZE_EW`, `_RESIZE_NS`, `_RESIZE_NWSE`, `_RESIZE_NESW`, `_MOVE`, `_NOT_ALLOWED`. |
| `window_set_cursor_image(w, rgba, width, height, hx, hy) -> bool` | Up to 256x256. |
| `window_set_cursor_mode(w, mode)`, `window_cursor_mode(w)` | `WINDOW_CURSOR_NORMAL`, `_HIDDEN`, `_LOCKED`, `_CONFINED`. |
| `window_set_double_click_time(us: int64)` | Default 400 ms. |

## Clipboard

| Function | Does |
|---|---|
| `window_clipboard_set(w, text: string)` | Copy UTF-8 text. |
| `window_clipboard_get(w) -> string` | Paste; free with `free(s.chars)`. "" when empty. |
| `window_uri_list_to_paths(s, n, out) -> int64` | text/uri-list to paths (used for drops). |

## Monitors

| Function | Does |
|---|---|
| `window_monitor_count() -> int32` | |
| `window_monitor(i, out: WindowMonitor*) -> bool` | |
| `window_monitor_of(w) -> int32` | The monitor holding most of the window. |

`WindowMonitor`: `x`, `y`, `w`, `h`, `work_x`, `work_y`, `work_w`, `work_h`,
`mm_w`, `mm_h`, `refresh_mhz`, `scale`, `primary`, `name[64]`, `name_len`, `id`.

## Gamepads

| Function | Does |
|---|---|
| `window_gamepad_count() -> int32` | Connected pads. |
| `window_gamepad_connected(pad) -> bool` | |
| `window_gamepad_name(pad) -> string` | |
| `window_gamepad_button(pad, button) -> bool` | `WINDOW_PAD_SOUTH`, `_EAST`, `_WEST`, `_NORTH`, `_BACK`, `_GUIDE`, `_START`, `_LEFT_STICK`, `_RIGHT_STICK`, `_LEFT_SHOULDER`, `_RIGHT_SHOULDER`, `_DPAD_UP`, `_DPAD_DOWN`, `_DPAD_LEFT`, `_DPAD_RIGHT`, `_MISC`. |
| `window_gamepad_axis(pad, axis) -> float32` | `WINDOW_AXIS_LEFT_X`, `_LEFT_Y`, `_RIGHT_X`, `_RIGHT_Y`, `_LEFT_TRIGGER`, `_RIGHT_TRIGGER`. |
| `window_gamepad_rumble(pad, low, high, ms) -> bool` | Motor strengths 0..1. |
| `window_gamepad_guid(pad, out: uint8*)` | 32 hex digits and a NUL, as in mapping files. |
| `window_gamepad_add_mappings(text: string) -> int32` | SDL_GameControllerDB lines; returns how many were added. |

## Native handles

`WindowNative`: `backend`, `x11_window`, `x11_visual`, `x11_screen`,
`display_name[64]` (X11 display, NUL-terminated), `wl_display` (always 0),
`wl_surface_id`, `hwnd`, `hinstance`.
