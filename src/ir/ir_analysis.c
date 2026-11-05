#include "ir_analysis.h"

#include "../common.h"
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IRAnalysis *ir_function_analysis_raw(IRFunction *function);
static const IRDomTree *ir_analysis_require_dom(IRFunction *function);
static const IRUseDefs *ir_analysis_require_use_defs(IRFunction *function);
static const IRJumpIndex *ir_analysis_require_jumps(IRFunction *function);
static const IRDestIndex *ir_analysis_require_dests(IRFunction *function);

static void ir_dom_destroy(IRDomTree *dom) {
  if (!dom) {
    return;
  }
  free(dom->idom);
  free(dom->rpo_index);
  free(dom->order);
  free(dom->child_head);
  free(dom->child_next);
  free(dom->enter);
  free(dom->leave);
  free(dom->frontier);
  free(dom->frontier_start);
  free(dom->frontier_count);
  memset(dom, 0, sizeof(*dom));
}

static void ir_use_defs_destroy(IRUseDefs *ud) {
  if (!ud) {
    return;
  }
  free(ud->def_first);
  free(ud->def_count);
  free(ud->defs);
  free(ud->uses);
  free(ud->use_start);
  free(ud->use_count);
  memset(ud, 0, sizeof(*ud));
}

static void ir_dest_index_destroy(IRDestIndex *dests) {
  if (!dests) {
    return;
  }
  free(dests->starts);
  free(dests->counts);
  free(dests->sites);
  free(dests->prev_label);
  memset(dests, 0, sizeof(*dests));
}

static void ir_jump_index_destroy(IRJumpIndex *jumps) {
  if (!jumps) {
    return;
  }
  free(jumps->starts);
  free(jumps->counts);
  free(jumps->targets);
  memset(jumps, 0, sizeof(*jumps));
}

static void ir_analysis_destroy(IRAnalysis *analysis) {
  if (!analysis) {
    return;
  }
  ir_dom_destroy(&analysis->dom);
  ir_use_defs_destroy(&analysis->ud);
  ir_jump_index_destroy(&analysis->jumps);
  ir_dest_index_destroy(&analysis->dests);
  if (analysis->labels) {
    ir_value_table_clear(analysis->labels);
    free(analysis->labels);
    analysis->labels = NULL;
  }
  free(analysis->instruction_block);
  analysis->instruction_block = NULL;
  analysis->instruction_count = 0;
  analysis->valid = 0;
}

void ir_function_release_analysis(IRFunction *function) {
  if (!function || !function->analysis) {
    return;
  }
  ir_analysis_destroy((IRAnalysis *)function->analysis);
  free(function->analysis);
  function->analysis = NULL;
}

static int ir_dom_allocate(IRDomTree *dom, size_t block_count) {
  dom->idom = (size_t *)malloc(block_count * sizeof(size_t));
  dom->rpo_index = (size_t *)malloc(block_count * sizeof(size_t));
  dom->order = (size_t *)malloc(block_count * sizeof(size_t));
  dom->child_head = (size_t *)malloc(block_count * sizeof(size_t));
  dom->child_next = (size_t *)malloc(block_count * sizeof(size_t));
  dom->enter = (size_t *)malloc(block_count * sizeof(size_t));
  dom->leave = (size_t *)malloc(block_count * sizeof(size_t));
  if (!dom->idom || !dom->rpo_index || !dom->order || !dom->child_head ||
      !dom->child_next || !dom->enter || !dom->leave) {
    return 0;
  }
  for (size_t i = 0; i < block_count; i++) {
    dom->idom[i] = IR_BLOCK_NONE;
    dom->rpo_index[i] = IR_BLOCK_NONE;
    dom->child_head[i] = IR_BLOCK_NONE;
    dom->child_next[i] = IR_BLOCK_NONE;
    dom->enter[i] = 0;
    dom->leave[i] = 0;
  }
  return 1;
}

static size_t ir_dom_postorder(const IRBasicBlock *blocks, size_t block_count,
                               size_t entry, size_t *stack, size_t *next_succ,
                               unsigned char *seen, size_t *postorder) {
  size_t post_count = 0;
  size_t depth = 1;
  stack[0] = entry;
  next_succ[0] = 0;
  seen[entry] = 1;
  while (depth > 0) {
    const size_t block = stack[depth - 1];
    if (next_succ[depth - 1] < blocks[block].successor_count) {
      const size_t successor = blocks[block].successors[next_succ[depth - 1]++];
      if (successor < block_count && !seen[successor]) {
        seen[successor] = 1;
        stack[depth] = successor;
        next_succ[depth] = 0;
        depth++;
      }
      continue;
    }
    postorder[post_count++] = block;
    depth--;
  }
  return post_count;
}

static int ir_dom_order_blocks(IRDomTree *dom, const IRBasicBlock *blocks,
                               size_t block_count, size_t entry,
                               size_t *stack) {
  size_t *next_succ = (size_t *)malloc(block_count * sizeof(size_t));
  unsigned char *seen = (unsigned char *)calloc(block_count, 1);
  size_t *postorder = (size_t *)malloc(block_count * sizeof(size_t));
  if (!next_succ || !seen || !postorder) {
    free(next_succ);
    free(seen);
    free(postorder);
    return 0;
  }

  const size_t post_count = ir_dom_postorder(blocks, block_count, entry, stack,
                                             next_succ, seen, postorder);
  dom->order_count = post_count;
  for (size_t i = 0; i < post_count; i++) {
    dom->order[i] = postorder[post_count - 1 - i];
    dom->rpo_index[dom->order[i]] = i;
  }

  free(postorder);
  free(next_succ);
  free(seen);
  return 1;
}

static void ir_dom_compress(size_t *ancestor, size_t *label,
                            const size_t *semi, size_t *path, size_t v) {
  size_t top = 0;
  while (ancestor[ancestor[v]] != IR_BLOCK_NONE) {
    path[top++] = v;
    v = ancestor[v];
  }
  while (top > 0) {
    const size_t x = path[--top];
    if (semi[label[ancestor[x]]] < semi[label[x]]) {
      label[x] = label[ancestor[x]];
    }
    ancestor[x] = ancestor[ancestor[x]];
  }
}

static int ir_dom_compute_idoms(IRDomTree *dom, const IRBasicBlock *blocks,
                                size_t block_count, size_t entry) {
  size_t *pre = (size_t *)malloc(block_count * sizeof(size_t));
  size_t *vert = (size_t *)malloc(block_count * sizeof(size_t));
  size_t *parent = (size_t *)malloc(block_count * sizeof(size_t));
  size_t *semi = (size_t *)malloc(block_count * sizeof(size_t));
  size_t *label = (size_t *)malloc(block_count * sizeof(size_t));
  size_t *ancestor = (size_t *)malloc(block_count * sizeof(size_t));
  size_t *idom = (size_t *)malloc(block_count * sizeof(size_t));
  size_t *stack = (size_t *)malloc(block_count * sizeof(size_t));
  size_t *next_succ = (size_t *)malloc(block_count * sizeof(size_t));
  int ok = pre && vert && parent && semi && label && ancestor && idom &&
           stack && next_succ;
  if (ok) {
    for (size_t i = 0; i < block_count; i++) {
      pre[i] = IR_BLOCK_NONE;
    }
    size_t n = 0;
    size_t depth = 1;
    stack[0] = entry;
    next_succ[0] = 0;
    pre[entry] = n;
    vert[n] = entry;
    parent[n] = IR_BLOCK_NONE;
    n++;
    while (depth > 0) {
      const size_t block = stack[depth - 1];
      if (next_succ[depth - 1] < blocks[block].successor_count) {
        const size_t successor =
            blocks[block].successors[next_succ[depth - 1]++];
        if (successor < block_count && pre[successor] == IR_BLOCK_NONE) {
          pre[successor] = n;
          vert[n] = successor;
          parent[n] = pre[block];
          n++;
          stack[depth] = successor;
          next_succ[depth] = 0;
          depth++;
        }
        continue;
      }
      depth--;
    }
    for (size_t i = 0; i < n; i++) {
      semi[i] = i;
      label[i] = i;
      ancestor[i] = IR_BLOCK_NONE;
    }
    for (size_t w = n; w-- > 1;) {
      const IRBasicBlock *bb = &blocks[vert[w]];
      for (size_t p = 0; p < bb->predecessor_count; p++) {
        const size_t pred = bb->predecessors[p];
        if (pred >= block_count || pre[pred] == IR_BLOCK_NONE) {
          continue;
        }
        size_t v = pre[pred];
        if (ancestor[v] != IR_BLOCK_NONE) {
          ir_dom_compress(ancestor, label, semi, stack, v);
          v = label[v];
        }
        if (semi[v] < semi[w]) {
          semi[w] = semi[v];
        }
      }
      ancestor[w] = parent[w];
    }
    idom[0] = 0;
    for (size_t w = 1; w < n; w++) {
      size_t d = parent[w];
      while (d > semi[w]) {
        d = idom[d];
      }
      idom[w] = d;
    }
    for (size_t w = 0; w < n; w++) {
      dom->idom[vert[w]] = vert[idom[w]];
    }
  }
  free(pre);
  free(vert);
  free(parent);
  free(semi);
  free(label);
  free(ancestor);
  free(idom);
  free(stack);
  free(next_succ);
  return ok;
}

static void ir_dom_build_child_lists(IRDomTree *dom, size_t entry) {
  for (size_t k = dom->order_count; k-- > 0;) {
    const size_t block = dom->order[k];
    if (block == entry || dom->idom[block] == IR_BLOCK_NONE) {
      continue;
    }
    const size_t parent = dom->idom[block];
    dom->child_next[block] = dom->child_head[parent];
    dom->child_head[parent] = block;
  }
}

static int ir_dom_number_tree(IRDomTree *dom, size_t block_count, size_t entry,
                              size_t *stack) {
  size_t *child_cursor = (size_t *)malloc(block_count * sizeof(size_t));
  if (!child_cursor) {
    return 0;
  }
  for (size_t i = 0; i < block_count; i++) {
    child_cursor[i] = dom->child_head[i];
  }

  size_t clock = 0;
  size_t sp = 1;
  stack[0] = entry;
  dom->enter[entry] = clock++;
  while (sp > 0) {
    const size_t block = stack[sp - 1];
    if (child_cursor[block] != IR_BLOCK_NONE) {
      const size_t child = child_cursor[block];
      child_cursor[block] = dom->child_next[child];
      dom->enter[child] = clock++;
      stack[sp++] = child;
      continue;
    }
    dom->leave[block] = clock++;
    sp--;
  }

  free(child_cursor);
  return 1;
}

static int ir_dom_frontier_pass(IRDomTree *dom, const IRBasicBlock *blocks,
                                size_t block_count, size_t *counts) {
  size_t *stamp = (size_t *)malloc(block_count * sizeof(size_t));
  if (!stamp) {
    return 0;
  }
  for (size_t i = 0; i < block_count; i++) {
    stamp[i] = IR_BLOCK_NONE;
  }
  for (size_t block = 0; block < block_count; block++) {
    if (blocks[block].predecessor_count < 2) {
      continue;
    }
    const size_t stop = dom->idom[block];
    for (size_t p = 0; p < blocks[block].predecessor_count; p++) {
      size_t runner = blocks[block].predecessors[p];
      if (runner >= block_count || dom->idom[runner] == IR_BLOCK_NONE) {
        continue;
      }
      while (runner != stop && runner != IR_BLOCK_NONE) {
        if (stamp[runner] == block) {
          break;
        }
        stamp[runner] = block;
        if (counts) {
          counts[runner]++;
        } else {
          dom->frontier[dom->frontier_start[runner] +
                        dom->frontier_count[runner]] = block;
          dom->frontier_count[runner]++;
        }
        if (runner == dom->idom[runner]) {
          break;
        }
        runner = dom->idom[runner];
      }
    }
  }
  free(stamp);
  return 1;
}

static int ir_dom_build_frontiers(IRDomTree *dom, const IRBasicBlock *blocks,
                                  size_t block_count) {
  size_t *counts = (size_t *)calloc(block_count, sizeof(size_t));
  if (!counts) {
    return 0;
  }
  if (!ir_dom_frontier_pass(dom, blocks, block_count, counts)) {
    free(counts);
    return 0;
  }

  dom->frontier_start = (size_t *)malloc(block_count * sizeof(size_t));
  dom->frontier_count = (size_t *)calloc(block_count, sizeof(size_t));
  if (!dom->frontier_start || !dom->frontier_count) {
    free(counts);
    return 0;
  }
  size_t total = 0;
  for (size_t i = 0; i < block_count; i++) {
    dom->frontier_start[i] = total;
    total += counts[i];
  }
  free(counts);

  dom->frontier_total = total;
  dom->frontier = total ? (size_t *)malloc(total * sizeof(size_t)) : NULL;
  if (total && !dom->frontier) {
    return 0;
  }

  return ir_dom_frontier_pass(dom, blocks, block_count, NULL);
}

static int ir_dom_build(IRDomTree *dom, const IRBasicBlock *blocks,
                        size_t block_count, size_t entry) {
  memset(dom, 0, sizeof(*dom));
  if (block_count == 0 || entry >= block_count) {
    return 0;
  }

  dom->block_count = block_count;
  dom->entry = entry;
  if (!ir_dom_allocate(dom, block_count)) {
    ir_dom_destroy(dom);
    return 0;
  }

  size_t *stack = (size_t *)malloc(block_count * sizeof(size_t));
  if (!stack || !ir_dom_order_blocks(dom, blocks, block_count, entry, stack)) {
    free(stack);
    ir_dom_destroy(dom);
    return 0;
  }

  if (!ir_dom_compute_idoms(dom, blocks, block_count, entry)) {
    free(stack);
    ir_dom_destroy(dom);
    return 0;
  }
  ir_dom_build_child_lists(dom, entry);

  const int numbered = ir_dom_number_tree(dom, block_count, entry, stack);
  free(stack);
  if (!numbered || !ir_dom_build_frontiers(dom, blocks, block_count)) {
    ir_dom_destroy(dom);
    return 0;
  }

  dom->built = 1;
  return 1;
}

static const IROperand *ir_analysis_operand_at(const IRInstruction *instruction,
                                               size_t index) {
  switch (index) {
  case 0:
    return &instruction->dest;
  case 1:
    return &instruction->lhs;
  case 2:
    return &instruction->rhs;
  default:
    break;
  }
  const size_t argument = index - 3;
  if (!instruction->arguments || argument >= instruction->argument_count) {
    return NULL;
  }
  return &instruction->arguments[argument];
}

static const IROperand *ir_use_defs_value_at(const IRInstruction *instruction,
                                             size_t index,
                                             size_t value_count) {
  const IROperand *operand = ir_analysis_operand_at(instruction, index);
  if (!operand || !ir_operand_is_value(operand) ||
      operand->value_id == IR_VALUE_ID_NONE ||
      operand->value_id >= value_count) {
    return NULL;
  }
  return operand;
}

static uint32_t ir_use_defs_written_value(const IRInstruction *instruction,
                                          int writes, size_t value_count) {
  if (!writes || !ir_operand_is_value(&instruction->dest) ||
      instruction->dest.value_id == IR_VALUE_ID_NONE ||
      instruction->dest.value_id >= value_count) {
    return IR_VALUE_ID_NONE;
  }
  return instruction->dest.value_id;
}

static int ir_use_defs_allocate(IRUseDefs *ud, size_t value_count) {
  ud->def_first = (uint32_t *)malloc(value_count * sizeof(uint32_t));
  ud->def_count = (size_t *)calloc(value_count, sizeof(size_t));
  ud->use_start = (size_t *)calloc(value_count, sizeof(size_t));
  ud->use_count = (size_t *)calloc(value_count, sizeof(size_t));
  if (!ud->def_first || !ud->def_count || !ud->use_start || !ud->use_count) {
    return 0;
  }
  for (size_t i = 0; i < value_count; i++) {
    ud->def_first[i] = IR_INSTRUCTION_NONE;
  }
  return 1;
}

static void ir_use_defs_count(IRUseDefs *ud, const IRFunction *function,
                              size_t value_count, size_t *def_total,
                              size_t *use_total) {
  *def_total = 0;
  *use_total = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    const int writes = ir_instruction_writes_destination(instruction);
    const uint32_t written =
        ir_use_defs_written_value(instruction, writes, value_count);
    if (written != IR_VALUE_ID_NONE) {
      ud->def_count[written]++;
      (*def_total)++;
    }
    const size_t operands = 3 + instruction->argument_count;
    for (size_t j = writes ? 1 : 0; j < operands; j++) {
      const IROperand *operand =
          ir_use_defs_value_at(instruction, j, value_count);
      if (!operand) {
        continue;
      }
      ud->use_count[operand->value_id]++;
      (*use_total)++;
    }
  }
}

static void ir_use_defs_fill(IRUseDefs *ud, const IRFunction *function,
                             size_t value_count, const size_t *def_start,
                             size_t *def_fill, size_t *use_fill) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    const int writes = ir_instruction_writes_destination(instruction);
    const uint32_t written =
        ir_use_defs_written_value(instruction, writes, value_count);
    if (written != IR_VALUE_ID_NONE) {
      ud->defs[def_start[written] + def_fill[written]] = (uint32_t)i;
      if (def_fill[written] == 0) {
        ud->def_first[written] = (uint32_t)i;
      }
      def_fill[written]++;
    }
    const size_t operands = 3 + instruction->argument_count;
    for (size_t j = writes ? 1 : 0; j < operands; j++) {
      const IROperand *operand =
          ir_use_defs_value_at(instruction, j, value_count);
      if (!operand) {
        continue;
      }
      const uint32_t id = operand->value_id;
      IRValueUse *slot = &ud->uses[ud->use_start[id] + use_fill[id]];
      slot->instruction = (uint32_t)i;
      slot->operand = (uint32_t)j;
      use_fill[id]++;
    }
  }
}

static int ir_use_defs_build(IRUseDefs *ud, const IRFunction *function) {
  memset(ud, 0, sizeof(*ud));
  const size_t value_count = ir_value_table_count(&function->values) + 1;
  ud->value_count = value_count;
  if (!ir_use_defs_allocate(ud, value_count)) {
    ir_use_defs_destroy(ud);
    return 0;
  }

  size_t def_total = 0;
  size_t use_total = 0;
  ir_use_defs_count(ud, function, value_count, &def_total, &use_total);

  ud->def_total = def_total;
  ud->use_total = use_total;
  ud->defs = def_total ? (uint32_t *)malloc(def_total * sizeof(uint32_t)) : NULL;
  ud->uses = use_total ? (IRValueUse *)malloc(use_total * sizeof(IRValueUse))
                       : NULL;
  if ((def_total && !ud->defs) || (use_total && !ud->uses)) {
    ir_use_defs_destroy(ud);
    return 0;
  }

  size_t *def_start = (size_t *)calloc(value_count, sizeof(size_t));
  if (!def_start) {
    ir_use_defs_destroy(ud);
    return 0;
  }
  size_t running = 0;
  for (size_t i = 0; i < value_count; i++) {
    def_start[i] = running;
    running += ud->def_count[i];
  }
  running = 0;
  for (size_t i = 0; i < value_count; i++) {
    ud->use_start[i] = running;
    running += ud->use_count[i];
  }

  size_t *def_fill = (size_t *)calloc(value_count, sizeof(size_t));
  size_t *use_fill = (size_t *)calloc(value_count, sizeof(size_t));
  if (!def_fill || !use_fill) {
    free(def_start);
    free(def_fill);
    free(use_fill);
    ir_use_defs_destroy(ud);
    return 0;
  }

  ir_use_defs_fill(ud, function, value_count, def_start, def_fill, use_fill);

  for (size_t i = 0; i < value_count; i++) {
    ud->def_count[i] = def_fill[i];
  }
  free(def_start);
  free(def_fill);
  free(use_fill);
  ud->built = 1;
  return 1;
}

static int ir_jump_index_build(IRAnalysis *analysis, const IRFunction *function) {
  IRJumpIndex *jumps = &analysis->jumps;
  memset(jumps, 0, sizeof(*jumps));

  analysis->labels = (IRValueTable *)calloc(1, sizeof(IRValueTable));
  if (!analysis->labels) {
    return 0;
  }
  ir_value_table_init(analysis->labels);

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_JUMP || !in->text) {
      continue;
    }
    if (ir_value_table_intern(analysis->labels, (unsigned char)IR_OPERAND_LABEL,
                              in->text) == IR_VALUE_ID_NONE) {
      return 0;
    }
  }

  const size_t label_count = ir_value_table_count(analysis->labels) + 1;
  jumps->label_count = label_count;
  jumps->starts = (uint32_t *)calloc(label_count, sizeof(uint32_t));
  jumps->counts = (uint32_t *)calloc(label_count, sizeof(uint32_t));
  if (!jumps->starts || !jumps->counts) {
    return 0;
  }

  size_t total = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_JUMP || !in->text) {
      continue;
    }
    const uint32_t id = ir_value_table_lookup(
        analysis->labels, (unsigned char)IR_OPERAND_LABEL, in->text);
    if (id == IR_VALUE_ID_NONE || id >= label_count) {
      continue;
    }
    jumps->counts[id]++;
    total++;
  }

  jumps->total = total;
  jumps->targets = total ? (uint32_t *)malloc(total * sizeof(uint32_t)) : NULL;
  if (total && !jumps->targets) {
    return 0;
  }

  size_t running = 0;
  for (size_t i = 0; i < label_count; i++) {
    jumps->starts[i] = (uint32_t)running;
    running += jumps->counts[i];
    jumps->counts[i] = 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_JUMP || !in->text) {
      continue;
    }
    const uint32_t id = ir_value_table_lookup(
        analysis->labels, (unsigned char)IR_OPERAND_LABEL, in->text);
    if (id == IR_VALUE_ID_NONE || id >= label_count) {
      continue;
    }
    jumps->targets[jumps->starts[id] + jumps->counts[id]] = (uint32_t)i;
    jumps->counts[id]++;
  }

  jumps->built = 1;
  return 1;
}

static int ir_dest_index_build(IRAnalysis *analysis, const IRFunction *function) {
  IRDestIndex *dests = &analysis->dests;
  memset(dests, 0, sizeof(*dests));

  const size_t value_count = ir_value_table_count(&function->values) + 1;
  dests->value_count = value_count;
  dests->starts = (uint32_t *)calloc(value_count, sizeof(uint32_t));
  dests->counts = (uint32_t *)calloc(value_count, sizeof(uint32_t));
  dests->prev_label = (uint32_t *)malloc(
      (function->instruction_count + 1) * sizeof(uint32_t));
  if (!dests->starts || !dests->counts || !dests->prev_label) {
    ir_dest_index_destroy(dests);
    return 0;
  }

  uint32_t seen = IR_INSTRUCTION_NONE;
  size_t total = 0;
  int complete = 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    dests->prev_label[i] = seen;
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LABEL) {
      seen = (uint32_t)i;
    }
    const IROperand *dest = &in->dest;
    if (dest->kind != IR_OPERAND_TEMP || !dest->name) {
      continue;
    }
    if (dest->value_id == IR_VALUE_ID_NONE || dest->value_id >= value_count) {
      complete = 0;
      continue;
    }
    dests->counts[dest->value_id]++;
    total++;
  }
  dests->prev_label[function->instruction_count] = seen;

  if (!complete) {
    dests->built = 1;
    dests->complete = 0;
    dests->built_instruction_count = function->instruction_count;
    return 1;
  }

  dests->total = total;
  dests->sites = total ? (uint32_t *)malloc(total * sizeof(uint32_t)) : NULL;
  if (total && !dests->sites) {
    ir_dest_index_destroy(dests);
    return 0;
  }

  size_t running = 0;
  for (size_t i = 0; i < value_count; i++) {
    dests->starts[i] = (uint32_t)running;
    running += dests->counts[i];
    dests->counts[i] = 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IROperand *dest = &function->instructions[i].dest;
    if (dest->kind != IR_OPERAND_TEMP || !dest->name ||
        dest->value_id == IR_VALUE_ID_NONE || dest->value_id >= value_count) {
      continue;
    }
    const uint32_t id = dest->value_id;
    dests->sites[dests->starts[id] + dests->counts[id]] = (uint32_t)i;
    dests->counts[id]++;
  }

  dests->built = 1;
  dests->complete = 1;
  dests->built_instruction_count = function->instruction_count;
  return 1;
}

const IRInstruction *ir_function_temp_producer_before(const IRFunction *function,
                                                      size_t before_index,
                                                      const char *temp_name,
                                                      int *usable) {
  if (usable) {
    *usable = 0;
  }
  if (!function || !temp_name || before_index == 0 ||
      before_index > function->instruction_count) {
    return NULL;
  }
  const IRDestIndex *dests = ir_analysis_require_dests((IRFunction *)function);
  const IRAnalysis *analysis =
      ir_function_analysis_raw((IRFunction *)function);
  if (!dests || !analysis ||
      analysis->instruction_count != function->instruction_count) {
    return NULL;
  }
  if (!dests->complete) {
    return NULL;
  }
  const uint32_t id = ir_value_table_lookup(
      &function->values, (unsigned char)IR_OPERAND_TEMP, temp_name);
  if (usable) {
    *usable = 1;
  }
  if (id == IR_VALUE_ID_NONE || id >= dests->value_count) {
    return NULL;
  }
  const uint32_t barrier = dests->prev_label[before_index];
  const uint32_t start = dests->starts[id];
  uint32_t lo = 0;
  uint32_t hi = dests->counts[id];
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if ((size_t)dests->sites[start + mid] < before_index) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  for (uint32_t k = lo; k-- > 0;) {
    const uint32_t at = dests->sites[start + k];
    if (barrier != IR_INSTRUCTION_NONE && at <= barrier) {
      break;
    }
    const IRInstruction *candidate = &function->instructions[at];
    if (candidate->op == IR_OP_NOP) {
      continue;
    }
    if (candidate->dest.kind != IR_OPERAND_TEMP || !candidate->dest.name ||
        strcmp(candidate->dest.name, temp_name) != 0) {
      if (usable) {
        *usable = 0;
      }
      return NULL;
    }
    return candidate;
  }
  return NULL;
}

static size_t ir_scan_jump_to(const IRFunction *function, size_t after,
                              const char *label, int last) {
  size_t found = IR_BLOCK_NONE;
  for (size_t i = after + 1; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_JUMP && in->text && strcmp(in->text, label) == 0) {
      found = i;
      if (!last) {
        return found;
      }
    }
  }
  return found;
}

static int ir_jump_index_mode(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *setting = getenv("METTLE_JUMP_INDEX");
    if (!setting || !*setting) {
      cached = 0;
    } else if (strcmp(setting, "index") == 0) {
      cached = 1;
    } else if (strcmp(setting, "verify") == 0) {
      cached = 2;
    } else {
      cached = 1;
    }
  }
  return cached;
}

static void ir_jump_index_report(const IRFunction *function, const char *label,
                                 size_t after, size_t fast, size_t slow) {
  fprintf(stderr,
          "mettle: jump index disagrees for '%s' after %zu in '%s': index %zd, "
          "scan %zd\n",
          label, after, function->name ? function->name : "<unnamed>",
          (ptrdiff_t)fast, (ptrdiff_t)slow);
}

size_t ir_function_first_jump_to(const IRFunction *function, size_t after,
                                 const char *label) {
  const int mode = ir_jump_index_mode();
  if (mode == 0) {
    return ir_scan_jump_to(function, after, label, 0);
  }
  const IRJumpIndex *jumps = ir_analysis_require_jumps((IRFunction *)function);
  const IRAnalysis *analysis =
      ir_function_analysis_raw((IRFunction *)function);
  if (!jumps || !analysis || !analysis->labels || !label) {
    return IR_BLOCK_NONE;
  }
  const uint32_t id = ir_value_table_lookup(
      analysis->labels, (unsigned char)IR_OPERAND_LABEL, label);
  if (id == IR_VALUE_ID_NONE || id >= jumps->label_count) {
    return IR_BLOCK_NONE;
  }
  const uint32_t start = jumps->starts[id];
  const uint32_t count = jumps->counts[id];
  size_t fast = IR_BLOCK_NONE;
  for (uint32_t k = 0; k < count; k++) {
    const uint32_t at = jumps->targets[start + k];
    if ((size_t)at > after) {
      fast = (size_t)at;
      break;
    }
  }
  if (mode == 2) {
    const size_t slow = ir_scan_jump_to(function, after, label, 0);
    if (slow != fast) {
      ir_jump_index_report(function, label, after, fast, slow);
      return slow;
    }
  }
  return fast;
}

size_t ir_function_last_jump_to(const IRFunction *function, size_t after,
                                const char *label) {
  const int mode = ir_jump_index_mode();
  if (mode == 0) {
    return ir_scan_jump_to(function, after, label, 1);
  }
  const IRJumpIndex *jumps = ir_analysis_require_jumps((IRFunction *)function);
  const IRAnalysis *analysis =
      ir_function_analysis_raw((IRFunction *)function);
  if (!jumps || !analysis || !analysis->labels || !label) {
    return IR_BLOCK_NONE;
  }
  const uint32_t id = ir_value_table_lookup(
      analysis->labels, (unsigned char)IR_OPERAND_LABEL, label);
  if (id == IR_VALUE_ID_NONE || id >= jumps->label_count) {
    return IR_BLOCK_NONE;
  }
  const uint32_t start = jumps->starts[id];
  const uint32_t count = jumps->counts[id];
  size_t fast = IR_BLOCK_NONE;
  for (uint32_t k = count; k-- > 0;) {
    const uint32_t at = jumps->targets[start + k];
    if ((size_t)at > after) {
      fast = (size_t)at;
      break;
    }
  }
  if (mode == 2) {
    const size_t slow = ir_scan_jump_to(function, after, label, 1);
    if (slow != fast) {
      ir_jump_index_report(function, label, after, fast, slow);
      return slow;
    }
  }
  return fast;
}

static const IRDomTree *ir_analysis_require_dom(IRFunction *function) {
  IRAnalysis *analysis = ir_function_analysis_raw(function);
  if (!analysis) {
    return NULL;
  }
  if (analysis->dom.built &&
      analysis->dom_generation != function->structure_generation) {
    ir_dom_destroy(&analysis->dom);
  }
  if (!analysis->dom.built) {
    size_t block_count = 0;
    const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
    if (blocks && block_count > 0) {
      ir_dom_build(&analysis->dom, blocks, block_count, function->entry_block);
      analysis->dom_generation = function->structure_generation;
    }
  }
  return analysis->dom.built ? &analysis->dom : NULL;
}

static const IRUseDefs *ir_analysis_require_use_defs(IRFunction *function) {
  IRAnalysis *analysis = ir_function_analysis_raw(function);
  if (!analysis) {
    return NULL;
  }
  if (analysis->ud.built &&
      analysis->use_defs_generation != function->generation) {
    ir_use_defs_destroy(&analysis->ud);
  }
  if (!analysis->ud.built) {
    ir_use_defs_build(&analysis->ud, function);
    analysis->use_defs_generation = function->generation;
  }
  return analysis->ud.built ? &analysis->ud : NULL;
}

static const IRJumpIndex *ir_analysis_require_jumps(IRFunction *function) {
  IRAnalysis *analysis = ir_function_analysis_raw(function);
  if (!analysis) {
    return NULL;
  }
  if (analysis->jumps.built &&
      analysis->jumps_generation != function->structure_generation) {
    ir_jump_index_destroy(&analysis->jumps);
    if (analysis->labels) {
      ir_value_table_clear(analysis->labels);
      free(analysis->labels);
      analysis->labels = NULL;
    }
  }
  if (analysis->jumps.built &&
      (analysis->jumps_writes != g_ir_operand_writes ||
       analysis->jumps_generation != function->structure_generation)) {
    ir_jump_index_destroy(&analysis->jumps);
    if (analysis->labels) {
      ir_value_table_clear(analysis->labels);
      free(analysis->labels);
      analysis->labels = NULL;
    }
  }
  if (!analysis->jumps.built) {
    ir_jump_index_build(analysis, function);
    analysis->jumps_generation = function->structure_generation;
    analysis->jumps_writes = g_ir_operand_writes;
  }
  return analysis->jumps.built ? &analysis->jumps : NULL;
}

static const IRDestIndex *ir_analysis_require_dests(IRFunction *function) {
  IRAnalysis *analysis = ir_function_analysis_raw(function);
  if (!analysis) {
    return NULL;
  }
  if (analysis->dests.built &&
      (analysis->dests_generation != function->generation ||
       analysis->dests_writes != g_ir_operand_writes ||
       analysis->dests.built_instruction_count !=
           function->instruction_count)) {
    ir_dest_index_destroy(&analysis->dests);
  }
  if (!analysis->dests.built) {
    ir_dest_index_build(analysis, function);
    analysis->dests_generation = function->generation;
    analysis->dests_writes = g_ir_operand_writes;
  }
  return analysis->dests.built ? &analysis->dests : NULL;
}

const IRAnalysis *ir_function_analysis(IRFunction *function) {
  IRAnalysis *analysis = ir_function_analysis_raw(function);
  if (analysis) {
    ir_analysis_require_dom(function);
    ir_analysis_require_use_defs(function);
  }
  return analysis;
}

static size_t g_analysis_queries = 0;
static size_t g_analysis_rebuilds = 0;

void ir_analysis_report_stats(void) {
  if (!getenv("METTLE_ANALYSIS_STATS")) {
    return;
  }
  fprintf(stderr, "mettle: analysis queries %zu rebuilds %zu\n",
          g_analysis_queries, g_analysis_rebuilds);
}

static IRAnalysis *ir_function_analysis_raw(IRFunction *function) {
  if (!function) {
    return NULL;
  }

  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);

  g_analysis_queries++;
  IRAnalysis *analysis = (IRAnalysis *)function->analysis;
  if (!analysis) {
    analysis = (IRAnalysis *)calloc(1, sizeof(IRAnalysis));
    if (!analysis) {
      return NULL;
    }
    function->analysis = analysis;
  }

  const int structure_moved =
      !analysis->valid ||
      analysis->structure_generation != function->structure_generation ||
      analysis->instruction_count != function->instruction_count ||
      analysis->block_count != block_count;

  if (!structure_moved) {
    analysis->generation = function->generation;
    return analysis;
  }
  g_analysis_rebuilds++;

  free(analysis->instruction_block);
  analysis->instruction_block = NULL;
  analysis->block_count = block_count;
  analysis->generation = function->generation;
  analysis->structure_generation = function->structure_generation;
  analysis->instruction_count = function->instruction_count;
  analysis->valid = 1;

  if (!blocks || block_count == 0) {
    return analysis;
  }

  analysis->instruction_block =
      (size_t *)malloc(function->instruction_count * sizeof(size_t));
  if (analysis->instruction_block) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      analysis->instruction_block[i] = IR_BLOCK_NONE;
    }
    for (size_t b = 0; b < block_count; b++) {
      const size_t start = blocks[b].first_instruction;
      for (size_t k = 0; k < blocks[b].instruction_count; k++) {
        if (start + k < function->instruction_count) {
          analysis->instruction_block[start + k] = b;
        }
      }
    }
  }

  return analysis;
}

size_t ir_function_instruction_block(IRFunction *function, size_t index) {
  const IRAnalysis *analysis = ir_function_analysis(function);
  if (!analysis || !analysis->instruction_block ||
      index >= analysis->instruction_count) {
    return IR_BLOCK_NONE;
  }
  return analysis->instruction_block[index];
}

int ir_block_dominates(const IRAnalysis *analysis, size_t a, size_t b) {
  if (!analysis || !analysis->dom.built) {
    return 0;
  }
  const IRDomTree *dom = &analysis->dom;
  if (a >= dom->block_count || b >= dom->block_count) {
    return 0;
  }
  if (dom->rpo_index[a] == IR_BLOCK_NONE || dom->rpo_index[b] == IR_BLOCK_NONE) {
    return 0;
  }
  return dom->enter[a] <= dom->enter[b] && dom->leave[b] <= dom->leave[a];
}

int ir_function_block_dominates(IRFunction *function, size_t a, size_t b) {
  return ir_block_dominates(ir_function_analysis(function), a, b);
}

int ir_function_instruction_dominates(IRFunction *function, size_t a,
                                      size_t b) {
  const IRAnalysis *analysis = ir_function_analysis(function);
  if (!analysis || !analysis->instruction_block) {
    return 0;
  }
  if (a >= analysis->instruction_count || b >= analysis->instruction_count) {
    return 0;
  }
  const size_t block_a = analysis->instruction_block[a];
  const size_t block_b = analysis->instruction_block[b];
  if (block_a == IR_BLOCK_NONE || block_b == IR_BLOCK_NONE) {
    return 0;
  }
  if (block_a == block_b) {
    return a <= b;
  }
  return ir_block_dominates(analysis, block_a, block_b);
}

uint32_t ir_function_value_single_def(IRFunction *function, uint32_t id) {
  const IRAnalysis *analysis = ir_function_analysis(function);
  if (!analysis || !analysis->ud.built || id == IR_VALUE_ID_NONE ||
      id >= analysis->ud.value_count) {
    return IR_INSTRUCTION_NONE;
  }
  if (analysis->ud.def_count[id] != 1) {
    return IR_INSTRUCTION_NONE;
  }
  return analysis->ud.def_first[id];
}

const IRValueUse *ir_function_value_uses(IRFunction *function, uint32_t id,
                                         size_t *count_out) {
  const IRAnalysis *analysis = ir_function_analysis(function);
  if (count_out) {
    *count_out = 0;
  }
  if (!analysis || !analysis->ud.built || id == IR_VALUE_ID_NONE ||
      id >= analysis->ud.value_count) {
    return NULL;
  }
  if (count_out) {
    *count_out = analysis->ud.use_count[id];
  }
  if (analysis->ud.use_count[id] == 0 || !analysis->ud.uses) {
    return NULL;
  }
  return &analysis->ud.uses[analysis->ud.use_start[id]];
}

size_t ir_function_value_def_count(IRFunction *function, uint32_t id) {
  const IRAnalysis *analysis = ir_function_analysis(function);
  if (!analysis || !analysis->ud.built || id == IR_VALUE_ID_NONE ||
      id >= analysis->ud.value_count) {
    return 0;
  }
  return analysis->ud.def_count[id];
}

static const size_t *ir_function_dominance_frontier_fwd(IRFunction *function,
                                                        size_t block,
                                                        size_t *count_out);

size_t ir_analysis_self_check(IRFunction *function) {
  static MettleEnvOnce crosscheck;
  const char *setting = mettle_env_once(&crosscheck, "METTLE_DOM_CROSSCHECK");
  if (!setting || !*setting) {
    return 0;
  }
  const IRAnalysis *analysis = ir_function_analysis(function);
  if (!analysis || !analysis->dom.built) {
    return 0;
  }
  const IRDomTree *dom = &analysis->dom;
  size_t mismatches = 0;

  for (size_t b = 0; b < dom->block_count; b++) {
    if (dom->rpo_index[b] == IR_BLOCK_NONE) {
      continue;
    }
    for (size_t a = 0; a < dom->block_count; a++) {
      if (dom->rpo_index[a] == IR_BLOCK_NONE) {
        continue;
      }
      int walked = 0;
      size_t runner = b;
      while (1) {
        if (runner == a) {
          walked = 1;
          break;
        }
        if (dom->idom[runner] == IR_BLOCK_NONE ||
            dom->idom[runner] == runner) {
          break;
        }
        runner = dom->idom[runner];
      }
      if (walked != ir_block_dominates(analysis, a, b)) {
        mismatches++;
        if (mismatches <= 4) {
          fprintf(stderr,
                  "mettle: dominance self-check says block %zu %s block %zu in "
                  "'%s' while the parent walk disagrees\n",
                  a, ir_block_dominates(analysis, a, b) ? "dominates" : "misses",
                  b, function->name ? function->name : "<unnamed>");
        }
      }
    }
  }

  for (size_t b = 0; b < dom->block_count; b++) {
    size_t count = 0;
    const size_t *frontier =
        ir_function_dominance_frontier_fwd(function, b, &count);
    for (size_t k = 0; k < count; k++) {
      const size_t f = frontier[k];
      if (f == b) {
        continue;
      }
      if (ir_block_dominates(analysis, b, f) && dom->idom[f] != b) {
        mismatches++;
        fprintf(stderr,
                "mettle: dominance self-check has block %zu strictly "
                "dominating its own frontier member %zu in '%s'\n",
                b, f, function->name ? function->name : "<unnamed>");
      }
    }
  }

  if (mismatches == 0 && strcmp(setting, "verbose") == 0) {
    fprintf(stderr,
            "mettle: dominance self-check agrees on %zu blocks in '%s'\n",
            dom->block_count, function->name ? function->name : "<unnamed>");
  }
  return mismatches;
}

static const size_t *ir_function_dominance_frontier_fwd(IRFunction *function,
                                                        size_t block,
                                                        size_t *count_out) {
  return ir_function_dominance_frontier(function, block, count_out);
}

const size_t *ir_function_dominance_frontier(IRFunction *function, size_t block,
                                             size_t *count_out) {
  const IRAnalysis *analysis = ir_function_analysis(function);
  if (count_out) {
    *count_out = 0;
  }
  if (!analysis || !analysis->dom.built || block >= analysis->dom.block_count ||
      !analysis->dom.frontier) {
    return NULL;
  }
  if (count_out) {
    *count_out = analysis->dom.frontier_count[block];
  }
  if (analysis->dom.frontier_count[block] == 0) {
    return NULL;
  }
  return &analysis->dom.frontier[analysis->dom.frontier_start[block]];
}
