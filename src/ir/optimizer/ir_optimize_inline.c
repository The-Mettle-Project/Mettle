#include "ir_optimize_internal.h"

#include <stdio.h>

static IRFunctionIndex g_ir_function_index = {0};

void ir_function_index_reset(void) {
  free(g_ir_function_index.slots);
  g_ir_function_index.slots = NULL;
  g_ir_function_index.slot_count = 0;
  g_ir_function_index.program = NULL;
  g_ir_function_index.function_count = 0;
}

static void ir_function_index_insert(IRFunctionIndex *index,
                                     IRFunction *function) {
  size_t mask = index->slot_count - 1;
  size_t i = mettle_fnv1a_hash(function->name) & mask;
  while (index->slots[i].name) {
    if (strcmp(index->slots[i].name, function->name) == 0) {
      return;
    }
    i = (i + 1) & mask;
  }
  index->slots[i].name = function->name;
  index->slots[i].function = function;
}

static int ir_function_index_ensure(const IRProgram *program) {
  if (g_ir_function_index.program == program &&
      g_ir_function_index.function_count == program->function_count &&
      g_ir_function_index.slots) {
    return 1;
  }

  ir_function_index_reset();

  size_t slot_count = 16;
  while (slot_count < program->function_count * 2) {
    slot_count *= 2;
  }

  IRFunctionIndexSlot *slots = calloc(slot_count, sizeof(IRFunctionIndexSlot));
  if (!slots) {
    return 0;
  }

  g_ir_function_index.slots = slots;
  g_ir_function_index.slot_count = slot_count;
  g_ir_function_index.program = program;
  g_ir_function_index.function_count = program->function_count;

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name) {
      ir_function_index_insert(&g_ir_function_index, function);
    }
  }

  return 1;
}

IRFunction *ir_program_find_function(IRProgram *program, const char *name) {
  if (!program || !name) {
    return NULL;
  }

  if (ir_function_index_ensure(program)) {
    const IRFunctionIndex *index = &g_ir_function_index;
    size_t mask = index->slot_count - 1;
    size_t i = mettle_fnv1a_hash(name) & mask;
    while (index->slots[i].name) {
      if (strcmp(index->slots[i].name, name) == 0) {
        return index->slots[i].function;
      }
      i = (i + 1) & mask;
    }
    return NULL;
  }

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function;
    }
  }

  return NULL;
}

static int ir_function_name_is_inline_denylisted(const char *name) {
  if (!name) {
    return 0;
  }
  return strcmp(name, "fib") == 0 || strcmp(name, "bench_looped") == 0 ||
         strcmp(name, "bench_unrolled") == 0;
}

static MTLC_THREAD_LOCAL const char *g_inline_refusal_code = NULL;
static MTLC_THREAD_LOCAL char g_inline_why[256];
static size_t ir_function_non_nop_instruction_count(const IRFunction *function);

static int ir_inline_call_counts(const IRInstruction *instruction) {
  if (instruction->op == IR_OP_CALL_INDIRECT) {
    return 1;
  }
  return instruction->op == IR_OP_CALL &&
         (!instruction->text ||
          strncmp(instruction->text, "mettle_safety_", 14) != 0);
}

static size_t ir_inline_call_total(const IRFunction *function) {
  size_t total = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    total += (size_t)ir_inline_call_counts(&function->instructions[i]);
  }
  return total;
}

static const char *ir_inline_call_count_why(const IRFunction *function,
                                            size_t budget) {
  char shown[128];
  size_t total = ir_inline_call_total(function);
  ir_display_name(function->name, shown, sizeof(shown));
  snprintf(g_inline_why, sizeof(g_inline_why),
           "`%s` makes %zu call%s of its own, and a callee here may make at "
           "most %zu",
           shown, total, total == 1 ? "" : "s", budget);
  return g_inline_why;
}

#define IR_INLINE_WHY(target, id, text)                                        \
  do {                                                                         \
    *(target) = (text);                                                        \
    g_inline_refusal_code = (id);                                              \
  } while (0)

#define IR_INLINE_SIZE_CACHE 8

static size_t ir_inline_measured_size(const IRFunction *function) {
  static const IRFunction *cached_fn[IR_INLINE_SIZE_CACHE];
  static size_t cached_count[IR_INLINE_SIZE_CACHE];
  static size_t cached_size[IR_INLINE_SIZE_CACHE];
  static size_t next_slot;
  for (size_t i = 0; i < IR_INLINE_SIZE_CACHE; i++) {
    if (cached_fn[i] == function &&
        cached_count[i] == function->instruction_count) {
      return cached_size[i];
    }
  }
  size_t size = ir_inline_cleaned_instruction_count(function);
  cached_fn[next_slot] = function;
  cached_count[next_slot] = function->instruction_count;
  cached_size[next_slot] = size;
  next_slot = (next_slot + 1) % IR_INLINE_SIZE_CACHE;
  return size;
}

typedef struct {
  size_t non_nop_count;
  size_t cleaned_count;
  int cleaned_known;
  size_t call_count;
  int has_return;
  int has_while_label;
} IRInlineScan;

static size_t ir_inline_loop_body_budget(int site_loop_depth) {
  return site_loop_depth >= 2 ? 2u * IR_INLINE_LOOP_BODY_INSTRUCTIONS
                              : IR_INLINE_LOOP_BODY_INSTRUCTIONS;
}

static int ir_inline_label_is_while(const IRInstruction *instruction) {
  return instruction->op == IR_OP_LABEL && instruction->text &&
         (strncmp(instruction->text, "ir_while_", 9) == 0 ||
          strstr(instruction->text, "_lbl_ir_while_") != NULL);
}

static int ir_inline_signature_rejects(const IRFunction *function, int forced,
                                       const char **why_not,
                                       const char **fix) {
  if (!forced && ir_function_name_is_inline_denylisted(function->name)) {
    IR_INLINE_WHY(why_not, "callee-denylisted",
                  "the callee is on the compiler's inline denylist "
                  "(a compile-time-blowup guard)");
    return 1;
  }
  if (!forced && function->parameter_count > IR_INLINE_MAX_PARAMETERS) {
    IR_INLINE_WHY(why_not, "too-many-parameters",
                  "the callee has more than 16 parameters");
    *fix = "pass a struct instead of a long parameter list";
    return 1;
  }
  if (function->parameter_count > 0 && !function->parameter_names) {
    IR_INLINE_WHY(why_not, "callee-parameter-names",
                  "the callee's parameter names are unavailable to the inliner");
    return 1;
  }
  return 0;
}

static int ir_inline_over_body_budget(const IRFunction *function, int forced,
                                      size_t body_budget, IRInlineScan *scan) {
  if (forced || scan->non_nop_count <= body_budget) {
    return 0;
  }
  if (!scan->cleaned_known) {
    scan->cleaned_count = ir_inline_measured_size(function);
    scan->cleaned_known = 1;
  }
  return scan->cleaned_count > body_budget;
}

static int ir_inline_scan_body(const IRFunction *function, int forced,
                               size_t body_budget, size_t nested_call_budget,
                               IRInlineScan *scan, const char **why_not,
                               const char **fix) {
  scan->non_nop_count = 0;
  scan->cleaned_count = SIZE_MAX;
  scan->cleaned_known = 0;
  scan->call_count = 0;
  scan->has_return = 0;
  scan->has_while_label = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];

    if (!instruction || instruction->op == IR_OP_NOP) {
      continue;
    }
    scan->non_nop_count++;
    if (ir_inline_over_body_budget(function, forced, body_budget, scan)) {
      char shown[128];
      ir_display_name(function->name, shown, sizeof(shown));
      snprintf(g_inline_why, sizeof(g_inline_why),
               "`%s` is %zu instructions, and a callee here may be at most %zu",
               shown,
               scan->cleaned_count != SIZE_MAX
                   ? scan->cleaned_count
                   : ir_function_non_nop_instruction_count(function),
               body_budget);
      IR_INLINE_WHY(why_not, "callee-over-budget", g_inline_why);
      *fix = "mark the callee @inline to override the budget, or compile "
             "with --pgo so a measured-hot callee overrides it";
      return 0;
    }
    if (instruction->op == IR_OP_INLINE_ASM &&
        (!instruction->asm_operands ||
         ir_inline_asm_defines_label(instruction->text))) {
      IR_INLINE_WHY(why_not, "callee-inline-asm",
                    "the callee contains inline assembly");
      return 0;
    }
    if (ir_inline_label_is_while(instruction)) {
      scan->has_while_label = 1;
    }
    if (ir_inline_call_counts(instruction)) {
      scan->call_count++;
      if (!forced && scan->call_count > nested_call_budget) {
        IR_INLINE_WHY(why_not, "callee-call-count",
                      ir_inline_call_count_why(function, nested_call_budget));
        *fix = "mark the callee @inline to override the call-count cap";
        return 0;
      }
    }
    if (instruction->op == IR_OP_RETURN) {
      scan->has_return = 1;
    }
  }
  return 1;
}

static int ir_inline_loop_body_rejects(const IRFunction *function, int forced,
                                       int site_loop_depth, IRInlineScan *scan,
                                       const char **why_not,
                                       const char **fix) {
  size_t budget = ir_inline_loop_body_budget(site_loop_depth);

  if (forced || !scan->has_while_label) {
    return 0;
  }
  if (!scan->cleaned_known &&
      scan->non_nop_count > IR_INLINE_LOOP_BODY_INSTRUCTIONS) {
    scan->cleaned_count = ir_inline_measured_size(function);
    scan->cleaned_known = 1;
  }
  if (scan->non_nop_count <= budget || scan->cleaned_count <= budget) {
    return 0;
  }
  IR_INLINE_WHY(why_not, "callee-has-loop",
                "the callee's loop body is over the inline size budget for "
                "a loop-bearing callee (the call itself costs little next "
                "to the loop inside it)");
  *fix = "mark the callee @inline to inline it anyway";
  return 1;
}

static int ir_function_is_inline_candidate_at(const IRFunction *function,
                                              int site_loop_depth,
                                              const char **why_not,
                                              const char **fix) {
  const char *unused;
  IRInlineScan scan;
  int forced;
  size_t body_budget;

  if (!why_not) {
    why_not = &unused;
  }
  if (!fix) {
    fix = &unused;
  }
  *why_not = NULL;
  *fix = NULL;
  if (!function || !function->name || function->instruction_count == 0) {
    IR_INLINE_WHY(why_not, "callee-no-body",
                  "the callee has no body available to the inliner");
    return 0;
  }
  if (function->is_noinline) {
    IR_INLINE_WHY(why_not, "callee-noinline",
                  "the callee is marked @noinline");
    *fix = "remove @noinline if inlining is wanted here";
    return 0;
  }
  forced = function->is_inline;
  body_budget = ir_opt_inline_body_budget(function);
  if (site_loop_depth >= 2) {
    body_budget *= 2u;
  }
  if (ir_inline_signature_rejects(function, forced, why_not, fix)) {
    return 0;
  }
  if (!ir_inline_scan_body(function, forced, body_budget,
                           ir_opt_inline_nested_call_budget(function), &scan,
                           why_not, fix)) {
    return 0;
  }
  if (ir_inline_loop_body_rejects(function, forced, site_loop_depth, &scan,
                                  why_not, fix)) {
    return 0;
  }
  if (!scan.has_return) {
    IR_INLINE_WHY(why_not, "callee-no-return",
                  "the callee has no return instruction the inliner can rewrite");
    return 0;
  }
  return 1;
}

static int ir_function_is_inline_candidate(const IRFunction *function,
                                           const char **why_not,
                                           const char **fix) {
  return ir_function_is_inline_candidate_at(function, 0, why_not, fix);
}

static size_t ir_function_non_nop_instruction_count(const IRFunction *function) {
  if (!function) {
    return 0;
  }

  size_t count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_NOP) {
      count++;
    }
  }
  return count;
}

static int ir_inline_rewrite_operand(const IROperand *source, IROperand *out,
                                     IRNameMap *symbol_map,
                                     IRNameMap *temp_map,
                                     IRNameMap *label_map,
                                     const char *inline_prefix) {
  if (!source || !out) {
    return 0;
  }

  if (source->kind == IR_OPERAND_SYMBOL && source->name) {
    const char *mapped = ir_name_map_lookup(symbol_map, source->name);
    if (mapped) {
      *out = ir_operand_symbol(mapped);
      out->float_bits = source->float_bits;
      return out->kind == IR_OPERAND_SYMBOL && out->name;
    }
  } else if (source->kind == IR_OPERAND_TEMP && source->name) {
    const char *mapped =
        ir_name_map_get_or_create(temp_map, source->name, inline_prefix, "tmp");
    if (!mapped) {
      return 0;
    }
    *out = ir_operand_temp(mapped);
    out->float_bits = source->float_bits;
    return out->kind == IR_OPERAND_TEMP && out->name;
  } else if (source->kind == IR_OPERAND_LABEL && source->name) {
    const char *mapped = ir_name_map_get_or_create(label_map, source->name,
                                                   inline_prefix, "lbl");
    if (!mapped) {
      return 0;
    }
    *out = ir_operand_label(mapped);
    return out->kind == IR_OPERAND_LABEL && out->name;
  }

  return ir_operand_clone(source, out);
}

int ir_clone_instruction_plain(const IRInstruction *source,
                                      IRInstruction *out) {
  if (!source || !out) {
    return 0;
  }

  memset(out, 0, sizeof(*out));
  out->op = source->op;
  out->intrinsic = source->intrinsic;
  out->address_space = source->address_space;
  out->memory_order = source->memory_order;
  out->failure_memory_order = source->failure_memory_order;
  out->memory_scope = source->memory_scope;
  out->memory_regions = source->memory_regions;
  out->alias_class = source->alias_class;
  out->async_copy_element_count = source->async_copy_element_count;
  out->async_copy_transaction_bytes = source->async_copy_transaction_bytes;
  out->async_copy_pending_groups = source->async_copy_pending_groups;
  out->async_copy_cache = source->async_copy_cache;
  out->async_copy_generated = source->async_copy_generated;
  if (!ir_instruction_tensor_copy(out, source)) {
    return 0;
  }
  out->tensor_transfer_has_prepared_view =
      source->tensor_transfer_has_prepared_view;
  out->tensor_mma_count = source->tensor_mma_count;
  out->tensor_residency_id = source->tensor_residency_id;
  out->tensor_residency_role = source->tensor_residency_role;
  out->tensor_residency_scope = source->tensor_residency_scope;
  out->location = source->location;
  out->is_float = source->is_float;
  out->is_unsigned = source->is_unsigned;
  out->float_bits = source->float_bits;
  out->is_unsigned = source->is_unsigned;
  out->allocates = source->allocates;
  out->asm_operands = source->asm_operands;
  out->uniform_condition = source->uniform_condition;
  out->ast_ref = source->ast_ref;
  out->value_type = source->value_type;
  out->column_base = source->column_base;

  if (!ir_operand_clone(&source->dest, &out->dest) ||
      !ir_operand_clone(&source->lhs, &out->lhs) ||
      !ir_operand_clone(&source->rhs, &out->rhs)) {
    ir_instruction_destroy_storage(out);
    return 0;
  }

  if (source->text) {
    out->text = mettle_strdup(source->text);
    if (!out->text) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
  }

  out->argument_count = source->argument_count;
  if (source->argument_count > 0) {
    out->arguments = calloc(source->argument_count, sizeof(IROperand));
    if (!out->arguments) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
    for (size_t i = 0; i < source->argument_count; i++) {
      if (!ir_operand_clone(&source->arguments[i], &out->arguments[i])) {
        ir_instruction_destroy_storage(out);
        return 0;
      }
    }
  }
  if (source->argument_types && source->argument_count > 0) {
    out->argument_types =
        malloc(source->argument_count * sizeof(*out->argument_types));
    if (!out->argument_types) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
    memcpy(out->argument_types, source->argument_types,
           source->argument_count * sizeof(*out->argument_types));
  }

  return 1;
}

static int ir_clone_instruction_for_inline(const IRInstruction *source,
                                           IRInstruction *out,
                                           IRNameMap *symbol_map,
                                           IRNameMap *temp_map,
                                           IRNameMap *label_map,
                                           const char *inline_prefix) {
  if (!source || !out || !symbol_map || !temp_map || !label_map ||
      !inline_prefix) {
    return 0;
  }

  memset(out, 0, sizeof(*out));
  out->op = source->op;
  out->intrinsic = source->intrinsic;
  out->address_space = source->address_space;
  out->memory_order = source->memory_order;
  out->failure_memory_order = source->failure_memory_order;
  out->memory_scope = source->memory_scope;
  out->memory_regions = source->memory_regions;
  out->alias_class = source->alias_class;
  out->async_copy_element_count = source->async_copy_element_count;
  out->async_copy_transaction_bytes = source->async_copy_transaction_bytes;
  out->async_copy_pending_groups = source->async_copy_pending_groups;
  out->async_copy_cache = source->async_copy_cache;
  out->async_copy_generated = source->async_copy_generated;
  if (!ir_instruction_tensor_copy(out, source)) {
    return 0;
  }
  out->tensor_transfer_has_prepared_view =
      source->tensor_transfer_has_prepared_view;
  out->tensor_mma_count = source->tensor_mma_count;
  out->tensor_residency_id = source->tensor_residency_id;
  out->tensor_residency_role = source->tensor_residency_role;
  out->tensor_residency_scope = source->tensor_residency_scope;
  out->location = source->location;
  out->is_float = source->is_float;
  out->is_unsigned = source->is_unsigned;
  out->float_bits = source->float_bits;
  out->is_unsigned = source->is_unsigned;
  out->allocates = source->allocates;
  out->asm_operands = source->asm_operands;
  out->uniform_condition = source->uniform_condition;
  out->ast_ref = NULL;
  out->value_type = source->value_type;
  out->column_base = source->column_base;

  if (!ir_inline_rewrite_operand(&source->dest, &out->dest, symbol_map,
                                 temp_map, label_map, inline_prefix) ||
      !ir_inline_rewrite_operand(&source->lhs, &out->lhs, symbol_map, temp_map,
                                 label_map, inline_prefix) ||
      !ir_inline_rewrite_operand(&source->rhs, &out->rhs, symbol_map, temp_map,
                                 label_map, inline_prefix)) {
    ir_instruction_destroy_storage(out);
    return 0;
  }

  if (source->text) {
    if (source->op == IR_OP_LABEL || source->op == IR_OP_JUMP ||
        source->op == IR_OP_BRANCH_ZERO || source->op == IR_OP_BRANCH_EQ) {
      const char *mapped =
          ir_name_map_get_or_create(label_map, source->text, inline_prefix, "lbl");
      if (!mapped) {
        ir_instruction_destroy_storage(out);
        return 0;
      }
      out->text = mettle_strdup(mapped);
    } else {
      out->text = mettle_strdup(source->text);
    }

    if (!out->text) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
  }

  out->argument_count = source->argument_count;
  if (source->argument_count > 0) {
    out->arguments = calloc(source->argument_count, sizeof(IROperand));
    if (!out->arguments) {
      ir_instruction_destroy_storage(out);
      return 0;
    }

    for (size_t i = 0; i < source->argument_count; i++) {
      if (!ir_inline_rewrite_operand(&source->arguments[i], &out->arguments[i],
                                     symbol_map, temp_map, label_map,
                                     inline_prefix)) {
        ir_instruction_destroy_storage(out);
        return 0;
      }
    }
  }
  if (source->argument_types && source->argument_count > 0) {
    out->argument_types =
        malloc(source->argument_count * sizeof(*out->argument_types));
    if (!out->argument_types) {
      ir_instruction_destroy_storage(out);
      return 0;
    }
    memcpy(out->argument_types, source->argument_types,
           source->argument_count * sizeof(*out->argument_types));
  }

  return 1;
}

static int ir_append_parameter_materialization(
    const IRProgram *program, IRInstructionVector *vector,
    const IRInstruction *call_instruction, const IRFunction *callee,
    IRNameMap *symbol_map) {
  if (!vector || !call_instruction || !callee || !symbol_map) {
    return 0;
  }
  const IRModuleSymbol *callee_symbol =
      ir_program_lookup_symbol(program, callee->name);
  if (callee_symbol && (callee_symbol->kind != IR_MODSYM_FUNCTION ||
                        callee_symbol->param_count != callee->parameter_count ||
                        !callee_symbol->param_types)) {
    callee_symbol = NULL;
  }

  for (size_t i = 0; i < callee->parameter_count; i++) {
    const char *parameter_name = callee->parameter_names[i];
    const char *mapped_name = ir_name_map_lookup(symbol_map, parameter_name);
    const char *type_name = "int64";
    if (!parameter_name || !mapped_name) {
      return 0;
    }
    if (call_instruction->arguments[i].kind == IR_OPERAND_SYMBOL &&
        call_instruction->arguments[i].name &&
        strcmp(mapped_name, call_instruction->arguments[i].name) == 0) {
      continue;
    }
    if (callee->parameter_types && callee->parameter_types[i] &&
        callee->parameter_types[i][0] != '\0') {
      type_name = callee->parameter_types[i];
    }

    IRInstruction declare_local = {0};
    declare_local.op = IR_OP_DECLARE_LOCAL;
    declare_local.location = call_instruction->location;
    declare_local.dest = ir_operand_symbol(mapped_name);
    declare_local.text = mettle_strdup(type_name);
    MtlcType *parameter_type =
        callee_symbol ? callee_symbol->param_types[i] : NULL;
    if (parameter_type && (parameter_type->kind == MTLC_TYPE_STRUCT ||
                           parameter_type->kind == MTLC_TYPE_ARRAY ||
                           parameter_type->kind == MTLC_TYPE_TAGGED_ENUM)) {
      declare_local.value_type = parameter_type;
    }
    if (!declare_local.dest.name || !declare_local.text ||
        !ir_instruction_vector_append_move(vector, &declare_local)) {
      ir_instruction_destroy_storage(&declare_local);
      return 0;
    }

    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = call_instruction->location;
    assign.dest = ir_operand_symbol(mapped_name);
    if (strcmp(type_name, "float32") == 0) {
      assign.is_float = 1;
      assign.float_bits = 32;
    } else if (strcmp(type_name, "float64") == 0 ||
               strcmp(type_name, "float") == 0) {
      assign.is_float = 1;
      assign.float_bits = 64;
    }
    if (!assign.dest.name ||
        !ir_operand_clone(&call_instruction->arguments[i], &assign.lhs) ||
        !ir_instruction_vector_append_move(vector, &assign)) {
      ir_instruction_destroy_storage(&assign);
      return 0;
    }
  }

  return 1;
}

static int ir_function_assigns_symbol(const IRFunction *function,
                                      const char *symbol_name) {
  if (!function || !symbol_name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction || instruction->op == IR_OP_NOP ||
        instruction->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        strcmp(instruction->dest.name, symbol_name) == 0) {
      return 1;
    }
    if (instruction->op == IR_OP_ADDRESS_OF &&
        ir_operand_is_symbol(&instruction->lhs) &&
        strcmp(instruction->lhs.name, symbol_name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_inline_return_is_narrow_integer(const IRFunction *callee) {
  static const char *const NARROW[] = {"int32", "uint32", "int16",
                                       "uint16", "int8",  "uint8"};
  size_t i = 0;
  if (!callee || !callee->return_type_name) {
    return 0;
  }
  for (i = 0; i < sizeof(NARROW) / sizeof(NARROW[0]); i++) {
    if (strcmp(callee->return_type_name, NARROW[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_inline_integer_bytes(const char *type, int *is_unsigned) {
  static const struct {
    const char *name;
    int bytes;
    int is_unsigned;
  } WIDTHS[] = {{"int8", 1, 0},   {"uint8", 1, 1},  {"bool", 1, 1},
                {"int16", 2, 0},  {"uint16", 2, 1}, {"int32", 4, 0},
                {"uint32", 4, 1}, {"int64", 8, 0},  {"uint64", 8, 1}};
  size_t i = 0;
  if (!type) {
    return 0;
  }
  for (i = 0; i < sizeof(WIDTHS) / sizeof(WIDTHS[0]); i++) {
    if (strcmp(type, WIDTHS[i].name) == 0) {
      if (is_unsigned) {
        *is_unsigned = WIDTHS[i].is_unsigned;
      }
      return WIDTHS[i].bytes;
    }
  }
  return 0;
}

static int ir_inline_width_fits(int src_bytes, int src_unsigned, int dst_bytes,
                                int dst_unsigned) {
  if (src_bytes <= 0 || dst_bytes <= 0 || src_bytes > dst_bytes) {
    return 0;
  }
  if (src_unsigned == dst_unsigned) {
    return 1;
  }
  return src_unsigned && src_bytes < dst_bytes;
}

static const IRInstruction *ir_inline_sole_temp_def(const IRFunction *callee,
                                                    const char *name) {
  const IRInstruction *found = NULL;
  size_t i = 0;
  for (i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *in = &callee->instructions[i];
    if (in->op == IR_OP_NOP || in->dest.kind != IR_OPERAND_TEMP ||
        !in->dest.name || strcmp(in->dest.name, name) != 0) {
      continue;
    }
    if (found) {
      return NULL;
    }
    found = in;
  }
  return found;
}

static int ir_inline_value_fits_return(const IRFunction *callee,
                                       const IROperand *value, int bytes,
                                       int is_unsigned, int depth) {
  if (!value || bytes <= 0 || depth > 4) {
    return 0;
  }
  if (value->kind == IR_OPERAND_INT) {
    if (bytes >= 8) {
      return 1;
    }
    if (is_unsigned) {
      return value->int_value >= 0 &&
             value->int_value <= (1LL << (bytes * 8)) - 1;
    }
    return value->int_value >= -(1LL << (bytes * 8 - 1)) &&
           value->int_value <= (1LL << (bytes * 8 - 1)) - 1;
  }
  if (value->kind == IR_OPERAND_SYMBOL && value->name) {
    int src_unsigned = 0;
    int declared = ir_inline_integer_bytes(
        ir_function_local_declared_type((IRFunction *)callee, value->name),
        &src_unsigned);
    size_t p = 0;
    for (p = 0; declared == 0 && p < callee->parameter_count; p++) {
      if (callee->parameter_names && callee->parameter_names[p] &&
          strcmp(callee->parameter_names[p], value->name) == 0) {
        declared = ir_inline_integer_bytes(
            callee->parameter_types ? callee->parameter_types[p] : NULL,
            &src_unsigned);
      }
    }
    return ir_inline_width_fits(declared, src_unsigned, bytes, is_unsigned);
  }
  if (value->kind != IR_OPERAND_TEMP || !value->name) {
    return 0;
  }
  {
    const IRInstruction *def = ir_inline_sole_temp_def(callee, value->name);
    if (!def || def->is_float) {
      return 0;
    }
    switch (def->op) {
    case IR_OP_CAST: {
      int cast_unsigned = 0;
      int cast_bytes = ir_inline_integer_bytes(def->text, &cast_unsigned);
      return ir_inline_width_fits(cast_bytes, cast_unsigned, bytes,
                                  is_unsigned);
    }
    case IR_OP_LOAD:
      return def->rhs.kind == IR_OPERAND_INT && def->rhs.int_value > 0 &&
             def->rhs.int_value <= 8 &&
             ir_inline_width_fits((int)def->rhs.int_value,
                                  def->is_unsigned ? 1 : 0, bytes, is_unsigned);
    case IR_OP_ASSIGN:
      return ir_inline_value_fits_return(callee, &def->lhs, bytes, is_unsigned,
                                         depth + 1);
    case IR_OP_BINARY:
      return def->text &&
             (strcmp(def->text, "==") == 0 || strcmp(def->text, "!=") == 0 ||
              strcmp(def->text, "<") == 0 || strcmp(def->text, "<=") == 0 ||
              strcmp(def->text, ">") == 0 || strcmp(def->text, ">=") == 0 ||
              strcmp(def->text, "&&") == 0 || strcmp(def->text, "||") == 0);
    default:
      return 0;
    }
  }
}

static int ir_inline_call_instruction(const IRProgram *program,
                                      IRInstructionVector *vector,
                                      const IRInstruction *call_instruction,
                                      const IRFunction *callee,
                                      size_t inline_site_id) {
  if (!vector || !call_instruction || !callee) {
    return 0;
  }

  char *inline_prefix = ir_make_inline_prefix(callee->name, inline_site_id);
  if (!inline_prefix) {
    return 0;
  }

  IRNameMap symbol_map = {0};
  IRNameMap temp_map = {0};
  IRNameMap label_map = {0};
  int ok = 0;

  for (size_t i = 0; i < callee->parameter_count; i++) {
    const char *parameter_name = callee->parameter_names[i];
    if (!parameter_name) {
      goto cleanup;
    }

    const IROperand *argument = &call_instruction->arguments[i];
    char *mapped = NULL;
    int add_ok = 0;
    if (argument->kind == IR_OPERAND_SYMBOL && argument->name &&
        !ir_function_assigns_symbol(callee, parameter_name)) {
      add_ok = ir_name_map_add(&symbol_map, parameter_name, argument->name);
    } else {
      mapped = ir_make_inline_name(inline_prefix, "param", parameter_name);
      if (!mapped) {
        goto cleanup;
      }
      add_ok = ir_name_map_add(&symbol_map, parameter_name, mapped);
      free(mapped);
    }
    if (!add_ok) {
      goto cleanup;
    }
  }

  for (size_t i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *instruction = &callee->instructions[i];
    if (instruction->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol(&instruction->dest)) {
      char *mapped =
          ir_make_inline_name(inline_prefix, "local", instruction->dest.name);
      if (!mapped) {
        goto cleanup;
      }
      int add_ok = ir_name_map_add(&symbol_map, instruction->dest.name, mapped);
      free(mapped);
      if (!add_ok) {
        goto cleanup;
      }
    }
  }

  if (!ir_append_parameter_materialization(program, vector, call_instruction,
                                           callee, &symbol_map)) {
    goto cleanup;
  }

  char *inline_end_label = ir_make_inline_name(inline_prefix, "label", "end");
  if (!inline_end_label) {
    goto cleanup;
  }

  for (size_t i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *source = &callee->instructions[i];
    IRInstruction emitted = {0};

    if (source->op == IR_OP_NOP && source->text &&
        strncmp(source->text, IR_SIMD_MARKER_PREFIX,
                strlen(IR_SIMD_MARKER_PREFIX)) == 0) {
      continue;
    }

    if (source->op == IR_OP_RETURN) {
      if (source->lhs.kind != IR_OPERAND_NONE &&
          call_instruction->dest.kind != IR_OPERAND_NONE) {
        emitted.op = IR_OP_ASSIGN;
        emitted.location = call_instruction->location;
        int ret_unsigned = 0;
        int ret_bytes =
            ir_inline_integer_bytes(callee->return_type_name, &ret_unsigned);
        if (!source->is_float && ir_inline_return_is_narrow_integer(callee) &&
            !ir_inline_value_fits_return(callee, &source->lhs, ret_bytes,
                                         ret_unsigned, 0)) {
          char *narrowed = mettle_strdup(callee->return_type_name);
          if (!narrowed) {
            ir_instruction_destroy_storage(&emitted);
            free(inline_end_label);
            goto cleanup;
          }
          emitted.op = IR_OP_CAST;
          emitted.text = narrowed;
        }
        emitted.is_float = source->is_float;
        emitted.is_unsigned = source->is_unsigned;
        emitted.float_bits = source->float_bits;
        if (!ir_operand_clone(&call_instruction->dest, &emitted.dest) ||
            !ir_inline_rewrite_operand(&source->lhs, &emitted.lhs, &symbol_map,
                                       &temp_map, &label_map, inline_prefix) ||
            !ir_instruction_vector_append_move(vector, &emitted)) {
          ir_instruction_destroy_storage(&emitted);
          free(inline_end_label);
          goto cleanup;
        }
      }

      memset(&emitted, 0, sizeof(emitted));
      emitted.op = IR_OP_JUMP;
      emitted.location = call_instruction->location;
      emitted.text = mettle_strdup(inline_end_label);
      if (!emitted.text || !ir_instruction_vector_append_move(vector, &emitted)) {
        ir_instruction_destroy_storage(&emitted);
        free(inline_end_label);
        goto cleanup;
      }
      continue;
    }

    if (!ir_clone_instruction_for_inline(source, &emitted, &symbol_map, &temp_map,
                                         &label_map, inline_prefix) ||
        !ir_instruction_vector_append_move(vector, &emitted)) {
      ir_instruction_destroy_storage(&emitted);
      free(inline_end_label);
      goto cleanup;
    }
  }

  {
    IRInstruction end_label = {0};
    end_label.op = IR_OP_LABEL;
    end_label.location = call_instruction->location;
    end_label.text = inline_end_label;
    if (!ir_instruction_vector_append_move(vector, &end_label)) {
      ir_instruction_destroy_storage(&end_label);
      free(inline_end_label);
      goto cleanup;
    }
  }

  ok = 1;

cleanup:
  ir_name_map_destroy(&label_map);
  ir_name_map_destroy(&temp_map);
  ir_name_map_destroy(&symbol_map);
  free(inline_prefix);
  return ok;
}

static int ir_call_site_is_in_loop(const IRFunction *function, size_t site) {
  for (size_t h = 0; h < site; h++) {
    const IRInstruction *header = &function->instructions[h];
    if (header->op != IR_OP_LABEL || !header->text ||
        !ir_label_is_while_header(header->text)) {
      continue;
    }
    for (size_t j = site + 1; j < function->instruction_count; j++) {
      const IRInstruction *jmp = &function->instructions[j];
      if (jmp->op == IR_OP_JUMP && jmp->text &&
          strcmp(jmp->text, header->text) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static char *ir_build_in_loop_bitmap(const IRFunction *function) {
  char *in_loop = NULL;
  for (size_t h = 0; h < function->instruction_count; h++) {
    const IRInstruction *header = &function->instructions[h];
    if (header->op != IR_OP_LABEL || !header->text ||
        !ir_label_is_while_header(header->text)) {
      continue;
    }
    size_t last = 0;
    int found = 0;
    for (size_t j = h + 1; j < function->instruction_count; j++) {
      const IRInstruction *jmp = &function->instructions[j];
      if (jmp->op == IR_OP_JUMP && jmp->text &&
          strcmp(jmp->text, header->text) == 0) {
        last = j;
        found = 1;
      }
    }
    if (!found) {
      continue;
    }
    if (!in_loop) {
      in_loop = calloc(function->instruction_count, 1);
      if (!in_loop) {
        return NULL;
      }
    }
    for (size_t k = h; k <= last; k++) {
      if ((unsigned char)in_loop[k] < 255) {
        in_loop[k] = (char)(in_loop[k] + 1);
      }
    }
  }
  return in_loop;
}

static int ir_function_is_tiny_leaf(const IRFunction *callee) {
  size_t non_nop = 0;
  for (size_t i = 0; i < callee->instruction_count; i++) {
    IROpcode op = callee->instructions[i].op;
    if (op == IR_OP_NOP) {
      continue;
    }
    if (op == IR_OP_CALL || op == IR_OP_CALL_INDIRECT) {
      return 0;
    }
    if (++non_nop > IR_INLINE_TINY_LEAF_NON_NOP_INSTRUCTIONS) {
      return 0;
    }
  }
  return 1;
}

static int ir_inline_calls_in_function(IRProgram *program, IRFunction *function,
                                       size_t *inline_counter, int *changed) {
  if (!program || !function || !inline_counter) {
    return 0;
  }

  int caller_over_budget = ir_function_non_nop_instruction_count(function) >
                           ir_opt_inline_caller_budget(function);
  char *in_loop = ir_build_in_loop_bitmap(function);

  size_t first_inline = function->instruction_count;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_CALL && instruction->text &&
        instruction->argument_count <= IR_INLINE_MAX_PARAMETERS) {
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (callee && callee != function &&
          instruction->argument_count == callee->parameter_count &&
          (!caller_over_budget || callee->is_inline ||
           ir_function_is_tiny_leaf(callee) || ir_opt_function_is_hot(callee) ||
           ir_opt_site_is_hot(function, instruction->location) ||
           (in_loop && in_loop[i])) &&
          ir_function_is_inline_candidate_at(callee, in_loop ? in_loop[i] : 0,
                                             NULL, NULL)) {
        first_inline = i;
        break;
      }
    }
  }
  if (first_inline == function->instruction_count) {
    free(in_loop);
    return 1;
  }

  IRInstructionVector vector = {0};
  int local_changed = 0;
  if (!ir_instruction_vector_reserve(&vector, function->instruction_count)) {
    free(in_loop);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    if (i >= first_inline && instruction->op == IR_OP_CALL &&
        instruction->text &&
        instruction->argument_count <= IR_INLINE_MAX_PARAMETERS) {
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (callee && callee != function &&
          instruction->argument_count == callee->parameter_count &&
          (!caller_over_budget || callee->is_inline ||
           ir_function_is_tiny_leaf(callee) || ir_opt_function_is_hot(callee) ||
           ir_opt_site_is_hot(function, instruction->location) ||
           (in_loop && in_loop[i])) &&
          ir_function_is_inline_candidate_at(callee, in_loop ? in_loop[i] : 0,
                                             NULL, NULL)) {
        if (ir_explain_enabled()) {
          char entity[160];
          size_t weight = ir_function_non_nop_instruction_count(callee);
          snprintf(entity, sizeof(entity), "call to `%s`", instruction->text);
          ir_explain_remark(function->name, entity, instruction->location, 1,
                            "inlined", NULL, NULL, NULL);
          ir_explain_remark_code("inlined");
          ir_explain_remark_quantity("calleeInstructions", (long)weight);
          if (weight <= IR_EXPLAIN_TRIVIAL_CALLEE_INSTRUCTIONS) {
            ir_explain_remark_trivial();
          }
        }
        if (!ir_inline_call_instruction(program, &vector, instruction, callee,
                                        (*inline_counter)++)) {
          ir_instruction_vector_destroy(&vector);
          free(in_loop);
          return 0;
        }
        local_changed = 1;
        continue;
      }
    }

    if (!ir_instruction_vector_append_move(&vector, instruction)) {
      ir_instruction_vector_destroy(&vector);
      free(in_loop);
      return 0;
    }
  }
  free(in_loop);

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);
  function->instructions = vector.items;
  function->instruction_count = vector.count;
  function->instruction_capacity = vector.capacity;
  vector.items = NULL;
  vector.count = 0;
  vector.capacity = 0;

  if (local_changed && changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_self_call_is_loop_resident(const IRFunction *function) {
  char *in_loop = ir_build_in_loop_bitmap(function);
  int resident = 0;
  if (!in_loop) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_CALL && instruction->text &&
        function->name && strcmp(instruction->text, function->name) == 0 &&
        in_loop[i]) {
      resident = 1;
      break;
    }
  }
  free(in_loop);
  return resident;
}

static int ir_function_is_self_inline_candidate(const IRFunction *function,
                                                size_t *self_call_count_out) {
  if (!function || !function->name || function->instruction_count == 0 ||
      function->is_noinline) {
    return 0;
  }
  if (function->parameter_count > IR_INLINE_MAX_PARAMETERS ||
      (function->parameter_count > 0 && !function->parameter_names)) {
    return 0;
  }
  size_t self_calls = 0;
  int has_return = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (instruction->op == IR_OP_INLINE_ASM ||
        instruction->op == IR_OP_CALL_INDIRECT) {
      return 0;
    }
    if (instruction->op == IR_OP_CALL && instruction->text &&
        strcmp(instruction->text, function->name) == 0) {
      if (instruction->argument_count != function->parameter_count) {
        return 0;
      }
      self_calls++;
      if (self_calls > IR_SELF_INLINE_MAX_SELF_CALLS) {
        return 0;
      }
    }
    if (instruction->op == IR_OP_RETURN) {
      has_return = 1;
    }
  }

  if (self_call_count_out) {
    *self_call_count_out = self_calls;
  }
  return has_return && self_calls > 0;
}

static int ir_inline_self_calls_once(const IRProgram *program,
                                     IRFunction *function,
                                     size_t *inline_counter, int *changed) {
  IRInstructionVector vector = {0};
  int local_changed = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    IRInstruction cloned = {0};

    if (instruction->op == IR_OP_CALL && instruction->text &&
        strcmp(instruction->text, function->name) == 0 &&
        instruction->argument_count == function->parameter_count) {
      if (!ir_inline_call_instruction(program, &vector, instruction, function,
                                      (*inline_counter)++)) {
        ir_instruction_vector_destroy(&vector);
        return 0;
      }
      local_changed = 1;
      continue;
    }

    if (!ir_clone_instruction_plain(instruction, &cloned) ||
        !ir_instruction_vector_append_move(&vector, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ir_instruction_vector_destroy(&vector);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);
  function->instructions = vector.items;
  function->instruction_count = vector.count;
  function->instruction_capacity = vector.capacity;
  vector.items = NULL;
  vector.count = 0;
  vector.capacity = 0;

  if (local_changed && changed) {
    *changed = 1;
  }
  return 1;
}

int ir_inline_self_recursion_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    size_t self_calls = 0;
    if (!ir_function_is_self_inline_candidate(function, &self_calls)) {
      continue;
    }
    int max_depth = ir_opt_self_inline_max_depth(function);
    size_t body_budget = ir_opt_self_inline_body_budget(function);
    if (max_depth > 1 && ir_self_call_is_loop_resident(function)) {
      max_depth = 1;
    }
    for (int depth = 0; depth < max_depth; depth++) {
      if (ir_function_non_nop_instruction_count(function) >
          body_budget) {
        break;
      }
      int round_changed = 0;
      if (!ir_inline_self_calls_once(program, function,
                                     &program->next_inline_id,
                                     &round_changed)) {
        return 0;
      }
      if (!round_changed) {
        break;
      }
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

static int ir_function_contains_simd_kernel(const IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IROpcode op = function->instructions[i].op;
    if (op >= IR_OP_COUNT_WORD_STARTS && op <= IR_OP_SIMD_OUTER_LANE_F64) {
      return 1;
    }
  }
  return 0;
}

static int ir_inline_site_loop_depth(IRFunction *caller,
                                     const IRInstruction *instruction) {
  char *depths;
  int depth = 0;
  if (!caller || instruction < caller->instructions ||
      (size_t)(instruction - caller->instructions) >=
          caller->instruction_count) {
    return 0;
  }
  depths = ir_build_in_loop_bitmap(caller);
  if (depths) {
    depth = (unsigned char)depths[instruction - caller->instructions];
    free(depths);
  }
  return depth;
}

static void ir_inline_site_reason(IRFunction *caller,
                                  const IRInstruction *instruction,
                                  IRFunction *callee, const char **reason,
                                  const char **fix) {
  *reason = NULL;
  *fix = NULL;
  g_inline_refusal_code = NULL;
  if (callee != caller && ir_function_contains_simd_kernel(callee)) {
    IR_INLINE_WHY(reason, "callee-has-kernel",
                  "the callee's loops were vectorized into SIMD kernels after "
                  "inlining ran; it stays a real call (the kernel runs the same "
                  "either way)");
    return;
  }
  if (callee == caller) {
    IR_INLINE_WHY(reason, "recursive",
                  "the call is directly recursive");
    *fix = "bounded self-recursion expansion applies automatically; rewrite "
           "as a loop for full control";
  } else if (ir_function_non_nop_instruction_count(caller) >
                 ir_opt_inline_caller_budget(caller) &&
             !callee->is_inline && !ir_function_is_tiny_leaf(callee) &&
             !ir_opt_function_is_hot(callee) &&
             instruction >= caller->instructions &&
             !ir_opt_site_is_hot(caller, instruction->location) &&
             !ir_call_site_is_in_loop(
                 caller, (size_t)(instruction - caller->instructions))) {
    char shown[128];
    ir_display_name(caller->name, shown, sizeof(shown));
    snprintf(g_inline_why, sizeof(g_inline_why),
             "`%s` is already %zu instructions against a budget of %zu, and "
             "this call sits outside every loop, so it runs once per call of "
             "`%s`",
             shown, ir_function_non_nop_instruction_count(caller),
             (size_t)ir_opt_inline_caller_budget(caller), shown);
    IR_INLINE_WHY(reason, "caller-over-budget", g_inline_why);
  } else if (instruction->argument_count > IR_INLINE_MAX_PARAMETERS ||
             instruction->argument_count != callee->parameter_count) {
    IR_INLINE_WHY(reason, "argument-count",
                  "the call's argument count doesn't match what the inliner "
                  "handles for this callee");
  } else if (ir_function_is_inline_candidate_at(
                 callee, ir_inline_site_loop_depth(caller, instruction), reason,
                 fix)) {
    IR_INLINE_WHY(reason, "rounds-exhausted",
                  "inlining rounds reached their limit before this call could "
                  "be revisited");
  }
}

static int ir_inline_refusal_is_declined(IRFunction *caller,
                                         const IRInstruction *instruction,
                                         const char *code) {
  size_t site = (size_t)(instruction - caller->instructions);
  if (code && (strcmp(code, "callee-has-kernel") == 0 ||
               strcmp(code, "caller-over-budget") == 0)) {
    return 1;
  }
  if (code && strcmp(code, "recursive") == 0) {
    return 0;
  }
  return !ir_call_site_is_in_loop(caller, site) &&
         !ir_opt_site_is_hot(caller, instruction->location);
}

static int ir_inline_callee_is_written(const IRFunction *callee) {
  return callee->name && strncmp(callee->name, "__", 2) != 0 &&
         callee->location.filename &&
         ir_explain_file_enabled(callee->location.filename);
}

void ir_inline_explain_report_remaining(IRProgram *program) {
  if (!program || !ir_explain_enabled()) {
    return;
  }

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function || function->rewrite_role) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *instruction = &function->instructions[i];
      if (instruction->op != IR_OP_CALL || !instruction->text) {
        continue;
      }
      if (!ir_explain_location_enabled(&instruction->location)) {
        continue;
      }
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (!callee) {
        continue;
      }
      const char *reason = NULL;
      const char *fix = NULL;
      ir_inline_site_reason(function, instruction, callee, &reason, &fix);
      const char *refusal_code = g_inline_refusal_code;
      int declined = ir_inline_refusal_is_declined(function, instruction,
                                                   refusal_code);
      if (declined || !ir_inline_callee_is_written(callee)) {
        fix = NULL;
      }

      const char *verified = NULL;
      int advisory = 0;
      char corrected_fix[320];
      if (fix && strstr(fix, "@inline") && !callee->is_inline &&
          !callee->is_noinline) {
        int saved = callee->is_inline;
        callee->is_inline = 1;
        const char *forced_reason = NULL;
        const char *unused_fix = NULL;
        if (ir_function_is_inline_candidate(callee, &forced_reason,
                                            &unused_fix)) {
          verified = "re-checked with @inline pretend-applied: the structural "
                     "guards pass, so this call will inline";
        } else if (forced_reason && reason &&
                   strcmp(forced_reason, reason) == 0) {
          fix = NULL;
        } else {
          snprintf(corrected_fix, sizeof(corrected_fix),
                   "none. Re-checked with @inline "
                   "pretend-applied and it still won't inline: %s",
                   forced_reason ? forced_reason : "a structural guard");
          fix = corrected_fix;
          advisory = 1;
        }
        callee->is_inline = saved;
      }

      char entity[160];
      snprintf(entity, sizeof(entity), "call to `%s`", instruction->text);
      ir_explain_remark(function->name, entity, instruction->location, 0,
                        declined ? "kept as a call" : "NOT inlined", reason,
                        fix, verified);
      if (advisory) {
        ir_explain_remark_advisory();
      }
      if (declined) {
        ir_explain_remark_declined();
      }
      ir_explain_remark_code(refusal_code);
      ir_explain_remark_quantity("calleeInstructions",
                                 (long)ir_function_non_nop_instruction_count(callee));
    }
  }
}

int ir_inline_enforce_contracts(IRProgram *program) {
  if (!program) {
    return 1;
  }
  struct {
    size_t line, column;
    const char *callee;
  } reported[64];
  size_t reported_count = 0;
  int ok = 1;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *instruction = &function->instructions[i];
      if (instruction->op != IR_OP_CALL || !instruction->text) {
        continue;
      }
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (!callee || !callee->is_inline_contract) {
        continue;
      }
      int already_reported = 0;
      for (size_t r = 0; r < reported_count; r++) {
        if (reported[r].line == instruction->location.line &&
            reported[r].column == instruction->location.column &&
            strcmp(reported[r].callee, instruction->text) == 0) {
          already_reported = 1;
          break;
        }
      }
      if (already_reported) {
        ok = 0;
        continue;
      }
      if (reported_count < 64) {
        reported[reported_count].line = instruction->location.line;
        reported[reported_count].column = instruction->location.column;
        reported[reported_count].callee = instruction->text;
        reported_count++;
      }
      const char *reason = NULL;
      const char *fix = NULL;
      ir_inline_site_reason(function, instruction, callee, &reason, &fix);
      fprintf(stderr,
              "%s:%zu:%zu: error: @inline! call to `%s` was not inlined: "
              "%s%s%s\n",
              instruction->location.filename ? instruction->location.filename
                                             : "<input>",
              instruction->location.line, instruction->location.column,
              instruction->text, reason ? reason : "unknown",
              fix ? "; " : "", fix ? fix : "");
      ok = 0;
    }
  }
  if (!ok) {
    ir_optimize_note_user_error();
  }
  return ok;
}

int ir_inline_explain_simulate_force_inline(IRProgram *program,
                                            IRFunction *caller,
                                            const char *callee_name,
                                            int *was_noinline_out,
                                            const char **decline_reason_out) {
  if (decline_reason_out) {
    *decline_reason_out = NULL;
  }
  if (!program || !caller || !callee_name) {
    return 0;
  }
  IRFunction *callee = ir_program_find_function(program, callee_name);
  if (!callee || callee == caller) {
    return 0;
  }

  for (size_t i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *ins = &callee->instructions[i];
    if (ins->op == IR_OP_CALL && ins->text &&
        strcmp(ins->text, callee_name) == 0) {
      if (decline_reason_out) {
        *decline_reason_out =
            "the callee is recursive (it calls itself), so inlining cannot "
            "remove the call from the loop body";
      }
      return 0;
    }
  }

  int saved_is_inline = callee->is_inline;
  int saved_is_noinline = callee->is_noinline;
  if (was_noinline_out) {
    *was_noinline_out = callee->is_noinline;
  }
  callee->is_inline = 1;
  callee->is_noinline = 0;
  const char *why_not = NULL;
  int candidate = ir_function_is_inline_candidate(callee, &why_not, NULL);
  if (!candidate && decline_reason_out) {
    *decline_reason_out = why_not;
  }
  int changed = 0;
  if (candidate) {
    size_t inline_counter = program->next_inline_id;
    for (int round = 0; round < IR_INLINE_MAX_ROUNDS; round++) {
      int round_changed = 0;
      if (!ir_inline_calls_in_function(program, caller, &inline_counter,
                                       &round_changed)) {
        changed = 0;
        break;
      }
      if (!round_changed) {
        break;
      }
      changed = 1;
      if (caller->instruction_count >
          16 * IR_INLINE_MAX_CALLER_NON_NOP_INSTRUCTIONS) {
        changed = 0;
        break;
      }
    }
  }
  callee->is_inline = saved_is_inline;
  callee->is_noinline = saved_is_noinline;
  return candidate && changed;
}

int ir_inline_small_functions_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }

  for (int round = 0; round < IR_INLINE_MAX_ROUNDS; round++) {
    int round_changed = 0;

    for (size_t i = 0; i < program->function_count; i++) {
      if (program->functions[i] && program->functions[i]->rewrite_role) {
        continue;
      }
      if (!ir_inline_calls_in_function(program, program->functions[i],
                                       &program->next_inline_id,
                                       &round_changed)) {
        return 0;
      }
    }

    if (round_changed && changed) {
      *changed = 1;
    }
    if (!round_changed) {
      break;
    }
  }

  return 1;
}
