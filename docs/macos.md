# macOS and Metal

Mettle builds programs for Macs with Apple silicon or Intel processors, and
runs GPU kernels on Apple GPUs through Metal. This page covers building the
compiler on a Mac, building programs there or from another machine, and what
is different about the platform.

## Building Mettle on a Mac

```bash
make
make install PREFIX=$HOME/.local
```

On macOS the Makefile uses `cc` and builds the hosted runtime: every operating
system service comes from libSystem through its POSIX interface, because
macOS has no stable system-call interface. The compiler is an ordinary
dynamically linked program, and so is everything it builds.

## Building programs

```bash
mettle --build app.mettle -o app
./app
```

`--build` compiles to a Mach-O object and links it with `cc`, which runs
Apple's linker. Executables are position independent, and on Apple silicon the
linker signs the result ad hoc, which macOS requires before it will run
anything. Unused runtime objects are left out; the program's own object is
kept whole. `--shared` produces a
`.dylib`; `--soname` sets its install name. `--static` is refused, because
libSystem is always linked dynamically.

From Windows or Linux, `--target` produces a Mac object to link on a Mac:

```bash
mettle --target aarch64-macos --emit-obj app.mettle -o app.o
mettle --target x86_64-macos --emit-obj app.mettle -o app.o
```

`arm64-apple-macos14`, `aarch64-apple-darwin` and the other Apple spellings
are accepted.

## The standard library

A `std/` import takes a `.macos.mettle` file when one exists and the Linux
file otherwise. The macOS files carry the values that differ: `mmap` flags,
the page size (16 KB on Apple silicon, read from `getpagesize`), socket
options, the `sockaddr_in` length byte, `SO_NOSIGPIPE` in place of
`MSG_NOSIGNAL`, and `CLOCK_MONOTONIC`. Import guards accept `macos`, and
`posix` for Linux or macOS. See [Modules](modules.md).

## Metal

On a Mac, `std/gpu` runs launches through Metal. Kernels compile to Metal
Shading Language with `--emit-metal`, and the same host program that drives
CUDA drives Metal:

```bash
mettle -O --emit-metal kernels.mettle -o kernels.metal \
  --emit-kernel-decls=kernel_decls.mettle
mettle --build host.mettle -o host
```

`gpu_open_kernels("kernels")` loads `kernels.metal` here and `kernels.ptx`
under CUDA. `examples/gpu_inference` is a transformer decode step that runs
unchanged on both. [GPU offload](gpu.md#metal-apple-gpu-target) covers what a
kernel becomes, what Metal refuses, how launches are batched (`gpu_flush`
starts the GPU on what is queued, `gpu_barrier_off`/`gpu_barrier_on` let
independent launches overlap), and the environment variables:

| Variable | Effect |
|---|---|
| `METTLE_METAL_SYNC=1` | Wait after every launch, to find the one that failed. |
| `METTLE_METAL_VERSION=3.1` | The Metal language version the runtime compiles with; 3.2 by default. |

`gpu_print` and a failed `gpu_assert` write through `os_log`, so on a Mac
their output lands in the system log rather than on stdout; read it with
`log stream --predicate 'process == "app"'` or in Console.

Mettle needs a Metal 3 GPU, because kernels address device memory through
64-bit GPU addresses: any Apple silicon Mac, or an Intel Mac with a recent
AMD GPU. Metal 3.2, the default language version, needs macOS 15. For macOS
14, emit with `--metal-version=3.1` and run with `METTLE_METAL_VERSION=3.1`;
kernel printing and ordered device-scope atomics need 3.2 and are refused at
3.1.

## Calling convention

Mach-O symbols carry a leading underscore, which Mettle adds. On arm64, Darwin
differs from Linux in three ways that Mettle handles:

- Arguments past the eighth integer or float register are packed on the stack
  at their natural size; Linux gives each an 8-byte slot.
- `x18` belongs to the operating system. Mettle never allocated it.
- Variadic arguments go on the stack. Mettle's own variadic functions take a
  slice, so this matters only for C functions, and the standard library calls
  none.

A C function that returns `int` leaves the upper half of the register
undefined, so declare its result `int32`, not `int64`.

A reference to a symbol from a dynamic library goes through the global offset
table. On arm64 the object writer rewrites every address it builds for an
undefined symbol into a table load. On x86-64 it does the same for `lea`, but
an instruction that reads a library's variable directly cannot be rewritten,
so x86-64 code cannot read data that lives in a dynamic library; call a
function that returns it.

## What has run where

| Part | How it is checked |
|---|---|
| Mach-O objects | `tests/macho_object_test.c` resolves random objects under Apple's relocation rules and under ELF rules and requires the same bytes, and checks real compiled objects; 23 planted defects were each caught. |
| Darwin argument layout | `tests/arm64_encode_test.c`, against Apple's documented examples. |
| Metal kernels | A strict interpreter for the emitted MSL runs 25 contract kernels and a Mettle host program end to end; Apple's `metal` compiler checks every emitted file when it is installed. |
| Hosted runtime | 99 programs give the same output on Linux built hosted and built freestanding. |
| A real Mac | An M4 Pro under macOS 26.6, with the Command Line Tools and no Xcode: the Darwin build, `--build` through `cc`, the hello, thread and `@parallel` programs (the last gives the same output as on Windows), `--gpu-info`, the 25 contracts, the interpreter's selftests on the GPU, `tests/metal/metal_host_main.mettle`, and `examples/gpu_inference` (every stage within its float32 bound, worst at 10% of it). |
