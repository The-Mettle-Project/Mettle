

# Mettle

A statically typed systems language that compiles to native x86-64.

[GitHub](https://github.com/The-Mettle-Project/Mettle) | [Releases](https://github.com/The-Mettle-Project/Mettle/releases) | Language reference | Apache-2.0

Mettle compiles `.mettle` source to native x86-64. On Windows, `mettle --build` produces a PE executable using a built-in linker. On Linux, it produces ELF and links with the system toolchain. There is no LLVM dependency, no VM, and no managed runtime.

## Features

- Static types, pointers, structs, and enums
- Direct calls to C and OS APIs; a bundled stdlib for I/O, memory, math, and more
- `defer` / `errdefer` for scope cleanup; compile errors with source snippets
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

Installs to `~/.mettle` (Linux) or `%LOCALAPPDATA%\Mettle` (Windows), updates user PATH, and checks for a C toolchain when linking stdlib programs. No root or admin required. Pin a release: `--version v0.3.0` (Linux) or `-Version v0.3.0` (Windows).

```bash
mettle --version
```

Dev builds from source report `v0.9.2` unless `METTLE_VERSION_RAW` is set at compile time.

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

Useful flags: `--build` (executable), `--release` / `-O` (optimized), `--emit-asm` (legacy NASM path on Windows), `-d` / `-s` / `-g` (debug and stack traces), `--profile-runtime`, `--tracy`. Full list: `mettle --help` and `mettle help build`.

## Documentation

- [Language reference](docs/LANGUAGE.md)
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

Install the `[mettle-syntax](mettle-syntax/)` extension for `.mettle` syntax highlighting, snippets, and compiler-backed diagnostics in VS Code or Cursor.

## License

Apache-2.0. See [LICENSE](LICENSE).