# libmtlc internals

A map for people changing the backend, not just calling it. Nothing here is
API; everything here can change without notice.

## Source layout

```
include/mtlc/            public API headers (the only thing consumers include)
src/
  ir/
    ir.c/.h              IR core: program/function/instruction/operand,
                         module type registry + symbol table, CFG rebuild
    mtlc_type.c          MtlcType queries + canonical scalar/pointer constructors
    ir_optimize*.c       classical optimizer entry + pass driver
    optimizer/           pass implementations (see optimizer/README.md)
    ir_interp.c          reference interpreter (translation validation, PGO,
                         comptime execution)
    ir_comptime.c        compile-time test/trace workflows on the interpreter
    ml_gnn.c, ml_opt.c   GNN inference + ML-opt pass and validation gate
    ir_pgo.c             zero-run PGO (interpret main during the compile)
    ir_verify.c          per-pass translation validation harness
  codegen/
    code_generator.c/.h  x86-64 driver state (no frontend types anywhere)
    binary_emitter.c     object-file model: sections/symbols/relocations,
                         COFF and ELF writers
    binary/              x86-64 encoding: MIR lowering + encoder, ABI,
                         peephole, globals, prologue/epilogue, startup object,
                         profile tables; arm64_*.c: the AArch64 path
    ptx_emitter.c        IR -> PTX text
    spirv_emitter.c      IR -> SPIR-V binary
  linker/                symbol resolution + PE image emission (+ COFF reader)
  error/error_reporter.c frontend-neutral diagnostics renderer
  compiler/              compiler context (thread-local session state), ICE report
  common.c/.h            shared helpers; MTLC_THREAD_LOCAL
  mtlc_api.c             public API implementation (context/module/pipeline)
  mtlc_build.c           public IR builder implementation
  mtlc_lib_fallbacks.c   self-containment fallback symbols (see below)
```

Everything above is archived into `bin/mtlc.lib` / `bin/libmtlc.a`. One
subtlety: `src/ir/` also contains the Mettle lowering TUs (`ir_lowering.c`,
`ir_lower_*.c`); those are frontend-side and are deliberately **not** archived
(the build scripts list the IR core explicitly to exclude them). The Mettle
reference frontend (`src/lexer`, `src/parser`, `src/semantic`,
`src/frontend/`, `src/main.c`, `src/error/error_explain.c`, plus those
lowering TUs) links against the archive to form `bin/mettle`.

## How the archive stays self-contained

Three mechanisms, all enforced by the suite:

1. **No frontend includes.** No TU in the archive includes a
   parser/semantic/AST header. The diagnostics reporter needs only
   `src/source_location.h` (a dependency-free header shared with the frontend).
2. **Fallback member.** Two backend TUs reference symbols the reference driver
   normally provides (`string_is_interned` from the lexer's intern table, and
   the Windows crash reporter's exception namer). `mtlc_lib_fallbacks.c` is a
   dedicated archive member with plain (strong) default definitions. Archive
   semantics make this correct on both toolchains: the member is pulled only
   when the symbol is otherwise undefined, and the driver links its own
   definitions as direct objects ahead of the archive, so no duplicate ever
   occurs. (A weak-symbol approach does not work here: on PE/COFF a weak-only
   archive member is never extracted to satisfy a reference.)
3. **The audit gate.** `libmtlc_selfcontained` in `tests/run_tests.ps1`
   computes the archive's external symbol closure with `nm` (undefined minus
   defined across all members) and fails if anything in it matches the
   project's naming conventions. The current build also combines the whole
   archive. Linux permits only the linker supplied GOT marker. Windows permits
   only explicit OS imports.

Two more gates prove the positive direction: `calc_frontend` builds and runs a
non-Mettle frontend against the archive alone, and `public_api` drives the
full builder surface to all four targets.

## Ownership and threading rules

- **Append clones.** `ir_function_append_instruction` deep-clones every
  operand and duplicates `text`. Builders and lowering passes construct
  instructions on the stack with borrowed operand aliases and discard them
  after appending. Nothing retains caller memory.
- **Types are borrowed, forever.** The module type registry and symbol table
  store `MtlcType *` by reference and never free them. Canonical descriptors
  are immortal by construction; the reference frontend's translation arena
  outlives its compile.
- **Per-compile mutable state is thread-local.** The compiler session context
  uses owned FLS or native TLS; the explain/annotate diagnostic sinks and the
  pointer-type intern cache use `MTLC_THREAD_LOCAL` (`common.h`). The rule for
  new code: a mutable file-scope variable in the archive must be thread-local
  or it is a bug. This is what lets two frontends compile concurrently on
  separate threads.
- **Owned thread local storage.** The Windows runtime implements the compiler
  emulated TLS ABI with FLS. Linux startup and clone paths install each thread
  pointer. Frontends do not select another thread runtime.

## The x86-64 path in one paragraph

`code_generator_generate_program` walks the module: functions from the IR
program list, globals/externs from the module symbol table. Eligible functions
lower through MIR (a register-allocating pipeline with liveness, allocation,
peephole, and encoding stages); the rest take the baseline stack-slot
generator. Types resolve through the module type registry by name;
per-instruction result types ride on the IR (`value_type`, baked at lowering).
Output accumulates in `binary_emitter` sections with relocations and is
serialized as COFF or ELF. The `--annotate-asm` and `--explain` machinery
observes codegen decisions through thread-local capture sinks.

## Adding things

**A builder capability** is additive: new function in `include/mtlc/build.h`,
implementation in `src/mtlc_build.c` emitting existing IR ops (mirror the
instruction shapes the reference lowering emits; `ir.md` documents them), a
case in `tests/public_api_test.c`, and a note in the docs. If codegen must
learn a new shape, that is a backend change first.

**An IR instruction** touches: the opcode enum and dump/interp support in
`src/ir/`, every consumer that must handle it (x86-64 MIR and baseline, and
explicit rejection or support in arm64/ptx/spirv), the optimizer's feature
scan if passes must see it, and the differential fuzzer's generator if it is
reachable from source.

**A target** follows the PTX/SPIR-V pattern: one emitter TU consuming
`IRProgram` + the module tables with no frontend knowledge, an `MtlcArch`
case in `mtlc_emit`, a driver flag if the reference frontend should expose it,
and a suite gate that validates output structurally plus an external validator
when one exists.

## Debugging the backend

- `METTLE_SKIP_PASS=<names>` bisects optimizer miscompiles per pass.
- `--verify` runs translation validation per pass per function.
- `tools/fuzz/` is the debug-vs-release differential fuzzer; any
  codegen/optimizer change runs it.
- `--dump-ir` writes the IR after lowering (and after optimization with `-O`)
  as text next to the output file.
- `--explain` / `--annotate-asm` show what the optimizer and encoder decided
  and why, per loop/call/function.
