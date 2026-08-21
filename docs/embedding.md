# Writing a frontend for libmtlc

libmtlc is a standalone compiler backend. Any frontend can lower its own
language into libmtlc's IR and drive the pipeline (custom IR, the classical +
GNN optimizers, native x86-64 / ARM64 / PTX / SPIR-V codegen, and native PE/ELF
linking) through the public C API in [`include/mtlc/`](../include/mtlc). Your
frontend includes only those headers and links only `bin/mtlc.lib` (Windows) or
`bin/libmtlc.a` (Linux). It never touches a backend-internal header.

The reference Mettle frontend is one consumer; [`examples/calc`](../examples/calc)
is a second, deliberately unrelated one: a tiny C-like language in a single
file. This document walks the same path.

Self-containment is audited, not assumed: the test suite computes the
archive's external-symbol closure with `nm` and fails if any lib member
references a driver/frontend symbol it doesn't define (`libmtlc_selfcontained`
gate), and builds+runs the calc example against the library alone
(`calc_frontend` gate).

This page is the tutorial. The full backend reference lives in
[`docs/libmtlc/`](libmtlc/README.md): the [API contract for every
function](libmtlc/api.md), [the IR model](libmtlc/ir.md), [the type
system](libmtlc/types.md), [the pipeline and per-target
limits](libmtlc/pipeline.md), and [internals](libmtlc/internals.md).

## The public surface

| Header | What it gives you |
|--------|-------------------|
| [`mtlc/type.h`](../include/mtlc/type.h) | `MtlcType`, the backend type descriptor, and `mtlc_type_scalar()` |
| [`mtlc/build.h`](../include/mtlc/build.h) | the IR builder: functions, values, instructions, control flow |
| [`mtlc/module.h`](../include/mtlc/module.h) | `MtlcModule`, an opaque unit of IR |
| [`mtlc/context.h`](../include/mtlc/context.h) | `MtlcContext`, a backend session holding the optimization knobs |
| [`mtlc/pipeline.h`](../include/mtlc/pipeline.h) | `mtlc_optimize`, `mtlc_apply_ml_opt`, `mtlc_emit_object`, `mtlc_build_executable` |
| [`mtlc/target.h`](../include/mtlc/target.h) | architecture / object-format / link-target enums |

## 1. Build IR

Create a builder, declare functions, and emit an instruction stream. Values are
opaque `MtlcValue` handles; control flow is explicit labels and branches (your
frontend lowers its own `if`/`while`/`for`).

```c
#include <mtlc/build.h>

const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
MtlcBuilder *b = mtlc_builder_create();

/* fn add(a, b) { return a + b; } */
const char *pn[] = {"a", "b"};
const MtlcType *pt[] = {i64, i64};
MtlcFn *add = mtlc_builder_function(b, "add", i64, pn, pt, 2, /*extern=*/0);
MtlcValue sum = mtlc_binary(add, "+", mtlc_fn_param(add, 0),
                            mtlc_fn_param(add, 1), i64);
mtlc_return(add, sum);

/* fn main() { return add(40, 2); } */
MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
MtlcValue args[] = {mtlc_const_int(m, i64, 40), mtlc_const_int(m, i64, 2)};
mtlc_return(m, mtlc_call(m, "add", args, 2, i64));

MtlcModule *module = mtlc_builder_finish(b);  /* consumes the builder */
```

`mtlc_builder_finish` populates the module's type registry and symbol table (the
tables codegen reads) from the types you declared, then hands back a module.
Calls resolve by name, so functions may be defined in any order.

## 2. Optimize

```c
#include <mtlc/pipeline.h>

MtlcContext *ctx = mtlc_context_create();
mtlc_context_set_opt_level(ctx, 1);
mtlc_context_set_whole_program(ctx, 1);   /* single-exe: every call site visible */
mtlc_optimize(ctx, module);
/* optional: mtlc_context_set_ml_opt(ctx, 1); mtlc_apply_ml_opt(ctx, module, NULL); */
```

The optimizer decides for itself which calls to inline. Four builder calls
override that, and they are the neutral form of the reference frontend's
decorators:

```c
mtlc_fn_set_inline(helper);           /* past the size/count heuristics */
mtlc_fn_set_inline_required(helper);  /* and fail the build on a surviving call */
mtlc_fn_set_noinline(helper);         /* keep it a real call */
mtlc_fn_set_pure(helper);             /* hoist loop-invariant calls out of loops */
```

They matter most in a GPU module: a device helper left out of line becomes a
call whose ABI cost every work item pays. `mtlc_fn_set_noinline` is how a
frontend keeps one deliberately.

## 3. Emit code

Emit a relocatable object, or go all the way to a native executable (on Windows
this uses libmtlc's own internal PE linker, with no external toolchain):

```c
mtlc_build_executable(ctx, module, "a.exe");   /* or mtlc_emit_object(ctx, module, "a.o") */

mtlc_module_destroy(module);
mtlc_context_destroy(ctx);
```

## Build your frontend

Inside a checkout, build the library and link against it in place:

```bash
# Windows, after .\build.bat
gcc -Iinclude my_frontend.c bin/mtlc.lib -o my_frontend.exe -ldbghelp

# Linux, after `make libmtlc`
cc -Iinclude my_frontend.c bin/libmtlc.a -o my_frontend
```

`-ldbghelp` on Windows satisfies the crash reporter's stack-walk imports; the
library needs nothing else beyond system libraries.

## Getting just the backend

You do not need the whole repository in your project. The backend is exactly two
things: the headers in `include/mtlc/` and the static library. The one-line
fetchers download the prebuilt release into `./libmtlc`:

```bash
# Linux
curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/get-libmtlc.sh | sh
```

```powershell
# Windows
irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/get-libmtlc.ps1 | iex
```

Both accept overrides: a specific tag (`LIBMTLC_VERSION`) and a target directory
(`LIBMTLC_DIR`, default `./libmtlc`).

From a checkout instead, stage the same folder from source with
`make dist-libmtlc` (Linux) or `.\tools\dist-libmtlc.ps1` (Windows), or do a
system install with a pkg-config file:

```bash
make install-libmtlc PREFIX=/usr/local   # honors DESTDIR
cc $(pkg-config --cflags libmtlc) -c my_frontend.c -o my_frontend.o
cc my_frontend.o $(pkg-config --libs libmtlc) -o my_frontend
```

The frontend links libmtlc's owned host runtime and startup through the package
flags. It does not link a host C runtime.

## Scope of the builder today

`mtlc/build.h` covers the imperative core a real language needs: functions
(including `extern` declarations resolved from the owned runtime or an OS API), module
globals with initializers, parameters, locals, assignment, integer/float
arithmetic and comparisons, casts (including int/pointer conversions), pointer
types (`mtlc_type_pointer`), memory (`mtlc_load` / `mtlc_store` /
`mtlc_address_of`, with array indexing as pointer arithmetic), calls, and
label/branch control flow.
The `public_api` test gate exercises every one of those against all four
targets. Struct/aggregate *layout helpers* are the one construct not yet
wrapped (the `MtlcType` fields for them are public; field access is
base-pointer + offset arithmetic today); wrapping them is additive.

## Targets through the public API

`mtlc_emit(ctx, module, arch, path)` reaches every backend:

| `MtlcArch` | Product |
|---|---|
| `MTLC_ARCH_X86_64` | host-format relocatable object (or `mtlc_build_executable` for a linked binary) |
| `MTLC_ARCH_ARM64` | AArch64 ELF64 relocatable object (AAPCS64) |
| `MTLC_ARCH_PTX` | NVIDIA PTX module (text) |
| `MTLC_ARCH_SPIRV` | SPIR-V binary module (OpenCL 2.0) |

Every target accepts unoptimized IR. For optimized output, call
`mtlc_optimize_for(ctx, module, arch)` with the same consumer architecture;
ARM64/PTX/SPIR-V receive only target-neutral transformations and never the
x86-only full pipeline.

See also: [compilation pipeline](compilation.md), [GPU offload](gpu.md).
