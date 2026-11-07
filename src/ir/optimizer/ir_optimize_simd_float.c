#include "ir_optimize_internal.h"
#include "../ir_explain_ledger.h"

static int ir_decode_float_indexed_load(IRFunction *function, size_t before,
                                        const char *load_temp, const char *iv,
                                        const char **base_out, int *bits_out) {
  const IRInstruction *load = NULL;
  const IRInstruction *addr = NULL;
  const IRInstruction *shl = NULL;
  long long size = 0;
  long long shift = 0;

  if (!load_temp || !iv || !base_out || !bits_out) {
    return 0;
  }
  load = ir_find_temp_producer_before(function, before, load_temp);
  if (!load || load->op != IR_OP_LOAD || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name || load->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  size = load->rhs.int_value;
  addr = ir_find_temp_producer_before(function, before, load->lhs.name);
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->lhs.kind != IR_OPERAND_SYMBOL ||
      !addr->lhs.name || addr->rhs.kind != IR_OPERAND_TEMP || !addr->rhs.name) {
    return 0;
  }
  shl = ir_find_temp_producer_before(function, before, addr->rhs.name);
  if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
      strcmp(shl->text, "<<") != 0 ||
      !ir_operand_is_symbol_named(&shl->lhs, iv) ||
      shl->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  shift = shl->rhs.int_value;
  if (shift == 3 && size == 8) {
    *bits_out = 64;
  } else if (shift == 2 && size == 4) {
    *bits_out = 32;
  } else {
    return 0;
  }
  *base_out = addr->lhs.name;
  return 1;
}

static int ir_decode_float_indexed_address(IRFunction *function, size_t before,
                                           const char *addr_temp,
                                           const char *iv,
                                           const char **base_out,
                                           int *bits_out) {
  const IRInstruction *addr = NULL;
  const IRInstruction *shl = NULL;
  long long shift = 0;

  if (!addr_temp || !iv || !base_out || !bits_out) {
    return 0;
  }
  addr = ir_find_temp_producer_before(function, before, addr_temp);
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->lhs.kind != IR_OPERAND_SYMBOL ||
      !addr->lhs.name || addr->rhs.kind != IR_OPERAND_TEMP ||
      !addr->rhs.name) {
    return 0;
  }
  shl = ir_find_temp_producer_before(function, before, addr->rhs.name);
  if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
      strcmp(shl->text, "<<") != 0 ||
      !ir_operand_is_symbol_named(&shl->lhs, iv) ||
      shl->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }

  shift = shl->rhs.int_value;
  if (shift == 3) {
    *bits_out = 64;
  } else if (shift == 2) {
    *bits_out = 32;
  } else {
    return 0;
  }
  *base_out = addr->lhs.name;
  return 1;
}

static int ir_decode_byte_indexed_address(IRFunction *function, size_t before,
                                          const char *addr_temp,
                                          const char *iv,
                                          const char **base_out) {
  const IRInstruction *addr = NULL;
  if (!addr_temp || !iv || !base_out) {
    return 0;
  }
  addr = ir_find_temp_producer_before(function, before, addr_temp);
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0) {
    return 0;
  }
  if (ir_operand_is_symbol(&addr->lhs) &&
      ir_operand_is_symbol_named(&addr->rhs, iv)) {
    *base_out = addr->lhs.name;
    return 1;
  }
  if (ir_operand_is_symbol(&addr->rhs) &&
      ir_operand_is_symbol_named(&addr->lhs, iv)) {
    *base_out = addr->rhs.name;
    return 1;
  }
  return 0;
}

static int ir_decode_byte_indexed_load(IRFunction *function, size_t before,
                                       const char *load_temp, const char *iv,
                                       const char **base_out,
                                       int *unsigned_out) {
  const IRInstruction *load = NULL;
  if (!load_temp || !iv || !base_out || !unsigned_out) {
    return 0;
  }
  load = ir_find_temp_producer_before(function, before, load_temp);
  if (!load || load->op != IR_OP_LOAD || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name || load->rhs.kind != IR_OPERAND_INT ||
      load->rhs.int_value != 1) {
    return 0;
  }
  if (!ir_decode_byte_indexed_address(function, before, load->lhs.name, iv,
                                      base_out)) {
    return 0;
  }
  *unsigned_out = load->is_unsigned ? 1 : 0;
  return 1;
}

static int ir_symbol_is_float_array_base(IRFunction *function,
                                         const char *symbol_name) {
  if (ir_function_symbol_is_parameter(function, symbol_name) ||
      ir_function_local_declared_type(function, symbol_name) != NULL) {
    return 1;
  }
  if (!symbol_name || ir_symbol_address_taken(function, symbol_name)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op >= IR_OP_SIMD_SUM_I32 && ins->op <= IR_OP_SIMD_LCG_U32) {
      continue;
    }
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        strcmp(ins->dest.name, symbol_name) == 0) {
      return 0;
    }
  }
  return 1;
}

static int ir_float_sum_type_matches(const char *sum_type, int width_bits) {
  if (!sum_type) {
    return 0;
  }
  if (width_bits == 64) {
    return strcmp(sum_type, "float64") == 0;
  }
  return strcmp(sum_type, "float32") == 0;
}

static const char *ir_function_param_declared_type(const IRFunction *function,
                                                   const char *name) {
  if (!function || !name || !function->parameter_names ||
      !function->parameter_types) {
    return NULL;
  }
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names[i] &&
        strcmp(function->parameter_names[i], name) == 0) {
      return function->parameter_types[i];
    }
  }
  return NULL;
}

static int ir_float_scalar_operand_matches(IRFunction *function,
                                           const IROperand *operand,
                                           int width_bits) {
  if (!operand) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_FLOAT) {
    if (operand->float_bits == width_bits) {
      return 1;
    }
    if (width_bits == 32 &&
        (double)(float)operand->float_value == operand->float_value) {
      return 1;
    }
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    const char *ty = ir_function_local_declared_type(function, operand->name);
    if (!ty) {
      ty = ir_function_param_declared_type(function, operand->name);
    }
    return ir_float_sum_type_matches(ty, width_bits);
  }
  return 0;
}

static int ir_try_clone_float_scalar_operand(IRFunction *function,
                                             size_t before_index,
                                             const IROperand *operand,
                                             int width_bits,
                                             IROperand *out) {
  const IRInstruction *producer = NULL;

  if (!out) {
    return 0;
  }
  *out = ir_operand_none();
  if (ir_float_scalar_operand_matches(function, operand, width_bits)) {
    if (operand->kind == IR_OPERAND_FLOAT) {
      *out = ir_operand_float_sized(operand->float_value, width_bits);
      return 1;
    }
    return ir_operand_clone(operand, out);
  }
  if (!operand || operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 0;
  }

  producer = ir_find_temp_producer_before(function, before_index, operand->name);
  if (!producer || producer->op != IR_OP_CAST || !producer->text ||
      !ir_float_sum_type_matches(producer->text, width_bits)) {
    return 0;
  }
  if (producer->lhs.kind == IR_OPERAND_FLOAT) {
    *out = ir_operand_float_sized(producer->lhs.float_value, width_bits);
    return 1;
  }
  if (producer->lhs.kind == IR_OPERAND_INT) {
    *out = ir_operand_float_sized((double)producer->lhs.int_value, width_bits);
    return 1;
  }
  return 0;
}

static int ir_float_reduction_frame(IRFunction *function, size_t header_index,
                                    const char **iv_out, size_t *branch_out,
                                    size_t *jump_out, IROperand *bound_compare,
                                    int *matched) {
  size_t compare_index = 0;
  size_t branch_index = 0;
  size_t jump_index = (size_t)-1;
  size_t increment_index = 0;
  const char *loop_label = NULL;
  const char *exit_label = NULL;

  *matched = 0;
  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  loop_label = header->text;

  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      (compare->rhs.kind != IR_OPERAND_SYMBOL &&
       compare->rhs.kind != IR_OPERAND_INT) ||
      (compare->rhs.kind == IR_OPERAND_SYMBOL && !compare->rhs.name) ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }
  exit_label = branch->text;

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, exit_label) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
    return 1;
  }
  if (ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }

  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
    if (ir_symbol_address_taken(function, compare->rhs.name)) {
      return 1;
    }
    for (size_t i = branch_index + 1; i < jump_index; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ir_instruction_writes_destination(ins) &&
          ir_operand_is_symbol(&ins->dest) &&
          ir_operand_names_match(&ins->dest, &compare->rhs)) {
        return 1;
      }
    }
  }

  increment_index = jump_index;
  while (increment_index > branch_index + 1) {
    increment_index--;
    if (function->instructions[increment_index].op != IR_OP_NOP) {
      break;
    }
  }
  if (!ir_try_parse_direct_unit_increment(
          &function->instructions[increment_index], compare->lhs.name)) {
    return 1;
  }
  if (!ir_iv_zero_at_header(function, header_index, compare->lhs.name)) {
    return 1;
  }

  if (!ir_operand_clone(&compare->rhs, bound_compare)) {
    return 0;
  }
  *iv_out = compare->lhs.name;
  *branch_out = branch_index;
  *jump_out = jump_index;
  *matched = 1;
  return 1;
}

static int ir_float_body_is_pure_reduction(IRFunction *function, size_t lo,
                                           size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_STORE || op == IR_OP_CALL || op == IR_OP_CALL_INDIRECT ||
        op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ || op == IR_OP_JUMP) {
      return 0;
    }
  }
  return 1;
}

static void ir_install_fused_reduction(IRFunction *function,
                                       size_t header_index, size_t jump_index,
                                       IRInstruction *fused, int *changed) {
  if (!ir_fused_loop_claims_body(function, header_index, jump_index, fused)) {
    ir_instruction_destroy_storage(fused);
    return;
  }
  ir_instruction_destroy_storage(&function->instructions[header_index]);
  function->instructions[header_index] = *fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
}

static int ir_try_vectorize_sum_float_at(IRFunction *function,
                                         size_t header_index, int *changed) {
  const char *iv_symbol = NULL;
  const char *sum_symbol = NULL;
  const char *base_symbol = NULL;
  const char *sum_type = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  IRInstruction fused = {0};
  int matched = 0;
  int width_bits = 0;
  int found = 0;

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_body_is_pure_reduction(function, branch_index + 1,
                                       jump_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ir_operand_is_temp(&ins->rhs)) {
      int bits = 0;
      const char *base = NULL;
      if (!ir_decode_float_indexed_load(function, i, ins->rhs.name, iv_symbol,
                                        &base, &bits)) {
        continue;
      }
      sum_symbol = ins->dest.name;
      base_symbol = base;
      width_bits = bits;
      found = 1;
    }
  }

  if (!found || !sum_symbol || !base_symbol ||
      strcmp(sum_symbol, iv_symbol) == 0) {
    ir_operand_destroy(&bound);
    return 1;
  }
  sum_type = ir_function_local_declared_type(function, sum_symbol);
  {
    double bound_lo = 0.0;
    double bound_hi = 0.0;
    if (ir_lookup_float_bound(sum_type, &bound_lo, &bound_hi)) {
      if (ir_explain_enabled()) {
        char reason[256];
        snprintf(reason, sizeof(reason),
                 "the accumulator's declared type pins it to %g..%g, and "
                 "reassociating the sum into lanes moves the answer by a "
                 "rounding this loop's trip count does not bound; the "
                 "declared bound does not survive the rewrite",
                 bound_lo, bound_hi);
        ir_explain_remark(function->name, "loop body",
                          function->instructions[header_index].location, 0,
                          "NOT vectorized", reason, NULL, NULL);
        ir_explain_remark_code("float-bound-declared");
      }
      ir_operand_destroy(&bound);
      return 1;
    }
    ir_explain_belief(
        "floating-point reassociation",
        "a float sum was vectorized into lanes, which is a different order of "
        "addition from the one written; no accumulator in this build carried a "
        "declared bound to check the difference against");
  }
  if (!ir_float_sum_type_matches(sum_type, width_bits) ||
      !ir_symbol_is_float_array_base(function, base_symbol) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = (width_bits == 64) ? IR_OP_SIMD_SUM_F64 : IR_OP_SIMD_SUM_F32;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = width_bits;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(base_symbol);
  fused.rhs = bound;
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

int ir_simd_sum_float_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_sum_float_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

static int ir_try_vectorize_dot_float_at(IRFunction *function,
                                         size_t header_index, int *changed) {
  const char *iv_symbol = NULL;
  const char *sum_symbol = NULL;
  const char *a_symbol = NULL;
  const char *b_symbol = NULL;
  const char *sum_type = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  IRInstruction fused = {0};
  int matched = 0;
  int width_bits = 0;
  int found = 0;

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_body_is_pure_reduction(function, branch_index + 1,
                                       jump_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IRInstruction *mul = NULL;
    int bits_a = 0;
    int bits_b = 0;
    const char *base_a = NULL;
    const char *base_b = NULL;
    if (!(ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name &&
          ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
          ir_operand_is_temp(&ins->rhs))) {
      continue;
    }
    mul = ir_find_temp_producer_before(function, i, ins->rhs.name);
    if (!mul || mul->op != IR_OP_BINARY || !mul->is_float || !mul->text ||
        strcmp(mul->text, "*") != 0 || mul->lhs.kind != IR_OPERAND_TEMP ||
        !mul->lhs.name || mul->rhs.kind != IR_OPERAND_TEMP || !mul->rhs.name) {
      continue;
    }
    if (!ir_decode_float_indexed_load(function, i, mul->lhs.name, iv_symbol,
                                      &base_a, &bits_a) ||
        !ir_decode_float_indexed_load(function, i, mul->rhs.name, iv_symbol,
                                      &base_b, &bits_b) ||
        bits_a != bits_b) {
      continue;
    }
    sum_symbol = ins->dest.name;
    a_symbol = base_a;
    b_symbol = base_b;
    width_bits = bits_a;
    found = 1;
  }

  if (!found || !sum_symbol || !a_symbol || !b_symbol ||
      strcmp(sum_symbol, iv_symbol) == 0) {
    ir_operand_destroy(&bound);
    return 1;
  }
  sum_type = ir_function_local_declared_type(function, sum_symbol);
  if (!ir_float_sum_type_matches(sum_type, width_bits) ||
      !ir_symbol_is_float_array_base(function, a_symbol) ||
      !ir_symbol_is_float_array_base(function, b_symbol) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = (width_bits == 64) ? IR_OP_SIMD_DOT_F64 : IR_OP_SIMD_DOT_F32;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = width_bits;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(a_symbol);
  fused.rhs = ir_operand_symbol(b_symbol);
  fused.arguments = calloc(1, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy(&bound);
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = 1;
  fused.arguments[0] = bound;
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

#define IR_MULTIDOT_MAX 8

typedef struct {
  const char *acc;
  const char *base_a;
  const char *base_b;
  size_t update_index;
} IRMultidotTerm;

static int ir_multidot_load_base(IRFunction *function, size_t before, size_t lo,
                                 size_t hi, const IROperand *op, const char *iv,
                                 const char **base) {
  int bits = 0;
  if (op->kind == IR_OPERAND_TEMP && op->name) {
    return ir_decode_float_indexed_load(function, before, op->name, iv, base,
                                        &bits) &&
           bits == 32;
  }
  if (op->kind != IR_OPERAND_SYMBOL || !op->name) {
    return 0;
  }
  const IRInstruction *def = NULL;
  size_t def_index = 0;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, op->name)) {
      if (def) {
        return 0;
      }
      def = ins;
      def_index = i;
    }
  }
  if (!def || def_index >= before ||
      ir_symbol_live_after_loop(function, hi + 1, op->name)) {
    return 0;
  }
  if (def->op == IR_OP_LOAD && def->is_float && ir_operand_is_temp(&def->lhs) &&
      def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value == 4) {
    return ir_decode_float_indexed_address(function, def_index, def->lhs.name,
                                           iv, base, &bits) &&
           bits == 32;
  }
  if (def->op == IR_OP_ASSIGN && ir_operand_is_temp(&def->lhs)) {
    return ir_decode_float_indexed_load(function, def_index, def->lhs.name, iv,
                                        base, &bits) &&
           bits == 32;
  }
  return 0;
}

static int ir_multidot_names(const IRMultidotTerm *terms, int count,
                             const char *name) {
  for (int i = 0; i < count; i++) {
    if (strcmp(terms[i].acc, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_multidot_is_base(const IRMultidotTerm *terms, int count,
                               const char *name) {
  for (int i = 0; i < count; i++) {
    if (strcmp(terms[i].base_a, name) == 0 ||
        strcmp(terms[i].base_b, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_multidot_reads_acc(const IRInstruction *ins,
                                 const IRMultidotTerm *terms, int count) {
  if (ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
      ir_multidot_names(terms, count, ins->lhs.name)) {
    return 1;
  }
  if (ins->rhs.kind == IR_OPERAND_SYMBOL && ins->rhs.name &&
      ir_multidot_names(terms, count, ins->rhs.name)) {
    return 1;
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    const IROperand *op = &ins->arguments[a];
    if (op->kind == IR_OPERAND_SYMBOL && op->name &&
        ir_multidot_names(terms, count, op->name)) {
      return 1;
    }
  }
  return 0;
}

static int ir_multidot_body_is_claimed(IRFunction *function, size_t lo,
                                       size_t hi, size_t increment,
                                       const char *iv,
                                       const IRMultidotTerm *terms, int k) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    int is_update = 0;
    for (int t = 0; t < k; t++) {
      if (terms[t].update_index == i) {
        is_update = 1;
      }
    }
    if (is_update || i == increment || ins->op == IR_OP_NOP ||
        ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->op != IR_OP_BINARY && ins->op != IR_OP_LOAD &&
        ins->op != IR_OP_ASSIGN && ins->op != IR_OP_CAST) {
      return 0;
    }
    if (ir_multidot_reads_acc(ins, terms, k)) {
      return 0;
    }
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind == IR_OPERAND_TEMP) {
      continue;
    }
    if (ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name ||
        strcmp(ins->dest.name, iv) == 0 ||
        ir_multidot_names(terms, k, ins->dest.name) ||
        ir_multidot_is_base(terms, k, ins->dest.name) ||
        ir_symbol_live_after_loop(function, hi + 1, ins->dest.name)) {
      return 0;
    }
  }
  return 1;
}

static int ir_multidot_collect_terms(IRFunction *function, size_t lo,
                                     size_t hi, const char *iv_symbol,
                                     IRMultidotTerm *terms) {
  int k = 0;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!(ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name &&
          ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
          ir_operand_is_temp(&ins->rhs))) {
      continue;
    }
    const IRInstruction *mul =
        ir_find_temp_producer_before(function, i, ins->rhs.name);
    if (!mul || mul < &function->instructions[lo] || mul->op != IR_OP_BINARY ||
        !mul->is_float || !mul->text || strcmp(mul->text, "*") != 0) {
      continue;
    }
    const char *base_a = NULL;
    const char *base_b = NULL;
    size_t mul_index = (size_t)(mul - function->instructions);
    if (!ir_multidot_load_base(function, mul_index, lo, hi, &mul->lhs,
                               iv_symbol, &base_a) ||
        !ir_multidot_load_base(function, mul_index, lo, hi, &mul->rhs,
                               iv_symbol, &base_b)) {
      continue;
    }
    if (k == IR_MULTIDOT_MAX) {
      return -1;
    }
    terms[k].acc = ins->dest.name;
    terms[k].base_a = base_a;
    terms[k].base_b = base_b;
    terms[k].update_index = i;
    k++;
  }
  return k;
}

static int ir_multidot_terms_valid(IRFunction *function,
                                   const IRMultidotTerm *terms, int k,
                                   const char *iv_symbol) {
  for (int t = 0; t < k; t++) {
    const char *acc_type = ir_function_local_declared_type(function, terms[t].acc);
    if (!acc_type || strcmp(acc_type, "float32") != 0 ||
        strcmp(terms[t].acc, iv_symbol) == 0 ||
        ir_symbol_address_taken(function, terms[t].acc) ||
        ir_multidot_names(terms, t, terms[t].acc) ||
        ir_multidot_is_base(terms, k, terms[t].acc) ||
        !ir_symbol_is_float_array_base(function, terms[t].base_a) ||
        !ir_symbol_is_float_array_base(function, terms[t].base_b)) {
      return 0;
    }
  }
  return 1;
}

static int ir_multidot_replace_loop(IRFunction *function, size_t header_index,
                                    size_t jump_index,
                                    const IRMultidotTerm *terms, int k,
                                    const IROperand *bound) {
  IRInstruction fused[IR_MULTIDOT_MAX];
  memset(fused, 0, sizeof(fused));
  for (int t = 0; t < k; t++) {
    fused[t].op = IR_OP_SIMD_DOT_F32;
    fused[t].location = function->instructions[header_index].location;
    fused[t].is_float = 1;
    fused[t].float_bits = 32;
    fused[t].dest = ir_operand_symbol(terms[t].acc);
    fused[t].lhs = ir_operand_symbol(terms[t].base_a);
    fused[t].rhs = ir_operand_symbol(terms[t].base_b);
    fused[t].arguments = calloc(1, sizeof(IROperand));
    if (!fused[t].arguments || !ir_operand_clone(bound, &fused[t].arguments[0])) {
      for (int u = 0; u <= t; u++) {
        ir_instruction_destroy_storage(&fused[u]);
      }
      return 0;
    }
    fused[t].argument_count = 1;
  }
  for (int t = 0; t < k; t++) {
    ir_instruction_destroy_storage(&function->instructions[header_index + t]);
    function->instructions[header_index + t] = fused[t];
  }
  for (size_t i = header_index + (size_t)k; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  return 1;
}

static int ir_try_split_multidot_float_at(IRFunction *function,
                                          size_t header_index, int *changed,
                                          int *taken) {
  const char *iv_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  int matched = 0;
  IRMultidotTerm terms[IR_MULTIDOT_MAX];

  *taken = 0;
  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  size_t lo = branch_index + 1;
  size_t hi = jump_index;
  size_t increment = hi;
  while (increment > lo) {
    increment--;
    if (function->instructions[increment].op != IR_OP_NOP) {
      break;
    }
  }
  int k = ir_multidot_collect_terms(function, lo, hi, iv_symbol, terms);
  if (k < 2 || !ir_multidot_terms_valid(function, terms, k, iv_symbol) ||
      !ir_multidot_body_is_claimed(function, lo, hi, increment, iv_symbol,
                                   terms, k) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  int replaced = ir_multidot_replace_loop(function, header_index, jump_index,
                                          terms, k, &bound);
  ir_operand_destroy(&bound);
  if (!replaced) {
    return 0;
  }
  *taken = 1;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_dot_float_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      int taken = 0;
      if (!ir_try_split_multidot_float_at(function, i, changed, &taken)) {
        return 0;
      }
      if (!taken && !ir_try_vectorize_dot_float_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

static IROperand ir_float_const_operand(double value, int width_bits) {
  return ir_operand_float_sized(value, width_bits == 32 ? 32 : 64);
}

static void ir_affine_map_terms_destroy(IRAffineMapTerms *terms) {
  if (!terms) {
    return;
  }
  if (terms->has_src_scale) {
    ir_operand_destroy(&terms->src_scale);
  }
  if (terms->has_dst_scale) {
    ir_operand_destroy(&terms->dst_scale);
  }
  if (terms->has_bias) {
    ir_operand_destroy(&terms->bias);
  }
  memset(terms, 0, sizeof(*terms));
}

typedef struct VLoopDiamond {
  size_t branch_index;
  size_t then_lo, then_hi;
  size_t else_lo, else_hi;
  size_t end;
  const char *sym;
  int has_else;
} VLoopDiamond;

static int vloop_match_diamond(const IRFunction *function, size_t at,
                               size_t body_hi, VLoopDiamond *out);
static int vloop_region_is_pure(const IRFunction *function, size_t lo, size_t hi,
                                int depth);
static int vloop_region_writes_escape(IRFunction *function, size_t lo, size_t hi,
                                      size_t after);

static int ir_float_map_body_is_safe_ex(IRFunction *function, size_t lo,
                                        size_t hi, const char *iv_symbol,
                                        size_t *store_index_out,
                                        int allow_diamonds) {
  size_t store_count = 0;

  if (!function || !store_index_out) {
    return 0;
  }
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_STORE) {
      store_count++;
      *store_index_out = i;
      continue;
    }
    if (allow_diamonds) {
      VLoopDiamond dm;
      if (ins->op == IR_OP_BRANCH_ZERO &&
          vloop_match_diamond(function, i, hi, &dm)) {
        if (!vloop_region_is_pure(function, dm.then_lo, dm.then_hi, 1) ||
            !vloop_region_is_pure(function, dm.else_lo, dm.else_hi, 1) ||
            vloop_region_writes_escape(function, dm.then_lo, dm.else_hi,
                                       hi + 1)) {
          return 0;
        }
        i = dm.end - 1;
        continue;
      }
      if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP) {
        continue;
      }
    }
    if (ir_instruction_writes_symbol(ins) &&
        !ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      if (ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name ||
          ir_symbol_live_after_loop(function, hi + 1, ins->dest.name)) {
        return 0;
      }
    }
    if (ins->op == IR_OP_CALL || ins->op == IR_OP_CALL_INDIRECT ||
        ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_INLINE_ASM ||
        ins->op == IR_OP_MEMCPY_INLINE || ins->op == IR_OP_COUNT_WORD_STARTS) {
      return 0;
    }
  }

  return store_count == 1;
}

static int ir_float_map_body_is_safe(IRFunction *function, size_t lo, size_t hi,
                                     const char *iv_symbol,
                                     size_t *store_index_out) {
  return ir_float_map_body_is_safe_ex(function, lo, hi, iv_symbol,
                                      store_index_out, 0);
}

static int ir_affine_map_add_bias(IRAffineMapTerms *terms,
                                  const IROperand *bias) {
  if (!terms || !bias || terms->has_bias) {
    return 0;
  }
  if (!ir_operand_clone(bias, &terms->bias)) {
    return 0;
  }
  terms->has_bias = 1;
  return 1;
}

static int ir_affine_map_add_indexed_term(IRAffineMapTerms *terms,
                                          const char *base,
                                          const IROperand *scale) {
  if (!terms || !base || !scale) {
    return 0;
  }
  if (strcmp(base, terms->dst_base) == 0) {
    if (terms->has_dst_scale) {
      return 0;
    }
    if (!ir_operand_clone(scale, &terms->dst_scale)) {
      return 0;
    }
    terms->has_dst_scale = 1;
    return 1;
  }

  if (terms->src_base && strcmp(base, terms->src_base) != 0) {
    return 0;
  }
  terms->src_base = base;
  if (terms->has_src_scale) {
    return 0;
  }
  if (!ir_operand_clone(scale, &terms->src_scale)) {
    return 0;
  }
  terms->has_src_scale = 1;
  return 1;
}

static int ir_try_parse_affine_map_term(IRFunction *function, size_t before,
                                        const IROperand *operand,
                                        const char *iv_symbol,
                                        IRAffineMapTerms *terms) {
  const IRInstruction *producer = NULL;
  const char *base = NULL;
  IROperand scalar = {0};
  int bits = 0;

  if (!function || !operand || !iv_symbol || !terms) {
    return 0;
  }

  if (operand->kind == IR_OPERAND_TEMP && operand->name &&
      ir_decode_float_indexed_load(function, before, operand->name, iv_symbol,
                                   &base, &bits) &&
      bits == terms->width_bits) {
    IROperand one = ir_float_const_operand(1.0, terms->width_bits);
    int ok = ir_affine_map_add_indexed_term(terms, base, &one);
    ir_operand_destroy(&one);
    return ok;
  }

  if (ir_try_clone_float_scalar_operand(function, before, operand,
                                        terms->width_bits, &scalar)) {
    int ok = ir_affine_map_add_bias(terms, &scalar);
    ir_operand_destroy(&scalar);
    return ok;
  }

  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 0;
  }

  producer = ir_find_temp_producer_before(function, before, operand->name);
  if (!producer || producer->op != IR_OP_BINARY || !producer->is_float ||
      !producer->text || strcmp(producer->text, "*") != 0) {
    return 0;
  }

  if (ir_operand_is_temp(&producer->lhs) &&
      ir_decode_float_indexed_load(function, before, producer->lhs.name,
                                   iv_symbol, &base, &bits) &&
      bits == terms->width_bits &&
      ir_try_clone_float_scalar_operand(function, before, &producer->rhs,
                                        terms->width_bits, &scalar)) {
    int ok = ir_affine_map_add_indexed_term(terms, base, &scalar);
    ir_operand_destroy(&scalar);
    return ok;
  }
  if (ir_operand_is_temp(&producer->rhs) &&
      ir_decode_float_indexed_load(function, before, producer->rhs.name,
                                   iv_symbol, &base, &bits) &&
      bits == terms->width_bits &&
      ir_try_clone_float_scalar_operand(function, before, &producer->lhs,
                                        terms->width_bits, &scalar)) {
    int ok = ir_affine_map_add_indexed_term(terms, base, &scalar);
    ir_operand_destroy(&scalar);
    return ok;
  }

  return 0;
}

static int ir_try_parse_affine_map_expr(IRFunction *function, size_t before,
                                        const IROperand *operand,
                                        const char *iv_symbol,
                                        IRAffineMapTerms *terms) {
  const IRInstruction *producer = NULL;

  if (!operand) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    producer = ir_find_temp_producer_before(function, before, operand->name);
    if (producer && producer->op == IR_OP_BINARY && producer->is_float &&
        producer->text && strcmp(producer->text, "+") == 0) {
      return ir_try_parse_affine_map_expr(function, before, &producer->lhs,
                                          iv_symbol, terms) &&
             ir_try_parse_affine_map_expr(function, before, &producer->rhs,
                                          iv_symbol, terms);
    }
  }

  return ir_try_parse_affine_map_term(function, before, operand, iv_symbol,
                                      terms);
}

static int ir_affine_map_terms_finalize(IRAffineMapTerms *terms) {
  if (!terms || !terms->dst_base) {
    return 0;
  }

  if (!terms->src_base) {
    terms->src_base = terms->dst_base;
    terms->src_scale = ir_float_const_operand(0.0, terms->width_bits);
    terms->has_src_scale = 1;
  }
  if (!terms->has_dst_scale) {
    terms->dst_scale = ir_float_const_operand(0.0, terms->width_bits);
    terms->has_dst_scale = 1;
  }
  if (!terms->has_bias) {
    terms->bias = ir_float_const_operand(0.0, terms->width_bits);
    terms->has_bias = 1;
  }
  return terms->has_src_scale && terms->has_dst_scale && terms->has_bias;
}

static int ir_try_vectorize_affine_map_float_at(IRFunction *function,
                                                size_t header_index,
                                                int *changed) {
  const char *iv_symbol = NULL;
  const char *dst_base = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t store_index = 0;
  IROperand bound = {0};
  IRAffineMapTerms terms = {0};
  IRInstruction fused = {0};
  int matched = 0;
  int store_bits = 0;
  const IRInstruction *store = NULL;

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_map_body_is_safe(function, branch_index + 1, jump_index,
                                 iv_symbol, &store_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  store = &function->instructions[store_index];
  if (!store->is_float || store->dest.kind != IR_OPERAND_TEMP ||
      !store->dest.name ||
      store->lhs.kind != IR_OPERAND_TEMP || !store->lhs.name ||
      store->rhs.kind != IR_OPERAND_INT ||
      (store->rhs.int_value != 4 && store->rhs.int_value != 8) ||
      !ir_decode_float_indexed_address(function, store_index, store->dest.name,
                                       iv_symbol, &dst_base, &store_bits) ||
      store_bits != store->rhs.int_value * 8) {
    ir_operand_destroy(&bound);
    return 1;
  }

  terms.dst_base = dst_base;
  terms.width_bits = store_bits;
  if (!ir_try_parse_affine_map_expr(function, store_index, &store->lhs,
                                    iv_symbol, &terms) ||
      !ir_affine_map_terms_finalize(&terms)) {
    ir_operand_destroy(&bound);
    ir_affine_map_terms_destroy(&terms);
    return 1;
  }

  if (!ir_symbol_is_float_array_base(function, terms.src_base) ||
      !ir_symbol_is_float_array_base(function, dst_base) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    ir_affine_map_terms_destroy(&terms);
    return 1;
  }

  fused.op = (store_bits == 64) ? IR_OP_SIMD_AFFINE_MAP_F64
                                : IR_OP_SIMD_AFFINE_MAP_F32;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = store_bits;
  fused.dest = ir_operand_symbol(dst_base);
  fused.lhs = ir_operand_symbol(terms.src_base);
  fused.rhs = ir_operand_symbol(dst_base);
  fused.arguments = calloc(4, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy(&bound);
    ir_affine_map_terms_destroy(&terms);
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = 4;
  fused.arguments[0] = bound;
  fused.arguments[1] = terms.src_scale;
  fused.arguments[2] = terms.dst_scale;
  fused.arguments[3] = terms.bias;
  terms.has_src_scale = 0;
  terms.has_dst_scale = 0;
  terms.has_bias = 0;
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  ir_affine_map_terms_destroy(&terms);
  return 1;
}

int ir_simd_affine_map_float_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_affine_map_float_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

#define I2F_STEP_MUL 0
#define I2F_STEP_ADD 1
#define I2F_STEP_SUBR 2
#define I2F_STEP_SUBL 3
#define I2F_STEP_DIVR 4
#define I2F_MAX_STEPS 8

typedef struct {
  int op_code;
  double k;
} I2fChainStep;

static const IRInstruction *ir_i2f_resolve_producer(IRFunction *function,
                                                    size_t before,
                                                    const IROperand *op) {
  if (!op || !op->name) {
    return NULL;
  }
  if (op->kind == IR_OPERAND_TEMP) {
    return ir_find_temp_producer_before(function, before, op->name);
  }
  if (op->kind == IR_OPERAND_SYMBOL) {
    size_t wi = 0;
    if (ir_find_last_writer_before(function, before, IR_OPERAND_SYMBOL, op->name,
                                   &wi)) {
      return &function->instructions[wi];
    }
  }
  return NULL;
}

static int ir_i2f_operand_is_f64_const(const IROperand *op, double *value_out) {
  if (!op || op->kind != IR_OPERAND_FLOAT || op->float_bits != 64) {
    return 0;
  }
  *value_out = op->float_value;
  return 1;
}

static int ir_i2f_extract_chain(IRFunction *function, size_t before,
                                const IROperand *op, const char *iv,
                                I2fChainStep *steps, int *nsteps) {
  const IRInstruction *p = ir_i2f_resolve_producer(function, before, op);
  if (!p) {
    return 0;
  }
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      strcmp(p->text, "float64") == 0 &&
      ir_operand_is_symbol_named(&p->lhs, iv)) {
    return 1;
  }
  if (p->op != IR_OP_BINARY || !p->is_float || !p->text) {
    return 0;
  }

  double k = 0.0;
  int l_const = ir_i2f_operand_is_f64_const(&p->lhs, &k);
  double kr = 0.0;
  int r_const = ir_i2f_operand_is_f64_const(&p->rhs, &kr);
  const IROperand *inner = NULL;
  int code = -1;

  if (r_const && !l_const) {
    inner = &p->lhs;
    k = kr;
    if (strcmp(p->text, "+") == 0) {
      code = I2F_STEP_ADD;
    } else if (strcmp(p->text, "-") == 0) {
      code = I2F_STEP_SUBR;
    } else if (strcmp(p->text, "*") == 0) {
      code = I2F_STEP_MUL;
    } else if (strcmp(p->text, "/") == 0) {
      code = I2F_STEP_DIVR;
    } else {
      return 0;
    }
  } else if (l_const && !r_const) {
    inner = &p->rhs;
    if (strcmp(p->text, "+") == 0) {
      code = I2F_STEP_ADD;
    } else if (strcmp(p->text, "*") == 0) {
      code = I2F_STEP_MUL;
    } else if (strcmp(p->text, "-") == 0) {
      code = I2F_STEP_SUBL;
    } else {
      return 0;
    }
  } else {
    return 0;
  }

  size_t pidx = (size_t)(p - function->instructions);
  if (!ir_i2f_extract_chain(function, pidx, inner, iv, steps, nsteps)) {
    return 0;
  }
  if (*nsteps >= I2F_MAX_STEPS) {
    return 0;
  }
  steps[*nsteps].op_code = code;
  steps[*nsteps].k = k;
  (*nsteps)++;
  return 1;
}

static double ir_i2f_eval_chain(const I2fChainStep *steps, int nsteps,
                                double i) {
  double x = i;
  for (int s = 0; s < nsteps; s++) {
    double k = steps[s].k;
    switch (steps[s].op_code) {
    case I2F_STEP_MUL: x = x * k; break;
    case I2F_STEP_ADD: x = x + k; break;
    case I2F_STEP_SUBR: x = x - k; break;
    case I2F_STEP_SUBL: x = k - x; break;
    case I2F_STEP_DIVR: x = x / k; break;
    default: break;
    }
  }
  return x;
}

static int ir_i2f_resolve_const_bound(IRFunction *function, size_t before,
                                      const IROperand *rhs, long long *out) {
  if (!rhs) {
    return 0;
  }
  if (rhs->kind == IR_OPERAND_INT) {
    *out = rhs->int_value;
    return 1;
  }
  if (rhs->kind == IR_OPERAND_TEMP && rhs->name) {
    const IRInstruction *p =
        ir_find_temp_producer_before(function, before, rhs->name);
    if (p && p->op == IR_OP_CAST && p->lhs.kind == IR_OPERAND_INT) {
      *out = p->lhs.int_value;
      return 1;
    }
  }
  return 0;
}

static int ir_i2f_body_is_safe(IRFunction *function, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    switch (function->instructions[i].op) {
    case IR_OP_STORE:
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_JUMP:
    case IR_OP_LABEL:
    case IR_OP_INLINE_ASM:
    case IR_OP_MEMCPY_INLINE:
    case IR_OP_NEW:
    case IR_OP_ADDRESS_OF:
    case IR_OP_RETURN:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

static int ir_try_vectorize_i2f_reduce_at(IRFunction *function,
                                          size_t header_index, int *changed) {
  size_t compare_index = 0;
  size_t branch_index = (size_t)-1;
  size_t jump_index = (size_t)-1;
  size_t increment_index = 0;
  const char *iv_symbol = NULL;
  const char *acc_symbol = NULL;
  const char *loop_label = NULL;
  const char *exit_label = NULL;
  long long bound = 0;
  I2fChainStep steps[I2F_MAX_STEPS];
  int nsteps = 0;
  int found = 0;
  IRInstruction fused = {0};

  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  loop_label = header->text;

  for (size_t i = header_index + 1; i < function->instruction_count; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_BRANCH_ZERO) {
      branch_index = i;
      break;
    }
    if (op == IR_OP_JUMP || op == IR_OP_LABEL || op == IR_OP_BRANCH_EQ) {
      break;
    }
  }
  if (branch_index == (size_t)-1) {
    return 1;
  }
  const IRInstruction *branch = &function->instructions[branch_index];
  if (!branch->text || branch->lhs.kind != IR_OPERAND_TEMP || !branch->lhs.name) {
    return 1;
  }
  const IRInstruction *compare =
      ir_find_temp_producer_before(function, branch_index, branch->lhs.name);
  if (!compare || compare->op != IR_OP_BINARY || compare->is_float ||
      !compare->text || strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name) {
    return 1;
  }
  compare_index = (size_t)(compare - function->instructions);
  iv_symbol = compare->lhs.name;
  exit_label = branch->text;

  if (!ir_i2f_resolve_const_bound(function, compare_index, &compare->rhs,
                                  &bound) ||
      bound < 1) {
    return 1;
  }

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, exit_label) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
    return 1;
  }
  if (ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (!ir_i2f_body_is_safe(function, branch_index + 1, jump_index)) {
    return 1;
  }

  increment_index = jump_index;
  while (increment_index > branch_index + 1) {
    increment_index--;
    if (function->instructions[increment_index].op != IR_OP_NOP) {
      break;
    }
  }
  if (!ir_try_parse_direct_unit_increment(
          &function->instructions[increment_index], iv_symbol)) {
    return 1;
  }
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IRInstruction *cast = NULL;
    const char *t = NULL;
    int local_nsteps = 0;
    if (!(ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name &&
          ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
          ir_operand_is_temp(&ins->rhs))) {
      continue;
    }
    t = ins->dest.name;
    if (strcmp(t, iv_symbol) == 0) {
      continue;
    }
    cast = ir_find_temp_producer_before(function, i, ins->rhs.name);
    if (!cast || cast->op != IR_OP_CAST || !cast->is_float || !cast->text ||
        strcmp(cast->text, "int64") != 0) {
      continue;
    }
    if (!ir_i2f_extract_chain(function, i, &cast->lhs, iv_symbol, steps,
                              &local_nsteps) ||
        local_nsteps < 1) {
      continue;
    }
    acc_symbol = ins->dest.name;
    nsteps = local_nsteps;
    found = 1;
  }

  if (!found || !acc_symbol) {
    return 1;
  }
  {
    const char *acc_type = ir_function_local_declared_type(function, acc_symbol);
    if (!acc_type || strcmp(acc_type, "int64") != 0) {
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    return 1;
  }

  {
    double v0 = ir_i2f_eval_chain(steps, nsteps, 0.0);
    double vN = ir_i2f_eval_chain(steps, nsteps, (double)(bound - 1));
    double vmax = v0 > vN ? v0 : vN;
    double vmin = v0 < vN ? v0 : vN;
    double abs_max = vmax > -vmin ? vmax : -vmin;
    if (!(vmin == vmin) || !(vmax == vmax)) {
      return 1;
    }
    if (abs_max >= 2147483647.0) {
      return 1;
    }
    if (abs_max * (double)bound >= 4503599627370496.0 ) {
      return 1;
    }
  }

  fused.op = IR_OP_SIMD_I2F_REDUCE_F64;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 0;
  fused.dest = ir_operand_symbol(acc_symbol);
  fused.argument_count = (size_t)(1 + 2 * nsteps);
  fused.arguments = calloc(fused.argument_count, sizeof(IROperand));
  if (!fused.arguments) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.arguments[0] = ir_operand_int(bound);
  for (int s = 0; s < nsteps; s++) {
    fused.arguments[1 + 2 * s] = ir_operand_int(steps[s].op_code);
    fused.arguments[2 + 2 * s] = ir_operand_float_sized(steps[s].k, 64);
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

int ir_simd_i2f_reduce_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_i2f_reduce_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

#define VLOOP_VN_LOAD 0
#define VLOOP_VN_IOTA 1
#define VLOOP_VN_CONST 2
#define VLOOP_VN_ADD 3
#define VLOOP_VN_SUB 4
#define VLOOP_VN_MUL 5
#define VLOOP_VN_DIV 6
#define VLOOP_VN_SCALAR 7
#define VLOOP_VN_AND 8
#define VLOOP_VN_OR 9
#define VLOOP_VN_XOR 10
#define VLOOP_VN_SHL 11
#define VLOOP_VN_SAR 12
#define VLOOP_VN_SHR 13
#define VLOOP_VN_MIN 14
#define VLOOP_VN_MAX 15
#define VLOOP_VN_CMPGT 16
#define VLOOP_VN_PAIR 17
#define VLOOP_VN_SELECT 18
#define VLOOP_VN_CMPEQ 19

#define VLOOP_MAX_DIAMOND_DEPTH 4

#define VLOOP_MAX_NODES 48
#define VLOOP_MAX_ARRAYS 4
#define VLOOP_MAX_CONSTS 16
#define VLOOP_MAX_SCALARS 8
#define VLOOP_REG_BUDGET 4
int code_generator_vloop_pool_size(int elem8, int has_iota);

typedef struct {
  int tag;
  int op0;
  int op1;
} VLoopNode;

typedef struct {
  VLoopNode nodes[VLOOP_MAX_NODES];
  int n_nodes;
  const char *arrays[VLOOP_MAX_ARRAYS];
  int n_arrays;
  double consts[VLOOP_MAX_CONSTS];
  long long iconsts[VLOOP_MAX_CONSTS];
  int n_consts;
  const char *scalars[VLOOP_MAX_SCALARS];
  int n_scalars;
  int width_bits;
  int is_int;
  int elem_bits;
  int elem_unsigned;
  size_t body_lo;
  size_t body_hi;
  int has_iota;
  int overflow;
  int resolve_depth;
  int iota_bound_known;
  long long iota_bound;
  struct {
    size_t lo;
    size_t hi;
    size_t incoming_hi;
  } regions[VLOOP_MAX_DIAMOND_DEPTH + 1];
  int n_regions;
} VLoopDag;

#define VLOOP_MAX_RESOLVE_DEPTH 16

static int vloop_map_budget(const VLoopDag *d) {
  return code_generator_vloop_pool_size(d->elem_bits == 8, d->has_iota);
}

static int vloop_tag_is_leaf(int tag) {
  return tag == VLOOP_VN_LOAD || tag == VLOOP_VN_IOTA ||
         tag == VLOOP_VN_CONST || tag == VLOOP_VN_SCALAR;
}

static int vloop_intern_array(VLoopDag *d, const char *base) {
  for (int i = 0; i < d->n_arrays; i++) {
    if (strcmp(d->arrays[i], base) == 0) {
      return i;
    }
  }
  if (d->n_arrays >= VLOOP_MAX_ARRAYS) {
    d->overflow = 1;
    return -1;
  }
  d->arrays[d->n_arrays] = base;
  return d->n_arrays++;
}

static int vloop_intern_const(VLoopDag *d, double v) {
  for (int i = 0; i < d->n_consts; i++) {
    if (memcmp(&d->consts[i], &v, sizeof(double)) == 0) {
      return i;
    }
  }
  if (d->n_consts >= VLOOP_MAX_CONSTS) {
    d->overflow = 1;
    return -1;
  }
  d->consts[d->n_consts] = v;
  return d->n_consts++;
}

static int vloop_intern_iconst(VLoopDag *d, long long v) {
  for (int i = 0; i < d->n_consts; i++) {
    if (d->iconsts[i] == v) {
      return i;
    }
  }
  if (d->n_consts >= VLOOP_MAX_CONSTS) {
    d->overflow = 1;
    return -1;
  }
  d->iconsts[d->n_consts] = v;
  return d->n_consts++;
}

static int vloop_intern_scalar(VLoopDag *d, const char *name) {
  for (int i = 0; i < d->n_scalars; i++) {
    if (strcmp(d->scalars[i], name) == 0) {
      return i;
    }
  }
  if (d->n_scalars >= VLOOP_MAX_SCALARS) {
    d->overflow = 1;
    return -1;
  }
  d->scalars[d->n_scalars] = name;
  return d->n_scalars++;
}

static int vloop_symbol_written_in_body(const IRFunction *function,
                                        const VLoopDag *d, const char *name) {
  for (size_t i = d->body_lo; i < d->body_hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        strcmp(ins->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int vloop_add_node(VLoopDag *d, int tag, int op0, int op1) {
  if (d->n_nodes >= VLOOP_MAX_NODES) {
    d->overflow = 1;
    return -1;
  }
  d->nodes[d->n_nodes].tag = tag;
  d->nodes[d->n_nodes].op0 = op0;
  d->nodes[d->n_nodes].op1 = op1;
  return d->n_nodes++;
}

static int vloop_text_is_float_width(const char *text, int width_bits) {
  return (width_bits == 64 && strcmp(text, "float64") == 0) ||
         (width_bits == 32 && strcmp(text, "float32") == 0);
}

static int vloop_f64_narrows_exactly(double v) {
  return (double)(float)v == v;
}

static int vloop_operand_is_literal(IRFunction *function, size_t before,
                                    const IROperand *op, int width_bits,
                                    double *out) {
  if (op->kind == IR_OPERAND_FLOAT) {
    if (op->float_bits == width_bits) {
      *out = op->float_value;
      return 1;
    }
    if (width_bits == 32 && op->float_bits == 64 &&
        vloop_f64_narrows_exactly(op->float_value)) {
      *out = op->float_value;
      return 1;
    }
  }
  if (op->kind == IR_OPERAND_TEMP && op->name) {
    const IRInstruction *p =
        ir_find_temp_producer_before(function, before, op->name);
    if (p && p->op == IR_OP_CAST && p->text &&
        vloop_text_is_float_width(p->text, width_bits)) {
      if (p->lhs.kind == IR_OPERAND_FLOAT) {
        *out = p->lhs.float_value;
        return 1;
      }
      if (p->lhs.kind == IR_OPERAND_INT) {
        *out = (double)p->lhs.int_value;
        return 1;
      }
    }
  }
  return 0;
}

static int vloop_binop_tag(const char *text) {
  if (strcmp(text, "+") == 0) return VLOOP_VN_ADD;
  if (strcmp(text, "-") == 0) return VLOOP_VN_SUB;
  if (strcmp(text, "*") == 0) return VLOOP_VN_MUL;
  if (strcmp(text, "/") == 0) return VLOOP_VN_DIV;
  return -1;
}

static int vloop_resolve_body_local(IRFunction *function, const char *sym,
                                    const char *iv, VLoopDag *d);

static int vloop_build(IRFunction *function, size_t before, const IROperand *op,
                       const char *iv, VLoopDag *d) {
  if (!op || d->overflow) {
    return -1;
  }
  double cv = 0.0;
  if (vloop_operand_is_literal(function, before, op, d->width_bits, &cv)) {
    int ci = vloop_intern_const(d, cv);
    return ci < 0 ? -1 : vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return -1;
  }
  if (op->kind == IR_OPERAND_TEMP) {
    const char *base = NULL;
    int bits = 0;
    if (ir_decode_float_indexed_load(function, before, op->name, iv, &base,
                                     &bits) &&
        bits == d->width_bits) {
      int ai = vloop_intern_array(d, base);
      return ai < 0 ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
    }
  }
  if (op->kind == IR_OPERAND_SYMBOL) {
    if (vloop_symbol_written_in_body(function, d, op->name)) {
      return vloop_resolve_body_local(function, op->name, iv, d);
    }
    if (ir_float_scalar_operand_matches(function, op, d->width_bits) &&
        !ir_symbol_address_taken(function, op->name)) {
      int si = vloop_intern_scalar(d, op->name);
      return si < 0 ? -1 : vloop_add_node(d, VLOOP_VN_SCALAR, si, 0);
    }
  }
  const IRInstruction *p = ir_i2f_resolve_producer(function, before, op);
  if (!p) {
    return -1;
  }
  size_t pidx = (size_t)(p - function->instructions);
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      vloop_text_is_float_width(p->text, d->width_bits) &&
      ir_operand_is_symbol_named(&p->lhs, iv)) {
    d->has_iota = 1;
    return vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
  }
  if (p->op == IR_OP_CAST && !p->is_float && p->text && d->width_bits == 64 &&
      vloop_text_is_float_width(p->text, d->width_bits) && d->iota_bound_known &&
      d->iota_bound > 0 &&
      (p->lhs.kind == IR_OPERAND_TEMP || p->lhs.kind == IR_OPERAND_SYMBOL)) {
    const IRInstruction *q = ir_i2f_resolve_producer(function, pidx, &p->lhs);
    if (q && q->op == IR_OP_BINARY && !q->is_float && q->text &&
        (strcmp(q->text, "+") == 0 || strcmp(q->text, "-") == 0)) {
      int is_sub = strcmp(q->text, "-") == 0;
      int iv_left = ir_operand_is_symbol_named(&q->lhs, iv);
      int iv_right = ir_operand_is_symbol_named(&q->rhs, iv);
      long long C = 0;
      int have = 0, on_left = 0;
      if (iv_left && q->rhs.kind == IR_OPERAND_INT) {
        C = q->rhs.int_value; have = 1; on_left = 1;
      } else if (iv_right && q->lhs.kind == IR_OPERAND_INT) {
        C = q->lhs.int_value; have = 1; on_left = 0;
      }
      if (have) {
        long long hi = d->iota_bound - 1;
        long long rmin, rmax;
        if (on_left) {
          rmin = is_sub ? -C : C;
          rmax = is_sub ? hi - C : hi + C;
        } else {
          rmin = is_sub ? C - hi : C;
          rmax = is_sub ? C : C + hi;
        }
        if (rmin >= -2147483648LL && rmax <= 2147483647LL) {
          int tag = vloop_binop_tag(q->text);
          int ci = vloop_intern_const(d, (double)C);
          if (tag < 0 || ci < 0) return -1;
          int a, b;
          if (on_left) {
            d->has_iota = 1;
            a = vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
            b = vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
          } else {
            a = vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
            d->has_iota = 1;
            b = vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
          }
          if (a < 0 || b < 0) return -1;
          return vloop_add_node(d, tag, a, b);
        }
      }
    }
  }
  if (p->op == IR_OP_BINARY && p->is_float && p->text) {
    int tag = vloop_binop_tag(p->text);
    if (tag < 0) {
      return -1;
    }
    int a = vloop_build(function, pidx, &p->lhs, iv, d);
    if (a < 0) {
      return -1;
    }
    int b = vloop_build(function, pidx, &p->rhs, iv, d);
    if (b < 0) {
      return -1;
    }
    return vloop_add_node(d, tag, a, b);
  }
  return -1;
}

static int vloop_resolve_body_local(IRFunction *function, const char *sym,
                                    const char *iv, VLoopDag *d) {
  if (!sym || d->resolve_depth >= VLOOP_MAX_RESOLVE_DEPTH || d->overflow) {
    return -1;
  }
  const IRInstruction *def = NULL;
  size_t def_idx = 0;
  for (size_t i = d->body_lo; i < d->body_hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        strcmp(ins->dest.name, sym) == 0) {
      if (def) {
        return -1;
      }
      def = ins;
      def_idx = i;
    }
  }
  if (!def || ir_symbol_live_after_loop(function, d->body_hi + 1, sym)) {
    return -1;
  }
  d->resolve_depth++;
  int result = -1;
  if (def->op == IR_OP_BINARY && def->is_float && def->text) {
    int tag = vloop_binop_tag(def->text);
    if (tag >= 0) {
      int a = vloop_build(function, def_idx, &def->lhs, iv, d);
      int b = (a < 0) ? -1 : vloop_build(function, def_idx, &def->rhs, iv, d);
      if (a >= 0 && b >= 0) {
        result = vloop_add_node(d, tag, a, b);
      }
    }
  } else if (def->op == IR_OP_ASSIGN) {
    result = vloop_build(function, def_idx, &def->lhs, iv, d);
  } else if (def->op == IR_OP_LOAD && def->is_float &&
             ir_operand_is_temp(&def->lhs) &&
             def->rhs.kind == IR_OPERAND_INT &&
             def->rhs.int_value == d->width_bits / 8) {
    const char *base = NULL;
    int bits = 0;
    if (ir_decode_float_indexed_address(function, def_idx, def->lhs.name, iv,
                                        &base, &bits) &&
        bits == d->width_bits) {
      int ai = vloop_intern_array(d, base);
      result = (ai < 0) ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
    }
  }
  d->resolve_depth--;
  return result;
}

static int vloop_eval_depth(const VLoopDag *d, int node) {
  const VLoopNode *n = &d->nodes[node];
  if (vloop_tag_is_leaf(n->tag)) {
    return 1;
  }
  if (n->tag == VLOOP_VN_SHL || n->tag == VLOOP_VN_SAR ||
      n->tag == VLOOP_VN_SHR) {
    return vloop_eval_depth(d, n->op0);
  }
  if (n->tag == VLOOP_VN_SELECT) {
    const VLoopNode *pair = &d->nodes[n->op1];
    int dm = vloop_eval_depth(d, n->op0);
    int dt = 1 + vloop_eval_depth(d, pair->op0);
    int de = 2 + vloop_eval_depth(d, pair->op1);
    int best = dm > dt ? dm : dt;
    return best > de ? best : de;
  }
  int da = vloop_eval_depth(d, n->op0);
  int db = vloop_eval_depth(d, n->op1);
  int alt = 1 + db;
  return da > alt ? da : alt;
}

static int vloop_distinct_bases(const VLoopDag *d, const char *dst_base) {
  int n = d->n_arrays;
  for (int i = 0; i < d->n_arrays; i++) {
    if (strcmp(d->arrays[i], dst_base) == 0) {
      return n;
    }
  }
  return n + 1;
}

static int vloop_serialize_into(IRInstruction *fused, const VLoopDag *d,
                                int reduce_op, int root, int depth) {
  size_t argc = (size_t)(7 + d->n_arrays + d->n_scalars + 3 * d->n_nodes +
                         d->n_consts);
  fused->arguments = calloc(argc, sizeof(IROperand));
  if (!fused->arguments) {
    return 0;
  }
  fused->argument_count = argc;
  size_t k = 0;
  fused->arguments[k++] = ir_operand_int(reduce_op);
  fused->arguments[k++] = ir_operand_int(d->n_arrays);
  fused->arguments[k++] = ir_operand_int(d->n_nodes);
  fused->arguments[k++] = ir_operand_int(root);
  fused->arguments[k++] = ir_operand_int(d->n_consts);
  fused->arguments[k++] = ir_operand_int(d->n_scalars);
  fused->arguments[k++] = ir_operand_int(depth);
  for (int i = 0; i < d->n_arrays; i++) {
    fused->arguments[k++] = ir_operand_symbol(d->arrays[i]);
  }
  for (int i = 0; i < d->n_scalars; i++) {
    fused->arguments[k++] = ir_operand_symbol(d->scalars[i]);
  }
  for (int i = 0; i < d->n_nodes; i++) {
    fused->arguments[k++] = ir_operand_int(d->nodes[i].tag);
    fused->arguments[k++] = ir_operand_int(d->nodes[i].op0);
    fused->arguments[k++] = ir_operand_int(d->nodes[i].op1);
  }
  for (int i = 0; i < d->n_consts; i++) {
    fused->arguments[k++] = d->is_int
                                ? ir_operand_int(d->iconsts[i])
                                : ir_operand_float_sized(d->consts[i], 64);
  }
  return 1;
}

static int ir_try_vectorize_map_at(IRFunction *function, size_t header_index,
                                   int *changed) {
  const char *iv_symbol = NULL;
  const char *dst_base = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t store_index = 0;
  IROperand bound = {0};
  int matched = 0;
  int store_bits = 0;
  VLoopDag d;
  int root = -1;
  int depth = 0;
  const IRInstruction *store = NULL;
  IRInstruction fused = {0};

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_map_body_is_safe(function, branch_index + 1, jump_index,
                                 iv_symbol, &store_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  store = &function->instructions[store_index];
  if (!store->is_float || store->dest.kind != IR_OPERAND_TEMP ||
      !store->dest.name ||
      (store->lhs.kind != IR_OPERAND_TEMP && store->lhs.kind != IR_OPERAND_SYMBOL &&
       store->lhs.kind != IR_OPERAND_FLOAT) ||
      store->rhs.kind != IR_OPERAND_INT ||
      (store->rhs.int_value != 4 && store->rhs.int_value != 8) ||
      !ir_decode_float_indexed_address(function, store_index, store->dest.name,
                                       iv_symbol, &dst_base, &store_bits) ||
      store_bits != store->rhs.int_value * 8) {
    ir_operand_destroy(&bound);
    return 1;
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = store_bits;
  d.body_lo = branch_index + 1;
  d.body_hi = jump_index;
  if (bound.kind == IR_OPERAND_INT) {
    d.iota_bound_known = 1;
    d.iota_bound = bound.int_value;
  }
  root = vloop_build(function, store_index, &store->lhs, iv_symbol, &d);
  if (root < 0 || d.overflow) {
    ir_operand_destroy(&bound);
    return 1;
  }

  if (!ir_symbol_is_float_array_base(function, dst_base)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  for (int i = 0; i < d.n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d.arrays[i])) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  depth = vloop_eval_depth(&d, root);
  if (depth > vloop_map_budget(&d) ||
      vloop_distinct_bases(&d, dst_base) > VLOOP_MAX_ARRAYS) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_F64;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = store_bits;
  fused.dest = ir_operand_symbol(dst_base);
  fused.lhs = bound;
  if (!vloop_serialize_into(&fused, &d, 0, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

static int ir_try_vectorize_reduce_at(IRFunction *function, size_t header_index,
                                      int *changed) {
  const char *iv_symbol = NULL;
  const char *acc_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t reduce_index = 0;
  IROperand bound = {0};
  int matched = 0;
  int found = 0;
  const IROperand *addend = NULL;
  VLoopDag d;
  int root = -1;
  int depth = 0;
  int width_bits = 0;
  IRInstruction fused = {0};

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_body_is_pure_reduction(function, branch_index + 1,
                                       jump_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  size_t assign_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!(ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 &&
          (ins->rhs.kind == IR_OPERAND_TEMP ||
           ins->rhs.kind == IR_OPERAND_SYMBOL))) {
      continue;
    }
    if (ir_operand_is_symbol(&ins->dest) &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name)) {
      acc_symbol = ins->dest.name;
      addend = &ins->rhs;
      reduce_index = i;
      assign_index = (size_t)-1;
      found++;
    } else if (ir_operand_is_temp(&ins->dest) &&
               ir_operand_is_symbol(&ins->lhs)) {
      size_t j = i + 1;
      while (j < jump_index && function->instructions[j].op == IR_OP_NOP) {
        j++;
      }
      if (j < jump_index) {
        const IRInstruction *asg = &function->instructions[j];
        if (asg->op == IR_OP_ASSIGN &&
            ir_operand_is_symbol_named(&asg->dest, ins->lhs.name) &&
            ir_operand_is_temp_named(&asg->lhs, ins->dest.name)) {
          acc_symbol = ins->lhs.name;
          addend = &ins->rhs;
          reduce_index = i;
          assign_index = j;
          found++;
        }
      }
    }
  }
  if (found != 1 || !acc_symbol || strcmp(acc_symbol, iv_symbol) == 0) {
    ir_operand_destroy(&bound);
    return 1;
  }
  {
    const char *acc_type = ir_function_local_declared_type(function, acc_symbol);
    if (acc_type && strcmp(acc_type, "float64") == 0) {
      width_bits = 64;
    } else if (acc_type && strcmp(acc_type, "float32") == 0) {
      width_bits = 32;
    } else {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i == reduce_index || i == assign_index) {
      continue;
    }
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest)) {
      if (strcmp(ins->dest.name, acc_symbol) == 0) {
        ir_operand_destroy(&bound);
        return 1;
      }
      if (strcmp(ins->dest.name, iv_symbol) != 0 &&
          ir_symbol_live_after_loop(function, jump_index + 1, ins->dest.name)) {
        ir_operand_destroy(&bound);
        return 1;
      }
    }
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = width_bits;
  d.body_lo = branch_index + 1;
  d.body_hi = jump_index;
  if (bound.kind == IR_OPERAND_INT) {
    d.iota_bound_known = 1;
    d.iota_bound = bound.int_value;
  }
  root = vloop_build(function, reduce_index, addend, iv_symbol, &d);
  if (root < 0 || d.overflow) {
    ir_operand_destroy(&bound);
    return 1;
  }
  for (int i = 0; i < d.n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d.arrays[i])) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  depth = vloop_eval_depth(&d, root);
  if (depth > VLOOP_REG_BUDGET - 1  ||
      d.n_arrays > VLOOP_MAX_ARRAYS) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_F64;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = width_bits;
  fused.dest = ir_operand_symbol(acc_symbol);
  fused.lhs = bound;
  if (!vloop_serialize_into(&fused, &d, 1, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

#define MULTISTORE_MAX 8

static int ir_msf_name_is_allocator(const char *n) {
  if (!n) return 0;
  static const char *const a[] = {"malloc",        "calloc",
                                  "aligned_alloc", "_aligned_malloc",
                                  "alloc_zeroed",  NULL};
  for (int i = 0; a[i]; i++)
    if (strcmp(n, a[i]) == 0) return 1;
  return 0;
}

static int ir_msf_single_def(IRFunction *function, const char *name,
                             int is_symbol) {
  int found = -1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op >= IR_OP_SIMD_SUM_I32 && ins->op <= IR_OP_SIMD_LCG_U32) continue;
    if (!ir_instruction_writes_destination(ins)) continue;
    const IROperand *d = &ins->dest;
    int match = is_symbol
                    ? (d->kind == IR_OPERAND_SYMBOL && d->name && name &&
                       strcmp(d->name, name) == 0)
                    : (d->kind == IR_OPERAND_TEMP && d->name && name &&
                       strcmp(d->name, name) == 0);
    if (match) {
      if (found >= 0) return -1;
      found = (int)i;
    }
  }
  return found;
}

static int ir_msf_value_is_fresh_alloc(IRFunction *function, const IROperand *v,
                                       int depth) {
  if (depth <= 0 || !v ||
      (v->kind != IR_OPERAND_TEMP && v->kind != IR_OPERAND_SYMBOL)) {
    return 0;
  }
  int di = ir_msf_single_def(function, v->name, v->kind == IR_OPERAND_SYMBOL);
  if (di < 0) return 0;
  const IRInstruction *def = &function->instructions[di];
  if (def->op == IR_OP_NEW) return 1;
  if (def->op == IR_OP_CALL && ir_msf_name_is_allocator(def->text)) return 1;
  if (def->op == IR_OP_CAST || def->op == IR_OP_ASSIGN) {
    return ir_msf_value_is_fresh_alloc(function, &def->lhs, depth - 1);
  }
  return 0;
}

static int ir_msf_bases_disjoint(IRFunction *function, const char **bases,
                                 int k) {
  for (int i = 0; i < k; i++) {
    if (!bases[i]) return 0;
    for (int j = i + 1; j < k; j++)
      if (!bases[j] || strcmp(bases[i], bases[j]) == 0) return 0;
  }
  for (int i = 0; i < k; i++) {
    if (ir_function_local_declared_type(function, bases[i]) == NULL) return 0;
    if (ir_symbol_address_taken(function, bases[i])) return 0;
    int di = ir_msf_single_def(function, bases[i], 1);
    if (di < 0) return 0;
    const IRInstruction *def = &function->instructions[di];
    if (def->op == IR_OP_NEW) continue;
    if (def->op == IR_OP_CALL && ir_msf_name_is_allocator(def->text)) continue;
    if ((def->op == IR_OP_CAST || def->op == IR_OP_ASSIGN) &&
        ir_msf_value_is_fresh_alloc(function, &def->lhs, 4)) {
      continue;
    }
    return 0;
  }
  return 1;
}

static int ir_msf_body_is_safe(IRFunction *function, size_t lo, size_t hi,
                               const char *iv_symbol, size_t *stores,
                               int *nstores) {
  int ns = 0;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_STORE) {
      if (ns >= MULTISTORE_MAX) return 0;
      stores[ns++] = i;
      continue;
    }
    if (ins->op == IR_OP_LOAD || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_INLINE_ASM || ins->op == IR_OP_MEMCPY_INLINE ||
        ins->op == IR_OP_COUNT_WORD_STARTS) {
      return 0;
    }
    if (ir_instruction_writes_symbol(ins) &&
        !ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      if (ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name ||
          ir_symbol_live_after_loop(function, hi + 1, ins->dest.name)) {
        return 0;
      }
    }
  }
  *nstores = ns;
  return ns >= 2;
}

static int ir_msf_build_store(IRFunction *function, size_t store_index,
                              const char *iv_symbol, size_t body_lo,
                              size_t body_hi, const IROperand *bound,
                              IRInstruction *fused, const char **dst_base_out,
                              const char **reads_out, int *reads_count_out,
                              int *elem_bytes_out) {
  const IRInstruction *store = &function->instructions[store_index];
  const char *dst_base = NULL;
  int store_bits = 0;
  VLoopDag d;
  int root, depth;

  if (!store->is_float || store->dest.kind != IR_OPERAND_TEMP ||
      !store->dest.name ||
      (store->lhs.kind != IR_OPERAND_TEMP && store->lhs.kind != IR_OPERAND_SYMBOL &&
       store->lhs.kind != IR_OPERAND_FLOAT) ||
      store->rhs.kind != IR_OPERAND_INT ||
      (store->rhs.int_value != 4 && store->rhs.int_value != 8) ||
      !ir_decode_float_indexed_address(function, store_index, store->dest.name,
                                       iv_symbol, &dst_base, &store_bits) ||
      store_bits != store->rhs.int_value * 8) {
    return 0;
  }
  memset(&d, 0, sizeof(d));
  d.width_bits = store_bits;
  d.body_lo = body_lo;
  d.body_hi = body_hi;
  if (bound->kind == IR_OPERAND_INT) {
    d.iota_bound_known = 1;
    d.iota_bound = bound->int_value;
  }
  root = vloop_build(function, store_index, &store->lhs, iv_symbol, &d);
  if (root < 0 || d.overflow) return 0;
  if (!ir_symbol_is_float_array_base(function, dst_base)) return 0;
  for (int i = 0; i < d.n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d.arrays[i])) return 0;
  }
  depth = vloop_eval_depth(&d, root);
  if (depth > vloop_map_budget(&d) ||
      vloop_distinct_bases(&d, dst_base) > VLOOP_MAX_ARRAYS) {
    return 0;
  }
  memset(fused, 0, sizeof(*fused));
  fused->op = IR_OP_SIMD_VLOOP_F64;
  fused->location = store->location;
  fused->is_float = 1;
  fused->float_bits = store_bits;
  fused->dest = ir_operand_symbol(dst_base);
  if (!ir_operand_clone(bound, &fused->lhs) ||
      !vloop_serialize_into(fused, &d, 0, root, depth)) {
    ir_instruction_destroy_storage(fused);
    return 0;
  }
  *dst_base_out = dst_base;
  if (reads_out && reads_count_out) {
    int n = d.n_arrays < VLOOP_MAX_ARRAYS ? d.n_arrays : VLOOP_MAX_ARRAYS;
    for (int i = 0; i < n; i++) {
      reads_out[i] = d.arrays[i];
    }
    *reads_count_out = n;
  }
  if (elem_bytes_out) {
    *elem_bytes_out = store_bits / 8;
  }
  return 1;
}

#define MSF_GUARD_MAX_REGIONS (MULTISTORE_MAX + VLOOP_MAX_ARRAYS * MULTISTORE_MAX)

typedef struct {
  IRInstruction items[256];
  size_t count;
  int failed;
} MsfGuard;

static IROperand msf_temp(IRFunction *function, MsfGuard *g) {
  IROperand out = ir_operand_none();
  char name[64];
  static unsigned long long counter;
  (void)function;
  snprintf(name, sizeof(name), ".ovl%llu", counter++);
  out = ir_operand_temp(name);
  if (!out.name) {
    g->failed = 1;
  }
  return out;
}

static void msf_emit(MsfGuard *g, IROpcode op, const IROperand *dest,
                     const IROperand *lhs, const IROperand *rhs,
                     const char *text, SourceLocation location) {
  IRInstruction *insn;
  if (g->failed || g->count >= sizeof(g->items) / sizeof(g->items[0])) {
    g->failed = 1;
    return;
  }
  insn = &g->items[g->count++];
  memset(insn, 0, sizeof(*insn));
  insn->op = op;
  insn->location = location;
  if (dest) {
    insn->dest = *dest;
  }
  if (lhs) {
    insn->lhs = *lhs;
  }
  if (rhs) {
    insn->rhs = *rhs;
  }
  insn->text = (char *)text;
}

static IROperand msf_region_end(IRFunction *function, MsfGuard *g,
                                const char *base, const IROperand *bound,
                                int bytes, SourceLocation location) {
  IROperand span = msf_temp(function, g);
  IROperand end = msf_temp(function, g);
  IROperand base_operand = ir_operand_symbol(base);
  IROperand width = ir_operand_int(bytes);
  IROperand copy_of_bound;
  if (!ir_operand_clone(bound, &copy_of_bound)) {
    g->failed = 1;
    return end;
  }
  msf_emit(g, IR_OP_BINARY, &span, &copy_of_bound, &width, "*", location);
  msf_emit(g, IR_OP_BINARY, &end, &base_operand, &span, "+", location);
  return end;
}

static IROperand msf_pair_disjoint(IRFunction *function, MsfGuard *g,
                                   const char *p, int p_bytes, const char *q,
                                   int q_bytes, const IROperand *bound,
                                   SourceLocation location) {
  IROperand p_end = msf_region_end(function, g, p, bound, p_bytes, location);
  IROperand q_end = msf_region_end(function, g, q, bound, q_bytes, location);
  IROperand p_symbol = ir_operand_symbol(p);
  IROperand q_symbol = ir_operand_symbol(q);
  IROperand before = msf_temp(function, g);
  IROperand after = msf_temp(function, g);
  IROperand either = msf_temp(function, g);
  msf_emit(g, IR_OP_BINARY, &before, &p_end, &q_symbol, "<=", location);
  msf_emit(g, IR_OP_BINARY, &after, &q_end, &p_symbol, "<=", location);
  msf_emit(g, IR_OP_BINARY, &either, &before, &after, "|", location);
  return either;
}

static int ir_msf_guard_and_version(IRFunction *function, size_t header_index,
                                    size_t jump_index,
                                    IRInstruction *fused, int ns,
                                    const char **regions,
                                    const int *region_bytes,
                                    const int *region_writes, int n_regions,
                                    const IROperand *bound) {
  MsfGuard g;
  IROperand disjoint = ir_operand_none();
  const char *header_label = NULL;
  const char *end_label = NULL;
  SourceLocation location;
  size_t at;
  int pairs = 0;

  if (n_regions < 2 || ns <= 0) {
    return 0;
  }
  if (function->instructions[header_index].op != IR_OP_LABEL ||
      !function->instructions[header_index].text) {
    return 0;
  }
  header_label = function->instructions[header_index].text;
  {
    size_t end_index = 0;
    if (!ir_find_next_non_nop(function, jump_index + 1, &end_index) ||
        function->instructions[end_index].op != IR_OP_LABEL ||
        !function->instructions[end_index].text) {
      return 0;
    }
    end_label = function->instructions[end_index].text;
  }
  location = function->instructions[header_index].location;
  for (int i = 0; i < n_regions; i++) {
    if (!regions[i] || region_bytes[i] <= 0) {
      return 0;
    }
    if (ir_symbol_address_taken(function, regions[i])) {
      return 0;
    }
  }

  memset(&g, 0, sizeof(g));
  for (int i = 0; i < n_regions && !g.failed; i++) {
    for (int j = i + 1; j < n_regions && !g.failed; j++) {
      IROperand pair;
      if (!region_writes[i] && !region_writes[j]) {
        continue;
      }
      if (strcmp(regions[i], regions[j]) == 0) {
        return 0;
      }
      pair = msf_pair_disjoint(function, &g, regions[i], region_bytes[i],
                               regions[j], region_bytes[j], bound, location);
      if (pairs == 0) {
        disjoint = pair;
      } else {
        IROperand both = msf_temp(function, &g);
        msf_emit(&g, IR_OP_BINARY, &both, &disjoint, &pair, "&", location);
        disjoint = both;
      }
      pairs++;
    }
  }
  if (g.failed || pairs == 0) {
    return 0;
  }
  msf_emit(&g, IR_OP_BRANCH_ZERO, NULL, &disjoint, NULL, header_label,
           location);
  if (g.failed) {
    return 0;
  }

  at = header_index;
  for (size_t i = 0; i < g.count; i++) {
    if (!ir_function_insert_instruction(function, at, &g.items[i])) {
      return 0;
    }
    at++;
  }
  for (int k = 0; k < ns; k++) {
    if (!ir_function_insert_instruction(function, at, &fused[k])) {
      return 0;
    }
    ir_instruction_destroy_storage(&fused[k]);
    at++;
  }
  {
    IRInstruction skip;
    memset(&skip, 0, sizeof(skip));
    skip.op = IR_OP_JUMP;
    skip.location = location;
    skip.text = (char *)end_label;
    if (!ir_function_insert_instruction(function, at, &skip)) {
      return 0;
    }
  }
  return 1;
}

static int ir_try_vectorize_multistore_map_at(IRFunction *function,
                                              size_t header_index,
                                              int *changed) {
  const char *iv_symbol = NULL;
  size_t branch_index = 0, jump_index = 0;
  IROperand bound = {0};
  int matched = 0;
  size_t stores[MULTISTORE_MAX];
  int ns = 0;
  IRInstruction fused[MULTISTORE_MAX];
  const char *bases[MULTISTORE_MAX];
  const char *regions[MSF_GUARD_MAX_REGIONS];
  int region_bytes[MSF_GUARD_MAX_REGIONS];
  int region_writes[MSF_GUARD_MAX_REGIONS];
  int n_regions = 0;
  int built = 0;

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) return 1;
  if (!ir_msf_body_is_safe(function, branch_index + 1, jump_index, iv_symbol,
                           stores, &ns)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  if ((size_t)ns > jump_index - header_index + 1) {
    ir_operand_destroy(&bound);
    return 1;
  }
  for (built = 0; built < ns; built++) {
    const char *reads[VLOOP_MAX_ARRAYS];
    int n_reads = 0;
    int bytes = 0;
    if (!ir_msf_build_store(function, stores[built], iv_symbol,
                            branch_index + 1, jump_index, &bound, &fused[built],
                            &bases[built], reads, &n_reads, &bytes)) {
      for (int k = 0; k < built; k++) ir_instruction_destroy_storage(&fused[k]);
      ir_operand_destroy(&bound);
      return 1;
    }
    if (n_regions < MSF_GUARD_MAX_REGIONS) {
      regions[n_regions] = bases[built];
      region_bytes[n_regions] = bytes;
      region_writes[n_regions] = 1;
      n_regions++;
    }
    for (int r = 0; r < n_reads && n_regions < MSF_GUARD_MAX_REGIONS; r++) {
      int seen = 0;
      for (int q = 0; q < n_regions; q++) {
        if (regions[q] && reads[r] && strcmp(regions[q], reads[r]) == 0) {
          seen = 1;
          break;
        }
      }
      if (seen || !reads[r]) {
        continue;
      }
      regions[n_regions] = reads[r];
      region_bytes[n_regions] = bytes;
      region_writes[n_regions] = 0;
      n_regions++;
    }
  }
  if (ir_msf_bases_disjoint(function, bases, ns)) {
    if (ir_explain_enabled()) {
      ir_explain_remark(function->name, "loop body",
                        function->instructions[header_index].location, 0,
                        "vectorized",
                        "each destination is a fresh allocation nothing else "
                        "names, so the regions cannot overlap and no run-time "
                        "overlap test was emitted",
                        NULL, NULL);
      ir_explain_remark_code("multistore-disjoint-proven");
    }
    for (int k = 0; k < ns; k++) {
      ir_instruction_destroy_storage(&function->instructions[header_index + k]);
      function->instructions[header_index + k] = fused[k];
    }
    for (size_t i = header_index + ns; i <= jump_index; i++) {
      ir_instruction_make_nop(&function->instructions[i]);
    }
    ir_operand_destroy(&bound);
    *changed = 1;
    return 1;
  }
  if (!ir_msf_guard_and_version(function, header_index, jump_index, fused, ns,
                                regions, region_bytes, region_writes,
                                n_regions, &bound)) {
    for (int k = 0; k < ns; k++) ir_instruction_destroy_storage(&fused[k]);
    ir_operand_destroy(&bound);
    return 1;
  }
  if (ir_explain_enabled()) {
    ir_explain_remark(function->name, "loop body",
                      function->instructions[header_index].location, 0,
                      "vectorized under a run-time overlap test",
                      "nothing here proves the regions are distinct, so the "
                      "loop was kept and a test put in front of it: disjoint "
                      "takes the kernels, anything else takes the loop. A "
                      "proof that the regions cannot overlap would delete the "
                      "test",
                      NULL, NULL);
    ir_explain_remark_code("multistore-overlap-tested");
  }
  ir_operand_destroy(&bound);
  *changed = 1;
  return 1;
}

int ir_auto_vectorize_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_multistore_map_at(function, i, changed)) {
        return 0;
      }
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_map_at(function, i, changed)) {
        return 0;
      }
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_reduce_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

#define IR_MINMAX_MAX_ACCUMULATORS 2

static int vloop_build_int(IRFunction *function, size_t before,
                           const IROperand *op, const char *iv, VLoopDag *d);

typedef struct {
  const char *acc;
  int is_max;
  IROperand value;
  size_t cmp_index;
  size_t end;
} IRMinMaxDiamond;

static int ir_minmax_same_operand(const IRFunction *function, size_t body_lo,
                                  size_t before, const IROperand *a,
                                  const IROperand *b) {
  if (!a || !b || !a->name || !b->name) {
    return 0;
  }
  if (a->kind == b->kind && ir_operand_names_match(a, b)) {
    return 1;
  }
  for (size_t i = body_lo; i < before; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_ASSIGN || ins->dest.kind != IR_OPERAND_SYMBOL ||
        !ins->dest.name || !ins->lhs.name) {
      continue;
    }
    if ((ir_operand_is_symbol_named(&ins->dest, a->name) &&
         strcmp(ins->lhs.name, b->name) == 0) ||
        (ir_operand_is_symbol_named(&ins->dest, b->name) &&
         strcmp(ins->lhs.name, a->name) == 0)) {
      return 1;
    }
  }
  return 0;
}

static int ir_minmax_arm_restates_value(IRFunction *function, size_t body_lo,
                                        const IRInstruction *arm,
                                        size_t arm_index,
                                        const IROperand *tested,
                                        const char *iv) {
  if (arm->op == IR_OP_ASSIGN) {
    return ir_minmax_same_operand(function, body_lo, arm_index, tested,
                                  &arm->lhs);
  }
  if (arm->op == IR_OP_LOAD && arm->lhs.kind == IR_OPERAND_TEMP &&
      arm->lhs.name && tested->kind == IR_OPERAND_TEMP && tested->name) {
    const char *tested_base = NULL;
    const char *arm_base = NULL;
    int tested_bits = 0;
    int arm_bits = 0;
    return ir_decode_float_indexed_load(function, arm_index, tested->name, iv,
                                        &tested_base, &tested_bits) &&
           ir_decode_float_indexed_address(function, arm_index, arm->lhs.name,
                                           iv, &arm_base, &arm_bits) &&
           tested_bits == arm_bits && tested_base && arm_base &&
           strcmp(tested_base, arm_base) == 0;
  }
  return 0;
}

static int ir_match_minmax_diamond(IRFunction *function, size_t at,
                                   size_t body_lo, size_t body_hi,
                                   const char *iv, IRMinMaxDiamond *out) {
  const IRInstruction *cmp = &function->instructions[at];
  size_t branch = 0, assign = 0, jump = 0, next_label = 0, end_label = 0;
  const IROperand *acc_side = NULL;
  const IROperand *val_side = NULL;
  int acc_is_left = 0;

  if (cmp->op != IR_OP_BINARY || !cmp->text || cmp->dest.kind != IR_OPERAND_TEMP ||
      !cmp->dest.name ||
      (strcmp(cmp->text, "<") != 0 && strcmp(cmp->text, ">") != 0)) {
    return 0;
  }
  if (!ir_find_next_non_nop(function, at + 1, &branch) || branch >= body_hi) {
    return 0;
  }
  {
    const IRInstruction *br = &function->instructions[branch];
    if (br->op != IR_OP_BRANCH_ZERO || !br->text ||
        !ir_operand_is_temp_named(&br->lhs, cmp->dest.name)) {
      return 0;
    }
    for (jump = branch + 1; jump < body_hi; jump++) {
      IROpcode op = function->instructions[jump].op;
      if (op == IR_OP_JUMP) {
        break;
      }
      if (op == IR_OP_LABEL || op == IR_OP_BRANCH_ZERO ||
          op == IR_OP_BRANCH_EQ || op == IR_OP_STORE || op == IR_OP_CALL ||
          op == IR_OP_CALL_INDIRECT || op == IR_OP_RETURN) {
        return 0;
      }
    }
    if (jump >= body_hi) {
      return 0;
    }
    assign = jump;
    while (assign > branch + 1 &&
           function->instructions[assign - 1].op == IR_OP_NOP) {
      assign--;
    }
    if (assign == branch + 1) {
      return 0;
    }
    assign--;
    if (!ir_find_next_non_nop(function, jump + 1, &next_label) ||
        next_label >= body_hi ||
        !ir_find_next_non_nop(function, next_label + 1, &end_label) ||
        end_label >= body_hi) {
      return 0;
    }
  }
  {
    const IRInstruction *br = &function->instructions[branch];
    const IRInstruction *as = &function->instructions[assign];
    const IRInstruction *jp = &function->instructions[jump];
    const IRInstruction *nl = &function->instructions[next_label];
    const IRInstruction *el = &function->instructions[end_label];
    if (!ir_instruction_writes_destination(as) ||
        as->dest.kind != IR_OPERAND_SYMBOL || !as->dest.name ||
        jp->op != IR_OP_JUMP || !jp->text ||
        nl->op != IR_OP_LABEL || !nl->text || strcmp(nl->text, br->text) != 0 ||
        el->op != IR_OP_LABEL || !el->text || strcmp(el->text, jp->text) != 0) {
      return 0;
    }
    for (size_t i = branch + 1; i < jump; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (i != assign && ir_instruction_writes_destination(ins) &&
          ins->dest.kind == IR_OPERAND_SYMBOL) {
        return 0;
      }
    }
    if (ir_operand_is_symbol_named(&cmp->lhs, as->dest.name)) {
      acc_side = &cmp->lhs;
      val_side = &cmp->rhs;
      acc_is_left = 1;
    } else if (ir_operand_is_symbol_named(&cmp->rhs, as->dest.name)) {
      acc_side = &cmp->rhs;
      val_side = &cmp->lhs;
    } else {
      return 0;
    }
    if (!ir_minmax_arm_restates_value(function, body_lo, as, assign, val_side,
                                      iv)) {
      return 0;
    }
    out->acc = acc_side->name;
    out->is_max = acc_is_left ? (strcmp(cmp->text, "<") == 0)
                              : (strcmp(cmp->text, ">") == 0);
    out->cmp_index = at;
    out->end = end_label + 1;
    if (!ir_operand_clone(val_side, &out->value)) {
      return 0;
    }
  }
  return 1;
}

static int ir_minmax_acc_width(const IRFunction *function, const char *acc,
                               int *is_int_out) {
  const char *ty = ir_function_local_declared_type(function, acc);
  if (!ty) {
    ty = ir_function_param_declared_type(function, acc);
  }
  if (!ty) {
    return 0;
  }
  if (strcmp(ty, "float64") == 0) {
    *is_int_out = 0;
    return 64;
  }
  if (strcmp(ty, "float32") == 0) {
    *is_int_out = 0;
    return 32;
  }
  if (strcmp(ty, "int32") == 0) {
    *is_int_out = 1;
    return 32;
  }
  return 0;
}

static int ir_minmax_reads_an_accumulator(const VLoopDag *d,
                                          const IRMinMaxDiamond *diamonds,
                                          int n_diamonds) {
  for (int s = 0; s < d->n_scalars; s++) {
    for (int j = 0; j < n_diamonds; j++) {
      if (d->scalars[s] && strcmp(d->scalars[s], diamonds[j].acc) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static int ir_minmax_claims_loop(const IRFunction *function,
                                 size_t header_index, size_t jump_index,
                                 IROpcode op, const IRMinMaxDiamond *diamonds,
                                 int n_diamonds) {
  const char *claimed[IR_MINMAX_MAX_ACCUMULATORS];
  for (int k = 0; k < n_diamonds; k++) {
    claimed[k] = diamonds[k].acc;
  }
  return ir_fused_loop_claims_symbols(function, header_index, jump_index, op,
                                      claimed, (size_t)n_diamonds);
}

static int ir_try_vectorize_minmax_at(IRFunction *function, size_t header_index,
                                      int *changed) {
  const char *iv_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  int matched = 0;
  IRMinMaxDiamond diamonds[IR_MINMAX_MAX_ACCUMULATORS];
  int n_diamonds = 0;
  IRInstruction fused[IR_MINMAX_MAX_ACCUMULATORS];
  int width_bits = 0;
  int is_int = 0;
  int ok = 1;

  memset(diamonds, 0, sizeof(diamonds));
  memset(fused, 0, sizeof(fused));

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index && ok;) {
    const IRInstruction *ins = &function->instructions[i];
    IRMinMaxDiamond d;
    memset(&d, 0, sizeof(d));
    if (ir_match_minmax_diamond(function, i, branch_index + 1, jump_index,
                                iv_symbol, &d)) {
      if (n_diamonds >= IR_MINMAX_MAX_ACCUMULATORS) {
        ir_operand_destroy(&d.value);
        ok = 0;
        break;
      }
      diamonds[n_diamonds++] = d;
      i = d.end;
      continue;
    }
    if (ins->op == IR_OP_STORE || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_LABEL ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_RETURN) {
      ok = 0;
      break;
    }
    i++;
  }
  if (!ok || n_diamonds == 0) {
    goto refuse;
  }

  for (int k = 0; k < n_diamonds; k++) {
    int k_int = 0;
    int w = ir_minmax_acc_width(function, diamonds[k].acc, &k_int);
    if (!w || strcmp(diamonds[k].acc, iv_symbol) == 0) {
      goto refuse;
    }
    if (k == 0) {
      width_bits = w;
      is_int = k_int;
    } else if (w != width_bits || k_int != is_int) {
      goto refuse;
    }
    for (int j = 0; j < k; j++) {
      if (strcmp(diamonds[j].acc, diamonds[k].acc) == 0) {
        goto refuse;
      }
    }
  }
  if (is_int && n_diamonds == 2) {
    goto refuse;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    int is_acc_write = 0;
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name) {
      continue;
    }
    for (int k = 0; k < n_diamonds; k++) {
      if (strcmp(ins->dest.name, diamonds[k].acc) == 0) {
        is_acc_write = 1;
        if (i < diamonds[k].cmp_index || i >= diamonds[k].end) {
          goto refuse;
        }
      }
    }
    if (is_acc_write || strcmp(ins->dest.name, iv_symbol) == 0) {
      continue;
    }
    if (ir_symbol_live_after_loop(function, jump_index + 1, ins->dest.name)) {
      goto refuse;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    goto refuse;
  }

  for (int k = 0; k < n_diamonds; k++) {
    VLoopDag d;
    int root = -1;
    int depth = 0;
    memset(&d, 0, sizeof(d));
    d.width_bits = width_bits;
    d.is_int = is_int;
    d.elem_bits = 32;
    d.body_lo = branch_index + 1;
    d.body_hi = jump_index;
    if (bound.kind == IR_OPERAND_INT) {
      d.iota_bound_known = 1;
      d.iota_bound = bound.int_value;
    }
    root = is_int ? vloop_build_int(function, diamonds[k].cmp_index,
                                    &diamonds[k].value, iv_symbol, &d)
                  : vloop_build(function, diamonds[k].cmp_index,
                                &diamonds[k].value, iv_symbol, &d);
    if (root < 0 || d.overflow) {
      goto refuse;
    }
    for (int a = 0; a < d.n_arrays; a++) {
      if (!ir_symbol_is_float_array_base(function, d.arrays[a])) {
        goto refuse;
      }
    }
    if (ir_minmax_reads_an_accumulator(&d, diamonds, n_diamonds)) {
      goto refuse;
    }
    depth = vloop_eval_depth(&d, root);
    if (depth > VLOOP_REG_BUDGET - 1 || d.n_arrays > VLOOP_MAX_ARRAYS) {
      goto refuse;
    }
    fused[k].op = is_int ? IR_OP_SIMD_VLOOP_I32 : IR_OP_SIMD_VLOOP_F64;
    fused[k].location = function->instructions[header_index].location;
    fused[k].is_float = !is_int;
    fused[k].float_bits = width_bits;
    fused[k].dest = ir_operand_symbol(diamonds[k].acc);
    if (!ir_operand_clone(&bound, &fused[k].lhs) ||
        !vloop_serialize_into(&fused[k], &d, diamonds[k].is_max ? 2 : 3, root,
                              depth)) {
      goto refuse;
    }
  }

  if (!ir_minmax_claims_loop(function, header_index, jump_index, fused[0].op,
                             diamonds, n_diamonds)) {
    goto refuse;
  }
  for (int k = 0; k < n_diamonds; k++) {
    ir_instruction_destroy_storage(&function->instructions[header_index + k]);
    function->instructions[header_index + k] = fused[k];
    memset(&fused[k], 0, sizeof(fused[k]));
  }
  for (size_t i = header_index + (size_t)n_diamonds; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  ir_operand_destroy(&bound);
  for (int k = 0; k < n_diamonds; k++) {
    ir_operand_destroy(&diamonds[k].value);
  }
  return 1;

refuse:
  ir_operand_destroy(&bound);
  for (int k = 0; k < n_diamonds; k++) {
    ir_operand_destroy(&diamonds[k].value);
  }
  for (int k = 0; k < IR_MINMAX_MAX_ACCUMULATORS; k++) {
    if (fused[k].op != IR_OP_NOP || fused[k].arguments) {
      ir_instruction_destroy_storage(&fused[k]);
      memset(&fused[k], 0, sizeof(fused[k]));
    }
  }
  return 1;
}

int ir_simd_minmax_reduce_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_minmax_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

static int vloop_int_scalar_type_ok(const char *ty) {
  return ty && (strcmp(ty, "int8") == 0 || strcmp(ty, "uint8") == 0 ||
                strcmp(ty, "int16") == 0 || strcmp(ty, "uint16") == 0 ||
                strcmp(ty, "int32") == 0 || strcmp(ty, "uint32") == 0 ||
                strcmp(ty, "int64") == 0 || strcmp(ty, "uint64") == 0 ||
                strcmp(ty, "int") == 0);
}

static int vloop_int_cast_is_transparent(const char *ty) {
  return ty && (strcmp(ty, "int32") == 0 || strcmp(ty, "uint32") == 0 ||
                strcmp(ty, "int64") == 0 || strcmp(ty, "uint64") == 0 ||
                strcmp(ty, "int") == 0);
}

#define VLOOP_RANGE_LIMIT 1099511627776LL

static int vloop_int_node_range(const VLoopDag *d, int node, long long *lo_out,
                                long long *hi_out, int depth) {
  const VLoopNode *n = NULL;
  long long alo = 0, ahi = 0, blo = 0, bhi = 0;
  int b_known = 0;

  if (node < 0 || node >= d->n_nodes || depth > VLOOP_MAX_RESOLVE_DEPTH) {
    return 0;
  }
  n = &d->nodes[node];
  switch (n->tag) {
  case VLOOP_VN_LOAD:
    if (d->elem_bits == 8) {
      *lo_out = d->elem_unsigned > 0 ? 0 : -128;
      *hi_out = d->elem_unsigned > 0 ? 255 : 127;
    } else {
      *lo_out = -2147483648LL;
      *hi_out = 2147483647LL;
    }
    return 1;
  case VLOOP_VN_CONST:
    if (n->op0 < 0 || n->op0 >= d->n_consts) {
      return 0;
    }
    *lo_out = d->iconsts[n->op0];
    *hi_out = d->iconsts[n->op0];
    return 1;
  case VLOOP_VN_IOTA:
    if (!d->iota_bound_known || d->iota_bound <= 0) {
      return 0;
    }
    *lo_out = 0;
    *hi_out = d->iota_bound - 1;
    return 1;
  case VLOOP_VN_SCALAR:
    return 0;
  default:
    break;
  }
  {
    int a_known = vloop_int_node_range(d, n->op0, &alo, &ahi, depth + 1);
    b_known = vloop_int_node_range(d, n->op1, &blo, &bhi, depth + 1);
    if (n->tag == VLOOP_VN_AND) {
      if (b_known && blo == bhi && blo >= 0) {
        *lo_out = 0;
        *hi_out = blo;
      } else if (a_known && alo == ahi && alo >= 0) {
        *lo_out = 0;
        *hi_out = alo;
      } else if (a_known && b_known && alo >= 0 && blo >= 0) {
        *lo_out = 0;
        *hi_out = ahi < bhi ? ahi : bhi;
      } else {
        return 0;
      }
      return 1;
    }
    if (!a_known) {
      return 0;
    }
  }
  switch (n->tag) {
  case VLOOP_VN_SHL:
    if (n->op1 < 0 || n->op1 > 30) {
      return 0;
    }
    *lo_out = alo << n->op1;
    *hi_out = ahi << n->op1;
    break;
  case VLOOP_VN_SAR:
  case VLOOP_VN_SHR:
    if (n->op1 < 0 || n->op1 > 31) {
      return 0;
    }
    *lo_out = alo >> n->op1 < 0 ? alo : alo >> n->op1;
    *hi_out = ahi >> n->op1;
    if (*lo_out > *hi_out) {
      return 0;
    }
    break;
  case VLOOP_VN_ADD:
  case VLOOP_VN_SUB:
  case VLOOP_VN_MUL: {
    if (!b_known) {
      return 0;
    }
    if (n->tag == VLOOP_VN_ADD) {
      *lo_out = alo + blo;
      *hi_out = ahi + bhi;
    } else if (n->tag == VLOOP_VN_SUB) {
      *lo_out = alo - bhi;
      *hi_out = ahi - blo;
    } else {
      long long c[4];
      c[0] = alo * blo;
      c[1] = alo * bhi;
      c[2] = ahi * blo;
      c[3] = ahi * bhi;
      *lo_out = c[0];
      *hi_out = c[0];
      for (int k = 1; k < 4; k++) {
        if (c[k] < *lo_out) *lo_out = c[k];
        if (c[k] > *hi_out) *hi_out = c[k];
      }
    }
    break;
  }
  case VLOOP_VN_OR:
  case VLOOP_VN_XOR: {
    if (!b_known || alo < 0 || blo < 0) {
      return 0;
    }
    {
      long long m = ahi > bhi ? ahi : bhi;
      long long p = 1;
      while (p <= m && p < VLOOP_RANGE_LIMIT) {
        p <<= 1;
      }
      *lo_out = 0;
      *hi_out = p - 1;
    }
    break;
  }
  default:
    return 0;
  }
  if (*lo_out < -VLOOP_RANGE_LIMIT || *hi_out > VLOOP_RANGE_LIMIT) {
    return 0;
  }
  return 1;
}

static int vloop_int_fits_int32(const VLoopDag *d, int node) {
  long long lo = 0, hi = 0;
  if (!vloop_int_node_range(d, node, &lo, &hi, 0)) {
    return 0;
  }
  return lo >= -2147483648LL && hi <= 2147483647LL;
}

static int vloop_int_shift_right_kind(const IRInstruction *ins) {
  return ins->is_unsigned ? VLOOP_VN_SHR : VLOOP_VN_SAR;
}

static int vloop_int_binop_tag(const char *text) {
  if (strcmp(text, "+") == 0) return VLOOP_VN_ADD;
  if (strcmp(text, "-") == 0) return VLOOP_VN_SUB;
  if (strcmp(text, "*") == 0) return VLOOP_VN_MUL;
  if (strcmp(text, "&") == 0) return VLOOP_VN_AND;
  if (strcmp(text, "|") == 0) return VLOOP_VN_OR;
  if (strcmp(text, "^") == 0) return VLOOP_VN_XOR;
  return -1;
}

static int vloop_int_is_comparison(const char *text) {
  return strcmp(text, "<") == 0 || strcmp(text, ">") == 0 ||
         strcmp(text, "<=") == 0 || strcmp(text, ">=") == 0 ||
         strcmp(text, "==") == 0 || strcmp(text, "!=") == 0;
}

static int vloop_compare_value_node(IRFunction *function, size_t before,
                                    const IRInstruction *cmp, const char *iv,
                                    VLoopDag *d) {
  int equality = strcmp(cmp->text, "==") == 0 || strcmp(cmp->text, "!=") == 0;
  int negate = strcmp(cmp->text, "!=") == 0 || strcmp(cmp->text, "<=") == 0 ||
               strcmp(cmp->text, ">=") == 0;
  int gt_left = strcmp(cmp->text, ">") == 0 || strcmp(cmp->text, "<=") == 0;
  const IROperand *first = (equality || gt_left) ? &cmp->lhs : &cmp->rhs;
  const IROperand *second = (equality || gt_left) ? &cmp->rhs : &cmp->lhs;
  int a, b, mask, one, value;

  if (cmp->is_unsigned) {
    return -1;
  }
  a = vloop_build_int(function, before, first, iv, d);
  if (a < 0) {
    return -1;
  }
  b = vloop_build_int(function, before, second, iv, d);
  if (b < 0) {
    return -1;
  }
  mask = vloop_add_node(d, equality ? VLOOP_VN_CMPEQ : VLOOP_VN_CMPGT, a, b);
  if (mask < 0) {
    return -1;
  }
  one = vloop_intern_iconst(d, 1);
  if (one < 0) {
    return -1;
  }
  one = vloop_add_node(d, VLOOP_VN_CONST, one, 0);
  if (one < 0) {
    return -1;
  }
  value = vloop_add_node(d, VLOOP_VN_AND, mask, one);
  if (value < 0 || !negate) {
    return value;
  }
  one = vloop_intern_iconst(d, 1);
  if (one < 0) {
    return -1;
  }
  one = vloop_add_node(d, VLOOP_VN_CONST, one, 0);
  return one < 0 ? -1 : vloop_add_node(d, VLOOP_VN_XOR, value, one);
}

static int vloop_region_is_pure(const IRFunction *function, size_t lo, size_t hi,
                                int depth);

static int vloop_match_diamond(const IRFunction *function, size_t at,
                               size_t body_hi, VLoopDiamond *out) {
  const IRInstruction *br = &function->instructions[at];
  size_t jump = 0, else_label = 0, end_label = 0, probe = 0;
  int nesting = 0;

  if (br->op != IR_OP_BRANCH_ZERO || !br->text ||
      br->lhs.kind != IR_OPERAND_TEMP || !br->lhs.name) {
    return 0;
  }
  for (jump = at + 1; jump < body_hi; jump++) {
    IROpcode op = function->instructions[jump].op;
    if (op == IR_OP_BRANCH_ZERO) {
      nesting++;
      continue;
    }
    if (op == IR_OP_JUMP) {
      if (nesting == 0) {
        break;
      }
      nesting--;
    }
  }
  if (jump >= body_hi || !function->instructions[jump].text) {
    return 0;
  }
  if (!ir_find_next_non_nop(function, jump + 1, &else_label) ||
      else_label >= body_hi) {
    return 0;
  }
  {
    const IRInstruction *el = &function->instructions[else_label];
    if (el->op != IR_OP_LABEL || !el->text || strcmp(el->text, br->text) != 0) {
      return 0;
    }
  }
  end_label = else_label;
  for (probe = else_label + 1; probe < body_hi; probe++) {
    const IRInstruction *ins = &function->instructions[probe];
    if (ins->op == IR_OP_LABEL && ins->text &&
        strcmp(ins->text, function->instructions[jump].text) == 0) {
      end_label = probe;
      break;
    }
  }
  if (end_label == else_label) {
    const IRInstruction *outer = body_hi < function->instruction_count
                                     ? &function->instructions[body_hi]
                                     : NULL;
    if (!outer || outer->op != IR_OP_LABEL || !outer->text ||
        strcmp(outer->text, function->instructions[jump].text) != 0) {
      return 0;
    }
    end_label = body_hi;
  }
  memset(out, 0, sizeof(*out));
  out->branch_index = at;
  out->then_lo = at + 1;
  out->then_hi = jump;
  out->else_lo = else_label + 1;
  out->else_hi = end_label;
  out->end = end_label + 1;
  for (probe = out->else_lo; probe < out->else_hi; probe++) {
    if (function->instructions[probe].op != IR_OP_NOP) {
      out->has_else = 1;
      break;
    }
  }
  return 1;
}

static int vloop_region_is_pure(const IRFunction *function, size_t lo, size_t hi,
                                int depth) {
  if (depth > VLOOP_MAX_DIAMOND_DEPTH) {
    return 0;
  }
  for (size_t i = lo; i < hi;) {
    const IRInstruction *ins = &function->instructions[i];
    VLoopDiamond dm;
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      i++;
      continue;
    }
    if (ins->op == IR_OP_BRANCH_ZERO) {
      if (!vloop_match_diamond(function, i, hi, &dm) ||
          !vloop_region_is_pure(function, dm.then_lo, dm.then_hi, depth + 1) ||
          !vloop_region_is_pure(function, dm.else_lo, dm.else_hi, depth + 1)) {
        return 0;
      }
      i = dm.end;
      continue;
    }
    if (ins->op == IR_OP_STORE || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_LABEL ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_EQ ||
        ins->op == IR_OP_RETURN || ins->op == IR_OP_INLINE_ASM ||
        ins->op == IR_OP_ADDRESS_OF || ins->op == IR_OP_NEW) {
      return 0;
    }
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind != IR_OPERAND_SYMBOL &&
        ins->dest.kind != IR_OPERAND_TEMP) {
      return 0;
    }
    i++;
  }
  return 1;
}

static int vloop_region_writes_escape(IRFunction *function, size_t lo, size_t hi,
                                      size_t after) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name) {
      continue;
    }
    if (ir_symbol_live_after_loop(function, after, ins->dest.name)) {
      return 1;
    }
  }
  return 0;
}

static int vloop_build_int(IRFunction *function, size_t before,
                           const IROperand *op, const char *iv, VLoopDag *d);
static int vloop_resolve_def_int(IRFunction *function, const IRInstruction *def,
                                 size_t def_idx, const char *iv, VLoopDag *d);
static int vloop_resolve_region_int(IRFunction *function, const char *name,
                                    size_t lo, size_t hi, const char *iv,
                                    VLoopDag *d);

static int vloop_instruction_writes_name(const IRInstruction *ins,
                                         const char *name) {
  return ir_instruction_writes_destination(ins) &&
         (ins->dest.kind == IR_OPERAND_SYMBOL ||
          ins->dest.kind == IR_OPERAND_TEMP) &&
         ins->dest.name && strcmp(ins->dest.name, name) == 0;
}

static int vloop_region_assigns(const IRFunction *function, size_t lo, size_t hi,
                                const char *name) {
  for (size_t i = lo; i < hi; i++) {
    if (vloop_instruction_writes_name(&function->instructions[i], name)) {
      return 1;
    }
  }
  return 0;
}

static int vloop_operand_same(const IROperand *a, const IROperand *b) {
  if (!a || !b || a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case IR_OPERAND_INT:
    return a->int_value == b->int_value;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return a->name && b->name && ir_operand_names_match(a, b);
  default:
    return 0;
  }
}

static const IRInstruction *vloop_arm_simple_assign(const IRFunction *function,
                                                    size_t lo, size_t hi,
                                                    const char *name) {
  const IRInstruction *found = NULL;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (!vloop_instruction_writes_name(ins, name)) {
      if (ir_instruction_writes_destination(ins)) {
        return NULL;
      }
      continue;
    }
    if (found || (ins->op != IR_OP_ASSIGN && ins->op != IR_OP_CAST)) {
      return NULL;
    }
    found = ins;
  }
  return found;
}

static int vloop_arm_side(const IRFunction *function, const IRInstruction *cmp,
                          const VLoopDiamond *dm, int is_then,
                          const char *name) {
  size_t lo = is_then ? dm->then_lo : dm->else_lo;
  size_t hi = is_then ? dm->then_hi : dm->else_hi;
  const IRInstruction *as = NULL;
  if (!vloop_region_assigns(function, lo, hi, name)) {
    if (ir_operand_is_symbol_named(&cmp->lhs, name) ||
        ir_operand_is_temp_named(&cmp->lhs, name)) {
      return 0;
    }
    if (ir_operand_is_symbol_named(&cmp->rhs, name) ||
        ir_operand_is_temp_named(&cmp->rhs, name)) {
      return 1;
    }
    return -1;
  }
  as = vloop_arm_simple_assign(function, lo, hi, name);
  if (!as) {
    return -1;
  }
  if (vloop_operand_same(&as->lhs, &cmp->lhs)) {
    return 0;
  }
  if (vloop_operand_same(&as->lhs, &cmp->rhs)) {
    return 1;
  }
  return -1;
}

static int vloop_incoming_node(IRFunction *function, const VLoopDiamond *dm,
                               const char *name, const char *iv, VLoopDag *d) {
  return vloop_resolve_region_int(function, name, d->body_lo, dm->branch_index,
                                  iv, d);
}

static int vloop_arm_value(IRFunction *function, const VLoopDiamond *dm,
                           int is_then, const char *name, const char *iv,
                           VLoopDag *d) {
  size_t lo = is_then ? dm->then_lo : dm->else_lo;
  size_t hi = is_then ? dm->then_hi : dm->else_hi;
  const IRInstruction *as = NULL;
  int result;
  if (!vloop_region_assigns(function, lo, hi, name)) {
    return vloop_incoming_node(function, dm, name, iv, d);
  }
  as = vloop_arm_simple_assign(function, lo, hi, name);
  if (as) {
    return vloop_build_int(function, dm->branch_index, &as->lhs, iv, d);
  }
  if (d->n_regions >= VLOOP_MAX_DIAMOND_DEPTH) {
    return -1;
  }
  d->regions[d->n_regions].lo = lo;
  d->regions[d->n_regions].hi = hi;
  d->regions[d->n_regions].incoming_hi = dm->branch_index;
  d->n_regions++;
  result = vloop_resolve_region_int(function, name, lo, hi, iv, d);
  d->n_regions--;
  return result;
}

static int vloop_diamond_node(IRFunction *function, const VLoopDiamond *dm,
                              const char *name, const char *iv, VLoopDag *d) {
  const IRInstruction *br = &function->instructions[dm->branch_index];
  const IRInstruction *cmp = NULL;
  int then_side, else_side, lt, gt, mask, then_node, else_node, pair;

  if (br->lhs.kind != IR_OPERAND_TEMP || !br->lhs.name) {
    return -1;
  }
  cmp = ir_find_temp_producer_before(function, dm->branch_index, br->lhs.name);
  if (!cmp || cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text ||
      cmp->is_unsigned) {
    return -1;
  }
  lt = strcmp(cmp->text, "<") == 0 || strcmp(cmp->text, "<=") == 0;
  gt = strcmp(cmp->text, ">") == 0 || strcmp(cmp->text, ">=") == 0;
  if (!lt && !gt) {
    return -1;
  }

  then_side = vloop_arm_side(function, cmp, dm, 1, name);
  else_side = vloop_arm_side(function, cmp, dm, 0, name);
  if (then_side >= 0 && else_side >= 0 && then_side != else_side) {
    int keeps_min = (then_side == 0) ? lt : gt;
    int a = vloop_build_int(function, dm->branch_index, &cmp->lhs, iv, d);
    int b = (a < 0) ? -1
                    : vloop_build_int(function, dm->branch_index, &cmp->rhs, iv,
                                      d);
    if (a < 0 || b < 0) {
      return -1;
    }
    return vloop_add_node(d, keeps_min ? VLOOP_VN_MIN : VLOOP_VN_MAX, a, b);
  }

  {
    int strict = strcmp(cmp->text, "<") == 0 || strcmp(cmp->text, ">") == 0;
    int gt_left = gt;
    int ma, mb;
    if (!strict) {
      gt_left = !gt_left;
    }
    ma = vloop_build_int(function, dm->branch_index,
                         gt_left ? &cmp->lhs : &cmp->rhs, iv, d);
    if (ma < 0) {
      return -1;
    }
    mb = vloop_build_int(function, dm->branch_index,
                         gt_left ? &cmp->rhs : &cmp->lhs, iv, d);
    if (mb < 0) {
      return -1;
    }
    mask = vloop_add_node(d, VLOOP_VN_CMPGT, ma, mb);
    if (mask < 0) {
      return -1;
    }
  }
  {
    int swap = strcmp(cmp->text, "<=") == 0 || strcmp(cmp->text, ">=") == 0;
    then_node = vloop_arm_value(function, dm, swap ? 0 : 1, name, iv, d);
    if (then_node < 0) {
      return -1;
    }
    else_node = vloop_arm_value(function, dm, swap ? 1 : 0, name, iv, d);
    if (else_node < 0) {
      return -1;
    }
    pair = vloop_add_node(d, VLOOP_VN_PAIR, then_node, else_node);
    if (pair < 0) {
      return -1;
    }
    return vloop_add_node(d, VLOOP_VN_SELECT, mask, pair);
  }
}

static int vloop_resolve_region_int(IRFunction *function, const char *name,
                                    size_t lo, size_t hi, const char *iv,
                                    VLoopDag *d) {
  VLoopDiamond last_dm;
  const IRInstruction *last_def = NULL;
  size_t last_def_idx = 0;
  int have_dm = 0;
  int result;

  memset(&last_dm, 0, sizeof(last_dm));
  if (!name || d->resolve_depth >= VLOOP_MAX_RESOLVE_DEPTH || d->overflow) {
    return -1;
  }
  for (size_t i = lo; i < hi;) {
    const IRInstruction *ins = &function->instructions[i];
    VLoopDiamond dm;
    if (ins->op == IR_OP_BRANCH_ZERO) {
      if (!vloop_match_diamond(function, i, hi, &dm)) {
        return -1;
      }
      if (vloop_region_assigns(function, dm.then_lo, dm.else_hi, name)) {
        last_dm = dm;
        have_dm = 1;
        last_def = NULL;
      }
      i = dm.end;
      continue;
    }
    if (vloop_instruction_writes_name(ins, name)) {
      last_def = ins;
      last_def_idx = i;
      have_dm = 0;
    }
    i++;
  }
  d->resolve_depth++;
  if (have_dm) {
    result = vloop_diamond_node(function, &last_dm, name, iv, d);
  } else if (last_def) {
    result = vloop_resolve_def_int(function, last_def, last_def_idx, iv, d);
  } else {
    result = -1;
  }
  d->resolve_depth--;
  return result;
}

static int vloop_resolve_def_int(IRFunction *function, const IRInstruction *def,
                                 size_t def_idx, const char *iv, VLoopDag *d) {
  int result = -1;

  if (!def || d->resolve_depth >= VLOOP_MAX_RESOLVE_DEPTH || d->overflow) {
    return -1;
  }
  d->resolve_depth++;
  if (def->op == IR_OP_BINARY && !def->is_float && def->text) {
    int tag = vloop_int_binop_tag(def->text);
    if (vloop_int_is_comparison(def->text)) {
      result = vloop_compare_value_node(function, def_idx, def, iv, d);
    } else if (tag >= 0) {
      int a = vloop_build_int(function, def_idx, &def->lhs, iv, d);
      int b = (a < 0) ? -1 : vloop_build_int(function, def_idx, &def->rhs, iv, d);
      if (a >= 0 && b >= 0) {
        result = vloop_add_node(d, tag, a, b);
      }
    } else if (strcmp(def->text, "<<") == 0 &&
               def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value >= 0 &&
               def->rhs.int_value < 32) {
      int a = vloop_build_int(function, def_idx, &def->lhs, iv, d);
      result = (a < 0) ? -1
                       : vloop_add_node(d, VLOOP_VN_SHL, a,
                                        (int)def->rhs.int_value);
    } else if (strcmp(def->text, ">>") == 0 &&
               def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value >= 0 &&
               def->rhs.int_value < 32) {
      int a = vloop_build_int(function, def_idx, &def->lhs, iv, d);
      result = (a < 0 || !vloop_int_fits_int32(d, a))
                   ? -1
                   : vloop_add_node(d, vloop_int_shift_right_kind(def), a,
                                    (int)def->rhs.int_value);
    }
  } else if (def->op == IR_OP_ASSIGN || def->op == IR_OP_CAST) {
    if (def->op == IR_OP_ASSIGN ||
        (def->text && vloop_int_cast_is_transparent(def->text))) {
      result = vloop_build_int(function, def_idx, &def->lhs, iv, d);
    }
  } else if (def->op == IR_OP_LOAD && def->lhs.kind == IR_OPERAND_TEMP &&
             def->lhs.name && def->rhs.kind == IR_OPERAND_INT) {
    const char *base = NULL;
    int bits = 0;
    int ai = -1;
    if (d->elem_bits == 8 && def->rhs.int_value == 1 &&
        ir_decode_byte_indexed_address(function, def_idx, def->lhs.name, iv,
                                       &base)) {
      int is_unsigned = 1;
      if (d->elem_unsigned < 0) {
        d->elem_unsigned = is_unsigned;
      }
      if (is_unsigned == d->elem_unsigned) {
        ai = vloop_intern_array(d, base);
      }
    } else if (d->elem_bits != 8 && def->rhs.int_value == 4 &&
               ir_decode_float_indexed_address(function, def_idx, def->lhs.name,
                                               iv, &base, &bits) &&
               bits == 32) {
      ai = vloop_intern_array(d, base);
    }
    if (ai >= 0) {
      result = vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
    }
  }
  d->resolve_depth--;
  return result;
}

static int vloop_resolve_body_local_int(IRFunction *function, const char *sym,
                                        const char *iv, size_t read_at,
                                        VLoopDag *d) {
  if (!sym || d->resolve_depth >= VLOOP_MAX_RESOLVE_DEPTH || d->overflow) {
    return -1;
  }
  if (ir_symbol_live_after_loop(function, d->body_hi + 1, sym)) {
    return -1;
  }
  if (read_at > d->body_hi) {
    read_at = d->body_hi;
  }
  for (int k = d->n_regions - 1; k >= 0; k--) {
    size_t stop = read_at < d->regions[k].hi ? read_at : d->regions[k].hi;
    if (stop > d->regions[k].lo) {
      int found = vloop_resolve_region_int(function, sym, d->regions[k].lo, stop,
                                           iv, d);
      if (found >= 0) {
        return found;
      }
    }
    read_at = d->regions[k].incoming_hi;
  }
  return vloop_resolve_region_int(function, sym, d->body_lo, read_at, iv, d);
}

static int vloop_build_int(IRFunction *function, size_t before,
                           const IROperand *op, const char *iv, VLoopDag *d) {
  if (!op || d->overflow) {
    return -1;
  }
  if (op->kind == IR_OPERAND_INT) {
    int ci = vloop_intern_iconst(d, op->int_value);
    return ci < 0 ? -1 : vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return -1;
  }
  if (op->kind == IR_OPERAND_SYMBOL) {
    if (strcmp(op->name, iv) == 0) {
      d->has_iota = 1;
      return vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
    }
    if (vloop_symbol_written_in_body(function, d, op->name)) {
      return vloop_resolve_body_local_int(function, op->name, iv, before, d);
    }
    {
      const char *ty = ir_function_local_declared_type(function, op->name);
      if (!ty) {
        ty = ir_function_param_declared_type(function, op->name);
      }
      if (vloop_int_scalar_type_ok(ty) &&
          !ir_symbol_address_taken(function, op->name)) {
        int si = vloop_intern_scalar(d, op->name);
        return si < 0 ? -1 : vloop_add_node(d, VLOOP_VN_SCALAR, si, 0);
      }
    }
  }
  if (op->kind == IR_OPERAND_TEMP && d->elem_bits == 8) {
    const char *base = NULL;
    int is_unsigned = 0;
    if (ir_decode_byte_indexed_load(function, before, op->name, iv, &base,
                                    &is_unsigned)) {
      if (d->elem_unsigned < 0) {
        d->elem_unsigned = is_unsigned;
      } else if (is_unsigned != d->elem_unsigned) {
        return -1;
      }
      {
        int ai = vloop_intern_array(d, base);
        return ai < 0 ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
      }
    }
    {
      const IRInstruction *from =
          ir_find_temp_producer_before(function, before, op->name);
      if (from && from->op == IR_OP_LOAD) {
        return -1;
      }
    }
  }
  if (op->kind == IR_OPERAND_TEMP) {
    const char *base = NULL;
    int bits = 0;
    if (ir_decode_float_indexed_load(function, before, op->name, iv, &base,
                                     &bits) &&
        bits == 32) {
      int ai = vloop_intern_array(d, base);
      return ai < 0 ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
    }
    {
      int defs = 0;
      for (size_t k = d->body_lo; k < d->body_hi && defs < 2; k++) {
        if (vloop_instruction_writes_name(&function->instructions[k],
                                          op->name)) {
          defs++;
        }
      }
      if (defs > 1) {
        size_t stop = before > d->body_hi ? d->body_hi : before;
        return vloop_resolve_region_int(function, op->name, d->body_lo, stop, iv,
                                        d);
      }
    }
  }
  const IRInstruction *p = ir_i2f_resolve_producer(function, before, op);
  if (!p) {
    return -1;
  }
  size_t pidx = (size_t)(p - function->instructions);
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      vloop_int_cast_is_transparent(p->text)) {
    return vloop_build_int(function, pidx, &p->lhs, iv, d);
  }
  if (p->op == IR_OP_BINARY && !p->is_float && p->text) {
    if (strcmp(p->text, "<<") == 0 && p->rhs.kind == IR_OPERAND_INT &&
        p->rhs.int_value >= 0 && p->rhs.int_value <= 31) {
      int a = vloop_build_int(function, pidx, &p->lhs, iv, d);
      return a < 0 ? -1
                   : vloop_add_node(d, VLOOP_VN_SHL, a, (int)p->rhs.int_value);
    }
    if (strcmp(p->text, ">>") == 0 && p->rhs.kind == IR_OPERAND_INT &&
        p->rhs.int_value >= 0 && p->rhs.int_value <= 31) {
      int a = vloop_build_int(function, pidx, &p->lhs, iv, d);
      if (a < 0 || !vloop_int_fits_int32(d, a)) {
        return -1;
      }
      return vloop_add_node(d, vloop_int_shift_right_kind(p), a,
                            (int)p->rhs.int_value);
    }
    if (vloop_int_is_comparison(p->text)) {
      return vloop_compare_value_node(function, pidx, p, iv, d);
    }
    int tag = vloop_int_binop_tag(p->text);
    if (tag < 0) {
      return -1;
    }
    int a = vloop_build_int(function, pidx, &p->lhs, iv, d);
    if (a < 0) {
      return -1;
    }
    int b = vloop_build_int(function, pidx, &p->rhs, iv, d);
    if (b < 0) {
      return -1;
    }
    return vloop_add_node(d, tag, a, b);
  }
  return -1;
}

static int ir_match_int_map_at(IRFunction *function, size_t header_index,
                               VLoopDag *d, IROperand *bound,
                               size_t *jump_index_out, const char **dst_base_out,
                               int *root_out, int *depth_out, int *claim_out) {
  const char *iv_symbol = NULL;
  const char *dst_base = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t store_index = 0;
  int matched = 0;
  int store_bits = 0;
  int root = -1;
  int depth = 0;
  const IRInstruction *store = NULL;

  *claim_out = 0;
  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_map_body_is_safe_ex(function, branch_index + 1, jump_index,
                                    iv_symbol, &store_index, 1)) {
    return 1;
  }

  store = &function->instructions[store_index];
  if (store->dest.kind != IR_OPERAND_TEMP || !store->dest.name ||
      (store->lhs.kind != IR_OPERAND_TEMP &&
       store->lhs.kind != IR_OPERAND_SYMBOL &&
       store->lhs.kind != IR_OPERAND_INT) ||
      store->rhs.kind != IR_OPERAND_INT) {
    return 1;
  }

  memset(d, 0, sizeof(*d));
  d->width_bits = 32;
  d->is_int = 1;
  d->elem_bits = 32;
  d->body_lo = branch_index + 1;
  d->body_hi = jump_index;

  if (store->rhs.int_value == 1) {
    const IROperand *value = &store->lhs;
    d->elem_bits = 8;
    d->elem_unsigned = -1;
    if (!ir_decode_byte_indexed_address(function, store_index,
                                        store->dest.name, iv_symbol,
                                        &dst_base)) {
      return 1;
    }
    if (value->kind == IR_OPERAND_TEMP && value->name) {
      const IRInstruction *cast =
          ir_find_temp_producer_before(function, store_index, value->name);
      if (cast && cast->op == IR_OP_CAST && cast->text &&
          (strcmp(cast->text, "uint8") == 0 ||
           strcmp(cast->text, "int8") == 0)) {
        value = &cast->lhs;
      }
    }
    root = vloop_build_int(function, store_index, value, iv_symbol, d);
    if (d->elem_unsigned < 0) {
      return 1;
    }
  } else if (store->rhs.int_value == 4 &&
             ir_decode_float_indexed_address(function, store_index,
                                             store->dest.name, iv_symbol,
                                             &dst_base, &store_bits) &&
             store_bits == 32) {
    root = vloop_build_int(function, store_index, &store->lhs, iv_symbol, d);
  } else {
    return 1;
  }
  if (root < 0 || d->overflow) {
    return 1;
  }
  if (d->n_nodes < 2) {
    return 1;
  }

  if (!ir_symbol_is_float_array_base(function, dst_base)) {
    return 1;
  }
  for (int i = 0; i < d->n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d->arrays[i])) {
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    return 1;
  }
  depth = vloop_eval_depth(d, root);
  if (depth > vloop_map_budget(d) ||
      vloop_distinct_bases(d, dst_base) > VLOOP_MAX_ARRAYS) {
    return 1;
  }

  *jump_index_out = jump_index;
  *dst_base_out = dst_base;
  *root_out = root;
  *depth_out = depth;
  *claim_out = 1;
  return 1;
}

int ir_auto_vectorize_int_claimable(IRFunction *function, size_t header_index) {
  VLoopDag d;
  IROperand bound = {0};
  size_t jump_index = 0;
  const char *dst_base = NULL;
  int root = -1;
  int depth = 0;
  int claim = 0;
  if (!ir_match_int_map_at(function, header_index, &d, &bound, &jump_index,
                           &dst_base, &root, &depth, &claim)) {
    return 0;
  }
  ir_operand_destroy(&bound);
  return claim;
}

static int ir_try_vectorize_int_map_at(IRFunction *function,
                                       size_t header_index, int *changed) {
  VLoopDag d;
  IROperand bound = {0};
  size_t jump_index = 0;
  const char *dst_base = NULL;
  int root = -1;
  int depth = 0;
  int claim = 0;
  IRInstruction fused = {0};

  if (!ir_match_int_map_at(function, header_index, &d, &bound, &jump_index,
                           &dst_base, &root, &depth, &claim)) {
    return 0;
  }
  if (!claim) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_I32;
  fused.location = function->instructions[header_index].location;
  fused.float_bits = d.elem_bits;
  fused.is_unsigned = (d.elem_bits == 8) ? (d.elem_unsigned != 0) : 0;
  fused.dest = ir_operand_symbol(dst_base);
  fused.lhs = bound;
  if (!vloop_serialize_into(&fused, &d, 0, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

static int ir_try_vectorize_int_reduce_at(IRFunction *function,
                                          size_t header_index, int *changed) {
  const char *iv_symbol = NULL;
  const char *acc_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  size_t reduce_index = 0;
  IROperand bound = {0};
  int matched = 0;
  int found = 0;
  const IROperand *addend = NULL;
  VLoopDag d;
  int root = -1;
  int depth = 0;
  IRInstruction fused = {0};

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  if (!ir_float_body_is_pure_reduction(function, branch_index + 1,
                                       jump_index)) {
    ir_operand_destroy(&bound);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && strcmp(ins->dest.name, iv_symbol) != 0 &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        (ins->rhs.kind == IR_OPERAND_TEMP ||
         ins->rhs.kind == IR_OPERAND_SYMBOL)) {
      acc_symbol = ins->dest.name;
      addend = &ins->rhs;
      reduce_index = i;
      found++;
    }
  }
  if (found != 1 || !acc_symbol) {
    ir_operand_destroy(&bound);
    return 1;
  }
  {
    const char *acc_type = ir_function_local_declared_type(function, acc_symbol);
    if (!acc_type) {
      acc_type = ir_function_param_declared_type(function, acc_symbol);
    }
    if (!acc_type || (strcmp(acc_type, "int32") != 0 &&
                      strcmp(acc_type, "uint32") != 0)) {
      ir_operand_destroy(&bound);
      return 1;
    }
    fused.is_unsigned = (strcmp(acc_type, "uint32") == 0);
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i != reduce_index && ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        (strcmp(ins->dest.name, acc_symbol) == 0 ||
         strcmp(ins->dest.name, iv_symbol) != 0)) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = 32;
  d.is_int = 1;
  d.elem_bits = 32;
  d.body_lo = branch_index + 1;
  d.body_hi = jump_index;
  root = vloop_build_int(function, reduce_index, addend, iv_symbol, &d);
  if (root < 0 || d.overflow) {
    ir_operand_destroy(&bound);
    return 1;
  }
  if (d.n_nodes < 2) {
    ir_operand_destroy(&bound);
    return 1;
  }
  for (int i = 0; i < d.n_arrays; i++) {
    if (!ir_symbol_is_float_array_base(function, d.arrays[i])) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
    ir_operand_destroy(&bound);
    return 1;
  }
  depth = vloop_eval_depth(&d, root);
  if (depth > VLOOP_REG_BUDGET - 1  ||
      d.n_arrays > VLOOP_MAX_ARRAYS) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_I32;
  fused.location = function->instructions[header_index].location;
  fused.float_bits = 32;
  fused.dest = ir_operand_symbol(acc_symbol);
  fused.lhs = bound;
  if (!vloop_serialize_into(&fused, &d, 1, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

int ir_auto_vectorize_int_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_int_map_at(function, i, changed)) {
        return 0;
      }
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_int_reduce_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

#define VFIND_P_EQ 0
#define VFIND_P_NE 1
#define VFIND_P_LT 2
#define VFIND_P_GT 3
#define VFIND_P_LE 4
#define VFIND_P_GE 5

static int vfind_pred_from_text(const char *text) {
  if (strcmp(text, "==") == 0) return VFIND_P_EQ;
  if (strcmp(text, "!=") == 0) return VFIND_P_NE;
  if (strcmp(text, "<") == 0) return VFIND_P_LT;
  if (strcmp(text, ">") == 0) return VFIND_P_GT;
  if (strcmp(text, "<=") == 0) return VFIND_P_LE;
  if (strcmp(text, ">=") == 0) return VFIND_P_GE;
  return -1;
}

static int vfind_pred_invert(int p) {
  switch (p) {
  case VFIND_P_EQ: return VFIND_P_NE;
  case VFIND_P_NE: return VFIND_P_EQ;
  case VFIND_P_LT: return VFIND_P_GE;
  case VFIND_P_GT: return VFIND_P_LE;
  case VFIND_P_LE: return VFIND_P_GT;
  default: return VFIND_P_LT;
  }
}

static int vfind_pred_mirror(int p) {
  switch (p) {
  case VFIND_P_LT: return VFIND_P_GT;
  case VFIND_P_GT: return VFIND_P_LT;
  case VFIND_P_LE: return VFIND_P_GE;
  case VFIND_P_GE: return VFIND_P_LE;
  default: return p;
  }
}

static int vfind_decode_indexed_load(IRFunction *function, size_t before,
                                     const char *temp, const char *iv,
                                     const char **base_out, int *u8_out,
                                     const IRInstruction **load_out) {
  const IRInstruction *load = NULL;
  const IRInstruction *addr = NULL;

  if (!temp || !iv) {
    return 0;
  }
  load = ir_find_temp_producer_before(function, before, temp);
  if (!load || load->op != IR_OP_LOAD || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name || load->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  addr = ir_find_temp_producer_before(
      function, (size_t)(load - function->instructions), load->lhs.name);
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->lhs.kind != IR_OPERAND_SYMBOL ||
      !addr->lhs.name) {
    return 0;
  }
  if (load->rhs.int_value == 4) {
    const IRInstruction *shl = NULL;
    if (addr->rhs.kind != IR_OPERAND_TEMP || !addr->rhs.name) {
      return 0;
    }
    shl = ir_find_temp_producer_before(
        function, (size_t)(addr - function->instructions), addr->rhs.name);
    if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
        strcmp(shl->text, "<<") != 0 ||
        !ir_operand_is_symbol_named(&shl->lhs, iv) ||
        shl->rhs.kind != IR_OPERAND_INT || shl->rhs.int_value != 2) {
      return 0;
    }
    *u8_out = 0;
  } else if (load->rhs.int_value == 1) {
    if (!ir_operand_is_symbol_named(&addr->rhs, iv)) {
      return 0;
    }
    *u8_out = 1;
  } else {
    return 0;
  }
  *base_out = addr->lhs.name;
  *load_out = load;
  return 1;
}

static int vfind_symbol_written_in(const IRFunction *function, size_t lo,
                                   size_t hi, const char *name) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        strcmp(ins->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

#define VFIND_P_ASCII_IDENT_END 6

static int vfind_same_operand(const IROperand *a, const IROperand *b) {
  if (!a || !b || a->kind != b->kind) return 0;
  if (a->kind == IR_OPERAND_INT) return a->int_value == b->int_value;
  if (a->kind == IR_OPERAND_SYMBOL || a->kind == IR_OPERAND_TEMP) {
    return a->name && b->name && ir_operand_names_match(a, b);
  }
  return 0;
}

static int vfind_temp_from(const IROperand *operand,
                           const IRInstruction *producer) {
  return operand && producer && producer->dest.kind == IR_OPERAND_TEMP &&
         producer->dest.name && operand->kind == IR_OPERAND_TEMP &&
         operand->name && strcmp(operand->name, producer->dest.name) == 0;
}

static int vfind_binary_const(const IRInstruction *ins, const char *op,
                              const IROperand *lhs, long long rhs) {
  return ins && ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
         strcmp(ins->text, op) == 0 && vfind_same_operand(&ins->lhs, lhs) &&
         ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == rhs;
}

static int vfind_label_target(const IRInstruction *branch,
                              const IRInstruction *label) {
  return branch && label && branch->text && label->op == IR_OP_LABEL &&
         label->text && strcmp(branch->text, label->text) == 0;
}

static int vfind_instruction_reads_symbol(const IRInstruction *ins,
                                          const char *name) {
  if (!ins || !name) return 0;
  if (ir_operand_is_symbol_named(&ins->lhs, name) ||
      ir_operand_is_symbol_named(&ins->rhs, name)) {
    return 1;
  }
  for (size_t i = 0; i < ins->argument_count; i++) {
    if (ir_operand_is_symbol_named(&ins->arguments[i], name)) return 1;
  }
  return 0;
}

static int vfind_symbol_is_signed_i32(const IRFunction *function,
                                      const char *name) {
  if (!function || !name) return 0;
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names && function->parameter_names[i] &&
        strcmp(function->parameter_names[i], name) == 0) {
      return function->parameter_types && function->parameter_types[i] &&
             strcmp(function->parameter_types[i], "int32") == 0;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol_named(&ins->dest, name)) {
      return ins->text && strcmp(ins->text, "int32") == 0;
    }
  }
  return 0;
}

static int vfind_operand_fits_signed_i32(const IRFunction *function,
                                         size_t at,
                                         const IROperand *operand) {
  IRValueRangeCtx ranges;
  IRIntRange value;
  ir_value_range_ctx_init(&ranges, function);
  ir_value_range_of(&ranges, at, operand, &value);
  ir_value_range_ctx_destroy(&ranges);
  return value.lo >= INT32_MIN && value.hi <= INT32_MAX;
}

static int ir_try_vectorize_ascii_ident_find_at(IRFunction *function,
                                                 size_t header_index,
                                                 int *changed) {
  size_t cmp_index = 0, branch_index = 0, jump_index = (size_t)-1;
  size_t real[24];
  size_t real_count = 0;
  const IROperand *base = NULL;
  IRInstruction fused = {0};

  if (!function || header_index >= function->instruction_count ||
      function->instructions[header_index].op != IR_OP_LABEL ||
      !ir_label_is_while_header(function->instructions[header_index].text)) {
    return 1;
  }
  if (!ir_find_next_non_nop(function, header_index + 1, &cmp_index) ||
      !ir_find_next_non_nop(function, cmp_index + 1, &branch_index)) {
    return 1;
  }

  IRInstruction *header = &function->instructions[header_index];
  IRInstruction *bound_cmp = &function->instructions[cmp_index];
  IRInstruction *bound_branch = &function->instructions[branch_index];
  if (bound_cmp->op != IR_OP_BINARY || bound_cmp->is_float ||
      !bound_cmp->text || strcmp(bound_cmp->text, "<") != 0 ||
      bound_cmp->lhs.kind != IR_OPERAND_SYMBOL || !bound_cmp->lhs.name ||
      (bound_cmp->rhs.kind != IR_OPERAND_SYMBOL &&
       bound_cmp->rhs.kind != IR_OPERAND_TEMP &&
       bound_cmp->rhs.kind != IR_OPERAND_INT) ||
      bound_branch->op != IR_OP_BRANCH_ZERO || !bound_branch->text ||
      !vfind_temp_from(&bound_branch->lhs, bound_cmp)) {
    return 1;
  }
  const char *iv = bound_cmp->lhs.name;

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_JUMP && ins->text &&
        strcmp(ins->text, header->text) == 0) {
      jump_index = i;
      break;
    }
    if (ins->op == IR_OP_LABEL && ins->text &&
        strcmp(ins->text, bound_branch->text) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      !ir_fused_loop_exit_is_adjacent(function, jump_index,
                                       bound_branch->text)) {
    return 1;
  }
  for (size_t i = branch_index + 1; i <= jump_index; i++) {
    if (function->instructions[i].op == IR_OP_NOP) continue;
    if (real_count >= sizeof(real) / sizeof(real[0])) return 1;
    real[real_count++] = i;
  }
  if (real_count != 21) return 1;

  IRInstruction *addr = &function->instructions[real[0]];
  IRInstruction *load = &function->instructions[real[1]];
  IRInstruction *assign = &function->instructions[real[2]];
  IRInstruction *fold = &function->instructions[real[3]];
  IRInstruction *sub = &function->instructions[real[4]];
  IRInstruction *alpha = &function->instructions[real[5]];
  IRInstruction *alpha_branch = &function->instructions[real[6]];
  IRInstruction *alpha_jump = &function->instructions[real[7]];
  IRInstruction *digit_label = &function->instructions[real[8]];
  IRInstruction *digit_lo = &function->instructions[real[9]];
  IRInstruction *digit_lo_branch = &function->instructions[real[10]];
  IRInstruction *digit_hi = &function->instructions[real[11]];
  IRInstruction *digit_hi_branch = &function->instructions[real[12]];
  IRInstruction *digit_jump = &function->instructions[real[13]];
  IRInstruction *digit_mid_label = &function->instructions[real[14]];
  IRInstruction *underscore_label = &function->instructions[real[15]];
  IRInstruction *underscore = &function->instructions[real[16]];
  IRInstruction *underscore_branch = &function->instructions[real[17]];
  IRInstruction *continue_label = &function->instructions[real[18]];
  IRInstruction *increment = &function->instructions[real[19]];
  IRInstruction *backedge = &function->instructions[real[20]];

  if (addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->dest.kind != IR_OPERAND_TEMP ||
      !addr->dest.name || load->op != IR_OP_LOAD || !load->is_unsigned ||
      !vfind_temp_from(&load->lhs, addr) ||
      load->rhs.kind != IR_OPERAND_INT || load->rhs.int_value != 1 ||
      assign->op != IR_OP_ASSIGN || assign->dest.kind != IR_OPERAND_SYMBOL ||
      !assign->dest.name || !vfind_temp_from(&assign->lhs, load)) {
    return 1;
  }
  if (ir_operand_is_symbol_named(&addr->lhs, iv)) base = &addr->rhs;
  else if (ir_operand_is_symbol_named(&addr->rhs, iv)) base = &addr->lhs;
  if (!base || (base->kind != IR_OPERAND_SYMBOL &&
                base->kind != IR_OPERAND_TEMP) || !base->name ||
      (base->kind == IR_OPERAND_TEMP &&
       !ir_find_temp_producer_before(function, header_index, base->name)) ||
      (bound_cmp->rhs.kind == IR_OPERAND_TEMP &&
       (!bound_cmp->rhs.name ||
        !ir_find_temp_producer_before(function, header_index,
                                      bound_cmp->rhs.name))) ||
      ir_symbol_address_taken(function, assign->dest.name)) {
    return 1;
  }
  if (strcmp(assign->dest.name, iv) == 0 ||
      (base->kind == IR_OPERAND_SYMBOL &&
       (strcmp(base->name, iv) == 0 ||
        strcmp(base->name, assign->dest.name) == 0)) ||
      (bound_cmp->rhs.kind == IR_OPERAND_SYMBOL &&
       (strcmp(bound_cmp->rhs.name, iv) == 0 ||
        ir_operand_names_match(&bound_cmp->rhs, &assign->dest)))) {
    return 1;
  }

  if (!vfind_binary_const(fold, "|", &assign->dest, 32) ||
      !vfind_binary_const(sub, "-", &fold->dest, 97) ||
      !vfind_binary_const(alpha, "<=", &sub->dest, 25) ||
      !alpha->is_unsigned || alpha_branch->op != IR_OP_BRANCH_ZERO ||
      !vfind_temp_from(&alpha_branch->lhs, alpha) ||
      alpha_jump->op != IR_OP_JUMP ||
      !vfind_label_target(alpha_branch, digit_label) ||
      !vfind_label_target(alpha_jump, continue_label) ||
      !vfind_binary_const(digit_lo, ">=", &assign->dest, 48) ||
      digit_lo_branch->op != IR_OP_BRANCH_ZERO ||
      !vfind_temp_from(&digit_lo_branch->lhs, digit_lo) ||
      !vfind_label_target(digit_lo_branch, underscore_label) ||
      !vfind_binary_const(digit_hi, "<=", &assign->dest, 57) ||
      digit_hi_branch->op != IR_OP_BRANCH_ZERO ||
      !vfind_temp_from(&digit_hi_branch->lhs, digit_hi) ||
      !vfind_label_target(digit_hi_branch, digit_mid_label) ||
      digit_jump->op != IR_OP_JUMP ||
      !vfind_label_target(digit_jump, continue_label) ||
      !vfind_binary_const(underscore, "==", &assign->dest, 95) ||
      underscore_branch->op != IR_OP_BRANCH_ZERO ||
      !vfind_temp_from(&underscore_branch->lhs, underscore) ||
      strcmp(underscore_branch->text, bound_branch->text) != 0 ||
      !ir_try_parse_direct_unit_increment(increment, iv) ||
      backedge->op != IR_OP_JUMP || !backedge->text ||
      strcmp(backedge->text, header->text) != 0) {
    return 1;
  }

  if (!vfind_symbol_is_signed_i32(function, iv) ||
      !vfind_operand_fits_signed_i32(function, header_index,
                                     &bound_cmp->rhs)) {
    return 1;
  }

  for (size_t i = jump_index + 1; i < function->instruction_count; i++) {
    if (vfind_instruction_reads_symbol(&function->instructions[i],
                                       assign->dest.name)) {
      return 1;
    }
  }

  for (size_t i = header_index; i-- > 0;) {
    IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) break;
    if (ins->op == IR_OP_SIMD_FIND &&
        ir_operand_is_symbol_named(&ins->dest, iv)) {
      return 1;
    }
  }

  fused.op = IR_OP_SIMD_FIND;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(iv);
  if (!ir_operand_clone(&bound_cmp->rhs, &fused.lhs) ||
      !ir_operand_clone(base, &fused.rhs)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.arguments = calloc(5, sizeof(IROperand));
  if (!fused.arguments) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = 5;
  fused.arguments[0] = ir_operand_int(VFIND_P_ASCII_IDENT_END);
  fused.arguments[1] = ir_operand_int(1);
  fused.arguments[2] = ir_operand_int(0);
  fused.arguments[3] = ir_operand_int(0);
  fused.arguments[4] = ir_operand_symbol(iv);
  if (!ir_function_insert_instruction(function, header_index, &fused)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_instruction_destroy_storage(&fused);
  if (changed) *changed = 1;
  return 1;
}

static int ir_try_vectorize_find_at(IRFunction *function, size_t header_index,
                                    int *changed, int *claimed_out,
                                    int install) {
  const char *iv_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  int matched = 0;
  size_t cb = 0;
  int n_cond = 0;
  size_t exit_lo = 0, exit_hi = 0;
  size_t l_idx = 0;
  int form_b = 0;
  const IRInstruction *br = NULL;
  const IRInstruction *cmp = NULL;
  int pred = -1;
  const char *a_base = NULL;
  int a_u8 = 0;
  const IRInstruction *a_load = NULL;
  const char *b_base = NULL;
  const IRInstruction *b_load = NULL;
  const IROperand *other = NULL;
  int rhs_kind = -1;
  IROperand rhs_arg = {0};
  size_t init_index = (size_t)-1;
  IRInstruction fused = {0};

  if (install &&
      !ir_try_vectorize_ascii_ident_find_at(function, header_index, changed)) {
    return 0;
  }

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_BRANCH_EQ) {
      ir_operand_destroy(&bound);
      return 1;
    }
    if (op == IR_OP_BRANCH_ZERO) {
      cb = i;
      n_cond++;
    }
  }
  if (n_cond != 1) {
    ir_operand_destroy(&bound);
    return 1;
  }
  br = &function->instructions[cb];
  if (br->lhs.kind != IR_OPERAND_TEMP || !br->lhs.name || !br->text ||
      strcmp(br->text, function->instructions[header_index].text) == 0) {
    ir_operand_destroy(&bound);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL) {
      continue;
    }
    if (ins->text && strcmp(ins->text, br->text) == 0 && i > cb && !l_idx) {
      l_idx = i;
      continue;
    }
    ir_operand_destroy(&bound);
    return 1;
  }
  form_b = (l_idx == 0);
  if (!form_b) {
    exit_lo = cb + 1;
    exit_hi = l_idx;
    int saw_term = 0;
    for (size_t i = exit_lo; i < exit_hi; i++) {
      const IRInstruction *e = &function->instructions[i];
      if (e->op == IR_OP_NOP) {
        continue;
      }
      if (saw_term) {
        ir_operand_destroy(&bound);
        return 1;
      }
      if (e->op == IR_OP_RETURN) {
        saw_term = 1;
        continue;
      }
      if (e->op == IR_OP_JUMP) {
        int inside = 0;
        for (size_t k = header_index; k <= jump_index; k++) {
          const IRInstruction *lab = &function->instructions[k];
          if (lab->op == IR_OP_LABEL && lab->text && e->text &&
              strcmp(lab->text, e->text) == 0) {
            inside = 1;
            break;
          }
        }
        if (inside) {
          ir_operand_destroy(&bound);
          return 1;
        }
        saw_term = 1;
        continue;
      }
      if (e->op == IR_OP_BRANCH_ZERO || e->op == IR_OP_BRANCH_EQ ||
          e->op == IR_OP_LABEL) {
        ir_operand_destroy(&bound);
        return 1;
      }
    }
    if (!saw_term) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }

  if (vfind_decode_indexed_load(function, cb, br->lhs.name, iv_symbol, &a_base,
                                &a_u8, &a_load)) {
    pred = VFIND_P_NE;
    other = NULL;
  } else {
    cmp = ir_find_temp_producer_before(function, cb, br->lhs.name);
    if (!cmp || cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text) {
      ir_operand_destroy(&bound);
      return 1;
    }
    pred = vfind_pred_from_text(cmp->text);
    if (pred < 0) {
      ir_operand_destroy(&bound);
      return 1;
    }
    if (ir_operand_is_temp(&cmp->lhs) &&
        vfind_decode_indexed_load(function, cb, cmp->lhs.name, iv_symbol,
                                  &a_base, &a_u8, &a_load)) {
      other = &cmp->rhs;
    } else if (ir_operand_is_temp(&cmp->rhs) &&
               vfind_decode_indexed_load(function, cb, cmp->rhs.name,
                                         iv_symbol, &a_base, &a_u8, &a_load)) {
      other = &cmp->lhs;
      pred = vfind_pred_mirror(pred);
    } else {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if ((size_t)(a_load - function->instructions) <= branch_index) {
    ir_operand_destroy(&bound);
    return 1;
  }

  if (!other) {
    rhs_kind = 0;
    rhs_arg = ir_operand_int(0);
  } else if (other->kind == IR_OPERAND_INT) {
    long long v = other->int_value;
    int ok = a_u8 ? (v >= 0 && v <= 255)
                  : (a_load->is_unsigned ? (v >= 0 && v <= 4294967295LL)
                                         : (v >= -2147483648LL &&
                                            v <= 2147483647LL));
    if (!ok) {
      ir_operand_destroy(&bound);
      return 1;
    }
    rhs_kind = 0;
    rhs_arg = ir_operand_int(v);
  } else if (other->kind == IR_OPERAND_TEMP && other->name) {
    int b_u8 = 0;
    if (!vfind_decode_indexed_load(function, cb, other->name, iv_symbol,
                                   &b_base, &b_u8, &b_load) ||
        b_u8 != a_u8 ||
        (size_t)(b_load - function->instructions) <= branch_index ||
        a_load->is_unsigned != b_load->is_unsigned ||
        !ir_symbol_is_float_array_base(function, b_base)) {
      ir_operand_destroy(&bound);
      return 1;
    }
    rhs_kind = 2;
    rhs_arg = ir_operand_symbol(b_base);
  } else if (other->kind == IR_OPERAND_SYMBOL && other->name) {
    const char *ty = ir_function_local_declared_type(function, other->name);
    if (!ty) {
      ty = ir_function_param_declared_type(function, other->name);
    }
    int ok = 0;
    if (a_u8) {
      ok = ty && (strcmp(ty, "int8") == 0 || strcmp(ty, "uint8") == 0);
    } else if (a_load->is_unsigned) {
      ok = ty && strcmp(ty, "uint32") == 0;
    } else {
      ok = ty && strcmp(ty, "int32") == 0;
    }
    if (!ok || strcmp(other->name, iv_symbol) == 0 ||
        ir_symbol_address_taken(function, other->name) ||
        vfind_symbol_written_in(function, branch_index + 1, jump_index,
                                other->name)) {
      ir_operand_destroy(&bound);
      return 1;
    }
    rhs_kind = 1;
    if (!ir_operand_clone(other, &rhs_arg)) {
      ir_operand_destroy(&bound);
      return 0;
    }
  } else {
    ir_operand_destroy(&bound);
    return 1;
  }

  if (pred != VFIND_P_EQ && pred != VFIND_P_NE &&
      (a_u8 || a_load->is_unsigned ||
       (rhs_kind == 2 && b_load->is_unsigned))) {
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  if (!ir_symbol_is_float_array_base(function, a_base)) {
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!form_b && i >= exit_lo && i < exit_hi) {
      continue;
    }
    if (ins->op == IR_OP_NOP || i == cb || (!form_b && i == l_idx)) {
      continue;
    }
    if (ins == a_load || (b_load && ins == b_load)) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        ins->dest.kind == IR_OPERAND_TEMP &&
        (strcmp(ins->text, "+") == 0 || strcmp(ins->text, "<<") == 0 ||
         ins == cmp)) {
      continue;
    }
    if (ins->op == IR_OP_ASSIGN && ins->dest.kind == IR_OPERAND_TEMP) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 &&
        ir_operand_is_symbol_named(&ins->dest, iv_symbol) &&
        ir_operand_is_symbol_named(&ins->lhs, iv_symbol) &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == 1) {
      continue;
    }
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  if (form_b) {
    pred = vfind_pred_invert(pred);
  }

  for (size_t i = header_index; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      break;
    }
    if (ins->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      init_index = i;
      break;
    }
  }
  if (init_index == (size_t)-1) {
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  {
    const char *reads[4];
    size_t read_count = 0;
    reads[read_count++] = a_base;
    if (b_base) {
      reads[read_count++] = b_base;
    }
    if (rhs_arg.kind == IR_OPERAND_SYMBOL && rhs_arg.name) {
      reads[read_count++] = rhs_arg.name;
    }
    if (bound.kind == IR_OPERAND_SYMBOL && bound.name) {
      reads[read_count++] = bound.name;
    }
    for (size_t r = 0; r < read_count; r++) {
      if (reads[r] && vfind_symbol_written_in(function, init_index + 1,
                                              header_index, reads[r])) {
        ir_operand_destroy(&bound);
        ir_operand_destroy(&rhs_arg);
        return 1;
      }
    }
  }

  if (claimed_out) {
    *claimed_out = 1;
  }
  if (!install) {
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  fused.op = IR_OP_SIMD_FIND;
  fused.location = function->instructions[header_index].location;
  fused.dest = ir_operand_symbol(iv_symbol);
  fused.lhs = bound;
  fused.rhs = ir_operand_symbol(a_base);
  fused.arguments = calloc(4, sizeof(IROperand));
  if (!fused.arguments) {
    ir_instruction_destroy_storage(&fused);
    ir_operand_destroy(&rhs_arg);
    return 0;
  }
  fused.argument_count = 4;
  fused.arguments[0] = ir_operand_int(pred);
  fused.arguments[1] = ir_operand_int(a_u8);
  fused.arguments[2] = ir_operand_int(rhs_kind);
  fused.arguments[3] = rhs_arg;

  ir_instruction_destroy_storage(&function->instructions[init_index]);
  function->instructions[init_index] = fused;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_auto_vectorize_find_claimable(IRFunction *function, size_t header_index) {
  int claimed = 0;
  if (!ir_try_vectorize_find_at(function, header_index, NULL, &claimed, 0)) {
    return 0;
  }
  return claimed;
}

int ir_auto_vectorize_find_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_find_at(function, i, changed, NULL, 1)) {
        return 0;
      }
    }
  }
  return 1;
}

#define OL_U_AND 1
#define OL_U_OR 2
#define OL_U_XOR 3
#define OL_U_ADD 4
#define OL_U_SUB 5
#define OL_U_MUL 6
#define OL_U_SHL 7
#define OL_U_SHR 8
#define OL_U_CVT 9
#define OL_U_FADD 10
#define OL_U_FSUB 11
#define OL_U_FMUL 12
#define OL_U_FDIV 13
#define OL_C_ADD 0
#define OL_C_SUB 1
#define OL_C_MUL 2
#define OL_C_DIV 3

#define OL_MAX_CHAIN 8
#define OL_MAX_UNIF 8
#define OL_MAX_MICRO 16
#define OL_MAX_FCONST 16

typedef struct {
  int op;
  long long imm;
} OlMicro;
typedef struct {
  OlMicro micro[OL_MAX_MICRO];
  int n_micro;
} OlUniform;
typedef struct {
  int op;
  int side;
  int term_kind;
  int term_idx;
} OlChainStep;
typedef struct {
  OlChainStep chain[OL_MAX_CHAIN];
  int n_chain;
  OlUniform unif[OL_MAX_UNIF];
  int n_unif;
  double fconst[OL_MAX_FCONST];
  int n_fconst;
  int init_mode;
  double iacc_init;
  OlUniform init_prog;
  int overflow;
} OlDag;

static int ol_intern_fconst(OlDag *d, double v) {
  for (int i = 0; i < d->n_fconst; i++) {
    if (memcmp(&d->fconst[i], &v, sizeof(double)) == 0) {
      return i;
    }
  }
  if (d->n_fconst >= OL_MAX_FCONST) {
    d->overflow = 1;
    return -1;
  }
  d->fconst[d->n_fconst] = v;
  return d->n_fconst++;
}

static int ol_operand_is_fconst(IRFunction *fn, size_t before,
                                const IROperand *op, double *out) {
  if (op->kind == IR_OPERAND_FLOAT && op->float_bits == 64) {
    *out = op->float_value;
    return 1;
  }
  if (op->kind == IR_OPERAND_TEMP && op->name) {
    const IRInstruction *p = ir_find_temp_producer_before(fn, before, op->name);
    if (p && p->op == IR_OP_CAST && p->text && strcmp(p->text, "float64") == 0) {
      if (p->lhs.kind == IR_OPERAND_FLOAT) { *out = p->lhs.float_value; return 1; }
      if (p->lhs.kind == IR_OPERAND_INT) { *out = (double)p->lhs.int_value; return 1; }
    }
  }
  return 0;
}

static int ol_contains_symbol(IRFunction *fn, size_t before, const IROperand *op,
                              const char *sym, int depth) {
  if (!op || depth > 24) {
    return 0;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name && sym &&
      strcmp(op->name, sym) == 0) {
    return 1;
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  const IRInstruction *p = ir_i2f_resolve_producer(fn, before, op);
  if (!p) {
    return 0;
  }
  size_t pidx = (size_t)(p - fn->instructions);
  if (p->op == IR_OP_BINARY || p->op == IR_OP_CAST) {
    if (ol_contains_symbol(fn, pidx, &p->lhs, sym, depth + 1)) return 1;
    if (p->op == IR_OP_BINARY &&
        ol_contains_symbol(fn, pidx, &p->rhs, sym, depth + 1))
      return 1;
  }
  return 0;
}

static int ol_build_uniform(IRFunction *fn, size_t before, const IROperand *op,
                            const char *iv, OlDag *d, OlUniform *prog) {
  if (!op || d->overflow) {
    return 0;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name && strcmp(op->name, iv) == 0) {
    return 1;
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  const IRInstruction *p = ir_i2f_resolve_producer(fn, before, op);
  if (!p) {
    return 0;
  }
  size_t pidx = (size_t)(p - fn->instructions);
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      strcmp(p->text, "float64") == 0) {
    if (!ol_build_uniform(fn, pidx, &p->lhs, iv, d, prog)) return 0;
    if (prog->n_micro >= OL_MAX_MICRO) { d->overflow = 1; return 0; }
    prog->micro[prog->n_micro].op = OL_U_CVT;
    prog->micro[prog->n_micro].imm = 0;
    prog->n_micro++;
    return 1;
  }
  if (p->op != IR_OP_BINARY || !p->text) {
    return 0;
  }
  const IROperand *L = &p->lhs;
  const IROperand *R = &p->rhs;
  int l_has = ol_contains_symbol(fn, pidx, L, iv, 0);
  int r_has = ol_contains_symbol(fn, pidx, R, iv, 0);
  const IROperand *inner = NULL;
  const IROperand *cst = NULL;
  int cst_on_right = 1;
  if (l_has && !r_has) { inner = L; cst = R; cst_on_right = 1; }
  else if (r_has && !l_has) { inner = R; cst = L; cst_on_right = 0; }
  else { return 0; }

  if (p->is_float) {
    double cv = 0.0;
    if (!ol_operand_is_fconst(fn, pidx, cst, &cv)) return 0;
    int op_code;
    if (strcmp(p->text, "+") == 0) { op_code = OL_U_FADD; }
    else if (strcmp(p->text, "*") == 0) { op_code = OL_U_FMUL; }
    else if (strcmp(p->text, "-") == 0) {
      if (!cst_on_right) return 0;
      op_code = OL_U_FSUB;
    } else if (strcmp(p->text, "/") == 0) {
      if (!cst_on_right) return 0;
      op_code = OL_U_FDIV;
    } else { return 0; }
    int ci = ol_intern_fconst(d, cv);
    if (ci < 0) return 0;
    if (!ol_build_uniform(fn, pidx, inner, iv, d, prog)) return 0;
    if (prog->n_micro >= OL_MAX_MICRO) { d->overflow = 1; return 0; }
    prog->micro[prog->n_micro].op = op_code;
    prog->micro[prog->n_micro].imm = ci;
    prog->n_micro++;
    return 1;
  }
  if (cst->kind != IR_OPERAND_INT) return 0;
  long long imm = cst->int_value;
  int op_code;
  if (strcmp(p->text, "&") == 0) { op_code = OL_U_AND; }
  else if (strcmp(p->text, "|") == 0) { op_code = OL_U_OR; }
  else if (strcmp(p->text, "^") == 0) { op_code = OL_U_XOR; }
  else if (strcmp(p->text, "+") == 0) { op_code = OL_U_ADD; }
  else if (strcmp(p->text, "*") == 0) { op_code = OL_U_MUL; }
  else if (strcmp(p->text, "-") == 0) {
    if (!cst_on_right) return 0;
    op_code = OL_U_SUB;
  } else if (strcmp(p->text, "<<") == 0) {
    if (!cst_on_right) return 0;
    op_code = OL_U_SHL;
  } else if (strcmp(p->text, ">>") == 0) {
    if (!cst_on_right) return 0;
    op_code = OL_U_SHR;
  } else { return 0; }
  if (!ol_build_uniform(fn, pidx, inner, iv, d, prog)) return 0;
  if (prog->n_micro >= OL_MAX_MICRO) { d->overflow = 1; return 0; }
  prog->micro[prog->n_micro].op = op_code;
  prog->micro[prog->n_micro].imm = imm;
  prog->n_micro++;
  return 1;
}

static int ol_extract_term(IRFunction *fn, size_t before, const IROperand *op,
                           const char *iv, OlDag *d, int *kind, int *idx) {
  double cv = 0.0;
  if (ol_operand_is_fconst(fn, before, op, &cv)) {
    int ci = ol_intern_fconst(d, cv);
    if (ci < 0) return 0;
    *kind = 0;
    *idx = ci;
    return 1;
  }
  if (d->n_unif >= OL_MAX_UNIF) { d->overflow = 1; return 0; }
  OlUniform *prog = &d->unif[d->n_unif];
  prog->n_micro = 0;
  if (!ol_build_uniform(fn, before, op, iv, d, prog)) return 0;
  *kind = 1;
  *idx = d->n_unif;
  d->n_unif++;
  return 1;
}

static int ol_build_chain(IRFunction *fn, size_t before, const IROperand *op,
                          const char *iacc, const char *iv, OlDag *d) {
  if (!op || d->overflow) {
    return 0;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name && strcmp(op->name, iacc) == 0) {
    return 1;
  }
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  const IRInstruction *p = ir_i2f_resolve_producer(fn, before, op);
  if (!p || p->op != IR_OP_BINARY || !p->is_float || !p->text) {
    return 0;
  }
  size_t pidx = (size_t)(p - fn->instructions);
  int l_has = ol_contains_symbol(fn, pidx, &p->lhs, iacc, 0);
  int r_has = ol_contains_symbol(fn, pidx, &p->rhs, iacc, 0);
  const IROperand *inner = NULL;
  const IROperand *term = NULL;
  int side;
  if (l_has && !r_has) { inner = &p->lhs; term = &p->rhs; side = 0; }
  else if (r_has && !l_has) { inner = &p->rhs; term = &p->lhs; side = 1; }
  else { return 0; }

  int op_code;
  if (strcmp(p->text, "+") == 0) { op_code = OL_C_ADD; }
  else if (strcmp(p->text, "-") == 0) { op_code = OL_C_SUB; }
  else if (strcmp(p->text, "*") == 0) { op_code = OL_C_MUL; }
  else if (strcmp(p->text, "/") == 0) { op_code = OL_C_DIV; }
  else { return 0; }

  int kind = 0, idx = 0;
  if (!ol_extract_term(fn, pidx, term, iv, d, &kind, &idx)) {
    return 0;
  }
  if (!ol_build_chain(fn, pidx, inner, iacc, iv, d)) {
    return 0;
  }
  if (d->n_chain >= OL_MAX_CHAIN) { d->overflow = 1; return 0; }
  d->chain[d->n_chain].op = op_code;
  d->chain[d->n_chain].side = side;
  d->chain[d->n_chain].term_kind = kind;
  d->chain[d->n_chain].term_idx = idx;
  d->n_chain++;
  return 1;
}

static int ol_inner_body_pure(IRFunction *fn, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    switch (fn->instructions[i].op) {
    case IR_OP_STORE:
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_LABEL:
    case IR_OP_INLINE_ASM:
    case IR_OP_MEMCPY_INLINE:
    case IR_OP_NEW:
    case IR_OP_ADDRESS_OF:
    case IR_OP_RETURN:
    case IR_OP_LOAD:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

static long long ol_find_branch_zero(IRFunction *fn, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    IROpcode op = fn->instructions[i].op;
    if (op == IR_OP_BRANCH_ZERO) return (long long)i;
    if (op == IR_OP_JUMP) return -1;
  }
  return -1;
}

static long long ol_find_jump_to(IRFunction *fn, size_t lo, size_t hi,
                                 const char *label) {
  for (size_t i = lo; i < hi; i++) {
    if (fn->instructions[i].op == IR_OP_JUMP && fn->instructions[i].text &&
        label && strcmp(fn->instructions[i].text, label) == 0) {
      return (long long)i;
    }
  }
  return -1;
}

static int ol_decode_loop_compare(IRFunction *fn, size_t branch_index,
                                  const char **iv_out, IROperand *bound_out,
                                  int *cmp_out) {
  const IRInstruction *br = &fn->instructions[branch_index];
  if (br->op != IR_OP_BRANCH_ZERO || br->lhs.kind != IR_OPERAND_TEMP ||
      !br->lhs.name) {
    return 0;
  }
  const IRInstruction *c =
      ir_find_temp_producer_before(fn, branch_index, br->lhs.name);
  if (!c || c->op != IR_OP_BINARY || c->is_float || !c->text ||
      c->lhs.kind != IR_OPERAND_SYMBOL || !c->lhs.name) {
    return 0;
  }
  if (strcmp(c->text, "<") == 0) { *cmp_out = 0; }
  else if (strcmp(c->text, "<=") == 0) { *cmp_out = 1; }
  else { return 0; }
  if (c->rhs.kind != IR_OPERAND_SYMBOL && c->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  *iv_out = c->lhs.name;
  return ir_operand_clone(&c->rhs, bound_out);
}

static int ir_try_vectorize_outer_lane_at(IRFunction *function,
                                           size_t header_index, int *changed) {
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  const char *outer_label = header->text;
  size_t n = function->instruction_count;
#define OL_DBG(msg) ((void)0)

  long long ob = ol_find_branch_zero(function, header_index + 1, n);
  if (ob < 0) {
    OL_DBG("no outer branch_zero");
    return 1;
  }
  size_t outer_branch = (size_t)ob;
  const char *p_sym = NULL;
  IROperand outerP = {0};
  int outer_cmp = 0;
  if (!ol_decode_loop_compare(function, outer_branch, &p_sym, &outerP,
                              &outer_cmp)) {
    OL_DBG("outer compare decode failed");
    return 1;
  }
  if (outer_cmp != 0) {
    OL_DBG("outer compare is not '<'");
    ir_operand_destroy_detached(&outerP);
    return 1;
  }
  long long oj = ol_find_jump_to(function, outer_branch + 1, n, outer_label);
  if (oj < 0) {
    OL_DBG("no outer back-jump");
    ir_operand_destroy_detached(&outerP);
    return 1;
  }
  size_t outer_jump = (size_t)oj;

  long long inner_hdr = -1;
  for (size_t i = outer_branch + 1; i < outer_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text &&
        ir_label_is_while_header(ins->text) &&
        !strstr(ins->text, "while_end")) {
      if (inner_hdr >= 0) {
        OL_DBG(">1 inner while header");
        ir_operand_destroy_detached(&outerP);
        return 1;
      }
      inner_hdr = (long long)i;
    }
  }
  if (inner_hdr < 0) {
    OL_DBG("no inner while header");
    ir_operand_destroy_detached(&outerP);
    return 1;
  }
  size_t inner_header = (size_t)inner_hdr;
  const char *inner_label = function->instructions[inner_header].text;

  long long ib = ol_find_branch_zero(function, inner_header + 1, outer_jump);
  if (ib < 0) { OL_DBG("no inner branch_zero"); ir_operand_destroy_detached(&outerP); return 1; }
  size_t inner_branch = (size_t)ib;
  const char *i_sym = NULL;
  IROperand innerN = {0};
  int inner_cmp = 0;
  if (!ol_decode_loop_compare(function, inner_branch, &i_sym, &innerN,
                              &inner_cmp)) {
    OL_DBG("inner compare decode failed");
    ir_operand_destroy_detached(&outerP);
    return 1;
  }
  long long ij = ol_find_jump_to(function, inner_branch + 1, outer_jump,
                                 inner_label);
  if (ij < 0) { OL_DBG("no inner back-jump"); ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1; }
  size_t inner_jump = (size_t)ij;

  {
    size_t inc = inner_jump;
    while (inc > inner_branch + 1) {
      inc--;
      if (function->instructions[inc].op != IR_OP_NOP) break;
    }
    if (!ir_try_parse_direct_unit_increment(&function->instructions[inc],
                                            i_sym)) {
      OL_DBG("inner increment not unit");
      ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
    }
  }
  {
    size_t inc = outer_jump;
    while (inc > inner_jump) {
      inc--;
      if (function->instructions[inc].op != IR_OP_NOP) break;
    }
    if (!ir_try_parse_direct_unit_increment(&function->instructions[inc],
                                            p_sym)) {
      OL_DBG("outer increment not unit");
      ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
    }
  }
  if (ir_loop_body_is_unclaimable(function, inner_branch + 1, inner_jump)) {
    OL_DBG("inner body has nested while");
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
  }

  if (!ir_iv_zero_at_header(function, header_index, p_sym)) {
    OL_DBG("outer iv does not start at 0");
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
  }
  for (size_t i = outer_branch + 1; i < inner_header; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
        op == IR_OP_BRANCH_EQ) {
      OL_DBG("control flow in outer init region");
      ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
    }
  }

  const char *total_sym = NULL;
  const char *iacc_sym = NULL;
  for (size_t i = inner_jump + 1; i < outer_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ir_operand_is_symbol(&ins->rhs)) {
      total_sym = ins->dest.name;
      iacc_sym = ins->rhs.name;
      break;
    }
  }
  if (!total_sym || !iacc_sym || strcmp(total_sym, iacc_sym) == 0) {
    OL_DBG("no outer reduction total+=iacc");
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
  }
  if (!ir_float_sum_type_matches(
          ir_function_local_declared_type(function, total_sym), 64) ||
      !ir_float_sum_type_matches(
          ir_function_local_declared_type(function, iacc_sym), 64)) {
    OL_DBG("total/iacc not float64");
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
  }

  long long i0 = 0;
  int found_i0 = 0;
  for (size_t i = outer_branch + 1; i < inner_header; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, i_sym)) {
      if (ins->op == IR_OP_ASSIGN && ins->lhs.kind == IR_OPERAND_INT) {
        i0 = ins->lhs.int_value;
        found_i0 = 1;
      } else {
        found_i0 = 0;
      }
    }
  }
  if (!found_i0) {
    OL_DBG("no inner i0");
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
  }

  OlDag d;
  memset(&d, 0, sizeof(d));
  {
    size_t init_idx = 0;
    if (!ir_find_last_writer_before(function, inner_header, IR_OPERAND_SYMBOL,
                                    iacc_sym, &init_idx) ||
        init_idx <= outer_branch) {
      OL_DBG("no iacc init writer in the outer init region");
      ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
    }
    const IRInstruction *init_ins = &function->instructions[init_idx];
    if (init_ins->op == IR_OP_ASSIGN && init_ins->lhs.kind == IR_OPERAND_FLOAT) {
      d.init_mode = 0;
      d.iacc_init = init_ins->lhs.float_value;
    } else {
      const IROperand *seed_op = NULL;
      IROperand iacc_op = ir_operand_symbol(iacc_sym);
      if (init_ins->op == IR_OP_ASSIGN) {
        seed_op = &init_ins->lhs;
      } else {
        seed_op = &iacc_op;
      }
      d.init_prog.n_micro = 0;
      if (!ol_build_uniform(function, inner_header, seed_op, p_sym, &d,
                            &d.init_prog) ||
          d.overflow) {
        OL_DBG("iacc seed neither const nor uniform-of-p");
        ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
      }
      d.init_mode = 1;
    }
  }

  long long iacc_upd = -1;
  for (size_t i = inner_branch + 1; i < inner_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float &&
        ir_operand_is_symbol_named(&ins->dest, iacc_sym)) {
      if (iacc_upd >= 0) { iacc_upd = -2; break; }
      iacc_upd = (long long)i;
    } else if (ir_operand_is_symbol_named(&ins->dest, iacc_sym)) {
      iacc_upd = -2;
      break;
    }
  }
  if (iacc_upd < 0) {
    OL_DBG("no single iacc recurrence update");
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
  }

  if (!ol_inner_body_pure(function, inner_branch + 1, inner_jump)) {
    OL_DBG("inner body not pure");
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
  }

  {
    const IRInstruction *upd = &function->instructions[iacc_upd];
    int l_has = ol_contains_symbol(function, (size_t)iacc_upd, &upd->lhs,
                                   iacc_sym, 0);
    int r_has = ol_contains_symbol(function, (size_t)iacc_upd, &upd->rhs,
                                   iacc_sym, 0);
    const IROperand *inner_op = NULL;
    const IROperand *term_op = NULL;
    int side;
    if (l_has && !r_has) { inner_op = &upd->lhs; term_op = &upd->rhs; side = 0; }
    else if (r_has && !l_has) { inner_op = &upd->rhs; term_op = &upd->lhs; side = 1; }
    else { OL_DBG("update: both/neither operand carries iacc"); ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1; }
    int op_code;
    if (strcmp(upd->text, "+") == 0) op_code = OL_C_ADD;
    else if (strcmp(upd->text, "-") == 0) op_code = OL_C_SUB;
    else if (strcmp(upd->text, "*") == 0) op_code = OL_C_MUL;
    else if (strcmp(upd->text, "/") == 0) op_code = OL_C_DIV;
    else { OL_DBG("update: top op not +-*/"); ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1; }
    int kind = 0, idx = 0;
    if (!ol_extract_term(function, (size_t)iacc_upd, term_op, i_sym, &d, &kind,
                         &idx) ||
        !ol_build_chain(function, (size_t)iacc_upd, inner_op, iacc_sym, i_sym,
                        &d)) {
      OL_DBG("chain/term build failed");
      ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
    }
    if (d.n_chain >= OL_MAX_CHAIN) { ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1; }
    d.chain[d.n_chain].op = op_code;
    d.chain[d.n_chain].side = side;
    d.chain[d.n_chain].term_kind = kind;
    d.chain[d.n_chain].term_idx = idx;
    d.n_chain++;
  }
  if (d.overflow || d.n_chain == 0) {
    OL_DBG("dag overflow or empty chain");
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
  }

  for (size_t i = inner_header; i <= inner_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *ops[3] = {&ins->lhs, &ins->rhs, &ins->dest};
    for (int k = 0; k < 3; k++) {
      if (ops[k]->kind == IR_OPERAND_SYMBOL && ops[k]->name &&
          strcmp(ops[k]->name, p_sym) == 0) {
        OL_DBG("inner loop references p (not p-invariant)");
        ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
      }
    }
  }
  for (size_t i = outer_branch + 1; i <= inner_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *ops[3] = {&ins->lhs, &ins->rhs, &ins->dest};
    for (int k = 0; k < 3; k++) {
      if (ops[k]->kind == IR_OPERAND_SYMBOL && ops[k]->name &&
          strcmp(ops[k]->name, total_sym) == 0) {
        OL_DBG("total referenced in inner region");
        ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 1;
      }
    }
  }

  IRInstruction fused = {0};
  size_t argc = 8 + (size_t)(4 * d.n_chain);
  for (int u = 0; u < d.n_unif; u++) {
    argc += 1 + (size_t)(2 * d.unif[u].n_micro);
  }
  if (d.init_mode == 1) {
    argc += 1 + (size_t)(2 * d.init_prog.n_micro);
  }
  argc += (size_t)d.n_fconst;
  fused.arguments = calloc(argc, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy_detached(&outerP); ir_operand_destroy_detached(&innerN); return 0;
  }
  fused.argument_count = argc;
  size_t k = 0;
  fused.arguments[k++] = ir_operand_int(inner_cmp);
  fused.arguments[k++] = ir_operand_int(1);
  fused.arguments[k++] = ir_operand_int(d.n_chain);
  fused.arguments[k++] = ir_operand_int(d.n_unif);
  fused.arguments[k++] = ir_operand_int(d.n_fconst);
  fused.arguments[k++] = ir_operand_int(i0);
  fused.arguments[k++] = ir_operand_int(d.init_mode);
  fused.arguments[k++] = ir_operand_float_sized(d.iacc_init, 64);
  for (int s = 0; s < d.n_chain; s++) {
    fused.arguments[k++] = ir_operand_int(d.chain[s].op);
    fused.arguments[k++] = ir_operand_int(d.chain[s].side);
    fused.arguments[k++] = ir_operand_int(d.chain[s].term_kind);
    fused.arguments[k++] = ir_operand_int(d.chain[s].term_idx);
  }
  for (int u = 0; u < d.n_unif; u++) {
    fused.arguments[k++] = ir_operand_int(d.unif[u].n_micro);
    for (int m = 0; m < d.unif[u].n_micro; m++) {
      fused.arguments[k++] = ir_operand_int(d.unif[u].micro[m].op);
      fused.arguments[k++] = ir_operand_int(d.unif[u].micro[m].imm);
    }
  }
  if (d.init_mode == 1) {
    fused.arguments[k++] = ir_operand_int(d.init_prog.n_micro);
    for (int m = 0; m < d.init_prog.n_micro; m++) {
      fused.arguments[k++] = ir_operand_int(d.init_prog.micro[m].op);
      fused.arguments[k++] = ir_operand_int(d.init_prog.micro[m].imm);
    }
  }
  for (int c = 0; c < d.n_fconst; c++) {
    fused.arguments[k++] = ir_operand_float_sized(d.fconst[c], 64);
  }
  fused.op = IR_OP_SIMD_OUTER_LANE_F64;
  fused.location = header->location;
  fused.is_float = 1;
  fused.float_bits = 64;
  fused.dest = ir_operand_symbol(total_sym);
  fused.lhs = outerP;
  fused.rhs = innerN;
  OL_DBG("INSTALLED outer-lane fusion");
  ir_install_fused_reduction(function, header_index, outer_jump, &fused,
                             changed);
  return 1;
}

int ir_outer_vectorize_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_outer_lane_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}
