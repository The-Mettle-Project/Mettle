

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/development/mettle-syntax/icons/mettle-dark.svg" />
  <img src="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/development/mettle-syntax/icons/mettle-light.svg" alt="Mettle" width="120" height="120" />
</picture>

# Mettle

**A from-scratch systems language that compiles straight to native x86-64.**

Its own backend, linker, and source-level debugger. An auto-vectorizer that beats `gcc -O3` on key kernels.
A compiler that explains every optimization decision and tells you when an edit made one regress.
A native NVIDIA GPU path. No LLVM, no VM, no managed runtime.

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
&nbsp;![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-2b6cb0.svg)
&nbsp;![Codegen](https://img.shields.io/badge/codegen-native%20x86--64-2f855a.svg)
&nbsp;![Dependencies](https://img.shields.io/badge/dependencies-no%20LLVM%20%C2%B7%20no%20VM-c53030.svg)
&nbsp;![GPU](https://img.shields.io/badge/GPU-CUDA%20%2F%20PTX-805ad5.svg)

[**Documentation**](docs/LANGUAGE.md)
&nbsp;·&nbsp; [**Install**](#install)
&nbsp;·&nbsp; [**Examples**](examples/)
&nbsp;·&nbsp; [**GitHub**](https://github.com/The-Mettle-Project/Mettle)
&nbsp;·&nbsp; [**Releases**](https://github.com/The-Mettle-Project/Mettle/releases)

</div>

---

Mettle compiles `.mettle` source to native x86-64. On Windows, `mettle --build` produces a PE executable using a built-in linker. On Linux, it produces ELF and links with the system toolchain. There is no LLVM dependency, no VM, and no managed runtime.

## Features

- Static types, pointers, structs, and enums
- Direct calls to C and OS APIs; a bundled stdlib for I/O, memory, math, and more
- `defer` / `errdefer` for scope cleanup; compile errors with source snippets
- Range-based `for` (`for i in 0..n`) and **`@simd` / `@simd!` vectorization contracts**: ask the optimizer to vectorize a loop, and with `@simd!` fail the build if it can't. An auto-vectorizer (AVX2) backs them, beating `gcc -O3` on several kernels; `--simd-report` shows what each loop became.
- `--explain` the optimizer as a collaborator: compile with `--explain` and the compiler reports every optimization decision in your file, which loops vectorized (and into what), which didn't (and why), which calls inlined, which were refused (and what to change). No annotations, no disassembly.
- **Suggested fixes are proven**: before printing "declare the accumulator as int64", the compiler applies that change to an internal copy, re-runs the optimizer, and only prints the suggestion when the loop actually vectorizes. Advice the simulation disproves is replaced with the honest reason.
- **Regression detection at compile time**: each `--explain` build remembers its decisions and the next one leads with the diff. A loop that was vectorized and no longer is gets reported as REGRESSED, in red, with the reason, before you ever run a benchmark. `--explain-json` emits the whole report as a machine-readable sidecar for CI (`fail if changesRegressed > 0` is a one-liner).
- **A built-in source-level debugger**, no gdb, no PDB, no DWARF: `--debug-hooks` instruments the program and a named-pipe runtime gives breakpoints (including conditional ones and logpoints), stepping, call stacks, struct and array expansion, and live variable inspection. Writes too: edit a variable while paused and the program takes the new value. Hardware faults stop the debugger at the faulting source line with the stack and every variable intact.
- **Optimization contracts**: `@simd!` (vectorize or fail the build), `@inline!` (every call inlines or fail with the inliner's reason), `@noalloc` (the function and everything it reaches is *proven* allocation-free, or the build fails pointing at the allocation). State intent; the compiler delivers it or explains the refusal.
- **GPU offload to NVIDIA** via a native CUDA/PTX backend: write `kernel` functions, launch them with `dispatch K[grid, block](args)`, no LLVM or `nvcc`. See [GPU offload](docs/gpu.md).
- Optional Tracy profiling, runtime timing, and debug stack traces

Windows is the most complete target (internal PE linker, Win32 GUI via `std/ui`). Linux supports builds, a libc-backed stdlib, and compiler development. See [known limitations](docs/known-limitations.md) for caveats.

## Example

Save as `hello.mettle`:

```mettle
import "std/io";

function fib(n: int32) -> int64 {
  if (n <= 1) { return n; }
  var a: int64 = 0;
  var b: int64 = 1;
  var i: int32 = 2;
  while (i <= n) {
    var next = a + b;
    a = b;
    b = next;
    i = i + 1;
  }
  return b;
}

function main() -> int32 {
  print("fib(10) = ");
  print_int(fib(10));
  newline();
  return 0;
}
```

```bash
mettle --build hello.mettle -o hello
./hello          # Windows: .\hello.exe
```

Pass `--prelude` to pull in common stdlib modules without explicit imports. Import networking yourself when you need it (`std/net` on Windows; `std/net` or `std/net_posix` on Linux).

## Install

**Linux (x86-64)**

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
```

**Windows (x86-64), PowerShell**

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex
```

Installs to `~/.mettle` (Linux) or `%LOCALAPPDATA%\Mettle` (Windows), updates user PATH, and checks for a C toolchain when linking stdlib programs. No root or admin required. Pin a release: `--version v0.11.0` (Linux) or `-Version v0.11.0` (Windows).

```bash
mettle --version
```

Dev builds from source report `v0.9.3` unless `METTLE_VERSION_RAW` is set at compile time.

## Build from source

**Windows** (gcc or clang):

```powershell
.\build.bat          # default: gcc
.\build.bat clang
```

**Linux / macOS** (build the compiler on the host; codegen targets x86-64 Windows and Linux):

```bash
make                 # bin/mettle + bundled stdlib/ and runtime/
make install         # optional: /usr/local/bin, stdlib, runtime
```

Typical release build:

```powershell
.\bin\mettle.exe --build --release hello.mettle -o hello.exe
```

```bash
./bin/mettle --build --release hello.mettle -o hello
```

Useful flags: `--build` (executable), `--release` / `-O` (optimized), `--emit-obj` (native object, the default), `--explain` / `--explain-json` (optimization report with since-last-build diffing), `--debug-hooks` (debugger instrumentation; what the editor's F5 uses), `-d` / `-s` / `-g` (debug and stack traces), `--profile-runtime`, `--tracy`, `--native-heap` (route `new`/`malloc`/`calloc`/`realloc`/`free` through Mettle's own allocator in `std/alloc` instead of the OS heap manager). Full list: `mettle --help` and `mettle help build`.

## Documentation

- [Language reference](docs/LANGUAGE.md)
- [Control flow](docs/control-flow.md) (range-`for`, `@simd` vectorization contracts)
- [GPU offload](docs/gpu.md) (`kernel` / `dispatch`, CUDA/PTX backend)
- [Compilation](docs/compilation.md) (CLI, link pipelines, Tracy, profiling)
- [Imports](docs/imports.md)
- [Runtime model](docs/runtime-model.md)
- [Standard library](docs/standard-library.md)
- [C interop](docs/c-interop.md)
- [Known limitations](docs/known-limitations.md)

`mettle docs` prints paths to these files next to the compiler binary.

## Repository layout

```
src/            compiler (lexer through codegen, linker, diagnostics)
stdlib/         standard library
src/runtime/    optional helper objects (crash traces, atomics, ...)
tests/          regression tests; run_tests.ps1 on Windows
examples/       benchmarks and demos
tools/          ELF tests, benchmarks, fuzz scripts
mettle-syntax/  VS Code / Cursor extension
docs/           language and tooling reference
```

## Examples and benchmarks

Runnable samples live under `[examples/](examples/)`. Benchmark suites pair Mettle, C, and Rust; run them with:

```powershell
.\tools\benchmark\run-benchmarks.ps1
```

See `[examples/README.md](examples/README.md)` for `fib`, `grep`, `ui_demo`, `tracy_demo`, and others.

## Development

**Windows** (primary CI: full test suite):

```powershell
.\build.bat
.\tests\run_tests.ps1
.\tests\run_tests.ps1 -BuildCompiler
```

**Linux** (native ELF backend):

```bash
make -j"$(nproc)"
bash tools/test-elf-native.sh
```

Optional: `tools/fuzz/` (nightly workflow). Crash-handler unit test: `make test`.

## Editor support

The `[mettle-syntax](mettle-syntax/)` extension turns VS Code or Cursor into a full Mettle IDE:

- **Debugging on F5**: breakpoints, conditional breakpoints, logpoints, step in/over/out, call stacks, struct/array/pointer expansion in the Variables pane, hover and watch evaluation of paths like `box.min.x`, and Set Value that genuinely writes program memory. Crashes stop at the faulting line with the stack and variables still inspectable. Hand-written kernel objects next to the source (`extern ... = "symbol"` plus a sibling `.o`) are detected and linked automatically.
- **Navigation**: go to definition across imports, find references, rename, outline, workspace symbols, completion with struct fields and signature help, CodeLens with Run / Debug / Build above `main`.
- **Optimization report panel**: the `--explain` report as an interactive dashboard. One-click fixes for suggestions the optimizer has already verified by simulation, inline loop annotations in the editor, and a "since the last compile" section that shows what your edit just did to the optimizer's decisions, regressions first.
- Syntax highlighting, snippets, hover documentation, and compiler-backed diagnostics, as before.

Everything runs against the compiler in your workspace; there is no separate language server to install.

## License

Apache-2.0. See [LICENSE](LICENSE).
