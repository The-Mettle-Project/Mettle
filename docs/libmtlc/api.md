# libmtlc API reference

Everything a frontend can call lives in eleven headers under
[`include/mtlc/`](../../include/mtlc/). `#include <mtlc/mtlc.h>` pulls in all of
them except `build.h`, which is included explicitly by frontends that construct
IR through the builder.

Conventions that hold across the whole API:

- **Return codes.** Functions that can fail return `int`: 1 on success, 0 on
  failure. There is no error-code enum and no `errno`-style state.
- **Diagnostics.** Every failure carries a human-readable message. By default
  it goes to `stderr`; install a handler (`mtlc_context_set_diagnostic_handler`,
  `mtlc_builder_set_diagnostic_handler`) to capture it instead, or read the last
  one back with `mtlc_context_last_error` / `mtlc_builder_error`. See
  [diag.h](#diagh).
- **NULL tolerance.** Setters and destroyers accept NULL and do nothing.
  Getters on NULL return 0/NULL. Constructors return NULL on allocation
  failure.
- **String ownership.** Unless a function's contract says otherwise, string
  arguments are copied; the caller keeps ownership of what it passed in.
- **Type ownership.** `const MtlcType *` arguments are borrowed and must stay
  valid for the life of the module they end up in. The canonical constructors
  (`mtlc_type_scalar`, `mtlc_type_pointer`, `mtlc_type_pointer_in`,
  `mtlc_type_array`, `mtlc_type_struct`, `mtlc_type_function_pointer`) return
  immortal descriptors that satisfy this automatically. See
  [The type system](types.md).
- **Thread safety.** There is no shared mutable global state in the backend
  (per-compile diagnostic state is thread-local). One thread may own one
  compilation at a time: a builder, the module it produces, and the context
  driving it must all be used from a single thread, but two threads can each
  run their own compilation concurrently.

---

## mtlc.h

Umbrella header. Includes `context.h`, `diag.h`, `intrinsic.h`, `memory.h`,
`module.h`, `pipeline.h`, `target.h`, `tensor.h`, and `type.h`.

```c
const char *mtlc_version(void);
```

Returns a static version string, e.g. `"libmtlc 0.1.0"`. Never NULL, never
freed.

---

## diag.h

How the backend says what went wrong. Both the pipeline and the IR builder
report through the same handler type, so a frontend routes every backend
message into its own diagnostic stream.

```c
typedef enum {
  MTLC_DIAG_ERROR,
  MTLC_DIAG_WARNING,
  MTLC_DIAG_NOTE
} MtlcDiagSeverity;

typedef void (*MtlcDiagHandler)(void *user_data, MtlcDiagSeverity severity,
                                const char *message);

const char *mtlc_diag_severity_name(MtlcDiagSeverity severity);
```

`message` is a complete sentence with no trailing newline, valid only for the
duration of the call -- copy it if you keep it. Builder messages are prefixed
with the API entry point that failed, so you learn which call was at fault:

```
mtlc_binary: '<=>' is not a binary operator
mtlc_builder_finish: function 'main' branches to label 'nowhere', which is never defined
```

`mtlc_diag_severity_name` returns `"error"`, `"warning"`, or `"note"`.

Without a handler, errors go to `stderr` as `mtlc: <message>` -- the behavior
libmtlc has always had, so an existing consumer that installs nothing sees no
change.

---

## context.h

A backend session handle holding the knobs the pipeline reads. It carries no
per-module state; one context can drive any number of modules sequentially.

```c
MtlcContext *mtlc_context_create(void);
void mtlc_context_destroy(MtlcContext *ctx);
```

`create` returns a context with optimization off (`opt_level` 0), ML-opt off,
whole-program off, explain off, and the PTX backend set to the GB10 profile
(PTX 8.8 / architecture-specific `sm_121a`). Returns NULL on allocation failure.

```c
void mtlc_context_set_diagnostic_handler(MtlcContext *ctx,
                                         MtlcDiagHandler handler,
                                         void *user_data);
const char *mtlc_context_last_error(const MtlcContext *ctx);
void mtlc_context_clear_error(MtlcContext *ctx);
```

Route pipeline diagnostics -- optimize, ML-opt, codegen, link -- to `handler`
instead of `stderr`. `user_data` is passed back unchanged and is neither copied
nor freed; a NULL handler restores the default.

`mtlc_context_last_error` returns the most recent error, or NULL if there has
been none. It is the low-ceremony alternative to a handler: call a pipeline
entry point and, on a 0 return, ask what happened.

```c
MtlcContext *ctx = mtlc_context_create();
if (!mtlc_build_executable(ctx, module, "out.exe")) {
  fprintf(stderr, "compile failed: %s\n", mtlc_context_last_error(ctx));
}
```

The string belongs to the context and is replaced by the next error.
`mtlc_context_clear_error` forgets it, so a later query reports only what
happened after that point.

```c
void mtlc_context_set_opt_level(MtlcContext *ctx, int level);
int  mtlc_context_opt_level(const MtlcContext *ctx);
```

0 means none: optimization becomes a no-op that returns success. Any value
`>= 1` enables the pipeline selected by `mtlc_optimize` or
`mtlc_optimize_for`. There are no graduated levels today; the distinction is
off/on.

```c
void mtlc_context_set_ml_opt(MtlcContext *ctx, int enabled);
int  mtlc_context_ml_opt(const MtlcContext *ctx);
```

Records the caller's intent to run the ML optimizer. Note that
`mtlc_apply_ml_opt` is a separate explicit call; this knob is informational for
drivers that decide from it.

```c
void mtlc_context_set_whole_program(MtlcContext *ctx, int enabled);
int  mtlc_context_whole_program(const MtlcContext *ctx);
```

Set this when the module will become a single executable whose only entry point
is `main`, making every call site visible. It gates whole-program transforms
whose soundness requires that property (for example allocation-site layout
factorization, which rewrites callee bodies). Leave it off when emitting a
relocatable object that other code will link against.

```c
void mtlc_context_set_explain(MtlcContext *ctx, int enabled,
                              const char *focus_file);
int  mtlc_context_explain(const MtlcContext *ctx);
const char *mtlc_context_explain_focus_file(const MtlcContext *ctx);
```

Enables optimization-decision reporting (each vectorization/inlining decision,
with a reason when declined). `focus_file`, when non-NULL, limits remarks to
locations whose filename matches it. **The string is borrowed, not copied**; it
must outlive every `mtlc_optimize` call that uses this context.

```c
int mtlc_context_set_ptx_target(MtlcContext *ctx, const char *target,
                                int isa_major, int isa_minor);
const char *mtlc_context_ptx_target(const MtlcContext *ctx);
int mtlc_context_ptx_isa_major(const MtlcContext *ctx);
int mtlc_context_ptx_isa_minor(const MtlcContext *ctx);
int mtlc_context_set_ptx_tensor_tuple_budget(MtlcContext *ctx,
                                             int tuple_budget);
int mtlc_context_ptx_tensor_tuple_budget(const MtlcContext *ctx);
```

Selects PTX backend policy without exposing it to a frontend AST. `target` is
copied and must be an `sm_NN[af]` or `compute_NN` name. The setter returns 0
without changing the context when the name/version is malformed.

The tensor tuple budget is a PTX code-generation policy, not a tensor semantic.
Zero selects the architecture default (64 logical fragment registers before
sm_90, 96 from sm_90); 1 through 4096 provides an explicit resident-fragment
ceiling. A chain or verified loop/pipeline above the ceiling replays exact
load/MMA/store operations. This lets a build or autotuning harness generate
resident/replay variants without changing frontend syntax, descriptors, or
shared IR. The setting does not claim that logical tuple count equals final
physical register allocation; `ptxas` resource output remains the structural
feedback gate.

---

## module.h

An opaque unit of IR, the currency of the pipeline.

```c
MtlcModule *mtlc_module_adopt_ir(void *ir_program);
```

Wraps an already-built backend IR program (an `IRProgram *` passed opaquely so
this header stays independent of the internal IR layout). **The module takes
ownership**: destroying the module destroys the IR. Frontends using the builder
never call this; `mtlc_builder_finish` does. It exists for consumers that
construct IR against the internal `src/ir/ir.h` directly (as the Mettle
reference frontend does).

```c
void *mtlc_module_ir(MtlcModule *module);
```

Borrows the underlying `IRProgram *`. The module retains ownership. NULL for a
NULL module.

```c
size_t mtlc_module_function_count(const MtlcModule *module);
```

Number of functions with bodies in the module (0 for NULL).

```c
void mtlc_module_destroy(MtlcModule *module);
```

Destroys the module and the IR program it owns. Do not destroy a module while
any pipeline call on it is in flight.

---

## tensor.h

Target-neutral cooperative-tensor vocabulary shared by every frontend and
backend. `MtlcTensorTransferDesc` describes a rank-1 through rank-5 rectangular
tile movement using global extents/byte strides, canonical dimension-0-major
workgroup storage, tile extents, element strides, zero-fill/discard bounds,
direction, format, and workgroup scope. It contains no vendor tensor-map or
async-proxy representation. `mtlc_tensor_transfer_desc_is_valid` checks the
portable structural contract; native encodability remains backend-owned.

`MtlcTensorMmaDesc` describes one collective `D = A * B + C` tile:
m/n/k, independent A/B/accumulator/result formats, four row/column layouts and
leading dimensions, transpose flags, packing, math mode, rounding, overflow,
structured sparsity, A/B scaling, and subgroup/workgroup scope. Data leading
dimensions are logical elements, never packed bytes. A nonzero data dimension
is static; zero selects a matching uniform runtime integer operand in
`MtlcTensorMmaOperands`. Dense sub-byte packing orders consecutive logical
elements from the least- to most-significant bits of each byte.

The shape is the logical whole tile, not necessarily one hardware instruction.
The PTX backend can currently subdivide byte-addressable stable f16/bf16/tf32,
f64, and i8/u8 logical M/N grids into exact WMMA subtiles and choose A or B
fragment reuse without changing this descriptor. Chains and verified
loop/pipeline groups retain every output subtile only when the backend tuple
budget admits the full accumulator set; otherwise they replay. Other backends
may choose different physical tiles or replay the same operation.

For structured sparsity, A is the sparse operand. Logical K is partitioned into
groups of 2, 4, or 8 for 1:2, 2:4, or 4:8; exactly half of each group is stored
in increasing logical-index order, and A's leading dimension counts these
compressed elements. `metadata` must point to uint8 occupancy masks in
row-major `[M][K/group]` order. Only the low group bits are meaningful and their
population count must equal `group/2`. Neither the mask layout nor compressed-A
order is a vendor fragment ABI.

Scaled A metadata is a canonical row-major `M x ceil(K/block)` matrix; scaled B
metadata is a canonical column-major `ceil(K/block) x N` matrix. Their scale
leading dimensions are static, with zero selecting the dense minimum. Scale
pointers are supplied only through `mtlc_tensor_mma_ex` or chain tiles, so this
contract remains independent of PTX lane selectors and fragment layouts.

The element enum spans f16, bf16, tf32, f32, f64, FP8 E4M3/E5M2, FP6
E2M3/E3M2, FP4 E2M1, UE8M0 and UE4M3 scale data, i8/u8, i4/u4, b1, and i32.
This is a semantic vocabulary, not a promise that every configured backend
implements every combination. `mtlc_tensor_mma_desc_is_valid` checks structural
validity; target capability selection remains the code generator's job.

`mtlc_tensor_mma_f16_f32_m16n16k16_desc()` is only a convenience constructor.
A frontend can populate any valid descriptor directly; it must not infer a
global tensor restriction from that helper.

`MtlcTensorEpilogueDesc` describes a separate cooperative in-place
post-operation over an arbitrary nonzero MxN tile:
`D[row,col] = activation(alpha*D[row,col] + beta*bias[row,col])`. It supports
f16/bf16 storage with f32 computation, native f32/f64 computation, independent
D and matrix-bias row/column layouts and static/runtime leading dimensions,
absent/per-row/per-column/matrix bias, identity/ReLU/clamp, and
subgroup/workgroup scope. Scale flags select uniform runtime alpha/beta values;
otherwise their semantic value is one. `mtlc_tensor_epilogue_desc_is_valid`
checks this structural contract, not backend availability. ReLU replaces only
ordered negative inputs with positive zero; clamp applies ordered lower- then
upper-bound replacement, preserving unordered values. Active bias storage must
not overlap the destination tile.

---

## build.h

The IR builder: how a frontend constructs a module without touching internal
headers. The programming model is documented in [The IR model](ir.md); this
section is the per-function contract.

### Handles

```c
typedef struct MtlcBuilder MtlcBuilder;   /* one module under construction */
typedef struct MtlcFn      MtlcFn;        /* one function body under construction */
typedef int MtlcValue;                    /* a value handle inside one MtlcFn */
#define MTLC_NO_VALUE (-1)
typedef struct { MtlcValue x, y, z; } MtlcDim3;
```

`MtlcValue` handles are indices into a per-function table. They are only
meaningful with the `MtlcFn` that produced them and never cross functions.
`MTLC_NO_VALUE` is the "no value" sentinel: a void call's result, a void
return's operand, and every error return.

**Error latching.** The builder is designed so a lowering pass can emit
straight-line calls without checking each one: any internal failure (allocation,
bad handle, NULL argument) latches an error on the builder. Subsequent calls
become no-ops returning `MTLC_NO_VALUE`, and `mtlc_builder_finish` returns
NULL. Check once, at finish.

```c
void mtlc_builder_set_diagnostic_handler(MtlcBuilder *builder,
                                         MtlcDiagHandler handler,
                                         void *user_data);
int mtlc_builder_ok(const MtlcBuilder *builder);
const char *mtlc_builder_error(const MtlcBuilder *builder);
int mtlc_fn_ok(const MtlcFn *fn);
```

`mtlc_builder_ok` is non-zero while nothing has failed; `mtlc_builder_error`
returns the first error, prefixed with the entry point that produced it, or
NULL if there has been none. `mtlc_fn_ok` asks the same question of the builder
that owns `fn`, which is the convenient form mid-body.

Because `mtlc_builder_finish` consumes the builder on the failure path too, its
message is gone by the time it returns NULL. Install a handler if you want to
see errors as they happen -- including the ones only detectable at finish:

```c
static void on_diag(void *user, MtlcDiagSeverity sev, const char *msg) {
  fprintf(stderr, "myfrontend: %s: %s\n", mtlc_diag_severity_name(sev), msg);
}
mtlc_builder_set_diagnostic_handler(b, on_diag, NULL);
```

### Lifecycle

```c
MtlcBuilder *mtlc_builder_create(void);
void mtlc_builder_destroy(MtlcBuilder *builder);
MtlcModule *mtlc_builder_finish(MtlcBuilder *builder);
```

`finish` **consumes the builder** in all cases: on success it returns the
module (never call `mtlc_builder_destroy` afterwards); on failure it frees
everything and returns NULL. `destroy` is for abandoning a build you have not
finished.

At finish the builder does the bookkeeping codegen depends on: it registers
every type it saw by name in the module's type registry, and adds one module
symbol per declared function and global (with parameter/return types, extern
flags, and folded initializers). See [The IR model](ir.md#module-tables).

### Declarations

```c
MtlcFn *mtlc_builder_function(MtlcBuilder *builder, const char *name,
                              const MtlcType *return_type,
                              const char *const *param_names,
                              const MtlcType *const *param_types,
                              size_t param_count, int is_extern);
```

Declares a function. `name` and every `param_names[i]` are copied. Types are
borrowed (use canonical descriptors). With `is_extern` nonzero this declares a
body-less external symbol resolved at link time (libc functions work on the
executable path) and **returns NULL by design**; do not treat that NULL as an
error. Otherwise it returns a function builder to emit the body into.

```c
int mtlc_builder_declare_function(MtlcBuilder *builder, const char *name,
                                  const MtlcType *return_type,
                                  const char *const *param_names,
                                  const MtlcType *const *param_types,
                                  size_t param_count);
```

The same extern declaration with an unambiguous result: 1 on success, 0 on
failure, rather than a NULL that means both. `param_names` may be NULL when the
names do not matter, which for an extern they usually do not.

```c
const MtlcType *pt[] = {i64};
mtlc_builder_declare_function(b, "malloc", mtlc_type_pointer(i64), NULL, pt, 1);
```

```c
MtlcFn *mtlc_builder_kernel(MtlcBuilder *builder, const char *name,
                            const char *const *param_names,
                            const MtlcType *const *param_types,
                            size_t param_count);
```

Defines a GPU entry point without involving a Mettle AST. Kernels return void
and accept only POD scalar values or pointers to POD scalars. PTX/SPIR-V emit
only functions marked through this API (or an equivalent frontend lowering);
ordinary functions in the same module are not launch entries. An ordinary
function becomes a non-entry device helper only when transitively reached by a
kernel; unrelated host functions are omitted. Both PTX and SPIR-V share the
same verifier and reject recursion, indirect/external calls, host launches from
device code, and direct calls to kernels.

The first non-extern function named `main` is the program entry point for
`mtlc_build_executable` and the ARM64 emitter. A `main` with exactly two
parameters is flagged as wanting `(argc, argv)` and the startup code passes
them.

```c
void mtlc_builder_global(MtlcBuilder *builder, const char *name,
                         const MtlcType *type, long long init_value,
                         int is_extern);
```

Declares a module-level global variable of a scalar type with a constant
integer initializer (`0` gives a zero-initialized global). Extern globals take
no initializer. Reference it in code with `mtlc_global_ref`.

### Values

```c
MtlcValue mtlc_fn_param(MtlcFn *fn, size_t index);
```

The value of parameter `index` (0-based). Out-of-range latches an error.
Parameters are mutable storage: `mtlc_assign` may write to them.

```c
MtlcValue mtlc_const_int(MtlcFn *fn, const MtlcType *type, long long value);
MtlcValue mtlc_const_float(MtlcFn *fn, const MtlcType *type, double value);
```

Literals. An integer literal's width and signedness are taken from its use
site (the `type` argument is currently advisory). A float literal is tagged 32
or 64 bit from `type`.

```c
MtlcValue mtlc_local(MtlcFn *fn, const char *name, const MtlcType *type);
MtlcValue mtlc_address_space_alloc(MtlcFn *fn, const char *name,
                                   const MtlcType *element_type, size_t count,
                                   MtlcAddressSpace address_space);
MtlcValue mtlc_dynamic_workgroup_view(MtlcFn *fn, const char *name,
                                      const MtlcType *element_type);
MtlcValue mtlc_global_ref(MtlcFn *fn, const char *name);
```

`mtlc_local` declares a mutable local variable (its `DECLARE_LOCAL` records the
type by name) and returns its storage handle. Local names must be unique within
a function; the builder binds by name. `mtlc_global_ref` returns a handle to a
global declared with `mtlc_builder_global`; reads use the handle directly and
`mtlc_assign` writes through it.

`mtlc_address_space_alloc` is the kernel-only static device-storage primitive.
It returns an exact workgroup/private pointer, rejects zero or non-portable
extents and unsupported spaces, and is lowered independently by PTX and SPIR-V.
Use `mtlc_load` / `mtlc_store` (and pointer arithmetic) on the returned value.

`mtlc_dynamic_workgroup_view` is the kernel-only launch-sized counterpart. It
returns an unbounded exact workgroup pointer. All such views in one kernel alias
one arena base, so a frontend can expose multiple element types and partition
the launch-provided byte span with explicit aligned offsets. PTX represents the
arena with one external shared symbol; SPIR-V appends one hidden Workgroup
pointer parameter. Access bounds remain the frontend/kernel's responsibility.

### Instructions

```c
void mtlc_assign(MtlcFn *fn, MtlcValue dest, MtlcValue value);
```

Stores `value` into the storage `dest` refers to (a local, parameter, or global
reference). Assigning to a literal or temp handle is meaningless and will
produce IR that codegen rejects; assign only to storage handles.

```c
typedef enum {
  MTLC_BINOP_ADD, MTLC_BINOP_SUB, MTLC_BINOP_MUL, MTLC_BINOP_DIV,
  MTLC_BINOP_REM, MTLC_BINOP_EQ, MTLC_BINOP_NE, MTLC_BINOP_LT,
  MTLC_BINOP_LE, MTLC_BINOP_GT, MTLC_BINOP_GE, MTLC_BINOP_LOGICAL_AND,
  MTLC_BINOP_LOGICAL_OR, MTLC_BINOP_BIT_AND, MTLC_BINOP_BIT_OR,
  MTLC_BINOP_BIT_XOR, MTLC_BINOP_SHL, MTLC_BINOP_SHR
} MtlcBinaryOp;

typedef enum {
  MTLC_UNOP_NEGATE, MTLC_UNOP_LOGICAL_NOT, MTLC_UNOP_BITWISE_NOT
} MtlcUnaryOp;

MtlcValue mtlc_binary_op(MtlcFn *fn, MtlcBinaryOp op, MtlcValue lhs,
                         MtlcValue rhs, const MtlcType *result_type);
MtlcValue mtlc_unary_op(MtlcFn *fn, MtlcUnaryOp op, MtlcValue operand,
                        const MtlcType *result_type);

const char *mtlc_binary_op_name(MtlcBinaryOp op);
const char *mtlc_unary_op_name(MtlcUnaryOp op);
```

A binary or unary operation producing a fresh temp. `result_type` is baked onto
the instruction so codegen never re-derives it: it decides integer width, signed
vs unsigned division/comparison/shift, and float arithmetic. Comparisons produce
a 0/1 integer, so pass an integer `result_type` for them. `MTLC_UNOP_NEGATE` is
arithmetic negation, `MTLC_UNOP_LOGICAL_NOT` yields 0/1, and
`MTLC_UNOP_BITWISE_NOT` complements.

`mtlc_binary_op_name` and `mtlc_unary_op_name` give the canonical spelling,
which is useful when a frontend prints IR alongside building it.

```c
MtlcValue mtlc_binary(MtlcFn *fn, const char *op, MtlcValue lhs, MtlcValue rhs,
                      const MtlcType *result_type);
MtlcValue mtlc_unary(MtlcFn *fn, const char *op, MtlcValue operand,
                     const MtlcType *result_type);
```

The string forms, for frontends that already carry operator text: `op` is one of
`+ - * / % == != < <= > >= && || & | ^ << >>` for binary and `- ! ~` for unary,
spelled exactly. Anything else fails the builder with a message naming the
operator, rather than emitting an instruction no backend can lower. Prefer the
enum forms, where a wrong operator will not compile.

```c
MtlcValue mtlc_call(MtlcFn *fn, const char *callee, const MtlcValue *args,
                    size_t arg_count, const MtlcType *return_type);
```

Calls `callee` by name. The name resolves at code-generation time against the
module symbol table, so calls may precede the callee's declaration in build
order, and externs resolve at link time. Returns the result temp, or
`MTLC_NO_VALUE` when `return_type` is void. Legacy atomic aliases are normalized
at this boundary to global-memory, relaxed, device-scoped operations.

```c
MtlcValue mtlc_intrinsic(MtlcFn *fn, MtlcIntrinsic intrinsic,
                         const MtlcValue *args, size_t arg_count,
                         const MtlcType *return_type);
```

Emits a semantic operation from `mtlc/intrinsic.h`. This is the preferred GPU
path for a non-Mettle frontend: PTX and SPIR-V switch on the enum, never on a
source function spelling. The builder checks the intrinsic arity and latches an
error for `MTLC_INTRINSIC_NONE`, a wrong argument count, or a NULL return type.
For subgroup identities it also checks the exact u32/f32/bool result type; the shared
GPU IR verifier checks reference-frontend declarations against the same ABI.

The subgroup identities cover local ID/size, typed u32/f32 broadcast and
variable-source shuffle, add/min/max reductions, inclusive/exclusive add scans,
word-addressed ballot, and boolean any/all votes. They describe an
implementation-sized subgroup: PTX selects a 32-lane NVIDIA warp, while SPIR-V
leaves size selection to the OpenCL implementation. Every lane must execute a
collective uniformly; a broadcast source lane must be uniform and valid, while
a shuffle source may vary and falls back to the caller value when inactive or
out of range. Ballot returns one requested 32-bit word, with words outside the
implementation mask defined as zero instead of narrowing the mask. U32
addition wraps, while f32 collective order is implementation-defined. SPIR-V
consumers require the relevant subgroup extensions. PTX supports the complete
surface. The current SPIR-V OpenCL 2.0 profile supports ballot/votes through
the KHR extensions and explicitly rejects non-uniform shuffle.
The shared GPU call-graph verifier proves statically visible collective
uniformity with separate workgroup/subgroup/work-item ranks, propagates helper
parameter ranks from all reachable call sites, and rejects a varying broadcast
source lane. A non-Mettle frontend receives the same legality checks without
reproducing them in its AST.

```c
MtlcValue mtlc_intrinsic_memory(MtlcFn *fn, MtlcIntrinsic intrinsic,
                                const MtlcValue *args, size_t arg_count,
                                const MtlcType *return_type,
                                MtlcAddressSpace address_space,
                                MtlcMemoryOrder order,
                                MtlcMemoryScope scope);
```

Emits a memory-bearing semantic intrinsic with an exact contract. The current
surface accepts unsigned u32/u64 atomic load/store and
add/sub/min/max/and/or/xor/exchange intrinsics and requires an explicit
generic/global/workgroup space, C/C++ memory order, and work-item through system
scope. It latches an error for a host function or an invalid combination.
Backends may strengthen a scope they cannot express (PTX maps work-item and
subgroup scope to CTA), but never weaken it. Loads accept relaxed, acquire, or
seq-cst; stores relaxed, release, or seq-cst; RMWs accept all five orders. PTX
uses NVIDIA's specified sequential-consistency ABI sequences; SPIR-V emits the
corresponding `OpAtomicLoad`/`OpAtomicStore`/RMW with Scope, ordering, and
memory-class operands. Store intrinsics require a void return type and produce
`MTLC_NO_VALUE` without losing the emitted side effect.

```c
MtlcValue mtlc_atomic_compare_exchange(
    MtlcFn *fn, MtlcIntrinsic intrinsic, const MtlcValue args[4],
    const MtlcType *return_type, MtlcAddressSpace address_space,
    MtlcMemoryOrder success_order, MtlcMemoryOrder failure_order,
    MtlcMemoryScope scope);
```

Emits u32/u64 compare-exchange and returns the observed old value. The four
operands are base, 64-bit-capable element index, expected, and desired. Failure
order is represented independently and rejects release/acq_rel or an order
stronger than success. SPIR-V receives both semantic operands; PTX has one CAS
qualifier and therefore safely strengthens a weaker failure order to success.
U64 SPIR-V atomics require both OpenCL `cl_khr_int64_*_atomics` extensions.

```c
void mtlc_workgroup_barrier(MtlcFn *fn, MtlcMemoryOrder order,
                             unsigned memory_regions);
```

Emits a first-class workgroup collective with an explicit order and a mask of
`MTLC_MEMORY_REGION_WORKGROUP` / `MTLC_MEMORY_REGION_GLOBAL`. It is legal only
in kernels; acquire, release, acq-rel, and seq-cst are supported. SPIR-V gets
the exact semantics operands. PTX safely strengthens the contract to its full
CTA `bar.sync` guarantee. The call must be uniform across live work-items.

```c
void mtlc_async_copy_workgroup(MtlcFn *fn, MtlcValue destination,
                               MtlcValue source,
                               const MtlcType *element_type,
                               uint32_t element_count,
                               uint32_t transaction_bytes,
                               MtlcAsyncCache cache);
void mtlc_async_copy_commit(MtlcFn *fn);
void mtlc_async_copy_wait(MtlcFn *fn, uint32_t pending_groups);
```

Emits a target-neutral per-work-item global-to-workgroup staging group. Source
and destination must be matching scalar pointers; the byte span is nonzero, a
multiple of the 4/8/16-byte transaction, and at most 65536 bytes. Both runtime
addresses must satisfy the requested alignment. Cache `ALL` or `GLOBAL` is a
performance hint, never a semantic weakening. Commit closes the current group;
wait accepts 0..7 and every CFG exit must have waited to zero with no
uncommitted operations. Shared IR validates that balance independently of any
backend.

Waiting completes copies for the issuing work-item. Use an acq-rel or seq-cst
workgroup barrier before other work-items consume the destinations. PTX 7.0 /
sm_80+ emits native `cp.async` groups. Portable PTX and the OpenCL 2.0 SPIR-V
profile replay typed copies synchronously. With target-neutral optimization,
ordinary single-use global-load/workgroup-store pairs can become the same IR
after provenance, alignment, control, and publication legality succeeds; no
frontend annotation is involved.

```c
void mtlc_tensor_transfer_workgroup(
    MtlcFn *fn, const MtlcTensorTransferDesc *desc,
    const MtlcTensorTransferOperands *operands);
```

Collectively transfers one complete tile between global and workgroup storage
and returns after destination publication. Destination/source pointer storage
must match the descriptor format; active signed-int32 coordinates and every
pointer/view value are workgroup-uniform. `prepared_view = MTLC_NO_VALUE`
requests portable replay. A supplied non-null opaque view may accelerate only
the exact same raw pointer and descriptor; a mismatched provider token is a
dynamic precondition violation, never permission to change semantics.

PTX profiles replay every valid descriptor cooperatively. PTX 8.3/sm_90+ may
select TMA only when the descriptor satisfies its additional encoding limits;
the generated code still checks for a null view, 64-byte view alignment, and
16-byte shared alignment at runtime. The current OpenCL 2.0 SPIR-V profile rejects this operation explicitly
until it has an exact cooperative implementation. Native TMA execution is
experimental and quarantined; offline `ptxas` acceptance is the current evidence.

```c
void mtlc_tensor_mma(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                     MtlcValue a, MtlcValue b,
                     MtlcValue c, MtlcValue d);
void mtlc_tensor_mma_ex(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                        const MtlcTensorMmaOperands *operands);
void mtlc_tensor_mma_chain(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                           const MtlcTensorMmaOperands *tiles,
                           size_t tile_count);
void mtlc_tensor_mma_strided(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                             MtlcValue a, MtlcValue b,
                             MtlcValue c, MtlcValue d,
                             MtlcValue lda, MtlcValue ldb,
                             MtlcValue ldc, MtlcValue ldd);
void mtlc_tensor_matmul(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                        const MtlcTensorMatmulOperands *operands);
void mtlc_tensor_epilogue(
    MtlcFn *fn, const MtlcTensorEpilogueDesc *desc,
    const MtlcTensorEpilogueOperands *operands);
```

Emits one whole-tile cooperative tensor operation in a kernel. The simple form
supplies dense, unscaled A/B/C/D pointers with static descriptor strides. The
extended operand struct adds metadata and A/B scale-table pointers exactly when
the descriptor requests structured sparsity or scaling, plus any selected
runtime leading dimensions. `runtime_stride_mask` must exactly match the zero
leading-dimension fields in the descriptor. Unused optional handles are
`MTLC_NO_VALUE`. `mtlc_tensor_mma_strided` is the dense, unscaled convenience
form when all four leading dimensions are runtime values.

`MtlcTensorMatmulOperands` embeds that same neutral matrix bundle as `matrix`
and adds `row_origin`, `column_origin`, and `problem_m/n/k`. These five values
must have unsigned scalar types and be uniform at the descriptor scope. Runtime
whole-matrix leading dimensions must be unsigned values no wider than uint32;
static descriptor dimensions are already uint32. The builder emits one
`IR_OP_TENSOR_MATMUL`, not a frontend-generated loop or chain.

For every descriptor-region output still inside problem M/N,
`mtlc_tensor_matmul` computes `D[r,c] = C[r,c] + sum(q=0..K-1) A[r,q]*B[q,c]`.
Out-of-range outputs are untouched and K=0 copies C to D. The descriptor M/N
are region extents and descriptor K is an exact preferred native chunk; it does
not truncate runtime K. PTX currently combines resident stable-WMMA or native
direct-MMA full chunks with exact cooperative M/N/K edge replay for dense
f16/bf16-to-f32, f64, i8/u8-to-i32-wrap, unscaled E4M3/E5M2-to-f32, and
block-scaled FP8/FP6/FP4-to-f32, plus canonical matching-f16/bf16 structured
2:4 A with f32 accumulation/result. Unscaled FP8 full interiors retain
backend-private direct-MMA accumulators across runtime K; partial M/N/K regions
use exact architectural conversion and cooperative replay.
Independent A/B transpose under either stored layout may use an
opposite-layout backend-local native view;
forced fallback replays the swapped coordinates exactly. Native transpose has
offline assembly but no device-numerical qualification. PTX rejects every
unsupported tail family explicitly. The OpenCL 2.0 SPIR-V profile rejects the
operation until it has an exact cooperative-matrix/edge implementation.

Packed E2M1 with UE8M0 block32 scales selects MXFP4 on a capable backend;
packed E2M1 with UE4M3 block16 scales selects NVFP4. These are descriptor
combinations, not builder entry points, and public modules receive the same
direct, chain-resident, and runtime-loop-resident lowering as Mettle source.
Matched UE8M0 block32 scales with K=32 select the Blackwell `mxf8f6f4` family
for any FP8 E4M3/E5M2, FP6 E2M3/E3M2, or FP4 E2M1 A/B pair. FP8 uses logical
byte storage; FP6/FP4 may use logical byte containers or the neutral dense
least-significant-bit-first stream. The public gate assembles the full 5x5
pairing matrix for both `mtlc_tensor_mma` and `mtlc_tensor_matmul` rather than
inferring it from a single format. For whole matrices, `scale_A` is canonical
row-major `[M,ceil(K/block)]` and `scale_B` canonical column-major
`[ceil(K/block),N]`; scale coordinates do not transpose with operands and both
scale leading dimensions must be explicit. PTX keeps complete scaled chunks
resident, advances their scale blocks with runtime K, and replays partial M/N/K
exactly. Canonical byte-aligned packed rows/columns use guarded contiguous
loads; other origins/strides take exact replay. On `sm_121a`, FP6/FP4 and UE8M0
edge decoding uses backend-local integer construction rather than emitting
alternate-format conversions that target does not promise.

Canonical structured-2:4 f16/bf16 A with f32 accumulation/result selects PTX
warp-level sparse MMA on PTX 7.1/sm_80+ when shape, layout, packing, and scope
are supported. The PTX backend alone translates uint8 occupancy masks into its
metadata selector, reuses each A/metadata fragment across adjacent N subtiles,
and selects ordered metadata when available. For `mtlc_tensor_matmul`, metadata
is compact row-major `[M,ceil(problem_k/4)]`; compressed A stores two values per
group, including a partial final group, and its leading dimension counts stored
values. Metadata coordinates stay logical when A is transposed. PTX advances
native K16 chunks by eight stored A values/four masks and decodes exact M/N/K
edges from the neutral mask. Source and public-builder bounded fixtures assemble
for `sm_121a` at 48 registers with zero stack/spills; CPU semantic evidence is
not numerical GPU or GB10 device qualification.

`mtlc_tensor_mma_chain` emits the exact sequential composition
`D=A0*B0+C; D=A1*B1+D; ...` as one neutral operation. `tile_count` must be
nonzero; accumulator and result formats and C/D layouts must match; every tile
uses the same D; every tile after the first uses D as C; and every later C/D
runtime stride equals the first output stride (the first input C stride may
differ). Values are compared by their underlying IR operand, not opaque handle
number, so repeated calls to `mtlc_fn_param` remain valid. A backend may replay
the tiles or retain a native accumulator fragment. The builder never exposes
that choice.

A straight-line sequence of two or more ordinary connected `mtlc_tensor_mma`
calls may also receive pipeline-scoped residency when an asynchronous group is
completed and published between every pair. The shared optimizer and verifier
require each ordered wait-then-workgroup-barrier handoff, identical
descriptor/output/stride connectivity, one basic block, and no intervening
observation of D. A capable backend may keep D resident across the entire
sequence; replay remains semantically valid. This is optimizer behavior, not a
separate public fragment or vendor pipeline API.

`mtlc_tensor_epilogue` emits the separate exact post-operation described above.
The destination is always required. Bias, alpha, beta, clamp bounds, D runtime
stride, and matrix-bias runtime stride must be real handles exactly when the
descriptor selects them and `MTLC_NO_VALUE` otherwise. PTX always has ordered
cooperative memory replay. It may consume an adjacent compatible resident
MMA/commit, or a verified loop commit whose exit jump is the epilogue's sole
predecessor, without changing the public operation. Bypass edges, unsupported
fragment mappings, and tuple budgets replay. Source/public and resident/replay modules
assemble for `sm_121a` with zero spills. The current OpenCL 2.0 SPIR-V profile
rejects this capability. No numerical, GB10-device, or performance claim
follows from assembly.

The builder validates descriptor structure, operand count, kernel placement,
exact pointer storage types, and integer runtime-stride types in shared IR.
Every live invocation in the selected scope must execute uniformly with
identical descriptor, pointer, and runtime-stride values.

Shared validation also proves that control flow, pointer operands, and runtime
leading dimensions are uniform at the descriptor's declared scope. Loads and
unknown call results are treated conservatively as work-item-varying; kernel
arguments and workgroup topology are workgroup-uniform; subgroup collective
results are uniform only within their subgroup.

No frontend-visible fragment ABI exists. A backend implements the exact
contract or rejects it. PTX currently selects the stable WMMA family plus
native narrow-float and canonical structured-2:4 profiles; the
OpenCL 2.0 SPIR-V profile reports that no cooperative-matrix capability is
enabled.

```c
void mtlc_gpu_launch(MtlcFn *fn, MtlcValue kernel_handle, MtlcDim3 grid,
                     MtlcDim3 block, MtlcValue dynamic_shared_bytes,
                     MtlcValue stream, const MtlcValue *args,
                     const MtlcType *const *arg_types, size_t arg_count);
```

Emits a semantic asynchronous host launch. Geometry is always explicit XYZ;
shared bytes and stream are values rather than CUDA constants. Every kernel
argument needs its exact scalar or pointer type so later host lowering can
materialize natural-width ABI cells on x86-64 or AArch64 without frontend type
queries. Calling it inside a kernel latches an error. The eventual host link
must supply `mtlc_gpu_launch_checked`; `std/gpu` supplies the CUDA Driver
provider, while another runtime can implement the same symbol. Both native
host backends accept this operation; AArch64 uses the AAPCS64 register and
overflow-stack layout for all eleven provider arguments.

```c
MtlcValue mtlc_cast(MtlcFn *fn, MtlcValue value, const MtlcType *type);
```

Converts `value` to `type`: integer width/sign changes, int/float conversions,
and int/pointer reinterpretation.

```c
MtlcValue mtlc_address_of(MtlcFn *fn, MtlcValue storage,
                          const MtlcType *pointer_type);
```

The address of a local or parameter, as a pointer value of `pointer_type`.

```c
MtlcValue mtlc_load(MtlcFn *fn, MtlcValue address, const MtlcType *elem_type);
void mtlc_store(MtlcFn *fn, MtlcValue address, MtlcValue value,
                const MtlcType *elem_type);
```

Scalar memory access through a pointer value (a pointer parameter, an
`mtlc_address_of` result, a `malloc` return, or pointer arithmetic done with
`mtlc_binary`). `elem_type` must be a scalar; its size, floatness, and
signedness are recorded on the instruction (an unsigned 32-bit load
zero-extends, a signed one sign-extends).

### Indexing

IR pointer arithmetic is in bytes. These helpers do the scaling, so a frontend
indexes by element and by field instead of computing byte offsets itself.

```c
MtlcValue mtlc_element_address(MtlcFn *fn, MtlcValue base,
                               const MtlcType *pointer_type, MtlcValue index);
MtlcValue mtlc_load_element(MtlcFn *fn, MtlcValue base,
                            const MtlcType *pointer_type, MtlcValue index);
void mtlc_store_element(MtlcFn *fn, MtlcValue base,
                        const MtlcType *pointer_type, MtlcValue index,
                        MtlcValue value);
```

`&base[index]`, `base[index]`, and `base[index] = value`. `pointer_type` is the
type of `base`: the pointee supplies the stride, and the result keeps the
pointer's address space, so a `global:float32*` indexes to a `global:float32*`.
`index` is a pointer-width integer value.

```c
MtlcValue mtlc_field_address(MtlcFn *fn, MtlcValue base,
                             const MtlcType *struct_pointer_type,
                             size_t field_index);
MtlcValue mtlc_load_field(MtlcFn *fn, MtlcValue base,
                          const MtlcType *struct_pointer_type,
                          size_t field_index);
void mtlc_store_field(MtlcFn *fn, MtlcValue base,
                      const MtlcType *struct_pointer_type, size_t field_index,
                      MtlcValue value);
```

`&base->field`, `base->field`, and `base->field = value`.
`struct_pointer_type` is a pointer to an `mtlc_type_struct`; the offset comes
from that struct's computed layout, and `mtlc_field_address` yields a pointer to
the field's own type in the same address space. Pair with
`mtlc_type_field_index` to address a field by name.

```c
const MtlcType *point = mtlc_type_struct("Point", names, types, 2);
const MtlcType *ppoint = mtlc_type_pointer(point);
mtlc_store_field(fn, p, ppoint, mtlc_type_field_index(point, "y"),
                 mtlc_const_int(fn, i32, 7));
```

### Control flow

```c
typedef int MtlcLabel;
#define MTLC_NO_LABEL (-1)

MtlcLabel mtlc_label_new(MtlcFn *fn, const char *hint);
void mtlc_label_here(MtlcFn *fn, MtlcLabel label);
void mtlc_jump_to(MtlcFn *fn, MtlcLabel label);
void mtlc_branch_if_zero_to(MtlcFn *fn, MtlcValue cond, MtlcLabel label);
```

`mtlc_label_new` allocates a label whose name is unique across the module;
`hint` is an optional readability tag ("else", "loop_top") that shows up in the
generated name and in diagnostics. This is what a frontend should use instead of
composing label strings, which is where duplicate and misspelled labels come
from. Placing a label twice is an error, as is branching to one that is never
placed.

```c
MtlcLabel done = mtlc_label_new(fn, "endif");
mtlc_branch_if_zero_to(fn, cond, done);
/* ... then-body ... */
mtlc_label_here(fn, done);
```

```c
void mtlc_label(MtlcFn *fn, const char *label);
void mtlc_jump(MtlcFn *fn, const char *label);
void mtlc_branch_if_zero(MtlcFn *fn, MtlcValue cond, const char *label);
```

The string forms, for frontends that name their own labels. Labels are
function-scoped; a jump or branch may target a label defined later, and
uniqueness is the caller's problem.

Either way, `mtlc_builder_finish` verifies that every branch has somewhere to
land and fails with a message naming the function and the label if one does not:

```
mtlc_builder_finish: function 'main' branches to label 'loop_exit', which is never defined
```

```c
void mtlc_return(MtlcFn *fn, MtlcValue value);
```

`mtlc_branch_if_zero` transfers control to its label when `cond` is zero and
falls through otherwise; it is the only conditional primitive (frontends compose
`if`/`else`/`while` from it, see [The IR model](ir.md#control-flow)).
`mtlc_return` with `MTLC_NO_VALUE` is a void return. **Every path through a
function body must reach a return**; the builder does not synthesize one.

---

## pipeline.h

The stages. All take the context first; where a stage does not yet read the
context that is documented rather than pretended otherwise.

```c
int mtlc_optimize(MtlcContext *ctx, MtlcModule *module);
int mtlc_optimize_for(MtlcContext *ctx, MtlcModule *module, MtlcArch arch);
```

Both use the context knobs (`opt_level`, `whole_program`, `explain`,
`explain_focus_file`). With `ctx` non-NULL and `opt_level <= 0` they are a
successful no-op. A NULL `ctx` optimizes with conservative defaults (on, not
whole-program, no explain).

`mtlc_optimize_for` is the preferred entry point. `MTLC_ARCH_X86_64` selects
the full classical pipeline. `MTLC_ARCH_ARM64` selects scalar/control-flow
passes that keep shared IR target-neutral. PTX/SPIR-V use that neutral schedule
only for transitively kernel-reachable device functions, validating the shared
device call graph first. They preserve kernel identity, address spaces,
barriers, atomics, subgroup operations, and tensor descriptors.

Without an architecture, `mtlc_optimize` conservatively selects the GPU policy
if the module contains a kernel and the full x86-64 policy otherwise. Returns 0
on error. See [The pipeline](pipeline.md#the-classical-optimizer) for the exact
schedules.

```c
typedef struct {
  int proposals;   /* rewrites the model proposed */
  int validated;   /* proposals that passed differential validation */
  int proven;      /* proposals proven equivalent */
  int rejected;    /* proposals rejected with a counterexample */
  int skipped;     /* proposals not attempted */
} MtlcMlOptStats;

int mtlc_apply_ml_opt(MtlcContext *ctx, MtlcModule *module,
                      MtlcMlOptStats *stats);
```

Runs the GNN-driven optimizer behind its translation-validation gate, then
hoists constants. The caller decides when to run it (typically after
`mtlc_optimize`); `ctx` is currently unused. `stats` may be NULL. Requires a
model to be present; default builds ship none, in which case the pass proposes
nothing and succeeds. It does not yet promise a target-neutral output subset;
do not apply it to a module intended for ARM64, PTX, or SPIR-V.

```c
int mtlc_emit_object(MtlcContext *ctx, MtlcModule *module, const char *path);
```

Generates native code for the whole module and writes a relocatable object in
the host format: COFF/x86-64 on Windows, ELF/x86-64 on x86-64 Linux, and
ELF/AArch64 on AArch64 Linux. `ctx` is currently unused. Optimize first if you
want optimized code.

```c
int mtlc_emit(MtlcContext *ctx, MtlcModule *module, MtlcArch arch,
              const char *path);
```

Lowers the module for `arch` and writes that target's natural product:

| `arch` | Product written to `path` |
|---|---|
| `MTLC_ARCH_X86_64` | host-format relocatable object (same as `mtlc_emit_object`) |
| `MTLC_ARCH_ARM64` | AArch64 ELF64 relocatable object with AAPCS64 calls, symbols, and relocations |
| `MTLC_ARCH_PTX` | NVIDIA PTX module (text), one `.visible .entry` per kernel plus reachable `.func` helpers |
| `MTLC_ARCH_SPIRV` | SPIR-V binary module (OpenCL 2.0), one entry point per kernel plus reachable device functions |

Every path accepts unoptimized IR. To optimize first, call
`mtlc_optimize_for(ctx, module, arch)` with the same architecture; the portable
targets must never consume the x86-only full-pipeline shape. Per-target subsets
and constraints are in [The pipeline](pipeline.md#the-code-generators).

```c
int mtlc_build_executable(MtlcContext *ctx, MtlcModule *module,
                          const char *output_path);
```

The whole back half in one call: emits a temporary object, synthesizes the
C-runtime startup that calls the module's `main`, links, and deletes the
temporaries. On Windows this uses libmtlc's own PE linker with imports resolved
by DLL name from `kernel32.dll`, `ucrtbase.dll`, and `msvcrt.dll` (no Windows
SDK import libraries needed), so extern libc calls like `malloc` and `putchar`
just work. On ELF hosts it invokes the system C compiler (`cc -no-pie`) to
link. Returns 1/0; failures report through the context's diagnostic handler, or
`mtlc_context_last_error` if you installed none.

---

## target.h

```c
typedef enum { MTLC_ARCH_X86_64, MTLC_ARCH_ARM64,
               MTLC_ARCH_PTX, MTLC_ARCH_SPIRV } MtlcArch;
typedef enum { MTLC_OBJ_COFF, MTLC_OBJ_ELF } MtlcObjectFormat;
typedef enum { MTLC_LINK_PE, MTLC_LINK_ELF } MtlcLinkTarget;

MtlcObjectFormat mtlc_host_object_format(void);  /* COFF on Windows, else ELF */
MtlcLinkTarget   mtlc_host_link_target(void);    /* PE on Windows, else ELF  */
const char *mtlc_arch_name(MtlcArch arch);       /* "x86-64", "arm64", ...   */
```

---

## type.h

The backend type descriptor and its canonical constructors. Documented in full
in [The type system](types.md); the short version:

```c
const MtlcType *mtlc_type_scalar(MtlcTypeKind kind);
const MtlcType *mtlc_type_pointer(const MtlcType *base);
const MtlcType *mtlc_type_pointer_in(const MtlcType *base,
                                     MtlcAddressSpace address_space);
```

All return immortal, immutable, canonical descriptors that never need freeing
and satisfy every "must outlive the module" requirement in this API. Scalars
cover `MTLC_TYPE_INT8` through `MTLC_TYPE_VOID`; `mtlc_type_scalar` returns
NULL for the kinds that carry their own layout. Pointers intern by
`(base, address_space)`, and pointer-to-pointer chains compose.
`mtlc_type_pointer` constructs a generic pointer. Use `mtlc_type_pointer_in` for
device-visible global, workgroup, constant, or private pointers; the address
space remains part of the backend-owned type and does not depend on frontend
syntax.

```c
const MtlcType *mtlc_type_array(const MtlcType *element, size_t count);
const MtlcType *mtlc_type_struct(const char *name,
                                 const char *const *field_names,
                                 const MtlcType *const *field_types,
                                 size_t field_count);
const MtlcType *mtlc_type_function_pointer(const MtlcType *return_type,
                                           const MtlcType *const *param_types,
                                           size_t param_count);
```

The composite constructors, interned and immortal on the same terms. They
compute the layout, so a frontend does not fill an `MtlcType` by hand and does
not have to reproduce the backend's padding rules.

`mtlc_type_struct` places each field at the next offset satisfying its own
alignment, takes the struct's alignment from the widest field, and rounds the
size up to that alignment -- the standard C rule. `name` becomes the registered
type name; structs intern by it, so declaring the same name twice returns the
first descriptor and a conflicting redeclaration returns NULL rather than
silently shadowing.

```c
const char *names[] = {"tag", "weight", "flag"};
const MtlcType *types[] = {i32, f64, i8};
const MtlcType *record = mtlc_type_struct("Record", names, types, 3);
/* offsets 0, 8, 16; size 24; alignment 8 */
```

```c
size_t mtlc_type_field_count(const MtlcType *t);
size_t mtlc_type_field_offset(const MtlcType *t, size_t index);
size_t mtlc_type_field_index(const MtlcType *t, const char *name);
```

Layout queries over a struct descriptor. `mtlc_type_field_count` returns 0 for a
non-struct; the other two return `(size_t)-1` for a non-struct, an index out of
range, or an unknown field name. These are what `mtlc_field_address` addresses
with.

```c
int mtlc_type_is_integer(const MtlcType *t);
int mtlc_type_is_unsigned(const MtlcType *t);
int mtlc_type_is_float(const MtlcType *t);
int mtlc_type_is_aggregate(const MtlcType *t);
size_t mtlc_type_size(const MtlcType *t);
size_t mtlc_type_alignment(const MtlcType *t);
const char *mtlc_type_kind_name(MtlcTypeKind kind);
```

Classification and layout queries; all NULL-safe (returning 0).

## memory.h

`MtlcAddressSpace`, `MtlcMemoryOrder`, `MtlcMemoryScope`, and the bitwise
`MtlcMemoryRegion` flags are the neutral GPU memory-model vocabulary used by
`MtlcType`, atomics, allocations, and barriers. The enums deliberately use
topology names (`WORKGROUP`, `DEVICE`, `SYSTEM`) rather than PTX or SPIR-V
spellings. `*_DEFAULT` exists for old IR compatibility and must not be used by a
new explicit memory operation.

---

## Linking a frontend

```bash
# Windows (after .\build.bat)
gcc -Iinclude my_frontend.c bin/mtlc.lib -o my_frontend.exe -ldbghelp

# Linux (after `make libmtlc`)
cc -Iinclude my_frontend.c bin/libmtlc.a -o my_frontend
```

`-ldbghelp` on Windows satisfies the crash reporter's stack-walk imports. The
library is self-contained beyond system libraries; the `libmtlc_selfcontained`
suite gate enforces that its symbol closure never reaches into frontend code.
