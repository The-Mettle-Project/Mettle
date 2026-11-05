#include "codegen/binary/mir.h"
#include "codegen/binary/mir_machine.h"
#include "codegen/binary/mir_cfg.h"
#include "codegen/binary/mir_color.h"
#include "codegen/binary/mir_external.h"
#include "../../common.h"
#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *mir_ra_trace_name(void);

static int mir_home_bytes_for(const MirVreg *vr, int *base) {
  int home = vr->home_bytes > 0 ? vr->home_bytes : 8;
  if (vr->home_granule) {
    home = (home + BINARY_SAFETY_GRANULE - 1) / BINARY_SAFETY_GRANULE *
           BINARY_SAFETY_GRANULE;
    *base = (*base + BINARY_SAFETY_GRANULE - 1) / BINARY_SAFETY_GRANULE *
            BINARY_SAFETY_GRANULE;
  }
  return home;
}

static const BinaryGpRegister MIR_GP_POOL[] = {
    BINARY_GP_RBX, BINARY_GP_R12, BINARY_GP_R13, BINARY_GP_R14, BINARY_GP_R15};
#define MIR_GP_POOL_COUNT (sizeof(MIR_GP_POOL) / sizeof(MIR_GP_POOL[0]))
static const BinaryGpRegister MIR_GP_EXTRA[] = {
    BINARY_GP_RAX, BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_RSI,
    BINARY_GP_RDI, BINARY_GP_R8,  BINARY_GP_R9};
#define MIR_GP_EXTRA_COUNT (sizeof(MIR_GP_EXTRA) / sizeof(MIR_GP_EXTRA[0]))
#define MIR_GP_LEAF_POOL_MAX (MIR_GP_POOL_COUNT + MIR_GP_EXTRA_COUNT)

static int mir_op_is_call_barrier(MirOpcode op) {
  return mir_op_has(op, MIR_OPF_CALL_BARRIER);
}

static int mir_fn_has_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_op_is_call_barrier(fn->insns[i].op)) {
      return 1;
    }
  }
  return 0;
}

static void mir_mark_crosses_call_exact(MirFunction *fn, const MirRaFacts *facts);

static void mir_mark_crosses_call(MirFunction *fn, const MirRaFacts *facts) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    fn->vregs[v].crosses_call = 0;
    fn->vregs[v].crosses_preserving_only = 1;
    fn->vregs[v].crosses_xmm_preserving_only = 1;
  }
  if (facts && facts->valid) {
    mir_mark_crosses_call_exact(fn, facts);
    return;
  }
  size_t n = fn->insn_count;
  int *prefix = (int *)calloc(3 * (n + 1), sizeof(int));
  if (prefix) {
    int *calls = prefix;
    int *clobber_rax = prefix + (n + 1);
    int *clobber_xmm = prefix + 2 * (n + 1);
    for (size_t i = 0; i < n; i++) {
      const MirInst *in = &fn->insns[i];
      int barrier = mir_op_is_call_barrier(in->op);
      int is_call = in->op == MIR_CALL;
      calls[i + 1] = calls[i] + barrier;
      clobber_rax[i + 1] =
          clobber_rax[i] + (barrier && !(is_call && in->preserves_rax));
      clobber_xmm[i + 1] =
          clobber_xmm[i] + (barrier && !(is_call && in->preserves_xmm));
    }
    for (size_t v = 0; v < fn->vreg_count; v++) {
      MirVreg *vr = &fn->vregs[v];
      long long lo;
      long long hi;
      if (vr->live_start == MIR_LIVE_NONE) {
        continue;
      }
      lo = vr->entry_live ? 0 : (long long)vr->live_start + 1;
      hi = vr->live_end;
      if (lo < 0) {
        lo = 0;
      }
      if (hi > (long long)n) {
        hi = (long long)n;
      }
      if (hi <= lo || calls[hi] == calls[lo]) {
        continue;
      }
      vr->crosses_call = 1;
      if (clobber_rax[hi] != clobber_rax[lo]) {
        vr->crosses_preserving_only = 0;
      }
      if (clobber_xmm[hi] != clobber_xmm[lo]) {
        vr->crosses_xmm_preserving_only = 0;
      }
    }
    free(prefix);
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (!mir_op_is_call_barrier(fn->insns[i].op)) {
      continue;
    }
    int is_call = fn->insns[i].op == MIR_CALL;
    int keeps_rax = is_call && fn->insns[i].preserves_rax;
    int keeps_xmm = is_call && fn->insns[i].preserves_xmm;
    int c = (int)i;
    for (size_t v = 0; v < fn->vreg_count; v++) {
      MirVreg *vr = &fn->vregs[v];
      if (vr->live_start != MIR_LIVE_NONE &&
          (vr->live_start < c || vr->entry_live) && vr->live_end > c) {
        vr->crosses_call = 1;
        if (!keeps_rax) {
          vr->crosses_preserving_only = 0;
        }
        if (!keeps_xmm) {
          vr->crosses_xmm_preserving_only = 0;
        }
      }
    }
  }
}

static void mir_drop_unused_preserves(MirFunction *fn) {
  int need_gp = 0;
  int need_xmm = 0;
  for (size_t v = 0; v < fn->vreg_count; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (!vr->in_register || !vr->crosses_call) {
      continue;
    }
    if (vr->rclass == MIR_RC_GP && vr->phys == (int)BINARY_GP_RAX) {
      need_gp = 1;
    } else if (vr->rclass == MIR_RC_XMM) {
      for (size_t i = 0; i < MIR_XMM_POOL_COUNT; i++) {
        if (vr->phys == (int)MIR_XMM_POOL[i]) {
          need_xmm = 1;
        }
      }
    }
  }
  if (!need_gp) {
    fn->preserve_slot = 0;
  }
  if (!need_xmm) {
    fn->preserve_xmm_slot = 0;
  }
}

static size_t mir_cross_pool_for(const MirVreg *vr,
                                 const BinaryGpRegister *base, size_t base_n,
                                 BinaryGpRegister *buffer) {
  size_t n = 0;
  for (; n < base_n; n++) {
    buffer[n] = base[n];
  }
  if (vr->crosses_preserving_only) {
    buffer[n++] = BINARY_GP_RAX;
  }
  return n;
}

static int mir_fn_has_preserving_call(const MirFunction *fn, int xmm) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op != MIR_CALL) {
      continue;
    }
    if (xmm ? fn->insns[i].preserves_xmm : fn->insns[i].preserves_rax) {
      return 1;
    }
  }
  return 0;
}

static int mir_fn_has_real_calls(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_op_has(fn->insns[i].op, MIR_OPF_REAL_CALL)) {
      return 1;
    }
  }
  return 0;
}

static int mir_fn_uses_slp(const MirFunction *fn) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_op_is_inline_kernel(fn->insns[i].op)) {
      return 1;
    }
  }
  return 0;
}

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

static int mir_reg_poolable(BinaryGpRegister reg, size_t param_count,
                            int is_leaf) {
  (void)is_leaf;
  (void)param_count;
  (void)reg;
  return 1;
}

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

#define MIR_GP_CROSSCALL_POOL_MAX 7
#define MIR_GP_CROSSCALL_POOL_EXT (MIR_GP_CROSSCALL_POOL_MAX + 1)

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

const BinaryXmmRegister MIR_XMM_POOL[MIR_XMM_POOL_COUNT] = {
    BINARY_XMM0, BINARY_XMM1, BINARY_XMM2, BINARY_XMM3};

static const BinaryXmmRegister MIR_XMM_NONVOL_POOL[] = {
    BINARY_XMM8,  BINARY_XMM9,  BINARY_XMM10, BINARY_XMM11,
    BINARY_XMM12, BINARY_XMM13, BINARY_XMM14, BINARY_XMM15};
#define MIR_XMM_NONVOL_POOL_COUNT \
  (sizeof(MIR_XMM_NONVOL_POOL) / sizeof(MIR_XMM_NONVOL_POOL[0]))

static int mir_gp_is_nonvolatile(BinaryGpRegister reg) {
  return code_generator_binary_gp_register_is_win64_nonvolatile(reg);
}

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

static int mir_inst_is_branch(const MirInst *in) {
  return mir_op_has(in->op, MIR_OPF_LABEL_BRANCH);
}

typedef struct {
  int l;
  int b;
} MirBackEdge;

static int mir_collect_back_edges(const MirFunction *fn,
                                  MirBackEdge **edges_out, size_t *count_out) {
  size_t label_count = 0;
  size_t branch_count = 0;
  *edges_out = NULL;
  *count_out = 0;

  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      label_count++;
    } else if (mir_inst_is_branch(in) && in->dst.kind == MIR_OPK_LABEL) {
      branch_count++;
    }
  }
  if (branch_count == 0) {
    return 1;
  }

  size_t slot_count = 16;
  while (slot_count < label_count * 2) {
    slot_count *= 2;
  }
  size_t *slots = calloc(slot_count, sizeof(*slots));
  MirBackEdge *edges = malloc(branch_count * sizeof(*edges));
  if (!slots || !edges) {
    free(slots);
    free(edges);
    return 0;
  }

  size_t mask = slot_count - 1;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op != MIR_LABEL || in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
      continue;
    }
    size_t h = mettle_fnv1a_hash(in->dst.sym) & mask;
    while (slots[h]) {
      if (strcmp(fn->insns[slots[h] - 1].dst.sym, in->dst.sym) == 0) {
        if (getenv("METTLE_MIR_DUPLABEL")) {
          fprintf(stderr, "MIR-DUPLABEL %s at %zu (first at %zu)\n",
                  in->dst.sym, i, slots[h] - 1);
        }
        h = SIZE_MAX;
        break;
      }
      h = (h + 1) & mask;
    }
    if (h != SIZE_MAX) {
      slots[h] = i + 1;
    }
  }

  size_t n = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (!mir_inst_is_branch(in) || in->dst.kind != MIR_OPK_LABEL ||
        !in->dst.sym) {
      continue;
    }
    int l = -1;
    size_t h = mettle_fnv1a_hash(in->dst.sym) & mask;
    while (slots[h]) {
      if (strcmp(fn->insns[slots[h] - 1].dst.sym, in->dst.sym) == 0) {
        l = (int)(slots[h] - 1);
        break;
      }
      h = (h + 1) & mask;
    }
    if (l < 0 || l >= (int)i) {
      continue;
    }
    edges[n].l = l;
    edges[n].b = (int)i;
    n++;
  }

  free(slots);
  if (n == 0) {
    free(edges);
    return 1;
  }
  *edges_out = edges;
  *count_out = n;
  return 1;
}

#define MIR_LIVE_CFG_MAX_WORK 8000000u
#define MIR_GRAPH_MAX_LIVE 2048
#define MIR_RA_BUSY_SLOTS 16
#define MIR_INTER_DENSE_MAX_BYTES (64u << 20)

typedef struct {
  size_t *slots;
  size_t mask;
} MirLabelMap;

typedef struct {
  int *block_of;
  int *block_start;
  unsigned long long *live_in;
  unsigned long long *live_out;
  size_t block_count;
  size_t words;
} MirLiveCfg;

static int mir_label_map_build(const MirFunction *fn, MirLabelMap *map) {
  size_t label_count = 0;
  size_t slot_count = 16;
  map->slots = NULL;
  map->mask = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      label_count++;
    }
  }
  while (slot_count < label_count * 2) {
    slot_count *= 2;
  }
  map->slots = (size_t *)calloc(slot_count, sizeof(*map->slots));
  if (!map->slots) {
    return 0;
  }
  map->mask = slot_count - 1;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    size_t h;
    int duplicate = 0;
    if (in->op != MIR_LABEL || in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
      continue;
    }
    h = mettle_fnv1a_hash(in->dst.sym) & map->mask;
    while (map->slots[h]) {
      if (strcmp(fn->insns[map->slots[h] - 1].dst.sym, in->dst.sym) == 0) {
        duplicate = 1;
        break;
      }
      h = (h + 1) & map->mask;
    }
    if (!duplicate) {
      map->slots[h] = i + 1;
    }
  }
  return 1;
}

static int mir_label_map_find(const MirFunction *fn, const MirLabelMap *map,
                              const char *name) {
  size_t h;
  if (!name || !map->slots) {
    return -1;
  }
  h = mettle_fnv1a_hash(name) & map->mask;
  while (map->slots[h]) {
    if (strcmp(fn->insns[map->slots[h] - 1].dst.sym, name) == 0) {
      return (int)(map->slots[h] - 1);
    }
    h = (h + 1) & map->mask;
  }
  return -1;
}

static int mir_inst_ends_block(const MirInst *in) {
  return mir_op_has(in->op, MIR_OPF_LABEL_BRANCH) ||
         in->op == MIR_JMP_TABLE || in->op == MIR_RET || in->op == MIR_TRAP;
}

static void mir_live_bit_set(unsigned long long *set, size_t v) {
  set[v >> 6] |= 1ull << (v & 63);
}

static int mir_live_bit_get(const unsigned long long *set, size_t v) {
  return (set[v >> 6] & (1ull << (v & 63))) != 0;
}

static void mir_live_note_uses(const MirFunction *fn, const MirOperand *op,
                               unsigned long long *use_set,
                               const unsigned long long *def_set,
                               int skip_plain_vreg) {
  MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
  if (op->kind == MIR_OPK_VREG) {
    if (skip_plain_vreg) {
      return;
    }
    ids[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    ids[0] = op->mem.base;
    ids[1] = op->mem.index;
  } else {
    return;
  }
  for (int k = 0; k < 2; k++) {
    MirVregId v = ids[k];
    if (v < 0 || (size_t)v >= fn->vreg_count) {
      continue;
    }
    if (!mir_live_bit_get(def_set, (size_t)v)) {
      mir_live_bit_set(use_set, (size_t)v);
    }
  }
}

static int mir_live_cfg_build(const MirFunction *fn, MirLiveCfg *cfg,
                              int precise_defs) {
  MirLabelMap map;
  unsigned char *leader = NULL;
  int *block_start = NULL;
  int *succ_head = NULL;
  int *succ_next = NULL;
  int *succ_block = NULL;
  unsigned long long *use_set = NULL;
  unsigned long long *def_set = NULL;
  unsigned char *killable = NULL;
  int *first_dst = NULL;
  int *first_any = NULL;
  size_t words = (fn->vreg_count + 63) / 64;
  size_t nblocks = 0;
  size_t succ_capacity = 0;
  size_t succ_count = 0;
  int ok = 0;
  int changed = 1;

  cfg->block_of = NULL;
  cfg->block_start = NULL;
  cfg->live_in = NULL;
  cfg->live_out = NULL;
  cfg->block_count = 0;
  cfg->words = words;
  if (fn->insn_count == 0 || fn->vreg_count == 0 || words == 0) {
    return 0;
  }
  if ((unsigned long long)fn->insn_count * (unsigned long long)words >
      MIR_LIVE_CFG_MAX_WORK) {
    return 0;
  }
  if (!mir_label_map_build(fn, &map)) {
    return 0;
  }

  leader = (unsigned char *)calloc(fn->insn_count, sizeof(*leader));
  cfg->block_of = (int *)malloc(fn->insn_count * sizeof(*cfg->block_of));
  if (!precise_defs) {
    killable = (unsigned char *)calloc(fn->vreg_count, sizeof(*killable));
    first_dst = (int *)malloc(fn->vreg_count * sizeof(*first_dst));
    first_any = (int *)malloc(fn->vreg_count * sizeof(*first_any));
  }
  if (!leader || !cfg->block_of ||
      (!precise_defs && (!killable || !first_dst || !first_any))) {
    goto done;
  }
  if (!precise_defs) {
    for (size_t v = 0; v < fn->vreg_count; v++) {
      first_dst[v] = -1;
      first_any[v] = -1;
    }
  }

  leader[0] = 1;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL) {
      leader[i] = 1;
    }
    if (mir_inst_ends_block(in) && i + 1 < fn->insn_count) {
      leader[i + 1] = 1;
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (leader[i]) {
      nblocks++;
    }
    cfg->block_of[i] = (int)nblocks - 1;
  }
  block_start = (int *)malloc(nblocks * sizeof(*block_start));
  succ_head = (int *)malloc(nblocks * sizeof(*succ_head));
  if (!block_start || !succ_head) {
    goto done;
  }
  {
    size_t b = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      if (leader[i]) {
        block_start[b++] = (int)i;
      }
    }
  }
  for (size_t b = 0; b < nblocks; b++) {
    succ_head[b] = -1;
  }

  succ_capacity = nblocks * 2 + 8;
  succ_next = (int *)malloc(succ_capacity * sizeof(*succ_next));
  succ_block = (int *)malloc(succ_capacity * sizeof(*succ_block));
  if (!succ_next || !succ_block) {
    goto done;
  }
  for (size_t b = 0; b < nblocks; b++) {
    size_t last = (b + 1 < nblocks) ? (size_t)block_start[b + 1] - 1
                                    : fn->insn_count - 1;
    const MirInst *in = &fn->insns[last];
    int targets[2];
    int target_count = 0;
    int fall_through = 1;
    const MirJumpTable *table = NULL;
    if (in->op == MIR_JMP) {
      fall_through = 0;
      targets[target_count++] = mir_label_map_find(fn, &map, in->dst.sym);
    } else if (mir_op_has(in->op, MIR_OPF_CONDITIONAL_BRANCH)) {
      targets[target_count++] = mir_label_map_find(fn, &map, in->dst.sym);
    } else if (in->op == MIR_JMP_TABLE) {
      fall_through = 0;
      table = (const MirJumpTable *)in->aux;
      if (!table) {
        goto done;
      }
    } else if (in->op == MIR_RET || in->op == MIR_TRAP) {
      fall_through = 0;
    }
    for (int t = 0; t < target_count; t++) {
      if (targets[t] < 0) {
        goto done;
      }
    }
    if (fall_through && b + 1 >= nblocks) {
      fall_through = 0;
    }
    {
      size_t need = succ_count + (size_t)target_count + (fall_through ? 1u : 0u);
      if (table) {
        need += table->count;
      }
      if (need > succ_capacity) {
        int *n1;
        int *n2;
        size_t grown = succ_capacity * 2 + need;
        n1 = (int *)realloc(succ_next, grown * sizeof(*succ_next));
        if (!n1) {
          goto done;
        }
        succ_next = n1;
        n2 = (int *)realloc(succ_block, grown * sizeof(*succ_block));
        if (!n2) {
          goto done;
        }
        succ_block = n2;
        succ_capacity = grown;
      }
    }
    for (int t = 0; t < target_count; t++) {
      succ_block[succ_count] = cfg->block_of[targets[t]];
      succ_next[succ_count] = succ_head[b];
      succ_head[b] = (int)succ_count;
      succ_count++;
    }
    if (table) {
      for (size_t t = 0; t < table->count; t++) {
        int target = mir_label_map_find(fn, &map, table->labels[t]);
        if (target < 0) {
          goto done;
        }
        succ_block[succ_count] = cfg->block_of[target];
        succ_next[succ_count] = succ_head[b];
        succ_head[b] = (int)succ_count;
        succ_count++;
      }
    }
    if (fall_through) {
      succ_block[succ_count] = (int)b + 1;
      succ_next[succ_count] = succ_head[b];
      succ_head[b] = (int)succ_count;
      succ_count++;
    }
  }

  if (!precise_defs) {
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      const MirOperand *ops[3] = {&in->dst, &in->a, &in->b};
      for (int k = 0; k < 3; k++) {
        MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
        if (ops[k]->kind == MIR_OPK_VREG) {
          ids[0] = ops[k]->vreg;
        } else if (ops[k]->kind == MIR_OPK_MEM) {
          ids[0] = ops[k]->mem.base;
          ids[1] = ops[k]->mem.index;
        }
        for (int j = 0; j < 2; j++) {
          MirVregId v = ids[j];
          if (v < 0 || (size_t)v >= fn->vreg_count) {
            continue;
          }
          if (first_any[v] < 0) {
            first_any[v] = (int)i;
          }
          if (k == 0 && ops[k]->kind == MIR_OPK_VREG && first_dst[v] < 0) {
            first_dst[v] = (int)i;
          }
        }
      }
    }
    for (size_t v = 0; v < fn->vreg_count; v++) {
      killable[v] = (first_dst[v] >= 0 && first_dst[v] == first_any[v] &&
                     !fn->vregs[v].entry_live)
                        ? 1
                        : 0;
    }
  }

  use_set = (unsigned long long *)calloc(nblocks * words, sizeof(*use_set));
  def_set = (unsigned long long *)calloc(nblocks * words, sizeof(*def_set));
  cfg->live_in =
      (unsigned long long *)calloc(nblocks * words, sizeof(*cfg->live_in));
  if (!use_set || !def_set || !cfg->live_in) {
    goto done;
  }
  for (size_t b = 0; b < nblocks; b++) {
    size_t lo = (size_t)block_start[b];
    size_t hi = (b + 1 < nblocks) ? (size_t)block_start[b + 1] : fn->insn_count;
    unsigned long long *u = use_set + b * words;
    unsigned long long *d = def_set + b * words;
    for (size_t i = lo; i < hi; i++) {
      const MirInst *in = &fn->insns[i];
      mir_live_note_uses(fn, &in->dst, u, d, 1);
      mir_live_note_uses(fn, &in->a, u, d, 0);
      mir_live_note_uses(fn, &in->b, u, d, 0);
      if (in->dst.kind == MIR_OPK_VREG) {
        MirVregId v = in->dst.vreg;
        if (v >= 0 && (size_t)v < fn->vreg_count) {
          if (precise_defs) {
            mir_live_bit_set(d, (size_t)v);
          } else if (killable[v] && first_dst[v] == (int)i) {
            mir_live_bit_set(d, (size_t)v);
          } else if (!mir_live_bit_get(d, (size_t)v)) {
            mir_live_bit_set(u, (size_t)v);
          }
        }
      }
    }
  }

  while (changed) {
    changed = 0;
    for (size_t bi = nblocks; bi > 0; bi--) {
      size_t b = bi - 1;
      const unsigned long long *u = use_set + b * words;
      const unsigned long long *d = def_set + b * words;
      unsigned long long *in_set = cfg->live_in + b * words;
      for (int e = succ_head[b]; e >= 0; e = succ_next[e]) {
        const unsigned long long *s =
            cfg->live_in + (size_t)succ_block[e] * words;
        for (size_t w = 0; w < words; w++) {
          unsigned long long next = in_set[w] | (s[w] & ~d[w]);
          if (next != in_set[w]) {
            in_set[w] = next;
            changed = 1;
          }
        }
      }
      for (size_t w = 0; w < words; w++) {
        unsigned long long next = in_set[w] | u[w];
        if (next != in_set[w]) {
          in_set[w] = next;
          changed = 1;
        }
      }
    }
  }

  if (precise_defs) {
    cfg->live_out =
        (unsigned long long *)calloc(nblocks * words, sizeof(*cfg->live_out));
    if (!cfg->live_out) {
      goto done;
    }
    for (size_t b = 0; b < nblocks; b++) {
      unsigned long long *out_set = cfg->live_out + b * words;
      for (int e = succ_head[b]; e >= 0; e = succ_next[e]) {
        const unsigned long long *s =
            cfg->live_in + (size_t)succ_block[e] * words;
        for (size_t w = 0; w < words; w++) {
          out_set[w] |= s[w];
        }
      }
    }

    cfg->block_start = block_start;
    block_start = NULL;
  }
  cfg->block_count = nblocks;
  ok = 1;

done:
  free(map.slots);
  free(leader);
  free(block_start);
  free(succ_head);
  free(succ_next);
  free(succ_block);
  free(use_set);
  free(def_set);
  free(killable);
  free(first_dst);
  free(first_any);
  if (!ok) {
    free(cfg->block_of);
    free(cfg->block_start);
    free(cfg->live_in);
    free(cfg->live_out);
    cfg->block_of = NULL;
    cfg->block_start = NULL;
    cfg->live_in = NULL;
    cfg->live_out = NULL;
    cfg->block_count = 0;
  }
  return ok;
}

static void mir_live_cfg_free(MirLiveCfg *cfg) {
  free(cfg->block_of);
  free(cfg->block_start);
  free(cfg->live_in);
  free(cfg->live_out);
  cfg->block_of = NULL;
  cfg->block_start = NULL;
  cfg->live_in = NULL;
  cfg->live_out = NULL;
  cfg->block_count = 0;
}

static unsigned char *mir_reads_before_def(const MirFunction *fn) {
  unsigned char *defined = (unsigned char *)calloc(fn->vreg_count, 1);
  unsigned char *early = (unsigned char *)calloc(fn->vreg_count, 1);
  MirVregId uses[6];
  if (!defined || !early) {
    free(defined);
    free(early);
    return NULL;
  }
  for (size_t p = 0; p < fn->param_count; p++) {
    if (fn->params[p].vreg >= 0 && (size_t)fn->params[p].vreg < fn->vreg_count) {
      defined[fn->params[p].vreg] = 1;
    }
  }
  if (fn->returns_indirect && fn->indirect_return_vreg >= 0 &&
      (size_t)fn->indirect_return_vreg < fn->vreg_count) {
    defined[fn->indirect_return_vreg] = 1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    MirVregId d = mir_cfg_insn_def(in);
    int n = mir_cfg_insn_uses(in, uses);
    for (int k = 0; k < n; k++) {
      if (uses[k] >= 0 && (size_t)uses[k] < fn->vreg_count &&
          !defined[uses[k]]) {
        early[uses[k]] = 1;
      }
    }
    if (d >= 0 && (size_t)d < fn->vreg_count) {
      defined[d] = 1;
    }
  }
  free(defined);
  return early;
}

static void mir_extend_across_edge(MirFunction *fn, int l, int b,
                                   const unsigned long long *header_live,
                                   int live_in_crosses,
                                   const unsigned char *read_first,
                                   int *changed) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->live_start == MIR_LIVE_NONE) {
      continue;
    }
    if (header_live && !mir_live_bit_get(header_live, v)) {
      continue;
    }
    if (vr->live_end < l || vr->live_start > b) {
      continue;
    }
    int crosses = (live_in_crosses && header_live != NULL) ||
                  (read_first && !header_live && read_first[v]) ||
                  (vr->live_start < l) || (vr->live_end > b) ||
                  (vr->entry_live && l == 0);
    if (!crosses) {
      continue;
    }
    vr->loop_carried = 1;
    if (vr->live_start > l) {
      vr->live_start = l;
      *changed = 1;
    }
    if (vr->live_end < b) {
      vr->live_end = b;
      *changed = 1;
    }
  }
}

static void mir_compute_liveness(MirFunction *fn, int live_in_crosses) {
  for (size_t i = 0; i < fn->vreg_count; i++) {
    fn->vregs[i].live_start = MIR_LIVE_NONE;
    fn->vregs[i].live_end = MIR_LIVE_NONE;
    fn->vregs[i].loop_carried = 0;
    fn->vregs[i].entry_live = 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP) {
      continue;
    }
    mir_note_operand_liveness(fn, &in->dst, (int)i);
    mir_note_operand_liveness(fn, &in->a, (int)i);
    mir_note_operand_liveness(fn, &in->b, (int)i);
  }

  for (size_t i = 0; i < fn->param_count; i++) {
    MirVreg *pv = &fn->vregs[fn->params[i].vreg];
    if (pv->live_end != MIR_LIVE_NONE) {
      pv->live_start = 0;
      pv->entry_live = 1;
    }
  }
  if (fn->returns_indirect && fn->indirect_return_vreg != MIR_VREG_NONE) {
    MirVreg *rv = &fn->vregs[fn->indirect_return_vreg];
    if (rv->live_end != MIR_LIVE_NONE) {
      rv->live_start = 0;
      rv->entry_live = 1;
    }
  }

  MirBackEdge *edges = NULL;
  size_t edge_count = 0;
  int changed = 1;
  unsigned char *read_first = live_in_crosses ? mir_reads_before_def(fn) : NULL;
  if (mir_collect_back_edges(fn, &edges, &edge_count)) {
    MirLiveCfg cfg;
    int have_cfg = edge_count > 0 && mir_live_cfg_build(fn, &cfg, 0);
    while (changed) {
      changed = 0;
      for (size_t e = 0; e < edge_count; e++) {
        const unsigned long long *header_live = NULL;
        if (have_cfg) {
          header_live =
              cfg.live_in + (size_t)cfg.block_of[edges[e].l] * cfg.words;
        }
        mir_extend_across_edge(fn, edges[e].l, edges[e].b, header_live,
                               live_in_crosses, read_first, &changed);
      }
    }
    if (have_cfg) {
      mir_live_cfg_free(&cfg);
    }
    free(edges);
    free(read_first);
    return;
  }

  while (changed) {
    changed = 0;
    for (size_t i = 0; i < fn->insn_count; i++) {
      const MirInst *in = &fn->insns[i];
      if (!mir_inst_is_branch(in) || in->dst.kind != MIR_OPK_LABEL) {
        continue;
      }
      int l = mir_find_label(fn, in->dst.sym);
      int b = (int)i;
      if (l < 0 || l >= b) {
        continue;
      }
      mir_extend_across_edge(fn, l, b, NULL, live_in_crosses, read_first,
                             &changed);
    }
  }
  free(read_first);
}

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

static int mir_ra_operand_dies(const MirRaFacts *facts, const MirFunction *fn,
                               size_t i, int which) {
  const MirInst *in = &fn->insns[i];
  const MirOperand *op = which ? &in->b : &in->a;
  if (facts && facts->valid) {
    return which ? facts->b_dies[i] : facts->a_dies[i];
  }
  return op->kind == MIR_OPK_VREG && fn->vregs[op->vreg].live_end == (int)i;
}

static void mir_compute_coalesce_hints(MirFunction *fn,
                                       const MirRaFacts *facts) {
  for (size_t v = 0; v < fn->vreg_count; v++) {
    fn->vregs[v].coalesce_hint = MIR_VREG_NONE;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    int commutative;
    if (!mir_op_has(in->op, MIR_OPF_COALESCE_CANDIDATE)) {
      continue;
    }
    commutative = mir_op_has(in->op, MIR_OPF_COMMUTATIVE);
    if (in->dst.kind != MIR_OPK_VREG) {
      continue;
    }
    MirRegClass dcls = fn->vregs[in->dst.vreg].rclass;
    if (in->op == MIR_IMUL && in->b.kind == MIR_OPK_IMM) {
      continue;
    }
    MirVregId d = in->dst.vreg;
    MirVregId cand = MIR_VREG_NONE;
    if (in->a.kind == MIR_OPK_VREG && in->a.vreg != d &&
        fn->vregs[in->a.vreg].rclass == dcls &&
        mir_ra_operand_dies(facts, fn, i, 0)) {
      cand = in->a.vreg;
    } else if (commutative && in->b.kind == MIR_OPK_VREG && in->b.vreg != d &&
               fn->vregs[in->b.vreg].rclass == dcls &&
               mir_ra_operand_dies(facts, fn, i, 1)) {
      cand = in->b.vreg;
    }
    fn->vregs[d].coalesce_hint = cand;
  }
}

typedef struct {
  int *pos;
  size_t count;
  size_t cap;
} MirClobberList;

typedef struct {
  const MirFunction *fn;
  const MirInst *insns;
  size_t insn_count;
  int valid;
  MirClobberList explicit_fixed[16];
  MirClobberList rax_implicit;
  MirClobberList rcx_implicit;
  MirClobberList rdx_implicit;
} MirClobberIndex;

static MirClobberIndex g_mir_clobber_index = {0};

static void mir_clobber_list_free(MirClobberList *l) {
  free(l->pos);
  l->pos = NULL;
  l->count = 0;
  l->cap = 0;
}

static int mir_clobber_list_push(MirClobberList *l, int k) {
  if (l->count == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 16;
    int *pos = realloc(l->pos, cap * sizeof(*pos));
    if (!pos) {
      return 0;
    }
    l->pos = pos;
    l->cap = cap;
  }
  l->pos[l->count++] = k;
  return 1;
}

static int mir_clobber_list_hit(const MirClobberList *l, int s, int e) {
  size_t lo = 0;
  size_t hi = l->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (l->pos[mid] <= s) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < l->count && l->pos[lo] < e;
}

static void mir_clobber_index_reset(void) {
  for (size_t r = 0; r < 16; r++) {
    mir_clobber_list_free(&g_mir_clobber_index.explicit_fixed[r]);
  }
  mir_clobber_list_free(&g_mir_clobber_index.rax_implicit);
  mir_clobber_list_free(&g_mir_clobber_index.rcx_implicit);
  mir_clobber_list_free(&g_mir_clobber_index.rdx_implicit);
  g_mir_clobber_index.fn = NULL;
  g_mir_clobber_index.insns = NULL;
  g_mir_clobber_index.insn_count = 0;
  g_mir_clobber_index.valid = 0;
}

static int mir_clobber_index_ensure(const MirFunction *fn) {
  MirClobberIndex *ix = &g_mir_clobber_index;
  if (ix->fn == fn && ix->insns == fn->insns &&
      ix->insn_count == fn->insn_count) {
    return ix->valid;
  }

  mir_clobber_index_reset();
  ix->fn = fn;
  ix->insns = fn->insns;
  ix->insn_count = fn->insn_count;
  ix->valid = 1;

  for (size_t k = 0; k < fn->insn_count; k++) {
    const MirInst *in = &fn->insns[k];
    int ok = 1;
    const MirOperand *fixed[3] = {&in->dst, &in->a, &in->b};
    for (int f = 0; ok && f < 3; f++) {
      if (fixed[f]->kind == MIR_OPK_PHYS && fixed[f]->rclass == MIR_RC_GP &&
          fixed[f]->phys >= 0 && fixed[f]->phys < 16) {
        ok = mir_clobber_list_push(&ix->explicit_fixed[fixed[f]->phys], (int)k);
      }
    }
    if (ok) {
      unsigned implicit = mir_inst_fixed_gp(in);
      if (implicit & (1u << (unsigned)BINARY_GP_RAX)) {
        ok = mir_clobber_list_push(&ix->rax_implicit, (int)k);
      }
      if (ok && (implicit & (1u << (unsigned)BINARY_GP_RDX))) {
        ok = mir_clobber_list_push(&ix->rdx_implicit, (int)k);
      }
      if (ok && (implicit & (1u << (unsigned)BINARY_GP_RCX))) {
        ok = mir_clobber_list_push(&ix->rcx_implicit, (int)k);
      }
      if (ok && mir_op_has(in->op, MIR_OPF_CLOBBERS_LISTED_BY_KERNEL)) {
        const MirKernelAux *ka = (const MirKernelAux *)in->aux;
        const MirIrKernel *kern = ka ? mir_ir_kernel_at(ka->kernel_index) : NULL;
        unsigned clobbers = kern ? kern->gp_clobbers : 0u;
        for (int r = 0; clobbers && ok; r++, clobbers >>= 1) {
          if (clobbers & 1u) {
            ok = mir_clobber_list_push(&ix->explicit_fixed[r], (int)k);
          }
        }
      }
      if (ok && mir_op_has(in->op, MIR_OPF_CLOBBERS_EVERY_GP)) {
        for (int r = 0; ok && r < mir_machine()->gp_register_count; r++) {
          ok = mir_clobber_list_push(&ix->explicit_fixed[r], (int)k);
        }
      }
    }
    if (!ok) {
      mir_clobber_index_reset();
      ix->fn = fn;
      ix->insns = fn->insns;
      ix->insn_count = fn->insn_count;
      ix->valid = 0;
      return 0;
    }
  }

  return 1;
}

static int mir_reg_is_pinned(BinaryGpRegister reg) {
  return mir_machine_gp_is_pinned((int)reg);
}

static int mir_reg_clobbered_by_index(const MirFunction *fn,
                                      BinaryGpRegister reg, int s, int e,
                                      int *answered) {
  const MirClobberIndex *ix = &g_mir_clobber_index;

  *answered = 0;
  if ((int)reg < 0 || (int)reg >= 16 || !mir_clobber_index_ensure(fn)) {
    return 0;
  }
  *answered = 1;
  if (mir_clobber_list_hit(&ix->explicit_fixed[reg], s, e)) {
    return 1;
  }
  if (reg == BINARY_GP_RAX) {
    return mir_clobber_list_hit(&ix->rax_implicit, s, e);
  }
  if (reg == BINARY_GP_RCX) {
    return mir_clobber_list_hit(&ix->rcx_implicit, s, e);
  }
  if (reg == BINARY_GP_RDX) {
    return mir_clobber_list_hit(&ix->rdx_implicit, s, e);
  }
  return 0;
}

static int mir_inst_pins_reg(const MirInst *in, BinaryGpRegister reg) {
  return mir_inst_pins_gp(in, (int)reg);
}

static int mir_inst_clobbers_reg(const MirInst *in, BinaryGpRegister reg) {
  const MirOperand *fixed[3] = {&in->dst, &in->a, &in->b};

  for (int f = 0; f < 3; f++) {
    if (fixed[f]->kind == MIR_OPK_PHYS && fixed[f]->rclass == MIR_RC_GP &&
        fixed[f]->phys == (int)reg) {
      return 1;
    }
  }
  if (in->op == MIR_IR_KERNEL && (int)reg >= 0 && (int)reg < 16) {
    const MirKernelAux *aux = (const MirKernelAux *)in->aux;
    const MirIrKernel *kernel = aux ? mir_ir_kernel_at(aux->kernel_index)
                                    : NULL;
    if (kernel && (kernel->gp_clobbers & (1u << (unsigned)reg))) {
      return 1;
    }
  }
  if (in->op == MIR_INLINE_ASM) {
    return 1;
  }
  return mir_reg_is_pinned(reg) && mir_inst_pins_reg(in, reg);
}

static int mir_reg_clobbered_in_range(const MirFunction *fn,
                                      BinaryGpRegister reg, int s, int e) {
  int answered = 0;
  int indexed = mir_reg_clobbered_by_index(fn, reg, s, e, &answered);

  if (answered) {
    return indexed;
  }
  for (int k = s + 1; k < e; k++) {
    if (mir_inst_clobbers_reg(&fn->insns[k], reg)) {
      return 1;
    }
  }
  return 0;
}

static uint32_t mir_ra_insn_busy_mask(const MirInst *in) {
  uint32_t m = 0;
  for (int r = 0; r < 16; r++) {
    if (mir_inst_clobbers_reg(in, (BinaryGpRegister)r)) {
      m |= 1u << (unsigned)r;
    }
  }
  return m;
}

static void mir_ra_facts_free(MirRaFacts *facts) {
  if (facts->valid) {
    mir_cfg_free(&facts->cfg);
  }
  free(facts->clobbered);
  free(facts->a_dies);
  free(facts->b_dies);
  free(facts->undef_live);
  memset(facts, 0, sizeof(*facts));
}

static void mir_ra_facts_note_operand(const MirRaFacts *facts,
                                      const MirCfgCursor *cur,
                                      const MirOperand *op,
                                      unsigned char *dies) {
  *dies = 0;
  if (op->kind == MIR_OPK_VREG && op->vreg >= 0 &&
      (size_t)op->vreg < facts->cfg.fn->vreg_count &&
      !mir_cfg_set_get(cur->live, (size_t)op->vreg)) {
    *dies = 1;
  }
}

typedef struct {
  unsigned long long *undef;
  unsigned long long *gp;
  unsigned long long *xmm;
  uint32_t busy[MIR_RA_BUSY_SLOTS];
  unsigned long long *busy_bits[MIR_RA_BUSY_SLOTS];
  size_t busy_count;
  size_t words;
} MirRaScan;

static void mir_ra_scan_free(MirRaScan *scan) {
  free(scan->undef);
  free(scan->gp);
  free(scan->xmm);
  for (size_t k = 0; k < scan->busy_count; k++) {
    free(scan->busy_bits[k]);
  }
  memset(scan, 0, sizeof(*scan));
}

static int mir_ra_scan_init(MirRaScan *scan, const MirFunction *fn,
                            size_t words) {
  size_t n = words ? words : 1;
  memset(scan, 0, sizeof(*scan));
  scan->words = words;
  scan->undef = (unsigned long long *)calloc(n, sizeof(unsigned long long));
  scan->gp = (unsigned long long *)calloc(n, sizeof(unsigned long long));
  scan->xmm = (unsigned long long *)calloc(n, sizeof(unsigned long long));
  if (!scan->undef || !scan->gp || !scan->xmm) {
    mir_ra_scan_free(scan);
    return 0;
  }
  for (size_t v = 0; v < fn->vreg_count && (v >> 6) < words; v++) {
    unsigned long long bit = 1ull << (v & 63);
    if (fn->vregs[v].address_taken) {
      continue;
    }
    if (fn->vregs[v].rclass == MIR_RC_XMM) {
      scan->xmm[v >> 6] |= bit;
    } else {
      scan->gp[v >> 6] |= bit;
    }
  }
  return 1;
}

static unsigned long long *mir_ra_scan_busy_bits(MirRaScan *scan,
                                                 uint32_t busy) {
  unsigned long long *bits;
  for (size_t k = 0; k < scan->busy_count; k++) {
    if (scan->busy[k] == busy) {
      return scan->busy_bits[k];
    }
  }
  if (scan->busy_count == MIR_RA_BUSY_SLOTS) {
    return NULL;
  }
  bits = (unsigned long long *)calloc(scan->words ? scan->words : 1,
                                      sizeof(unsigned long long));
  if (!bits) {
    return NULL;
  }
  scan->busy[scan->busy_count] = busy;
  scan->busy_bits[scan->busy_count] = bits;
  scan->busy_count++;
  return bits;
}

static void mir_ra_scan_finish(MirRaScan *scan, MirRaFacts *facts) {
  const MirFunction *fn = facts->cfg.fn;
  for (size_t w = 0; w < scan->words; w++) {
    unsigned long long bits = scan->undef[w];
    while (bits) {
      size_t v = w * 64 + (size_t)__builtin_ctzll(bits);
      bits &= bits - 1;
      if (v < fn->vreg_count) {
        facts->undef_live[v] = 1;
      }
    }
  }
  for (size_t k = 0; k < scan->busy_count; k++) {
    for (size_t w = 0; w < scan->words; w++) {
      unsigned long long bits = scan->busy_bits[k][w];
      while (bits) {
        size_t v = w * 64 + (size_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        facts->clobbered[v] |= scan->busy[k];
      }
    }
  }
}

static void mir_ra_facts_at(MirRaFacts *facts, MirRaScan *scan,
                            const MirCfgCursor *cur, size_t i) {
  const MirFunction *fn = facts->cfg.fn;
  const MirInst *in = &fn->insns[i];
  MirVregId d = mir_cfg_insn_def(in);
  uint32_t busy = in->op == MIR_NOP ? 0 : mir_ra_insn_busy_mask(in);
  unsigned long long *busy_bits = busy != 0 ? mir_ra_scan_busy_bits(scan, busy)
                                            : NULL;
  int live_gp = 0;
  int live_xmm = 0;
  mir_ra_facts_note_operand(facts, cur, &in->a, &facts->a_dies[i]);
  mir_ra_facts_note_operand(facts, cur, &in->b, &facts->b_dies[i]);
  for (size_t w = 0; w < scan->words; w++) {
    unsigned long long live = cur->live[w];
    unsigned long long both = live & cur->defd[w];
    scan->undef[w] |= live & ~cur->defd[w];
    live_gp += __builtin_popcountll(both & scan->gp[w]);
    live_xmm += __builtin_popcountll(both & scan->xmm[w]);
    if (busy != 0) {
      unsigned long long hit = both & (scan->gp[w] | scan->xmm[w]);
      if (d >= 0 && ((size_t)d >> 6) == w) {
        hit &= ~(1ull << ((size_t)d & 63));
      }
      if (busy_bits) {
        busy_bits[w] |= hit;
      } else {
        while (hit) {
          size_t v = w * 64 + (size_t)__builtin_ctzll(hit);
          hit &= hit - 1;
          facts->clobbered[v] |= busy;
        }
      }
    }
  }
  if (live_gp > facts->max_live_gp) {
    facts->max_live_gp = live_gp;
    facts->max_live_at = i;
    if (getenv("METTLE_RA_LIVE_DUMP")) {
      fprintf(stderr, "RA-LIVE\t%s\tinsn=%zu\tgp=%d:", mir_ra_trace_name(), i,
              live_gp);
      for (size_t w = 0; w < facts->cfg.words; w++) {
        unsigned long long bits = cur->live[w] & cur->defd[w];
        while (bits) {
          size_t v = w * 64 + (size_t)__builtin_ctzll(bits);
          bits &= bits - 1;
          if (v < fn->vreg_count && !fn->vregs[v].address_taken &&
              fn->vregs[v].rclass == MIR_RC_GP) {
            fprintf(stderr, " v%zu", v);
          }
        }
      }
      fputc('\n', stderr);
    }
  }
  if (live_xmm > facts->max_live_xmm) {
    facts->max_live_xmm = live_xmm;
  }
}

static int mir_ra_facts_build(MirRaFacts *facts, MirFunction *fn) {
  MirCfgCursor cur;
  MirRaScan scan;
  static int interval_only = -1;
  memset(facts, 0, sizeof(*facts));
  if (interval_only < 0) {
    interval_only = getenv("METTLE_INTERVAL_INTERFERENCE") ? 1 : 0;
  }
  if (interval_only || !mir_cfg_build(&facts->cfg, fn)) {
    return 0;
  }
  facts->valid = 1;
  facts->clobbered = (uint32_t *)calloc(fn->vreg_count, sizeof(uint32_t));
  facts->a_dies = (unsigned char *)calloc(fn->insn_count, 1);
  facts->b_dies = (unsigned char *)calloc(fn->insn_count, 1);
  facts->undef_live = (unsigned char *)calloc(fn->vreg_count, 1);
  if (!facts->clobbered || !facts->a_dies || !facts->b_dies ||
      !facts->undef_live || !mir_ra_scan_init(&scan, fn, facts->cfg.words)) {
    mir_ra_facts_free(facts);
    return 0;
  }
  if (!mir_cfg_cursor_init(&cur, &facts->cfg)) {
    mir_ra_scan_free(&scan);
    mir_ra_facts_free(facts);
    return 0;
  }
  for (size_t b = 0; b < facts->cfg.block_count; b++) {
    const MirCfgBlock *blk = &facts->cfg.blocks[b];
    mir_cfg_cursor_start_block(&cur, b);
    for (int at = blk->end; at > blk->start; at--) {
      mir_ra_facts_at(facts, &scan, &cur, (size_t)at - 1);
      mir_cfg_cursor_step_back(&cur);
    }
  }
  mir_cfg_cursor_free(&cur);
  mir_ra_scan_finish(&scan, facts);
  mir_ra_scan_free(&scan);
  return 1;
}

static void mir_mark_crosses_call_exact(MirFunction *fn,
                                        const MirRaFacts *facts) {
  MirCfgCursor cur;
  if (!mir_cfg_cursor_init(&cur, &facts->cfg)) {
    return;
  }
  for (size_t b = 0; b < facts->cfg.block_count; b++) {
    const MirCfgBlock *blk = &facts->cfg.blocks[b];
    mir_cfg_cursor_start_block(&cur, b);
    for (int at = blk->end; at > blk->start; at--) {
      const MirInst *in = &fn->insns[at - 1];
      MirVregId d = mir_cfg_insn_def(in);
      int is_call = in->op == MIR_CALL;
      int keeps_rax = is_call && in->preserves_rax;
      int keeps_xmm = is_call && in->preserves_xmm;
      if (!mir_op_is_call_barrier(in->op)) {
        mir_cfg_cursor_step_back(&cur);
        continue;
      }
      for (size_t w = 0; w < facts->cfg.words; w++) {
        unsigned long long bits = cur.live[w] & cur.defd[w];
        while (bits) {
          size_t v = w * 64 + (size_t)__builtin_ctzll(bits);
          MirVreg *vr = &fn->vregs[v];
          bits &= bits - 1;
          if ((MirVregId)v == d) {
            continue;
          }
          vr->crosses_call = 1;
          if (!keeps_rax) {
            vr->crosses_preserving_only = 0;
          }
          if (!keeps_xmm) {
            vr->crosses_xmm_preserving_only = 0;
          }
        }
      }
      mir_cfg_cursor_step_back(&cur);
    }
  }
  mir_cfg_cursor_free(&cur);
}

static int mir_ra_reg_busy(const MirRaFacts *facts, const MirFunction *fn,
                           MirVregId v, BinaryGpRegister reg) {
  const MirVreg *vr = &fn->vregs[v];
  if (facts && facts->valid) {
    return (facts->clobbered[v] >> (unsigned)reg) & 1u;
  }
  return mir_reg_clobbered_in_range(fn, reg, vr->live_start, vr->live_end);
}

static int mir_vreg_is_param_in_reg(const MirFunction *fn, MirVregId v,
                                    BinaryGpRegister reg) {
  int ai = mir_reg_arg_index(reg);
  if (ai < 0) {
    return 0;
  }
  /* Only where the function calls nothing. A parameter that dies AT a call
     does not cross it, so nothing else stops it sitting in an argument
     register -- and then marshalling the outgoing arguments overwrites it
     while it is still live. `invoke(cb, v)` placed v in RCX over cb and
     called v. */
  if (mir_fn_has_real_calls(fn)) {
    return 0;
  }
  for (size_t i = 0; i < fn->param_count; i++) {
    if (fn->params[i].vreg == v && !fn->params[i].is_float &&
        fn->params[i].arg_index == ai) {
      return 1;
    }
  }
  return 0;
}

static uint32_t mir_color_reg_mask(const MirFunction *fn, MirVregId v,
                                   const BinaryGpRegister *gp_leaf_pool,
                                   size_t gp_leaf_n,
                                   const BinaryGpRegister *gp_cross_pool,
                                   size_t gp_cross_n, int allow_rbp,
                                   const MirRaFacts *facts) {
  const MirVreg *vr = &fn->vregs[v];
  uint32_t m = 0;
  BinaryGpRegister cross_ext[MIR_GP_CROSSCALL_POOL_EXT];
  if (vr->rclass == MIR_RC_GP) {
    if (vr->crosses_call) {
      gp_cross_n = mir_cross_pool_for(vr, gp_cross_pool, gp_cross_n, cross_ext);
      gp_cross_pool = cross_ext;
    }
    const BinaryGpRegister *pool =
        vr->crosses_call ? gp_cross_pool : gp_leaf_pool;
    size_t n = vr->crosses_call ? gp_cross_n : gp_leaf_n;
    size_t incoming = fn->incoming_arg_slots
                          ? fn->incoming_arg_slots
                          : fn->param_count + (fn->returns_indirect ? 1 : 0);
    for (size_t i = 0; i < n; i++) {
      BinaryGpRegister reg = pool[i];
      if (fn->reserve_rbx && reg == BINARY_GP_RBX) {
        continue;
      }
      if (vr->entry_live) {
        int ai = mir_reg_arg_index(reg);
        /* An incoming argument register is barred from holding a value that
           is live at entry, because the prologue would have to shuffle the
           arguments past each other to place it. The one register that needs
           no shuffle is the one this parameter already arrives in: leaving it
           there costs nothing and spares a callee-saved register, which is a
           store in the prologue and a load at every exit on every call. */
        if (ai >= 0 && (size_t)ai < incoming &&
            !mir_vreg_is_param_in_reg(fn, v, reg)) {
          continue;
        }
      }
      if (!mir_ra_reg_busy(facts, fn, v, reg)) {
        m |= 1u << reg;
      }
    }
    if (allow_rbp && !mir_ra_reg_busy(facts, fn, v, BINARY_GP_RBP)) {
      m |= 1u << BINARY_GP_RBP;
    }
  } else if (vr->rclass == MIR_RC_XMM &&
             (!vr->crosses_call || vr->crosses_xmm_preserving_only)) {
    if (!fn->has_xmm_arg_call) {
      for (size_t i = 0; i < MIR_XMM_POOL_COUNT; i++) {
        m |= 1u << MIR_XMM_POOL[i];
      }
    }
    if (!vr->crosses_call) {
      for (size_t i = 0; i < MIR_XMM_NONVOL_POOL_COUNT; i++) {
        if (mir_xmm_is_encoder_scratch(MIR_XMM_NONVOL_POOL[i])) {
          continue;
        }
        m |= 1u << MIR_XMM_NONVOL_POOL[i];
      }
    }
  }
  return m;
}

static int mir_color_interferes(const MirVreg *a, const MirVreg *b) {
  if (a->rclass != b->rclass) {
    return 0;
  }
  if (a->entry_live && b->entry_live) {
    return 1;
  }
  return a->live_start < b->live_end && b->live_start < a->live_end;
}

static MirVregId *mir_build_narrowing_extend_map(const MirFunction *fn) {
  if (fn->vreg_count == 0) {
    return NULL;
  }
  MirVregId *map = (MirVregId *)malloc(fn->vreg_count * sizeof(MirVregId));
  if (!map) {
    return NULL;
  }
  for (size_t v = 0; v < fn->vreg_count; v++) {
    map[v] = MIR_VREG_NONE;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if ((in->op != MIR_MOVZX && in->op != MIR_MOVSX) || in->width >= 8 ||
        in->dst.kind != MIR_OPK_VREG || in->a.kind != MIR_OPK_VREG ||
        in->dst.vreg == in->a.vreg) {
      continue;
    }
    map[in->dst.vreg] = in->a.vreg;
  }
  return map;
}

static int mir_narrowing_avoid_reg(const MirFunction *fn, const MirVregId *map,
                                   MirVregId v) {
  if (!map || map[v] == MIR_VREG_NONE) {
    return -1;
  }
  const MirVreg *sv = &fn->vregs[map[v]];
  return sv->in_register ? (int)sv->phys : -1;
}

#define MIR_MAX_WEIGHTED_DEPTH 3
#define MIR_MAX_SPILL_COST (1 << 24)

static unsigned char *mir_build_loop_depths(const MirFunction *fn) {
  MirBackEdge *edges = NULL;
  size_t edge_count = 0;
  unsigned char *depth;
  if (fn->insn_count == 0) {
    return NULL;
  }
  depth = (unsigned char *)calloc(fn->insn_count, sizeof(*depth));
  if (!depth) {
    return NULL;
  }
  if (!mir_collect_back_edges(fn, &edges, &edge_count) || edge_count == 0) {
    free(edges);
    return depth;
  }
  for (size_t e = 0; e < edge_count; e++) {
    int l = edges[e].l;
    int b = edges[e].b;
    int seen = 0;
    if (l < 0 || b < l || (size_t)b >= fn->insn_count) {
      continue;
    }
    for (size_t k = 0; k < e; k++) {
      if (edges[k].l == l) {
        seen = 1;
        break;
      }
    }
    if (seen) {
      continue;
    }
    for (size_t k = e + 1; k < edge_count; k++) {
      if (edges[k].l == l && edges[k].b > b && (size_t)edges[k].b < fn->insn_count) {
        b = edges[k].b;
      }
    }
    for (int i = l; i <= b; i++) {
      if (depth[i] < MIR_MAX_WEIGHTED_DEPTH) {
        depth[i]++;
      }
    }
  }
  free(edges);
  return depth;
}

static int mir_scale_cost(int cost, int factor) {
  long long scaled = (long long)cost * factor;
  return scaled > MIR_MAX_SPILL_COST ? MIR_MAX_SPILL_COST : (int)scaled;
}

static void mir_note_operand_depth(const MirOperand *op, unsigned char depth,
                                   unsigned char *use_depth, size_t n) {
  MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};
  if (op->kind == MIR_OPK_VREG) {
    ids[0] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    ids[0] = op->mem.base;
    ids[1] = op->mem.index;
  }
  for (int j = 0; j < 2; j++) {
    if (ids[j] >= 0 && (size_t)ids[j] < n && use_depth[ids[j]] < depth) {
      use_depth[ids[j]] = depth;
    }
  }
}

static int mir_spill_rank(const MirFunction *fn, const unsigned char *use_depth,
                          MirVregId v) {
  (void)fn;
  return (int)use_depth[v];
}

const char *g_mir_ra_trace_name = NULL;

static int mir_env_no_coalesce(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_RA_NO_COALESCE") ? 1 : 0;
  }
  return cached;
}

static int mir_env_regalloc_trace(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_REGALLOC_TRACE") ? 1 : 0;
  }
  return cached;
}

static const char *mir_ra_trace_name(void) {
  return g_mir_ra_trace_name ? g_mir_ra_trace_name : "?";
}

static void mir_color_spill_costs(const MirFunction *fn,
                                  const int *colorable, int *cost,
                                  unsigned char *use_depth, size_t count,
                                  const MirRaFacts *facts) {
  unsigned char *owned_depth =
      facts && facts->valid ? NULL : mir_build_loop_depths(fn);
  const unsigned char *loop_depth =
      facts && facts->valid ? facts->cfg.insn_depth : owned_depth;

  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    const MirOperand *ops[3] = {&in->dst, &in->a, &in->b};
    unsigned char depth = loop_depth ? loop_depth[i] : 0;
    int weight = 1;

    for (int k = 0; k < depth; k++) {
      weight *= 10;
    }
    for (int k = 0; k < 3; k++) {
      const MirOperand *op = ops[k];
      MirVregId ids[2] = {MIR_VREG_NONE, MIR_VREG_NONE};

      mir_note_operand_depth(op, depth, use_depth, count);
      if (op->kind == MIR_OPK_VREG) {
        ids[0] = op->vreg;
      } else if (op->kind == MIR_OPK_MEM) {
        ids[0] = op->mem.base;
        ids[1] = op->mem.index;
      }
      for (int j = 0; j < 2; j++) {
        MirVregId id = ids[j];
        if (id >= 0 && (size_t)id < count && colorable[id]) {
          cost[id] = mir_scale_cost(cost[id] + weight, 1);
        }
      }
    }
  }
  free(owned_depth);
  for (size_t i = 0; i < fn->iconst_count; i++) {
    MirVregId v = fn->iconsts[i].vreg;
    if (v >= 0 && (size_t)v < count && colorable[v]) {
      cost[v] = mir_scale_cost(cost[v], 64);
    }
  }
  for (size_t i = 0; i < fn->fconst_count; i++) {
    MirVregId v = fn->fconsts[i].vreg;
    if (v >= 0 && (size_t)v < count && colorable[v]) {
      cost[v] = mir_scale_cost(cost[v], 32);
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LEA_FUNC && in->dst.kind == MIR_OPK_VREG) {
      MirVregId v = in->dst.vreg;
      if (v >= 0 && (size_t)v < count && colorable[v]) {
        cost[v] = mir_scale_cost(cost[v], 128);
      }
    }
  }
}

static long long mir_color_metric(const MirColorState *st, size_t v) {
  return (long long)st->cost[v] * 1000 / (st->degree[v] + 1);
}

void mir_inter_set(MirColorState *st, size_t a, size_t b) {
  MirInterRow *row;
  uint32_t word = (uint32_t)(b >> 6);
  uint32_t at;
  if (!st->rows) {
    st->inter[a * st->words + (b >> 6)] |= (uint64_t)1 << (b & 63);
    return;
  }
  row = &st->rows[a];
  at = mir_inter_row_find(row, word);
  if (at < row->count && row->idx[at] == word) {
    row->bits[at] |= (uint64_t)1 << (b & 63);
    return;
  }
  if (row->count >= row->capacity) {
    uint32_t capacity = row->capacity ? row->capacity * 2u : 4u;
    uint32_t *idx = (uint32_t *)realloc(row->idx, capacity * sizeof(uint32_t));
    uint64_t *bits;
    if (!idx) {
      st->fn->has_error = 1;
      return;
    }
    row->idx = idx;
    bits = (uint64_t *)realloc(row->bits, capacity * sizeof(uint64_t));
    if (!bits) {
      st->fn->has_error = 1;
      return;
    }
    row->bits = bits;
    row->capacity = capacity;
  }
  memmove(&row->idx[at + 1u], &row->idx[at],
          (row->count - at) * sizeof(uint32_t));
  memmove(&row->bits[at + 1u], &row->bits[at],
          (row->count - at) * sizeof(uint64_t));
  row->idx[at] = word;
  row->bits[at] = (uint64_t)1 << (b & 63);
  row->count++;
}

void mir_inter_clear(MirColorState *st, size_t a, size_t b) {
  MirInterRow *row;
  uint32_t word = (uint32_t)(b >> 6);
  uint32_t at;
  if (!st->rows) {
    st->inter[a * st->words + (b >> 6)] &= ~((uint64_t)1 << (b & 63));
    return;
  }
  row = &st->rows[a];
  at = mir_inter_row_find(row, word);
  if (at < row->count && row->idx[at] == word) {
    row->bits[at] &= ~((uint64_t)1 << (b & 63));
  }
}

void mir_inter_clear_row(MirColorState *st, size_t a) {
  if (!st->rows) {
    memset(st->inter + a * st->words, 0, st->words * sizeof(uint64_t));
    return;
  }
  st->rows[a].count = 0;
}

static size_t mir_inter_bit_count(const MirColorState *st) {
  size_t total = 0;
  if (!st->rows) {
    for (size_t w = 0; w < st->count * st->words; w++) {
      total += (size_t)__builtin_popcountll(st->inter[w]);
    }
    return total;
  }
  for (size_t a = 0; a < st->count; a++) {
    for (uint32_t w = 0; w < st->rows[a].count; w++) {
      total += (size_t)__builtin_popcountll(st->rows[a].bits[w]);
    }
  }
  return total;
}

static void mir_inter_add(MirColorState *st, size_t a, size_t b) {
  if (a == b || mir_inter_get(st, a, b)) {
    return;
  }
  mir_inter_set(st, a, b);
  mir_inter_set(st, b, a);
  st->degree[a]++;
  st->degree[b]++;
}

static void mir_color_state_free(MirColorState *st) {
  free(st->inter);
  if (st->rows) {
    for (size_t a = 0; a < st->count; a++) {
      free(st->rows[a].idx);
      free(st->rows[a].bits);
    }
  }
  free(st->rows);
  free(st->mask);
  free(st->degree);
  free(st->cost);
  free(st->colorable);
  free(st->removed);
  free(st->reg_count);
  free(st->metric);
  free(st->stack);
  free(st->narrow_src);
  free(st->use_depth);
  free(st->rep);
  free(st->phys_hint);
  free(st->avoid_shift_count);
  free(st->copy_partner);
}

static int mir_color_state_init(MirColorState *st, MirFunction *fn,
                                const BinaryGpRegister *gp_leaf_pool,
                                size_t gp_leaf_n,
                                const BinaryGpRegister *gp_cross_pool,
                                size_t gp_cross_n, int allow_rbp,
                                const MirRaFacts *facts) {
  size_t n = fn->vreg_count;

  memset(st, 0, sizeof(*st));
  st->fn = fn;
  st->facts = facts;
  st->count = n;
  st->words = (n + 63) / 64;
  if (n * st->words * sizeof(uint64_t) > MIR_INTER_DENSE_MAX_BYTES) {
    st->rows = (MirInterRow *)calloc(n ? n : 1, sizeof(MirInterRow));
  } else {
    st->inter = (uint64_t *)calloc(n * st->words, sizeof(uint64_t));
  }
  st->mask = (uint32_t *)calloc(n, sizeof(uint32_t));
  st->degree = (int *)calloc(n, sizeof(int));
  st->cost = (int *)calloc(n, sizeof(int));
  st->colorable = (int *)calloc(n, sizeof(int));
  st->removed = (int *)calloc(n, sizeof(int));
  st->reg_count = (int *)calloc(n, sizeof(int));
  st->metric = (long long *)calloc(n, sizeof(long long));
  st->stack = (MirVregId *)malloc(n * sizeof(MirVregId));
  st->use_depth = (unsigned char *)calloc(n, sizeof(unsigned char));
  st->narrow_src = mir_build_narrowing_extend_map(fn);
  st->rep = (MirVregId *)malloc(n * sizeof(MirVregId));
  st->phys_hint = (int *)malloc(n * sizeof(int));
  st->avoid_shift_count = (unsigned char *)calloc(n, sizeof(unsigned char));
  st->copy_partner = (MirVregId *)malloc(n * sizeof(MirVregId));
  if ((!st->inter && !st->rows) || !st->mask || !st->degree || !st->cost ||
      !st->colorable ||
      !st->removed || !st->reg_count || !st->metric || !st->stack ||
      !st->use_depth || !st->rep || !st->phys_hint || !st->copy_partner ||
      !st->avoid_shift_count) {
    mir_color_state_free(st);
    return 0;
  }
  for (size_t v = 0; v < n; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (vr->live_start == MIR_LIVE_NONE || vr->address_taken) {
      continue;
    }
    st->colorable[v] = 1;
    st->mask[v] = mir_color_reg_mask(fn, (MirVregId)v, gp_leaf_pool, gp_leaf_n,
                                     gp_cross_pool, gp_cross_n, allow_rbp,
                                     facts);
    st->reg_count[v] = __builtin_popcount(st->mask[v]);
    st->cost[v] = 1;
  }
  mir_color_spill_costs(fn, st->colorable, st->cost, st->use_depth, n, facts);
  return 1;
}

static int mir_color_same_class(const MirColorState *st, size_t a, size_t b) {
  return st->fn->vregs[a].rclass == st->fn->vregs[b].rclass;
}

static void mir_color_add_live_edges(MirColorState *st, size_t d,
                                     const unsigned long long *live) {
  for (size_t w = 0; w < st->words; w++) {
    uint64_t bits = live[w];
    while (bits) {
      size_t v = w * 64 + (size_t)__builtin_ctzll(bits);
      bits &= bits - 1;
      if (v < st->count && st->colorable[v] && mir_color_same_class(st, d, v)) {
        mir_inter_add(st, d, v);
      }
    }
  }
}

static void mir_color_add_interval_edges(MirColorState *st) {
  for (size_t a = 0; a < st->count; a++) {
    if (!st->colorable[a]) {
      continue;
    }
    for (size_t b = a + 1; b < st->count; b++) {
      if (st->colorable[b] &&
          mir_color_interferes(&st->fn->vregs[a], &st->fn->vregs[b])) {
        mir_inter_add(st, a, b);
      }
    }
  }
}

static void mir_color_add_exact_edges(MirColorState *st) {
  const MirRaFacts *facts = st->facts;
  MirFunction *fn = st->fn;
  MirCfgCursor cur;
  unsigned long long *both;
  if (!mir_cfg_cursor_init(&cur, &facts->cfg)) {
    mir_color_add_interval_edges(st);
    return;
  }
  both = (unsigned long long *)malloc(st->words * sizeof(*both));
  if (!both) {
    mir_cfg_cursor_free(&cur);
    mir_color_add_interval_edges(st);
    return;
  }
  for (size_t b = 0; b < facts->cfg.block_count; b++) {
    const MirCfgBlock *blk = &facts->cfg.blocks[b];
    mir_cfg_cursor_start_block(&cur, b);
    for (int at = blk->end; at > blk->start; at--) {
      MirVregId d = mir_cfg_insn_def(&fn->insns[at - 1]);
      if (d >= 0 && (size_t)d < st->count && st->colorable[d]) {
        for (size_t w = 0; w < st->words; w++) {
          both[w] = cur.live[w] & cur.defd[w];
        }
        mir_color_add_live_edges(st, (size_t)d, both);
      }
      mir_cfg_cursor_step_back(&cur);
    }
  }
  mir_cfg_entry_live_defined(&facts->cfg, both, st->words);
  for (size_t a = 0; a < st->count; a++) {
    if (st->colorable[a] && mir_cfg_set_get(both, a)) {
      mir_color_add_live_edges(st, a, both);
    }
  }
  free(both);
  mir_cfg_cursor_free(&cur);
}

static void mir_color_build_interference(MirColorState *st) {
  MirFunction *fn = st->fn;
  int exact = st->facts && st->facts->valid;

  if (mir_env_regalloc_trace()) {
    fprintf(stderr,
            "RA\t%s\tvregs=%zu\tinsns=%zu\tblocks=%zu\tcfg=%d\tmaxlive_gp=%d"
            "\tmaxlive_xmm=%d\tmaxlive_at=%zu\n",
            mir_ra_trace_name(), st->count, fn->insn_count,
            exact ? st->facts->cfg.block_count : (size_t)0, exact,
            exact ? st->facts->max_live_gp : -1,
            exact ? st->facts->max_live_xmm : -1,
            exact ? st->facts->max_live_at : (size_t)0);
  }
  if (exact) {
    mir_color_add_exact_edges(st);
    return;
  }
  mir_color_add_interval_edges(st);
}

static void mir_color_add_narrowing_edges(MirColorState *st) {
  if (!st->narrow_src) {
    return;
  }
  for (size_t v = 0; v < st->count; v++) {
    MirVregId s = st->narrow_src[v];
    if (!st->colorable[v] || s == MIR_VREG_NONE || (size_t)s >= st->count ||
        !st->colorable[s]) {
      continue;
    }
    mir_inter_add(st, v, (size_t)s);
  }
}

static MirVregId mir_color_pick(const MirColorState *st, int low_degree_only) {
  MirVregId pick = MIR_VREG_NONE;
  long long best = -1;
  int best_rank = MIR_MAX_WEIGHTED_DEPTH + 1;

  for (size_t v = 0; v < st->count; v++) {
    int rank;
    int better;

    if (!st->colorable[v] || st->removed[v]) {
      continue;
    }
    if (low_degree_only && st->degree[v] >= st->reg_count[v]) {
      continue;
    }
    rank = mir_spill_rank(st->fn, st->use_depth, (MirVregId)v);
    if (pick == MIR_VREG_NONE) {
      better = 1;
    } else if (rank != best_rank) {
      better = (rank < best_rank);
    } else {
      better = (st->metric[v] < best);
    }
    if (better) {
      best = st->metric[v];
      best_rank = rank;
      pick = (MirVregId)v;
    }
  }
  return pick;
}

typedef struct {
  int rank;
  long long metric;
  MirVregId v;
} MirPickEntry;

typedef struct {
  MirPickEntry *items;
  size_t count;
  size_t capacity;
} MirPickHeap;

static int mir_pick_before(const MirPickEntry *a, const MirPickEntry *b) {
  if (a->rank != b->rank) {
    return a->rank < b->rank;
  }
  if (a->metric != b->metric) {
    return a->metric < b->metric;
  }
  return a->v < b->v;
}

static void mir_pick_push(MirPickHeap *h, const MirColorState *st, size_t v) {
  size_t i = h->count++;
  h->items[i].rank = mir_spill_rank(st->fn, st->use_depth, (MirVregId)v);
  h->items[i].metric = st->metric[v];
  h->items[i].v = (MirVregId)v;
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (!mir_pick_before(&h->items[i], &h->items[parent])) {
      break;
    }
    MirPickEntry t = h->items[i];
    h->items[i] = h->items[parent];
    h->items[parent] = t;
    i = parent;
  }
}

static void mir_pick_pop(MirPickHeap *h) {
  size_t i = 0;
  h->items[0] = h->items[--h->count];
  for (;;) {
    size_t l = 2 * i + 1;
    size_t r = l + 1;
    size_t m = i;
    if (l < h->count && mir_pick_before(&h->items[l], &h->items[m])) {
      m = l;
    }
    if (r < h->count && mir_pick_before(&h->items[r], &h->items[m])) {
      m = r;
    }
    if (m == i) {
      break;
    }
    MirPickEntry t = h->items[i];
    h->items[i] = h->items[m];
    h->items[m] = t;
    i = m;
  }
}

static MirVregId mir_pick_top(MirPickHeap *h, const MirColorState *st,
                              int low_degree_only) {
  while (h->count > 0) {
    const MirPickEntry *top = &h->items[0];
    size_t v = top->v;
    if (!st->removed[v] && top->metric == st->metric[v] &&
        (!low_degree_only || st->degree[v] < st->reg_count[v])) {
      return top->v;
    }
    mir_pick_pop(h);
  }
  return MIR_VREG_NONE;
}

static int mir_color_order_vregs_heap(MirColorState *st, size_t remaining,
                                      size_t *sp_out) {
  MirPickHeap low = {NULL, 0, 0};
  MirPickHeap all = {NULL, 0, 0};
  size_t sp = 0;
  size_t capacity = remaining + 1;

  capacity += mir_inter_bit_count(st);
  if (capacity - remaining - 1 > remaining * remaining / 32) {
    return 0;
  }
  low.items = (MirPickEntry *)malloc(capacity * sizeof(MirPickEntry));
  all.items = (MirPickEntry *)malloc(capacity * sizeof(MirPickEntry));
  if (!low.items || !all.items) {
    free(low.items);
    free(all.items);
    return 0;
  }
  low.capacity = capacity;
  all.capacity = capacity;
  for (size_t v = 0; v < st->count; v++) {
    if (!st->colorable[v]) {
      continue;
    }
    mir_pick_push(&all, st, v);
    if (st->degree[v] < st->reg_count[v]) {
      mir_pick_push(&low, st, v);
    }
  }
  while (remaining > 0) {
    MirVregId pick = mir_pick_top(&low, st, 1);

    if (pick == MIR_VREG_NONE) {
      pick = mir_pick_top(&all, st, 0);
    }
    if (pick == MIR_VREG_NONE) {
      break;
    }
    st->removed[pick] = 1;
    st->stack[sp++] = pick;
    remaining--;
    MIR_INTER_FOR_EACH(st, pick, b) {
      if (!st->removed[b]) {
        st->degree[b]--;
        st->metric[b] = mir_color_metric(st, b);
        if (!st->colorable[b]) {
          continue;
        }
        mir_pick_push(&all, st, b);
        if (st->degree[b] < st->reg_count[b]) {
          mir_pick_push(&low, st, b);
        }
      }
    }
  }
  free(low.items);
  free(all.items);
  *sp_out = sp;
  return 1;
}

static size_t mir_color_order_vregs(MirColorState *st) {
  size_t sp = 0;
  size_t remaining = 0;

  for (size_t v = 0; v < st->count; v++) {
    if (st->colorable[v]) {
      remaining++;
      st->metric[v] = mir_color_metric(st, v);
    }
  }
  if (mir_color_order_vregs_heap(st, remaining, &sp)) {
    return sp;
  }
  while (remaining > 0) {
    MirVregId pick = mir_color_pick(st, 1);

    if (pick == MIR_VREG_NONE) {
      pick = mir_color_pick(st, 0);
    }
    if (pick == MIR_VREG_NONE) {
      break;
    }
    st->removed[pick] = 1;
    st->stack[sp++] = pick;
    remaining--;
    MIR_INTER_FOR_EACH(st, pick, b) {
      if (!st->removed[b]) {
        st->degree[b]--;
        st->metric[b] = mir_color_metric(st, b);
      }
    }
  }
  return sp;
}

static uint32_t mir_color_neighbour_mask(const MirColorState *st, size_t v) {
  uint32_t used = 0;

  MIR_INTER_FOR_EACH(st, v, b) {
    if (st->fn->vregs[b].in_register) {
      used |= 1u << st->fn->vregs[b].phys;
    }
  }
  return used;
}

static int mir_color_reg_is_saved(const MirVreg *vr, int r) {
  if (vr->rclass == MIR_RC_XMM) {
    return r >= 8;
  }
  return mir_gp_is_nonvolatile((BinaryGpRegister)r) || r == BINARY_GP_RBP;
}

static int mir_color_choose_reg(const MirColorState *st, MirVregId v,
                                uint32_t avail) {
  const MirVreg *vr = &st->fn->vregs[v];
  uint32_t preferred = avail;
  int avoid = mir_narrowing_avoid_reg(st->fn, st->narrow_src, v);

  if (avoid >= 0 && (preferred & ~(1u << (unsigned)avoid)) != 0) {
    preferred &= ~(1u << (unsigned)avoid);
  }
  if (st->avoid_shift_count[v] &&
      (preferred & ~(1u << (unsigned)BINARY_GP_RCX)) != 0) {
    preferred &= ~(1u << (unsigned)BINARY_GP_RCX);
  }
  if (vr->coalesce_hint != MIR_VREG_NONE) {
    const MirVreg *hv = &st->fn->vregs[vr->coalesce_hint];
    if (hv->in_register && (preferred & (1u << hv->phys))) {
      return hv->phys;
    }
  }
  if (st->phys_hint[v] >= 0 && (preferred & (1u << (unsigned)st->phys_hint[v]))) {
    return st->phys_hint[v];
  }
  if (st->copy_partner[v] != MIR_VREG_NONE) {
    const MirVreg *pv = &st->fn->vregs[st->copy_partner[v]];
    if (pv->in_register && pv->rclass == vr->rclass &&
        (preferred & (1u << pv->phys))) {
      return pv->phys;
    }
  }
  for (int saved = 0; saved < 2; saved++) {
    for (int r = 0; r < 16; r++) {
      if ((preferred & (1u << r)) &&
          mir_color_reg_is_saved(vr, r) == saved) {
        return r;
      }
    }
  }
  return -1;
}

static void mir_color_assign(MirColorState *st, size_t sp, int *next_spill) {
  while (sp > 0) {
    MirVregId v = st->stack[--sp];
    MirVreg *vr = &st->fn->vregs[v];
    uint32_t avail = st->mask[v] & ~mir_color_neighbour_mask(st, (size_t)v);

    if (avail == 0) {
      *next_spill += vr->width > 8 ? 16 : 8;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = *next_spill;
      continue;
    }
    vr->assigned = 1;
    vr->in_register = 1;
    vr->phys = mir_color_choose_reg(st, v, avail);
  }
}

static int mir_color_move_is_coalescable(const MirColorState *st,
                                         const MirInst *in, size_t at,
                                         MirVregId *out_d, MirVregId *out_s) {
  MirVregId d;
  MirVregId s;
  const MirVreg *dv;
  const MirVreg *sv;

  if (in->op != MIR_MOV || in->dst.kind != MIR_OPK_VREG ||
      in->a.kind != MIR_OPK_VREG) {
    return 0;
  }
  d = in->dst.vreg;
  s = in->a.vreg;
  if (d < 0 || s < 0 || (size_t)d >= st->count || (size_t)s >= st->count ||
      d == s || !st->colorable[d] || !st->colorable[s]) {
    return 0;
  }
  dv = &st->fn->vregs[d];
  sv = &st->fn->vregs[s];
  if (!dv->in_register || !sv->in_register || dv->rclass != sv->rclass ||
      dv->phys == sv->phys || !mir_ra_operand_dies(st->facts, st->fn, at, 0) ||
      mir_inter_get(st, (size_t)d, (size_t)s) ||
      !(st->mask[d] & (1u << sv->phys))) {
    return 0;
  }
  if (mir_color_neighbour_mask(st, (size_t)d) & (1u << sv->phys)) {
    return 0;
  }
  *out_d = d;
  *out_s = s;
  return 1;
}

static void mir_color_coalesce_moves(MirColorState *st) {
  int coalesced = 1;
  int rounds = 0;

  while (coalesced && rounds++ < 16) {
    coalesced = 0;
    for (size_t i = 0; i < st->fn->insn_count; i++) {
      MirVregId d;
      MirVregId s;

      if (!mir_color_move_is_coalescable(st, &st->fn->insns[i], i, &d, &s)) {
        continue;
      }
      st->fn->vregs[d].phys = st->fn->vregs[s].phys;
      coalesced = 1;
    }
  }
}

static void mir_color_check_merged_graph(const MirColorState *st,
                                         const BinaryGpRegister *gp_leaf_pool,
                                         size_t gp_leaf_n,
                                         const BinaryGpRegister *gp_cross_pool,
                                         size_t gp_cross_n, int allow_rbp) {
  MirRaFacts fresh;
  MirColorState again;
  size_t missing = 0;
  if (!mir_ra_facts_build(&fresh, st->fn)) {
    return;
  }
  if (!mir_color_state_init(&again, st->fn, gp_leaf_pool, gp_leaf_n,
                            gp_cross_pool, gp_cross_n, allow_rbp, &fresh)) {
    mir_ra_facts_free(&fresh);
    return;
  }
  mir_color_build_interference(&again);
  if (mir_env_regalloc_trace()) {
    fprintf(stderr, "RA-COALESCE-CHECK\t%s\tmaxlive_gp=%d\tmaxlive_xmm=%d\n",
            mir_ra_trace_name(), fresh.max_live_gp, fresh.max_live_xmm);
  }
  for (size_t a = 0; a < st->count; a++) {
    if (!st->colorable[a]) {
      continue;
    }
    MIR_INTER_FOR_EACH(&again, a, b) {
      if (b > a && st->colorable[b] && !mir_inter_get(st, a, b)) {
        fprintf(stderr, "RA-COALESCE-CHECK\t%s\tmissing edge v%zu v%zu\n",
                mir_ra_trace_name(), a, b);
        missing++;
      }
    }
  }
  if (missing) {
    st->fn->has_error = 1;
    if (st->fn->generator && !st->fn->generator->has_error) {
      code_generator_set_error(st->fn->generator,
                               "coalescing check: %zu interference edges lost "
                               "by merging in function '%s'",
                               missing, mir_ra_trace_name());
    }
  }
  mir_color_state_free(&again);
  mir_ra_facts_free(&fresh);
}

static MirExternalAllocator g_mir_external_allocator = NULL;
static const char *g_mir_external_allocator_name = NULL;

void mir_set_external_allocator(MirExternalAllocator allocator,
                                const char *name) {
  g_mir_external_allocator = allocator;
  g_mir_external_allocator_name = name;
}

const char *mir_external_allocator_name(void) {
  return g_mir_external_allocator ? g_mir_external_allocator_name : NULL;
}

static int mir_color_ask_external(MirColorState *st, int *next_spill) {
  MirFunction *fn = st->fn;
  size_t count = 0;
  int *index = (int *)malloc((st->count + 1) * sizeof(int));
  if (!index) {
    return 0;
  }
  for (size_t v = 0; v < st->count; v++) {
    index[v] = st->colorable[v] ? (int)count++ : -1;
  }
  MirAllocVreg *vregs = (MirAllocVreg *)calloc(count + 1, sizeof(MirAllocVreg));
  int *registers = (int *)calloc(count + 1, sizeof(int));
  size_t total_edges = 0;
  for (size_t v = 0; v < st->count; v++) {
    if (st->colorable[v]) {
      MIR_INTER_FOR_EACH(st, v, b) {
        if (st->colorable[b]) {
          total_edges++;
        }
      }
    }
  }
  int *edges = (int *)malloc((total_edges + 1) * sizeof(int));
  if (!vregs || !registers || !edges) {
    free(index);
    free(vregs);
    free(registers);
    free(edges);
    return 0;
  }
  size_t edge_at = 0;
  for (size_t v = 0; v < st->count; v++) {
    if (!st->colorable[v]) {
      continue;
    }
    MirAllocVreg *out = &vregs[index[v]];
    const MirVreg *vr = &fn->vregs[v];
    out->id = (int)v;
    out->rclass = (int)vr->rclass;
    out->width = vr->width;
    out->allowed = st->mask[v];
    out->cost = st->cost[v];
    out->hint = st->phys_hint[v];
    for (int r = 0; r < 16; r++) {
      if (mir_color_reg_is_saved(vr, r)) {
        out->saved |= 1u << r;
      }
    }
    out->neighbors = edges + edge_at;
    MIR_INTER_FOR_EACH(st, v, b) {
      if (st->colorable[b]) {
        edges[edge_at++] = index[b];
        out->neighbor_count++;
      }
    }
  }
  MirAllocRequest request;
  char why[400];
  const char *function_name = mir_ra_trace_name();
  why[0] = '\0';
  request.function = function_name;
  request.vregs = vregs;
  request.count = count;
  int ok = g_mir_external_allocator(&request, registers, why, sizeof(why));
  if (!ok) {
    if (fn->generator && !fn->generator->has_error) {
      code_generator_set_error(fn->generator, "'@%s' could not allocate '%s': %s",
                               g_mir_external_allocator_name, function_name, why);
      fn->generator->has_user_error = 1;
    }
    fn->has_error = 1;
  }
  for (size_t v = 0; ok && v < st->count; v++) {
    if (!st->colorable[v]) {
      continue;
    }
    MirVreg *vr = &fn->vregs[v];
    int reg = registers[index[v]];
    if (reg >= 0 && (reg > 31 || !(st->mask[v] & (1u << (unsigned)reg)))) {
      if (fn->generator && !fn->generator->has_error) {
        code_generator_set_error(
            fn->generator,
            "'@%s' gave v%d of '%s' register %d, and the allowed set is 0x%x",
            g_mir_external_allocator_name, (int)v, function_name, reg,
            (unsigned)st->mask[v]);
        fn->generator->has_user_error = 1;
      }
      fn->has_error = 1;
      ok = 0;
      break;
    }
    vr->assigned = 1;
    if (reg < 0) {
      *next_spill += vr->width > 8 ? 16 : 8;
      vr->in_register = 0;
      vr->spill_offset = *next_spill;
    } else {
      vr->in_register = 1;
      vr->phys = reg;
    }
  }
  free(index);
  free(vregs);
  free(registers);
  free(edges);
  return ok;
}

static int mir_color_graph(MirFunction *fn, const BinaryGpRegister *gp_leaf_pool,
                           size_t gp_leaf_n,
                           const BinaryGpRegister *gp_cross_pool,
                           size_t gp_cross_n, int *next_spill, int allow_rbp,
                           const MirRaFacts *facts) {
  MirColorState st;

  if (fn->vreg_count == 0) {
    return 1;
  }
  if (!mir_color_state_init(&st, fn, gp_leaf_pool, gp_leaf_n, gp_cross_pool,
                            gp_cross_n, allow_rbp, facts)) {
    return 0;
  }
  mir_color_build_interference(&st);
  mir_color_add_narrowing_edges(&st);
  for (size_t v = 0; v < st.count; v++) {
    st.rep[v] = (MirVregId)v;
  }
  if (!mir_env_no_coalesce() && !mir_color_coalesce(&st)) {
    mir_color_state_free(&st);
    return 0;
  }
  if (getenv("METTLE_RA_COALESCE_CHECK")) {
    mir_color_check_merged_graph(&st, gp_leaf_pool, gp_leaf_n, gp_cross_pool,
                                 gp_cross_n, allow_rbp);
  }
  mir_color_note_phys_hints(&st);
  if (g_mir_external_allocator) {
    if (!mir_color_ask_external(&st, next_spill)) {
      mir_color_state_free(&st);
      return 0;
    }
  } else {
    mir_color_assign(&st, mir_color_order_vregs(&st), next_spill);
    mir_color_coalesce_moves(&st);
  }
  mir_color_mirror_members(&st);
  mir_color_state_free(&st);
  return 1;
}

static int mir_regalloc_report_saved(MirFunction *fn) {
  if (!fn->context) {
    return 1;
  }
  int used_nonvol[16];
  memset(used_nonvol, 0, sizeof(used_nonvol));
  for (size_t i = 0; i < fn->vreg_count; i++) {
    MirVreg *vr = &fn->vregs[i];
    if (vr->in_register && vr->rclass == MIR_RC_GP &&
        (mir_gp_is_nonvolatile((BinaryGpRegister)vr->phys) ||
         vr->phys == BINARY_GP_RBP)) {
      used_nonvol[vr->phys] = 1;
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op == MIR_INLINE_ASM) {
      for (int reg = 0; reg < 16; reg++) {
        if (mir_gp_is_nonvolatile((BinaryGpRegister)reg)) {
          used_nonvol[reg] = 1;
        }
      }
      continue;
    }
    if (fn->insns[i].op != MIR_IR_KERNEL) {
      continue;
    }
    const MirKernelAux *ka = (const MirKernelAux *)fn->insns[i].aux;
    const MirIrKernel *kern = ka ? mir_ir_kernel_at(ka->kernel_index) : NULL;
    unsigned clobbers = kern ? kern->gp_clobbers : 0u;
    for (int reg = 0; clobbers; reg++, clobbers >>= 1) {
      if ((clobbers & 1u) && (mir_gp_is_nonvolatile((BinaryGpRegister)reg) ||
                              reg == BINARY_GP_RBP)) {
        used_nonvol[reg] = 1;
      }
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

static void mir_regalloc_trace_done(const MirFunction *fn) {
  size_t spilled = 0;
  size_t kept = 0;
  size_t copies = 0;
  size_t coalesced = 0;
  size_t spill_side = 0;
  if (!mir_env_regalloc_trace()) {
    return;
  }
  for (size_t v = 0; v < fn->vreg_count; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (vr->live_start == MIR_LIVE_NONE || vr->address_taken ||
        vr->coalesced_into != MIR_VREG_NONE) {
      continue;
    }
    if (vr->assigned && vr->in_register) {
      kept++;
    } else if (vr->assigned) {
      spilled++;
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    const MirVreg *dv;
    const MirVreg *sv;
    if (in->op != MIR_MOV || in->dst.kind != MIR_OPK_VREG ||
        in->a.kind != MIR_OPK_VREG || in->dst.vreg == in->a.vreg) {
      continue;
    }
    dv = &fn->vregs[in->dst.vreg];
    sv = &fn->vregs[in->a.vreg];
    copies++;
    if (!dv->in_register || !sv->in_register) {
      spill_side++;
    } else if (dv->phys == sv->phys) {
      coalesced++;
    }
  }
  fprintf(stderr,
          "RA-DONE\t%s\tkept=%zu\tspilled=%zu\tcopies=%zu\tcoalesced=%zu"
          "\tspill_side=%zu\tmerged=%zu\n",
          mir_ra_trace_name(), kept, spilled, copies + fn->merged_copies,
          coalesced + fn->merged_copies, spill_side, fn->merged_copies);
}

static int mir_regalloc_finish(MirFunction *fn) {
  if (mir_regalloc_verify_sabotage_enabled()) {
    mir_regalloc_verify_sabotage(fn);
  }
  if ((mir_regalloc_verify_enabled() || g_mir_external_allocator) &&
      !mir_regalloc_verify(fn)) {
    fn->has_error = 1;
    return 0;
  }
  return 1;
}

static int mir_regalloc_color(MirFunction *fn) {
  MirRaFacts facts;
  mir_compute_liveness(fn, 0);
  mir_ra_facts_build(&facts, fn);
  if (facts.valid &&
      facts.max_live_gp + facts.max_live_xmm > MIR_GRAPH_MAX_LIVE) {
    if (mir_env_regalloc_trace()) {
      fprintf(stderr, "RA-LINEAR\t%s\tmaxlive_gp=%d\tmaxlive_xmm=%d\n",
              mir_ra_trace_name(), facts.max_live_gp, facts.max_live_xmm);
    }
    mir_ra_facts_free(&facts);
    return -1;
  }
  mir_compute_coalesce_hints(fn, &facts);
  mir_mark_crosses_call(fn, &facts);

  int next_spill = fn->context ? fn->context->raw_frame_size : 0;
  fn->preserve_slot = 0;
  fn->preserve_xmm_slot = 0;
  if (mir_fn_has_preserving_call(fn, 0)) {
    next_spill += 8;
    fn->preserve_slot = next_spill;
  }
  if (mir_fn_has_preserving_call(fn, 1)) {
    next_spill += (int)MIR_XMM_POOL_COUNT * 8;
    fn->preserve_xmm_slot = next_spill;
  }
  for (size_t v = 0; v < fn->vreg_count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->address_taken) {
      int home = mir_home_bytes_for(vr, &next_spill);
      next_spill += home;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = next_spill;
    }
  }

  BinaryGpRegister gp_leaf_pool[MIR_GP_LEAF_POOL_MAX];
  size_t gp_leaf_n = mir_build_gp_leaf_pool(
      gp_leaf_pool, fn->param_count + (fn->returns_indirect ? 1 : 0),
      !mir_fn_has_real_calls(fn));
  BinaryGpRegister gp_cross_pool[MIR_GP_CROSSCALL_POOL_MAX];
  size_t gp_cross_n = mir_build_gp_crosscall_pool(gp_cross_pool);

  int allow_rbp = fn->context && fn->context->omit_frame_pointer &&
                  !mir_fn_uses_slp(fn);
  if (!mir_color_graph(fn, gp_leaf_pool, gp_leaf_n, gp_cross_pool, gp_cross_n,
                       &next_spill, allow_rbp, &facts)) {
    mir_ra_facts_free(&facts);
    fn->has_error = 1;
    return 0;
  }
  mir_ra_facts_free(&facts);
  mir_drop_unused_preserves(fn);
  mir_regalloc_trace_done(fn);
  fn->spill_bytes = next_spill - (fn->context ? fn->context->raw_frame_size : 0);
  if (!mir_regalloc_report_saved(fn)) {
    fn->has_error = 1;
    return 0;
  }
  return mir_regalloc_finish(fn);
}

static int mir_op_pure_def(MirOpcode op) {
  return mir_op_has(op, MIR_OPF_PURE_DEF);
}

typedef struct {
  const MirFunction *fn;
  int *def_head;
  int *def_next;
  unsigned char *marked;
  unsigned char *defs_marked;
  size_t *work;
  size_t work_count;
} MirDce;

static void mir_dce_mark(MirDce *d, size_t i) {
  if (d->marked[i]) {
    return;
  }
  d->marked[i] = 1;
  d->work[d->work_count++] = i;
}

static int mir_dce_is_root(const MirFunction *fn, const MirInst *in) {
  if (!mir_op_pure_def(in->op) || in->dst.kind != MIR_OPK_VREG) {
    return 1;
  }
  if (in->op == MIR_MOV && in->a.kind == MIR_OPK_MEM) {
    return 1;
  }
  if (in->dst.vreg < 0 || (size_t)in->dst.vreg >= fn->vreg_count) {
    return 1;
  }
  return in->dst.vreg == fn->indirect_return_vreg ||
         fn->vregs[in->dst.vreg].address_taken;
}

static void mir_dce_mark_defs_of(MirDce *d, MirVregId v) {
  if (v < 0 || (size_t)v >= d->fn->vreg_count || d->defs_marked[v]) {
    return;
  }
  d->defs_marked[v] = 1;
  for (int j = d->def_head[v]; j >= 0; j = d->def_next[j]) {
    mir_dce_mark(d, (size_t)j);
  }
}

static void mir_dce_mark_uses(MirDce *d, const MirInst *in) {
  MirVregId ids[6];
  int n = mir_cfg_insn_uses(in, ids);
  const MirOperand *ops[3] = {&in->dst, &in->a, &in->b};
  for (int k = 0; k < n; k++) {
    mir_dce_mark_defs_of(d, ids[k]);
  }
  for (int k = 0; k < 3; k++) {
    if (ops[k]->kind == MIR_OPK_MEM && ops[k]->mem.frame_home_valid) {
      mir_dce_mark_defs_of(d, ops[k]->mem.frame_home);
    }
  }
}

static int mir_dce_init(MirDce *d, const MirFunction *fn) {
  memset(d, 0, sizeof(*d));
  d->fn = fn;
  d->def_head = (int *)malloc(fn->vreg_count * sizeof(int));
  d->def_next = (int *)malloc(fn->insn_count * sizeof(int));
  d->marked = (unsigned char *)calloc(fn->insn_count, 1);
  d->defs_marked = (unsigned char *)calloc(fn->vreg_count, 1);
  d->work = (size_t *)malloc(fn->insn_count * sizeof(size_t));
  if (!d->def_head || !d->def_next || !d->marked || !d->defs_marked ||
      !d->work) {
    return 0;
  }
  for (size_t v = 0; v < fn->vreg_count; v++) {
    d->def_head[v] = -1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirVregId v = mir_cfg_insn_def(&fn->insns[i]);
    d->def_next[i] = -1;
    if (v >= 0 && (size_t)v < fn->vreg_count) {
      d->def_next[i] = d->def_head[v];
      d->def_head[v] = (int)i;
    }
  }
  return 1;
}

static void mir_dce_free(MirDce *d) {
  free(d->def_head);
  free(d->def_next);
  free(d->marked);
  free(d->defs_marked);
  free(d->work);
}

static void mir_dce(MirFunction *fn) {
  MirDce d;
  if (fn->vreg_count == 0 || fn->insn_count == 0 || !mir_dce_init(&d, fn)) {
    mir_dce_free(&d);
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op != MIR_NOP && mir_dce_is_root(fn, in)) {
      mir_dce_mark(&d, i);
    }
  }
  while (d.work_count > 0) {
    size_t i = d.work[--d.work_count];
    mir_dce_mark_uses(&d, &fn->insns[i]);
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op != MIR_NOP && !d.marked[i]) {
      fn->insns[i].op = MIR_NOP;
    }
  }
  mir_dce_free(&d);
}

int mir_regalloc(MirFunction *fn) {
  if (!fn) {
    return 0;
  }
  if (fn->vreg_count == 0) {
    return 1;
  }

  mir_clobber_index_reset();
  fn->merged_copies = 0;

  mir_dce(fn);

  if (fn->context && fn->context->omit_frame_pointer && mir_fn_has_calls(fn)) {
    fn->context->omit_frame_pointer = 0;
  }

  {
    static int linear = -1;
    if (linear < 0) {
      linear = getenv("METTLE_LINEAR_ALLOC") ? 1 : 0;
    }
    if (!linear) {
      int colored = mir_regalloc_color(fn);
      if (colored >= 0) {
        return colored;
      }
    }
  }

  mir_compute_liveness(fn, 1);
  mir_compute_coalesce_hints(fn, NULL);

  mir_mark_crosses_call(fn, NULL);

  size_t order_count = 0;
  MirVregId *order = mir_order_by_start(fn, &order_count);
  if (fn->has_error) {
    free(order);
    return 0;
  }
  MirVregId *narrow_src = mir_build_narrowing_extend_map(fn);

  int gp_held_by[16];
  int xmm_held_by[16];
  for (int i = 0; i < 16; i++) {
    gp_held_by[i] = -1;
    xmm_held_by[i] = -1;
  }
  xmm_held_by[mir_xmm_scratch_a()] = -2;
  xmm_held_by[mir_xmm_scratch_b()] = -2;
  BinaryGpRegister gp_leaf_pool[MIR_GP_LEAF_POOL_MAX];
  size_t gp_leaf_pool_count = mir_build_gp_leaf_pool(
      gp_leaf_pool, fn->param_count + (fn->returns_indirect ? 1 : 0),
      !mir_fn_has_real_calls(fn));
  BinaryGpRegister gp_cross_pool[MIR_GP_CROSSCALL_POOL_MAX];
  size_t gp_cross_pool_count = mir_build_gp_crosscall_pool(gp_cross_pool);
  for (int r = 0; r < 16; r++) {
    gp_held_by[r] = -2;
  }
  for (size_t i = 0; i < gp_leaf_pool_count; i++) {
    gp_held_by[gp_leaf_pool[i]] = -1;
  }

  int next_spill_offset = fn->context ? fn->context->raw_frame_size : 0;
  fn->preserve_slot = 0;
  fn->preserve_xmm_slot = 0;
  if (mir_fn_has_preserving_call(fn, 0)) {
    next_spill_offset += 8;
    fn->preserve_slot = next_spill_offset;
  }
  if (mir_fn_has_preserving_call(fn, 1)) {
    next_spill_offset += (int)MIR_XMM_POOL_COUNT * 8;
    fn->preserve_xmm_slot = next_spill_offset;
  }

  for (size_t v = 0; v < fn->vreg_count; v++) {
    MirVreg *vr = &fn->vregs[v];
    if (vr->address_taken) {
      int home = mir_home_bytes_for(vr, &next_spill_offset);
      next_spill_offset += home;
      vr->assigned = 1;
      vr->in_register = 0;
      vr->spill_offset = next_spill_offset;
    }
  }

  MirVregId *active = (MirVregId *)malloc(order_count * sizeof(MirVregId));
  if (!active && order_count > 0) {
    free(order);
    free(narrow_src);
    fn->has_error = 1;
    return 0;
  }
  size_t active_count = 0;

  for (size_t oi = 0; oi < order_count; oi++) {
    MirVregId cur = order[oi];
    MirVreg *cv = &fn->vregs[cur];
    int point = cv->live_start;

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

    if (cv->address_taken) {
      continue;
    }

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
    if (!got_reg && cv->rclass == MIR_RC_XMM) {
      if (!cv->crosses_call || cv->crosses_xmm_preserving_only) {
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
        for (size_t p = 0;
             !got_reg && !cv->crosses_call && p < MIR_XMM_NONVOL_POOL_COUNT;
             p++) {
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
      BinaryGpRegister cross_ext[MIR_GP_CROSSCALL_POOL_EXT];
      size_t cross_ext_n =
          mir_cross_pool_for(cv, gp_cross_pool, gp_cross_pool_count, cross_ext);
      const BinaryGpRegister *pool =
          cv->crosses_call ? cross_ext : gp_leaf_pool;
      size_t pool_n = cv->crosses_call ? cross_ext_n : gp_leaf_pool_count;
      int avoid = mir_narrowing_avoid_reg(fn, narrow_src, cur);
      for (int relax = 0; !got_reg && relax < 2; relax++) {
        for (size_t p = 0; p < pool_n; p++) {
          BinaryGpRegister reg = pool[p];
          if (relax == 0 && avoid >= 0 && (int)reg == avoid) {
            continue;
          }
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
        if (avoid < 0) {
          break;
        }
      }
    }

    if (got_reg) {
      active[active_count++] = cur;
      continue;
    }

    if (cv->crosses_call) {
      next_spill_offset += cv->width > 8 ? 16 : 8;
      cv->assigned = 1;
      cv->in_register = 0;
      cv->spill_offset = next_spill_offset;
      continue;
    }

    MirVregId spill_victim = MIR_VREG_NONE;
    int victim_end = -1;
    int victim_lc = 1;
    for (size_t r = 0; r < active_count; r++) {
      MirVregId a = active[r];
      MirVreg *av = &fn->vregs[a];
      if (av->rclass != cv->rclass || !av->in_register) {
        continue;
      }
      if (av->rclass == MIR_RC_GP &&
          mir_reg_clobbered_in_range(fn, (BinaryGpRegister)av->phys,
                                     cv->live_start, cv->live_end)) {
        continue;
      }
      int better;
      if (spill_victim == MIR_VREG_NONE) {
        better = 1;
      } else if (av->loop_carried != victim_lc) {
        better = (av->loop_carried < victim_lc);
      } else {
        better = (av->live_end > victim_end);
      }
      if (better) {
        victim_end = av->live_end;
        victim_lc = av->loop_carried;
        spill_victim = a;
      }
    }

    int prefer_victim = 0;
    if (spill_victim != MIR_VREG_NONE) {
      if (victim_lc != cv->loop_carried) {
        prefer_victim = (cv->loop_carried && !victim_lc);
      } else {
        prefer_victim = fn->vregs[spill_victim].live_end > cv->live_end;
      }
    }
    if (prefer_victim) {
      MirVreg *vv = &fn->vregs[spill_victim];
      int reg = vv->phys;
      next_spill_offset += vv->width > 8 ? 16 : 8;
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
      for (size_t r = 0; r < active_count; r++) {
        if (active[r] == spill_victim) {
          active[r] = cur;
          break;
        }
      }
    } else {
      next_spill_offset += cv->width > 8 ? 16 : 8;
      cv->assigned = 1;
      cv->in_register = 0;
      cv->spill_offset = next_spill_offset;
    }
  }

  mir_drop_unused_preserves(fn);
  fn->spill_bytes =
      next_spill_offset - (fn->context ? fn->context->raw_frame_size : 0);

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
        free(narrow_src);
        fn->has_error = 1;
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
      if (used_xmm[reg] &&
          !code_generator_binary_context_add_saved_xmm_register(
              fn->context, (BinaryXmmRegister)reg)) {
        free(order);
        free(active);
        free(narrow_src);
        fn->has_error = 1;
        return 0;
      }
    }
  }

  free(order);
  free(active);
  free(narrow_src);
  mir_regalloc_trace_done(fn);
  return mir_regalloc_finish(fn);
}
