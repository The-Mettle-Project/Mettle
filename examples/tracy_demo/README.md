# Tracy Demo

Demonstrates [Tracy](https://github.com/wolfpld/tracy) profiling from Mettle via `std/tracy`. The program simulates a small game-style frame loop with nested zones, live plots, tracked heap allocations, a background loader thread, and intentional frame hitches.

## Prerequisites

- Mettle built (`bin\mettle.exe`)
- Tracy source tree with `public\tracy\TracyC.h` and `public\TracyClient.cpp`
- MSVC (`cl`) or MinGW (`g++`) to compile the Tracy C++ client (handled by `mettle --build --tracy`)
- [Tracy Profiler](https://github.com/wolfpld/tracy/releases) running on the same machine

On the first build, `build.bat` prompts for your Tracy repo root and saves it to `examples/tracy_demo/tracy_dir.local.bat` (gitignored). Mettle also saves the path to `.mettle/tracy_dir`. Later runs reuse those paths automatically.

Override without editing the saved file:

```bat
set TRACY_DIR=C:\path\to\tracy
examples\tracy_demo\build.bat
```

Or pass the path as the first argument (also updates the saved config):

```bat
examples\tracy_demo\build.bat "C:\path\to\tracy"
```

## Build

From the repository root:

```bat
examples\tracy_demo\build.bat
```

This produces `examples\tracy_demo\tracy_demo.exe` via `mettle --build --tracy`.

Equivalent from the repo root (after setting `TRACY_DIR` or `--tracy-dir`):

```bat
bin\mettle.exe --build --tracy examples\tracy_demo\tracy_demo.mettle -o examples\tracy_demo\tracy_demo.exe
```

### Stub build (no Tracy client)

Use `mettle --build` without `--tracy`. The compiler links the bundled no-op `tracy_helpers.o` stub automatically when `std/tracy` is imported. Zones, plots, and memory events become no-ops, but the program still runs.

## Run

1. Start **Tracy Profiler**.
2. Run `examples\tracy_demo\tracy_demo.exe`.
3. Connect when Tracy prompts for the client.

The demo runs **600 frames** (~10 seconds at 16 ms pacing) and exits.

## What to look for in Tracy

| Tracy view | Demo feature |
|------------|--------------|
| **Frame timeline** | Top-level `Frame` zone each frame; `tracy_frame_mark()` separates frames |
| **Nested zones** | `Input` → `Simulation` (`Physics`, `Gameplay`) → `RenderPrep` |
| **Zone colors** | Green input, blue simulation, orange render (via `tracy_color_*`) |
| **Zone metadata** | `Gameplay` sets entity count via `tracy_zone_value`; hitch frames add `tracy_zone_text` |
| **Plots** | `frame_ms`, `entity_count`, `heap_bytes` (memory format) |
| **Messages** | Startup, hitch warnings (colored), loader finished, shutdown |
| **Threads** | `Main` (frame loop) and `AssetLoader` (background load with progress text) |
| **Memory** | 64 KiB + 4 KiB buffers allocated via `tracy_malloc` / `tracy_heap_free` |
| **Hitches** | Every 90 frames: extra ~45 ms sleep + warning message (visible as slow frames) |

## Source locations in Mettle

Mettle does not yet provide `__FILE__` / `__LINE__` builtins. Zones pass an explicit file string (`examples/tracy_demo/tracy_demo.mettle`), approximate line numbers, and function names. **Zone names** are the primary navigation aid in Tracy until compiler source-location support exists.

## Recommended instrumentation pattern

```mettle
import "std/tracy";

fn work() {
  var z: TracyZone = tracy_zone_colored(
    cstr("work"), cstr("myapp.mettle"), 42, cstr("work"), tracy_color_update());
  defer tracy_scope_end(z);
  ...
}
```

See [`stdlib/std/tracy.mettle`](../../stdlib/std/tracy.mettle) for the full API.
