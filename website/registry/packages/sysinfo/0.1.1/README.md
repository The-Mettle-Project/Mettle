# sysinfo

System information for [Mettle](https://github.com/the-mettle-project/Mettle) programs: host and OS,
CPU, memory, disks, network, processes, hardware sensors, GPUs and batteries,
with per-second rates. Written in pure Mettle, with no C.

Linux is implemented. The library is built so that Windows and macOS backends
can be added without changing the public API; until then every call on those
platforms returns `SYSINFO_E_UNSUPPORTED`.

```mettle
import "std/io";
import "std/time";
import "sysinfo";

fn main() -> int32 {
  var s: Sysinfo;
  sysinfo_open(&s, (uint32)SYSINFO_ALL);
  while (true) {
    time_sleep_ms(1000);
    sysinfo_refresh(&s, (uint32)(SYSINFO_CPU | SYSINFO_MEM | SYSINFO_NET));
    println("cpu {s.cpu.total.busy}%  mem {s.mem.used_pct}%");
    for i in 0..s.net.n_ifs {
      var f: SysinfoNetIf* = &s.net.ifs[i];
      println("  {f.name}: rx {f.rx_bps} B/s, tx {f.tx_bps} B/s");
    }
  }
  return 0;
}
```

## What it gives

| Section | Flag | Contents |
|---|---|---|
| System | `SYSINFO_SYSTEM` | host name, OS name and version, kernel, architecture, uptime, boot time, load averages, logged-in users, task counts |
| CPU | `SYSINFO_CPU` | model, vendor, logical/physical cores, sockets; total and per-core usage (user, nice, system, irq, softirq, iowait, idle, steal, guest); per-core frequency with limits; package and per-core temperature; context switches and interrupts per second |
| Memory | `SYSINFO_MEM` | total, used, available, free, buffers, cached, shared; swap total/used/free; swap in/out per second |
| Disks | `SYSINFO_DISK` | mounted file systems (device, mount point, type, options, read-only, sizes, inodes); per block device bytes and operations per second and busy % |
| Network | `SYSINFO_NET` | interfaces (MAC, IPv4 and IPv6 addresses, MTU, up/running, speed) with counters and per-second rates; TCP and UDP socket counts |
| Processes | `SYSINFO_PROC` | pid, parent, name, command line, executable, state, user and group, nice and priority, threads, start time, CPU %, memory (RSS, virtual, %); a parent/child tree; with `SYSINFO_PROC_IO`, `SYSINFO_PROC_FILES` and `SYSINFO_PROC_SHARED` also I/O per second, open files and shared memory |
| Sensors | `SYSINFO_SENSORS` | every hwmon temperature, fan, voltage, power and current input, with labels and limits |
| GPUs | `SYSINFO_GPU` | vendor, name, driver, PCI slot, utilisation, video memory, temperature, power, clocks (AMD and Intel through sysfs, NVIDIA through `/proc/driver/nvidia` and, with `SYSINFO_GPU_NVSMI`, `nvidia-smi`) |
| Battery | `SYSINFO_BATTERY` | mains online; per battery: status, capacity, rate, voltage, energy, health, time to empty/full, cycle count |

Helpers: `sysinfo_proc_find`, `sysinfo_proc_children`, `sysinfo_proc_descendants`,
`sysinfo_proc_filter`, `sysinfo_proc_sort` and `sysinfo_signal`.

## Rules worth knowing

- **Two samples make a rate.** Percentages and per-second values come from the
  difference between the last two refreshes, timed with the monotonic clock.
  After `sysinfo_open` they read 0 until the next `sysinfo_refresh`.
- **Missing data is per field.** A value that cannot be read (no permission,
  no sensor) is `SYSINFO_NA` (-1) or, for temperatures, `SYSINFO_NA_TEMP`;
  a string is empty. Every call returns a status code, and each section has
  its own `status`. Nothing crashes on odd input; the parsers are fuzzed.
- **Strings are borrowed.** Each string stays valid until the second refresh
  of its section after the one that set it. Copy what you keep longer.
- **Refreshing is cheap.** Files are kept open and re-read, buffers are reused,
  and slow-changing facts (command lines, user names, sensor paths) are read
  once. See the benchmark below.

## Using it

Add it to `mbuild.mettle` and import `sysinfo`:

```mettle
fn deps() {
  mb_dep("sysinfo", "0.1.0");      // or mb_dep_path("sysinfo", "../sysinfo")
}
```

`examples/demo.mettle` prints a report of the machine (`mbuild run sysinfo_demo`).
The full reference is in [docs/api.md](docs/api.md); how it is built and how to
add a platform is in [docs/design.md](docs/design.md); the standards and the
libraries it learned from are in [docs/references.md](docs/references.md).

## Tests and benchmark

`mbuild test` (and `mbuild test -r` for release) runs:

- `fixture`: every section read from a fake `/proc`, `/sys`, `/etc` and `/run`
  tree in `tests/fixtures/linux` (made by `tools/make_fixtures.py`), checked
  value by value;
- `rates`: a tree changed between two refreshes, checking percentages exactly
  and per-second rates by their ratios;
- `parse`: the parsers on fixed input, including RFC 5952 IPv6 examples and
  hand-built rtnetlink messages;
- `fuzz`: random and mutated input to every parser;
- `live`: a smoke test on the machine running it.

`mbuild bench` times each section's refresh. On a 16-thread Ryzen 7 8845HS laptop
with 545 processes, a full refresh of every section takes about 20 ms, most of
it the process list (about 8 to 9 µs per process). psutil 7.0 takes about 300 ms for
the same information on the same machine.

## License

Apache-2.0. See [LICENSE](LICENSE).
