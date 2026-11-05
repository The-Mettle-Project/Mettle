#include "codegen/binary/mir_color.h"
#include "codegen/target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIR_COALESCE_MAX_COST (1 << 24)
#define MIR_COALESCE_WORK_PER_INSN 256ull
#define MIR_COALESCE_MIN_WORK 16000000ull

extern const char *g_mir_ra_trace_name;

MirVregId mir_color_find(const MirColorState *st, MirVregId v) {
  MirVregId root = v;
  if (!st->rep || v < 0 || (size_t)v >= st->count) {
    return v;
  }
  while (st->rep[root] != root) {
    root = st->rep[root];
  }
  while (st->rep[v] != root) {
    MirVregId next = st->rep[v];
    st->rep[v] = root;
    v = next;
  }
  return root;
}

static int mir_coalesce_aggressive(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_RA_COALESCE_AGGRESSIVE") ? 1 : 0;
  }
  return cached;
}

static int mir_coalesce_trace(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_REGALLOC_TRACE") ? 1 : 0;
  }
  return cached;
}

static int mir_coalesce_is_copy(const MirInst *in) {
  return in->op == MIR_MOV && in->dst.kind == MIR_OPK_VREG &&
         in->a.kind == MIR_OPK_VREG;
}

static int mir_coalesce_significant(const MirColorState *st, size_t x,
                                    int lost) {
  return st->degree[x] - lost >= st->reg_count[x];
}

static int mir_coalesce_briggs_ok(const MirColorState *st, size_t u, size_t v) {
  uint32_t merged_mask = st->mask[u] & st->mask[v];
  int k = __builtin_popcount(merged_mask);
  int significant = 0;
  if (k == 0) {
    return 0;
  }
  MIR_INTER_FOR_EACH(st, u, x) {
    if (mir_coalesce_significant(st, x, mir_inter_get(st, x, v) ? 1 : 0)) {
      significant++;
    }
  }
  MIR_INTER_FOR_EACH(st, v, x) {
    if (!mir_inter_get(st, u, x) && mir_coalesce_significant(st, x, 0)) {
      significant++;
    }
  }
  return significant < k;
}

static int mir_coalesce_george_ok(const MirColorState *st, size_t u, size_t v) {
  uint32_t merged_mask = st->mask[u] & st->mask[v];
  if (merged_mask == 0) {
    return 0;
  }
  MIR_INTER_FOR_EACH(st, u, x) {
    if (mir_inter_get(st, x, v)) {
      continue;
    }
    if (st->degree[x] >= st->reg_count[x] || st->degree[x] >= (int)__builtin_popcount(merged_mask)) {
      return 0;
    }
  }
  return 1;
}

static int mir_coalesce_safe(const MirColorState *st, size_t u, size_t v) {
  return mir_coalesce_briggs_ok(st, u, v) || mir_coalesce_george_ok(st, u, v) ||
         mir_coalesce_george_ok(st, v, u);
}

static void mir_coalesce_merge_edges(MirColorState *st, size_t u, size_t v) {
  MIR_INTER_FOR_EACH(st, v, x) {
    mir_inter_clear(st, x, v);
    st->degree[x]--;
    if (!mir_inter_get(st, u, x)) {
      mir_inter_set(st, u, x);
      mir_inter_set(st, x, u);
      st->degree[u]++;
      st->degree[x]++;
    }
  }
  mir_inter_clear_row(st, v);
  st->degree[v] = 0;
}

static void mir_coalesce_merge_vreg(MirFunction *fn, size_t u, size_t v) {
  MirVreg *a = &fn->vregs[u];
  const MirVreg *b = &fn->vregs[v];
  if (b->live_start != MIR_LIVE_NONE &&
      (a->live_start == MIR_LIVE_NONE || b->live_start < a->live_start)) {
    a->live_start = b->live_start;
  }
  if (b->live_end > a->live_end) {
    a->live_end = b->live_end;
  }
  a->entry_live |= b->entry_live;
  a->crosses_call |= b->crosses_call;
  a->loop_carried |= b->loop_carried;
  a->crosses_preserving_only &= b->crosses_preserving_only;
  a->crosses_xmm_preserving_only &= b->crosses_xmm_preserving_only;
  if (a->coalesce_hint == MIR_VREG_NONE) {
    a->coalesce_hint = b->coalesce_hint;
  }
}

static void mir_coalesce_merge(MirColorState *st, size_t u, size_t v) {
  long long cost = (long long)st->cost[u] + st->cost[v];
  mir_coalesce_merge_edges(st, u, v);
  st->mask[u] &= st->mask[v];
  st->reg_count[u] = __builtin_popcount(st->mask[u]);
  st->cost[u] = cost > MIR_COALESCE_MAX_COST ? MIR_COALESCE_MAX_COST : (int)cost;
  if (st->use_depth[v] > st->use_depth[u]) {
    st->use_depth[u] = st->use_depth[v];
  }
  if (st->narrow_src && st->narrow_src[u] == MIR_VREG_NONE) {
    st->narrow_src[u] = st->narrow_src[v];
  }
  st->colorable[v] = 0;
  st->removed[v] = 1;
  st->rep[v] = (MirVregId)u;
  mir_coalesce_merge_vreg(st->fn, u, v);
}

static int mir_coalesce_try(MirColorState *st, const MirInst *in) {
  MirVregId d = mir_color_find(st, in->dst.vreg);
  MirVregId s = mir_color_find(st, in->a.vreg);
  size_t u;
  size_t v;
  if (d == s || d < 0 || s < 0 || (size_t)d >= st->count ||
      (size_t)s >= st->count || !st->colorable[d] || !st->colorable[s]) {
    return 0;
  }
  if (st->fn->vregs[d].rclass != st->fn->vregs[s].rclass ||
      mir_inter_get(st, (size_t)d, (size_t)s)) {
    return 0;
  }
  if (!st->facts || !st->facts->valid || st->facts->undef_live[d] ||
      st->facts->undef_live[s]) {
    return 0;
  }
  if (st->fn->vregs[s].entry_live || (!st->fn->vregs[d].entry_live && s < d)) {
    u = (size_t)s;
    v = (size_t)d;
  } else {
    u = (size_t)d;
    v = (size_t)s;
  }
  st->coalesce_work += (unsigned long long)st->degree[u] +
                       (unsigned long long)st->degree[v] + 1ull;
  if (st->coalesce_work > st->coalesce_budget) {
    return 0;
  }
  if (!mir_coalesce_aggressive() && !mir_coalesce_safe(st, u, v)) {
    return 0;
  }
  mir_coalesce_merge(st, u, v);
  return 1;
}

static void mir_coalesce_rewrite_operand(const MirColorState *st,
                                         MirOperand *op) {
  if (op->kind == MIR_OPK_VREG) {
    op->vreg = mir_color_find(st, op->vreg);
  } else if (op->kind == MIR_OPK_MEM) {
    op->mem.base = mir_color_find(st, op->mem.base);
    op->mem.index = mir_color_find(st, op->mem.index);
    if (op->mem.frame_home_valid) {
      op->mem.frame_home = mir_color_find(st, op->mem.frame_home);
    }
  }
}

static void mir_coalesce_rewrite(MirColorState *st) {
  MirFunction *fn = st->fn;
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP) {
      continue;
    }
    mir_coalesce_rewrite_operand(st, &in->dst);
    mir_coalesce_rewrite_operand(st, &in->a);
    mir_coalesce_rewrite_operand(st, &in->b);
    if (mir_coalesce_is_copy(in) && in->dst.vreg == in->a.vreg) {
      in->op = MIR_NOP;
      st->merged_copies++;
    }
  }
  for (size_t v = 0; v < st->count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->coalesce_hint != MIR_VREG_NONE) {
      vr->coalesce_hint = mir_color_find(st, vr->coalesce_hint);
      if (vr->coalesce_hint == (MirVregId)v) {
        vr->coalesce_hint = MIR_VREG_NONE;
      }
    }
    if (st->narrow_src && st->narrow_src[v] != MIR_VREG_NONE) {
      st->narrow_src[v] = mir_color_find(st, st->narrow_src[v]);
    }
  }
  fn->merged_copies = st->merged_copies;
}

int mir_color_coalesce(MirColorState *st) {
  MirFunction *fn = st->fn;
  size_t *copies;
  size_t copy_count = 0;
  int changed = 1;
  st->merged_copies = 0;
  for (size_t v = 0; v < st->count; v++) {
    st->rep[v] = (MirVregId)v;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_coalesce_is_copy(&fn->insns[i])) {
      copy_count++;
    }
  }
  if (copy_count == 0) {
    return 1;
  }
  copies = (size_t *)malloc(copy_count * sizeof(size_t));
  if (!copies) {
    return 0;
  }
  copy_count = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_coalesce_is_copy(&fn->insns[i])) {
      copies[copy_count++] = i;
    }
  }
  st->coalesce_work = 0;
  st->coalesce_budget = (unsigned long long)fn->insn_count *
                        MIR_COALESCE_WORK_PER_INSN;
  if (st->coalesce_budget < MIR_COALESCE_MIN_WORK) {
    st->coalesce_budget = MIR_COALESCE_MIN_WORK;
  }
  while (changed && st->coalesce_work <= st->coalesce_budget) {
    changed = 0;
    for (size_t k = 0;
         k < copy_count && st->coalesce_work <= st->coalesce_budget; k++) {
      if (mir_coalesce_try(st, &fn->insns[copies[k]])) {
        changed = 1;
      }
    }
  }
  free(copies);
  if (mir_coalesce_trace()) {
    fprintf(stderr, "RA-COALESCE\t%s\twork=%llu\tbudget=%llu\tinsns=%zu%s\n",
            g_mir_ra_trace_name ? g_mir_ra_trace_name : "?", st->coalesce_work,
            st->coalesce_budget, fn->insn_count,
            st->coalesce_work > st->coalesce_budget ? "\texhausted" : "");
  }
  mir_coalesce_rewrite(st);
  return 1;
}

static void mir_coalesce_hint_phys(MirColorState *st, const MirOperand *vop,
                                   const MirOperand *pop) {
  MirVregId v;
  if (vop->kind != MIR_OPK_VREG || pop->kind != MIR_OPK_PHYS) {
    return;
  }
  v = mir_color_find(st, vop->vreg);
  if (v < 0 || (size_t)v >= st->count || !st->colorable[v] ||
      st->fn->vregs[v].rclass != pop->rclass || pop->phys < 0 ||
      pop->phys >= 32) {
    return;
  }
  if (st->mask[v] & (1u << (unsigned)pop->phys)) {
    st->phys_hint[v] = pop->phys;
  }
}

void mir_color_note_phys_hints(MirColorState *st) {
  const MirFunction *fn = st->fn;
  for (size_t v = 0; v < st->count; v++) {
    st->phys_hint[v] = -1;
    st->copy_partner[v] = MIR_VREG_NONE;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if ((in->op == MIR_SHL || in->op == MIR_SHR || in->op == MIR_SAR) &&
        in->b.kind == MIR_OPK_VREG && mtlc_target()->arch == MTLC_TARGET_ARCH_X86_64) {
      MirOperand count_register = mir_op_phys(BINARY_GP_RCX, MIR_RC_GP);
      const MirOperand *values[2] = {&in->dst, &in->a};
      mir_coalesce_hint_phys(st, &in->b, &count_register);
      for (int k = 0; k < 2; k++) {
        if (values[k]->kind == MIR_OPK_VREG) {
          MirVregId held = mir_color_find(st, values[k]->vreg);
          if (held >= 0 && (size_t)held < st->count &&
              held != mir_color_find(st, in->b.vreg)) {
            st->avoid_shift_count[held] = 1;
          }
        }
      }
      continue;
    }
    if (in->op != MIR_MOV) {
      continue;
    }
    mir_coalesce_hint_phys(st, &in->a, &in->dst);
    mir_coalesce_hint_phys(st, &in->dst, &in->a);
    if (mir_coalesce_is_copy(in) && in->dst.vreg != in->a.vreg &&
        (size_t)in->dst.vreg < st->count && (size_t)in->a.vreg < st->count) {
      st->copy_partner[in->dst.vreg] = in->a.vreg;
      st->copy_partner[in->a.vreg] = in->dst.vreg;
    }
  }
}

void mir_color_mirror_members(MirColorState *st) {
  MirFunction *fn = st->fn;
  for (size_t v = 0; v < st->count; v++) {
    MirVregId r = mir_color_find(st, (MirVregId)v);
    if (r == (MirVregId)v) {
      continue;
    }
    fn->vregs[v].assigned = fn->vregs[r].assigned;
    fn->vregs[v].in_register = fn->vregs[r].in_register;
    fn->vregs[v].phys = fn->vregs[r].phys;
    fn->vregs[v].spill_offset = fn->vregs[r].spill_offset;
    fn->vregs[v].coalesced_into = r;
  }
}
