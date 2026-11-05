# sandbox

Run untrusted or semi-trusted programs (build steps, plugins, per-node
workers) with limits, from Mettle. Pure Mettle on raw Linux system calls, in
the same role as bubblewrap, nsjail and minijail.

```mettle
import "sandbox/sandbox";

var p: SandboxPolicy;
sandbox_policy_init(&p);                 // new namespaces, no caps, no network
sandbox_mount_system(&p.mounts);         // read-only /usr, /etc...; own /proc, /dev, /tmp
sandbox_policy_rlimit(&p, SANDBOX_RLIMIT_NPROC, 64, 64);

var f: SandboxSeccomp;
sandbox_seccomp_init(&f, SANDBOX_SC_ERRNO(SANDBOX_EPERM));
sandbox_seccomp_allow_basic(&f);         // ordinary calls only
sandbox_policy_seccomp(&p, &f);

var argv: string[2] = ["/usr/bin/python3", "job.py"];
var c: SandboxChild;
if (sandbox_spawn(&p, argv, &c) == 0) {
  var st: SandboxStatus;
  sandbox_wait(&c, &st);
}
```

## What a sandbox gets by default

| Area | Default |
|------|---------|
| Namespaces | new user, mount, PID, network, IPC, UTS and cgroup namespaces |
| Identity | same uid and gid as the caller, mapped into the user namespace |
| Network | none: only a loopback device |
| Processes | its own PID namespace with a small init as PID 1; host processes are invisible |
| Files | the host's, unless mounts are given; with mounts, an empty read-only root holding only what you list. A Landlock ruleset can narrow either view further |
| Privileges | every capability dropped, `no_new_privs` set |
| Environment | `PATH` only |
| Descriptors | only 0, 1 and 2 reach the program |
| Session | new session (no terminal access); killed when the parent dies |

Each default can be changed with a `sandbox_policy_*` call. Errors are
negative errno values; when the child fails during setup,
`SandboxChild.failed_step` names the step.

## Build and test

```bash
mbuild build
mbuild test
mbuild test -r
mbuild run run -- /bin/sh -c 'id; ls /'
```

The tests start real sandboxes and check that the limits hold (read-only
mounts refuse writes, the network has only loopback, seccomp blocks calls,
leaked descriptors do not reach the child). They need unprivileged user
namespaces (`kernel.unprivileged_userns_clone=1`, the default on most
distributions).

## Documentation

- [docs/guide.md](docs/guide.md): the API, the setup order, and the security model
- [docs/references.md](docs/references.md): the standards and projects this follows
- [PLAN.md](PLAN.md): what is done and what comes next

## License

Apache-2.0
