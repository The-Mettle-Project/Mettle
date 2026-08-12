# Contributing to Mettle

Thanks for your interest in Mettle, a from-scratch systems language that compiles
straight to native x86-64. No LLVM, no VM, no managed runtime. This guide covers how
to build, test, and land changes.

## Where the change belongs

This repository is the **frontend**: the language, the type system, the memory safety
analysis, IR lowering, the driver, and the standard library. The **backend** is
[libmtlc](https://github.com/The-Mettle-Project/libmtlc): the IR itself, the
optimizers, code generation, and linking.

A change to an optimizer pass, an instruction encoding, the register allocator, or the
PE/ELF writers goes **upstream to libmtlc**, not here. A change to syntax, type
checking, diagnostics, lowering, the driver, or the stdlib goes here.
[docs/mettle-and-libmtlc.md](docs/mettle-and-libmtlc.md) has the full boundary and the
sync workflow.

## Ground rules

The whole point of Mettle is that it emits machine code itself. Never route codegen
through LLVM, Cranelift, or GNU `as`, not even as a reference oracle. Instructions are
hand-built from the ISA. `ptxas` (for the PTX/GPU path) and QEMU (for AArch64 testing)
are the only accepted external tools.

Correctness comes first. A miscompile is the worst possible bug, so any change to
lowering, or to the backend upstream, must keep debug and release output in agreement
(see the differential fuzzer below).

The compiler stays in C99. Everything under `src/` builds with `-std=c99 -Wall -Wextra`
and must compile clean under both GCC and Clang, on Windows and Linux.

## Repository layout

```
src/lexer/      lexer
src/parser/     parser and AST
src/semantic/   imports, monomorphization, type checking, memory safety
src/ir/         the IR, its optimizers, and AST to IR lowering
src/frontend/   adapters from Mettle types onto the backend's
src/codegen/    code generation: x86-64, ARM64, PTX, SPIR-V
src/linker/     native PE and ELF linking
src/compiler/   compiler context, ICE reporting
src/debug/      debug info
src/main.c      the compiler driver
src/runtime/    the owned host runtime, plus helper objects linked into user programs
include/mtlc/   libmtlc's public API
stdlib/         standard library (.mettle modules)
tests/          regression tests; run_tests.ps1 on Windows
tools/          ELF tests, benchmarks, fuzz scripts, ML optimizer
examples/       runnable samples and benchmarks
docs/           language and tooling reference
```

The frontend/backend boundary inside `src/` is described in
[docs/mettle-and-libmtlc.md](docs/mettle-and-libmtlc.md). It is not cosmetic:
`make libmtlc` builds the backend half on its own and the build fails if any
frontend symbol has leaked into it.

The VS Code and JetBrains extensions live in a separate project,
[MettleMisc](https://github.com/The-Mettle-Project/MettleMisc).

## Building the compiler

Everything builds from this checkout. There is nothing to fetch and the build
needs no network.

On Windows, the primary target, build with GCC or Clang via MinGW-w64 or LLVM:

```powershell
.\build.bat                 # default: gcc
.\build.bat clang
.\build.bat --skip-tests    # build only
```

`build.bat` starts from a clean `obj\` tree, compiles the backend into
`bin\mtlc.lib`, links `bin\mettle.exe` against it, and bundles `stdlib/` and the
runtime objects into `bin\`. Run it from PowerShell (`build.bat` under Bash
no-ops on some setups). `--backend-only` stops after the archive.

On Linux or macOS the compiler builds on the host, and codegen still targets x86-64
Windows and Linux:

```bash
make -j"$(nproc)"           # bin/mettle + bundled stdlib/ and runtime/
make install                # optional: /usr/local
make clean
```

Both builds use `-O2 -g -fno-omit-frame-pointer`. When you add a new `.c` file,
register it in `build.bat` and the `Makefile` — and in the `Makefile`, decide
whether it joins `BACKEND_SOURCES` or `FRONTEND_SOURCES`. Those two lists are the
frontend/backend boundary in executable form.

## Testing

Every change must pass the regression suite before it goes up for review.

On Windows, which is what CI runs in full:

```powershell
.\build.bat                          # builds + runs tests
.\tests\run_tests.ps1                # tests against the current bin\mettle.exe
.\tests\run_tests.ps1 -BuildCompiler # rebuild first, then test
```

On Linux, exercising the ELF backend:

```bash
make -j"$(nproc)"
bash tools/test-elf-native.sh
make test                            # crash-handler unit test
```

### Differential fuzzer (required for anything that changes generated code)

Any change touching lowering, codegen or an optimizer pass must be run through the
debug-vs-release miscompile fuzzer in `tools/fuzz/`. It compiles generated programs at
`-O0` and `--release` and diffs the results; `METTLE_SKIP_PASS` bisects which pass
introduced a divergence. A single silent miscompile that escapes review is worse than a
missed optimization. The nightly workflow runs the heavy sweep; run a local pass on
anything that changes generated code.

## Making changes

### Optimizer / codegen

New recognizers and rewrites are easy to write and easy to silently rot when an
unrelated pass reorders IR, so guard exact-shape recognizers with an IR-match assertion
on the benchmark source; that way drift is caught, not discovered later. Prefer
inference over annotations, since the borrow checker and capability analysis are pure
inference by design and should not grow `@owned` / `@borrow`-style ownership
annotations. Vectorizers and aggressive rewrites run under `--release`, so prove their
soundness preconditions (for example, that the loop starts at zero) rather than assuming
them. Use `--explain` and `--simd-report` to confirm a loop actually became what you
intended, and `--verify` (per-pass translation validation) to catch a pass that changed
program meaning.

### Standard library

`stdlib/` is written in Mettle. Windows is the most complete target; keep the Linux
shims (`.linux` variants, `std/net_posix`, and similar) working where they exist.

## Code style

Match the surrounding code: same naming, same idiom, same comment density. Comment
minimally, defaulting to zero comments and adding one only to explain a non-obvious why,
with no docstrings, no what-comments, and no task references. Types are explicit: `var`
and function-local `const` need explicit types and nothing is inferred, though
range-`for` induction variables and global `const` are exempt.

## Submitting a pull request

Branch off `development`, which is where PRs land; `main` is the release branch. Keep the
change focused, and let unrelated cleanups go in their own PR. Build clean under both GCC
and Clang on your platform, run `tests\run_tests.ps1` on Windows, and for codegen changes
run the differential fuzzer. When you open the PR, CI runs Windows GCC (full suite plus
fuzz gate), Windows Clang (compile-only), Linux GCC/Clang (ELF backend), and Linux
ASan/UBSan, and every job must be green. Describe the change, the platforms you tested,
and, for optimizer or codegen work, how you verified correctness (fuzzer, `--verify`,
benchmark numbers).

## Reporting bugs

Open an issue with a minimal `.mettle` reproducer, the exact command line (including
`--release` and any flags), the platform and host compiler, and expected vs actual
output. For a suspected miscompile, note whether it reproduces at `-O0`, `--release`, or
both. That alone narrows it down enormously.

## License

By contributing you agree that your contributions are licensed under Apache-2.0, the
same license as the project. See [LICENSE](LICENSE).
