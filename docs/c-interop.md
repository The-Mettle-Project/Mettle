# C Interoperability

Mettle can call C functions and access C globals, but Windows programs should prefer `std/win32`, `std/thread`, and `std/net` for common OS APIs. Those modules keep Win32 declarations in the standard library and let the internal PE linker resolve common DLL exports directly.

## Calling C Functions

Declare C functions with `extern fn`. Use the `= "symbol"` suffix to specify the C link name when it differs from the Mettle name. Parameters and return types must match the target ABI. On Windows, the Microsoft x64 ABI applies. On Linux and macOS, the System V AMD64 ABI applies.

```mettle
extern fn puts(msg: cstring) -> int32 = "puts";
extern fn malloc(size: int64) -> cstring = "malloc";

fn main() -> int32 {
  puts("Hello");
  var p: cstring = malloc(100);
  return 0;
}
```

## Native Win32

For Win32 APIs, import `std/win32` instead of repeating raw extern declarations:

```mettle
import "std/win32";

fn main() -> int32 {
  win32_write_stdout("hello\n", 6);
  win32_sleep_ms(10);
  return 0;
}
```

With the internal linker, common Windows DLLs are probed directly:

```bash
mettle --build main.mettle -o main.exe
```

The default native import set includes `kernel32`, `user32`, `gdi32`, `advapi32`, `ws2_32`, `ucrtbase`, and `msvcrt`. If you call APIs from another DLL, pass it with `--link-arg -lname` for DLL export probing or a `.lib` path for import-library resolution.

## cstring

`cstring` is an alias for `uint8*`. Use it for C `char*`, `void*`, or opaque pointers. `cstring` and `uint8*` are interchangeable. When passing a Mettle `string` to a C function that expects `char*`, use `s.chars` or the `cstr` helper from `std/io`.

## Passing Structs to C

Structs are laid out in declaration order. For C interop, define the struct to match the C layout exactly. Field order, types, and alignment must be compatible. Padding between fields follows the target ABI.

On Windows, Mettle follows the Microsoft x64 aggregate rule for struct-by-value calls:

- structs sized exactly 1, 2, 4, or 8 bytes pass and return directly in one integer register
- all other aggregate sizes pass indirectly by pointer
- indirect returns use a hidden first argument in RCX, and the callee returns that pointer in RAX

This is covered for Mettle calling C functions that take or return structs by value, including `--emit-obj` builds linked with Mettle's internal linker. C calling exported Mettle functions with struct-by-value arguments or returns is not yet documented as supported.

When a C API expects a pointer to a struct, pass `&my_struct` or a `T*` variable. For portable Linux/macOS C interop, prefer pointer parameters until the System V AMD64 aggregate classifier is implemented.

```mettle
struct SockAddrIn {
  sin_family: int16;
  sin_port: uint16;
  sin_addr: uint32;
  sin_zero: uint8[8];
}
```

## Linking

On Windows, the recommended path is to let Mettle do the assemble/link step for you:

```bash
mettle --build main.mettle -o main.exe
```

When using the internal linker, common Win32 APIs and the C runtime resolve without external C toolchains. Use `--link-arg -lcustomdll` or `--link-arg path/to/custom.lib` for additional DLLs/import libraries. Raw COFF `.o` / `.obj` files passed through `--link-arg` are also included in the internal linker object list.

For Windows builds, prefer the internal linker path above. It is the path covered by current struct ABI and Win32 interop tests.

## POSIX Networking (Linux / macOS)

For networking on Linux or macOS, use `stdlib/std/net_posix.mettle`. This module provides POSIX socket bindings and requires the C helper functions in `stdlib/posix_helpers.c` for thread-safe errno access and atomic operations.

```bash
# Linux
gcc -o myapp output.o stdlib/posix_helpers.c -lpthread

# macOS
gcc -o myapp output.o stdlib/posix_helpers.c
```

The helper file provides:

- `posix_get_errno()` for thread-safe errno access
- `posix_cas_i32()` for atomic compare-and-swap
- `posix_yield()` for CPU yield in spin locks
- `posix_atomic_exchange_i32()` for atomic exchange

On macOS, `SOL_SOCKET` is `0xFFFF` and `SO_REUSEADDR` is `4`. On Linux, they are `1` and `2` respectively. Use the constants from `std/net_posix` or override them at the call site for macOS.
