# C interoperability

Mettle calls symbols that follow the target's native ABI, and C calls back
into Mettle. This page covers the declarations, the types that cross the
boundary, the struct rules, and linking.

There is no C runtime underneath. A Mettle program's `malloc` and `puts` come
from Mettle's own runtime, and a link argument naming a C or compiler runtime
fails. Reach the operating system through `std/win32`, `std/thread`, and
`std/net`.

## Calling out

Declare the function with `extern fn`. Add `= "symbol"` when the link name
differs from the Mettle name.

```mettle
extern fn puts(msg: cstring) -> int32 = "puts";
extern fn malloc(size: int64) -> rawptr = "malloc";
```

```mettle
puts("Hello");
var p: int32* = malloc(100);
```

Parameter and return types must match what the other side declares. The
convention comes from the target: the Microsoft ABI on Windows x86-64, System
V on Linux x86-64, AAPCS64 on Linux AArch64.

## Calling in

Mark a Mettle function `export` and C can call it by name:

```mettle
export fn add_two(a: int32, b: int32) -> int32 {
  return a + b;
}
```

```c
extern int32_t add_two(int32_t a, int32_t b);
```

Calls between two Mettle functions use Mettle's own convention. Both sides
agree, so it makes no difference to a Mettle program, and it is why the
platform rule is applied to the functions reachable from outside: `extern`
callees, `export`ed functions, and `main`.

## cstring and rawptr

`cstring` is `uint8*`: a pointer to bytes that a C function reads up to a nul.
Use it for C's `char*`. `cstring` and `uint8*` are interchangeable.

`rawptr` is an address with no element type, C's `void*`, and what an
allocator hands out. It converts to and from every pointer type in both
directions, so `var p: int32* = malloc(n);` and `free(p)` need no cast. It
cannot be indexed, dereferenced, or offset, because it names no element.

## Passing a string to C

A [`string`](types.md) is a pointer and a length, with no terminator.
Termination is a property of this boundary.

A string literal is already terminated in read-only memory, so it flows
straight into a `cstring` parameter and allocates nothing:

```mettle
var fp: cstring = fopen("data.txt", "rb");
```

Anything built at run time needs a terminated copy. `cstr` from
[`std/io`](standard-library.md) makes one, and it takes the allocator to make
it from, so the cost sits in the signature:

```mettle
var path: cstring = cstr(name, &malloc);
defer free(path);
var fp: cstring = fopen(path, "rb");
```

`cstr` returns 0 when the allocator does.

For a C function that takes a pointer and a length, pass `s.chars` and
`s.length` and copy nothing.

## Structs by value

Define the struct to match the C layout: same field order, same types. Fields
are laid out in declaration order, each on its own alignment, with the whole
struct padded to its widest member.

```mettle
struct SockAddrIn {
  sin_family: int16;
  sin_port: uint16;
  sin_addr: uint32;
  sin_zero: uint8[8];
}
```

On Windows, Mettle follows the Microsoft x64 aggregate rule:

- A struct of exactly 1, 2, 4, or 8 bytes passes and returns in one integer
  register.
- Every other size passes indirectly, by pointer.
- An indirect return uses a hidden first argument in RCX, and the callee
  returns that pointer in RAX.

On Linux, Mettle follows System V, which cuts the struct into eight-byte
chunks and classifies each:

- 16 bytes or less passes in registers, one per eightbyte. A chunk holding
  integers or pointers takes a general register; a chunk holding only floats
  takes an XMM. So `{int64, double}` arrives as one of each, and
  `{double, double}` as two XMMs.
- Anything larger is MEMORY: the caller copies the bytes into the outgoing
  stack area.
- A struct of 16 bytes or less returns the same way, in RAX and RDX or XMM0
  and XMM1, with no hidden pointer.

Both rules work in both directions: Mettle calling a C function that takes or
returns a struct by value, and C calling an exported Mettle function that
does.

When the C API wants a pointer to a struct, pass `&my_struct` or a `T*`.

## Win32

Import [`std/win32`](standard-library.md) rather than repeating raw `extern`
declarations:

```mettle
import "std/win32";
```

```mettle
win32_write_stdout("hello\n", 6);
win32_sleep_ms(10);
```

The internal linker probes the common Windows DLLs directly, so an ordinary
build needs no import libraries:

```bash
mettle --build main.mettle -o main.exe
```

The default import set is `kernel32`, `user32`, `gdi32`, `advapi32`, and
`ws2_32`. UCRT and MSVCRT are excluded. For another DLL, pass
`--link-arg -lname` or an import library path. Raw COFF objects go through
`--link-arg` too, and the PE import audit still runs afterwards.

## Linux

A Linux build is freestanding: no libc on the link line, and no shared library
ever links, because the ELF writer refuses a `PT_INTERP`. Static archives work
against the owned subset.

For sockets, `std/net` covers both platforms from one source. `std/net_posix`
is the older Linux-only path. Its socket, error, atomic, and yield names come
from Mettle's own syscall runtime, so no helper C source and no pthread flag
is needed.

## See also

- [Runtime model](runtime-model.md)
- [Linker and build pipelines](linker-build-pipelines.md)
- [Types](types.md)
