# Mettle OS

A bootable operating system written in Mettle. The boot sector brings the
machine from real mode into 64-bit long mode, loads the kernel, and hands
control to it. The kernel drives the screen, the timer, and the keyboard, and
runs a shell.

Nothing outside the Mettle compiler builds it. The 512-byte boot sector is an
`asm` block the compiler assembles itself, and the kernel is ordinary Mettle
compiled to a flat image with `--target x86_64-none`.

## Files

| File | What it is |
| --- | --- |
| `boot.mettle` | The boot sector at 0x7c00. Reads the memory map, loads 128 sectors to 0x20000, enables A20, builds page tables, enters long mode. |
| `kernel.mettle` | The kernel at 0x20000. VGA console, IDT, PIC, 100 Hz timer, PS/2 keyboard, shell. |
| `build.ps1` | Compiles both and writes `mettleos.img`, a 1.44 MB floppy image. |
| `run.ps1` | Creates the VirtualBox machine, attaches the image, starts it. |

## Build

```bash
pwsh examples/os/build.ps1
```

That writes `examples/os/mettleos.img`. The script fails if the boot sector
loses its signature or the kernel outgrows the 128 sectors the boot sector
reads.

## Run it in VirtualBox

### The scripted way

```bash
pwsh examples/os/run.ps1
```

It creates a machine called `MettleOS` if there is none, attaches the image to
a floppy controller, and starts it with the GUI. `-Headless` starts it without
a window. `-Recreate` deletes the machine and builds it again from scratch.

A machine called `MettleOS` is already registered and powered off, pointing at
`mettleos.img`, so the script starts it as it stands.

### By hand, from the command line

```bash
VBoxManage createvm --name MettleOS --ostype Other_64 --register
```

```bash
VBoxManage modifyvm MettleOS --memory 64 --vram 16 --boot1 floppy --boot2 none --boot3 none --boot4 none --nic1 none
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
2. Give it 64 MB of memory and no hard disk. Answer yes when it warns about
   starting without one.
3. Settings, System, Motherboard: tick `Floppy` in the boot order and move it
   to the top.
4. Settings, Storage: add a Floppy controller, then attach
   `examples/os/mettleos.img` to it. The file picker wants "Choose a disk
   file", and `.img` is one of the types it takes.
5. Start.

## What you get

The boot sector prints one line through the BIOS, then the kernel clears the
screen and draws its greeting. The prompt is `mettle>`.

| Command | What it does |
| --- | --- |
| `help` | The command list |
| `clear` | Wipe the screen |
| `echo <text>` | Print the rest of the line |
| `mem` | The E820 memory map the firmware reported, and the usable total |
| `uptime` | Seconds and raw ticks since the timer started |
| `cpu` | The processor's vendor string, read with `cpuid` |
| `banner` | Draw the greeting again |
| `reboot` | Pulse the keyboard controller's reset line |
| `halt` | Stop the processor |

Typing works through an IRQ1 handler that fills a ring buffer, so the shell
sleeps on `hlt` between keys. Shift, backspace, and enter are handled.

## How it fits together

The boot sector loads the kernel to 0x20000 and identity maps the first
gigabyte with 2 MB pages, so the kernel addresses physical memory directly.
The stack sits at 0x90000. The firmware's memory map is left at 0x5000 with
its entry count in the first word, which is where `mem` reads it.

The kernel's interrupt handlers are `@interrupt` functions: the compiler emits
the entry that saves the registers and the `iretq` that returns. Vectors 0
through 31 hold fault handlers, and the pair of them shows both shapes, with
and without an error code. The PIC is remapped to 32 so the timer and the
keyboard land on 32 and 33.

## Limits

The kernel has no allocator, no filesystem, and no userspace. A flat image
links no runtime, so string interpolation and the heap are out; text goes to
the screen a byte at a time.
