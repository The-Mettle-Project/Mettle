#include "ir_optimize_internal.h"

/* -------------------------------------------------------------------------- */
/* float64/float32 horizontal sum -> IR_OP_SIMD_SUM_F64/F32                    */
/* float64/float32 dot product   -> IR_OP_SIMD_DOT_F64/F32                     */
/* -------------------------------------------------------------------------- */

/* Decode a temp that must be the value of `*(base + (iv << shift)) [size]`,
 * the canonical lowering of `base[iv]` for a float array. On success records
 * the array base symbol and the element width in bits: 64 for the float64
 * shape (iv<<3, load size 8) and 32 for the float32 shape (iv<<2, load size 4).
 * Any other shape is rejected so the recognizer cannot mistake an int32 index
 * expression (or a differently-strided access) for a float reduction. */
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

/* A symbol is an acceptable float-array base if it is a function parameter or a
 * declared local (covers inlined-callee parameter copies). The strict
 * load-shape decode above already pins element width and float-ness, so this
 * stays permissive like ir_symbol_is_sum_array_base. */
static int ir_symbol_is_float_array_base(IRFunction *function,
                                         const char *symbol_name) {
  return ir_function_symbol_is_parameter(function, symbol_name) ||
         ir_function_local_declared_type(function, symbol_name) != NULL;
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

static int ir_float_scalar_operand_matches(IRFunction *function,
                                           const IROperand *operand,
                                           int width_bits) {
  if (!operand) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_FLOAT) {
    return operand->float_bits == width_bits;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    return ir_float_sum_type_matches(
        ir_function_local_declared_type(function, operand->name), width_bits);
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

/* Shared loop-frame matcher for the float reductions. Confirms `header_index`
 * begins a `while (iv < bound)` loop with a unit increment of `iv`, no nested
 * while, and a back-jump, returning the body bounds and key symbols. Returns 1
 * with *matched=1 on a clean frame; *matched=0 means "not this shape, skip". */
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
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
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
  if (ir_loop_body_has_nested_while(function, branch_index + 1, jump_index)) {
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
          &function->instructions[increment_index], compare->lhs.name)) {
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

/* Reject body shapes that are not a pure read-only reduction (a store or call
 * would make the fused kernel unsound). */
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
        ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name) {
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
  if (!ir_float_sum_type_matches(sum_type, width_bits) ||
      !ir_symbol_is_float_array_base(function, base_symbol) ||
      ir_symbol_read_after(function, jump_index + 1, iv_symbol)) {
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
          ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name)) {
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
      ir_symbol_read_after(function, jump_index + 1, iv_symbol)) {
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

int ir_simd_dot_float_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_dot_float_at(function, i, changed)) {
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

static int ir_float_map_body_is_safe(IRFunction *function, size_t lo,
                                     size_t hi, const char *iv_symbol,
                                     size_t *store_index_out) {
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
    if (ir_instruction_writes_symbol(ins) &&
        !ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      return 0;
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

  if (producer->lhs.kind == IR_OPERAND_TEMP && producer->lhs.name &&
      ir_decode_float_indexed_load(function, before, producer->lhs.name,
                                   iv_symbol, &base, &bits) &&
      bits == terms->width_bits &&
      ir_try_clone_float_scalar_operand(function, before, &producer->rhs,
                                        terms->width_bits, &scalar)) {
    int ok = ir_affine_map_add_indexed_term(terms, base, &scalar);
    ir_operand_destroy(&scalar);
    return ok;
  }
  if (producer->rhs.kind == IR_OPERAND_TEMP && producer->rhs.name &&
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
  if (store->dest.kind != IR_OPERAND_TEMP || !store->dest.name ||
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
      ir_symbol_read_after(function, jump_index + 1, iv_symbol)) {
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
