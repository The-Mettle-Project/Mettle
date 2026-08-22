# Writing a frontend for libmtlc

libmtlc is the backend: the IR, the optimizers, code generation for x86-64,
ARM64, PTX, and SPIR-V, and native linking. Mettle is one frontend that drives
it. This page shows you how to write another.

Everything here uses only the public headers in
[`include/mtlc/`](../include/mtlc/). Nothing reaches into the compiler's own
sources.

## The shape of it

Four steps, in order:

1. Create a context and a builder.
2. Build IR: declare functions, emit their bodies.
3. Finish the builder, which gives you a module.
4. Optimize the module, then emit an object or link an executable.

## A complete example

This program builds two functions, `twice` and `main`, optimizes them, and
links an executable that exits 42.

```c
#include <mtlc/build.h>
#include <mtlc/mtlc.h>
#include <mtlc/pipeline.h>
#include <stdio.h>

static void on_diag(void *ud, MtlcDiagSeverity sev, const char *msg) {
  (void)ud;
  fprintf(stderr, "%s: %s\n", mtlc_diag_severity_name(sev), msg);
}

int main(void) {
  MtlcContext *ctx = mtlc_context_create();
  MtlcBuilder *b = mtlc_builder_create();
  mtlc_builder_set_diagnostic_handler(b, on_diag, NULL);

  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  const char *names[1] = {"n"};
  const MtlcType *types[1] = {i64};

  MtlcFn *twice = mtlc_builder_function(b, "twice", i64, names, types, 1, 0);
  MtlcValue n = mtlc_fn_param(twice, 0);
  MtlcValue two = mtlc_const_int(twice, i64, 2);
  mtlc_return(twice, mtlc_binary_op(twice, MTLC_BINOP_MUL, n, two, i64));

  MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
  MtlcValue args[1] = {mtlc_const_int(m, i64, 21)};
  mtlc_return(m, mtlc_call(m, "twice", args, 1, i64));

  MtlcModule *mod = mtlc_builder_finish(b);
  if (!mod) { fprintf(stderr, "build failed\n"); return 1; }

  mtlc_context_set_runtime_directory(ctx, "runtime");
  mtlc_context_set_opt_level(ctx, 2);
  if (!mtlc_optimize(ctx, mod)) {
    fprintf(stderr, "%s\n", mtlc_context_last_error(ctx));
    return 1;
  }
  if (!mtlc_build_executable(ctx, mod, "tiny_out.exe")) {
    fprintf(stderr, "%s\n", mtlc_context_last_error(ctx));
    return 1;
  }
  printf("built tiny_out.exe (%zu functions)\n",
         mtlc_module_function_count(mod));

  mtlc_module_destroy(mod);
  mtlc_context_destroy(ctx);
  return 0;
}
```

Build and run it:

```bash
gcc -I include tiny.c bin/libmtlc.a -ldbghelp -o tiny
./tiny
./tiny_out.exe
```

```text
built tiny_out.exe (2 functions)
```

`tiny_out.exe` exits 42. On Linux, drop `-ldbghelp`.

## The runtime directory

`mtlc_build_executable` needs the runtime objects. Point the context at the
directory holding them before you link:

```c
mtlc_context_set_runtime_directory(ctx, "runtime");
```

Without it the call fails with `set the libmtlc runtime directory first`. The
objects ship in `bin/runtime`.

## Errors

The builder latches. Once a call fails, the builder records the error, every
later call is a no-op, and `mtlc_builder_finish` returns NULL. That lets you
emit a whole function without checking each call, and test once at the end.

`mtlc_builder_ok(b)` is that test. `mtlc_fn_ok(fn)` asks the same question
through a function handle, which is more convenient mid-body.

`mtlc_builder_error(b)` gives the first recorded message. It dies with the
builder, and `mtlc_builder_finish` consumes the builder on both paths, so
install a diagnostic handler if you want to know which call was at fault while
the offending frontend code is still on the stack.

For the pipeline, `mtlc_context_last_error(ctx)` carries the message and
`mtlc_context_clear_error(ctx)` resets it.

## Types

Types are immortal. `mtlc_type_scalar`, `mtlc_type_pointer`,
`mtlc_type_array`, `mtlc_type_struct`, and `mtlc_type_function_pointer` return
canonical `const MtlcType *` values that live as long as the process. You never
free one, and two calls with the same arguments give you the same pointer.

[The type system](libmtlc/types.md) has the kinds and the layout rules.

## Declaring and defining

`mtlc_builder_function(builder, name, return_type, param_names, param_types,
param_count, is_extern)` returns a function builder to emit a body into. Pass
NULL, NULL, 0 for no parameters. A void function uses
`mtlc_type_scalar(MTLC_TYPE_VOID)`.

With `is_extern` non-zero it declares a body-less external symbol and returns
NULL. `mtlc_builder_declare_function` is the same thing with an unambiguous
result: 1 on success, 0 on failure.

The first non-extern function named `main` becomes the entry point for
`mtlc_build_executable`.

## Emitting a body

Values are `MtlcValue`, returned by the emitters and passed back in.
`MTLC_NO_VALUE` stands for no value, which is what a void call gives you.

| Call | Emits |
|------|-------|
| `mtlc_fn_param(fn, i)` | The i-th parameter |
| `mtlc_const_int`, `mtlc_const_float` | A constant |
| `mtlc_local(fn, name, type)` | A local slot |
| `mtlc_binary_op(fn, op, lhs, rhs, type)` | A binary operation |
| `mtlc_unary_op(fn, op, operand, type)` | A unary operation |
| `mtlc_call(fn, callee, args, n, type)` | A call by name |
| `mtlc_cast`, `mtlc_address_of` | A conversion, an address |
| `mtlc_load`, `mtlc_store` | Memory access |
| `mtlc_load_element`, `mtlc_store_element`, `mtlc_element_address` | Indexed access |
| `mtlc_load_field`, `mtlc_store_field`, `mtlc_field_address` | Field access |
| `mtlc_return(fn, value)` | A return |

`mtlc_binary_op` takes an enum, `MTLC_BINOP_MUL` and its siblings.
`mtlc_binary` takes the operator as text, `"*"`, for a frontend that already
carries operator strings. An operator neither recognizes fails the builder with
a diagnostic naming it.

Both forms take the result type, which is baked onto the instruction so code
generation never re-derives it.

## Control flow

```c
MtlcLabel done = mtlc_label_new(fn, "done");
mtlc_branch_if_zero_to(fn, cond, done);
/* ... */
mtlc_label_here(fn, done);
```

`mtlc_label_new`, `mtlc_label_here`, `mtlc_jump_to`, and
`mtlc_branch_if_zero_to` work with `MtlcLabel` handles. The string forms,
`mtlc_label`, `mtlc_jump`, and `mtlc_branch_if_zero`, take names instead.

`mtlc_builder_finish` verifies that every branch target is defined.

## The pipeline

```c
mtlc_context_set_opt_level(ctx, 2);
mtlc_optimize(ctx, mod);
```

| Call | Does |
|------|------|
| `mtlc_optimize(ctx, mod)` | Run the optimizer for the host |
| `mtlc_optimize_for(ctx, mod, arch)` | Run it for a named architecture |
| `mtlc_apply_ml_opt(ctx, mod, stats)` | Run the learned optimizer, filling in `MtlcMlOptStats` |
| `mtlc_emit_object(ctx, mod, path)` | Write a native object for the host |
| `mtlc_emit(ctx, mod, arch, path)` | Write for `MTLC_ARCH_X86_64`, `ARM64`, `PTX`, or `SPIRV` |
| `mtlc_build_executable(ctx, mod, path)` | Object plus link |

Each returns non-zero on success.

Context settings that change the run: `mtlc_context_set_opt_level`,
`mtlc_context_set_ml_opt`, `mtlc_context_set_whole_program`,
`mtlc_context_set_explain`, and the PTX target setters.

[The pipeline](libmtlc/pipeline.md) covers the pass families and each
generator's limits.

## Ownership

`mtlc_builder_finish` consumes the builder. Do not also destroy it. Destroying
a builder you have not finished frees everything it holds.

The module is yours; free it with `mtlc_module_destroy`. The context is yours;
free it with `mtlc_context_destroy`. Types are never freed.

`mtlc_module_adopt_ir` takes ownership of a raw IR program, for a frontend that
built IR through the internal representation directly.

## A larger example

[`examples/calc`](../examples/calc) is a complete second frontend: a lexer, a
recursive-descent parser, and lowering into the IR builder, for a small C-like
language with functions, locals, `if`, `while`, and recursion. It uses only the
public API, which is what makes it the frontend-agnostic proof.

## See also

- [libmtlc reference](libmtlc/README.md)
- [The API](libmtlc/api.md)
- [Mettle and libmtlc](mettle-and-libmtlc.md)
