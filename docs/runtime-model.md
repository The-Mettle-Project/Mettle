# Owned Runtime Model

Every native Mettle product uses a runtime that this project owns. It does not
link libc, a C startup package, a compiler support library, pthread, musl, UCRT,
MSVCRT, or a C++ runtime.

Mettle still has no garbage collector, async scheduler, thread pool, or hidden
background service. The required runtime is small, but it is real. It owns the
startup path and each basic service that generated code and libmtlc need.

## Required runtime

| Service | Windows x86_64 | Linux x86_64 and AArch64 |
|---|---|---|
| Entry | `mettle_start` | `_start` |
| Arguments | `GetCommandLineA` parser | Initial process stack |
| Exit | `ExitProcess` | `exit` system call |
| Heap | Process heap APIs | Anonymous memory maps |
| Files and console | Kernel32 file APIs | File system calls |
| Threads | Kernel32 thread and wait APIs | `clone` and `futex` system calls |
| Thread local state | Fiber local storage | Owned TLS image and thread pointer setup |
| Sockets | Winsock when requested | Socket system calls |
| Clocks | Kernel32 clocks | Clock system calls |
| Process launch | `CreateProcessA` | `fork`, `execve`, and `wait4` system calls |

The source lives in `src/runtime/freestanding.c`. It does not include a standard
C header. It compiles with freestanding flags and has no unresolved symbol on
Linux. Its Windows object refers only to OS imports.

The runtime exports familiar ABI names such as `malloc`, `calloc`, `memcpy`,
`puts`, `strtod`, `clock_gettime`, and `pthread_create`. Those names do not mean
that a host C library supplies them. Mettle supplies them. The POSIX thread
names exist only as a source compatibility layer over the owned clone and futex
code.

## Startup objects

`src/codegen/binary/startup.c` writes the target startup object. It supports the
Windows x86_64 COFF ABI, the Linux x86_64 ELF ABI, and the Linux AArch64 ELF ABI.
The startup code initializes the owned runtime, passes arguments to `main`, runs
optional diagnostic hooks, and exits through the OS.

`src/runtime/host_startup.c` provides the same entry contract for the reference
compiler and for C programs that embed libmtlc.

## Optional owned code

The linker adds these objects only when a program asks for their feature:

* `crash_handler.o` prints source locations and stack frames.
* `profile.o` records and prints the runtime profile.
* `debug.o` implements interactive debug hooks on Windows.
* `tracy_helpers.o` supplies the local no op Tracy ABI when external Tracy is
  not in use.

These objects use the same owned ABI. They do not add a host runtime.

External Tracy needs a C++ runtime, so `--tracy` fails in owned runtime mode.
Use `--profile-runtime` for the built in profiler.

## Link rules

Windows uses the internal PE linker by default. The GCC fallback passes
`-nostdlib`, `-nostartfiles`, and `-nodefaultlibs`, selects `mettle_start`, and
links only requested OS libraries.

Linux uses `ld` directly when it can. Its GCC fallback uses GCC only as a linker
driver and passes the same three no runtime switches. Every Linux executable is
a static `ET_EXEC` image. `--static` remains as a compatible no op. `--musl`
fails because linking musl would break the owned runtime rule.

## Hard checks

Mettle audits each executable before it reports success.

For PE32+ it reads normal and delayed import tables and rejects names for UCRT,
MSVCRT, VCRuntime, the Microsoft C++ library, libgcc, libstdc++, and
libwinpthread.

For ELF64 it requires `ET_EXEC` and rejects `PT_INTERP` and `PT_DYNAMIC`.

The driver also rejects link arguments that name a C, compiler, or thread
runtime. Build scripts audit the compiler itself. The libmtlc build combines the
whole archive and checks its final external symbol set.

## Target rule

A new native target is not complete until it has all four parts:

1. An owned entry object.
2. An owned service layer for every ABI symbol that code generation can emit.
3. A link path with all default startup and libraries disabled.
4. A format check that rejects hidden runtime dependencies.

This rule applies to the reference compiler, libmtlc embedders, generated
programs, optional diagnostics, and both internal and external linker paths.
