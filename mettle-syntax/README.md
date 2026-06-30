# Mettle Language Support

VS Code and Cursor support for **Mettle** (`.mettle`) files.

## Features

- TextMate syntax highlighting for current Mettle syntax.
- Nested block comments and line comments.
- Bracket matching, auto-pairs, folding, and indentation rules.
- Snippets for functions, imports, structs, enums, traits, loops, `defer`, `errdefer`, and `match`.
- **Navigation**: go to definition (cross-file, following imports), find all references, rename symbol, outline/breadcrumbs, and workspace symbol search (`Ctrl+T`).
- **Completion**: visible functions/structs/enums/globals across the import closure, locals and parameters, struct fields and methods after `.`/`->`, decorators after `@`, and import path completion inside `import "..."`.
- **Signature help**: parameter hints with the declaration's doc comment while typing a call.
- **Inlay hints**: parameter names at call sites (literal arguments by default).
- **CodeLens**: `Run`/`Build`/`Optimization Report` above `main`, and per-function loop statistics (`3 loops vectorized · 1 scalar`) while the report panel is open.
- Clickable import paths (document links to the resolved module).
- Rich hover documentation for Mettle keywords, primitive types, strings, imports, traits, generics, defers, control flow, and allocation.
- Contextual hovers for import paths, including best-effort resolution against the workspace, stdlib, and configured include paths.
- Same-file declaration hovers for functions, globals, structs, enums, traits, methods, and impls.
- Fast editor diagnostics for lexer-level mistakes.
- Optional compiler-backed diagnostics for real semantic errors.
- **Optimization Report panel**: an interactive view of the compiler's `--explain` report, with an **Optimizations** tab (vectorization, inlining, verified fixes) and a **Memory** tab (use-after-free, leaks, dangling borrows, and more, with fixes).
- Syntax highlighting and click-to-source links for `.explain.txt` report sidecars.
- **Source-level debugger** (F5): breakpoints, stepping, call stack, typed locals, hover evaluation, and live variable editing -- see the Debugging section.
- Commands:
  - `Mettle: Run File` (also a play icon in the editor title bar)
  - `Mettle: Debug File`
  - `Mettle: Build File`
  - `Mettle: Lint Active File`
  - `Mettle: Clear Diagnostics`
  - `Mettle: Show Output`
  - `Mettle: Show Optimization Report`

## Debugging

Press F5 in a Mettle file (or click the `Debug` CodeLens above `main`) for full source-level debugging on Windows -- no gdb, no PDBs, no external debugger:

- Breakpoints (set/remove while running or paused), pause, and stop-on-entry.
- **Conditional breakpoints** (`i == 4`, `total > 100`, `flag != true`), **hit counts**, and **logpoints** (`value is {box.min.x}` prints to the debug console without stopping).
- Step over / into / out at source-line granularity.
- Call stack with function names and source locations.
- Locals and parameters per frame with typed, rendered values: integers by signedness and width, floats, `bool`, pointers (with a printable-byte preview), `string` (contents + length).
- **Structs, arrays, and pointers expand** in the Variables pane: struct fields (nested, any depth), array elements, and struct pointers auto-dereference to their fields. The compiler embeds full struct layouts in the debug build.
- Hover/Watch/Debug-Console evaluation of variable **paths**: `box.min.x`, `grid[2]`, `pp.y` (pointers auto-dereference).
- **Set Value works for real, anywhere in the tree**: variables -- including individual struct fields and array elements -- are written through live pointers, so editing `i` or `box.max.x` genuinely changes what the program does next.
- **Crash interception**: a hardware fault (access violation, division by zero, ...) stops the debugger AT the faulting source line with the full call stack and every variable inspectable. Continue hands the fault to normal crash handling.

How it works: the launch compiles the program with `--debug-hooks` (an unoptimized debug build) which instruments every function with entry/exit/line hooks and per-variable registrations, then talks to the running process over a named pipe. With no debugger attached, an instrumented binary runs normally -- each hook is a single early-out branch.

**Hand-written kernel objects link automatically**: if the program declares `extern fn ... = "symbol"` and a sibling `.o`/`.obj` defines that symbol (e.g. `llm_kernels.o` next to `engine.mettle`), the build passes it to the linker -- for F5 and for `Mettle: Run File` alike. No launch configuration needed.

`launch.json` attributes (type `mettle`): `program`, `stopOnEntry`, `args`, `cwd`, `compilerArgs`, `console` (`internalConsole` default; `integratedTerminal` runs the program in a terminal so interactive stdin -- like the LLM engine's prompt -- works while debugging). No `launch.json` is needed for the default F5-on-active-file flow.

Current limits: the main thread is debugged (worker threads run freely); evaluate accepts variable paths like `box.min.x`, with arithmetic expressions still on the roadmap; the compiler's own software traps (e.g. null-pointer checks) exit before the exception handler can intercept them, so the faults caught are hardware ones.

## Navigation and IntelliSense

All language understanding comes from a masked-source indexer (comments and strings blanked, brace depth tracked) that scans the active file plus everything reachable through `import` -- the same search order the compiler uses (stdlib for `std/`, the importing file's directory, the workspace root, then configured include paths). Modules are cached by file mtime; the open buffer is indexed live, so navigation works on unsaved edits and on code the compiler would currently reject.

Module visibility follows the compiler's rule: once an imported module exports anything, only its exported declarations are offered.

Rename and find-references operate on word-boundary occurrences across the import closure (comments and strings excluded). Rename refuses keywords and built-in types.

`Mettle: Run File` builds the active file with the discovered compiler (`--release` by default) into an executable next to the source and runs it in the shared `Mettle` terminal. `Mettle: Build File` stops after the build.

| Setting | Default | Meaning |
| --- | --- | --- |
| `mettle.hints.parameterNames` | `off` | Parameter-name inlay hints: `off`, `literals` (only literal args), or `all`. VS Code renders inlay hints into the minimap at an unscaled size, so enabling this adds oversized text there. |
| `mettle.codeLens.enabled` | `true` | Run/Build/report CodeLens and per-function loop stats. |
| `mettle.run.release` | `true` | Pass `--release` for Run/Build File. |
| `mettle.run.extraArgs` | `[]` | Extra compiler arguments for Run/Build File. |

## Optimization Report Panel

`Mettle: Show Optimization Report` (also a graph icon in the editor title bar for `.mettle` files) compiles the active file with `--release --explain` and renders the report beside your code.

**One-click verified fixes.** The compiler's `verified:` fix suggestions are not guesses: each was applied to an internal clone and re-accepted by the optimizer itself. The extension turns the mechanical ones into source edits:

- accumulator retypes (`var s: int32` -> `int64` for int32/byte sums, including the `(int64)` cast the byte-sum kernel requires),
- removing a blocking `@noinline`,
- marking a refused callee `@inline`.

Apply them from the card's `Apply fix` button, the `Apply all` banner, or the editor lightbulb (Quick Fix) on the loop line. Applying saves the file, which recompiles the report; the remark then shows the loop as vectorized.

**What changed since the last compile.** The compiler keeps a baseline of its decisions per output and leads each report with the diff: loops that newly vectorize, calls that newly inline -- and **regressions**, in red, with the reason ("loop @ 19: REGRESSED -- was vectorized, now scalar: each iteration calls `scale`..."). Save the file, and the panel's top section tells you what your edit just did to the optimizer. Performance regressions surface at compile time, before any benchmark runs. Entities are matched by position within their function, so ordinary edits that shift line numbers don't raise false alarms.

The panel parses the compiler's `--explain-json` sidecar (machine-readable, schema-versioned), and scalar loops nested two or more levels deep carry a `nest depth N` chip -- the deepest loops are the ones worth fixing first.

**A Memory tab.** Alongside the optimization view, a second tab surfaces the compiler's compile-time memory analysis from the same `--explain-json` sidecar: use-after-free, double free, leaks, dangling borrows (a borrow that outlives its scope, or an interior pointer used after a `realloc`/`free`), null dereference, and out-of-bounds. Each diagnostic shows its severity, headline, the analyzer's suggested fix, and jumps to the source line on click. A badge on the tab counts the issues (red for errors, gold for warnings); a clean file says so. Because the analyzer speaks only when it can prove the bug, everything in the tab is real -- there is no false-positive noise to triage.

The panel itself:

- A dashboard: a stacked loop bar (vectorized / fix-ready / scalar), instruction-weighted backend coverage, and inlining totals.
- Remarks grouped by function with per-function rollup pills; cards carry the optimizer's reason, fix, and `verified:` proof; click to jump to the line.
- Filters (`Needs attention`, `Vectorized`, `Fixes`, `Calls`) plus free-text search by function, kernel, or reason.
- The backend section groups spill-everything fallbacks by cause, largest functions first, with the consequence spelled out (a function containing a SIMD kernel still runs the kernel at full vector speed).
- While the panel is open: loop lines get end-of-line annotations (`vectorized -> vfmadd231ps ...`, `fix available: ...`, or the refusal reason) with full detail on hover, plus scrollbar marks. A status bar item summarizes the file (`12/14 vec, 2 fixes`).
- The panel refreshes when you save the file (`mettle.explain.refreshOnSave`). While the compiler runs, the report stays visible (dimmed) and the Mettle mark in the header pulses; on a cold open a centered mark shows the same pulse. Animations honor `prefers-reduced-motion`.

Large terminal builds write the report to `<output-stem>.explain.txt` next to the binary; opening one of those files gets report highlighting, and `@ line N` locations are clickable links into the source.

| Setting | Default | Meaning |
| --- | --- | --- |
| `mettle.explain.inlineHints` | `true` | Annotate loop lines with their vectorization outcome while the panel is open. |
| `mettle.explain.refreshOnSave` | `true` | Recompile and refresh the panel when the source file is saved. |
| `mettle.explain.timeoutMs` | `30000` | Timeout for one `--release --explain` compile. |

## Compiler Diagnostics

The extension can run `mettle` on open/save and show compiler errors in the Problems panel.

Compiler discovery order:

1. `mettle.linter.compilerPath`, if set.
2. `bin/mettle.exe` or `bin/mettle` found by walking up from the current file.
3. Workspace `bin/mettle.exe`, `bin/mettle`, `mettle.exe`, or `mettle`.
4. `mettle.exe` or `mettle` on `PATH`.

Settings:

| Setting | Default | Meaning |
| --- | --- | --- |
| `mettle.linter.compilerEnabled` | `true` | Run compiler diagnostics. |
| `mettle.linter.compilerPath` | `""` | Absolute path, or path relative to the workspace. |
| `mettle.linter.stdlibPath` | `""` | Optional `--stdlib` override. |
| `mettle.linter.extraIncludePaths` | `[]` | Additional `-I` include directories. |
| `mettle.linter.compilerTimeoutMs` | `10000` | Timeout for one compiler diagnostics run. |

## Language Coverage

The grammar is aligned with the current compiler surface:

- Declarations: `import`, `import_str`, `extern`, `export`, `var`, `fn`, `struct`, `enum`, `method`, `trait`, `impl`, `where`.
- Control flow: `if`, `else`, `while`, `for`, `switch`, `match`, `case`, `default`, `break`, `continue`, `return`, `defer`, `errdefer`.
- Types: integer and float primitives, `string`, `cstring`, `bool`, `void`, `fn(...) -> ...`, and user types.
- Literals: decimal, hex, binary, float, strings, and character literals.
- Operators: assignment, compound assignment, comparisons, logical operators, bitwise operators, pointer/member access, and casts.
- Inline `asm { ... }` blocks with basic x86 mnemonic/register highlighting.

## Hover Coverage

Hover cards are designed as compact reference docs. They include syntax examples, gotchas, and the rules that tend to matter while coding:

- Module behavior for `import`, `import_str`, `export`, and `extern`.
- Control-flow semantics for `if`, loops, `switch`, `match`, `break`, `continue`, and `return`.
- Cleanup behavior for `defer` and `errdefer`, including LIFO ordering and error-return convention.
- Type notes for primitives, `string`, `cstring`, `fn`, pointers, structs, enums, traits, `impl`, and `where`.
- Common standard-library and C interop calls from `std/io`, `std/mem`, `std/conv`, `std/process`, and `std/system`.
- Compile-time helpers such as `sizeof`, `alignof`, and `static_assert`.

When hovering a quoted import path, the extension tries to show the resolved file. It uses the current file directory, workspace root, `mettle.linter.stdlibPath`, discovered `stdlib/`, and `mettle.linter.extraIncludePaths`.

## Local Installation

### Install From Folder

1. Open the Command Palette.
2. Run `Developer: Install Extension from Location...`.
3. Pick this `mettle-syntax` directory.
4. Reload the editor window.

### Development Symlink

```powershell
New-Item -ItemType Junction -Path "$env:USERPROFILE\.vscode\extensions\mettle-syntax" -Target "$PWD\mettle-syntax"
```

## Validation

Run the extension self-check:

```powershell
cd mettle-syntax
npm run check
```

The check parses all JSON contribution files, verifies contributed paths exist, verifies command registrations, and guards against known README/package mojibake.

## Minimal Program

```mettle
fn main() -> int32 {
  return 0;
}
```

See the repository `docs/` directory for the full language reference.
