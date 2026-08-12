# calc: a second frontend for libmtlc

`calc` is a tiny C-like language with its own compiler in a single file,
[`calc.c`](calc.c). It exists to demonstrate one thing: **libmtlc is a
frontend-agnostic backend.** `calc` is not Mettle and shares no code with the
Mettle frontend. It is a self-contained lexer + recursive-descent parser that
lowers straight into libmtlc's IR through the public API and drives the backend
all the way to a native executable.

It includes **only** the public headers:

```c
#include <mtlc/build.h>     // the IR builder
#include <mtlc/mtlc.h>      // context, module, version
#include <mtlc/pipeline.h>  // optimize, emit object, link executable
```

and links **only** `bin/mtlc.lib` (or `bin/libmtlc.a`). No internal backend
headers, no Mettle frontend.

## The language

64-bit integers only. Functions, parameters, `var` locals, assignment,
`if`/`else`, `while`, `return`, calls (including recursion), and the usual
arithmetic / relational / logical operators. `main` is the entry point and its
return value becomes the process exit code.

```
// programs/factorial.calc
fn fact(n) {
  if (n < 2) { return 1; }
  return n * fact(n - 1);
}

fn main() {
  return fact(5);   // process exits 120
}
```

## Build and run

Build the compiler against the installed library and headers:

```bash
# Windows (after .\build.bat has produced bin\mtlc.lib)
gcc -Iinclude examples/calc/calc.c bin/mtlc.lib -o calc.exe -ldbghelp

# Linux (after `make libmtlc`)
cc -Iinclude examples/calc/calc.c bin/libmtlc.a -o calc
```

Then compile a `.calc` program to a native binary and run it:

```bash
./calc programs/factorial.calc factorial.exe
./factorial.exe ; echo $?      # -> 120
```

## What actually happens

`calc.c` walks its parse and calls the builder as it goes
(`mtlc_builder_function`, `mtlc_local`, `mtlc_binary_op`, `mtlc_call`,
`mtlc_branch_if_zero_to`, `mtlc_return`, and so on), then hands the finished
module to the backend:

```c
mtlc_optimize(ctx, module);              // classical optimizer (fold, inline, ...)
mtlc_build_executable(ctx, module, out); // native x86-64 codegen + internal PE link
```

Two things worth copying into a real frontend:

**Labels come from the builder.** `mtlc_label_new` hands out a name unique
across the module, so `calc` never composes label strings for its `if` and
`while` lowering, and a target it forgets to place is caught by name at
`mtlc_builder_finish` rather than becoming broken IR:

```c
MtlcLabel lelse = mtlc_label_new(p->fn, "else");
MtlcLabel lend  = mtlc_label_new(p->fn, "endif");
mtlc_branch_if_zero_to(p->fn, cond, lelse);
/* then-body */
mtlc_jump_to(p->fn, lend);
mtlc_label_here(p->fn, lelse);
/* else-body */
mtlc_label_here(p->fn, lend);
```

**Backend messages come out in the frontend's voice.** One handler, installed on
both the builder and the context, routes every libmtlc diagnostic through
`calc`'s own error reporting instead of letting it reach `stderr` from inside
the library:

```c
static void on_backend_diagnostic(void *user_data, MtlcDiagSeverity severity,
                                  const char *message) {
  (void)user_data;
  fprintf(stderr, "calc: backend %s: %s\n", mtlc_diag_severity_name(severity),
          message);
}
```

Because builder errors are sticky, `calc` emits the whole program without
checking each call and asks `mtlc_builder_ok` once at the end.

Everything after `mtlc_builder_finish` (optimization, register allocation,
instruction selection/encoding, and on Windows linking a PE executable with no
external toolchain) is libmtlc doing exactly what it does for the Mettle
frontend, driven here by a completely different language.
