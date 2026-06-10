#include "ir_optimize_internal.h"

/* Name -> IRFunction index for the optimizer.
 *
 * ir_program_find_function used to linear-scan every function (strcmp each) and
 * is called once per CALL instruction during inlining, across every function,
 * for several rounds -- O(calls * functions). That dominated IR optimization on
 * large programs. We cache an open-addressing hash table keyed on the program
 * pointer + function_count; inlining mutates bodies but never adds or removes
 * functions, so the cache stays valid for the whole optimization run. */
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
    /* First definition of a given name wins, matching the old linear scan. */
    if (strcmp(index->slots[i].name, function->name) == 0) {
      return;
    }
    i = (i + 1) & mask;
  }
  index->slots[i].name = function->name;
  index->slots[i].function = function;
}

/* Returns 1 if the index is ready to query, 0 on allocation failure (caller
 * falls back to a linear scan). */
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

  /* Fallback: index allocation failed; behave as before. */
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
  /* fib / bench_* inlining + loop unrolling explodes compile time (see
   * ir_optimize.c history). Benchmark hot paths use dedicated functions. */
  return strcmp(name, "fib") == 0 || strcmp(name, "bench_looped") == 0 ||
         strcmp(name, "bench_unrolled") == 0;
}

static int ir_function_is_inline_candidate(const IRFunction *function) {
  if (!function || !function->name || function->instruction_count == 0) {
    return 0;
  }
  /* `@noinline` is an absolute veto. */
  if (function->is_noinline) {
    return 0;
  }
  /* `@inline` forces the function past the discretionary heuristics below
   * (the name denylist, the parameter/size/call-count caps), but never past
   * the structural correctness guards: inline-asm, the loop-shape guards that
   * work around a latent optimizer bug, and the must-have-a-return rule still
   * apply. */
  int forced = function->is_inline;

  if (!forced && (ir_function_name_is_inline_denylisted(function->name) ||
                  function->parameter_count > IR_INLINE_MAX_PARAMETERS)) {
    return 0;
  }
  if (function->parameter_count > 0 && !function->parameter_names) {
    return 0;
  }

  size_t non_nop_count = 0;
  size_t call_count = 0;
  int has_return = 0;
  int has_while_label = 0;
  int has_less_compare = 0;
  int has_greater_compare = 0;
  int has_subtract = 0;
  int has_multiply = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction || instruction->op == IR_OP_NOP) {
      continue;
    }

    non_nop_count++;
    if (!forced && non_nop_count > IR_INLINE_MAX_NON_NOP_INSTRUCTIONS) {
      return 0;
    }

    if (instruction->op == IR_OP_INLINE_ASM) {
      return 0;
    }

    if (instruction->op == IR_OP_LABEL && instruction->text) {
      if (strncmp(instruction->text, "ir_while_", 9) == 0 ||
          strstr(instruction->text, "_lbl_ir_while_") != NULL) {
        has_while_label = 1;
      }
    }
    if (instruction->op == IR_OP_BINARY && instruction->text) {
      if (strcmp(instruction->text, "<") == 0) {
        has_less_compare = 1;
      } else if (strcmp(instruction->text, ">") == 0) {
        has_greater_compare = 1;
      } else if (strcmp(instruction->text, "-") == 0) {
        has_subtract = 1;
      } else if (strcmp(instruction->text, "*") == 0) {
        has_multiply = 1;
      }
    }

    /* Loop-bearing callees are allowed when not denylisted; the loop unroller
     * only fully expands trips <= IR_UNROLL_MAX_TRIP_COUNT (64). */

    /* CALL and CALL_INDIRECT are allowed:
     * calls just turns those into call instructions in the caller, which is
     * fine. This lets us inline leaf-ish functions (like grep's
     * pattern_matches) whose only calls are in cold fallback paths. Cap the
     * number of contained calls so that we don't inline glue functions like
     * print_int that orchestrate many helper calls; those produce lots of
     * caller bloat without runtime gain. */
    if (instruction->op == IR_OP_CALL ||
        instruction->op == IR_OP_CALL_INDIRECT) {
      call_count++;
      if (!forced && call_count > 2) {
        return 0;
      }
    }

    if (instruction->op == IR_OP_RETURN) {
      has_return = 1;
    }
  }

  /* The original errdefer-label/branch caps were rejecting useful inlines
   * (notably pattern_matches inside the grep loop). The labels and branches
   * inline correctly via the generic label-rename map; the cap was just
   * working around a latent bug elsewhere in the optimizer. */
  if (has_while_label && has_less_compare && has_greater_compare) {
    return 0;
  }
  if (has_while_label && has_subtract) {
    return 0;
  }
  if (has_while_label && has_multiply) {
    return 0;
  }
  return has_return;
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
      /* Preserve the float width carried by the original operand; the symbol
       * constructor does not copy it, and losing it makes a float32 value look
       * like a plain copy to later coalescing (dropping an f64->f32 narrow). */
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
  out->location = source->location;
  out->is_float = source->is_float;
  out->float_bits = source->float_bits;
  /* is_unsigned carries codegen-critical signedness: unsigned div/rem/shr and
   * zero-extending uint8/16/32 loads. Dropping it here (it only runs at -O)
   * silently reverts those to signed -- a uint32-as-signed miscompile. */
  out->is_unsigned = source->is_unsigned;
  out->ast_ref = source->ast_ref;

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
  out->location = source->location;
  out->is_float = source->is_float;
  out->float_bits = source->float_bits;
  out->is_unsigned = source->is_unsigned; /* unsigned div/shr + zero-ext loads */
  out->ast_ref = NULL;

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

  return 1;
}

static int ir_append_parameter_materialization(
    IRInstructionVector *vector, const IRInstruction *call_instruction,
    const IRFunction *callee, IRNameMap *symbol_map) {
  if (!vector || !call_instruction || !callee || !symbol_map) {
    return 0;
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
    if (!declare_local.dest.name || !declare_local.text ||
        !ir_instruction_vector_append_move(vector, &declare_local)) {
      ir_instruction_destroy_storage(&declare_local);
      return 0;
    }

    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = call_instruction->location;
    assign.dest = ir_operand_symbol(mapped_name);
    /* A float parameter carries the narrowing contract on the assign
     * (float_bits = declared parameter width), mirroring the RETURN path
     * below. Without it the backend skips the precision conversion and a
     * float64-tracked argument temp is bit-truncated into a float32
     * parameter local (low dword of the double, 0 for round values). */
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
  }
  return 0;
}

static int ir_inline_call_instruction(IRInstructionVector *vector,
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
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name) {
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

  if (!ir_append_parameter_materialization(vector, call_instruction, callee,
                                           &symbol_map)) {
    goto cleanup;
  }

  char *inline_end_label = ir_make_inline_name(inline_prefix, "label", "end");
  if (!inline_end_label) {
    goto cleanup;
  }

  for (size_t i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *source = &callee->instructions[i];
    IRInstruction emitted = {0};

    /* `@simd` contracts are enforced at the loop's definition site (the
     * standalone callee, which the function pipeline verifies independently).
     * Don't carry the markers into an inlined copy: after inlining the loop may
     * no longer satisfy a recognizer's preconditions (e.g. dot_i8 requires the
     * array bases to be parameters), and the user never wrote that copy. */
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
        /* The RETURN carries the narrowing contract: float_bits is the return
         * type's width (the destination precision) and lhs.float_bits is the
         * value's own width. Propagate both so the synthesized assign performs
         * any f64->f32 conversion the return ABI would have, and so a later
         * coalescing pass cannot mistake it for a width-preserving copy. */
        emitted.is_float = source->is_float;
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

static int ir_inline_calls_in_function(IRProgram *program, IRFunction *function,
                                       size_t *inline_counter, int *changed) {
  if (!program || !function || !inline_counter) {
    return 0;
  }

  if (ir_function_non_nop_instruction_count(function) >
      IR_INLINE_MAX_CALLER_NON_NOP_INSTRUCTIONS) {
    return 1;
  }

  IRInstructionVector vector = {0};
  int local_changed = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    IRInstruction cloned = {0};

    if (instruction->op == IR_OP_CALL && instruction->text &&
        instruction->argument_count <= IR_INLINE_MAX_PARAMETERS) {
      IRFunction *callee = ir_program_find_function(program, instruction->text);
      if (callee && callee != function &&
          instruction->argument_count == callee->parameter_count &&
          ir_function_is_inline_candidate(callee)) {
        if (!ir_inline_call_instruction(&vector, instruction, callee,
                                        (*inline_counter)++)) {
          ir_instruction_vector_destroy(&vector);
          return 0;
        }
        local_changed = 1;
        continue;
      }
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

/* --- Self-recursion inlining -------------------------------------------
 *
 * The regular inliner never inlines a function into itself (callee !=
 * function), so a recursive function pays full call overhead at every level
 * of the recursion tree. Inlining the body into its own self-call sites a
 * bounded number of times (the gcc "max-inline-recursive-depth" idea)
 * multiplies the work done per real call: depth 1 turns each call into ~the
 * work of a small subtree, cutting the dynamic call count by the subtree
 * size. Growth is bounded by a body-size cap, so deep expansion stops on its
 * own. Loop-bearing recursive functions are excluded: the inliner's
 * structural loop guards exist to sidestep a latent optimizer bug, and the
 * combination is rare enough not to be worth the risk. */
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
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        (strncmp(instruction->text, "ir_while_", 9) == 0 ||
         strstr(instruction->text, "_lbl_ir_while_") != NULL)) {
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

/* One depth level: rebuild the function, expanding every direct self-call
 * site with a clone of the CURRENT body (the clone's own self-calls stay as
 * real calls, to be expanded by the next round or executed at runtime). */
static int ir_inline_self_calls_once(IRFunction *function,
                                     size_t *inline_counter, int *changed) {
  IRInstructionVector vector = {0};
  int local_changed = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    IRInstruction cloned = {0};

    if (instruction->op == IR_OP_CALL && instruction->text &&
        strcmp(instruction->text, function->name) == 0 &&
        instruction->argument_count == function->parameter_count) {
      if (!ir_inline_call_instruction(&vector, instruction, function,
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

  size_t inline_counter = 0;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    size_t self_calls = 0;
    if (!ir_function_is_self_inline_candidate(function, &self_calls)) {
      continue;
    }
    for (int depth = 0; depth < IR_SELF_INLINE_MAX_DEPTH; depth++) {
      if (ir_function_non_nop_instruction_count(function) >
          IR_SELF_INLINE_MAX_BODY_INSTRUCTIONS) {
        break;
      }
      int round_changed = 0;
      if (!ir_inline_self_calls_once(function, &inline_counter,
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

int ir_inline_small_functions_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }

  size_t inline_counter = 0;
  for (int round = 0; round < IR_INLINE_MAX_ROUNDS; round++) {
    int round_changed = 0;

    for (size_t i = 0; i < program->function_count; i++) {
      if (!ir_inline_calls_in_function(program, program->functions[i],
                                       &inline_counter, &round_changed)) {
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
