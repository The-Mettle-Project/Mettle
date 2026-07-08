#ifndef IR_H
#define IR_H

#include "../parser/ast.h"
#include "../semantic/symbol_table.h"
#include "../semantic/type_checker.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define IR_PROFILE_ID_NONE UINT32_MAX

/* `@simd` loop markers. A marker is an IR_OP_NOP whose `text` is
 * "@@simd:B:<id>:<mode>" (emitted just before a vectorization-requested loop)
 * or "@@simd:E:<id>:0" (just after it); <mode> is a SimdAttr. NOP is skipped by
 * every recognizer and is a no-op in every backend, so the marker never
 * perturbs vectorization or codegen. The release-stage contract verifier
 * (ir_optimize_simd_contract.c) pairs B/E by nesting, checks whether a SIMD
 * intrinsic landed between them, enforces the contract, and clears the
 * markers. Emitted by ir_lowering.c. */
#define IR_SIMD_MARKER_PREFIX "@@simd:"

typedef enum {
  IR_OPERAND_NONE,
  IR_OPERAND_TEMP,
  IR_OPERAND_SYMBOL,
  IR_OPERAND_INT,
  IR_OPERAND_FLOAT,
  IR_OPERAND_STRING,
  IR_OPERAND_LABEL
} IROperandKind;

typedef struct {
  IROperandKind kind;
  char *name;
  long long int_value;
  double float_value;
  /* IEEE-754 width of a floating operand: 32 or 64. 0 means "not a float /
   * unspecified" and callers must treat it as the default double width (64).
   * Carried so backends never have to re-derive single vs double precision
   * from scattered symbol/type lookups. */
  int float_bits;
} IROperand;

typedef enum {
  IR_OP_NOP,
  IR_OP_LABEL,
  IR_OP_JUMP,
  IR_OP_BRANCH_ZERO,
  IR_OP_BRANCH_EQ,
  IR_OP_DECLARE_LOCAL,
  IR_OP_ASSIGN,
  IR_OP_ADDRESS_OF,
  IR_OP_LOAD,
  IR_OP_STORE,
  IR_OP_BINARY,
  IR_OP_UNARY,
  /* Fibonacci-style rotate: dest=next, lhs=a, rhs=b => next=a+b; a=b; b=next */
  IR_OP_ROTATE_ADD,
  IR_OP_CALL,
  IR_OP_CALL_INDIRECT,
  IR_OP_NEW,
  IR_OP_RETURN,
  IR_OP_INLINE_ASM,
  IR_OP_CAST,
  /* Vectorized idiom: count word starts in a byte buffer. Produced only by
   * ir_vectorize_simple_loops_pass when it recognizes the exact
   * "while (i<len){c=buf[i]; if(ws(c)) in_word=0; else {if(!in_word)count++;
   * in_word=1;} i++}" shape. Semantics: dest receives the number of maximal
   * non-whitespace runs in lhs[0..rhs-1] (whitespace = 0x20/0x09/0x0A/0x0D),
   * added to dest's prior value (the scalar code initializes count=0, so the
   * pass only matches when that holds). lhs = buffer base symbol, rhs = length
   * symbol/operand, dest = count symbol. Codegen lowers this to an SSE2
   * 16-bytes/iteration scan plus a scalar tail. */
  IR_OP_COUNT_WORD_STARTS,
  /* Inline memory copy: dest = dst pointer, lhs = src pointer, rhs = byte count
   * (INT). Produced by ir_memcpy_inline_pass for constant-size memcpy calls. */
  IR_OP_MEMCPY_INLINE,
  /* Horizontal sum of int32 array into int64 accumulator. dest = sum symbol
   * (added to prior value), lhs = base pointer, rhs = element count. */
  IR_OP_SIMD_SUM_I32,
  /* Horizontal sum of a uint8 array into an int64 accumulator. dest = sum
   * symbol (added to prior value), lhs = base pointer, rhs = element count.
   * Bytes are summed as unsigned (vpsadbw), matching (int64)(uint8)load. */
  IR_OP_SIMD_SUM_U8,
  /* In-place element-wise map of a uint8 buffer: for each byte b, apply a chain
   * of constant byte operations (mod 256). lhs = base pointer, rhs = element
   * count, dest = NONE. arguments hold the chain as (op_code INT, const INT)
   * pairs in application order; op_code is an IRByteMapOp. */
  IR_OP_SIMD_BYTE_MAP,
  /* Constant/invariant fill (the memset/frame-clear class): store one value
   * into every element of a buffer. Address modes, selected by arguments[1]
   * (INT):
   *   mode 0: lhs = base pointer, rhs = element BOUND; elements filled =
   *           bound - start, first element at base + (offset+start)*size.
   *           arguments[3] = start (the iv's entry value; INT 0 when the iv
   *           provably starts at zero), arguments[4] = invariant index
   *           offset (INT 0, a symbol, or a temp materialized just before
   *           this op). 32-bit index math, like the loops it replaces.
   *   mode 1: lhs = begin pointer symbol, rhs = end pointer symbol (byte
   *           length = end - begin; the element tail may overshoot `end` by
   *           up to size-1 bytes exactly as the scalar loop did)
   *   mode 2: byte-offset walk `*(base + i) <- v; i += size` with 64-bit
   *           locals: lhs = base (an int64 local), rhs = byte bound,
   *           arguments[3] = start byte offset (iv entry value). Same tail
   *           semantics as mode 1.
   * arguments[0] = element size in bytes (1/2/4/8), arguments[2] = the fill
   * value (INT immediate -- float fills carry their raw bit pattern -- or an
   * invariant SYMBOL). dest = NONE. */
  IR_OP_SIMD_FILL,
  /* Fixed 32x32 int32 matrix multiply. dest = c, lhs = a, rhs = b (pointers). */
  /* Reserved for an explicit 32x32 int32 SIMD matmul API. Do not introduce
   * this from ordinary source by function name or benchmark-shaped matching. */
  IR_OP_SIMD_MATMUL_N32,
  /* In-place signed int32 insertion sort. dest = base pointer, rhs = len. */
  IR_OP_SIMD_INSERTION_SORT_I32,
  /* Signed int32 dot product into int64. dest = sum/result, lhs = a, rhs = b,
   * arguments[0] = element count. */
  IR_OP_SIMD_DOT_I32,
  /* Signed int8 x int8 -> int32 dot product (the quantized GEMM/GEMV inner
   * loop). dest = int32 sum, lhs = a (int8*), rhs = b (int8*), arguments[0] =
   * element count. AVX2 vpmaddwd kernel. */
  IR_OP_SIMD_DOT_I8,
  /* SLP-vectorized group of K parallel int32 multiply-accumulate reductions
   * (K in {4,8}). For lane j in 0..K-1:
   *     out[out_off + j] += sum_{k=0..count-1} a[a_off + k] * b[b_off + k*bstr + j]
   * i.e. one shared scalar a[k] broadcast against K contiguous b lanes, K
   * independent accumulators stored to K contiguous outputs. Matched from the
   * instruction-level parallelism of K isomorphic accumulator chains (broadcast
   * scalar x contiguous loads) -- NOT from matmul's shape or names.
   * dest=out base ptr, lhs=a base ptr, rhs=b base ptr; arguments:
   * [0]=K, [1]=count, [2]=a_off, [3]=b_off, [4]=b_stride, [5]=out_off. */
  IR_OP_SIMD_SLP_MAC_I32,
  /* int8 x int8 -> int32 variant of SLP_MAC: the quantized GEMM tile. Same
   * operand/argument layout, but a and b are int8 arrays (byte loads, widened to
   * int32) while c (out) is int32. Same AVX2 broadcast-MAC kernel with int8
   * widening. */
  IR_OP_SIMD_SLP_MAC_I8,
  /* dst[i] = src[i]*mul+add; dest += sum of outputs. lhs=src, rhs=dst,
   * arguments[0]=len, [1]=mul, [2]=add (int32). */
  IR_OP_SIMD_SCALE_I32,
  /* dst[i] = clamp(src[i], lo, hi); dest += sum of outputs. lhs=src, rhs=dst,
   * arguments[0]=len, [1]=lo, [2]=hi (int32). */
  IR_OP_SIMD_CLAMP_I32,
  /* dst[i] = src[n-1-i]; dest += sum of outputs. lhs=src, rhs=dst,
   * arguments[0]=len. */
  IR_OP_SIMD_REVERSE_COPY_I32,
  /* Lower-bound index search over sorted int32 array:
   * dest=lo index result, lhs=arr, rhs=n, arguments[0]=key(int32).
   * dest is IN/OUT: codegen seeds the running lo from dest's prior value,
   * so the recognizer must prove the source loop initializes it to 0. */
  IR_OP_LOWER_BOUND_I32,
  /* Inclusive int32 prefix sum: dst[i]=sum(src[0..i]) in int32, dest holds
   * int64 running sum. lhs=src, rhs=dst, arguments[0]=len. */
  IR_OP_PREFIX_SUM_I32,
  /* Min/max scan over arr[1..n-1] updating dest=minv and arguments[0]=maxv;
   * caller initializes both from arr[0]. lhs=arr, rhs=n. */
  IR_OP_SIMD_MINMAX_I32,
  /* Horizontal sum of a float64/float32 array into the dest float accumulator
   * (added to dest's prior value). lhs = base pointer, rhs = element count. */
  IR_OP_SIMD_SUM_F64,
  IR_OP_SIMD_SUM_F32,
  /* Float64/float32 dot product into the dest float accumulator (added to
   * dest's prior value). lhs = a, rhs = b, arguments[0] = element count. */
  IR_OP_SIMD_DOT_F64,
  IR_OP_SIMD_DOT_F32,
  /* Float affine memory map:
   * rhs[i] = arguments[1] * lhs[i] + arguments[2] * rhs[i] + arguments[3].
   * lhs = src, rhs = dst, arguments[0] = element count. */
  IR_OP_SIMD_AFFINE_MAP_F64,
  IR_OP_SIMD_AFFINE_MAP_F32,
  /* In-place a[i] = exp(a[i]) over a float32 array (vectorized libm exp).
   * dest = array base, arguments[0] = element count. */
  IR_OP_SIMD_EXP_F32,
  /* SwiGLU gate: out[i] = silu(g[i]) * u[i] = (g[i] / (1 + exp(-g[i]))) * u[i],
   * over float32 arrays (in-place when out aliases g). dest = out base,
   * lhs = g base, rhs = u base, arguments[0] = element count. When rhs is the
   * sentinel "" the multiply is dropped (plain SiLU: out[i] = silu(g[i])). */
  IR_OP_SIMD_SILU_F32,
  /* Counted-loop reduction where each iteration adds (int64)trunc(CHAIN) to the
   * dest accumulator, with CHAIN a straight-line float64 expression in the loop
   * counter: x0 = (float64)i, then a sequence of {x*=k, x+=k, x-=k, x=k-x, x/=k}
   * steps. dest = int64 accumulator symbol; arguments[0] = trip count (a
   * compile-time INT constant); the remaining arguments are alternating
   * (op-code INT, constant FLOAT64) pairs describing the chain, applied to
   * (float64)i in order. Emitted only by ir_simd_i2f_reduce_pass after it proves
   * every per-element value fits int32 and the integer sum stays < 2^53, so an
   * AVX2 f64-lane kernel is bit-identical to the scalar loop. Direct-object
   * backend only. */
  IR_OP_SIMD_I2F_REDUCE_F64,
  /* General auto-vectorized counted unit-stride loop over a straight-line
   * float DAG. Emitted by ir_auto_vectorize_pass for loops the per-shape
   * recognizers above did not claim. The element width is carried in
   * instruction->float_bits (64 = f64x4 lanes / 8-byte elements, 32 = f32x8
   * lanes / 4-byte elements); both stride 32 bytes per vector iteration. The
   * body DAG is serialized into arguments[]:
   *   header (7 INT): [0] reduce_op (0 = element-wise map, 1 = '+' reduction)
   *                   [1] n_arrays  [2] n_nodes  [3] root_node
   *                   [4] n_consts  [5] n_scalars
   *                   [6] max_live (peak simultaneous live ymm)
   *   then n_arrays SYMBOL array-base operands (index k),
   *   then n_scalars SYMBOL loop-invariant scalar operands (read once at loop
   *   entry and broadcast -- a runtime coefficient like saxpy's `a`),
   *   then n_nodes nodes, each 3 INT operands (tag, op0, op1):
   *       tag 0=LOAD(op0=array idx) 1=IOTA 2=CONST(op0=const idx)
   *           3=ADD 4=SUB 5=MUL 6=DIV (op0,op1 = earlier node indices)
   *           7=SCALAR(op0=scalar idx)
   *           8=AND 9=OR 10=XOR (int lanes only; op0,op1 = node indices)
   *           11=SHL (int lanes only; op0 = node index, op1 = literal count),
   *   then n_consts FLOAT64 operands (the kernel narrows them to f32 when
   *   float_bits==32). The int form serializes consts as INT operands instead.
   * dest = reduction accumulator symbol (reduce_op==1) or stored array base
   * (reduce_op==0); lhs = trip count (SYMBOL or INT). Direct-object backend
   * only. The kernel replays the DAG over the packed lanes with stack-hoisted
   * constants + a scalar remainder; element-wise maps are bit-identical to the
   * scalar loop, '+' reductions reassociate like the sum/dot kernels. */
  IR_OP_SIMD_VLOOP_F64,
  /* Integer twin of IR_OP_SIMD_VLOOP_F64: int32/uint32 lanes (i32x8, 4-byte
   * elements, 32 bytes per vector iteration), same arguments[] serialization.
   * Body ops are + - * & | ^ and << by a literal count -- every one congruent
   * mod 2^32 -- so maps AND '+' reductions are BIT-EXACT against the scalar
   * loop (integer wraparound is associative; no float reassociation caveat).
   * Division, %, and >> are never emitted (not congruent / trapping). Emitted
   * by ir_auto_vectorize_int_pass. Direct-object backend only. */
  IR_OP_SIMD_VLOOP_I32,
  /* Vectorized SKIP-AHEAD for early-exit search loops (find / memchr /
   * mismatch). Replaces ONLY the loop counter's zero-init: dest(iv) = the
   * exact first index in [0, n) where the loop's exit predicate holds, else
   * n. The original scalar loop is left fully intact and re-runs from that
   * index, so it executes at most one hit iteration (+ the <lane tail) and
   * every exit path / side effect replays natively -- the recognizer only
   * has to prove the SKIPPED iterations are pure (load, compare, increment).
   * lhs = trip bound n (SYMBOL or INT); rhs = array base `a`.
   *   arguments[0] = predicate (INT: 0 == , 1 != , 2 < , 3 > , 4 <= , 5 >= ;
   *                  hit when `a[i] PRED rhs-value` -- ordered forms signed)
   *   arguments[1] = element kind (INT: 0 = 4-byte int32, 1 = 1-byte u8)
   *   arguments[2] = rhs kind (INT: 0 = literal, 1 = invariant scalar symbol,
   *                  2 = second array `b[i]`)
   *   arguments[3] = the rhs operand (INT literal or SYMBOL)
   * The kernel walks an align-to-32 scalar head, then 32-byte ALIGNED blocks
   * with vpcmpeq/vpcmpgt + movemask + bsf. Alignment makes sentinel searches
   * (an overstated n, e.g. strlen) safe: an aligned block never crosses into
   * a page the scalar loop would not itself touch, and the kernel stops at
   * the first hit block. Two-array reads on `b` are bound-limited instead
   * (both arrays must be valid for n, the memcmp contract). Bit-exact: the
   * returned index is exactly the scalar loop's first-exit index. Direct-
   * object backend only. */
  IR_OP_SIMD_FIND,
  /* Outer-loop lane vectorization of a reduction over an outer-IV-INVARIANT
   * inner counted loop carrying one float64 accumulator (a serial recurrence,
   * e.g. a divide chain). The outer loop `while(p<P){ inner; total += iacc; p++ }`
   * has identical independent iterations; this runs 4 of them in lockstep f64x4
   * lanes to hide the inner recurrence's latency (genuinely running all the
   * inner work, 4-wide), then accumulates the (lane-identical) result into total
   * with exact scalar adds. Serialized into arguments[]; see
   * ir_outer_vectorize_pass. dest = total accumulator; lhs = outer trip count P;
   * rhs = inner trip count N. Direct-object backend only. */
  IR_OP_SIMD_OUTER_LANE_F64,
  /* Vectorized linear-congruential recurrence reduction. Replaces a counted
   * loop `state = state*A + C; sum += (int64)(state & MASK); i++` whose state is
   * a uint32 carried serially -- normally unvectorizable -- by advancing 8 lanes
   * in lockstep via the closed form state_{k+8} = A^8*state_k + (sum_{j<8}A^j)*C
   * (all mod 2^32, exact under vpmulld), masking + widening each lane to int64,
   * and accumulating. A scalar remainder finishes iters % 8. Bit-exact vs the
   * scalar loop. dest = sum accumulator symbol; lhs = trip count (SYMBOL/INT);
   * rhs = state symbol (its value at loop entry is the seed); arguments[0]=A,
   * [1]=C, [2]=MASK (all INT, compile-time). Direct-object backend only. */
  IR_OP_SIMD_LCG_U32,
  /* Software prefetch hint (prefetcht0): lhs = a TEMP holding the fully
   * computed byte address. Advisory only -- never faults, no destination, a
   * no-op in the IR interpreter. Emitted by ir_optimize_prefetch.c for
   * indirect (gather) accesses whose future address is computable early. */
  IR_OP_PREFETCH,
  /* Branchless select (conditional move): dest = (lhs != 0) ? rhs :
   * arguments[0]. lhs is the condition, rhs the then-value, arguments[0] the
   * else-value (each a temp/symbol/int). Emitted by ir_optimize_if_convert.c
   * to replace a data-dependent register-only if/else diamond, lowered to a
   * cmov so an unpredictable branch becomes straight-line code. */
  IR_OP_SELECT
} IROpcode;

/* Chain operation codes for IR_OP_SIMD_BYTE_MAP arguments. Each step applies
 * `b = b <op> k` in uint8 (mod 256) arithmetic. The numeric values are part of
 * the IR contract between the recognizer and the backend kernel. */
typedef enum {
  IR_BYTE_MAP_ADD = 0,
  IR_BYTE_MAP_SUB = 1,
  IR_BYTE_MAP_MUL = 2,
  IR_BYTE_MAP_XOR = 3,
  IR_BYTE_MAP_AND = 4,
  IR_BYTE_MAP_OR = 5
} IRByteMapOp;

typedef struct {
  IROpcode op;
  SourceLocation location;
  IROperand dest;
  IROperand lhs;
  IROperand rhs;
  char *text;
  IROperand *arguments;
  size_t argument_count;
  int is_float;
  /* Width of the floating result when is_float is set: 32 or 64. 0 means
   * unspecified and is treated as 64 (double) for backward compatibility with
   * code paths that only ever produced float64. */
  int float_bits;
  /* Set on an IR_OP_LOAD whose loaded scalar is an UNSIGNED integer (uint8/16/32),
   * recorded from the pointee type at lowering time. A 32-bit load zero-extends
   * into the 64-bit register on x86-64, but the backend otherwise sign-extends a
   * 4-byte load into a temp (it cannot recover the load's signedness from the
   * untyped destination temp). Honoring this flag keeps an unsigned value's high
   * bits clean, so the 64-bit ops the fallback emits (compare, divide, (int64)
   * widening) see the true value instead of a sign-extended one. */
  int is_unsigned;
  /* This instruction allocates heap memory at runtime even though its opcode
   * doesn't say so (today: string '+' concatenation, which codegen lowers to a
   * heap-allocating kernel). Set by ir_lowering, consumed by the `@noalloc`
   * contract checker. IR_OP_NEW and allocator calls are recognized by opcode/
   * name and don't need it. */
  int allocates;
  ASTNode *ast_ref;
} IRInstruction;

typedef struct {
  const char *label;
  IRInstruction *instructions;
  size_t instruction_count;
  size_t first_instruction;
  size_t *successors;
  size_t successor_count;
  size_t *predecessors;
  size_t predecessor_count;
} IRBasicBlock;

typedef struct {
  char *name;
  uint32_t profile_id;
  char **parameter_names;
  char **parameter_types;
  size_t parameter_count;
  IRInstruction *instructions;
  size_t instruction_count;
  size_t instruction_capacity;
  IRBasicBlock *blocks;
  size_t block_count;
  size_t entry_block;
  int cfg_valid;
  // Function-decorator flags propagated from the AST (see ast.h):
  int is_inline;          // `@inline`  : force inline past the heuristic gate
  int is_inline_contract; // `@inline!` : every call inlines or compile error
  int is_noinline;        // `@noinline`: never inline this function
  int is_pure;            // `@pure`    : side-effect-free; enables call LICM
  int is_noalloc;         // `@noalloc` : proven allocation-free or error
  int is_test;            // `@test`    : compile-time unit test (mettle test)
} IRFunction;

typedef struct {
  char *name;
  char *filename;
  uint64_t line;
} IRProfileEntry;

/* One debugger variable registration site (--debug-hooks): the name and
 * type are embedded in binary tables and referenced by index, because a
 * string-literal call argument's ABI differs between the MIR and fallback
 * backends (flat cstring vs string-struct pointer). */
typedef struct {
  char *name;
  char *type_name;
} IRDebugLocalEntry;

typedef struct {
  IRFunction **functions;
  size_t function_count;
  size_t function_capacity;
  IRProfileEntry *profile_entries;
  size_t profile_entry_count;
  size_t profile_entry_capacity;
  IRDebugLocalEntry *debug_local_entries;
  size_t debug_local_entry_count;
  size_t debug_local_entry_capacity;
  /* Set once ir_program_eliminate_dead_functions has run. The binary backend
   * walks AST function declarations and treats a missing IR body as an
   * internal error; this flag tells it a missing body means "eliminated as
   * unreachable", which is expected, not a lowering bug. */
  int dead_functions_eliminated;
} IRProgram;

IROperand ir_operand_none(void);
IROperand ir_operand_temp(const char *name);
IROperand ir_operand_symbol(const char *name);
IROperand ir_operand_int(long long value);
/* Defaults float_bits to 64 (double) for backward compatibility. */
IROperand ir_operand_float(double value);
/* Like ir_operand_float but tags the IEEE-754 width (32 or 64). Any other
 * value is normalized to 64. */
IROperand ir_operand_float_sized(double value, int float_bits);
IROperand ir_operand_string(const char *value);
IROperand ir_operand_label(const char *name);
IROperand ir_operand_copy(const IROperand *operand);
void ir_operand_destroy(IROperand *operand);

IRFunction *ir_function_create(const char *name);
int ir_function_set_parameters(IRFunction *function, const char **parameter_names,
                               const char **parameter_types,
                               size_t parameter_count);
void ir_function_destroy(IRFunction *function);
int ir_function_append_instruction(IRFunction *function,
                                   const IRInstruction *instruction);
int ir_function_insert_instruction(IRFunction *function, size_t index,
                                   const IRInstruction *instruction);
void ir_function_clear_cfg(IRFunction *function);
int ir_function_rebuild_cfg(IRFunction *function);
const IRBasicBlock *ir_function_blocks(IRFunction *function,
                                       size_t *block_count);

IRProgram *ir_program_create(void);
void ir_program_destroy(IRProgram *program);
int ir_program_add_function(IRProgram *program, IRFunction *function);

IRProgram *ir_lower_program(ASTNode *program, TypeChecker *type_checker,
                            SymbolTable *symbol_table, char **error_message,
                            int emit_runtime_checks);
/* --explain: when enabled, lowering brackets EVERY loop (not just `@simd` ones)
 * with report-only markers (SIMD_ATTR_REPORT) so the optimizer can report what
 * became of each one. Set by the driver before ir_lower_program; only
 * meaningful when optimization will run. */
void ir_lowering_set_explain(int enabled);
int ir_program_dump(IRProgram *program, FILE *output);
/* Human-readable mnemonic for an opcode (e.g. "simd_dot_i8"), used by dumps and
 * the `--simd-report` diagnostics. */
const char *ir_opcode_name(IROpcode op);
int ir_instruction_dump(const IRInstruction *instruction,
                        char *buffer, size_t capacity);

/* --native-heap: retarget the allocation surface onto std/alloc's Mettle
 * allocator at the IR level, so the rewritten calls flow through the normal,
 * fully-optimized call path on every backend (MIR and legacy) instead of a
 * fragile backend-injected call. Rewrites, in every function:
 *   - IR_OP_NEW          -> IR_OP_CALL "mettle_heap_zeroed"(size)
 *   - call "malloc"      -> call "mettle_heap_alloc"
 *   - call "calloc"      -> call "mettle_heap_calloc"
 *   - call "realloc"     -> call "mettle_heap_realloc"
 *   - call "free"        -> call "mettle_heap_free"
 * Returns 1 on success, 0 on allocation failure. */
int ir_program_route_to_native_heap(IRProgram *program);

/* Executable-build dead code elimination: drops every function unreachable
 * from `main`. A function is considered referenced when any instruction of a
 * live function names it in `text` (direct calls), a SYMBOL operand
 * (function-pointer uses), or a STRING operand (dispatch-by-name). Programs
 * without a `main` (library objects) are left untouched. Run it after
 * inlining so fully-inlined helpers are swept too. Returns 1 on success
 * (including no-op), 0 on allocation failure. */
int ir_program_eliminate_dead_functions(IRProgram *program);

#endif // IR_H
