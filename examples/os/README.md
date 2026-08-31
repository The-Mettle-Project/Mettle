# Mettle OS

A bootable operating system written in Mettle, with a desktop. The boot sector
loads a second stage, which asks the firmware for a linear framebuffer and
brings the machine into 64-bit long mode. The kernel manages memory, schedules
tasks, drives the screen, the mouse, the keyboard, the clock and a serial line,
and runs a windowed desktop with a shell inside it.

Nothing outside the Mettle compiler builds it. The 512-byte boot sector is an
`asm` block the compiler assembles itself, and the kernel is ordinary Mettle
compiled to a flat image with `--target x86_64-none`.

## What it does

- **Memory.** The boot sector asks the firmware for the E820 map. The kernel
  turns every usable region above 2 MB into a free page list, and runs a heap
  on top of it that splits blocks on allocation and merges neighbours on free.
- **Processes.** A process has a pid, a parent, a priority, a state, its own
  stack, the pages it claimed, and accounting for the ticks it has burned. The
  scheduler runs on the timer: highest ready priority first, round robin inside
  a level, with anything passed over for a quarter second promoted so nothing
  starves, and an idle process when nothing is ready. Sleeping and blocking are
  real, so a waiting process costs nothing. Exits leave a zombie holding an
  exit code until a parent waits for it.
- **Time.** A 100 Hz PIT timer, and the CMOS clock read through ports 0x70
  and 0x71.
- **Input.** A PS/2 keyboard on IRQ1 filling a ring buffer, with shift,
  backspace, and arrow keys walking the command history.
- **Files.** `build.ps1` packs everything in `files/` into an archive that
  rides along on the disk. `ls` and `cat` read it out of memory.
- **GPU.** The kernel finds the VMware SVGA II device on the PCI bus, claims
  it, negotiates the protocol version, sets the mode itself, and drives it
  through its command FIFO. Damage rectangles go to the device rather than
  whole screens, and the pointer is a hardware alpha cursor the device
  composites on its own. Everything falls back to the firmware framebuffer when
  the device is absent.
- **Graphics.** The second stage walks the VESA mode list for a fallback linear
  framebuffer and copies the BIOS 8x16 font out of the video ROM on the way
  past.
- **Desktop.** Windows XP, drawn in flat gradients: blue title bars with
  minimise, maximise and close, the green start button and a start menu, task
  buttons, the clock in the tray, and desktop icons on the plain blue
  background.
- **Responsiveness.** Nothing repaints unless it changed. Every draw is clipped
  to the damaged rectangle, the timer runs at 1 kHz so input is read within a
  millisecond, and a task waiting for a key is skipped by the scheduler rather
  than spinning.
- **Pointer.** A PS/2 mouse on IRQ12, three-byte packets, movement and the left
  button.
- **Output.** Everything the kernel prints goes to the terminal window, to the
  80x25 text console when there is no framebuffer, and to COM1 either way.

## Modules

`kernel.mettle` is the entry file and does nothing but wire the modules
together in `kernel/`. Each one owns its state and hands out an interface;
nothing reaches into another module's globals.

| Module | Owns |
| --- | --- |
| `layout` | Where things sit in physical memory. Every fixed address in the system is here. |
| `pci` | Configuration space, and finding a device by its identity. |
| `svga` | The VMware SVGA II driver: registers, command FIFO, hardware cursor. |
| `video` | Drawing: fills, gradients, alpha, rounded shapes, discs, blits, presenting. |
| `cursor` | The pointer, sampled into an alpha sprite, uploaded to the device or drawn by hand. |
| `theme` | The Luna palette: window frames, taskbar, start button, tray. |
| `font` | The BIOS 8x16 glyphs, drawn as pixels. |
| `theme` | Every colour the desktop uses. |
| `mouse` | The PS/2 pointer. |
| `terminal` | A text grid that the console writes into and a window draws. |
| `window` | Window records, z-order, hit testing. |
| `desktop` | Wallpaper, icons, chrome, taskbar, cursor, and the event loop. |
| `apps` | What each window draws: terminal, files, tasks, clock, about. |
| `port` | `in`, `out`, and the instructions that stop or idle the processor. |
| `text` | Comparing, measuring, copying, and parsing bytes. |
| `format` | Turning numbers into digits. The console and the screen share it. |
| `vga` | The screen: cells, colour, the cursor, scrolling, the text region. |
| `serial` | COM1. |
| `console` | Byte output. Writes to the screen and to every registered stream. |
| `interrupt` | The IDT, the fault handlers, and the PIC. |
| `clock` | The timer, the CMOS clock, sleeping, and the speaker. |
| `page` | The firmware memory map and the free page list built from it. |
| `heap` | Splitting and merging blocks inside pages. |
| `process` | The process table, states, priorities, scheduling, sleep, wait channels, exits. |
| `jobs` | Demonstration processes: counters, processor burners, sleepers. |
| `keyboard` | Scancodes to characters, and the ring buffer between them. |
| `archive` | The files packed into the boot image. |
| `shell` | The command registry, the line editor, and the history. |
| `status` | The status bar and the counter tasks that draw it. |
| `commands` | Every built-in command, registered at startup. |

The dependencies run one way. `port`, `text`, `format`, and `layout` depend on
nothing. `vga` and `serial` sit on `port`; `console` sits on those; `interrupt`
reports faults through `console`; `task` and `keyboard` install themselves into
`interrupt`; `shell` and `commands` sit on top of everything.

Three registries keep it that way, so a module adds itself rather than being
named by the layer below:

```mettle
interrupt_install(VECTOR_KEYBOARD, (uint64)&on_keyboard)
console_add_stream(&serial_put)
shell_register("ps", "the task table", &cmd_ps)
```

## Adding to it

- **An app.** Write `draw`, `key`, and `click` functions in
  `kernel/apps.mettle`, open a window with them, and add one `register` line in
  `kernel/desktop.mettle` to give it an icon.
- **A command.** Write `fn cmd_thing(argument: cstring)` in
  `kernel/commands.mettle` and add one `shell_register` line. Nothing else
  changes, and `help` picks it up on its own.
- **An output device.** Write a `fn put(ch: uint8)` and call
  `console_add_stream(&put)` once. Every line the kernel prints goes there too.
- **An interrupt.** Write an `@interrupt fn`, then
  `interrupt_install(vector, (uint64)&handler)` in your module's init.
- **A file.** Drop it in `files/` and rebuild.

## Files

| File | What it is |
| --- | --- |
| `boot.mettle` | The boot sector at 0x7c00. Reads the second stage and 192 KB of kernel and files off the floppy. |
| `stage2.mettle` | At 0x8000. Memory map, VESA mode, BIOS font, A20, page tables for the first four gigabytes, long mode. |
| `kernel.mettle` | The entry point and the boot order. |
| `kernel/` | The modules above. |
| `files/` | Text carried in the boot image and read back by `ls` and `cat`. |
| `build.ps1` | Compiles both images, packs the archive, writes `mettleos.img`. |
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

## The desktop

Click a desktop icon or the start button to open a window. Drag a window by its
title bar, minimise it to the taskbar, maximise it to fill the work area, close
it with the red button. A taskbar button raises its window, or minimises it
again if it is already in front. The terminal window runs the shell, and the
keyboard goes to whichever window is in front. The files window lists what the
boot image carried; click a file to read it. The processes window is the
scheduler's own table with live processor shares, redrawn every second.

If the firmware offers no 32-bit linear framebuffer, the kernel says so on the
serial line and falls back to the 80x25 text console with the same shell and
the same commands.

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
| `ps` | Every process with state, priority, processor share, and parent |
| `top` | The same, ordered by processor share |
| `spawn <n>` | Start counter processes |
| `work <ms>` | Start a process that burns the processor |
| `wait <pid>` | Block until a process exits, then read its exit code |
| `kill <pid>` | Ask a process to stop |
| `nice <pid> <level>` | Move a process between high, normal and low |
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
