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

/* A symbol is an acceptable float-array base if it is a function parameter, a
 * declared local (covers inlined-callee parameter copies), or a GLOBAL the
 * function never writes and never takes the address of -- real programs (an
 * LLM engine's scratch buffers, a game's framebuffer pointer) keep their hot
 * arrays in global pointers, and rejecting those left every such loop
 * scalar. The strict load-shape decode above already pins element width and
 * float-ness. */
static int ir_symbol_is_float_array_base(IRFunction *function,
                                         const char *symbol_name) {
  if (ir_function_symbol_is_parameter(function, symbol_name) ||
      ir_function_local_declared_type(function, symbol_name) != NULL) {
    return 1;
  }
  /* Global: its VALUE must be stable across the loop. The recognizers'
   * bodies are store/call-free, so only a direct write inside this function
   * or an escaped address could change it mid-loop. */
  if (!symbol_name || ir_symbol_address_taken(function, symbol_name)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
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

/* Declared type of a function parameter by name, or NULL. (Locals come from
 * ir_function_local_declared_type, which does not see params -- they aren't
 * DECLARE_LOCAL'd.) */
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
    /* A literal used in float32 context usually still carries the default
     * float64 tag (`2.5` lowers as a double). The kernel broadcasts the
     * constant at its own lane width, so accept the mismatch whenever that
     * narrowing is exact -- then the kernel's coefficient is bit-identical to
     * the one the scalar loop multiplies by. */
    if (width_bits == 32 &&
        (double)(float)operand->float_value == operand->float_value) {
      return 1;
    }
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    /* A scalar coefficient may be a local OR a parameter (e.g. saxpy's `a` in
     * `y[i] = a*x[i] + y[i]` when `a` is a function arg). The kernel reads it as
     * a symbol either way; only the float width must match. */
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
      /* Normalize the tag to the kernel's lane width (the match may have
       * accepted an exactly-narrowable float64-tagged literal). */
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

  /* Bound: a parameter/inlined-param (always invariant), or any other
   * symbol -- a local or a GLOBAL (dimension globals like an LLM's D/HD are
   * the norm in real code) -- that the loop region never writes and whose
   * address never escapes. The kernel reads it once at entry; invariance
   * makes that identical to the scalar loop's per-iteration read. */
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
    if (ir_symbol_address_taken(function, compare->rhs.name)) {
      return 1;
    }
    for (size_t i = branch_index + 1; i < jump_index; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ir_instruction_writes_destination(ins) &&
          ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
          strcmp(ins->dest.name, compare->rhs.name) == 0) {
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

/* -------------------------------------------------------------------------- */
/* counter -> float64 chain -> (int64)trunc reduction -> IR_OP_SIMD_I2F_REDUCE  */
/* -------------------------------------------------------------------------- */

/* Op-codes for one chain step (stored as the INT operand of each argument pair;
 * the FLOAT operand is the step constant k). Applied to the running value x. */
#define I2F_STEP_MUL 0  /* x = x * k */
#define I2F_STEP_ADD 1  /* x = x + k */
#define I2F_STEP_SUBR 2 /* x = x - k */
#define I2F_STEP_SUBL 3 /* x = k - x */
#define I2F_STEP_DIVR 4 /* x = x / k */
#define I2F_MAX_STEPS 8

typedef struct {
  int op_code;
  double k;
} I2fChainStep;

/* Resolve the producer instruction of a temp (its definition) or a symbol (its
 * last write) before `before`. Returns NULL when none. */
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

/* Walk the straight-line float64 expression `op` down to the base `(float64)iv`,
 * pushing each binary-with-constant step into `steps` in base->outermost order.
 * Returns 1 on a fully-decoded affine/constant chain rooted at the counter. */
static int ir_i2f_extract_chain(IRFunction *function, size_t before,
                                const IROperand *op, const char *iv,
                                I2fChainStep *steps, int *nsteps) {
  const IRInstruction *p = ir_i2f_resolve_producer(function, before, op);
  if (!p) {
    return 0;
  }
  /* Base: x0 = (float64)i (an int->float cast of the loop counter). */
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
    /* k already holds the left constant. */
    if (strcmp(p->text, "+") == 0) {
      code = I2F_STEP_ADD;
    } else if (strcmp(p->text, "*") == 0) {
      code = I2F_STEP_MUL;
    } else if (strcmp(p->text, "-") == 0) {
      code = I2F_STEP_SUBL;
    } else {
      return 0; /* k / x is not affine in x; reject */
    }
  } else {
    return 0; /* both or neither constant: not a counter-affine step */
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

/* Evaluate the decoded chain at counter value i (host double, for range proof). */
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

/* Resolve a compile-time-constant trip bound from the loop compare's rhs: either
 * a direct INT, or an (int*)cast of an INT constant. Returns 1 and *out on
 * success. */
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

/* The loop body may only contain the reduction's straight-line float work: local
 * decls, casts, float/assign temps, nops, and the single counter increment. Any
 * store/call/branch/jump/nested-loop makes the fused kernel unsound. */
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

  /* Find the loop's exit test. Unlike the array reductions, a constant trip
   * bound is materialized by an (int64)const cast between the header and the
   * compare, so locate the branch first, then its compare via the temp it
   * tests. */
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

  /* Trip count must be a compile-time constant so the range proof is sound. */
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
  if (ir_loop_body_has_nested_while(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (!ir_i2f_body_is_safe(function, branch_index + 1, jump_index)) {
    return 1;
  }

  /* Counter must step by +1 and be initialized to 0 before the loop (the kernel
   * walks i = 0..bound-1). */
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
  {
    size_t init_index = 0;
    if (!ir_find_last_writer_before(function, header_index, IR_OPERAND_SYMBOL,
                                    iv_symbol, &init_index)) {
      return 1;
    }
    IRInstruction *init = &function->instructions[init_index];
    if (init->op != IR_OP_ASSIGN || init->lhs.kind != IR_OPERAND_INT ||
        init->lhs.int_value != 0) {
      return 1;
    }
  }

  /* Find the reduction: acc = acc + t, acc an int64 local, t = (int64)CHAIN. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IRInstruction *cast = NULL;
    const char *t = NULL;
    int local_nsteps = 0;
    if (!(ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name &&
          ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
          ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name)) {
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

  /* Range proof: the chain is affine in i, so its extrema are at i=0 and
   * i=bound-1. Require every truncated value to fit a signed int32 (so the
   * packed cvttpd2dq is exact) and the integer sum to stay below 2^52 (so f64
   * accumulation of integer addends is exact and reassociation-safe). */
  {
    double v0 = ir_i2f_eval_chain(steps, nsteps, 0.0);
    double vN = ir_i2f_eval_chain(steps, nsteps, (double)(bound - 1));
    double vmax = v0 > vN ? v0 : vN;
    double vmin = v0 < vN ? v0 : vN;
    double abs_max = vmax > -vmin ? vmax : -vmin;
    if (!(vmin == vmin) || !(vmax == vmax)) {
      return 1; /* NaN (e.g. divide by zero in the chain) */
    }
    if (abs_max >= 2147483647.0) {
      return 1; /* per-element value would overflow int32 */
    }
    if (abs_max * (double)bound >= 4503599627370496.0 /* 2^52 */) {
      return 1; /* running integer sum could exceed exact f64 range */
    }
  }

  /* Build the fused instruction: dest = acc; arguments[0] = bound (int64),
   * then (op_code INT, constant FLOAT64) per chain step. */
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

/* -------------------------------------------------------------------------- */
/* General auto-vectorizer: float32/float64 straight-line-DAG counted loops    */
/*   -> IR_OP_SIMD_VLOOP_F64 (width carried in float_bits: 64 or 32)           */
/*                                                                             */
/* Runs AFTER the per-shape recognizers (sum/dot/affine/i2f) so it only claims */
/* loops they did not. Handles element-wise maps out[iv] = DAG(...) and '+'    */
/* reductions over either float width; the store/accumulator type pins it.     */
/* -------------------------------------------------------------------------- */

/* Node tags — must match the kernel decoder in simd_float.c. */
#define VLOOP_VN_LOAD 0  /* op0 = loaded-array index */
#define VLOOP_VN_IOTA 1  /* (float64)iv */
#define VLOOP_VN_CONST 2 /* op0 = constant index */
#define VLOOP_VN_ADD 3
#define VLOOP_VN_SUB 4
#define VLOOP_VN_MUL 5
#define VLOOP_VN_DIV 6

#define VLOOP_MAX_NODES 48
#define VLOOP_MAX_ARRAYS 4 /* loaded bases; +dst must keep distinct bases <= 4 */
#define VLOOP_MAX_CONSTS 16
#define VLOOP_REG_BUDGET 4 /* ymm node-eval stack depth the kernel supports */

typedef struct {
  int tag;
  int op0;
  int op1;
} VLoopNode;

typedef struct {
  VLoopNode nodes[VLOOP_MAX_NODES];
  int n_nodes;
  const char *arrays[VLOOP_MAX_ARRAYS]; /* loaded base symbols (deduped) */
  int n_arrays;
  double consts[VLOOP_MAX_CONSTS]; /* deduped (bit-compare) */
  int n_consts;
  int width_bits;
  int has_iota;
  int overflow; /* a table limit was exceeded -> refuse */
} VLoopDag;

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

/* A compile-time float literal: a FLOAT operand of the right width, or a temp
 * that is a cast of an int/float literal to that width. Crucially this does NOT
 * match loop-invariant scalar *symbols* (parameters) — those are a runtime
 * broadcast not yet supported (affine_map already covers a*x+y), so leaving them
 * unmatched makes the pass cleanly refuse rather than miscompile. */
static int vloop_operand_is_literal(IRFunction *function, size_t before,
                                    const IROperand *op, int width_bits,
                                    double *out) {
  if (op->kind == IR_OPERAND_FLOAT && op->float_bits == width_bits) {
    *out = op->float_value;
    return 1;
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

/* Recursively lower a float operand into the DAG; returns the node index or -1
 * to refuse. Builds a TREE (shared subexpressions are re-evaluated) so a simple
 * stack-machine kernel can replay it. */
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
  /* array load a[iv] (only a TEMP names a load result) */
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
  const IRInstruction *p = ir_i2f_resolve_producer(function, before, op);
  if (!p) {
    return -1;
  }
  size_t pidx = (size_t)(p - function->instructions);
  /* (float64)iv */
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      vloop_text_is_float_width(p->text, d->width_bits) &&
      ir_operand_is_symbol_named(&p->lhs, iv)) {
    d->has_iota = 1;
    return vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
  }
  /* binary float op */
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

/* Stack-machine evaluation depth (= ymm registers the kernel needs). Matches
 * the kernel's naive left-then-right post-order: eval a, hold it while eval b,
 * then combine. */
static int vloop_eval_depth(const VLoopDag *d, int node) {
  const VLoopNode *n = &d->nodes[node];
  if (n->tag <= VLOOP_VN_CONST) {
    return 1; /* leaf: LOAD / IOTA / CONST */
  }
  int da = vloop_eval_depth(d, n->op0);
  int db = vloop_eval_depth(d, n->op1);
  int alt = 1 + db;
  return da > alt ? da : alt;
}

/* Count distinct base pointers the kernel must keep in GP registers: the loaded
 * arrays plus the destination if it is not already among them. */
static int vloop_distinct_bases(const VLoopDag *d, const char *dst_base) {
  int n = d->n_arrays;
  for (int i = 0; i < d->n_arrays; i++) {
    if (strcmp(d->arrays[i], dst_base) == 0) {
      return n; /* dst is a loaded array too */
    }
  }
  return n + 1;
}

static int vloop_serialize_into(IRInstruction *fused, const VLoopDag *d,
                                int reduce_op, int root, int depth) {
  size_t argc = (size_t)(6 + d->n_arrays + 3 * d->n_nodes + d->n_consts);
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
  fused->arguments[k++] = ir_operand_int(depth);
  for (int i = 0; i < d->n_arrays; i++) {
    fused->arguments[k++] = ir_operand_symbol(d->arrays[i]);
  }
  for (int i = 0; i < d->n_nodes; i++) {
    fused->arguments[k++] = ir_operand_int(d->nodes[i].tag);
    fused->arguments[k++] = ir_operand_int(d->nodes[i].op0);
    fused->arguments[k++] = ir_operand_int(d->nodes[i].op1);
  }
  for (int i = 0; i < d->n_consts; i++) {
    fused->arguments[k++] = ir_operand_float_sized(d->consts[i], 64);
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
  if (store->dest.kind != IR_OPERAND_TEMP || !store->dest.name ||
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
  d.width_bits = store_bits; /* 64 (float64) or 32 (float32) */
  root = vloop_build(function, store_index, &store->lhs, iv_symbol, &d);
  if (root < 0 || d.overflow) {
    ir_operand_destroy(&bound);
    return 1;
  }

  /* Gates. */
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
  if (depth > VLOOP_REG_BUDGET ||
      vloop_distinct_bases(&d, dst_base) > VLOOP_MAX_ARRAYS) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_F64;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = store_bits;
  fused.dest = ir_operand_symbol(dst_base);
  fused.lhs = bound; /* take ownership of the cloned bound operand */
  if (!vloop_serialize_into(&fused, &d, /*reduce_op=*/0, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

/* '+' reduction over a float64 DAG: acc = acc + DAG(a_k[iv], (float64)iv,
 * consts). Picks up reductions the sum/dot recognizers (which run earlier) did
 * not claim: sum-of-products, polynomial-in-iv, multi-array combinations. */
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

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        (ins->rhs.kind == IR_OPERAND_TEMP || ins->rhs.kind == IR_OPERAND_SYMBOL)) {
      acc_symbol = ins->dest.name;
      addend = &ins->rhs;
      reduce_index = i;
      found++;
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
  /* acc must be written only by the single reduction instruction. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    if (i != reduce_index &&
        ir_operand_is_symbol_named(&function->instructions[i].dest,
                                   acc_symbol)) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = width_bits; /* 64 (float64) or 32 (float32) */
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
  if (depth > VLOOP_REG_BUDGET - 1 /* ymm2 reserved as accumulator */ ||
      d.n_arrays > VLOOP_MAX_ARRAYS) {
    ir_operand_destroy(&bound);
    return 1;
  }

  fused.op = IR_OP_SIMD_VLOOP_F64;
  fused.location = function->instructions[header_index].location;
  fused.is_float = 1;
  fused.float_bits = width_bits;
  fused.dest = ir_operand_symbol(acc_symbol);
  fused.lhs = bound; /* take ownership */
  if (!vloop_serialize_into(&fused, &d, /*reduce_op=*/1, root, depth)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  ir_install_fused_reduction(function, header_index, jump_index, &fused,
                             changed);
  return 1;
}

int ir_auto_vectorize_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_map_at(function, i, changed)) {
        return 0;
      }
    }
    /* map may have fused (and NOP'd) the loop; reduce re-checks the header and
     * no-ops if so. The two shapes are mutually exclusive (map stores, reduce
     * accumulates). */
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_reduce_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Phase B: outer-loop lane vectorizer -> IR_OP_SIMD_OUTER_LANE_F64            */
/*                                                                             */
/* Recognizes `while(p<P){ <inner counted loop carrying float64 iacc>;         */
/*   total += iacc; p++ }` where the inner loop is outer-IV-invariant and its  */
/*   body is a serial recurrence iacc = CHAIN(iacc, uniform-of-i terms). Runs  */
/*   4 outer iterations in lockstep f64x4 lanes to hide the recurrence latency.*/
/* -------------------------------------------------------------------------- */

/* uniform-of-i linear micro-program ops (applied left to right, starting at i
 * in the integer domain; OL_U_CVT switches to the float domain). */
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
/* inner-recurrence chain ops */
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
  long long imm; /* int literal for int ops; fconst index for float ops */
} OlMicro;
typedef struct {
  OlMicro micro[OL_MAX_MICRO];
  int n_micro;
} OlUniform;
typedef struct {
  int op;        /* OL_C_* */
  int side;      /* 0: iacc OP term ; 1: term OP iacc */
  int term_kind; /* 0: const (fconst idx) ; 1: uniform (unif idx) */
  int term_idx;
} OlChainStep;
typedef struct {
  OlChainStep chain[OL_MAX_CHAIN];
  int n_chain;
  OlUniform unif[OL_MAX_UNIF];
  int n_unif;
  double fconst[OL_MAX_FCONST];
  int n_fconst;
  /* init_mode 0: the inner accumulator starts at a compile-time float const
   * (iacc_init) -> all outer iterations identical (lane0 fast path, bit-exact).
   * init_mode 1: the seed is a function of the outer index p (a uniform program
   * over p in init_prog) -> outer iterations differ; lanes diverge and are
   * summed by per-lane extraction in p order (still bit-exact). */
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

/* float64 literal (direct or cast-of-literal), like vloop_operand_is_literal. */
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

/* True if operand's expression references symbol `sym` (bounded walk). */
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

/* Build a linear uniform-of-i program for `op` (a value derived from the inner
 * counter `iv` and constants only). Emits micro-ops in i-first apply order. */
static int ol_build_uniform(IRFunction *fn, size_t before, const IROperand *op,
                            const char *iv, OlDag *d, OlUniform *prog) {
  if (!op || d->overflow) {
    return 0;
  }
  /* base: the inner counter i */
  if (op->kind == IR_OPERAND_SYMBOL && op->name && strcmp(op->name, iv) == 0) {
    return 1; /* program starts implicitly at i (int domain) */
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
    /* int -> float64 cast */
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
  /* Identify the i-bearing operand and the constant operand. */
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
      if (!cst_on_right) return 0; /* c - x not supported */
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
  /* integer op with an integer-literal constant */
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

/* Extract one chain term (const or uniform-of-i) into the dag; sets *kind/*idx. */
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

/* Walk the inner accumulator update expression into the recurrence chain
 * (base-first). Exactly one operand at each binary leads to iacc; the other is a
 * uniform term. */
static int ol_build_chain(IRFunction *fn, size_t before, const IROperand *op,
                          const char *iacc, const char *iv, OlDag *d) {
  if (!op || d->overflow) {
    return 0;
  }
  if (op->kind == IR_OPERAND_SYMBOL && op->name && strcmp(op->name, iacc) == 0) {
    return 1; /* base: the carried accumulator */
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
  if (!ol_build_chain(fn, pidx, inner, iacc, iv, d)) { /* recurse base-side first */
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

/* The inner loop body must be pure straight-line float/int work (the recurrence
 * + the uniform computations + the counter increment). No memory, calls, or
 * control flow. */
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

/* Scan [lo,hi) for the first BRANCH_ZERO, returning its index or -1 (stops at a
 * jump/second label so it stays within the loop header region). */
static long long ol_find_branch_zero(IRFunction *fn, size_t lo, size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    IROpcode op = fn->instructions[i].op;
    if (op == IR_OP_BRANCH_ZERO) return (long long)i;
    if (op == IR_OP_JUMP) return -1;
  }
  return -1;
}

/* Find a JUMP to `label` in [lo,hi); returns index or -1. */
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

/* Decode a `iv <cmp> N` loop compare feeding the branch at branch_index. Returns
 * 1 with *iv_out/*bound_out/*cmp_out (0:<, 1:<=) on success. */
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
/* Diagnostic hook (no-op in production); set to an fprintf during bring-up. */
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
  long long oj = ol_find_jump_to(function, outer_branch + 1, n, outer_label);
  if (oj < 0) {
    OL_DBG("no outer back-jump");
    ir_operand_destroy(&outerP);
    return 1;
  }
  size_t outer_jump = (size_t)oj;

  /* Find the (single) inner while header in the outer body. ir_label_is_while_header
   * also matches the loop's *end* label (it contains "_lbl_ir_while_"), so skip
   * any label naming a while-end marker — only true headers count. */
  long long inner_hdr = -1;
  for (size_t i = outer_branch + 1; i < outer_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text &&
        ir_label_is_while_header(ins->text) &&
        !strstr(ins->text, "while_end")) {
      if (inner_hdr >= 0) {
        OL_DBG(">1 inner while header");
        ir_operand_destroy(&outerP);
        return 1;
      }
      inner_hdr = (long long)i;
    }
  }
  if (inner_hdr < 0) {
    OL_DBG("no inner while header");
    ir_operand_destroy(&outerP);
    return 1;
  }
  size_t inner_header = (size_t)inner_hdr;
  const char *inner_label = function->instructions[inner_header].text;

  long long ib = ol_find_branch_zero(function, inner_header + 1, outer_jump);
  if (ib < 0) { OL_DBG("no inner branch_zero"); ir_operand_destroy(&outerP); return 1; }
  size_t inner_branch = (size_t)ib;
  const char *i_sym = NULL;
  IROperand innerN = {0};
  int inner_cmp = 0;
  if (!ol_decode_loop_compare(function, inner_branch, &i_sym, &innerN,
                              &inner_cmp)) {
    OL_DBG("inner compare decode failed");
    ir_operand_destroy(&outerP);
    return 1;
  }
  long long ij = ol_find_jump_to(function, inner_branch + 1, outer_jump,
                                 inner_label);
  if (ij < 0) { OL_DBG("no inner back-jump"); ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1; }
  size_t inner_jump = (size_t)ij;

  /* Inner increment: i = i + istep (unit). */
  {
    size_t inc = inner_jump;
    while (inc > inner_branch + 1) {
      inc--;
      if (function->instructions[inc].op != IR_OP_NOP) break;
    }
    if (!ir_try_parse_direct_unit_increment(&function->instructions[inc],
                                            i_sym)) {
      OL_DBG("inner increment not unit");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
  }
  /* Outer increment: p = p + 1 (unit), just before the outer back-jump. */
  {
    size_t inc = outer_jump;
    while (inc > inner_jump) {
      inc--;
      if (function->instructions[inc].op != IR_OP_NOP) break;
    }
    if (!ir_try_parse_direct_unit_increment(&function->instructions[inc],
                                            p_sym)) {
      OL_DBG("outer increment not unit");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
  }
  if (ir_loop_body_has_nested_while(function, inner_branch + 1, inner_jump)) {
    OL_DBG("inner body has nested while");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* The outer reduction total = total + iacc, after the inner loop. */
  const char *total_sym = NULL;
  const char *iacc_sym = NULL;
  for (size_t i = inner_jump + 1; i < outer_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ins->rhs.kind == IR_OPERAND_SYMBOL && ins->rhs.name) {
      total_sym = ins->dest.name;
      iacc_sym = ins->rhs.name;
      break;
    }
  }
  if (!total_sym || !iacc_sym || strcmp(total_sym, iacc_sym) == 0) {
    OL_DBG("no outer reduction total+=iacc");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }
  if (!ir_float_sum_type_matches(
          ir_function_local_declared_type(function, total_sym), 64) ||
      !ir_float_sum_type_matches(
          ir_function_local_declared_type(function, iacc_sym), 64)) {
    OL_DBG("total/iacc not float64");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* i init (i0) before the inner header. */
  long long i0 = 0;
  int found_i0 = 0;
  for (size_t i = outer_branch + 1; i < inner_header; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&ins->dest, i_sym) &&
        ins->lhs.kind == IR_OPERAND_INT) {
      i0 = ins->lhs.int_value;
      found_i0 = 1;
    }
  }
  if (!found_i0) {
    OL_DBG("no inner i0");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* The inner accumulator seed: either a compile-time float const (all outer
   * iterations identical -> lane0 fast path) or a function of the outer index p
   * (iterations differ -> divergent lanes). */
  OlDag d;
  memset(&d, 0, sizeof(d));
  {
    size_t init_idx = 0;
    if (!ir_find_last_writer_before(function, inner_header, IR_OPERAND_SYMBOL,
                                    iacc_sym, &init_idx)) {
      OL_DBG("no iacc init writer");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
    const IRInstruction *init_ins = &function->instructions[init_idx];
    if (init_ins->op == IR_OP_ASSIGN && init_ins->lhs.kind == IR_OPERAND_FLOAT) {
      d.init_mode = 0;
      d.iacc_init = init_ins->lhs.float_value;
    } else {
      /* Build a uniform program over the OUTER index p for the seed value. For
       * `iacc <- value` walk the value; for `iacc = a OP b` walk the binary. */
      const IROperand *seed_op = NULL;
      IROperand iacc_op = ir_operand_symbol(iacc_sym);
      if (init_ins->op == IR_OP_ASSIGN) {
        seed_op = &init_ins->lhs;
      } else {
        seed_op = &iacc_op; /* ol_build_uniform resolves iacc's producer */
      }
      d.init_prog.n_micro = 0;
      if (!ol_build_uniform(function, inner_header, seed_op, p_sym, &d,
                            &d.init_prog) ||
          d.overflow) {
        OL_DBG("iacc seed neither const nor uniform-of-p");
        ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
      }
      d.init_mode = 1;
    }
  }

  /* The single inner recurrence write: iacc = <expr>. */
  long long iacc_upd = -1;
  for (size_t i = inner_branch + 1; i < inner_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_BINARY && ins->is_float &&
        ir_operand_is_symbol_named(&ins->dest, iacc_sym)) {
      if (iacc_upd >= 0) { iacc_upd = -2; break; }
      iacc_upd = (long long)i;
    } else if (ir_operand_is_symbol_named(&ins->dest, iacc_sym)) {
      iacc_upd = -2; /* iacc written by a non-float-binary -> reject */
      break;
    }
  }
  if (iacc_upd < 0) {
    OL_DBG("no single iacc recurrence update");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* Body purity: no stores/calls/branches/etc. in the inner body. */
  if (!ol_inner_body_pure(function, inner_branch + 1, inner_jump)) {
    OL_DBG("inner body not pure");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* Build the recurrence chain from the iacc update RHS (which is the full
   * expression; reconstruct it from dest = lhs OP rhs of the update). */
  {
    /* The update is `iacc = A OP B` (a float binary). Treat its result as the
     * chain root expression by walking from a synthetic temp == the update. */
    const IRInstruction *upd = &function->instructions[iacc_upd];
    /* Re-express: build chain over the binary `upd`. We emulate ol_build_chain
     * on the update by handling its top node directly. */
    int l_has = ol_contains_symbol(function, (size_t)iacc_upd, &upd->lhs,
                                   iacc_sym, 0);
    int r_has = ol_contains_symbol(function, (size_t)iacc_upd, &upd->rhs,
                                   iacc_sym, 0);
    const IROperand *inner_op = NULL;
    const IROperand *term_op = NULL;
    int side;
    if (l_has && !r_has) { inner_op = &upd->lhs; term_op = &upd->rhs; side = 0; }
    else if (r_has && !l_has) { inner_op = &upd->rhs; term_op = &upd->lhs; side = 1; }
    else { OL_DBG("update: both/neither operand carries iacc"); ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1; }
    int op_code;
    if (strcmp(upd->text, "+") == 0) op_code = OL_C_ADD;
    else if (strcmp(upd->text, "-") == 0) op_code = OL_C_SUB;
    else if (strcmp(upd->text, "*") == 0) op_code = OL_C_MUL;
    else if (strcmp(upd->text, "/") == 0) op_code = OL_C_DIV;
    else { OL_DBG("update: top op not +-*/"); ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1; }
    int kind = 0, idx = 0;
    if (!ol_extract_term(function, (size_t)iacc_upd, term_op, i_sym, &d, &kind,
                         &idx) ||
        !ol_build_chain(function, (size_t)iacc_upd, inner_op, iacc_sym, i_sym,
                        &d)) {
      OL_DBG("chain/term build failed");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
    if (d.n_chain >= OL_MAX_CHAIN) { ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1; }
    d.chain[d.n_chain].op = op_code;
    d.chain[d.n_chain].side = side;
    d.chain[d.n_chain].term_kind = kind;
    d.chain[d.n_chain].term_idx = idx;
    d.n_chain++;
  }
  if (d.overflow || d.n_chain == 0) {
    OL_DBG("dag overflow or empty chain");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }

  /* The INNER loop must be invariant in p (p may only feed the seed, in the init
   * region before inner_header): reject any p reference in [inner_header,
   * inner_jump]. And `total` must be touched only by the reduction (after
   * inner_jump): reject it anywhere in [outer_branch+1, inner_jump]. */
  for (size_t i = inner_header; i <= inner_jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *ops[3] = {&ins->lhs, &ins->rhs, &ins->dest};
    for (int k = 0; k < 3; k++) {
      if (ops[k]->kind == IR_OPERAND_SYMBOL && ops[k]->name &&
          strcmp(ops[k]->name, p_sym) == 0) {
        OL_DBG("inner loop references p (not p-invariant)");
        ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
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
        ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
      }
    }
  }

  /* Serialize and install. Layout (mirror in the kernel):
   * header: [0]inner_cmp [1]istep [2]n_chain [3]n_unif [4]n_fconst [5]i0
   *         [6]init_mode [7]iacc_init(FLOAT)
   * then n_chain*4 INT chain steps; then n_unif chain-uniform programs
   * (n_micro INT + n_micro*2 INT); then IF init_mode==1 the seed program
   * (same shape); then n_fconst FLOAT. dest=total, lhs=P, rhs=N. */
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
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 0;
  }
  fused.argument_count = argc;
  size_t k = 0;
  fused.arguments[k++] = ir_operand_int(inner_cmp);
  fused.arguments[k++] = ir_operand_int(1); /* istep */
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
  fused.lhs = outerP; /* take ownership */
  fused.rhs = innerN; /* take ownership */
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
