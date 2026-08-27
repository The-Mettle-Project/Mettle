# Contributing to Mettle

Thanks for your interest in Mettle. It is a systems language built from
scratch, compiling straight to 64 bit x86, with no LLVM, no virtual machine and
no garbage collector. This guide covers how to build it, test it, and land a
change.

Plenty of it is unusual, and some of it will surprise you. Ask when something
looks wrong. Often it is.

## Where the change belongs

Both halves of the toolchain live in this repository. The frontend owns the
language: syntax, type checking, memory safety analysis, lowering to IR, the
driver and the standard library. **libmtlc** owns everything from the IR
onwards: the optimizers, code generation, and native PE and ELF linking.

The boundary between them is real and the build enforces it. `make libmtlc`
archives the backend on its own, and the build fails when a frontend symbol has
leaked into it. [Mettle and libmtlc](docs/mettle-and-libmtlc.md) draws the line
in full.

## Ground rules

Mettle emits its own machine code. Instructions are built by hand from the ISA.
Never route code generation through LLVM, Cranelift or GNU `as`, including as a
reference oracle. `ptxas` for the PTX path and QEMU for AArch64 testing are the
only outside tools this project accepts.

Correctness comes first. A miscompile is the worst bug this project can ship.
Any change to lowering or code generation must keep debug and release output in
agreement, and the differential fuzzer below is how you show it.

The compiler stays in C99. Everything under `src/` builds with
`-std=c99 -Wall -Wextra` and compiles clean under GCC and Clang, on Windows and
Linux.

## Repository layout

```
src/lexer/      lexer
src/parser/     parser and AST
src/semantic/   imports, monomorphization, type checking, memory safety
src/ir/         the IR, its optimizers, and AST to IR lowering
src/frontend/   adapters from Mettle types onto the backend's
src/codegen/    code generation: x86, ARM64, PTX, SPIR-V
src/linker/     native PE and ELF linking
src/compiler/   compiler context, ICE reporting
src/debug/      debug info
src/main.c      the compiler driver
src/runtime/    the owned host runtime, plus objects linked into user programs
include/mtlc/   libmtlc's public API
stdlib/         standard library, written in Mettle
tests/          regression tests; run_tests.ps1 on Windows
tools/          ELF tests, benchmarks, fuzz scripts, ML optimizer
examples/       runnable samples and benchmarks
docs/           language and tooling reference
```

The VS Code and JetBrains extensions live in
[MettleMisc](https://github.com/suidvandiewereld/MettleMisc).

## Building the compiler

Everything builds from this checkout. There is nothing to fetch. The build needs
no network.

On Windows, the primary target, build with GCC or Clang through MinGW or LLVM:

```powershell
.\build.bat                 # default: gcc
.\build.bat clang
.\build.bat --skip-tests    # build only
```

`build.bat` starts from a clean `obj\` tree, compiles the backend into
`bin\mtlc.lib`, links `bin\mettle.exe` against it, and bundles `stdlib/` and the
runtime objects into `bin\`. Run it from PowerShell, since `build.bat` under
Bash does nothing on some setups. `--backend-only` stops after the archive.

On Linux or macOS the compiler builds on the host, and code generation still
targets Windows and Linux on x86:

```bash
make -j"$(nproc)"           # bin/mettle + bundled stdlib/ and runtime/
make install                # optional: /usr/local
make clean
```

Both builds use `-O2 -g -fno-omit-frame-pointer`. When you add a `.c` file,
register it in `build.bat` and in the `Makefile`. In the `Makefile`, put it in
`BACKEND_SOURCES` or in `FRONTEND_SOURCES`. Those two lists are the boundary in
executable form.

## Testing

Every change needs to pass the regression suite before it goes up for review.

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
make test                            # crash handler unit test
```

### Differential fuzzer, required for anything that changes generated code

Run the miscompile fuzzer in `tools/fuzz/` over any change touching lowering,
code generation or an optimizer pass. It compiles generated programs at `-O0`
and `--release` and diffs the results. `METTLE_SKIP_PASS` bisects which pass
introduced a divergence. A silent miscompile that escapes review is the failure
this gate exists to catch. The nightly workflow runs the heavy sweep, so run a
local pass on anything that reaches the generated code.

### Complexity budget

Every function under `src/` and `include/` carries a cyclomatic complexity
number: one, plus a count of the branch points in its body. The ceiling is 40.

```bash
make complexity                                 # audit, then gate
python3 tools/ci/complexity_audit.py            # the audit on its own
python3 tools/ci/complexity_audit.py --check    # what CI gates on
```

`tools/ci/complexity-budget.json` pins every function already past the ceiling
at the number it carries today. Those are free to shrink and will fail the gate
if they grow. A new function past the ceiling fails it too. Split the function.
Where the debt is deliberate, record it with `--update-budget` and say why in
the pull request.

The audit prints a second column, `mcc`, which counts a `switch` once instead
of once per `case`. A dispatch table over every opcode scores badly in `cc` and
still reads cleanly on the page. Where `cc` and `mcc` sit close together, the
branching is real.

A breach that reaches `main` opens a GitHub issue carrying the full report, and
that issue closes itself once `main` is back under budget.

## Making changes

### Optimizer and code generation

A recognizer that matches an exact instruction shape rots quietly once an
unrelated pass reorders the IR. Guard each one with an IR match assertion
against its benchmark source, so the build catches the drift.

A loop recognizer must leave `--safe` bookkeeping where it finds it. Ask
`ir_loop_body_is_unclaimable` before replacing a body. A recognizer that claims
a body holding a safety check erases the check along with it, and the mode goes
missing from the hot loops it exists to cover.

Keep the analysis inferring. The memory analyser reaches its conclusions with no
help from the source, and it stays that way. Ownership annotations along the
lines of `@owned` and `@borrow` do not belong in it.

Vectorizers and aggressive rewrites run under `--release`. Prove every
precondition they rest on, such as a loop starting at zero. Use `--explain` and
`--simd-report` to confirm a loop became what you intended, and `--verify` to
catch a pass that changed what the program means.

### Standard library

`stdlib/` is written in Mettle. Windows is the most complete target. Keep the
Linux shims working where they exist, meaning the `.linux` variants,
`std/net_posix` and their kin.

## Code style

Match the surrounding code: same naming, same idiom, same comment density.
Default to no comments. Add one only where the reason behind something is hard
to see. Skip docstrings, skip comments that restate the code, and skip task
references.

Types are explicit. Every `var` and every local `const` states its type.
Induction variables in a `for` over a range and global `const` are the
exceptions.

## Submitting a pull request

Branch off `main`. Pull requests land there.

Keep the change focused and let unrelated cleanups go in their own pull request.
Build clean under GCC and Clang on your platform, run `tests\run_tests.ps1` on
Windows, and run the differential fuzzer for anything touching generated code.

CI runs Windows GCC with the full suite and the fuzz gate, Windows Clang for
compilation, Linux GCC and Clang over the ELF backend, Linux ASan and UBSan,
and the complexity budget. All of them need to be green.

Describe the change, the platforms you tested, and for optimizer or code
generation work, how you established correctness: the fuzzer, `--verify`,
benchmark numbers.

## Reporting bugs

Open an issue with a small `.mettle` reproducer, the exact command line
including `--release` and any flags, the platform and host compiler, what you
expected, and what you got. For a suspected miscompile, say whether it
reproduces at `-O0`, at `--release`, or at both. That alone narrows it down a
long way.

A bug report is a contribution. So is telling us the build fought you, or that
a page of this guide made no sense.

## License

By contributing you agree that your contributions are licensed under Apache 2.0,
the same license as the project. See [LICENSE](LICENSE).
