# Mettle OS

A bootable operating system written in Mettle. The boot sector brings the
machine from real mode into 64-bit long mode, loads the kernel and a small
file archive, and hands control over. The kernel manages memory, schedules
tasks, drives the screen, the keyboard, the clock and a serial line, and runs
a shell.

Nothing outside the Mettle compiler builds it. The 512-byte boot sector is an
`asm` block the compiler assembles itself, and the kernel is ordinary Mettle
compiled to a flat image with `--target x86_64-none`.

## What it does

- **Memory.** The boot sector asks the firmware for the E820 map. The kernel
  turns every usable region above 2 MB into a free page list, and runs a heap
  on top of it that splits blocks on allocation and merges neighbours on free.
- **Tasks.** A preemptive round robin scheduler, switched from the timer
  interrupt. Each task gets a page of stack, an entry in the task table, and a
  time slice. Tasks that finish are reaped and their stack goes back to the
  page list.
- **Time.** A 100 Hz PIT timer, and the CMOS clock read through ports 0x70
  and 0x71.
- **Input.** A PS/2 keyboard on IRQ1 filling a ring buffer, with shift,
  backspace, and arrow keys walking the command history.
- **Files.** `build.ps1` packs everything in `files/` into an archive that
  rides along on the disk. `ls` and `cat` read it out of memory.
- **Output.** An 80x25 VGA console with scrolling and a hardware cursor, a
  status bar drawn by a background task, and a copy of everything on COM1.

## Files

| File | What it is |
| --- | --- |
| `boot.mettle` | The boot sector at 0x7c00. Memory map, 128 KB read off the floppy, A20, page tables, long mode. |
| `kernel.mettle` | The kernel at 0x20000. |
| `files/` | Text carried in the boot image and read back by `ls` and `cat`. |
| `build.ps1` | Compiles both, packs the archive, writes `mettleos.img`. |
| `run.ps1` | Creates the VirtualBox machine, attaches the image, starts it. |

## Build

```bash
pwsh examples/os/build.ps1
```

That writes `examples/os/mettleos.img`, a 1.44 MB floppy image. The script
fails if the boot sector loses its signature, if the kernel outgrows the 64 KB
the image reserves for it, or if the files outgrow theirs.

## Run it in VirtualBox

### The scripted way

```bash
pwsh examples/os/run.ps1
```

It creates a machine called `MettleOS` if there is none, gives it 128 MB and a
serial line, attaches the image to a floppy controller, and starts it with the
GUI. `-Headless` starts it without a window. `-Recreate` deletes the machine
and builds it again. Everything the kernel prints also lands in
`examples/os/serial.log`, which is the quickest way to see what happened.

A machine called `MettleOS` is already registered and powered off, pointing at
`mettleos.img`, so the script starts it as it stands.

### By hand, from the command line

```bash
VBoxManage createvm --name MettleOS --ostype Other_64 --register
```

```bash
VBoxManage modifyvm MettleOS --memory 128 --vram 16 --boot1 floppy --boot2 none --boot3 none --boot4 none --nic1 none
```

```bash
VBoxManage storagectl MettleOS --name Floppy --add floppy
```

```bash
VBoxManage storageattach MettleOS --storagectl Floppy --port 0 --device 0 --type fdd --medium examples/os/mettleos.img
```

```bash
VBoxManage startvm MettleOS
```

### By hand, in the VirtualBox window

1. Machine, New. Name it `MettleOS`, leave the ISO field empty, and tick
   "Skip Unattended Installation" if the wizard offers it. Type `Other`,
   version `Other/Unknown (64-bit)`.
2. Give it 128 MB of memory and no hard disk. Answer yes when it warns about
   starting without one.
3. Settings, System, Motherboard: tick `Floppy` in the boot order and move it
   to the top.
4. Settings, Storage: add a Floppy controller, then attach
   `examples/os/mettleos.img` to it. The file picker wants "Choose a disk
   file", and `.img` is one of the types it takes.
5. Start.

## The shell

| Command | What it does |
| --- | --- |
| `help` | The command list |
| `clear` | Wipe the screen |
| `echo <text>` | Print the rest of the line |
| `mem` | The firmware memory map, free pages, heap usage |
| `alloc` | Allocate eight blocks, write them, read them back, free them |
| `uptime` | Seconds and raw ticks since boot |
| `date` | The CMOS clock |
| `cpu` | The processor's vendor string, read with `cpuid` |
| `ls` | The files carried in the boot image |
| `cat <file>` | Print one of them |
| `ps` | The task table with state, slices, and stack |
| `spawn` | Start a task that counts on the bottom line |
| `kill <id>` | Ask a task to stop |
| `hexdump <addr>` | 128 bytes of memory, hex and text |
| `beep` | A note on the speaker |
| `banner` | Draw the greeting again |
| `reboot` | Pulse the keyboard controller's reset line |
| `halt` | Stop the processor |

Arrow up and down walk the last eight commands. `spawn` a few times and watch
several counters advance on the bottom line while you keep typing: that is the
timer interrupt switching between them.

## How it fits together

The boot sector loads the kernel to 0x20000 and the file archive to 0x30000,
identity maps the first gigabyte with 2 MB pages, and leaves the firmware's
memory map at 0x5000. The kernel's stack starts at 0x90000, and every other
stack comes from the page allocator.

The timer's entry point is a `@naked` function that pushes all fifteen general
registers, hands the stack pointer to a Mettle function, and returns on the
stack pointer that function answers with. That one exchange is the whole
context switch: the scheduler picks a task, and the epilogue pops that task's
registers instead of the ones it saved. The other handlers are `@interrupt`
functions, where the compiler writes the entry and the `iretq` itself, in both
shapes, with and without an error code.

## Limits

There is no userspace, no privilege separation, and no disk driver: the kernel
never talks to the floppy again after the boot sector reads it. The heap tops
out at one page per allocation. `hexdump` past the first gigabyte walks off
the identity map and takes a page fault, which the fault handler reports
before stopping. Tasks share the kernel's address space, so a wild pointer in
one is a wild pointer in all of them.
