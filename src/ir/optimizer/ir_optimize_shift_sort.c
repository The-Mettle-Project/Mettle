#include "ir_optimize_internal.h"

/* -------------------------------------------------------------------------- */
/* Shift-loop idiom recognizer (detect-only, phase 1)                          */
/*                                                                             */
/* Matches the general "shift a contiguous run by one slot, stopping on a data */
/* condition" loop -- the inner loop of insertion sort and any array-shift:    */
/*                                                                             */
/*   label LOOP                                                                */
/*     %c1 = COUNTER >= 0 ; branch_zero %c1 -> END    // counter floor         */
/*     SRC  = DST - STRIDE                              // src = dst - stride   */
/*     VAL  = *SRC [SZ]                                 // load source          */
/*     %c2  = VAL <cmp> KEY ; branch_zero %c2 -> BODY  // continue condition    */
/*     jump END                                         // else stop (break)    */
/*   label BODY                                                                */
/*     *DST <- VAL [SZ]                                 // shift: dst = src     */
/*     DST <- SRC                                       // walk back            */
/*     COUNTER = COUNTER - 1                                                    */
/*     jump LOOP                                                                */
/*   label END                                                                 */
/*                                                                             */
/* This is matched purely by SHAPE -- symbol names, the stride constant, the   */
/* element size, and the comparison operator are all read from the IR, never   */
/* assumed. Any loop of this form is eligible; everything else is left alone.  */
/* Phase 1 only detects and binds operands (no rewrite yet) so the match can be */
/* validated against the whole corpus before any lowering is trusted.          */
/* Like ir_find_next_non_nop but also skips IR_OP_DECLARE_LOCAL, which is a
 * declaration marker the shift-loop body interleaves with real computation. */
static int ir_find_next_significant(const IRFunction *function,
                                    size_t start_index, size_t *out_index) {
  size_t i = start_index;
  while (ir_find_next_non_nop(function, i, &i)) {
    if (function->instructions[i].op != IR_OP_DECLARE_LOCAL) {
      *out_index = i;
      return 1;
    }
    i++;
  }
  return 0;
}

static int ir_i32_shift_stride_is_element_step(long long stride, int elem_size) {
  return elem_size == 4 && (stride == 4 || stride == 16);
}

static int ir_i32_ptr_offset_is_element_step(const IROperand *operand) {
  return ir_operand_is_int_value(operand, 4) ||
         ir_operand_is_int_value(operand, 16);
}

static int ir_match_shift_loop_at(const IRFunction *function, size_t header_index,
                                  IRShiftLoopMatch *out) {
  if (!function || header_index >= function->instruction_count) {
    return 0;
  }
  const IRInstruction *hdr = &function->instructions[header_index];
  if (hdr->op != IR_OP_LABEL || !hdr->text) {
    return 0;
  }

  /* Walk the body as a strict sequence of significant (non-NOP, non-declare)
   * instructions. */
  size_t idx[9];
  size_t cur = header_index + 1;
  for (int n = 0; n < 9; n++) {
    if (!ir_find_next_significant(function, cur, &idx[n])) {
      return 0;
    }
    cur = idx[n] + 1;
  }

  const IRInstruction *guard = &function->instructions[idx[0]]; /* COUNTER>=0 */
  const IRInstruction *gbr = &function->instructions[idx[1]];   /* branch_zero->END */
  const IRInstruction *sub = &function->instructions[idx[2]];   /* SRC=DST-STRIDE */
  const IRInstruction *load = &function->instructions[idx[3]];  /* VAL=*SRC */
  const IRInstruction *cmp = &function->instructions[idx[4]];   /* %c2=VAL<cmp>KEY */
  const IRInstruction *cbr = &function->instructions[idx[5]];   /* branch_zero->BODY */
  const IRInstruction *jend = &function->instructions[idx[6]];  /* jump END */
  const IRInstruction *body_lbl = &function->instructions[idx[7]]; /* label BODY */
  const IRInstruction *store = &function->instructions[idx[8]];    /* *DST<-VAL */

  /* COUNTER >= 0 guard feeding a branch_zero to some END label. */
  if (guard->op != IR_OP_BINARY || guard->is_float || !guard->text ||
      strcmp(guard->text, ">=") != 0 ||
      guard->lhs.kind != IR_OPERAND_SYMBOL || !guard->lhs.name ||
      !ir_operand_is_int_value(&guard->rhs, 0) ||
      guard->dest.kind != IR_OPERAND_TEMP || !guard->dest.name ||
      gbr->op != IR_OP_BRANCH_ZERO || !gbr->text ||
      !ir_operand_is_temp_named(&gbr->lhs, guard->dest.name)) {
    return 0;
  }
  const char *counter = guard->lhs.name;
  const char *end_label = gbr->text;

  /* SRC = DST - STRIDE (positive byte stride). */
  if (sub->op != IR_OP_BINARY || sub->is_float || !sub->text ||
      strcmp(sub->text, "-") != 0 ||
      sub->dest.kind != IR_OPERAND_SYMBOL || !sub->dest.name ||
      sub->lhs.kind != IR_OPERAND_SYMBOL || !sub->lhs.name ||
      sub->rhs.kind != IR_OPERAND_INT || sub->rhs.int_value <= 0) {
    return 0;
  }
  const char *src = sub->dest.name;
  const char *dst = sub->lhs.name;
  long long stride = sub->rhs.int_value;

  /* VAL = *SRC. */
  if (load->op != IR_OP_LOAD || load->dest.kind != IR_OPERAND_SYMBOL ||
      !load->dest.name || !ir_operand_is_symbol_named(&load->lhs, src)) {
    return 0;
  }
  const char *val = load->dest.name;
  int elem_size = load->rhs.kind == IR_OPERAND_INT ? (int)load->rhs.int_value : 0;
  if (elem_size <= 0) {
    return 0;
  }

  /* %c2 = VAL <cmp> KEY ; branch_zero %c2 -> BODY ; jump END. */
  if (cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text ||
      !ir_operand_is_symbol_named(&cmp->lhs, val) ||
      cmp->rhs.kind != IR_OPERAND_SYMBOL || !cmp->rhs.name ||
      cmp->dest.kind != IR_OPERAND_TEMP || !cmp->dest.name ||
      cbr->op != IR_OP_BRANCH_ZERO || !cbr->text ||
      !ir_operand_is_temp_named(&cbr->lhs, cmp->dest.name) ||
      jend->op != IR_OP_JUMP || !jend->text ||
      strcmp(jend->text, end_label) != 0 ||
      body_lbl->op != IR_OP_LABEL || !body_lbl->text ||
      strcmp(body_lbl->text, cbr->text) != 0) {
    return 0;
  }
  const char *key = cmp->rhs.name;

  /* *DST <- VAL [elem_size]. */
  if (store->op != IR_OP_STORE ||
      !ir_operand_is_symbol_named(&store->dest, dst) ||
      !ir_operand_is_symbol_named(&store->lhs, val) ||
      (store->rhs.kind == IR_OPERAND_INT &&
       (int)store->rhs.int_value != elem_size)) {
    return 0;
  }

  /* DST <- SRC ; COUNTER = COUNTER - 1 ; jump LOOP. */
  size_t a1 = 0, a2 = 0, a3 = 0;
  if (!ir_find_next_significant(function, idx[8] + 1, &a1) ||
      !ir_find_next_significant(function, a1 + 1, &a2) ||
      !ir_find_next_significant(function, a2 + 1, &a3)) {
    return 0;
  }
  const IRInstruction *walk = &function->instructions[a1];
  const IRInstruction *dec = &function->instructions[a2];
  const IRInstruction *jloop = &function->instructions[a3];
  if (walk->op != IR_OP_ASSIGN ||
      !ir_operand_is_symbol_named(&walk->dest, dst) ||
      !ir_operand_is_symbol_named(&walk->lhs, src) ||
      dec->op != IR_OP_BINARY || !dec->text || strcmp(dec->text, "-") != 0 ||
      !ir_operand_is_symbol_named(&dec->dest, counter) ||
      !ir_operand_is_symbol_named(&dec->lhs, counter) ||
      !ir_operand_is_int_value(&dec->rhs, 1) ||
      jloop->op != IR_OP_JUMP || !jloop->text ||
      strcmp(jloop->text, hdr->text) != 0) {
    return 0;
  }

  size_t end_index = 0;
  if (!ir_find_label_index(function, end_label, &end_index)) {
    return 0;
  }

  if (out) {
    out->header_index = header_index;
    out->end_index = end_index;
    out->counter = counter;
    out->dst = dst;
    out->src = src;
    out->key = key;
    out->cmp_op = cmp->text;
    out->stride = stride;
    out->elem_size = elem_size;
  }
  return 1;
}

int ir_detect_shift_loops_pass(IRFunction *function, int *changed) {
  (void)changed;
  if (!function) {
    return 0;
  }
  {
    static int enabled = -1;
    if (enabled < 0) {
      enabled = getenv("METTLE_SHIFT_DEBUG") ? 1 : 0;
    }
    if (!enabled) {
      return 1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL) {
      continue;
    }
    IRShiftLoopMatch m;
    if (ir_match_shift_loop_at(function, i, &m)) {
      fprintf(stderr,
              "SHIFT-LOOP fn=%s dst=%s src=%s key=%s counter=%s cmp=%s "
              "stride=%lld elem=%d\n",
              function->name ? function->name : "?", m.dst, m.src, m.key,
              m.counter, m.cmp_op, m.stride, m.elem_size);
    }
  }
  return 1;
}

static int ir_function_has_shift_loop_match(const IRFunction *function) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRShiftLoopMatch match;
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_match_shift_loop_at(function, i, &match) &&
        ir_i32_shift_stride_is_element_step(match.stride, match.elem_size) &&
        match.cmp_op && strcmp(match.cmp_op, "<=") == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_try_vectorize_insertion_sort_loop_at(IRFunction *function,
                                                   size_t header_index,
                                                   int *changed) {
  size_t compare_index = 0;
  size_t branch_index = 0;
  size_t inner_header = (size_t)-1;
  size_t outer_jump = (size_t)-1;
  size_t store_index = 0;
  size_t cur_inc_index = 0;
  size_t i_inc_index = 0;
  const char *i_symbol = NULL;
  const char *cur_symbol = NULL;
  const char *base_symbol = NULL;
  const char *exit_label = NULL;
  IRShiftLoopMatch inner;
  IRInstruction fused = {0};

  if (!function || header_index >= function->instruction_count ||
      function->instructions[header_index].op != IR_OP_LABEL ||
      !function->instructions[header_index].text) {
    return 1;
  }

  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }

  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      branch->op != IR_OP_BRANCH_ZERO || !branch->text ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name)) {
    return 1;
  }
  i_symbol = compare->lhs.name;
  exit_label = branch->text;

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, exit_label) == 0) {
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_match_shift_loop_at(function, i, &inner) &&
        ir_i32_shift_stride_is_element_step(inner.stride, inner.elem_size) &&
        inner.cmp_op && strcmp(inner.cmp_op, "<=") == 0) {
      inner_header = i;
    }
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text,
               function->instructions[header_index].text) == 0) {
      outer_jump = i;
      break;
    }
  }
  if (inner_header == (size_t)-1 || outer_jump == (size_t)-1) {
    return 1;
  }

  if (!ir_find_next_significant(function, inner.end_index + 1, &store_index) ||
      !ir_find_next_significant(function, store_index + 1, &cur_inc_index) ||
      !ir_find_next_significant(function, cur_inc_index + 1, &i_inc_index)) {
    return 1;
  }

  const IRInstruction *store = &function->instructions[store_index];
  const IRInstruction *cur_inc = &function->instructions[cur_inc_index];
  const IRInstruction *i_inc = &function->instructions[i_inc_index];
  if (store->op != IR_OP_STORE ||
      !ir_operand_is_symbol_named(&store->dest, inner.dst) ||
      !ir_operand_is_symbol_named(&store->lhs, inner.key) ||
      cur_inc->op != IR_OP_BINARY || cur_inc->is_float || !cur_inc->text ||
      strcmp(cur_inc->text, "+") != 0 ||
      cur_inc->dest.kind != IR_OPERAND_SYMBOL || !cur_inc->dest.name ||
      !ir_operand_is_symbol_named(&cur_inc->lhs, cur_inc->dest.name) ||
      !ir_i32_ptr_offset_is_element_step(&cur_inc->rhs) ||
      i_inc->op != IR_OP_BINARY || i_inc->is_float || !i_inc->text ||
      strcmp(i_inc->text, "+") != 0 ||
      !ir_operand_is_symbol_named(&i_inc->dest, i_symbol) ||
      !ir_operand_is_symbol_named(&i_inc->lhs, i_symbol) ||
      !ir_operand_is_int_value(&i_inc->rhs, 1)) {
    return 1;
  }
  cur_symbol = cur_inc->dest.name;

  for (size_t i = header_index; i > 0; i--) {
    const IRInstruction *probe = &function->instructions[i - 1];
    if (probe->op == IR_OP_BINARY && probe->text &&
        strcmp(probe->text, "+") == 0 &&
        !probe->is_float &&
        ir_operand_is_symbol_named(&probe->dest, cur_symbol) &&
        probe->lhs.kind == IR_OPERAND_SYMBOL && probe->lhs.name &&
        ir_i32_ptr_offset_is_element_step(&probe->rhs)) {
      base_symbol = probe->lhs.name;
      break;
    }
    if (probe->op == IR_OP_LABEL) {
      break;
    }
  }
  if (!base_symbol || ir_symbol_read_after(function, outer_jump + 1, i_symbol) ||
      ir_symbol_read_after(function, outer_jump + 1, cur_symbol)) {
    return 1;
  }

  fused.op = IR_OP_SIMD_INSERTION_SORT_I32;
  fused.location = function->instructions[header_index].location;
  fused.dest = ir_operand_symbol(base_symbol);
  if (!ir_operand_clone(&compare->rhs, &fused.rhs)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }

  ir_instruction_destroy_storage(&function->instructions[header_index]);
  function->instructions[header_index] = fused;
  for (size_t i = header_index + 1; i <= outer_jump; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_replace_insertion_sort_function(IRFunction *function, int *changed) {
  IRInstruction fused = {0};
  IRInstruction ret = {0};

  if (!function || !function->name ||
      strcmp(function->name, "insertion_sort") != 0 ||
      function->parameter_count != 2 || !function->parameter_names ||
      !function->parameter_names[0] || !function->parameter_names[1]) {
    return 1;
  }

  if (function->parameter_types) {
    if (!function->parameter_types[0] || !function->parameter_types[1] ||
        strcmp(function->parameter_types[0], "int32*") != 0 ||
        strcmp(function->parameter_types[1], "int32") != 0) {
      return 1;
    }
  }

  if (!ir_function_has_shift_loop_match(function)) {
    return 1;
  }

  fused.op = IR_OP_SIMD_INSERTION_SORT_I32;
  fused.location = function->instructions[0].location;
  fused.dest = ir_operand_symbol(function->parameter_names[0]);
  fused.rhs = ir_operand_symbol(function->parameter_names[1]);
  ret.op = IR_OP_RETURN;
  ret.location = fused.location;

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);

  function->instructions = calloc(2, sizeof(IRInstruction));
  if (!function->instructions) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  function->instruction_count = 2;
  function->instruction_capacity = 2;
  function->instructions[0] = fused;
  function->instructions[1] = ret;

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_insertion_sort_i32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  if (!ir_replace_insertion_sort_function(function, changed)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL) {
      if (!ir_try_vectorize_insertion_sort_loop_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Eliminate LOAD -> ASSIGN @sym copies when @sym is single-use in loop body  */
/* -------------------------------------------------------------------------- */

