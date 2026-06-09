# Standard Library

The standard library lives under `stdlib/`. Modules are imported by path. The `std/` prefix is resolved under the stdlib root (default bundled auto-detect then `./stdlib`).

## Platform Support

The compiler and most stdlib modules work on Linux and Windows. The compiler emits `elf64` assembly on Linux and `win64` on Windows. Use `make` to build the compiler on Linux and macOS; use `build.bat` on Windows.

**Cross-platform modules:** `std/io`, `std/mem`, `std/math`, `std/conv`, and `std/process` use the C runtime and work on both Linux and Windows.

**Windows-only:** `std/win32` provides native Win32 bindings, `std/net` provides Winsock2 bindings, `std/thread` provides Win32 threading bindings, and `std/ui` provides a Win32 GUI layer. They do not work on Linux. Programs that import these modules or use `--prelude` (which includes `std/net`) will fail to link on Linux with undefined references to Win32 symbols. For networking/threading on Linux, use `std/net_posix` and `std/thread_posix` respectively.

**Linux/macOS:** `std/net_posix` provides POSIX socket bindings and `std/thread_posix` provides pthread-based threading. These modules require linking with `stdlib/posix_helpers.c`.

## std/io

Console and file I/O. `puts` writes a null-terminated string and appends a newline; `putchar` writes a single character; `getchar` reads one. `print` and `println` write a cstring (println adds a newline). `print_err` and `println_err` write to stderr (for error messages). `print_int` and `println_int` write an integer in decimal. `cstr(s: string) -> cstring` converts a Mettle string to a C string for passing to C functions. File operations: `fopen`, `fclose`, `fread`, `fwrite`, `fputs`, `fgets`, `fflush`. File handles are `cstring` (opaque `FILE*`). Stream accessors: `get_stdin`, `get_stdout`, `get_stderr`.

## std/mem

Memory management. C runtime functions: `malloc`, `calloc`, `realloc`, `free`, `memset`, `memcpy`, `memmove`, `memcmp`. Helpers: `alloc_zeroed` (allocate and zero-initialize), `buf_dup` (allocate and copy a buffer). Use `malloc`/`free` for buffers, C interop, and explicit lifetimes. `new` emits direct zero-initialized `calloc` allocation. See [Heap Allocation](heap-allocation.md).

## std/math

Math utilities. `abs` (C runtime). `min`, `max`, `clamp` (integer operations on int64).

## std/conv

Conversions and character classification. C runtime: `atoi`, `atol`. Digit helpers: `digit_to_char`, `char_to_digit`. Classification: `is_digit`, `is_upper`, `is_lower`, `is_alpha`, `is_alnum`, `is_space`. Case conversion: `to_lower`, `to_upper`. String utilities: `strlen`, `streq`, `strncmp(buf, offset, needle, buf_len)`.

## std/process

Process control. `exit` terminates the program with an exit code. `rand`, `srand` for pseudo-random numbers.

## std/win32

Native Win32 bindings for Windows-only programs. The module exports prefixed raw bindings such as `Win32_GetLastError`, `Win32_GetStdHandle`, `Win32_WriteFile`, `Win32_GetSystemMetrics`, and `Win32_MessageBoxA`, plus friendlier wrappers such as `win32_last_error`, `win32_stdout`, `win32_write_stdout`, `win32_get_system_metrics`, `win32_tick_count64`, and `win32_sleep_ms`.

The internal PE linker probes common Win32 DLLs directly (`kernel32`, `user32`, `gdi32`, `advapi32`, `ws2_32`, plus the C runtime DLLs), so `mettle --build --emit-obj --linker internal` can call those APIs without a C bridge or import-library link flags. External GCC/MSVC linking may still need the matching `-l...` or `.lib` arguments.

## std/ui

Windows-only native GUI framework built on Win32 (`user32` + `gdi32`). Does not work on Linux. Import with `import "std/ui";` and build with the internal linker:

```bash
mettle --build --linker internal app.mettle -o app.exe
```

App and window lifecycle:

- `ui_init()` registers the shared window class (safe to call multiple times)
- `ui_window_create(title, x, y, width, height, window_proc) -> int64` creates a top-level window; pass `&your_proc` as the callback
- `ui_window_create_centered(title, width, height, window_proc) -> int64` centers a top-level window on the primary display
- `UiAppConfig` + `ui_app_run(config)` provide a tiny app shell for create/show/run
- `ui_window_show(hwnd)` displays the window
- `ui_window_hide`, `ui_window_close`, `ui_window_move`, `ui_window_set_pos`, `ui_window_set_title`
- `ui_window_client_rect(hwnd) -> UiRect`, `ui_window_rect(hwnd) -> UiRect`, `ui_client_width`, `ui_client_height`
- `ui_run_message_loop() -> int32` runs until `ui_quit(code)` or `WM_DESTROY`
- `ui_shutdown()` unregisters the window class (optional)

Window procedure helpers:

- `ui_def_window_proc(hwnd, msg, wparam, lparam)` forwards to `DefWindowProcA`
- Message constants such as `UI_WM_PAINT()`, `UI_WM_COMMAND()`, `UI_WM_TIMER()`, `UI_WM_SIZE()`, `UI_WM_KEYDOWN()`, mouse messages, and `UI_WM_DESTROY()`
- `ui_command_id(wparam)` / `ui_command_notify(wparam)` decode `WM_COMMAND`
- `ui_size_width(lparam)` / `ui_size_height(lparam)` decode `WM_SIZE`
- `ui_mouse_x(lparam)` / `ui_mouse_y(lparam)` decode mouse coordinates
- `ui_set_user_data(hwnd, value)` / `ui_get_user_data(hwnd)` expose `GWLP_USERDATA`
- `ui_timer_start(hwnd, id, elapsed_ms)` / `ui_timer_stop(hwnd, id)` wrap Win32 timers

Geometry, layout, and theme helpers:

- `UiRect`, `UiPoint`, `UiSize`, `UiInsets`, `UiLayoutCursor`, `UiTheme`, `UiFontConfig`
- `ui_rect_xywh`, `ui_make_rect`, `ui_rect_width`, `ui_rect_height`, `ui_rect_inset`, `ui_rect_offset`, `ui_rect_contains`
- `ui_stack_vertical`, `ui_stack_horizontal`, `ui_stack_next`, `ui_stack_next_h`
- `ui_row_rect(&parent, index, row_height, gap)` and `ui_column_rect(&parent, index, count, gap)`
- `ui_default_theme()` returns a practical neutral/accent color set
- `ui_rgb`, `ui_color_r`, `ui_color_g`, `ui_color_b`, `ui_scale`

Drawing and fonts (typically inside `UI_WM_PAINT`):

- `ui_begin_paint(hwnd, &ps) -> hdc`, `ui_end_paint(hwnd, &ps)`
- `ui_fill_rect_color`, `ui_fill_rect_color_rect`, `ui_frame_rect_color`
- `ui_draw_line`, `ui_draw_rect_outline`, `ui_draw_ellipse_outline`
- `ui_draw_text`, `ui_draw_text_color`, `ui_draw_text_transparent`
- `ui_draw_text_rect`, `ui_draw_text_rect_color` with `UI_DT_*` flags
- `ui_create_font`, `ui_create_font_config`, `ui_control_set_font`, `ui_select_object`, `ui_delete_object`
- `ui_rgb(r, g, b)` builds a `COLORREF`

Common controls (child windows):

- Generic creation: `UiControlConfig`, `ui_control_create`, `ui_control_create_raw`
- `ui_button_create(parent, x, y, w, h, label, id)`
- `ui_button_create_default(...)` for the default push button
- `ui_checkbox_create`, `ui_checkbox_is_checked`, `ui_checkbox_set_checked`
- `ui_radio_create`, `ui_groupbox_create`
- `ui_label_create(parent, x, y, w, h, text, id)`
- `ui_label_create_center(...)`
- `ui_textbox_create(parent, x, y, w, h, id)`
- `ui_textbox_create_multiline(...)`
- `ui_listbox_create`, `ui_listbox_add`, `ui_listbox_clear`, `ui_listbox_selected`, `ui_listbox_select`
- `ui_combobox_create`, `ui_combobox_add`, `ui_combobox_clear`, `ui_combobox_selected`, `ui_combobox_select`
- `ui_control_set_text(control, text)` / `ui_control_get_text(control, buf, max_len)`
- `ui_control_move`, `ui_control_move_rect`, `ui_control_show`, `ui_control_hide`, `ui_control_enable`, `ui_control_focus`

Menus and dialogs:

- `ui_menu_create`, `ui_menu_popup_create`, `ui_menu_append_item`, `ui_menu_append_separator`, `ui_menu_append_popup`
- `ui_window_set_menu(hwnd, menu)`, `ui_menu_destroy(menu)`
- `ui_alert(owner, title, text)` displays an informational message box

See `examples/ui_demo/ui_demo.mettle` for a documentation browser that dynamically scans `docs/`, loads Markdown at runtime, and renders styled headings, lists, and code blocks.

## std/system

Process spawning. `system(cmd: cstring) -> int32` runs a shell command via the C runtime. Use for invoking mettle, nasm, gcc, git, curl, etc. On Windows invokes cmd.exe; on Linux invokes sh -c.

## std/dir

Directory and file operations. Requires linking `stdlib/dir_helpers.c`. `dir_exists(path: cstring) -> int32` returns 1 if the path is an existing directory. `dir_create(path: cstring) -> int32` creates a directory (returns 0 on success). `file_exists(path: cstring) -> int32` returns 1 if the path is an existing regular file. `getcwd(buf: cstring, size: int32) -> int32` fills `buf` with the current working directory (returns 0 on success). `dir_list_md_files(root_dir, paths_buf, paths_size, max_files) -> int32` recursively lists `.md` files under `root_dir`, writing null-separated relative paths into `paths_buf`. Cross-platform (Windows and Linux).

## std/http

HTTP fetch (MVP). `http_fetch_to_file(url: cstring, output_path: cstring) -> int32` downloads a URL to a file using curl. Requires curl in PATH. Returns curl's exit code (0 = success).

## std/net

Winsock2 bindings for Windows only. Does not work on Linux. The internal PE linker resolves `ws2_32.dll` directly; external GCC/MSVC linking may still require `-lws2_32` or `ws2_32.lib`. Constants include address/socket/protocol values (`AF_INET`, `SOCK_STREAM`, `IPPROTO_TCP`) and common socket options (`SOL_SOCKET`, `SO_REUSEADDR`) plus shutdown values (`SD_RECEIVE`, `SD_SEND`, `SD_BOTH`).

Core functions: `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `shutdown`, `closesocket`, `setsockopt`. Lifecycle: `net_init`, `net_cleanup`, `net_last_error`.

`net_init`/`net_cleanup` are thread-safe and reference-counted. Multiple threads can call `net_init` safely; Winsock startup happens once and cleanup happens when the last caller releases via `net_cleanup`. Extra cleanup calls are treated as no-op for robustness.

Convenience helpers:
- `socket_tcp`, `socket_udp`
- `sockaddr_in(ip, port)`, `sockaddr_in_any(port)`
- `set_reuseaddr(sock, enabled)`
- `send_all(sock, buf, len)` (looping send until full write or error)
- `net_is_initialized()`

For HTTP responses, prefer sending header and body in separate `send_all` calls. If you omit `Content-Length`, include `Connection: close` and close the socket after sending.

## std/net_posix

POSIX socket bindings for Linux and macOS. Does not work on Windows. Socket functions are in libc on both platforms; no extra link flags are required for basic sockets. However, this module requires `stdlib/posix_helpers.c` for thread-safe errno access and atomic spin-lock operations.

Link command:
```bash
# Linux
gcc -o myapp output.o stdlib/posix_helpers.c -lpthread
# macOS  
gcc -o myapp output.o stdlib/posix_helpers.c
```

Constants include address/socket/protocol values (`AF_INET_POSIX`, `SOCK_STREAM_POSIX`, `IPPROTO_TCP_POSIX`) and socket options (`SOL_SOCKET_POSIX`, `SO_REUSEADDR_POSIX`). Note: macOS uses different values for `SOL_SOCKET` (0xFFFF) and `SO_REUSEADDR` (4) than Linux (1 and 2).

Core functions: `socket`, `close_fd`, `posix_bind`, `posix_listen`, `posix_accept`, `posix_connect`, `posix_send`, `posix_recv`, `posix_shutdown`, `posix_setsockopt`. Lifecycle: `net_posix_init`, `net_posix_cleanup`, `net_posix_last_error`.

`net_posix_init`/`net_posix_cleanup` are thread-safe and reference-counted for API compatibility with `std/net`, though POSIX sockets don't require initialization.

Convenience wrappers:
- `socket_tcp_posix`, `socket_udp_posix`
- `sockaddr_in_posix(ip, port)`, `sockaddr_in_any_posix(port)`
- `set_reuseaddr_posix(sock, enabled)`
- `send_all_posix(sock, buf, len)` (looping send until full write or error)
- `net_posix_is_initialized()`

The function names are prefixed with `posix_` to avoid conflicts with the Windows `std/net` module when writing cross-platform code.

## std/thread

Windows Win32 thread primitives. Includes:
- Thread APIs: `CreateThread`, `WaitForSingleObject`, `CloseHandle`, `GetCurrentThreadId`, `Sleep`
- Mutex APIs: `CreateMutexA`, `ReleaseMutex` with wrappers (`mutex_create`, `mutex_lock`, `mutex_unlock`, `mutex_close`)
- Atomics: `InterlockedCompareExchange`, `InterlockedExchange`, `InterlockedIncrement`, `InterlockedDecrement` with wrapper helpers
- Spin lock helpers: `spin_try_lock`, `spin_lock`, `spin_unlock` for short critical sections

`CreateThread` accepts a function pointer `fn(cstring) -> uint32` for the thread entry. Pass `&my_thread_proc` directly; no C bridge is required. See [Types](types.md#function-pointer-type) for function pointer syntax.

## std/thread_posix

pthread-based threading for Linux and macOS. Includes `pthread_create`, `pthread_join`, mutexes, and atomics. Requires linking with `stdlib/posix_helpers.c` and `-lpthread` on Linux. `pthread_create` accepts a function pointer `fn(cstring) -> cstring` for the thread entry; pass `&my_thread_proc` directly.

## std/tracy

[Tracy](https://github.com/wolfpld/tracy) real-time profiler bindings. Use `mettle --build --tracy` to compile and link the Tracy client automatically (see `docs/compilation.md`). Without `--tracy`, `mettle --build` still links a no-op stub from `bin/runtime/tracy_helpers.o` when the program references `std/tracy`.

**Zones:** `tracy_zone`, `tracy_zone_colored`, `tracy_zone_on_demand` (respects `tracy_connected()` for on-demand builds). End with `defer tracy_scope_end(z)` (alias of `tracy_zone_end`). Zone text/name/value/color helpers available.

**Ergonomics:** `tracy_color_input`, `tracy_color_update`, `tracy_color_render`, `tracy_color_load`, `tracy_color_warn`; `tracy_plot_setup_number`, `tracy_plot_setup_memory`; `tracy_malloc` / `tracy_heap_free` (tracked heap for memory timeline).

**Frames, plots, messages:** `tracy_frame_mark`, `tracy_plot`, `tracy_plot_int`, `tracy_message`, `tracy_message_colored`, `tracy_app_info`. Thread names: `tracy_set_thread_name`. Connection: `tracy_connected`, `tracy_startup`, `tracy_shutdown`.

Full demonstrative program: [`examples/tracy_demo/`](../examples/tracy_demo/).

```powershell
mettle --build --tracy app.mettle -o app.exe
# or: examples\tracy_demo\build.bat
```

Set `TRACY_DIR`, pass `--tracy-dir <path>`, or create `.mettle\tracy_dir` with the Tracy repo root.

Manual link (advanced, MSVC + internal linker):

```powershell
cl /c /DTRACY_ENABLE /I <tracy>\public stdlib\tracy_helpers.c
cl /c /DTRACY_ENABLE /I <tracy>\public <tracy>\public\TracyClient.cpp /TP
mettle --build app.mettle -o app.exe --link-arg stdlib\tracy_helpers.obj --link-arg TracyClient.obj
```

## std/prelude

The prelude re-exports `std/io`, `std/math`, `std/conv`, `std/mem`, `std/process`, and `std/net`. Use with `--prelude` to automatically import these modules without explicit `import` statements. The prelude is opt-in; it is not loaded by default. On Linux, `--prelude` will fail at link time because it pulls in `std/net` (Windows-only). Use explicit imports instead.

```bash
mettle --prelude main.mettle -o main.s
```

## std/gpu

NVIDIA CUDA Driver API bindings plus ergonomic helpers for running PTX kernels
on the GPU. Thin bindings over `nvcuda` (the OS driver, no `cudart`, no `nvcc`).
Link the driver import stub at build time (`--link-arg <CUDA>/lib/x64/cuda.lib`
on Windows; `-lcuda` on Linux).

Raw bindings: `cuInit`, `cuDeviceGet`, `cuCtxCreate_v2`, `cuModuleLoadData`,
`cuModuleGetFunction`, `cuMemAlloc_v2`, `cuMemFree_v2`, `cuMemcpyHtoD_v2`,
`cuMemcpyDtoH_v2`, `cuLaunchKernel`, `cuCtxSynchronize`.

Helpers (return handles directly; `0` on failure):

- `gpu_init() -> int64`: initialize CUDA on device 0 and create a context.
- `gpu_module(ptx_image: uint8*) -> int64`: load a module from a
  null-terminated PTX image in host memory.
- `gpu_func(mod: int64, name: cstring) -> int64`: resolve a kernel by name.
- `gpu_malloc(bytes: int64) -> int64`, `gpu_free(dptr: int64)`.
- `gpu_to_device(dst: int64, src: uint8*, bytes: int64)`,
  `gpu_to_host(dst: uint8*, src: int64, bytes: int64)`.
- `gpu_sync()`: wraps `cuCtxSynchronize`.
- `gpu_launch(f, grid, block, params, nargs)`: the primitive the
  [`dispatch`](gpu.md) statement lowers to (1-D `cuLaunchKernel` + synchronize).

See [GPU Offload](gpu.md) for kernel syntax (`kernel`, `thread.x`, `dispatch`)
and a complete example.
