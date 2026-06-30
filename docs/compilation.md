# Compilation

This document describes how to compile Mettle programs and the available compiler options.

## Compiler Usage

```bash
mettle [options] <input.mettle>
mettle help [topic]
mettle docs [topic]
```

The input file is the main source file. Imports are resolved relative to it. By default the compiler produces a native object (`output.obj` on Windows, `output.o` on Linux). `--build` produces a native executable directly: on Windows it emits a COFF object and links with the built-in PE linker; on Linux it emits an ELF object and links it with the platform C toolchain.

`std/...` imports use the stdlib bundled with the compiler by default. You do not need to copy `stdlib/` into every project directory. Use `--stdlib <dir>` only when you want to override the bundled stdlib.

## Built-In Help and Docs

The compiler includes topic-oriented help commands:

- `mettle help` prints CLI usage.
- `mettle help build` explains the Windows build flow.
- `mettle help runtime` explains the runtime model and the two opt-in helper objects (aliases: `heap`, `gc`).
- `mettle docs` lists the main documentation entry points and their paths.

Available topics: `build`, `runtime` (aliases `heap`, `gc`), `interop`, `stdlib`, `web`.

## Options

`-o <file>` output object file (default `output.obj` / `output.o`, or executable path when used with `--build`). `-i <file>` input file (alternative to positional argument). `-I <dir>` add import search directory (repeatable). `--stdlib <dir>` set stdlib root (default auto-detects bundled stdlib near the compiler binary, then falls back to `./stdlib`). `--build` build a native executable: Windows uses a COFF object + the internal PE linker by default; Linux emits an ELF object and links it with `gcc -no-pie`. `--static` adds static Linux linking, and `--musl` uses `musl-gcc -static`. `--emit-obj` emit a native object (the default). `--linker <internal|auto|gcc|msvc>` choose the Windows linker path (**default: `internal`**). `--link-arg <arg>` pass an extra linker argument in `--build` mode for additional DLLs/import libraries. `--tracy` link `std/tracy` with the Tracy profiler (requires `--build`; set `TRACY_DIR` or `--tracy-dir`). `--tracy-dir <dir>` Tracy repo root for `--tracy`. `--prelude` auto-import `std/prelude` (std/io, std/math, std/conv, std/mem, std/process, std/net). `-d`/`--debug` debug mode and embedded runtime crash traceback support. `-g`/`--debug-symbols` generate debug symbols. `-l`/`--line-mapping` source line mapping. `-s`/`--stack-trace` embeds runtime crash traceback support without the rest of debug mode. `-O`/`--optimize` enable optimizations. `-r`/`--release` enables `-O`, removes unreachable functions, and disables generated runtime null/bounds checks in IR lowering. `--profile-runtime` emit function-level runtime timing hooks and print a sorted report at process exit. `--emit-ptx` compile each function to an NVIDIA PTX `.entry` (the GPU backend) instead of a native object; see [GPU Offload](gpu.md). `--simd-report` (with `-O`/`--release`) print a note for every `@simd` loop saying whether it vectorized and into which kernel. `--explain` (with `-O`/`--release`) print a grouped optimization report for the main input file — per loop: vectorized (into which instruction-level kernel, e.g. `vfmadd231ps, 8-wide float32`) or `NOT vectorized` with a `└ reason:` and, when actionable, a `└ fix:` line; per call: inlined or not, with the same reason/fix treatment; loop nests are summarized (`vectorized inner, scalar outer`); a final backend section shows which functions got the register-allocating backend vs baseline codegen and why. No annotations required; remarks from imported modules are filtered out. `-h`/`--help` print usage. See [Imports](imports.md) for path resolution and `-I`/`--stdlib` details.

## Compilation Pipeline

The compiler runs these phases in order:

1. **Lexing** - tokenize source
2. **Parsing** - build AST
3. **Import resolution** - resolve and inline `import` directives
4. **Monomorphization** - expand generic functions and structs into concrete instantiations
5. **Type checking** - semantic analysis and symbol resolution
6. **IR lowering** - convert AST to intermediate representation
7. **Optimization** (optional, `-O`) - copy/constant propagation, integer folding/simplification, branch cleanup, unreachable IR cleanup, and control-flow/codegen branch peepholes
8. **Code generation** - emit a native object (COFF on Windows, ELF on Linux)

`--release` uses the same optimization pipeline as `-O` and additionally lowers without runtime null/bounds trap checks. Use `-O` for optimized builds that still keep those generated checks.

### Token String Views

Lexer tokens carry a `StringView` (`data` pointer + `length`) in addition to a null-terminated `value` string. This gives parser and diagnostics code direct token extents without requiring repeated `strlen`.

### String Interning

Identifier-like token text is interned: each distinct string is stored once in a global hash table, and subsequent occurrences reuse the same pointer. This reduces memory for repeated names and enables fast pointer-first equality checks in semantic structures.

The AST and symbol/type metadata intern name-bearing strings (identifier names, member names, type names, and type parameter names). The intern table is process-global for a compilation run and is cleared after compilation, so interned pointers are not reused across compiler invocations.

## Build Pipeline

### Recommended Windows Flow

1. Default end-to-end build: `mettle --build main.mettle -o main.exe`
2. Optional extra libraries: `mettle --build main.mettle -o main.exe --link-arg -lcustomdll`
3. External linker fallback: `mettle --build --linker auto main.mettle -o main.exe`

`--build` keeps compilation inside Mettle's COFF object emitter, bundled runtime objects, and internal PE linker. That path does not require `NASM`, `gcc`, or `link.exe` for the target executable. The internal linker probes common Win32 DLLs directly (`kernel32`, `user32`, `gdi32`, `advapi32`, `ws2_32`, `ucrtbase`, and `msvcrt`), so `std/win32`, `std/thread`, and `std/net` work without hand-written C bridge objects or default import-library flags. `--linker auto` tries the internal linker first and falls back to external linkers if needed. Four optional helper objects ship with the Mettle installation: `crash_handler.o` (linked only for `-d`/`-s`/`-g` or IR null/bounds traps), `atomics.o` (linked only when `std/thread` interlocked atomics are referenced), `profile.o` (linked when `--profile-runtime` is used), and `tracy_helpers.o` (no-op stub when `std/tracy` is referenced without `--tracy`). `--build` pulls them in automatically when needed.

### Linux Flow

On Linux, `--build` uses the native ELF backend end to end:

```bash
mettle --build main.mettle -o main
```

The compiler emits an ELF object, then links it with the platform C toolchain (`gcc -no-pie` by default). The Linux linker path automatically includes bundled helper objects for POSIX shims, atomics, stack tracebacks, and runtime profiling when the generated object references them. `--static` switches to `gcc -static`; `--musl` switches to `musl-gcc -static`.

The Linux path supports the syscall-backed standard-library variants (`*.linux.mettle`), runtime stack traces (`-d`/`-s`), `--profile-runtime`, and embedded DWARF sections in ELF objects.

### Tracy profiling (`--tracy`)

Build with live Tracy instrumentation (compiles `stdlib/tracy_helpers.c` and `TracyClient.cpp` from your Tracy repo):

```powershell
mettle --build --tracy app.mettle -o app.exe
```

Tracy repo resolution order: `--tracy-dir <path>`, then `TRACY_DIR`, then `.mettle\tracy_dir` in the current directory, then `examples\tracy_demo\tracy_dir.local.bat`. The resolved path is saved to `.mettle\tracy_dir` for later builds.

With MSVC (`cl`), Tracy objects link through the internal PE linker. With MinGW only (`g++`, no `cl`), Mettle uses `g++` for the final link and pulls in `secur32` / `dbghelp` automatically.

Without `--tracy`, programs that import `std/tracy` still link the bundled no-op `tracy_helpers.o` stub so the same source builds for release and profiled runs.

See [`examples/tracy_demo/`](../examples/tracy_demo/) for a full demo and [`stdlib/std/tracy.mettle`](../stdlib/std/tracy.mettle) for the API.

### Runtime profiling (`--profile-runtime`)

Build with profiling instrumentation:

```powershell
mettle --build --release --profile-runtime bench.mettle -o bench.exe
```

Every compiled Mettle function (non-`extern`, with a body) gets entry/exit hooks. At process exit the runtime prints a flat table to stderr (sorted by inclusive time) and a caller/callee call graph:

```
Runtime profile:
function             location                             calls    total_us    avg_ns    self_us     pct
__inl_helper_0       examples/sort_insertion.mettle:42    200      1200      6000      1200      25.0%
helper               examples/sort_insertion.mettle:1     0        0         0         0         0.0%
work                 examples/sort_insertion.mettle:9     1        43000     43000000  5000      100.0%
main                 examples/sort_insertion.mettle:17    1        43000     43000000  200       100.0%

Runtime profile (call graph):
main
  work  1 calls  43000 us
    __inl_helper_0  200 calls  1200 us
```

Each profile row includes a `file:line` location taken from the function definition (or from the call site for inlined bodies). When the optimizer inlines a callee, duplicated hooks get distinct `__inl_<name>_<N>` ids so you can still see hot inline sites.

v1 limitations: function-level only (no loop or inlined-pattern probes), Windows + direct COFF backend only, single-threaded stats, and meaningful overhead from per-call timing. Profile hooks are inserted in IR before optimization; when the inliner expands a callee, those hooks are duplicated at each call site so inlined functions still appear in the report.

### Compiler debugging / internal errors (ICE)

If the compiler crashes or hits a fatal internal failure, it prints an **internal compiler error** report to stderr with:

- compilation phase and IR pass name (when known)
- source file and function name
- IR instruction index and text (when available)
- last tracked compiler action (with `--debug-compiler`)
- native C backtrace

Example:

```
Mettle internal compiler error: access violation (0xC0000005)

Phase: IR optimization
Pass: memcpy_inline
File: examples/grep/grep.mettle
Function: fill_buffer
IR instruction: #184
  %tmp42 = memcpy_inline %src, %size, ...

Last action:
  collecting temp uses for IR_OP_MEMCPY_INLINE

Compiler backtrace:
  #0 mettle_compiler_ice_report at src/compiler/compiler_crash.c:282 (0x...)
  #1 main at src/main.c:123 (0x...)
  ...

Please rerun with:
  mettle --dump-ir --debug-compiler examples/grep/grep.mettle
```

`--debug-compiler` enables fine-grained last-action tracking and writes an IR snapshot (`<input>.ice.ir`) when an ICE occurs.

### Manual Object Link Flow

1. Compile: `mettle main.mettle -o main.obj` (or `main.o` on Linux)
2. Link: `gcc -nostartfiles main.obj -o main.exe -lkernel32` on Windows, or use the platform linker on Linux. Use `-nostartfiles` on Windows when linking compiler-emitted startup yourself.

The emitted entry point does not call any Mettle runtime initialization. Programs that do not use `-d`/`-s` crash tracebacks or `std/thread` interlocked atomics link **zero** Mettle runtime objects, even when they use `new` or string concatenation.

Link the relevant helper object(s) only when your program references their symbols:

- `crash_handler.o` if the program references `mettle_crash_`* (compiled with `-d`, `-s`, `-g`, or with IR null/bounds traps left enabled).
- `atomics.o` if the program references `mettle_atomic_*` (any use of `std/thread`'s `atomic_compare_exchange_i32` / `_exchange_i32` / `_inc_i32` / `_dec_i32`).
- `profile.o` if the program references `mettle_profile_*` (compiled with `--profile-runtime`).

```bash
gcc -nostartfiles main.o \
    path/to/runtime/crash_handler.o \
    path/to/runtime/atomics.o \
    -o main -lkernel32
```

Omit either object when the corresponding symbols are not referenced.

For concurrency, import `std/thread` (Windows) or `std/thread_posix` and call `CreateThread`/`pthread_create` directly — Mettle no longer has built-in `async`/`spawn`/`Channel<T>` keywords.

**Programs with `main(argc, argv)`:** If your entry point has the signature `fn main(argc: int32, argv: cstring*) -> int32`, Windows startup calls CRT `__getmainargs` before `main`. No Mettle argv shim is required.

## Compiler Diagnostics

Compile-time errors and warnings are printed with a stable error code, the
source location, a code snippet with a caret pointing at the offending span
(plus one line of surrounding context), and — where possible — a `help:`
suggestion. Output is colorized on a TTY and respects `NO_COLOR`,
`CLICOLOR`/`CLICOLOR_FORCE`, and `TERM=dumb`. Up to 100 diagnostics are
reported per run rather than stopping at the first.

Error codes: `E0001` lexical, `E0002` syntax, `E0003` semantic, `E0004`
type, `E0005` scope, `E0006` I/O, `E0007` internal. They are stable across
versions and useful for `grep` and documentation.

### "Did you mean?" suggestions

When you reference an undefined variable or function, the compiler searches
every name visible in the current scope chain and, if one is a close match
(case-insensitive Levenshtein distance within a length-scaled threshold),
suggests it:

```text
error[E0003]: Undefined variable 'countr'
  --> app.mettle:5:10
  |
4 |   var counter: int32 = 41;
5 |   return countr + 1;
  |          ^^^^^^
6 | }
   = help: did you mean 'counter'? (or declare 'countr' before using it)
```

The suggestion is scope-aware: only symbols actually reachable from the
error site are considered. If nothing is close enough, the diagnostic falls
back to the generic "declare it before using it" guidance, so unrelated
names never produce a misleading suggestion.

## Runtime Crash Tracebacks

Compile with `-s` or `-d` to embed runtime crash traceback support in the generated program. This adds failure-path-only metadata for Mettle function names and source locations and installs a crash handler at program startup. The crash handler is **cross-platform**: Windows uses a Structured Exception Handler, and POSIX (Linux/macOS) uses a `sigaction` handler running on an alternate signal stack. Both produce the same symbolized stack-trace format from the same embedded debug-info tables.

On the default **`--build` (direct COFF) path**, `-s` works without NASM: the compiler embeds `mettle_debug_functions` / `mettle_debug_locations` in the object, emits a `mettle_crash_startup` helper that calls `mettle_crash_install` and `mettle_crash_register_image`, and the internal linker's `mainCRTStartup` calls `mettle_crash_startup` before `main`. IR null/bounds traps call `mettle_crash_trap` when `-s` is active (otherwise they use `puts`/`exit` with no backtrace). Example:

```powershell
mettle --build -s tests\test_runtime_null_deref_check.mettle -o demo.exe
.\demo.exe
```

```text
Fatal error: Null pointer dereference
Stack trace:
  #0 main at tests\test_runtime_null_deref_check.mettle:9:10 (0x000000014000106A)
```

- `-s` enables embedded runtime crash tracebacks without the rest of debug mode.
- `-d` enables debug output and also implies embedded runtime crash tracebacks.
- `--release` still disables generated null/bounds runtime checks, so only native crashes remain traceable there.

A fatal fault prints the exception/signal, the faulting address (with a hint when it is a null-pointer dereference), and a symbolized stack trace. Compiler-generated null-dereference and array-bounds traps print the same traceback shape. Frames without registered Mettle debug info (libc/CRT) show as `<unknown>`, the same on both platforms.

Windows example (native access violation):

```text
Unhandled runtime exception 0xC0000005 (access violation)
Exception address: 0x00007FF7DFD71046
write access violation at 0x0000000000000001
Stack trace:
  #0 leaf_crash at app.mettle:2:3 (0x00007FF7DFD71046)
  #1 intermediate at app.mettle:6:3 (0x00007FF7DFD71080)
  #2 main at app.mettle:10:3 (0x00007FF7DFD710A0)
```

POSIX example (null dereference via SIGSEGV):

```text
Unhandled runtime signal 11 (segmentation fault (invalid memory access))
Faulting address: 0x0000000000000000  (null pointer dereference)
Fault instruction: 0x000057AD4388733D
Stack trace:
  #0 compute_total at app.mettle:12:5 (0x000057AD4388733D)
  #1 main at app.mettle:20:3 (0x000057AD438873CE)
```

Compiler-generated runtime traps are formatted the same on both platforms:

```text
Fatal error: Null pointer dereference
Stack trace:
  #0 main at app.mettle:9:10 (0x00007FF7DFD71046)
```

The signal handler runs on a dedicated alternate stack so that a stack-overflow `SIGSEGV` can still be reported rather than silently re-faulting, and uses only async-signal-safe primitives.

## Stack Safety Diagnostics

The compiler emits a warning for unusually large function stack frames (currently 256 KiB). This is intended as an early signal for stack overflow risk in deeply nested calls or thread stacks with limited reserve.

On Windows x64, Mettle now emits stack probing (`___chkstk_ms`) for large frame allocations (>4 KiB) before subtracting `rsp`, to avoid guard-page skips.

The warning threshold is currently fixed and may become configurable in a future release.

## Compiler debugging

Use explicit dump flags to inspect the compiler pipeline. Artifacts are written
next to `-o` (or under `--dump-dir` when set) using the output stem:


| Flag                                   | Output                                                                                    |
| -------------------------------------- | ----------------------------------------------------------------------------------------- |
| `--dump-ir`                            | `{stem}.ir.before.mettle` and `{stem}.ir.after.mettle` (same as `--dump-ir=before,after`) |
| `--dump-ir=before` / `--dump-ir=after` | Selected stage only                                                                       |
| `--dump-ir-passes`                     | `{stem}.ir.pass-<label>.mettle` after optimization milestones (requires `-O`)             |
| `--dump-ast`                           | `{stem}.ast.mettle` after type checking                                                   |
| `--dump-mono`                          | `{stem}.mono.mettle` index plus `{stem}.mono.<mangled>.mettle` per expansion              |
| `--dump-dir <dir>`                     | Directory for all dump files                                                              |


IR dumps include source locations (`; @file:line:col`), basic-block labels, and
parameter types. `-d`/`--debug` also enables `--dump-ir=before,after` plus debug
symbol generation.

```powershell
mettle --dump-ir -O app.mettle -o app.obj
mettle --dump-ir-passes -O app.mettle -o app.obj
mettle --dump-ast app.mettle -o app.obj
mettle --dump-mono tests/test_generics_multiple_instantiations.mettle -o out.obj
mettle help debug
```

Helper scripts live under `tools/debug/` (`dump-compiler-artifacts.ps1`,
`diff-ir.ps1`, `disasm-obj.ps1`).

With object emission or `--build`, `-g` embeds binary DWARF 4
sections (`.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_str`,
`.debug_frame`) in native objects (ELF on Linux; COFF/PE on Windows as supported by the active linker path) for GDB/LLDB. Locals and
parameters kept in GP registers by the optimizer (for example `r12`–`r15`) are
described with `DW_OP_regN` location expressions; stack-homed symbols use
`DW_OP_fbreg`. Runtime stack-trace tables (`-s`) are embedded in COFF objects on the default object / `--build` path (`.rdata` symbols `mettle_debug_functions`, `mettle_debug_locations`, plus `mettle_crash_startup`). They are separate from `-g` DWARF sections: `-s` alone enables tracebacks without emitting `.debug_*`.

## Testing

The test suite compiles and runs a set of programs. Run:

```powershell
.\tests\run_tests.ps1
.\tests\run_tests.ps1 -BuildCompiler
.\tests\run_tests.ps1 -SkipRuntime
```

`-BuildCompiler` rebuilds the compiler before running. `-SkipRuntime` skips optional runtime executable tests.
