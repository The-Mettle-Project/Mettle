# Linker and Build Pipelines

This note maps which linker runs for each `--build` combination so bug reports land in the right subsystem.

## Pipelines

| Command | Codegen output | Link step | In-tree `src/linker`? |
| --- | --- | --- | --- |
| `--build` on Linux | ELF `.o` | `mettle_link_elf_executable` invokes the platform C toolchain | No |
| `--build --linker internal` on Windows | COFF `.obj` | `write_internal_startup_object` + `mettle_link_internal` -> PE | Yes |
| `--build --linker gcc` on Windows | COFF `.obj` | `mettle_link_object_with_gcc` (`gcc -nostartfiles`) | No |
| `--build --linker msvc` on Windows | COFF `.obj` | `mettle_link_object_with_link` (`link.exe`) | No |

On Linux, `--build` always uses the ELF object backend. The `--linker` modes apply on Windows only.

**Regression triage:** If a bug appears only on `--linker internal`, prioritize `src/linker` and the binary emitter's COFF output. If internal linking is correct but `--linker gcc` or `--linker msvc` is wrong, suspect command-line parity, startup, or default libraries in `src/main.c` before deep-diving relocations.

## Runtime Symbols Are Overridable Defaults

The bundled runtime objects (`freestanding.o` above all, plus `crash_handler.o`,
`atomics.o`, `profile.o`, `debug.o` and the Tracy helpers) put their symbols into
the same flat namespace as the program. `freestanding.o` alone defines around 330
C names, so without special handling any Mettle program or stdlib module defining
`strlen`, `exp`, `malloc` and so on would fail the link on a duplicate symbol.

The internal linker therefore treats those objects' definitions as *defaults*: a
definition from a program object replaces one from a runtime object, in either
arrival order. Two program-object definitions of the same name are still an
error, as are two runtime definitions. `main.c` and `mtlc_api.c` mark the runtime
inputs via `LinkResolutionOptions.object_is_runtime_default`.

This does not reroute the runtime's own calls. A relocation whose own object also
defines the symbol is bound locally in `relocation.c`, before the global symbol
table is consulted, so `freestanding.o` keeps calling its own `strlen` even when
the program supplies one.

Note this applies to `--linker internal` only. Under `--linker gcc` the system
linker sees two strong definitions and still rejects the link, so the standard
library avoids the C names outright (`std/conv` exports `cstr_len`, not
`strlen`).

## Artifacts To Capture

1. Full compiler/link stdout and stderr.
2. Compiler-produced `.obj` / `.o` using the same flags as the failure.
3. Exact `mettle` arguments and, if relevant, the `gcc` / `link.exe` command line from `src/main.c`.

Optional: `objdump -x` / `llvm-readobj` on the object and final PE for symbol and relocation comparison.

## Binary Emitter To Internal Linker Relocation Map

The object backend records relocations in [`binary_emitter_map_relocation_kind`](../src/codegen/binary_emitter.c). The internal linker applies them in [`link_apply_relocations`](../src/linker/relocation.c):

| `BinaryRelocationKind` | AMD64 COFF type | Width |
| --- | --- | --- |
| `BINARY_RELOCATION_REL32` (default) | `COFF_RELOC_AMD64_REL32` | 4 |
| `BINARY_RELOCATION_ADDR64` | `COFF_RELOC_AMD64_ADDR64` | 8 |
| `BINARY_RELOCATION_ADDR32NB` | `COFF_RELOC_AMD64_ADDR32NB` | 4 |
| `BINARY_RELOCATION_SECTION_REL32` | `COFF_RELOC_AMD64_SECREL` | 4 |

Unsupported COFF relocation types in a merged input fail with a clear error from `link_apply_relocations`.

## ELF Relocations

The ELF writer in [`elf_emitter.c`](../src/codegen/elf_emitter.c) maps the same `BinaryRelocationKind` values to ELF x86-64 types, then leaves resolution to the system linker:

| `BinaryRelocationKind` | ELF x86-64 type | Implicit addend |
| --- | --- | --- |
| `BINARY_RELOCATION_REL32` (default) | `R_X86_64_PC32` | -4 |
| `BINARY_RELOCATION_ADDR64` | `R_X86_64_64` | 0 |

The `-4` addend on `R_X86_64_PC32` reproduces the field-end bias that COFF `REL32` applies implicitly. `ADDR32NB` and `SECTION_REL32` are COFF debug-table relocations with no ELF analogue and are rejected.
