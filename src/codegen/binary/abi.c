#include "codegen/binary/internal.h"

#include "common.h" /* mettle_fnv1a_hash, for the operand-type index */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BINARY_LOOP_WEIGHT_CAP 262144

int code_generator_binary_get_parameter_offset(
    BinaryFunctionContext *context, const char *name) {
  return binary_named_slot_table_get_offset(&context->parameter_slots, name);
}

int code_generator_binary_get_local_offset(BinaryFunctionContext *context,
                                                  const char *name) {
  return binary_named_slot_table_get_offset(&context->local_slots, name);
}

int code_generator_binary_get_temp_offset(BinaryFunctionContext *context,
                                                 const char *name) {
  return binary_named_slot_table_get_offset(&context->temp_slots, name);
}

int code_generator_binary_get_symbol_offset(BinaryFunctionContext *context,
                                                   const char *name) {
  int offset = 0;
  if (!context || !name) {
    return -1;
  }

  offset = code_generator_binary_get_parameter_offset(context, name);
  if (offset > 0) {
    return offset;
  }

  return code_generator_binary_get_local_offset(context, name);
}


int code_generator_binary_resolved_type_is_stack_scalar(MtlcType *type) {
  if (!type) {
    return 0;
  }

  if (type->kind == MTLC_TYPE_STRING) {
    return 0;
  }

  if (code_generator_binary_resolved_type_is_supported(type, 0)) {
    return 1;
  }

  return type->kind == MTLC_TYPE_FLOAT64 && type->size == 8;
}

int code_generator_binary_type_is_direct_aggregate(MtlcType *type) {
  return type && code_generator_type_is_aggregate(type) &&
         code_generator_abi_classify(type) == ABI_PASS_DIRECT &&
         type->size > 0 && type->size <= 8;
}

int code_generator_binary_resolved_type_is_float64(MtlcType *type) {
  return type && type->kind == MTLC_TYPE_FLOAT64 && type->size == 8;
}

/* IEEE-754 width of a resolved type: 32 for float32, 64 for float64, else 0
 * (not a floating type). */
int code_generator_binary_resolved_type_float_bits(MtlcType *type) {
  if (!type) {
    return 0;
  }
  if (type->kind == MTLC_TYPE_FLOAT32 && type->size == 4) {
    return 32;
  }
  if (type->kind == MTLC_TYPE_FLOAT64 && type->size == 8) {
    return 64;
  }
  return 0;
}

int code_generator_binary_resolved_type_is_abi_supported(MtlcType *type,
                                                                int allow_void) {
  if (!type) {
    return 0;
  }

  if (type->kind == MTLC_TYPE_STRING) {
    return 1;
  }

  /* Aggregates are supported through the ABI classifier: DIRECT aggregates
   * are raw 1/2/4/8-byte register values; INDIRECT aggregates use hidden
   * pointers. */
  if (code_generator_type_is_aggregate(type)) {
    return 1;
  }

  return code_generator_binary_resolved_type_is_supported(type, allow_void);
}

MtlcType *code_generator_binary_get_resolved_type(CodeGenerator *generator,
                                                     const char *type_name,
                                                     int allow_void) {
  const char *resolved_name = NULL;

  if (!generator || !generator->ir_program) {
    return NULL;
  }

  resolved_name = type_name;
  if (!resolved_name || resolved_name[0] == '\0') {
    resolved_name = allow_void ? "void" : "int64";
  }

  return code_generator_named_type(generator, resolved_name);
}

int code_generator_binary_named_type_is_float64(CodeGenerator *generator,
                                                       const char *type_name,
                                                       int allow_void) {
  return code_generator_binary_resolved_type_is_float64(
      code_generator_binary_get_resolved_type(generator, type_name, allow_void));
}

/* Float width (0/32/64) of a named type, e.g. a parameter/local type name. */
int code_generator_binary_named_type_float_bits(CodeGenerator *generator,
                                                       const char *type_name) {
  if (!type_name || type_name[0] == '\0') {
    return 0;
  }
  return code_generator_binary_resolved_type_float_bits(
      code_generator_binary_get_resolved_type(generator, type_name, 0));
}

int code_generator_binary_is_marked_float64_symbol(
    const BinaryFunctionContext *context, const char *name) {
  return context && name &&
         binary_named_slot_table_get_offset(&context->float64_symbols, name) >=
             0;
}

/* The float64_symbols table doubles as a float-width map: the stored slot
 * value is the IEEE-754 width (32 or 64) of the named symbol/temp. Width 0
 * means "not recorded". */
int code_generator_binary_marked_symbol_float_bits(
    const BinaryFunctionContext *context, const char *name) {
  int width = 0;
  if (!context || !name) {
    return 0;
  }
  width = binary_named_slot_table_get_offset(&context->float64_symbols, name);
  return (width == 32 || width == 64) ? width : 0;
}

int code_generator_binary_mark_float_symbol(
    BinaryFunctionContext *context, const char *name, int bits) {
  if (!context || !name || name[0] == '\0') {
    return 0;
  }
  /* binary_named_slot_table_add fails a re-add with a different value, but a
   * symbol/temp may legitimately be visited by more than one marking pass
   * (declared-type pass and instruction-result pass). The first recorded
   * width is authoritative; treat an already-present entry as success
   * instead of aborting code generation. */
  if (binary_named_slot_table_get_offset(&context->float64_symbols, name) >=
      0) {
    return 1;
  }
  return binary_named_slot_table_add(&context->float64_symbols, name,
                                     (bits == 32) ? 32 : 64);
}

int code_generator_binary_mark_float64_symbol(
    BinaryFunctionContext *context, const char *name) {
  return code_generator_binary_mark_float_symbol(context, name, 64);
}

int code_generator_binary_symbol_is_scalar_accessible(
    CodeGenerator *generator, const char *name) {
  const CgSym *symbol = NULL;

  if (!generator || !name || !generator->ir_program) {
    return 1;
  }

  symbol = code_generator_lookup_symbol(generator, name);
  if (!symbol || !symbol->type) {
    return 1;
  }

  /* Indirect parameters: the home slot holds a struct POINTER (8 bytes),
   * which is scalar-accessible even though the symbol's type is aggregate.
   * Downstream consumers use that pointer as the struct's base address. */
  if (symbol->kind == CG_SYM_PARAMETER &&
      symbol->data.variable.is_indirect_param) {
    return 1;
  }

  if (code_generator_binary_type_is_direct_aggregate(symbol->type)) {
    return 1;
  }

  return code_generator_binary_resolved_type_is_stack_scalar(symbol->type);
}

int code_generator_binary_immediate_fits_signed_32(long long value) {
  return value >= INT32_MIN && value <= INT32_MAX;
}

int code_generator_binary_extract_positive_power_of_two(
    long long value, unsigned int *shift_out, unsigned long long *mask_out) {
  unsigned long long uvalue = 0;
  unsigned int shift = 0;

  if (!shift_out || !mask_out || value <= 0) {
    return 0;
  }

  uvalue = (unsigned long long)value;
  if ((uvalue & (uvalue - 1ULL)) != 0ULL) {
    return 0;
  }

  while (uvalue > 1ULL) {
    uvalue >>= 1ULL;
    shift++;
  }

  *shift_out = shift;
  *mask_out = ((unsigned long long)value) - 1ULL;
  return 1;
}

int code_generator_binary_emit_and_mask(BinaryFunctionContext *context,
                                               BinaryGpRegister target_register,
                                               unsigned long long mask) {
  if (!context) {
    return 0;
  }

  if (mask <= 0x7fffffffULL) {
    return binary_emit_and_reg_imm32(&context->code, target_register,
                                     (uint32_t)mask);
  }

  return binary_emit_mov_reg_imm64(&context->code, BINARY_GP_R10, mask) &&
         binary_emit_alu_reg_reg(&context->code, 0x21, target_register,
                                 BINARY_GP_R10);
}


int code_generator_binary_gp_register_is_win64_nonvolatile(
    BinaryGpRegister reg) {
  return reg == BINARY_GP_RBX || reg == BINARY_GP_RSI ||
         reg == BINARY_GP_RDI || reg == BINARY_GP_R12 ||
         reg == BINARY_GP_R13 || reg == BINARY_GP_R14 ||
         reg == BINARY_GP_R15;
}

int code_generator_binary_context_add_saved_register(
    BinaryFunctionContext *context, BinaryGpRegister reg) {
  if (!context) {
    return 0;
  }

  for (size_t i = 0; i < context->saved_register_count; i++) {
    if (context->saved_registers[i] == reg) {
      return 1;
    }
  }

  if (context->saved_register_count >=
      sizeof(context->saved_registers) / sizeof(context->saved_registers[0])) {
    return 0;
  }

  context->saved_registers[context->saved_register_count++] = reg;
  return 1;
}

int code_generator_binary_context_add_saved_xmm_register(
    BinaryFunctionContext *context, BinaryXmmRegister reg) {
  if (!context) {
    return 0;
  }

  for (size_t i = 0; i < context->saved_xmm_count; i++) {
    if (context->saved_xmm_registers[i] == reg) {
      return 1;
    }
  }

  if (context->saved_xmm_count >= sizeof(context->saved_xmm_registers) /
                                      sizeof(context->saved_xmm_registers[0])) {
    return 0;
  }

  context->saved_xmm_registers[context->saved_xmm_count++] = reg;
  return 1;
}

int code_generator_binary_type_is_gp_promotable(MtlcType *type) {
  if (!type || !code_generator_binary_resolved_type_is_supported(type, 0)) {
    return 0;
  }

  if (code_generator_binary_resolved_type_float_bits(type) != 0 ||
      type->kind == MTLC_TYPE_STRING || type->kind == MTLC_TYPE_VOID) {
    return 0;
  }

  return type->size > 0 && type->size <= 8;
}

int code_generator_binary_instruction_writes_dest(IROpcode op) {
  switch (op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_JUMP:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_DECLARE_LOCAL:
    return 0;
  default:
    return 1;
  }
}

size_t code_generator_binary_symbol_write_count(
    const IRFunction *function, const char *name) {
  size_t count = 0;
  if (!function || !name) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction ||
        !code_generator_binary_instruction_writes_dest(instruction->op) ||
        instruction->dest.kind != IR_OPERAND_SYMBOL ||
        !instruction->dest.name) {
      continue;
    }
    if (strcmp(instruction->dest.name, name) == 0) {
      count++;
    }
  }

  return count;
}

int code_generator_binary_collect_symbol_aliases(
    CodeGenerator *generator, BinaryFunctionContext *context,
    IRFunction *ir_function) {
  if (!generator || !context || !ir_function) {
    return 0;
  }

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *instruction = &ir_function->instructions[i];
    const char *name = NULL;
    const char *target = NULL;
    const CgSym *symbol = NULL;
    const CgSym *target_symbol = NULL;
    MtlcType *symbol_type = NULL;
    MtlcType *target_type = NULL;

    if (!instruction || instruction->op != IR_OP_ASSIGN ||
        instruction->dest.kind != IR_OPERAND_SYMBOL ||
        instruction->lhs.kind != IR_OPERAND_SYMBOL ||
        !instruction->dest.name || !instruction->lhs.name) {
      continue;
    }

    name = instruction->dest.name;
    target = instruction->lhs.name;
    if (strcmp(name, target) == 0 ||
        code_generator_binary_get_local_offset(context, name) <= 0 ||
        code_generator_binary_get_symbol_offset(context, target) <= 0 ||
        code_generator_binary_symbol_write_count(ir_function, name) != 1 ||
        /* The alias makes `name` share `target`'s storage, which is only sound
         * if `target` keeps the aliased value for as long as `name` is live.
         * If `target` is written more than once it can be mutated after the
         * `name <- target` copy while `name` is still read later, so the alias
         * would observe the mutated value (silent miscompile). Require `target`
         * to have a single, definitional write. */
        code_generator_binary_symbol_write_count(ir_function, target) != 1 ||
        binary_named_slot_table_get_offset(&context->address_taken_symbols,
                                           name) >= 0 ||
        binary_named_slot_table_get_offset(&context->address_taken_symbols,
                                           target) >= 0 ||
        binary_symbol_alias_table_get(&context->symbol_aliases, target)) {
      continue;
    }

    symbol = generator->ir_program
                 ? code_generator_lookup_symbol(generator, name)
                 : NULL;
    target_symbol = generator->ir_program
                        ? code_generator_lookup_symbol(generator, target)
                        : NULL;
    symbol_type = symbol && symbol->type
                      ? symbol->type
                      : code_generator_binary_get_operand_type_in_context(
                            generator, context, &instruction->dest);
    target_type = target_symbol && target_symbol->type
                      ? target_symbol->type
                      : code_generator_binary_get_operand_type_in_context(
                            generator, context, &instruction->lhs);
    if (!symbol_type || !target_type ||
        !code_generator_binary_type_is_gp_promotable(symbol_type) ||
        !code_generator_binary_type_is_gp_promotable(target_type) ||
        code_generator_binary_resolved_type_scalar_size(symbol_type) !=
            code_generator_binary_resolved_type_scalar_size(target_type) ||
        code_generator_binary_resolved_type_float_bits(symbol_type) !=
            code_generator_binary_resolved_type_float_bits(target_type) ||
        code_generator_binary_resolved_type_is_signed_integer(symbol_type) !=
            code_generator_binary_resolved_type_is_signed_integer(target_type) ||
        code_generator_binary_marked_symbol_float_bits(context, name) ||
        code_generator_binary_marked_symbol_float_bits(context, target) ||
        !code_generator_binary_symbol_is_scalar_accessible(generator, name) ||
        !code_generator_binary_symbol_is_scalar_accessible(generator, target)) {
      continue;
    }

    if (!binary_symbol_alias_table_add(&context->symbol_aliases, name,
                                       target)) {
      code_generator_set_error(
          generator,
          "Failed to record local alias '%s' in direct object function '%s'",
          name, context->function_name);
      return 0;
    }
  }

  return 1;
}

int code_generator_binary_operand_mentions_symbol(
    const IROperand *operand, const char *name) {
  return operand && operand->kind == IR_OPERAND_SYMBOL && operand->name &&
         name && strcmp(operand->name, name) == 0;
}

int code_generator_binary_operand_mentions_symbol_or_alias(
    const BinaryFunctionContext *context, const IROperand *operand,
    const char *name) {
  const char *alias_target = NULL;
  if (code_generator_binary_operand_mentions_symbol(operand, name)) {
    return 1;
  }
  if (!context || !operand || operand->kind != IR_OPERAND_SYMBOL ||
      !operand->name || !name) {
    return 0;
  }
  alias_target =
      binary_symbol_alias_table_get(&context->symbol_aliases, operand->name);
  return alias_target && strcmp(alias_target, name) == 0;
}

int code_generator_binary_instruction_in_backward_loop(
    const IRFunction *function, size_t instruction_index) {
  if (!function || instruction_index >= function->instruction_count) {
    return 0;
  }

  for (size_t jump_index = instruction_index + 1;
       jump_index < function->instruction_count; jump_index++) {
    const IRInstruction *jump = &function->instructions[jump_index];
    if (!jump || jump->op != IR_OP_JUMP || !jump->text) {
      continue;
    }

    for (size_t label_index = 0; label_index <= instruction_index;
         label_index++) {
      const IRInstruction *label = &function->instructions[label_index];
      if (label && label->op == IR_OP_LABEL && label->text &&
          strcmp(label->text, jump->text) == 0) {
        return 1;
      }
    }
  }

  return 0;
}

size_t *code_generator_binary_build_loop_weights(
    const IRFunction *function) {
  if (!function) {
    return NULL;
  }

  size_t count = function->instruction_count;
  size_t *weights = malloc((count ? count : 1) * sizeof(size_t));
  if (!weights) {
    return NULL;
  }

  for (size_t i = 0; i < count; i++) {
    weights[i] = 1;
  }

  /* Weight each instruction by 4^(loop nesting depth) so that values used in
   * inner loops outscore those used only in outer loops. A back-jump to an
   * earlier label marks [label, jump] as one loop body; nested bodies multiply,
   * matching how often the instruction actually executes. Without compounding,
   * a hot innermost temporary (e.g. the insertion-sort scan value) ties with
   * every outer-loop variable and loses the register-promotion contest. */
  for (size_t jump_index = 0; jump_index < count; jump_index++) {
    const IRInstruction *jump = &function->instructions[jump_index];
    if (!jump || jump->op != IR_OP_JUMP || !jump->text) {
      continue;
    }

    for (size_t label_index = 0; label_index < jump_index; label_index++) {
      const IRInstruction *label = &function->instructions[label_index];
      if (!label || label->op != IR_OP_LABEL || !label->text ||
          strcmp(label->text, jump->text) != 0) {
        continue;
      }

      for (size_t i = label_index; i <= jump_index; i++) {
        /* Cap to avoid overflow on pathologically deep nesting; 4^10 already
         * dwarfs any realistic outer-loop score. */
        if (weights[i] <= (size_t)BINARY_LOOP_WEIGHT_CAP) {
          weights[i] *= 4;
        }
      }
      break;
    }
  }

  return weights;
}

size_t code_generator_binary_function_symbol_score(
    const BinaryFunctionContext *context, const IRFunction *function,
    const char *name, const size_t *loop_weights) {
  size_t score = 0;

  if (!function || !name) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    size_t weight = loop_weights ? loop_weights[i] : 1;
    if (!instruction) {
      continue;
    }

    if (code_generator_binary_operand_mentions_symbol_or_alias(
            context, &instruction->dest, name)) {
      score += weight;
    }
    if (code_generator_binary_operand_mentions_symbol_or_alias(
            context, &instruction->lhs, name)) {
      score += weight;
    }
    if (code_generator_binary_operand_mentions_symbol_or_alias(
            context, &instruction->rhs, name)) {
      score += weight;
    }
    for (size_t arg_index = 0; arg_index < instruction->argument_count;
         arg_index++) {
      if (code_generator_binary_operand_mentions_symbol_or_alias(
              context, &instruction->arguments[arg_index], name)) {
        score += weight;
      }
    }
  }

  if (name && strstr(name, "__ptr_") != NULL) {
    score *= 2;
  }

  return score;
}

int code_generator_binary_symbol_already_promoted(
    BinaryFunctionContext *context, const char *name) {
  return context && name &&
         binary_named_slot_table_get_offset(&context->register_symbols, name) >=
             0;
}

int code_generator_binary_symbol_assigned_register(
    CodeGenerator *generator, BinaryFunctionContext *context, const char *name,
    BinaryGpRegister *register_out) {
  const CgSym *symbol = NULL;
  BinaryGpRegister mapped = BINARY_GP_RAX;
  int promoted_register = -1;

  if (!generator || !context || !name || !register_out) {
    return 0;
  }

  if (binary_named_slot_table_get_offset(&context->address_taken_symbols,
                                         name) >= 0) {
    return 0;
  }

  symbol = generator->ir_program
               ? code_generator_lookup_symbol(generator, name)
               : NULL;
  if (code_generator_binary_get_symbol_offset(context, name) <= 0 &&
      !(symbol && symbol->scope && symbol->scope->type == CG_SCOPE_GLOBAL)) {
    return 0;
  }

  if (symbol && symbol->type &&
      (!code_generator_binary_resolved_type_is_supported(symbol->type, 0) ||
       code_generator_binary_resolved_type_float_bits(symbol->type) != 0 ||
       symbol->type->kind == MTLC_TYPE_STRING)) {
    return 0;
  }

  promoted_register =
      binary_named_slot_table_get_offset(&context->register_symbols, name);
  if (promoted_register >= 0) {
    mapped = (BinaryGpRegister)promoted_register;
    if (code_generator_binary_gp_register_is_win64_nonvolatile(mapped)) {
      *register_out = mapped;
      return 1;
    }
  }
  /* A module symbol is never itself register-resident (that was a frontend
   * register-allocator property of locals/params, which codegen no longer
   * consults), so there is no further register to assign here. */
  return 0;
}

int code_generator_binary_function_has_calls(const IRFunction *function) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_CALL || op == IR_OP_CALL_INDIRECT) {
      return 1;
    }
  }
  return 0;
}

int code_generator_binary_function_can_promote_rsi_rdi(
    CodeGenerator *generator, IRFunction *function, MtlcType *return_type) {
  if (!generator || !function) {
    return 0;
  }

  if (code_generator_abi_classify(return_type) == ABI_PASS_INDIRECT) {
    return 0;
  }

  /* RSI/RDI are callee-saved (non-volatile) only under the MS-x64 ABI, where a
   * value promoted into them survives a call because the callee preserves them.
   * Under SysV (Linux/ELF) RSI/RDI are CALLER-saved: any call clobbers them, so
   * a hot local promoted there would be silently destroyed across the call.
   * On SysV, therefore, allow RSI/RDI promotion only when the function makes no
   * calls at all. (The promoter has a separate no-calls fast path; this guard
   * covers the with-calls case.) */
  if (code_generator_binary_active_abi()->counts_classes_separately &&
      code_generator_binary_function_has_calls(function)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!instruction) {
      continue;
    }

    if (instruction->ast_ref || instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_INLINE_ASM || instruction->op == IR_OP_NEW) {
      return 0;
    }

    if (instruction->op != IR_OP_CALL || !instruction->text) {
      continue;
    }

    const CgSym *callee = generator->ir_program
                         ? code_generator_lookup_symbol(generator,
                                               instruction->text)
                         : NULL;
    MtlcType *callee_return = NULL;
    if (callee && callee->kind == CG_SYM_FUNCTION) {
      callee_return = callee->data.function.return_type
                          ? callee->data.function.return_type
                          : callee->type;
    }
    if (code_generator_abi_classify(callee_return) == ABI_PASS_INDIRECT) {
      return 0;
    }

    if (callee && callee->kind == CG_SYM_FUNCTION &&
        callee->data.function.parameter_types) {
      for (size_t arg_i = 0; arg_i < instruction->argument_count &&
                             arg_i < callee->data.function.parameter_count;
           arg_i++) {
        MtlcType *arg_type = callee->data.function.parameter_types[arg_i];
        if (code_generator_abi_classify(arg_type) == ABI_PASS_INDIRECT) {
          return 0;
        }
      }
    }
  }

  return 1;
}

int code_generator_binary_promote_hot_symbols(
    CodeGenerator *generator, BinaryFunctionContext *context,
    IRFunction *ir_function) {
  static const BinaryGpRegister promotion_registers[] = {
      BINARY_GP_R12, BINARY_GP_R13, BINARY_GP_R14, BINARY_GP_R15,
      BINARY_GP_RBX, BINARY_GP_RSI, BINARY_GP_RDI};

  if (!generator || !context || !ir_function) {
    return 0;
  }

  MtlcType *return_type = code_generator_binary_get_resolved_type(
      generator, ir_function->return_type_name, 1);
  size_t max_promoted =
      sizeof(promotion_registers) / sizeof(promotion_registers[0]);
  if (!code_generator_binary_function_can_promote_rsi_rdi(
          generator, ir_function, return_type) &&
      max_promoted >= 2) {
    max_promoted -= 2;
  }
  size_t promoted_count = 0;
  int function_has_no_calls =
      !code_generator_binary_function_has_calls(ir_function);
  size_t *loop_weights =
      code_generator_binary_build_loop_weights(ir_function);
  if (!loop_weights) {
    code_generator_set_error(
        generator,
        "Failed to allocate loop-weight metadata for direct object function "
        "'%s'",
        ir_function->name);
    return 0;
  }

  if (function_has_no_calls) {
    for (size_t insn_i = 0;
         insn_i < ir_function->instruction_count && promoted_count < max_promoted;
         insn_i++) {
      const IRInstruction *insn = &ir_function->instructions[insn_i];
      const IROperand *operands[3];
      size_t op_i = 0;
      int is_pointer_step = 0;

      if (insn->op == IR_OP_BINARY && insn->text &&
          strcmp(insn->text, "+") == 0 && !insn->is_float &&
          insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name &&
          insn->lhs.kind == IR_OPERAND_SYMBOL && insn->lhs.name &&
          strcmp(insn->dest.name, insn->lhs.name) == 0 &&
          insn->rhs.kind == IR_OPERAND_INT &&
          (insn->rhs.int_value == 4 || insn->rhs.int_value == -4 ||
           insn->rhs.int_value == 1 || insn->rhs.int_value == -1)) {
        is_pointer_step = 1;
      }

      if (insn->op != IR_OP_ROTATE_ADD && !is_pointer_step) {
        continue;
      }

      operands[0] = &insn->dest;
      operands[1] = &insn->lhs;
      operands[2] = &insn->rhs;
      for (op_i = 0; op_i < 3 && promoted_count < max_promoted; op_i++) {
        const char *name = operands[op_i]->name;
        MtlcType *type = NULL;
        if (operands[op_i]->kind != IR_OPERAND_SYMBOL || !name ||
            binary_symbol_alias_table_get(&context->symbol_aliases, name) ||
            code_generator_binary_symbol_already_promoted(context, name) ||
            binary_named_slot_table_get_offset(&context->address_taken_symbols,
                                               name) >= 0) {
          continue;
        }

        if (is_pointer_step && op_i == 2) {
          continue;
        }

        type = code_generator_binary_get_resolved_type(
            generator,
            is_pointer_step ? "int32*" : "int64",
            0);
        if (!code_generator_binary_type_is_gp_promotable(type) &&
            !(is_pointer_step && strstr(name, "__ptr_") != NULL)) {
          continue;
        }

        if (!binary_named_slot_table_add(
                &context->register_symbols, name,
                (int)promotion_registers[promoted_count]) ||
            !code_generator_binary_context_add_saved_register(
                context, promotion_registers[promoted_count])) {
          return 0;
        }
        promoted_count++;
      }
    }
  }

  for (size_t reg_index = promoted_count;
       reg_index < max_promoted;
       reg_index++) {
    const char *best_name = NULL;
    size_t best_score = 0;

    for (size_t i = 0; i < ir_function->parameter_count; i++) {
      const char *name = ir_function->parameter_names[i];
      MtlcType *type = NULL;
      if (!name || code_generator_binary_symbol_already_promoted(context, name) ||
          binary_symbol_alias_table_get(&context->symbol_aliases, name) ||
          binary_named_slot_table_get_offset(&context->address_taken_symbols,
                                             name) >= 0) {
        continue;
      }

      type = code_generator_binary_get_resolved_type(
          generator,
          ir_function->parameter_types ? ir_function->parameter_types[i]
                                         : NULL,
          0);
      if (!code_generator_binary_type_is_gp_promotable(type)) {
        continue;
      }

      size_t score =
          code_generator_binary_function_symbol_score(context, ir_function,
                                                      name, loop_weights);
      if (score > best_score) {
        best_score = score;
        best_name = name;
      }
    }

    if (!best_name || best_score < 2) {
      for (size_t i = 0; i < ir_function->instruction_count; i++) {
        const IRInstruction *instruction = &ir_function->instructions[i];
        const char *name = NULL;
        MtlcType *type = NULL;
        if (!instruction || instruction->op != IR_OP_DECLARE_LOCAL ||
            instruction->dest.kind != IR_OPERAND_SYMBOL ||
            !instruction->dest.name) {
          continue;
        }

        name = instruction->dest.name;
        if (code_generator_binary_symbol_already_promoted(context, name) ||
            binary_symbol_alias_table_get(&context->symbol_aliases, name) ||
            binary_named_slot_table_get_offset(&context->address_taken_symbols,
                                               name) >= 0) {
          continue;
        }

        type = code_generator_binary_get_resolved_type(
            generator,
            instruction->text && instruction->text[0] != '\0' ? instruction->text
                                                              : "int64",
            0);
        if (!code_generator_binary_type_is_gp_promotable(type)) {
          continue;
        }

        size_t score =
            code_generator_binary_function_symbol_score(context, ir_function,
                                                        name, loop_weights);
        if (score > best_score) {
          best_score = score;
          best_name = name;
        }
      }
    }

    if (function_has_no_calls) {
      for (size_t i = 0; i < ir_function->instruction_count; i++) {
        const IRInstruction *instruction = &ir_function->instructions[i];
        const IROperand *operands[3];
        if (!instruction) {
          continue;
        }
        operands[0] = &instruction->dest;
        operands[1] = &instruction->lhs;
        operands[2] = &instruction->rhs;
        for (size_t op_i = 0; op_i < 3; op_i++) {
          const char *name = operands[op_i]->name;
          const CgSym *symbol = NULL;
          size_t score = 0;
          if (operands[op_i]->kind != IR_OPERAND_SYMBOL || !name ||
              code_generator_binary_symbol_already_promoted(context, name) ||
              binary_symbol_alias_table_get(&context->symbol_aliases, name) ||
              binary_named_slot_table_get_offset(&context->address_taken_symbols,
                                                 name) >= 0) {
            continue;
          }
          symbol = generator->ir_program
                       ? code_generator_lookup_symbol(generator, name)
                       : NULL;
          if (!symbol || !symbol->scope ||
              symbol->scope->type != CG_SCOPE_GLOBAL || symbol->is_extern ||
              !code_generator_binary_type_is_gp_promotable(symbol->type)) {
            continue;
          }
          score = code_generator_binary_function_symbol_score(
              context, ir_function, name, loop_weights);
          if (score > best_score) {
            best_score = score;
            best_name = name;
          }
        }
        for (size_t arg_i = 0; arg_i < instruction->argument_count; arg_i++) {
          const IROperand *operand = &instruction->arguments[arg_i];
          const char *name = operand->name;
          const CgSym *symbol = NULL;
          size_t score = 0;
          if (operand->kind != IR_OPERAND_SYMBOL || !name ||
              code_generator_binary_symbol_already_promoted(context, name) ||
              binary_symbol_alias_table_get(&context->symbol_aliases, name) ||
              binary_named_slot_table_get_offset(&context->address_taken_symbols,
                                                 name) >= 0) {
            continue;
          }
          symbol = generator->ir_program
                       ? code_generator_lookup_symbol(generator, name)
                       : NULL;
          if (!symbol || !symbol->scope ||
              symbol->scope->type != CG_SCOPE_GLOBAL || symbol->is_extern ||
              !code_generator_binary_type_is_gp_promotable(symbol->type)) {
            continue;
          }
          score = code_generator_binary_function_symbol_score(
              context, ir_function, name, loop_weights);
          if (score > best_score) {
            best_score = score;
            best_name = name;
          }
        }
      }
    }

    if (!best_name || best_score < 2) {
      break;
    }

    if (!binary_named_slot_table_add(&context->register_symbols, best_name,
                                     (int)promotion_registers[reg_index]) ||
        !code_generator_binary_context_add_saved_register(
            context, promotion_registers[reg_index])) {
      code_generator_set_error(
          generator,
          "Failed to promote hot symbol '%s' in direct object function '%s'",
          best_name, ir_function->name);
      free(loop_weights);
      return 0;
    }
  }

  free(loop_weights);
  return 1;
}

int code_generator_binary_resolved_type_is_signed_integer(MtlcType *type) {
  if (!type) {
    return 0;
  }

  switch (type->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_INT64:
    return 1;
  default:
    return 0;
  }
}

int code_generator_binary_resolved_type_scalar_size(MtlcType *type) {
  if (!type) {
    return 8;
  }

  if (type->kind == MTLC_TYPE_POINTER || type->kind == MTLC_TYPE_FUNCTION_POINTER) {
    return 8;
  }

  if (type->size > 0 && type->size <= 8) {
    return (int)type->size;
  }

  return 8;
}

int code_generator_binary_resolved_type_is_supported(MtlcType *type,
                                                            int allow_void) {
  if (!type) {
    return 0;
  }

  switch (type->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_FLOAT32:
  case MTLC_TYPE_FLOAT64:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ENUM:
    return type->size <= 8;
  case MTLC_TYPE_FUNCTION_POINTER:
    return 1;
  case MTLC_TYPE_VOID:
    return allow_void;
  default:
    return 0;
  }
}

int code_generator_binary_type_is_abi_supported(CodeGenerator *generator,
                                                       const char *type_name,
                                                       int allow_void) {
  if (!generator || !generator->ir_program) {
    return 1;
  }

  MtlcType *type =
      code_generator_binary_get_resolved_type(generator, type_name, allow_void);
  if (!type) {
    return 0;
  }

  return code_generator_binary_resolved_type_is_abi_supported(type, allow_void);
}

int code_generator_binary_type_is_cstring(MtlcType *type) {
  if (!type || type->kind != MTLC_TYPE_POINTER) {
    return 0;
  }
  if (type->name && strcmp(type->name, "cstring") == 0) {
    return 1;
  }
  return type->base_type && type->base_type->name &&
         strcmp(type->base_type->name, "uint8") == 0;
}

int code_generator_binary_type_is_string(MtlcType *type) {
  return type && type->kind == MTLC_TYPE_STRING;
}

MtlcType *code_generator_binary_get_operand_type(CodeGenerator *generator,
                                                    const IROperand *operand) {
  const CgSym *symbol = NULL;

  if (!generator || !operand) {
    return NULL;
  }

  switch (operand->kind) {
  case IR_OPERAND_STRING:
    return generator->ir_program ? code_generator_named_type(generator, "string")
                                   : NULL;

  case IR_OPERAND_SYMBOL:
    if (!generator->ir_program || !operand->name) {
      return NULL;
    }
    symbol = code_generator_lookup_symbol(generator, operand->name);
    return symbol ? symbol->type : NULL;

  default:
    return NULL;
  }
}

/* Slot for `name`, appending an empty entry when absent. NULL only on OOM. */
static BinaryOperandTypeEntry *binary_operand_type_slot(
    BinaryOperandTypeIndex *ix, const char *name) {
  size_t b = mettle_fnv1a_hash(name) & (ix->bucket_count - 1);
  while (ix->buckets[b]) {
    BinaryOperandTypeEntry *e = &ix->items[ix->buckets[b] - 1];
    if (strcmp(e->name, name) == 0) {
      return e;
    }
    b = (b + 1) & (ix->bucket_count - 1);
  }
  if (ix->count >= ix->capacity) {
    size_t nc = ix->capacity ? ix->capacity * 2 : 32;
    BinaryOperandTypeEntry *grown = (BinaryOperandTypeEntry *)realloc(
        ix->items, nc * sizeof(BinaryOperandTypeEntry));
    if (!grown) {
      return NULL;
    }
    ix->items = grown;
    ix->capacity = nc;
  }
  memset(&ix->items[ix->count], 0, sizeof(ix->items[ix->count]));
  ix->items[ix->count].name = name;
  ix->count++;
  ix->buckets[b] = ix->count;

  if ((ix->count + 1) * 4 >= ix->bucket_count * 3) {
    size_t nb = ix->bucket_count * 2;
    size_t *fresh = (size_t *)calloc(nb, sizeof(size_t));
    if (!fresh) {
      return NULL;
    }
    for (size_t i = 0; i < ix->count; i++) {
      size_t nbk = mettle_fnv1a_hash(ix->items[i].name) & (nb - 1);
      while (fresh[nbk]) {
        nbk = (nbk + 1) & (nb - 1);
      }
      fresh[nbk] = i + 1;
    }
    free(ix->buckets);
    ix->buckets = fresh;
    ix->bucket_count = nb;
  }
  return &ix->items[ix->count - 1];
}

/* One pass over the function, recording for each operand name the first
 * DECLARE_LOCAL type text and the first baked value_type per operand kind.
 * Leaves `built` set either way: a function whose index cannot be allocated
 * simply resolves nothing here, exactly as an empty function would. */
static void binary_operand_type_index_build(BinaryOperandTypeIndex *ix,
                                            const IRFunction *fn) {
  ix->built = 1;
  ix->bucket_count = 64;
  while (ix->bucket_count < (fn->instruction_count + 1) * 2) {
    ix->bucket_count *= 2;
  }
  ix->buckets = (size_t *)calloc(ix->bucket_count, sizeof(size_t));
  if (!ix->buckets) {
    ix->bucket_count = 0;
    return;
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name && in->text) {
      BinaryOperandTypeEntry *e = binary_operand_type_slot(ix, in->dest.name);
      if (!e) {
        return;
      }
      if (!e->decl_type) {
        e->decl_type = in->text;
      }
    }
    if (in->value_type && in->dest.name &&
        (in->dest.kind == IR_OPERAND_SYMBOL ||
         in->dest.kind == IR_OPERAND_TEMP)) {
      BinaryOperandTypeEntry *e = binary_operand_type_slot(ix, in->dest.name);
      if (!e) {
        return;
      }
      if (in->dest.kind == IR_OPERAND_SYMBOL) {
        if (!e->symbol_type) {
          e->symbol_type = in->value_type;
        }
      } else if (!e->temp_type) {
        e->temp_type = in->value_type;
      }
    }
  }
}

static const BinaryOperandTypeEntry *binary_operand_type_find(
    const BinaryOperandTypeIndex *ix, const char *name) {
  if (!ix->bucket_count) {
    return NULL;
  }
  {
    size_t b = mettle_fnv1a_hash(name) & (ix->bucket_count - 1);
    while (ix->buckets[b]) {
      const BinaryOperandTypeEntry *e = &ix->items[ix->buckets[b] - 1];
      if (strcmp(e->name, name) == 0) {
        return e;
      }
      b = (b + 1) & (ix->bucket_count - 1);
    }
  }
  return NULL;
}

void binary_operand_type_index_destroy(BinaryOperandTypeIndex *ix) {
  free(ix->items);
  free(ix->buckets);
  memset(ix, 0, sizeof(*ix));
}

MtlcType *code_generator_binary_get_operand_type_in_context(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand) {
  MtlcType *type = code_generator_binary_get_operand_type(generator, operand);
  IRFunction *ir_function = NULL;
  const BinaryOperandTypeEntry *entry = NULL;

  if (type || !generator || !operand || !operand->name ||
      (operand->kind != IR_OPERAND_SYMBOL &&
       operand->kind != IR_OPERAND_TEMP)) {
    return type;
  }

  if (context) {
    /* prepare_function_context sets both from the same IRFunction, so the
     * pointer it recorded is the function this name lookup used to perform. */
    ir_function = context->ir_function;
    if (!ir_function && context->function_name) {
      ir_function = code_generator_find_ir_function_binary(
          generator, context->function_name);
    }
  }
  if (!ir_function) {
    return NULL;
  }

  /* Both remaining lookups used to scan every instruction with a strcmp per
   * instruction, once per operand -- quadratic in the function's size. */
  if (context && ir_function == context->ir_function) {
    if (!context->operand_types.built) {
      binary_operand_type_index_build(&context->operand_types, ir_function);
    }
    entry = binary_operand_type_find(&context->operand_types, operand->name);
  }

  if (operand->kind == IR_OPERAND_SYMBOL) {
    /* Parameters carry no IR_OP_DECLARE_LOCAL, and the param symbol is often out
     * of scope in the symbol table by codegen time, so resolve them from the
     * function signature. Without this a uint64/int32/etc. parameter used as a
     * divide/shift/compare operand falls back to "signed", miscompiling unsigned
     * arithmetic at -O0 (where copy-prop hasn't replaced the symbol with a typed
     * temp). */
    for (size_t i = 0; i < ir_function->parameter_count; i++) {
      if (ir_function->parameter_names && ir_function->parameter_names[i] &&
          strcmp(ir_function->parameter_names[i], operand->name) == 0) {
        return code_generator_binary_get_resolved_type(
            generator,
            ir_function->parameter_types ? ir_function->parameter_types[i]
                                         : NULL,
            0);
      }
    }

    if (entry && entry->decl_type) {
      return code_generator_binary_get_resolved_type(generator,
                                                     entry->decl_type, 0);
    }
  }

  /* Builder-API temps (and symbols with no param/local home) carry no
   * DECLARE_LOCAL; their defining instruction bakes the result type into
   * value_type (mtlc_binary/mtlc_cast/mtlc_load). For a temp this is the only
   * type source, so resolving it lets a narrow temp be canonicalized just like
   * a narrow named home -- `(x << 28)` computed into a temp gets sign-extended
   * before a following arithmetic shift reads it. Without this an unsigned or
   * narrow temp operand resolves NULL -> "signed"/unwidened and miscompiles
   * unsigned / % >> and compares in API-built modules. */
  if (entry) {
    MtlcType *baked = operand->kind == IR_OPERAND_SYMBOL ? entry->symbol_type
                                                         : entry->temp_type;
    if (baked) {
      return baked;
    }
  }

  return NULL;
}

int code_generator_binary_validate_signature(CodeGenerator *generator,
                                                    IRFunction *ir_function) {
  if (!generator || !ir_function) {
    return 0;
  }

  MtlcType *return_type = NULL;
  const CgSym *function_symbol =
      generator->ir_program && ir_function->name
          ? code_generator_lookup_symbol(generator, ir_function->name)
          : NULL;
  if (function_symbol && function_symbol->kind == CG_SYM_FUNCTION &&
      function_symbol->data.function.return_type) {
    return_type = function_symbol->data.function.return_type;
  } else {
    return_type = code_generator_binary_get_resolved_type(
        generator, ir_function->return_type_name, 1);
  }

  if (!code_generator_binary_resolved_type_is_abi_supported(return_type, 1)) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports integer/pointer/string/float64 "
        "returns in function '%s'",
        ir_function->name);
    return 0;
  }

  for (size_t i = 0; i < ir_function->parameter_count; i++) {
    const char *type_name = ir_function->parameter_types
                                ? ir_function->parameter_types[i]
                                : NULL;
    if (!code_generator_binary_type_is_abi_supported(generator, type_name, 0)) {
      code_generator_set_error(
          generator,
          "Direct object backend only supports integer/pointer/string/float64 "
          "parameters in function '%s'",
          ir_function->name);
      return 0;
    }
  }

  return 1;
}


int code_generator_binary_prepare_function_context(
    CodeGenerator *generator,
    IRFunction *ir_function, BinaryFunctionContext *context) {
  if (!generator || !ir_function || !context) {
    return 0;
  }

  memset(context, 0, sizeof(*context));
  context->ir_function = ir_function;
  context->function_name = ir_function->name;
  context->return_float_bits = code_generator_binary_resolved_type_float_bits(
      code_generator_binary_get_resolved_type(generator,
                                              ir_function->return_type_name, 1));

  int parameter_home_size = 0;
  if (ir_function->parameter_count >
      (size_t)(INT_MAX / BINARY_FUNCTION_STACK_SLOT_SIZE)) {
    code_generator_set_error(generator,
                             "Too many parameters in function '%s'",
                             ir_function->name);
    return 0;
  }
  parameter_home_size =
      (int)(ir_function->parameter_count * BINARY_FUNCTION_STACK_SLOT_SIZE);

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *instruction = &ir_function->instructions[i];
    if (!instruction || instruction->op != IR_OP_ADDRESS_OF ||
        instruction->lhs.kind != IR_OPERAND_SYMBOL || !instruction->lhs.name) {
      continue;
    }
    if (!binary_named_slot_table_add(&context->address_taken_symbols,
                                     instruction->lhs.name, 1)) {
      code_generator_set_error(
          generator,
          "Failed to record address-taken symbol metadata in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }
  }

  /* Does this function return INDIRECT? The Win64 ABI passes the hidden
   * out-pointer as the first integer argument, consuming home slot 0 and
   * shifting user-parameter homes up by one. */
  MtlcType *fn_return_type =
      ir_function->return_type_name
          ? code_generator_binary_get_resolved_type(
                generator, ir_function->return_type_name, 1)
          : NULL;
  int has_hidden_return =
      (code_generator_abi_classify(fn_return_type) == ABI_PASS_INDIRECT) ? 1 : 0;
  if (has_hidden_return) {
    /* Account for the extra home slot in parameter_home_size so the frame
     * layout includes room for the hidden pointer. */
    if (ir_function->parameter_count >
        (size_t)(INT_MAX / BINARY_FUNCTION_STACK_SLOT_SIZE - 1)) {
      code_generator_set_error(generator,
                               "Too many parameters in function '%s'",
                               ir_function->name);
      return 0;
    }
    parameter_home_size += BINARY_FUNCTION_STACK_SLOT_SIZE;
  }
  context->returns_indirect = has_hidden_return;
  context->indirect_return_size =
      has_hidden_return ? code_generator_abi_type_size(fn_return_type) : 0;

  for (size_t i = 0; i < ir_function->parameter_count; i++) {
    const char *parameter_name = ir_function->parameter_names[i];
    int offset = (int)((i + 1 + (has_hidden_return ? 1 : 0)) *
                       BINARY_FUNCTION_STACK_SLOT_SIZE);
    if (!parameter_name ||
        !binary_named_slot_table_add(&context->parameter_slots, parameter_name,
                                     offset)) {
      code_generator_set_error(
          generator,
          "Failed to allocate parameter slot metadata in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    /* Mark INDIRECT parameters on the symbol so load/lvalue paths know to
     * deref the home slot (which holds a pointer, not the struct itself). */
    {
      MtlcType *param_type =
          ir_function->parameter_types
              ? code_generator_binary_get_resolved_type(
                    generator, ir_function->parameter_types[i], 0)
              : NULL;
      if (code_generator_binary_type_is_cstring(param_type) &&
          !binary_named_slot_table_add(&context->cstring_symbols,
                                       parameter_name, offset)) {
        code_generator_set_error(
            generator,
            "Failed to allocate cstring parameter metadata in function '%s'",
            ir_function->name);
        binary_function_context_destroy(context);
        return 0;
      }
      /* The historical indirect-parameter marking mutated the frontend param
       * symbol here, but at codegen the function scope is already popped, so the
       * lookup never returned a parameter and this never fired -- the indirect
       * struct-parameter ABI is realized from the IR. Removed with the frontend
       * symbol-table dependency; parameter_name is still resolved for its type
       * below. (void) it to keep the loop variable used. */
      (void)parameter_name;
    }

    {
      int param_fbits = code_generator_binary_named_type_float_bits(
          generator, ir_function->parameter_types
                         ? ir_function->parameter_types[i]
                         : NULL);
      if (param_fbits &&
          !code_generator_binary_mark_float_symbol(context, parameter_name,
                                                   param_fbits)) {
        code_generator_set_error(
            generator,
            "Failed to allocate float parameter metadata in function '%s'",
            ir_function->name);
        binary_function_context_destroy(context);
        return 0;
      }
    }
  }

  size_t temp_slot_count = 0;
  size_t local_slot_count = 0;
  int local_storage_size_total = 0;
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *instruction = &ir_function->instructions[i];
    MtlcType *local_type = NULL;
    int local_alignment = 0;
    int local_storage_size = 0;
    int scalar_local = 0;
    int existing_offset = 0;

    if (!instruction || instruction->op != IR_OP_DECLARE_LOCAL) {
      continue;
    }

    if (instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
        instruction->dest.name[0] == '\0') {
      code_generator_set_error(
          generator, "Malformed local declaration in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    local_type = generator->ir_program
                     ? code_generator_named_type(generator,
                                                     instruction->text)
                     : NULL;
    if (!local_type || local_type->kind == MTLC_TYPE_VOID || local_type->size == 0) {
      code_generator_set_error(
          generator,
          "Direct object backend does not support local type '%s' in function "
          "'%s'",
          instruction->text ? instruction->text : "<unknown>",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    scalar_local = code_generator_binary_resolved_type_is_stack_scalar(local_type) ||
                   code_generator_binary_type_is_direct_aggregate(local_type);
    local_alignment = scalar_local ? BINARY_FUNCTION_STACK_SLOT_SIZE
                                   : (int)local_type->alignment;
    if (local_alignment <= 0) {
      local_alignment = 1;
    }

    local_storage_size = scalar_local ? BINARY_FUNCTION_STACK_SLOT_SIZE
                                      : (int)local_type->size;
    if (local_storage_size <= 0) {
      code_generator_set_error(generator,
                               "Invalid local storage size in function '%s'",
                               ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    existing_offset =
        binary_named_slot_table_get_offset(&context->local_slots,
                                           instruction->dest.name);
    if (existing_offset > 0) {
      int local_fbits =
          code_generator_binary_resolved_type_float_bits(local_type);
      if (local_fbits &&
          !code_generator_binary_mark_float_symbol(context,
                                                   instruction->dest.name,
                                                   local_fbits)) {
        code_generator_set_error(
            generator,
            "Failed to allocate float local metadata in function '%s'",
            ir_function->name);
        binary_function_context_destroy(context);
        return 0;
      }
      continue;
    }

    if (!binary_align_up_int(local_storage_size_total, local_alignment,
                             &local_storage_size_total) ||
        local_storage_size_total > INT_MAX - local_storage_size) {
      code_generator_set_error(generator, "Stack frame too large in function '%s'",
                               ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    local_storage_size_total += local_storage_size;
    local_slot_count++;
    if (local_slot_count > (size_t)(INT_MAX / BINARY_FUNCTION_STACK_SLOT_SIZE)) {
      code_generator_set_error(generator, "Too many locals in function '%s'",
                               ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    if (!binary_named_slot_table_add(
            &context->local_slots, instruction->dest.name,
            parameter_home_size + local_storage_size_total)) {
      code_generator_set_error(
          generator,
          "Failed to allocate local slot metadata in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    if (code_generator_binary_type_is_cstring(local_type) &&
        !binary_named_slot_table_add(&context->cstring_symbols,
                                     instruction->dest.name,
                                     parameter_home_size +
                                         local_storage_size_total)) {
      code_generator_set_error(
          generator,
          "Failed to allocate cstring local metadata in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    if (local_type->kind == MTLC_TYPE_STRING &&
        !binary_named_slot_table_add(&context->string_symbols,
                                     instruction->dest.name,
                                     parameter_home_size +
                                         local_storage_size_total)) {
      code_generator_set_error(
          generator,
          "Failed to allocate string local metadata in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    {
      int local_fbits =
          code_generator_binary_resolved_type_float_bits(local_type);
      if (local_fbits &&
          !code_generator_binary_mark_float_symbol(context,
                                                   instruction->dest.name,
                                                   local_fbits)) {
        code_generator_set_error(
            generator,
            "Failed to allocate float local metadata in function '%s'",
            ir_function->name);
        binary_function_context_destroy(context);
        return 0;
      }
    }
  }

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *instruction = &ir_function->instructions[i];
    if (!instruction || instruction->dest.kind != IR_OPERAND_TEMP ||
        !instruction->dest.name) {
      continue;
    }

    if (binary_named_slot_table_get_offset(&context->temp_slots,
                                           instruction->dest.name) >= 0) {
      continue;
    }

    temp_slot_count++;
    if (temp_slot_count > (size_t)(INT_MAX / BINARY_FUNCTION_STACK_SLOT_SIZE)) {
      code_generator_set_error(
          generator, "Too many temporaries in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }

    int offset =
        parameter_home_size + local_storage_size_total +
        (int)(temp_slot_count * BINARY_FUNCTION_STACK_SLOT_SIZE);
    if (!binary_named_slot_table_add(&context->temp_slots,
                                     instruction->dest.name, offset)) {
      code_generator_set_error(
          generator, "Failed to allocate temp slot metadata in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }
  }

  /* Pre-mark referenced global float symbols with their DECLARED float width,
   * symmetric with the parameter/local declared-type passes above. The mark map
   * is first-wins, and a written float global (`@g <- t`, where t is a float64
   * temp from a double-precision expression) would otherwise be recorded by the
   * instruction-result pass below at the temp's width (64), mislabeling a
   * float32 global. Globals are not declared by a DECLARE_LOCAL, so without this
   * they have no authoritative declared-width mark. */
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *instruction = &ir_function->instructions[i];
    if (!instruction) {
      continue;
    }
    for (int k = 0;; k++) {
      const IROperand *op;
      if (k == 0) {
        op = &instruction->dest;
      } else if (k == 1) {
        op = &instruction->lhs;
      } else if (k == 2) {
        op = &instruction->rhs;
      } else if ((size_t)(k - 3) < instruction->argument_count) {
        op = &instruction->arguments[k - 3];
      } else {
        break;
      }
      if (op->kind != IR_OPERAND_SYMBOL || !op->name || op->name[0] == '\0' ||
          !generator->ir_program) {
        continue;
      }
      const CgSym *sym = code_generator_lookup_symbol(generator, op->name);
      if (!sym || !sym->scope || sym->scope->type != CG_SCOPE_GLOBAL) {
        continue;
      }
      int gfbits = code_generator_binary_resolved_type_float_bits(sym->type);
      if (gfbits &&
          !code_generator_binary_mark_float_symbol(context, op->name, gfbits)) {
        code_generator_set_error(
            generator, "Failed to allocate float global metadata in function "
                       "'%s'",
            ir_function->name);
        binary_function_context_destroy(context);
        return 0;
      }
    }
  }

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *instruction = &ir_function->instructions[i];
    if (!instruction || !instruction->dest.name ||
        (instruction->dest.kind != IR_OPERAND_SYMBOL &&
         instruction->dest.kind != IR_OPERAND_TEMP)) {
      continue;
    }

    {
      int result_fbits = code_generator_binary_instruction_result_float_bits(
          generator, context, instruction);
      if (!result_fbits) {
        continue;
      }
      if (!code_generator_binary_mark_float_symbol(
              context, instruction->dest.name, result_fbits)) {
        code_generator_set_error(
            generator,
            "Failed to allocate float temporary metadata in function '%s'",
            ir_function->name);
        binary_function_context_destroy(context);
        return 0;
      }
    }
  }

  if (!code_generator_binary_collect_symbol_aliases(generator, context,
                                                    ir_function)) {
    binary_function_context_destroy(context);
    return 0;
  }

  if (!code_generator_binary_promote_hot_symbols(generator, context,
                                                 ir_function)) {
    binary_function_context_destroy(context);
    return 0;
  }

  for (size_t i = 0; i < ir_function->parameter_count; i++) {
    BinaryGpRegister assigned_register = BINARY_GP_RAX;
    const char *parameter_name = ir_function->parameter_names[i];
    if (code_generator_binary_symbol_assigned_register(
            generator, context, parameter_name, &assigned_register) &&
        !code_generator_binary_context_add_saved_register(context,
                                                          assigned_register)) {
      code_generator_set_error(
          generator,
          "Too many callee-saved register-backed symbols in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }
  }

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *instruction = &ir_function->instructions[i];
    BinaryGpRegister assigned_register = BINARY_GP_RAX;
    if (!instruction || instruction->op != IR_OP_DECLARE_LOCAL ||
        instruction->dest.kind != IR_OPERAND_SYMBOL ||
        !instruction->dest.name) {
      continue;
    }
    if (code_generator_binary_symbol_assigned_register(
            generator, context, instruction->dest.name, &assigned_register) &&
        !code_generator_binary_context_add_saved_register(context,
                                                          assigned_register)) {
      code_generator_set_error(
          generator,
          "Too many callee-saved register-backed symbols in function '%s'",
          ir_function->name);
      binary_function_context_destroy(context);
      return 0;
    }
  }

  int local_home_size = local_storage_size_total;
  if (!binary_align_up_int(local_home_size, BINARY_FUNCTION_STACK_SLOT_SIZE,
                           &local_home_size)) {
    code_generator_set_error(generator,
                             "Stack frame too large in function '%s'",
                             ir_function->name);
    binary_function_context_destroy(context);
    return 0;
  }
  int temp_home_size = (int)(temp_slot_count * BINARY_FUNCTION_STACK_SLOT_SIZE);
  if (parameter_home_size > INT_MAX - local_home_size ||
      parameter_home_size + local_home_size > INT_MAX - temp_home_size) {
    code_generator_set_error(generator,
                             "Stack frame too large in function '%s'",
                             ir_function->name);
    binary_function_context_destroy(context);
    return 0;
  }

  /* Reserve a function-level slot for each IR_OP_CALL whose return type is
   * INDIRECT. Each slot's rbp offset goes into context->indirect_return_slot_offsets
   * in instruction order and is consumed by emit_call. */
  int indirect_return_total = 0;
  for (size_t pp_i = 0; pp_i < ir_function->instruction_count; pp_i++) {
    const IRInstruction *pp_insn = &ir_function->instructions[pp_i];
    if (pp_insn->op != IR_OP_CALL || !pp_insn->text) continue;
    const CgSym *callee =
        code_generator_lookup_symbol(generator, pp_insn->text);
    MtlcType *ret_t = NULL;
    if (callee && callee->kind == CG_SYM_FUNCTION) {
      ret_t = callee->data.function.return_type
                  ? callee->data.function.return_type
                  : callee->type;
    }
    if (code_generator_abi_classify(ret_t) != ABI_PASS_INDIRECT) continue;
    size_t sz = code_generator_abi_type_size(ret_t);
    int slot_bytes = (int)((sz + 15u) & ~(size_t)15);
    int slot_base_offset =
        parameter_home_size + local_home_size + temp_home_size +
        indirect_return_total + slot_bytes;
    if (context->indirect_return_slot_count >=
        context->indirect_return_slot_capacity) {
      size_t new_cap = context->indirect_return_slot_capacity
                           ? context->indirect_return_slot_capacity * 2
                           : 8;
      int *grown = realloc(context->indirect_return_slot_offsets,
                           new_cap * sizeof(int));
      if (!grown) {
        code_generator_set_error(generator,
                                 "Out of memory recording indirect-return "
                                 "slot in function '%s'",
                                 ir_function->name);
        binary_function_context_destroy(context);
        return 0;
      }
      context->indirect_return_slot_offsets = grown;
      context->indirect_return_slot_capacity = new_cap;
    }
    context->indirect_return_slot_offsets[context->indirect_return_slot_count++] =
        slot_base_offset;
    indirect_return_total += slot_bytes;
  }

  int saved_register_home_size =
      (int)(context->saved_register_count * BINARY_FUNCTION_STACK_SLOT_SIZE);
  for (size_t i = 0; i < context->saved_register_count; i++) {
    context->saved_register_offsets[i] =
        parameter_home_size + local_home_size + temp_home_size +
        indirect_return_total +
        (int)((i + 1) * BINARY_FUNCTION_STACK_SLOT_SIZE);
  }

  context->raw_frame_size = parameter_home_size + local_home_size +
                            temp_home_size + indirect_return_total +
                            saved_register_home_size;
  if (!binary_align_up_int(context->raw_frame_size, 16, &context->frame_size)) {
    code_generator_set_error(generator,
                             "Stack frame too large in function '%s'",
                             ir_function->name);
    binary_function_context_destroy(context);
    return 0;
  }

  return 1;
}

int code_generator_binary_instruction_compare_width(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  MtlcType *type = NULL;

  if (!generator || !instruction) {
    return 8;
  }

  /* Result type baked onto the IR at lowering (was: inferred from ast_ref). */
  type = instruction->value_type;
  if (!type) {
    MtlcType *lhs_type = code_generator_binary_get_operand_type_in_context(
        generator, context, &instruction->lhs);
    MtlcType *rhs_type = code_generator_binary_get_operand_type_in_context(
        generator, context, &instruction->rhs);
    if (lhs_type && rhs_type) {
      int lhs_size = code_generator_binary_resolved_type_scalar_size(lhs_type);
      int rhs_size = code_generator_binary_resolved_type_scalar_size(rhs_type);
      if (lhs_size == rhs_size && lhs_size == 4) {
        return 4;
      }
    }
  }
  if (type && !instruction->is_float) {
    int size = code_generator_binary_resolved_type_scalar_size(type);
    if (size == 4) {
      return 4;
    }
  }

  return 8;
}

int code_generator_binary_emit_reg_reg_compare(
    BinaryCodeBuffer *buffer, BinaryGpRegister lhs, BinaryGpRegister rhs,
    int width) {
  if (!buffer) {
    return 0;
  }
  if (width == 4) {
    return binary_emit_cmp_reg_reg32(buffer, lhs, rhs);
  }
  return binary_emit_cmp_reg_reg(buffer, lhs, rhs);
}

int code_generator_binary_emit_reg_reg_move(
    BinaryCodeBuffer *buffer, BinaryGpRegister destination,
    BinaryGpRegister source, MtlcType *type) {
  int width = 8;
  int is_signed = 0;
  int is_integer = 0;

  if (!buffer) {
    return 0;
  }
  if (type) {
    width = code_generator_binary_resolved_type_scalar_size(type);
    is_signed = code_generator_binary_resolved_type_is_signed_integer(type);
    is_integer = !code_generator_type_is_aggregate(type) &&
                 code_generator_binary_resolved_type_float_bits(type) == 0;
  }

  if (width == 4) {
    if (is_signed) {
      return binary_emit_movsxd_reg_reg32(buffer, destination, source);
    }
    return binary_emit_movzx_reg_reg32(buffer, destination, source);
  }
  /* Sub-4-byte integers extend so the destination holds the canonical wrapped
   * value for its signedness, matching stack homes and the MIR backend. */
  if (is_integer && width == 2) {
    return is_signed ? binary_emit_movsx_reg_reg16(buffer, destination, source)
                     : binary_emit_movzx_reg_reg16(buffer, destination, source);
  }
  if (is_integer && width == 1) {
    return is_signed ? binary_emit_movsx_reg_reg8(buffer, destination, source)
                     : binary_emit_movzx_reg_reg8(buffer, destination, source);
  }
  if (destination == source) {
    return 1;
  }
  return binary_emit_mov_reg_reg(buffer, destination, source);
}

int code_generator_binary_emit_temp_stack_load(
    CodeGenerator *generator, BinaryFunctionContext *context, int stack_offset,
    BinaryGpRegister target_register, MtlcType *type) {
  int width = code_generator_binary_type_scalar_width(type);
  int is_signed = type
                      ? code_generator_binary_resolved_type_is_signed_integer(type)
                      : 0;

  if (!generator || !context || stack_offset <= 0) {
    return 0;
  }
  if (width == 4) {
    if (!binary_emit_mov_reg_mem32(&context->code, target_register,
                                   BINARY_GP_RBP, -stack_offset)) {
      return 0;
    }
    if (is_signed) {
      return binary_emit_movsxd_reg_reg32(&context->code, target_register,
                                          target_register);
    }
    return 1;
  }
  return binary_emit_mov_reg_mem(&context->code, target_register,
                                 BINARY_GP_RBP, -stack_offset);
}

int code_generator_binary_emit_temp_stack_store(
    CodeGenerator *generator, BinaryFunctionContext *context, int stack_offset,
    BinaryGpRegister source_register, MtlcType *type) {
  int width = code_generator_binary_type_scalar_width(type);

  if (!generator || !context || stack_offset <= 0) {
    return 0;
  }
  if (width == 4) {
    return binary_emit_mov_mem_reg32(&context->code, BINARY_GP_RBP, -stack_offset,
                                     source_register);
  }
  return binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP, -stack_offset,
                                 source_register);
}

int code_generator_binary_emit_symbol_stack_load(
    CodeGenerator *generator, BinaryFunctionContext *context, MtlcType *type,
    int stack_offset, BinaryGpRegister target_register) {
  int size = 8;
  int is_signed = 0;

  if (!generator || !context || stack_offset <= 0) {
    return 0;
  }

  if (type) {
    size = code_generator_binary_resolved_type_scalar_size(type);
    is_signed = code_generator_binary_resolved_type_is_signed_integer(type);
  }

  switch (size) {
  case 1:
    if (!binary_emit_movzx_reg_mem8(&context->code, target_register,
                                    BINARY_GP_RBP, -stack_offset)) {
      return 0;
    }
    if (is_signed &&
        !binary_emit_movsx_reg_reg8(&context->code, target_register,
                                    target_register)) {
      return 0;
    }
    return 1;
  case 2:
    if (!binary_emit_movzx_reg_mem16(&context->code, target_register,
                                     BINARY_GP_RBP, -stack_offset)) {
      return 0;
    }
    if (is_signed &&
        !binary_emit_movsx_reg_reg16(&context->code, target_register,
                                     target_register)) {
      return 0;
    }
    return 1;
  case 4:
    if (!binary_emit_mov_reg_mem32(&context->code, target_register,
                                   BINARY_GP_RBP, -stack_offset)) {
      return 0;
    }
    if (is_signed &&
        !binary_emit_movsxd_reg_reg32(&context->code, target_register,
                                      target_register)) {
      return 0;
    }
    return 1;
  default:
    return binary_emit_mov_reg_mem(&context->code, target_register,
                                   BINARY_GP_RBP, -stack_offset);
  }
}

int code_generator_binary_emit_symbol_stack_store(
    CodeGenerator *generator, BinaryFunctionContext *context, MtlcType *type,
    int stack_offset, BinaryGpRegister source_register) {
  int size = 8;

  if (!generator || !context || stack_offset <= 0) {
    return 0;
  }

  if (type) {
    size = code_generator_binary_resolved_type_scalar_size(type);
  }

  switch (size) {
  case 1:
    return binary_emit_mov_mem_reg8(&context->code, BINARY_GP_RBP, -stack_offset,
                                    source_register);
  case 2:
    return binary_emit_mov_mem_reg16(&context->code, BINARY_GP_RBP, -stack_offset,
                                     source_register);
  case 4:
    return binary_emit_mov_mem_reg32(&context->code, BINARY_GP_RBP, -stack_offset,
                                     source_register);
  default:
    return binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP, -stack_offset,
                                   source_register);
  }
}

int code_generator_binary_symbol_move_width(const CgSym *symbol) {
  if (!symbol || !symbol->type) {
    return 8;
  }
  return code_generator_binary_resolved_type_scalar_size(symbol->type);
}

int code_generator_binary_type_scalar_width(MtlcType *type) {
  if (!type) {
    return 8;
  }
  return code_generator_binary_resolved_type_scalar_size(type);
}
