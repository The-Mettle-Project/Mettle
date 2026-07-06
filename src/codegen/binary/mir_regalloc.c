#include "codegen/binary/mir.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Linear-scan register allocation over MIR.
 *
 * Design choices that buy correctness cheaply:
 *  - RAX/RCX/RDX are NOT allocatable; they are left as encoder scratch. This
 *    means fixed-physreg ops (IDIV/DIV need RDX:RAX, variable shifts need CL)
 *    never have to be modeled as interval constraints — the encoder just moves
 *    vreg operands through the scratch regs. 11 GP regs remain allocatable,
 *    more than the legacy promoter's 7.
 *  - Allocatable GP, in preference order: volatile R8..R11 first (a leaf
 *    function need not save them), then nonvolatile RBX,RSI,RDI,R12..R15
 *    (saved/restored by the encoder's prologue/epilogue when used).
 *  - Liveness is computed over the linear MIR order, then conservatively
 *    extended across every backward branch to a fixpoint so a value live around
 *    a loop stays allocated across the back-edge. Over-extension only costs
 *    register pressure, never correctness.
 *  - When no register is free, the longest-remaining interval is spilled to a
 *    fresh rbp-relative slot (classic linear-scan "spill at interval"). */

/* Preference-ordered GP allocation pool. Volatile first.
 *
 * The incoming/outgoing argument registers are excluded so that no allocatable
 * register is ever an ABI argument register on EITHER calling convention:
 *   - Win64 args: RCX, RDX, R8, R9 (RCX/RDX are already scratch).
 *   - SysV  args: RDI, RSI, RDX, RCX, R8, R9.
 * Excluding R8/R9 AND RSI/RDI means parameter homing (prologue) and outgoing
 * call-argument moves can never clobber a not-yet-consumed argument that still
 * lives in one of those registers — the parallel-move hazard cannot arise on
 * Windows or Linux. The remaining pool is R10/R11 (volatile) plus the
 * universally callee-saved RBX/R12..R15. */
static const BinaryGpRegister MIR_GP_POOL[] = {
    BINARY_GP_RBX, BINARY_GP_R12, BINARY_GP_R13, BINARY_GP_R14, BINARY_GP_R15};
#define MIR_GP_POOL_COUNT (sizeof(MIR_GP_POOL) / sizeof(MIR_GP_POOL[0]))
/* Reclaimable registers, tried after the callee-saved base pool. RAX/RCX/RDX
 * are now allocatable (the encoder scratch moved to R10/R11): they are volatile
 * — so a cross-call value never lands in them (it uses MIR_GP_CROSSCALL_POOL) —
 * and they carry implicit clobbers from the divide family, setcc, and variable
 * shifts, which mir_reg_clobbered_in_range keeps a live value out of. RAX is not
 * an ABI argument register (poolable even in non-leaf functions); RCX/RDX/R8/R9
 * are, so they are reclaimed only in leaf functions (mir_reg_poolable). RSI/RDI
 * are callee-saved on Win64. */
static const BinaryGpRegister MIR_GP_EXTRA[] = {
    BINARY_GP_RAX, BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_RSI,
    BINARY_GP_RDI, BINARY_GP_R8,  BINARY_GP_R9};
#define MIR_GP_EXTRA_COUNT (sizeof(MIR_GP_EXTRA) / sizeof(MIR_GP_EXTRA[0]))
/* Upper bound on the leaf pool: the static base plus every extra. */
#define MIR_GP_LEAF_POOL_MAX (MIR_GP_POOL_COUNT + MIR_GP_EXTRA_COUNT)

/* True if the function makes any call (so caller-saved regs are unsafe to hold
 * values across, and outgoing-arg registers must not be reclaimed). */
static int mir_fn_has_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    /* The inline SLP kernel clobbers the ABI argument/caller-saved registers, so
     * the function must be treated as non-leaf: RCX/RDX/R8/R9 are unsafe to
     * reclaim into the general pool (the kernel marshalling overwrites them). */
    if (fn->insns[i].op == MIR_CALL ||
        fn->insns[i].op == MIR_CALL_INDIRECT ||
        fn->insns[i].op == MIR_SIMD_SLP_MAC ||
        fn->insns[i].op == MIR_SIMD_FILL ||
        fn->insns[i].op == MIR_SIMD_AFFINE_MAP_F32 ||
        fn->insns[i].op == MIR_SIMD_AFFINE_MAP_F64 ||
        fn->insns[i].op == MIR_SIMD_SILU_F32 ||
        fn->insns[i].op == MIR_SIMD_VLOOP) {
      return 1;
    }
  }
  return 0;
}

/* True if the function makes a REAL call (MIR_CALL / MIR_CALL_INDIRECT), as
 * opposed to an inline SIMD kernel. Used for LEAF-POOL BUILDING: a real call
 * clobbers all caller-saved registers with no per-clobber PHYS write for the
 * allocator to see, so arg registers stay out of the general pool. An inline
 * kernel is different -- it marshals its operands through explicit
 * `MIR_MOV phys(RCX/RDX/R8/R9/RAX), value` writes (which mir_reg_clobbered_in_range
 * detects) and is itself a crosses_call barrier (which bars spanning values from
 * the arg registers). Both per-vreg mechanisms run regardless of the pool, so a
 * kernel-only function can keep the full leaf pool; the histogram/RMW loops that
 * run AFTER a one-time `simd_fill` init were needlessly spilling because the fill
 * shrank the whole function's pool (radix_sort). */
static int mir_fn_has_real_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op == MIR_CALL || fn->insns[i].op == MIR_CALL_INDIRECT) {
      return 1;
    }
  }
  return 0;
}

/* True if the function contains an inline SLP vector kernel. Such kernels
 * marshal through fixed registers without representing every clobber as a MIR
 * PHYS write, so when the frame pointer is omitted we conservatively keep rbp
 * out of the allocatable pool for these functions (they are matmul/dot kernels
 * that already beat gcc and gain nothing from one more GP register). */
static int mir_fn_uses_slp(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op == MIR_SIMD_SLP_MAC ||
        fn->insns[i].op == MIR_SIMD_FILL ||
        fn->insns[i].op == MIR_SIMD_AFFINE_MAP_F32 ||
        fn->insns[i].op == MIR_SIMD_AFFINE_MAP_F64 ||
        fn->insns[i].op == MIR_SIMD_SILU_F32 ||
        fn->insns[i].op == MIR_SIMD_VLOOP) {
      return 1;
    }
  }
  return 0;
}

/* Integer argument-register index of `reg` under the active ABI, or -1 if it is
 * not an argument register at all. */
static int mir_reg_arg_index(BinaryGpRegister reg) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  if (abi && abi->int_param_registers) {
    for (size_t i = 0; i < abi->int_param_count; i++) {
      if (abi->int_param_registers[i] == reg) {
        return (int)i;
      }
    }
  }
  return -1;
}

/* Whether `reg` may join the non-cross-call allocatable pool.
 * An arg register is poolable only when it carries NO incoming parameter (its
 * arg index >= param_count, so homing never reads it) AND, if it is an arg
 * register at all, its live range must not cross a call (handled separately by
 * MIR_GP_CROSSCALL_POOL). Non-arg callee-saved registers (RSI/RDI on Win64) are
 * always poolable. This keeps parameter homing a plain sequential copy — no
 * arg register is ever both a homing source and an allocation target. */
static int mir_reg_poolable(BinaryGpRegister reg, size_t param_count,
                            int is_leaf) {
  (void)is_leaf;
  int ai = mir_reg_arg_index(reg);
  if (ai < 0) {
    return 1; /* not an arg register: safe on both ABIs */
  }
  if ((size_t)ai < param_count) {
    return 0; /* holds a live incoming parameter -> homing source */
  }
  return 1; /* dead arg reg: call clobbers are handled by liveness/masks */
}

/* Build the non-cross-call GP pool: the universally-safe base, then any
 * arg-capable register the function does not need for its own parameters. On
 * Win64 this reclaims RSI/RDI (callee-saved, never args) plus trailing unused
 * arg registers; on SysV it reclaims every arg register past the parameter
 * count. Reclaimed nonvolatiles are saved by the used-nonvolatile machinery;
 * caller-saved regs are used only for values that do not cross calls. */
static size_t mir_build_gp_leaf_pool(BinaryGpRegister *out, size_t param_count,
                                     int is_leaf) {
  size_t n = 0;
  for (size_t i = 0; i < MIR_GP_POOL_COUNT; i++) {
    out[n++] = MIR_GP_POOL[i];
  }
  for (size_t i = 0; i < MIR_GP_EXTRA_COUNT; i++) {
    if (mir_reg_poolable(MIR_GP_EXTRA[i], param_count, is_leaf)) {
      out[n++] = MIR_GP_EXTRA[i];
    }
  }
  return n;
}

/* Max cross-call GP pool: Win64 can also use RSI/RDI (nonvolatile there), while
 * SysV must keep them out because calls may clobber argument registers. */
#define MIR_GP_CROSSCALL_POOL_MAX 7

static size_t mir_build_gp_crosscall_pool(BinaryGpRegister *out) {
  size_t n = 0;
  const BinaryAbi *abi = code_generator_binary_active_abi();
  int sysv = abi && abi->counts_classes_separately;
  out[n++] = BINARY_GP_RBX;
  if (!sysv) {
    out[n++] = BINARY_GP_RSI;
    out[n++] = BINARY_GP_RDI;
  }
  out[n++] = BINARY_GP_R12;
  out[n++] = BINARY_GP_R13;
  out[n++] = BINARY_GP_R14;
  out[n++] = BINARY_GP_R15;
  return n;
}

/* XMM pool: Win64 volatile lanes XMM0..XMM3. XMM4/XMM5 are reserved as the two
 * float scratch registers the encoder uses (analogous to RAX/RCX for GP) — for
 * staging spilled/immediate float operands and breaking non-commutative
 * aliasing. All are caller-saved, so a leaf function need not preserve them. */
static const BinaryXmmRegister MIR_XMM_POOL[] = {
    BINARY_XMM0, BINARY_XMM1, BINARY_XMM2, BINARY_XMM3};
#define MIR_XMM_POOL_COUNT (sizeof(MIR_XMM_POOL) / sizeof(MIR_XMM_POOL[0]))

/* Second-tier XMM pool: xmm8..xmm15. These are callee-saved on Win64 (the
 * prologue saves/restores the ones used) and caller-saved on SysV; either way a
 * value placed here that does NOT live across a call is correct. They are
 * argument registers on neither ABI (Win64 floats: xmm0-3; SysV: xmm0-7), so no
 * parameter-homing or call-marshalling hazard arises. Tried only after the
 * volatile xmm0-3 are exhausted, so leaf code with light float pressure pays no
 * save/restore. */
static const BinaryXmmRegister MIR_XMM_NONVOL_POOL[] = {
    BINARY_XMM8,  BINARY_XMM9,  BINARY_XMM10, BINARY_XMM11,
    BINARY_XMM12, BINARY_XMM13, BINARY_XMM14, BINARY_XMM15};
#define MIR_XMM_NONVOL_POOL_COUNT \
  (sizeof(MIR_XMM_NONVOL_POOL) / sizeof(MIR_XMM_NONVOL_POOL[0]))

static int mir_gp_is_nonvolatile(BinaryGpRegister reg) {
  return code_generator_binary_gp_register_is_win64_nonvolatile(reg);
}

/* Record each vreg use/def site into the vreg's [live_start, live_end]. */
static void mir_note_operand_liveness(MirFunction *fn, const MirOperand *op,
                                      int index) {
  if (!op) {
    return;
  }
  MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
  if (op->kind == MIR_OPK_VREG) {
    ids[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    ids[0] = op->mem.base;
    ids[1] = op->mem.index;
  }
  for (int k = 0; k < 2; k++) {
    MirVregId v = ids[k];
    if (v < 0 || (size_t)v >= fn->vreg_count) {
      continue;
    }
    MirVreg *vr = &fn->vregs[v];
    if (vr->live_start == MIR_LIVE_NONE || index < vr->live_start) {
      vr->live_start = index;
    }
    if (vr->live_end == MIR_LIVE_NONE || index > vr->live_end) {
      vr->live_end = index;
    }
  }
}

/* Find the MIR index of a label definition, or -1. */
static int mir_find_label(const MirFunction *fn, const char *name) {
  if (!name) {
    return -1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
        strcmp(in->dst.sym, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static void mir_compute_liveness(MirFunction *fn) {
  for (size_t i = 0; i < fn->vreg_count; i++) {
    fn->vregs[i].live_start = MIR_LIVE_NONE;
    fn->vregs[i].live_end = MIR_LIVE_NONE;
    fn->vregs[i].loop_carried = 0;
    fn->vregs[i].entry_live = 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    mir_note_operand_liveness(fn, &in->dst, (int)i);
    mir_note_operand_liveness(fn, &in->a, (int)i);
    mir_note_operand_liveness(fn, &in->b, (int)i);
  }

  /* Parameters are defined by the prologue, before any MIR instruction, so they
   * are live from index 0. This MUST happen before the loop-extension below: a
   * param used only inside a loop would otherwise have an interval sitting
   * entirely within the loop and be (wrongly) judged not to cross the loop
   * boundary, so it would not be extended across the back-edge and could share
   * a register with a loop-body temp — clobbering the param every iteration. */
  for (size_t i = 0; i < fn->param_count; i++) {
    MirVreg *pv = &fn->vregs[fn->params[i].vreg];
    if (pv->live_end != MIR_LIVE_NONE) {
      pv->live_start = 0;
      pv->entry_live = 1;
    }
  }
  /* The hidden INDIRECT-return out-pointer is also defined by the prologue, so
   * it is live from entry to its last use (the struct copy at each RETURN). */
  if (fn->returns_indirect && fn->indirect_return_vreg != MIR_VREG_NONE) {
    MirVreg *rv = &fn->vregs[fn->indirect_return_vreg];
    if (rv->live_end != MIR_LIVE_NONE) {
      rv->live_start = 0;
      rv->entry_live = 1;
    }
  }

  /* Conservatively extend intervals across backward branches (loops) to a
   * fixpoint. For each branch at B targeting a label at L < B, any vreg whose
   * interval crosses the [L,B] boundary must stay live across the whole loop. */
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      const MirOperand *target = NULL;
      if (in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR ||
          in->op == MIR_FCMPBR) {
        /* All carry the branch-target label in dst. A backward target makes this
         * a loop back-edge (e.g. a rotated loop's bottom-test CMPBR). */
        target = &in->dst;
      }
      if (!target || target->kind != MIR_OPK_LABEL) {
        continue;
      }
      int l = mir_find_label(fn, target->sym);
      int b = (int)i;
      if (l < 0 || l >= b) {
        continue; /* forward branch: no loop back-edge */
      }
      for (size_t v = 0; v < fn->vreg_count; v++) {
        MirVreg *vr = &fn->vregs[v];
        if (vr->live_start == MIR_LIVE_NONE) {
          continue;
        }
        /* interval overlaps [l,b]? */
        if (vr->live_end < l || vr->live_start > b) {
          continue;
        }
        /* crosses a boundary (defined before l, or used after b)? An
         * entry-live vreg (param / hidden out-pointer) is defined by the
         * prologue BEFORE instruction 0, so when the loop header is at index
         * 0 (tail-recursion loops) it crosses even though live_start == l. */
        int crosses = (vr->live_start < l) || (vr->live_end > b) ||
                      (vr->entry_live && l == 0);
        if (!crosses) {
          continue;
        }
        vr->loop_carried = 1; /* reused across this loop's back-edge */
        if (vr->live_start > l) {
          vr->live_start = l;
          changed = 1;
        }
        if (vr->live_end < b) {
          vr->live_end = b;
          changed = 1;
        }
      }
    }
  }
}

/* Order vregs by ascending live_start for the scan. Returns a malloc'd array of
 * vreg ids (caller frees), or NULL on OOM / when there are no live vregs. */
static MirVregId *mir_order_by_start(MirFunction *fn, size_t *count_out) {
  size_t live = 0;
  for (size_t i = 0; i < fn->vreg_count; i++) {
    if (fn->vregs[i].live_start != MIR_LIVE_NONE) {
      live++;
    }
  }
  *count_out = live;
  if (live == 0) {
    return NULL;
  }
  MirVregId *order = (MirVregId *)malloc(live * sizeof(MirVregId));
  if (!order) {
    fn->has_error = 1;
    return NULL;
  }
  size_t n = 0;
  for (size_t i = 0; i < fn->vreg_count; i++) {
    if (fn->vregs[i].live_start != MIR_LIVE_NONE) {
      order[n++] = (MirVregId)i;
    }
  }
  /* insertion sort by (live_start, then id) — vreg counts are small. */
  for (size_t i = 1; i < live; i++) {
    MirVregId key = order[i];
    int ks = fn->vregs[key].live_start;
    size_t j = i;
    while (j > 0) {
      int prev_s = fn->vregs[order[j - 1]].live_start;
      if (prev_s < ks || (prev_s == ks && order[j - 1] <= key)) {
        break;
      }
      order[j] = order[j - 1];
      j--;
    }
    order[j] = key;
  }
  return order;
}

/* Compute two-address coalescing hints: for each commutative 2-address op whose
 * result is a GP vreg, if a source operand is a GP vreg that DIES at this op,
 * record it as the destination's coalesce hint. The allocator then tries to
 * place the destination in that dying source's register, after which the
 * encoder emits the op in place (no `mov dst, a` copy). SUB is non-commutative,
 * so only its minuend (a) qualifies; IMUL-by-immediate is 3-operand already. */
static void mir_compute_coalesce_hints(MirFunction *fn) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    fn->vregs[v].coalesce_hint = MIR_VREG_NONE;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    int commutative;
    switch (in->op) {
    case MIR_ADD:
    case MIR_AND:
    case MIR_OR:
    case MIR_XOR:
    case MIR_IMUL:
    /* Float ops are two-address too (e.g. addsd D,b computes D=D+b), so the
     * encoder copies operand a into D unless the allocator already placed a (or,
     * for commutative ops, b) there. Hinting the dying source elides that
     * per-op movaps -- the dominant overhead in tight scalar-float loops. */
    case MIR_FADD:
    case MIR_FMUL:
      commutative = 1;
      break;
    case MIR_SUB:
    case MIR_FSUB:
    /* A plain register copy `dst <- a` is the most basic coalescing target: if a
     * dies at the copy, dst and a never overlap, so they can share a register and
     * the move disappears entirely (store_from/materialize elide a `mov R,R`).
     * This removes loop-carried rotation copies (e.g. an unrolled `a=b; b=next`)
     * that the per-op ALU/float coalescing above never sees. Loads/stores/
     * immediates (a not a vreg, or dst a memory store) fall out via the vreg/
     * same-class checks below. */
    case MIR_MOV:
      commutative = 0;
      break;
    default:
      continue;
    }
    if (in->dst.kind != MIR_OPK_VREG) {
      continue;
    }
    MirRegClass dcls = fn->vregs[in->dst.vreg].rclass;
    if (in->op == MIR_IMUL && in->b.kind == MIR_OPK_IMM) {
      continue; /* imul r, a, imm32 needs no copy */
    }
    MirVregId d = in->dst.vreg;
    MirVregId cand = MIR_VREG_NONE;
    /* Prefer the left operand when it dies here, even for commutative ops. The
     * frontend naturally lowers accumulator chains as left-associated adds
     * (`acc + t1`, then previous + `t2`); preserving that left register carries
     * the running value across fixed-register ops like MULHI and avoids spilling
     * the partial sum. If a is not a candidate, fall back to b. */
    if (in->a.kind == MIR_OPK_VREG && in->a.vreg != d &&
        fn->vregs[in->a.vreg].rclass == dcls &&
        fn->vregs[in->a.vreg].live_end == (int)i) {
      cand = in->a.vreg;
    } else if (commutative && in->b.kind == MIR_OPK_VREG && in->b.vreg != d &&
               fn->vregs[in->b.vreg].rclass == dcls &&
               fn->vregs[in->b.vreg].live_end == (int)i) {
      cand = in->b.vreg;
    }
    fn->vregs[d].coalesce_hint = cand;
  }
}

/* True if a value occupying `reg` would be clobbered by an instruction strictly
 * inside the interval (s, e). Only RAX/RCX/RDX carry implicit clobbers: the
 * divide family (IDIV/DIV/MULHI) writes RAX:RDX, setcc writes RAX, and a
 * variable-count shift routes its count through CL (RCX). Boundary instructions
 * (k == s or k == e) are the value's own def/last-use as a div/shift operand or
 * result, which the encoder places correctly, so they are not clobbers. Calls
 * are handled separately by crosses_call (a value spanning a call is barred from
 * all volatiles, including these three). */
static int mir_reg_clobbered_in_range(const MirFunction *fn,
                                      BinaryGpRegister reg, int s, int e) {
  int constrained = (reg == BINARY_GP_RAX || reg == BINARY_GP_RCX ||
                     reg == BINARY_GP_RDX);
  for (int k = s + 1; k < e; k++) {
    const MirInst *in = &fn->insns[k];
    /* An explicit write to this physical register (return value into RAX, ABI
     * argument setup, hidden-return pointer, ...) clobbers a value held there. */
    if (in->dst.kind == MIR_OPK_PHYS && in->dst.rclass == MIR_RC_GP &&
        in->dst.phys == (int)reg) {
      return 1;
    }
    if (!constrained) {
      continue;
    }
    switch (in->op) {
    case MIR_IDIV:
    case MIR_DIV:
    case MIR_MULHI:
      if (reg == BINARY_GP_RAX || reg == BINARY_GP_RDX) {
        return 1;
      }
      break;
    case MIR_CQO:
    case MIR_XOR_RDX:
      if (reg == BINARY_GP_RDX) {
        return 1;
      }
      break;
    case MIR_SETCC:
    case MIR_FSETCC:
      if (reg == BINARY_GP_RAX) {
        return 1;
      }
      break;
    case MIR_SHL:
    case MIR_SHR:
    case MIR_SAR:
      if (reg == BINARY_GP_RCX && in->b.kind != MIR_OPK_IMM) {
        return 1;
      }
      break;
    default:
      break;
    }
  }
  return 0;
}

/* ---- graph-coloring allocator (Chaitin-Briggs, optimistic) -----------------
 *
 * A second, higher-quality allocator that replaces the greedy linear scan's
 * local decisions with a global view: it builds the interference graph, picks
 * spill victims by a dynamic cost model (use density x loop weight) rather than
 * "farthest live_end", and biases copy-related values onto the same register so
 * the encoder elides the move. It reuses every correctness primitive the linear
 * scan established -- the same liveness, the same crosses_call / clobber-range /
 * address-taken / ABI-pool constraints -- so it can only differ in QUALITY, not
 * legality. Two simplifications make it a single pass with no spill-rewrite
 * loop: (1) a "spilled" vreg is simply memory-resident (the encoder loads/stores
 * it per access, exactly like an address-taken local), so a node that fails to
 * color is just marked in_register=0; (2) interference uses STRICT interval
 * overlap (`a.start < b.end && b.start < a.end`), which models "an instruction
 * reads its sources before writing its dest", so a value and the result that
 * consumes-and-overwrites it at the same point do NOT interfere -- preserving
 * the two-address sharing the linear scan got from its coalesce hint, now for
 * any copy, not just the def-point one. The SELECT phase assigns only a color
 * absent from every interfering neighbour and present in the node's allowed-
 * register mask, so the result is always a legal allocation. */

/* Allowed physical registers for `v`, as a bitmask over phys 0..15: the ABI
 * pool for its class/cross-call status, minus any register clobbered somewhere
 * inside its live range. Empty when the value must be memory-resident. */
static uint32_t mir_color_reg_mask(const MirFunction *fn, MirVregId v,
                                   const BinaryGpRegister *gp_leaf_pool,
                                   size_t gp_leaf_n,
                                   const BinaryGpRegister *gp_cross_pool,
                                   size_t gp_cross_n, int allow_rbp) {
  const MirVreg *vr = &fn->vregs[v];
  uint32_t m = 0;
  if (vr->rclass == MIR_RC_GP) {
    const BinaryGpRegister *pool =
        vr->crosses_call ? gp_cross_pool : gp_leaf_pool;
    size_t n = vr->crosses_call ? gp_cross_n : gp_leaf_n;
    for (size_t i = 0; i < n; i++) {
      BinaryGpRegister reg = pool[i];
      if (!mir_reg_clobbered_in_range(fn, reg, vr->live_start, vr->live_end)) {
        m |= 1u << reg;
      }
    }
    /* With the frame pointer omitted, rbp is a free callee-saved register —
     * usable for cross-call and leaf values alike — provided nothing writes it
     * in the live range. This is the FPO payoff: an extra register that removes
     * a spill (and, in call-heavy code, the rsp-relative stack access that would
     * otherwise force a stack-engine sync uop). */
    if (allow_rbp &&
        !mir_reg_clobbered_in_range(fn, BINARY_GP_RBP, vr->live_start,
                                    vr->live_end)) {
      m |= 1u << BINARY_GP_RBP;
    }
  } else if (vr->rclass == MIR_RC_XMM && !vr->crosses_call) {
    /* Volatile xmm0-3 then callee-saved xmm8-15. A cross-call XMM has no
     * register that survives the call on BOTH ABIs, so it stays memory (m=0).
     * When the function homes a float argument into an XMM register (xmm0-3),
     * those volatile lanes are excluded from the pool — exactly as the GP arg
     * registers always are — so no allocated value sits in an outgoing argument
     * register and the pre-call homing moves cannot clobber one another. */
    if (!fn->has_xmm_arg_call) {
      for (size_t i = 0; i < MIR_XMM_POOL_COUNT; i++) {
        m |= 1u << MIR_XMM_POOL[i];
      }
    }
    for (size_t i = 0; i < MIR_XMM_NONVOL_POOL_COUNT; i++) {
      m |= 1u << MIR_XMM_NONVOL_POOL[i];
    }
  }
  return m;
}

/* STRICT live-interval overlap (see header): touching at a single point is NOT
 * overlap, so a dying source and the result that overwrites it can share a
 * register. */
static int mir_color_interferes(const MirVreg *a, const MirVreg *b) {
  if (a->rclass != b->rclass) {
    return 0;
  }
  /* Two prologue-defined values (parameters / hidden return pointer) are both
   * live from entry, so they always interfere -- even when each is used at only
   * a single shared instruction index, where the strict-overlap test below would
   * (wrongly) judge their point intervals disjoint and let them share a register.
   * This only ADDS edges that genuinely exist; a non-degenerate param pair
   * already interferes via the interval test. */
  if (a->entry_live && b->entry_live) {
    return 1;
  }
  return a->live_start < b->live_end && b->live_start < a->live_end;
}

/* The Chaitin-Briggs core. Returns 1 on success (every vreg has assigned set,
 * to a register or a fresh stack slot), 0 on OOM. `*next_spill` is advanced for
 * each value that ends up memory-resident. */
static int mir_color_graph(MirFunction *fn, const BinaryGpRegister *gp_leaf_pool,
                           size_t gp_leaf_n,
                           const BinaryGpRegister *gp_cross_pool,
                           size_t gp_cross_n, int *next_spill, int allow_rbp) {
  size_t N = fn->vreg_count;
  if (N == 0) {
    return 1;
  }
  size_t words = (N + 63) / 64;
  uint64_t *inter = (uint64_t *)calloc(N * words, sizeof(uint64_t));
  uint32_t *mask = (uint32_t *)calloc(N, sizeof(uint32_t));
  int *degree = (int *)calloc(N, sizeof(int));
  int *cost = (int *)calloc(N, sizeof(int));
  int *colorable = (int *)calloc(N, sizeof(int));
  int *removed = (int *)calloc(N, sizeof(int));
  MirVregId *stack = (MirVregId *)malloc(N * sizeof(MirVregId));
  if (!inter || !mask || !degree || !cost || !colorable || !removed || !stack) {
    free(inter); free(mask); free(degree); free(cost); free(colorable);
    free(removed); free(stack);
    return 0;
  }
#define MIR_INTER_SET(a, b)                                                    \
  (inter[(size_t)(a) * words + ((size_t)(b) >> 6)] |= (uint64_t)1                \
                                                       << ((size_t)(b) & 63))
#define MIR_INTER_GET(a, b)                                                    \
  ((inter[(size_t)(a) * words + ((size_t)(b) >> 6)] >>                          \
    ((size_t)(b) & 63)) & 1u)

  /* Colorable set: live, not address-taken (those are already memory-resident).
   * Mask + per-operand access count (the spill cost, weighted up for loop-
   * carried values so the recurrence's hot registers are kept). */
  for (size_t v = 0; v < N; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->live_start == MIR_LIVE_NONE || vr->address_taken) {
      continue;
    }
    colorable[v] = 1;
    mask[v] = mir_color_reg_mask(fn, (MirVregId)v, gp_leaf_pool, gp_leaf_n,
                                 gp_cross_pool, gp_cross_n, allow_rbp);
    cost[v] = 1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    const MirOperand *ops[3] = {&in->dst, &in->a, &in->b};
    for (int k = 0; k < 3; k++) {
      const MirOperand *op = ops[k];
      MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
      if (op->kind == MIR_OPK_VREG) {
        ids[0] = op->vreg;
      } else if (op->kind == MIR_OPK_MEM) {
        ids[0] = op->mem.base;
        ids[1] = op->mem.index;
      }
      for (int j = 0; j < 2; j++) {
        MirVregId id = ids[j];
        if (id >= 0 && (size_t)id < N && colorable[id]) {
          cost[id]++;
        }
      }
    }
  }
  for (size_t v = 0; v < N; v++) {
    if (colorable[v] && fn->vregs[v].loop_carried) {
      cost[v] *= 8; /* a loop-carried value is touched every iteration */
    }
  }
  for (size_t i = 0; i < fn->iconst_count; i++) {
    MirVregId v = fn->iconsts[i].vreg;
    if (v >= 0 && (size_t)v < N && colorable[v]) {
      cost[v] *= 64; /* spilling a pooled movabs constant reloads every trip */
    }
  }
  for (size_t i = 0; i < fn->fconst_count; i++) {
    MirVregId v = fn->fconsts[i].vreg;
    if (v >= 0 && (size_t)v < N && colorable[v]) {
      cost[v] *= 32;
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LEA_FUNC && in->dst.kind == MIR_OPK_VREG) {
      MirVregId v = in->dst.vreg;
      if (v >= 0 && (size_t)v < N && colorable[v]) {
        cost[v] *= 128; /* loop-invariant indirect-call targets are hot reloads */
      }
    }
  }

  /* Interference graph: strict-overlapping colorable vregs of the same class. */
  for (size_t a = 0; a < N; a++) {
    if (!colorable[a]) {
      continue;
    }
    for (size_t b = a + 1; b < N; b++) {
      if (colorable[b] &&
          mir_color_interferes(&fn->vregs[a], &fn->vregs[b])) {
        MIR_INTER_SET(a, b);
        MIR_INTER_SET(b, a);
        degree[a]++;
        degree[b]++;
      }
    }
  }

  /* Simplify / optimistic-spill: repeatedly remove a node whose current degree
   * is below the number of registers it could take (trivially colorable),
   * pushing it on the stack; when none qualifies, optimistically remove the
   * node with the lowest spill cost per unit of relief (cost/degree). The order
   * only affects quality -- SELECT guarantees legality either way. */
  size_t sp = 0;
  size_t remaining = 0;
  for (size_t v = 0; v < N; v++) {
    if (colorable[v]) {
      remaining++;
    }
  }
  while (remaining > 0) {
    MirVregId pick = MIR_VREG_NONE;
    long long best_simplify = -1;
    int best_simplify_lc = 1;
    for (size_t v = 0; v < N; v++) {
      if (colorable[v] && !removed[v]) {
        int k = __builtin_popcount(mask[v]);
        if (degree[v] < k) {
          int lc = fn->vregs[v].loop_carried ? 1 : 0;
          long long metric = (long long)cost[v] * 1000 / (degree[v] + 1);
          int better;
          if (pick == MIR_VREG_NONE) {
            better = 1;
          } else if (lc != best_simplify_lc) {
            better = (lc < best_simplify_lc);
          } else {
            better = (metric < best_simplify);
          }
          if (better) {
            best_simplify = metric;
            best_simplify_lc = lc;
            pick = (MirVregId)v;
          }
        }
      }
    }
    if (pick == MIR_VREG_NONE) {
      /* Spill candidate: prefer a NON-loop-carried node (a loop-carried base
       * pointer / accumulator / induction var is reused every iteration, so
       * spilling it reloads it each pass -- exactly backwards); within the same
       * loop-carried class, minimise cost/(degree+1) (favour cheap to spill,
       * high relief). */
      long long best = -1;
      int best_lc = 1;
      for (size_t v = 0; v < N; v++) {
        if (!colorable[v] || removed[v]) {
          continue;
        }
        int lc = fn->vregs[v].loop_carried ? 1 : 0;
        long long metric = (long long)cost[v] * 1000 / (degree[v] + 1);
        int better;
        if (pick == MIR_VREG_NONE) {
          better = 1;
        } else if (lc != best_lc) {
          better = (lc < best_lc); /* non-loop-carried first */
        } else {
          better = (metric < best);
        }
        if (better) {
          best = metric;
          best_lc = lc;
          pick = (MirVregId)v;
        }
      }
    }
    removed[pick] = 1;
    stack[sp++] = pick;
    remaining--;
    for (size_t b = 0; b < N; b++) {
      if (colorable[b] && !removed[b] && MIR_INTER_GET(pick, b)) {
        degree[b]--;
      }
    }
  }

  /* SELECT: pop in reverse and assign a legal colour, or spill to memory. A
   * copy partner's colour (the coalesce hint, i.e. a dying two-address source)
   * is preferred when free, so the encoder's store_from elides the move. */
  while (sp > 0) {
    MirVregId v = stack[--sp];
    MirVreg *vr = &fn->vregs[v];
    uint32_t used = 0;
    for (size_t b = 0; b < N; b++) {
      if (colorable[b] && MIR_INTER_GET(v, b) && fn->vregs[b].in_register) {
        used |= 1u << fn->vregs[b].phys;
      }
    }
    uint32_t avail = mask[v] & ~used;
    if (avail == 0) {
      *next_spill += 8;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = *next_spill;
      continue;
    }
    int chosen = -1;
    /* Bias toward a copy partner's register to elide the move. */
    if (vr->coalesce_hint != MIR_VREG_NONE) {
      MirVreg *hv = &fn->vregs[vr->coalesce_hint];
      if (hv->in_register && (avail & (1u << hv->phys))) {
        chosen = hv->phys;
      }
    }
    if (chosen < 0) {
      /* Otherwise the lowest-numbered available register: keeps volatile/low
       * regs busy first and is deterministic. */
      for (int r = 0; r < 16; r++) {
        if (avail & (1u << r)) {
          chosen = r;
          break;
        }
      }
    }
    vr->assigned = 1;
    vr->in_register = 1;
    vr->phys = chosen;
  }

  /* Post-colouring coalescing: eliminate a register-to-register copy `dst <- src`
   * by giving dst the SAME register as src, when src dies at the copy, the two do
   * not interfere, and src's register is unused among dst's interfering
   * neighbours. The encoder then emits nothing for a `mov R,R`. This catches move
   * chains (e.g. an unrolled `a=b; b=next` rotation, or a value's last copy into a
   * loop-carried home) that the SELECT-time bias misses because the partner had
   * not been coloured yet. It only rewrites a register ASSIGNMENT -- both vregs
   * hold the same value at the copy point and the freed-register check preserves
   * graph legality -- so it can never change behaviour, only remove a move.
   * Iterated to a fixpoint so a recoloured dst can in turn feed the next copy. */
  int coalesced = 1;
  while (coalesced) {
    coalesced = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (in->op != MIR_MOV || in->dst.kind != MIR_OPK_VREG ||
          in->a.kind != MIR_OPK_VREG) {
        continue;
      }
      MirVregId d = in->dst.vreg;
      MirVregId s = in->a.vreg;
      if (d < 0 || s < 0 || (size_t)d >= N || (size_t)s >= N || d == s ||
          !colorable[d] || !colorable[s]) {
        continue;
      }
      MirVreg *dv = &fn->vregs[d];
      MirVreg *sv = &fn->vregs[s];
      if (!dv->in_register || !sv->in_register || dv->rclass != sv->rclass ||
          dv->phys == sv->phys || sv->live_end != (int)i ||
          MIR_INTER_GET(d, s) || !(mask[d] & (1u << sv->phys))) {
        continue;
      }
      uint32_t used = 0;
      for (size_t b = 0; b < N; b++) {
        if (colorable[b] && MIR_INTER_GET(d, b) && fn->vregs[b].in_register) {
          used |= 1u << fn->vregs[b].phys;
        }
      }
      if (used & (1u << sv->phys)) {
        continue;
      }
      dv->phys = sv->phys;
      coalesced = 1;
    }
  }

#undef MIR_INTER_SET
#undef MIR_INTER_GET
  free(inter); free(mask); free(degree); free(cost); free(colorable);
  free(removed); free(stack);
  return 1;
}

/* Shared tail: tell the context which callee-saved GP/XMM registers the
 * allocation used so the prologue/epilogue preserve them. */
static int mir_regalloc_report_saved(MirFunction *fn) {
  if (!fn->context) {
    return 1;
  }
  int used_nonvol[16];
  memset(used_nonvol, 0, sizeof(used_nonvol));
  for (size_t i = 0; i < fn->vreg_count; i++) {
    MirVreg *vr = &fn->vregs[i];
    /* RBP is callee-saved on both ABIs but is excluded from the global Win64
     * nonvolatile classifier (it is normally the frame pointer). When the frame
     * pointer is omitted the allocator may place a value in it, and that value
     * must be preserved like any other callee-saved register -- otherwise the
     * caller's frame pointer is destroyed. */
    if (vr->in_register && vr->rclass == MIR_RC_GP &&
        (mir_gp_is_nonvolatile((BinaryGpRegister)vr->phys) ||
         vr->phys == BINARY_GP_RBP)) {
      used_nonvol[vr->phys] = 1;
    }
  }
  for (int reg = 0; reg < 16; reg++) {
    if (used_nonvol[reg] && !code_generator_binary_context_add_saved_register(
                                fn->context, (BinaryGpRegister)reg)) {
      return 0;
    }
  }
  int used_xmm[16];
  memset(used_xmm, 0, sizeof(used_xmm));
  for (size_t i = 0; i < fn->vreg_count; i++) {
    MirVreg *vr = &fn->vregs[i];
    if (vr->in_register && vr->rclass == MIR_RC_XMM && vr->phys >= 8) {
      used_xmm[vr->phys] = 1;
    }
  }
  for (int reg = 8; reg < 16; reg++) {
    if (used_xmm[reg] && !code_generator_binary_context_add_saved_xmm_register(
                             fn->context, (BinaryXmmRegister)reg)) {
      return 0;
    }
  }
  return 1;
}

/* Graph-coloring entry: shares the linear scan's setup (liveness, crosses_call,
 * address-taken homing, ABI pool) then colours. */
static int mir_regalloc_color(MirFunction *fn) {
  mir_compute_liveness(fn);
  mir_compute_coalesce_hints(fn);
  for (size_t v = 0; v < fn->vreg_count; v++) {
    fn->vregs[v].crosses_call = 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op != MIR_CALL &&
        fn->insns[i].op != MIR_CALL_INDIRECT &&
        fn->insns[i].op != MIR_SIMD_SLP_MAC &&
        fn->insns[i].op != MIR_SIMD_FILL &&
        fn->insns[i].op != MIR_SIMD_AFFINE_MAP_F32 &&
        fn->insns[i].op != MIR_SIMD_AFFINE_MAP_F64 &&
        fn->insns[i].op != MIR_SIMD_SILU_F32 &&
        fn->insns[i].op != MIR_SIMD_VLOOP) {
      continue;
    }
    int c = (int)i;
    for (size_t v = 0; v < fn->vreg_count; v++) {
      MirVreg *vr = &fn->vregs[v];
      if (vr->live_start != MIR_LIVE_NONE && vr->live_start < c &&
          vr->live_end > c) {
        vr->crosses_call = 1;
      }
    }
  }

  int next_spill = fn->context ? fn->context->raw_frame_size : 0;
  for (size_t v = 0; v < fn->vreg_count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->address_taken) {
      int home = vr->home_bytes > 0 ? vr->home_bytes : 8;
      next_spill += home;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = next_spill;
    }
  }

  BinaryGpRegister gp_leaf_pool[MIR_GP_LEAF_POOL_MAX];
  /* An indirect (>8-byte) struct return passes a hidden out-pointer in the
   * first arg slot, shifting the real parameters up by one; count it so a
   * register still holding an incoming parameter is never reclaimed. */
  size_t gp_leaf_n = mir_build_gp_leaf_pool(
      gp_leaf_pool, fn->param_count + (fn->returns_indirect ? 1 : 0),
      !mir_fn_has_real_calls(fn));
  BinaryGpRegister gp_cross_pool[MIR_GP_CROSSCALL_POOL_MAX];
  size_t gp_cross_n = mir_build_gp_crosscall_pool(gp_cross_pool);

  int allow_rbp = fn->context && fn->context->omit_frame_pointer &&
                  !mir_fn_uses_slp(fn);
  if (!mir_color_graph(fn, gp_leaf_pool, gp_leaf_n, gp_cross_pool, gp_cross_n,
                       &next_spill, allow_rbp)) {
    fn->has_error = 1;
    return 0;
  }
  fn->spill_bytes = next_spill - (fn->context ? fn->context->raw_frame_size : 0);
  if (!mir_regalloc_report_saved(fn)) {
    fn->has_error = 1;
    return 0;
  }
  return 1;
}

/* True if `op` solely produces its vreg dst with no other observable effect, so
 * the instruction is removable when that dst is never read. Excludes anything
 * that can fault or has a side effect a later instruction may depend on: loads
 * (handled at the call site via a MEM source check), stores, divides (trap on
 * zero), calls, branches, returns, traps, flag/compare producers consumed
 * elsewhere, and SIMD/SLP/vector-memory ops. Flags set by these ALU ops are
 * never relied upon (MIR reads flags only via explicit CMP/TEST/UCOMIS), so
 * dropping a dead one cannot change a branch outcome. */
static int mir_op_pure_def(MirOpcode op) {
  switch (op) {
  case MIR_MOV:
  case MIR_MOVZX:
  case MIR_MOVSX:
  case MIR_LEA:
  case MIR_LEA_LOCAL:
  case MIR_LEA_GLOBAL:
  case MIR_LEA_FUNC:
  case MIR_LEA_CSTR:
  case MIR_ADD:
  case MIR_SUB:
  case MIR_AND:
  case MIR_OR:
  case MIR_XOR:
  case MIR_IMUL:
  case MIR_NEG:
  case MIR_NOT:
  case MIR_SHL:
  case MIR_SHR:
  case MIR_SAR:
  case MIR_SETCC:
  case MIR_CMOVCC:
  case MIR_FADD:
  case MIR_FSUB:
  case MIR_FMUL:
  case MIR_FDIV:
  case MIR_CVTSI2F:
  case MIR_CVTF2SI:
  case MIR_CVTF2F:
  case MIR_MOVD_TO_XMM:
  case MIR_MOVD_TO_GP:
    return 1;
  default:
    return 0;
  }
}

static void mir_dce_add_read(MirVregId v, int *reads, size_t n) {
  if (v >= 0 && (size_t)v < n) {
    reads[v]++;
  }
}

/* Count the vregs READ by one operand: a plain vreg, or the base/index of a
 * memory address. (A vreg dst is a definition, never a read — MIR is
 * three-address: dst = a OP b.) */
static void mir_dce_count_operand(const MirOperand *op, int *reads, size_t n) {
  if (op->kind == MIR_OPK_VREG) {
    mir_dce_add_read(op->vreg, reads, n);
  } else if (op->kind == MIR_OPK_MEM) {
    mir_dce_add_read(op->mem.base, reads, n);
    mir_dce_add_read(op->mem.index, reads, n);
  }
}

/* Dead-code elimination: drop pure value-producing ops whose vreg dst is never
 * read. Iterates to a fixpoint, since removing one dead def can orphan the
 * sources that fed it. This cleans up dead register shuffles left behind by
 * IR-level constant folding -- e.g. a source-unrolled `next=a+b; a=b; b=next`
 * whose result folds to a constant leaves the `a=b; b=next` rotation copies
 * running every loop iteration. */
static void mir_dce(MirFunction *fn) {
  if (fn->vreg_count == 0 || fn->insn_count == 0) {
    return;
  }
  int *reads = (int *)malloc(fn->vreg_count * sizeof(int));
  if (!reads) {
    return;
  }
  int changed = 1;
  while (changed) {
    changed = 0;
    memset(reads, 0, fn->vreg_count * sizeof(int));
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (in->op == MIR_NOP) {
        continue;
      }
      mir_dce_count_operand(&in->a, reads, fn->vreg_count);
      mir_dce_count_operand(&in->b, reads, fn->vreg_count);
      if (in->dst.kind == MIR_OPK_MEM) {
        mir_dce_add_read(in->dst.mem.base, reads, fn->vreg_count);
        mir_dce_add_read(in->dst.mem.index, reads, fn->vreg_count);
      }
    }
    for (size_t i = 0; i < fn->insn_count; i++) {
      MirInst *in = &fn->insns[i];
      if (in->op == MIR_NOP || !mir_op_pure_def(in->op)) {
        continue;
      }
      if (in->dst.kind != MIR_OPK_VREG) {
        continue; /* a store (dst MEM) is not a pure def */
      }
      if (in->op == MIR_MOV && in->a.kind == MIR_OPK_MEM) {
        continue; /* a load may fault; leave it */
      }
      MirVregId d = in->dst.vreg;
      if (d < 0 || (size_t)d >= fn->vreg_count ||
          d == fn->indirect_return_vreg) {
        continue;
      }
      if (reads[d] == 0) {
        in->op = MIR_NOP;
        changed = 1;
      }
    }
  }
  free(reads);
}

int mir_regalloc(MirFunction *fn) {
  if (!fn) {
    return 0;
  }
  if (fn->vreg_count == 0) {
    return 1;
  }

  /* Strip dead pure defs before allocation so they neither consume registers nor
   * emit instructions. */
  mir_dce(fn);

  /* Finalize the frame-pointer-omission decision now that the MIR body exists.
   * The mir_lower stage cleared it for feature gates (stack traces / debug);
   * here we additionally require the function to be a LEAF. In a leaf, rsp is
   * never perturbed by call/push in the body, so rsp-relative slot addressing
   * pays no stack-engine sync uop -- the freed rbp and the shorter prologue are
   * a clean win. In a call-heavy function, rsp-relative spill accesses near each
   * call would each force a sync, which cancels the prologue saving (measured
   * slightly negative on rec_fib), so those keep the rbp frame. */
  if (fn->context && fn->context->omit_frame_pointer && mir_fn_has_calls(fn)) {
    fn->context->omit_frame_pointer = 0;
  }

  /* Graph coloring is the default; METTLE_LINEAR_ALLOC forces the legacy
   * linear scan (an escape hatch for differential debugging). */
  if (!getenv("METTLE_LINEAR_ALLOC")) {
    return mir_regalloc_color(fn);
  }

  mir_compute_liveness(fn);
  mir_compute_coalesce_hints(fn);

  /* A value is "cross-call" if its live interval strictly spans a MIR_CALL
   * (defined before the call, used after it). Such values must survive the
   * callee's clobber of caller-saved registers. (A value defined by the call's
   * return, or whose last use is feeding an argument, does not span it.) */
  for (size_t v = 0; v < fn->vreg_count; v++) {
    fn->vregs[v].crosses_call = 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    /* MIR_SIMD_SLP_MAC and MIR_SIMD_FILL clobber the caller-saved set
     * (RAX/RCX/RDX/R8/R9/R10/R11 + xmm0..) exactly like a call, so a value
     * spanning one must also live in a callee-saved register or spill. */
    if (fn->insns[i].op != MIR_CALL &&
        fn->insns[i].op != MIR_CALL_INDIRECT &&
        fn->insns[i].op != MIR_SIMD_SLP_MAC &&
        fn->insns[i].op != MIR_SIMD_FILL &&
        fn->insns[i].op != MIR_SIMD_AFFINE_MAP_F32 &&
        fn->insns[i].op != MIR_SIMD_AFFINE_MAP_F64 &&
        fn->insns[i].op != MIR_SIMD_SILU_F32 &&
        fn->insns[i].op != MIR_SIMD_VLOOP) {
      continue;
    }
    int c = (int)i;
    for (size_t v = 0; v < fn->vreg_count; v++) {
      MirVreg *vr = &fn->vregs[v];
      if (vr->live_start != MIR_LIVE_NONE && vr->live_start < c &&
          vr->live_end > c) {
        vr->crosses_call = 1;
      }
    }
  }

  size_t order_count = 0;
  MirVregId *order = mir_order_by_start(fn, &order_count);
  if (fn->has_error) {
    free(order);
    return 0;
  }

  /* Per-class free pools, tracked as "register r is free / held by vreg". */
  int gp_held_by[16];  /* index by BinaryGpRegister -> vreg id or -1 */
  int xmm_held_by[16]; /* index by BinaryXmmRegister -> vreg id or -1 */
  for (int i = 0; i < 16; i++) {
    gp_held_by[i] = -1;
    xmm_held_by[i] = -1;
  }
  /* XMM4/XMM5 are encoder scratch (see MIR_XMM_POOL) — never allocate them. */
  xmm_held_by[BINARY_XMM4] = -2;
  xmm_held_by[BINARY_XMM5] = -2;
  /* Leaf pool for this ABI/shape (base + any arg-capable reg this function does
   * not need for its own params or outgoing calls). */
  BinaryGpRegister gp_leaf_pool[MIR_GP_LEAF_POOL_MAX];
  /* +1 for the hidden out-pointer of an indirect struct return (it occupies the
   * first incoming arg slot, shifting the real parameters up). */
  size_t gp_leaf_pool_count = mir_build_gp_leaf_pool(
      gp_leaf_pool, fn->param_count + (fn->returns_indirect ? 1 : 0),
      !mir_fn_has_real_calls(fn));
  BinaryGpRegister gp_cross_pool[MIR_GP_CROSSCALL_POOL_MAX];
  size_t gp_cross_pool_count = mir_build_gp_crosscall_pool(gp_cross_pool);
  /* Start every GP register reserved, then open exactly the leaf-pool members.
   * RAX/RCX/RDX (encoder scratch) and RSP/RBP (stack/frame) are never in the
   * pool, so they stay reserved. */
  for (int r = 0; r < 16; r++) {
    gp_held_by[r] = -2;
  }
  for (size_t i = 0; i < gp_leaf_pool_count; i++) {
    gp_held_by[gp_leaf_pool[i]] = -1;
  }

  /* Spill slots grow downward below the existing frame. The encoder adds
   * fn->spill_bytes to the prologue allocation; slot k lives at
   * [rbp - (base_frame + (k+1)*8)]. We record only the running total here and
   * store each vreg's own positive offset. */
  int next_spill_offset = fn->context ? fn->context->raw_frame_size : 0;

  /* Address-taken values must be memory-resident; give each a stack slot up
   * front (independent of liveness — one may be written only through its alias
   * pointer and never appear in the interval order). The main scan then skips
   * them so they never occupy a register. */
  for (size_t v = 0; v < fn->vreg_count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->address_taken) {
      /* A struct local owns a multi-slot home (home_bytes); the slot offset is
       * the FAR (highest) end since homes grow downward from rbp, so the home
       * spans [rbp - offset .. rbp - offset + home_bytes). */
      int home = vr->home_bytes > 0 ? vr->home_bytes : 8;
      next_spill_offset += home;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = next_spill_offset;
    }
  }

  /* Active intervals, kept as a simple array we scan/expire each step. */
  MirVregId *active = (MirVregId *)malloc(order_count * sizeof(MirVregId));
  if (!active && order_count > 0) {
    free(order);
    fn->has_error = 1;
    return 0;
  }
  size_t active_count = 0;

  for (size_t oi = 0; oi < order_count; oi++) {
    MirVregId cur = order[oi];
    MirVreg *cv = &fn->vregs[cur];
    int point = cv->live_start;

    /* Expire intervals that ended before this start. */
    size_t w = 0;
    for (size_t r = 0; r < active_count; r++) {
      MirVregId a = active[r];
      MirVreg *av = &fn->vregs[a];
      if (av->live_end < point) {
        if (av->in_register) {
          if (av->rclass == MIR_RC_XMM) {
            xmm_held_by[av->phys] = -1;
          } else {
            gp_held_by[av->phys] = -1;
          }
        }
      } else {
        active[w++] = a;
      }
    }
    active_count = w;

    /* Address-taken values are memory-resident (their stack slot was assigned
     * up front, below): never give them a register, so every use loads and
     * every def stores through the home, keeping a by-name access and an
     * aliasing-pointer access on the same memory. */
    if (cv->address_taken) {
      continue;
    }

    /* Two-address coalescing: reuse the register of a source that dies exactly
     * here, so the encoder writes the result in place. Only for non-cross-call
     * GP values (a cross-call dst needs a callee-saved reg, which the dying
     * source may not be in). The source is still `active` (its live_end == this
     * point, so the expire above kept it); steal its register and drop it from
     * the active set so the next expire does not free what is now ours. */
    int got_reg = 0;
    if (cv->rclass == MIR_RC_GP && !cv->crosses_call &&
        cv->coalesce_hint != MIR_VREG_NONE) {
      MirVreg *hv = &fn->vregs[cv->coalesce_hint];
      if (hv->in_register && hv->rclass == MIR_RC_GP &&
          hv->live_end == point && gp_held_by[hv->phys] == cv->coalesce_hint &&
          !mir_reg_clobbered_in_range(fn, (BinaryGpRegister)hv->phys,
                                      cv->live_start, cv->live_end)) {
        cv->phys = hv->phys;
        cv->assigned = 1;
        cv->in_register = 1;
        gp_held_by[hv->phys] = cur;
        for (size_t r = 0; r < active_count; r++) {
          if (active[r] == cv->coalesce_hint) {
            active[r] = active[--active_count];
            break;
          }
        }
        got_reg = 1;
      }
    }
    /* Try to grab a free physical register. Cross-call values may only use the
     * callee-saved pool (GP), or must spill (XMM has no callee-saved lane in our
     * allocatable set). */
    if (!got_reg && cv->rclass == MIR_RC_XMM) {
      if (!cv->crosses_call) {
        /* Skip the volatile xmm0-3 when they serve as outgoing float-argument
         * registers (see has_xmm_arg_call / mir_color_reg_mask). */
        for (size_t p = 0; !fn->has_xmm_arg_call && p < MIR_XMM_POOL_COUNT; p++) {
          BinaryXmmRegister reg = MIR_XMM_POOL[p];
          if (xmm_held_by[reg] == -1) {
            xmm_held_by[reg] = cur;
            cv->assigned = 1;
            cv->in_register = 1;
            cv->phys = reg;
            got_reg = 1;
            break;
          }
        }
        /* Spill to the callee-saved xmm8..15 tier before the stack. */
        for (size_t p = 0; !got_reg && p < MIR_XMM_NONVOL_POOL_COUNT; p++) {
          BinaryXmmRegister reg = MIR_XMM_NONVOL_POOL[p];
          if (xmm_held_by[reg] == -1) {
            xmm_held_by[reg] = cur;
            cv->assigned = 1;
            cv->in_register = 1;
            cv->phys = reg;
            got_reg = 1;
            break;
          }
        }
      }
    } else if (!got_reg) {
      const BinaryGpRegister *pool =
          cv->crosses_call ? gp_cross_pool : gp_leaf_pool;
      size_t pool_n =
          cv->crosses_call ? gp_cross_pool_count : gp_leaf_pool_count;
      for (size_t p = 0; p < pool_n; p++) {
        BinaryGpRegister reg = pool[p];
        if (gp_held_by[reg] == -1 &&
            !mir_reg_clobbered_in_range(fn, reg, cv->live_start,
                                        cv->live_end)) {
          gp_held_by[reg] = cur;
          cv->assigned = 1;
          cv->in_register = 1;
          cv->phys = reg;
          got_reg = 1;
          break;
        }
      }
    }

    if (got_reg) {
      active[active_count++] = cur;
      continue;
    }

    /* Cross-call values that found no callee-saved register simply spill — they
     * must not steal a volatile register (it would be clobbered by the call). */
    if (cv->crosses_call) {
      next_spill_offset += 8;
      cv->assigned = 1;
      cv->in_register = 0;
      cv->spill_offset = next_spill_offset;
      continue;
    }

    /* No free register: choose a spill victim. The classic linear-scan choice
     * is the farthest live_end, but in a loop that is exactly a loop-carried
     * value (base pointer / accumulator / induction var) reused every
     * iteration -- spilling it reloads it each pass. So prefer a NON-loop-
     * carried victim (a body temp, often a cold sub-path's value that costs one
     * reload); only fall back to farthest-live_end within the same loop-carried
     * category. Same class, not clobbered inside cur's interval. */
    MirVregId spill_victim = MIR_VREG_NONE;
    int victim_end = -1;
    int victim_lc = 1;
    for (size_t r = 0; r < active_count; r++) {
      MirVregId a = active[r];
      MirVreg *av = &fn->vregs[a];
      if (av->rclass != cv->rclass || !av->in_register) {
        continue;
      }
      /* Don't steal a register that would be clobbered inside cur's interval. */
      if (av->rclass == MIR_RC_GP &&
          mir_reg_clobbered_in_range(fn, (BinaryGpRegister)av->phys,
                                     cv->live_start, cv->live_end)) {
        continue;
      }
      int better;
      if (spill_victim == MIR_VREG_NONE) {
        better = 1;
      } else if (av->loop_carried != victim_lc) {
        better = (av->loop_carried < victim_lc); /* prefer non-loop-carried */
      } else {
        better = (av->live_end > victim_end);
      }
      if (better) {
        victim_end = av->live_end;
        victim_lc = av->loop_carried;
        spill_victim = a;
      }
    }

    /* Spill the victim instead of `cur` when the victim is the worse one to
     * keep: a loop-carried `cur` should evict a non-loop-carried victim
     * regardless of live_end; otherwise the standard farthest-live_end rule. */
    int prefer_victim = 0;
    if (spill_victim != MIR_VREG_NONE) {
      if (victim_lc != cv->loop_carried) {
        prefer_victim = (cv->loop_carried && !victim_lc);
      } else {
        prefer_victim = fn->vregs[spill_victim].live_end > cv->live_end;
      }
    }
    if (prefer_victim) {
      /* Steal the victim's register; spill the victim. */
      MirVreg *vv = &fn->vregs[spill_victim];
      int reg = vv->phys;
      next_spill_offset += 8;
      vv->in_register = 0;
      vv->assigned = 1;
      vv->spill_offset = next_spill_offset;
      cv->assigned = 1;
      cv->in_register = 1;
      cv->phys = reg;
      if (cv->rclass == MIR_RC_XMM) {
        xmm_held_by[reg] = cur;
      } else {
        gp_held_by[reg] = cur;
      }
      /* Replace victim with cur in the active set. */
      for (size_t r = 0; r < active_count; r++) {
        if (active[r] == spill_victim) {
          active[r] = cur;
          break;
        }
      }
    } else {
      /* Spill current. */
      next_spill_offset += 8;
      cv->assigned = 1;
      cv->in_register = 0;
      cv->spill_offset = next_spill_offset;
    }
  }

  fn->spill_bytes =
      next_spill_offset - (fn->context ? fn->context->raw_frame_size : 0);

  /* Tell the function context which nonvolatile registers the allocation used,
   * so the encoder's prologue/epilogue saves and restores them. */
  if (fn->context) {
    int used_nonvol[16];
    memset(used_nonvol, 0, sizeof(used_nonvol));
    for (size_t i = 0; i < fn->vreg_count; i++) {
      MirVreg *vr = &fn->vregs[i];
      if (vr->in_register && vr->rclass == MIR_RC_GP &&
          mir_gp_is_nonvolatile((BinaryGpRegister)vr->phys)) {
        used_nonvol[vr->phys] = 1;
      }
    }
    for (int reg = 0; reg < 16; reg++) {
      if (used_nonvol[reg] &&
          !code_generator_binary_context_add_saved_register(
              fn->context, (BinaryGpRegister)reg)) {
        free(order);
        free(active);
        fn->has_error = 1;
        return 0;
      }
    }

    /* Callee-saved XMM (xmm8..15) the allocation used: the prologue/epilogue
     * preserve them (a no-op cost on SysV where they are caller-saved). */
    int used_xmm[16];
    memset(used_xmm, 0, sizeof(used_xmm));
    for (size_t i = 0; i < fn->vreg_count; i++) {
      MirVreg *vr = &fn->vregs[i];
      if (vr->in_register && vr->rclass == MIR_RC_XMM && vr->phys >= 8) {
        used_xmm[vr->phys] = 1;
      }
    }
    for (int reg = 8; reg < 16; reg++) {
      if (used_xmm[reg] &&
          !code_generator_binary_context_add_saved_xmm_register(
              fn->context, (BinaryXmmRegister)reg)) {
        free(order);
        free(active);
        fn->has_error = 1;
        return 0;
      }
    }
  }

  free(order);
  free(active);
  return 1;
}
