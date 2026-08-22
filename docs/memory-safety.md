# Memory safety

What the compiler proves about memory before your program runs, and what
`--safe` checks while it runs.

Two rules shape all of it. The compiler reports only what it can prove, so a
diagnostic here is a fact rather than a suspicion. And it never rejects a
program for want of an annotation: there is nothing to write, and nothing to
silence.

## Compile-time diagnostics

These run on every build. Each is reported once, at the line that does the
damage, with the line that set it up named in the message.

| Code | What it catches |
|------|-----------------|
| M0101 | Use after free |
| M0102 | Double free |
| M0103 | Returning the address of a stack local |
| M0104 | Storing a stack address in a global |
| M0105 | Constant array index out of bounds |
| M0106 | A memory operation that overflows a stack array |
| M0107 | Memory leak |
| M0108 | Use after a pointer freed by a call |
| M0109 | Double free through a call |
| M0110 | A borrowed interior pointer outliving its scope |
| M0111 | A borrowed pointer invalidated by realloc |
| M0112 | A borrowed pointer invalidated by free |
| M0113 | Dereference of a null pointer |
| M0114 | Dereference of an unmapped constant address |
| M0117 | A loop index running past the end of an array |

`mettle explain M0101` prints the reasoning and the fix for any of them.

### Use after free and double free

```mettle
var p: int32* = malloc(16);
free(p);
free(p);
```

```text
warning[M0102]: Double free of `p` (already freed at line 6)
```

Reading through the pointer instead gives:

```text
warning[M0101]: Use of `p` after it was freed (freed at line 6); this is
use-after-free
```

Both are conservative. They fire when the freed pointer and the second use are
provably the same allocation on the same path.

### Escaping stack addresses

Returning the address of a local, or of a field inside one, fails the build:

```mettle
fn leak() -> int32* {
  var p: P;
  return &p.x;
}
```

```text
error[M0103]: Returning the address of stack local `p`; the frame is destroyed
when this function returns, so the caller receives a dangling pointer
```

Storing one in a global is the same mistake with a longer fuse, and it draws
M0104.

### Leaks

M0107 fires when an allocation never escapes the function, is never returned,
stored, or passed on, and is never freed. It stays quiet when ownership leaves
the function, because then the leak is somebody else's to prove.

### Constant out-of-bounds

An index the compiler can fold is checked against the array's declared size:

```mettle
var a: int32[4];
a[7] = 1;
```

```text
error[E0003]: Array index 7 is out of bounds for 'int32[4]' (size 4)
```

M0117 covers the loop version, where the bound and the length are both known
and the bound is larger:

```mettle
var a: int64[8];
var i: int32 = 0;
while (i < 9) { a[i] = (int64)i; i = i + 1; }
```

```text
error[M0117]: This loop runs `i` up to 8, but `a` has 8 elements (valid
indexes 0..7); the final iteration reads or writes past the end
```

The same mistake written as `for i in 0..9` is missed today and traps at run
time instead. [Known limitations](known-limitations.md) tracks it.

## Run-time checks with --safe

`--safe` inserts a check on every memory access the compiler could not prove
in bounds, and it keeps them under `--release`.

```bash
mettle --safe --build program.mettle
```

A failed check stops the program and names the access:

```text
Fatal error: `a[]` is outside its bounds
```

The process exits with status 1.

Most checks cost nothing, because the compiler removes the ones it can settle
statically. It recognizes an index written as a multiple of a loop counter plus
an invariant plus a constant, and proves that shape in bounds against the
array's length. A loop over `0..n` indexing an array of `n` elements gets no
checks at all.

What is left is the accesses that genuinely depend on run-time values.
Measured over the benchmark suite the cost ranges from nothing to a small
multiple, depending on how much of the indexing the compiler could settle.

## What is not covered

The compiler proves what it can see. It stays quiet about the rest:

- Memory reached through a `cstring` or a `rawptr` handed in by C. There is
  no length to check against.
- Aliasing between two pointers it cannot relate.
- Integer overflow. Arithmetic wraps, by design.
- Data races between threads.
- Anything inside an `asm` block.

`--safe` checks bounds. It is not a sanitizer and it does not track ownership
across the C boundary.

## See also

- [Borrow checker](borrow-checker.md)
- [Heap allocation](heap-allocation.md)
- [Diagnostics](diagnostics.md)
