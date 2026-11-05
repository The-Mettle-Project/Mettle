# Design

## Layout

```
src/sysinfo.mettle               public functions: open, refresh, close, signal
src/sysinfo/types.mettle         public data: every struct, flag and code
src/sysinfo/procs.mettle         process helpers (find, tree, filter, sort)
src/sysinfo/os/none.mettle       the backend for platforms not done yet
src/sysinfo/os/linux/            the Linux backend
  backend.mettle                 entry points, one state struct, section order
  ctx.mettle                     shared state: root, buffers, clock tick, page size
  util.mettle                    file reads, text scanner, paths, pools, sorted listings
  system cpu mem disk net proc sensors gpu battery .mettle   one collector each
  arch_x86_64.mettle, arch_aarch64.mettle                    syscall numbers
```

The public API knows nothing about any operating system. `sysinfo.mettle`
picks a backend with guarded imports:

```mettle
import "sysinfo/os/linux/backend.mettle" if linux;
import "sysinfo/os/none.mettle" if windows;
import "sysinfo/os/none.mettle" if macos;
```

A backend exports four functions:

```mettle
fn sysinfo_os_open(s: Sysinfo*, what: uint32, root: string) -> int32
fn sysinfo_os_refresh(s: Sysinfo*, what: uint32) -> int32
fn sysinfo_os_close(s: Sysinfo*)
fn sysinfo_os_signal(pid: int64, sig: int32) -> int32
```

It keeps its private state behind `Sysinfo.os` and fills the public structs.

## Adding a platform

1. Make `src/sysinfo/os/windows/backend.mettle` (or `macos/`) with the four
   functions, and change the guarded import in `sysinfo.mettle`.
2. Fill the sections the platform can give; set the rest to the missing
   values. Use `SYSINFO_E_UNSUPPORTED` as a section's `status` when the
   platform has nothing for it.
3. Sources to start from:
   - Windows: `GetSystemTimes` and `NtQuerySystemInformation`
     (SystemProcessorPerformanceInformation) for CPU, `GlobalMemoryStatusEx`,
     `GetLogicalDriveStringsW` with `GetDiskFreeSpaceExW`, `DeviceIoControl`
     IOCTL_DISK_PERFORMANCE, `GetIfTable2` and `GetAdaptersAddresses`,
     `CreateToolhelp32Snapshot` with `OpenProcess`/`GetProcessTimes`/
     `GetProcessMemoryInfo`, `GetSystemPowerStatus`, PDH counters where a
     rate is needed. No WMI.
   - macOS: `sysctl` (hw.model, hw.ncpu, kern.boottime, vm.swapusage),
     `host_processor_info` and `host_statistics64` (Mach),
     `getfsstat`, IOKit (IOBlockStorageDriver statistics, IOPMPowerSource,
     SMC for sensors), `getifaddrs` with `sysctl` NET_RT_IFLIST2,
     `proc_listpids` and `proc_pidinfo`.
4. The fixture tests are Linux only; a new backend brings its own tests and
   reuses `tests/live_test.mettle`.

Mettle's stdlib already compiles for Windows and macOS; this library
cross-compiles for both today (`mettle --target x86_64-windows ...`).

## Sampling

Every section keeps what it needs from the last refresh (counters and the
monotonic time) and computes rates from the difference. A counter that went
backwards gives 0, never a huge number. Process rates use the pid plus start
time as identity.

## Cost

A 1-second refresh loop should cost next to nothing, so:

- Files read on every refresh (`/proc/stat`, `/proc/meminfo`, the hwmon
  inputs, the GPU files, cpufreq) are opened once and re-read with `pread`
  at offset 0. procfs and sysfs both serve fresh contents that way.
- One scratch buffer is reused by every collector and grows when a file
  outgrows it.
- Facts that do not change are read once: the CPU model, topology and
  frequency limits; which hwmon inputs exist; GPU and sensor names; user and
  group names; and for each process its command line, executable and ids.
- Sensors that answer slowly are read at most every 5 seconds (see
  `sensors.mettle`).
- Strings go into pairs of pools used in turn, so a refresh allocates nothing
  once the pools have grown, and the views handed out stay valid for one
  extra refresh.

`mbuild bench` prints the cost of each section.

## Strings and memory

Public strings are views. Each section owns two pools; a refresh empties
the older one and writes into it, copying strings it keeps (such as a
process's command line) from the newer one. A view is therefore good until
the second refresh of its section. Lists are arrays that grow and are reused.
`sysinfo_close` frees everything.

## Errors

A failed read never stops a refresh: the field gets its missing value and
the rest goes on. A section whose main source is missing reports
`SYSINFO_E_IO` in its `status`, and `sysinfo_refresh` returns the first such
code.
