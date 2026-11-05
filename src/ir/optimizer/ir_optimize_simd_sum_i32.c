#include "ir_optimize_internal.h"
#include "ir_loop_shape.h"

const char *ir_function_local_declared_type(const IRFunction *function,
                                                   const char *symbol_name) {
  const IRInstruction *declaration =
      ir_function_find_declaration(function, symbol_name, 1);

  return declaration ? declaration->text : NULL;
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

static int ir_symbol_assigned_once(const IRFunction *function,
                                   const char *symbol_name) {
  int writes = 0;
  if (!function || !symbol_name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        strcmp(ins->dest.name, symbol_name) == 0 && ++writes > 1) {
      return 0;
    }
  }
  return writes == 1;
}

int ir_symbol_is_settled_local(const IRFunction *function,
                               const char *symbol_name,
                               const char *expected_type) {
  const char *type = ir_function_local_declared_type(function, symbol_name);
  if (!type || (expected_type && strcmp(type, expected_type) != 0)) {
    return 0;
  }
  return ir_symbol_assigned_once(function, symbol_name);
}

int ir_symbol_is_sum_loop_bound(const IRFunction *function,
                                       const char *symbol_name) {
  return ir_function_symbol_is_parameter(function, symbol_name) ||
         ir_symbol_is_settled_local(function, symbol_name, "int32") ||
         ir_symbol_is_settled_local(function, symbol_name, "int64");
}

int ir_symbol_is_loop_bound(const IRFunction *function,
                            const char *symbol_name, size_t header_index,
                            size_t jump_index) {
  if (ir_symbol_is_sum_loop_bound(function, symbol_name)) {
    return 1;
  }
  {
    const char *type = ir_function_local_declared_type(function, symbol_name);
    if (!type || (strcmp(type, "int32") != 0 && strcmp(type, "int64") != 0)) {
      return 0;
    }
  }
  for (size_t i = header_index; i <= jump_index &&
                                i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        strcmp(ins->dest.name, symbol_name) == 0) {
      return 0;
    }
  }
  return 1;
}

int ir_symbol_is_sum_array_base(const IRFunction *function,
                                       const char *symbol_name) {
  return ir_function_symbol_is_parameter(function, symbol_name) ||
         ir_symbol_is_settled_local(function, symbol_name, "int32*") ||
         ir_symbol_is_settled_local(function, symbol_name, "uint32*");
}

int ir_label_is_while_header(const char *label) {
  if (!label) {
    return 0;
  }
  if (strncmp(label, "ir_while_", 9) == 0 ||
      strncmp(label, "ir_for_cond_", 12) == 0) {
    return 1;
  }
  const char *inlined = strstr(label, "_lbl_ir_");
  return inlined && (strncmp(inlined + 8, "while_", 6) == 0 ||
                     strncmp(inlined + 8, "for_cond_", 9) == 0);
}

int ir_fused_loop_exit_is_adjacent(const IRFunction *function,
                                   size_t jump_index, const char *exit_label) {
  if (!function || !exit_label) {
    return 0;
  }
  for (size_t i = jump_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    return ins->op == IR_OP_LABEL && ins->text &&
           strcmp(ins->text, exit_label) == 0;
  }
  return 0;
}

int ir_range_has_safety_call(const IRFunction *function, size_t start,
                                    size_t end) {
  if (!function) {
    return 0;
  }
  for (size_t i = start; i < end && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_safety_intrinsic(ins) != IR_SAFETY_INTRINSIC_NONE) return 1;
    if (ins->op == IR_OP_CALL && ins->text &&
        strncmp(ins->text, "mettle_safety_", 14) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_loop_body_is_unclaimable(const IRFunction *function, size_t start,
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

  return ir_range_has_safety_call(function, start, end);
}

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
      (compare->rhs.kind != IR_OPERAND_SYMBOL &&
       compare->rhs.kind != IR_OPERAND_INT) ||
      (compare->rhs.kind == IR_OPERAND_SYMBOL && !compare->rhs.name) ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }

  iv_symbol = compare->lhs.name;
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
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_symbol_is_loop_bound(function, compare->rhs.name, header_index,
                               jump_index)) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
    return 1;
  }

  if (ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
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

  sum_symbol = NULL;
  base_symbol = NULL;
  size_t sum_load_index = (size_t)-1;
  size_t indexed_load_index = (size_t)-1;
  size_t body_load_count = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_LOAD) {
      body_load_count++;
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
          strcmp(prod->text, "int64") == 0 &&
          ir_operand_is_temp(&prod->lhs)) {
        const IRInstruction *load =
            ir_find_temp_producer_before(function, i, prod->lhs.name);
        if (load && load->op == IR_OP_LOAD &&
            load->rhs.kind == IR_OPERAND_INT && load->rhs.int_value == 4 &&
            !load->is_float && !load->is_unsigned) {
          ok = 1;
          sum_load_index = (size_t)(load - function->instructions);
        }
      } else if (prod && prod->op == IR_OP_LOAD &&
                 prod->rhs.kind == IR_OPERAND_INT &&
                 prod->rhs.int_value == 4 && !prod->is_float &&
                 !prod->is_unsigned &&
                 ir_sum_accumulator_is_int64(function, ins->dest.name)) {
        ok = 1;
        sum_load_index = (size_t)(prod - function->instructions);
      } else if (prod && prod->op == IR_OP_CAST && prod->text &&
                 strcmp(prod->text, "int32") == 0 &&
                 ir_operand_is_temp(&prod->lhs) &&
                 ir_sum_accumulator_is_int64(function, ins->dest.name)) {
        const IRInstruction *load =
            ir_find_temp_producer_before(function, i, prod->lhs.name);
        if (load && load->op == IR_OP_LOAD &&
            load->rhs.kind == IR_OPERAND_INT && load->rhs.int_value == 4 &&
            !load->is_float && !load->is_unsigned) {
          ok = 1;
          sum_load_index = (size_t)(load - function->instructions);
        }
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
              ir_operand_is_symbol(&addr->lhs)) {
            load = probe;
            base_symbol = addr->lhs.name;
            has_indexed_load = 1;
            indexed_load_index = j;
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
  if (sum_load_index != indexed_load_index || body_load_count != 1) {
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

  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
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

int ir_instruction_is_safety_scaffolding(const IRInstruction *instruction) {
  if (!instruction) {
    return 0;
  }
  if (instruction->op == IR_OP_CALL && instruction->text &&
      strncmp(instruction->text, "mettle_safety_", 14) == 0) {
    return 1;
  }
  return ir_operand_is_temp(&instruction->dest) &&
         strncmp(instruction->dest.name, IR_SAFETY_TEMP_PREFIX,
                 sizeof(IR_SAFETY_TEMP_PREFIX) - 1) == 0;
}

int ir_instruction_is_ptr_count_guard(const IRInstruction *instruction) {
  return instruction && instruction->op == IR_OP_BRANCH_ZERO &&
         ir_operand_is_temp(&instruction->lhs) &&
         strncmp(instruction->lhs.name, "__ptr_t", 7) == 0 &&
         strstr(instruction->lhs.name, "_any") != NULL;
}

int ir_iv_zero_at_header(const IRFunction *function, size_t header_index,
                         const char *iv) {
  for (size_t i = header_index; i-- > 0;) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ir_instruction_is_safety_scaffolding(ins) ||
        ir_instruction_is_ptr_count_guard(ins)) {
      continue;
    }
    if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
        ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
      return 0;
    }
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, iv)) {
      if (ins->op != IR_OP_ASSIGN) {
        return 0;
      }
      if (ins->lhs.kind == IR_OPERAND_INT) {
        return ins->lhs.int_value == 0;
      }
      if (ir_operand_is_temp(&ins->lhs)) {
        const IRInstruction *p =
            ir_find_temp_producer_before(function, i, ins->lhs.name);
        return p && p->op == IR_OP_CAST && !p->is_float &&
               p->lhs.kind == IR_OPERAND_INT && p->lhs.int_value == 0;
      }
      return 0;
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
      (compare->rhs.kind != IR_OPERAND_SYMBOL &&
       compare->rhs.kind != IR_OPERAND_INT) ||
      (compare->rhs.kind == IR_OPERAND_SYMBOL && !compare->rhs.name) ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }

  iv_symbol = compare->lhs.name;
  exit_label = branch->text;

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
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_symbol_is_loop_bound(function, compare->rhs.name, header_index,
                               jump_index)) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
    return 1;
  }

  if (ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
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

  size_t sum_load_index = (size_t)-1;
  size_t indexed_load_index = (size_t)-1;
  size_t body_load_count = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_LOAD) {
      body_load_count++;
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
          strcmp(cast->text, "int64") != 0 ||
          cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name) {
        continue;
      }
      const IRInstruction *load =
          ir_find_temp_producer_before(function, i, cast->lhs.name);
      if (!load || load->op != IR_OP_LOAD) {
        continue;
      }
      has_int64_cast = 1;
      sum_symbol = ins->dest.name;
      sum_load_index = (size_t)(load - function->instructions);
    }
    if (ins->op == IR_OP_LOAD && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 1 && ins->lhs.kind == IR_OPERAND_TEMP &&
        ins->lhs.name) {
      const IRInstruction *addr =
          ir_find_temp_producer_before(function, i, ins->lhs.name);
      if (addr && addr->op == IR_OP_BINARY && addr->text &&
          strcmp(addr->text, "+") == 0 &&
          ir_operand_is_symbol_named(&addr->rhs, iv_symbol) &&
          ir_operand_is_symbol(&addr->lhs)) {
        base_symbol = addr->lhs.name;
        has_indexed_load = 1;
        indexed_load_index = i;
      }
    }
  }

  if (!sum_symbol || !base_symbol || !has_int64_cast || !has_indexed_load) {
    return 1;
  }
  if (sum_load_index != indexed_load_index || body_load_count != 1) {
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
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
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

#define BYTE_MAP_MAX_STEPS 16

static int ir_type_name_is_integer(const char *name) {
  return name && (strcmp(name, "int8") == 0 || strcmp(name, "uint8") == 0 ||
                  strcmp(name, "int16") == 0 || strcmp(name, "uint16") == 0 ||
                  strcmp(name, "int32") == 0 || strcmp(name, "uint32") == 0 ||
                  strcmp(name, "int64") == 0 || strcmp(name, "uint64") == 0 ||
                  strcmp(name, "bool") == 0);
}

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
  const char *cur = NULL;
  int op_codes[BYTE_MAP_MAX_STEPS];
  int op_consts[BYTE_MAP_MAX_STEPS];
  int nsteps = 0;
  int have_store = 0;
  int dirty = 0;
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
      (compare->rhs.kind != IR_OPERAND_SYMBOL &&
       compare->rhs.kind != IR_OPERAND_INT) ||
      (compare->rhs.kind == IR_OPERAND_SYMBOL && !compare->rhs.name) ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }
  iv_symbol = compare->lhs.name;
  exit_label = branch->text;

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
      ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_symbol_is_loop_bound(function, compare->rhs.name, header_index,
                               jump_index)) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, exit_label)) {
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

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        i == increment_index) {
      continue;
    }

    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ir_operand_is_temp(&ins->dest) &&
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

    if (ins->op == IR_OP_LOAD && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 1 && addr_temp &&
        ir_operand_is_temp_named(&ins->lhs, addr_temp) &&
        ir_operand_is_temp(&ins->dest)) {
      cur = ins->dest.name;
      continue;
    }

    if (ins->op == IR_OP_STORE && ins->rhs.kind == IR_OPERAND_INT &&
        ins->rhs.int_value == 1 && addr_temp &&
        ir_operand_is_temp_named(&ins->dest, addr_temp) && cur &&
        ir_operand_is_temp_named(&ins->lhs, cur)) {
      have_store = 1;
      dirty = 0;
      continue;
    }

    if (ins->op == IR_OP_CAST && !ins->is_float && cur &&
        ir_operand_is_temp(&ins->dest) &&
        ir_operand_is_temp_named(&ins->lhs, cur) &&
        ir_type_name_is_integer(ins->text)) {
      cur = ins->dest.name;
      continue;
    }

    if (ins->op == IR_OP_BINARY && !ins->is_float &&
        ir_operand_is_temp(&ins->dest) && cur) {
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

    return 1;
  }

  if (!have_store || dirty || nsteps < 1 || !base_symbol ||
      !ir_symbol_is_uint8_ptr(function, base_symbol)) {
    return 1;
  }
  if (ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol)) {
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

static int ir_try_vectorize_lcg_at(IRFunction *function, size_t header_index,
                                   int *changed) {
  IRLoopShape loop;
  if (!ir_loop_shape_at(function, header_index, &loop)) {
    return 1;
  }
  if (strcmp(loop.compare->text, "<") != 0) {
    return 1;
  }
  IRInstruction *header = loop.header;
  IRInstruction *compare = loop.compare;
  IRInstruction *branch = loop.branch;
  size_t branch_index = loop.branch_index;
  const char *loop_label = loop.loop_label;
  const char *iv_symbol = loop.iv_symbol;
  if (compare->rhs.kind != IR_OPERAND_SYMBOL &&
      compare->rhs.kind != IR_OPERAND_INT) {
    return 1;
  }

  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      ir_loop_body_is_unclaimable(function, branch_index + 1, jump_index)) {
    return 1;
  }
  if (!ir_fused_loop_exit_is_adjacent(function, jump_index, branch->text)) {
    return 1;
  }

  const char *state_sym = NULL, *sum_sym = NULL;
  const char *mul_tmp = NULL, *mask_tmp = NULL, *cast_tmp = NULL;
  long long A = 0, C = 0, MASK = 0;
  int have_mul = 0, have_upd = 0, have_mask = 0, have_cast = 0, have_acc = 0;
  int have_inc = 0;

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_BINARY && !in->is_float && in->text &&
        strcmp(in->text, "*") == 0 && in->dest.kind == IR_OPERAND_TEMP &&
        in->dest.name && ir_operand_is_symbol(&in->lhs) &&
        in->rhs.kind == IR_OPERAND_INT) {
      state_sym = in->lhs.name;
      A = in->rhs.int_value;
      mul_tmp = in->dest.name;
      have_mul = 1;
      break;
    }
  }
  if (!have_mul) {
    return 1;
  }
  {
    const char *st = ir_function_local_declared_type(function, state_sym);
    if (!st || strcmp(st, "uint32") != 0) {
      return 1;
    }
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_BINARY && !in->is_float && in->text &&
        strcmp(in->text, "+") == 0 && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, state_sym) == 0 &&
        ir_operand_is_temp_named(&in->lhs, mul_tmp) &&
        in->rhs.kind == IR_OPERAND_INT) {
      C = in->rhs.int_value;
      have_upd = 1;
      break;
    }
  }
  if (!have_upd) {
    return 1;
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_BINARY && !in->is_float && in->text &&
        strcmp(in->text, "&") == 0 && in->dest.kind == IR_OPERAND_TEMP &&
        in->dest.name && ir_operand_is_symbol_named(&in->lhs, state_sym) &&
        in->rhs.kind == IR_OPERAND_INT) {
      MASK = in->rhs.int_value;
      mask_tmp = in->dest.name;
      have_mask = 1;
      break;
    }
  }
  if (!have_mask) {
    return 1;
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_CAST && in->dest.kind == IR_OPERAND_TEMP &&
        in->dest.name && ir_operand_is_temp_named(&in->lhs, mask_tmp)) {
      cast_tmp = in->dest.name;
      have_cast = 1;
      break;
    }
  }
  if (!have_cast) {
    return 1;
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_BINARY && !in->is_float && in->text &&
        strcmp(in->text, "+") == 0 && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && ir_operand_is_symbol(&in->lhs) &&
        ir_operand_names_match(&in->lhs, &in->dest) &&
        ir_operand_is_temp_named(&in->rhs, cast_tmp)) {
      sum_sym = in->dest.name;
      have_acc = 1;
      break;
    }
  }
  if (!have_acc) {
    return 1;
  }
  {
    const char *st = ir_function_local_declared_type(function, sum_sym);
    if (!st || strcmp(st, "int64") != 0) {
      return 1;
    }
  }
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    if (ir_try_parse_direct_unit_increment(&function->instructions[i],
                                           iv_symbol)) {
      have_inc = 1;
      break;
    }
  }
  if (!have_inc) {
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue;
    }
    if (in->op == IR_OP_STORE || in->op == IR_OP_LOAD ||
        in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT ||
        in->op == IR_OP_JUMP || in->op == IR_OP_BRANCH_ZERO ||
        in->op == IR_OP_BRANCH_EQ) {
      return 1;
    }
    if (!in->dest.name) {
      return 1;
    }
    int known = (in->dest.kind == IR_OPERAND_TEMP &&
                 ((mul_tmp && strcmp(in->dest.name, mul_tmp) == 0) ||
                  (mask_tmp && strcmp(in->dest.name, mask_tmp) == 0) ||
                  (cast_tmp && strcmp(in->dest.name, cast_tmp) == 0) ||
                  (compare->dest.name &&
                   ir_operand_names_match(&in->dest, &compare->dest)))) ||
                (in->dest.kind == IR_OPERAND_SYMBOL &&
                 (strcmp(in->dest.name, state_sym) == 0 ||
                  strcmp(in->dest.name, sum_sym) == 0 ||
                  strcmp(in->dest.name, iv_symbol) == 0));
    if (!known) {
      return 1;
    }
  }

  if (!ir_iv_zero_at_header(function, header_index, iv_symbol) ||
      ir_symbol_live_after_loop(function, jump_index + 1, iv_symbol) ||
      ir_symbol_live_after_loop(function, jump_index + 1, state_sym)) {
    return 1;
  }

  IRInstruction fused = {0};
  fused.op = IR_OP_SIMD_LCG_U32;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(sum_sym);
  if (compare->rhs.kind == IR_OPERAND_INT) {
    fused.lhs = ir_operand_int(compare->rhs.int_value);
  } else {
    fused.lhs = ir_operand_symbol(compare->rhs.name);
  }
  fused.rhs = ir_operand_symbol(state_sym);
  fused.arguments = calloc(3, sizeof(IROperand));
  if (!fused.arguments) {
    return 0;
  }
  fused.argument_count = 3;
  fused.arguments[0] = ir_operand_int(A);
  fused.arguments[1] = ir_operand_int(C);
  fused.arguments[2] = ir_operand_int(MASK);

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

int ir_simd_lcg_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_lcg_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}

