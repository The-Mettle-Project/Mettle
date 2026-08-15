<div align="center">

<picture>
  <source
    media="(prefers-color-scheme: dark)"
    srcset="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/docs/assets/mettle-dark.svg"
    width="120" height="120" />
  <img
    src="https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/docs/assets/mettle-light.svg"
    alt="Mettle" width="120" height="120" />
</picture>

# Mettle

**A systems language that brings its own compiler, linker, debugger and GPU backend.**

It compiles straight to 64 bit x86. No LLVM. No virtual machine. No garbage collector.

</div>

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
./hello           # on Windows, .\hello.exe
```

Types are always written out, on every `var`.

## Install

Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
```

Windows, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex
```

It installs to `~/.mettle` or `%LOCALAPPDATA%\Mettle` and puts that on your PATH.
Neither needs root or admin.

## What it does

**Finds memory bugs while it compiles.** It reads the whole program and reports
use after free, double free, leaks, dangling returns, and pointers `realloc`
left stale. You write no lifetimes and no ownership markers. It infers them. It
reports only what it can prove. See
[the memory analyser](docs/borrow-checker.md).

**Checks the rest as it runs, cheaply enough to ship.** `--safe` checks every
memory access at every optimization level. It then proves away what it can: a
constant index, a counter its loop already bounds, an index its own arithmetic
bounds, one check covering a whole loop. Whatever is left compares against an
allocation the loop resolved once. A surviving check costs a few instructions. A
vectorized dot product pays nothing, a CRC 1.04x, a heapsort whose indices come
out of comparisons 2.5x. See [checked access](docs/memory-safety.md).

**Says what the optimizer did.** `--explain` prints what became of every loop and
every call, what stopped a loop from vectorizing, and what changed since your
last build. It simulates each suggested fix before printing it. Every suggestion
has already been shown to work. Use `--explain-json` in CI.

**Fails the build when a promise breaks.** `@simd!` demands that a loop
vectorize, `@inline!` that every call site inline, `@noalloc` that a call graph
allocate nothing. When the compiler cannot deliver, it stops and names the site
that defeated it.

**Vectorizes for AVX2** across reductions, maps, dot products, byte kernels,
kernels over quantized integers, and some serial recurrences. It beats
`gcc -O3` on several kernels in the benchmark suite.

A branch that only picks a value is a value, not control flow, so a clamp, a
floor, a ReLU, a running extremum and a count of matches all vectorize, in
whatever order you write the tests and whether or not you factored them into
a helper. Buffers declared at file scope reach the same kernels as pointers
passed in. `--explain` names the reason for every loop it leaves alone.

**Offloads to NVIDIA GPUs**, straight to PTX, with no `nvcc` and no CUDA
runtime. Write `kernel` functions, declare them on the host, and launch them:

```mettle
extern kernel(block = 256) vadd(a: float32*, b: float32*, c: float32*, n: int32);

dispatch vadd[work: n](da, db, dc, n);
```

Arguments are checked against the declaration. The grid follows from the declared
block. Subgroup collectives, atomics, tensor core operations, `printf` inside a
kernel, and an occupancy report at build time all work. See
[GPU offload](docs/gpu.md).

**Runs your code while it compiles.** `@test` functions run in the compiler and
produce no binary. `mettle trace` interprets one function and prints its values
line by line. `--pgo` runs `main` at build time and feeds the call counts it
measured back to the optimizer.

**Debugs and reports crashes without outside tools.** Breakpoints, stepping, and
reading and writing live variables over `--debug-hooks`, with no gdb, no PDB and
no DWARF. Build with `-s` and a fault reports what the bad address was, such as a
null field or a freed block.

Windows is the most complete target, with an internal PE linker and `std/ui` for
windows and controls. Linux builds, links against libc, and fully supports
compiler development. See [what is missing](docs/known-limitations.md).

## Build from source

This repository holds the whole toolchain under one `src/`: the language and its
frontend, and **libmtlc**, which is the IR, the optimizers, code generation and
native linking. There is nothing to fetch. The build runs offline.

Windows, with gcc or clang:

```powershell
.\build.bat
.\tests\run_tests.ps1
```

Linux:

```bash
make -j"$(nproc)"
bash tools/test-elf-native.sh
```

For the backend alone, the archive another frontend links against:

```powershell
.\build.bat --backend-only
```

See [Mettle and libmtlc](docs/mettle-and-libmtlc.md) for the line between the
frontend and the backend.

Samples live in [examples/](examples/). The benchmark suites pair Mettle against
C:

```powershell
.\tools\benchmark\run-benchmarks.ps1
```

The editor extensions live in
[MettleMisc](https://github.com/The-Mettle-Project/MettleMisc): `mettle-syntax`
for VS Code and Cursor, `clion-plugin` for the IntelliJ family.

## License

Apache 2.0. See [LICENSE](LICENSE).
