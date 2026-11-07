#include "ir_optimize_internal.h"
#include "../../common.h"

static int ir_try_parse_loop_increment(const IRFunction *function, size_t body_start,
                                       size_t body_end, const char *counter_symbol,
                                       size_t *increment_index, int *step_out) {
  if (!function || !counter_symbol || !increment_index || !step_out) {
    return 0;
  }

  for (size_t i = body_end; i > body_start; ) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }

    if (instruction->op == IR_OP_BINARY && !instruction->is_float && instruction->text &&
        strcmp(instruction->text, "+") == 0 &&
        ir_operand_is_symbol(&instruction->dest) &&
        strcmp(instruction->dest.name, counter_symbol) == 0) {
      if (ir_operand_is_symbol(&instruction->lhs) &&
          strcmp(instruction->lhs.name, counter_symbol) == 0 &&
          instruction->rhs.kind == IR_OPERAND_INT) {
        if (instruction->rhs.int_value == 1) {
          *step_out = 1;
          *increment_index = i;
          return 1;
        }
        if (instruction->rhs.int_value == -1) {
          *step_out = -1;
          *increment_index = i;
          return 1;
        }
      }
    }

    if (instruction->op == IR_OP_ASSIGN &&
        ir_operand_is_symbol(&instruction->dest) &&
        strcmp(instruction->dest.name, counter_symbol) == 0) {
      if (ir_operand_is_temp(&instruction->lhs)) {
        size_t producer_index = 0;
        if (!ir_find_last_writer_before(function, i, IR_OPERAND_TEMP,
                                        instruction->lhs.name, &producer_index)) {
          return 0;
        }

        const IRInstruction *producer = &function->instructions[producer_index];
        if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text ||
            strcmp(producer->text, "+") != 0) {
          return 0;
        }

        if (ir_operand_is_symbol(&producer->lhs) &&
            strcmp(producer->lhs.name, counter_symbol) == 0 &&
            producer->rhs.kind == IR_OPERAND_INT) {
          if (producer->rhs.int_value == 1) {
            *step_out = 1;
            *increment_index = i;
            return 1;
          }
          if (producer->rhs.int_value == -1) {
            *step_out = -1;
            *increment_index = i;
            return 1;
          }
        }

        if (ir_operand_is_symbol(&producer->rhs) &&
            strcmp(producer->rhs.name, counter_symbol) == 0 &&
            producer->lhs.kind == IR_OPERAND_INT) {
          if (producer->lhs.int_value == 1) {
            *step_out = 1;
            *increment_index = i;
            return 1;
          }
          if (producer->lhs.int_value == -1) {
            *step_out = -1;
            *increment_index = i;
            return 1;
          }
        }
      }

      return 0;
    }

    return 0;
  }

  return 0;
}

typedef struct {
  IRSymbolValueMap prefix;
  size_t upto;
  IRNameIndex tail_writes;
  size_t last_barrier;
  int has_barrier;
  size_t backedge;
  int has_backedge;
} IRUnrollSymbols;

static int ir_unroll_symbols_init(IRUnrollSymbols *symbols,
                                  const IRFunction *function) {
  memset(symbols, 0, sizeof(*symbols));
  if (!ir_temp_value_map_init(&symbols->prefix)) {
    return 0;
  }
  if (!ir_name_index_init(&symbols->tail_writes,
                          function->instruction_count * 3)) {
    ir_temp_value_map_destroy(&symbols->prefix);
    return 0;
  }
  for (size_t k = function->instruction_count; k-- > 0;) {
    const IRInstruction *instruction = &function->instructions[k];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (instruction->op == IR_OP_CALL ||
        instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_STORE ||
        instruction->op == IR_OP_INLINE_ASM) {
      if (!symbols->has_barrier) {
        symbols->last_barrier = k;
        symbols->has_barrier = 1;
      }
      continue;
    }
    if (instruction->op == IR_OP_ROTATE_ADD) {
      if (instruction->lhs.name) {
        ir_name_index_insert(&symbols->tail_writes, instruction->lhs.name, k);
      }
      if (instruction->rhs.name) {
        ir_name_index_insert(&symbols->tail_writes, instruction->rhs.name, k);
      }
    }
    if (ir_instruction_writes_destination(instruction) &&
        instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name) {
      ir_name_index_insert(&symbols->tail_writes, instruction->dest.name, k);
    }
  }
  return 1;
}

static void ir_unroll_symbols_destroy(IRUnrollSymbols *symbols) {
  ir_temp_value_map_destroy(&symbols->prefix);
  ir_name_index_destroy(&symbols->tail_writes);
}

static int ir_unroll_symbols_at(IRUnrollSymbols *symbols,
                                const IRFunction *function,
                                size_t header_index) {
  for (; symbols->upto < header_index &&
         symbols->upto < function->instruction_count;
       symbols->upto++) {
    if (!ir_symbol_int_map_step(&function->instructions[symbols->upto],
                                &symbols->prefix)) {
      return 0;
    }
  }
  symbols->has_backedge = 0;
  const char *loop_label = function->instructions[header_index].text;
  for (size_t j = header_index + 1; j < function->instruction_count; j++) {
    const IRInstruction *jump = &function->instructions[j];
    if (jump->op == IR_OP_JUMP && jump->text &&
        strcmp(jump->text, loop_label) == 0) {
      symbols->backedge = j;
      symbols->has_backedge = 1;
      break;
    }
  }
  return 1;
}

static int ir_unroll_resolve_int(const IRUnrollSymbols *symbols,
                                 const IROperand *operand,
                                 long long *out_value) {
  if (!operand || !out_value) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_INT) {
    *out_value = operand->int_value;
    return 1;
  }
  if (operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
    return 0;
  }
  if (symbols->has_backedge) {
    size_t written = 0;
    if (symbols->has_barrier && symbols->last_barrier > symbols->backedge) {
      return 0;
    }
    if (ir_name_index_find(&symbols->tail_writes, operand->name, &written) &&
        written > symbols->backedge) {
      return 0;
    }
  }
  const IROperand *mapped =
      ir_temp_value_map_lookup(&symbols->prefix, operand->name);
  if (mapped && mapped->kind == IR_OPERAND_INT) {
    *out_value = mapped->int_value;
    return 1;
  }
  return 0;
}

static int ir_try_parse_counted_while_loop(const IRFunction *function,
                                           size_t header_index,
                                           const IRUnrollSymbols *symbols,
                                           const char **counter_symbol_out,
                                           long long *start_value_out,
                                           long long *limit_value_out,
                                           int *inclusive_out, int *step_out,
                                           size_t *branch_index_out,
                                           size_t *body_start_out,
                                           size_t *body_end_out,
                                           size_t *jump_index_out,
                                           size_t *increment_index_out) {
  if (!function || !symbols || !counter_symbol_out || !start_value_out ||
      !limit_value_out || !inclusive_out || !step_out || !branch_index_out ||
      !body_start_out || !body_end_out || !jump_index_out ||
      !increment_index_out) {
    return 0;
  }

  const IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !header->text) {
    return 0;
  }

  const char *loop_label = header->text;
  size_t branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &branch_index)) {
    return 0;
  }

  const IRInstruction *branch = &function->instructions[branch_index];
  if (branch->op != IR_OP_BRANCH_ZERO || !branch->text) {
    size_t probe_index = branch_index;
    if (!ir_find_next_non_nop(function, branch_index + 1, &probe_index)) {
      return 0;
    }
    branch = &function->instructions[probe_index];
    branch_index = probe_index;
    if (branch->op != IR_OP_BRANCH_ZERO || !branch->text) {
      return 0;
    }
  }

  const char *end_label = branch->text;
  size_t jump_index = (size_t)-1;
  for (size_t j = branch_index + 1; j < function->instruction_count; j++) {
    const IRInstruction *probe = &function->instructions[j];
    if (probe->op == IR_OP_JUMP && probe->text &&
        strcmp(probe->text, loop_label) == 0) {
      jump_index = j;
      break;
    }
    if (probe->op == IR_OP_LABEL) {
      if (probe->text && strcmp(probe->text, end_label) == 0) {
        break;
      }
      return 0;
    }
  }

  if (jump_index == (size_t)-1) {
    return 0;
  }

  size_t compare_index = 0;
  const IRInstruction *compare = NULL;
  if (ir_operand_is_temp(&branch->lhs)) {
    if (!ir_find_last_writer_before(function, branch_index, IR_OPERAND_TEMP,
                                    branch->lhs.name, &compare_index)) {
      return 0;
    }
    compare = &function->instructions[compare_index];
  } else if (branch->lhs.kind == IR_OPERAND_INT) {
    return 0;
  } else {
    return 0;
  }

  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text) {
    return 0;
  }

  const char *counter_symbol = NULL;
  const IROperand *limit_operand = NULL;
  long long limit_value = 0;
  int inclusive = 0;

  if (strcmp(compare->text, "<=") == 0) {
    inclusive = 1;
  } else if (strcmp(compare->text, "<") == 0) {
    inclusive = 0;
  } else if (strcmp(compare->text, ">=") == 0) {
    inclusive = 1;
    counter_symbol =
        compare->rhs.kind == IR_OPERAND_SYMBOL ? compare->rhs.name : NULL;
    limit_operand = &compare->lhs;
    if (!ir_unroll_resolve_int(symbols, &compare->lhs, &limit_value) ||
        !counter_symbol) {
      return 0;
    }
    goto parsed_compare;
  } else if (strcmp(compare->text, ">") == 0) {
    inclusive = 0;
    counter_symbol =
        compare->rhs.kind == IR_OPERAND_SYMBOL ? compare->rhs.name : NULL;
    limit_operand = &compare->lhs;
    if (!ir_unroll_resolve_int(symbols, &compare->lhs, &limit_value) ||
        !counter_symbol) {
      return 0;
    }
    goto parsed_compare;
  } else {
    return 0;
  }

  counter_symbol =
      compare->lhs.kind == IR_OPERAND_SYMBOL ? compare->lhs.name : NULL;
  limit_operand = &compare->rhs;
  if (!counter_symbol ||
      !ir_unroll_resolve_int(symbols, &compare->rhs, &limit_value)) {
    return 0;
  }

parsed_compare: {
  IROperand counter_operand = ir_operand_symbol(counter_symbol);
  if (!counter_operand.name ||
      !ir_unroll_resolve_int(symbols, &counter_operand,
                                     start_value_out)) {
    return 0;
  }
}

  size_t body_start = branch_index + 1;
  size_t body_end = jump_index;
  int step = 0;
  size_t increment_index = 0;
  if (!ir_try_parse_loop_increment(function, body_start, body_end, counter_symbol,
                                 &increment_index, &step)) {
    return 0;
  }

  for (size_t j = body_start; j < body_end; j++) {
    if (!ir_loop_body_opcode_is_unroll_safe(function->instructions[j].op)) {
      return 0;
    }
    if (j != increment_index &&
        ir_instruction_writes_symbol(&function->instructions[j]) &&
        function->instructions[j].dest.name &&
        strcmp(function->instructions[j].dest.name, counter_symbol) == 0) {
      return 0;
    }
    if (limit_operand && limit_operand->kind == IR_OPERAND_SYMBOL &&
        limit_operand->name &&
        ir_instruction_writes_symbol(&function->instructions[j]) &&
        function->instructions[j].dest.name &&
        strcmp(function->instructions[j].dest.name, limit_operand->name) == 0) {
      return 0;
    }
  }

  *counter_symbol_out = counter_symbol;
  *limit_value_out = limit_value;
  *inclusive_out = inclusive;
  *step_out = step;
  *branch_index_out = branch_index;
  *body_start_out = body_start;
  *body_end_out = body_end;
  *jump_index_out = jump_index;
  *increment_index_out = increment_index;
  return 1;
}

static int ir_symbol_used_in_range(const IRFunction *function, size_t start,
                                   size_t end, const char *symbol_name,
                                   size_t skip_index) {
  if (!function || !symbol_name) {
    return 0;
  }

  for (size_t i = start; i < end; i++) {
    if (i == skip_index) {
      continue;
    }

    const IRInstruction *instruction = &function->instructions[i];
    const IROperand *operands[3] = {&instruction->dest, &instruction->lhs,
                                    &instruction->rhs};
    size_t operand_count = 3 + instruction->argument_count;

    for (size_t o = 0; o < operand_count; o++) {
      const IROperand *operand =
          o < 3 ? operands[o] : &instruction->arguments[o - 3];
      if (operand->kind == IR_OPERAND_SYMBOL && operand->name &&
          strcmp(operand->name, symbol_name) == 0) {
        return 1;
      }
    }
  }

  return 0;
}

int ir_function_replace_instructions(IRFunction *function,
                                            IRInstructionVector *vector) {
  if (!function || !vector) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);

  function->instructions = vector->items;
  function->instruction_count = vector->count;
  function->instruction_capacity = vector->capacity;
  vector->items = NULL;
  vector->count = 0;
  vector->capacity = 0;
  return 1;
}

typedef struct {
  const char **names;
  size_t count;
  size_t capacity;
} IrUnrollTempSet;

static void ir_unroll_temp_set_destroy(IrUnrollTempSet *set) {
  free(set->names);
  set->names = NULL;
  set->count = set->capacity = 0;
}

static int ir_unroll_temp_set_contains(const IrUnrollTempSet *set,
                                       const char *name) {
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_unroll_temp_set_add(IrUnrollTempSet *set, const char *name) {
  if (ir_unroll_temp_set_contains(set, name)) {
    return 1;
  }
  if (set->count >= set->capacity) {
    size_t grown = set->capacity ? set->capacity * 2 : 16;
    const char **names =
        (const char **)realloc(set->names, grown * sizeof(*names));
    if (!names) {
      return 0;
    }
    set->names = names;
    set->capacity = grown;
  }
  set->names[set->count++] = name;
  return 1;
}

static size_t ir_unroll_operand_slots(IRInstruction *in, IROperand **slots,
                                      size_t max_slots) {
  size_t n = 0;
  if (n < max_slots) slots[n++] = &in->dest;
  if (n < max_slots) slots[n++] = &in->lhs;
  if (n < max_slots) slots[n++] = &in->rhs;
  for (size_t a = 0; a < in->argument_count && n < max_slots; a++) {
    slots[n++] = &in->arguments[a];
  }
  return n;
}

static int ir_unroll_temp_used_outside(const IRFunction *function, size_t lo,
                                       size_t hi, const char *name) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (i >= lo && i < hi) {
      continue;
    }
    IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue;
    }
    IROperand *slots[3 + 16];
    size_t n = ir_unroll_operand_slots(in, slots, sizeof(slots) / sizeof(*slots));
    for (size_t s = 0; s < n; s++) {
      if (slots[s]->kind == IR_OPERAND_TEMP && slots[s]->name &&
          strcmp(slots[s]->name, name) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static int ir_unroll_collect_private_temps(const IRFunction *function, size_t lo,
                                           size_t hi, IrUnrollTempSet *set) {
  for (size_t i = lo; i < hi; i++) {
    IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP || in->op == IR_OP_STORE) {
      continue;
    }
    if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name) {
      continue;
    }
    if (ir_unroll_temp_set_contains(set, in->dest.name)) {
      continue;
    }
    if (ir_unroll_temp_used_outside(function, lo, hi, in->dest.name)) {
      continue;
    }
    if (!ir_unroll_temp_set_add(set, in->dest.name)) {
      return 0;
    }
  }
  return 1;
}

static int ir_unroll_rename_copy_temps(IRInstruction *out,
                                       const IrUnrollTempSet *set,
                                       long long trip) {
  IROperand *slots[3 + 16];
  size_t n = ir_unroll_operand_slots(out, slots, sizeof(slots) / sizeof(*slots));
  for (size_t s = 0; s < n; s++) {
    IROperand *o = slots[s];
    if (o->kind != IR_OPERAND_TEMP || !o->name ||
        !ir_unroll_temp_set_contains(set, o->name)) {
      continue;
    }
    size_t len = strlen(o->name) + 24;
    char *renamed = (char *)malloc(len);
    if (!renamed) {
      return 0;
    }
    snprintf(renamed, len, "%s__u%lld", o->name, trip);
    mettle_free_string(o->name);
    o->name = renamed;
  }
  return 1;
}

static int ir_unroll_header_reaches_branch(const IRFunction *function,
                                           size_t header_index) {
  size_t branch_index = 0;
  const IRInstruction *branch = NULL;

  if (!ir_find_next_non_nop(function, header_index + 1, &branch_index)) {
    return 0;
  }
  branch = &function->instructions[branch_index];
  if (branch->op == IR_OP_BRANCH_ZERO && branch->text) {
    return 1;
  }
  if (!ir_find_next_non_nop(function, branch_index + 1, &branch_index)) {
    return 0;
  }
  branch = &function->instructions[branch_index];
  return branch->op == IR_OP_BRANCH_ZERO && branch->text != NULL;
}

static int ir_try_unroll_loop_at(IRFunction *function, size_t header_index,
                                 IRUnrollSymbols *symbols, int *changed) {
  if (!ir_unroll_header_reaches_branch(function, header_index)) {
    return 1;
  }
  if (!ir_unroll_symbols_at(symbols, function, header_index)) {
    return 0;
  }

  const char *counter_symbol = NULL;
  long long start_value = 0;
  long long limit_value = 0;
  int inclusive = 0;
  int step = 0;
  size_t branch_index = 0;
  size_t body_start = 0;
  size_t body_end = 0;
  size_t jump_index = 0;
  size_t increment_index = 0;

  if (!ir_try_parse_counted_while_loop(function, header_index, symbols,
                                       &counter_symbol, &start_value,
                                       &limit_value, &inclusive, &step,
                                       &branch_index, &body_start, &body_end,
                                       &jump_index, &increment_index)) {
    return 1;
  }

  long long trips = 0;
  if (step > 0) {
    trips = inclusive ? (limit_value - start_value + 1)
                      : (limit_value - start_value);
  } else if (step < 0) {
    trips = inclusive ? (start_value - limit_value + 1)
                      : (start_value - limit_value);
  }

  ir_explain_loop_trips(function->name,
                        function->instructions[header_index].location, trips);
  long long max_trips =
      ir_opt_unroll_max_trip_count(function,
                                   function->instructions[header_index].location);
  if (trips <= 0 || trips > max_trips) {
    return 1;
  }

  int counter_used_in_body =
      ir_symbol_used_in_range(function, body_start, body_end, counter_symbol,
                              increment_index);

  IRInstructionVector vector = {0};
  for (size_t i = 0; i < header_index; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  IrUnrollTempSet private_temps = {0};
  if (!ir_unroll_collect_private_temps(function, body_start, body_end,
                                       &private_temps)) {
    ir_unroll_temp_set_destroy(&private_temps);
  }

  for (long long trip = 0; trip < trips; trip++) {
    for (size_t b = body_start; b < body_end; b++) {
      if (!counter_used_in_body && b == increment_index) {
        continue;
      }

      IRInstruction cloned = {0};
      if (!ir_clone_instruction_plain(&function->instructions[b], &cloned) ||
          !ir_unroll_rename_copy_temps(&cloned, &private_temps, trip) ||
          !ir_instruction_vector_append_move(&vector, &cloned)) {
        ir_instruction_destroy_storage(&cloned);
        ir_instruction_vector_destroy(&vector);
        ir_unroll_temp_set_destroy(&private_temps);
        return 0;
      }
    }
  }
  ir_unroll_temp_set_destroy(&private_temps);

  if (!counter_used_in_body) {
    IRInstruction final_value = {0};
    final_value.op = IR_OP_ASSIGN;
    final_value.dest = ir_operand_symbol(counter_symbol);
    final_value.lhs = ir_operand_int(start_value + trips * step);
    if (!final_value.dest.name ||
        !ir_instruction_vector_append_move(&vector, &final_value)) {
      ir_instruction_destroy_storage(&final_value);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  {
    const IRInstruction *branch = &function->instructions[branch_index];
    if (branch->text) {
      IRInstruction exit_jump = {0};
      exit_jump.op = IR_OP_JUMP;
      exit_jump.text = mettle_strdup(branch->text);
      if (!exit_jump.text ||
          !ir_instruction_vector_append_move(&vector, &exit_jump)) {
        ir_instruction_destroy_storage(&exit_jump);
        ir_instruction_vector_destroy(&vector);
        return 0;
      }
    }
  }

  for (size_t i = jump_index + 1; i < function->instruction_count; i++) {
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  SourceLocation header_location = function->instructions[header_index].location;

  if (!ir_function_replace_instructions(function, &vector)) {
    ir_instruction_vector_destroy(&vector);
    return 0;
  }

  if (ir_explain_enabled()) {
    char headline[96];
    snprintf(headline, sizeof(headline),
             "fully unrolled (%lld iteration%s, constant trip count)", trips,
             trips == 1 ? "" : "s");
    ir_explain_remark(function->name, "loop", header_location, 1, headline,
                      NULL, NULL, NULL);
    ir_explain_remark_code("unrolled");
    ir_explain_remark_quantity("iterations", (long)trips);
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

#define IR_VEC_UNROLL 4

int ir_resolve_indexed_address_temp(const IRFunction *function,
                                            size_t before_index, const char *iv,
                                            const char *bound,
                                            const char *addr_temp,
                                            const char **base_out,
                                            int *elem_size_out, int *step_out);
const char *ir_function_local_declared_type(const IRFunction *function,
                                                   const char *symbol_name);
int ir_symbol_is_sum_array_base(const IRFunction *function,
                                       const char *symbol_name);

static int ir_vec_symbol_invariant_in_body(const IRFunction *fn, size_t lo,
                                            size_t hi, const char *sym) {
  if (!sym) {
    return 0;
  }
  for (size_t k = lo; k < hi; k++) {
    const IRInstruction *m = &fn->instructions[k];
    if (ir_operand_is_symbol(&m->dest) &&
        strcmp(m->dest.name, sym) == 0) {
      return 0;
    }
  }
  return 1;
}

static int ir_vec_binary_is(const IRInstruction *in, const char *op) {
  return in && in->op == IR_OP_BINARY && !in->is_float && !in->ast_ref &&
         in->text && strcmp(in->text, op) == 0;
}

static int ir_vec_assign_sym_from_temp(const IRInstruction *in,
                                       const char *sym) {
  return in && in->op == IR_OP_ASSIGN && !in->ast_ref &&
         ir_operand_is_symbol_named(&in->dest, sym) &&
         ir_operand_is_temp(&in->lhs);
}

static int ir_vec_expr_is_pure(const IRFunction *fn, size_t lo, size_t hi,
                               const char *root, const char *iv,
                               const char *acc, char **seen, size_t *seen_n,
                               size_t seen_cap, int depth) {
  if (depth > 64 || !root) {
    return 0;
  }
  for (size_t s = 0; s < *seen_n; s++) {
    if (strcmp(seen[s], root) == 0) {
      return 1;
    }
  }
  const IRInstruction *prod = NULL;
  for (size_t k = lo; k < hi; k++) {
    const IRInstruction *m = &fn->instructions[k];
    if (ir_operand_is_temp(&m->dest) &&
        strcmp(m->dest.name, root) == 0) {
      if (prod) {
        return 0;
      }
      prod = m;
    }
  }
  if (!prod) {
    return 0;
  }
  if (prod->op == IR_OP_LOAD && prod->lhs.kind == IR_OPERAND_TEMP &&
      prod->lhs.name) {
    const char *base = NULL;
    size_t load_idx = hi;
    for (size_t k = lo; k < hi; k++) {
      if (&fn->instructions[k] == prod) {
        load_idx = k;
        break;
      }
    }
    if (load_idx < hi &&
        ir_resolve_indexed_address_temp(fn, load_idx, iv, NULL,
                                        prod->lhs.name, &base, NULL, NULL) &&
        base && strcmp(base, iv) != 0 && strcmp(base, acc) != 0 &&
        ir_vec_symbol_invariant_in_body(fn, lo, hi, base)) {
      if (*seen_n >= seen_cap) {
        return 0;
      }
      seen[(*seen_n)++] = prod->dest.name;
      return 1;
    }
    return 0;
  }
  int is_binary = ir_vec_binary_is(prod, "+") || ir_vec_binary_is(prod, "-") ||
                  ir_vec_binary_is(prod, "*");
  int is_cast = prod->op == IR_OP_CAST && !prod->is_float;
  if (!is_binary && !is_cast) {
    return 0;
  }
  if (*seen_n >= seen_cap) {
    return 0;
  }
  seen[(*seen_n)++] = prod->dest.name;

  const IROperand *ops[2] = {&prod->lhs, &prod->rhs};
  int nops = is_cast ? 1 : 2;
  for (int oi = 0; oi < nops; oi++) {
    const IROperand *o = ops[oi];
    if (o->kind == IR_OPERAND_INT) {
      continue;
    }
    if (o->kind == IR_OPERAND_SYMBOL && o->name) {
      if (strcmp(o->name, iv) == 0) {
        continue;
      }
      return 0;
    }
    if (o->kind == IR_OPERAND_TEMP && o->name) {
      if (!ir_vec_expr_is_pure(fn, lo, hi, o->name, iv, acc, seen, seen_n,
                               seen_cap, depth + 1)) {
        return 0;
      }
      continue;
    }
    return 0;
  }
  return 1;
}

static int ir_vec_clone_body_inst(const IRInstruction *src, IRInstruction *out,
                                  int lane, const char *acc,
                                  const char *acc_lane) {
  if (!ir_clone_instruction_plain(src, out)) {
    return 0;
  }
  IROperand *slots[3 + 8];
  int n = 0;
  slots[n++] = &out->dest;
  slots[n++] = &out->lhs;
  slots[n++] = &out->rhs;
  for (size_t a = 0; a < out->argument_count && n < 3 + 8; a++) {
    slots[n++] = &out->arguments[a];
  }
  for (int s = 0; s < n; s++) {
    IROperand *o = slots[s];
    if (o->kind == IR_OPERAND_TEMP && o->name) {
      size_t len = strlen(o->name) + 16;
      char *nn = malloc(len);
      if (!nn) {
        return 0;
      }
      snprintf(nn, len, "%s__l%d", o->name, lane);
      mettle_free_string(o->name);
      o->name = nn;
    } else if (o->kind == IR_OPERAND_SYMBOL && o->name && acc &&
               strcmp(o->name, acc) == 0) {
      char *nn = mettle_strdup(acc_lane);
      if (!nn) {
        return 0;
      }
      mettle_free_string(o->name);
      o->name = nn;
    }
  }
  return 1;
}

static int ir_vec_try_unroll_reduction_at(IRFunction *function, size_t h,
                                           size_t J, int *changed) {
  IRInstruction *head = &function->instructions[h];
  if (head->op != IR_OP_LABEL || !head->text) {
    return 1;
  }
  const char *head_label = head->text;

  if (J == (size_t)-1 || J < h + 5) {
    return 1;
  }

  IRInstruction *g = &function->instructions[h + 1];
  IRInstruction *gb = &function->instructions[h + 2];
  int op_lt = ir_vec_binary_is(g, "<");
  int op_le = ir_vec_binary_is(g, "<=");
  if ((!op_lt && !op_le) || g->dest.kind != IR_OPERAND_TEMP || !g->dest.name ||
      g->lhs.kind != IR_OPERAND_SYMBOL || !g->lhs.name ||
      (g->rhs.kind != IR_OPERAND_SYMBOL && g->rhs.kind != IR_OPERAND_INT) ||
      gb->op != IR_OP_BRANCH_ZERO || gb->lhs.kind != IR_OPERAND_TEMP ||
      !gb->lhs.name || !gb->text ||
      !ir_operand_names_match(&gb->lhs, &g->dest)) {
    return 1;
  }
  const char *iv = g->lhs.name;
  const char *exit_label = gb->text;

  IRInstruction *inc = &function->instructions[J - 2];
  IRInstruction *incs = &function->instructions[J - 1];
  if (!ir_vec_binary_is(inc, "+") ||
      !ir_operand_is_symbol_named(&inc->lhs, iv) ||
      inc->rhs.kind != IR_OPERAND_INT || inc->rhs.int_value != 1 ||
      inc->dest.kind != IR_OPERAND_TEMP || !inc->dest.name ||
      !ir_vec_assign_sym_from_temp(incs, iv) ||
      !ir_operand_names_match(&incs->lhs, &inc->dest)) {
    return 1;
  }

  size_t body_lo = h + 3, body_hi = J - 2;
  if (body_hi <= body_lo) {
    return 1;
  }
  const char *acc = NULL;
  const char *expr_root = NULL;
  size_t acc_add_idx = (size_t)-1;
  for (size_t k = body_lo; k + 1 < body_hi + 1 && k + 1 < J - 1; k++) {
    IRInstruction *m = &function->instructions[k];
    IRInstruction *st = &function->instructions[k + 1];
    if (ir_vec_binary_is(m, "+") && m->dest.kind == IR_OPERAND_TEMP &&
        m->dest.name && ir_operand_is_symbol(&m->lhs) &&
        strcmp(m->lhs.name, iv) != 0 && m->rhs.kind == IR_OPERAND_TEMP &&
        m->rhs.name && ir_vec_assign_sym_from_temp(st, m->lhs.name) &&
        ir_operand_names_match(&st->lhs, &m->dest)) {
      if (acc) {
        return 1;
      }
      acc = m->lhs.name;
      expr_root = m->rhs.name;
      acc_add_idx = k;
    }
  }
  if (!acc || !expr_root || strcmp(acc, iv) == 0) {
    return 1;
  }

  int acc_zero_init = 0;
  for (size_t bi = h; bi-- > 0;) {
    IRInstruction *m = &function->instructions[bi];
    if (m->op == IR_OP_ASSIGN && ir_operand_is_symbol_named(&m->dest, acc)) {
      acc_zero_init =
          (m->lhs.kind == IR_OPERAND_INT && m->lhs.int_value == 0);
      break;
    }
    if (m->op == IR_OP_LABEL) {
      break;
    }
  }
  if (!acc_zero_init) {
    return 1;
  }

  char *seen[128];
  size_t seen_n = 0;
  if (!ir_vec_expr_is_pure(function, body_lo, body_hi, expr_root, iv, acc,
                           seen, &seen_n, 128, 0)) {
    return 1;
  }
  int body_has_load = 0;
  int all_loads_sum_array = 1;
  for (size_t k = body_lo; k < body_hi; k++) {
    IRInstruction *m = &function->instructions[k];
    if (k == acc_add_idx || k == acc_add_idx + 1) {
      continue;
    }
    switch (m->op) {
    case IR_OP_NOP:
    case IR_OP_BINARY:
    case IR_OP_CAST:
      if (m->dest.kind == IR_OPERAND_SYMBOL) {
        return 1;
      }
      break;
    case IR_OP_LOAD: {
      const char *base = NULL;
      if (m->dest.kind != IR_OPERAND_TEMP || m->lhs.kind != IR_OPERAND_TEMP ||
          !m->lhs.name ||
          !ir_resolve_indexed_address_temp(function, k, iv, NULL, m->lhs.name,
                                           &base, NULL, NULL) ||
          !base || strcmp(base, iv) == 0 || strcmp(base, acc) == 0 ||
          !ir_vec_symbol_invariant_in_body(function, body_lo, body_hi, base)) {
        return 1;
      }
      body_has_load = 1;
      if (!ir_symbol_is_sum_array_base(function, base)) {
        all_loads_sum_array = 0;
      }
      break;
    }
    default:
      return 1;
    }
  }

  if (body_has_load && all_loads_sum_array) {
    const char *at = ir_function_local_declared_type(function, acc);
    if (at && strcmp(at, "int64") == 0) {
      return 1;
    }
  }

  IRInstruction *out = NULL;
  size_t out_n = 0, out_cap = 0;
#define VEC_EMIT(INIT)                                                          \
  do {                                                                         \
    if (out_n >= out_cap) {                                                     \
      size_t nc = out_cap ? out_cap * 2 : 64;                                   \
      IRInstruction *np = realloc(out, nc * sizeof(IRInstruction));             \
      if (!np) {                                                               \
        for (size_t fi = 0; fi < out_n; fi++)                                   \
          ir_instruction_destroy_storage(&out[fi]);                            \
        free(out);                                                             \
        return 0;                                                              \
      }                                                                        \
      out = np;                                                                 \
      out_cap = nc;                                                             \
    }                                                                          \
    memset(&out[out_n], 0, sizeof(IRInstruction));                              \
    INIT;                                                                       \
    out_n++;                                                                    \
  } while (0)

  char pre[32];
  snprintf(pre, sizeof(pre), "vu%zu", h);
  char buf[96];

  char *acc_name[IR_VEC_UNROLL];
  acc_name[0] = (char *)acc;
  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    snprintf(buf, sizeof(buf), "%s__a%d", acc, L);
    acc_name[L] = mettle_strdup(buf);
    if (!acc_name[L]) {
      for (int q = 1; q < L; q++) free(acc_name[q]);
      return 0;
    }
  }

#define MKLBL(name, kind)                                                      \
  snprintf(buf, sizeof(buf), "%s_%s_%s", pre, kind, name)

  const char *acc_type = "int64";
  for (size_t di = 0; di < function->instruction_count; di++) {
    IRInstruction *m = &function->instructions[di];
    if (m->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol_named(&m->dest, acc) && m->text) {
      acc_type = m->text;
      break;
    }
  }

  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    VEC_EMIT({
      out[out_n].op = IR_OP_DECLARE_LOCAL;
      out[out_n].dest = ir_operand_symbol(acc_name[L]);
      out[out_n].text = mettle_strdup(acc_type);
      if (!out[out_n].text) { goto oom; }
    });
    VEC_EMIT({
      out[out_n].op = IR_OP_ASSIGN;
      out[out_n].dest = ir_operand_symbol(acc_name[L]);
      out[out_n].lhs = ir_operand_int(0);
    });
  }

  char hm[64], htail[64], hcomb[64];
  snprintf(hm, sizeof(hm), "%s_main", pre);
  snprintf(htail, sizeof(htail), "%s_tail", pre);
  snprintf(hcomb, sizeof(hcomb), "%s_comb", pre);

  VEC_EMIT({ out[out_n].op = IR_OP_LABEL; out[out_n].text = mettle_strdup(hm); if(!out[out_n].text){goto oom;} });

  char t_ub[64], t_gu[64];
  snprintf(t_ub, sizeof(t_ub), "%s_ub", pre);
  snprintf(t_gu, sizeof(t_gu), "%s_gu", pre);
  VEC_EMIT({
    out[out_n].op = IR_OP_BINARY; out[out_n].text = mettle_strdup("+");
    if(!out[out_n].text){goto oom;}
    out[out_n].dest = ir_operand_temp(t_ub);
    out[out_n].lhs = ir_operand_symbol(iv);
    out[out_n].rhs = ir_operand_int(IR_VEC_UNROLL - 1);
  });
  VEC_EMIT({
    out[out_n].op = IR_OP_BINARY;
    out[out_n].text = mettle_strdup(op_le ? "<=" : "<");
    if(!out[out_n].text){goto oom;}
    out[out_n].dest = ir_operand_temp(t_gu);
    out[out_n].lhs = ir_operand_temp(t_ub);
    if (g->rhs.kind == IR_OPERAND_INT)
      out[out_n].rhs = ir_operand_int(g->rhs.int_value);
    else
      out[out_n].rhs = ir_operand_symbol(g->rhs.name);
  });
  VEC_EMIT({
    out[out_n].op = IR_OP_BRANCH_ZERO;
    out[out_n].lhs = ir_operand_temp(t_gu);
    out[out_n].text = mettle_strdup(htail);
    if(!out[out_n].text){goto oom;}
  });

  for (int L = 0; L < IR_VEC_UNROLL; L++) {
    char tiL[64];
    if (L > 0) {
      snprintf(tiL, sizeof(tiL), "%s_ti%d", pre, L);
      VEC_EMIT({
        out[out_n].op = IR_OP_BINARY; out[out_n].text = mettle_strdup("+");
        if(!out[out_n].text){goto oom;}
        out[out_n].dest = ir_operand_temp(tiL);
        out[out_n].lhs = ir_operand_symbol(iv);
        out[out_n].rhs = ir_operand_int(L);
      });
    }
    for (size_t k = body_lo; k < body_hi; k++) {
      IRInstruction tmp;
      if (!ir_vec_clone_body_inst(&function->instructions[k], &tmp, L, acc,
                                  acc_name[L])) {
        goto oom;
      }
      if (L > 0) {
        IROperand *sl[3 + 8]; int sn = 0;
        sl[sn++] = &tmp.dest; sl[sn++] = &tmp.lhs; sl[sn++] = &tmp.rhs;
        for (size_t a = 0; a < tmp.argument_count && sn < 3 + 8; a++)
          sl[sn++] = &tmp.arguments[a];
        for (int s = 0; s < sn; s++) {
          if (sl[s]->kind == IR_OPERAND_SYMBOL && sl[s]->name &&
              strcmp(sl[s]->name, iv) == 0) {
            char *nn = mettle_strdup(tiL);
            if (!nn) { ir_instruction_destroy_storage(&tmp); goto oom; }
            mettle_free_string(sl[s]->name); sl[s]->name = nn;
            sl[s]->kind = IR_OPERAND_TEMP;
          }
        }
      }
      VEC_EMIT({ out[out_n] = tmp; });
    }
  }

  {
    char t_st[64];
    snprintf(t_st, sizeof(t_st), "%s_st", pre);
    VEC_EMIT({
      out[out_n].op = IR_OP_BINARY; out[out_n].text = mettle_strdup("+");
      if(!out[out_n].text){goto oom;}
      out[out_n].dest = ir_operand_temp(t_st);
      out[out_n].lhs = ir_operand_symbol(iv);
      out[out_n].rhs = ir_operand_int(IR_VEC_UNROLL);
    });
    VEC_EMIT({
      out[out_n].op = IR_OP_ASSIGN;
      out[out_n].dest = ir_operand_symbol(iv);
      out[out_n].lhs = ir_operand_temp(t_st);
    });
    VEC_EMIT({ out[out_n].op = IR_OP_JUMP; out[out_n].text = mettle_strdup(hm); if(!out[out_n].text){goto oom;} });
  }

  VEC_EMIT({ out[out_n].op = IR_OP_LABEL; out[out_n].text = mettle_strdup(htail); if(!out[out_n].text){goto oom;} });
  {
    char sh[64];
    snprintf(sh, sizeof(sh), "%s_sh", pre);
    for (size_t k = h; k <= J; k++) {
      IRInstruction src = function->instructions[k];
      IRInstruction tmp;
      if (!ir_clone_instruction_plain(&src, &tmp)) {
        goto oom;
      }
      if (tmp.op == IR_OP_LABEL && tmp.text &&
          strcmp(tmp.text, head_label) == 0) {
        mettle_free_string(tmp.text); tmp.text = mettle_strdup(sh);
        if(!tmp.text){ir_instruction_destroy_storage(&tmp);goto oom;}
      }
      if (tmp.op == IR_OP_JUMP && tmp.text &&
          strcmp(tmp.text, head_label) == 0) {
        mettle_free_string(tmp.text); tmp.text = mettle_strdup(sh);
        if(!tmp.text){ir_instruction_destroy_storage(&tmp);goto oom;}
      }
      if (tmp.op == IR_OP_BRANCH_ZERO && tmp.text &&
          strcmp(tmp.text, exit_label) == 0) {
        mettle_free_string(tmp.text); tmp.text = mettle_strdup(hcomb);
        if(!tmp.text){ir_instruction_destroy_storage(&tmp);goto oom;}
      }
      VEC_EMIT({ out[out_n] = tmp; });
    }
  }

  VEC_EMIT({ out[out_n].op = IR_OP_LABEL; out[out_n].text = mettle_strdup(hcomb); if(!out[out_n].text){goto oom;} });
  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    char t_c[64];
    snprintf(t_c, sizeof(t_c), "%s_c%d", pre, L);
    VEC_EMIT({
      out[out_n].op = IR_OP_BINARY; out[out_n].text = mettle_strdup("+");
      if(!out[out_n].text){goto oom;}
      out[out_n].dest = ir_operand_temp(t_c);
      out[out_n].lhs = ir_operand_symbol(acc);
      out[out_n].rhs = ir_operand_symbol(acc_name[L]);
    });
    VEC_EMIT({
      out[out_n].op = IR_OP_ASSIGN;
      out[out_n].dest = ir_operand_symbol(acc);
      out[out_n].lhs = ir_operand_temp(t_c);
    });
  }

  {
    size_t old_span = J - h + 1;
    size_t tail_n = function->instruction_count - (J + 1);
    size_t new_count = h + out_n + tail_n;
    IRInstruction *ni = calloc(new_count ? new_count : 1, sizeof(IRInstruction));
    if (!ni) {
      goto oom;
    }
    for (size_t k = 0; k < h; k++) {
      ni[k] = function->instructions[k];
    }
    for (size_t k = h; k <= J; k++) {
      ir_instruction_destroy_storage(&function->instructions[k]);
    }
    for (size_t k = 0; k < out_n; k++) {
      ni[h + k] = out[k];
    }
    for (size_t k = 0; k < tail_n; k++) {
      ni[h + out_n + k] = function->instructions[J + 1 + k];
    }
    free(out);
    free(function->instructions);
    function->instructions = ni;
    function->instruction_count = new_count;
    function->instruction_capacity = new_count;
    (void)old_span;
  }

  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    free(acc_name[L]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;

oom:
  for (size_t fi = 0; fi < out_n; fi++) {
    ir_instruction_destroy_storage(&out[fi]);
  }
  free(out);
  for (int L = 1; L < IR_VEC_UNROLL; L++) {
    free(acc_name[L]);
  }
  return 0;
#undef VEC_EMIT
#undef MKLBL
}

static int ir_unroll_mark_loop_headers(const IRFunction *function,
                                      IRNameIndex *headers);

int ir_reduction_unroll_pass(IRFunction *function, int *changed) {
  IRNameIndex back_edges;

  if (!function) {
    return 1;
  }
  if (!ir_unroll_mark_loop_headers(function, &back_edges)) {
    return 0;
  }
  for (size_t h = 0; h + 5 < function->instruction_count; h++) {
    size_t back_edge = (size_t)-1;
    size_t before = function->instruction_count;
    if (function->instructions[h].op != IR_OP_LABEL ||
        !function->instructions[h].text ||
        !ir_name_index_find(&back_edges, function->instructions[h].text,
                            &back_edge)) {
      continue;
    }
    if (!ir_vec_try_unroll_reduction_at(function, h, back_edge, changed)) {
      ir_name_index_destroy(&back_edges);
      return 0;
    }
    if (function->instruction_count != before) {
      ir_name_index_destroy(&back_edges);
      if (!ir_unroll_mark_loop_headers(function, &back_edges)) {
        return 0;
      }
      h = (size_t)-1;
    }
  }
  ir_name_index_destroy(&back_edges);
  return 1;
}

static int ir_unroll_mark_loop_headers(const IRFunction *function,
                                       IRNameIndex *headers) {
  IRNameIndex labels;

  if (!ir_name_index_init(&labels, function->instruction_count)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text) {
      ir_name_index_insert(&labels, instruction->text, i);
    }
  }
  if (!ir_name_index_init(headers, function->instruction_count)) {
    ir_name_index_destroy(&labels);
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    size_t target = 0;
    if (instruction->op != IR_OP_JUMP || !instruction->text) {
      continue;
    }
    if (ir_name_index_find(&labels, instruction->text, &target) &&
        target < i) {
      ir_name_index_insert(headers, instruction->text, i);
    }
  }
  ir_name_index_destroy(&labels);
  return 1;
}

int ir_unroll_small_const_bound_loops_pass(IRFunction *function,
                                                  int *changed) {
  IRNameIndex loop_headers;
  IRUnrollSymbols symbols;

  if (!function) {
    return 0;
  }
  if (!ir_unroll_mark_loop_headers(function, &loop_headers)) {
    return 0;
  }
  if (!ir_unroll_symbols_init(&symbols, function)) {
    ir_name_index_destroy(&loop_headers);
    return 0;
  }

  int local_changed = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL ||
        !function->instructions[i].text ||
        !ir_name_index_find(&loop_headers, function->instructions[i].text,
                            NULL)) {
      continue;
    }

    int unrolled = 0;
    if (!ir_try_unroll_loop_at(function, i, &symbols, &unrolled)) {
      ir_unroll_symbols_destroy(&symbols);
      ir_name_index_destroy(&loop_headers);
      return 0;
    }
    if (unrolled) {
      local_changed = 1;
      break;
    }
  }

  ir_unroll_symbols_destroy(&symbols);
  ir_name_index_destroy(&loop_headers);
  if (local_changed && changed) {
    *changed = 1;
  }
  return 1;
}

int ir_symbol_address_taken(const IRFunction *function,
                                   const char *symbol_name) {
  if (!function || !symbol_name) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_ADDRESS_OF &&
        instruction->lhs.kind == IR_OPERAND_SYMBOL &&
        instruction->lhs.name &&
        strcmp(instruction->lhs.name, symbol_name) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_match_null_trap_diamond(const IRFunction *function,
                                      size_t start_index, size_t *end_index_out,
                                      const char **symbol_name_out) {
  if (!function || start_index >= function->instruction_count) {
    return 0;
  }

  const IRInstruction *branch = &function->instructions[start_index];
  if (branch->op != IR_OP_BRANCH_ZERO || !branch->text ||
      branch->lhs.kind != IR_OPERAND_SYMBOL || !branch->lhs.name ||
      strncmp(branch->text, "ir_trap_null_", 13) != 0) {
    return 0;
  }
  const char *trap_label = branch->text;
  const char *symbol_name = branch->lhs.name;

  size_t idx = 0;
  if (!ir_find_next_non_nop(function, start_index + 1, &idx)) {
    return 0;
  }
  const IRInstruction *jmp = &function->instructions[idx];
  if (jmp->op != IR_OP_JUMP || !jmp->text ||
      strncmp(jmp->text, "ir_nonnull_", 11) != 0) {
    return 0;
  }
  const char *ok_label = jmp->text;

  if (!ir_find_next_non_nop(function, idx + 1, &idx)) {
    return 0;
  }
  const IRInstruction *trap_lbl = &function->instructions[idx];
  if (trap_lbl->op != IR_OP_LABEL || !trap_lbl->text ||
      strcmp(trap_lbl->text, trap_label) != 0) {
    return 0;
  }

  if (!ir_find_next_non_nop(function, idx + 1, &idx)) {
    return 0;
  }
  const IRInstruction *call = &function->instructions[idx];
  if (call->op != IR_OP_CALL || !call->text ||
      (strcmp(call->text, "mettle_crash_trap") != 0 &&
       strcmp(call->text, "mettle_crash_trap_ex") != 0)) {
    return 0;
  }

  if (!ir_find_next_non_nop(function, idx + 1, &idx)) {
    return 0;
  }
  const IRInstruction *ok_lbl = &function->instructions[idx];
  if (ok_lbl->op != IR_OP_LABEL || !ok_lbl->text ||
      strcmp(ok_lbl->text, ok_label) != 0) {
    return 0;
  }

  *end_index_out = idx;
  *symbol_name_out = symbol_name;
  return 1;
}

static int ir_unroll_annotated_body_op_safe(const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_NOP:
    return !(in->text &&
             strncmp(in->text, IR_UNROLL_MARKER_PREFIX,
                     strlen(IR_UNROLL_MARKER_PREFIX)) == 0);
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ASSIGN:
  case IR_OP_CAST:
  case IR_OP_ROTATE_ADD:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_LOAD:
  case IR_OP_STORE:
  case IR_OP_ADDRESS_OF:
  case IR_OP_SELECT:
  case IR_OP_CALL:
    return 1;
  default:
    return 0;
  }
}

static int ir_unroll_symbol_is_private(const IRFunction *function,
                                       const char *name) {
  int declared = 0;
  for (size_t i = 0; i < function->parameter_count && !declared; i++) {
    declared = function->parameter_names && function->parameter_names[i] &&
               strcmp(function->parameter_names[i], name) == 0;
  }
  for (size_t i = 0; i < function->instruction_count && !declared; i++) {
    const IRInstruction *in = &function->instructions[i];
    declared = in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
               strcmp(in->dest.name, name) == 0;
  }
  return declared && !ir_symbol_address_taken(function, name);
}

static int ir_unroll_annotated_parse_increment(const IRFunction *function,
                                               size_t body_start,
                                               size_t body_end,
                                               const char *counter_symbol,
                                               size_t *increment_index,
                                               size_t *producer_index_out,
                                               long long *step_out) {
  *producer_index_out = (size_t)-1;
  for (size_t i = body_end; i > body_start;) {
    i--;
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP) continue;
    if (in->op == IR_OP_BINARY && !in->is_float && in->text &&
        strcmp(in->text, "+") == 0 && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, counter_symbol) == 0 &&
        ir_operand_is_symbol(&in->lhs) &&
        strcmp(in->lhs.name, counter_symbol) == 0 &&
        in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 0) {
      *increment_index = i;
      *step_out = in->rhs.int_value;
      return 1;
    }
    if (in->op == IR_OP_ASSIGN && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, counter_symbol) == 0 &&
        ir_operand_is_temp(&in->lhs)) {
      size_t producer_index = 0;
      if (!ir_find_last_writer_before(function, i, IR_OPERAND_TEMP,
                                      in->lhs.name, &producer_index) ||
          producer_index < body_start) {
        return 0;
      }
      const IRInstruction *producer = &function->instructions[producer_index];
      if (producer->op != IR_OP_BINARY || producer->is_float ||
          !producer->text || strcmp(producer->text, "+") != 0) {
        return 0;
      }
      if (ir_operand_is_symbol(&producer->lhs) &&
          strcmp(producer->lhs.name, counter_symbol) == 0 &&
          producer->rhs.kind == IR_OPERAND_INT &&
          producer->rhs.int_value > 0) {
        *increment_index = i;
        *producer_index_out = producer_index;
        *step_out = producer->rhs.int_value;
        return 1;
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

static int ir_unroll_annotated_append_clone(IRInstructionVector *vector,
                                            const IRInstruction *source) {
  IRInstruction cloned = {0};
  if (!ir_clone_instruction_plain(source, &cloned) ||
      !ir_instruction_vector_append_move(vector, &cloned)) {
    ir_instruction_destroy_storage(&cloned);
    return 0;
  }
  return 1;
}

static int ir_unroll_annotated_try_marker(IRFunction *function,
                                          size_t marker_index, int factor,
                                          int *changed) {
  size_t header_index = 0;
  if (!ir_find_next_non_nop(function, marker_index + 1, &header_index)) {
    return 1;
  }
  const IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !header->text) {
    return 1;
  }
  const char *header_label = header->text;

  size_t compare_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index)) {
    return 1;
  }
  const IRInstruction *compare = &function->instructions[compare_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      (strcmp(compare->text, "<") != 0 && strcmp(compare->text, "<=") != 0) ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name) {
    return 1;
  }
  const char *counter_symbol = compare->lhs.name;
  const IROperand *limit = &compare->rhs;
  if (limit->kind != IR_OPERAND_INT && limit->kind != IR_OPERAND_SYMBOL &&
      limit->kind != IR_OPERAND_TEMP) {
    return 1;
  }

  size_t branch_index = 0;
  if (!ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  const IRInstruction *branch = &function->instructions[branch_index];
  if (branch->op != IR_OP_BRANCH_ZERO || !branch->text ||
      branch->lhs.kind != IR_OPERAND_TEMP || !branch->lhs.name ||
      !ir_operand_names_match(&branch->lhs, &compare->dest)) {
    return 1;
  }

  size_t jump_index = (size_t)-1;
  for (size_t j = branch_index + 1; j < function->instruction_count; j++) {
    const IRInstruction *probe = &function->instructions[j];
    if (probe->op == IR_OP_JUMP && probe->text &&
        strcmp(probe->text, header_label) == 0) {
      jump_index = j;
      break;
    }
    if (!ir_unroll_annotated_body_op_safe(probe)) {
      return 1;
    }
  }
  if (jump_index == (size_t)-1) {
    return 1;
  }

  size_t body_start = branch_index + 1;
  size_t increment_index = 0;
  size_t producer_index = (size_t)-1;
  long long step = 0;
  if (!ir_unroll_annotated_parse_increment(function, body_start, jump_index,
                                           counter_symbol, &increment_index,
                                           &producer_index, &step)) {
    return 1;
  }
  (void)producer_index;

  int body_reaches_memory = 0;
  for (size_t j = body_start; j < jump_index; j++) {
    IROpcode op = function->instructions[j].op;
    if (op == IR_OP_CALL || op == IR_OP_STORE) {
      body_reaches_memory = 1;
    }
  }
  if (body_reaches_memory &&
      (!ir_unroll_symbol_is_private(function, counter_symbol) ||
       (limit->kind == IR_OPERAND_SYMBOL &&
        !ir_unroll_symbol_is_private(function, limit->name)))) {
    return 1;
  }

  for (size_t j = body_start; j < jump_index; j++) {
    if (j == increment_index) continue;
    const IRInstruction *in = &function->instructions[j];
    if (in->op == IR_OP_NOP || in->op == IR_OP_STORE) continue;
    if (ir_operand_is_symbol(&in->dest) &&
        strcmp(in->dest.name, counter_symbol) == 0) {
      return 1;
    }
    if ((limit->kind == IR_OPERAND_SYMBOL || limit->kind == IR_OPERAND_TEMP) &&
        in->dest.kind == limit->kind && in->dest.name && limit->name &&
        strcmp(in->dest.name, limit->name) == 0) {
      return 1;
    }
  }

  long long lead = step * (long long)(factor - 1);

  IRInstructionVector vector = {0};
  for (size_t i = 0; i < header_index; i++) {
    if (i == marker_index) continue;
    if (!ir_unroll_annotated_append_clone(&vector, &function->instructions[i]))
      goto fail;
  }

  char main_label[64];
  char guard_add[64];
  char guard_cmp[64];
  snprintf(main_label, sizeof(main_label), "ir_unroll_main_%zu", marker_index);
  snprintf(guard_add, sizeof(guard_add), ".unroll%zu_g0", marker_index);
  snprintf(guard_cmp, sizeof(guard_cmp), ".unroll%zu_g1", marker_index);

  {
    IRInstruction label = {0};
    label.op = IR_OP_LABEL;
    label.location = header->location;
    label.text = mettle_strdup(main_label);
    if (!label.text || !ir_instruction_vector_append_move(&vector, &label)) {
      ir_instruction_destroy_storage(&label);
      goto fail;
    }
  }
  {
    IRInstruction add = {0};
    add.op = IR_OP_BINARY;
    add.location = header->location;
    add.text = mettle_strdup("+");
    add.dest = ir_operand_temp(guard_add);
    add.lhs = ir_operand_symbol(counter_symbol);
    add.rhs = ir_operand_int(lead);
    if (!add.text || !add.dest.name || !add.lhs.name ||
        !ir_instruction_vector_append_move(&vector, &add)) {
      ir_instruction_destroy_storage(&add);
      goto fail;
    }
  }
  {
    IRInstruction cmp = {0};
    cmp.op = IR_OP_BINARY;
    cmp.location = header->location;
    cmp.text = mettle_strdup(compare->text);
    cmp.dest = ir_operand_temp(guard_cmp);
    cmp.lhs = ir_operand_temp(guard_add);
    cmp.rhs = limit->kind == IR_OPERAND_INT
                  ? ir_operand_int(limit->int_value)
                  : (limit->kind == IR_OPERAND_SYMBOL
                         ? ir_operand_symbol(limit->name)
                         : ir_operand_temp(limit->name));
    if (!cmp.text || !cmp.dest.name || !cmp.lhs.name ||
        (limit->kind != IR_OPERAND_INT && !cmp.rhs.name) ||
        !ir_instruction_vector_append_move(&vector, &cmp)) {
      ir_instruction_destroy_storage(&cmp);
      goto fail;
    }
  }
  {
    IRInstruction guard = {0};
    guard.op = IR_OP_BRANCH_ZERO;
    guard.location = header->location;
    guard.text = mettle_strdup(header_label);
    guard.lhs = ir_operand_temp(guard_cmp);
    if (!guard.text || !guard.lhs.name ||
        !ir_instruction_vector_append_move(&vector, &guard)) {
      ir_instruction_destroy_storage(&guard);
      goto fail;
    }
  }

  {
    IrUnrollTempSet private_temps = {0};
    if (!ir_unroll_collect_private_temps(function, body_start, jump_index,
                                         &private_temps)) {
      ir_unroll_temp_set_destroy(&private_temps);
      goto fail;
    }
    for (int copy = 0; copy < factor; copy++) {
      for (size_t b = body_start; b < jump_index; b++) {
        const IRInstruction *in = &function->instructions[b];
        if (in->op == IR_OP_NOP) continue;
        IRInstruction cloned = {0};
        if (!ir_clone_instruction_plain(in, &cloned) ||
            !ir_unroll_rename_copy_temps(
                &cloned, &private_temps,
                (long long)marker_index * 16 + copy) ||
            !ir_instruction_vector_append_move(&vector, &cloned)) {
          ir_instruction_destroy_storage(&cloned);
          ir_unroll_temp_set_destroy(&private_temps);
          goto fail;
        }
      }
    }
    ir_unroll_temp_set_destroy(&private_temps);
  }

  {
    IRInstruction back = {0};
    back.op = IR_OP_JUMP;
    back.location = header->location;
    back.text = mettle_strdup(main_label);
    if (!back.text || !ir_instruction_vector_append_move(&vector, &back)) {
      ir_instruction_destroy_storage(&back);
      goto fail;
    }
  }

  for (size_t i = header_index; i < function->instruction_count; i++) {
    if (!ir_unroll_annotated_append_clone(&vector, &function->instructions[i]))
      goto fail;
  }

  if (!ir_function_replace_instructions(function, &vector)) {
    goto fail;
  }
  *changed = 1;
  return 1;

fail:
  ir_instruction_vector_destroy(&vector);
  return 0;
}

int ir_unroll_annotated_loops_pass(IRFunction *function, int *changed) {
  if (!function || !changed) {
    return 0;
  }
  size_t prefix_len = strlen(IR_UNROLL_MARKER_PREFIX);
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_NOP || !in->text ||
        strncmp(in->text, IR_UNROLL_MARKER_PREFIX, prefix_len) != 0) {
      continue;
    }
    int factor = atoi(in->text + prefix_len);
    if (factor < 2 || factor > 16) {
      continue;
    }
    int fired = 0;
    if (!ir_unroll_annotated_try_marker(function, i, factor, &fired)) {
      return 0;
    }
    if (fired) {
      *changed = 1;
      return 1;
    }
  }
  return 1;
}
