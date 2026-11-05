# References

## Standards and kernel interfaces

| Area | Source | Used for |
|---|---|---|
| procfs | proc(5); kernel `Documentation/filesystems/proc.rst` | `/proc/stat`, `cpuinfo`, `meminfo`, `vmstat`, `loadavg`, `uptime`, `mounts`, `filesystems`, `net/dev`, `net/sockstat{,6}`, `<pid>/{stat,statm,status,cmdline,exe,io,fd}` |
| Disk I/O | kernel `Documentation/admin-guide/iostats.rst` | `/proc/diskstats` fields; sectors are always 512 bytes |
| CPU frequency | kernel `Documentation/admin-guide/pm/cpufreq.rst` | `scaling_cur_freq`, `cpuinfo_{min,max}_freq` (kHz) |
| CPU topology | kernel `Documentation/admin-guide/cputopology.rst` | `topology/core_id`, `physical_package_id` |
| Sensors | kernel `Documentation/hwmon/sysfs-interface.rst` | input names, units (millidegree, millivolt, microwatt, milliampere), `_label`, `_min`, `_max`, `_crit`, `_cap` |
| Thermal zones | kernel `Documentation/driver-api/thermal/sysfs-api.rst` | fallback package temperature |
| Power supplies | kernel `Documentation/ABI/testing/sysfs-class-power` | `type`, `status`, `capacity`, `energy_*`, `charge_*`, `power_now`, `current_now`, `voltage_now`, `cycle_count`, `scope` |
| Network devices | kernel `Documentation/ABI/testing/sysfs-class-net` | `address`, `mtu`, `ifindex`, `flags`, `operstate`, `speed` |
| Addresses | rtnetlink(7), netlink(7), RFC 3549 | RTM_GETADDR dump, IFA_ADDRESS and IFA_LOCAL |
| IPv6 text | RFC 5952 | the canonical form of IPv6 addresses (tests use its section 4 examples) |
| Mount escapes | getmntent(3) | `\040`-style octal escapes in `/proc/self/mounts` |
| File system sizes | statfs(2) | block counts, fragment size, `ST_RDONLY` |
| OS release | os-release(5) | `NAME`, `VERSION_ID`, `ID`, `PRETTY_NAME`, quoting |
| Logins | utmp(5); systemd-logind | `USER_PROCESS` records; `/run/systemd/sessions` where there is no utmp |
| AMD GPUs | kernel `Documentation/gpu/amdgpu/` (thermal, driver-misc) | `gpu_busy_percent`, `mem_info_vram_{used,total}`, hwmon `temp1`, `power1_average`, `power1_cap`, `freq1`, `freq2` |
| Intel GPUs | i915 and xe sysfs | `gt_cur_freq_mhz`, `tile0/gt0/freq0/act_freq`, hwmon `energy1_input` |
| NVIDIA GPUs | NVIDIA driver README (procfs interface); `nvidia-smi --help-query-gpu` | `/proc/driver/nvidia/gpus/<slot>/information`; CSV query fields |
| PCI names | the PCI ID Repository (pci.ids format) | vendor and device names, read from the installed file |

## Libraries studied

No code was copied from any of them; these are the ideas taken.

**psutil** (Python, BSD-3-Clause)
- Leave out file systems that `/proc/filesystems` marks `nodev`, but keep zfs.
- When the kernel's 15-byte process name is cut, take the full name from
  the first argument of the command line if it starts with the cut name.
- The first CPU percentage is 0, not an average since boot.
- `used_pct` of a file system is used / (used + available to users), as df.
- Estimate available memory from free + buffers + cached on kernels without
  `MemAvailable`.

**Rust sysinfo** (MIT)
- A refresh-based API where the caller picks what to refresh and rates come
  from the change between refreshes.
- Keep files that are read on every refresh open and read them again from
  the start (Rust sysinfo does this with per-process `stat` files); here it
  is done for every per-tick file except the per-process ones.
- Memory "used" is total minus available.

**gopsutil** (Go, BSD-3-Clause)
- Point the whole library at another root (`HOST_PROC`, `HOST_SYS`); here
  `sysinfo_open_at`, which also makes the fixture tests possible.
- Sensors as every hwmon `*_input` with its `*_label` and limits.
- Logged-in users from utmp `USER_PROCESS` records.

**btop** (C++, Apache-2.0)
- Read a process's command line once and keep it while the process lives.
- CPU temperature: `coretemp` package and per-core labels mapped to logical
  CPUs through the topology, `k10temp` and `zenpower` `Tctl`/`Tdie` (the
  real die temperature wins), and `cpu_thermal` on ARM boards.
- AMD GPU figures from amdgpu's sysfs files.
- Read only what changes on each tick.

## Choices made here

- **Kept-open files and `pread` at offset 0** for every file read on each
  refresh, instead of open/read/close.
- **Process identity = pid + start time**, so a reused pid starts fresh.
- **Two string pools per section, used in turn**, so refreshes allocate
  nothing in steady state and views outlive one refresh.
- **Slow sensors are rate-limited.** NVMe temperature reads took 5 to 46 ms
  each on the test laptop (the drive answers an admin command and may wake
  up), which made sensors cost 79 ms per refresh. An input slower than 5 ms
  is now read at most every 5 seconds; sensors cost under 1 ms.
- **Socket counts from `sockstat`**, which the kernel keeps as counters,
  instead of reading and counting every line of `/proc/net/tcp`.
- **Addresses through netlink**, which gives IPv4 and IPv6 with prefixes in
  one dump; procfs has no per-interface IPv4 file.
- **nvidia-smi only on request**, since it costs tens of milliseconds and
  wakes a GPU that is powered down.
- **Users from logind** when `/run/utmp` does not exist (newer Debian and
  others dropped it); manager and background sessions are not counted, which
  matches who(1).

## Benchmark

`mbuild bench`, release build, Ryzen 7 8845HS (16 threads), Linux 6.12,
545 processes, 5 file systems, 10 interfaces, 14 sensors:

| Refresh | Time |
|---|---|
| system | 16-36 µs |
| CPU | 37-82 µs |
| memory | 20-47 µs |
| disks | 57-135 µs |
| network | 0.4-0.8 ms |
| processes | 4.4-5 ms (6-12 ms with `SYSINFO_PROC_SHARED`) |
| sensors | 0.7 ms |
| GPUs | 4-13 µs |
| batteries | 0.14-0.38 ms |
| all | 19-24 ms |

The ranges are from separate runs on a busy desktop.

psutil 7.0 on the same machine, the same information (`process_iter` with
name, command line, exe, status, user, nice, threads, start, CPU and memory,
plus every system-wide call): 111 ms for processes and 299 ms for
everything; this library is about 12 times faster.
