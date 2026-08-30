# Bare metal

A program with no operating system under it needs things an ordinary program
never asks for: the exact instruction the manual specifies, a load address the
firmware chose, a function the CPU enters rather than a caller, memory whose
reads and writes are the point rather than a means to a value, and sometimes a
processor mode that is forty years old.

This page covers the six features that make that possible: inline assembly,
`volatile`, `@naked` and `@interrupt`, cross-compilation, a chosen link
address, and 16-bit code generation.

## Inline assembly

An `asm` block holds Intel-syntax assembly. It is assembled by the compiler
itself into the function it appears in: there is no external assembler, no
text handed to a tool, and no separate object to link.

```mettle
fn count_set_bits(value: uint64) -> int64 {
    var total: int64 = 0
    asm {
        mov rcx, {value}
        xor rax, rax
    next:
        test rcx, rcx
        je done
        mov rdx, rcx
        and rdx, 1
        add rax, rdx
        shr rcx, 1
        jmp next
    done:
        mov {total}, rax
    }
    return total
}
```

### Operand bindings

`{name}` names a Mettle local, parameter, or global, and expands to where that
variable lives. A local or parameter expands to its stack home; a global
expands to a rip-relative reference in 64-bit code and an absolute one in
16- and 32-bit code, where there is no such addressing. Write it wherever an
operand goes:

```mettle
mov rax, {a}        // read a
mov {result}, rax   // write result
```

The binding is a memory operand, so a pointer variable is *loaded* by
`mov rax, {p}`; to reach what it points at, load it first and then dereference
the register.

A machine register is written directly (`rax`, `xmm3`, `cr0`), never through a
binding. The compiler keeps no value in a register across an `asm` block, so a
block may clobber whatever it likes; it must still preserve the callee-saved
registers its calling convention names, and restore the stack it moves.

### What the assembler accepts

Intel syntax, one instruction per line, `;`, `#`, `//` and `/* */` comments.

- The full integer instruction set a systems program reaches for: the ALU
  group, `mov`/`movzx`/`movsx`/`movsxd`/`movabs`, `lea`, `test`, `xchg`, the
  shift and rotate group, `imul` in all three forms, `div`/`idiv`, `push`/`pop`,
  `inc`/`dec`, `not`/`neg`, `setcc`, `cmovcc`, `bt`/`bts`/`btr`/`btc`,
  `bsf`/`bsr`/`popcnt`/`lzcnt`/`tzcnt`, `xadd`, `cmpxchg`, and the string
  instructions with `rep`/`repe`/`repne`.
- Control transfer: `jmp`, `call`, `ret`, `retf`, `jcc`, `loop`, `jrcxz`, far
  `jmp`/`call` in the `selector:offset` form.
- System instructions: `cli`, `sti`, `hlt`, `cld`, `std`, `int`, `iret`,
  `iretd`, `iretq`, `in`, `out`, `lgdt`, `lidt`, `sgdt`, `sidt`, `lldt`, `ltr`,
  `lmsw`, `smsw`, `invlpg`, `cpuid`, `rdmsr`, `wrmsr`, `rdtsc`, `swapgs`,
  `xgetbv`, `syscall`, `sysret`, `sysenter`, `sysexit`, `wbinvd`, `clts`, and
  `mov` to and from the control and debug registers.
- SSE and SSE2 moves and arithmetic, `movd`/`movq`, and the conversions.
- Prefixes: `lock`, `rep`, segment overrides written `fs:[...]`.
- Directives: `db`, `dw`, `dd`, `dq` (constants, symbols, and strings), `resb`
  and friends, `align`, `times`, and `bits 16` / `bits 32` / `bits 64` inside a
  `@naked` function.
- `$` is the current address and `$$` the image origin, so
  `times 510-($-$$) db 0` means what it means everywhere else.

Labels defined in a block are local to it. A name the block does not define is
a symbol reference, and the compiler relocates it like any other: `call
some_function` reaches a Mettle function, and `dq some_global` stores its
address.

Branches are relaxed: a branch that reaches its target in one signed byte takes
the short form, and the assembler repeats the layout until no more shrink. That
is what keeps 16-bit output inside encodings an 8086 accepts.

An unrecognized instruction, an ambiguous operand size, or an unreachable
branch is a compile error naming the line inside the block.

### What it costs

A function containing an `asm` block is emitted by the baseline code generator
rather than the register allocator, and nothing is promoted to a register
across it. That is the price of letting the block clobber registers freely and
of `{x}` resolving to a stack home that actually exists.

## `volatile`

`volatile T` says that reading or writing a `T` is observable in itself. Such
an access is never removed, never merged with another, never hoisted out of a
loop, and never served from a register.

```mettle
fn wait_for_ready(status: volatile uint32*) {
    while (*status == 0) { }
}
```

The qualifier binds to the value being accessed, so `volatile uint16*` is a
pointer to volatile `uint16` -- the shape memory-mapped hardware has. Write it
anywhere a type goes: on a parameter, a local, a global, or a struct field.

```mettle
var vga: volatile uint16* = (volatile uint16*)0xB8000
vga[0] = 0x0F41
```

The guarantee is enforced twice. Each optimization pass that could move or drop
a memory access is taught to leave volatile ones alone, and the pass driver
takes a signature of the function's volatile accesses -- how many, in what
order, at what width -- before and after every pass. A pass that changes it
fails the compile naming itself, so a missed guard is a loud error rather than a
silent miscompile.

A function holding a volatile access is emitted by the baseline code generator,
for the same reason an `asm` block is: the register allocator's job is to keep
values out of memory, which is the one thing volatile forbids.

## `@naked` and `@interrupt`

### `@naked`

A `@naked` function has no prologue, no frame, and no epilogue. Its body may
hold only `asm` blocks, and the block returns by itself.

```mettle
@naked fn cpu_vendor_ebx() -> uint32 {
    asm {
        push rbx
        xor eax, eax
        cpuid
        mov eax, ebx
        pop rbx
        ret
    }
}
```

`@naked` is where `bits 16` and `bits 32` are allowed, because a naked function
is the only one whose bytes the compiler contributes nothing to. It is how a
boot sector's entry point is written.

### `@interrupt`

An `@interrupt` function is entered by the CPU, not by a caller. The compiler
emits the entry sequence: it clears the direction flag, saves all fifteen
general-purpose registers, aligns the stack, runs the body, restores the
registers, and returns with `iretq`.

```mettle
@interrupt fn timer_isr() {
    ticks = ticks + 1
}

@interrupt fn page_fault(frame: InterruptFrame*, error_code: uint64) {
    handle_fault(frame, error_code)
}
```

The parameters say what the vector pushes:

| Parameters | Meaning |
| --- | --- |
| none | a vector with no error code |
| one pointer | the pointer receives the interrupt frame |
| a pointer and an integer | the vector pushes an error code, which the entry reads and pops before `iretq` |

An `@interrupt` function returns nothing -- `iretq` has nowhere to hand a value
back to -- and is emitted for 64-bit targets. Write a `@naked` handler for
16-bit or 32-bit code.

## Cross-compilation

`--target <triple>` compiles for a machine that is not this one. It picks the
object format, the calling convention, and the width the code generators and
the inline assembler emit for.

| Triple | Output |
| --- | --- |
| `x86_64-windows` | COFF object, MS-x64 convention |
| `x86_64-linux` | ELF object, System V AMD64 convention |
| `x86_64-none` | ELF object, freestanding |
| `aarch64-linux` | AArch64 ELF object |
| `aarch64-none` | AArch64 ELF object, freestanding |
| `i386-none`, `i686-none` | 32-bit, flat image only |
| `i8086-none` | 16-bit real mode, flat image only |

```bash
mettle server.mettle --target x86_64-linux --emit-obj -o server.o
```

A trailing vendor or environment is accepted and ignored, so
`x86_64-unknown-linux-gnu` selects the same target as `x86_64-linux`.

The 16- and 32-bit targets have no object format that carries their
relocations, so they produce a flat image and nothing else.

## A chosen link address

`--image-base <addr>` sets where the linked image is loaded, replacing the
format's default. It applies to all three products: a PE's `ImageBase`, an ELF
executable's load address, and where a flat image's first byte lands. A PE
loads on a 64K boundary and an ELF on a page, and a base that does not sit on
one is a compile error rather than an image nothing can load.

`--emit-flat <file>` writes a raw image with no container at all: sections laid
out from the image base, every relocation resolved against the final addresses,
and the bytes written out.

```bash
mettle boot.mettle --target i8086-none --image-base 0x7c00 --emit-flat boot.bin
```

The entry point is a function named `_start`, or `main` when there is none, and
it is placed at offset zero. At image base `0x7c00` the compiler recognizes a
boot sector: the image is padded to 512 bytes and signed `0x55AA`.

A flat image links no library, so every name it uses has to be defined in it.
A program that reaches the runtime -- a null-pointer check's trap, string
formatting, the allocator -- names symbols nothing in the image provides, and
the compiler says which section referenced what. Define them yourself, or keep
the image to code that does not need them.

## 16-bit code generation

With `--target i8086-none` the compiler generates 16-bit real-mode code for
ordinary Mettle functions, not only for `asm` blocks.

```mettle
fn putchar(c: int16) -> int16 {
    asm {
        mov ax, {c}
        mov ah, 0x0e
        mov bx, 7
        int 0x10
    }
    return c
}

fn main() -> int16 {
    var i: int16 = 0
    while (i < 5) {
        putchar((int16)(65 + i))
        i = (int16)(i + 1)
    }
    return 0
}
```

Real mode is 16 bits wide, and the code generator says so:

- Values are `int8`, `uint8`, `int16`, `uint16`, and near pointers. There is no
  `int32`, no `int64`, and no floating point.
- Arguments are pushed right to left and the caller cleans up; the result comes
  back in `AX`. Locals and parameters live in a `bp`-relative frame.
- Arithmetic, comparison, control flow, calls, loads and stores, address-of,
  and `asm` blocks with `{}` bindings all work. Anything else is a compile
  error naming the construct.

The test suite compiles a boot sector out of Mettle and then *runs* it in a
real-mode emulator, checking what it printed through the BIOS. Bytes that look
right are not evidence that real-mode code runs.

## A complete boot sector

```mettle
@naked fn _start() {
    asm {
        bits 16
        cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7c00
        sti
        call main
        cli
    halt:
        hlt
        jmp halt
    }
}

fn putchar(c: int16) -> int16 {
    asm {
        mov ax, {c}
        mov ah, 0x0e
        mov bx, 7
        int 0x10
    }
    return c
}

fn main() -> int16 {
    var i: int16 = 0
    while (i < 5) {
        putchar((int16)(65 + i))
        i = (int16)(i + 1)
    }
    putchar(13)
    putchar(10)
    return 0
}
```

```bash
mettle boot.mettle --target i8086-none --image-base 0x7c00 --emit-flat boot.bin
```

512 bytes, signed, and it prints `ABCDE`.
