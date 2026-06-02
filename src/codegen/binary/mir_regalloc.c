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

int mir_regalloc(MirFunction *fn) {
  if (!fn) {
    return 0;
  }
  if (fn->vreg_count == 0) {
    return 1;
  }

  mir_compute_liveness(fn);

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
  /* Mark non-allocatable GP regs as permanently busy so they are never handed
   * out: RAX/RCX/RDX (scratch), RSP/RBP (stack/frame). -2 = reserved. */
  gp_held_by[BINARY_GP_RAX] = -2;
  gp_held_by[BINARY_GP_RCX] = -2;
  gp_held_by[BINARY_GP_RDX] = -2;
  gp_held_by[BINARY_GP_RSP] = -2;
  gp_held_by[BINARY_GP_RBP] = -2;
  /* Argument registers on Win64/SysV (see MIR_GP_POOL): never allocate. */
  gp_held_by[BINARY_GP_R8] = -2;
  gp_held_by[BINARY_GP_R9] = -2;
  gp_held_by[BINARY_GP_RSI] = -2;
  gp_held_by[BINARY_GP_RDI] = -2;

  /* Spill slots grow downward below the existing frame. The encoder adds
   * fn->spill_bytes to the prologue allocation; slot k lives at
   * [rbp - (base_frame + (k+1)*8)]. We record only the running total here and
   * store each vreg's own positive offset. */
  int next_spill_offset = fn->context ? fn->context->raw_frame_size : 0;

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

    /* Try to grab a free physical register. Cross-call values may only use the
     * callee-saved pool (GP), or must spill (XMM has no callee-saved lane in our
     * allocatable set). */
    int got_reg = 0;
    if (cv->rclass == MIR_RC_XMM) {
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
      }
    } else {
      const BinaryGpRegister *pool =
          cv->crosses_call ? MIR_GP_CROSSCALL_POOL : MIR_GP_POOL;
      size_t pool_n =
          cv->crosses_call ? MIR_GP_CROSSCALL_POOL_COUNT : MIR_GP_POOL_COUNT;
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
  }

  free(order);
  free(active);
  return 1;
}
