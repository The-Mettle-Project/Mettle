# args

Command-line arguments for Mettle, in pure Mettle.

- Short flags (`-v`), combined (`-abc`), long (`--verbose`), values joined
  or apart (`-ofile`, `-o file`, `--out=file`, `--out file`), and `--`.
- Typed options: bool, count (`-vvv`), int, uint, float, string, one of a
  set of names, repeated lists, and your own parser.
- Defaults, required options, environment-variable fallbacks.
- Positional arguments: required, optional with a default, and "the rest".
- Subcommands with aliases, nested to any depth, with global options.
- Help and usage text, wrapped to the terminal, and `prog help SUB`.
- Clear errors with "did you mean" suggestions, and exit code 2.
- Completion scripts for bash, zsh and fish.
- No allocation: values are views into argv, written straight into your
  variables. About 320 ns to parse a 12-word command line.

It follows the POSIX Utility Syntax Guidelines with GNU long options, and
agrees with GNU getopt(1) on 1500 test command lines. See
[docs/references.md](docs/references.md).

## Example

```mettle
import "std/io";
import "args/args";

fn main() -> int32 {
  var a: Args;
  args_init(&a, "tool", "tool: builds things");
  args_version(&a, "1.0.0");

  var verbose: int64 = 0;
  args_count(&a, &verbose, 'v', "verbose", "say more");
  args_global(&a);                       // also allowed after a subcommand

  var build: int32 = args_cmd(&a, 0, "build", "compile the project");
  var release: bool = false;
  args_flag(&a, &release, 'r', "release", "optimize");
  var jobs: int64 = 0;
  args_int(&a, &jobs, 'j', "jobs", 4, "parallel jobs");
  args_env(&a, "TOOL_JOBS");             // used when -j is not given
  var files: string[64];
  var nfiles: int32 = 0;
  args_rest(&a, &files[0], 64, &nfiles, "FILE", "files to build");

  args_completion_cmd(&a);               // tool completion bash|zsh|fish
  args_use(&a, 0);
  args_need_sub(&a);                     // a subcommand must be given

  var code: int32 = args_run(&a);        // prints help, version or errors
  if (code >= 0) { return code; }

  if (a.cmd == build) {
    println("jobs={jobs} release={release} files={nfiles}");
  }
  return 0;
}
```

```text
$ tool build -rj8 a.m b.m
jobs=8 release=true files=2
$ tool biuld
error: unknown command 'biuld' (did you mean 'build'?)

Usage: tool [OPTIONS] <COMMAND>

For more information, try 'tool --help'.
```

A fuller program is in [examples/demo](examples/demo/main.mettle).

## Using it

Add it with mbuild (or point `mettle.deps` at a checkout) and import the facade:

```bash
mbuild get args
```

```mettle
import "args/args";
```

Every exported name starts with `args_`, `Args` or `ARGS_`. The full list
is in [docs/api.md](docs/api.md).

## Building and testing

```bash
mbuild test
```

```bash
mbuild test -r
```

```bash
mbuild ref
```

```bash
mbuild bench
```

`mbuild ref` needs GNU getopt(1), bash and python3.

## License

Apache-2.0. See [LICENSE](LICENSE).
