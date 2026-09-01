# Linker and build pipelines

Which linker runs for each `--build` combination, so a bug report lands in the
right subsystem.

## The pipelines

| Command | Codegen output | Link step | In-tree |
|---------|----------------|-----------|---------|
| `--build` on Linux | ELF `.o` | Internal ELF linker in `src/linker`, falling back to `ld` and then `gcc` | Yes |
| `--build` on Linux with `-l`, `--shared`, or `--export-dynamic` | ELF `.o` | Internal ELF linker only, no fallback | Yes |
| `--build` on Linux with `--link-arg` | ELF `.o` | `gcc -nostartfiles`, because the arguments need driver parsing | No |
| `--build --linker internal` on Windows | COFF `.obj` | Internal PE linker in `src/linker` | Yes |
| `--build --linker gcc` on Windows | COFF `.obj` | `gcc -nostartfiles` | No |
| `--build --linker msvc` on Windows | COFF `.obj` | `link.exe` | No |

`--linker` applies on Windows. On Linux, `--build` always goes through the ELF
object backend.

The Linux path merges the owned runtime objects and the program's own in
`src/linker`, entry `_start`, dead sections dropped. No libc, no C startup
files. It falls back to `ld` and then to the compiler driver, and goes straight
to the driver when `--link-arg` is present, because those arguments are written
for a driver.

A link that names a shared library, emits one, or exports its symbols has no
fallback: `ld` and `gcc` would produce an image whose runtime this compiler
does not own, so the internal linker's failure is reported instead. See
[Shared libraries](shared-libraries.md).

## Triage

A bug that appears only under `--linker internal` points at `src/linker` and
the COFF output of the binary emitter. Internal linking correct and `gcc` or
`msvc` wrong points at command-line parity, startup, or default libraries in
`src/main.c`, which is cheaper to check than relocations.

Capture, for a report:

1. The full compiler and linker output, both streams.
2. The `.obj` or `.o` the compiler produced, from the same flags.
3. The exact `mettle` arguments, and the `gcc` or `link.exe` command line when
   one of those ran.

`objdump -x` or `llvm-readobj` over the object and the final image helps when
symbols or relocations are in question.

## Runtime symbols are overridable defaults

The bundled runtime objects put their symbols in the same flat namespace as
your program. `freestanding.o` alone defines around 330 C names, so any program
or stdlib module defining `strlen`, `exp`, or `malloc` would collide.

The internal linker treats a runtime object's definitions as defaults. A
definition from a program object replaces one from a runtime object, in either
arrival order. Two program definitions of one name remain an error, as do two
runtime definitions. `main.c` and `mtlc_api.c` mark which inputs are runtime
defaults.

Overriding does not reroute the runtime's own calls. A relocation whose own
object also defines the symbol binds locally, before the global table is
consulted, so `freestanding.o` keeps calling its own `strlen` when the program
supplies another.

This is an internal-linker property. Under `--linker gcc` the system linker
sees two strong definitions and rejects the link, which is why the standard
library avoids the C names: `std/conv` exports `cstr_len`.

## Relocations to the internal linker

The object backend records relocations in `binary_emitter_map_relocation_kind`
in [binary_emitter.c](../src/codegen/binary_emitter.c). The internal linker
applies them in `link_apply_relocations` in
[relocation.c](../src/linker/relocation.c).

| Kind | AMD64 COFF type | Width |
|------|-----------------|-------|
| `BINARY_RELOCATION_REL32` (default) | `COFF_RELOC_AMD64_REL32` | 4 |
| `BINARY_RELOCATION_ADDR64` | `COFF_RELOC_AMD64_ADDR64` | 8 |
| `BINARY_RELOCATION_ADDR32NB` | `COFF_RELOC_AMD64_ADDR32NB` | 4 |
| `BINARY_RELOCATION_SECTION_REL32` | `COFF_RELOC_AMD64_SECREL` | 4 |

A COFF relocation type the linker does not support fails with a named error
from `link_apply_relocations`.

## ELF relocations

The ELF writer in [elf_emitter.c](../src/codegen/elf_emitter.c) maps the same
kinds to ELF x86-64 types and leaves resolution to `ld`.

| Kind | ELF x86-64 type | Implicit addend |
|------|-----------------|-----------------|
| `BINARY_RELOCATION_REL32` (default) | `R_X86_64_PC32` | -4 |
| `BINARY_RELOCATION_ADDR64` | `R_X86_64_64` | 0 |

The `-4` addend reproduces the field-end bias COFF `REL32` applies implicitly.
`ADDR32NB` and `SECTION_REL32` are COFF debug-table relocations with no ELF
counterpart, and the writer rejects them.

## See also

- [Runtime model](runtime-model.md)
- [Compilation](compilation.md)
- [C interoperability](c-interop.md)
