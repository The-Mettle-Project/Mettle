<a href="https://suidvandiewereld.github.io/Mettle/"><img src="website/media/readme-banner.jpg" alt="This is your language. Do what you want with it." width="100%"></a>

**Mettle is a systems language with its own optimizer, linker, runtime and debugger.** It compiles to native code for x86-64 and ARM64, and to PTX, SPIR-V and Metal for GPUs.

## Faster than llama.cpp, on the same card

[Systm](https://systm.co.za/)'s Potentia runs LLMs on Mettle.

- **2.4× faster to the first token.** 35.7 ms against 87.1 ms.
- **1.41× faster writing code.** 140.5 against 99.3 tokens a second.
- **12× less memory after four users.** 1.8 GB against 21.5 GB.
- **38% less energy per token at eight requests.** 0.38 J against 0.61 J.

<sub>Gemma 4, QAT Q4_0, on one RTX 5060 Ti, against llama.cpp from LM Studio with every layer on the GPU and one engine in VRAM at a time. First token: 12B, a 128-token prompt. Code: 12B, with Gemma 4's assistant drafter in both engines. Memory: 26B-A4B, RAM right after four users.</sub>

## Everything else stays close to C

Across 16 whole programs, including parsers, compressors, interpreters and solvers, against `clang -O3` with the same answers on all 16:

- **1.09× clang's run time, on average.** 22.2 ms against 20.4 ms.
- **1.42× faster on a bytecode VM.** 28.0 ms against 39.7 ms.
- **1.22× faster on a sudoku solver.** 24.5 ms against 29.9 ms.
- **2× faster to build 90,000 lines.** 7.6 s against 16.0 s.

<sub>clang 22.1.6 on Windows, on an i7-12700K, best of 15 pinned runs. The slowest program is physics_grid at 1.29×. With `-march=native`, clang leads on all 16 and Mettle averages 1.13×. Build time is one generated file, and the lead grows on smaller ones. Run them yourself with `tools/benchmark/mc.ps1 -Suite 3`.</sub>

## What you can do

- **Change it while it runs.** Save a file and the running program takes the new code, even after a crash. [Live programs](docs/live.md)
- **Program the GPU directly.** A kernel is a Mettle function, compiled straight to PTX or Metal. [GPU offload](docs/gpu.md)
- **Make the compiler obey.** Put `@simd!` on a loop and it vectorizes, or the build stops and tells you why.
- **Make it explain itself.** `--explain` names what it did with every loop and call, and what it took on trust. [Compilation](docs/compilation.md)
- **Write your own laws.** A rule is a function you write that runs in the compiler and can stop the build. [Rules](docs/rules.md)
- **Never write a lifetime.** It reads the whole program and tells you what it can prove. [Memory analyser](docs/borrow-checker.md)
- **Redefine the machine.** A target is a Mettle value you can print, change and build for. [Bare metal](docs/bare-metal.md)

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
checksum (loop @ line 6): vectorized -> vpsadbw, 32-wide byte sum (AVX2)
```

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
```

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex
```

These install v0.17.0. Live programs, rules, target descriptions and Metal are newer, so until the next release, build them from source with `make -j"$(nproc)"` on Linux or `.\build.bat` on Windows.

[Getting started](docs/getting-started.md) | [Documentation](docs/README.md) | [What does not work yet](docs/known-limitations.md) | [Contributing](CONTRIBUTING.md)

Apache 2.0.
