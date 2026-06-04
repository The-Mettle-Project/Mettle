#include "codegen/binary/mir.h"

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
    BINARY_GP_R10, BINARY_GP_R11, BINARY_GP_RBX, BINARY_GP_R12,
    BINARY_GP_R13, BINARY_GP_R14, BINARY_GP_R15};
#define MIR_GP_POOL_COUNT (sizeof(MIR_GP_POOL) / sizeof(MIR_GP_POOL[0]))
/* Reclaimable arg-capable registers, tried after the base pool. */
static const BinaryGpRegister MIR_GP_EXTRA[] = {
    BINARY_GP_RSI, BINARY_GP_RDI, BINARY_GP_R8, BINARY_GP_R9};
#define MIR_GP_EXTRA_COUNT (sizeof(MIR_GP_EXTRA) / sizeof(MIR_GP_EXTRA[0]))
/* Upper bound on the leaf pool: the static base plus every extra. */
#define MIR_GP_LEAF_POOL_MAX (MIR_GP_POOL_COUNT + MIR_GP_EXTRA_COUNT)

/* True if the function makes any call (so caller-saved regs are unsafe to hold
 * values across, and outgoing-arg registers must not be reclaimed). */
static int mir_fn_has_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op == MIR_CALL) {
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

/* Whether `reg` may join the leaf allocatable pool given this function's shape.
 * An arg register is poolable only when it carries NO incoming parameter (its
 * arg index >= param_count, so homing never reads it) AND, if it is an arg
 * register at all, the function is a leaf (otherwise outgoing-call marshalling
 * would clobber it). Non-arg callee-saved registers (RSI/RDI on Win64) are
 * always poolable. This keeps parameter homing a plain sequential copy — no
 * arg register is ever both a homing source and an allocation target. */
static int mir_reg_poolable(BinaryGpRegister reg, size_t param_count,
                            int is_leaf) {
  int ai = mir_reg_arg_index(reg);
  if (ai < 0) {
    return 1; /* not an arg register: safe on both ABIs */
  }
  if ((size_t)ai < param_count) {
    return 0; /* holds a live incoming parameter -> homing source */
  }
  return is_leaf; /* dead arg reg: usable only without outgoing calls */
}

/* Build the leaf (non-cross-call) GP pool: the universally-safe base, then any
 * arg-capable register the function does not need for its own parameters or
 * outgoing calls. On Win64 this reclaims RSI/RDI (callee-saved, never args)
 * plus the trailing unused arg registers (e.g. R9 for a 3-arg leaf, R8+R9 for
 * <=2 args) — the difference between holding an unrolled matmul's accumulators
 * AND base pointers in registers versus reloading them every access. Reclaimed
 * nonvolatiles are saved by the used-nonvolatile machinery; reclaimed volatiles
 * (R8/R9) need no save in a leaf. */
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

/* Registers a value may occupy across a call. Restricted to those that are
 * callee-saved under BOTH Win64 and SysV (RBX, R12..R15), so cross-call
 * allocation is correct on Windows and Linux. RSI/RDI are nonvolatile on Win64
 * but caller-saved arg regs on SysV, so they are excluded here. */
static const BinaryGpRegister MIR_GP_CROSSCALL_POOL[] = {
    BINARY_GP_RBX, BINARY_GP_R12, BINARY_GP_R13, BINARY_GP_R14, BINARY_GP_R15};
#define MIR_GP_CROSSCALL_POOL_COUNT \
  (sizeof(MIR_GP_CROSSCALL_POOL) / sizeof(MIR_GP_CROSSCALL_POOL[0]))

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
      if (in->op == MIR_JMP || in->op == MIR_JCC) {
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
        /* crosses a boundary (defined before l, or used after b)? */
        int crosses = (vr->live_start < l) || (vr->live_end > b);
        if (!crosses) {
          continue;
        }
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
      commutative = 1;
      break;
    case MIR_SUB:
      commutative = 0;
      break;
    default:
      continue;
    }
    if (in->dst.kind != MIR_OPK_VREG ||
        fn->vregs[in->dst.vreg].rclass != MIR_RC_GP) {
      continue;
    }
    if (in->op == MIR_IMUL && in->b.kind == MIR_OPK_IMM) {
      continue; /* imul r, a, imm32 needs no copy */
    }
    MirVregId d = in->dst.vreg;
    MirVregId cand = MIR_VREG_NONE;
    /* Prefer b for commutative ops (matches the encoder's first elision check),
     * otherwise the (only legal for SUB) operand a. A candidate must be a GP
     * vreg, distinct from dst, whose last use is exactly this instruction. */
    if (commutative && in->b.kind == MIR_OPK_VREG && in->b.vreg != d &&
        fn->vregs[in->b.vreg].rclass == MIR_RC_GP &&
        fn->vregs[in->b.vreg].live_end == (int)i) {
      cand = in->b.vreg;
    } else if (in->a.kind == MIR_OPK_VREG && in->a.vreg != d &&
               fn->vregs[in->a.vreg].rclass == MIR_RC_GP &&
               fn->vregs[in->a.vreg].live_end == (int)i) {
      cand = in->a.vreg;
    }
    fn->vregs[d].coalesce_hint = cand;
  }
}

int mir_regalloc(MirFunction *fn) {
  if (!fn) {
    return 0;
  }
  if (fn->vreg_count == 0) {
    return 1;
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
    if (fn->insns[i].op != MIR_CALL) {
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
  size_t gp_leaf_pool_count = mir_build_gp_leaf_pool(
      gp_leaf_pool, fn->param_count, !mir_fn_has_calls(fn));
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
      next_spill_offset += 8;
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
          hv->live_end == point && gp_held_by[hv->phys] == cv->coalesce_hint) {
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
        for (size_t p = 0; p < MIR_XMM_POOL_COUNT; p++) {
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
          cv->crosses_call ? MIR_GP_CROSSCALL_POOL : gp_leaf_pool;
      size_t pool_n =
          cv->crosses_call ? MIR_GP_CROSSCALL_POOL_COUNT : gp_leaf_pool_count;
      for (size_t p = 0; p < pool_n; p++) {
        BinaryGpRegister reg = pool[p];
        if (gp_held_by[reg] == -1) {
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

    /* No free register: spill the active interval (or current) with the
     * farthest live_end, same class. */
    MirVregId spill_victim = MIR_VREG_NONE;
    int victim_end = -1;
    for (size_t r = 0; r < active_count; r++) {
      MirVregId a = active[r];
      MirVreg *av = &fn->vregs[a];
      if (av->rclass != cv->rclass || !av->in_register) {
        continue;
      }
      if (av->live_end > victim_end) {
        victim_end = av->live_end;
        spill_victim = a;
      }
    }

    if (spill_victim != MIR_VREG_NONE &&
        fn->vregs[spill_victim].live_end > cv->live_end) {
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
