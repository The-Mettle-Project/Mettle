# args API

`import "args/args";` brings in everything below. A program fills one
`Args` (it is about 60 KB, so a global or a local in `main` is best), calls
`args_run` or `args_parse`, and reads its own variables.

## Building the spec

Every builder adds to the *current command*: the root after `args_init`,
or the command made last by `args_cmd` (switch with `args_use`).

`short_name` is a character such as `'v'`, or `0` for none. `long_name` is
the name without dashes, or `""` for none. Option builders return the
option's index (for `args_seen` and `args_is_set`), or -1 when the spec is
wrong; `args_parse` then returns `ARGS_E_SPEC` and explains why.

| Call | Gives |
|---|---|
| `args_init(a, name, about)` | a fresh parser; adds `-h`/`--help` |
| `args_version(a, version)` | adds `-V`/`--version` to the root |
| `args_flag(a, &b, s, l, help)` | `bool`; `--l=false` also works |
| `args_count(a, &n, s, l, help)` | `int64`, one more each time (`-vvv` is 3) |
| `args_int(a, &n, s, l, def, help)` | `int64`: decimal, `0x`, `0o`, `0b` |
| `args_uint(a, &n, s, l, def, help)` | `uint64` |
| `args_float(a, &x, s, l, def, help)` | `float64`, also `inf` and `nan` |
| `args_str(a, &s, s, l, def, help)` | `string` (a view into argv) |
| `args_choice(a, &i, s, l, names, def, help)` | `int32` index into `names` (`string[]`, kept alive by you) |
| `args_list(a, buf, cap, &n, s, l, help)` | each value into `buf[0..cap)`; `n` is how many |
| `args_custom(a, dest, parse, s, l, metavar, help)` | `parse(text, dest)` returns 0 when good |
| `args_pos(a, &s, NAME, help)` | a required positional |
| `args_pos_opt(a, &s, NAME, def, help)` | an optional positional |
| `args_rest(a, buf, cap, &n, NAME, help)` | every remaining positional (must be last) |
| `args_cmd(a, parent, name, help)` | a subcommand of `parent` (0 = root); returns its id and makes it current |
| `args_completion_cmd(a)` | a `completion SHELL` command that `args_run` handles |

Variables passed by pointer must be initialised first (`var n: int64 = 0;`).

### Modifiers

These change the option, positional or command added last.

| Call | Does |
|---|---|
| `args_env(a, "NAME")` | use `$NAME` when the option is not given |
| `args_required(a)` | the option or positional must be given |
| `args_global(a)` | the option is also accepted after this command's subcommands |
| `args_hidden(a)` | keep the option or command out of help and completions |
| `args_metavar(a, "FILE")` | the value name in help (`--out <FILE>`), or a positional's name |
| `args_alias(a, "b bld")` | other names for the current command |
| `args_about(a, text)` | a paragraph under the current command's usage line |
| `args_need_sub(a)` | the current command needs a subcommand |
| `args_use(a, cmd)` | make `cmd` current again |

### Settings

| Call | Does |
|---|---|
| `args_set_abbrev(a, true)` | accept unique prefixes (`--verb`), off by default |
| `args_set_posix(a, true)` | stop reading options at the first positional |
| `args_set_width(a, cols)` | help width; 0 uses `$COLUMNS`, else 80 |

## Parsing

| Call | Does |
|---|---|
| `args_run(a) -> int32` | parses this process's arguments and prints help, version, completion scripts or errors. Returns -1 to go on, else the exit code |
| `args_parse(a, words) -> int32` | parses `words` (no program name) and prints nothing |
| `args_report(a, r) -> int32` | prints what `args_parse`'s result calls for; same return as `args_run` |
| `args_error_text(a) -> string` | the error message, kept inside `a` |
| `args_seen(a, opt) -> int32` | times the option was on the command line |
| `args_is_set(a, opt) -> bool` | given on the command line or from its environment variable |
| `args_exit_code(r) -> int32` | 0 for success, help and version; 2 for errors |

After parsing, `a.cmd` is the chosen command's id (0 for the root).

Values come from, in order: the command line, the environment variable,
the default. For a repeated option the last value wins, except lists,
which keep them all.

### Results

`ARGS_OK`, `ARGS_SHOW_HELP`, `ARGS_SHOW_VERSION`, or an error:

| Code | Example | Message |
|---|---|---|
| `ARGS_E_UNKNOWN` | `--verbos` | unknown option '--verbos' (did you mean '--verbose'?) |
| `ARGS_E_NO_VALUE` | `--out` at the end | option '--out' needs a value |
| `ARGS_E_BAD_VALUE` | `-j x` | invalid value 'x' for '--jobs': not a number |
| `ARGS_E_EXTRA_VALUE` | `--help=1` | option '--help' does not take a value |
| `ARGS_E_MISSING` | | missing required option '--out' |
| `ARGS_E_MISSING_POS` | | missing required argument 'FILE' |
| `ARGS_E_TOO_MANY` | | unexpected argument 'x' |
| `ARGS_E_FULL` | | too many values for '--tag' (at most 8) |
| `ARGS_E_UNKNOWN_CMD` | `biuld` | unknown command 'biuld' (did you mean 'build'?) |
| `ARGS_E_NEED_CMD` | | tool needs a command |
| `ARGS_E_AMBIGUOUS` | `--ve` | option '--ve' is ambiguous: --verbose, --version |
| `ARGS_E_SPEC` | | bad argument spec: an option name is used twice |

## Syntax rules

- `-abc` is `-a -b -c`. If `-b` takes a value, `-abc` is `-a -b c`.
- `-o=x` is `-o x`, as in clap (getopt would give `=x`).
- A word starting with `-` is never taken as an option's separate value,
  except `-` and negative numbers; write `--opt=-x` for such a value.
- `-5` is a positional unless the command has a digit short option.
- `--` ends the options; a lone `-` is a positional.
- The first positional that names a subcommand switches to it.
  `prog help a b` shows the help for `prog a b`.
- A command's own `-h` or `--help` replaces the built-in one for that name.

## Text output

`args_help`, `args_usage` and `args_completion` write into a buffer and
return the length they need (more than `cap` when it did not fit).
`args_write_help`, `args_write_usage`, `args_write_version`,
`args_write_error` and `args_write_completion` write to an `ArgsOut`, which
is a buffer (`args_out_buf`) or a stream such as `get_stdout()`
(`args_out_stream`, then `args_out_flush`).

Shells: `ARGS_BASH`, `ARGS_ZSH`, `ARGS_FISH`; `args_shell_id("zsh")` maps a
name. Load the scripts with `source <(prog completion bash)`,
`source <(prog completion zsh)` and `prog completion fish | source`.

## Values on their own

`args_parse_bool`, `args_parse_int`, `args_parse_uint` and
`args_parse_float` return `Result<T, string>`. `args_edit_distance` is
Levenshtein distance (names up to 63 bytes).

## Limits

At most 256 options, 64 positionals and 64 commands per parser
(`ARGS_MAX_*` in `src/types.mettle`). `args_run` reads up to 1024 arguments
without allocating, and allocates once for longer command lines.
