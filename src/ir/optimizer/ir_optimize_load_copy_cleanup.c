#include "ir_optimize_internal.h"
#include "../ir_explain_ledger.h"

static IROpcode ir_cleanup_op_before(const IRFunction *function, size_t at) {
  size_t p = at - 1;

  while (p > 0 && function->instructions[p].op == IR_OP_NOP) {
    p--;
  }
  return function->instructions[p].op;
}

static int ir_cleanup_op_leaves_block(IROpcode op) {
  return op == IR_OP_JUMP || op == IR_OP_RETURN || op == IR_OP_BRANCH_ZERO ||
         op == IR_OP_BRANCH_EQ;
}

static size_t ir_load_copy_count_symbol_reads(const IRInstruction *ins,
                                              const char *sym) {
  size_t count = 0;
  if (ir_operand_is_symbol_named(&ins->lhs, sym)) {
    count++;
  }
  if (ir_operand_is_symbol_named(&ins->rhs, sym)) {
    count++;
  }
  if (ins->op == IR_OP_STORE && ir_operand_is_symbol_named(&ins->dest, sym)) {
    count++;
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    if (ir_operand_is_symbol_named(&ins->arguments[a], sym)) {
      count++;
    }
  }
  return count;
}

static const char *ir_load_copy_mentioned_name(const IROperand *operand) {
  return (operand && operand->kind == IR_OPERAND_SYMBOL) ? operand->name : NULL;
}

static size_t ir_load_copy_count_symbol_mentions(const IRInstruction *ins,
                                                 const char *sym) {
  size_t count = 0;
  if (ir_operand_is_symbol_named(&ins->dest, sym)) {
    count++;
  }
  if (ir_operand_is_symbol_named(&ins->lhs, sym)) {
    count++;
  }
  if (ir_operand_is_symbol_named(&ins->rhs, sym)) {
    count++;
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    if (ir_operand_is_symbol_named(&ins->arguments[a], sym)) {
      count++;
    }
  }
  return count;
}

static void ir_load_copy_replace_operand(IROperand *operand, const char *sym,
                                         const char *temp) {
  if (!ir_operand_is_symbol_named(operand, sym)) {
    return;
  }
  int float_bits = operand->float_bits;
  ir_operand_destroy(operand);
  *operand = ir_operand_temp(temp);
  operand->float_bits = float_bits;
}

static int ir_cleanup_label_is_loop_header(const char *label) {
  if (!label) {
    return 0;
  }
  if (strstr(label, "ir_for_cond_") != NULL) {
    return 1;
  }
  return strstr(label, "ir_while_") != NULL &&
         strstr(label, "ir_while_end_") == NULL;
}

static size_t ir_cleanup_loop_latch(const IRFunction *function, size_t header,
                                    const char *label) {
  const size_t latch = ir_pass_loop_latch(function, header, label);
  return latch == IR_BLOCK_NONE ? 0 : latch;
}

int ir_hoist_body_locals_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    size_t latch = 0;
    size_t insert = header;

    if (label->op != IR_OP_LABEL ||
        !ir_cleanup_label_is_loop_header(label->text)) {
      continue;
    }
    latch = ir_function_last_jump_to(function, header, label->text);
    if (latch == IR_BLOCK_NONE || !latch) {
      continue;
    }

    for (size_t i = header + 1; i < latch; i++) {
      IRInstruction saved;
      if (function->instructions[i].op != IR_OP_DECLARE_LOCAL ||
          function->instructions[i].dest.kind != IR_OPERAND_SYMBOL) {
        continue;
      }
      saved = function->instructions[i];
      memmove(&function->instructions[insert + 1],
              &function->instructions[insert],
              (i - insert) * sizeof(IRInstruction));
      function->instructions[insert] = saved;
      insert++;
      if (changed) {
        *changed = 1;
      }
    }
    header = insert;
  }
  return 1;
}

int ir_verify_loop_canonical_form(const IRFunction *function, char *detail,
                                  size_t detail_size) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }

  size_t count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_cleanup_label_is_loop_header(ins->text)) {
      count++;
    }
  }
  if (count == 0) {
    return 1;
  }

  size_t *header_idx = (size_t *)malloc(count * sizeof(size_t));
  size_t *latch = (size_t *)calloc(count, sizeof(size_t));
  const char **header_label =
      (const char **)malloc(count * sizeof(const char *));
  if (!header_idx || !latch || !header_label) {
    free(header_idx);
    free(latch);
    free(header_label);
    return 1;
  }

  size_t h = 0;
  for (size_t i = 0; i < function->instruction_count && h < count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_cleanup_label_is_loop_header(ins->text)) {
      header_idx[h] = i;
      header_label[h] = ins->text;
      h++;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_JUMP || !ins->text) {
      continue;
    }
    for (size_t k = 0; k < count; k++) {
      if (header_idx[k] < i && header_label[k] &&
          strcmp(header_label[k], ins->text) == 0) {
        if (i > latch[k]) {
          latch[k] = i;
        }
        break;
      }
    }
  }

  int ok = 1;
  size_t next_header = 0;
  size_t reach = 0;
  for (size_t i = 0; i < function->instruction_count && ok; i++) {
    while (next_header < count && header_idx[next_header] <= i) {
      if (latch[next_header] > reach) {
        reach = latch[next_header];
      }
      next_header++;
    }
    if (reach <= i) {
      continue;
    }
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_DECLARE_LOCAL ||
        ins->dest.kind != IR_OPERAND_SYMBOL) {
      continue;
    }
    if (detail && detail_size) {
      const char *loop = "?";
      for (size_t k = 0; k < count; k++) {
        if (header_idx[k] < i && latch[k] > i) {
          loop = header_label[k] ? header_label[k] : "?";
        }
      }
      snprintf(detail, detail_size,
               "declaration of '%s' still inside loop '%s'",
               ins->dest.name ? ins->dest.name : "?", loop);
    }
    ok = 0;
  }

  free(header_idx);
  free(latch);
  free(header_label);
  return ok;
}

static const char *ir_hoist_element_pointer_type(const IRInstruction *mem) {
  if (mem->rhs.kind != IR_OPERAND_INT) {
    return NULL;
  }
  if (mem->is_float) {
    return mem->rhs.int_value == 4 ? "float32*" : "float64*";
  }
  switch (mem->rhs.int_value) {
  case 1: return mem->is_unsigned ? "uint8*" : "int8*";
  case 2: return mem->is_unsigned ? "uint16*" : "int16*";
  case 8: return mem->is_unsigned ? "uint64*" : "int64*";
  default: return mem->is_unsigned ? "uint32*" : "int32*";
  }
}

static const IROperand *ir_hoist_memory_address(const IRInstruction *ins) {
  if (ins->op == IR_OP_LOAD) {
    return &ins->lhs;
  }
  if (ins->op == IR_OP_STORE) {
    return &ins->dest;
  }
  return NULL;
}

static const char *ir_hoist_base_pointer_type(const IRFunction *function,
                                              size_t lo, size_t hi,
                                              const char *addr_temp) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *addr = ir_hoist_memory_address(ins);
    const char *derived = NULL;
    if (addr && ir_operand_is_temp_named(addr, addr_temp)) {
      return ir_hoist_element_pointer_type(ins);
    }
    if (ins->op != IR_OP_BINARY || ins->is_float || !ins->text ||
        strcmp(ins->text, "+") != 0 || ins->dest.kind != IR_OPERAND_TEMP ||
        !ins->dest.name ||
        !(ir_operand_is_temp_named(&ins->lhs, addr_temp) ||
          ir_operand_is_temp_named(&ins->rhs, addr_temp))) {
      continue;
    }
    derived = ins->dest.name;
    for (size_t j = i + 1; j < hi; j++) {
      const IRInstruction *mem = &function->instructions[j];
      const IROperand *maddr = ir_hoist_memory_address(mem);
      if (maddr && ir_operand_is_temp_named(maddr, derived)) {
        return ir_hoist_element_pointer_type(mem);
      }
    }
  }
  return NULL;
}

static int ir_hoist_symbol_is_global(const IRFunction *function,
                                     const char *sym) {
  return sym && !ir_function_symbol_is_parameter(function, sym) &&
         ir_function_local_declared_type(function, sym) == NULL;
}

static void ir_hoist_rename_temp_reads(IRFunction *function, size_t lo,
                                       size_t hi, const char *temp,
                                       const char *sym) {
  for (size_t i = lo; i < hi; i++) {
    IRInstruction *ins = &function->instructions[i];
    IROperand *slots[3];
    slots[0] = &ins->lhs;
    slots[1] = &ins->rhs;
    slots[2] = (ins->op == IR_OP_STORE) ? &ins->dest : NULL;
    for (int k = 0; k < 3; k++) {
      if (!slots[k] || !ir_operand_is_temp_named(slots[k], temp)) {
        continue;
      }
      {
        int float_bits = slots[k]->float_bits;
        ir_operand_destroy(slots[k]);
        *slots[k] = ir_operand_symbol(sym);
        slots[k]->float_bits = float_bits;
      }
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_temp_named(&ins->arguments[a], temp)) {
        int float_bits = ins->arguments[a].float_bits;
        ir_operand_destroy(&ins->arguments[a]);
        ins->arguments[a] = ir_operand_symbol(sym);
        ins->arguments[a].float_bits = float_bits;
      }
    }
  }
}

static int ir_hoist_temp_escapes(const IRFunction *function, size_t lo,
                                 size_t hi, const char *temp) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i >= lo && i < hi) {
      continue;
    }
    if (ir_operand_is_temp_named(&ins->lhs, temp) ||
        ir_operand_is_temp_named(&ins->rhs, temp) ||
        ir_operand_is_temp_named(&ins->dest, temp)) {
      return 1;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_temp_named(&ins->arguments[a], temp)) {
        return 1;
      }
    }
  }
  return 0;
}

/* Whether `temp` is written by exactly one instruction in the function.
 * The rename below replaces every read of the temp in the loop with the
 * hoisted global base, which is only sound when the ADDRESS_OF is the
 * temp's sole definition. An inlined call that returns a pointer writes
 * its result temp on each return path -- `if (k == cur) return &A;
 * return &arr[k].st;` assigns one temp in both branches -- and renaming
 * those reads made every path read &A. */
static int ir_hoist_temp_single_def(const IRFunction *function,
                                    const char *temp) {
  int defs = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_STORE) {
      continue; /* dest is the address stored through, not a write */
    }
    if (ir_operand_is_temp_named(&ins->dest, temp) && ++defs > 1) {
      return 0;
    }
  }
  return defs == 1;
}

int ir_hoist_global_bases_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    char loop_label[128];
    size_t latch = 0;

    {
      const IRInstruction *label = &function->instructions[header];
      if (label->op != IR_OP_LABEL ||
          !ir_cleanup_label_is_loop_header(label->text) ||
          snprintf(loop_label, sizeof(loop_label), "%s", label->text) >=
              (int)sizeof(loop_label)) {
        continue;
      }
    }
    latch = ir_cleanup_loop_latch(function, header, loop_label);
    if (!latch) {
      continue;
    }

    for (size_t i = header + 1; i < latch; i++) {
      char base_name[128];
      char global[128];
      char temp[128];
      const char *ptr_type = NULL;
      IRInstruction decl = {0};
      IRInstruction init = {0};
      int already = 0;

      {
        const IRInstruction *addr = &function->instructions[i];
        if (addr->op != IR_OP_ADDRESS_OF ||
            addr->dest.kind != IR_OPERAND_TEMP || !addr->dest.name ||
            addr->lhs.kind != IR_OPERAND_SYMBOL || !addr->lhs.name ||
            snprintf(global, sizeof(global), "%s", addr->lhs.name) >=
                (int)sizeof(global) ||
            snprintf(temp, sizeof(temp), "%s", addr->dest.name) >=
                (int)sizeof(temp)) {
          continue;
        }
      }
      if (!ir_hoist_symbol_is_global(function, global)) {
        continue;
      }
      ptr_type = ir_hoist_base_pointer_type(function, i, latch, temp);
      if (!ptr_type || ir_hoist_temp_escapes(function, header, latch, temp) ||
          !ir_hoist_temp_single_def(function, temp)) {
        continue;
      }
      if (snprintf(base_name, sizeof(base_name), "__gbase_%s_%s", loop_label,
                   global) >= (int)sizeof(base_name)) {
        continue;
      }
      already = ir_function_local_declared_type(function, base_name) != NULL;

      ir_hoist_rename_temp_reads(function, i, latch, temp, base_name);
      ir_instruction_make_nop(&function->instructions[i]);
      if (changed) {
        *changed = 1;
      }
      if (already) {
        continue;
      }
      decl.op = IR_OP_DECLARE_LOCAL;
      decl.dest = ir_operand_symbol(base_name);
      decl.text = mettle_strdup(ptr_type);
      init.op = IR_OP_ADDRESS_OF;
      init.dest = ir_operand_symbol(base_name);
      init.lhs = ir_operand_symbol(global);
      if (!decl.dest.name || !decl.text || !init.dest.name || !init.lhs.name ||
          !ir_function_insert_instruction(function, header, &decl) ||
          !ir_function_insert_instruction(function, header + 1, &init)) {
        ir_instruction_destroy_storage(&decl);
        ir_instruction_destroy_storage(&init);
        return 0;
      }
      ir_instruction_destroy_storage(&decl);
      ir_instruction_destroy_storage(&init);
      header += 2;
      i += 2;
      latch += 2;
    }
  }
  return 1;
}

static int ir_lbase_address_key(const IRFunction *function, size_t before,
                                const IROperand *address, char *out,
                                size_t out_size) {
  long long offset = 0;
  const char *base = NULL;
  const IROperand *cursor = address;
  for (int hops = 0; hops < 4 && cursor; hops++) {
    if (cursor->kind == IR_OPERAND_SYMBOL && cursor->name) {
      base = cursor->name;
      break;
    }
    if (cursor->kind != IR_OPERAND_TEMP || !cursor->name) {
      return 0;
    }
    const IRInstruction *def = NULL;
    for (size_t i = 0; i < before; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ir_operand_is_temp(&ins->dest) &&
          strcmp(ins->dest.name, cursor->name) == 0) {
        if (def) {
          return 0;
        }
        def = ins;
      }
    }
    if (!def) {
      return 0;
    }
    if (def->op == IR_OP_ADDRESS_OF && def->lhs.kind == IR_OPERAND_SYMBOL &&
        def->lhs.name) {
      base = def->lhs.name;
      break;
    }
    if (def->op == IR_OP_BINARY && def->text && strcmp(def->text, "+") == 0 &&
        def->rhs.kind == IR_OPERAND_INT) {
      offset += def->rhs.int_value;
      cursor = &def->lhs;
      continue;
    }
    return 0;
  }
  if (!base) {
    return 0;
  }
  return snprintf(out, out_size, "%s+%lld", base, offset) < (int)out_size;
}

static int ir_lbase_temp_read_in(const IRFunction *function, size_t lo,
                                 size_t hi, const char *temp) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_operand_is_temp_named(&ins->lhs, temp) ||
        ir_operand_is_temp_named(&ins->rhs, temp)) {
      return 1;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_temp_named(&ins->arguments[a], temp)) {
        return 1;
      }
    }
  }
  return 0;
}

static int ir_lbase_is_eight_byte_load(const IRInstruction *load) {
  return load->op == IR_OP_LOAD && load->rhs.kind == IR_OPERAND_INT &&
         load->rhs.int_value == 8;
}

static size_t *ir_lbase_collect_candidates(const IRFunction *function,
                                           size_t *out_count) {
  size_t cand_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (ir_lbase_is_eight_byte_load(&function->instructions[i])) {
      cand_count++;
    }
  }
  size_t *cand = (size_t *)malloc((cand_count ? cand_count : 1) *
                                  sizeof(size_t));
  if (!cand) {
    return NULL;
  }
  cand_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (ir_lbase_is_eight_byte_load(&function->instructions[i])) {
      cand[cand_count++] = i;
    }
  }
  *out_count = cand_count;
  return cand;
}

int ir_hoist_load_bases_pass(IRFunction *function, int *changed) {
  static int g_load_base_counter;
  if (!function) {
    return 0;
  }
  size_t cand_count = 0;
  size_t *cand = ir_lbase_collect_candidates(function, &cand_count);
  if (!cand) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    char loop_label[128];
    char seen_keys[8][160];
    char seen_names[8][128];
    size_t seen_count = 0;
    size_t latch = 0;

    {
      const IRInstruction *label = &function->instructions[header];
      if (label->op != IR_OP_LABEL ||
          !ir_cleanup_label_is_loop_header(label->text) ||
          snprintf(loop_label, sizeof(loop_label), "%s", label->text) >=
              (int)sizeof(loop_label)) {
        continue;
      }
    }
    latch = ir_cleanup_loop_latch(function, header, loop_label);
    if (!latch) {
      continue;
    }

    for (size_t c = 0; c < cand_count && cand[c] < header; c++) {
      size_t i = cand[c];
      char temp[128];
      char base_name[128];
      char key[160];
      const char *ptr_type = NULL;
      const char *reuse = NULL;
      IRInstruction decl = {0};
      IRInstruction init = {0};
      const IRInstruction *load = &function->instructions[i];
      if (load->op != IR_OP_LOAD || load->is_float ||
          load->dest.kind != IR_OPERAND_TEMP || !load->dest.name ||
          load->rhs.kind != IR_OPERAND_INT || load->rhs.int_value != 8 ||
          (load->alias_class != IR_ALIAS_CLASS_POINTER &&
           load->alias_class != IR_ALIAS_CLASS_I64) ||
          snprintf(temp, sizeof(temp), "%s", load->dest.name) >=
              (int)sizeof(temp)) {
        continue;
      }
      if (load->alias_class == IR_ALIAS_CLASS_POINTER) {
        ptr_type = ir_hoist_base_pointer_type(function, header, latch, temp);
      } else if (ir_lbase_temp_read_in(function, header, latch, temp)) {
        ptr_type = "int64";
      }
      if (!ptr_type) {
        continue;
      }
      if (ir_lbase_address_key(function, i, &load->lhs, key, sizeof(key))) {
        for (size_t k = 0; k < seen_count; k++) {
          if (strcmp(seen_keys[k], key) == 0) {
            reuse = seen_names[k];
            break;
          }
        }
      } else {
        key[0] = 0;
      }
      if (reuse) {
        ir_hoist_rename_temp_reads(function, header, latch, temp, reuse);
        if (changed) {
          *changed = 1;
        }
        continue;
      }
      if (snprintf(base_name, sizeof(base_name), "__lbase_%d",
                   g_load_base_counter) >= (int)sizeof(base_name)) {
        continue;
      }
      g_load_base_counter++;
      decl.op = IR_OP_DECLARE_LOCAL;
      decl.dest = ir_operand_symbol(base_name);
      decl.text = mettle_strdup(ptr_type);
      decl.location = load->location;
      init.op = IR_OP_ASSIGN;
      init.dest = ir_operand_symbol(base_name);
      init.lhs = ir_operand_temp(temp);
      init.location = load->location;
      if (!decl.dest.name || !decl.text || !init.dest.name || !init.lhs.name ||
          !ir_function_insert_instruction(function, i + 1, &decl) ||
          !ir_function_insert_instruction(function, i + 2, &init)) {
        ir_instruction_destroy_storage(&decl);
        ir_instruction_destroy_storage(&init);
        free(cand);
        return 0;
      }
      ir_instruction_destroy_storage(&decl);
      ir_instruction_destroy_storage(&init);
      ir_hoist_rename_temp_reads(function, i + 3, function->instruction_count,
                                 temp, base_name);
      if (key[0] && seen_count < 8) {
        snprintf(seen_keys[seen_count], sizeof(seen_keys[0]), "%s", key);
        snprintf(seen_names[seen_count], sizeof(seen_names[0]), "%s",
                 base_name);
        seen_count++;
      }
      header += 2;
      latch += 2;
      for (size_t m = c + 1; m < cand_count; m++) {
        cand[m] += 2;
      }
      if (changed) {
        *changed = 1;
      }
    }
  }
  free(cand);
  return 1;
}

#define IR_ASSIGN_CHAIN_MAX 16

static size_t ir_chain_reads_in(const IRFunction *function, size_t lo,
                                size_t hi, const char *temp) {
  size_t count = 0;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_operand_is_temp_named(&ins->lhs, temp)) {
      count++;
    }
    if (ir_operand_is_temp_named(&ins->rhs, temp)) {
      count++;
    }
    if (ins->op == IR_OP_STORE && ir_operand_is_temp_named(&ins->dest, temp)) {
      count++;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_temp_named(&ins->arguments[a], temp)) {
        count++;
      }
    }
  }
  return count;
}

static int ir_chain_collect(const IRFunction *function, size_t header,
                            size_t latch, const IROperand *operand,
                            size_t *chain, size_t *chain_count, int depth) {
  if (operand->kind == IR_OPERAND_INT || operand->kind == IR_OPERAND_FLOAT ||
      operand->kind == IR_OPERAND_NONE) {
    return 1;
  }
  if (!operand->name || depth > 8) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL) {
    if (ir_symbol_address_taken(function, operand->name)) {
      return 0;
    }
    for (size_t i = header; i < latch; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ir_instruction_writes_destination(ins) &&
          ir_operand_is_symbol_named(&ins->dest, operand->name)) {
        return 0;
      }
    }
    return 1;
  }
  if (operand->kind != IR_OPERAND_TEMP) {
    return 0;
  }
  {
    size_t def = (size_t)-1;
    for (size_t i = 0; i < latch; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ir_operand_is_temp(&ins->dest) &&
          strcmp(ins->dest.name, operand->name) == 0) {
        if (def != (size_t)-1) {
          return 0;
        }
        def = i;
      }
    }
    if (def == (size_t)-1) {
      return 0;
    }
    if (def < header) {
      return 1;
    }
    for (size_t k = 0; k < *chain_count; k++) {
      if (chain[k] == def) {
        return 1;
      }
    }
    {
      const IRInstruction *ins = &function->instructions[def];
      if ((ins->op != IR_OP_BINARY && ins->op != IR_OP_UNARY &&
           ins->op != IR_OP_CAST && ins->op != IR_OP_ADDRESS_OF) ||
          ins->is_volatile || *chain_count >= IR_ASSIGN_CHAIN_MAX ||
          ir_chain_reads_in(function, header, latch, operand->name) != 1) {
        return 0;
      }
      if (ins->op == IR_OP_ADDRESS_OF) {
        if (ins->lhs.kind != IR_OPERAND_SYMBOL || !ins->lhs.name) {
          return 0;
        }
      } else if (!ir_chain_collect(function, header, latch, &ins->lhs, chain,
                                   chain_count, depth + 1) ||
                 !ir_chain_collect(function, header, latch, &ins->rhs, chain,
                                   chain_count, depth + 1)) {
        return 0;
      }
      chain[(*chain_count)++] = def;
      return 1;
    }
  }
}

static int ir_chain_index_less(const void *a, const void *b) {
  size_t x = *(const size_t *)a;
  size_t y = *(const size_t *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int ir_hoist_invariant_at_header(IRFunction *function,
                                        size_t header, int *changed) {
  char loop_label[128];
  size_t latch = 0;
  {
    const IRInstruction *label = &function->instructions[header];
    if (label->op != IR_OP_LABEL ||
        !ir_cleanup_label_is_loop_header(label->text) ||
        snprintf(loop_label, sizeof(loop_label), "%s", label->text) >=
            (int)sizeof(loop_label)) {
      return 1;
    }
  }
  latch = ir_cleanup_loop_latch(function, header, loop_label);
  if (!latch || header == 0) {
    return 1;
  }
  if (ir_cleanup_op_leaves_block(ir_cleanup_op_before(function, header))) {
    return 1;
  }
  for (size_t i = header + 1; i < latch; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const char *name = NULL;
    size_t writes = 0;
    int read_before = 0;
    size_t chain[IR_ASSIGN_CHAIN_MAX];
    size_t chain_count = 0;
    size_t moved = 0;
    if (ins->op != IR_OP_ASSIGN || ins->dest.kind != IR_OPERAND_SYMBOL ||
        !ins->dest.name || ins->is_volatile) {
      continue;
    }
    name = ins->dest.name;
    if (ir_function_local_declared_type(function, name) == NULL ||
        ir_symbol_address_taken(function, name) ||
        (strncmp(name, "ir_row_", 7) != 0 &&
         strncmp(name, "ir_view_", 8) != 0)) {
      continue;
    }
    for (size_t k = 0; k < function->instruction_count; k++) {
      const IRInstruction *other = &function->instructions[k];
      if (ir_instruction_writes_destination(other) &&
          ir_operand_is_symbol_named(&other->dest, name)) {
        writes++;
      }
    }
    if (writes != 1) {
      continue;
    }
    for (size_t k = header; k < i && !read_before; k++) {
      const IRInstruction *other = &function->instructions[k];
      read_before = ir_operand_is_symbol_named(&other->lhs, name) ||
                    ir_operand_is_symbol_named(&other->rhs, name) ||
                    (other->op == IR_OP_STORE &&
                     ir_operand_is_symbol_named(&other->dest, name));
      for (size_t a = 0; a < other->argument_count && !read_before; a++) {
        read_before = ir_operand_is_symbol_named(&other->arguments[a], name);
      }
    }
    if (read_before || ir_symbol_live_after_loop(function, latch + 1, name)) {
      continue;
    }
    if (!ir_chain_collect(function, header, latch, &ins->lhs, chain,
                          &chain_count, 0)) {
      continue;
    }
    chain[chain_count++] = i;
    qsort(chain, chain_count, sizeof(chain[0]), ir_chain_index_less);
    for (size_t k = 0; k < chain_count; k++) {
      IRInstruction hoisted;
      size_t at = chain[k] + moved;
      if (!ir_clone_instruction_plain(&function->instructions[at],
                                      &hoisted)) {
        return 0;
      }
      ir_instruction_make_nop(&function->instructions[at]);
      if (!ir_function_insert_instruction(function, header + moved,
                                          &hoisted)) {
        ir_instruction_destroy_storage(&hoisted);
        return 0;
      }
      ir_instruction_destroy_storage(&hoisted);
      moved++;
    }
    header += moved;
    latch += moved;
    i += moved;
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}

int ir_hoist_invariant_assigns_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    if (!ir_hoist_invariant_at_header(function, header, changed)) {
      return 0;
    }
  }
  return 1;
}

static int ir_row_symbol_written(const IRFunction *function, size_t lo,
                                 size_t hi, const char *sym) {
  for (size_t i = lo; i < hi && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol(&ins->dest) &&
        strcmp(ins->dest.name, sym) == 0) {
      return 1;
    }
  }
  return 0;
}

static const char *ir_row_symbol_pointer_type(const IRFunction *function,
                                              const char *sym) {
  const char *t = ir_function_local_declared_type(function, sym);
  if (!t) {
    for (size_t p = 0; p < function->parameter_count; p++) {
      if (function->parameter_names[p] &&
          strcmp(function->parameter_names[p], sym) == 0) {
        t = function->parameter_types[p];
        break;
      }
    }
  }
  if (!t) {
    return NULL;
  }
  size_t len = strlen(t);
  if ((len > 0 && t[len - 1] == '*') || strcmp(t, "cstring") == 0 ||
      strcmp(t, "rawptr") == 0) {
    return t;
  }
  return NULL;
}

static int ir_row_instruction_reads_temp(const IRInstruction *ins,
                                         const char *name) {
  if (ir_operand_is_temp_named(&ins->lhs, name) ||
      ir_operand_is_temp_named(&ins->rhs, name)) {
    return 1;
  }
  if (ins->op == IR_OP_STORE && ir_operand_is_temp_named(&ins->dest, name)) {
    return 1;
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    if (ir_operand_is_temp_named(&ins->arguments[a], name)) {
      return 1;
    }
  }
  return 0;
}

static int ir_row_operand_invariant(const IRFunction *function, size_t lo,
                                    size_t hi, const IROperand *op) {
  return op->kind == IR_OPERAND_SYMBOL && op->name &&
         !ir_row_symbol_written(function, lo, hi, op->name) &&
         !ir_symbol_address_taken(function, op->name);
}

#define IR_LICM_MAX_PER_LOOP 8

static int ir_licm_divisor_never_zero(const IRFunction *function,
                                      const IRInstruction *ins) {
  const char *declared;
  if (ins->rhs.kind == IR_OPERAND_INT) {
    return ins->rhs.int_value != 0;
  }
  if (ins->rhs.kind != IR_OPERAND_SYMBOL || !ins->rhs.name) {
    return 0;
  }
  declared = ir_function_local_declared_type(function, ins->rhs.name);
  return ir_type_is_nonzero(declared);
}

static int ir_licm_reads_volatile_definition(const IRFunction *function,
                                             const IROperand *op) {
  if (!op || !op->name ||
      (op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *def = &function->instructions[i];
    if (def->is_volatile && def->dest.kind == op->kind && def->dest.name &&
        strcmp(def->dest.name, op->name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_licm_has_volatile_definitions(const IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *def = &function->instructions[i];
    if (def->is_volatile && def->dest.name &&
        (def->dest.kind == IR_OPERAND_TEMP ||
         def->dest.kind == IR_OPERAND_SYMBOL)) {
      return 1;
    }
  }
  return 0;
}

static int ir_licm_op_is_pure_arith(const IRFunction *function,
                                    const IRInstruction *ins,
                                    int volatile_definitions) {
  if (!ins) {
    return 0;
  }
  if (ins->is_volatile ||
      (volatile_definitions &&
       (ir_licm_reads_volatile_definition(function, &ins->lhs) ||
        ir_licm_reads_volatile_definition(function, &ins->rhs)))) {
    return 0;
  }
  if (ins->op == IR_OP_CAST) {
    return 1;
  }
  if (ins->op != IR_OP_BINARY || !ins->text) {
    return 0;
  }
  if (strcmp(ins->text, "/") != 0 && strcmp(ins->text, "%") != 0) {
    return 1;
  }
  if (!ir_licm_divisor_never_zero(function, ins)) {
    return 0;
  }
  if (ir_explain_enabled()) {
    ir_explain_type_payoff(
        ins->location.filename, ins->location.line,
        function ? function->name : NULL,
        ir_function_local_declared_type(function, ins->rhs.name),
        "a loop-invariant divide was hoisted out of the loop",
        "rules the divisor out of being zero, so the trap that keeps a divide "
        "in place cannot happen; consumed by invariant-arithmetic LICM");
  }
  return 1;
}

static int ir_licm_operand_named(const IROperand *op, const char **name) {
  if (!op || !op->name) {
    return 0;
  }
  if (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) {
    *name = op->name;
    return 1;
  }
  return 0;
}

static int ir_licm_instruction_reads(const IRInstruction *ins,
                                     const char *name) {
  const IROperand *slots[3] = {&ins->lhs, &ins->rhs,
                               ins->op == IR_OP_STORE ? &ins->dest : NULL};
  for (int k = 0; k < 3; k++) {
    const IROperand *op = slots[k];
    if (op && op->name &&
        (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) &&
        strcmp(op->name, name) == 0) {
      return 1;
    }
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    const IROperand *op = &ins->arguments[a];
    if (op->name &&
        (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) &&
        strcmp(op->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static size_t ir_licm_read_count(const IRFunction *function, size_t lo,
                                 size_t hi, size_t skip, const char *name) {
  size_t count = 0;
  for (size_t i = lo; i < hi && i < function->instruction_count; i++) {
    if (i != skip &&
        ir_licm_instruction_reads(&function->instructions[i], name)) {
      count++;
    }
  }
  return count;
}

static size_t ir_licm_def_count(const IRFunction *function, size_t lo,
                                size_t hi, const char *name) {
  size_t count = 0;
  for (size_t i = lo; i < hi && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!ir_instruction_writes_destination(ins) || !ins->dest.name) {
      continue;
    }
    if ((ins->dest.kind == IR_OPERAND_TEMP ||
         ins->dest.kind == IR_OPERAND_SYMBOL) &&
        strcmp(ins->dest.name, name) == 0) {
      count++;
    }
  }
  return count;
}

static int ir_licm_written_in(const IRFunction *function, size_t lo, size_t hi,
                              const char *name) {
  for (size_t i = lo; i < hi && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (!ir_instruction_writes_destination(ins) || !ins->dest.name) {
      continue;
    }
    if ((ins->dest.kind == IR_OPERAND_TEMP ||
         ins->dest.kind == IR_OPERAND_SYMBOL) &&
        strcmp(ins->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_licm_loop_has_call(const IRFunction *function, size_t lo,
                                 size_t hi) {
  for (size_t i = lo; i < hi && i < function->instruction_count; i++) {
    const IROpcode op = function->instructions[i].op;
    if (op == IR_OP_CALL || op == IR_OP_CALL_INDIRECT) {
      return 1;
    }
  }
  return 0;
}

static int ir_licm_symbol_is_global(const IRFunction *function,
                                    const char *name) {
  return !ir_function_symbol_is_parameter(function, name) &&
         !ir_function_find_declaration(function, name, 1);
}

int ir_hoist_invariant_arith_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  const int volatile_definitions = ir_licm_has_volatile_definitions(function);
  for (size_t header = 0; header < function->instruction_count; header++) {
    char loop_label[128];
    size_t latch = 0;
    size_t picked[IR_LICM_MAX_PER_LOOP];
    size_t pick_count = 0;

    {
      const IRInstruction *label = &function->instructions[header];
      if (label->op != IR_OP_LABEL ||
          !ir_cleanup_label_is_loop_header(label->text) ||
          snprintf(loop_label, sizeof(loop_label), "%s", label->text) >=
              (int)sizeof(loop_label)) {
        continue;
      }
    }
    latch = ir_cleanup_loop_latch(function, header, loop_label);
    if (!latch || header == 0 || latch - header > 40) {
      continue;
    }
    if (ir_cleanup_op_leaves_block(ir_cleanup_op_before(function, header))) {
      continue;
    }

    const int loop_calls = ir_licm_loop_has_call(function, header + 1, latch);
    for (size_t i = header + 1; i < latch && pick_count < IR_LICM_MAX_PER_LOOP;
         i++) {
      const IRInstruction *ins = &function->instructions[i];
      const char *operands[2];
      size_t operand_count = 0;
      int ok = 1;
      if (!ir_licm_op_is_pure_arith(function, ins, volatile_definitions) ||
          ins->dest.kind != IR_OPERAND_TEMP || !ins->dest.name) {
        continue;
      }
      if (ir_licm_written_in(function, header + 1, latch, ins->dest.name) &&
          ir_licm_read_count(function, header + 1, latch, i,
                             ins->dest.name) == 0) {
        continue;
      }
      if (ir_licm_def_count(function, header + 1, latch, ins->dest.name) != 1) {
        continue;
      }
      {
        const IROperand *slots[2] = {&ins->lhs, &ins->rhs};
        for (int k = 0; k < 2 && ok; k++) {
          const char *name = NULL;
          if (!ir_licm_operand_named(slots[k], &name)) {
            if (slots[k]->kind != IR_OPERAND_NONE &&
                slots[k]->kind != IR_OPERAND_INT &&
                slots[k]->kind != IR_OPERAND_FLOAT) {
              ok = 0;
            }
            continue;
          }
          if (ir_licm_written_in(function, header + 1, latch, name) ||
              (slots[k]->kind == IR_OPERAND_SYMBOL &&
               (ir_symbol_address_taken(function, name) ||
                (loop_calls && ir_licm_symbol_is_global(function, name))))) {
            ok = 0;
            break;
          }

          operands[operand_count++] = name;
        }
      }
      (void)operands;
      if (!ok) {
        continue;
      }
      picked[pick_count++] = i;
    }

    if (pick_count == 0) {
      continue;
    }
    for (size_t k = 0; k < pick_count; k++) {
      IRInstruction hoisted;
      if (!ir_clone_instruction_plain(&function->instructions[picked[k]],
                                      &hoisted)) {
        return 0;
      }
      ir_instruction_make_nop(&function->instructions[picked[k]]);
      if (!ir_function_insert_instruction(function, header + k, &hoisted)) {
        ir_instruction_destroy_storage(&hoisted);
        return 0;
      }
      ir_instruction_destroy_storage(&hoisted);
      for (size_t m = k + 1; m < pick_count; m++) {
        picked[m]++;
      }
      latch++;
    }
    header += pick_count;
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}
#define IR_ROW_MAX_CONSUMERS 8

typedef struct {
  long long k;
  long long bias;
  size_t idx_pos;
  size_t hop_at;
  char sh_name[128];
  IROperand inv;
  IROperand var;
} IRRowShape;

static int ir_row_index_is_read_in_loop(const IRFunction *function, size_t s,
                                        size_t latch, const char *sh_name) {
  for (size_t j = s + 1; j < latch; j++) {
    const IRInstruction *use = &function->instructions[j];
    if (use->op == IR_OP_BINARY && !use->is_float && use->text &&
        strcmp(use->text, "+") == 0 &&
        ir_operand_is_symbol(&use->lhs) &&
        ir_operand_is_temp_named(&use->rhs, sh_name)) {
      return 1;
    }
  }
  return 0;
}

static size_t ir_narrowing_width(const char *type_name);

static struct {
  const IRFunction *function;
  int valid;
  IRNameIndex count;
  IRNameIndex first;
} g_row_readers;

static void ir_row_readers_drop(void) {
  if (g_row_readers.valid) {
    ir_name_index_destroy(&g_row_readers.count);
    ir_name_index_destroy(&g_row_readers.first);
  }
  g_row_readers.valid = 0;
  g_row_readers.function = NULL;
}

static const IROperand *ir_row_read_operand(const IRInstruction *ins,
                                            size_t k) {
  if (k == 0) {
    return &ins->lhs;
  }
  if (k == 1) {
    return &ins->rhs;
  }
  if (k == 2) {
    return ins->op == IR_OP_STORE ? &ins->dest : NULL;
  }
  return &ins->arguments[k - 3];
}

static int ir_row_readers_build(const IRFunction *function) {
  size_t operands = 0;
  ir_row_readers_drop();
  for (size_t j = 0; j < function->instruction_count; j++) {
    operands += 3 + function->instructions[j].argument_count;
  }
  if (!ir_name_index_init(&g_row_readers.count, operands)) {
    return 0;
  }
  if (!ir_name_index_init(&g_row_readers.first, operands)) {
    ir_name_index_destroy(&g_row_readers.count);
    return 0;
  }
  g_row_readers.valid = 1;
  g_row_readers.function = function;
  for (size_t j = 0; j < function->instruction_count; j++) {
    const IRInstruction *ins = &function->instructions[j];
    size_t total = 3 + ins->argument_count;
    for (size_t k = 0; k < total; k++) {
      const IROperand *op = ir_row_read_operand(ins, k);
      int seen = 0;
      if (!op || op->kind != IR_OPERAND_TEMP || !op->name) {
        continue;
      }
      for (size_t e = 0; e < k && !seen; e++) {
        const IROperand *earlier = ir_row_read_operand(ins, e);
        seen = earlier && ir_operand_is_temp_named(earlier, op->name);
      }
      if (!seen) {
        ir_name_index_add(&g_row_readers.count, op->name, 1);
        ir_name_index_insert(&g_row_readers.first, op->name, j);
      }
    }
  }
  return 1;
}

static const char *ir_row_index_through_narrowing(const IRFunction *function,
                                                  const char *name,
                                                  size_t *cast_at) {
  const IRInstruction *cast = NULL;
  size_t readers = 0;
  if (g_row_readers.function != function || !g_row_readers.valid) {
    ir_row_readers_build(function);
  }
  if (g_row_readers.valid && g_row_readers.function == function) {
    size_t at = 0;
    if (ir_name_index_find(&g_row_readers.count, name, &readers) &&
        readers == 1 && ir_name_index_find(&g_row_readers.first, name, &at) &&
        at < function->instruction_count) {
      cast = &function->instructions[at];
      *cast_at = at;
    }
  } else {
    for (size_t j = 0; j < function->instruction_count; j++) {
      const IRInstruction *ins = &function->instructions[j];
      if (!ir_row_instruction_reads_temp(ins, name)) {
        continue;
      }
      readers++;
      cast = ins;
      *cast_at = j;
    }
  }
  if (readers != 1 || !cast || cast->op != IR_OP_CAST || cast->is_float ||
      !cast->text || ir_narrowing_width(cast->text) == 0 ||
      cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name ||
      strcmp(cast->lhs.name, name) != 0 ||
      cast->dest.kind != IR_OPERAND_TEMP || !cast->dest.name) {
    return NULL;
  }
  return cast->dest.name;
}

static const IRInstruction *ir_row_nearest_def(const IRFunction *function,
                                               size_t from, size_t header,
                                               const char *name, size_t *at) {
  for (size_t j = from; j-- > header + 1;) {
    const IRInstruction *cand = &function->instructions[j];
    if (ir_instruction_writes_destination(cand) &&
        ir_operand_is_temp(&cand->dest) &&
        strcmp(cand->dest.name, name) == 0) {
      *at = j;
      return cand;
    }
  }
  return NULL;
}

static const IRInstruction *ir_row_see_through_narrowing(
    const IRFunction *function, size_t header, const IRInstruction *idx,
    size_t *at) {
  while (idx && idx->op == IR_OP_CAST && !idx->is_float && idx->text &&
         ir_operand_is_temp(&idx->lhs) &&
         ir_narrowing_width(idx->text) != 0) {
    idx = ir_row_nearest_def(function, *at, header, idx->lhs.name, at);
  }
  return idx;
}

static int ir_row_still_holds(const IRFunction *function, size_t from,
                              size_t to, const IROperand *value) {
  for (size_t j = from; j < to; j++) {
    const IRInstruction *mid = &function->instructions[j];
    if (ir_instruction_writes_destination(mid) &&
        mid->dest.kind == value->kind && mid->dest.name &&
        strcmp(mid->dest.name, value->name) == 0) {
      return 0;
    }
  }
  return 1;
}

static const IRInstruction *ir_row_carry_bias(const IRFunction *function,
                                              size_t header,
                                              const IRInstruction *idx,
                                              IRRowShape *out) {
  size_t inner_at = 0;
  const IRInstruction *inner;
  if (!idx || idx->op != IR_OP_BINARY || idx->is_float || !idx->text ||
      (strcmp(idx->text, "+") != 0 && strcmp(idx->text, "-") != 0) ||
      idx->rhs.kind != IR_OPERAND_INT || idx->lhs.kind != IR_OPERAND_TEMP ||
      !idx->lhs.name) {
    return idx;
  }
  inner = ir_row_nearest_def(function, out->idx_pos, header, idx->lhs.name,
                             &inner_at);
  if (!inner || inner->op != IR_OP_BINARY || inner->is_float || !inner->text ||
      strcmp(inner->text, "+") != 0) {
    return idx;
  }
  out->bias =
      strcmp(idx->text, "-") == 0 ? -idx->rhs.int_value : idx->rhs.int_value;
  out->idx_pos = inner_at;
  return inner;
}

static int ir_row_pick_sides(const IRFunction *function, size_t header,
                             size_t latch, size_t s, const IRInstruction *idx,
                             IRRowShape *out) {
  const IROperand *inv_side;
  const IROperand *var_side;
  int lhs_inv =
      ir_row_operand_invariant(function, header + 1, latch, &idx->lhs);
  int rhs_inv =
      ir_row_operand_invariant(function, header + 1, latch, &idx->rhs);
  if (lhs_inv == rhs_inv) {
    return 0;
  }
  inv_side = lhs_inv ? &idx->lhs : &idx->rhs;
  var_side = lhs_inv ? &idx->rhs : &idx->lhs;
  if ((var_side->kind != IR_OPERAND_SYMBOL &&
       var_side->kind != IR_OPERAND_TEMP) ||
      !var_side->name) {
    return 0;
  }
  if (inv_side->kind == IR_OPERAND_INT &&
      inv_side->int_value + out->bias == 0) {
    return 0;
  }
  if (!ir_row_still_holds(function, out->idx_pos + 1, s, var_side)) {
    return 0;
  }
  out->inv = ir_operand_copy(inv_side);
  out->var = ir_operand_copy(var_side);
  return 1;
}

static int ir_row_match_shape(const IRFunction *function, size_t header,
                              size_t latch, size_t s, IRRowShape *out) {
  const IRInstruction *shl = &function->instructions[s];
  const IRInstruction *idx = NULL;
  int scaled = shl->op == IR_OP_BINARY && !shl->is_float && shl->text &&
               strcmp(shl->text, "<<") == 0 &&
               shl->rhs.kind == IR_OPERAND_INT && shl->rhs.int_value >= 1 &&
               shl->rhs.int_value <= 3 && shl->lhs.kind == IR_OPERAND_TEMP &&
               shl->lhs.name && shl->dest.kind == IR_OPERAND_TEMP &&
               shl->dest.name;
  int unscaled = !scaled && shl->op == IR_OP_BINARY && !shl->is_float &&
                 shl->text && strcmp(shl->text, "+") == 0 &&
                 ir_operand_is_temp(&shl->dest);

  out->hop_at = (size_t)-1;
  if (!scaled && !unscaled) {
    return 0;
  }
  if (snprintf(out->sh_name, sizeof(out->sh_name), "%s", shl->dest.name) >=
      (int)sizeof(out->sh_name)) {
    return 0;
  }
  out->k = scaled ? shl->rhs.int_value : 0;

  if (!ir_row_index_is_read_in_loop(function, s, latch, out->sh_name)) {
    size_t cast_at = (size_t)-1;
    const char *through =
        ir_row_index_through_narrowing(function, out->sh_name, &cast_at);
    if (!through || cast_at <= s || cast_at >= latch ||
        snprintf(out->sh_name, sizeof(out->sh_name), "%s", through) >=
            (int)sizeof(out->sh_name) ||
        !ir_row_index_is_read_in_loop(function, s, latch, out->sh_name)) {
      return 0;
    }
    out->hop_at = cast_at;
  }

  if (scaled) {
    idx = ir_row_nearest_def(function, s, header, shl->lhs.name,
                             &out->idx_pos);
  } else {
    idx = shl;
    out->idx_pos = s;
  }
  idx = ir_row_see_through_narrowing(function, header, idx, &out->idx_pos);
  idx = ir_row_carry_bias(function, header, idx, out);
  idx = ir_row_see_through_narrowing(function, header, idx, &out->idx_pos);
  if (!idx || idx->op != IR_OP_BINARY || idx->is_float || !idx->text ||
      strcmp(idx->text, "+") != 0) {
    return 0;
  }
  return ir_row_pick_sides(function, header, latch, s, idx, out);
}

#define IR_ROW_MAX_CACHE 16

typedef struct {
  char base[128];
  char name[48];
  long long bias;
  long long k;
  IROperand inv;
} IRRowCacheEntry;

static void ir_row_cache_clear(IRRowCacheEntry *cache, size_t *count) {
  for (size_t i = 0; i < *count; i++) {
    ir_operand_destroy(&cache[i].inv);
  }
  *count = 0;
}

static int ir_row_cache_find(const IRRowCacheEntry *cache, size_t count,
                             const char *base, long long bias, long long k,
                             const IROperand *inv) {
  for (size_t i = 0; i < count; i++) {
    if (cache[i].bias == bias && cache[i].k == k &&
        strcmp(cache[i].base, base) == 0 &&
        ir_operand_same(&cache[i].inv, inv)) {
      return (int)i;
    }
  }
  return -1;
}

static void ir_row_replace_temp_with(IRFunction *function, const char *temp,
                                     const IROperand *value) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *in = &function->instructions[i];
    IROperand *slots[2] = {&in->lhs, &in->rhs};
    for (int s = 0; s < 2; s++) {
      if (ir_operand_is_temp_named(slots[s], temp)) {
        ir_operand_destroy(slots[s]);
        *slots[s] = ir_operand_copy(value);
      }
    }
    if (in->op == IR_OP_STORE && ir_operand_is_temp_named(&in->dest, temp)) {
      ir_operand_destroy(&in->dest);
      in->dest = ir_operand_copy(value);
    }
    for (size_t a = 0; a < in->argument_count; a++) {
      if (ir_operand_is_temp_named(&in->arguments[a], temp)) {
        ir_operand_destroy(&in->arguments[a]);
        in->arguments[a] = ir_operand_copy(value);
      }
    }
  }
}

static void ir_row_collapse_hop(IRFunction *function, size_t s, size_t hop_at) {
  if (hop_at == (size_t)-1 || s >= function->instruction_count ||
      hop_at >= function->instruction_count) {
    return;
  }
  IRInstruction *assign = &function->instructions[s];
  IRInstruction *cast = &function->instructions[hop_at];
  const char *declared;
  if (assign->op != IR_OP_ASSIGN || assign->lhs.kind != IR_OPERAND_SYMBOL ||
      !assign->lhs.name || assign->dest.kind != IR_OPERAND_TEMP ||
      !assign->dest.name || cast->op != IR_OP_CAST || cast->is_float ||
      !cast->text || cast->dest.kind != IR_OPERAND_TEMP || !cast->dest.name ||
      !ir_operand_is_temp_named(&cast->lhs, assign->dest.name)) {
    return;
  }
  declared = ir_function_local_declared_type(function, assign->lhs.name);
  if (!declared && function->parameter_names && function->parameter_types) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      if (function->parameter_names[i] &&
          strcmp(function->parameter_names[i], assign->lhs.name) == 0) {
        declared = function->parameter_types[i];
        break;
      }
    }
  }
  if (!declared || strcmp(declared, cast->text) != 0) {
    return;
  }
  {
    IROperand value = ir_operand_symbol(assign->lhs.name);
    if (!value.name) {
      return;
    }
    ir_row_replace_temp_with(function, cast->dest.name, &value);
    ir_operand_destroy(&value);
  }
  ir_instruction_make_nop(cast);
  ir_instruction_make_nop(assign);
}

typedef struct {
  IRFunction *function;
  IRRowCacheEntry *cache;
  size_t cache_count;
  size_t header;
  size_t latch;
  int *changed;
} IRRowPass;

static void ir_row_retarget_shift(IRFunction *function, size_t s, long long k,
                                  IROperand *var) {
  IRInstruction *shl = &function->instructions[s];

  ir_operand_destroy(&shl->lhs);
  if (k == 0) {
    ir_operand_destroy(&shl->rhs);
    mettle_free_string(shl->text);
    shl->text = NULL;
    shl->op = IR_OP_ASSIGN;
    shl->rhs = ir_operand_none();
  }
  shl->lhs = *var;
  *var = ir_operand_none();
}

static void ir_row_drop_dead_index(IRFunction *function, size_t idx_pos) {
  IRInstruction *idx_ins = &function->instructions[idx_pos];

  if (idx_ins->dest.kind != IR_OPERAND_TEMP || !idx_ins->dest.name) {
    return;
  }
  for (size_t j = 0; j < function->instruction_count; j++) {
    if (j == idx_pos) {
      continue;
    }
    if (ir_row_instruction_reads_temp(&function->instructions[j],
                                      idx_ins->dest.name)) {
      return;
    }
  }
  ir_instruction_make_nop(idx_ins);
}

static int ir_row_insert(IRFunction *function, size_t *at, size_t *inserted,
                         IRInstruction *in, int ready) {
  int ok = ready && ir_function_insert_instruction(function, *at, in);

  ir_instruction_destroy_storage(in);
  if (ok) {
    (*at)++;
    (*inserted)++;
  }
  return ok;
}

static int ir_row_consumer_is_base_add(const IRFunction *function,
                                       size_t header, size_t latch,
                                       const IRInstruction *ins,
                                       const char *sh_name) {
  if (!(ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ins->lhs.name && ir_operand_is_temp_named(&ins->rhs, sh_name) &&
        ins->dest.kind == IR_OPERAND_TEMP)) {
    return 0;
  }
  return ir_row_symbol_pointer_type(function, ins->lhs.name) != NULL &&
         !ir_row_symbol_written(function, header + 1, latch, ins->lhs.name) &&
         !ir_symbol_address_taken(function, ins->lhs.name);
}

static int ir_row_collect_consumers(const IRFunction *function, size_t header,
                                    size_t latch, size_t s, size_t hop_at,
                                    const char *sh_name, size_t *consumers,
                                    size_t *out_count) {
  size_t count = 0;

  for (size_t j = 0; j < function->instruction_count; j++) {
    const IRInstruction *ins = &function->instructions[j];

    if (j == hop_at) {
      continue;
    }
    if (j != s && ir_instruction_writes_destination(ins) &&
        ir_operand_is_temp(&ins->dest) &&
        strcmp(ins->dest.name, sh_name) == 0) {
      return 0;
    }
    if (j == s || !ir_row_instruction_reads_temp(ins, sh_name)) {
      continue;
    }
    if (j <= s || j >= latch || count >= IR_ROW_MAX_CONSUMERS) {
      return 0;
    }
    if (!ir_row_consumer_is_base_add(function, header, latch, ins, sh_name)) {
      return 0;
    }
    consumers[count++] = j;
  }
  *out_count = count;
  return count > 0;
}

static int ir_row_consumer_names(const IRFunction *function,
                                 const size_t *consumers, size_t count,
                                 char base_names[][128],
                                 char ptr_types[][64]) {
  for (size_t c = 0; c < count; c++) {
    const IRInstruction *addr = &function->instructions[consumers[c]];
    const char *pt = ir_row_symbol_pointer_type(function, addr->lhs.name);

    if (!pt ||
        snprintf(base_names[c], 128, "%s", addr->lhs.name) >= 128 ||
        snprintf(ptr_types[c], 64, "%s", pt) >= 64) {
      return 0;
    }
  }
  return 1;
}

static int ir_row_reuse_cached(IRRowPass *pass, IRRowShape *shape, size_t s,
                               size_t consumer, const char *base) {
  IRFunction *function = pass->function;
  int hit = ir_row_cache_find(pass->cache, pass->cache_count, base,
                              shape->bias, shape->k, &shape->inv);
  IRInstruction *addr;

  if (hit < 0) {
    return 0;
  }
  addr = &function->instructions[consumer];
  ir_operand_destroy(&addr->lhs);
  addr->lhs = ir_operand_symbol(pass->cache[hit].name);
  ir_row_retarget_shift(function, s, shape->k, &shape->var);
  if (shape->k != 0) {
    ir_row_drop_dead_index(function, shape->idx_pos);
  }
  ir_row_collapse_hop(function, s, shape->hop_at);
  if (pass->changed) {
    *pass->changed = 1;
  }
  return 1;
}

static int ir_row_emit_bias(IRRowPass *pass, size_t *at, size_t *inserted,
                            IROperand *inv, long long *bias,
                            const char *bias_name) {
  IRInstruction adj = {0};

  if (*bias == 0 || inv->kind == IR_OPERAND_INT) {
    return 1;
  }
  adj.op = IR_OP_BINARY;
  adj.text = mettle_strdup("+");
  adj.dest = ir_operand_temp(bias_name);
  adj.lhs = ir_operand_copy(inv);
  adj.rhs = ir_operand_int(*bias);
  if (!ir_row_insert(pass->function, at, inserted, &adj,
                     adj.text && adj.dest.name)) {
    return 0;
  }
  ir_operand_destroy(inv);
  *inv = ir_operand_temp(bias_name);
  *bias = 0;
  return 1;
}

static int ir_row_emit_offset(IRRowPass *pass, size_t *at, size_t *inserted,
                              const IROperand *inv, long long k,
                              const char *off_name) {
  IRInstruction off = {0};

  off.op = IR_OP_BINARY;
  off.text = mettle_strdup("<<");
  off.dest = ir_operand_temp(off_name);
  off.lhs = ir_operand_copy(inv);
  off.rhs = ir_operand_int(k);
  return ir_row_insert(pass->function, at, inserted, &off,
                       off.text && off.dest.name);
}

static int ir_row_emit_row_add(IRRowPass *pass, size_t *at, size_t *inserted,
                               const char *row_name, const char *base_name,
                               const IROperand *inv, long long bias,
                               long long k, int need_off_temp,
                               const char *off_name) {
  IRInstruction add = {0};

  add.op = IR_OP_BINARY;
  add.text = mettle_strdup("+");
  add.dest = ir_operand_symbol(row_name);
  add.lhs = ir_operand_symbol(base_name);
  add.rhs = need_off_temp ? ir_operand_temp(off_name)
                          : (inv->kind == IR_OPERAND_INT
                                 ? ir_operand_int((inv->int_value + bias) << k)
                                 : ir_operand_copy(inv));
  return ir_row_insert(pass->function, at, inserted, &add,
                       add.text && add.dest.name && add.lhs.name);
}

static int ir_row_emit_declarations(IRRowPass *pass, size_t *at,
                                    size_t *inserted, size_t count,
                                    char row_names[][48],
                                    char ptr_types[][64]) {
  for (size_t c = 0; c < count; c++) {
    IRInstruction decl = {0};

    decl.op = IR_OP_DECLARE_LOCAL;
    decl.dest = ir_operand_symbol(row_names[c]);
    decl.text = mettle_strdup(ptr_types[c]);
    if (!ir_row_insert(pass->function, at, inserted, &decl,
                       decl.dest.name && decl.text)) {
      return 0;
    }
  }
  return 1;
}

static void ir_row_cache_record(IRRowPass *pass, const char *base,
                                const char *row_name, long long bias,
                                long long k, const IROperand *inv) {
  IRRowCacheEntry *entry;

  if (pass->cache_count >= IR_ROW_MAX_CACHE) {
    return;
  }
  entry = &pass->cache[pass->cache_count];
  if (snprintf(entry->base, sizeof(entry->base), "%s", base) >=
          (int)sizeof(entry->base) ||
      snprintf(entry->name, sizeof(entry->name), "%s", row_name) >=
          (int)sizeof(entry->name)) {
    return;
  }
  entry->bias = bias;
  entry->k = k;
  entry->inv = ir_operand_copy(inv);
  pass->cache_count++;
}

static int ir_row_hoist_shift(IRRowPass *pass, size_t s, size_t *out_inserted) {
  static int g_row_counter;
  IRFunction *function = pass->function;
  IRRowShape shape = {0};
  size_t consumers[IR_ROW_MAX_CONSUMERS];
  size_t consumer_count = 0;
  char base_names[IR_ROW_MAX_CONSUMERS][128];
  char ptr_types[IR_ROW_MAX_CONSUMERS][64];
  char row_names[IR_ROW_MAX_CONSUMERS][48];
  char off_name[48];
  char bias_name[48];
  size_t at = pass->header;
  size_t inserted = 0;
  int need_off_temp;
  int ok;

  *out_inserted = 0;
  if (!ir_row_match_shape(function, pass->header, pass->latch, s, &shape)) {
    return 1;
  }
  ir_row_readers_drop();
  if (!ir_row_collect_consumers(function, pass->header, pass->latch, s,
                                shape.hop_at, shape.sh_name, consumers,
                                &consumer_count) ||
      !ir_row_consumer_names(function, consumers, consumer_count, base_names,
                             ptr_types)) {
    ir_operand_destroy(&shape.inv);
    ir_operand_destroy(&shape.var);
    return 1;
  }
  if (consumer_count == 1 &&
      ir_row_reuse_cached(pass, &shape, s, consumers[0], base_names[0])) {
    ir_operand_destroy(&shape.inv);
    ir_operand_destroy(&shape.var);
    return 1;
  }

  need_off_temp = shape.k != 0 && shape.inv.kind != IR_OPERAND_INT;
  snprintf(off_name, sizeof(off_name), "__rowoff_%d", g_row_counter);
  snprintf(bias_name, sizeof(bias_name), "__rowbias_%d", g_row_counter);
  for (size_t c = 0; c < consumer_count; c++) {
    IRInstruction *addr = &function->instructions[consumers[c]];
    snprintf(row_names[c], sizeof(row_names[c]), "__rowp_%d_%zu",
             g_row_counter, c);
    ir_operand_destroy(&addr->lhs);
    addr->lhs = ir_operand_symbol(row_names[c]);
  }
  g_row_counter++;
  ir_row_retarget_shift(function, s, shape.k, &shape.var);
  if (shape.k != 0) {
    ir_row_drop_dead_index(function, shape.idx_pos);
  }

  ok = ir_row_emit_declarations(pass, &at, &inserted, consumer_count,
                                row_names, ptr_types) &&
       ir_row_emit_bias(pass, &at, &inserted, &shape.inv, &shape.bias,
                        bias_name) &&
       (!need_off_temp ||
        ir_row_emit_offset(pass, &at, &inserted, &shape.inv, shape.k,
                           off_name));
  for (size_t c = 0; ok && c < consumer_count; c++) {
    ok = ir_row_emit_row_add(pass, &at, &inserted, row_names[c], base_names[c],
                             &shape.inv, shape.bias, shape.k, need_off_temp,
                             off_name);
  }
  if (ok && consumer_count == 1) {
    ir_row_cache_record(pass, base_names[0], row_names[0], shape.bias, shape.k,
                        &shape.inv);
  }
  ir_operand_destroy(&shape.inv);
  ir_operand_destroy(&shape.var);
  if (!ok) {
    return 0;
  }
  pass->header += inserted;
  pass->latch += inserted;
  if (shape.hop_at != (size_t)-1) {
    shape.hop_at += inserted;
  }
  ir_row_collapse_hop(function, s + inserted, shape.hop_at);
  if (pass->changed) {
    *pass->changed = 1;
  }
  *out_inserted = inserted;
  return 1;
}

int ir_hoist_row_pointers_pass(IRFunction *function, int *changed) {
  IRRowCacheEntry cache[IR_ROW_MAX_CACHE];
  IRRowPass pass;

  if (!function) {
    return 0;
  }
  pass.function = function;
  pass.cache = cache;
  pass.cache_count = 0;
  pass.changed = changed;
  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    size_t latch;

    if (label->op != IR_OP_LABEL ||
        !ir_cleanup_label_is_loop_header(label->text)) {
      continue;
    }
    latch = ir_cleanup_loop_latch(function, header, label->text);
    if (!latch) {
      continue;
    }
    ir_row_cache_clear(cache, &pass.cache_count);
    pass.header = header;
    pass.latch = latch;
    for (size_t s = pass.header + 1; s < pass.latch; s++) {
      size_t inserted = 0;
      if (!ir_row_hoist_shift(&pass, s, &inserted)) {
        ir_row_cache_clear(cache, &pass.cache_count);
        ir_row_readers_drop();
        return 0;
      }
      s += inserted;
    }
    header = pass.header;
  }
  ir_row_cache_clear(cache, &pass.cache_count);
  ir_row_readers_drop();
  return 1;
}

static int ir_scan_decode_indexed(const IRFunction *function, size_t before,
                                  const char *addr_temp, const char *iv,
                                  const char **base_out, long long *width_out) {
  const IRInstruction *addr =
      ir_find_temp_producer_before(function, before, addr_temp);
  const IRInstruction *shl = NULL;
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->lhs.kind != IR_OPERAND_SYMBOL ||
      !addr->lhs.name || addr->rhs.kind != IR_OPERAND_TEMP || !addr->rhs.name) {
    return 0;
  }
  shl = ir_find_temp_producer_before(function, before, addr->rhs.name);
  if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
      strcmp(shl->text, "<<") != 0 ||
      !ir_operand_is_symbol_named(&shl->lhs, iv) ||
      shl->rhs.kind != IR_OPERAND_INT || shl->rhs.int_value < 0 ||
      shl->rhs.int_value > 3) {
    return 0;
  }
  *base_out = addr->lhs.name;
  *width_out = 1LL << shl->rhs.int_value;
  return 1;
}

static int ir_scan_assigns_element(const IRFunction *function, size_t at,
                                   const char *sym, const char *iv,
                                   const char **base_out, long long *width_out) {
  const IRInstruction *ins = &function->instructions[at];
  const IRInstruction *load = ins;
  if (!ir_operand_is_symbol_named(&ins->dest, sym)) {
    return 0;
  }
  if (ins->op == IR_OP_ASSIGN && ins->lhs.kind == IR_OPERAND_TEMP &&
      ins->lhs.name) {
    load = ir_find_temp_producer_before(function, at, ins->lhs.name);
  }
  if (!load || load->op != IR_OP_LOAD || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name || load->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  {
    size_t li = (size_t)(load - function->instructions);
    const char *base = NULL;
    long long width = 0;
    if (!ir_scan_decode_indexed(function, li, load->lhs.name, iv, &base,
                                &width) ||
        width != load->rhs.int_value) {
      return 0;
    }
    *base_out = base;
    *width_out = width;
    return 1;
  }
}

static void ir_normalize_scan_at(int *changed, IRFunction *function) {
for (size_t header = 0; header < function->instruction_count; header++) {
  const IRInstruction *label = &function->instructions[header];
  size_t latch = 0;
  size_t init_index = 0;
  const char *iv = NULL;
  const char *acc = NULL;
  const char *base = NULL;
  long long width = 0;
  int found_init = 0;
  int ok = 1;

  if (label->op != IR_OP_LABEL ||
      !ir_cleanup_label_is_loop_header(label->text)) {
    continue;
  }
  latch = ir_cleanup_loop_latch(function, header, label->text);
  if (!latch) {
    continue;
  }
  {
    size_t compare_index = 0;
    const IRInstruction *cmp = NULL;
    if (!ir_find_next_non_nop(function, header + 1, &compare_index) ||
        compare_index >= latch) {
      continue;
    }
    cmp = &function->instructions[compare_index];
    if (cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text ||
        strcmp(cmp->text, "<") != 0 || cmp->lhs.kind != IR_OPERAND_SYMBOL ||
        !cmp->lhs.name) {
      continue;
    }
    iv = cmp->lhs.name;
  }
  for (size_t k = 0; k < header; k++) {
    const IRInstruction *ins = &function->instructions[k];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, iv)) {
      init_index = k;
      found_init = (ins->op == IR_OP_ASSIGN &&
                    ins->lhs.kind == IR_OPERAND_INT && ins->lhs.int_value == 1);
    }
  }
  if (!found_init) {
    continue;
  }
  for (size_t k = header + 1; k < latch && ok; k++) {
    const IRInstruction *ins = &function->instructions[k];
    const char *b = NULL;
    long long w = 0;
    if (ins->op == IR_OP_STORE || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_INLINE_ASM ||
        ins->op == IR_OP_ADDRESS_OF || ins->op == IR_OP_NEW) {
      ok = 0;
      break;
    }
    if (!ir_instruction_writes_destination(ins) ||
        ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name ||
        strcmp(ins->dest.name, iv) == 0) {
      continue;
    }
    if (acc && strcmp(acc, ins->dest.name) != 0) {
      ok = 0;
      break;
    }
    if (!ir_scan_assigns_element(function, k, ins->dest.name, iv, &b, &w) ||
        (base && strcmp(base, b) != 0) || (width && w != width)) {
      ok = 0;
      break;
    }
    acc = ins->dest.name;
    base = b;
    width = w;
  }
  if (!ok || !acc || !base) {
    continue;
  }
  {
    const IRInstruction *seed = NULL;
    for (size_t k = 0; k < header; k++) {
      const IRInstruction *ins = &function->instructions[k];
      if (ir_instruction_writes_destination(ins) &&
          ir_operand_is_symbol_named(&ins->dest, acc)) {
        seed = ins;
      }
      if (seed && ir_instruction_writes_destination(ins) &&
          ir_operand_is_symbol_named(&ins->dest, base)) {
        seed = NULL;
        break;
      }
    }
    if (!seed || seed->op != IR_OP_LOAD ||
        !ir_operand_is_symbol_named(&seed->lhs, base) ||
        seed->rhs.kind != IR_OPERAND_INT || seed->rhs.int_value != width) {
      continue;
    }
  }
  {
    IRInstruction *init = &function->instructions[init_index];
    ir_operand_destroy(&init->lhs);
    init->lhs = ir_operand_int(0);
    if (changed) {
      *changed = 1;
    }
  }
}
}

int ir_normalize_scan_from_first_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  ir_normalize_scan_at(changed, function);
  return 1;
}

static int ir_accum_condition_is_boolean(const IRFunction *function, size_t at,
                                         const char *temp) {
  const IRInstruction *p = ir_find_temp_producer_before(function, at, temp);
  if (!p || p->op != IR_OP_BINARY || p->is_float || !p->text) {
    return 0;
  }
  return strcmp(p->text, "<") == 0 || strcmp(p->text, ">") == 0 ||
         strcmp(p->text, "<=") == 0 || strcmp(p->text, ">=") == 0 ||
         strcmp(p->text, "==") == 0 || strcmp(p->text, "!=") == 0;
}

static int ir_accum_condition_is_nonzero_load(const IRFunction *function,
                                              size_t at, const char *temp) {
  const IRInstruction *p = ir_find_temp_producer_before(function, at, temp);
  return p && p->op == IR_OP_LOAD && !p->is_float;
}

static size_t ir_accum_condition_defined_at(const IRFunction *function,
                                            size_t header, size_t before,
                                            const char *cond) {
  for (size_t i = before; i > header; i--) {
    const IRInstruction *ins = &function->instructions[i - 1];

    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_temp_named(&ins->dest, cond)) {
      return i - 1;
    }
  }
  return header;
}

static size_t ir_accum_free_nop_slot(const IRFunction *function, size_t header,
                                     size_t before, const char *cond) {
  size_t defined_at =
      ir_accum_condition_defined_at(function, header, before, cond);

  for (size_t i = before; i > defined_at + 1; i--) {
    if (function->instructions[i - 1].op == IR_OP_NOP) {
      return i - 1;
    }
  }
  return 0;
}

static size_t ir_accum_open_nop_slot(IRFunction *function, size_t header,
                                     size_t *before, size_t *latch,
                                     const char *cond) {
  size_t at = ir_accum_condition_defined_at(function, header, *before, cond) + 1;
  IRInstruction nop = {0};

  nop.op = IR_OP_NOP;
  nop.location = function->instructions[*before].location;
  if (!ir_function_insert_instruction(function, at, &nop)) {
    return 0;
  }
  (*before)++;
  (*latch)++;
  return at;
}

static int ir_accum_operand_same(const IROperand *a, const IROperand *b) {
  if (a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case IR_OPERAND_NONE:
    return 1;
  case IR_OPERAND_INT:
    return a->int_value == b->int_value;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return a->name && b->name && ir_operand_names_match(a, b);
  default:
    return 0;
  }
}

static int ir_accum_chain_same(const IRFunction *function, size_t at_a,
                               const IROperand *a, size_t at_b,
                               const IROperand *b, int depth) {
  const IRInstruction *pa = NULL;
  const IRInstruction *pb = NULL;
  if (depth > 4) {
    return 0;
  }
  if (a->kind != IR_OPERAND_TEMP || b->kind != IR_OPERAND_TEMP) {
    return ir_accum_operand_same(a, b);
  }
  if (!a->name || !b->name) {
    return 0;
  }
  pa = ir_find_temp_producer_before(function, at_a, a->name);
  pb = ir_find_temp_producer_before(function, at_b, b->name);
  if (!pa || !pb || pa->op != pb->op || pa->is_float != pb->is_float ||
      pa->is_unsigned != pb->is_unsigned) {
    return 0;
  }
  if (pa->op != IR_OP_BINARY && pa->op != IR_OP_ADDRESS_OF &&
      pa->op != IR_OP_CAST) {
    return 0;
  }
  if ((pa->text == NULL) != (pb->text == NULL) ||
      (pa->text && strcmp(pa->text, pb->text) != 0)) {
    return 0;
  }
  {
    size_t ia = (size_t)(pa - function->instructions);
    size_t ib = (size_t)(pb - function->instructions);
    return ir_accum_chain_same(function, ia, &pa->lhs, ib, &pb->lhs,
                               depth + 1) &&
           ir_accum_chain_same(function, ia, &pa->rhs, ib, &pb->rhs, depth + 1);
  }
}

static int ir_accum_load_is_redundant(const IRFunction *function, size_t lo,
                                      size_t at) {
  const IRInstruction *load = &function->instructions[at];
  if (load->lhs.kind != IR_OPERAND_TEMP || !load->lhs.name) {
    return 0;
  }
  for (size_t i = lo; i < at; i++) {
    const IRInstruction *prior = &function->instructions[i];
    if (prior->op != IR_OP_LOAD || prior->rhs.kind != IR_OPERAND_INT ||
        load->rhs.kind != IR_OPERAND_INT ||
        prior->rhs.int_value != load->rhs.int_value) {
      continue;
    }
    if (ir_accum_chain_same(function, i, &prior->lhs, at, &load->lhs, 0)) {
      return 1;
    }
  }
  return 0;
}

static int ir_accum_label_is_reached(const IRFunction *function,
                                     const char *label) {
  if (!label) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if ((ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
         ins->op == IR_OP_BRANCH_EQ) &&
        ins->text && strcmp(ins->text, label) == 0) {
      return 1;
    }
  }
  return 0;
}

typedef struct {
  size_t add_index;
  size_t jump;
  size_t else_label;
  size_t end_label;
  const char *acc;
} IRAccumArm;

static size_t ir_accum_find_add(const IRFunction *function, size_t header,
                                size_t latch, size_t branch_index,
                                size_t *jump_out) {
  size_t add_index = 0;
  size_t jump;
  for (jump = branch_index + 1; jump < latch; jump++) {
    const IRInstruction *ins = &function->instructions[jump];
    if (ins->op == IR_OP_JUMP) {
      break;
    }
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_LOAD) {
      if (!ir_accum_load_is_redundant(function, header + 1, jump)) {
        add_index = 0;
        break;
      }
      continue;
    }
    if (ins->op == IR_OP_BINARY || ins->op == IR_OP_CAST ||
        ins->op == IR_OP_UNARY) {
      if (ins->dest.kind == IR_OPERAND_TEMP) {
        continue;
      }
    } else {
      add_index = 0;
      break;
    }
    if (add_index) {
      add_index = 0;
      break;
    }
    add_index = jump;
  }
  *jump_out = jump;
  return add_index;
}

static int ir_accum_match_arm(const IRFunction *function, size_t header,
                              size_t latch, size_t branch_index,
                              IRAccumArm *arm) {
  const IRInstruction *br = &function->instructions[branch_index];
  const IRInstruction *add;
  const IRInstruction *el;
  const IRInstruction *en;

  arm->add_index = ir_accum_find_add(function, header, latch, branch_index,
                                     &arm->jump);
  if (!arm->add_index || arm->jump >= latch ||
      !function->instructions[arm->jump].text) {
    return 0;
  }

  add = &function->instructions[arm->add_index];
  if (add->op != IR_OP_BINARY || add->is_float || !add->text ||
      strcmp(add->text, "+") != 0 || add->dest.kind != IR_OPERAND_SYMBOL ||
      !add->dest.name ||
      !ir_operand_is_symbol_named(&add->lhs, add->dest.name) ||
      (add->rhs.kind != IR_OPERAND_INT && add->rhs.kind != IR_OPERAND_TEMP &&
       add->rhs.kind != IR_OPERAND_SYMBOL)) {
    return 0;
  }
  arm->acc = add->dest.name;

  if (!ir_find_next_non_nop(function, arm->jump + 1, &arm->else_label) ||
      arm->else_label >= latch ||
      !ir_find_next_non_nop(function, arm->else_label + 1, &arm->end_label) ||
      arm->end_label >= latch) {
    return 0;
  }
  el = &function->instructions[arm->else_label];
  en = &function->instructions[arm->end_label];
  if (el->op != IR_OP_LABEL || !el->text || strcmp(el->text, br->text) != 0 ||
      en->op != IR_OP_LABEL || !en->text ||
      strcmp(en->text, function->instructions[arm->jump].text) != 0) {
    return 0;
  }

  for (size_t k = header + 1; k < latch; k++) {
    if (k != arm->add_index &&
        ir_instruction_writes_destination(&function->instructions[k]) &&
        ir_operand_is_symbol_named(&function->instructions[k].dest, arm->acc)) {
      return 0;
    }
  }
  return 1;
}

static int ir_accum_scale_addend(IRFunction *function, const IRAccumArm *arm,
                                 const char *cond, unsigned *minted) {
  IRInstruction *add = &function->instructions[arm->add_index];
  char product[64];
  IRInstruction mul = {0};
  IRInstruction sum = {0};

  snprintf(product, sizeof(product), ".ifacc%u", (*minted)++);
  mul.op = IR_OP_BINARY;
  mul.location = add->location;
  mul.text = mettle_strdup("*");
  mul.dest = ir_operand_temp(product);
  mul.lhs = ir_operand_copy(&add->rhs);
  mul.rhs = ir_operand_temp(cond);
  sum.op = IR_OP_BINARY;
  sum.location = add->location;
  sum.text = mettle_strdup("+");
  sum.dest = ir_operand_copy(&add->dest);
  sum.lhs = ir_operand_copy(&add->lhs);
  sum.rhs = ir_operand_temp(product);
  if (!mul.text || !mul.dest.name || !mul.rhs.name || !sum.text ||
      !sum.dest.name || !sum.lhs.name || !sum.rhs.name) {
    ir_instruction_destroy_storage(&mul);
    ir_instruction_destroy_storage(&sum);
    return 0;
  }
  ir_instruction_destroy_storage(&function->instructions[arm->add_index]);
  function->instructions[arm->add_index] = mul;
  ir_instruction_destroy_storage(&function->instructions[arm->jump]);
  function->instructions[arm->jump] = sum;
  return 1;
}

static int ir_accum_rewrite_arm(IRFunction *function, size_t branch_index,
                                const IRAccumArm *arm, const char *cond,
                                size_t rematerialize_at, unsigned *minted,
                                int *changed) {
  IRInstruction *add = &function->instructions[arm->add_index];
  int addend_is_one =
      add->rhs.kind == IR_OPERAND_INT && add->rhs.int_value == 1;
  char boolean[64];

  if (rematerialize_at) {
    IRInstruction cmp = {0};
    snprintf(boolean, sizeof(boolean), ".ifne%u", (*minted)++);
    cmp.op = IR_OP_BINARY;
    cmp.location = function->instructions[branch_index].location;
    cmp.text = mettle_strdup("!=");
    cmp.dest = ir_operand_temp(boolean);
    cmp.lhs = ir_operand_temp(cond);
    cmp.rhs = ir_operand_int(0);
    if (!cmp.text || !cmp.dest.name || !cmp.lhs.name) {
      ir_instruction_destroy_storage(&cmp);
      return 0;
    }
    ir_instruction_destroy_storage(&function->instructions[rematerialize_at]);
    function->instructions[rematerialize_at] = cmp;
    cond = boolean;
  }

  if (addend_is_one) {
    ir_operand_destroy(&add->rhs);
    add->rhs = ir_operand_temp(cond);
    if (!add->rhs.name) {
      return 0;
    }
    ir_instruction_make_nop(&function->instructions[arm->jump]);
  } else if (!ir_accum_scale_addend(function, arm, cond, minted)) {
    return 0;
  }
  ir_instruction_make_nop(&function->instructions[branch_index]);

  if (!ir_accum_label_is_reached(
          function, function->instructions[arm->else_label].text)) {
    ir_instruction_make_nop(&function->instructions[arm->else_label]);
  }
  if (!ir_accum_label_is_reached(
          function, function->instructions[arm->end_label].text)) {
    ir_instruction_make_nop(&function->instructions[arm->end_label]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_if_convert_accumulate_pass(IRFunction *function, int *changed) {
  static unsigned minted;

  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    size_t latch = 0;

    if (label->op != IR_OP_LABEL ||
        !ir_cleanup_label_is_loop_header(label->text)) {
      continue;
    }
    latch = ir_cleanup_loop_latch(function, header, label->text);
    if (!latch) {
      continue;
    }

    for (size_t i = header + 1; i < latch; i++) {
      const IRInstruction *br = &function->instructions[i];
      IRAccumArm arm = {0};
      const char *cond = NULL;
      size_t rematerialize_at = 0;

      if (br->op != IR_OP_BRANCH_ZERO || !br->text ||
          br->lhs.kind != IR_OPERAND_TEMP || !br->lhs.name) {
        continue;
      }
      cond = br->lhs.name;
      if (!ir_accum_condition_is_boolean(function, i, cond)) {
        if (!ir_accum_condition_is_nonzero_load(function, i, cond)) {
          continue;
        }
        rematerialize_at = ir_accum_free_nop_slot(function, header, i, cond);
        if (!rematerialize_at) {
          rematerialize_at =
              ir_accum_open_nop_slot(function, header, &i, &latch, cond);
          if (!rematerialize_at) {
            continue;
          }
          br = &function->instructions[i];
          cond = br->lhs.name;
        }
      }

      if (!ir_accum_match_arm(function, header, latch, i, &arm)) {
        continue;
      }
      if (!ir_accum_rewrite_arm(function, i, &arm, cond, rematerialize_at,
                                &minted, changed)) {
        return 0;
      }
      i = arm.end_label;
    }
  }
  return 1;
}

static int ir_load_copy_build_facts(const IRFunction *function,
                                    IRNameIndex *address_taken,
                                    IRNameIndex *reads) {
  if (address_taken) {
    if (!ir_name_index_init(address_taken, function->instruction_count)) {
      return 0;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ins->op == IR_OP_ADDRESS_OF && ins->lhs.kind == IR_OPERAND_SYMBOL &&
          ins->lhs.name) {
        ir_name_index_insert(address_taken, ins->lhs.name, 1);
      }
    }
  }
  if (!ir_name_index_init(reads, function->instruction_count)) {
    if (address_taken) {
      ir_name_index_destroy(address_taken);
    }
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_operand_is_symbol(&ins->lhs)) {
      ir_name_index_add(reads, ins->lhs.name, 1);
    }
    if (ir_operand_is_symbol(&ins->rhs)) {
      ir_name_index_add(reads, ins->rhs.name, 1);
    }
    if (ins->op == IR_OP_STORE && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name) {
      ir_name_index_add(reads, ins->dest.name, 1);
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ins->arguments[a].kind == IR_OPERAND_SYMBOL &&
          ins->arguments[a].name) {
        ir_name_index_add(reads, ins->arguments[a].name, 1);
      }
    }
  }
  return 1;
}

int ir_eliminate_load_symbol_copy_pass(IRFunction *function,
                                              int *changed) {
  IRNameIndex address_taken;
  IRNameIndex reads;

  if (!function) {
    return 0;
  }
  if (!ir_load_copy_build_facts(function, &address_taken, &reads)) {
    return 0;
  }

  for (size_t i = 0; i + 1 < function->instruction_count; i++) {
    IRInstruction *load = &function->instructions[i];
    IRInstruction *assign = NULL;
    size_t assign_index = i + 1;
    const char *sym = NULL;
    const char *temp = NULL;
    size_t window_reads = 0;
    size_t total_reads = 0;
    size_t window_end = function->instruction_count;
    size_t j = 0;
    int unsafe_use = 0;

    if (load->op != IR_OP_LOAD || load->dest.kind != IR_OPERAND_TEMP ||
        !load->dest.name) {
      continue;
    }

    while (assign_index < function->instruction_count &&
           (function->instructions[assign_index].op == IR_OP_NOP ||
            function->instructions[assign_index].op == IR_OP_DECLARE_LOCAL)) {
      assign_index++;
    }
    if (assign_index >= function->instruction_count) {
      continue;
    }
    assign = &function->instructions[assign_index];

    if (assign->op != IR_OP_ASSIGN ||
        assign->dest.kind != IR_OPERAND_SYMBOL || !assign->dest.name ||
        assign->lhs.kind != IR_OPERAND_TEMP || !assign->lhs.name ||
        !ir_operand_names_match(&assign->lhs, &load->dest)) {
      continue;
    }

    if (assign->is_float) {
      long long width_bytes = (assign->float_bits == 32) ? 4 : 8;
      if (load->rhs.kind != IR_OPERAND_INT ||
          load->rhs.int_value != width_bytes) {
        continue;
      }
    }

    sym = assign->dest.name;
    temp = load->dest.name;

    if (ir_name_index_find(&address_taken, sym, NULL)) {
      continue;
    }

    if (!ir_name_index_find(&reads, sym, &total_reads)) {
      total_reads = 0;
    }

    for (j = assign_index + 1; j < function->instruction_count; j++) {
      const IRInstruction *ins = &function->instructions[j];
      if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
          ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
        window_end = j;
        break;
      }
      if (ir_instruction_writes_symbol(ins) &&
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        if (ir_load_copy_count_symbol_reads(ins, sym) > 0) {
          unsafe_use = 1;
        }
        window_end = j;
        break;
      }
      if (ins->op == IR_OP_STORE &&
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        unsafe_use = 1;
        break;
      }
      window_reads += ir_load_copy_count_symbol_reads(ins, sym);
      if (window_reads > 6) {
        window_end = j + 1;
        break;
      }
      if (total_reads > 0 && window_reads == total_reads) {
        window_end = j + 1;
        break;
      }
    }

    if (unsafe_use || window_reads == 0 || window_reads > 6) {
      continue;
    }

    if (window_reads != total_reads) {
      continue;
    }

    for (j = assign_index + 1; j < window_end; j++) {
      IRInstruction *ins = &function->instructions[j];
      ir_load_copy_replace_operand(&ins->lhs, sym, temp);
      ir_load_copy_replace_operand(&ins->rhs, sym, temp);
      for (size_t a = 0; a < ins->argument_count; a++) {
        ir_load_copy_replace_operand(&ins->arguments[a], sym, temp);
      }
    }

    ir_instruction_make_nop(assign);
    ir_name_index_destroy(&reads);
    if (!ir_load_copy_build_facts(function, NULL, &reads)) {
      ir_name_index_destroy(&address_taken);
      return 0;
    }
    if (changed) {
      *changed = 1;
    }
  }

  ir_name_index_destroy(&address_taken);
  ir_name_index_destroy(&reads);

  {
    IRNameIndex mentions;
    if (!ir_name_index_init(&mentions, function->instruction_count)) {
      return 0;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ins->op == IR_OP_NOP) {
        continue;
      }
      ir_name_index_add(&mentions, ir_load_copy_mentioned_name(&ins->dest), 1);
      ir_name_index_add(&mentions, ir_load_copy_mentioned_name(&ins->lhs), 1);
      ir_name_index_add(&mentions, ir_load_copy_mentioned_name(&ins->rhs), 1);
      for (size_t a = 0; a < ins->argument_count; a++) {
        ir_name_index_add(&mentions,
                          ir_load_copy_mentioned_name(&ins->arguments[a]), 1);
      }
    }

    for (size_t i = 0; i < function->instruction_count; i++) {
      IRInstruction *decl = &function->instructions[i];
      size_t total = 0;
      size_t own;

      if (decl->op != IR_OP_DECLARE_LOCAL ||
          decl->dest.kind != IR_OPERAND_SYMBOL || !decl->dest.name) {
        continue;
      }

      own = ir_load_copy_count_symbol_mentions(decl, decl->dest.name);
      if (ir_name_index_find(&mentions, decl->dest.name, &total) &&
          total > own) {
        continue;
      }

      ir_name_index_sub(&mentions, decl->dest.name, own);
      ir_instruction_make_nop(decl);
      if (changed) {
        *changed = 1;
      }
    }
    ir_name_index_destroy(&mentions);
  }

  return 1;
}

static size_t ir_narrowing_width(const char *type_name) {
  if (!type_name) {
    return 0;
  }
  if (strcmp(type_name, "int8") == 0 || strcmp(type_name, "uint8") == 0) {
    return 1;
  }
  if (strcmp(type_name, "int16") == 0 || strcmp(type_name, "uint16") == 0) {
    return 2;
  }
  if (strcmp(type_name, "int32") == 0 || strcmp(type_name, "uint32") == 0) {
    return 4;
  }
  return 0;
}

static int ir_narrowing_op_keeps_low_bits(const IRInstruction *in, int slot) {
  if (in->op != IR_OP_BINARY || in->is_float || !in->text) {
    return 0;
  }
  if (strcmp(in->text, "+") == 0 || strcmp(in->text, "-") == 0 ||
      strcmp(in->text, "*") == 0) {
    return 1;
  }
  return slot == 1 && strcmp(in->text, "<<") == 0;
}

static void ir_narrowing_count_operand(IRNameIndex *uses,
                                       const IROperand *operand) {
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    ir_name_index_add(uses, operand->name, 1);
  }
}

typedef struct {
  IRNameIndex uses;
  IRNameIndex defs;
  IRNameIndex def_at;
  size_t *narrowed_to;
} IRNarrowingFacts;

static void ir_narrowing_facts_destroy(IRNarrowingFacts *facts) {
  ir_name_index_destroy(&facts->uses);
  ir_name_index_destroy(&facts->defs);
  ir_name_index_destroy(&facts->def_at);
  free(facts->narrowed_to);
  facts->narrowed_to = NULL;
}

static int ir_narrowing_single_def(const IRNarrowingFacts *facts,
                                   const char *name) {
  size_t count = 0;
  return ir_name_index_find(&facts->defs, name, &count) && count == 1;
}

static int ir_narrowing_from_arithmetic(const IRFunction *function,
                                        const IRNarrowingFacts *facts,
                                        const char *name) {
  size_t at = 0;
  const IRInstruction *def;
  if (!ir_name_index_find(&facts->def_at, name, &at) ||
      at >= function->instruction_count) {
    return 0;
  }
  def = &function->instructions[at];
  return (def->op == IR_OP_BINARY || def->op == IR_OP_UNARY) && !def->is_float;
}

static int ir_narrowing_single_use(const IRNarrowingFacts *facts,
                                   const char *name) {
  size_t count = 0;
  return ir_name_index_find(&facts->uses, name, &count) && count == 1;
}

static size_t ir_narrowing_type_bytes(const char *type_name) {
  size_t len;
  if (!type_name) {
    return 0;
  }
  if (strcmp(type_name, "bool") == 0) {
    return 1;
  }
  if (strcmp(type_name, "int64") == 0 || strcmp(type_name, "uint64") == 0 ||
      strcmp(type_name, "cstring") == 0 || strcmp(type_name, "rawptr") == 0) {
    return 8;
  }
  len = strlen(type_name);
  if (len > 0 && type_name[len - 1] == '*') {
    return 8;
  }
  return ir_narrowing_width(type_name);
}

static size_t ir_narrowing_symbol_bytes(const IRFunction *function,
                                        const char *name) {
  const char *type_name = ir_function_local_declared_type(function, name);
  if (!type_name && function->parameter_names && function->parameter_types) {
    for (size_t p = 0; p < function->parameter_count; p++) {
      if (function->parameter_names[p] &&
          strcmp(function->parameter_names[p], name) == 0) {
        type_name = function->parameter_types[p];
        break;
      }
    }
  }
  return ir_narrowing_type_bytes(type_name);
}

static size_t ir_narrowing_constant_bytes(long long value) {
  if (value >= -128 && value <= 255) {
    return 1;
  }
  if (value >= -32768 && value <= 65535) {
    return 2;
  }
  if (value >= -2147483648LL && value <= 4294967295LL) {
    return 4;
  }
  return 8;
}

static size_t ir_narrowing_def_bytes(const IRFunction *function,
                                     const IRNarrowingFacts *facts,
                                     const char *name, int depth);

static size_t ir_narrowing_operand_bytes(const IRFunction *function,
                                         const IRNarrowingFacts *facts,
                                         const IROperand *operand,
                                         int depth) {
  if (operand->kind == IR_OPERAND_INT) {
    return ir_narrowing_constant_bytes(operand->int_value);
  }
  if (!operand->name) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL) {
    return ir_narrowing_symbol_bytes(function, operand->name);
  }
  if (operand->kind == IR_OPERAND_TEMP) {
    return ir_narrowing_def_bytes(function, facts, operand->name, depth + 1);
  }
  return 0;
}

static int ir_narrowing_op_is_predicate(const char *op) {
  static const char *const PREDICATES[] = {"<",  "<=", ">", ">=",
                                           "==", "!=", "&&", "||"};
  for (size_t i = 0; i < sizeof(PREDICATES) / sizeof(PREDICATES[0]); i++) {
    if (strcmp(op, PREDICATES[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static size_t ir_narrowing_def_bytes(const IRFunction *function,
                                     const IRNarrowingFacts *facts,
                                     const char *name, int depth) {
  size_t at = 0;
  size_t lhs;
  size_t rhs;
  const IRInstruction *def;
  if (depth > 8 || !ir_narrowing_single_def(facts, name) ||
      !ir_name_index_find(&facts->def_at, name, &at) ||
      at >= function->instruction_count) {
    return 0;
  }
  def = &function->instructions[at];
  switch (def->op) {
  case IR_OP_CAST:
    return ir_narrowing_type_bytes(def->text);
  case IR_OP_LOAD:
    return def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value > 0
               ? (size_t)def->rhs.int_value
               : 0;
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
    return def->value_type ? mtlc_type_size(def->value_type) : 0;
  case IR_OP_ASSIGN:
    return ir_narrowing_operand_bytes(function, facts, &def->lhs, depth);
  case IR_OP_UNARY:
    if (def->is_float || !def->text) {
      return 0;
    }
    if (strcmp(def->text, "!") == 0) {
      return 1;
    }
    return ir_narrowing_operand_bytes(function, facts, &def->lhs, depth);
  case IR_OP_BINARY:
    if (def->is_float || !def->text) {
      return 0;
    }
    if (ir_narrowing_op_is_predicate(def->text)) {
      return 1;
    }
    lhs = ir_narrowing_operand_bytes(function, facts, &def->lhs, depth);
    if (strcmp(def->text, "<<") == 0 || strcmp(def->text, ">>") == 0) {
      return lhs;
    }
    rhs = ir_narrowing_operand_bytes(function, facts, &def->rhs, depth);
    return lhs > rhs ? lhs : rhs;
  default:
    return 0;
  }
}

static int ir_narrowing_producer_is_wider(const IRFunction *function,
                                          const IRNarrowingFacts *facts,
                                          const char *name, size_t width) {
  return ir_narrowing_def_bytes(function, facts, name, 0) > width;
}

static int ir_narrowing_sink_is_narrower(const IRFunction *function,
                                         const IRNarrowingFacts *facts,
                                         size_t use_at, int slot,
                                         size_t width) {
  const IRInstruction *use = &function->instructions[use_at];
  size_t sink = 0;
  if (slot == 1 && use->op == IR_OP_CAST) {
    sink = ir_narrowing_width(use->text);
  } else if (slot == 1 && use->op == IR_OP_ASSIGN &&
             ir_operand_is_symbol(&use->dest) &&
             !use->is_float) {
    sink = ir_narrowing_width(
        ir_function_local_declared_type(function, use->dest.name));
  } else if (ir_narrowing_op_keeps_low_bits(use, slot)) {
    sink = facts->narrowed_to[use_at];
  }
  return sink != 0 && sink <= width;
}

static void ir_narrowing_count_names(IRFunction *function,
                                     IRNarrowingFacts *facts) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    ir_narrowing_count_operand(&facts->uses, &in->lhs);
    ir_narrowing_count_operand(&facts->uses, &in->rhs);
    for (size_t a = 0; a < in->argument_count; a++) {
      ir_narrowing_count_operand(&facts->uses, &in->arguments[a]);
    }
    if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name) {
      continue;
    }
    if (ir_instruction_writes_destination(in)) {
      ir_name_index_add(&facts->defs, in->dest.name, 1);
      ir_name_index_insert(&facts->def_at, in->dest.name, i);
    } else {
      ir_name_index_add(&facts->uses, in->dest.name, 1);
    }
  }
}

static void ir_narrowing_mark_producers(IRFunction *function,
                                        IRNarrowingFacts *facts) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *cast = &function->instructions[i];
    size_t width;
    size_t producer = 0;
    if (cast->op != IR_OP_CAST || cast->is_float ||
        cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name) {
      continue;
    }
    width = ir_narrowing_width(cast->text);
    if (width == 0 || !ir_narrowing_single_def(facts, cast->lhs.name) ||
        !ir_narrowing_single_use(facts, cast->lhs.name) ||
        !ir_narrowing_from_arithmetic(function, facts, cast->lhs.name)) {
      continue;
    }
    if (ir_name_index_find(&facts->def_at, cast->lhs.name, &producer)) {
      facts->narrowed_to[producer] = width;
    }
  }
}

static void ir_narrowing_choose_retirements(IRFunction *function,
                                            IRNarrowingFacts *facts,
                                            unsigned char *retire,
                                            size_t *retire_use) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *cast = &function->instructions[i];
    size_t width;
    if (cast->op != IR_OP_CAST || cast->is_float ||
        cast->dest.kind != IR_OPERAND_TEMP || !cast->dest.name ||
        cast->lhs.kind != IR_OPERAND_TEMP || !cast->lhs.name) {
      continue;
    }
    width = ir_narrowing_width(cast->text);
    if (width == 0 || !ir_narrowing_single_use(facts, cast->dest.name) ||
        !ir_narrowing_single_def(facts, cast->dest.name) ||
        !ir_narrowing_single_def(facts, cast->lhs.name) ||
        !ir_narrowing_from_arithmetic(function, facts, cast->lhs.name) ||
        ir_narrowing_producer_is_wider(function, facts, cast->lhs.name,
                                       width)) {
      continue;
    }
    for (size_t u = i + 1; u < function->instruction_count; u++) {
      const IRInstruction *use = &function->instructions[u];
      int in_lhs = ir_operand_is_temp_named(&use->lhs, cast->dest.name);
      int in_rhs = ir_operand_is_temp_named(&use->rhs, cast->dest.name);
      if (!in_lhs && !in_rhs) {
        continue;
      }
      if (in_lhs && in_rhs) {
        break;
      }
      if (ir_narrowing_sink_is_narrower(function, facts, u, in_lhs ? 1 : 2,
                                        width)) {
        retire[i] = in_lhs ? 1 : 2;
        retire_use[i] = u;
      }
      break;
    }
  }
}

static int ir_narrowing_apply_retirements(IRFunction *function,
                                          const unsigned char *retire,
                                          const size_t *retire_use,
                                          int *changed) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *cast;
    IRInstruction *use;
    IROperand *target;
    char *source;
    if (!retire[i]) {
      continue;
    }
    cast = &function->instructions[i];
    use = &function->instructions[retire_use[i]];
    target = retire[i] == 1 ? &use->lhs : &use->rhs;
    source = mettle_strdup(cast->lhs.name);
    if (!source) {
      return 0;
    }
    ir_operand_destroy(target);
    *target = ir_operand_temp(source);
    free(source);
    if (!target->name) {
      return 0;
    }
    ir_instruction_make_nop(cast);
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}

int ir_drop_dead_narrowing_pass(IRFunction *function, int *changed) {
  IRNarrowingFacts facts;
  unsigned char *retire = NULL;
  size_t *retire_use = NULL;
  int applied;
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  memset(&facts, 0, sizeof(facts));
  if (!ir_name_index_init(&facts.uses, function->instruction_count * 2) ||
      !ir_name_index_init(&facts.defs, function->instruction_count * 2) ||
      !ir_name_index_init(&facts.def_at, function->instruction_count * 2)) {
    ir_narrowing_facts_destroy(&facts);
    return 0;
  }
  facts.narrowed_to =
      (size_t *)calloc(function->instruction_count, sizeof(size_t));
  if (!facts.narrowed_to) {
    ir_narrowing_facts_destroy(&facts);
    return 0;
  }

  ir_narrowing_count_names(function, &facts);
  ir_narrowing_mark_producers(function, &facts);

  retire = (unsigned char *)calloc(function->instruction_count, 1);
  retire_use = (size_t *)calloc(function->instruction_count, sizeof(size_t));
  if (!retire || !retire_use) {
    free(retire);
    free(retire_use);
    ir_narrowing_facts_destroy(&facts);
    return 0;
  }
  ir_narrowing_choose_retirements(function, &facts, retire, retire_use);
  ir_narrowing_facts_destroy(&facts);

  applied = ir_narrowing_apply_retirements(function, retire, retire_use,
                                           changed);
  free(retire);
  free(retire_use);
  return applied;
}

static size_t ir_guard_temp_def_index(const IRFunction *function,
                                      const char *name) {
  size_t found = (size_t)-1;
  size_t count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (ir_instruction_writes_destination(in) &&
        ir_operand_is_temp(&in->dest) &&
        strcmp(in->dest.name, name) == 0) {
      found = i;
      count++;
    }
  }
  return count == 1 ? found : (size_t)-1;
}

static int ir_guard_body_is_quiet(const IRFunction *function, size_t lo,
                                  size_t hi) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->is_volatile) {
      return 0;
    }
    switch (in->op) {
    case IR_OP_STORE:
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_INLINE_ASM:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

static int ir_guard_header_entry_is_sole(const IRFunction *function,
                                         const char *header_label,
                                         size_t header, size_t latch) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LABEL || !in->text ||
        strcmp(in->text, header_label) != 0) {
      continue;
    }
    if (i < header || i > latch) {
      return 0;
    }
  }
  return 1;
}

/* A load whose address is already loop invariant still costs one access per
   iteration, and in a byte compare that is a third of the body. Moving it to
   the preheader would read memory a zero-trip loop never touched, so copy the
   loop's own entry test in front of it: the read then happens exactly when the
   first iteration would have made it. */
typedef struct {
  const char *header_label;
  const IRInstruction *test;
  const IRInstruction *branch;
  size_t latch;
} IRGuardLoop;

static int ir_guard_match_loop(const IRFunction *function, size_t header,
                               IRGuardLoop *loop) {
  const IRInstruction *label = &function->instructions[header];
  IROpcode before;

  if (label->op != IR_OP_LABEL || !label->text ||
      !ir_cleanup_label_is_loop_header(label->text)) {
    return 0;
  }
  loop->header_label = label->text;
  loop->test = &function->instructions[header + 1];
  loop->branch = &function->instructions[header + 2];
  if (loop->test->op != IR_OP_BINARY || loop->test->is_float ||
      !loop->test->text || loop->test->dest.kind != IR_OPERAND_TEMP ||
      !loop->test->dest.name ||
      !ir_licm_op_is_pure_arith(function, loop->test, 1)) {
    return 0;
  }
  if (loop->branch->op != IR_OP_BRANCH_ZERO || !loop->branch->text ||
      loop->branch->lhs.kind != IR_OPERAND_TEMP || !loop->branch->lhs.name ||
      !ir_operand_names_match(&loop->branch->lhs, &loop->test->dest)) {
    return 0;
  }
  loop->latch = ir_cleanup_loop_latch(function, header, loop->header_label);
  if (!loop->latch || header == 0 || loop->latch <= header + 3) {
    return 0;
  }
  before = ir_cleanup_op_before(function, header);
  if (ir_cleanup_op_leaves_block(before) || before == IR_OP_LABEL) {
    return 0;
  }
  return ir_guard_header_entry_is_sole(function, loop->header_label, header,
                                       loop->latch) &&
         ir_guard_body_is_quiet(function, header + 3, loop->latch);
}

static size_t ir_guard_body_prefix_end(const IRFunction *function, size_t from,
                                       size_t latch) {
  size_t at = from;

  while (at < latch) {
    IROpcode op = function->instructions[at].op;
    if (op == IR_OP_LABEL || ir_cleanup_op_leaves_block(op)) {
      break;
    }
    at++;
  }
  return at;
}

static int ir_guard_load_is_hoistable(const IRFunction *function,
                                      size_t header, size_t latch, size_t at) {
  const IRInstruction *in = &function->instructions[at];
  size_t addr_def;

  if (in->op != IR_OP_LOAD || in->is_volatile ||
      in->rhs.kind != IR_OPERAND_INT || in->lhs.kind != IR_OPERAND_TEMP ||
      !in->lhs.name || in->dest.kind != IR_OPERAND_TEMP || !in->dest.name) {
    return 0;
  }
  addr_def = ir_guard_temp_def_index(function, in->lhs.name);
  return addr_def != (size_t)-1 && addr_def < header &&
         ir_guard_temp_def_index(function, in->dest.name) == at &&
         ir_licm_read_count(function, header + 3, latch, at, in->dest.name) != 0;
}

static size_t ir_guard_pick_load(const IRFunction *function, size_t header,
                                 size_t latch, size_t prefix_end) {
  for (size_t i = header + 3; i < prefix_end; i++) {
    if (ir_guard_load_is_hoistable(function, header, latch, i)) {
      return i;
    }
  }
  return (size_t)-1;
}

static int ir_guard_emit(IRFunction *function, size_t header, size_t pick,
                         const char *guard_name) {
  IRInstruction guard_test = {0};
  IRInstruction guard_branch = {0};
  IRInstruction hoisted = {0};
  int ok;

  if (!ir_clone_instruction_plain(&function->instructions[header + 1],
                                  &guard_test) ||
      !ir_clone_instruction_plain(&function->instructions[header + 2],
                                  &guard_branch) ||
      !ir_clone_instruction_plain(&function->instructions[pick], &hoisted)) {
    ir_instruction_destroy_storage(&guard_test);
    ir_instruction_destroy_storage(&guard_branch);
    ir_instruction_destroy_storage(&hoisted);
    return 0;
  }
  ir_operand_destroy(&guard_test.dest);
  guard_test.dest = ir_operand_temp(guard_name);
  ir_operand_destroy(&guard_branch.lhs);
  guard_branch.lhs = ir_operand_temp(guard_name);
  ok = guard_test.dest.name && guard_branch.lhs.name &&
       ir_function_insert_instruction(function, header, &guard_test) &&
       ir_function_insert_instruction(function, header + 1, &guard_branch) &&
       ir_function_insert_instruction(function, header + 2, &hoisted);
  ir_instruction_destroy_storage(&guard_test);
  ir_instruction_destroy_storage(&guard_branch);
  ir_instruction_destroy_storage(&hoisted);
  if (!ok) {
    return 0;
  }
  ir_instruction_make_nop(&function->instructions[pick + 3]);
  return 1;
}

int ir_guard_loop_and_hoist_load_pass(IRFunction *function, int *changed) {
  static int g_guard_counter;

  if (!function || function->instruction_count == 0) {
    return 1;
  }
  for (size_t header = 0; header + 3 < function->instruction_count; header++) {
    IRGuardLoop loop = {0};
    size_t pick;
    char guard_name[48];

    if (!ir_guard_match_loop(function, header, &loop)) {
      continue;
    }
    pick = ir_guard_pick_load(
        function, header, loop.latch,
        ir_guard_body_prefix_end(function, header + 3, loop.latch));
    if (pick == (size_t)-1) {
      continue;
    }
    snprintf(guard_name, sizeof(guard_name), "__lguard_%d", g_guard_counter++);
    if (!ir_guard_emit(function, header, pick, guard_name)) {
      return 0;
    }
    header += 3;
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}
