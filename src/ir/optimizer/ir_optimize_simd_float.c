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
    /* A SIMD array op carries the output array's base symbol as its dest by
     * convention (`@a = simd_affine_map_f32(...)`, `@a = simd_fill(...)`): it
     * writes the array ELEMENTS through the base, it does NOT reassign the
     * base pointer's value. So such an op (typically a SIBLING loop already
     * vectorized on the same array) must not disqualify the base -- otherwise
     * vectorizing one `a[i]=...` loop would poison every later one on `a`. */
    if (ins->op >= IR_OP_SIMD_SUM_I32 && ins->op <= IR_OP_SIMD_LCG_U32) {
      continue;
    }
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
 * begins a `while (iv < bound)` loop starting at iv == 0 with a unit increment
 * of `iv`, no nested while, and a back-jump, returning the body bounds and key
 * symbols. Returns 1
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
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
    return 1; /* threaded exit: fusing would delete the exit edge */
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
  /* Every fused kernel walks its array bases from element 0 and treats the
   * compare bound as the element COUNT, so the loop must provably start at
   * iv == 0. Catches `j = 3; while (j < n)` and `for i in 1..n` reductions
   * that previously vectorized as 0..n. */
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
      /* A per-iteration local the DAG builder can substitute (`var x = a[i];
       * out[i] = x*x*x`) is fine, provided it does not outlive the loop -- the
       * fused kernel deletes the body, so a value read afterward would vanish. */
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
  /* The indexed-address decode only proves a 4/8-byte unit-stride store; it
   * canNOT tell a float32 array from a uint32 one (both lower to base+(i<<2),
   * size 4). Without `store->is_float` an integer copy `out[i]=a[i]` matched
   * this FLOAT kernel, and `1.0*x` is not a bit-identity for integer data
   * whose bits form a float NaN (the multiply canonicalizes the payload). */
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
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
    return 1; /* threaded exit: fusing would delete the exit edge */
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
  /* ir_iv_zero_at_header refuses at the first control-flow join, unlike a
   * textual last-writer scan that an if/else init upstream could fool. */
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
    return 1;
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
#define VLOOP_VN_IOTA 1  /* (float64)iv, or the raw iv for int lanes */
#define VLOOP_VN_CONST 2 /* op0 = constant index */
#define VLOOP_VN_ADD 3
#define VLOOP_VN_SUB 4
#define VLOOP_VN_MUL 5
#define VLOOP_VN_DIV 6
#define VLOOP_VN_SCALAR 7 /* op0 = runtime loop-invariant scalar index */
#define VLOOP_VN_AND 8    /* int lanes only */
#define VLOOP_VN_OR 9     /* int lanes only */
#define VLOOP_VN_XOR 10   /* int lanes only */
#define VLOOP_VN_SHL 11   /* int lanes only; op0 = node, op1 = literal count */

#define VLOOP_MAX_NODES 48
#define VLOOP_MAX_ARRAYS 4 /* loaded bases; +dst must keep distinct bases <= 4 */
#define VLOOP_MAX_CONSTS 16
#define VLOOP_MAX_SCALARS 8
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
  double consts[VLOOP_MAX_CONSTS];      /* deduped (bit-compare); float DAGs */
  long long iconsts[VLOOP_MAX_CONSTS];  /* deduped; int DAGs */
  int n_consts;
  const char *scalars[VLOOP_MAX_SCALARS]; /* invariant scalar symbols (deduped) */
  int n_scalars;
  int width_bits;
  int is_int; /* 0 = float lanes (width_bits 32/64), 1 = int32 lanes */
  size_t body_lo; /* loop body region, for symbol-invariance checks */
  size_t body_hi;
  int has_iota;
  int overflow; /* a table limit was exceeded -> refuse */
  int resolve_depth; /* body-local substitution recursion guard */
  int iota_bound_known; /* the loop's trip count is a compile-time constant */
  long long iota_bound; /* that constant (iv ranges over [0, iota_bound)) */
} VLoopDag;

#define VLOOP_MAX_RESOLVE_DEPTH 16

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

/* A symbol written anywhere in the loop body is not a single value across
 * iterations (the accumulator, a rotating local): it can be neither broadcast
 * nor producer-chased (the chase would bake the loop-ENTRY value into every
 * lane). */
static int vloop_symbol_written_in_body(const IRFunction *function,
                                        const VLoopDag *d, const char *name) {
  for (size_t i = d->body_lo; i < d->body_hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
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

/* A float64 literal is admissible in a float32 DAG only when it narrows to
 * float32 EXACTLY (round-trips). Mettle defaults float literals to float64, so
 * `a[i] * 2.0` carries a float64 `2.0`; the f32 kernel broadcasts `(float)2.0`,
 * which for an exactly-representable value is the IDENTICAL number the literal
 * denotes. The only residual difference from the scalar loop is then the same
 * f32-lane-vs-f64-intermediate rounding the runtime-scalar reduction (`k*a[i]`)
 * already ships with -- so this is exactly as faithful as that. A non-exact
 * literal (0.1) would make the f32 coefficient differ from the value the scalar
 * loop multiplies by, so it is refused. Mirrors the affine kernel's policy. */
static int vloop_f64_narrows_exactly(double v) {
  return (double)(float)v == v;
}

/* A compile-time float literal: a FLOAT operand of the right width (or an
 * exactly-narrowable float64 literal in a float32 DAG), or a temp that is a
 * cast of an int/float literal to that width. Crucially this does NOT match
 * loop-invariant scalar *symbols* (parameters) — those are a runtime broadcast
 * handled via VLOOP_VN_SCALAR, so leaving them here makes the pass cleanly
 * refuse rather than miscompile. */
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
  if (op->kind == IR_OPERAND_SYMBOL) {
    if (vloop_symbol_written_in_body(function, d, op->name)) {
      /* A symbol written in the body is not a stable broadcast value, but if
       * it is a single-assignment per-iteration LOCAL (`var d = a[i]-b[i]`),
       * substitute its defining expression into the DAG -- this is what makes
       * SSD / variance / `var x=...; x*x*x` shapes vectorize. */
      return vloop_resolve_body_local(function, op->name, iv, d);
    }
    /* Loop-invariant float scalar of the lane width (a local or parameter,
     * e.g. saxpy's runtime `a`): read once at loop entry and broadcast.
     * Preferred over chasing its pre-loop producer -- one slot beats
     * re-evaluating an invariant expression per lane. */
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
  /* (float64)iv */
  if (p->op == IR_OP_CAST && !p->is_float && p->text &&
      vloop_text_is_float_width(p->text, d->width_bits) &&
      ir_operand_is_symbol_named(&p->lhs, iv)) {
    d->has_iota = 1;
    return vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
  }
  /* (float64)(C +/- iv) / (float64)(iv +/- C): an INTEGER affine function of the
   * induction var cast to float64. Sound to evaluate in the f64 domain as
   * `IOTA +/- (float64)C` because every int32 value (and the int32 sum/difference
   * when it does not overflow) is exactly representable in float64, so the f64
   * op is bit-identical to casting the integer result. Gated to float64 (the
   * equivalence fails for float32 once |value| >= 2^24) and to a compile-time
   * trip count, so the no-overflow range check is decidable. */
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
        long long hi = d->iota_bound - 1; /* max iv */
        long long rmin, rmax;
        if (on_left) { /* iv (+/-) C */
          rmin = is_sub ? -C : C;
          rmax = is_sub ? hi - C : hi + C;
        } else { /* C (+/-) iv */
          rmin = is_sub ? C - hi : C;
          rmax = is_sub ? C : C + hi;
        }
        if (rmin >= -2147483648LL && rmax <= 2147483647LL) {
          int tag = vloop_binop_tag(q->text);
          int ci = vloop_intern_const(d, (double)C);
          if (tag < 0 || ci < 0) return -1;
          /* The kernel is a postorder stack machine: a binary node pops the two
           * most-recently built results as (left, right) and computes left OP
           * right. So build the LEFT operand first to preserve subtraction order
           * (`iv - C` vs `C - iv`). */
          int a, b;
          if (on_left) { /* iv OP C : left = iota */
            d->has_iota = 1;
            a = vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
            b = vloop_add_node(d, VLOOP_VN_CONST, ci, 0);
          } else { /* C OP iv : left = const */
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

/* Substitute a single-assignment loop-body local into the DAG by building from
 * its defining expression in place of the symbol. This is what lets
 * `var d = a[i] - b[i]; s = s + d*d` (sum-of-squared-differences / variance)
 * and `var x = a[i]; out[i] = x*x*x` vectorize: the local is not a broadcast
 * value, it is an alias for a per-iteration expression. Guards keep it sound:
 *   - exactly ONE in-body definition (else it could be a recurrence or a
 *     conditionally-set value, neither of which is a pure alias);
 *   - not live after the loop (the fused kernel deletes the body);
 *   - bounded substitution depth, so a cycle of mutually-referential locals
 *     (or the accumulator referring to itself) refuses instead of recursing
 *     without end.
 * Re-evaluating the aliased subexpression at each use is correct (the local
 * held exactly that value); it only costs redundant compute the kernel could
 * later CSE. */
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
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, sym) == 0) {
      if (def) {
        return -1; /* written more than once: not a simple per-iteration alias */
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
             def->lhs.kind == IR_OPERAND_TEMP && def->lhs.name &&
             def->rhs.kind == IR_OPERAND_INT &&
             def->rhs.int_value == d->width_bits / 8) {
    /* `var x = a[i]` lowers to a LOAD straight into the symbol; rebuild it as
     * an indexed array load by decoding the address. */
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

/* Stack-machine evaluation depth (= ymm registers the kernel needs). Matches
 * the kernel's naive left-then-right post-order: eval a, hold it while eval b,
 * then combine. */
static int vloop_eval_depth(const VLoopDag *d, int node) {
  const VLoopNode *n = &d->nodes[node];
  if (vloop_tag_is_leaf(n->tag)) {
    return 1; /* leaf: LOAD / IOTA / CONST / SCALAR */
  }
  if (n->tag == VLOOP_VN_SHL) {
    return vloop_eval_depth(d, n->op0); /* unary, evaluated in place */
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
  /* `store->is_float` gate: a uint32 store has the same base+(i<<2) shape as a
   * float32 one, so without this an integer map would build a float DAG. */
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
  d.width_bits = store_bits; /* 64 (float64) or 32 (float32) */
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

  /* The accumulation appears in one of two equivalent IR forms:
   *   direct       `acc = acc + X`              (dest is the acc symbol)
   *   temp+ASSIGN  `%t = acc + X; acc <- %t`    (dest is a temp, then copied)
   * The latter survives when X is a float64-tracked expression narrowed to a
   * float32 acc (e.g. `s += a[i] * 2.0`): copy-prop won't fold the temp across
   * the narrowing, so the direct form never forms. Both are the same reduction;
   * `assign_index` records the trailing ASSIGN so it is exempted from the
   * written-once check below. */
  size_t assign_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!(ins->op == IR_OP_BINARY && ins->is_float && ins->text &&
          strcmp(ins->text, "+") == 0 &&
          (ins->rhs.kind == IR_OPERAND_TEMP ||
           ins->rhs.kind == IR_OPERAND_SYMBOL))) {
      continue;
    }
    if (ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name)) {
      acc_symbol = ins->dest.name;
      addend = &ins->rhs;
      reduce_index = i;
      assign_index = (size_t)-1;
      found++;
    } else if (ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
               ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name) {
      /* `%t = acc + X`: confirm the next non-NOP copies %t straight back into
       * the same symbol (`acc <- %t`). */
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
  /* acc must be written only by the single reduction instruction, and no
   * OTHER symbol may be written in the body besides the iv increment -- a
   * rotating local would be lost when the loop is fused away. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i == reduce_index || i == assign_index) {
      continue; /* the accumulation itself (temp+ASSIGN spans two slots) */
    }
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name) {
      /* The accumulator may be written ONLY by the reduction. */
      if (strcmp(ins->dest.name, acc_symbol) == 0) {
        ir_operand_destroy(&bound);
        return 1;
      }
      /* A non-iv symbol write is tolerated only when the symbol is a
       * per-iteration LOCAL that does not outlive the loop: the DAG builder
       * substitutes it (`var d = a[i]-b[i]; s += d*d`), and the fused kernel
       * deletes the body, so a value live afterward would be lost. */
      if (strcmp(ins->dest.name, iv_symbol) != 0 &&
          ir_symbol_live_after_loop(function, jump_index + 1, ins->dest.name)) {
        ir_operand_destroy(&bound);
        return 1;
      }
    }
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = width_bits; /* 64 (float64) or 32 (float32) */
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

/* -------------------------------------------------------------------------- */
/* Multi-store map fission: a counted loop whose body is K>=2 independent       */
/* unit-stride float stores (e.g. saxpy init `x[i]=f(i); y[i]=g(i)`). Each      */
/* store is emitted as its own full-count IR_OP_SIMD_VLOOP_F64 -- semantically  */
/* loop fission (all of store_1, then all of store_2) -- reusing the proven     */
/* single-store kernel unchanged. SOUND ONLY when the destinations are disjoint */
/* (reordering writes across the lane window is otherwise observable), so it is */
/* gated on a conservative non-aliasing proof below.                            */

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

/* The single instruction that defines symbol/temp `name`, or -1 if there is not
 * exactly one (a conservative "give up" for the disjointness proof). */
static int ir_msf_single_def(IRFunction *function, const char *name,
                             int is_symbol) {
  int found = -1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    /* A SIMD array op carries the output array's base symbol as its dest by
     * convention (`@a = simd_vloop_f64(...)`): it writes the array ELEMENTS, not
     * the base pointer's value, so it must not count as a (re)definition of the
     * pointer -- otherwise a sibling already-vectorized loop on the same array
     * (e.g. saxpy's hot loop on @y) would mask its fresh-allocation provenance. */
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

/* True if `v` provably holds the result of a fresh heap allocation, following
 * cast/assign hops through singly-defined temps (bounded depth). */
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

/* Every destination is a distinct local pointer, address never taken, defined
 * exactly once from a fresh allocation -- two distinct allocations never alias,
 * so the per-store full-count rewrite preserves the scalar memory state. */
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

/* Collect the store indices of a multi-store map body; require it otherwise
 * pure (no loads -- so the only memory dependence is between the stores -- no
 * calls/branches, and any per-iteration local does not outlive the loop). */
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

/* Build the IR_OP_SIMD_VLOOP_F64 fused op for one store of a multi-store loop,
 * mirroring the single-store path. Returns 1 (filling *fused and *dst_base) or 0
 * to reject the whole loop. `bound` is cloned into the fused op. */
static int ir_msf_build_store(IRFunction *function, size_t store_index,
                              const char *iv_symbol, size_t body_lo,
                              size_t body_hi, const IROperand *bound,
                              IRInstruction *fused, const char **dst_base_out) {
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
  if (depth > VLOOP_REG_BUDGET ||
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
      !vloop_serialize_into(fused, &d, /*reduce_op=*/0, root, depth)) {
    ir_instruction_destroy_storage(fused);
    return 0;
  }
  *dst_base_out = dst_base;
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
  /* Room to place ns fused ops in the loop's instruction range. */
  if ((size_t)ns > jump_index - header_index + 1) {
    ir_operand_destroy(&bound);
    return 1;
  }
  for (built = 0; built < ns; built++) {
    if (!ir_msf_build_store(function, stores[built], iv_symbol,
                            branch_index + 1, jump_index, &bound, &fused[built],
                            &bases[built])) {
      for (int k = 0; k < built; k++) ir_instruction_destroy_storage(&fused[k]);
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  if (!ir_msf_bases_disjoint(function, bases, ns)) {
    for (int k = 0; k < ns; k++) ir_instruction_destroy_storage(&fused[k]);
    ir_operand_destroy(&bound);
    return 1;
  }
  /* Commit: one full-count store-kernel per destination, then NOP the loop. */
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

int ir_auto_vectorize_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    /* Multi-store map fission runs first: it claims K>=2 store loops the
     * single-store recognizer would reject, lowering each to its own kernel. */
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
/* Integer twin of the general auto-vectorizer -> IR_OP_SIMD_VLOOP_I32         */
/*                                                                             */
/* int32/uint32 unit-stride maps and '+' reductions over + - * & | ^ and       */
/* <<literal DAGs. Every supported op is congruent mod 2^32, intermediates     */
/* are truncated to the 4-byte store / int32 accumulator anyway, and integer   */
/* '+' is associative -- so unlike the float form, maps AND reductions are     */
/* BIT-EXACT against the scalar loop. Division, %, and >> are never taken      */
/* (not congruent / trapping). Runs after auto_vectorize (a float32 store      */
/* shape that pass refused has float-tagged BINARYs and refuses here too).     */
/* -------------------------------------------------------------------------- */

static int vloop_int_scalar_type_ok(const char *ty) {
  /* Any plain integer type: the kernel uses only the low 32 bits and every
   * supported op is congruent mod 2^32, so width and signedness don't matter
   * (operand_load extends per the declared type; bits >= 32 are irrelevant). */
  return ty && (strcmp(ty, "int8") == 0 || strcmp(ty, "uint8") == 0 ||
                strcmp(ty, "int16") == 0 || strcmp(ty, "uint16") == 0 ||
                strcmp(ty, "int32") == 0 || strcmp(ty, "uint32") == 0 ||
                strcmp(ty, "int64") == 0 || strcmp(ty, "uint64") == 0 ||
                strcmp(ty, "int") == 0);
}

/* An int->int cast whose target keeps at least 32 bits only changes bits the
 * 32-bit lanes never see (trunc-to-32 / sign- or zero-extension), so it is
 * transparent to the DAG. Casts to int8/int16 fold sign back into the low 32
 * bits and are refused. */
static int vloop_int_cast_is_transparent(const char *ty) {
  return ty && (strcmp(ty, "int32") == 0 || strcmp(ty, "uint32") == 0 ||
                strcmp(ty, "int64") == 0 || strcmp(ty, "uint64") == 0 ||
                strcmp(ty, "int") == 0);
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
    /* the counter used directly as a value: out[i] = a[i] + i */
    if (strcmp(op->name, iv) == 0) {
      d->has_iota = 1;
      return vloop_add_node(d, VLOOP_VN_IOTA, 0, 0);
    }
    if (vloop_symbol_written_in_body(function, d, op->name)) {
      return -1;
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
  /* array load a[iv]: 4-byte elements only (the shape decoder pins iv<<2) */
  if (op->kind == IR_OPERAND_TEMP) {
    const char *base = NULL;
    int bits = 0;
    if (ir_decode_float_indexed_load(function, before, op->name, iv, &base,
                                     &bits) &&
        bits == 32) {
      int ai = vloop_intern_array(d, base);
      return ai < 0 ? -1 : vloop_add_node(d, VLOOP_VN_LOAD, ai, 0);
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

/* Shared matcher for the int32 map shape. Fills the DAG and the loop facts;
 * *claim_out = 1 when every gate passes (the loop WOULD be fused). The bound
 * operand is cloned into *bound on a successful frame match regardless and
 * must be destroyed by the caller unless ownership is taken. Returns 0 only
 * on allocation failure. */
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
  if (!ir_float_map_body_is_safe(function, branch_index + 1, jump_index,
                                 iv_symbol, &store_index)) {
    return 1;
  }

  store = &function->instructions[store_index];
  if (store->dest.kind != IR_OPERAND_TEMP || !store->dest.name ||
      (store->lhs.kind != IR_OPERAND_TEMP &&
       store->lhs.kind != IR_OPERAND_SYMBOL &&
       store->lhs.kind != IR_OPERAND_INT) ||
      store->rhs.kind != IR_OPERAND_INT || store->rhs.int_value != 4 ||
      !ir_decode_float_indexed_address(function, store_index, store->dest.name,
                                       iv_symbol, &dst_base, &store_bits) ||
      store_bits != 32) {
    return 1;
  }

  memset(d, 0, sizeof(*d));
  d->width_bits = 32;
  d->is_int = 1;
  d->body_lo = branch_index + 1;
  d->body_hi = jump_index;
  root = vloop_build_int(function, store_index, &store->lhs, iv_symbol, d);
  if (root < 0 || d->overflow) {
    return 1;
  }
  /* Trivial bodies (a plain copy or a constant splat) belong to the tuned
   * memory-map/fill kernels; claiming them here would only swap one kernel
   * for a slower one. */
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
  if (depth > VLOOP_REG_BUDGET ||
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

/* Read-only probe for other passes (pointer-induction must not convert a loop
 * this vectorizer would claim -- the kernel needs the indexed form). */
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
  fused.float_bits = 32;
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
    /* The kernel accumulates in 32-bit lanes and stores 32 bits back, which
     * is only congruent when the accumulator itself is 32-bit. A widening
     * int64 += int32 sum keeps high bits the lanes never compute -- refuse
     * (the dedicated sum_i32 kernel handles the bare-load form of that). */
    const char *acc_type = ir_function_local_declared_type(function, acc_symbol);
    if (!acc_type) {
      acc_type = ir_function_param_declared_type(function, acc_symbol);
    }
    if (!acc_type || (strcmp(acc_type, "int32") != 0 &&
                      strcmp(acc_type, "uint32") != 0)) {
      ir_operand_destroy(&bound);
      return 1;
    }
    /* The kernel re-extends the folded 32-bit sum into the accumulator's
     * 8-byte stack home, and the extension must match the declared
     * signedness (homes hold canonically-extended values). */
    fused.is_unsigned = (strcmp(acc_type, "uint32") == 0);
  }
  /* acc written only by the reduction; no other symbol writes besides iv. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i != reduce_index && ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        (strcmp(ins->dest.name, acc_symbol) == 0 ||
         strcmp(ins->dest.name, iv_symbol) != 0)) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }

  memset(&d, 0, sizeof(d));
  d.width_bits = 32;
  d.is_int = 1;
  d.body_lo = branch_index + 1;
  d.body_hi = jump_index;
  root = vloop_build_int(function, reduce_index, addend, iv_symbol, &d);
  if (root < 0 || d.overflow) {
    ir_operand_destroy(&bound);
    return 1;
  }
  if (d.n_nodes < 2) { /* a bare-load sum belongs to the tuned sum kernels */
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

  fused.op = IR_OP_SIMD_VLOOP_I32;
  fused.location = function->instructions[header_index].location;
  fused.float_bits = 32;
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

/* -------------------------------------------------------------------------- */
/* Early-exit search skip-ahead -> IR_OP_SIMD_FIND                             */
/*                                                                             */
/* Vectorizes find / memchr / mismatch loops WITHOUT touching their control    */
/* flow: only the counter's zero-init is replaced by a kernel that computes    */
/* the exact first index where the exit predicate holds (else n). The scalar   */
/* loop survives and re-runs from that index, so it executes at most the hit   */
/* iteration plus the sub-block tail, and every exit path (break, return, a    */
/* flag store) replays natively. Soundness needs only two facts, both proved   */
/* here: iterations BEFORE the first hit are observably pure (address math +   */
/* the decoded loads + the compare + the increment, nothing trapping), and     */
/* the kernel's predicate is exactly the loop's exit predicate.                */
/*                                                                             */
/* Two source shapes:                                                          */
/*   Form A: while (i < n) { if (a[i] PRED rhs) { <anything that returns or   */
/*           breaks> } i++; }      -- hit when the condition is TRUE.          */
/*   Form B: while (i < n) { <pure> if (!(...)) -> exits via the condition    */
/*           branch jumping OUT of the body (e.g. `while (i < n && a[i] !=    */
/*           key)`) -- hit when the condition is FALSE (predicate inverted).   */
/* rhs forms: int literal, loop-invariant scalar symbol, or b[i] (mismatch).   */
/* Elements: int32 (8 lanes) or bytes (32 lanes; == / != only). Ordered        */
/* predicates are signed-gated; literals/scalars are width/signedness-gated    */
/* so the 32-bit lane compare agrees with the scalar 64-bit compare.           */
/* -------------------------------------------------------------------------- */

/* Predicate codes -- must match the kernel decoder in simd_int.c. */
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

static int vfind_pred_mirror(int p) { /* a P b == b P' a */
  switch (p) {
  case VFIND_P_LT: return VFIND_P_GT;
  case VFIND_P_GT: return VFIND_P_LT;
  case VFIND_P_LE: return VFIND_P_GE;
  case VFIND_P_GE: return VFIND_P_LE;
  default: return p; /* EQ / NE symmetric */
  }
}

/* Decode `temp` as the canonical a[iv] load for int32 (addr = base + (iv<<2),
 * size 4) or byte (addr = base + iv, size 1) elements. Returns the LOAD
 * instruction so callers can pin identity and signedness. */
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
  if (load->rhs.int_value == 4) { /* int32: index temp = iv << 2 */
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
  } else if (load->rhs.int_value == 1) { /* byte: addr = base + iv */
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
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_try_vectorize_find_at(IRFunction *function, size_t header_index,
                                    int *changed, int *claimed_out,
                                    int install) {
  const char *iv_symbol = NULL;
  size_t branch_index = 0;
  size_t jump_index = 0;
  IROperand bound = {0};
  int matched = 0;
  size_t cb = 0;            /* the single conditional branch in the body */
  int n_cond = 0;
  size_t exit_lo = 0, exit_hi = 0; /* Form A exit region (cb+1 .. L_idx) */
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

  if (!ir_float_reduction_frame(function, header_index, &iv_symbol,
                                &branch_index, &jump_index, &bound, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }

  /* exactly one conditional branch in the body */
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

  /* locate the branch target inside the body (Form A) or not (Form B), and
   * refuse any other label in the body. */
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
    /* The exit region runs ONLY on a hit (the kernel never skips a hit), so
     * its contents are unconstrained -- but it must actually EXIT: exactly
     * one terminator (RETURN, or JUMP out of the loop) as its last real
     * instruction, no other control flow, so a hit can never fall through
     * into the increment as if nothing happened with iterations skipped. */
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
        /* must leave the loop: target label not within [header, jump] */
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
      /* anything else (stores, calls, math) is fine: it only runs on a hit */
    }
    if (!saw_term) {
      ir_operand_destroy(&bound);
      return 1;
    }
  }

  /* decode the condition: load CMP rhs, or the folded `load != 0` form
   * (x != 0 lowers to branching on the raw loaded value -- the strlen shape) */
  if (vfind_decode_indexed_load(function, cb, br->lhs.name, iv_symbol, &a_base,
                                &a_u8, &a_load)) {
    pred = VFIND_P_NE; /* branch condition == the value: "value != 0" */
    other = NULL;      /* implicit literal 0 */
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
    if (cmp->lhs.kind == IR_OPERAND_TEMP && cmp->lhs.name &&
        vfind_decode_indexed_load(function, cb, cmp->lhs.name, iv_symbol,
                                  &a_base, &a_u8, &a_load)) {
      other = &cmp->rhs;
    } else if (cmp->rhs.kind == IR_OPERAND_TEMP && cmp->rhs.name &&
               vfind_decode_indexed_load(function, cb, cmp->rhs.name,
                                         iv_symbol, &a_base, &a_u8, &a_load)) {
      other = &cmp->lhs;
      pred = vfind_pred_mirror(pred);
    } else {
      ir_operand_destroy(&bound);
      return 1;
    }
  }
  /* the load must be the loop's own (in-body), not a stale pre-loop value */
  if ((size_t)(a_load - function->instructions) <= branch_index) {
    ir_operand_destroy(&bound);
    return 1;
  }

  /* classify the other side */
  if (!other) { /* implicit `!= 0` */
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

  /* ordered predicates: signed 32-bit only (vpcmpgtd is signed) */
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

  /* continue-path purity: outside the exit region, only the decoded loads,
   * non-trapping address math (+, <<), temp copies, the compare, the branch,
   * the increment, and the back-jump may appear. A stray load (a page the
   * kernel never touches) or a trapping op (/) in a SKIPPED iteration would
   * be an observable difference, so anything else refuses. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!form_b && i >= exit_lo && i < exit_hi) {
      continue; /* exit region: runs only on a hit, checked above */
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

  /* Form B: the branch exits when the condition is FALSE */
  if (form_b) {
    pred = vfind_pred_invert(pred);
  }

  /* locate the zero-init to replace (the frame already proved it exists on
   * the straight-line path into the header) */
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

  if (claimed_out) {
    *claimed_out = 1;
  }
  if (!install) { /* read-only probe (pointer-induction asks before converting) */
    ir_operand_destroy(&bound);
    ir_operand_destroy(&rhs_arg);
    return 1;
  }

  fused.op = IR_OP_SIMD_FIND;
  fused.location = function->instructions[header_index].location;
  fused.dest = ir_operand_symbol(iv_symbol);
  fused.lhs = bound; /* take ownership */
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
  fused.arguments[3] = rhs_arg; /* take ownership */

  /* Replace ONLY the init; the loop itself is untouched. */
  ir_instruction_destroy_storage(&function->instructions[init_index]);
  function->instructions[init_index] = fused;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

/* Read-only probe for pointer-induction: converting a claimable find loop to
 * a pointer walk would hide the indexed shape and leave it scalar. */
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

  /* The kernel replays the outer iterations as p = 0..P-1, so the outer iv
   * must provably start at 0. */
  if (!ir_iv_zero_at_header(function, header_index, p_sym)) {
    OL_DBG("outer iv does not start at 0");
    ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
  }
  /* The per-outer-iteration init region (outer branch -> inner header) must be
   * straight-line: the i0 and iacc-seed scans below take the textually-last
   * writer, which is only the executed value when no label/jump/branch can
   * reorder control flow through the region. */
  for (size_t i = outer_branch + 1; i < inner_header; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
        op == IR_OP_BRANCH_EQ) {
      OL_DBG("control flow in outer init region");
      ir_operand_destroy(&outerP); ir_operand_destroy(&innerN); return 1;
    }
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

  /* i init (i0) before the inner header: the LAST write to i in the init
   * region must be a constant assign (a later non-constant write would make
   * the recorded i0 stale). */
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
                                    iacc_sym, &init_idx) ||
        init_idx <= outer_branch) {
      /* The seed must be written INSIDE the per-outer-iteration init region;
       * a writer before the outer loop would mean iacc carries across outer
       * iterations (a serial dependence the per-lane reseed would break). */
      OL_DBG("no iacc init writer in the outer init region");
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
