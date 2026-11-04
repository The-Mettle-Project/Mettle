#include "codegen/binary/mir_cfg.h"
#include "codegen/binary/mir_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIR_SINK_MAX_ROUNDS 4
#define MIR_TEST_MAX_HEADERS 16
#define MIR_TEST_MAX_BLOCKS 20000
#define MIR_TRACE_MAX_INSNS 24
#define MIR_TRACE_MAX_STEPS 64
#define MIR_THREAD_MAX_GROWTH 8192

typedef struct {
  size_t before;
  size_t seq;
  MirInst inst;
} MirInsertion;

typedef struct {
  MirInsertion *items;
  size_t count;
  size_t capacity;
} MirInsertions;

static int mir_insertions_push(MirInsertions *list, size_t before,
                               const MirInst *inst) {
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2u : 16u;
    MirInsertion *grown =
        (MirInsertion *)realloc(list->items, capacity * sizeof(MirInsertion));
    if (!grown) {
      return 0;
    }
    list->items = grown;
    list->capacity = capacity;
  }
  list->items[list->count].before = before;
  list->items[list->count].seq = list->count;
  list->items[list->count].inst = *inst;
  list->count++;
  return 1;
}

static int mir_insertion_order(const void *x, const void *y) {
  const MirInsertion *a = (const MirInsertion *)x;
  const MirInsertion *b = (const MirInsertion *)y;
  if (a->before != b->before) {
    return (a->before > b->before) - (a->before < b->before);
  }
  return (a->seq > b->seq) - (a->seq < b->seq);
}

static int mir_insertions_apply(MirFunction *fn, MirInsertions *list) {
  size_t total = fn->insn_count + list->count;
  MirInst *out = (MirInst *)malloc((total ? total : 1u) * sizeof(MirInst));
  size_t n = 0;
  size_t next = 0;
  if (!out) {
    return 0;
  }
  qsort(list->items, list->count, sizeof(MirInsertion), mir_insertion_order);
  for (size_t i = 0; i < fn->insn_count; i++) {
    while (next < list->count && list->items[next].before == i) {
      out[n++] = list->items[next++].inst;
    }
    out[n++] = fn->insns[i];
  }
  while (next < list->count) {
    out[n++] = list->items[next++].inst;
  }
  free(fn->insns);
  fn->insns = out;
  fn->insn_count = n;
  fn->insn_capacity = total ? total : 1u;
  return 1;
}

static void mir_make_nop(MirInst *in) {
  memset(in, 0, sizeof(*in));
  in->op = MIR_NOP;
  in->ir_index = -1;
}

static int mir_inst_is_vreg_load(const MirInst *in) {
  return (in->op == MIR_MOV || in->op == MIR_MOVSX || in->op == MIR_MOVZX) &&
         in->dst.kind == MIR_OPK_VREG && in->a.kind == MIR_OPK_MEM &&
         !in->a.mem.phys_base_valid;
}

static int mir_inst_reads(const MirInst *in, MirVregId v) {
  MirVregId ids[6];
  int n = mir_cfg_insn_uses(in, ids);
  for (int k = 0; k < n; k++) {
    if (ids[k] == v) {
      return 1;
    }
  }
  return 0;
}

static int mir_sink_may_cross(const MirInst *in) {
  if (in->op == MIR_NOP || mir_inst_is_vreg_load(in)) {
    return 1;
  }
  if (mir_op_has(in->op, MIR_OPF_LABEL_BRANCH) || in->op == MIR_JMP_TABLE) {
    return 1;
  }
  return mir_op_has(in->op, MIR_OPF_PURE_DEF) && in->dst.kind == MIR_OPK_VREG &&
         in->a.kind != MIR_OPK_MEM && in->b.kind != MIR_OPK_MEM;
}

static int mir_sink_path_is_clear(const MirFunction *fn, const MirCfgBlock *blk,
                                  size_t at) {
  const MirInst *load = &fn->insns[at];
  MirVregId v = load->dst.vreg;
  for (int j = (int)at + 1; j < blk->end; j++) {
    const MirInst *in = &fn->insns[j];
    MirVregId d = mir_cfg_insn_def(in);
    if (!mir_sink_may_cross(in) || mir_inst_reads(in, v)) {
      return 0;
    }
    if (d != MIR_VREG_NONE &&
        (d == v || d == load->a.mem.base || d == load->a.mem.index)) {
      return 0;
    }
  }
  return 1;
}

static int mir_block_has_one_pred(const MirCfg *cfg, int block, int pred) {
  for (int e = cfg->blocks[block].pred_head; e >= 0; e = cfg->edge_next[e]) {
    if (cfg->edge_to[e] != pred) {
      return 0;
    }
  }
  return 1;
}

static size_t mir_block_body_start(const MirFunction *fn, const MirCfgBlock *blk) {
  size_t at = (size_t)blk->start;
  if (fn->insns[at].op == MIR_LABEL) {
    at++;
  }
  return at;
}

static int mir_sink_targets(const MirCfg *cfg, int block, MirVregId v,
                            int *targets, size_t capacity, size_t *count_out) {
  size_t count = 0;
  int edges = 0;
  int dead_edge = 0;
  for (int e = cfg->blocks[block].succ_head; e >= 0; e = cfg->edge_next[e]) {
    int to = cfg->edge_to[e];
    edges++;
    if (!mir_cfg_is_live_in(cfg, (size_t)to, v)) {
      dead_edge = 1;
      continue;
    }
    if (to == block || !mir_block_has_one_pred(cfg, to, block)) {
      return 0;
    }
    int seen = 0;
    for (size_t k = 0; k < count; k++) {
      seen |= targets[k] == to;
    }
    if (!seen) {
      if (count == capacity) {
        return 0;
      }
      targets[count++] = to;
    }
  }
  *count_out = count;
  return edges >= 2 && dead_edge && count > 0;
}

static int mir_sink_block_loads(MirFunction *fn, const MirCfg *cfg, int block,
                                int *targets, MirInsertions *list) {
  const MirCfgBlock *blk = &cfg->blocks[block];
  size_t capacity = cfg->block_count;
  if (blk->rpo < 0 || blk->end - blk->start < 2) {
    return 1;
  }
  for (int at = blk->end - 1; at >= blk->start; at--) {
    MirInst load = fn->insns[at];
    size_t count = 0;
    if (!mir_inst_is_vreg_load(&load) ||
        fn->vregs[load.dst.vreg].address_taken ||
        !mir_sink_path_is_clear(fn, blk, (size_t)at) ||
        !mir_sink_targets(cfg, block, load.dst.vreg, targets, capacity,
                          &count)) {
      continue;
    }
    for (size_t k = 0; k < count; k++) {
      size_t before = mir_block_body_start(fn, &cfg->blocks[targets[k]]);
      if (!mir_insertions_push(list, before, &load)) {
        return 0;
      }
    }
    mir_make_nop(&fn->insns[at]);
  }
  return 1;
}

static int mir_sink_round(MirFunction *fn, int *changed) {
  MirCfg cfg;
  MirInsertions list = {0};
  int ok = 1;
  int *targets = NULL;
  *changed = 0;
  if (!mir_cfg_build(&cfg, fn)) {
    return 0;
  }
  targets = (int *)malloc(cfg.block_count * sizeof(int));
  ok = targets != NULL;
  for (size_t b = 0; ok && b < cfg.block_count; b++) {
    ok = mir_sink_block_loads(fn, &cfg, (int)b, targets, &list);
  }
  mir_cfg_free(&cfg);
  free(targets);
  if (ok && list.count > 0) {
    ok = mir_insertions_apply(fn, &list);
    *changed = ok;
  }
  free(list.items);
  return ok;
}

static int mir_extend_narrows_load(const MirInst *ext, const MirInst *load) {
  return (ext->op == MIR_MOVSX || ext->op == MIR_MOVZX) && !ext->is_float &&
         ext->dst.kind == MIR_OPK_VREG && ext->a.kind == MIR_OPK_VREG &&
         ext->a.vreg == load->dst.vreg && ext->width < load->width &&
         (ext->width == 1 || ext->width == 2 || ext->width == 4);
}

static int mir_narrow_one_load(MirFunction *fn, const MirCfg *cfg, size_t at) {
  const MirInst load = fn->insns[at];
  const MirCfgBlock *blk = &cfg->blocks[cfg->block_of[at]];
  MirVregId v = load.dst.vreg;
  size_t ext_at = 0;
  for (size_t j = at + 1; j < (size_t)blk->end; j++) {
    const MirInst *in = &fn->insns[j];
    MirVregId d = mir_cfg_insn_def(in);
    if (ext_at == 0 && mir_extend_narrows_load(in, &load)) {
      ext_at = j;
      if (d == v) {
        break;
      }
      continue;
    }
    if (mir_inst_reads(in, v)) {
      return 0;
    }
    if (d == v) {
      break;
    }
    if (ext_at == 0 &&
        (!mir_sink_may_cross(in) || d == load.a.mem.base ||
         d == load.a.mem.index)) {
      return 0;
    }
    if (j + 1u == (size_t)blk->end && ext_at != 0 &&
        mir_cfg_is_live_out(cfg, (size_t)cfg->block_of[at], v)) {
      return 0;
    }
  }
  if (ext_at == 0) {
    return 0;
  }
  if (ext_at + 1u == (size_t)blk->end && fn->insns[ext_at].dst.vreg != v &&
      mir_cfg_is_live_out(cfg, (size_t)cfg->block_of[at], v)) {
    return 0;
  }
  {
    MirInst *ext = &fn->insns[ext_at];
    MirInst narrowed = load;
    narrowed.dst = ext->dst;
    narrowed.width = ext->width;
    narrowed.is_unsigned = ext->op == MIR_MOVZX;
    narrowed.ir_index = ext->ir_index;
    *ext = narrowed;
  }
  mir_make_nop(&fn->insns[at]);
  return 1;
}

static void mir_narrow_extended_loads(MirFunction *fn) {
  MirCfg cfg;
  if (!mir_cfg_build(&cfg, fn)) {
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_MOV && !in->is_float && in->width >= 2 &&
        mir_inst_is_vreg_load(in) && !fn->vregs[in->dst.vreg].address_taken &&
        fn->vregs[in->dst.vreg].rclass == MIR_RC_GP) {
      mir_narrow_one_load(fn, &cfg, i);
    }
  }
  mir_cfg_free(&cfg);
}

void mir_sink_partial_loads(MirFunction *fn) {
  int branches = 0;
  int sank = 0;
  if (!fn || fn->insn_count < 3 || fn->vreg_count == 0) {
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    branches |= fn->insns[i].op == MIR_JMP_TABLE ||
                mir_op_has(fn->insns[i].op, MIR_OPF_CONDITIONAL_BRANCH);
  }
  if (!branches) {
    return;
  }
  for (int round = 0; round < MIR_SINK_MAX_ROUNDS; round++) {
    int changed = 0;
    if (!mir_sink_round(fn, &changed)) {
      fn->has_error = 1;
      return;
    }
    if (!changed) {
      break;
    }
    sank = 1;
  }
  if (sank) {
    mir_narrow_extended_loads(fn);
  }
}

static int mir_shared_address_use_ok(const MirInst *in, MirVregId t) {
  if (in->b.kind == MIR_OPK_VREG && in->b.vreg == t) {
    return 0;
  }
  if (in->b.kind == MIR_OPK_MEM &&
      (in->b.mem.base == t || in->b.mem.index == t)) {
    return 0;
  }
  if (in->op != MIR_MOV || in->is_float) {
    return 0;
  }
  if (in->a.kind == MIR_OPK_MEM && in->dst.kind == MIR_OPK_VREG) {
    return in->a.mem.base == t && in->a.mem.index == MIR_VREG_NONE &&
           !in->a.mem.phys_base_valid && !in->a.mem.frame_home_valid;
  }
  if (in->dst.kind == MIR_OPK_MEM) {
    return in->dst.mem.base == t && in->dst.mem.index == MIR_VREG_NONE &&
           !in->dst.mem.phys_base_valid && !in->dst.mem.frame_home_valid &&
           !(in->a.kind == MIR_OPK_VREG && in->a.vreg == t);
  }
  return 0;
}

static int mir_shared_address_uses(const MirFunction *fn, MirVregId t,
                                   int *count) {
  *count = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP || !mir_inst_reads(in, t)) {
      continue;
    }
    if (!mir_shared_address_use_ok(in, t)) {
      return 0;
    }
    (*count)++;
  }
  return *count > 0;
}

static int mir_source_survives(const MirFunction *fn, const MirCfg *cfg,
                               size_t def_at, MirVregId t, MirVregId s) {
  int home = cfg->block_of[def_at];
  for (int i = (int)def_at + 1; i < cfg->blocks[home].end; i++) {
    if (mir_cfg_insn_def(&fn->insns[i]) == s) {
      return 0;
    }
  }
  for (size_t b = 0; b < cfg->block_count; b++) {
    if (!mir_cfg_is_live_in(cfg, b, t)) {
      continue;
    }
    for (int i = cfg->blocks[b].start; i < cfg->blocks[b].end; i++) {
      if (mir_cfg_insn_def(&fn->insns[i]) == s) {
        return 0;
      }
    }
  }
  return 1;
}

static int mir_vreg_is_scaled_product(const MirFunction *fn, MirVregId v,
                                      size_t before) {
  for (size_t i = before; i > 0; i--) {
    const MirInst *in = &fn->insns[i - 1];
    if (mir_cfg_insn_def(in) == v) {
      return in->op == MIR_IMUL && in->b.kind == MIR_OPK_IMM;
    }
    if (in->op == MIR_LABEL) {
      return 0;
    }
  }
  return 0;
}

static void mir_point_uses_at_pair(MirFunction *fn, MirVregId t, MirVregId base,
                                   MirVregId index) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP) {
      continue;
    }
    MirOperand *op = NULL;
    if (in->a.kind == MIR_OPK_MEM && in->a.mem.base == t) {
      op = &in->a;
    } else if (in->dst.kind == MIR_OPK_MEM && in->dst.mem.base == t) {
      op = &in->dst;
    }
    if (op) {
      op->mem.base = base;
      op->mem.index = index;
      op->mem.scale = 1;
    }
  }
}

static int mir_shared_address_candidate(const MirFunction *fn, const MirInst *in,
                                        const int *defs) {
  return in->op == MIR_ADD && !in->is_float && in->width == 8 &&
         in->dst.kind == MIR_OPK_VREG && in->a.kind == MIR_OPK_VREG &&
         in->b.kind == MIR_OPK_VREG && in->dst.vreg != in->a.vreg &&
         in->dst.vreg != in->b.vreg && defs[in->dst.vreg] == 1 &&
         !fn->vregs[in->dst.vreg].address_taken &&
         fn->vregs[in->a.vreg].rclass == MIR_RC_GP &&
         fn->vregs[in->b.vreg].rclass == MIR_RC_GP;
}

static size_t mir_named_copy_of_address(const MirFunction *fn, const MirCfg *cfg,
                                        size_t add_at, const int *defs) {
  const MirInst *add = &fn->insns[add_at];
  MirVregId t = add->dst.vreg;
  size_t copy_at = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP || !mir_inst_reads(in, t)) {
      continue;
    }
    if (copy_at != 0 || in->op != MIR_MOV || in->is_float || in->width != 8 ||
        in->dst.kind != MIR_OPK_VREG || in->a.kind != MIR_OPK_VREG ||
        in->a.vreg != t || defs[in->dst.vreg] != 1 ||
        fn->vregs[in->dst.vreg].address_taken) {
      return 0;
    }
    copy_at = i;
  }
  if (copy_at <= add_at || cfg->block_of[copy_at] != cfg->block_of[add_at]) {
    return 0;
  }
  for (size_t i = add_at + 1; i < copy_at; i++) {
    MirVregId d = mir_cfg_insn_def(&fn->insns[i]);
    if (d == add->a.vreg || d == add->b.vreg) {
      return 0;
    }
  }
  return copy_at;
}

static void mir_fold_one_shared_address(MirFunction *fn, const MirCfg *cfg,
                                        size_t at, const int *defs) {
  MirInst *add = &fn->insns[at];
  size_t copy_at = mir_named_copy_of_address(fn, cfg, at, defs);
  size_t def_at = copy_at ? copy_at : at;
  MirVregId t = copy_at ? fn->insns[copy_at].dst.vreg : add->dst.vreg;
  MirVregId base = add->a.vreg;
  MirVregId index = add->b.vreg;
  int uses = 0;
  if (t == base || t == index || !mir_shared_address_uses(fn, t, &uses) ||
      !mir_source_survives(fn, cfg, def_at, t, base) ||
      !mir_source_survives(fn, cfg, def_at, t, index)) {
    return;
  }
  if (mir_vreg_is_scaled_product(fn, base, at) &&
      !mir_vreg_is_scaled_product(fn, index, at)) {
    MirVregId swap = base;
    base = index;
    index = swap;
  }
  mir_point_uses_at_pair(fn, t, base, index);
  if (copy_at) {
    mir_make_nop(&fn->insns[copy_at]);
  }
  mir_make_nop(add);
}

static void mir_replace_reads(MirFunction *fn, MirVregId from, MirVregId to) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    MirOperand *ops[3] = {&in->a, &in->b, &in->dst};
    if (in->op == MIR_NOP) {
      continue;
    }
    for (int k = 0; k < 3; k++) {
      MirOperand *op = ops[k];
      if (op->kind == MIR_OPK_VREG && op->vreg == from && k != 2) {
        op->vreg = to;
      } else if (op->kind == MIR_OPK_MEM) {
        if (op->mem.base == from) {
          op->mem.base = to;
        }
        if (op->mem.index == from) {
          op->mem.index = to;
        }
      }
    }
  }
}

static int mir_copy_follows_its_add(const MirFunction *fn, size_t copy_at,
                                    MirVregId t, MirVregId copy) {
  for (size_t back = 1; back <= 16 && back <= copy_at; back++) {
    const MirInst *in = &fn->insns[copy_at - back];
    if (in->op == MIR_NOP) {
      continue;
    }
    if (in->op == MIR_LABEL || mir_op_has(in->op, MIR_OPF_LABEL_BRANCH) ||
        in->op == MIR_JMP_TABLE || mir_inst_reads(in, copy)) {
      return 0;
    }
    if (mir_cfg_insn_def(in) == t) {
      return in->op == MIR_ADD && !in->is_float && in->width == 8 &&
             in->a.kind == MIR_OPK_VREG && in->b.kind == MIR_OPK_VREG;
    }
  }
  return 0;
}

static void mir_forward_address_copies(MirFunction *fn, int *defs) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    if (in->op != MIR_MOV || in->is_float || in->width != 8 ||
        in->dst.kind != MIR_OPK_VREG || in->a.kind != MIR_OPK_VREG ||
        in->dst.vreg == in->a.vreg || defs[in->dst.vreg] != 1 ||
        defs[in->a.vreg] != 1 || fn->vregs[in->dst.vreg].address_taken ||
        fn->vregs[in->a.vreg].address_taken ||
        fn->vregs[in->dst.vreg].rclass != MIR_RC_GP) {
      continue;
    }
    MirVregId copy = in->dst.vreg;
    MirVregId t = in->a.vreg;
    int is_param = 0;
    for (size_t p = 0; p < fn->param_count; p++) {
      is_param |= fn->params[p].vreg == copy ||
                  fn->params[p].sysv_storage == copy;
    }
    if (is_param || !mir_copy_follows_its_add(fn, i, t, copy)) {
      continue;
    }
    mir_make_nop(in);
    mir_replace_reads(fn, copy, t);
    defs[copy] = 0;
  }
}

void mir_fold_shared_address(MirFunction *fn) {
  MirCfg cfg;
  int *defs;
  int any = 0;
  if (!fn || fn->insn_count < 3 || fn->vreg_count == 0) {
    return;
  }
  defs = (int *)calloc(fn->vreg_count, sizeof(int));
  if (!defs) {
    fn->has_error = 1;
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirVregId d = mir_cfg_insn_def(&fn->insns[i]);
    if (d >= 0 && (size_t)d < fn->vreg_count) {
      defs[d]++;
    }
  }
  mir_forward_address_copies(fn, defs);
  for (size_t i = 0; i < fn->insn_count; i++) {
    any |= mir_shared_address_candidate(fn, &fn->insns[i], defs);
  }
  if (any && mir_cfg_build(&cfg, fn)) {
    int budget = 64;
    for (size_t i = 0; budget > 0 && i < fn->insn_count; i++) {
      if (mir_shared_address_candidate(fn, &fn->insns[i], defs)) {
        mir_fold_one_shared_address(fn, &cfg, i, defs);
        budget--;
      }
    }
    mir_cfg_free(&cfg);
  }
  free(defs);
}

static char *mir_fresh_label(MirFunction *fn, const char *prefix) {
  static unsigned long long counter = 0;
  char *name = (char *)malloc(48);
  if (!name) {
    return NULL;
  }
  snprintf(name, 48, ".mir_%s_%llu", prefix, ++counter);
  if (!mir_function_own_aux(fn, name)) {
    return NULL;
  }
  return name;
}

static MirInst mir_label_inst(const char *name, int ir_index) {
  MirInst label;
  memset(&label, 0, sizeof(label));
  label.op = MIR_LABEL;
  label.dst = mir_op_label(name);
  label.width = 8;
  label.ir_index = ir_index;
  return label;
}

static int mir_increment_extend_at(const MirFunction *fn, size_t at,
                                   size_t *ext_at) {
  const MirInst *add = &fn->insns[at];
  size_t next = at + 1;
  const MirInst *ext;
  if ((add->op != MIR_ADD && add->op != MIR_SUB) || add->width != 8 ||
      add->dst.kind != MIR_OPK_VREG || add->a.kind != MIR_OPK_VREG ||
      add->dst.vreg != add->a.vreg || add->b.kind != MIR_OPK_IMM ||
      add->b.imm == 0 || add->b.imm > 0x7FFFFFFFLL ||
      add->b.imm < -0x7FFFFFFFLL) {
    return 0;
  }
  while (next < fn->insn_count && fn->insns[next].op == MIR_NOP) {
    next++;
  }
  if (next >= fn->insn_count) {
    return 0;
  }
  ext = &fn->insns[next];
  if (ext->op != MIR_MOVSX || ext->width != 4 || ext->dst.kind != MIR_OPK_VREG ||
      ext->a.kind != MIR_OPK_VREG || ext->dst.vreg != add->dst.vreg ||
      ext->a.vreg != add->dst.vreg) {
    return 0;
  }
  *ext_at = next;
  return 1;
}

static int mir_def_is_canonical_int32(const MirInst *in) {
  if (in->op == MIR_MOVSX) {
    return in->width <= 4;
  }
  if (in->op == MIR_MOVZX) {
    return in->width <= 2;
  }
  if (in->op != MIR_MOV || in->is_float) {
    return 0;
  }
  if (in->a.kind == MIR_OPK_IMM) {
    return in->a.imm >= -0x80000000LL && in->a.imm <= 0x7FFFFFFFLL;
  }
  if (in->a.kind == MIR_OPK_MEM) {
    return in->width <= 2 || (in->width == 4 && !in->is_unsigned);
  }
  return 0;
}

static int mir_counter_stays_canonical(const MirFunction *fn, MirVregId v) {
  for (size_t p = 0; p < fn->param_count; p++) {
    if (fn->params[p].vreg == v || fn->params[p].sysv_storage == v) {
      return 0;
    }
  }
  if (fn->vregs[v].address_taken || fn->vregs[v].rclass != MIR_RC_GP) {
    return 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    size_t ext_at = 0;
    if (mir_increment_extend_at(fn, i, &ext_at) && fn->insns[i].dst.vreg == v) {
      i = ext_at;
      continue;
    }
    if (mir_cfg_insn_def(&fn->insns[i]) == v &&
        !mir_def_is_canonical_int32(&fn->insns[i])) {
      return 0;
    }
  }
  return 1;
}

static int mir_function_ends_closed(const MirFunction *fn) {
  size_t at = fn->insn_count;
  while (at > 0 && fn->insns[at - 1].op == MIR_NOP) {
    at--;
  }
  if (at == 0) {
    return 0;
  }
  return fn->insns[at - 1].op == MIR_RET || fn->insns[at - 1].op == MIR_JMP ||
         fn->insns[at - 1].op == MIR_TRAP ||
         fn->insns[at - 1].op == MIR_JMP_TABLE;
}

static int mir_guard_one_counter(MirFunction *fn, size_t add_at, size_t ext_at,
                                 MirInsertions *list) {
  const MirInst add = fn->insns[add_at];
  MirInst *ext = &fn->insns[ext_at];
  const char *back = mir_fresh_label(fn, "wrap_back");
  const char *stub = mir_fresh_label(fn, "wrap_stub");
  int grows = (add.op == MIR_ADD) == (add.b.imm > 0);
  MirInst guard;
  MirInst jump;
  MirInst moved = *ext;
  MirInst back_label;
  MirInst stub_label;
  if (!back || !stub) {
    return 0;
  }
  memset(&guard, 0, sizeof(guard));
  guard.op = MIR_CMPBR;
  guard.dst = mir_op_label(stub);
  guard.a = mir_op_vreg(add.dst.vreg);
  guard.b = mir_op_imm(grows ? 0x7FFFFFFFLL : -0x80000000LL);
  guard.width = 8;
  guard.cc = grows ? 0x8F : 0x8C;
  guard.ir_index = ext->ir_index;
  memset(&jump, 0, sizeof(jump));
  jump.op = MIR_JMP;
  jump.dst = mir_op_label(back);
  jump.width = 8;
  jump.ir_index = ext->ir_index;
  back_label = mir_label_inst(back, ext->ir_index);
  stub_label = mir_label_inst(stub, ext->ir_index);
  *ext = guard;
  return mir_insertions_push(list, ext_at + 1u, &back_label) &&
         mir_insertions_push(list, fn->insn_count, &stub_label) &&
         mir_insertions_push(list, fn->insn_count, &moved) &&
         mir_insertions_push(list, fn->insn_count, &jump);
}

void mir_guard_counter_wrap(MirFunction *fn) {
  MirCfg cfg;
  MirInsertions list = {0};
  int ok = 1;
  if (!fn || fn->insn_count < 3 || fn->vreg_count == 0 ||
      !mir_function_ends_closed(fn)) {
    return;
  }
  if (!mir_cfg_build(&cfg, fn)) {
    return;
  }
  for (size_t i = 0; ok && i < fn->insn_count; i++) {
    size_t ext_at = 0;
    if (cfg.blocks[cfg.block_of[i]].loop_depth > 0 &&
        mir_increment_extend_at(fn, i, &ext_at) &&
        cfg.block_of[ext_at] == cfg.block_of[i] &&
        mir_counter_stays_canonical(fn, fn->insns[i].dst.vreg)) {
      ok = mir_guard_one_counter(fn, i, ext_at, &list);
      i = ext_at;
    }
  }
  mir_cfg_free(&cfg);
  if (ok && list.count > 0) {
    ok = mir_insertions_apply(fn, &list);
  }
  free(list.items);
  if (!ok) {
    fn->has_error = 1;
  }
}

typedef enum {
  MIR_TEST_UNSEEN = 0,
  MIR_TEST_ANY,
  MIR_TEST_FALLS,
  MIR_TEST_TAKEN
} MirTestState;

typedef struct {
  size_t test;
  int block;
  int fall_block;
  int taken_block;
  MirVregId v;
} MirTestHeader;

static MirTestState mir_test_meet(MirTestState a, MirTestState b) {
  if (a == MIR_TEST_UNSEEN || a == b) {
    return b;
  }
  if (b == MIR_TEST_UNSEEN) {
    return a;
  }
  return MIR_TEST_ANY;
}

static int mir_eval_cc(unsigned char cc, long long a, long long b, int *taken) {
  unsigned long long ua = (unsigned long long)a;
  unsigned long long ub = (unsigned long long)b;
  switch (cc) {
    case 0x84: *taken = a == b; return 1;
    case 0x85: *taken = a != b; return 1;
    case 0x8C: *taken = a < b; return 1;
    case 0x8D: *taken = a >= b; return 1;
    case 0x8E: *taken = a <= b; return 1;
    case 0x8F: *taken = a > b; return 1;
    case 0x82: *taken = b >= 0 && ua < ub; return b >= 0;
    case 0x83: *taken = b >= 0 && ua >= ub; return b >= 0;
    case 0x86: *taken = b >= 0 && ua <= ub; return b >= 0;
    case 0x87: *taken = b >= 0 && ua > ub; return b >= 0;
    default: return 0;
  }
}

static MirTestState mir_test_of_constant(const MirInst *test, long long k) {
  long long rhs = 0;
  int taken = 0;
  if (k < 0 || k > 0x7FFFFFFFLL) {
    return MIR_TEST_ANY;
  }
  if (test->op == MIR_CMPBR) {
    rhs = test->b.imm;
    if (rhs < -0x80000000LL || rhs > 0x7FFFFFFFLL) {
      return MIR_TEST_ANY;
    }
  }
  if (!mir_eval_cc(test->cc, k, rhs, &taken)) {
    return MIR_TEST_ANY;
  }
  return taken ? MIR_TEST_TAKEN : MIR_TEST_FALLS;
}

static MirTestState mir_test_block_effect(const MirFunction *fn,
                                          const MirCfgBlock *blk,
                                          const MirTestHeader *h,
                                          MirTestState in) {
  const MirInst *test = &fn->insns[h->test];
  MirTestState state = in;
  for (int i = blk->start; i < blk->end; i++) {
    const MirInst *ins = &fn->insns[i];
    if (mir_cfg_insn_def(ins) != h->v) {
      continue;
    }
    if (ins->op == MIR_MOV && ins->a.kind == MIR_OPK_IMM && ins->width >= 4) {
      state = mir_test_of_constant(test, ins->a.imm);
    } else {
      state = MIR_TEST_ANY;
    }
  }
  return state;
}

static int mir_test_flow_changed(MirTestState *states, int to, MirTestState out) {
  MirTestState merged = mir_test_meet(states[to], out);
  if (merged == states[to]) {
    return 0;
  }
  states[to] = merged;
  return 1;
}

static int mir_test_flow_block(const MirCfg *cfg, const MirTestHeader *h,
                               MirTestState *states, int b) {
  const MirFunction *fn = cfg->fn;
  int changed = 0;
  if (states[b] == MIR_TEST_UNSEEN) {
    return 0;
  }
  if (b == h->block) {
    changed |= mir_test_flow_changed(states, h->fall_block, MIR_TEST_FALLS);
    changed |= mir_test_flow_changed(states, h->taken_block, MIR_TEST_TAKEN);
    return changed;
  }
  MirTestState out = mir_test_block_effect(fn, &cfg->blocks[b], h, states[b]);
  for (int e = cfg->blocks[b].succ_head; e >= 0; e = cfg->edge_next[e]) {
    changed |= mir_test_flow_changed(states, cfg->edge_to[e], out);
  }
  return changed;
}

static void mir_test_solve(const MirCfg *cfg, const MirTestHeader *h,
                           MirTestState *states) {
  int changed = 1;
  memset(states, 0, cfg->block_count * sizeof(MirTestState));
  states[0] = MIR_TEST_ANY;
  while (changed) {
    changed = 0;
    for (size_t k = 0; k < cfg->rpo_count; k++) {
      changed |= mir_test_flow_block(cfg, h, states, cfg->rpo_order[k]);
    }
  }
}

static int mir_test_is_candidate(const MirFunction *fn, const MirCfg *cfg,
                                 size_t label, MirTestHeader *h) {
  size_t at = label + 1;
  const MirInst *test;
  int block;
  int taken_insn;
  while (at < fn->insn_count && fn->insns[at].op == MIR_NOP) {
    at++;
  }
  if (at + 1 >= fn->insn_count) {
    return 0;
  }
  test = &fn->insns[at];
  if (test->dst.kind != MIR_OPK_LABEL || test->a.kind != MIR_OPK_VREG ||
      test->is_float) {
    return 0;
  }
  if (test->op == MIR_JCC) {
    if (test->cc != 0x84 && test->cc != 0x85) {
      return 0;
    }
  } else if (test->op != MIR_CMPBR || test->b.kind != MIR_OPK_IMM ||
             test->width < 4) {
    return 0;
  }
  if (fn->vregs[test->a.vreg].address_taken) {
    return 0;
  }
  block = cfg->block_of[at];
  taken_insn = mir_cfg_label_insn(cfg, test->dst.sym);
  if (taken_insn < 0 || block != cfg->block_of[label]) {
    return 0;
  }
  h->test = at;
  h->block = block;
  h->fall_block = cfg->block_of[at + 1];
  h->taken_block = cfg->block_of[taken_insn];
  h->v = test->a.vreg;
  return h->fall_block != h->taken_block;
}

static int mir_test_retarget_sites(MirFunction *fn, const MirCfg *cfg,
                                   const MirTestHeader *h,
                                   const MirTestState *states,
                                   const char *label, const char **after) {
  int moved = 0;
  const char *taken = fn->insns[h->test].dst.sym;
  for (size_t b = 0; b < cfg->block_count; b++) {
    MirInst *last;
    MirTestState out;
    if (states[b] == MIR_TEST_UNSEEN || (int)b == h->block) {
      continue;
    }
    last = &fn->insns[cfg->blocks[b].end - 1];
    if (!mir_op_has(last->op, MIR_OPF_LABEL_BRANCH) ||
        last->dst.kind != MIR_OPK_LABEL ||
        strcmp(last->dst.sym, label) != 0) {
      continue;
    }
    out = mir_test_block_effect(fn, &cfg->blocks[b], h, states[b]);
    if (out == MIR_TEST_TAKEN) {
      last->dst = mir_op_label(taken);
      moved = 1;
    } else if (out == MIR_TEST_FALLS) {
      if (!*after) {
        *after = mir_fresh_label(fn, "after_test");
        if (!*after) {
          return -1;
        }
      }
      last->dst = mir_op_label(*after);
      moved = 1;
    }
  }
  return moved;
}

static int mir_test_settle_entry(MirFunction *fn, const MirTestHeader *h,
                                 const MirTestState *states) {
  MirInst *test = &fn->insns[h->test];
  if (states[h->block] == MIR_TEST_FALLS) {
    mir_make_nop(test);
    return 1;
  }
  if (states[h->block] == MIR_TEST_TAKEN) {
    MirOperand target = test->dst;
    int ir_index = test->ir_index;
    memset(test, 0, sizeof(*test));
    test->op = MIR_JMP;
    test->dst = target;
    test->width = 8;
    test->ir_index = ir_index;
    return 1;
  }
  return 0;
}

static int mir_def_is_dead(const MirFunction *fn, const MirCfg *cfg, size_t at,
                           MirVregId v) {
  int block = cfg->block_of[at];
  for (int j = (int)at + 1; j < cfg->blocks[block].end; j++) {
    const MirInst *in = &fn->insns[j];
    if (mir_inst_reads(in, v)) {
      return 0;
    }
    if (mir_cfg_insn_def(in) == v) {
      return 1;
    }
  }
  return !mir_cfg_is_live_out(cfg, (size_t)block, v);
}

static void mir_test_drop_dead_constants(MirFunction *fn, MirVregId v) {
  MirCfg cfg;
  if (!mir_cfg_build(&cfg, fn)) {
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    if (in->op == MIR_MOV && in->dst.kind == MIR_OPK_VREG && in->dst.vreg == v &&
        in->a.kind == MIR_OPK_IMM && mir_def_is_dead(fn, &cfg, i, v)) {
      mir_make_nop(in);
    }
  }
  mir_cfg_free(&cfg);
}

static int mir_test_insert_after(MirFunction *fn, const MirTestHeader *h,
                                 const char *after) {
  MirInsertions list = {0};
  MirInst label = mir_label_inst(after, fn->insns[h->test].ir_index);
  int ok;
  ok = mir_insertions_push(&list, h->test + 1u, &label) &&
       mir_insertions_apply(fn, &list);
  free(list.items);
  return ok;
}

static int mir_test_thread_one(MirFunction *fn, const MirCfg *cfg, size_t label,
                               MirTestState *states, MirVregId *dirty) {
  MirTestHeader h;
  const char *after = NULL;
  int moved;
  if (!mir_test_is_candidate(fn, cfg, label, &h)) {
    return 0;
  }
  mir_test_solve(cfg, &h, states);
  if (states[h.block] == MIR_TEST_UNSEEN) {
    return 0;
  }
  moved = mir_test_retarget_sites(fn, cfg, &h, states,
                                  fn->insns[label].dst.sym, &after);
  if (moved < 0) {
    return -1;
  }
  if (moved == 0) {
    if (!mir_test_settle_entry(fn, &h, states)) {
      return 0;
    }
    *dirty = h.v;
    return 1;
  }
  if (after && !mir_test_insert_after(fn, &h, after)) {
    return -1;
  }
  *dirty = h.v;
  return 1;
}

static int mir_test_round(MirFunction *fn, size_t *cursor, MirVregId *dirty) {
  MirCfg cfg;
  MirTestState *states;
  int result = 0;
  if (!mir_cfg_build(&cfg, fn)) {
    return 0;
  }
  if (cfg.block_count > MIR_TEST_MAX_BLOCKS) {
    mir_cfg_free(&cfg);
    return 0;
  }
  states = (MirTestState *)malloc(cfg.block_count * sizeof(MirTestState));
  if (!states) {
    mir_cfg_free(&cfg);
    return -1;
  }
  while (*cursor < fn->insn_count && result == 0) {
    if (fn->insns[*cursor].op == MIR_LABEL) {
      result = mir_test_thread_one(fn, &cfg, *cursor, states, dirty);
    }
    if (result == 0) {
      (*cursor)++;
    }
  }
  free(states);
  mir_cfg_free(&cfg);
  return result;
}

void mir_thread_known_tests(MirFunction *fn) {
  size_t cursor = 0;
  if (!fn || fn->insn_count < 4 || fn->vreg_count == 0) {
    return;
  }
  for (int work = 0; work < 2 * MIR_TEST_MAX_HEADERS; work++) {
    MirVregId dirty = MIR_VREG_NONE;
    int r = mir_test_round(fn, &cursor, &dirty);
    if (r < 0) {
      fn->has_error = 1;
      return;
    }
    if (r == 0) {
      return;
    }
    if (dirty != MIR_VREG_NONE) {
      mir_test_drop_dead_constants(fn, dirty);
    }
  }
}

typedef struct {
  size_t at[MIR_TRACE_MAX_INSNS];
  size_t count;
  size_t table;
} MirTrace;

typedef struct {
  size_t table;
  unsigned char *seen;
} MirTableReach;

typedef struct {
  MirFunction *fn;
  MirCfg cfg;
  MirInst *out;
  size_t count;
  size_t capacity;
  size_t added;
  MirTableReach *reach;
  size_t reach_count;
  int *stack;
  int failed;
} MirThread;

static int mir_trace_may_copy(const MirInst *in) {
  switch (in->op) {
    case MIR_MOV:
    case MIR_MOVSX:
    case MIR_MOVZX:
    case MIR_LEA:
    case MIR_LEA_LOCAL:
    case MIR_LEA_GLOBAL:
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
    case MIR_CMPBR:
    case MIR_JCC:
    case MIR_FCMPBR:
    case MIR_BT:
    case MIR_JMP_TABLE:
      return 1;
    default:
      return 0;
  }
}

static int mir_trace_build(const MirThread *th, const char *label, MirTrace *tr) {
  const MirFunction *fn = th->fn;
  int at = mir_cfg_label_insn(&th->cfg, label);
  size_t i;
  tr->count = 0;
  if (at < 0) {
    return 0;
  }
  i = (size_t)at;
  for (int steps = 0; steps < MIR_TRACE_MAX_STEPS && i < fn->insn_count; steps++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_NOP || in->op == MIR_LABEL) {
      i++;
      continue;
    }
    if (in->op == MIR_JMP) {
      at = mir_cfg_label_insn(&th->cfg, in->dst.sym);
      if (at < 0) {
        return 0;
      }
      i = (size_t)at;
      continue;
    }
    if (!mir_trace_may_copy(in) || tr->count == MIR_TRACE_MAX_INSNS) {
      return 0;
    }
    tr->at[tr->count++] = i;
    if (in->op == MIR_JMP_TABLE) {
      tr->table = i;
      return 1;
    }
    i++;
  }
  return 0;
}

static const unsigned char *mir_thread_reach(MirThread *th, size_t table) {
  const MirCfg *cfg = &th->cfg;
  size_t top = 0;
  unsigned char *seen;
  MirTableReach *grown;
  for (size_t k = 0; k < th->reach_count; k++) {
    if (th->reach[k].table == table) {
      return th->reach[k].seen;
    }
  }
  seen = (unsigned char *)calloc(cfg->block_count, 1);
  grown = (MirTableReach *)realloc(th->reach,
                                   (th->reach_count + 1u) * sizeof(MirTableReach));
  if (!seen || !grown) {
    free(seen);
    if (grown) {
      th->reach = grown;
    }
    th->failed = 1;
    return NULL;
  }
  th->reach = grown;
  th->reach[th->reach_count].table = table;
  th->reach[th->reach_count].seen = seen;
  th->reach_count++;
  th->stack[top++] = cfg->block_of[table];
  while (top > 0) {
    int b = th->stack[--top];
    for (int e = cfg->blocks[b].succ_head; e >= 0; e = cfg->edge_next[e]) {
      int to = cfg->edge_to[e];
      if (!seen[to]) {
        seen[to] = 1;
        th->stack[top++] = to;
      }
    }
  }
  return seen;
}

static int mir_thread_emit(MirThread *th, const MirInst *in) {
  if (th->count == th->capacity) {
    size_t capacity = th->capacity * 2u;
    MirInst *grown = (MirInst *)realloc(th->out, capacity * sizeof(MirInst));
    if (!grown) {
      th->failed = 1;
      return 0;
    }
    th->out = grown;
    th->capacity = capacity;
  }
  th->out[th->count++] = *in;
  return 1;
}

static int mir_thread_emit_trace(MirThread *th, const MirTrace *tr) {
  for (size_t k = 0; k < tr->count; k++) {
    if (!mir_thread_emit(th, &th->fn->insns[tr->at[k]])) {
      return 0;
    }
  }
  th->added += tr->count;
  return 1;
}

static int mir_thread_site_trace(MirThread *th, size_t site, const char *target,
                                 MirTrace *tr) {
  const unsigned char *reach;
  if (th->added >= MIR_THREAD_MAX_GROWTH || !target ||
      strncmp(target, ".mir_wrap_", 10) == 0 ||
      !mir_trace_build(th, target, tr)) {
    return 0;
  }
  reach = mir_thread_reach(th, tr->table);
  return reach && reach[th->cfg.block_of[site]];
}

static int mir_thread_conditional(MirThread *th, const MirInst *in,
                                  const MirTrace *tr) {
  MirInst branch = *in;
  MirInst label;
  const char *skip = mir_fresh_label(th->fn, "thr");
  if (!skip) {
    th->failed = 1;
    return 0;
  }
  branch.cc ^= 1;
  branch.dst = mir_op_label(skip);
  label = mir_label_inst(skip, in->ir_index);
  return mir_thread_emit(th, &branch) && mir_thread_emit_trace(th, tr) &&
         mir_thread_emit(th, &label);
}

static int mir_thread_fallthrough(MirThread *th, size_t at, const MirInst *in) {
  MirTrace tr;
  size_t prev = at;
  while (prev > 0 && th->fn->insns[prev - 1].op == MIR_NOP) {
    prev--;
  }
  if (prev == 0 || in->dst.kind != MIR_OPK_LABEL) {
    return 1;
  }
  prev--;
  if (th->fn->insns[prev].op == MIR_JMP || th->fn->insns[prev].op == MIR_JMP_TABLE ||
      th->fn->insns[prev].op == MIR_RET || th->fn->insns[prev].op == MIR_TRAP) {
    return 1;
  }
  if (!mir_thread_site_trace(th, prev, in->dst.sym, &tr)) {
    return !th->failed;
  }
  return mir_thread_emit_trace(th, &tr);
}

static int mir_thread_one(MirThread *th, size_t at) {
  const MirInst *in = &th->fn->insns[at];
  MirTrace tr;
  if (in->op == MIR_LABEL && !mir_thread_fallthrough(th, at, in)) {
    return 0;
  }
  if ((in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR ||
       in->op == MIR_FCMPBR) &&
      in->dst.kind == MIR_OPK_LABEL &&
      mir_thread_site_trace(th, at, in->dst.sym, &tr)) {
    if (in->op == MIR_JMP) {
      return mir_thread_emit_trace(th, &tr);
    }
    return mir_thread_conditional(th, in, &tr);
  }
  if (th->failed) {
    return 0;
  }
  return mir_thread_emit(th, in);
}

void mir_thread_dispatch(MirFunction *fn) {
  MirThread th;
  int any = 0;
  if (!fn || fn->insn_count < 3 || fn->vreg_count == 0) {
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    any |= fn->insns[i].op == MIR_JMP_TABLE;
  }
  if (!any) {
    return;
  }
  memset(&th, 0, sizeof(th));
  th.fn = fn;
  if (!mir_cfg_build(&th.cfg, fn)) {
    return;
  }
  th.capacity = fn->insn_count * 2u;
  th.out = (MirInst *)malloc(th.capacity * sizeof(MirInst));
  th.stack = (int *)malloc(th.cfg.block_count * sizeof(int));
  th.failed = !th.out || !th.stack;
  for (size_t i = 0; !th.failed && i < fn->insn_count; i++) {
    mir_thread_one(&th, i);
  }
  mir_cfg_free(&th.cfg);
  for (size_t k = 0; k < th.reach_count; k++) {
    free(th.reach[k].seen);
  }
  free(th.reach);
  free(th.stack);
  if (th.failed) {
    free(th.out);
    fn->has_error = 1;
    return;
  }
  if (th.added == 0) {
    free(th.out);
    return;
  }
  free(fn->insns);
  fn->insns = th.out;
  fn->insn_count = th.count;
  fn->insn_capacity = th.capacity;
  fn->code_moved_after_allocation = 1;
  free(fn->label_slots);
  fn->label_slots = NULL;
  fn->label_slot_capacity = 0;
  fn->label_slot_insns = 0;
}
