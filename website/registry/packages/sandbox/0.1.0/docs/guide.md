# Guide

## The model

A sandbox is a child process that sets up its own limits between `clone`
and `exec`. The caller writes a `SandboxPolicy`; the program inside never
sees the policy and cannot change it. The aim is to stop a misbehaving
program from reading or changing what it was not given, reaching the
network, seeing or signalling host processes, gaining privileges, or using
kernel features it has no need for.

The limits stack, so one weak layer is covered by the others:

1. **Namespaces** give the program its own view of processes, mounts,
   network, hostname, IPC and users.
2. **The mount list** decides which host files exist for it at all, and
   which are read-only.
3. **rlimits** cap file descriptors, processes, file size, memory and CPU time.
4. **Capabilities** are all dropped, so even uid 0 inside has no special power.
5. **no_new_privs** stops setuid programs and file capabilities from
   granting anything at exec.
6. **Landlock** limits which files and TCP ports it may use, whatever the
   mounts and file permissions say.
7. **Seccomp** limits which system calls the program may make at all.

## Setup order in the child

| Step | `SANDBOX_STEP_*` | What happens |
|------|------------------|--------------|
| clone | `CLONE` | the child starts in the new namespaces |
| parent | `PARENT` | parent-death signal armed |
| id maps | `ID_MAP` | `setgroups` deny, then `uid_map` and `gid_map` |
| hostname | `HOSTNAME` | `sethostname` in the new UTS namespace |
| loopback | `LOOPBACK` | `lo` brought up in the new network namespace |
| mounts | `MOUNT` | `/` made private, then the new root built and entered |
| init | `INIT` | in a PID namespace, PID 1 forks the program and waits |
| rlimits | `RLIMIT` | each listed limit set |
| stdio | `STDIO` | new session, streams set up, other descriptors close-on-exec, `chdir` |
| caps | `CAPS` | bounding, ambient, effective, permitted, inheritable cleared |
| no_new_privs | `NO_NEW_PRIVS` | set |
| Landlock | `LANDLOCK` | the ruleset applied, with paths as the program sees them |
| seccomp | `SECCOMP` | the filter loaded, last, so setup itself is not filtered |
| exec | `EXEC` | `execve`, trying each `PATH` entry when argv[0] has no `/` |

A failure at any step ends the child before the program runs.
`sandbox_spawn` returns `-errno` and sets `failed_step`.

## Policies

```mettle
sandbox_policy_init(&p);                      // secure defaults
sandbox_policy_share(&p, SANDBOX_NS_NET);     // use the host network instead
sandbox_policy_ids(&p, 0, 0);                 // look like root inside
sandbox_policy_hostname(&p, "worker-3");
sandbox_policy_env(&p, "PATH=/usr/bin:/bin"); // first call replaces the default
sandbox_policy_env(&p, "LANG=C.UTF-8");
sandbox_policy_cwd(&p, "/work");
sandbox_policy_rlimit(&p, SANDBOX_RLIMIT_AS, 1 << 30, 1 << 30);
sandbox_policy_keep_caps(&p, 0);              // the default
sandbox_policy_seccomp(&p, &filter);
sandbox_policy_landlock(&p, &rules);
sandbox_policy_stdio(&p, SANDBOX_IO_NULL, SANDBOX_IO_PIPE, SANDBOX_IO_INHERIT);
```

Builder calls that get bad input return `-SANDBOX_EINVAL` and remember the
error; `sandbox_spawn` then refuses to start. Strings given to the policy are
kept as views, so they must live until `sandbox_spawn` returns.

## Mounts

Without mounts the program sees the host's files (still under the other
limits). With at least one, it gets an empty read-only root holding only:

```mettle
sandbox_mount_system(&p.mounts);                     // /usr /bin /lib /etc ro; /proc /dev /tmp
sandbox_mount_ro(&p.mounts, "/srv/data", "/data");   // read-only
sandbox_mount_rw(&p.mounts, "/srv/out", "/out");     // writable: changes reach the host
sandbox_mount_tmpfs(&p.mounts, "/scratch", 64 << 20);
sandbox_mount_dir(&p.mounts, "/work");
sandbox_mount_symlink(&p.mounts, "usr/bin", "/bin");
sandbox_mount_proc(&p.mounts, "/proc");              // needs SANDBOX_NS_PID
sandbox_mount_dev(&p.mounts, "/dev");
```

Destinations must be absolute with no `.` or `..` parts. Read-only binds are
recursive on Linux 5.12 and later; on older kernels mounts below a
read-only bind keep their own flags.

## Seccomp filters

```mettle
var f: SandboxSeccomp;
sandbox_seccomp_init(&f, SANDBOX_SC_ERRNO(SANDBOX_EPERM));  // or SANDBOX_SC_KILL
sandbox_seccomp_allow_basic(&f);      // files, memory, signals, time, exit
sandbox_seccomp_allow_process(&f);    // threads and child processes
sandbox_seccomp_allow(&f, "socket", "connect");
sandbox_seccomp_rule_name(&f, "ptrace", SANDBOX_SC_KILL);
sandbox_seccomp_compile(&f);
var action: uint32 = sandbox_seccomp_eval(&f, sandbox_syscall_number("mount"));
```

Names the current CPU lacks are skipped, so one list works on x86-64 and
AArch64. `sandbox_seccomp_eval` runs the compiled program the way the
kernel would, which is useful for checking a policy in tests. A filter can
also be loaded into the current process with `sandbox_seccomp_load`; it
can never be removed.

## Landlock rules

```mettle
var l: SandboxLandlock;
sandbox_landlock_init(&l);                         // refuse every file and TCP right
sandbox_landlock_allow_system(&l);                 // read and run /usr, /bin, /lib, /etc...
sandbox_landlock_allow_path(&l, "/work", SANDBOX_LL_READ | SANDBOX_LL_WRITE);
sandbox_landlock_allow_port(&l, 443, SANDBOX_LL_CONNECT_TCP);
sandbox_policy_landlock(&p, &l);
```

A right on a directory covers everything below it. Rules on a file keep
only the rights that apply to files. Paths that do not exist when the
ruleset is applied are skipped, since they grant nothing.

Kernels differ in what Landlock supports: ABI 1 (Linux 5.13) has file
rights, ABI 2 adds rename and link across directories, ABI 3 truncate, ABI 4
TCP ports, ABI 5 device ioctl, ABI 6 scopes for abstract unix sockets and
signals. By default rights the kernel lacks are left out, so the ruleset is
as strict as the kernel allows; set `l.strict = 1` to fail with
`-SANDBOX_EOPNOTSUPP` instead. `sandbox_landlock_abi()` reports the version.
On ABI 1 renaming or linking a file into another directory is always
refused.

## The current process

`sandbox_rlimit_set`, `sandbox_caps_drop`, `sandbox_no_new_privs`,
`sandbox_landlock_restrict` and `sandbox_seccomp_load` work on the calling process too, for a program that
wants to give up what it no longer needs after start-up.

## Limits to know

- The parent must be single-threaded when calling `sandbox_spawn`.
- Unprivileged user namespaces must be allowed by the kernel.
- With a PID namespace, a program killed by signal N is reported as exit
  code 128 + N, because the status comes from the init.
- Supplementary groups are not mapped; they show as `nogroup` inside.
- cgroups v2 limits and Windows job objects are not done yet
  (see PLAN.md).
