#include "ir_optimize_internal.h"

/* -------------------------------------------------------------------------- */
/* int32 array horizontal sum -> IR_OP_SIMD_SUM_I32                           */
/* -------------------------------------------------------------------------- */

const char *ir_function_local_declared_type(const IRFunction *function,
                                                   const char *symbol_name) {
  if (!function || !symbol_name) {
    return NULL;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol_named(&ins->dest, symbol_name) && ins->text) {
      return ins->text;
    }
  }

  return NULL;
}

int ir_function_symbol_is_parameter(const IRFunction *function,
                                           const char *symbol_name) {
  if (!function || !symbol_name || !function->parameter_names) {
    return 0;
  }

  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names[i] &&
        strcmp(function->parameter_names[i], symbol_name) == 0) {
      return 1;
    }
  }

  return 0;
}

int ir_function_symbol_is_inlined_param(const IRFunction *function,
                                               const char *symbol_name,
                                               const char *expected_type,
                                               const char *param_tag) {
  const char *type = NULL;
  const char *tag = NULL;

  if (!function || !symbol_name || !expected_type || !param_tag) {
    return 0;
  }

  type = ir_function_local_declared_type(function, symbol_name);
  if (!type || strcmp(type, expected_type) != 0) {
    return 0;
  }

  tag = strstr(symbol_name, param_tag);
  return tag != NULL;
}

int ir_symbol_is_sum_loop_bound(const IRFunction *function,
                                       const char *symbol_name) {
  return ir_function_symbol_is_parameter(function, symbol_name) ||
         ir_function_symbol_is_inlined_param(function, symbol_name, "int32",
                                             "_param_len") ||
         ir_function_symbol_is_inlined_param(function, symbol_name, "int64",
                                             "_param_n") ||
         ir_function_symbol_is_inlined_param(function, symbol_name, "int64",
                                             "_param_count");
}

int ir_symbol_is_sum_array_base(const IRFunction *function,
                                       const char *symbol_name) {
  return ir_function_symbol_is_parameter(function, symbol_name) ||
         ir_function_symbol_is_inlined_param(function, symbol_name, "int32*",
                                             "_param_data");
}

int ir_label_is_while_header(const char *label) {
  if (!label) {
    return 0;
  }
  if (strncmp(label, "ir_while_", 9) == 0) {
    return 1;
  }
  if (strstr(label, "_lbl_ir_while_") != NULL) {
    return 1;
  }
  /* Counted `for` loops (including desugared range-for, `for i in lo..hi`)
   * lower to the same header/compare/branch_zero/body/increment/back-jump shape
   * as a `while` once the unused step label is cleaned up. Treat their cond
   * header as a loop header too so every vectorizer considers them; each
   * recognizer still fully validates the loop's structure before firing. */
  if (strncmp(label, "ir_for_cond_", 12) == 0) {
    return 1;
  }
  return strstr(label, "_lbl_ir_for_cond_") != NULL;
}

int ir_loop_body_has_nested_while(IRFunction *function, size_t start,
                                         size_t end) {
  if (!function) {
    return 0;
  }

  for (size_t i = start; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_label_is_while_header(ins->text)) {
      return 1;
    }
  }

  return 0;
}

/* True if the reduction accumulator `sym` is declared int64 (a local). Used to
 * admit the cast-free widening sum `s += a[i]` only when `s` is genuinely 64-bit
 * (so summing int32 into int64 cannot overflow-diverge from the scalar loop). */
static int ir_sum_accumulator_is_int64(const IRFunction *function,
                                       const char *sym) {
  const char *t = ir_function_local_declared_type(function, sym);
  return t && strcmp(t, "int64") == 0;
}

static int ir_try_vectorize_sum_i32_at(IRFunction *function, size_t header_index,
                                       int *changed) {
  size_t compare_index = 0;
  size_t branch_index = 0;
  size_t jump_index = (size_t)-1;
  size_t increment_index = 0;
  const char *iv_symbol = NULL;
  const char *sum_symbol = NULL;
  const char *base_symbol = NULL;
  const char *sum_type = NULL;
  const char *loop_label = NULL;
  const char *exit_label = NULL;
  IRInstruction fused = {0};
  int has_indexed_load = 0;
  int has_int64_cast = 0;

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
      compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }

  iv_symbol = compare->lhs.name;
  exit_label = branch->text;

  if (!ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
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

  /* Body must be: idx = iv << 2; ptr = base + idx; load; cast (int64); sum += cast. */
  sum_symbol = NULL;
  base_symbol = NULL;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_STORE || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_JUMP) {
      return 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->rhs.kind == IR_OPERAND_TEMP &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name)) {
      const IRInstruction *prod =
          ir_find_temp_producer_before(function, i, ins->rhs.name);
      int ok = 0;
      if (prod && prod->op == IR_OP_CAST && prod->text &&
          strcmp(prod->text, "int64") == 0) {
        ok = 1; /* s += (int64)a[i] */
      } else if (prod && prod->op == IR_OP_LOAD &&
                 prod->rhs.kind == IR_OPERAND_INT &&
                 prod->rhs.int_value == 4 && !prod->is_float &&
                 !prod->is_unsigned &&
                 ir_sum_accumulator_is_int64(function, ins->dest.name)) {
        /* s += a[i] : a signed int32 load widened directly into an int64
         * accumulator -- semantically identical to the (int64)-cast form (the
         * kernel sums int32 into int64 with sign-extension). Lets the natural
         * cast-free reduction vectorize. */
        ok = 1;
      }
      if (!ok) {
        continue;
      }
      has_int64_cast = 1;
      sum_symbol = ins->dest.name;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "<<") == 0 &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == 2 &&
        ir_operand_is_symbol_named(&ins->lhs, iv_symbol)) {
      const IRInstruction *load = NULL;
      for (size_t j = i + 1; j < jump_index; j++) {
        const IRInstruction *probe = &function->instructions[j];
        if (probe->op == IR_OP_LOAD && probe->rhs.kind == IR_OPERAND_INT &&
            probe->rhs.int_value == 4 && probe->lhs.kind == IR_OPERAND_TEMP &&
            probe->lhs.name) {
          const IRInstruction *addr = ir_find_temp_producer_before(
              function, j, probe->lhs.name);
          if (addr && addr->op == IR_OP_BINARY && addr->text &&
              strcmp(addr->text, "+") == 0 &&
              addr->rhs.kind == IR_OPERAND_TEMP &&
              ir_operand_is_temp_named(&addr->rhs, ins->dest.name) &&
              addr->lhs.kind == IR_OPERAND_SYMBOL && addr->lhs.name) {
            load = probe;
            base_symbol = addr->lhs.name;
            has_indexed_load = 1;
            break;
          }
        }
      }
      (void)load;
    }
  }

  if (!sum_symbol || !base_symbol || !has_int64_cast || !has_indexed_load) {
    return 1;
  }

  if (strcmp(sum_symbol, iv_symbol) == 0) {
    return 1;
  }

  sum_type = ir_function_local_declared_type(function, sum_symbol);
  if (!sum_type || strcmp(sum_type, "int64") != 0) {
    return 1;
  }

  if (!ir_symbol_is_sum_array_base(function, base_symbol)) {
    return 1;
  }

  if (ir_symbol_read_after(function, jump_index + 1, iv_symbol)) {
    return 1;
  }

  fused.op = IR_OP_SIMD_SUM_I32;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(base_symbol);
  if (!ir_operand_clone(&compare->rhs, &fused.rhs)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }

  ir_instruction_destroy_storage(header);
  *header = fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_sum_i32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_sum_i32_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* uint8 array horizontal sum -> IR_OP_SIMD_SUM_U8                            */
/* -------------------------------------------------------------------------- */

/* True if @name is a uint8* parameter or local. vpsadbw sums bytes as
 * unsigned, so this gate keeps the kernel bit-identical to the scalar
 * (int64)(uint8)load reduction (an int8* would sign-extend and diverge). */
static int ir_symbol_is_uint8_ptr(const IRFunction *function,
                                  const char *name) {
  if (!function || !name) {
    return 0;
  }
  if (function->parameter_names && function->parameter_types) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      if (function->parameter_names[i] &&
          strcmp(function->parameter_names[i], name) == 0) {
        return function->parameter_types[i] &&
               strcmp(function->parameter_types[i], "uint8*") == 0;
      }
    }
  }
  {
    const char *type = ir_function_local_declared_type(function, name);
    return type && strcmp(type, "uint8*") == 0;
  }
}

/* True if @iv is provably 0 at the loop header: the nearest preceding write to
 * @iv (in straight-line order) is `iv <- 0`. Bails on any control-flow join
 * before finding it, so the fused kernel only ever sums base[0..len). */
static int ir_iv_zero_at_header(const IRFunction *function, size_t header_index,
                                const char *iv) {
  for (size_t i = header_index; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
      return 0;
    }
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, iv)) {
      return ins->op == IR_OP_ASSIGN && ins->lhs.kind == IR_OPERAND_INT &&
             ins->lhs.int_value == 0;
    }
  }
  return 0;
}

static int ir_try_vectorize_sum_u8_at(IRFunction *function, size_t header_index,
                                      int *changed) {
  size_t compare_index = 0;
  size_t branch_index = 0;
  size_t jump_index = (size_t)-1;
  size_t increment_index = 0;
  const char *iv_symbol = NULL;
  const char *sum_symbol = NULL;
  const char *base_symbol = NULL;
  const char *sum_type = NULL;
  const char *loop_label = NULL;
  const char *exit_label = NULL;
  IRInstruction fused = {0};
  int has_indexed_load = 0;
  int has_int64_cast = 0;

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
      compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }

  iv_symbol = compare->lhs.name;
  exit_label = branch->text;

  if (!ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
    return 1;
  }
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
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

  /* Body must be: addr = base + iv; load[1]; cast(int64); sum += cast. The only
   * memory op is that single byte load; reject stores, calls, other loads. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_STORE || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_JUMP) {
      return 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->rhs.kind == IR_OPERAND_TEMP &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name)) {
      const IRInstruction *cast =
          ir_find_temp_producer_before(function, i, ins->rhs.name);
      if (!cast || cast->op != IR_OP_CAST || !cast->text ||
          strcmp(cast->text, "int64") != 0) {
        continue;
      }
      has_int64_cast = 1;
      sum_symbol = ins->dest.name;
    }
    if (ins->op == IR_OP_LOAD && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 1 && ins->lhs.kind == IR_OPERAND_TEMP &&
        ins->lhs.name) {
      const IRInstruction *addr =
          ir_find_temp_producer_before(function, i, ins->lhs.name);
      if (addr && addr->op == IR_OP_BINARY && addr->text &&
          strcmp(addr->text, "+") == 0 &&
          ir_operand_is_symbol_named(&addr->rhs, iv_symbol) &&
          addr->lhs.kind == IR_OPERAND_SYMBOL && addr->lhs.name) {
        base_symbol = addr->lhs.name;
        has_indexed_load = 1;
      }
    }
  }

  if (!sum_symbol || !base_symbol || !has_int64_cast || !has_indexed_load) {
    return 1;
  }
  if (strcmp(sum_symbol, iv_symbol) == 0 ||
      strcmp(base_symbol, iv_symbol) == 0) {
    return 1;
  }

  sum_type = ir_function_local_declared_type(function, sum_symbol);
  if (!sum_type || strcmp(sum_type, "int64") != 0) {
    return 1;
  }
  if (!ir_symbol_is_uint8_ptr(function, base_symbol)) {
    return 1;
  }
  if (ir_symbol_read_after(function, jump_index + 1, iv_symbol)) {
    return 1;
  }

  fused.op = IR_OP_SIMD_SUM_U8;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(base_symbol);
  if (!ir_operand_clone(&compare->rhs, &fused.rhs)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }

  ir_instruction_destroy_storage(header);
  *header = fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_sum_u8_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_sum_u8_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* uint8 in-place element-wise map -> IR_OP_SIMD_BYTE_MAP                      */
/* -------------------------------------------------------------------------- */

#define BYTE_MAP_MAX_STEPS 16

/* Maps a binary operator's text to its IRByteMapOp, or -1 if unsupported.
 * *commutative_out reports whether the constant may be on either side. */
static int ir_byte_map_op_code(const char *text, int *commutative_out) {
  if (!text) {
    return -1;
  }
  if (strcmp(text, "+") == 0) { *commutative_out = 1; return IR_BYTE_MAP_ADD; }
  if (strcmp(text, "-") == 0) { *commutative_out = 0; return IR_BYTE_MAP_SUB; }
  if (strcmp(text, "*") == 0) { *commutative_out = 1; return IR_BYTE_MAP_MUL; }
  if (strcmp(text, "^") == 0) { *commutative_out = 1; return IR_BYTE_MAP_XOR; }
  if (strcmp(text, "&") == 0) { *commutative_out = 1; return IR_BYTE_MAP_AND; }
  if (strcmp(text, "|") == 0) { *commutative_out = 1; return IR_BYTE_MAP_OR; }
  return -1;
}

static int ir_try_vectorize_byte_map_at(IRFunction *function,
                                        size_t header_index, int *changed) {
  size_t compare_index = 0;
  size_t branch_index = 0;
  size_t jump_index = (size_t)-1;
  size_t increment_index = 0;
  const char *iv_symbol = NULL;
  const char *base_symbol = NULL;
  const char *loop_label = NULL;
  const char *exit_label = NULL;
  const char *addr_temp = NULL;
  const char *cur = NULL; /* temp holding the live byte value */
  int op_codes[BYTE_MAP_MAX_STEPS];
  int op_consts[BYTE_MAP_MAX_STEPS];
  int nsteps = 0;
  int have_store = 0;
  int dirty = 0; /* a chain op has run since the last store of `cur` */
  IRInstruction fused = {0};

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
      compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }
  iv_symbol = compare->lhs.name;
  exit_label = branch->text;

  if (!ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
    return 1;
  }
  if (!ir_iv_zero_at_header(function, header_index, iv_symbol)) {
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
  if (jump_index == (size_t)-1 ||
      ir_loop_body_has_nested_while(function, branch_index + 1, jump_index)) {
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

  /* Walk the body, extracting one straight-line chain from the byte load to the
   * final byte store. Anything outside the {address, byte load/store, chain op,
   * loop increment} shapes aborts the match. */
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        i == increment_index) {
      continue;
    }

    /* addr = base + iv (either operand order). */
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
        ins->lhs.kind == IR_OPERAND_SYMBOL && ins->rhs.kind == IR_OPERAND_SYMBOL) {
      const char *b = NULL;
      if (ir_operand_is_symbol_named(&ins->rhs, iv_symbol)) {
        b = ins->lhs.name;
      } else if (ir_operand_is_symbol_named(&ins->lhs, iv_symbol)) {
        b = ins->rhs.name;
      } else {
        return 1;
      }
      if (base_symbol && strcmp(base_symbol, b) != 0) {
        return 1;
      }
      base_symbol = b;
      addr_temp = ins->dest.name;
      continue;
    }

    /* v = *addr [1] (load or reload). */
    if (ins->op == IR_OP_LOAD && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 1 && addr_temp &&
        ir_operand_is_temp_named(&ins->lhs, addr_temp) &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
      cur = ins->dest.name;
      continue;
    }

    /* *addr <- cur [1] (store of the live value). */
    if (ins->op == IR_OP_STORE && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 1 && addr_temp &&
        ir_operand_is_temp_named(&ins->dest, addr_temp) && cur &&
        ir_operand_is_temp_named(&ins->lhs, cur)) {
      have_store = 1;
      dirty = 0;
      continue;
    }

    /* cur = cur <op> const (one constant operand, the other is the live value). */
    if (ins->op == IR_OP_BINARY && !ins->is_float &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name && cur) {
      int commutative = 0;
      int code = ir_byte_map_op_code(ins->text, &commutative);
      int lhs_is_cur = ir_operand_is_temp_named(&ins->lhs, cur);
      int rhs_is_cur = ir_operand_is_temp_named(&ins->rhs, cur);
      int k = 0;
      int matched = 0;
      if (code >= 0 && lhs_is_cur && ins->rhs.kind == IR_OPERAND_INT) {
        k = (int)ins->rhs.int_value;
        matched = 1;
      } else if (code >= 0 && commutative && rhs_is_cur &&
                 ins->lhs.kind == IR_OPERAND_INT) {
        k = (int)ins->lhs.int_value;
        matched = 1;
      }
      if (matched) {
        if (nsteps >= BYTE_MAP_MAX_STEPS) {
          return 1;
        }
        op_codes[nsteps] = code;
        op_consts[nsteps] = k;
        nsteps++;
        cur = ins->dest.name;
        dirty = 1;
        continue;
      }
    }

    /* Anything else in the body defeats the match. */
    return 1;
  }

  if (!have_store || dirty || nsteps < 1 || !base_symbol ||
      !ir_symbol_is_uint8_ptr(function, base_symbol)) {
    return 1;
  }
  /* The loop counter must be dead after the loop (the fused op drops it). */
  if (ir_symbol_read_after(function, jump_index + 1, iv_symbol)) {
    return 1;
  }

  fused.op = IR_OP_SIMD_BYTE_MAP;
  fused.location = header->location;
  fused.lhs = ir_operand_symbol(base_symbol);
  if (!ir_operand_clone(&compare->rhs, &fused.rhs)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = (size_t)(2 * nsteps);
  fused.arguments = calloc(fused.argument_count, sizeof(IROperand));
  if (!fused.arguments) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  for (int s = 0; s < nsteps; s++) {
    fused.arguments[2 * s] = ir_operand_int(op_codes[s]);
    fused.arguments[2 * s + 1] = ir_operand_int(op_consts[s]);
  }

  ir_instruction_destroy_storage(header);
  *header = fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_simd_byte_map_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_byte_map_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

