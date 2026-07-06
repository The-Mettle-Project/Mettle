

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/development/mettle-syntax/icons/mettle-dark.svg" />
  <img src="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/development/mettle-syntax/icons/mettle-light.svg" alt="Mettle" width="120" height="120" />
</picture>

# Mettle

**A from-scratch systems language that compiles straight to native x86-64.**

Its own backend, linker, and source-level debugger. No LLVM, no VM, no managed runtime.

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

Mettle compiles `.mettle` source to native x86-64. On Windows, `mettle --build` produces a PE executable using a built-in linker. On Linux, it produces ELF and links with the system toolchain.

## Features

- Static types, pointers, structs, enums, closures, and `defer`/`errdefer`
- Direct calls to C and OS APIs; a bundled stdlib for I/O, memory, math, and more
- **Compile-time memory diagnostics**: use-after-free, double free, leaks, and dangling pointers caught at compile time, with no annotations. Interprocedural and zero false positives. See [borrow checker](docs/borrow-checker.md).
- **AVX2 auto-vectorizer** that beats `gcc -O3` on several kernels, plus `@simd` / `@simd!` contracts (vectorize or fail the build)
- **`--explain`**: the compiler reports every optimization decision, why loops did or didn't vectorize, and flags regressions since your last build. Suggested fixes are verified by simulation before they're printed. `--explain-json` for CI.
- **Optimization contracts**: `@inline!` and `@noalloc` fail the build with the compiler's reason if the guarantee can't be met
- **Built-in source-level debugger**: breakpoints, stepping, live variable read/write via `--debug-hooks`. No gdb, no PDB, no DWARF.
- **GPU offload to NVIDIA**: write `kernel` functions and launch with `dispatch K[grid, block](args)`. Native CUDA/PTX backend, no `nvcc`. See [GPU offload](docs/gpu.md).
- **Crash forensics**: with `-s`, faults report what the bad address is (null field access, freed heap block, etc.); `--native-heap` catches use-after-free at the faulting instruction
- Optional Tracy profiling, runtime timing, and debug stack traces

Windows is the most complete target (internal PE linker, Win32 GUI via `std/ui`). Linux supports builds, a libc-backed stdlib, and compiler development. See [known limitations](docs/known-limitations.md).

## Example

Save as `hello.mettle`:

```mettle
import "std/io";

fn fib(n: int32) -> int64 {
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

fn main() -> int32 {
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

## Install

**Linux (x86-64)**

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
```

**Windows (x86-64), PowerShell**

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex
```

Installs to `~/.mettle` (Linux) or `%LOCALAPPDATA%\Mettle` (Windows) and updates user PATH. No root or admin required. Pin a release with `--version v0.13.0` (Linux) or `-Version v0.13.0` (Windows).

## Build from source

**Windows** (gcc or clang):

```powershell
.\build.bat          # default: gcc
.\build.bat clang
```

**Linux / macOS**:

```bash
make                 # bin/mettle + bundled stdlib/ and runtime/
make install         # optional: /usr/local/bin, stdlib, runtime
```

Typical release build:

```bash
./bin/mettle --build --release hello.mettle -o hello
```

Common flags: `--release` / `-O`, `--explain`, `--debug-hooks`, `-d` / `-s` / `-g`, `--native-heap`. Full list: `mettle --help`.

## Documentation

- [Language reference](docs/LANGUAGE.md)
- [Borrow checker](docs/borrow-checker.md)
- [Control flow](docs/control-flow.md)
- [GPU offload](docs/gpu.md)
- [Compilation](docs/compilation.md)
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

Runnable samples live under [examples/](examples/). Benchmark suites pair Mettle, C, and Rust:

```powershell
.\tools\benchmark\run-benchmarks.ps1
```

## Development

**Windows** (primary CI: full test suite):

```powershell
.\build.bat
.\tests\run_tests.ps1
```

**Linux** (native ELF backend):

```bash
make -j"$(nproc)"
bash tools/test-elf-native.sh
```

## Editor support

The [mettle-syntax](mettle-syntax/) extension turns VS Code or Cursor into a full Mettle IDE: debugging on F5, go to definition, rename, completion, an interactive `--explain` dashboard, and compiler-backed diagnostics. Everything runs against the compiler in your workspace; no separate language server.

## License

Apache-2.0. See [LICENSE](LICENSE).
