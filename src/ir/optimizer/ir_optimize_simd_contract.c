#include "ir_optimize_internal.h"
#include "common.h"

#include <stdio.h>
#include <string.h>

static int g_simd_contract_user_error = 0;
static int g_simd_report = 0;

void ir_optimize_reset_user_error(void) { g_simd_contract_user_error = 0; }

int ir_optimize_had_user_error(void) { return g_simd_contract_user_error; }

void ir_optimize_note_user_error(void) { g_simd_contract_user_error = 1; }

void ir_optimize_set_simd_report(int enabled) { g_simd_report = enabled; }

static MTLC_THREAD_LOCAL IRProgram *g_explain_program = NULL;

void ir_explain_set_program(IRProgram *program) { g_explain_program = program; }

const IRModuleSymbol *ir_optimize_module_symbol(const char *name) {
  if (!g_explain_program || !name) {
    return NULL;
  }
  return ir_program_lookup_symbol(g_explain_program, name);
}

static int ir_instruction_is_simd_marker(const IRInstruction *instruction) {
  return instruction && instruction->op == IR_OP_NOP && instruction->text &&
         strncmp(instruction->text, IR_SIMD_MARKER_PREFIX,
                 strlen(IR_SIMD_MARKER_PREFIX)) == 0;
}

static int ir_op_is_vectorized(IROpcode op) {
  return op >= IR_OP_COUNT_WORD_STARTS && op <= IR_OP_SIMD_OUTER_LANE_F64;
}

static const IRInstruction *ir_region_vectorized_ins(const IRFunction *function,
                                                     size_t begin, size_t end,
                                                     int any_depth) {
  int depth = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (ir_instruction_is_simd_marker(instruction)) {
      depth += (instruction->text[strlen(IR_SIMD_MARKER_PREFIX)] == 'B') ? 1
                                                                         : -1;
      continue;
    }
    if ((any_depth || depth == 0) && ir_op_is_vectorized(instruction->op)) {
      return instruction;
    }
  }
  return NULL;
}

static int ir_region_vectorized_op(const IRFunction *function, size_t begin,
                                   size_t end) {
  const IRInstruction *ins = ir_region_vectorized_ins(function, begin, end, 0);
  return ins ? (int)ins->op : -1;
}

static const IRInstruction *ir_region_skipahead_ins(const IRFunction *function,
                                                    size_t begin, size_t end) {
  const char *iv = NULL;
  for (size_t i = begin + 1; i < end && !iv; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL || !ir_label_is_while_header(ins->text)) {
      continue;
    }
    for (size_t k = i + 1; k < end; k++) {
      const IRInstruction *c = &function->instructions[k];
      if (c->op == IR_OP_NOP) {
        continue;
      }
      if (c->op == IR_OP_BINARY && c->lhs.kind == IR_OPERAND_SYMBOL &&
          c->lhs.name) {
        iv = c->lhs.name;
      }
      break;
    }
  }
  if (!iv) {
    return NULL;
  }
  for (size_t i = begin + 1; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_SIMD_FIND && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && strcmp(ins->dest.name, iv) == 0) {
      return ins;
    }
    if (ins->op == IR_OP_LABEL ||
        (ir_instruction_writes_destination(ins) &&
         ir_operand_is_symbol_named(&ins->dest, iv))) {
      return NULL;
    }
  }
  return NULL;
}

static int ir_region_has_loop_label(const IRFunction *function, size_t begin,
                                    size_t end) {
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text &&
        (strstr(ins->text, "ir_while_") != NULL ||
         strstr(ins->text, "ir_for_cond_") != NULL)) {
      return 1;
    }
  }
  return 0;
}

static int ir_label_is_loop_header(const char *label);

static size_t ir_region_loop_header_count(const IRFunction *function,
                                          size_t begin, size_t end) {
  size_t count = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_label_is_loop_header(ins->text)) {
      count++;
    }
  }
  return count;
}

static int ir_label_is_loop_header(const char *label) {
  if (!label) {
    return 0;
  }
  if (strstr(label, "ir_for_cond_") != NULL) {
    return 1;
  }
  return strstr(label, "ir_while_") != NULL &&
         strstr(label, "ir_while_end_") == NULL;
}

static size_t ir_region_inner_loop_line(const IRFunction *function,
                                        size_t begin, size_t end) {
  int seen_header = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL || !ir_label_is_loop_header(ins->text)) {
      continue;
    }
    if (!seen_header) {
      seen_header = 1;
      continue;
    }
    for (size_t j = i + 1; j < end; j++) {
      if (function->instructions[j].location.line) {
        return function->instructions[j].location.line;
      }
    }
    return 0;
  }
  return 0;
}

static int ir_label_is_runtime_check(const char *label) {
  if (!label) {
    return 0;
  }
  return strstr(label, "trap_null") != NULL || strstr(label, "nonnull") != NULL ||
         strstr(label, "nullhoist") != NULL ||
         strstr(label, "trap_bounds") != NULL || strstr(label, "in_bounds") != NULL;
}

typedef enum {
  IR_BRANCH_SHAPE_OTHER = 0,
  IR_BRANCH_SHAPE_EXTREMUM,
  IR_BRANCH_SHAPE_COUNT,
  IR_BRANCH_SHAPE_CLAMP_STORE
} IRBranchShape;

static IRBranchShape ir_classify_one_diamond(const IRFunction *function,
                                             size_t at, size_t end,
                                             const IRInstruction *compare,
                                             const char **written_out) {
  size_t jump = at + 1;
  size_t write = 0;
  int found = 0;

  for (; jump < end; jump++) {
    IROpcode op = function->instructions[jump].op;
    if (op == IR_OP_JUMP) {
      break;
    }
    if (op == IR_OP_LABEL || op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ) {
      return IR_BRANCH_SHAPE_OTHER;
    }
  }
  if (jump >= end) {
    return IR_BRANCH_SHAPE_OTHER;
  }
  for (size_t i = at + 1; i < jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name) {
      continue;
    }
    found++;
    if (!write) {
      write = i;
    }
    if (compare && (ir_operand_is_symbol_named(&compare->lhs, ins->dest.name) ||
                    ir_operand_is_symbol_named(&compare->rhs, ins->dest.name))) {
      write = i;
      break;
    }
  }
  if (!found) {
    return IR_BRANCH_SHAPE_OTHER;
  }
  {
    const IRInstruction *w = &function->instructions[write];
    int writes_tested_operand =
        compare && (ir_operand_is_symbol_named(&compare->lhs, w->dest.name) ||
                    ir_operand_is_symbol_named(&compare->rhs, w->dest.name));
    *written_out = w->dest.name;
    if (!writes_tested_operand && w->op == IR_OP_BINARY && w->text &&
        w->text[0] == '+' && !w->text[1] &&
        (ir_operand_is_symbol_named(&w->lhs, w->dest.name) ||
         ir_operand_is_symbol_named(&w->rhs, w->dest.name))) {
      return IR_BRANCH_SHAPE_COUNT;
    }
    if (writes_tested_operand && w->op == IR_OP_LOAD) {
      return IR_BRANCH_SHAPE_EXTREMUM;
    }
    if (writes_tested_operand && w->op == IR_OP_ASSIGN && w->lhs.name) {
      const IROperand *other =
          ir_operand_is_symbol_named(&compare->lhs, w->dest.name)
              ? &compare->rhs
              : &compare->lhs;
      if (other->name && strcmp(other->name, w->lhs.name) == 0) {
        return IR_BRANCH_SHAPE_EXTREMUM;
      }
      if (w->lhs.kind == IR_OPERAND_TEMP) {
        const IRInstruction *src =
            ir_find_temp_producer_before(function, write, w->lhs.name);
        if (src && (src->op == IR_OP_LOAD || src->op == IR_OP_CAST)) {
          return IR_BRANCH_SHAPE_EXTREMUM;
        }
      }
    }
    if (writes_tested_operand) {
      return IR_BRANCH_SHAPE_CLAMP_STORE;
    }
    return IR_BRANCH_SHAPE_OTHER;
  }
}

static IRBranchShape ir_region_branch_shape(const IRFunction *function,
                                            size_t begin, size_t end,
                                            const char **written_out) {
  IRBranchShape shape = IR_BRANCH_SHAPE_OTHER;
  int seen = 0;
  int has_store = 0;
  size_t body_lo = begin + 1;
  size_t exit_test = (size_t)-1;

  for (size_t i = begin; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_label_is_loop_header(ins->text)) {
      for (size_t j = i + 1; j < end; j++) {
        if (function->instructions[j].op == IR_OP_BRANCH_ZERO ||
            function->instructions[j].op == IR_OP_BRANCH_EQ) {
          exit_test = j;
          break;
        }
      }
      body_lo = (exit_test == (size_t)-1) ? i + 1 : exit_test + 1;
      break;
    }
  }
  for (size_t i = body_lo; i < end; i++) {
    if (function->instructions[i].op == IR_OP_STORE) {
      has_store = 1;
    }
  }
  for (size_t i = body_lo; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IRInstruction *compare = NULL;
    const char *written = NULL;
    IRBranchShape one;
    if (ins->op != IR_OP_BRANCH_ZERO || ir_label_is_runtime_check(ins->text)) {
      continue;
    }
    for (size_t j = i; j > body_lo; j--) {
      const IRInstruction *c = &function->instructions[j - 1];
      if (c->op == IR_OP_BINARY && c->dest.kind == IR_OPERAND_TEMP &&
          c->dest.name && ir_operand_is_temp_named(&ins->lhs, c->dest.name)) {
        compare = c;
        break;
      }
    }
    one = ir_classify_one_diamond(function, i, end, compare, &written);
    if (one == IR_BRANCH_SHAPE_EXTREMUM && has_store) {
      one = IR_BRANCH_SHAPE_CLAMP_STORE;
    } else if (one == IR_BRANCH_SHAPE_CLAMP_STORE && !has_store) {
      one = IR_BRANCH_SHAPE_OTHER;
    }
    if (!seen) {
      shape = one;
      *written_out = written;
      seen = 1;
    } else if (one != shape) {
      return IR_BRANCH_SHAPE_OTHER;
    }
  }
  return seen ? shape : IR_BRANCH_SHAPE_OTHER;
}

static const char *ir_region_loop_counter(const IRFunction *function,
                                          size_t begin, size_t end) {
  for (size_t i = begin; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    size_t cmp = 0;
    if (ins->op != IR_OP_LABEL || !ir_label_is_loop_header(ins->text)) {
      continue;
    }
    if (!ir_find_next_non_nop(function, i + 1, &cmp) || cmp >= end) {
      return NULL;
    }
    {
      const IRInstruction *c = &function->instructions[cmp];
      if (c->op != IR_OP_BINARY || c->is_float || !c->text ||
          strcmp(c->text, "<") != 0 || c->lhs.kind != IR_OPERAND_SYMBOL ||
          !c->lhs.name) {
        return NULL;
      }
      if (!ir_symbol_contains(c->lhs.name, "__ptr_")) {
        return c->lhs.name;
      }
      for (size_t j = cmp + 1; j < end; j++) {
        const IRInstruction *step = &function->instructions[j];
        if (step->op == IR_OP_BINARY && !step->is_float && step->text &&
            strcmp(step->text, "+") == 0 &&
            ir_operand_is_symbol(&step->dest) &&
            !ir_symbol_contains(step->dest.name, "__ptr_") &&
            ir_operand_is_symbol_named(&step->lhs, step->dest.name) &&
            ir_operand_is_int_value(&step->rhs, 1)) {
          return step->dest.name;
        }
      }
    }
    return NULL;
  }
  return NULL;
}

static long long ir_index_element_stride(const IRFunction *function,
                                         size_t before, const IROperand *index,
                                         const char *iv, long long elem_size,
                                         int depth) {
  const IRInstruction *p = NULL;
  if (!index || !iv || elem_size <= 0 || depth > 4) {
    return 0;
  }
  if (ir_operand_is_symbol_named(index, iv)) {
    return elem_size == 1 ? 1 : 0;
  }
  if (index->kind != IR_OPERAND_TEMP || !index->name) {
    return 0;
  }
  p = ir_find_temp_producer_before(function, before, index->name);
  if (!p || p->op != IR_OP_BINARY || p->is_float || !p->text) {
    return 0;
  }
  if ((strcmp(p->text, "<<") == 0 || strcmp(p->text, "*") == 0) &&
      p->rhs.kind == IR_OPERAND_INT) {
    long long scale = strcmp(p->text, "<<") == 0
                          ? (p->rhs.int_value < 40 ? (1LL << p->rhs.int_value)
                                                   : 0)
                          : p->rhs.int_value;
    if (scale <= 0) {
      return 0;
    }
    if (ir_operand_is_symbol_named(&p->lhs, iv)) {
      return (scale % elem_size) ? 0 : scale / elem_size;
    }
    {
      long long inner = ir_index_element_stride(function, before, &p->lhs, iv,
                                                1, depth + 1);
      if (!inner) {
        return 0;
      }
      return ((inner * scale) % elem_size) ? 0 : (inner * scale) / elem_size;
    }
  }
  if (strcmp(p->text, "+") == 0) {
    if (p->rhs.kind == IR_OPERAND_INT) {
      return ir_index_element_stride(function, before, &p->lhs, iv, elem_size,
                                     depth + 1);
    }
    if (p->lhs.kind == IR_OPERAND_INT) {
      return ir_index_element_stride(function, before, &p->rhs, iv, elem_size,
                                     depth + 1);
    }
  }
  return 0;
}

static long long ir_region_strided_access(const IRFunction *function,
                                          size_t begin, size_t end) {
  const char *iv = ir_region_loop_counter(function, begin, end);
  long long worst = 0;
  if (!iv) {
    return 0;
  }
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *addr = NULL;
    const IRInstruction *sum = NULL;
    long long size = 0;
    long long stride = 0;
    if (ins->op == IR_OP_LOAD) {
      addr = &ins->lhs;
    } else if (ins->op == IR_OP_STORE) {
      addr = &ins->dest;
    } else {
      continue;
    }
    if (addr->kind != IR_OPERAND_TEMP || !addr->name ||
        ins->rhs.kind != IR_OPERAND_INT) {
      continue;
    }
    size = ins->rhs.int_value;
    sum = ir_find_temp_producer_before(function, i, addr->name);
    if (!sum || sum->op != IR_OP_BINARY || !sum->text ||
        strcmp(sum->text, "+") != 0 || sum->lhs.kind != IR_OPERAND_SYMBOL) {
      continue;
    }
    stride = ir_index_element_stride(function, i, &sum->rhs, iv, size, 0);
    if (stride > worst) {
      worst = stride;
    }
  }
  return worst > 1 ? worst : 0;
}

static int ir_region_invariant_index_term(const IRFunction *function,
                                          size_t begin, size_t end,
                                          const char **base_out) {
  const char *iv = ir_region_loop_counter(function, begin, end);
  if (!iv) {
    return 0;
  }
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *addr = NULL;
    const IRInstruction *sum = NULL;
    const IRInstruction *scale = NULL;
    const IRInstruction *inner = NULL;
    if (ins->op == IR_OP_LOAD) {
      addr = &ins->lhs;
    } else if (ins->op == IR_OP_STORE) {
      addr = &ins->dest;
    } else {
      continue;
    }
    if (addr->kind != IR_OPERAND_TEMP || !addr->name) {
      continue;
    }
    sum = ir_find_temp_producer_before(function, i, addr->name);
    if (!sum || sum->op != IR_OP_BINARY || !sum->text ||
        strcmp(sum->text, "+") != 0 || sum->lhs.kind != IR_OPERAND_SYMBOL ||
        sum->rhs.kind != IR_OPERAND_TEMP || !sum->rhs.name) {
      continue;
    }
    scale = ir_find_temp_producer_before(function, i, sum->rhs.name);
    if (!scale || scale->op != IR_OP_BINARY || !scale->text ||
        strcmp(scale->text, "<<") != 0 || scale->lhs.kind != IR_OPERAND_TEMP ||
        !scale->lhs.name) {
      continue;
    }
    inner = ir_find_temp_producer_before(function, i, scale->lhs.name);
    if (!inner || inner->op != IR_OP_BINARY || !inner->text ||
        strcmp(inner->text, "+") != 0) {
      continue;
    }
    if ((ir_operand_is_symbol_named(&inner->lhs, iv) &&
         inner->rhs.kind != IR_OPERAND_INT) ||
        (ir_operand_is_symbol_named(&inner->rhs, iv) &&
         inner->lhs.kind != IR_OPERAND_INT)) {
      *base_out = sum->lhs.name;
      return 1;
    }
  }
  return 0;
}

static const char *ir_region_find_serial_recurrence(const IRFunction *function,
                                                    size_t begin, size_t end,
                                                    const char **ops,
                                                    size_t *n_ops);

static int ir_region_has_runtime_check(const IRFunction *function, size_t begin,
                                       size_t end) {
  for (size_t i = begin + 1; i < end && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if ((ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT) &&
        ins->text && strstr(ins->text, "crash_trap") != NULL) {
      return 1;
    }
    if ((ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ ||
         ins->op == IR_OP_JUMP || ins->op == IR_OP_LABEL) &&
        ir_label_is_runtime_check(ins->text)) {
      return 1;
    }
  }
  return 0;
}

static const char *ir_simd_bail_reason(const IRFunction *function, size_t begin,
                                       size_t end) {
  static char reason_buffer[512];
  int has_call = 0, has_new = 0, has_asm = 0;
  int branch_count = 0, jump_count = 0;
  int has_i16 = 0, has_i64 = 0;
  int past_header = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (!past_header) {
      continue;
    }
    switch (ins->op) {
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
      if (!(ins->text && strstr(ins->text, "crash_trap"))) {
        has_call = 1;
      }
      break;
    case IR_OP_NEW:
      has_new = 1;
      break;
    case IR_OP_INLINE_ASM:
      has_asm = 1;
      break;
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!ir_label_is_runtime_check(ins->text)) {
        branch_count++;
      }
      break;
    case IR_OP_JUMP:
      if (!ir_label_is_runtime_check(ins->text)) {
        jump_count++;
      }
      break;
    case IR_OP_LOAD:
    case IR_OP_STORE: {
      long long sz = (ins->rhs.kind == IR_OPERAND_INT) ? ins->rhs.int_value : 4;
      if (!ins->is_float) {
        if (sz == 2) {
          has_i16 = 1;
        } else if (sz == 8) {
          has_i64 = 1;
        }
      }
      break;
    }
    default:
      break;
    }
  }
  const char *keeps_checks =
      ir_region_has_runtime_check(function, begin, end)
          ? "; this build also keeps the runtime checks inside the loop, which "
            "no kernel can carry (--release drops them)"
          : "";
#define IR_SIMD_BAIL(text)                                                       do {                                                                             if (!*keeps_checks) {                                                            return (text);                                                               }                                                                              snprintf(reason_buffer, sizeof(reason_buffer), "%s%s", (text),                          keeps_checks);                                                        return reason_buffer;                                                        } while (0)

  if (has_call) {
    IR_SIMD_BAIL("the loop body contains a function call (only call-free or "
                 "fully-inlined loops vectorize)");
  }
  if (has_new) {
    IR_SIMD_BAIL("the loop body allocates memory (new)");
  }
  if (has_asm) {
    IR_SIMD_BAIL("the loop body contains inline assembly");
  }
  if (ir_region_inner_loop_line(function, begin, end)) {
    IR_SIMD_BAIL("the loop body contains a nested loop (possibly from an "
                 "inlined call); only the innermost loop of a nest vectorizes");
  }
  if (branch_count > 1 || jump_count > 1) {
    IR_SIMD_BAIL("the loop body branches on data, with an `if`, `&&` or `||` "
                 "each iteration; only straight-line loop bodies vectorize");
  }
  if (has_i16) {
    return "the loop accesses 16-bit integers, which no vectorizer takes; "
           "use int32, int8, float32 or float64";
  }
  if (has_i64) {
    return "the loop accesses 64-bit integers, which no vectorizer takes; "
           "use int32, int8, float32 or float64";
  }
  {
    const char *ops[6];
    size_t n_ops = 0;
    if (ir_region_find_serial_recurrence(function, begin, end, ops, &n_ops)) {
      return "the loop carries a serial recurrence: a value is computed from "
             "its own previous value through `*`, `/`, a shift or a bitwise "
             "operation, so each iteration waits on the one before it. Sums "
             "and differences can be regrouped and vectorize; these cannot";
    }
  }
  IR_SIMD_BAIL("no vectorizer recognized this loop's shape: a stride other "
               "than one element, a value carried from one iteration to the "
               "next, or a reduction no kernel covers");
#undef IR_SIMD_BAIL
}

const char *ir_simd_bail_id_name(int id) {
  switch ((IRSimdBailId)id) {
  case IR_SIMD_BAIL_NONE:                return "none";
  case IR_SIMD_BAIL_CALL_IN_BODY:        return "call-in-body";
  case IR_SIMD_BAIL_EXTERN_CALL_IN_BODY: return "extern-call-in-body";
  case IR_SIMD_BAIL_SAFETY_IN_BODY: return "memory-safety-in-body";
  case IR_SIMD_BAIL_INDIRECT_CALL:       return "indirect-call";
  case IR_SIMD_BAIL_ALLOC_IN_BODY:       return "alloc-in-body";
  case IR_SIMD_BAIL_INLINE_ASM:          return "inline-asm";
  case IR_SIMD_BAIL_CONTROL_FLOW:        return "control-flow";
  case IR_SIMD_BAIL_EARLY_EXIT:          return "early-exit";
  case IR_SIMD_BAIL_INT16_ELEMENTS:      return "int16-elements";
  case IR_SIMD_BAIL_INT64_ELEMENTS:      return "int64-elements";
  case IR_SIMD_BAIL_RELOADED_BASE:       return "reloaded-base";
  case IR_SIMD_BAIL_SERIAL_RECURRENCE:   return "serial-recurrence";
  case IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS:  return "mixed-float-widths";
  case IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC: return "byte-sum-narrow-acc";
  case IR_SIMD_BAIL_I32_SUM_NARROW_ACC:  return "int32-sum-narrow-acc";
  case IR_SIMD_BAIL_INLINED_PARAM_LOCAL: return "inlined-param-local";
  case IR_SIMD_BAIL_BODY_LOCAL:          return "body-local";
  case IR_SIMD_BAIL_DOT_SHAPE_ADDRESS:   return "dot-shape-address";
  case IR_SIMD_BAIL_STORE_ONLY_FILL:     return "store-only-fill";
  case IR_SIMD_BAIL_EXTREMUM_SHAPE:      return "extremum-shape";
  case IR_SIMD_BAIL_PREDICATED_COUNT:    return "predicated-count";
  case IR_SIMD_BAIL_CLAMP_STORE:         return "clamp-store";
  case IR_SIMD_BAIL_STRIDED_ACCESS:      return "strided-access";
  case IR_SIMD_BAIL_UNBOUNDED_SHIFT:     return "unbounded-shift";
  case IR_SIMD_BAIL_VARIABLE_SHIFT:      return "variable-shift";
  case IR_SIMD_BAIL_COUNTER_STORE:       return "counter-store";
  case IR_SIMD_BAIL_SCATTER_STORE:       return "scatter-store";
  case IR_SIMD_BAIL_CHOSEN_INDEX:        return "chosen-index";
  case IR_SIMD_BAIL_INVARIANT_ADDEND:    return "invariant-addend";
  case IR_SIMD_BAIL_UNRECOGNIZED_SHAPE:  return "unrecognized-shape";
  }
  return "unknown";
}

#define IR_SIMD_SET_DIAG(value)                                                \
  do {                                                                         \
    if (diagnosis_out) {                                                       \
      *diagnosis_out = (value);                                                \
    }                                                                          \
  } while (0)

#define IR_SIMD_MARK_ADVISORY()                                                \
  do {                                                                         \
    if (advisory_out) {                                                        \
      *advisory_out = 1;                                                       \
    }                                                                          \
  } while (0)

static int ir_recur_op_is_reassociable(const char *text) {
  return text && text[0] && !text[1] && (text[0] == '+' || text[0] == '-');
}

#define IR_RECUR_MAX_OPS 6
static void ir_recur_note_op(const char **ops, size_t *n_ops, const char *t) {
  if (!t || !t[0]) {
    return;
  }
  for (size_t i = 0; i < *n_ops; i++) {
    if (strcmp(ops[i], t) == 0) {
      return;
    }
  }
  if (*n_ops < IR_RECUR_MAX_OPS) {
    ops[(*n_ops)++] = t;
  }
}

static int ir_recur_operand_reaches(const IRFunction *function, size_t before,
                                    const IROperand *op, const char *sym,
                                    int *nonreassoc, const char **ops,
                                    size_t *n_ops, int depth) {
  if (!op || depth > 32) {
    return 0;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name && strcmp(op->name, sym) == 0) {
    return 1;
  }
  if (op->kind != IR_OPERAND_TEMP || !op->name) {
    return 0;
  }
  const IRInstruction *p =
      ir_find_temp_producer_before(function, before, op->name);
  if (!p) {
    return 0;
  }
  if (p->op != IR_OP_BINARY && p->op != IR_OP_CAST && p->op != IR_OP_ASSIGN) {
    return 0;
  }
  size_t pidx = (size_t)(p - function->instructions);
  int found = 0;
  if (ir_recur_operand_reaches(function, pidx, &p->lhs, sym, nonreassoc, ops,
                               n_ops, depth + 1)) {
    found = 1;
  }
  if (ir_recur_operand_reaches(function, pidx, &p->rhs, sym, nonreassoc, ops,
                               n_ops, depth + 1)) {
    found = 1;
  }
  if (found && p->op == IR_OP_BINARY && p->text) {
    ir_recur_note_op(ops, n_ops, p->text);
    if (!ir_recur_op_is_reassociable(p->text)) {
      *nonreassoc = 1;
    }
  }
  return found;
}

static const char *ir_region_find_serial_recurrence(const IRFunction *function,
                                                    size_t begin, size_t end,
                                                    const char **ops,
                                                    size_t *n_ops) {
  int past_header = 0;
  *n_ops = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (!past_header || ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name) {
      continue;
    }
    int nonreassoc = 0, reaches = 0;
    const char *cand_ops[IR_RECUR_MAX_OPS];
    size_t cand_n = 0;
    if (ins->op == IR_OP_BINARY) {
      if (ir_recur_operand_reaches(function, i, &ins->lhs, ins->dest.name,
                                   &nonreassoc, cand_ops, &cand_n, 0)) {
        reaches = 1;
      }
      if (ir_recur_operand_reaches(function, i, &ins->rhs, ins->dest.name,
                                   &nonreassoc, cand_ops, &cand_n, 0)) {
        reaches = 1;
      }
      if (reaches && ins->text) {
        ir_recur_note_op(cand_ops, &cand_n, ins->text);
        if (!ir_recur_op_is_reassociable(ins->text)) {
          nonreassoc = 1;
        }
      }
    } else if (ins->op == IR_OP_ASSIGN && ins->lhs.kind == IR_OPERAND_TEMP) {
      reaches = ir_recur_operand_reaches(function, i, &ins->lhs, ins->dest.name,
                                         &nonreassoc, cand_ops, &cand_n, 0);
    }
    if (reaches && nonreassoc) {
      for (size_t k = 0; k < cand_n; k++) {
        ops[k] = cand_ops[k];
      }
      *n_ops = cand_n;
      return ins->dest.name;
    }
  }
  return NULL;
}

static long long ir_simd_majority_float_width(const IRFunction *function,
                                              size_t begin, size_t end);

static void ir_type_element_name(const char *declared, char *out, size_t cap) {
  const char *bracket = declared ? strchr(declared, '[') : NULL;
  size_t n = bracket ? (size_t)(bracket - declared) : 0;
  while (n > 0 && declared[n - 1] == ' ') {
    n--;
  }
  if (n == 0 || n >= cap) {
    snprintf(out, cap, "T");
    return;
  }
  memcpy(out, declared, n);
  out[n] = '\0';
}

static const char *ir_simd_field_base_symbol(const IRFunction *function,
                                             size_t begin, size_t at,
                                             const IROperand *addr) {
  if (addr->kind == IR_OPERAND_SYMBOL && addr->name) {
    return addr->name;
  }
  if (addr->kind != IR_OPERAND_TEMP || !addr->name) {
    return NULL;
  }
  for (size_t k = at; k-- > begin;) {
    const IRInstruction *def = &function->instructions[k];
    if (def->dest.kind != IR_OPERAND_TEMP || !def->dest.name ||
        strcmp(def->dest.name, addr->name) != 0) {
      continue;
    }
    if (def->op == IR_OP_BINARY && !def->is_float && def->text &&
        strcmp(def->text, "+") == 0 && def->lhs.kind == IR_OPERAND_SYMBOL &&
        def->lhs.name && def->rhs.kind == IR_OPERAND_INT) {
      return def->lhs.name;
    }
    return NULL;
  }
  return NULL;
}

typedef struct {
  const IRFunction *function;
  size_t begin;
  size_t end;
  char *reason;
  size_t reason_cap;
  char *fix;
  size_t fix_cap;
  int *diagnosis_out;
  int *advisory_out;
  const char *callee;
  int has_indirect_call;
  int has_new;
  int has_asm;
  int branch_count;
  int jump_count;
  const char *branch_targets[8];
  size_t branch_target_count;
  int has_return_in_body;
  int has_i16;
  int has_i64;
  int has_f32;
  int has_f64;
  int has_byte_load;
  int has_i32_load;
  int has_int_accum;
  const char *int_accum_sym;
  int has_float_accum;
  int has_float_mul;
  int load_count;
  int store_count;
  int byte_store_count;
  int reloaded_base_count;
  const char *reloaded_base_sym;
  int past_header;
  const char *body_local;
  const char *rebased_array;
} IrSimdBail;

typedef int (*IrSimdBailRule)(IrSimdBail *d);

static void ir_simd_diag(IrSimdBail *d, int value) {
  if (d->diagnosis_out) {
    *d->diagnosis_out = value;
  }
}

static void ir_simd_advisory(IrSimdBail *d) {
  if (d->advisory_out) {
    *d->advisory_out = 1;
  }
}

static void ir_simd_scan_memory(IrSimdBail *d, const IRInstruction *ins,
                                size_t i) {
  long long sz = (ins->rhs.kind == IR_OPERAND_INT) ? ins->rhs.int_value : 4;
  if (ins->op == IR_OP_LOAD) {
    d->load_count++;
  } else {
    d->store_count++;
    if (sz == 1) {
      d->byte_store_count++;
    }
  }
  if (ins->is_float) {
    if (sz == 4) {
      d->has_f32 = 1;
    } else if (sz == 8) {
      d->has_f64 = 1;
    }
  } else {
    if (sz == 1 && ins->op == IR_OP_LOAD) {
      d->has_byte_load = 1;
    } else if (sz == 2) {
      d->has_i16 = 1;
    } else if (sz == 4 && ins->op == IR_OP_LOAD) {
      d->has_i32_load = 1;
    } else if (sz == 8) {
      const char *base =
          (ins->op == IR_OP_LOAD && ins->dest.kind == IR_OPERAND_TEMP)
              ? ir_simd_field_base_symbol(d->function, d->begin, i, &ins->lhs)
              : NULL;
      if (base) {
        d->reloaded_base_count++;
        if (!d->reloaded_base_sym) {
          d->reloaded_base_sym = base;
        }
      } else {
        d->has_i64 = 1;
      }
    }
  }

}

static int ir_simd_temp_adds_to(const IrSimdBail *d, const IROperand *temp,
                                 const IROperand *acc) {
  if (temp->kind != IR_OPERAND_TEMP || !temp->name) {
    return 0;
  }
  for (size_t i = d->begin + 1; i < d->end; i++) {
    const IRInstruction *def = &d->function->instructions[i];
    if (def->op == IR_OP_BINARY && def->text && strcmp(def->text, "+") == 0 &&
        def->dest.kind == IR_OPERAND_TEMP && def->dest.name &&
        strcmp(def->dest.name, temp->name) == 0) {
      return (ir_operand_is_symbol(&def->lhs) &&
              ir_operand_names_match(&def->lhs, acc)) ||
             (ir_operand_is_symbol(&def->rhs) &&
              ir_operand_names_match(&def->rhs, acc));
    }
  }
  return 0;
}

static void ir_simd_scan_binary(IrSimdBail *d, const IRInstruction *ins) {
  int plus = !ins->is_float && ins->text && ins->text[0] == '+' &&
             !ins->text[1] && ins->dest.kind == IR_OPERAND_SYMBOL &&
             ins->dest.name;
  if (plus &&
      ((ir_operand_is_symbol(&ins->lhs) &&
        ir_operand_names_match(&ins->lhs, &ins->dest) &&
        ins->rhs.kind == IR_OPERAND_TEMP) ||
       (ir_operand_is_symbol(&ins->rhs) &&
        ir_operand_names_match(&ins->rhs, &ins->dest) &&
        ins->lhs.kind == IR_OPERAND_TEMP) ||
       ir_simd_temp_adds_to(d, &ins->lhs, &ins->dest) ||
       ir_simd_temp_adds_to(d, &ins->rhs, &ins->dest))) {
    d->has_int_accum = 1;
    d->int_accum_sym = ins->dest.name;
  }
  if (ins->is_float && ins->text && ins->text[0] == '*' && !ins->text[1]) {
    d->has_float_mul = 1;
  }
  if (ins->is_float && ins->text && ins->text[0] == '+' &&
      !ins->text[1] &&
      ((ir_operand_is_symbol(&ins->lhs)) ||
       (ir_operand_is_symbol(&ins->rhs)))) {
    d->has_float_accum = 1;
  }

}

static void ir_simd_bail_scan(IrSimdBail *d) {
  for (size_t i = d->begin + 1; i < d->end; i++) {
    const IRInstruction *ins = &d->function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        d->past_header = 1;
      }
      continue;
    }
    if (!d->past_header) {
      continue;
    }
    switch (ins->op) {
    case IR_OP_DECLARE_LOCAL:
      if (d->past_header && !d->body_local && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name) {
        d->body_local = ins->dest.name;
      }
      break;
    case IR_OP_ADDRESS_OF:
      if (!d->rebased_array && ins->lhs.kind == IR_OPERAND_SYMBOL &&
          ins->lhs.name) {
        const char *declared =
            ir_function_local_declared_type(d->function, ins->lhs.name);
        if (declared && strchr(declared, '[')) {
          d->rebased_array = ins->lhs.name;
        }
      }
      break;
    case IR_OP_CALL:
      if (!(ins->text && strstr(ins->text, "crash_trap")) && !d->callee) {
        d->callee = ins->text ? ins->text : "?";
      }
      break;
    case IR_OP_CALL_INDIRECT:
      d->has_indirect_call = 1;
      break;
    case IR_OP_NEW:
      d->has_new = 1;
      break;
    case IR_OP_INLINE_ASM:
      d->has_asm = 1;
      break;
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!ir_label_is_runtime_check(ins->text)) {
        d->branch_count++;
        if (ins->text && d->branch_target_count < 8) {
          d->branch_targets[d->branch_target_count++] = ins->text;
        }
      }
      break;
    case IR_OP_JUMP:
      if (!ir_label_is_runtime_check(ins->text)) {
        d->jump_count++;
      }
      break;
    case IR_OP_RETURN:
      d->has_return_in_body = 1;
      break;
    case IR_OP_LOAD:
    case IR_OP_STORE:
      ir_simd_scan_memory(d, ins, i);
      break;
    case IR_OP_BINARY:
      ir_simd_scan_binary(d, ins);
      break;
    default:
      break;
    }
  }
}

static int ir_simd_bail_call_in_body(IrSimdBail *d) {
  if (d->callee) {
    for (size_t i = 0; i < d->function->instruction_count; i++) {
      const IRInstruction *in = &d->function->instructions[i];
      IRSafetyIntrinsic kind = in->text && strcmp(in->text, d->callee) == 0
                                   ? ir_safety_intrinsic(in)
                                   : IR_SAFETY_INTRINSIC_NONE;
      if (kind == IR_SAFETY_INTRINSIC_NONE) continue;
      snprintf(d->reason, d->reason_cap,
               "`--safe` keeps a check inside the loop, from line %zu: %s. A "
               "vector body has no place for a check per element",
               in->location.line,
               kind == IR_SAFETY_INTRINSIC_CHECK
                   ? "a bounds check against an extent the loop does not "
                     "hold fixed"
               : kind == IR_SAFETY_INTRINSIC_LIFETIME
                   ? "it registers or releases memory every iteration"
                   : "it asks the runtime which allocation a pointer came "
                     "from, every iteration");
      if (kind == IR_SAFETY_INTRINSIC_CHECK) {
        snprintf(d->fix, d->fix_cap,
                 "a build without `--safe` drops the check and gives this "
                 "loop the vectorizer's own verdict. Under `--safe`, index "
                 "an array whose length the loop bound already proves, such "
                 "as `for i in 0..a.length`, and the check leaves the loop");
      } else {
        snprintf(d->fix, d->fix_cap,
                 "nothing to change in the loop: this is `--safe` catching a "
                 "use after free through a pointer the loop does not pin. A "
                 "build without `--safe` drops it and gives this loop the "
                 "vectorizer's own verdict");
      }
      ir_simd_advisory(d);
      ir_simd_diag(d, IR_SIMD_BAIL_SAFETY_IN_BODY);
      return 1;
    }
    IRFunction *callee_fn =
        g_explain_program ? ir_program_find_function(g_explain_program, d->callee)
                          : NULL;
    char shown[128];
    ir_display_name(d->callee, shown, sizeof(shown));
    if (g_explain_program && !callee_fn &&
        strncmp(d->callee, "mettle_string_", 14) == 0) {
      snprintf(d->reason, d->reason_cap,
               "each iteration builds a string, and the runtime's `%s` does "
               "that work out of line. Text has no lanes to vectorize",
               d->callee);
      snprintf(d->fix, d->fix_cap,
               "nothing to change if the string is the point. Building it "
               "once outside the loop, or writing the pieces straight to the "
               "output, saves an allocation per iteration");
      ir_simd_advisory(d);
      ir_simd_diag(d, IR_SIMD_BAIL_EXTERN_CALL_IN_BODY);
      return 1;
    }
    if (g_explain_program && !callee_fn) {
      snprintf(d->reason, d->reason_cap,
               "each iteration calls `%s`, an external function with no body "
               "this compiler can see, so it can never be inlined and this "
               "loop cannot vectorize as written",
               d->callee);
      snprintf(d->fix, d->fix_cap,
               "if the call is the work, such as I/O or a system call, the "
               "scalar loop is the right code. If it returns the same value "
               "every iteration, hoist it. If it is hot arithmetic, write it "
               "in Mettle so the inliner can take it");
      ir_simd_advisory(d);
      ir_simd_diag(d, IR_SIMD_BAIL_EXTERN_CALL_IN_BODY);
      return 1;
    }
    snprintf(d->reason, d->reason_cap,
             "each iteration calls `%s`; loops vectorize only after every "
             "call in the body has been inlined away",
             shown);
    snprintf(d->fix, d->fix_cap,
             "make `%s` inline-eligible (small body, or mark it @inline), or "
             "hoist the call out of the loop",
             shown);
    ir_simd_diag(d, IR_SIMD_BAIL_CALL_IN_BODY);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_indirect_call(IrSimdBail *d) {
  if (d->has_indirect_call) {
    snprintf(d->reason, d->reason_cap,
             "each iteration calls through a function pointer, which can "
             "never be inlined away");
    snprintf(d->fix, d->fix_cap,
             "call the target directly if it is known at compile time");
    ir_simd_diag(d, IR_SIMD_BAIL_INDIRECT_CALL);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_allocation(IrSimdBail *d) {
  if (d->has_new) {
    snprintf(d->reason, d->reason_cap, "the loop body allocates memory (`new`) "
                                 "every iteration");
    snprintf(d->fix, d->fix_cap, "hoist the allocation out of the loop");
    ir_simd_diag(d, IR_SIMD_BAIL_ALLOC_IN_BODY);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_inline_asm(IrSimdBail *d) {
  if (d->has_asm) {
    snprintf(d->reason, d->reason_cap,
             "the loop body contains inline assembly, which is opaque to the "
             "vectorizer");
    ir_simd_diag(d, IR_SIMD_BAIL_INLINE_ASM);
    return 1;
  }
  return 0;
}

static size_t ir_simd_arm_of(const IrSimdBail *d, size_t def_at);

static void ir_simd_source_name(const char *name, char *out, size_t cap) {
  char *cut;
  snprintf(out, cap, "%s", name ? name : "?");
  cut = strstr(out, "__ssa");
  if (cut) {
    *cut = '\0';
  }
  cut = strstr(out, "$$");
  if (cut) {
    *cut = '\0';
  }
}

static int ir_simd_bail_two_extrema(IrSimdBail *d) {
  char names[2][64];
  size_t arms[2] = {0, 0};
  size_t found = 0;
  for (size_t i = d->begin + 1; i < d->end && found < 2; i++) {
    const IRInstruction *ins = &d->function->instructions[i];
    char shown[64];
    size_t arm = ins->op == IR_OP_STORE ? 0 : ir_simd_arm_of(d, i);
    if (ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name || arm == 0) {
      continue;
    }
    ir_simd_source_name(ins->dest.name, shown, sizeof(shown));
    if (found == 1 && (strcmp(names[0], shown) == 0 || arms[0] == arm)) {
      continue;
    }
    arms[found] = arm;
    snprintf(names[found++], sizeof(names[0]), "%s", shown);
  }
  if (found < 2) {
    return 0;
  }
  snprintf(d->reason, d->reason_cap,
           "the loop keeps two running extremes, `%s` and `%s`, and the "
           "extremum kernel carries one per loop",
           names[0], names[1]);
  snprintf(d->fix, d->fix_cap,
           "split it into one loop for `%s` and one for `%s`, each comparing "
           "the element directly (`if (a[i] < %s) { %s = a[i]; }`); each "
           "becomes a vpminsd or vpmaxsd reduction",
           names[0], names[1], names[0], names[0]);
  ir_simd_diag(d, IR_SIMD_BAIL_EXTREMUM_SHAPE);
  return 1;
}

static int ir_simd_bail_control_flow(IrSimdBail *d) {
  if (d->branch_count > 1 || d->jump_count > 1) {
    int outward_branches = 0;
    for (size_t t = 0; t < d->branch_target_count; t++) {
      int inside = 0;
      for (size_t i = d->begin + 1; i < d->end && !inside; i++) {
        const IRInstruction *lab = &d->function->instructions[i];
        if (lab->op == IR_OP_LABEL && lab->text &&
            strcmp(lab->text, d->branch_targets[t]) == 0) {
          inside = 1;
        }
      }
      if (!inside) {
        outward_branches++;
      }
    }
    if (d->has_return_in_body || outward_branches > 1) {
      snprintf(d->reason, d->reason_cap,
               "the loop can exit before its trip count, and its body does "
               "more than test the exit condition. The search kernel skips "
               "ahead, so it covers only loops that look for a match or a "
               "mismatch and do nothing in the iterations it skips");
      snprintf(d->fix, d->fix_cap,
               "a loop whose body is only `if (a[i] == key) ...`, with any "
               "comparison, a constant or variable key, or `a[i] != b[i]`, "
               "becomes an 8-wide compare and movemask scan. Move the other "
               "work out of this loop, or search in a loop of its own and "
               "carry on from the index it finds");
      ir_simd_diag(d, IR_SIMD_BAIL_EARLY_EXIT);
      return 1;
    }
    {
      const char *written = NULL;
      IRBranchShape shape =
          ir_region_branch_shape(d->function, d->begin, d->end, &written);
      if (shape == IR_BRANCH_SHAPE_EXTREMUM && ir_simd_bail_two_extrema(d)) {
        return 1;
      }
      if (shape == IR_BRANCH_SHAPE_EXTREMUM) {
        const char *acc_type =
            written ? ir_function_local_declared_type(d->function, written) : NULL;
        snprintf(d->reason, d->reason_cap,
                 "this is a running %s, a shape that does vectorize, but "
                 "the kernel did not claim it%s%s%s",
                 "minimum or maximum",
                 acc_type ? " (`" : "", acc_type ? written : "",
                 acc_type ? "` is the accumulator)" : "");
        if (acc_type && strcmp(acc_type, "float64") != 0 &&
            strcmp(acc_type, "float32") != 0 && strcmp(acc_type, "int32") != 0) {
          snprintf(d->fix, d->fix_cap,
                   "widen the array to int32 elements (float32 and float64 "
                   "work too): the extremum kernel carries those lane widths "
                   "and these are %s. Declaring `%s` alone does not move it, "
                   "because the width the kernel reads is the element's",
                   acc_type, written);
        } else {
          snprintf(d->fix, d->fix_cap,
                   "the extremum kernel needs the compared value to come from "
                   "`base[i]` over float32, float64 or int32 elements, and the "
                   "counter to start at 0 or at 1 after seeding from `a[0]`; "
                   "one that widens narrower elements, or indexes anything "
                   "else, falls outside it");
        }
        ir_simd_diag(d, IR_SIMD_BAIL_EXTREMUM_SHAPE);
        return 1;
      }
      if (shape == IR_BRANCH_SHAPE_COUNT) {
        snprintf(d->reason, d->reason_cap,
                 "the body accumulates into `%s` under a condition. Over int32 "
                 "elements that is made unconditional and vectorized, so this "
                 "one is out for one of three reasons: the elements are float "
                 "(no kernel, and the branchless form has none either), the "
                 "guard is not a comparison (`if (x & 6)` fires on 2, 4 and 6 "
                 "alike, and the addend would be multiplied by those), or an "
                 "else arm writes `%s` too, which is a select rather than an "
                 "accumulate",
                 written ? written : "the accumulator",
                 written ? written : "the accumulator");
        snprintf(d->fix, d->fix_cap,
                 "over int32 elements, add the comparison rather than "
                 "branching on it: `%s = %s + ((a[i] & 6) != 0);` vectorizes, "
                 "and so does any other comparison written that way. Over "
                 "float elements there is nothing to change here",
                 written ? written : "count", written ? written : "count");
        ir_simd_diag(d, IR_SIMD_BAIL_PREDICATED_COUNT);
        return 1;
      }
      if (shape == IR_BRANCH_SHAPE_CLAMP_STORE) {
        snprintf(d->reason, d->reason_cap,
                 "the body chooses `%s` under a condition before storing it. "
                 "That shape does vectorize over int32 elements, as vpminsd, "
                 "vpmaxsd or a lane select, so what stopped this one is either "
                 "the element type (no float select kernel yet) or a nest too "
                 "deep for the six ymm registers an int32 map has",
                 written ? written : "a local");
        snprintf(d->fix, d->fix_cap,
                 "if the elements are int32, split the deepest arm into its own "
                 "loop so fewer values are live at once; otherwise the "
                 "compiler has no kernel for this loop yet");
        ir_simd_advisory(d);
        ir_simd_diag(d, IR_SIMD_BAIL_CLAMP_STORE);
        return 1;
      }
    }
    if (d->store_count > 0) {
      snprintf(d->reason, d->reason_cap,
               "the body branches on data and writes memory under the branch "
               "(%d store%s). An `if` that only chooses a value becomes a "
               "lane select, but a store some lanes skip needs a masked store, "
               "and the compiler emits none",
               d->store_count, d->store_count == 1 ? "" : "s");
      snprintf(d->fix, d->fix_cap,
               "compute the value with a select and store it every iteration "
               "(`a[i] = c ? x : a[i]`), or move the guarded work into a "
               "loop of its own");
    } else {
      snprintf(d->reason, d->reason_cap,
               "the body branches on data, and its arms set more values than "
               "one lane select carries: %d branches lead to %zu join points",
               d->branch_count, d->branch_target_count);
      snprintf(d->fix, d->fix_cap,
               "reduce each arm to one value chosen by one comparison, or "
               "split the work into two simpler loops");
    }
    ir_simd_diag(d, IR_SIMD_BAIL_CONTROL_FLOW);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_int16_elements(IrSimdBail *d) {
  if (d->has_i16) {
    snprintf(d->reason, d->reason_cap,
             "the loop reads or writes 16-bit integers, and no 16-bit kernels "
             "exist");
    if (d->has_int_accum) {
      const char *acc_type =
          d->int_accum_sym ? ir_function_local_declared_type(d->function,
                                                          d->int_accum_sym)
                        : NULL;
      if (acc_type && strcmp(acc_type, "int64") == 0) {
        snprintf(d->fix, d->fix_cap, "use int32 elements");
      } else {
        snprintf(d->fix, d->fix_cap,
                 "use int32 elements and declare the accumulator as int64");
      }
    } else {
      snprintf(d->fix, d->fix_cap, "use int32 (or int8 if the values fit)");
    }
    ir_simd_diag(d, IR_SIMD_BAIL_INT16_ELEMENTS);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_int64_elements(IrSimdBail *d) {
  if (d->has_i64) {
    snprintf(d->reason, d->reason_cap,
             "the loop reads or writes 64-bit integers, and no 64-bit "
             "integer kernels exist");
    snprintf(d->fix, d->fix_cap, "use int32 arrays if the values fit");
    ir_simd_diag(d, IR_SIMD_BAIL_INT64_ELEMENTS);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_reloaded_base(IrSimdBail *d) {
  if (d->reloaded_base_count > 0 &&
      d->load_count + d->store_count > d->reloaded_base_count) {
    snprintf(d->reason, d->reason_cap,
             "the body re-reads the array's base pointer out of `%s` every "
             "iteration; a store in the body could change that pointer, so the "
             "read cannot be lifted out and the kernels have no base to hold "
             "fixed for the whole loop",
             d->reloaded_base_sym ? d->reloaded_base_sym : "a struct");
    snprintf(d->fix, d->fix_cap,
             "bind the base pointer to a local before the loop and index that "
             "local in the body");
    ir_simd_diag(d, IR_SIMD_BAIL_RELOADED_BASE);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_serial_recurrence(IrSimdBail *d) {
  {
    const char *ops[IR_RECUR_MAX_OPS];
    size_t n_ops = 0;
    const char *recur_symbol =
        ir_region_find_serial_recurrence(d->function, d->begin, d->end, ops, &n_ops);
    if (recur_symbol) {
      char op_list[64];
      size_t w = 0;
      op_list[0] = '\0';
      for (size_t i = 0; i < n_ops && w + 10 < sizeof(op_list); i++) {
        int n = snprintf(op_list + w, sizeof(op_list) - w, "%s`%s`",
                         i ? ", " : "", ops[i]);
        if (n < 0) {
          break;
        }
        w += (size_t)n;
      }
      snprintf(d->reason, d->reason_cap,
               "`%s` carries a loop-carried recurrence: each iteration computes "
               "it from its own previous value (through %s), so the iterations "
               "form a dependency chain that cannot run as independent SIMD "
               "lanes",
               recur_symbol, n_ops ? op_list : "a non-reassociable operation");
      snprintf(d->fix, d->fix_cap,
               "sums and differences can be regrouped, so they vectorize. "
               "Products, quotients, shifts and bitwise recurrences cannot. "
               "When this running value is the algorithm, as in a hash, a "
               "random number generator or an IIR filter, the loop is "
               "already at its scalar floor");
      ir_simd_advisory(d);
      ir_simd_diag(d, IR_SIMD_BAIL_SERIAL_RECURRENCE);
      return 1;
    }
  }
  return 0;
}

static int ir_simd_bail_mixed_float_widths(IrSimdBail *d) {
  if (d->has_f32 && d->has_f64) {
    long long keep = ir_simd_majority_float_width(d->function, d->begin, d->end);
    snprintf(d->reason, d->reason_cap,
             "the loop mixes float32 and float64 elements; each kernel "
             "handles one width");
    if (keep == 4) {
      snprintf(d->fix, d->fix_cap,
               "keep the loop in float32: the float64 accesses are the "
               "minority, so retype those arrays (or convert outside the "
               "loop)");
    } else if (keep == 8) {
      snprintf(d->fix, d->fix_cap,
               "keep the loop in float64: the float32 accesses are the "
               "minority, so retype those arrays (or convert outside the "
               "loop)");
    } else {
      snprintf(d->fix, d->fix_cap, "keep the loop in a single float width");
    }
    ir_simd_diag(d, IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_byte_sum_accumulator(IrSimdBail *d) {
  if (d->has_byte_load && d->has_int_accum) {
    snprintf(d->reason, d->reason_cap,
             "this is a byte-sum loop, but the vpsadbw kernel accumulates "
             "into int64 and this loop's accumulator is narrower");
    snprintf(d->fix, d->fix_cap,
             "declare the accumulator as int64 (sum bytes as "
             "`total = total + (int64)data[i]`)");
    ir_simd_diag(d, IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_int32_sum_accumulator(IrSimdBail *d) {
  if (d->has_i32_load && d->int_accum_sym) {
    const char *acc_type =
        ir_function_local_declared_type(d->function, d->int_accum_sym);
    if (!acc_type || strcmp(acc_type, "int64") != 0) {
      snprintf(d->reason, d->reason_cap,
               "this loop sums int32 values into `%s`, which is %s. The int32 "
               "sum kernel widens each element and adds eight lanes into an "
               "int64 total, so it needs `%s` to be int64",
               d->int_accum_sym, acc_type ? acc_type : "narrower",
               d->int_accum_sym);
      snprintf(d->fix, d->fix_cap, "declare the accumulator `%s` as int64",
               d->int_accum_sym);
      ir_simd_diag(d, IR_SIMD_BAIL_I32_SUM_NARROW_ACC);
      return 1;
    }
  }
  return 0;
}

static int ir_simd_bail_body_local(IrSimdBail *d) {
  if (d->body_local) {
    if (strstr(d->body_local, "__inl_") != NULL) {
      snprintf(d->reason, d->reason_cap,
               "the body's data flow passes through the local `%s`, left over "
               "from an inlined call; the recognizers' "
               "load\xE2\x86\x92" "compute\xE2\x86\x92" "store matching "
               "cannot see through it",
               d->body_local);
      snprintf(d->fix, d->fix_cap,
               "the recognizers cannot see through an inlined call's locals "
               "yet; write the expression in the loop body and it vectorizes");
      ir_simd_diag(d, IR_SIMD_BAIL_INLINED_PARAM_LOCAL);
    } else {
      snprintf(d->reason, d->reason_cap,
               "the body declares the local `%s` each iteration; the "
               "recognizers' load\xE2\x86\x92" "compute\xE2\x86\x92" "store "
               "matching cannot see through it",
               d->body_local);
      snprintf(d->fix, d->fix_cap,
               "declare `%s` before the loop, or fold the expression in "
               "directly",
               d->body_local);
      ir_simd_diag(d, IR_SIMD_BAIL_BODY_LOCAL);
    }
    return 1;
  }
  return 0;
}

static int ir_simd_bail_dot_shape_address(IrSimdBail *d) {
  if (d->has_float_mul && d->has_float_accum && d->load_count >= 2) {
    snprintf(d->reason, d->reason_cap,
             "this is a float multiply and accumulate, a dot product, but no "
             "kernel matched its addresses: each base must be a plain "
             "pointer indexed by the loop counter, `base[i]`");
    snprintf(d->fix, d->fix_cap,
             "hoist invariant index math into a pointer before the loop: "
             "`var row: float32* = &m[r * cols];` and then `row[c]`");
    ir_simd_diag(d, IR_SIMD_BAIL_DOT_SHAPE_ADDRESS);
    return 1;
  }
  return 0;
}

static const IRInstruction *ir_simd_def_in(const IrSimdBail *d,
                                           const IROperand *op, size_t *at) {
  const IRInstruction *found = NULL;
  if (!op->name ||
      (op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL)) {
    return NULL;
  }
  for (size_t i = d->begin + 1; i < d->end; i++) {
    const IRInstruction *ins = &d->function->instructions[i];
    if (ins->op != IR_OP_STORE && ins->dest.kind == op->kind &&
        ins->dest.name && strcmp(ins->dest.name, op->name) == 0) {
      found = ins;
      if (at) {
        *at = i;
      }
    }
  }
  return found;
}

static int ir_simd_chain_loads(const IrSimdBail *d, const IROperand *op,
                               int depth) {
  const IRInstruction *def = ir_simd_def_in(d, op, NULL);
  if (!def || depth > 8) {
    return 0;
  }
  if (def->op == IR_OP_LOAD) {
    return 1;
  }
  return ir_simd_chain_loads(d, &def->lhs, depth + 1) ||
         ir_simd_chain_loads(d, &def->rhs, depth + 1);
}

static int ir_simd_same_address(const IrSimdBail *d, const IROperand *a,
                                const IROperand *b) {
  const IRInstruction *da;
  const IRInstruction *db;
  if (!a->name || !b->name) {
    return 0;
  }
  if (a->kind == b->kind && strcmp(a->name, b->name) == 0) {
    return 1;
  }
  da = ir_simd_def_in(d, a, NULL);
  db = ir_simd_def_in(d, b, NULL);
  return da && db && da->op == IR_OP_BINARY && db->op == IR_OP_BINARY &&
         da->text && db->text && strcmp(da->text, db->text) == 0 &&
         ir_operand_equals(&da->lhs, &db->lhs) &&
         (ir_operand_equals(&da->rhs, &db->rhs) ||
          ir_simd_same_address(d, &da->rhs, &db->rhs));
}

static int ir_simd_chain_loads_at(const IrSimdBail *d, const IROperand *op,
                                  const IROperand *address, int depth) {
  const IRInstruction *def = ir_simd_def_in(d, op, NULL);
  if (!def || depth > 8) {
    return 0;
  }
  if (def->op == IR_OP_LOAD) {
    return ir_simd_same_address(d, &def->lhs, address);
  }
  return ir_simd_chain_loads_at(d, &def->lhs, address, depth + 1) ||
         ir_simd_chain_loads_at(d, &def->rhs, address, depth + 1);
}

static const IROperand *ir_simd_index_operand(const IrSimdBail *d,
                                              const IROperand *address) {
  const IRInstruction *def = ir_simd_def_in(d, address, NULL);
  const IROperand *index;
  if (!def || def->op != IR_OP_BINARY || !def->text ||
      strcmp(def->text, "+") != 0) {
    return NULL;
  }
  index = &def->rhs;
  for (int hop = 0; hop < 4; hop++) {
    const IRInstruction *step = ir_simd_def_in(d, index, NULL);
    if (!step || !((step->op == IR_OP_BINARY && step->text &&
                    (strcmp(step->text, "<<") == 0 ||
                     strcmp(step->text, "*") == 0) &&
                    step->rhs.kind == IR_OPERAND_INT) ||
                   step->op == IR_OP_CAST)) {
      break;
    }
    index = &step->lhs;
  }
  return index;
}

static int ir_simd_bail_counter_store(IrSimdBail *d) {
  int varying = 0;
  if (d->store_count == 0 || d->load_count != 0) {
    return 0;
  }
  for (size_t i = d->begin + 1; i < d->end; i++) {
    const IRInstruction *ins = &d->function->instructions[i];
    if (ins->op == IR_OP_STORE && ir_simd_def_in(d, &ins->lhs, NULL)) {
      varying++;
    }
  }
  if (varying == 0) {
    return 0;
  }
  if (d->store_count == 1) {
    snprintf(d->reason, d->reason_cap,
             "each iteration stores a value computed from the loop counter, "
             "a ramp such as `a[i] = i`. The fill kernel stores one fixed "
             "value, and no kernel builds a ramp");
  } else {
    snprintf(d->reason, d->reason_cap,
             "%d of the body's %d stores write a value computed from the loop "
             "counter, a ramp such as `a[i] = i`. The fill kernel stores one "
             "fixed value, and no kernel builds a ramp",
             varying, d->store_count);
  }
  snprintf(d->fix, d->fix_cap,
           "nothing to change in the loop: the compiler has no ramp kernel. "
           "A loop that stores one fixed value, such as `a[i] = 0`, becomes "
           "a fill");
  ir_simd_advisory(d);
  ir_simd_diag(d, IR_SIMD_BAIL_COUNTER_STORE);
  return 1;
}

static int ir_simd_bail_scatter_store(IrSimdBail *d) {
  for (size_t i = d->begin + 1; i < d->end; i++) {
    const IRInstruction *ins = &d->function->instructions[i];
    const IROperand *index;
    if (ins->op != IR_OP_STORE) {
      continue;
    }
    index = ir_simd_index_operand(d, &ins->dest);
    if (!index || !ir_simd_chain_loads(d, index, 0)) {
      continue;
    }
    if (ir_simd_chain_loads_at(d, &ins->lhs, &ins->dest, 0)) {
      snprintf(d->reason, d->reason_cap,
               "this is a histogram: the element each iteration updates is "
               "chosen by a value the loop reads, so two lanes of one vector "
               "can land on the same element and a vector store would keep "
               "only one of their updates. Catching that collision takes "
               "AVX-512's vpconflictd, and AVX2 has nothing like it");
    } else {
      snprintf(d->reason, d->reason_cap,
               "the store's index is a value the loop reads from memory, a "
               "scatter. AVX2 can gather but cannot scatter, so each lane "
               "would be its own store anyway");
    }
    snprintf(d->fix, d->fix_cap,
             "nothing to change: the scalar loop is the floor for a scatter "
             "on this target");
    ir_simd_advisory(d);
    ir_simd_diag(d, IR_SIMD_BAIL_SCATTER_STORE);
    return 1;
  }
  return 0;
}

static const IRInstruction *ir_simd_split_sum(const IrSimdBail *d,
                                              const IROperand **extra) {
  for (size_t i = d->begin + 1; i < d->end; i++) {
    const IRInstruction *ins = &d->function->instructions[i];
    if (ins->op != IR_OP_BINARY || ins->is_float || !ins->text ||
        strcmp(ins->text, "+") != 0 || ins->dest.kind != IR_OPERAND_SYMBOL) {
      continue;
    }
    if (ir_simd_temp_adds_to(d, &ins->lhs, &ins->dest)) {
      *extra = &ins->rhs;
      return ins;
    }
    if (ir_simd_temp_adds_to(d, &ins->rhs, &ins->dest)) {
      *extra = &ins->lhs;
      return ins;
    }
  }
  return NULL;
}

static int ir_simd_bail_invariant_addend(IrSimdBail *d) {
  const IROperand *extra = NULL;
  const IRInstruction *sum = ir_simd_split_sum(d, &extra);
  const char *acc_type;
  char acc[64];
  if (!sum || !d->has_i32_load || ir_simd_chain_loads(d, extra, 0)) {
    return 0;
  }
  ir_simd_source_name(sum->dest.name, acc, sizeof(acc));
  acc_type = ir_function_local_declared_type(d->function, sum->dest.name);
  snprintf(d->reason, d->reason_cap,
           "each iteration adds an element to `%s` and also a term that is "
           "the same on every iteration. The sum kernel adds one element per "
           "lane and nothing else, so the extra term keeps it out",
           acc);
  snprintf(d->fix, d->fix_cap,
           "sum the elements alone, `%s = %s + a[i]`, and add the term once "
           "after the loop as `n * term`%s",
           acc, acc,
           acc_type && strcmp(acc_type, "int64") == 0
               ? ""
               : ", with the accumulator declared int64");
  ir_simd_diag(d, IR_SIMD_BAIL_INVARIANT_ADDEND);
  return 1;
}

static size_t ir_simd_arm_of(const IrSimdBail *d, size_t def_at) {
  size_t arm = 0;
  for (size_t i = d->begin + 1; i < def_at; i++) {
    const IRInstruction *ins = &d->function->instructions[i];
    if (ins->op != IR_OP_BRANCH_ZERO || !ins->text ||
        strncmp(ins->text, "ir_for_end", 10) == 0 ||
        strncmp(ins->text, "ir_while_end", 12) == 0) {
      continue;
    }
    for (size_t j = def_at + 1; j < d->end; j++) {
      const IRInstruction *lab = &d->function->instructions[j];
      if (lab->op == IR_OP_LABEL && lab->text &&
          strcmp(lab->text, ins->text) == 0) {
        arm = i + 1;
        break;
      }
    }
  }
  return arm;
}

static int ir_simd_def_in_arm(const IrSimdBail *d, size_t def_at) {
  return ir_simd_arm_of(d, def_at) != 0;
}

static int ir_simd_bail_chosen_index(IrSimdBail *d) {
  if (d->branch_count < 2) {
    return 0;
  }
  for (size_t i = d->begin + 1; i < d->end; i++) {
    const IRInstruction *ins = &d->function->instructions[i];
    const IROperand *index;
    size_t def_at = 0;
    char shown[96];
    char *cut;
    if (ins->op != IR_OP_LOAD) {
      continue;
    }
    index = ir_simd_index_operand(d, &ins->lhs);
    if (!index || index->kind != IR_OPERAND_SYMBOL ||
        !ir_simd_def_in(d, index, &def_at) || !ir_simd_def_in_arm(d, def_at)) {
      continue;
    }
    snprintf(shown, sizeof(shown), "%s", index->name);
    cut = strstr(shown, "__ssa");
    if (cut) {
      *cut = '\0';
    }
    cut = strstr(shown, "$$");
    if (cut) {
      *cut = '\0';
    }
    snprintf(d->reason, d->reason_cap,
             "the condition reads an element at `%s`, and `%s` is set inside "
             "the `if`, so each iteration's load waits on the choice the one "
             "before it made. That chain is serial, whatever the lanes",
             shown, shown);
    snprintf(d->fix, d->fix_cap,
             "carrying the best value beside its index (`if (a[i] > best) { "
             "best = a[i]; %s = i; }`) takes the waiting load out of each "
             "iteration. No kernel vectorizes an index of the maximum yet, so "
             "the loop stays scalar either way",
             shown);
    ir_simd_advisory(d);
    ir_simd_diag(d, IR_SIMD_BAIL_CHOSEN_INDEX);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_store_only_fill(IrSimdBail *d) {
  if (d->store_count > 0 && d->load_count == 0) {
    if (d->store_count > 1) {
      snprintf(d->reason, d->reason_cap,
               "the body writes %d destinations; the fill kernel fills one "
               "region per loop, so a body with several stores has no single "
               "region to fill",
               d->store_count);
      snprintf(d->fix, d->fix_cap,
               "split it into one loop per destination; each becomes its own "
               "fill kernel");
      ir_simd_diag(d, IR_SIMD_BAIL_STORE_ONLY_FILL);
      return 1;
    }
    if (d->rebased_array) {
      char element[64];
      ir_type_element_name(
          ir_function_local_declared_type(d->function, d->rebased_array), element,
          sizeof(element));
      snprintf(d->reason, d->reason_cap,
               "the loop fills the stack array `%s`, whose address is retaken "
               "on every iteration; the fill kernel indexes off one invariant "
               "base pointer, and a fresh base each iteration is not one",
               d->rebased_array);
      snprintf(d->fix, d->fix_cap,
               "bind the array to a pointer once before the loop "
               "(`var p: %s* = &%s[0];`) and write `p[i]` in the body",
               element, d->rebased_array);
    } else {
      snprintf(d->reason, d->reason_cap,
               "the loop only writes a value that does not change, a fill, "
               "but its store address did not match the fill vectorizer's "
               "shapes: a unit-stride element `a[i]`, `a[c + i]` with `c` a "
               "loop-invariant scalar, or a pointer walked by a constant "
               "stride");
      snprintf(d->fix, d->fix_cap,
               "hoist the invariant part of the index into a base pointer "
               "before the loop (`var row = &a[c]; ... row[i] = v;`) so the "
               "write is a plain unit-stride `row[i]`");
    }
    ir_simd_diag(d, IR_SIMD_BAIL_STORE_ONLY_FILL);
    return 1;
  }
  return 0;
}

static int ir_simd_bail_strided_access(IrSimdBail *d) {
  {
    long long stride = ir_region_strided_access(d->function, d->begin, d->end);
    if (stride > 1) {
      snprintf(d->reason, d->reason_cap,
               "the loop steps %lld elements at a time (`a[i*%lld]` or "
               "similar); every kernel walks its arrays one contiguous vector "
               "per iteration, and none of them gathers or scatters",
               stride, stride);
      snprintf(d->fix, d->fix_cap,
               "nothing to change here unless the layout can change: %s "
               "arrays (one per component) make each loop unit-stride and all "
               "of them vectorize",
               "separate");
      ir_simd_advisory(d);
      ir_simd_diag(d, IR_SIMD_BAIL_STRIDED_ACCESS);
      return 1;
    }
  }
  return 0;
}

static int ir_simd_bail_shift_shape(IrSimdBail *d) {
  {
    int shift_count = 0;
    int variable_shift = 0;
    for (size_t i = d->begin + 1; i < d->end; i++) {
      const IRInstruction *ins = &d->function->instructions[i];
      if (ins->op != IR_OP_BINARY || ins->is_float || !ins->text) {
        continue;
      }
      int is_shift = strcmp(ins->text, ">>") == 0 ||
                     strcmp(ins->text, "<<") == 0;
      if (!is_shift) {
        continue;
      }
      if (ins->rhs.kind == IR_OPERAND_INT) {
        if (strcmp(ins->text, ">>") == 0) {
          shift_count++;
        }
      } else {
        variable_shift++;
      }
    }
    if (variable_shift > 0 && d->load_count > 0) {
      snprintf(d->reason, d->reason_cap,
               "the body shifts by a value that is not a constant; the kernels "
               "carry a shift only when the distance is written in the source, "
               "so a distance read at run time has none");
      snprintf(d->fix, d->fix_cap,
               "nothing to change here unless the distance can be a constant: "
               "a loop per distance, or a constant, both vectorize. This is a "
               "gap in the compiler rather than a problem with the loop");
      ir_simd_advisory(d);
      ir_simd_diag(d, IR_SIMD_BAIL_VARIABLE_SHIFT);
      return 1;
    }
    if (shift_count > 0 && d->load_count > 0 && d->store_count > 0) {
      snprintf(d->reason, d->reason_cap,
               "the body shifts right, and the value being shifted cannot be "
               "shown to stay inside int32; the lanes are 32 bits wide, so a "
               "shift is only reproduced exactly when no wider intermediate "
               "could have been shifted");
      snprintf(d->fix, d->fix_cap,
               "mask the value down to the bits the shift needs: "
               "`(x & 65535) >> 8` bounds it inside int32 and the kernel then "
               "takes it. A constant multiplier is not enough on its own");
      ir_simd_diag(d, IR_SIMD_BAIL_UNBOUNDED_SHIFT);
      return 1;
    }
  }
  return 0;
}

static int ir_simd_bail_invariant_index_term(IrSimdBail *d) {
  {
    const char *base = NULL;
    if (ir_region_invariant_index_term(d->function, d->begin, d->end, &base)) {
      snprintf(d->reason, d->reason_cap,
               "the accesses into `%s` add a loop-invariant term to the "
               "counter (`%s[k + i]`); the kernels walk one base pointer from "
               "element 0, so the index must be the counter alone",
               base, base);
      snprintf(d->fix, d->fix_cap,
               "bind the row to a pointer before the loop (`var row = &%s[k];`) "
               "and index it with the counter (`row[i]`): same addresses, and "
               "the shape the kernels read",
               base);
      ir_simd_diag(d, IR_SIMD_BAIL_DOT_SHAPE_ADDRESS);
      return 1;
    }
  }
  return 0;
}
static const IrSimdBailRule IR_SIMD_BAIL_RULES[] = {
    ir_simd_bail_call_in_body,
    ir_simd_bail_indirect_call,
    ir_simd_bail_allocation,
    ir_simd_bail_inline_asm,
    ir_simd_bail_scatter_store,
    ir_simd_bail_chosen_index,
    ir_simd_bail_control_flow,
    ir_simd_bail_int16_elements,
    ir_simd_bail_int64_elements,
    ir_simd_bail_serial_recurrence,
    ir_simd_bail_reloaded_base,
    ir_simd_bail_mixed_float_widths,
    ir_simd_bail_byte_sum_accumulator,
    ir_simd_bail_invariant_addend,
    ir_simd_bail_int32_sum_accumulator,
    ir_simd_bail_body_local,
    ir_simd_bail_dot_shape_address,
    ir_simd_bail_counter_store,
    ir_simd_bail_store_only_fill,
    ir_simd_bail_strided_access,
    ir_simd_bail_shift_shape,
    ir_simd_bail_invariant_index_term,
};

static void ir_simd_explain_bail(const IRFunction *function, size_t begin,
                                 size_t end, char *reason, size_t reason_cap,
                                 char *fix, size_t fix_cap, int *diagnosis_out,
                                 int *advisory_out) {
  IrSimdBail d = {0};
  size_t i = 0;
  d.function = function;
  d.begin = begin;
  d.end = end;
  d.reason = reason;
  d.reason_cap = reason_cap;
  d.fix = fix;
  d.fix_cap = fix_cap;
  d.diagnosis_out = diagnosis_out;
  d.advisory_out = advisory_out;
  reason[0] = '\0';
  fix[0] = '\0';
  if (advisory_out) {
    *advisory_out = 0;
  }
  ir_simd_diag(&d, IR_SIMD_BAIL_UNRECOGNIZED_SHAPE);
  ir_simd_bail_scan(&d);
  for (i = 0; i < sizeof(IR_SIMD_BAIL_RULES) / sizeof(IR_SIMD_BAIL_RULES[0]);
       i++) {
    if (IR_SIMD_BAIL_RULES[i](&d)) {
      return;
    }
  }
  snprintf(d.reason, d.reason_cap,
           "no vectorizer recognized this loop's shape: by the time the "
           "vectorizers ran, its body had no call, branch, unsupported "
           "element width or carried dependence left to blame");
  snprintf(d.fix, d.fix_cap,
           "compare the loop against the shapes that do vectorize: "
           "unit-stride `a[i]` over int8, int32, float32 or float64, a "
           "straight-line body, and one of a map (`a[i] = expr`), a sum "
           "(`s = s + expr`) or a dot product");
  ir_simd_advisory(&d);
}

static int ir_simd_marker_id_at(const IRFunction *function, size_t begin) {
  const IRInstruction *marker = &function->instructions[begin];
  char which = 0;
  int id = 0, mode = 0;
  if (!ir_instruction_is_simd_marker(marker) ||
      sscanf(marker->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d", &which,
             &id, &mode) != 3) {
    return -1;
  }
  return id;
}

static int ir_simd_find_marker_region(const IRFunction *function, int id,
                                      size_t *begin_out, size_t *end_out) {
  size_t begin = (size_t)-1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    char which = 0;
    int marker_id = 0, mode = 0;
    if (!ir_instruction_is_simd_marker(ins) ||
        sscanf(ins->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d", &which,
               &marker_id, &mode) != 3 ||
        marker_id != id) {
      continue;
    }
    if (which == 'B') {
      begin = i;
    } else if (begin != (size_t)-1) {
      *begin_out = begin;
      *end_out = i;
      return 1;
    }
  }
  return 0;
}

#define IR_SIMD_FIX_INAPPLICABLE (-1)
typedef int (*IRSimdFixMutator)(IRFunction *clone, size_t begin, size_t end);

static int ir_simd_widen_accumulator(IRFunction *clone, size_t begin,
                                     size_t end, int cast_required) {
  int rewrote_cast = 0, rewrote_decl = 0;
  const char *acc_symbol = NULL;
  for (size_t i = begin + 1; i < end && !acc_symbol; i++) {
    const IRInstruction *ins = &clone->instructions[i];
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ir_operand_is_temp(&ins->rhs) &&
        ir_operand_is_symbol(&ins->lhs) &&
        ir_operand_names_match(&ins->lhs, &ins->dest)) {
      acc_symbol = ins->dest.name;
      for (size_t j = i; j-- > begin;) {
        IRInstruction *cast = &clone->instructions[j];
        if (cast->op == IR_OP_CAST && cast->dest.kind == IR_OPERAND_TEMP &&
            cast->dest.name && ir_operand_names_match(&cast->dest, &ins->rhs)) {
          mettle_free_string(cast->text);
          cast->text = mettle_strdup("int64");
          rewrote_cast = cast->text != NULL;
          break;
        }
      }
    }
  }
  if (!acc_symbol) {
    return 0;
  }
  for (size_t i = 0; i < clone->instruction_count; i++) {
    IRInstruction *decl = &clone->instructions[i];
    if (decl->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol(&decl->dest) &&
        strcmp(decl->dest.name, acc_symbol) == 0) {
      mettle_free_string(decl->text);
      decl->text = mettle_strdup("int64");
      rewrote_decl = decl->text != NULL;
      break;
    }
  }
  return rewrote_decl && (rewrote_cast || !cast_required);
}

static int ir_simd_mutate_byte_sum_int64(IRFunction *clone, size_t begin,
                                         size_t end) {
  return ir_simd_widen_accumulator(clone, begin, end, 1);
}

static int ir_simd_mutate_i32_sum_int64(IRFunction *clone, size_t begin,
                                        size_t end) {
  return ir_simd_widen_accumulator(clone, begin, end, 0);
}

static int ir_simd_retype_int_elems_to_i32(IRFunction *clone, size_t begin,
                                           size_t end, long long from_size,
                                           long long from_shift) {
  int rewrote = 0;
  for (size_t i = begin + 1; i < end; i++) {
    IRInstruction *ins = &clone->instructions[i];
    if ((ins->op == IR_OP_LOAD || ins->op == IR_OP_STORE) && !ins->is_float &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == from_size) {
      ins->rhs.int_value = 4;
      rewrote = 1;
      continue;
    }
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "<<") == 0 && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == from_shift &&
        ins->lhs.kind == IR_OPERAND_SYMBOL) {
      ins->rhs.int_value = 2;
    }
  }
  if (!rewrote) {
    return 0;
  }
  ir_simd_widen_accumulator(clone, begin, end, 0);
  return 1;
}

static int ir_simd_mutate_int16_to_i32(IRFunction *clone, size_t begin,
                                       size_t end) {
  return ir_simd_retype_int_elems_to_i32(clone, begin, end, 2, 1);
}

static int ir_simd_mutate_int64_to_i32(IRFunction *clone, size_t begin,
                                       size_t end) {
  return ir_simd_retype_int_elems_to_i32(clone, begin, end, 8, 3);
}

static long long ir_simd_majority_float_width(const IRFunction *function,
                                              size_t begin, size_t end) {
  int f32 = 0, f64 = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if ((ins->op != IR_OP_LOAD && ins->op != IR_OP_STORE) || !ins->is_float ||
        ins->rhs.kind != IR_OPERAND_INT) {
      continue;
    }
    if (ins->rhs.int_value == 4) {
      f32++;
    } else if (ins->rhs.int_value == 8) {
      f64++;
    }
  }
  if (!f32 || !f64) {
    return 0;
  }
  return (f32 >= f64) ? 4 : 8;
}

static int ir_simd_mutate_single_float_width(IRFunction *clone, size_t begin,
                                             size_t end) {
  long long keep = ir_simd_majority_float_width(clone, begin, end);
  if (!keep) {
    return 0;
  }
  long long drop = (keep == 4) ? 8 : 4;
  long long keep_shift = (keep == 4) ? 2 : 3;
  long long drop_shift = (keep == 4) ? 3 : 2;

  char retyped[32][64];
  size_t retyped_count = 0;

  int rewrote = 0;
  for (size_t i = begin + 1; i < end; i++) {
    IRInstruction *ins = &clone->instructions[i];
    if ((ins->op == IR_OP_LOAD || ins->op == IR_OP_STORE) && ins->is_float &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == drop) {
      ins->rhs.int_value = keep;
      rewrote = 1;
      if (ins->op == IR_OP_LOAD && ins->dest.kind == IR_OPERAND_TEMP &&
          ins->dest.name && retyped_count < 32) {
        snprintf(retyped[retyped_count], sizeof(retyped[0]), "%s",
                 ins->dest.name);
        retyped_count++;
      }
      continue;
    }
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "<<") == 0 && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == drop_shift &&
        ins->lhs.kind == IR_OPERAND_SYMBOL) {
      ins->rhs.int_value = keep_shift;
      continue;
    }
    if (ins->op == IR_OP_CAST && ins->is_float &&
        ir_operand_is_temp(&ins->lhs)) {
      for (size_t r = 0; r < retyped_count; r++) {
        if (strcmp(retyped[r], ins->lhs.name) == 0) {
          ins->op = IR_OP_ASSIGN;
          break;
        }
      }
    }
  }
  return rewrote;
}

static int ir_simd_symbol_written_in_region(const IRFunction *function,
                                            size_t begin, size_t end,
                                            const char *sym) {
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        strcmp(ins->dest.name, sym) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_simd_symbol_is_region_invariant(const IRFunction *function,
                                              size_t begin, size_t end,
                                              const char *sym,
                                              const char *iv) {
  if (!sym || strcmp(sym, iv) == 0) {
    return 0;
  }
  return !ir_simd_symbol_written_in_region(function, begin, end, sym);
}

static int ir_simd_temp_is_region_invariant(const IRFunction *function,
                                            size_t begin, size_t end,
                                            size_t before, const char *temp,
                                            const char *iv, int depth) {
  if (depth > 4) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, temp);
  if (!producer) {
    return 0;
  }
  const IROperand *sides[2] = {&producer->lhs, &producer->rhs};
  for (int s = 0; s < 2; s++) {
    if (sides[s]->kind == IR_OPERAND_SYMBOL && sides[s]->name &&
        !ir_simd_symbol_is_region_invariant(function, begin, end,
                                            sides[s]->name, iv)) {
      return 0;
    }
    if (sides[s]->kind == IR_OPERAND_TEMP &&
        (!sides[s]->name ||
         !ir_simd_temp_is_region_invariant(function, begin, end, before,
                                           sides[s]->name, iv, depth + 1))) {
      return 0;
    }
  }
  return 1;
}

static const char *ir_simd_region_induction_variable(const IRFunction *clone,
                                                     size_t begin,
                                                     size_t end) {
  int past_header = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &clone->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (past_header && ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "<") == 0 && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ins->lhs.name) {
      return ins->lhs.name;
    }
  }
  return NULL;
}

static IRInstruction *ir_simd_index_producer(IRFunction *clone, size_t begin,
                                             size_t at, const char *want) {
  IRInstruction *idx = NULL;
  for (;;) {
    idx = NULL;
    for (size_t j = at; j-- > begin;) {
      IRInstruction *cand = &clone->instructions[j];
      if (ir_instruction_writes_destination(cand) &&
          ir_operand_is_temp(&cand->dest) &&
          strcmp(cand->dest.name, want) == 0) {
        idx = cand;
        break;
      }
    }
    if (!idx || idx->op != IR_OP_CAST || idx->is_float ||
        idx->lhs.kind != IR_OPERAND_TEMP || !idx->lhs.name) {
      return idx;
    }
    want = idx->lhs.name;
  }
}

static const IROperand *ir_simd_row_invariant_half(IRFunction *clone,
                                                   size_t begin, size_t end,
                                                   size_t at,
                                                   const IRInstruction *idx,
                                                   const char *iv,
                                                   int *variant_index_seen) {
  const IROperand *other = NULL;
  if (ir_operand_is_symbol(&idx->lhs) &&
      strcmp(idx->lhs.name, iv) == 0) {
    other = &idx->rhs;
  } else if (ir_operand_is_symbol(&idx->rhs) &&
             strcmp(idx->rhs.name, iv) == 0) {
    other = &idx->lhs;
  } else {
    return NULL;
  }
  if (other->kind == IR_OPERAND_SYMBOL) {
    if (!ir_simd_symbol_is_region_invariant(clone, begin, end, other->name,
                                            iv)) {
      *variant_index_seen = 1;
      return NULL;
    }
  } else if (other->kind == IR_OPERAND_TEMP) {
    if (!other->name ||
        !ir_simd_temp_is_region_invariant(clone, begin, end, at, other->name,
                                          iv, 0)) {
      *variant_index_seen = 1;
      return NULL;
    }
  } else if (other->kind != IR_OPERAND_INT) {
    return NULL;
  }
  return other;
}

static IRInstruction *ir_simd_address_add(IRFunction *clone, size_t at,
                                          size_t end, const char *shifted) {
  for (size_t k = at + 1; k < end; k++) {
    IRInstruction *cand = &clone->instructions[k];
    if (cand->op == IR_OP_BINARY && !cand->is_float && cand->text &&
        strcmp(cand->text, "+") == 0 && cand->lhs.kind == IR_OPERAND_SYMBOL &&
        cand->lhs.name && ir_operand_is_temp(&cand->rhs) &&
        strcmp(cand->rhs.name, shifted) == 0) {
      return cand;
    }
  }
  return NULL;
}

static int ir_simd_is_index_scale(const IRInstruction *shl) {
  return shl->op == IR_OP_BINARY && !shl->is_float && shl->text &&
         strcmp(shl->text, "<<") == 0 && shl->rhs.kind == IR_OPERAND_INT &&
         (shl->rhs.int_value == 2 || shl->rhs.int_value == 3) &&
         ir_operand_is_temp(&shl->lhs) &&
         ir_operand_is_temp(&shl->dest);
}

static int ir_simd_mutate_dot_row_pointer(IRFunction *clone, size_t begin,
                                          size_t end) {
  const char *iv = ir_simd_region_induction_variable(clone, begin, end);
  int rewrites = 0;
  int variant_index_seen = 0;

  if (!iv) {
    return 0;
  }

  for (size_t i = begin + 1; i < end; i++) {
    IRInstruction *shl = &clone->instructions[i];
    IRInstruction *idx;
    IRInstruction *addr;
    const char *elem_type;
    char row_name[32];

    if (!ir_simd_is_index_scale(shl)) {
      continue;
    }
    idx = ir_simd_index_producer(clone, begin, i, shl->lhs.name);
    if (!idx || idx->op != IR_OP_BINARY || idx->is_float || !idx->text ||
        strcmp(idx->text, "+") != 0) {
      continue;
    }
    if (!ir_simd_row_invariant_half(clone, begin, end, i, idx, iv,
                                    &variant_index_seen)) {
      continue;
    }
    addr = ir_simd_address_add(clone, i, end, shl->dest.name);
    if (!addr) {
      continue;
    }

    snprintf(row_name, sizeof(row_name), "__hypo_row%d", rewrites);
    elem_type = (shl->rhs.int_value == 3) ? "float64*" : "float32*";

    ir_operand_destroy(&shl->lhs);
    shl->lhs = ir_operand_symbol(iv);
    ir_operand_destroy(&addr->lhs);
    addr->lhs = ir_operand_symbol(row_name);
    ir_instruction_destroy_storage(idx);
    memset(idx, 0, sizeof(*idx));
    idx->op = IR_OP_DECLARE_LOCAL;
    idx->dest = ir_operand_symbol(row_name);
    idx->text = mettle_strdup(elem_type);
    rewrites++;
  }
  if (rewrites > 0) {
    return 1;
  }
  return variant_index_seen ? IR_SIMD_FIX_INAPPLICABLE : 0;
}

static int ir_simd_mutate_fill_base_pointer(IRFunction *clone, size_t begin,
                                            size_t end) {
  int rewrites = 0;
  int past_header = 0;

  for (size_t i = begin + 1; i < end; i++) {
    IRInstruction *addr_of = &clone->instructions[i];
    if (addr_of->op == IR_OP_LABEL) {
      if (addr_of->text && (strstr(addr_of->text, "ir_while_") != NULL ||
                            strstr(addr_of->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (!past_header || addr_of->op != IR_OP_ADDRESS_OF ||
        addr_of->lhs.kind != IR_OPERAND_SYMBOL || !addr_of->lhs.name ||
        addr_of->dest.kind != IR_OPERAND_TEMP || !addr_of->dest.name) {
      continue;
    }
    const char *declared =
        ir_function_local_declared_type(clone, addr_of->lhs.name);
    if (!declared || !strchr(declared, '[')) {
      continue;
    }

    IRInstruction *addr = NULL;
    for (size_t k = i + 1; k < end; k++) {
      IRInstruction *cand = &clone->instructions[k];
      if (cand->op == IR_OP_BINARY && !cand->is_float && cand->text &&
          strcmp(cand->text, "+") == 0 &&
          ir_operand_is_temp(&cand->lhs) &&
          ir_operand_names_match(&cand->lhs, &addr_of->dest)) {
        addr = cand;
        break;
      }
    }
    if (!addr) {
      continue;
    }

    ir_operand_destroy(&addr->lhs);
    addr->lhs = ir_operand_symbol(addr_of->lhs.name);
    ir_instruction_destroy_storage(addr_of);
    memset(addr_of, 0, sizeof(*addr_of));
    addr_of->op = IR_OP_NOP;
    rewrites++;
  }
  return rewrites > 0;
}

static int ir_simd_mutate_hoist_body_local(IRFunction *clone, size_t begin,
                                           size_t end) {
  size_t header = 0, decl = 0;
  for (size_t i = begin + 1; i < end && i < clone->instruction_count; i++) {
    IRInstruction *ins = &clone->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (!header && ir_label_is_loop_header(ins->text)) {
        header = i;
      }
      continue;
    }
    if (header && ins->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol(&ins->dest)) {
      decl = i;
      break;
    }
  }
  if (!header || !decl) {
    return 0;
  }
  IRInstruction saved = clone->instructions[decl];
  memmove(&clone->instructions[header + 1], &clone->instructions[header],
          (decl - header) * sizeof(IRInstruction));
  clone->instructions[header] = saved;
  return 1;
}

static const struct {
  IRSimdBailId diagnosis;
  IRSimdFixMutator mutate;
  const char *inapplicable_fix;
} g_simd_fix_transforms[] = {
    {IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC, ir_simd_mutate_byte_sum_int64, NULL},
    {IR_SIMD_BAIL_I32_SUM_NARROW_ACC, ir_simd_mutate_i32_sum_int64, NULL},
    {IR_SIMD_BAIL_INT16_ELEMENTS, ir_simd_mutate_int16_to_i32, NULL},
    {IR_SIMD_BAIL_INT64_ELEMENTS, ir_simd_mutate_int64_to_i32, NULL},
    {IR_SIMD_BAIL_STORE_ONLY_FILL, ir_simd_mutate_fill_base_pointer, NULL},
    {IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS, ir_simd_mutate_single_float_width, NULL},
    {IR_SIMD_BAIL_BODY_LOCAL, ir_simd_mutate_hoist_body_local, NULL},
    {IR_SIMD_BAIL_DOT_SHAPE_ADDRESS, ir_simd_mutate_dot_row_pointer,
     "none via hoisting. Re-checked: the index half that is not the loop "
     "counter changes every iteration, so it cannot be hoisted out; this "
     "access is genuinely non-unit-stride"},
};

#define IR_SIMD_FIX_PARTIAL 2
#define IR_SIMD_FIX_DEAD_END 3
static int ir_explain_simulate_fix(const IRFunction *function, size_t begin,
                                   size_t end, IRSimdFixMutator mutate,
                                   int diagnosis, char *desc, size_t desc_cap,
                                   char *next_reason, size_t next_reason_cap) {
  int marker_id = ir_simd_marker_id_at(function, begin);
  if (marker_id < 0) {
    return 0;
  }

  IRFunction *clone = ir_explain_clone_function(function);
  if (!clone) {
    return 0;
  }
  int mutated = mutate(clone, begin, end);
  if (mutated <= 0) {
    ir_function_destroy(clone);
    return mutated;
  }

  ir_explain_set_hypothesis(1);
  int ran = ir_optimize_function_revectorize(clone);
  ir_explain_set_hypothesis(0);

  int verified = 0;
  size_t new_begin = 0, new_end = 0;
  if (ran && ir_simd_find_marker_region(clone, marker_id, &new_begin,
                                        &new_end)) {
    const IRInstruction *kernel =
        ir_region_vectorized_ins(clone, new_begin, new_end, 0);
    if (kernel) {
      ir_explain_kernel_desc(kernel, desc, desc_cap);
      verified = 1;
    } else if (next_reason && next_reason_cap) {
      int next_diagnosis = IR_SIMD_BAIL_NONE, next_advisory = 0;
      char scratch[320];
      ir_simd_explain_bail(clone, new_begin, new_end, next_reason,
                           next_reason_cap, scratch, sizeof(scratch),
                           &next_diagnosis, &next_advisory);
      if (next_diagnosis != IR_SIMD_BAIL_NONE && next_diagnosis != diagnosis) {
        verified = next_advisory &&
                           next_diagnosis != IR_SIMD_BAIL_UNRECOGNIZED_SHAPE
                       ? IR_SIMD_FIX_DEAD_END
                       : IR_SIMD_FIX_PARTIAL;
      } else {
        next_reason[0] = '\0';
      }
    }
  }
  ir_function_destroy(clone);
  return verified;
}

static int ir_explain_try_fix_for_diagnosis(const IRFunction *function,
                                            size_t begin, size_t end,
                                            int diagnosis, char *desc,
                                            size_t desc_cap,
                                            const char **inapplicable_fix_out,
                                            char *next_reason,
                                            size_t next_reason_cap) {
  size_t transform_count =
      sizeof(g_simd_fix_transforms) / sizeof(g_simd_fix_transforms[0]);
  for (size_t t = 0; t < transform_count; t++) {
    if ((int)g_simd_fix_transforms[t].diagnosis == diagnosis) {
      int result = ir_explain_simulate_fix(
          function, begin, end, g_simd_fix_transforms[t].mutate, diagnosis,
          desc, desc_cap, next_reason, next_reason_cap);
      if (result == IR_SIMD_FIX_INAPPLICABLE && inapplicable_fix_out) {
        *inapplicable_fix_out = g_simd_fix_transforms[t].inapplicable_fix;
      }
      return result;
    }
  }
  return 0;
}

static int ir_explain_simulate_inline_fix(const IRFunction *function,
                                          size_t begin, size_t end,
                                          char *callee_out, size_t callee_cap,
                                          char *desc, size_t desc_cap,
                                          int *was_noinline_out,
                                          const char **decline_reason_out,
                                          int *nest_after_out) {
  *decline_reason_out = NULL;
  *nest_after_out = 0;
  if (!g_explain_program) {
    return 0;
  }
  int marker_id = ir_simd_marker_id_at(function, begin);
  if (marker_id < 0) {
    return 0;
  }

  const char *callee = NULL;
  int past_header = 0;
  for (size_t i = begin + 1; i < end && !callee; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    if (past_header && ins->op == IR_OP_CALL && ins->text &&
        strstr(ins->text, "crash_trap") == NULL) {
      callee = ins->text;
    }
  }
  if (!callee) {
    return 0;
  }
  snprintf(callee_out, callee_cap, "%s", callee);

  IRFunction *clone = ir_explain_clone_function(function);
  if (!clone) {
    return 0;
  }

  ir_explain_set_hypothesis(1);
  int verified = 0;
  if (ir_inline_explain_simulate_force_inline(g_explain_program, clone, callee,
                                              was_noinline_out,
                                              decline_reason_out)) {
    if (ir_optimize_function_revectorize(clone)) {
      size_t new_begin = 0, new_end = 0;
      if (ir_simd_find_marker_region(clone, marker_id, &new_begin, &new_end)) {
        size_t before = ir_region_loop_header_count(function, begin, end);
        size_t after = ir_region_loop_header_count(clone, new_begin, new_end);
        const IRInstruction *kernel =
            ir_region_vectorized_ins(clone, new_begin, new_end, 1);
        if (after == 0 && kernel) {
          ir_explain_kernel_desc(kernel, desc, desc_cap);
          verified = 1;
        } else {
          *nest_after_out = (after > before);
          verified = -1;
        }
      }
    }
  } else if (*decline_reason_out) {
    verified = -1;
  }
  ir_explain_set_hypothesis(0);
  ir_function_destroy(clone);
  return verified;
}

static void ir_clear_simd_markers(IRFunction *function) {
  size_t write = 0;
  for (size_t read = 0; read < function->instruction_count; read++) {
    IRInstruction *instruction = &function->instructions[read];
    if (ir_instruction_is_simd_marker(instruction)) {
      ir_instruction_destroy_storage(instruction);
      continue;
    }
    if (write != read) {
      function->instructions[write] = *instruction;
    }
    write++;
  }
  if (write != function->instruction_count) {
    function->instruction_count = write;
    function->cfg_valid = 0;
  }
}

#define IR_SIMD_MAX_NESTING 64
#define IR_SIMD_MAX_LOOPS 256

typedef struct {
  size_t begin;
  size_t end;
  int mode;
  SourceLocation location;
} IRSimdLoopRecord;

static size_t ir_simd_loop_end_line(const IRFunction *function,
                                    const IRSimdLoopRecord *loop) {
  size_t last = loop->location.line;
  if (!function) {
    return last;
  }
  for (size_t i = loop->begin;
       i <= loop->end && i < function->instruction_count; i++) {
    const SourceLocation *at = &function->instructions[i].location;
    if (at->line <= last) {
      continue;
    }
    if (at->filename && loop->location.filename &&
        strcmp(at->filename, loop->location.filename) != 0) {
      continue;
    }
    last = at->line;
  }
  return last;
}


typedef struct {
  char *fix;
  size_t fix_cap;
  char *verified;
  size_t verified_cap;
  char *partial;
  size_t partial_cap;
} IrFixText;

static int ir_explain_fix_verdict(int sim, const char *inapplicable_fix,
                                  const char *next_reason,
                                  const char *kernel_desc, IrFixText *t) {
  if (sim == 1) {
    snprintf(t->verified, t->verified_cap,
             "simulated that fix and re-ran the optimizer: this loop "
             "then vectorizes \xE2\x86\x92 %s",
             kernel_desc);
  } else if (sim == IR_SIMD_FIX_INAPPLICABLE && inapplicable_fix) {
    snprintf(t->fix, t->fix_cap, "%s", inapplicable_fix);
    return 1;
  } else if (sim == IR_SIMD_FIX_PARTIAL && next_reason[0]) {
    size_t used = strlen(t->fix);
    snprintf(t->fix + used, t->fix_cap - used, " (first step only)");
    snprintf(t->partial, t->partial_cap,
             "re-checked with that change applied: the loop still does "
             "not vectorize, because %s",
             next_reason);
  } else if (sim == IR_SIMD_FIX_DEAD_END && next_reason[0]) {
    char tried[256];
    snprintf(tried, sizeof(tried), "%s", t->fix);
    snprintf(t->fix, t->fix_cap,
             "nothing worth changing here. The compiler applied \"%s\" to "
             "a copy of the loop, and it still would not vectorize",
             tried);
    snprintf(t->partial, t->partial_cap, "with that change, %s", next_reason);
    return 1;
  }
  return 0;
}

static void ir_explain_report_loop(const IRFunction *function,
                                   const IRSimdLoopRecord *loops,
                                   size_t loop_count, size_t k) {
const IRSimdLoopRecord *L = &loops[k];
if (!ir_explain_location_enabled(&L->location)) {
  
}
const IRInstruction *own =
    ir_region_vectorized_ins(function, L->begin, L->end, 0);
if (!own) {
  own = ir_region_skipahead_ins(function, L->begin, L->end);
}
const IRInstruction *any =
    own ? own : ir_region_vectorized_ins(function, L->begin, L->end, 1);

size_t nest_depth = 1;
for (size_t m = 0; m < loop_count; m++) {
  if (m != k && loops[m].begin < L->begin && loops[m].end > L->end) {
    nest_depth++;
  }
}

int has_inner = 0;
size_t inner_line = 0;
for (size_t m = 0; m < loop_count; m++) {
  if (m == k || loops[m].begin <= L->begin || loops[m].end >= L->end) {
    continue;
  }
  has_inner = 1;
  if (inner_line == 0) {
    inner_line = loops[m].location.line;
  }
  if (!own && any &&
      ir_region_vectorized_ins(function, loops[m].begin, loops[m].end, 1)) {
    inner_line = loops[m].location.line;
  }
}
size_t inlined_loop_from_line = 0;
char inlined_loop_callee[128];
inlined_loop_callee[0] = '\0';
if (!has_inner) {
  size_t structural = ir_region_inner_loop_line(function, L->begin, L->end);
  if (structural) {
    has_inner = 1;
    inner_line = structural;
    size_t last = ir_simd_loop_end_line(function, L);
    if (ir_explain_inlined_calls_in_range(
            function->name, L->location.line, last ? last : L->location.line,
            &inlined_loop_from_line, inlined_loop_callee,
            sizeof(inlined_loop_callee)) != 1) {
      inlined_loop_from_line = 0;
      inlined_loop_callee[0] = '\0';
    }
  }
}

char headline[192], reason[320], fix[320];
  if (own) {
  char desc[128];
  ir_explain_kernel_desc(own, desc, sizeof(desc));
  snprintf(headline, sizeof(headline), "vectorized \xE2\x86\x92 %s", desc);
  ir_explain_remark(function->name, "loop", L->location, 1, headline, NULL,
                    NULL, NULL);
  ir_explain_remark_code("vectorized");
  ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
  } else if (any) {
  snprintf(reason, sizeof(reason),
           "only the innermost loop of a nest is vectorized; this loop "
           "drives the vectorized inner loop (line %zu)",
           inner_line);
  ir_explain_remark(function->name, "loop", L->location, 1,
                    "vectorized inner, scalar outer", reason, NULL, NULL);
  ir_explain_remark_code("vectorized-inner");
  ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
  } else if (has_inner) {
  if (inlined_loop_callee[0]) {
    snprintf(reason, sizeof(reason),
             "the call to `%s` on line %zu was inlined, so that callee's "
             "loop (line %zu) now sits in this body; only innermost loops "
             "are vectorized",
             inlined_loop_callee, inlined_loop_from_line, inner_line);
    snprintf(fix, sizeof(fix),
             "nothing to change on this line: this loop drives the work, "
             "and the vectorizable part is `%s`'s loop. See the remark "
             "on line %zu",
             inlined_loop_callee, inner_line);
  } else {
    snprintf(reason, sizeof(reason),
             "the body contains a nested loop (line %zu), and only "
             "innermost loops are vectorized; the inner loop did not "
             "vectorize either. See its remark",
             inner_line);
    snprintf(fix, sizeof(fix),
             "nothing to change on this line: this loop drives the nest, "
             "so the fix belongs on the inner loop at line %zu",
             inner_line);
  }
  ir_explain_remark(function->name, "loop", L->location, 0,
                    "NOT vectorized", reason, fix, NULL);
  ir_explain_remark_code("outer-of-nest");
  ir_explain_remark_advisory();
  ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
  } else if (!ir_region_has_loop_label(function, L->begin, L->end)) {
  if (!ir_explain_has_remark_at(L->location.line, "loop")) {
    ir_explain_remark(function->name, "loop", L->location, 1,
                      "eliminated: no loop remains after "
                      "optimization (fully unrolled or folded away)",
                      NULL, NULL, NULL);
    ir_explain_remark_code("eliminated");
    ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
  }
  } else {
  int diagnosis = IR_SIMD_BAIL_NONE;
  int advisory = 0;
  ir_simd_explain_bail(function, L->begin, L->end, reason, sizeof(reason),
                       fix, sizeof(fix), &diagnosis, &advisory);
  char verified[512], partial[512];
  verified[0] = partial[0] = '\0';
  char kernel_desc[128];
  if (diagnosis == IR_SIMD_BAIL_CALL_IN_BODY) {
    char callee[128];
    int was_noinline = 0;
    const char *decline_reason = NULL;
    int nest_after = 0;
    int sim = ir_explain_simulate_inline_fix(
        function, L->begin, L->end, callee, sizeof(callee), kernel_desc,
        sizeof(kernel_desc), &was_noinline, &decline_reason, &nest_after);
    if (sim == -1) {
      char shown[128];
      ir_display_name(callee, shown, sizeof(shown));
      snprintf(callee, sizeof(callee), "%s", shown);
      if (decline_reason) {
        snprintf(reason, sizeof(reason),
                 "each iteration calls `%s`, and `@inline` cannot help: "
                 "%s",
                 callee, decline_reason);
      } else if (nest_after) {
        snprintf(reason, sizeof(reason),
                 "each iteration calls `%s`; even with it inlined, its "
                 "loops would land in this body, making this the outer "
                 "loop of a nest, and only innermost loops vectorize",
                 callee);
      } else {
        snprintf(reason, sizeof(reason),
                 "each iteration calls `%s`; the compiler simulated "
                 "inlining it, and this loop still does not vectorize, "
                 "so inlining is not the blocker",
                 callee);
      }
      snprintf(fix, sizeof(fix),
               "nothing to change on this line: this loop is a driver "
               "and scalar is the right code for it. The vectorizable "
               "work is inside `%s`, so check the remarks on its loops",
               callee);
      advisory = 1;
    } else if (sim == 1) {
      if (was_noinline) {
        snprintf(fix, sizeof(fix),
                 "remove `@noinline` from `%s` (it blocks this loop's "
                 "vectorization), or hoist the call out of the loop",
                 callee);
        snprintf(verified, sizeof(verified),
                 "simulated removing `@noinline` from `%s` and re-ran "
                 "the inliner and the optimizer: this loop then "
                 "vectorizes \xE2\x86\x92 %s",
                 callee, kernel_desc);
      } else {
        snprintf(verified, sizeof(verified),
                 "simulated marking `%s` @inline and re-ran the inliner "
                 "and the optimizer: this loop then vectorizes "
                 "\xE2\x86\x92 %s",
                 callee, kernel_desc);
      }
    }
  } else {
    const char *inapplicable_fix = NULL;
    char next_reason[320];
    next_reason[0] = '\0';
    int sim = ir_explain_try_fix_for_diagnosis(
        function, L->begin, L->end, diagnosis, kernel_desc,
        sizeof(kernel_desc), &inapplicable_fix, next_reason,
        sizeof(next_reason));
    IrFixText text = {fix, sizeof(fix), verified, sizeof(verified), partial,
                      sizeof(partial)};
    advisory |= ir_explain_fix_verdict(sim, inapplicable_fix, next_reason,
                                       kernel_desc, &text);
  }
  ir_explain_remark(function->name, "loop", L->location, 0,
                    "NOT vectorized", reason, fix[0] ? fix : NULL,
                    verified[0] ? verified : NULL);
  if (partial[0]) {
    ir_explain_remark_partial(partial);
  }
  if (advisory) {
    ir_explain_remark_advisory();
  }
  ir_explain_remark_code(ir_simd_bail_id_name(diagnosis));
  ir_explain_remark_extent(ir_simd_loop_end_line(function, L));
  }
ir_explain_remark_loop_depth(L->location.line, nest_depth);

}

static void ir_explain_report_each_loop(const IRFunction *function,
                                        size_t loop_count,
                                        const IRSimdLoopRecord *loops) {
  for (size_t k = 0; k < loop_count; k++) {
    ir_explain_report_loop(function, loops, loop_count, k);
  }
}

static void ir_explain_report_loops(const IRFunction *function,
                                    const IRSimdLoopRecord *loops,
                                    size_t loop_count) {
  ir_explain_report_each_loop(function, loop_count, loops);
}

int ir_verify_simd_contracts(IRFunction *function) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }

  struct {
    int mode;
    size_t begin_index;
    SourceLocation location;
  } open[IR_SIMD_MAX_NESTING];
  IRSimdLoopRecord loops[IR_SIMD_MAX_LOOPS];
  size_t loop_count = 0;
  int depth = 0;
  int had_fatal = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!ir_instruction_is_simd_marker(instruction)) {
      continue;
    }

    char which = 0;
    int id = 0, mode = 0;
    if (sscanf(instruction->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d",
               &which, &id, &mode) != 3) {
      continue;
    }

    if (which == 'B') {
      if (depth < IR_SIMD_MAX_NESTING) {
        open[depth].mode = mode;
        open[depth].begin_index = i;
        open[depth].location = instruction->location;
      }
      depth++;
      continue;
    }

    if (depth <= 0) {
      continue;
    }
    depth--;
    if (depth >= IR_SIMD_MAX_NESTING) {
      continue;
    }

    size_t begin_index = open[depth].begin_index;
    int loop_mode = open[depth].mode;
    SourceLocation loc = open[depth].location;
    const char *file = loc.filename ? loc.filename : "<input>";

    if (loop_count < IR_SIMD_MAX_LOOPS) {
      loops[loop_count].begin = begin_index;
      loops[loop_count].end = i;
      loops[loop_count].mode = loop_mode;
      loops[loop_count].location = loc;
      loop_count++;
    }

    if (loop_mode == SIMD_ATTR_REPORT) {
      continue;
    }

    int vec_op = ir_region_vectorized_op(function, begin_index, i);
    if (vec_op < 0 && ir_region_skipahead_ins(function, begin_index, i)) {
      vec_op = (int)IR_OP_SIMD_FIND;
    }
    if (vec_op >= 0) {
      if (g_simd_report) {
        fprintf(stderr, "%s:%zu:%zu: note: @simd loop vectorized (%s)\n", file,
                loc.line, loc.column, ir_opcode_name((IROpcode)vec_op));
      }
      continue;
    }

    const char *reason = ir_simd_bail_reason(function, begin_index, i);
    if (loop_mode == SIMD_ATTR_CONTRACT &&
        ir_program_lookup_symbol(ir_optimize_get_program(),
                                 "mettle_safety_call_enter")) {
      fprintf(stderr,
              "%s:%zu:%zu: warning: @simd! loop stays scalar under --safe: %s\n",
              file, loc.line, loc.column, reason);
    } else if (loop_mode == SIMD_ATTR_CONTRACT) {
      fprintf(stderr, "%s:%zu:%zu: error: @simd! loop was not vectorized: %s\n",
              file, loc.line, loc.column, reason);
      g_simd_contract_user_error = 1;
      had_fatal = 1;
    } else {
      fprintf(stderr,
              "%s:%zu:%zu: warning: @simd loop was not vectorized: %s\n", file,
              loc.line, loc.column, reason);
    }
  }

  if (ir_explain_enabled()) {
    ir_explain_report_loops(function, loops, loop_count);
  }

  ir_clear_simd_markers(function);
  return had_fatal ? 0 : 1;
}

void ir_note_simd_contracts_unverified(IRProgram *program) {
  if (!program) {
    return;
  }
  int marker_count = 0;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (!ir_instruction_is_simd_marker(&function->instructions[i])) {
        continue;
      }
      char which = 0;
      int id = 0, mode = 0;
      if (sscanf(function->instructions[i].text +
                     strlen(IR_SIMD_MARKER_PREFIX),
                 "%c:%d:%d", &which, &id, &mode) == 3 &&
          which == 'B' && mode != SIMD_ATTR_REPORT) {
        marker_count++;
      }
    }
    ir_clear_simd_markers(function);
  }
  if (marker_count > 0) {
    fprintf(stderr,
            "note: %d `@simd` loop%s present but not verified; vectorization "
            "contracts are only checked with -O/--release\n",
            marker_count, marker_count == 1 ? "" : "s");
  }

  int contract_count = 0;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (function && (function->is_inline_contract || function->is_noalloc)) {
      contract_count++;
    }
  }
  if (contract_count > 0) {
    fprintf(stderr,
            "note: %d `@inline!`/`@noalloc` contract%s present but not "
            "verified; contracts are only checked with -O/--release\n",
            contract_count, contract_count == 1 ? "" : "s");
  }
}
