# Standards and references

## Standards followed

- **POSIX Utility Syntax Guidelines**, IEEE Std 1003.1-2017 (The Open Group
  Base Specifications Issue 7), XBD chapter 12.2. Short options are one
  letter after `-`; flags can be grouped (`-abc`); an option's value can be
  joined (`-ofile`) or the next word (`-o file`); `--` ends the options; a
  lone `-` is an operand. args follows all of these.
- **GNU long options** as defined by `getopt_long` in the GNU C Library
  manual ("Parsing Long Options"): `--name`, `--name=value`, `--name value`,
  options mixed with operands (argument permutation), and optional unique
  prefixes (`--verb` for `--verbose`). args follows these, with prefixes
  off by default (`args_set_abbrev`). POSIX order (stop at the first
  operand) is `args_set_posix`.
- **Bash programmable completion** (Bash Reference Manual, 8.6 and 8.7:
  `complete -F`, `compgen -W`, `COMPREPLY`, `COMP_WORDS`, `COMP_CWORD`).
- **zsh completion system** (zsh manual, chapter 20 "Completion System":
  `#compdef`, `compdef`, `_describe`, `compadd`, `_files`).
- **fish `complete` builtin** (fish documentation, "complete": `-c`, `-n`,
  `-s`, `-l`, `-a`, `-d`, `-r`, `-x`, `-f`, and `commandline -opc`).
- Integers follow Go's `strconv.ParseInt` with base 0 (sign, then decimal,
  `0x`, `0o` or `0b`). Floats take the decimal form of IEEE 754-2019
  text (section 5.12), plus `inf`, `infinity` and `nan`.

There is no official conformance suite for command-line parsing. The
executable reference is **GNU getopt(1)** from util-linux: `mbuild ref`
runs 1500 command lines (26 fixed, the rest random) through both and
checks that they agree on every option, value and operand, and on which
lines are errors. The completion scripts are checked by running the bash
one in bash (`tests/ref/check_bash.sh`).

## Implementations studied

| Idea in args | Taken from |
|---|---|
| Values written straight into the caller's variables, defaults set at registration, no allocation | Go `flag` (BSD-3-Clause) |
| `--flag=false` for booleans; exit code 2 for usage errors | Go `flag` |
| Count flags (`-vvv`), global options that reach subcommands, `[default: ...]`, `[env: ...]` and `[possible values: ...]` notes in help, "Usage / Commands / Arguments / Options" help layout | Rust `clap` (MIT or Apache-2.0) |
| Order of sources: command line, then environment variable, then default | `clap` (`env` feature) |
| Words starting with `-` are not taken as a value unless they are a negative number; negative numbers are operands unless a short option is a digit | Python `argparse` (PSF License) |
| Unique prefixes of long options as an opt-in; "ambiguous option" errors listing the matches | GNU `getopt_long`, `argparse` (`allow_abbrev`) |
| Error line, then usage, then "For more information, try '--help'." | `clap` |
| Subcommand trees, aliases, `help SUB`, a `completion SHELL` command | Go `cobra` (Apache-2.0) |
| "Did you mean" suggestions: Levenshtein distance at most 2, or a prefix | `cobra` (`SuggestionsMinimumDistance = 2`) |
| A small, flat API; required positionals; one struct describes the program | Rust `argh` (BSD-3-Clause) |

No code was copied from any of them; args is written from their documented
behavior. Test cases in `tests/value_test.mettle` and
`tests/parse_test.mettle` follow the kinds of cases in Go's
`strconv/atoi_test.go` and `flag/flag_test.go` and Python's
`test_argparse.py`.

## Benchmark

`mbuild bench` parses a 12-word command line (3 flags, an int, a string,
a repeated option twice, two operands) a million times in release.
`bench/go/main.go` does the same with Go's `flag` package, which has to
build a new `FlagSet` for every parse. On the development machine
(Linux x86-64, Go 1.24):

| | ns per parse |
|---|---|
| args, parse only | 324 |
| args, spec built each time | 743 |
| Go flag, `FlagSet` built each time | 1228 |
