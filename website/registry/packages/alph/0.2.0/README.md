# alph runtime

<img width="3419" height="1389" alt="image" src="https://github.com/user-attachments/assets/f685df06-1016-4a6d-a726-b67c24c767b5" />


alph runs work where it will finish first: on this core, another core, a GPU, another machine or a board on a serial line. It is written in [Mettle](https://github.com/The-Mettle-Project/Mettle), and the same source builds for a microcontroller with no OS and for several computers working together.

The whole model is small. An actor is a state and a handler. A message is four words. A node runs one message at a time. Everything else (time, a GPU, other cores, other machines) is something that sends messages, added as a layer you can leave out.

## Example

```mettle
import "std/io";
import "alph/actor/actor";

const ADD: Word = 1;

struct Counter {
  total: Word;
}

fn counter(a: AlphActor*, m: AlphMsg*) -> Word {
  var c: Counter* = (Counter*)a->state;
  c->total = c->total + m->arg;
  if (c->total >= 6) {
    println("total {c->total}");
    a->node->stop();
  }
  return 0;
}

var g_node: AlphNode;
var g_actors: AlphActor[4];
var g_mail: AlphMsg[32];

fn main() -> int32 {
  var c: Counter;
  c.total = 0;
  g_node.init(&g_actors[0], 4, &g_mail[0], 8);
  var pid: Word = g_node.spawn(&counter, (rawptr)&c);
  for i in 1..4 {
    g_node.send(pid, ADD, (Word)i);
  }
  g_node.run();
  return 0;
}
```

This is [examples/count.mettle](examples/count.mettle). The node owns no memory: the program hands it the actor slots and the mailboxes, which is why the same code fits a microcontroller.

On a computer most programs want work done on whatever cores and other machines there are. That takes a function for one piece of the work, one for each answer, and one call:

```mettle
fn primes_in(chunk: Word) -> Word {
  ...                        // the primes in this chunk
  return found;
}

fn counted(chunk: Word, found: Word, ctx: rawptr) {
  g_primes = g_primes + found;
}

fn main() -> int32 {
  g_m.init(4, 0, false);             // four cores
  g_m.piece(PRIMES, &primes_in);     // how one piece is done
  g_m.pieces(PRIMES, 200, &counted, 0);
  g_m.shut();
  return 0;
}
```

This is [examples/primes.mettle](examples/primes.mettle). Name another machine and share the kind, and the pieces also run there. A board shares the same kind of function. The program never says where a piece goes, and pieces of little work are handed out several at a time, so even a piece of a microsecond is worth handing out. Rows of data are handed out the same way with `piece_of` and `pieces_of` ([examples/readings.mettle](examples/readings.mettle)), and `piece_gpu` adds the GPU, which gets much bigger jobs than a core ([examples/reach.mettle](examples/reach.mettle)). Work that runs on the GPU or gives data back is a job (`kind` and `work`), shown below.

## What is here

| Layer | What it does | Where |
|---|---|---|
| Core | Actors, messages, the run loop. On Linux a handler that crashes ends only its actor | [src/actor](src/actor/actor.mettle) |
| Cores | One node per core: a thread each on Linux, a hart each on a chip with several. An actor sends to another core the way it sends next door. On Linux, lanes let cores that send each other a lot do it without a lock | [src/cores](src/cores/cores.mettle) |
| Time | A message sent later. Small timers for a board, a timer wheel for thousands | [src/time](src/time/time.mettle), [wheel](src/time/wheel.mettle) |
| Places | A job goes to a core, the GPU or another machine, whichever alph has timed as finishing it first. A job stays where its data is. A kind can be done at most once, for work that must never happen twice | [src/place](src/place/place.mettle) |
| Polls | A socket sends a message when it is ready, so no handler waits. TCP calls that go with it, and other programs: start one and get a message when it ends | [src/poll](src/poll/poll.mettle) |
| Lives | Watch an actor to be told when it ends and why. Keep one, and a crash restarts it in place with the same pid | [src/life](src/life/life.mettle) |
| Record | The last messages each core ran, with their times, kept in memory or in a file. After a crash, or after the program was killed, they say what it was doing | [src/record](src/record/record.mettle) |
| Links | Other machines over UDP. Messages arrive once and in order, can carry bytes (1,024 in a parcel, more as a long message put together at the other end), and a lost machine is noticed in half a second | [src/link](src/link/link.mettle) |
| Names and the door | Find an actor on another machine by name, and watch it | [src/name](src/name/name.mettle), [src/door](src/door/door.mettle) |
| Seals | A link only the holders of one key can read or speak on | [src/seal](src/seal/seal.mettle) |
| Tether | A microcontroller tied to a bigger machine by its serial line: each side's actors reach the other's by pid. That machine can pass the board's mail on, so the whole network reaches it. The line can be made sure, so what is lost or damaged on it is sent again | [src/tether](src/tether/tether.mettle) |
| Board | The short way for a microcontroller: it says which kinds of work it does, and the bigger machines send it jobs as they would to any other place. It asks for work too, so two boards share work with no computer between them, and work that must never happen twice can be asked for and done at most once. An interrupt can be made a message to an actor | [src/board](src/board/board.mettle) |
| Machine and batch | The short way: a machine set up in one call, and one kind of work handed out in pieces to cores, the GPU, other machines and boards | [src/machine](src/machine/machine.mettle), [src/batch](src/batch/batch.mettle) |
| Ports | What differs per device: `mcu` (one hart, no OS), `harts` (several harts, no OS), `pico2` (a Raspberry Pi Pico 2), `host` (x86-64 Linux) and `arm64` (64-bit ARM Linux: a Raspberry Pi, a Jetson) | [ports](ports) |

[docs/start.md](docs/start.md) shows how to use each of these. [docs/design.md](docs/design.md) explains why they are built this way, what was measured, and what is not built yet. Every struct, constant and function of these layers is listed in [docs/reference.md](docs/reference.md).

## How fast

Measured on one 16-CPU x86-64 laptop with an RTX 4070, release builds, with other work running on it. "Two machines" is two links in one process over loopback. Nothing here is measured on a real board or on ARM.

| What | How fast |
|---|---|
| A message between two actors on one core | 23 to 25 ns |
| A message to another core | 100 to 250 ns, 56 to 135 with lanes on |
| Setting and cancelling a timer, 10,000 waiting | 222 ns |
| Keeping a record of every message a core runs | 4 ns a message, 9 with its time |
| A request and its reply between two machines | 15 to 18 µs |
| Short messages to another machine | 1.1 to 1.25 million a second |
| Messages of 64 KB to another machine | 123 to 133 MB a second, 64 to 80 with one datagram in a hundred lost |
| Handing out a piece of work that is a number in and a number out | 20 ns a piece |
| Handing out a job | about 500 ns |
| 2,000,000 rows of 64 bytes, 2.2 µs of work each, on four cores and the GPU | 27 to 40 ms, against 1,236 on the four cores alone |
| A call of `pieces` for 100 pieces on eight cores | 120 to 137 µs |
| 100,000 rows of 64 bytes, 12 µs of work each, sent to a helper of four cores | 425,000 to 446,000 rows a second, 216,000 to 220,000 as one job a row |
| 4,000 jobs of 2 ms on three helpers of 15 cores | 181 ms, where every core busy all the time would take 173 |
| 200 camera frames worked on by another machine's GPU | 182 ms, against about 950 on this machine's two cores |

On a board the core and timers are 3,048 bytes, and a message on its serial line is 12 to 15 bytes.

## Try it

Map alph in your program's `mettle.deps`, then pick a port when you build:

```text
alph=<path to alph>/src
```

```bash
mettle --build -s --release examples/count.mettle -I ports/host -o count
```

Build with `-s`: without it the compiler turns a failed check (a bad index, a division by zero) into an exit that alph cannot catch.

Work spread over four cores, [examples/spread.mettle](examples/spread.mettle):

```bash
mettle --build -s --release examples/spread.mettle -I ports/host -o spread
```

A list of commands run on two machines, as a build would, [examples/run.mettle](examples/run.mettle): each goes to a core here or to the other machine, and its exit status comes back. It takes the same arguments as `far`:

```bash
mettle --build -s --release examples/run.mettle -I ports/host -o run
```

Tiles of a picture rendered on cores or on the GPU, whichever should finish each first, with the same pixels either way, [examples/render.mettle](examples/render.mettle). Without a GPU it runs on the cores:

```bash
mettle --build -s --release examples/render.mettle -I ports/host -o render
```

The same program on every machine, [examples/share.mettle](examples/share.mettle): each knows how to do the work and shares it, and the one started as the boss hands it out. This is the short way to use alph, and [docs/start.md](docs/start.md) begins with it. With no arguments two machines run in one process:

```bash
mettle --build -s --release examples/share.mettle -I ports/host -o share
```

A picture drawn by several machines, [examples/picture.mettle](examples/picture.mettle): each row is a job, its number goes out and its pixels come back as bytes. The helper has a GPU way for the same work, so a machine with no GPU uses another machine's: of 1,200 rows the boss draws about 320 on its own cores and sends about 860 to the helper, which draws about 800 of them on its GPU, and every pixel is the same as when one core draws it all. It takes the same arguments as `share`:

```bash
mettle --build -s --release examples/picture.mettle -I ports/host -o picture
```

A camera's frames worked on by another machine's GPU, [examples/frames.mettle](examples/frames.mettle): each frame is a job that carries its 61,440 bytes and gives back as many, the frame made smooth. One function is the work on a core and on the GPU. The boss has two working cores and no GPU: of 200 frames it sends about 180 to the helper, which does nearly all of them on its GPU, in about 190 ms. On its own two cores they take about 950 ms. Every frame is the same as when a core here does it. It takes the same arguments as `share`:

```bash
mettle --build -s --release examples/frames.mettle -I ports/host -o frames
```

A picture rendered on the cores and the GPU of one machine, the short way too, [examples/tiles.mettle](examples/tiles.mettle). One function is the work, a core and the GPU both call it, and the pixels are the same either way. Without a GPU it runs on the cores:

```bash
mettle --build -s --release examples/tiles.mettle -I ports/host -o tiles
```

The layers under it, used directly: work shared with another machine, [examples/far.mettle](examples/far.mettle). With no arguments both machines run in one process. On two computers, start `./far helper <the boss's address>` on one and `./far boss <the helper's address>` on the other. If the other machine is not there, or is lost on the way, the work is done here:

```bash
mettle --build -s --release examples/far.mettle -I ports/host -o far
```

An echo server and a client over TCP, with an actor for each connection, [examples/talk.mettle](examples/talk.mettle):

```bash
mettle --build -s --release examples/talk.mettle -I ports/host -o talk
```

For 64-bit ARM Linux, such as a Raspberry Pi or a Jetson, build on that machine with the ARM port in front of the Linux one:

```bash
mettle --build -s --release examples/share.mettle --target aarch64-linux -I ports/arm64 -I ports/host -o share
```

Two actors and a timer on a bare 32-bit RISC-V board, in a 3.3 KB image, [examples/mcu/blink.mettle](examples/mcu/blink.mettle):

```bash
mettle --release examples/mcu/blink.mettle --target riscv32-none --board qemu-virt --emit-flat blink.bin -I ports/mcu
qemu-system-riscv32 -M virt -bios none -nographic -device loader,file=blink.bin,addr=0x80000000,force-raw=on
```

A board and a Linux process on its serial line, [examples/mcu/tether.mettle](examples/mcu/tether.mettle) and [examples/mcu/tether_host.mettle](examples/mcu/tether_host.mettle). The board's image is 3.5 KB, and a message is 12 to 15 bytes on the line:

```bash
mettle --release examples/mcu/tether.mettle --target riscv32-none --board qemu-virt --emit-flat tether.bin -I ports/mcu
mettle --build -s --release examples/mcu/tether_host.mettle -I ports/host -o tether_host
./tether_host 7421 &
qemu-system-riscv32 -M virt -bios none -display none -monitor none -serial tcp:127.0.0.1:7421 -device loader,file=tether.bin,addr=0x80000000,force-raw=on
```

The same programs for a Raspberry Pi Pico 2 (its RISC-V cores). They have run on the compiler's model of the chip, not yet on a real board:

```bash
mettle --release examples/mcu/blink.mettle --target riscv32-none --board pico2 --emit-flat blink.uf2 -I ports/pico2 -I ports/mcu
```

Both harts of one board, [examples/mcu/harts.mettle](examples/mcu/harts.mettle), the same `AlphCores` as on Linux:

```bash
mettle --release examples/mcu/harts.mettle --target riscv32-none --board qemu-virt --isa rv32imac --emit-flat harts.bin -I ports/harts -I ports/mcu
qemu-system-riscv32 -M virt -smp 2 -bios none -nographic -device loader,file=harts.bin,addr=0x80000000,force-raw=on
```

Two boards on one serial line, with no other machine: [examples/mcu/asker.mettle](examples/mcu/asker.mettle) asks the board above for numbers. Each board's actor reaches the other's by its pid, so a device with several chips is one program. `check.sh` runs the pair in two QEMUs.

A computer and a board working as one machine, [examples/mcu/arm.mettle](examples/mcu/arm.mettle) and [examples/mcu/arm_host.mettle](examples/mcu/arm_host.mettle): the chip in a robot's arm, say, on the serial line of the computer that plans. The computer asks for joint angles, which only the board can read, and for primes, which both can count. It never says where either is done. Every angle comes from the board, and the board counts some of the primes. It also asks for five moves of the arm, a kind that is done at most once: the board remembers what it did, so a move sent again because its answer was lost is answered and not made again. The function that counts is one file built into both programs. The board's image is 5.5 KB:

With a real board in place of QEMU, name its serial device in place of the port: `./arm_host /dev/ttyUSB0`. A program of your own opens it with `alph_serial_open("/dev/ttyUSB0", 115200)` and gives it to `alph_line_open`. [docs/hardware.md](docs/hardware.md) has the wiring and the order to try things in.

```bash
mettle --release examples/mcu/arm.mettle --target riscv32-none --board qemu-virt --emit-flat arm.bin -I ports/mcu
mettle --build -s --release examples/mcu/arm_host.mettle -I ports/host -o arm_host
./arm_host 7461 &
qemu-system-riscv32 -M virt -bios none -display none -monitor none -serial tcp:127.0.0.1:7461 -device loader,file=arm.bin,addr=0x80000000,force-raw=on
```

Two boards that share work with no computer at all, [examples/mcu/two.mettle](examples/mcu/two.mettle): the two chips of one arm, joined by a serial line. Each does one kind of work for the other and asks the other for the kind it cannot do. Neither knows the other's actors: a board asks for a kind and alph finds who does it down the line. One file is the program for both boards. `check.sh` runs the pair in two QEMUs:

```bash
mettle --release examples/mcu/two_a.mettle --target riscv32-none --board qemu-virt --emit-flat two_a.bin -I ports/mcu
mettle --release examples/mcu/two_b.mettle --target riscv32-none --board qemu-virt --emit-flat two_b.bin -I ports/mcu
```

## What it does not do yet

- Nothing here has run on real hardware other than an x86-64 computer. The boards run in QEMU and, for the Pico 2, on the compiler's model of its chip. The ARM Linux programs run under an emulator. [docs/hardware.md](docs/hardware.md) is the order to try a real Pico 2 in.
- On ARM Linux a handler that crashes ends the process.
- A sealed link has one key for the whole network: any holder can speak as any machine, and someone who gets the key later can read what they recorded before.
- A job for another machine carries at most 1,024 bytes and gives back at most 1,016, unless the machines are given long buffers. Then the most is what one buffer holds.
- Sockets are Linux only, and there is no file IO.
- A board keeps no record of its last messages.
- A message between two cores costs 150 to 300 ns against about 20 ns inside a node. Lanes bring a stream of them down to about 70 ns, but a single request and its reply are no quicker.
- A microcontroller joins through a serial line to one machine, and a machine has at most four such lines. What is lost on that line is sent again only when both ends make the line sure, which costs a board about 1.1 KB.
- A board's jobs are one word in and one word out, one at a time.
- A job of a kind done at most once carries a word and gives a word back, and when its worker is lost the program is told it cannot know, not what happened.

## Test

```bash
./check.sh
```

It builds every test and example in debug and release and runs them, several at a time. Then it runs the board programs in QEMU, fails if the smallest image is over 5 KB, builds everything for ARM Linux, and checks that the two reference pages match the source. It takes about two minutes on a 16-core machine. `./check.sh --quick` runs only the new layers in release, in under a minute, and `./check.sh tests/t_name` runs one program.

## The first runtime

Everything under `src/core`, `src/heal`, `src/work`, `src/net`, `src/io`, `src/sched` and `src/sys` is the runtime alph started as: Erlang-style actors for x86-64 Linux with supervisors, medics, a heap per actor, non-blocking IO and TCP, child processes, a worker pool and Noise-encrypted distribution, in one import (`alph/alph`). It works and its tests run. It is the reference while its parts are rebuilt on the small core, and each part goes when its replacement does the same job.

- [Guide](docs/guide.md): how to use the first runtime.
- [API reference](docs/api.md): every function, type and constant of the first runtime.

## Layout

```text
src/actor/actor.mettle      the core: actors, messages, the run loop
src/cores/cores.mettle      the core on several threads, one node each
src/cores/lanes.mettle      lanes: a lock-free way between each pair of cores
src/time/time.mettle        timers: a message sent later
src/time/wheel.mettle       a timer wheel, for thousands of timers
src/place/place.mettle      places: a job goes to a core, the GPU or another machine
src/poll/poll.mettle        polls: a socket sends a message when it is ready
src/life/life.mettle        lives: watching and keeping actors
src/record/record.mettle    a record: the last messages a node ran, for after a crash
src/link/link.mettle        links: other machines, over UDP
src/name/name.mettle        names: a number for a pid
src/door/door.mettle        the door: find and watch actors on other machines
src/machine/machine.mettle  a machine: every layer set up in one call, and work shared between machines
src/batch/batch.mettle      a batch: one kind of work handed out in pieces
src/seal/seal.mettle        seals: an encrypted and signed link (needs the cryptex package)
src/tether/tether.mettle    tether: a board and a bigger machine on a serial line
src/board/board.mettle      a board: a microcontroller that takes work from bigger machines
                            (once and asks: work done at most once; irq: interrupts as messages)
ports/mcu, ports/harts, ports/pico2, ports/host, ports/arm64
                            what differs per device, picked with -I ports/<name>
src/alph.mettle             the first runtime in one import
src/core, src/heal, src/work, src/net, src/io, src/sched, src/sys
                            the first runtime
docs/start.md               how to use the new alph, layer by layer
docs/design.md              the design of the new alph, with what was measured
docs/reference.md           every struct, constant and function of the new alph, built from the source
docs/hardware.md            the first run on a real Pico 2, step by step
docs/guide.md, docs/api.md  the first runtime's guide and reference
examples/                   small programs, run by check.sh
tests/                      self-checking test programs
tools/                      apidoc.sh, which builds docs/api.md and docs/reference.md
```

## License

Apache License 2.0. See [LICENSE](LICENSE).
