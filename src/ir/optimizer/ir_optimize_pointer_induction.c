#include "ir_optimize_internal.h"

int ir_instruction_insert_move(IRFunction *function, size_t index,
                                      IRInstruction *instruction) {
  if (!function || !instruction || index > function->instruction_count) {
    return 0;
  }

  if (function->instruction_count >= function->instruction_capacity) {
    size_t new_capacity =
        function->instruction_capacity ? function->instruction_capacity * 2 : 64;
    IRInstruction *grown =
        realloc(function->instructions, new_capacity * sizeof(IRInstruction));
    if (!grown) {
      return 0;
    }
    function->instructions = grown;
    function->instruction_capacity = new_capacity;
  }

  memmove(&function->instructions[index + 1], &function->instructions[index],
          (function->instruction_count - index) * sizeof(IRInstruction));
  function->instructions[index] = *instruction;
  function->instruction_count++;

  instruction->tensor = NULL;
  instruction->op = IR_OP_NOP;
  instruction->dest = ir_operand_none();
  instruction->lhs = ir_operand_none();
  instruction->rhs = ir_operand_none();
  instruction->text = NULL;
  instruction->arguments = NULL;
  instruction->argument_count = 0;
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  return 1;
}

int ir_binary_is_unit_increment_of_iv(const IRInstruction *instruction,
                                             const char *iv_symbol) {
  if (!instruction || instruction->op != IR_OP_BINARY || instruction->is_float ||
      !instruction->text || strcmp(instruction->text, "+") != 0 ||
      !ir_operand_is_symbol_named(&instruction->lhs, iv_symbol) ||
      !ir_operand_is_int_value(&instruction->rhs, 1)) {
    return 0;
  }
  if (ir_operand_is_symbol_named(&instruction->dest, iv_symbol)) {
    return 1;
  }
  return instruction->dest.kind == IR_OPERAND_TEMP;
}

int ir_match_forward_i32_index(const IRInstruction *index_prod,
                                      const char *iv) {
  if (!index_prod || index_prod->op != IR_OP_BINARY || index_prod->is_float ||
      !index_prod->text || !iv ||
      !ir_operand_is_symbol_named(&index_prod->lhs, iv)) {
    return 0;
  }
  if (strcmp(index_prod->text, "<<") == 0 &&
      ir_operand_is_int_value(&index_prod->rhs, 2)) {
    return 1;
  }
  if (strcmp(index_prod->text, "*") == 0 &&
      ir_operand_is_int_value(&index_prod->rhs, 4)) {
    return 1;
  }
  return 0;
}

int ir_resolve_indexed_address_temp(const IRFunction *function,
                                             size_t before_index,
                                             const char *iv, const char *bound,
                                             const char *addr_temp,
                                             const char **base_out,
                                             int *elem_size_out,
                                             int *step_out) {
  const IRInstruction *add = NULL;
  const IRInstruction *index = NULL;
  const char *base = NULL;
  const char *other = NULL;

  (void)bound;

  if (!function || !iv || !addr_temp || !base_out) {
    return 0;
  }

  add = ir_find_temp_producer_before(function, before_index, addr_temp);
  if (!add || add->op != IR_OP_BINARY || add->is_float || !add->text ||
      strcmp(add->text, "+") != 0 ||
      add->dest.kind != IR_OPERAND_TEMP ||
      !ir_operand_is_temp_named(&add->dest, addr_temp)) {
    return 0;
  }

  if (ir_operand_is_symbol(&add->lhs) &&
      ir_operand_is_temp(&add->rhs)) {
    base = add->lhs.name;
    other = add->rhs.name;
  } else if (ir_operand_is_symbol(&add->rhs) &&
             ir_operand_is_temp(&add->lhs)) {
    base = add->rhs.name;
    other = add->lhs.name;
  } else if (ir_operand_is_symbol(&add->lhs) &&
             ir_operand_is_symbol_named(&add->rhs, iv)) {
    if (elem_size_out) {
      *elem_size_out = 1;
    }
    if (step_out) {
      *step_out = 1;
    }
    *base_out = add->lhs.name;
    return 1;
  } else {
    return 0;
  }

  index = ir_find_temp_producer_before(function, before_index, other);
  if ((!index || index->op == IR_OP_ASSIGN) && other) {
    const IRInstruction *assign =
        ir_find_temp_producer_before(function, before_index, other);
    if (assign && assign->op == IR_OP_ASSIGN &&
        assign->dest.kind == IR_OPERAND_TEMP &&
        ir_operand_is_temp_named(&assign->dest, other) &&
        ir_operand_is_temp(&assign->rhs)) {
      index = ir_find_temp_producer_before(function, before_index,
                                           assign->rhs.name);
    }
  }
  if (index && ir_match_forward_i32_index(index, iv)) {
    if (elem_size_out) {
      *elem_size_out = 4;
    }
    if (step_out) {
      *step_out = 4;
    }
    *base_out = base;
    return 1;
  }

  if (index && index->op == IR_OP_BINARY && index->text &&
      strcmp(index->text, "*") == 0 &&
      ir_operand_is_symbol_named(&index->lhs, iv) &&
      ir_operand_is_int_value(&index->rhs, 4)) {
    if (elem_size_out) {
      *elem_size_out = 4;
    }
    if (step_out) {
      *step_out = 4;
    }
    *base_out = base;
    return 1;
  }

  if (ir_operand_is_symbol_named(&add->rhs, base) &&
      ir_operand_is_symbol_named(&add->lhs, iv)) {
    if (elem_size_out) {
      *elem_size_out = 1;
    }
    if (step_out) {
      *step_out = 1;
    }
    *base_out = base;
    return 1;
  }

  return 0;
}

static int ir_ptr_header_reentered_past(const IRFunction *function,
                                        size_t init_index,
                                        size_t header_index) {
  for (size_t l = init_index + 1; l < header_index; l++) {
    const IRInstruction *label = &function->instructions[l];
    if (label->op != IR_OP_LABEL || !label->text) {
      continue;
    }
    for (size_t j = header_index; j < function->instruction_count; j++) {
      const IRInstruction *ins = &function->instructions[j];
      if ((ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
           ins->op == IR_OP_BRANCH_EQ) &&
          ins->text && strcmp(ins->text, label->text) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

int ir_ptr_induction_iv_start_value(const IRFunction *function,
                                           size_t header_index,
                                           const char *iv_symbol,
                                           long long *out_start) {
  if (!function || !iv_symbol || !out_start) {
    return 0;
  }

  for (size_t i = header_index; i > 0;) {
    i--;
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      break;
    }
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_DECLARE_LOCAL ||
        ir_instruction_is_safety_scaffolding(ins)) {
      continue;
    }
    if (ins->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
      if (ins->lhs.kind == IR_OPERAND_INT) {
        *out_start = ins->lhs.int_value;
        return 1;
      }
      return 0;
    }
  }

  {
    const IRInstruction *init = NULL;
    size_t init_index = 0;
    for (size_t i = 0; i < header_index; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (!ir_instruction_writes_destination(ins) ||
          !ir_operand_is_symbol_named(&ins->dest, iv_symbol)) {
        continue;
      }
      if (init || ins->op != IR_OP_ASSIGN || ins->lhs.kind != IR_OPERAND_INT) {
        return 0;
      }
      init = ins;
      init_index = i;
    }
    if (init && ir_ptr_header_reentered_past(function, init_index,
                                             header_index)) {
      return 0;
    }
    if (init) {
      *out_start = init->lhs.int_value;
      return 1;
    }
  }

  return 0;
}

static const char *ir_ptr_induction_base_tag(const char *base) {
  if (!base) {
    return "base";
  }
  if (base[0] == '@') {
    return base + 1;
  }
  return base;
}

static char *ir_ptr_induction_make_name(const char *base, size_t header_index,
                                        const char *suffix) {
  char buf[96];
  const char *tag = ir_ptr_induction_base_tag(base);
  snprintf(buf, sizeof(buf), "__ptr_%zu_%s_%s", header_index, tag, suffix);
  return mettle_strdup(buf);
}

static int ir_ptr_binding_find(IRPtrBaseBinding *bindings, size_t count,
                               const char *base) {
  for (size_t i = 0; i < count; i++) {
    if (bindings[i].base && strcmp(bindings[i].base, base) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static int ir_ptr_binding_add(IRPtrBaseBinding *bindings, size_t *count,
                              size_t header_index, const char *base,
                              const char *addr_temp) {
  int idx = 0;
  if (!bindings || !count || !base || !addr_temp) {
    return 0;
  }
  if (*count >= IR_PTR_BIND_MAX &&
      ir_ptr_binding_find(bindings, *count, base) < 0) {
    return -1;
  }
  idx = ir_ptr_binding_find(bindings, *count, base);
  if (idx < 0) {
    bindings[*count].base = base;
    bindings[*count].ptr_p =
        ir_ptr_induction_make_name(base, header_index, "p");
    if (!bindings[*count].ptr_p) {
      return 0;
    }
    bindings[*count].addr_temps[0] = mettle_strdup(addr_temp);
    if (!bindings[*count].addr_temps[0]) {
      free(bindings[*count].ptr_p);
      return 0;
    }
    bindings[*count].addr_temp_count = 1;
    (*count)++;
    return 1;
  }
  if (bindings[idx].addr_temp_count >= 8) {
    return -1;
  }
  for (size_t t = 0; t < bindings[idx].addr_temp_count; t++) {
    if (bindings[idx].addr_temps[t] &&
        strcmp(bindings[idx].addr_temps[t], addr_temp) == 0) {
      return 1;
    }
  }
  bindings[idx].addr_temps[bindings[idx].addr_temp_count] =
      mettle_strdup(addr_temp);
  if (!bindings[idx].addr_temps[bindings[idx].addr_temp_count]) {
    return 0;
  }
  bindings[idx].addr_temp_count++;
  return 1;
}

static void ir_ptr_bindings_destroy(IRPtrBaseBinding *bindings, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(bindings[i].ptr_p);
    for (size_t t = 0; t < bindings[i].addr_temp_count; t++) {
      free(bindings[i].addr_temps[t]);
    }
  }
}

static const char *ir_ptr_lookup_addr_temp(const IRPtrBaseBinding *bindings,
                                           size_t count, const char *addr_temp) {
  for (size_t i = 0; i < count; i++) {
    for (size_t t = 0; t < bindings[i].addr_temp_count; t++) {
      if (bindings[i].addr_temps[t] &&
          strcmp(bindings[i].addr_temps[t], addr_temp) == 0) {
        return bindings[i].ptr_p;
      }
    }
  }
  return NULL;
}

static int ir_ptr_induction_rewrite_instruction(
    IRInstruction *ins, const IRPtrBaseBinding *bindings, size_t binding_count,
    const char *iv_symbol, const char *bound_symbol, const char *end_ptr) {
  if (!ins) {
    return 0;
  }
  if (ins->op == IR_OP_LOAD && ins->lhs.kind == IR_OPERAND_TEMP &&
      ins->lhs.name) {
    const char *ptr =
        ir_ptr_lookup_addr_temp(bindings, binding_count, ins->lhs.name);
    if (ptr) {
      ir_operand_destroy(&ins->lhs);
      ins->lhs = ir_operand_symbol(ptr);
      return 1;
    }
  }
  if (ins->op == IR_OP_STORE && ins->dest.kind == IR_OPERAND_TEMP &&
      ins->dest.name) {
    const char *ptr =
        ir_ptr_lookup_addr_temp(bindings, binding_count, ins->dest.name);
    if (ptr) {
      ir_operand_destroy(&ins->dest);
      ins->dest = ir_operand_symbol(ptr);
      return 1;
    }
  }
  if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "<") == 0 &&
      end_ptr && binding_count > 0 &&
      ir_operand_is_symbol_named(&ins->lhs, iv_symbol) &&
      ir_operand_is_symbol_named(&ins->rhs, bound_symbol)) {
    ir_operand_destroy(&ins->lhs);
    ins->lhs = ir_operand_symbol(bindings[0].ptr_p);
    ir_operand_destroy(&ins->rhs);
    ins->rhs = ir_operand_symbol(end_ptr);
    return 1;
  }
  return 1;
}

static int ir_ptr_induction_should_drop_body_insn(
    const IRInstruction *ins, const IRPtrBaseBinding *bindings,
    size_t binding_count, const char *iv_symbol, int keep_iv) {
  if (!ins || !iv_symbol) {
    return 0;
  }
  if (ir_operand_is_temp(&ins->dest) &&
      ir_ptr_lookup_addr_temp(bindings, binding_count, ins->dest.name)) {
    return 1;
  }
  if (keep_iv) {
    return 0;
  }
  if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "<<") == 0 &&
      ir_operand_is_symbol_named(&ins->lhs, iv_symbol)) {
    return 1;
  }
  if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
      ir_operand_is_symbol_named(&ins->dest, iv_symbol) &&
      ir_operand_is_int_value(&ins->rhs, 1)) {
    return 1;
  }
  return 0;
}

static int ir_ptr_iv_chain_dies_in_drops(const IRFunction *function,
                                         size_t body_start, size_t body_end,
                                         const IRPtrBaseBinding *bindings,
                                         size_t binding_count,
                                         const char *iv_symbol,
                                         const IRInstruction *producer,
                                         int depth) {
  if (depth > 8) {
    return 0;
  }
  if (producer->dest.kind != IR_OPERAND_TEMP || !producer->dest.name) {
    return 0;
  }
  switch (producer->op) {
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_ASSIGN:
    break;
  default:
    return 0;
  }
  const char *temp = producer->dest.name;
  for (size_t j = 0; j < function->instruction_count; j++) {
    const IRInstruction *reader = &function->instructions[j];
    int reads = (ir_operand_is_temp(&reader->lhs) &&
                 strcmp(reader->lhs.name, temp) == 0) ||
                (ir_operand_is_temp(&reader->rhs) &&
                 strcmp(reader->rhs.name, temp) == 0) ||
                (reader->op == IR_OP_STORE &&
                 ir_operand_is_temp(&reader->dest) &&
                 strcmp(reader->dest.name, temp) == 0);
    for (size_t a = 0; !reads && a < reader->argument_count; a++) {
      reads = reader->arguments[a].kind == IR_OPERAND_TEMP &&
              reader->arguments[a].name &&
              strcmp(reader->arguments[a].name, temp) == 0;
    }
    if (!reads || reader == producer) {
      continue;
    }
    if (j < body_start || j >= body_end) {
      return 0;
    }
    if (reader->op == IR_OP_STORE) {
      int as_value =
          (ir_operand_is_temp(&reader->lhs) &&
           strcmp(reader->lhs.name, temp) == 0);
      int as_bound_address =
          (ir_operand_is_temp(&reader->dest) &&
           strcmp(reader->dest.name, temp) == 0 &&
           ir_ptr_lookup_addr_temp(bindings, binding_count,
                                   reader->dest.name) != NULL);
      if (as_value || !as_bound_address) {
        return 0;
      }
      continue;
    }
    if (reader->op == IR_OP_LOAD) {
      if (ir_operand_is_temp(&reader->lhs) &&
          strcmp(reader->lhs.name, temp) == 0 &&
          ir_ptr_lookup_addr_temp(bindings, binding_count,
                                  reader->lhs.name) != NULL) {
        continue;
      }
      return 0;
    }
    if (ir_ptr_induction_should_drop_body_insn(reader, bindings, binding_count,
                                               iv_symbol, 0)) {
      continue;
    }
    if (!ir_ptr_iv_chain_dies_in_drops(function, body_start, body_end,
                                       bindings, binding_count, iv_symbol,
                                       reader, depth + 1)) {
      return 0;
    }
  }
  return 1;
}

static int ir_ptr_loop_is_pure_reduction(const IRFunction *function,
                                         size_t body_start, size_t body_end,
                                         const char *iv_symbol) {
  int loop_has_reduction = 0;
  int loop_has_store = 0;
  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_STORE) {
      loop_has_store = 1;
    }
    if (ins->op == IR_OP_BINARY && ins->text &&
        (strcmp(ins->text, "+") == 0 || strcmp(ins->text, "-") == 0) &&
        ir_operand_is_symbol(&ins->dest) &&
        (!iv_symbol || strcmp(ins->dest.name, iv_symbol) != 0) &&
        (ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) ||
         ir_operand_is_symbol_named(&ins->rhs, ins->dest.name))) {
      loop_has_reduction = 1;
    }
  }
  return loop_has_reduction && !loop_has_store;
}

static int ir_ptr_collect_one_access(const IRFunction *function, size_t index,
                                     const char *iv_symbol,
                                     const char *bound_symbol,
                                     const IROperand *address,
                                     size_t header_index,
                                     IRPtrBaseBinding *bindings,
                                     size_t *binding_count,
                                     int *has_unconvertible_iv_access) {
  const char *base = NULL;
  if (address->kind != IR_OPERAND_TEMP || !address->name) {
    return 1;
  }
  if (!ir_resolve_indexed_address_temp(function, index, iv_symbol, bound_symbol,
                                       address->name, &base, NULL, NULL)) {
    return 1;
  }
  if (!ir_symbol_is_i32_ptr_param(function, base)) {
    *has_unconvertible_iv_access = 1;
    return 1;
  }
  return ir_ptr_binding_add(bindings, binding_count, header_index, base,
                            address->name);
}

static int ir_ptr_collect_bindings(const IRFunction *function,
                                   size_t header_index, size_t body_start,
                                   size_t body_end, const char *iv_symbol,
                                   const char *bound_symbol,
                                   IRPtrBaseBinding *bindings,
                                   size_t *binding_count,
                                   int *has_unconvertible_iv_access) {
  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *address = NULL;
    int added;
    if (ins->op == IR_OP_LOAD) {
      address = &ins->lhs;
    } else if (ins->op == IR_OP_STORE) {
      address = &ins->dest;
    } else {
      continue;
    }
    added = ir_ptr_collect_one_access(function, i, iv_symbol, bound_symbol,
                                      address, header_index, bindings,
                                      binding_count,
                                      has_unconvertible_iv_access);
    if (added <= 0) {
      return added == 0 ? 0 : -1;
    }
  }
  for (size_t b = 0; b < *binding_count; b++) {
    for (size_t i = body_start; i < body_end; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ir_instruction_writes_destination(ins) &&
          ir_operand_is_symbol_named(&ins->dest, bindings[b].base)) {
        return -1;
      }
    }
  }
  return 1;
}

static int ir_ptr_must_keep_counter(const IRFunction *function,
                                    size_t body_start, size_t body_end,
                                    const IRPtrBaseBinding *bindings,
                                    size_t binding_count,
                                    const char *iv_symbol) {
  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    int reads_iv;
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ir_operand_is_temp(&ins->dest) &&
        ir_ptr_lookup_addr_temp(bindings, binding_count, ins->dest.name)) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        ir_operand_is_symbol_named(&ins->dest, iv_symbol) &&
        ir_operand_is_int_value(&ins->rhs, 1)) {
      continue;
    }
    reads_iv = ir_operand_is_symbol_named(&ins->lhs, iv_symbol) ||
               ir_operand_is_symbol_named(&ins->rhs, iv_symbol) ||
               (ins->op == IR_OP_STORE &&
                ir_operand_is_symbol_named(&ins->dest, iv_symbol));
    for (size_t a = 0; !reads_iv && a < ins->argument_count; a++) {
      reads_iv = ir_operand_is_symbol_named(&ins->arguments[a], iv_symbol);
    }
    if (reads_iv &&
        !ir_ptr_iv_chain_dies_in_drops(function, body_start, body_end, bindings,
                                       binding_count, iv_symbol, ins, 0)) {
      return 1;
    }
  }
  return 0;
}

static int ir_ptr_emit_count_guard(IRFunction *function, size_t header_index,
                                   const char *bound_symbol,
                                   const char *exit_label,
                                   IRInstructionVector *vector);

static int ir_ptr_emit_prologue(IRFunction *function,
                                size_t header_index,
                                const IRPtrBaseBinding *bindings,
                                size_t binding_count, const char *bound_symbol,
                                const char *end_ptr, const char *exit_label,
                                IRInstructionVector *vector) {
  IRInstruction end_decl = {0};
  IRInstruction end_init = {0};
  IRInstruction end_scale = {0};
  IRInstruction end_add = {0};
  char end_scale_temp[64];
  const char *end_type;
  int ok;

  for (size_t i = 0; i < header_index; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      return 0;
    }
  }

  for (size_t b = 0; b < binding_count; b++) {
    IRInstruction decl = {0};
    IRInstruction init = {0};
    const char *ptr_type =
        ir_function_local_declared_type(function, bindings[b].base);
    if (!ptr_type) {
      ptr_type = "int32*";
    }
    decl.op = IR_OP_DECLARE_LOCAL;
    decl.dest = ir_operand_symbol(bindings[b].ptr_p);
    decl.text = mettle_strdup(ptr_type);
    init.op = IR_OP_ASSIGN;
    init.dest = ir_operand_symbol(bindings[b].ptr_p);
    init.lhs = ir_operand_symbol(bindings[b].base);
    ok = decl.dest.name && decl.text && init.dest.name && init.lhs.name &&
         ir_instruction_vector_append_move(vector, &decl) &&
         ir_instruction_vector_append_move(vector, &init);
    ir_instruction_destroy_storage(&decl);
    ir_instruction_destroy_storage(&init);
    if (!ok) {
      return 0;
    }
  }

  end_type = ir_function_local_declared_type(function, bindings[0].base);
  snprintf(end_scale_temp, sizeof(end_scale_temp), "__ptr_t%zu_end",
           header_index);
  end_decl.op = IR_OP_DECLARE_LOCAL;
  end_decl.dest = ir_operand_symbol(end_ptr);
  end_decl.text = mettle_strdup(end_type ? end_type : "int32*");
  end_init.op = IR_OP_ASSIGN;
  end_init.dest = ir_operand_symbol(end_ptr);
  end_init.lhs = ir_operand_symbol(bindings[0].base);
  end_scale.op = IR_OP_BINARY;
  end_scale.text = mettle_strdup("<<");
  end_scale.dest = ir_operand_temp(end_scale_temp);
  end_scale.lhs = ir_operand_symbol(bound_symbol);
  end_scale.rhs = ir_operand_int(2);
  if (!ir_ptr_emit_count_guard(function, header_index, bound_symbol, exit_label,
                               vector)) {
    ir_instruction_destroy_storage(&end_decl);
    ir_instruction_destroy_storage(&end_init);
    ir_instruction_destroy_storage(&end_scale);
    return 0;
  }
  end_add.op = IR_OP_BINARY;
  end_add.text = mettle_strdup("+");
  end_add.dest = ir_operand_symbol(end_ptr);
  end_add.lhs = ir_operand_symbol(end_ptr);
  end_add.rhs = ir_operand_temp(end_scale_temp);
  ok = end_decl.text && end_scale.text && end_add.text &&
       ir_instruction_vector_append_move(vector, &end_decl) &&
       ir_instruction_vector_append_move(vector, &end_init) &&
       ir_instruction_vector_append_move(vector, &end_scale) &&
       ir_instruction_vector_append_move(vector, &end_add);
  ir_instruction_destroy_storage(&end_decl);
  ir_instruction_destroy_storage(&end_init);
  ir_instruction_destroy_storage(&end_scale);
  ir_instruction_destroy_storage(&end_add);
  return ok;
}

static int ir_ptr_emit_count_guard(IRFunction *function, size_t header_index,
                                   const char *bound_symbol,
                                   const char *exit_label,
                                   IRInstructionVector *vector) {
  const char *type = ir_function_local_declared_type(function, bound_symbol);
  int bits = 0;
  int is_unsigned = 0;
  char positive[64];
  IRInstruction compare = {0};
  IRInstruction branch = {0};
  int ok;
  for (size_t p = 0; !type && p < function->parameter_count; p++) {
    if (function->parameter_names[p] &&
        strcmp(function->parameter_names[p], bound_symbol) == 0) {
      type = function->parameter_types[p];
    }
  }
  if (!type || !ir_int_type_name_info(type, &bits, &is_unsigned) || is_unsigned) {
    return 1;
  }
  snprintf(positive, sizeof(positive), "__ptr_t%zu_any", header_index);
  compare.op = IR_OP_BINARY;
  compare.text = mettle_strdup(">");
  compare.dest = ir_operand_temp(positive);
  compare.lhs = ir_operand_symbol(bound_symbol);
  compare.rhs = ir_operand_int(0);
  branch.op = IR_OP_BRANCH_ZERO;
  branch.text = mettle_strdup(exit_label);
  branch.lhs = ir_operand_temp(positive);
  ok = compare.text && compare.dest.name && branch.text && branch.lhs.name &&
       ir_instruction_vector_append_move(vector, &compare) &&
       ir_instruction_vector_append_move(vector, &branch);
  ir_instruction_destroy_storage(&compare);
  ir_instruction_destroy_storage(&branch);
  return ok;
}

static int ir_ptr_emit_step(IRFunction *function, size_t index,
                            const IRPtrBaseBinding *bindings,
                            size_t binding_count,
                            IRInstructionVector *vector) {
  for (size_t b = 0; b < binding_count; b++) {
    IRInstruction step = {0};
    int ok;
    step.op = IR_OP_BINARY;
    step.location = function->instructions[index].location;
    step.text = mettle_strdup("+");
    step.dest = ir_operand_symbol(bindings[b].ptr_p);
    step.lhs = ir_operand_symbol(bindings[b].ptr_p);
    step.rhs = ir_operand_int(4);
    ok = step.text && ir_instruction_vector_append_move(vector, &step);
    ir_instruction_destroy_storage(&step);
    if (!ok) {
      return 0;
    }
  }
  return 1;
}

static int ir_ptr_emit_loop(IRFunction *function, size_t header_index,
                            size_t increment_index, size_t jump_index,
                            size_t body_start, size_t body_end,
                            const IRPtrBaseBinding *bindings,
                            size_t binding_count, const char *iv_symbol,
                            const char *bound_symbol, const char *end_ptr,
                            int keep_iv, IRInstructionVector *vector) {
  for (size_t i = header_index; i < function->instruction_count; i++) {
    IRInstruction rewritten = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &rewritten)) {
      return 0;
    }

    if (i == increment_index) {
      if (!ir_ptr_emit_step(function, i, bindings, binding_count, vector) ||
          !ir_instruction_vector_append_move(vector, &rewritten)) {
        ir_instruction_destroy_storage(&rewritten);
        return 0;
      }
      continue;
    }

    if (i <= jump_index &&
        !ir_ptr_induction_rewrite_instruction(&rewritten, bindings,
                                              binding_count, iv_symbol,
                                              bound_symbol, end_ptr)) {
      ir_instruction_destroy_storage(&rewritten);
      return 0;
    }

    if (i >= body_start && i < body_end &&
        ir_ptr_induction_should_drop_body_insn(&rewritten, bindings,
                                               binding_count, iv_symbol,
                                               keep_iv)) {
      ir_instruction_destroy_storage(&rewritten);
      continue;
    }

    if (!ir_instruction_vector_append_move(vector, &rewritten)) {
      ir_instruction_destroy_storage(&rewritten);
      return 0;
    }
  }
  return 1;
}

static int ir_try_pointer_induction_at(IRFunction *function, size_t header_index,
                                       int *changed) {
  IRWhileLoopBounds bounds = {0};
  const char *iv_symbol = NULL;
  size_t body_start = 0;
  size_t body_end = 0;
  size_t increment_index = 0;
  IRPtrBaseBinding bindings[IR_PTR_BIND_MAX] = {0};
  size_t binding_count = 0;
  char *end_ptr = NULL;
  IRInstructionVector vector = {0};
  const IRInstruction *compare = NULL;
  const char *bound_symbol = NULL;
  long long iv_start = 0;
  int has_unconvertible_iv_access = 0;
  int collected;
  int keep_iv;

  if (!function || !ir_find_while_loop_bounds(function, header_index, &bounds)) {
    return 1;
  }

  body_start = bounds.branch_index + 1;
  body_end = bounds.jump_index;

  compare = &function->instructions[bounds.compare_index];
  iv_symbol = compare->lhs.name;
  if (!iv_symbol || compare->rhs.kind != IR_OPERAND_SYMBOL ||
      !compare->rhs.name ||
      !ir_symbol_is_loop_bound(function, compare->rhs.name, header_index,
                               bounds.jump_index)) {
    return 1;
  }
  bound_symbol = compare->rhs.name;

  if (!ir_ptr_induction_iv_start_value(function, header_index, iv_symbol,
                                       &iv_start) ||
      iv_start != 0) {
    return 1;
  }

  increment_index = bounds.jump_index;
  while (increment_index > body_start) {
    increment_index--;
    if (function->instructions[increment_index].op != IR_OP_NOP) {
      break;
    }
  }
  if (!ir_try_parse_direct_unit_increment(
          &function->instructions[increment_index], iv_symbol)) {
    return 1;
  }

  if (ir_loop_body_is_unclaimable(function, body_start, body_end)) {
    return 1;
  }

  if (ir_ptr_loop_is_pure_reduction(function, body_start, body_end,
                                    iv_symbol)) {
    return 1;
  }

  if (ir_auto_vectorize_int_claimable(function, header_index)) {
    return 1;
  }

  if (ir_auto_vectorize_find_claimable(function, header_index)) {
    return 1;
  }

  collected = ir_ptr_collect_bindings(function, header_index, body_start,
                                      body_end, iv_symbol, bound_symbol,
                                      bindings, &binding_count,
                                      &has_unconvertible_iv_access);
  if (collected <= 0) {
    ir_ptr_bindings_destroy(bindings, binding_count);
    return collected == 0 ? 0 : 1;
  }

  keep_iv = has_unconvertible_iv_access ||
            ir_ptr_must_keep_counter(function, body_start, body_end, bindings,
                                     binding_count, iv_symbol);

  if (binding_count == 0) {
    return 1;
  }

  end_ptr = ir_ptr_induction_make_name(bindings[0].base, header_index, "end");
  if (!end_ptr) {
    ir_ptr_bindings_destroy(bindings, binding_count);
    return 0;
  }

  if (!ir_ptr_emit_prologue(function, header_index, bindings, binding_count,
                            bound_symbol, end_ptr,
                            function->instructions[bounds.branch_index].text,
                            &vector) ||
      !ir_ptr_emit_loop(function, header_index, increment_index,
                        bounds.jump_index, body_start, body_end, bindings,
                        binding_count, iv_symbol, bound_symbol, end_ptr,
                        keep_iv, &vector) ||
      !ir_function_replace_instructions(function, &vector)) {
    ir_instruction_vector_destroy(&vector);
    ir_ptr_bindings_destroy(bindings, binding_count);
    free(end_ptr);
    return 0;
  }

  ir_ptr_bindings_destroy(bindings, binding_count);
  free(end_ptr);
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_pointer_induction_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (int iteration = 0; iteration < 16; iteration++) {
    int any_changed = 0;

    for (size_t i = 0; i < function->instruction_count; i++) {
      if (function->instructions[i].op != IR_OP_LABEL) {
        continue;
      }

      int local_changed = 0;
      if (!ir_try_pointer_induction_at(function, i, &local_changed)) {
        return 0;
      }
      if (local_changed) {
        any_changed = 1;
        if (changed) {
          *changed = 1;
        }
        break;
      }
    }

    if (!any_changed) {
      return 1;
    }
  }

  return 1;
}

