<a href="https://the-mettle-project.github.io/Mettle/"><img src="website/media/readme-banner.jpg" alt="This is your language." width="100%"></a>

Mettle is a systems language with its own optimizer, assembler, linker, runtime and debugger. It compiles to native code for x86-64 and ARM64, and to PTX, SPIR-V and Metal for GPUs.

```mettle
@simd! fn checksum(data: uint8*, n: int64) -> int64 {
  var sum: int64 = 0;
  for i: int64 in 0..n {
    sum = sum + (int64)data[i];
  }
  return sum;
}
```

```text
checksum (loop @ line 3): vectorized -> vpsadbw, 32-wide byte sum (AVX2)
```

`@simd!` is a contract. If the loop cannot vectorize, the build stops and says why.

## What it does

- [Live programs](docs/live.md). Save a file and the running program takes the new code, even after a crash.
- [GPU kernels](docs/gpu.md). A kernel is a Mettle function, compiled to PTX, SPIR-V or Metal.
- [Explain](docs/explain.md). `--explain` reports what the compiler did with every loop and call, and what it took on trust.
- [Rules](docs/rules.md). A rule is a function you write that runs in the compiler and can stop the build.
- [Inferred lifetimes](docs/borrow-checker.md). The compiler works out ownership from the whole program. You write no annotations.
- [Metaprogramming](docs/metaprogramming.md). Layouts, optimizer passes and register allocators are Mettle functions.
- [Bare metal](docs/bare-metal.md). A target is described in Mettle, down to a boot sector or an instruction set you made up.

[Ideology](docs/ideology.md) explains why it is built this way.

## Speed

Over 16 whole programs Mettle runs in 1.04× the time of `clang -O3`, with the same answers on all 16. Run them with `tools/benchmark/suite3_exhibit.ps1`, or read the [benchmarks](docs/benchmarks/README.md).

[Potentia](https://systm.co.za/), an LLM engine written in Mettle, reaches the first token 1.7 to 2.4 times sooner than llama.cpp on the same card.

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/master/install.sh | sh
```

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/master/install.ps1 | iex
```

These install v0.18.0. To build from source, run `make -j"$(nproc)"` on Linux or `.\build.bat` on Windows.

[Getting started](docs/getting-started.md) | [Documentation](docs/README.md) | [What does not work yet](docs/known-limitations.md) | [Contributing](.github/CONTRIBUTING.md)

<sub>Apache 2.0.</sub>
