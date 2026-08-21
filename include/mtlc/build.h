/* mtlc/build.h - a public IR builder for frontends.
 *
 * This is how a frontend that is NOT the reference Mettle frontend constructs
 * libmtlc IR without touching the backend's internal headers. You describe
 * functions and an imperative instruction stream through opaque handles; the
 * builder produces an MtlcModule (see mtlc/module.h) ready for the pipeline
 * (mtlc_optimize -> mtlc_emit_object -> mtlc_build_executable in mtlc/pipeline.h).
 *
 * The model mirrors the backend IR: values are SSA-like temporaries or named
 * locals/parameters, referenced by opaque MtlcValue handles; control flow is
 * explicit labels and branches (the frontend lowers its own if/while/for). A
 * function whose body you emit is a definition; declare an `extern` function to
 * reference a symbol from the owned runtime or an explicit OS API.
 *
 *   MtlcBuilder *b = mtlc_builder_create();
 *   const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
 *   MtlcFn *f = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
 *   MtlcValue r = mtlc_const_int(f, i64, 42);
 *   mtlc_return(f, r);
 *   MtlcModule *m = mtlc_builder_finish(b);   // b is consumed
 */
#ifndef MTLC_BUILD_H
#define MTLC_BUILD_H

#include "diag.h"
#include "intrinsic.h"
#include "module.h"
#include "tensor.h"
#include "type.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtlcBuilder MtlcBuilder;
typedef struct MtlcFn MtlcFn;

/* An opaque handle to an IR value within one function builder. Values do not
 * cross function boundaries. MTLC_NO_VALUE is the "no value" sentinel (a void
 * return, an unset operand). */
typedef int MtlcValue;
#define MTLC_NO_VALUE (-1)

/* Optional tensor operands use MTLC_NO_VALUE when the descriptor does not
 * request them. Dense unscaled operations need only A/B/C/D. Structured
 * sparsity and scaled low-precision formats can additionally carry metadata
 * and scale tables without changing the shared operation's identity. Sparse
 * metadata is the canonical uint8 group-mask matrix defined by tensor.h; scale
 * pointers follow tensor.h's canonical A-row/B-column matrices. Packed data
 * strides and pointer arithmetic remain counts of stored elements. A zero
 * descriptor data leading dimension selects the corresponding masked runtime
 * integer value below; nonzero data and scale dimensions remain compile-time
 * constants. */
typedef struct {
  MtlcValue a;
  MtlcValue b;
  MtlcValue c;
  MtlcValue d;
  MtlcValue metadata;
  MtlcValue a_scale;
  MtlcValue b_scale;
  unsigned runtime_stride_mask;
  MtlcValue a_leading_dimension;
  MtlcValue b_leading_dimension;
  MtlcValue c_leading_dimension;
  MtlcValue d_leading_dimension;
} MtlcTensorMmaOperands;

/* One descriptor-sized output region of a whole matrix multiplication. The
 * matrix bundle has the same exact format/layout/stride contract as one MMA;
 * row_origin, column_origin, and problem_m/n/k are unsigned scalar values
 * uniform at the descriptor's collective scope. All four matrix leading
 * dimensions must be explicit in the descriptor or selected by the matrix
 * bundle's runtime-stride mask. */
typedef struct {
  MtlcTensorMmaOperands matrix;
  MtlcValue row_origin;
  MtlcValue column_origin;
  MtlcValue problem_m;
  MtlcValue problem_n;
  MtlcValue problem_k;
} MtlcTensorMatmulOperands;

/* Optional epilogue operands use MTLC_NO_VALUE unless selected by the
 * descriptor. Operand order never exposes a target fragment representation. */
typedef struct {
  MtlcValue destination;
  MtlcValue bias;
  MtlcValue alpha;
  MtlcValue beta;
  MtlcValue clamp_min;
  MtlcValue clamp_max;
  MtlcValue leading_dimension;
  MtlcValue bias_leading_dimension;
} MtlcTensorEpilogueOperands;

/* Operands for one rank-aware tensor transfer. Coordinates are signed int32
 * logical element indices. Set `prepared_view` to MTLC_NO_VALUE for the
 * portable path; a provider-specific non-null handle may enable native tensor
 * transfer hardware without changing the operation's semantics. */
typedef struct {
  MtlcValue destination;
  MtlcValue source;
  MtlcValue prepared_view;
  MtlcValue coordinates[MTLC_TENSOR_MAX_RANK];
} MtlcTensorTransferOperands;

typedef struct {
  MtlcValue x;
  MtlcValue y;
  MtlcValue z;
} MtlcDim3;

/* Create/destroy a builder. Destroying a builder you have not finished frees
 * everything it holds; mtlc_builder_finish consumes the builder instead. */
MtlcBuilder *mtlc_builder_create(void);
void mtlc_builder_destroy(MtlcBuilder *builder);

/* ---- diagnostics ---- */

/* Report every IR-construction diagnostic to `handler` as it happens. Without
 * one the builder stays silent and only records the first error for
 * mtlc_builder_error below; a handler is what tells you WHICH call failed,
 * while the offending frontend code is still on the stack. `user_data` is
 * passed back unchanged and is neither copied nor freed. */
void mtlc_builder_set_diagnostic_handler(MtlcBuilder *builder,
                                         MtlcDiagHandler handler,
                                         void *user_data);

/* Non-zero while no builder call has failed. Once a call fails the builder
 * latches the error, every later call is a no-op, and mtlc_builder_finish
 * returns NULL -- so a frontend can emit a whole function without checking
 * each call and test once at the end. */
int mtlc_builder_ok(const MtlcBuilder *builder);

/* The first error recorded on this builder, or NULL if none has been. The
 * string belongs to the builder and dies with it, so copy it if you need it
 * after mtlc_builder_finish (which consumes the builder on both paths). */
const char *mtlc_builder_error(const MtlcBuilder *builder);

/* mtlc_builder_ok for the builder that owns `fn`: the convenient form while
 * emitting a body, when the builder handle is not at hand. Returns 0 for a
 * NULL fn. */
int mtlc_fn_ok(const MtlcFn *fn);

/* Declare a function. `return_type` may be mtlc_type_scalar(MTLC_TYPE_VOID).
 * `param_names`/`param_types` each hold `param_count` entries (pass NULL/NULL/0
 * for no parameters). `is_extern` != 0 declares a body-less external symbol
 * (return value is NULL; do not emit a body into it). Otherwise returns a
 * function builder to emit the body into. The first non-extern function named
 * "main" is treated as the program entry point by mtlc_build_executable. */
MtlcFn *mtlc_builder_function(MtlcBuilder *builder, const char *name,
                             const MtlcType *return_type,
                             const char *const *param_names,
                             const MtlcType *const *param_types,
                             size_t param_count, int is_extern);

/* Declare a body-less external function -- a symbol resolved at link time,
 * such as an owned runtime routine or explicit OS API. This is
 * mtlc_builder_function's is_extern form
 * with an unambiguous result: it returns 1 on success and 0 on failure, where
 * the general entry point returns NULL for both. `param_names` may be NULL
 * when the names do not matter, which for an extern they usually do not. */
int mtlc_builder_declare_function(MtlcBuilder *builder, const char *name,
                                  const MtlcType *return_type,
                                  const char *const *param_names,
                                  const MtlcType *const *param_types,
                                  size_t param_count);

/* ---- inlining policy ----
 *
 * The optimizer decides for itself which calls to inline. These say otherwise,
 * and are the neutral form of the reference frontend's `@inline` / `@inline!` /
 * `@noinline` / `@pure` decorators. Each returns 1 on success, 0 for a NULL or
 * failed `fn`. They take effect only when the module is optimized.
 *
 * They matter most in a GPU module, where a helper left out of line becomes a
 * device function reached by a real call: the call ABI's parameter space plus a
 * register allocation that stops at the call boundary, paid once per work item.
 */

/* Force `fn` past the inliner's size, parameter-count and call-count
 * heuristics. Structural refusals (recursion above all) still apply. */
int mtlc_fn_set_inline(MtlcFn *fn);

/* As mtlc_fn_set_inline, and a contract: a surviving call site fails the
 * build. */
int mtlc_fn_set_inline_required(MtlcFn *fn);

/* Never inline `fn`. This is how a helper is kept as a real call -- a device
 * function in a GPU module, its own symbol on the host. */
int mtlc_fn_set_noinline(MtlcFn *fn);

/* Assert that `fn` is free of side effects and safe to evaluate
 * speculatively, which lets the optimizer hoist a loop-invariant call out of
 * a loop and reuse the one result. Trusted, not verified: asserting it of a
 * function that writes observable state is a program error. */
int mtlc_fn_set_pure(MtlcFn *fn);

/* Define a GPU entry-point function. Kernel entry points always return void
 * and accept only POD scalar parameters or pointers to POD scalars. Unlike an
 * ordinary mtlc_builder_function definition, a kernel is exported as a device
 * entry point by the selected GPU emitter. Returns NULL when the signature is
 * not a valid kernel ABI or on allocation failure. */
MtlcFn *mtlc_builder_kernel(MtlcBuilder *builder, const char *name,
                           const char *const *param_names,
                           const MtlcType *const *param_types,
                           size_t param_count);

/* Declare a module-level global variable of a scalar `type`, optionally with a
 * constant integer initializer (pass 0 for zero-initialized). `is_extern` != 0
 * declares a global defined elsewhere. Reference it inside a function with
 * mtlc_global_ref. */
void mtlc_builder_global(MtlcBuilder *builder, const char *name,
                        const MtlcType *type, long long init_value,
                        int is_extern);

/* ---- values ---- */

/* Reference parameter `index` (0-based) of this function as a value. */
MtlcValue mtlc_fn_param(MtlcFn *fn, size_t index);

/* An integer literal of `type`. */
MtlcValue mtlc_const_int(MtlcFn *fn, const MtlcType *type, long long value);

/* A floating literal; `type` selects float32 or float64. */
MtlcValue mtlc_const_float(MtlcFn *fn, const MtlcType *type, double value);

/* Declare a mutable local variable and return a value referring to it: reads
 * use the returned handle, mtlc_assign writes through it. */
MtlcValue mtlc_local(MtlcFn *fn, const char *name, const MtlcType *type);

/* Allocate a statically sized device-memory array and return an explicitly
 * addressed pointer to its first element. WORKGROUP is shared by a workgroup;
 * PRIVATE is per work-item. This is legal only in kernels, and `count` must be
 * nonzero and fit the portable 32-bit static-array extent contract. It is a
 * semantic memory operation, not a target-API spelling. */
MtlcValue mtlc_address_space_alloc(MtlcFn *fn, const char *name,
                                   const MtlcType *element_type, size_t count,
                                   MtlcAddressSpace address_space);

/* Return an unbounded typed view of the launch-provided workgroup-memory
 * arena. Every dynamic view in a kernel aliases the same base; callers can
 * partition it with ordinary element offsets and must keep every access inside
 * the dynamic_shared_bytes supplied at launch. This is a neutral memory
 * contract; the selected backend owns its physical representation. */
MtlcValue mtlc_dynamic_workgroup_view(MtlcFn *fn, const char *name,
                                      const MtlcType *element_type);

/* Reference a module-level global declared with mtlc_builder_global. Reads use
 * the handle; mtlc_assign writes through it. */
MtlcValue mtlc_global_ref(MtlcFn *fn, const char *name);

/* ---- instructions ---- */

/* Store `value` into the storage `dest` refers to (a local or a parameter). */
void mtlc_assign(MtlcFn *fn, MtlcValue dest, MtlcValue value);

/* The operators the IR understands. Prefer these over the string spellings
 * below: an unknown enumerator will not compile, whereas a mistyped string is
 * only caught when the builder rejects it at emission. */
typedef enum {
  MTLC_BINOP_ADD,          /* "+"  */
  MTLC_BINOP_SUB,          /* "-"  */
  MTLC_BINOP_MUL,          /* "*"  */
  MTLC_BINOP_DIV,          /* "/"  */
  MTLC_BINOP_REM,          /* "%"  */
  MTLC_BINOP_EQ,           /* "==" */
  MTLC_BINOP_NE,           /* "!=" */
  MTLC_BINOP_LT,           /* "<"  */
  MTLC_BINOP_LE,           /* "<=" */
  MTLC_BINOP_GT,           /* ">"  */
  MTLC_BINOP_GE,           /* ">=" */
  MTLC_BINOP_LOGICAL_AND,  /* "&&" */
  MTLC_BINOP_LOGICAL_OR,   /* "||" */
  MTLC_BINOP_BIT_AND,      /* "&"  */
  MTLC_BINOP_BIT_OR,       /* "|"  */
  MTLC_BINOP_BIT_XOR,      /* "^"  */
  MTLC_BINOP_SHL,          /* "<<" */
  MTLC_BINOP_SHR           /* ">>" */
} MtlcBinaryOp;

typedef enum {
  MTLC_UNOP_NEGATE,       /* "-" */
  MTLC_UNOP_LOGICAL_NOT,  /* "!" */
  MTLC_UNOP_BITWISE_NOT   /* "~" */
} MtlcUnaryOp;

/* The canonical string spelling of an operator, or NULL if the enumerator is
 * out of range. Useful for a frontend that prints IR alongside building it. */
const char *mtlc_binary_op_name(MtlcBinaryOp op);
const char *mtlc_unary_op_name(MtlcUnaryOp op);

/* A binary op. `result_type` is the type of the result (baked onto the
 * instruction so codegen never re-derives it). */
MtlcValue mtlc_binary_op(MtlcFn *fn, MtlcBinaryOp op, MtlcValue lhs,
                         MtlcValue rhs, const MtlcType *result_type);

/* A unary op, likewise. */
MtlcValue mtlc_unary_op(MtlcFn *fn, MtlcUnaryOp op, MtlcValue operand,
                        const MtlcType *result_type);

/* The string forms, kept for frontends that already carry operator text. `op`
 * is one of: "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&",
 * "||", "&", "|", "^", "<<", ">>" for binary, and "-", "!", "~" for unary.
 * Anything else fails the builder with a diagnostic naming the operator rather
 * than emitting an instruction no backend can lower. */
MtlcValue mtlc_binary(MtlcFn *fn, const char *op, MtlcValue lhs, MtlcValue rhs,
                     const MtlcType *result_type);
MtlcValue mtlc_unary(MtlcFn *fn, const char *op, MtlcValue operand,
                    const MtlcType *result_type);

/* Call `callee` by name with `arg_count` arguments; returns the result value,
 * or MTLC_NO_VALUE when `return_type` is void. */
MtlcValue mtlc_call(MtlcFn *fn, const char *callee, const MtlcValue *args,
                   size_t arg_count, const MtlcType *return_type);

/* Emit a target-neutral intrinsic. Unlike mtlc_call, this does not ask the
 * backend to infer semantics from a symbol name. The intrinsic's documented
 * arity is checked; returns MTLC_NO_VALUE for void or on builder error.
 *
 * The subgroup family exposes implementation-sized execution subgroups without
 * naming a warp or wavefront: LOCAL_ID/SIZE take no arguments, BROADCAST_* takes
 * (value, uniform source_local_id), and REDUCE_ADD_* takes one value. Every live
 * invocation in the subgroup must execute a collective uniformly. The selected
 * target profile defines the subgroup size. */
MtlcValue mtlc_intrinsic(MtlcFn *fn, MtlcIntrinsic intrinsic,
                         const MtlcValue *args, size_t arg_count,
                         const MtlcType *return_type);

/* Emit a memory-bearing intrinsic with an explicit target-neutral contract.
 * The current surface accepts GPU atomic load/store and RMW intrinsics;
 * invalid order/scope/address-space combinations fail the builder instead of
 * being weakened. Loads accept relaxed/acquire/seq-cst; stores accept
 * relaxed/release/seq-cst. */
MtlcValue mtlc_intrinsic_memory(MtlcFn *fn, MtlcIntrinsic intrinsic,
                                const MtlcValue *args, size_t arg_count,
                                const MtlcType *return_type,
                                MtlcAddressSpace address_space,
                                MtlcMemoryOrder order,
                                MtlcMemoryScope scope);

/* Emit an atomic compare-exchange returning the observed old value. `args`
 * are (base, element_index, expected, desired). Success and failure orders are
 * independent: failure may be relaxed/acquire/seq_cst, may not be stronger
 * than success, and may never be release/acq_rel. */
MtlcValue mtlc_atomic_compare_exchange(
    MtlcFn *fn, MtlcIntrinsic intrinsic, const MtlcValue args[4],
    const MtlcType *return_type, MtlcAddressSpace address_space,
    MtlcMemoryOrder success_order, MtlcMemoryOrder failure_order,
    MtlcMemoryScope scope);

/* Synchronize every work-item in the current workgroup and publish the selected
 * memory regions with the requested order. `memory_regions` is a bitwise OR of
 * MtlcMemoryRegion flags. Backends may strengthen but never weaken the order.
 * Every live work-item must execute this collective uniformly. */
void mtlc_workgroup_barrier(MtlcFn *fn, MtlcMemoryOrder order,
                            unsigned memory_regions);

/* Initiate a per-work-item copy of `element_count` contiguous scalar elements
 * from global storage to workgroup storage. The byte span must be a multiple
 * of four and at most 65536 bytes. Native asynchronous backends may divide it
 * into multiple transactions; other backends copy synchronously. Every path
 * must eventually commit and wait for all groups before returning. The caller
 * must supply source and destination addresses aligned to transaction_bytes;
 * this dynamic precondition cannot be established by the builder. Waiting
 * makes a thread's copies complete; use a workgroup barrier before other work
 * items consume those destinations. Reading an overlapping destination before
 * its wait is complete is invalid. */
void mtlc_async_copy_workgroup(MtlcFn *fn, MtlcValue destination,
                               MtlcValue source,
                               const MtlcType *element_type,
                               uint32_t element_count,
                               uint32_t transaction_bytes,
                               MtlcAsyncCache cache);

/* Close the current per-work-item asynchronous-copy group. Empty groups are
 * permitted. */
void mtlc_async_copy_commit(MtlcFn *fn);

/* Wait until at most `pending_groups` newer groups remain outstanding. The
 * public portable range is 0..7; every function exit requires zero. */
void mtlc_async_copy_wait(MtlcFn *fn, uint32_t pending_groups);

/* Transfer one complete rectangular tensor tile collectively between global
 * and workgroup storage. Every live work-item in the workgroup must execute
 * the operation uniformly with identical pointers, coordinates, and optional
 * prepared-view handle. The call returns only after the destination is ready
 * for ordinary accesses by every participating work-item. */
void mtlc_tensor_transfer_workgroup(
    MtlcFn *fn, const MtlcTensorTransferDesc *desc,
    const MtlcTensorTransferOperands *operands);

/* Emit one cooperative D=A*B+C tensor tile. Arguments are base pointers in
 * A, B, C, D order. The descriptor carries all arithmetic, shape, layout,
 * stride, precision, packing, overflow, sparsity, scale, and collective-scope
 * semantics; a backend must implement that exact contract or reject it. */
void mtlc_tensor_mma(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                     MtlcValue a, MtlcValue b, MtlcValue c, MtlcValue d);

/* General operand form for sparse/scaled operations and any mixture of static
 * and runtime data leading dimensions. Sparse metadata must be a uint8 pointer.
 * A runtime-stride mask bit requires the
 * matching descriptor data dimension to be zero and the matching value to be
 * a scalar integer uniform at the collective scope. Scale leading dimensions
 * are static descriptor values; zero means their canonical dense minimum. */
void mtlc_tensor_mma_ex(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                        const MtlcTensorMmaOperands *operands);

/* Emit the exact sequential composition of `tile_count` tensor MMAs as one
 * neutral accumulator chain. Tile 0 computes D=A*B+C; every later tile must
 * use that same D as both C and D. Accumulator/result format, C/D layout, and
 * C/D stride must match so a capable backend may keep the tile resident and
 * perform one initial C load and one final D store. A backend may replay the
 * component operations without changing semantics. */
void mtlc_tensor_mma_chain(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                           const MtlcTensorMmaOperands *tiles,
                           size_t tile_count);

/* Convenience for a dense, unscaled operation with four runtime leading
 * dimensions. The descriptor's four leading-dimension fields must all be zero.
 * Use mtlc_tensor_mma_ex when metadata or scale operands are also required. */
void mtlc_tensor_mma_strided(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                             MtlcValue a, MtlcValue b,
                             MtlcValue c, MtlcValue d,
                             MtlcValue lda, MtlcValue ldb,
                             MtlcValue ldc, MtlcValue ldd);

/* Compute every in-bounds output in one descriptor-sized whole-matrix region:
 * D[row,col] = C[row,col] + sum(q=0..problem_k-1) A(row,q)*B(q,col).
 * The operation is a neutral exact semantic contract. A backend may use native
 * matrix instructions for full interior chunks and must handle all M/N/K edge
 * work exactly or reject the descriptor before emitting a module. */
void mtlc_tensor_matmul(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                        const MtlcTensorMatmulOperands *operands);

/* Emit one cooperative, in-place tensor epilogue. Optional values must match
 * the descriptor exactly. A capable backend may fuse it into a native tensor
 * path; portable lowering remains an ordered cooperative memory replay. */
void mtlc_tensor_epilogue(
    MtlcFn *fn, const MtlcTensorEpilogueDesc *desc,
    const MtlcTensorEpilogueOperands *operands);

/* Emit a semantic asynchronous GPU launch. `kernel_handle` and `stream` are
 * runtime-provider handles; grid/block are full 3-D dimensions.
 * `dynamic_shared_bytes` is a scalar byte count. `arg_types` must describe
 * each kernel argument's exact scalar/pointer ABI width. Host codegen lowers
 * this to the provider-neutral mtlc_gpu_launch_checked ABI; device-module
 * emitters never consume host launch operations. */
void mtlc_gpu_launch(MtlcFn *fn, MtlcValue kernel_handle, MtlcDim3 grid,
                     MtlcDim3 block, MtlcValue dynamic_shared_bytes,
                     MtlcValue stream, const MtlcValue *args,
                     const MtlcType *const *arg_types, size_t arg_count);

/* Real address of a function symbol (defined or extern-declared): usable as
 * a callback for OS APIs and with mtlc_call_indirect. */
MtlcValue mtlc_function_address(MtlcFn *fn, const char *name);

/* Call through a function-pointer value with `arg_count` arguments. Without a
 * typed function-pointer symbol, arguments classify as integer/pointer. */
MtlcValue mtlc_call_indirect(MtlcFn *fn, MtlcValue callee,
                             const MtlcValue *args, size_t arg_count,
                             const MtlcType *return_type);

/* Convert `value` to `type` (integer width/sign changes, int<->float,
 * int<->pointer). */
MtlcValue mtlc_cast(MtlcFn *fn, MtlcValue value, const MtlcType *type);

/* The address of local/parameter `storage`, as a pointer value. */
MtlcValue mtlc_address_of(MtlcFn *fn, MtlcValue storage,
                         const MtlcType *pointer_type);

/* Load a scalar of `elem_type` from the address held in `address` (a pointer
 * value, e.g. a parameter, an mtlc_address_of result, or malloc'd memory). */
MtlcValue mtlc_load(MtlcFn *fn, MtlcValue address, const MtlcType *elem_type);

/* Store scalar `value` of `elem_type` to the address held in `address`. */
void mtlc_store(MtlcFn *fn, MtlcValue address, MtlcValue value,
               const MtlcType *elem_type);

/* ---- indexing ---- */

/* Address of element `index` of the array `base` points at, i.e. `&base[index]`.
 * `pointer_type` is the type of `base` -- an mtlc_type_pointer (or
 * mtlc_type_pointer_in) whose element type supplies the stride, and whose
 * address space the result keeps. The builder emits the scaling, so a frontend
 * never converts an element index into a byte offset itself. `index` is a
 * pointer-width integer value. Returns MTLC_NO_VALUE if `pointer_type` is not
 * a pointer. */
MtlcValue mtlc_element_address(MtlcFn *fn, MtlcValue base,
                               const MtlcType *pointer_type, MtlcValue index);

/* `base[index]` and `base[index] = value`: mtlc_element_address followed by a
 * load or store of the pointer's element type. */
MtlcValue mtlc_load_element(MtlcFn *fn, MtlcValue base,
                            const MtlcType *pointer_type, MtlcValue index);
void mtlc_store_element(MtlcFn *fn, MtlcValue base,
                        const MtlcType *pointer_type, MtlcValue index,
                        MtlcValue value);

/* Address of field `field_index` of the struct `base` points at, i.e.
 * `&base->field`. `struct_pointer_type` is the type of `base`: a pointer to an
 * mtlc_type_struct. The offset comes from that struct's computed layout, and
 * the result is a pointer to the field's own type in the same address space.
 * Returns MTLC_NO_VALUE if the type is not a pointer to a struct or the index
 * is out of range. */
MtlcValue mtlc_field_address(MtlcFn *fn, MtlcValue base,
                             const MtlcType *struct_pointer_type,
                             size_t field_index);

/* `base->field` and `base->field = value`, for a field of scalar or pointer
 * type. */
MtlcValue mtlc_load_field(MtlcFn *fn, MtlcValue base,
                          const MtlcType *struct_pointer_type,
                          size_t field_index);
void mtlc_store_field(MtlcFn *fn, MtlcValue base,
                      const MtlcType *struct_pointer_type, size_t field_index,
                      MtlcValue value);

/* ---- control flow ---- */

/* An opaque handle to a label within one function builder, allocated by
 * mtlc_label_new. Labels do not cross function boundaries. */
typedef int MtlcLabel;
#define MTLC_NO_LABEL (-1)

/* Allocate a label with a name unique within the module. `hint` is an optional
 * readability tag ("else", "loop_top") that appears in the generated name and
 * in diagnostics; pass NULL for none. This is the alternative to composing
 * unique label strings in the frontend, which is where duplicate and misspelled
 * labels come from. Returns MTLC_NO_LABEL on error. */
MtlcLabel mtlc_label_new(MtlcFn *fn, const char *hint);

/* Place `label` at the current position. Placing a label twice, or branching to
 * one never placed by the time the module is finished, fails the builder with a
 * diagnostic that names the label. */
void mtlc_label_here(MtlcFn *fn, MtlcLabel label);
/* Unconditional branch to `label`. */
void mtlc_jump_to(MtlcFn *fn, MtlcLabel label);
/* Branch to `label` when `cond` is zero; fall through otherwise. */
void mtlc_branch_if_zero_to(MtlcFn *fn, MtlcValue cond, MtlcLabel label);

/* The string forms, for frontends that name their own labels. Uniqueness is
 * the caller's problem, but a branch to a label that is never defined is still
 * caught at mtlc_builder_finish. */
void mtlc_label(MtlcFn *fn, const char *label);
void mtlc_jump(MtlcFn *fn, const char *label);
void mtlc_branch_if_zero(MtlcFn *fn, MtlcValue cond, const char *label);

/* Return `value` (or MTLC_NO_VALUE for a void return). */
void mtlc_return(MtlcFn *fn, MtlcValue value);

/* Finish building: verify that every branch target is defined, populate the
 * module's type registry and symbol table, and return the module. The builder
 * is consumed either way and must not be used afterwards (do not also call
 * mtlc_builder_destroy). Returns NULL on error -- including an error latched
 * earlier -- so install a diagnostic handler if you want to know which call
 * was at fault, since the builder's own message dies with it here. */
MtlcModule *mtlc_builder_finish(MtlcBuilder *builder);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_BUILD_H */
