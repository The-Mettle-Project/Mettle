#include "ir_lowering_internal.h"
#include "frontend/mtlc_lower_module.h"
#include "string_intern.h"

static void ir_lowering_free_control_stack(IRLoweringContext *context) {
  for (size_t j = 0; j < context->control_count; j++) {
    free(context->control_stack[j].break_label);
    free(context->control_stack[j].continue_label);
    free(context->control_stack[j].user_label);
  }
  free(context->control_stack);
  free(context->value_frames);
  context->control_stack = NULL;
  context->control_count = 0;
}

static IRProgram *ir_lowering_fail(IRProgram *ir_program,
                                   IRLoweringContext *context,
                                   char **error_message) {
  ir_program_destroy(ir_program);
  ir_lowering_free_control_stack(context);
  ir_local_bindings_reset(context);
  if (error_message) {
    *error_message = context->error_message
                         ? context->error_message
                         : mettle_strdup("Unknown IR lowering error");
  } else {
    free(context->error_message);
  }
  return NULL;
}

static int ir_symbol_is_volatile_global(const IRProgram *program,
                                        const IROperand *operand) {
  const IRModuleSymbol *symbol;
  if (!operand || operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
    return 0;
  }
  symbol = ir_program_lookup_symbol(program, operand->name);
  return symbol && symbol->kind == IR_MODSYM_VARIABLE && symbol->is_volatile;
}

static void ir_mark_volatile_global_accesses(IRProgram *program) {
  size_t f;
  if (!program) {
    return;
  }
  for (f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    size_t i;
    if (!function) {
      continue;
    }
    for (i = 0; i < function->instruction_count; i++) {
      IRInstruction *instruction = &function->instructions[i];
      size_t a;
      int touches = ir_symbol_is_volatile_global(program, &instruction->dest) ||
                    ir_symbol_is_volatile_global(program, &instruction->lhs) ||
                    ir_symbol_is_volatile_global(program, &instruction->rhs);
      for (a = 0; !touches && a < instruction->argument_count; a++) {
        touches = ir_symbol_is_volatile_global(program,
                                               &instruction->arguments[a]);
      }
      if (!touches) {
        continue;
      }
      instruction->is_volatile = 1;
      function->has_volatile_access = 1;
    }
  }
}

IRProgram *ir_lower_program(ASTNode *program, TypeChecker *type_checker,
                            SymbolTable *symbol_table, char **error_message,
                            int emit_runtime_checks, int emit_safety_checks) {
  if (error_message) {
    *error_message = NULL;
  }

  if (!program || program->type != AST_PROGRAM) {
    if (error_message) {
      *error_message =
          mettle_strdup("Expected AST_PROGRAM root for IR lowering");
    }
    return NULL;
  }

  IRProgram *ir_program = ir_program_create();
  if (!ir_program) {
    if (error_message) {
      *error_message = mettle_strdup("Failed to allocate IR program");
    }
    return NULL;
  }

  IRLoweringContext context = {0};
  context.type_checker = type_checker;
  context.symbol_table = symbol_table;
  context.emit_runtime_checks = emit_runtime_checks ? 1 : 0;
  context.emit_safety_checks = emit_safety_checks ? 1 : 0;
  context.emit_refinement_checks = g_ir_lowering_refinement_checks;
  context.emit_task_checks = g_ir_lowering_task_checks;
  context.emit_overflow_checks = g_ir_lowering_overflow_checks;
  context.assume_no_signed_overflow = g_ir_lowering_assume_no_signed_overflow;
  context.program = ir_program;
  ir_lower_live_forget_sources();
  ir_lower_live_forget_heap_types();

  Program *program_data = (Program *)program->data;
  if (!program_data) {
    return ir_program;
  }

  for (size_t i = 0; i < program_data->declaration_count; i++) {
    ASTNode *declaration = program_data->declarations[i];
    if (!declaration || declaration->type != AST_FUNCTION_DECLARATION) {
      continue;
    }
    FunctionDeclaration *function_data =
        (FunctionDeclaration *)declaration->data;
    if (!function_data) {
      ir_set_error(&context, "Malformed function declaration");
      return ir_lowering_fail(ir_program, &context, error_message);
    }
    if (!function_data->body || function_data->meta_role) {
      continue;
    }

    IRFunction *function = ir_lower_function(&context, declaration);
    if (!function) {
      if (!context.error_message) {
        ir_set_error(&context, "Failed to lower function declaration to IR");
      }
      return ir_lowering_fail(ir_program, &context, error_message);
    }

    if (!ir_program_add_function(ir_program, function)) {
      ir_function_destroy(function);
      ir_set_error(&context, "Out of memory while appending IR function");
      return ir_lowering_fail(ir_program, &context, error_message);
    }
  }

  if (!ir_lower_live_program(&context, ir_program)) {
    ir_set_error(&context, "Out of memory while preparing live frames");
    return ir_lowering_fail(ir_program, &context, error_message);
  }

  ir_lowering_free_control_stack(&context);
  ir_local_bindings_reset(&context);

  if (context.error_message) {
    if (error_message) {
      *error_message = context.error_message;
    } else {
      free(context.error_message);
    }
    return ir_program;
  }

  mtlc_lower_populate_module(ir_program, program, type_checker, symbol_table);
  ir_lower_live_register_symbols(&context, ir_program);
  ir_mark_volatile_global_accesses(ir_program);
  if (context.emitted_task_check &&
      !ir_program_lookup_symbol(ir_program, "mettle_safety_task_capture_check")) {
    IRModuleSymbol entry;
    MtlcType *params[4];
    MtlcType *cstring = ir_program_lookup_type(ir_program, "cstring");
    MtlcType *word = ir_program_lookup_type(ir_program, "uint32");
    memset(&entry, 0, sizeof(entry));
    entry.name = "mettle_safety_task_capture_check";
    entry.kind = IR_MODSYM_FUNCTION;
    entry.is_extern = 1;
    entry.has_body = 0;
    entry.return_type = ir_program_lookup_type(ir_program, "void");
    if (cstring && word) {
      params[0] = cstring;
      params[1] = cstring;
      params[2] = cstring;
      params[3] = word;
      entry.param_types = params;
      entry.param_count = 4;
    }
    ir_program_add_symbol(ir_program, &entry);
  }

  return ir_program;
}

static int ir_value_scope_writes_locals(const ASTNode *node, int inside_scope) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION) {
    return 0;
  }
  if (inside_scope && node->type == AST_ASSIGNMENT) {
    return 1;
  }
  if (node->type == AST_VALUE_SCOPE) {
    inside_scope = 1;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (ir_value_scope_writes_locals(node->children[i], inside_scope)) {
      return 1;
    }
  }
  return 0;
}

IRFunction *ir_lower_function(IRLoweringContext *context,
                                     ASTNode *declaration) {
  if (!declaration || declaration->type != AST_FUNCTION_DECLARATION) {
    return NULL;
  }

  FunctionDeclaration *function_data = (FunctionDeclaration *)declaration->data;
  if (!function_data || !function_data->name) {
    ir_set_error(context, "Malformed function declaration");
    return NULL;
  }

  context->current_return_type_name = function_data->return_type;
  context->current_function_name = function_data->name;
  mettle_compiler_ctx_set_function_name(function_data->name);

  IRFunction *function = ir_function_create(function_data->name);
  if (!function) {
    ir_set_error(context, "Out of memory while creating IR function");
    return NULL;
  }
  function->location = declaration->location;
  if (function_data->return_type) {
    function->return_type_name =
        mettle_strdup(ir_backend_type_name(function_data->return_type));
  }
  function->is_inline = function_data->is_inline;
  function->is_inline_contract = function_data->is_inline_contract;
  function->is_swappable = function_data->is_swappable;
  function->is_naked = function_data->is_naked;
  function->is_interrupt = function_data->is_interrupt;
  function->is_exported = function_data->is_exported;
  function->is_noinline = function_data->is_noinline ||
                          function_data->is_swappable ||
                          function_data->is_naked ||
                          function_data->is_interrupt;
  function->is_pure = function_data->is_pure;
  function->deadline_cycles = function_data->deadline_cycles;
  function->has_deadline = function_data->has_deadline;
  function->deadline_inclusive = function_data->deadline_inclusive;
  function->reference_twin =
      function_data->reference_twin
          ? string_intern(function_data->reference_twin)
          : NULL;
  function->explain_code = function_data->explain_code
                               ? string_intern(function_data->explain_code)
                               : NULL;
  function->explain_text = function_data->explain_text
                               ? string_intern(function_data->explain_text)
                               : NULL;
  function->is_noalloc = function_data->is_noalloc;
  ir_function_set_effects(function, IR_EFFECT_CLAUSE_WITH,
                          (const char *const *)function_data->effects_with,
                          function_data->effects_with_count);
  ir_function_set_effects(function, IR_EFFECT_CLAUSE_FORBIDS,
                          (const char *const *)function_data->effects_forbids,
                          function_data->effects_forbids_count);
  ir_function_set_effects(function, IR_EFFECT_CLAUSE_REQUIRES,
                          (const char *const *)function_data->effects_requires,
                          function_data->effects_requires_count);
  if (function_data->is_kernel) {
    const char *groups[8];
    size_t group_count = 0;
    for (size_t g = 0;
         g < function_data->effects_provides_count && group_count < 6; g++) {
      groups[group_count++] = function_data->effects_provides[g];
    }
    groups[group_count++] = "Warp";
    groups[group_count++] = "Block";
    ir_function_set_effects(function, IR_EFFECT_CLAUSE_PROVIDES, groups,
                            group_count);
  } else {
    ir_function_set_effects(function, IR_EFFECT_CLAUSE_PROVIDES,
                            (const char *const *)function_data->effects_provides,
                            function_data->effects_provides_count);
  }
  function->is_test = function_data->is_test;
  function->numerics_contract =
      function_data->numerics_contract
          ? string_intern(function_data->numerics_contract)
          : NULL;
  function->is_rule = function_data->is_rule;
  function->rewrite_role = function_data->rewrite_role;
  function->is_kernel = function_data->is_kernel;
  function->kernel_block[0] = function_data->kernel_block[0];
  function->kernel_block[1] = function_data->kernel_block[1];
  function->kernel_block[2] = function_data->kernel_block[2];
  function->kernel_threads_per_item = function_data->kernel_threads_per_item;
  context->current_function_simd_default = function_data->simd_mode;
  ir_local_bindings_reset(context);
  if (!ir_function_set_parameters(function,
                                  (const char **)function_data->parameter_names,
                                  (const char **)function_data->parameter_types,
                                  function_data->parameter_count)) {
    ir_set_error(context,
                 "Out of memory while recording IR function parameters");
    ir_function_destroy(function);
    return NULL;
  }
  for (size_t i = 0; i < function_data->parameter_count; i++) {
    if (!function_data->parameter_names[i]) {
      continue;
    }
    ir_local_bind_parameter(context, function_data->parameter_names[i],
                            function_data->parameter_types
                                ? ir_backend_type_name(
                                      function_data->parameter_types[i])
                                : NULL);
  }

  char *entry_label = ir_new_label_name(context, "entry");
  if (!entry_label) {
    ir_set_error(context,
                 "Out of memory while allocating function entry label");
    ir_function_destroy(function);
    return NULL;
  }
  if (!ir_emit_label_instruction(context, function, entry_label,
                                 declaration->location)) {
    free(entry_label);
    ir_function_destroy(function);
    return NULL;
  }
  free(entry_label);

  IRDeferScope defers = {0};
  context->current_defers = &defers;
  context->value_frame_count = 0;
  context->pin_identifier_reads =
      ir_value_scope_writes_locals(function_data->body, 0);
  if (function_data->body &&
      !ir_lower_statement_with_defers(context, function, function_data->body,
                                      &defers)) {
    ir_defer_stack_free(&defers.stack);
    ir_function_destroy(function);
    return NULL;
  }

  if (!function_data->is_naked &&
      (function->instruction_count == 0 ||
       function->instructions[function->instruction_count - 1].op !=
           IR_OP_RETURN)) {
    IROperand implicit_value = ir_operand_none();
    if (!ir_emit_return_with_defers(context, function, &defers, &implicit_value,
                                    declaration->location)) {
      ir_operand_destroy(&implicit_value);
      ir_defer_stack_free(&defers.stack);
      ir_function_destroy(function);
      return NULL;
    }
    ir_operand_destroy(&implicit_value);
  }

  ir_defer_stack_free(&defers.stack);
  if (!ir_function_rebuild_cfg(function)) {
    ir_set_error(context, "Out of memory while building IR control-flow graph");
    ir_function_destroy(function);
    return NULL;
  }
  return function;
}

int g_ir_lowering_explain = 0;
int g_ir_lowering_refinement_checks = 0;
int g_ir_lowering_task_checks = 0;
int g_ir_lowering_live = 0;
int g_ir_lowering_overflow_checks = 0;
int g_ir_lowering_assume_no_signed_overflow = 0;

void ir_lowering_set_explain(int enabled) { g_ir_lowering_explain = enabled; }

void ir_lowering_set_refinement_checks(int enabled) {
  g_ir_lowering_refinement_checks = enabled;
}

void ir_lowering_set_live(int enabled) {
  g_ir_lowering_live = enabled ? 1 : 0;
}

void ir_lowering_set_task_checks(int enabled) {
  g_ir_lowering_task_checks = enabled;
}

void ir_lowering_set_assume_no_signed_overflow(int enabled) {
  g_ir_lowering_assume_no_signed_overflow = enabled ? 1 : 0;
}

void ir_lowering_set_overflow_checks(int enabled) {
  g_ir_lowering_overflow_checks = enabled;
}

void ir_set_error(IRLoweringContext *context, const char *format, ...) {
  if (!context || context->error_message || !format) {
    return;
  }

  if (context->current_function_name) {
    mettle_compiler_ctx_set_function_name(context->current_function_name);
  }
  mettle_compiler_ctx_set_phase(METTLE_COMPILER_PHASE_IR_LOWERING);

  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, format, copy);
  va_end(copy);

  if (needed > 0) {
    context->error_message = malloc((size_t)needed + 1);
    if (context->error_message) {
      vsnprintf(context->error_message, (size_t)needed + 1, format, args);
    }
  }
  va_end(args);
}

static char *ir_write_id(char *buffer, size_t size, int id) {
  char *at = buffer + size - 1;

  *at = '\0';
  if (id == 0) {
    *--at = '0';
    return at;
  }
  while (id > 0) {
    *--at = (char)('0' + id % 10);
    id /= 10;
  }
  return at;
}

char *ir_new_temp_name(IRLoweringContext *context) {
  char buffer[64];
  char *digits = ir_write_id(buffer, sizeof(buffer), context->next_temp_id++);

  *--digits = 't';
  *--digits = '.';
  return mettle_strdup(digits);
}

char *ir_new_label_name(IRLoweringContext *context, const char *prefix) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "ir_%s_%d", prefix ? prefix : "label",
           context->next_label_id++);
  return mettle_strdup(buffer);
}

int ir_emit(IRLoweringContext *context, IRFunction *function,
                   const IRInstruction *instruction) {
  IRInstruction stamped;
  if (context && context->current_expansion_note && instruction &&
      !instruction->expansion_note) {
    stamped = *instruction;
    stamped.expansion_note = context->current_expansion_note;
    instruction = &stamped;
  }
  if (!ir_function_append_instruction(function, instruction)) {
    ir_set_error(context, "Out of memory while appending IR instruction");
    return 0;
  }
  return 1;
}

int ir_emit_jump_instruction(IRLoweringContext *context,
                                    IRFunction *function, const char *label,
                                    SourceLocation location) {
  if (!context || !function || !label) {
    return 0;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_JUMP;
  instruction.location = location;
  instruction.text = (char *)label;
  return ir_emit(context, function, &instruction);
}

int ir_emit_label_instruction(IRLoweringContext *context,
                                     IRFunction *function, const char *label,
                                     SourceLocation location) {
  if (!context || !function || !label) {
    return 0;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_LABEL;
  instruction.location = location;
  instruction.text = (char *)label;
  return ir_emit(context, function, &instruction);
}

int ir_emit_simd_marker(IRLoweringContext *context, IRFunction *function,
                               char which, int id, int mode,
                               SourceLocation location) {
  if (!context || !function) {
    return 0;
  }
  char buffer[48];
  snprintf(buffer, sizeof(buffer), IR_SIMD_MARKER_PREFIX "%c:%d:%d", which, id,
           mode);
  IRInstruction instruction = {0};
  instruction.op = IR_OP_NOP;
  instruction.location = location;
  instruction.text = buffer;
  return ir_emit(context, function, &instruction);
}

int ir_emit_unroll_marker(IRLoweringContext *context, IRFunction *function,
                          int factor, SourceLocation location) {
  if (!context || !function) {
    return 0;
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), IR_UNROLL_MARKER_PREFIX "%d", factor);
  IRInstruction instruction = {0};
  instruction.op = IR_OP_NOP;
  instruction.location = location;
  instruction.text = buffer;
  return ir_emit(context, function, &instruction);
}

int ir_emit_parallel_marker(IRLoweringContext *context, IRFunction *function,
                            const char *loop_label, SourceLocation location) {
  char buffer[256];
  if (!context || !function || !loop_label) {
    return 0;
  }
  snprintf(buffer, sizeof(buffer), IR_PARALLEL_MARKER_PREFIX "1:%s",
           loop_label);
  IRInstruction instruction = {0};
  instruction.op = IR_OP_NOP;
  instruction.location = location;
  instruction.text = buffer;
  return ir_emit(context, function, &instruction);
}

int ir_make_temp_operand(IRLoweringContext *context,
                                IROperand *out_temp) {
  char buffer[32];
  char *temp_name;

  if (!context || !out_temp) {
    return 0;
  }

  temp_name = ir_write_id(buffer, sizeof(buffer), context->next_temp_id++);
  *--temp_name = 't';
  *--temp_name = '.';
  *out_temp = ir_operand_temp(temp_name);
  if (out_temp->kind != IR_OPERAND_TEMP || !out_temp->name) {
    ir_set_error(context, "Failed to create IR temp operand");
    return 0;
  }

  return 1;
}
