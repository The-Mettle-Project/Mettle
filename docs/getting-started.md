# Getting started

Install the compiler, build a program, and take a short tour of the language.

## Install

Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
```

Windows, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex
```

It installs to `~/.mettle` or `%LOCALAPPDATA%\Mettle` and adds that to your
PATH. Neither needs root or administrator.

## Build from source

The repository holds the whole toolchain under one `src/`. There is nothing to
fetch and the build runs offline.

Windows, with gcc or clang:

```powershell
.\build.bat
```

Linux:

```bash
make -j"$(nproc)"
```

Run the suite with `.\tests\run_tests.ps1` or `make check`. Both run the same
tests, so a test written on either platform is answered by both. It needs
PowerShell Core. Without it, `bash tools/test-elf-native.sh` still covers the
Linux product.

For the backend archive alone, `.\build.bat --backend-only`.

## Your first program

```mettle
import "std/io";

fn main() -> int32 {
  println("hello");
  return 0;
}
```

```bash
mettle --build hello.mettle
```

That writes `hello` next to the source, or `hello.exe` on Windows. Run it:

```bash
./hello
```

```text
hello
```

Leave `--build` off and you get an object file instead. Add `--release` when
you want it optimized.

`main` returns the process exit code. To read the command line, declare it with
arguments:

```mettle
fn main(argc: int32, argv: cstring*) -> int32 {
  println("argc={argc}");
  return 0;
}
```

## Where the standard library lives

The compiler finds the stdlib bundled next to itself, then `./stdlib` in the
working directory. `--stdlib <dir>` overrides that.
[Modules and imports](modules.md) covers the full search order.

`--prelude` imports the common modules for you, so `println` needs no `import`.

## Editor support

The VS Code and Cursor extension, `mettle-syntax`, and the IntelliJ plugin,
`clion-plugin`, live in the
[MettleMisc](https://github.com/The-Mettle-Project/MettleMisc) repository.

## A short tour

Nothing is inferred. Every `var` and every local `const` carries a type.

### Structs and methods

Fields are separated by semicolons. A method is a function named for its type:

```mettle
struct Item {
  name: string;
  qty: int32;
}

fn Item_label(it: Item) -> string {
  return "{it.qty} x {it.name}";
}
```

Call it with `.`, and note that any string literal interpolates `{expr}`:

```mettle
var basket: Item[2] = [ { name: "bolt", qty: 12 }, { name: "nut", qty: 30 } ];
for i in 0..2 {
  println("{basket[i].label()}");
}
```

```text
12 x bolt
30 x nut
```

### Tagged enums and match

A variant may carry one payload. `match` binds it and must cover every case:

```mettle
enum Parsed {
  Number(int64),
  Word(string)
}
```

```mettle
match (classify("42")) {
  case Number(n): { println("number {n}"); }
  case Word(w): { println("word {w}"); }
}
```

### Result

[`std/core`](standard-library.md) defines `Result<T, E>` and `Option<T>`, and
the standard library returns them. Read one the same way:

```mettle
import "std/conv";
import "std/core";

fn classify(s: string) -> Parsed {
  match (str_to_i64(s)) {
    case Ok(n): { return Number(n); }
    case Err(e): { return Word(s); }
  }
}
```

### Reading a file

`defer` runs on every path out of the scope, which is where the close belongs:

```mettle
import "std/io";

fn main() -> int32 {
  var fp: cstring = fopen("input.txt", "r");
  if (fp == 0) { println("cannot open"); return 1; }
  defer fclose(fp);

  var buf: uint8[256];
  var line: string = read_line((cstring)&buf[0], 256, fp);
  while (line.length > 0) {
    println("got: {line}");
    line = read_line((cstring)&buf[0], 256, fp);
  }
  return 0;
}
```

```text
got: first line
got: second line
```

## What to read next

- [Quick reference](quick-reference.md) for the idioms you will look up
- [Types](types.md) and [Declarations](declarations.md) for the language
- [Standard library](standard-library.md) for what ships with it
- [Compilation](compilation.md) for the compiler's options
- [Known limitations](known-limitations.md) for what does not work yet
