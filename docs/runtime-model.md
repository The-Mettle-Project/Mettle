# Runtime Model

Mettle has no GC, async scheduler, heap manager, thread pool, or startup shim that every program must link.

A typical program links **libc only**. The compiler emits `mainCRTStartup` (Windows) or `_start` (Linux), calls your `main`, and exits. Heap use goes straight to `calloc(1, n)`. There is no `runtime_init`, `runtime_shutdown`, background thread, or allocator state inside Mettle.

This page covers what gets emitted, the two optional helper objects in `src/runtime/`, and when the linker pulls them in.

## Call boundary

| Mettle feature | Compiler output | Linked at build time |
|---|---|---|
| `main` / `main(argc, argv)` | Startup object calls `__getmainargs` on Windows or reads `argc`/`argv` from the stack on Linux, then calls `main` and exits | libc |
| `new T`, array literals, `string + string` | `extern calloc`; `call calloc` with `(1, size)` | libc `calloc` |
| `std/win32` / `std/net` / `std/thread` | `extern` to DLL symbols (`CreateFileA`, etc.) | Win32 DLLs |
| `std/thread` interlocked atomics | `extern mettle_atomic_*`; `call mettle_atomic_*` | `atomics.o` |
| Null/bounds traps (normal build), `-d` / `-s` | `extern mettle_crash_trap_ex` (or `mettle_crash_trap`); startup via `mettle_crash_startup` | `crash_handler.o` |
| `-g` / `--debug-symbols` alone | No crash hooks unless `-s`/`-d` or IR traps reference `mettle_crash_*` | usually nothing extra |
| `--release` | Traps off; startup skips crash-handler init | nothing extra |

Older Mettle shipped a large runtime (GC, async executor, coroutine scheduler, channels, tracked heap). That code is removed. What remains are two small helper objects, linked only when referenced.

## Helper objects

Sources live in `src/runtime/`. Installed copies sit under `bin/runtime/` (local build) or `runtime/` (installer). The linker includes a helper only if the emitted object has undefined symbols with the matching prefix.

### `crash_handler.o`

Linked when the object references `mettle_crash_*`. That happens if you pass:

- `-d` / `--debug`
- `-s` / `--stack-trace`
- or you build without `--release` and IR null/bounds checks are enabled **and** you also pass `-s` or `-d` so trap sites call `mettle_crash_trap` (without `-s`, dev builds still trap but use `puts`/`exit` with no symbolized backtrace)

Provides:

- Windows: vectored SEH handler. POSIX: `sigaction` on an alternate stack. Both catch access violations and similar faults.
- Frame-pointer walk plus compiler-embedded debug tables (`MettleCrashFunctionInfo`, `MettleCrashLocationInfo`, `MettleCrashTrapSiteInfo`) for symbolized backtraces. On `--build`, these live in COFF `.rdata` as `mettle_debug_header`, `mettle_debug_functions`, `mettle_debug_locations`, `mettle_debug_trap_sites`, and `mettle_debug_image`.
- `mettle_crash_trap_ex(kind, msg, pc, fp, arg0, arg1)`: trap sites call this when `-s` is active; prints a rich report (kind, file:line:column, source snippet with caret, dynamic index/length for bounds checks) and a symbolized stack trace, then exits with status 1.

Example (`mettle --build -s`):

```text
Fatal error: null pointer dereference
  --> app.mettle:9:10 in main
   |
9 |   return *p;
   |          ^ null pointer dereference
Stack trace:
  #0 main at app.mettle:9:10 (0x000000014000106A)
```

Native fault example:

```text
Unhandled runtime exception 0xC0000005 (access violation)
Attempted to write inaccessible memory at 0x0000000000000001
  --> app.mettle:3:3 in leaf_crash
Stack trace:
  #0 leaf_crash at app.mettle:3:3 (0x0000000140001086)
  #1 main at app.mettle:11:1 (0x0000000140001120)
```

Without `-d`, `-s`, or `-g`, and with `--release`, this object is not linked.

### `atomics.o`

Linked when the object references `mettle_atomic_*`. That happens when you use `std/thread` (Windows) or `std/thread_posix` and call:

- `atomic_compare_exchange_i32`
- `atomic_exchange_i32`
- `atomic_inc_i32`
- `atomic_dec_i32`

Each wrapper is a few lines over Win32 `Interlocked*` or GCC `__sync_*` builtins. Mettle cannot call `__sync_*` as normal externs and has no inline assembly, so these live in a linkable object.

`CreateThread` / `pthread_create` without those four helpers does not pull in `atomics.o`.

## `--build`

`mettle --build` scans the emitted object and links helpers as needed. You do not pass them on the command line.

```text
mettle --build hello.mettle -o hello.exe
  → hello.obj
  → libc only
```

```text
mettle --build -s hello.mettle -o hello.exe
  → hello.obj (mettle_crash_* referenced)
  → libc + crash_handler.o
```

```text
mettle --build hello.mettle -o hello.exe   (uses std/thread atomics)
  → libc + atomics.o
```

```text
mettle --build -s hello.mettle -o hello.exe   (crash traces + atomics)
  → libc + crash_handler.o + atomics.o
```

## Manual gcc link

With `-nostartfiles` (Mettle supplies `mainCRTStartup` / `_start`, not the CRT entry):

```bash
# no helpers
gcc -nostartfiles main.o -o main -lkernel32

# crash tracebacks (-d / -s / -g, or non-release with traps)
gcc -nostartfiles main.o path/to/runtime/crash_handler.o -o main -lkernel32

# std/thread atomics
gcc -nostartfiles main.o path/to/runtime/atomics.o -o main -lkernel32

# both
gcc -nostartfiles main.o \
    path/to/runtime/crash_handler.o \
    path/to/runtime/atomics.o \
    -o main -lkernel32
```

## "Runtime: none" in the README

Means no required runtime objects, no managed services, no hidden init/shutdown. Helpers are opt-in. `crash_handler.o` is diagnostics only; neither helper runs before `main`, spawns background work, or owns memory.

Rough analogy: these objects are like `crt0.o` for C: small linker glue, not a language runtime.

## Why two objects

`gc.o` used to bundle everything. Splitting by symbol prefix keeps link size minimal:

- `--release`, no thread atomics: zero helpers
- `-d` only: `crash_handler.o`
- atomics only: `atomics.o`

`objdump -t` on the binary shows which `mettle_crash_*` / `mettle_atomic_*` symbols came from which file.

## Possible future removal

`atomics.o`: on Windows, `std/thread` could extern `Interlocked*` from `kernel32.dll` directly. POSIX still needs compiler inline asm or a user C shim (same pattern as `posix_helpers.c` for `std/net_posix`).

`crash_handler.o`: harder to drop without losing line-level backtraces from `-d`/`-s`/`-g`. Opt-in linking already keeps it out of release builds that do not ask for it.

Both helpers are stable for now; ABI is not expected to change.
