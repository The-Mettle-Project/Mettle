#include "codegen/binary/mir_cfg.h"
#include "codegen/binary/mir_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIR_LAYOUT_MAX_BLOCKS 20000u
#define MIR_LAYOUT_FREQ_SWEEPS 24
#define MIR_LAYOUT_FREQ_CAP 1e12
#define MIR_LAYOUT_COLD_RATIO 0.002
#define MIR_LAYOUT_HOT_FALL_SHARE 0.75

typedef struct {
  int from;
  int to;
  double weight;
} MirLayoutEdge;

typedef struct {
  const MirFunction *fn;
  const MirCfg *cfg;
  double *freq;
  double *taken;
  unsigned char *cold;
  unsigned char *returns;
  unsigned char *calls;
  MirLayoutEdge *edges;
  size_t edge_count;
  int *chain_next;
  int *chain_prev;
  int *other_end;
  int *order;
  const char **labels;
  unsigned char *hot_fall;
} MirLayout;

static const MirInst *mir_block_last(const MirCfg *cfg, int b) {
  return &cfg->fn->insns[cfg->blocks[b].end - 1];
}

static int mir_is_conditional(const MirInst *in) {
  return mir_op_has(in->op, MIR_OPF_CONDITIONAL_BRANCH);
}

static int mir_target_block(const MirCfg *cfg, const MirInst *branch) {
  int at = mir_cfg_label_insn(cfg, branch->dst.sym);
  return at < 0 ? -1 : cfg->block_of[at];
}

static void mir_layout_classify_blocks(MirLayout *l) {
  const MirCfg *cfg = l->cfg;
  for (size_t b = 0; b < cfg->block_count; b++) {
    for (int i = cfg->blocks[b].start; i < cfg->blocks[b].end; i++) {
      const MirInst *in = &cfg->fn->insns[i];
      if (in->op == MIR_TRAP) {
        l->cold[b] = 1;
      }
      if (in->op == MIR_LABEL && in->dst.sym &&
          strncmp(in->dst.sym, ".mir_wrap_stub", 14) == 0) {
        l->cold[b] = 1;
      }
      if (in->op == MIR_RET) {
        l->returns[b] = 1;
      }
      if (mir_op_has(in->op, MIR_OPF_REAL_CALL)) {
        l->calls[b] = 1;
      }
    }
  }
}

static double mir_side_is_cold(const MirLayout *l, int taken, int fall) {
  if (l->cold[taken] != l->cold[fall]) {
    return l->cold[taken] ? MIR_LAYOUT_COLD_RATIO : 1.0 - MIR_LAYOUT_COLD_RATIO;
  }
  return -1.0;
}

static double mir_side_by_loop(const MirCfg *cfg, int from, int taken, int fall) {
  int taken_back = cfg->blocks[taken].rpo >= 0 &&
                   cfg->blocks[taken].rpo <= cfg->blocks[from].rpo;
  int fall_back = cfg->blocks[fall].rpo >= 0 &&
                  cfg->blocks[fall].rpo <= cfg->blocks[from].rpo;
  int depth = cfg->blocks[from].loop_depth;
  if (taken_back != fall_back) {
    return taken_back ? 0.88 : 0.12;
  }
  if (depth > 0 && cfg->blocks[taken].loop_depth < depth &&
      cfg->blocks[fall].loop_depth >= depth) {
    return 0.12;
  }
  if (depth > 0 && cfg->blocks[fall].loop_depth < depth &&
      cfg->blocks[taken].loop_depth >= depth) {
    return 0.88;
  }
  return -1.0;
}

static double mir_side_by_flags(const MirLayout *l, int taken, int fall,
                                const unsigned char *flags, double p) {
  (void)l;
  if (flags[taken] != flags[fall]) {
    return flags[taken] ? p : 1.0 - p;
  }
  return -1.0;
}

static double mir_side_by_compare(const MirInst *last) {
  if (last->cc == 0x84) {
    return 0.35;
  }
  if (last->cc == 0x85) {
    return 0.65;
  }
  return 0.5;
}

static double mir_taken_probability(const MirLayout *l, int from, int taken,
                                    int fall) {
  const MirCfg *cfg = l->cfg;
  double p = mir_side_is_cold(l, taken, fall);
  if (p >= 0.0) {
    return p;
  }
  p = mir_side_by_loop(cfg, from, taken, fall);
  if (p >= 0.0) {
    return p;
  }
  p = mir_side_by_flags(l, taken, fall, l->returns, 0.28);
  if (p >= 0.0) {
    return p;
  }
  p = mir_side_by_flags(l, taken, fall, l->calls, 0.29);
  if (p >= 0.0) {
    return p;
  }
  return mir_side_by_compare(mir_block_last(cfg, from));
}

static void mir_layout_probabilities(MirLayout *l) {
  const MirCfg *cfg = l->cfg;
  for (size_t b = 0; b < cfg->block_count; b++) {
    const MirInst *last = mir_block_last(cfg, (int)b);
    l->taken[b] = 0.5;
    if (mir_is_conditional(last) && b + 1 < cfg->block_count) {
      int target = mir_target_block(cfg, last);
      if (target >= 0 && target != (int)b + 1) {
        l->taken[b] = mir_taken_probability(l, (int)b, target, (int)b + 1);
      }
    }
  }
}

static double mir_edge_probability(const MirLayout *l, int from, int to,
                                   int edge_count) {
  const MirCfg *cfg = l->cfg;
  const MirInst *last = mir_block_last(cfg, from);
  if (!mir_is_conditional(last) || edge_count < 2) {
    return 1.0 / (double)(edge_count ? edge_count : 1);
  }
  if (to == mir_target_block(cfg, last)) {
    return l->taken[from];
  }
  return 1.0 - l->taken[from];
}

static int mir_succ_edges(const MirCfg *cfg, int b) {
  int n = 0;
  for (int e = cfg->blocks[b].succ_head; e >= 0; e = cfg->edge_next[e]) {
    n++;
  }
  return n;
}

static void mir_layout_frequencies(MirLayout *l) {
  const MirCfg *cfg = l->cfg;
  double *next = (double *)calloc(cfg->block_count, sizeof(double));
  if (!next) {
    return;
  }
  l->freq[0] = 1.0;
  for (int sweep = 0; sweep < MIR_LAYOUT_FREQ_SWEEPS; sweep++) {
    memset(next, 0, cfg->block_count * sizeof(double));
    next[0] = 1.0;
    for (size_t k = 0; k < cfg->rpo_count; k++) {
      int b = cfg->rpo_order[k];
      int n = mir_succ_edges(cfg, b);
      double out = l->freq[b] > next[b] ? l->freq[b] : next[b];
      if (out > MIR_LAYOUT_FREQ_CAP) {
        out = MIR_LAYOUT_FREQ_CAP;
      }
      for (int e = cfg->blocks[b].succ_head; e >= 0; e = cfg->edge_next[e]) {
        next[cfg->edge_to[e]] += out * mir_edge_probability(l, b, cfg->edge_to[e], n);
      }
    }
    memcpy(l->freq, next, cfg->block_count * sizeof(double));
  }
  free(next);
}

static int mir_layout_edge_order(const void *x, const void *y) {
  const MirLayoutEdge *a = (const MirLayoutEdge *)x;
  const MirLayoutEdge *b = (const MirLayoutEdge *)y;
  if (a->weight != b->weight) {
    return a->weight > b->weight ? -1 : 1;
  }
  if (a->from != b->from) {
    return a->from < b->from ? -1 : 1;
  }
  return (a->to > b->to) - (a->to < b->to);
}

static int mir_layout_collect_edges(MirLayout *l) {
  const MirCfg *cfg = l->cfg;
  size_t total = cfg->edge_count;
  l->edges = (MirLayoutEdge *)malloc((total ? total : 1u) * sizeof(MirLayoutEdge));
  if (!l->edges) {
    return 0;
  }
  for (size_t b = 0; b < cfg->block_count; b++) {
    const MirInst *last = mir_block_last(cfg, (int)b);
    int n = mir_succ_edges(cfg, (int)b);
    if (cfg->blocks[b].rpo < 0 || last->op == MIR_JMP_TABLE) {
      continue;
    }
    for (int e = cfg->blocks[b].succ_head; e >= 0; e = cfg->edge_next[e]) {
      int to = cfg->edge_to[e];
      double p = mir_edge_probability(l, (int)b, to, n);
      double weight = l->freq[b] * p;
      if (to == (int)b + 1) {
        weight *= 1.01;
      }
      if (to != 0 && to != (int)b && l->cold[to] == l->cold[b]) {
        l->edges[l->edge_count].from = (int)b;
        l->edges[l->edge_count].to = to;
        l->edges[l->edge_count].weight = weight;
        l->edge_count++;
      }
    }
  }
  qsort(l->edges, l->edge_count, sizeof(MirLayoutEdge), mir_layout_edge_order);
  return 1;
}

static void mir_layout_build_chains(MirLayout *l) {
  const MirCfg *cfg = l->cfg;
  for (size_t b = 0; b < cfg->block_count; b++) {
    l->chain_next[b] = -1;
    l->chain_prev[b] = -1;
    l->other_end[b] = (int)b;
  }
  for (size_t k = 0; k < l->edge_count; k++) {
    int from = l->edges[k].from;
    int to = l->edges[k].to;
    int head;
    int tail;
    if (l->chain_next[from] >= 0 || l->chain_prev[to] >= 0 ||
        l->other_end[from] == to) {
      continue;
    }
    head = l->other_end[from];
    tail = l->other_end[to];
    l->chain_next[from] = to;
    l->chain_prev[to] = from;
    l->other_end[head] = tail;
    l->other_end[tail] = head;
  }
}

typedef struct {
  int head;
  double heat;
  int cold;
} MirChainRank;

static int mir_chain_rank_order(const void *x, const void *y) {
  const MirChainRank *a = (const MirChainRank *)x;
  const MirChainRank *b = (const MirChainRank *)y;
  if (a->head == 0 || b->head == 0) {
    return a->head == 0 ? -1 : 1;
  }
  if (a->cold != b->cold) {
    return a->cold < b->cold ? -1 : 1;
  }
  if (a->heat != b->heat) {
    return a->heat > b->heat ? -1 : 1;
  }
  return a->head < b->head ? -1 : 1;
}

static int mir_layout_order_chains(MirLayout *l) {
  const MirCfg *cfg = l->cfg;
  MirChainRank *ranks =
      (MirChainRank *)malloc(cfg->block_count * sizeof(MirChainRank));
  size_t count = 0;
  size_t placed = 0;
  double entry_cut = l->freq[0] * MIR_LAYOUT_COLD_RATIO;
  if (!ranks) {
    return 0;
  }
  for (size_t b = 0; b < cfg->block_count; b++) {
    double heat = 0.0;
    int all_cold = 1;
    if (l->chain_prev[b] >= 0) {
      continue;
    }
    for (int m = (int)b; m >= 0; m = l->chain_next[m]) {
      if (l->freq[m] > heat) {
        heat = l->freq[m];
      }
      all_cold &= l->cold[m] || cfg->blocks[m].rpo < 0;
    }
    ranks[count].head = (int)b;
    ranks[count].heat = heat;
    ranks[count].cold = all_cold || heat < entry_cut;
    count++;
  }
  qsort(ranks, count, sizeof(MirChainRank), mir_chain_rank_order);
  for (size_t c = 0; c < count; c++) {
    for (int m = ranks[c].head; m >= 0; m = l->chain_next[m]) {
      l->order[placed++] = m;
    }
  }
  free(ranks);
  return 1;
}

static const char *mir_layout_label_of(MirLayout *l, MirFunction *fn, int b) {
  static unsigned long long counter = 0;
  const MirInst *first = &fn->insns[l->cfg->blocks[b].start];
  char *name;
  if (first->op == MIR_LABEL && first->dst.sym) {
    return first->dst.sym;
  }
  if (l->labels[b]) {
    return l->labels[b];
  }
  name = (char *)malloc(48);
  if (!name) {
    return NULL;
  }
  snprintf(name, 48, ".mir_block_%llu", ++counter);
  if (!mir_function_own_aux(fn, name)) {
    return NULL;
  }
  l->labels[b] = name;
  return name;
}

static int mir_emit_inst(MirInst **out, size_t *count, size_t *capacity,
                         const MirInst *in) {
  if (*count == *capacity) {
    size_t grown_capacity = *capacity * 2u;
    MirInst *grown = (MirInst *)realloc(*out, grown_capacity * sizeof(MirInst));
    if (!grown) {
      return 0;
    }
    *out = grown;
    *capacity = grown_capacity;
  }
  (*out)[(*count)++] = *in;
  return 1;
}

static int mir_block_falls_through(const MirInst *last) {
  return last->op != MIR_JMP && last->op != MIR_JMP_TABLE &&
         last->op != MIR_RET && last->op != MIR_TRAP;
}

static int mir_layout_mark_fall_labels(MirLayout *l, MirFunction *fn) {
  const MirCfg *cfg = l->cfg;
  for (size_t k = 0; k < cfg->block_count; k++) {
    int b = l->order[k];
    int next = k + 1 < cfg->block_count ? l->order[k + 1] : -1;
    const MirInst *last = mir_block_last(cfg, b);
    if (mir_block_falls_through(last) && (size_t)b + 1 < cfg->block_count &&
        b + 1 != next && !mir_layout_label_of(l, fn, b + 1)) {
      return 0;
    }
  }
  return 1;
}

static int mir_layout_emit_block(MirLayout *l, MirFunction *fn, int b, int next,
                                 MirInst **out, size_t *count, size_t *capacity) {
  const MirCfg *cfg = l->cfg;
  const MirCfgBlock *blk = &cfg->blocks[b];
  const MirInst *last_ptr = &fn->insns[blk->end - 1];
  MirInst last = *last_ptr;
  int fall = (size_t)b + 1 < cfg->block_count ? b + 1 : -1;
  int emit_jump = 0;
  if (l->labels[b]) {
    MirInst label;
    memset(&label, 0, sizeof(label));
    label.op = MIR_LABEL;
    label.dst = mir_op_label(l->labels[b]);
    label.width = 8;
    label.ir_index = fn->insns[blk->start].ir_index;
    label.hot_fall = l->hot_fall[b];
    if (!mir_emit_inst(out, count, capacity, &label)) {
      return 0;
    }
  }
  for (int i = blk->start; i < blk->end - 1; i++) {
    if (!mir_emit_inst(out, count, capacity, &fn->insns[i])) {
      return 0;
    }
  }
  if (mir_is_conditional(&last) && fall >= 0 && fall != next &&
      mir_target_block(cfg, &last) == next) {
    last.cc ^= 1;
    last.dst = mir_op_label(mir_layout_label_of(l, fn, fall));
  } else if (mir_block_falls_through(&last) && fall >= 0 && fall != next) {
    emit_jump = 1;
  }
  if (!mir_emit_inst(out, count, capacity, &last)) {
    return 0;
  }
  if (emit_jump) {
    MirInst jump;
    memset(&jump, 0, sizeof(jump));
    jump.op = MIR_JMP;
    jump.dst = mir_op_label(mir_layout_label_of(l, fn, fall));
    jump.width = 8;
    jump.ir_index = last.ir_index;
    return mir_emit_inst(out, count, capacity, &jump);
  }
  return 1;
}

static int mir_layout_rewrite(MirLayout *l, MirFunction *fn) {
  const MirCfg *cfg = l->cfg;
  size_t capacity = fn->insn_count * 2u + 16u;
  size_t count = 0;
  MirInst *out = (MirInst *)malloc(capacity * sizeof(MirInst));
  if (!out || !mir_layout_mark_fall_labels(l, fn)) {
    free(out);
    return 0;
  }
  for (size_t k = 0; k < cfg->block_count; k++) {
    int next = k + 1 < cfg->block_count ? l->order[k + 1] : -1;
    if (!mir_layout_emit_block(l, fn, l->order[k], next, &out, &count,
                               &capacity)) {
      free(out);
      return 0;
    }
  }
  free(fn->insns);
  fn->insns = out;
  fn->insn_count = count;
  fn->insn_capacity = capacity;
  fn->code_moved_after_allocation = 1;
  free(fn->label_slots);
  fn->label_slots = NULL;
  fn->label_slot_capacity = 0;
  fn->label_slot_insns = 0;
  return 1;
}

static int mir_layout_order_is_identity(const MirLayout *l) {
  for (size_t k = 0; k < l->cfg->block_count; k++) {
    if (l->order[k] != (int)k) {
      return 0;
    }
  }
  return 1;
}

static int mir_layout_allocate(MirLayout *l, size_t n) {
  l->freq = (double *)calloc(n, sizeof(double));
  l->taken = (double *)calloc(n, sizeof(double));
  l->cold = (unsigned char *)calloc(n, 1);
  l->returns = (unsigned char *)calloc(n, 1);
  l->calls = (unsigned char *)calloc(n, 1);
  l->chain_next = (int *)malloc(n * sizeof(int));
  l->chain_prev = (int *)malloc(n * sizeof(int));
  l->other_end = (int *)malloc(n * sizeof(int));
  l->order = (int *)malloc(n * sizeof(int));
  l->labels = (const char **)calloc(n, sizeof(const char *));
  l->hot_fall = (unsigned char *)calloc(n, 1);
  return l->freq && l->taken && l->cold && l->returns && l->calls &&
         l->chain_next && l->chain_prev && l->other_end && l->order &&
         l->labels && l->hot_fall;
}

static void mir_layout_free(MirLayout *l) {
  free(l->freq);
  free(l->taken);
  free(l->cold);
  free(l->returns);
  free(l->calls);
  free(l->edges);
  free(l->chain_next);
  free(l->chain_prev);
  free(l->other_end);
  free(l->order);
  free((void *)l->labels);
  free(l->hot_fall);
}

static double mir_layout_fall_weight(const MirLayout *l, int from, int to) {
  const MirCfg *cfg = l->cfg;
  int n = mir_succ_edges(cfg, from);
  if (mir_block_last(cfg, from)->op == MIR_JMP_TABLE) {
    return 0.0;
  }
  for (int e = cfg->blocks[from].succ_head; e >= 0; e = cfg->edge_next[e]) {
    if (cfg->edge_to[e] == to) {
      return l->freq[from] * mir_edge_probability(l, from, to, n);
    }
  }
  return 0.0;
}

static void mir_layout_mark_hot_falls(MirLayout *l, MirFunction *fn) {
  const MirCfg *cfg = l->cfg;
  for (size_t k = 1; k < cfg->block_count; k++) {
    int b = l->order[k];
    MirInst *first = &fn->insns[cfg->blocks[b].start];
    double into = mir_layout_fall_weight(l, l->order[k - 1], b);
    l->hot_fall[b] = into > l->freq[b] * MIR_LAYOUT_HOT_FALL_SHARE;
    if (first->op == MIR_LABEL) {
      first->hot_fall = l->hot_fall[b];
    }
  }
}

static int mir_layout_may_move(const MirFunction *fn) {
  int branches = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirOpcode op = fn->insns[i].op;
    if (op == MIR_INLINE_ASM) {
      return 0;
    }
    branches |= mir_op_has(op, MIR_OPF_LABEL_BRANCH);
  }
  return branches;
}

void mir_place_blocks(MirFunction *fn) {
  MirCfg cfg;
  MirLayout l;
  int ok;
  if (!fn || fn->insn_count < 4 || fn->vreg_count == 0 ||
      !mir_layout_may_move(fn) || !mir_cfg_build(&cfg, fn)) {
    return;
  }
  if (cfg.block_count < 3 || cfg.block_count > MIR_LAYOUT_MAX_BLOCKS) {
    mir_cfg_free(&cfg);
    return;
  }
  memset(&l, 0, sizeof(l));
  l.fn = fn;
  l.cfg = &cfg;
  ok = mir_layout_allocate(&l, cfg.block_count);
  if (ok) {
    mir_layout_classify_blocks(&l);
    mir_layout_probabilities(&l);
    mir_layout_frequencies(&l);
    ok = mir_layout_collect_edges(&l);
  }
  if (ok) {
    mir_layout_build_chains(&l);
    ok = mir_layout_order_chains(&l);
  }
  if (ok) {
    mir_layout_mark_hot_falls(&l, fn);
  }
  if (ok && !mir_layout_order_is_identity(&l)) {
    ok = mir_layout_rewrite(&l, fn);
  }
  mir_layout_free(&l);
  mir_cfg_free(&cfg);
  if (!ok) {
    fn->has_error = 1;
  }
}
