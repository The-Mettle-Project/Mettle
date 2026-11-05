# sysinfo API

Import `sysinfo`. Everything is prefixed `sysinfo_`, `Sysinfo` or `SYSINFO_`.
Names with `sysinfo_lx_` belong to the Linux backend and are not part of the
API, even though Mettle makes them visible.

## Status codes

Every function that can fail returns one of these.

| Code | Value | Meaning |
|---|---|---|
| `SYSINFO_OK` | 0 | |
| `SYSINFO_E_UNSUPPORTED` | -1 | the platform has no backend yet (Windows, macOS) |
| `SYSINFO_E_INVALID` | -2 | a bad argument |
| `SYSINFO_E_NOMEM` | -3 | out of memory |
| `SYSINFO_E_IO` | -4 | a section's main source could not be read |
| `SYSINFO_E_NOTFOUND` | -5 | no such process |
| `SYSINFO_E_PERM` | -6 | not allowed |
| `SYSINFO_E_NOTOPEN` | -7 | the Sysinfo was not opened |

`sysinfo_strerror(code) -> string` gives a short English description.

## Missing values

A number that could not be read is `SYSINFO_NA` (-1, or -1.0 for rates and
percentages). A temperature that could not be read is `SYSINFO_NA_TEMP`
(-273.15). A string that could not be read is empty. A section whose main
source is missing sets its `status` to `SYSINFO_E_IO` and leaves its list
empty; the other sections are still filled.

## The sampler

```mettle
fn sysinfo_open(s: Sysinfo*, what: uint32) -> int32
fn sysinfo_open_at(s: Sysinfo*, what: uint32, root: string) -> int32
fn sysinfo_refresh(s: Sysinfo*, what: uint32) -> int32
fn sysinfo_close(s: Sysinfo*)
```

`what` is a set of section flags: `SYSINFO_SYSTEM`, `SYSINFO_CPU`,
`SYSINFO_MEM`, `SYSINFO_DISK`, `SYSINFO_NET`, `SYSINFO_PROC`,
`SYSINFO_SENSORS`, `SYSINFO_GPU`, `SYSINFO_BATTERY`, or `SYSINFO_ALL`. Four
extras turn on costly details: `SYSINFO_PROC_IO` (per-process I/O, which
usually needs root for other users' processes), `SYSINFO_PROC_FILES`
(per-process open file count), `SYSINFO_PROC_SHARED` (per-process shared
memory, one more file read per process) and `SYSINFO_GPU_NVSMI` (run `nvidia-smi`, which
can wake a sleeping NVIDIA GPU).

`sysinfo_open` sets up the sections and takes a first sample. Rates and
percentages need two samples, so they are 0 until the first
`sysinfo_refresh`. Refreshing a section that was not opened sets it up first.
`sysinfo_refresh` returns the first error met but always refreshes every
section asked for. `sysinfo_close` releases everything; closing twice is safe,
and a closed Sysinfo can be opened again.

`sysinfo_open_at` reads `/proc`, `/sys`, `/etc`, `/run` and `/usr/share`
under `root` instead of `/`. With a root set, nothing is asked of the kernel
directly: file system sizes (statfs), interface addresses (netlink) and
`nvidia-smi` are left out. Tests use it with a fake tree.

The sampler is not thread safe: use one Sysinfo per thread, or a lock.

Each string in the data stays valid until the second refresh of its section
after the one that set it, and until `sysinfo_close`. Pointers to list
entries (`s.procs.list`, `s.disk.fs` ...) stay valid until the next refresh
of their section.

## Sysinfo

```mettle
struct Sysinfo {
  what: uint32;              // sections opened
  system: SysinfoSystem;
  cpu: SysinfoCpu;
  mem: SysinfoMem;
  disk: SysinfoDisk;
  net: SysinfoNet;
  procs: SysinfoProcs;
  sensors: SysinfoSensors;
  gpus: SysinfoGpus;
  power: SysinfoPower;
}
```

Every section starts with `status: int32`.

### SysinfoSystem

| Field | Type | Notes |
|---|---|---|
| `hostname` | string | read on every refresh |
| `os_name`, `os_version`, `os_id`, `os_pretty` | string | from os-release: NAME, VERSION_ID, ID, PRETTY_NAME |
| `kernel_name`, `kernel_version` | string | "Linux", "6.12.115+deb13.1-amd64" |
| `arch` | string | "x86_64", "aarch64" |
| `uptime_s` | float64 | seconds |
| `boot_time` | int64 | Unix seconds |
| `load1`, `load5`, `load15` | float64 | |
| `users` | int32 | login sessions (utmp, or systemd-logind sessions where there is no utmp) |
| `procs_running`, `procs_total` | int32 | tasks, threads included |

### SysinfoCpu

| Field | Type | Notes |
|---|---|---|
| `model`, `vendor` | string | "AMD Ryzen 7 8845HS ...", "AuthenticAMD"; on ARM the vendor comes from the implementer code |
| `logical` | int32 | online logical CPUs; also the length of `cores` |
| `physical`, `packages` | int32 | cores and sockets, from the topology; -1 when unknown |
| `total` | SysinfoCpuUsage | the whole machine |
| `cores` | SysinfoCore* | one per online logical CPU, in CPU order |
| `pkg_temp_c`, `temp_crit_c` | float64 | package temperature and its critical mark |
| `ctx_switches`, `interrupts` | int64 | since boot |
| `ctx_switches_s`, `interrupts_s` | float64 | per second |

`SysinfoCpuUsage` holds percentages of the last interval: `busy` (everything
but idle and iowait), `user`, `nice`, `system`, `irq`, `softirq`, `iowait`,
`idle`, `steal` and `guest`. The first eight add up to 100. Guest time is also
inside `user`, as the kernel counts it.

`SysinfoCore`: `cpu` (number), `core_id`, `package_id`, `usage`, `freq_mhz`,
`freq_min_mhz`, `freq_max_mhz`, `temp_c` (its core's temperature, on Intel).

### SysinfoMem

All sizes in bytes: `total`, `used`, `available`, `free`, `buffers`,
`cached`, `shared`, `swap_total`, `swap_used`, `swap_free`. Also `used_pct`,
`swap_used_pct`, and `swap_in_s`, `swap_out_s` in bytes per second. `used` is
`total - available`; `cached` is the page cache plus reclaimable slab, as
free(1) shows it.

### SysinfoDisk

`fs: SysinfoFs*` with `n_fs`, and `dev: SysinfoBlock*` with `n_dev`.

`SysinfoFs`: `device`, `mount`, `fstype`, `options` (strings), `read_only`,
`total`, `used`, `free` (free for root), `avail` (free for others),
`used_pct` (used / (used + avail), as df shows it), `inodes_total`,
`inodes_used`, `inodes_free`. Virtual file systems (proc, sysfs, tmpfs ...)
are left out.

`SysinfoBlock`: `name`, `partition`, `reads`, `writes`, `read_bytes`,
`write_bytes` (since boot), and `read_bps`, `write_bps`, `read_iops`,
`write_iops`, `busy_pct`. Devices that never did any I/O are left out.

### SysinfoNet

`ifs: SysinfoNetIf*` with `n_ifs`; socket counts `tcp`, `tcp6`,
`tcp_time_wait`, `tcp_orphan`, `udp`, `udp6`.

`SysinfoNetIf`: `name`, `index`, `mac` (empty when none), `mtu`, `up`
(administratively), `running` (link up), `loopback`, `speed_mbps` (-1 when
unknown, as on Wi-Fi), `addrs: SysinfoAddr*` with `n_addrs` (IPv4 first, then
IPv6), counters `rx_bytes`, `tx_bytes`, `rx_packets`, `tx_packets`,
`rx_errors`, `tx_errors`, `rx_drops`, `tx_drops`, and the same per second:
`rx_bps`, `tx_bps`, `rx_pps`, `tx_pps`, `rx_errors_s`, `tx_errors_s`,
`rx_drops_s`, `tx_drops_s`.

`SysinfoAddr`: `family` (4 or 6), `addr` (text; IPv6 per RFC 5952), `prefix`.

### SysinfoProcs

`list: SysinfoProc*` sorted by pid, with `n`; totals `threads`, `running`,
`sleeping`, `zombies`.

`SysinfoProc`:

| Field | Type | Notes |
|---|---|---|
| `pid`, `ppid` | int32 | |
| `name` | string | the kernel's name, made whole from the command line when the kernel cut it at 15 bytes |
| `cmdline` | string | arguments joined by spaces; empty for kernel threads |
| `exe` | string | empty when not allowed |
| `state` | uint8 | 'R', 'S', 'D', 'Z', 'T', 'I' ... |
| `uid`, `gid`, `user`, `group` | | real ids; the name, or the number when it has none |
| `nice`, `priority`, `threads` | int32 | |
| `start_time` | int64 | Unix seconds |
| `utime_s`, `stime_s` | float64 | CPU seconds used |
| `cpu_pct` | float64 | percent of one core (400 = four busy cores) |
| `cpu_total_pct` | float64 | percent of the whole machine |
| `rss`, `vms` | int64 | bytes |
| `shared` | int64 | bytes; with `SYSINFO_PROC_SHARED`, else -1 |
| `mem_pct` | float64 | RSS of total memory |
| `io_read_bytes`, `io_write_bytes` | int64 | storage I/O; with `SYSINFO_PROC_IO` |
| `io_read_bps`, `io_write_bps` | float64 | with `SYSINFO_PROC_IO` |
| `open_files` | int32 | with `SYSINFO_PROC_FILES` |
| `parent`, `first_child`, `next_sibling` | int32 | the tree, as indexes into `list` (-1 for none); children come in pid order |

A process is matched between refreshes by pid and start time, so a reused pid
never inherits the old process's CPU time.

### SysinfoSensors

`list: SysinfoSensor*` with `n`. `SysinfoSensor`: `chip` ("coretemp",
"nvme"), `label` (the driver's label, or "temp1"), `kind`, `value`, `min`,
`max`, `crit`. Kinds and units: `SYSINFO_SENSOR_TEMP` (°C),
`SYSINFO_SENSOR_FAN` (RPM), `SYSINFO_SENSOR_VOLT` (V), `SYSINFO_SENSOR_POWER`
(W), `SYSINFO_SENSOR_CURRENT` (A). A sensor whose read takes more than 5 ms
(NVMe drives, SPD hubs) is read at most every 5 seconds.

### SysinfoGpus

`list: SysinfoGpu*` with `n`. `SysinfoGpu`: `vendor` (`SYSINFO_GPU_AMD`,
`SYSINFO_GPU_INTEL`, `SYSINFO_GPU_NVIDIA`, `SYSINFO_GPU_OTHER`),
`vendor_name`, `name` (from pci.ids when installed), `driver`, `pci_slot`,
`util_pct`, `mem_used`, `mem_total` (bytes), `temp_c`, `power_w`,
`power_cap_w`, `clock_core_mhz`, `clock_mem_mhz`.

What each vendor gives without `nvidia-smi`: AMD everything; Intel the core
clock, temperature and power (power from the second refresh, on dedicated
cards with hwmon), no utilisation; NVIDIA the name only.

### SysinfoPower

`ac_online` (1, 0, or -1 when there is no mains entry) and `list:
SysinfoBattery*` with `n`. Peripheral batteries (mice, headsets) are left
out.

`SysinfoBattery`: `name`, `present`, `status` (`SYSINFO_BAT_UNKNOWN`,
`_CHARGING`, `_DISCHARGING`, `_FULL`, `_NOT_CHARGING`), `capacity_pct`,
`rate_w` (always positive), `voltage_v`, `energy_wh`, `energy_full_wh`,
`energy_design_wh`, `health_pct` (full / design), `time_to_empty_s`,
`time_to_full_s`, `cycles`, `technology`, `model`, `manufacturer`.

## Process helpers

```mettle
fn sysinfo_proc_index(p: SysinfoProcs*, pid: int32) -> int32
fn sysinfo_proc_find(p: SysinfoProcs*, pid: int32) -> SysinfoProc*
fn sysinfo_proc_children(p: SysinfoProcs*, pid: int32, out: int32*, max: int32) -> int32
fn sysinfo_proc_descendants(p: SysinfoProcs*, pid: int32, out: int32*, max: int32) -> int32
fn sysinfo_proc_filter_init(f: SysinfoProcFilter*)
fn sysinfo_proc_filter(p: SysinfoProcs*, f: SysinfoProcFilter*, out: int32*, max: int32) -> int32
fn sysinfo_proc_sort(p: SysinfoProcs*, idx: int32*, n: int32, key: int32, descending: bool) -> int32
fn sysinfo_signal(pid: int32, sig: int32) -> int32
```

`sysinfo_proc_find` returns 0 when there is no such pid. The functions that
fill `out` write at most `max` indexes and return the full count.
`sysinfo_proc_descendants` walks depth first.

`SysinfoProcFilter` keeps processes that match all of: `text` (a substring of
the name or command line, any case), `uid`, `ppid`, `state`, `min_cpu_pct`,
`min_rss`. `sysinfo_proc_filter_init` makes a filter that keeps everything
(`uid` and `ppid` -1 mean any).

`sysinfo_proc_sort` sorts an index array by `SYSINFO_SORT_PID`, `_NAME`,
`_CPU`, `_MEM`, `_USER`, `_THREADS`, `_START` or `_IO`. The sort is stable
and ties stay in pid order. It returns `SYSINFO_E_INVALID` if an index is out
of range.

```mettle
var idx: int32* = (int32*)malloc((int64)s.procs.n * 4);
for i in 0..s.procs.n { idx[i] = i; }
sysinfo_proc_sort(&s.procs, idx, s.procs.n, SYSINFO_SORT_CPU, true);
```

`sysinfo_signal` sends a signal (15 is SIGTERM, 9 is SIGKILL, 0 only checks
the process exists) and returns `SYSINFO_E_NOTFOUND` or `SYSINFO_E_PERM` when
that is why it failed.
