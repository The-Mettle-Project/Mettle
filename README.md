<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/docs/assets/mettle-dark.svg" />
  <img src="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/docs/assets/mettle-light.svg" alt="Mettle" width="120" height="120" />
</picture>

# Mettle

**A systems language with its own compiler, all the way down.**

Native x86-64, its own linker, its own source-level debugger, and a CUDA backend.
No LLVM, no VM, no managed runtime.

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
&nbsp;![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-2b6cb0.svg)
&nbsp;![Dependencies](https://img.shields.io/badge/dependencies-no%20LLVM%20%C2%B7%20no%20VM-c53030.svg)

</div>

---

## Example

```mettle
import "std/io";

fn fib(n: int32) -> int64 {
  if (n <= 1) { return (int64)n; }
  var a: int64 = 0;
  var b: int64 = 1;
  var i: int32 = 2;
  while (i <= n) {
    var next: int64 = a + b;
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
mettle --build hello.mettle
./hello              # Windows: .\hello.exe
```

Types are always written out. Mettle infers no binding types, so every `var`
carries one.

## Install

**Linux (x86-64)**

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
```

**Windows (x86-64), PowerShell**

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex
```

Installs to `~/.mettle` or `%LOCALAPPDATA%\Mettle` and updates your PATH. No root
or admin. Pin a version with `--version v0.15.1` (Linux) or `-Version v0.15.1`
(Windows).

## What it does

**Memory safety without annotations.** A whole-program borrow analyser reports
use-after-free, double free, leaks, dangling returns, and pointers invalidated
by `realloc`, at compile time and across function boundaries. You write no
lifetimes and no ownership markers; it infers everything, and it is built to
report nothing it cannot prove. See [the borrow analyser](docs/borrow-checker.md).

**An optimizer that explains itself.** `--explain` prints what the optimizer did
to every loop and call, why a loop did or did not vectorize, and what changed
since your last build. Suggested fixes are simulated before they are printed, so
a suggestion has already been shown to work. `--explain-json` for CI.

**Contracts that fail the build.** `@simd!` requires a loop to vectorize,
`@inline!` requires every call site to inline, and `@noalloc` requires a proven
allocation-free call graph. If the compiler cannot deliver one, it stops and
says which site defeated it.

**An AVX2 auto-vectorizer** covering reductions, maps, dot products, byte and
quantized-integer kernels, and some serial recurrences. It beats `gcc -O3` on
several kernels in the benchmark suite.

**GPU offload to NVIDIA**, straight to PTX with no `nvcc` and no CUDA runtime.
Write `kernel` functions, declare them host-side, and launch:

```mettle
extern kernel(block = 256) vadd(a: float32*, b: float32*, c: float32*, n: int32);

dispatch vadd[work: n](da, db, dc, n);
```

Arguments are type-checked against the declaration, the grid is computed from the
declared block, and the launch handle resolves by name. Also: subgroup
collectives, atomics, tensor-core operations, kernel-side `printf`, and a
compile-time occupancy report. See [GPU offload](docs/gpu.md).

**A debugger with no external format.** Breakpoints, stepping, and live variable
read and write over `--debug-hooks`. No gdb, no PDB, no DWARF.

**Compile-time execution.** `@test` functions run in the compiler's interpreter
via `mettle test`, with no binary produced. `mettle trace` interprets one
function and prints a line-by-line value trace. `--pgo` interprets `main()` at
build time and feeds the measured call frequencies back into the optimizer.

**Crash forensics.** With `-s`, a fault reports what the bad address actually was
(a null field access, a freed heap block) instead of an address.
`--native-heap` catches use-after-free at the faulting instruction.

Windows is the most complete target: internal PE linker, Win32 GUI through
`std/ui`. Linux builds, links against libc, and is fully supported for compiler
development. See [known limitations](docs/known-limitations.md).

## Build from source

This repository is the whole toolchain: the language and its frontend, and
**libmtlc** — the IR, the optimizers, code generation and native linking — under
the same `src/`. There is nothing to fetch. The build is offline.

**Windows** (gcc or clang):

```powershell
.\build.bat
.\tests\run_tests.ps1
```

**Linux**:

```bash
make -j"$(nproc)"
bash tools/test-elf-native.sh
```

To build the backend on its own — the archive a foreign frontend links against,
with none of the Mettle frontend in it:

```powershell
.\build.bat --backend-only   # Linux: make libmtlc
```

See [Mettle and libmtlc](docs/mettle-and-libmtlc.md) for where the frontend ends
and the backend begins, and why that line is worth keeping inside one repository.


The editor extensions are deliberately elsewhere: they live in
[MettleMisc](https://github.com/The-Mettle-Project/MettleMisc) — `mettle-syntax`
for VS Code and Cursor, and `clion-plugin` for the IntelliJ family.

## Examples and benchmarks

Runnable samples live in [examples/](examples/). The benchmark suites pair
Mettle against C:

```powershell
.\tools\benchmark\run-benchmarks.ps1
```

## License

Apache-2.0. See [LICENSE](LICENSE).
