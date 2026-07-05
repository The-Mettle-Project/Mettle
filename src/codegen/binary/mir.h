#ifndef CODEGEN_BINARY_MIR_H
#define CODEGEN_BINARY_MIR_H

/* Machine-IR (MIR) for the direct-object backend.
 *
 * MIR is a flat list of near-machine instructions over VIRTUAL registers. It
 * sits between the IR and the byte encoder so a real linear-scan allocator can
 * keep short-lived temps in registers instead of round-tripping every value
 * through a stack home.
 *
 * Pipeline: IR --mir_lower--> MIR(vregs) --mir_regalloc--> MIR(physregs)
 *           --mir_encode--> bytes in BinaryFunctionContext.code.
 *
 * MIR opcodes map 1:1 onto the existing binary_emit_* encoders; this header
 * defines only the data model and construction helpers. Lowering, allocation,
 * and encoding live in mir_lower.c / mir_regalloc.c / mir_encode.c. */

#include "codegen/binary/internal.h"

#include <stddef.h>
#include <stdint.h>

/* A virtual register id. Physical registers are modeled separately (see
 * MirOperand): a vreg is an allocation unit the allocator assigns to a physreg
 * or a spill slot. MIR_VREG_NONE marks "no register". */
typedef int MirVregId;
#define MIR_VREG_NONE (-1)

typedef enum {
  MIR_RC_GP = 0,  /* general-purpose integer/pointer */
  MIR_RC_XMM = 1, /* scalar float/double in an XMM lane */
  MIR_RC_VEC = 2  /* packed SIMD vector in a YMM register (auto-vectorizer) */
} MirRegClass;

/* One virtual register: its class, byte width (1/2/4/8), and — filled in by the
 * allocator — either an assigned physical register or a spill-slot rbp offset. */
typedef struct {
  MirRegClass rclass;
  int width; /* 1,2,4,8 (GP); 4,8 (XMM); per-lane byte width for VEC */
  int lanes; /* VEC only: number of packed lanes (e.g. 4 for f64x4 in YMM) */
  /* Allocation result (set by mir_regalloc). */
  int assigned;      /* 1 once allocated */
  int in_register;   /* 1 = physical register, 0 = spilled to stack */
  int phys;          /* BinaryGpRegister or BinaryXmmRegister when in_register */
  int spill_offset;  /* positive rbp-relative offset when spilled (mem = [rbp-off]) */
  /* Liveness, computed by the allocator: first def and last use as MIR indices.
   * MIR_LIVE_NONE until seen. */
  int live_start;
  int live_end;
  /* Set by the allocator when the interval spans a MIR_CALL: the value must be
   * kept in a register the callee preserves (or spilled), since a call clobbers
   * the caller-saved registers. */
  int crosses_call;
  /* Set when this value's live interval spans a loop back-edge (extended across
   * [label, branch] during liveness): it is reused on every iteration, so the
   * spill heuristic prefers to evict a NON-loop-carried value instead -- evicting
   * a loop-carried base pointer / accumulator / induction var reloads it each
   * iteration, the opposite of what we want. */
  int loop_carried;
  /* Two-address coalescing hint: a source vreg of this value's defining op that
   * dies exactly at the def, so this value can reuse its register and the
   * encoder elides the `mov dst, a` copy. -1 (MIR_VREG_NONE) when absent. */
  int coalesce_hint;
  /* Set when this value's address is taken (IR_OP_ADDRESS_OF): it must be
   * memory-resident — the allocator never assigns it a register, so every use
   * loads and every def stores through its stack home, and a store through an
   * aliasing pointer is visible to a later by-name read. */
  int address_taken;
  /* Byte size of this value's stack home when address_taken. 0 means the
   * default single 8-byte slot (scalars and DIRECT small aggregates). An
   * INDIRECT struct local sets this to its struct size rounded up to 8 so the
   * home covers the whole aggregate (field stores reach past the first 8
   * bytes). Always a multiple of 8. */
  int home_bytes;
  /* Set for values defined by the prologue (parameters and the hidden
   * indirect-return pointer): they are ALL simultaneously live from function
   * entry, each arriving in its own incoming ABI register. Two such values must
   * therefore interfere even when each is used at only a single (shared)
   * instruction index -- their point-like [i,i] intervals would otherwise be
   * judged disjoint by the strict-overlap test and wrongly share a register
   * (the parallel param-homing move would then clobber one with the other). */
  int entry_live;
} MirVreg;
#define MIR_LIVE_NONE (-1)

typedef enum {
  MIR_OPK_NONE = 0,
  MIR_OPK_VREG,      /* a virtual register (reg.vreg) */
  MIR_OPK_PHYS,      /* a fixed physical reg (reg.phys, reg.rclass) */
  MIR_OPK_IMM,       /* integer immediate (imm) */
  MIR_OPK_FIMM,      /* float immediate: raw IEEE-754 bits in `imm`; the width
                        (4 or 8) comes from the consuming instruction. Encoded by
                        staging the bits through a GP reg + movd/movq. */
  MIR_OPK_MEM,       /* [base + index*scale + disp]; base/index are vreg ids or
                        MIR_VREG_NONE, may also be RBP-relative via phys_base */
  MIR_OPK_LABEL,     /* a branch/jump target label name (sym, borrowed) */
  MIR_OPK_SYMBOL,    /* a global/extern/function symbol name (sym, borrowed) */
  MIR_OPK_STACKHOME  /* rbp-relative home of an existing named symbol: [rbp-disp].
                        Used to read/write params/locals that already own a home
                        in BinaryFunctionContext; never allocated or spilled. */
} MirOperandKind;

/* A memory address: [base + index*scale + disp]. base/index are vreg ids, or
 * MIR_VREG_NONE when absent. When phys_base_valid, the base is the fixed
 * physical register phys_base instead of a vreg (e.g. RBP for homes/spills). */
typedef struct {
  MirVregId base;
  MirVregId index;
  int scale;          /* 1,2,4,8 (0/1 when no index) */
  int disp;
  int phys_base_valid;
  int phys_base;      /* BinaryGpRegister when phys_base_valid */
} MirMem;

typedef struct {
  MirOperandKind kind;
  MirVregId vreg;       /* VREG */
  int phys;             /* PHYS: BinaryGpRegister/BinaryXmmRegister */
  MirRegClass rclass;   /* PHYS: which bank */
  long long imm;        /* IMM */
  MirMem mem;           /* MEM */
  const char *sym;      /* LABEL/SYMBOL/STACKHOME name (borrowed, interned) */
  int disp;             /* STACKHOME: rbp-relative offset (mem = [rbp-disp]) */
} MirOperand;

/* MIR opcodes. Each maps onto one or a tiny fixed sequence of binary_emit_*
 * calls in mir_encode.c. Operand roles are documented per-op where non-obvious;
 * the common shape is dst = op(a, b). */
typedef enum {
  MIR_NOP = 0,

  /* data movement */
  MIR_MOV,        /* dst <- a (reg/imm/mem load/mem store depending on kinds) */
  MIR_LEA,        /* dst(reg) <- address of a(mem) */
  MIR_LEA_LOCAL,  /* dst(reg) <- address of the spill home of local vreg a. The
                     local is forced memory-resident (address_taken) so its
                     stack slot is its canonical storage; this leas that slot. */
  MIR_LEA_GLOBAL, /* dst(reg) <- address of global symbol a.sym (RIP-relative).
                     The global stays cached, but is flushed/reloaded around
                     pointer memory ops since the alias can read/write it. */
  MIR_LEA_FUNC,   /* dst(reg) <- address of function symbol a.sym (RIP-relative).
                     Used to initialize function-pointer values without falling
                     back to the stack-home backend. */
  MIR_LEA_CSTR,   /* dst(reg) <- address of the string literal a.sym (RIP-relative
                     lea into a .rdata cstring). Carries no vreg source, so the
                     allocator ignores it. Used to pass a string-literal call
                     argument. */
  MIR_MOVZX,      /* dst <- zero-extend a (width from a.mem/src width to dst) */
  MIR_MOVSX,      /* dst <- sign-extend a */
  MIR_LOAD_GLOBAL,/* dst <- value of global scalar a(SYMBOL); width/is_unsigned
                     give the load size and signedness. Emitted once at entry to
                     cache a global in a register (leaf fns only). */
  MIR_STORE_GLOBAL,/* global scalar a(SYMBOL) <- b(value vreg); width gives the
                      store size. Emitted before each return to write a
                      register-promoted global back to memory (leaf fns only). */

  /* integer ALU: dst = dst OP a   (two-address; lowering pre-copies into dst) */
  MIR_ADD,
  MIR_SUB,
  MIR_AND,
  MIR_OR,
  MIR_XOR,
  MIR_IMUL,       /* dst = dst * a (or dst = a * imm via b when IMM present) */
  MIR_NEG,        /* dst = -dst */
  MIR_NOT,        /* dst = ~dst */
  MIR_SHL,        /* dst <<= a (a is IMM or CL/phys) */
  MIR_SHR,        /* logical >> */
  MIR_SAR,        /* arithmetic >> */

  /* signed/unsigned divide: uses RDX:RAX, result in RAX(quot)/RDX(rem).
   * Modeled with fixed-phys constraints; lowering sets a=divisor vreg. */
  MIR_CQO,        /* sign-extend RAX into RDX */
  MIR_XOR_RDX,    /* zero RDX (unsigned divide) */
  MIR_IDIV,       /* signed divide RDX:RAX / a */
  MIR_DIV,        /* unsigned divide */
  MIR_MULHI,      /* dst = high 64 bits of (a * b); is_unsigned picks mul vs
                     imul. b is the magic IMM (or a reg). Uses RAX:RDX like a
                     divide; emitted by constant-divisor strength reduction. */

  /* compares + materialization */
  MIR_CMP,        /* flags = a - b */
  MIR_TEST,       /* flags = a & b */
  MIR_SETCC,      /* dst(8-bit) <- cc (imm carries x86 setcc opcode) */
  MIR_CMOVCC,     /* dst <- a if cc (imm carries cmov opcode) */

  /* control flow */
  MIR_JMP,        /* -> label */
  MIR_JCC,        /* test a; cc -> label (cc carries jcc opcode) */
  MIR_CMPBR,      /* cmp a,b; cc -> label (fused compare-and-branch) */
  MIR_LABEL,      /* defines label (sym) at this point */
  MIR_PREFETCH,   /* prefetcht0 [a]: a is a MEM operand (usually [vreg+0]).
                     A read-only use of the address register(s); no def, and
                     the access never faults. */
  MIR_CMOV,       /* dst = (a != 0) ? b : dst. dst is PRE-LOADED with the
                     else-value by a preceding MIR_MOV, so its live range
                     starts there and overlaps a/b at this point (forcing
                     distinct registers). Encodes to `test a,a; cmovnz dst,b`.
                     b must be a register (cmov has no immediate source). */

  /* calls / return (Stage 3 for full ABI; declared now for completeness) */
  MIR_CALL,       /* call sym; clobbers volatiles */
  MIR_CALL_INDIRECT, /* call a(reg); clobbers volatiles */
  MIR_STORE_OUTARG,/* store outgoing stack call argument a to [rsp + b.imm].
                      Used for the 5th+ GP argument (beyond the ABI's argument
                      registers); the prologue reserves the outgoing region. The
                      encoder adds outgoing_indirect_bytes (the struct-arg copy
                      region sits below the shadow/stack-arg area). */
  MIR_LEA_OUTARG, /* dst <- lea [rsp + a.imm]: address of a slot in the outgoing
                     INDIRECT struct-argument copy region (at the bottom of the
                     frame, rsp-relative). Used to pass a struct by value. */
  MIR_TRAP,       /* terminal runtime trap: puts(a.sym)+exit(1). a.sym is the
                     abort message. Reached only on a cold guard-fail path and
                     never returns, so it needs no vreg operands and the
                     allocator treats it as a non-call (its volatile clobbers
                     never reach the normal path). */
  MIR_RET,        /* function return (epilogue emitted separately) */

  /* float scalar (Stage 3) */
  MIR_FADD,
  MIR_FSUB,
  MIR_FMUL,
  MIR_FDIV,
  MIR_CVTSI2F,    /* xmm dst <- int a */
  MIR_CVTF2SI,    /* gp dst  <- float a (truncating) */
  MIR_CVTF2F,     /* float width convert (sd<->ss) */
  MIR_UCOMIS,     /* float compare -> flags */
  MIR_FSETCC,     /* dst <- (a CMP b) as 0/1: ucomis a,b; setcc; movzx (cc set) */
  MIR_FCMPBR,     /* ucomis a,b; jcc -> label (fused float compare-and-branch) */
  MIR_MOVD_TO_XMM,
  MIR_MOVD_TO_GP,

  /* packed SIMD (auto-vectorizer; see VECTORIZER_DESIGN.md). width = per-lane
   * bytes, lane count from the vreg. */
  MIR_VADD,        /* vaddpd/vaddps */
  MIR_VSUB,
  MIR_VMUL,
  MIR_VDIV,
  MIR_VCVTSI2F,    /* vcvtdq2pd: int32 lanes -> f64 lanes */
  MIR_VCVTF2SI,    /* vcvttpd2dq: f64 lanes -> int32 lanes (truncating) */
  MIR_VLOAD,       /* vmovupd dst <- [mem] */
  MIR_VSTORE,      /* vmovupd [mem] <- a */
  MIR_VBROADCAST,  /* vbroadcastsd: scalar/imm -> all lanes */
  MIR_VIOTA,       /* lane i <- base + i (induction vector) */
  MIR_VHREDUCE,    /* horizontal add/min/max of all lanes -> scalar xmm */

  /* SLP multiply-accumulate kernel, emitted inline inside an otherwise
   * register-allocated function (so the surrounding outer loops keep MIR-quality
   * codegen instead of dropping the whole function to the spill-everything
   * fallback). Call-like: the lowering marshals a_ptr->RCX, b_ptr->RDX,
   * out_ptr->R8, count->R9 with preceding MIR_MOVs (exactly like call args), and
   * this op emits the pure inner loop. dst.imm = K (4 or 8); a.imm = row stride
   * in BYTES (baked as an imm32 b-advance). Clobbers RAX/RCX/RDX/R8/R9/R10/R11 +
   * xmm0..3, so the allocator treats it like a call (no live value crosses it in
   * a volatile register). */
  MIR_SIMD_SLP_MAC,

  /* Inline element-counted memset/fill kernel (IR_OP_SIMD_FILL), run in place so
   * the surrounding function keeps register-allocated codegen instead of
   * dropping to the spill-everything fallback. Call-like: the lowering marshals
   * base->RCX, element_count->R8, value->RAX with preceding MIR_MOVs, and this op
   * emits the splat-build + 16-byte-store loop + scalar tail. dst.imm = element
   * size in BYTES (1/2/4/8). Covers only the no-offset, no-live-iv-writeback
   * subset (the frame-clear / `a[i] = c` shape); other fill forms stay in the
   * fallback. Clobbers RAX/RCX/RDX/R8/R9 + xmm0, so the allocator treats it like
   * a call. */
  MIR_SIMD_FILL,

  /* Inline float32 affine-map kernel (IR_OP_SIMD_AFFINE_MAP_F32, the
   * `dst[i] = a*src[i] + b*dst[i] + c` / float-copy class) run in place so the
   * function keeps register-allocated codegen. Call-like: the lowering marshals
   * src->RCX, dst->RDX, count->R8 with preceding MIR_MOVs; this op materializes
   * the (compile-time) coefficient broadcasts and emits the AVX2 loop + scalar
   * tail + vzeroupper. dst.imm/a.imm/b.imm carry the a/b/c float bits; cc carries
   * b_is_one|b_is_zero<<1|c_is_zero<<2. Clobbers RAX/RCX/RDX/R8/R9/R10 + ymm0-5. */
  MIR_SIMD_AFFINE_MAP_F32,

  /* Inline float64 affine-map kernel (IR_OP_SIMD_AFFINE_MAP_F64, the saxpy
   * `dst[i] = a*src[i] + b*dst[i] + c` class) run in place so the function keeps
   * register-allocated codegen instead of dropping to the spill-everything
   * fallback. Identical shape to the F32 variant but 4-wide f64 (vfmadd231pd):
   * lowering marshals src->RCX, dst->RDX, count->R8; this op materializes the
   * 64-bit coefficient broadcasts and emits the AVX2 loop + scalar tail +
   * vzeroupper. dst.imm/a.imm/b.imm carry the a/b/c double bits; cc carries
   * b_is_one|b_is_zero<<1|c_is_zero<<2. Clobbers RAX/RCX/RDX/R8/R9/R11 + ymm0-5. */
  MIR_SIMD_AFFINE_MAP_F64,

  /* Inline float32 SiLU/SwiGLU gate (IR_OP_SIMD_SILU_F32) run in place. Call-like:
   * the lowering marshals g/out->RCX, u->RDX (SwiGLU only), count->R8; this op
   * emits the AVX2 exp-poly SiLU loop. dst.imm = has_mul (1 = SwiGLU `* u[i]`).
   * Clobbers RAX/RCX/RDX/R8/R9/R10/R11 + ymm0-7 and reserves a scratch frame. */
  MIR_SIMD_SILU_F32,

  /* Inline general auto-vectorized loop kernel (IR_OP_SIMD_VLOOP_F64 maps) run
   * in place so the function keeps register-allocated codegen. The DAG is too
   * large for MirOperands, so the op borrows a pointer to the source
   * IRInstruction in `aux`; the lowering marshals the <=3 distinct base pointers
   * into RCX/RDX/R8/R9 and the element count into the next arg register (the
   * kernel moves it to R10 -- R10/R11 are MIR scratch and unsafe to marshal
   * into). Call-like: clobbers the caller-saved set + ymm0-5. Maps only. */
  MIR_SIMD_VLOOP,

  MIR_OPCODE_COUNT
} MirOpcode;

/* One MIR instruction. dst/a/b are the general operand slots; mem is the address
 * for load/store/lea; width is the operation width in bytes; cc holds an x86
 * condition opcode for SETCC/CMOVCC/JCC. ir_index records the source IR
 * instruction (for debug line markers); -1 if synthetic. */
typedef struct {
  MirOpcode op;
  MirOperand dst;
  MirOperand a;
  MirOperand b;
  int width;       /* 1/2/4/8 */
  int is_float;
  int is_unsigned; /* affects shifts, divides, compares, extensions */
  unsigned char cc;/* x86 condition opcode for SETCC/CMOVCC/JCC */
  int ir_index;    /* source IR index, or -1 */
  const void *aux; /* MIR_SIMD_VLOOP: borrowed const IRInstruction* (the DAG) */
} MirInst;

/* A pooled (loop-invariant) float constant: its IEEE bits at `width`, and the
 * vreg materialized once near the loop that first uses it. */
typedef struct {
  uint64_t bits;
  int width;
  MirVregId vreg;
} MirFConst;

/* A pooled (loop-invariant) 64-bit integer constant: its raw value and the GP
 * vreg materialized once near the loop that first uses it. Used to hoist the
 * div/mod magic-multiply constant out of a loop so it is not re-materialized
 * with a 10-byte movabs every iteration. */
typedef struct {
  int64_t value;
  MirVregId vreg;
} MirIConst;

/* An incoming parameter: which vreg it lives in, its ABI argument index, and
 * how it must be extended from the (possibly narrow) incoming register to the
 * 64-bit value MIR computes with. */
typedef struct {
  MirVregId vreg;
  int arg_index; /* positional index among all parameters */
  int width;     /* 1/2/4/8 */
  int is_signed; /* sign-extend (1) vs zero-extend (0) into 64 bits */
  int is_float;  /* arrives in an XMM register (float32/float64) */
} MirParam;

/* Upper bound on parameters a MIR function can take. The first few arrive in
 * ABI registers; the rest are homed from the caller's stack frame. 16 covers
 * essentially all real signatures while keeping the fixed per-function param
 * arrays small. */
#define MIR_MAX_PARAMS 16

typedef struct {
  MirVreg *vregs;
  size_t vreg_count;
  size_t vreg_capacity;

  MirInst *insns;
  size_t insn_count;
  size_t insn_capacity;

  /* Owned label-name strings synthesized by layout passes (e.g. cold-block
   * sinking). The encoder strdups label names into its tables, so these only
   * need to outlive mir_encode; freed in mir_function_destroy. */
  char **owned_syms;
  size_t owned_sym_count;
  size_t owned_sym_capacity;

  /* Borrowed: the function context owning stack homes, ABI, fixup tables, and
   * the output code buffer; and the code generator (for type queries, fixup
   * resolution, and error reporting). Not owned by the MIR function. */
  BinaryFunctionContext *context;
  CodeGenerator *generator;

  /* Incoming parameters (GP only in Stage 2), consumed by the encoder prologue
   * to move ABI arg registers into the param vregs with correct extension. */
  MirParam params[MIR_MAX_PARAMS];
  size_t param_count;

  /* INDIRECT struct return (Win64: hidden out-pointer in RCX, SysV: RDI). When
   * set, the prologue homes that register into indirect_return_vreg (shifting
   * every user parameter up one ABI slot), and each RETURN copies the struct
   * into [indirect_return_vreg] and leaves the pointer in RAX. */
  int returns_indirect;
  int indirect_return_size;       /* struct size in bytes (>8, INDIRECT) */
  MirVregId indirect_return_vreg; /* holds the hidden out-pointer */

  /* A sub-64-bit integer return type: its byte width (1/2/4) and signedness.
   * RETURN canonicalizes the value to 64 bits (sign/zero-extend) before `mov
   * rax` so callers using the full register read no garbage. 0 = not narrow. */
  int scalar_return_width;
  int scalar_return_signed;

  /* A float return type: declared width in bits (32/64). RETURN converts the
   * value to this width before placing it in XMM0 — a float64-tracked temp
   * returned from a float32 function must cvtsd2ss, not pass through raw. */
  int float_return_bits;

  /* Divmod fusion: when `x / d` and `x % d` appear together, one div produces
   * both quotient (RAX) and remainder (RDX). Lowering the first of the pair
   * captures both and records the sibling's IR dest name -> the vreg holding the
   * result it needs, so the sibling lowers to a plain move (no second div). */
  struct {
    const char *name; /* sibling IR dest temp/symbol name (borrowed) */
    MirVregId vreg;   /* vreg already holding its quotient/remainder */
  } divmod_precomp[16];
  size_t divmod_precomp_count;

  /* Loop-invariant float constants materialized once near their first hot loop. */
  MirFConst *fconsts;
  size_t fconst_count;
  size_t fconst_capacity;

  /* Loop-invariant 64-bit integer constants (div/mod magic numbers) materialized
   * once near their first hot loop instead of re-emitted per iter. */
  MirIConst *iconsts;
  size_t iconst_count;
  size_t iconst_capacity;

  /* Bytes of spill area the allocator appended below the existing frame; the
   * encoder grows the prologue allocation by this much. */
  int spill_bytes;

  /* Max bytes of outgoing stack-argument space any call in this function needs
   * (for calls with more GP arguments than the ABI has argument registers).
   * Reserved once at the bottom of the frame, above the shadow space, so calls
   * write stack args at a fixed rsp offset without adjusting rsp in-body. */
  int outgoing_stack_bytes;

  /* Max bytes any single call needs for copying INDIRECT (by-value) struct
   * arguments. The Win64/SysV ABI passes such a struct as a pointer to a
   * caller-made copy; this region (at the very bottom of the frame, below the
   * shadow space) holds those copies. 16-aligned. */
  int outgoing_indirect_bytes;

  /* Set by the encoder when it emits an inline vector kernel (MIR_SIMD_SLP_MAC).
   * Such a kernel leaves the YMM upper halves dirty; the epilogue emits one
   * vzeroupper before returning so a caller using legacy SSE pays no AVX->SSE
   * transition penalty. Doing it once per function (not per kernel invocation)
   * keeps tiled inner loops cheap. */
  int used_inline_vector;

  /* Set during lowering when this function passes a float argument to a call in
   * an XMM register (XMM0..XMM3). Those registers are then removed from the
   * float allocation pool for the whole function (only the callee-saved XMM8..15
   * remain), exactly as the GP integer arg registers are never allocatable: it
   * guarantees no allocated value ever sits in an outgoing XMM argument register,
   * so the sequence of arg-homing moves before a call can never clobber a
   * not-yet-consumed argument source (the parallel-move hazard for 2+ float
   * args). Single-float-arg calls are safe regardless, but the exclusion is
   * applied uniformly for simplicity. */
  int has_xmm_arg_call;

  /* --annotate-asm: index of the IR instruction currently being lowered. The
   * mir_emit chokepoint stamps it onto every MirInst whose ir_index is still
   * unset (-1), so each emitted op can be traced back to its source line. Inert
   * unless the annotator is enabled. */
  int cur_ir_index;

  int has_error;
} MirFunction;

/* ---- construction ------------------------------------------------------- */

void mir_function_init(MirFunction *fn, BinaryFunctionContext *context);
void mir_function_destroy(MirFunction *fn);

/* Create a fresh virtual register; returns its id or MIR_VREG_NONE on OOM
 * (which also sets fn->has_error). */
MirVregId mir_new_vreg(MirFunction *fn, MirRegClass rclass, int width);

/* Append an instruction; returns 0 on OOM (and sets fn->has_error). */
int mir_emit(MirFunction *fn, const MirInst *inst);

/* Operand builders. */
MirOperand mir_op_none(void);
MirOperand mir_op_vreg(MirVregId v);
MirOperand mir_op_phys(int phys, MirRegClass rclass);
MirOperand mir_op_imm(long long value);
MirOperand mir_op_fimm(uint64_t ieee_bits);
MirOperand mir_op_label(const char *name);
MirOperand mir_op_symbol(const char *name);
MirOperand mir_op_stackhome(const char *name, int rbp_disp);
MirOperand mir_op_mem_vreg(MirVregId base, MirVregId index, int scale, int disp);
MirOperand mir_op_mem_rbp(int rbp_disp);

/* Debug dump of a MIR function to a FILE (used under METTLE_MIR_DUMP). */
void mir_function_dump(const MirFunction *fn, FILE *out);

/* Human-readable mnemonic for a MIR opcode (e.g. "mov", "simd_fill"). Used by
 * the dump and by the --annotate-asm codegen annotator. */
const char *mir_opcode_name(MirOpcode op);

/* ---- passes ------------------------------------------------------------- */

/* Assign every vreg a physical register or spill slot via linear scan. Returns
 * 0 on failure (sets fn->has_error). Defined in mir_regalloc.c. */
int mir_regalloc(MirFunction *fn);

/* Encode an allocated MIR function into fn->context->code, emitting prologue and
 * epilogue and populating the context's label/relocation tables. Returns 0 on
 * failure. Defined in mir_encode.c. */
int mir_encode(MirFunction *fn);

/* ---- driver hooks (defined in mir_lower.c) ------------------------------ */

/* True if every instruction and the signature of `ir_function` are in the
 * Stage 2 supported scalar-integer subset (no calls/floats/aggregates/
 * address-of, <=4 GP params, plain --release). */
int mir_function_is_eligible(CodeGenerator *generator,
                             FunctionDeclaration *function_data,
                             IRFunction *ir_function);

/* Lower + allocate + encode an eligible function into context->code (full
 * prologue..epilogue, fixups resolved). Returns 0 on failure. */
int code_generator_binary_emit_function_via_mir(
    CodeGenerator *generator, FunctionDeclaration *function_data,
    IRFunction *ir_function, BinaryFunctionContext *context);

#endif /* CODEGEN_BINARY_MIR_H */
