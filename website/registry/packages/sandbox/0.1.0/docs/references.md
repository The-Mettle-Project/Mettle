# References

## Kernel interfaces

The library follows the Linux kernel's documented interfaces:

- namespaces(7), user_namespaces(7) (uid and gid maps, `setgroups` deny),
  pid_namespaces(7), mount_namespaces(7), network_namespaces(7)
- clone(2), pivot_root(2), mount(2), mount_setattr(2), umount2(2)
- Documentation/userspace-api/seccomp_filter.rst and seccomp(2): the BPF
  program format, `seccomp_data`, return actions, the `no_new_privs` rule
- capabilities(7), prctl(2) (`PR_CAPBSET_DROP`, `PR_CAP_AMBIENT`,
  `PR_SET_NO_NEW_PRIVS`, `PR_SET_PDEATHSIG`)
- Documentation/userspace-api/landlock.rst and landlock(7): rulesets,
  path and port rules, ABI versions
- getrlimit(2) / prlimit(2)
- close_range(2)

System call numbers come from the kernel's uapi headers
(`asm/unistd_64.h` for x86-64, `asm-generic/unistd.h` for AArch64).

## Projects studied

No code was copied; these are the ideas taken.

| Idea | From |
|------|------|
| New root on tmpfs, pivot into it with the host root at `/oldroot`, bind what is listed, then detach the old root and pivot again (`pivot_root(".", ".")`) | bubblewrap (LGPL-2.0+) |
| A tiny init as PID 1 that reaps orphans and exits with the program's status | bubblewrap |
| Minimal `/dev` of null, zero, full, random, urandom, tty plus fd links | bubblewrap |
| New session and parent-death signal by default (`--new-session`, `--die-with-parent`) | bubblewrap |
| One policy struct covering namespaces, mounts, rlimits and seccomp; fixed setup order; report which step failed | nsjail (Apache-2.0) |
| Bring up loopback inside a new network namespace | nsjail, bubblewrap |
| Drop the bounding set, clear ambient caps, then capset; set `no_new_privs` before seccomp | minijail (BSD-3-Clause) |
| Check the audit architecture first and kill on mismatch; refuse x32 calls on x86-64 | libseccomp (LGPL-2.1), Chromium's seccomp-bpf |
| Merge neighbouring call numbers with the same action into ranges, then binary-search the ranges | Chromium's seccomp-bpf code generator (BSD-3-Clause) |
| Fixed, documented order of jail steps; secure defaults; nothing optional left on | Firecracker's jailer (Apache-2.0) |
| Deny by default and keep the allowed system call surface small | gVisor's design (Apache-2.0) |
| Best-effort Landlock: leave out rights the kernel lacks so one policy works on every kernel, with an opt-in strict mode | rust-landlock (MIT or Apache-2.0) |
| Keep only file rights on a rule for a file, not a directory | the kernel's Landlock sample (samples/landlock/sandboxer.c, BSD-3-Clause) |
| Recursive read-only binds with `mount_setattr(AT_RECURSIVE)` | util-linux, runc |
| Keep locked mount flags (read through `statfs`) when remounting read-only on old kernels | runc, bubblewrap |
