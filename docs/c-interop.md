# Native ABI Interoperability

Mettle can call symbols that follow the target native ABI and can access native
globals. This does not permit a C runtime. Use `std/win32`, `std/thread`, and
`std/net` for common OS APIs.

## Calling Native Functions

Declare native functions with `extern fn`. Use the `= "symbol"` suffix when the
link name differs. Parameters and return types must match the target ABI.
Windows x86_64 uses the Microsoft ABI. Linux x86_64 uses System V. Linux
AArch64 uses AAPCS64.

```mettle
extern fn puts(msg: cstring) -> int32 = "puts";
extern fn malloc(size: int64) -> cstring = "malloc";

fn main() -> int32 {
  puts("Hello");
  var p: cstring = malloc(100);
  return 0;
}
```

The example names come from Mettle's owned runtime. They do not resolve to
libc. Link arguments that name a C or compiler runtime fail.

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

The default native import set includes `kernel32`, `user32`, `gdi32`,
`advapi32`, and `ws2_32`. It excludes UCRT and MSVCRT. If you call another OS
or vendor DLL, pass it with `--link-arg -lname` or an import library path.

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

The internal linker resolves common Win32 APIs and owned runtime symbols. Use
`--link-arg -lcustomdll` or an import library path for an extra DLL. Raw COFF
objects can also be passed through `--link-arg`. The final PE import audit still
runs after those additions.

For Windows builds, prefer the internal linker path above. It is the path covered by current struct ABI and Win32 interop tests.

## Linux Networking

Use `std/net` for portable Windows and Linux source. Older code can import
`std/net_posix` on Linux. Its socket, error, atomic, and yield names all come
from Mettle's owned syscall runtime. No helper C source or pthread link flag is
needed.
