#include "codegen/binary/internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int binary_emit_mov_reg_from_saved_string_source(
    BinaryCodeBuffer *code, BinaryGpRegister destination,
    BinaryGpRegister source) {
  if (source == BINARY_GP_RDI) {
    return binary_emit_mov_reg_mem(code, destination, BINARY_GP_RSP, 0);
  }
  if (source == BINARY_GP_RSI) {
    return binary_emit_mov_reg_mem(code, destination, BINARY_GP_RSP, 8);
  }
  return binary_emit_mov_reg_reg(code, destination, source);
}

#define BINARY_HEAP_ZERO_MEMORY 8u

int code_generator_binary_declare_external_symbol(
    CodeGenerator *generator, const char *symbol_name) {
  BinaryEmitter *emitter = NULL;

  if (!generator || !symbol_name || symbol_name[0] == '\0') {
    return 0;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }

  if (!binary_emitter_declare_external(emitter, symbol_name)) {
    code_generator_binary_emitter_error(
        generator, emitter, "Failed to declare external symbol");
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_symbol_address(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const char *symbol_name, int declare_external,
    BinaryGpRegister target_register) {
  size_t displacement_offset = 0;

  if (!generator || !context || !symbol_name || symbol_name[0] == '\0') {
    return 0;
  }

  if (declare_external &&
      !code_generator_binary_declare_external_symbol(generator, symbol_name)) {
    return 0;
  }

  if (!binary_emit_lea_reg_rip_placeholder(&context->code, target_register,
                                           &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, symbol_name,
                                        displacement_offset)) {
    code_generator_set_error(generator,
                             "Out of memory while emitting symbol reference");
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_cstring_literal_address(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const char *value, BinaryGpRegister target_register) {
  BinaryEmitter *emitter = NULL;
  size_t rdata_section = 0;
  size_t literal_offset = 0;
  size_t length = 0;
  unsigned char terminator = 0;
  char *label = NULL;
  int success = 0;

  if (!generator || !context || !value) {
    return 0;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }

  label = code_generator_generate_label(generator, "str_chars");
  if (!label) {
    code_generator_set_error(generator,
                             "Out of memory while creating string label");
    return 0;
  }

  rdata_section = binary_emitter_get_or_create_section(
      emitter, ".rdata", BINARY_SECTION_RDATA, 0, 1);
  if (rdata_section == (size_t)-1) {
    code_generator_binary_emitter_error(
        generator, emitter, "Failed to create .rdata section");
    goto cleanup;
  }

  length = strlen(value);
  if (!binary_emitter_append_bytes(emitter, rdata_section, value, length,
                                   &literal_offset) ||
      !binary_emitter_append_bytes(emitter, rdata_section, &terminator, 1,
                                   NULL) ||
      !binary_emitter_define_symbol(emitter, label, BINARY_SYMBOL_LOCAL,
                                    rdata_section, literal_offset,
                                    length + 1) ||
      !code_generator_binary_emit_symbol_address(generator, context, label, 0,
                                                 target_register)) {
    if (!generator->has_error) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to emit string literal");
    }
    goto cleanup;
  }

  success = 1;

cleanup:
  free(label);
  return success;
}

int code_generator_binary_emit_string_literal_value_address(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const char *value, size_t value_length,
    BinaryGpRegister target_register) {
  BinaryEmitter *emitter = NULL;
  BinarySection *section = NULL;
  size_t rdata_section = 0;
  size_t chars_offset = 0;
  size_t struct_offset = 0;
  size_t length = 0;
  unsigned char terminator = 0;
  uint64_t string_length = 0;
  char *chars_label = NULL;
  char *struct_label = NULL;
  int success = 0;

  if (!generator || !context || !value) {
    return 0;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }

  chars_label = code_generator_generate_label(generator, "str_chars");
  struct_label = code_generator_generate_label(generator, "str_struct");
  if (!chars_label || !struct_label) {
    code_generator_set_error(generator,
                             "Out of memory while creating string labels");
    goto cleanup;
  }

  {
    const char *rdata_name = ".rdata";
    char granular_name[128];
    uint32_t rdata_characteristics = 0;

    if (emitter->target_format == BINARY_TARGET_FORMAT_COFF_WIN64 &&
        emitter->section_count < 60000u &&
        strlen(struct_label) + 8u <= sizeof(granular_name)) {
      snprintf(granular_name, sizeof(granular_name), ".rdata$%s", struct_label);
      rdata_name = granular_name;
      rdata_characteristics = 0x40000040u | 0x00400000u;
    }
    rdata_section = binary_emitter_get_or_create_section(
        emitter, rdata_name, BINARY_SECTION_RDATA, rdata_characteristics, 8);
  }
  if (rdata_section == (size_t)-1) {
    code_generator_binary_emitter_error(
        generator, emitter, "Failed to create .rdata section");
    goto cleanup;
  }

  length = value_length;
  string_length = (uint64_t)length;
  if (!binary_emitter_append_bytes(emitter, rdata_section, value, length,
                                   &chars_offset) ||
      !binary_emitter_append_bytes(emitter, rdata_section, &terminator, 1,
                                   NULL) ||
      !binary_emitter_define_symbol(emitter, chars_label, BINARY_SYMBOL_LOCAL,
                                    rdata_section, chars_offset, length + 1) ||
      !binary_emitter_align_section(emitter, rdata_section, 8, 0) ||
      !binary_emitter_append_zeros(emitter, rdata_section, 16, &struct_offset)) {
    if (!generator->has_error) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to emit string literal");
    }
    goto cleanup;
  }

  section = binary_emitter_get_section(emitter, rdata_section);
  if (!section || !section->data || struct_offset + 16 > section->size) {
    code_generator_set_error(generator,
                             "Failed to access emitted string literal storage");
    goto cleanup;
  }

  memcpy(section->data + struct_offset + 8, &string_length,
         sizeof(string_length));
  if (!binary_emitter_define_symbol(emitter, struct_label, BINARY_SYMBOL_LOCAL,
                                    rdata_section, struct_offset, 16) ||
      !binary_emitter_add_relocation(emitter, rdata_section, struct_offset,
                                     BINARY_RELOCATION_ADDR64, chars_label, 0) ||
      !code_generator_binary_emit_symbol_address(generator, context, struct_label,
                                                 0, target_register)) {
    if (!generator->has_error) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to emit string literal");
    }
    goto cleanup;
  }

  success = 1;

cleanup:
  free(chars_label);
  free(struct_label);
  return success;
}

int code_generator_binary_emit_global_string_variable(
    CodeGenerator *generator, const char *link_name, const char *value,
    size_t value_length) {
  BinaryEmitter *emitter = NULL;
  BinarySection *section = NULL;
  size_t data_section = 0;
  size_t rdata_section = 0;
  size_t chars_offset = 0;
  size_t struct_offset = 0;
  size_t length = 0;
  uint64_t string_length = 0;
  unsigned char terminator = 0;
  char *chars_label = NULL;

  if (!generator || !link_name || link_name[0] == '\0') {
    return 0;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }

  data_section = binary_emitter_get_or_create_section(emitter, ".data",
                                                      BINARY_SECTION_DATA, 0, 8);
  if (data_section == (size_t)-1) {
    code_generator_binary_emitter_error(
        generator, emitter, "Failed to create .data section");
    return 0;
  }

  if (value) {
    chars_label = code_generator_generate_label(generator, "str_chars");
    if (!chars_label) {
      code_generator_set_error(generator,
                               "Out of memory while creating string labels");
      return 0;
    }

    rdata_section = binary_emitter_get_or_create_section(
        emitter, ".rdata", BINARY_SECTION_RDATA, 0, 8);
    if (rdata_section == (size_t)-1) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to create .rdata section");
      free(chars_label);
      return 0;
    }

    length = value_length;
    string_length = (uint64_t)length;
    if (!binary_emitter_append_bytes(emitter, rdata_section, value, length,
                                     &chars_offset) ||
        !binary_emitter_append_bytes(emitter, rdata_section, &terminator, 1,
                                     NULL) ||
        !binary_emitter_define_symbol(emitter, chars_label, BINARY_SYMBOL_LOCAL,
                                      rdata_section, chars_offset,
                                      length + 1)) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to emit global string characters");
      free(chars_label);
      return 0;
    }
  }

  if (!binary_emitter_align_section(emitter, data_section, 8, 0) ||
      !binary_emitter_append_zeros(emitter, data_section, 16, &struct_offset)) {
    code_generator_binary_emitter_error(
        generator, emitter, "Failed to reserve global string storage");
    free(chars_label);
    return 0;
  }

  section = binary_emitter_get_section(emitter, data_section);
  if (!section || !section->data || struct_offset + 16 > section->size) {
    code_generator_set_error(generator,
                             "Failed to access emitted global string storage");
    free(chars_label);
    return 0;
  }

  if (value) {
    memcpy(section->data + struct_offset + 8, &string_length,
           sizeof(string_length));
    if (!binary_emitter_add_relocation(emitter, data_section, struct_offset,
                                       BINARY_RELOCATION_ADDR64, chars_label,
                                       0)) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to emit global string relocation");
      free(chars_label);
      return 0;
    }
  }

  if (!binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                    data_section, struct_offset, 16)) {
    code_generator_binary_emitter_error(
        generator, emitter, "Failed to define global string symbol");
    free(chars_label);
    return 0;
  }

  free(chars_label);
  return 1;
}

int code_generator_binary_get_access_size(CodeGenerator *generator,
                                                 BinaryFunctionContext *context,
                                                 const IROperand *size_operand) {
  if (!generator || !context || !size_operand || size_operand->kind != IR_OPERAND_INT) {
    code_generator_set_error(generator,
                             "IR memory access width must be integer in "
                             "function '%s'",
                             context ? context->function_name : "<unknown>");
    return 0;
  }

  if (size_operand->int_value <= 0) {
    code_generator_set_error(generator,
                             "Invalid IR memory access width %lld in function "
                             "'%s'",
                             size_operand->int_value, context->function_name);
    return 0;
  }

  return (int)size_operand->int_value;
}

int code_generator_binary_emit_load_from_address(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister address_register, int size, BinaryGpRegister target_register) {
  if (!generator || !context) {
    return 0;
  }

  switch (size) {
  case 1:
    return binary_emit_movzx_reg_mem8(&context->code, target_register,
                                      address_register, 0);
  case 2:
    return binary_emit_movzx_reg_mem16(&context->code, target_register,
                                       address_register, 0);
  case 4:
    return binary_emit_mov_reg_mem32(&context->code, target_register,
                                     address_register, 0);
  case 8:
    return binary_emit_mov_reg_mem(&context->code, target_register,
                                   address_register, 0);
  default:
    code_generator_set_error(
        generator,
        "Direct object backend does not yet support memory loads wider than "
        "8 bytes in function '%s'",
        context->function_name);
    return 0;
  }
}

int code_generator_binary_emit_store_to_address(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister address_register, int size, BinaryGpRegister source_register) {
  if (!generator || !context) {
    return 0;
  }

  switch (size) {
  case 1:
    return binary_emit_mov_mem_reg8(&context->code, address_register, 0,
                                    source_register);
  case 2:
    return binary_emit_mov_mem_reg16(&context->code, address_register, 0,
                                     source_register);
  case 4:
    return binary_emit_mov_mem_reg32(&context->code, address_register, 0,
                                     source_register);
  case 8:
    return binary_emit_mov_mem_reg(&context->code, address_register, 0,
                                   source_register);
  default: {
    uint64_t n = (uint64_t)size;
    if (n != (uint64_t)size || n == 0) {
      code_generator_set_error(
          generator,
          "Invalid aggregate store size %d in function '%s'",
          size, context->function_name);
      return 0;
    }
    return binary_emit_push_reg(&context->code, BINARY_GP_RCX) &&
           binary_emit_push_reg(&context->code, BINARY_GP_RSI) &&
           binary_emit_push_reg(&context->code, BINARY_GP_RDI) &&
           binary_emit_mov_reg_from_saved_string_source(
               &context->code, BINARY_GP_RSI, source_register) &&
           binary_emit_mov_reg_from_saved_string_source(
               &context->code, BINARY_GP_RDI, address_register) &&
           binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RCX, n) &&
           binary_code_buffer_append_u8(&context->code, 0xF3) &&
           binary_code_buffer_append_u8(&context->code, 0xA4) &&
           binary_emit_pop_reg(&context->code, BINARY_GP_RDI) &&
           binary_emit_pop_reg(&context->code, BINARY_GP_RSI) &&
           binary_emit_pop_reg(&context->code, BINARY_GP_RCX);
  }
  }
}

int code_generator_binary_emit_rep_movsb(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister src_addr_reg, BinaryGpRegister dst_addr_reg, size_t size) {
  if (!generator || !context || size == 0) {
    return 0;
  }
  if (!binary_emit_push_reg(&context->code, BINARY_GP_RSI) ||
      !binary_emit_push_reg(&context->code, BINARY_GP_RDI) ||
      !binary_emit_mov_reg_from_saved_string_source(
          &context->code, BINARY_GP_RSI, src_addr_reg) ||
      !binary_emit_mov_reg_from_saved_string_source(
          &context->code, BINARY_GP_RDI, dst_addr_reg) ||
      !binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RCX,
                                 (uint64_t)size) ||
      !binary_code_buffer_append_u8(&context->code, 0xFC) ||
      !binary_code_buffer_append_u8(&context->code, 0xF3) ||
      !binary_code_buffer_append_u8(&context->code, 0xA4) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RDI) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RSI)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_rep_movsq(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister src_addr_reg, BinaryGpRegister dst_addr_reg,
    size_t qword_count) {
  if (!generator || !context || qword_count == 0) {
    return 0;
  }
  if (!binary_emit_push_reg(&context->code, BINARY_GP_RSI) ||
      !binary_emit_push_reg(&context->code, BINARY_GP_RDI) ||
      !binary_emit_mov_reg_from_saved_string_source(
          &context->code, BINARY_GP_RSI, src_addr_reg) ||
      !binary_emit_mov_reg_from_saved_string_source(
          &context->code, BINARY_GP_RDI, dst_addr_reg) ||
      !binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RCX,
                                 (uint64_t)qword_count) ||
      !binary_code_buffer_append_u8(&context->code, 0xFC) ||
      !binary_code_buffer_append_u8(&context->code, 0xF3) ||
      !binary_emit_rex(&context->code, 1, 0, 0, 0) ||
      !binary_code_buffer_append_u8(&context->code, 0xA5) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RDI) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RSI)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_global_symbol_load(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const char *symbol_name, const MtlcType *type, int declare_external,
    BinaryGpRegister target_register) {
  size_t displacement_offset = 0;
  int size = code_generator_binary_resolved_type_scalar_size(type);
  int is_signed = code_generator_binary_resolved_type_is_signed_integer(type);

  if (!generator || !context || !symbol_name || symbol_name[0] == '\0') {
    return 0;
  }

  if (declare_external &&
      !code_generator_binary_declare_external_symbol(generator, symbol_name)) {
    return 0;
  }

  switch (size) {
  case 1:
    if ((!binary_emit_movzx_reg_rip_mem8(&context->code, target_register,
                                         &displacement_offset)) ||
        (is_signed &&
         !binary_emit_movsx_reg_reg8(&context->code, target_register,
                                     target_register))) {
      return 0;
    }
    break;
  case 2:
    if ((!binary_emit_movzx_reg_rip_mem16(&context->code, target_register,
                                          &displacement_offset)) ||
        (is_signed &&
         !binary_emit_movsx_reg_reg16(&context->code, target_register,
                                      target_register))) {
      return 0;
    }
    break;
  case 4:
    if (!binary_emit_mov_reg32_rip_mem(&context->code, target_register,
                                       &displacement_offset) ||
        (is_signed &&
         !binary_emit_movsxd_reg_reg32(&context->code, target_register,
                                       target_register))) {
      return 0;
    }
    break;
  case 8:
    if (!binary_emit_mov_reg_rip_mem(&context->code, target_register,
                                     &displacement_offset)) {
      return 0;
    }
    break;
  default:
    code_generator_set_error(
        generator,
        "Direct object backend does not yet support global scalar loads wider "
        "than 8 bytes in function '%s'",
        context->function_name);
    return 0;
  }

  if (!binary_call_relocation_table_add(&context->call_relocations, symbol_name,
                                        displacement_offset)) {
    code_generator_set_error(generator,
                             "Out of memory while emitting global load");
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_global_symbol_store(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const char *symbol_name, const MtlcType *type, int declare_external,
    BinaryGpRegister source_register) {
  size_t displacement_offset = 0;
  int size = code_generator_binary_resolved_type_scalar_size(type);

  if (!generator || !context || !symbol_name || symbol_name[0] == '\0') {
    return 0;
  }

  if (declare_external &&
      !code_generator_binary_declare_external_symbol(generator, symbol_name)) {
    return 0;
  }

  switch (size) {
  case 1:
    if (!binary_emit_mov_mem_rip_reg8(&context->code, source_register,
                                      &displacement_offset)) {
      return 0;
    }
    break;
  case 2:
    if (!binary_emit_mov_mem_rip_reg16(&context->code, source_register,
                                       &displacement_offset)) {
      return 0;
    }
    break;
  case 4:
    if (!binary_emit_mov_mem_rip_reg32(&context->code, source_register,
                                       &displacement_offset)) {
      return 0;
    }
    break;
  case 8:
    if (!binary_emit_mov_mem_rip_reg(&context->code, source_register,
                                     &displacement_offset)) {
      return 0;
    }
    break;
  default:
    code_generator_set_error(
        generator,
        "Direct object backend does not yet support global scalar stores wider "
        "than 8 bytes in function '%s'",
        context->function_name);
    return 0;
  }

  if (!binary_call_relocation_table_add(&context->call_relocations, symbol_name,
                                        displacement_offset)) {
    code_generator_set_error(generator,
                             "Out of memory while emitting global store");
    return 0;
  }

  return 1;
}

int code_generator_binary_operand_is_known_float64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand) {
  const CgSym *symbol = NULL;

  if (!context || !operand) {
    return 0;
  }

  if (operand->kind == IR_OPERAND_FLOAT) {
    return 1;
  }

  if ((operand->kind == IR_OPERAND_SYMBOL || operand->kind == IR_OPERAND_TEMP) &&
      operand->name &&
      code_generator_binary_is_marked_float64_symbol(context, operand->name)) {
    return 1;
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    symbol =
        code_generator_binary_value_symbol(generator, context, operand->name);
    return symbol && code_generator_binary_resolved_type_is_float64(symbol->type);
  }

  return 0;
}

int code_generator_binary_operand_float_bits(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand) {
  const CgSym *symbol = NULL;

  if (!context || !operand) {
    return 0;
  }

  if (operand->kind == IR_OPERAND_FLOAT) {
    return operand->float_bits == 32 ? 32 : 64;
  }

  if ((operand->kind == IR_OPERAND_SYMBOL ||
       operand->kind == IR_OPERAND_TEMP)) {
    if (operand->float_bits == 32 || operand->float_bits == 64) {
      return operand->float_bits;
    }
    if (operand->name) {
      int marked = code_generator_binary_marked_symbol_float_bits(
          context, operand->name);
      if (marked) {
        return marked;
      }
    }
  }

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    symbol =
        code_generator_binary_value_symbol(generator, context, operand->name);
    if (symbol) {
      return code_generator_binary_resolved_type_float_bits(symbol->type);
    }
  }

  return 0;
}

int code_generator_binary_instruction_result_is_float64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  const CgSym *symbol = NULL;
  const MtlcType *function_type = NULL;
  const char *op = NULL;

  if (!context || !instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
    return code_generator_binary_operand_is_known_float64(generator, context,
                                                          &instruction->lhs);

  case IR_OP_BINARY:
    op = instruction->text;
    return instruction->is_float && op &&
           (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
            strcmp(op, "*") == 0 || strcmp(op, "/") == 0);

  case IR_OP_UNARY:
    op = instruction->text;
    return instruction->is_float && op &&
           (strcmp(op, "+") == 0 || strcmp(op, "-") == 0);

  case IR_OP_CALL:
    symbol = generator && generator->ir_program && instruction->text
                 ? code_generator_lookup_symbol(generator,
                                       instruction->text)
                 : NULL;
    return symbol && symbol->kind == CG_SYM_FUNCTION &&
           code_generator_binary_resolved_type_float_bits(
               symbol->data.function.return_type) != 0;

  case IR_OP_CALL_INDIRECT:
    function_type = code_generator_binary_indirect_callee_type(
        generator, context, instruction);
    return code_generator_binary_resolved_type_float_bits(
               function_type ? function_type->fn_return_type
                             : instruction->value_type) != 0;

  case IR_OP_CAST:
    return code_generator_binary_named_type_float_bits(generator,
                                                       instruction->text) != 0;

  case IR_OP_LOAD:
    return instruction->is_float;

  default:
    return 0;
  }
}

int code_generator_binary_instruction_result_float_bits(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  const CgSym *symbol = NULL;
  const MtlcType *function_type = NULL;

  if (!context || !instruction) {
    return 0;
  }

  if (!code_generator_binary_instruction_result_is_float64(generator, context,
                                                           instruction)) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
    return code_generator_binary_operand_float_bits(generator, context,
                                                    &instruction->lhs);

  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_LOAD:
    return (instruction->float_bits == 32) ? 32 : 64;

  case IR_OP_CALL:
    symbol = generator && generator->ir_program && instruction->text
                 ? code_generator_lookup_symbol(generator,
                                       instruction->text)
                 : NULL;
    return (symbol && symbol->kind == CG_SYM_FUNCTION)
               ? code_generator_binary_resolved_type_float_bits(
                     symbol->data.function.return_type)
               : 64;

  case IR_OP_CALL_INDIRECT:
    function_type = code_generator_binary_indirect_callee_type(
        generator, context, instruction);
    if (!function_type) {
      int bits = code_generator_binary_resolved_type_float_bits(
          instruction->value_type);
      return bits ? bits : 64;
    }
    return code_generator_binary_resolved_type_float_bits(
        function_type->fn_return_type);

  case IR_OP_CAST: {
    const MtlcType *t = generator && generator->ir_program
                  ? code_generator_named_type(generator,
                                                  instruction->text)
                  : NULL;
    return code_generator_binary_resolved_type_float_bits(t);
  }

  default:
    return 64;
  }
}

int code_generator_binary_emit_string_symbol_load(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const char *symbol_name, const CgSym *symbol,
    BinaryGpRegister target_register) {
  int offset = 0;

  if (!generator || !context || !symbol_name || symbol_name[0] == '\0') {
    return 0;
  }

  offset = code_generator_binary_get_symbol_offset(context, symbol_name);
  if (offset > 0) {
    if (symbol && symbol->kind == CG_SYM_PARAMETER) {
      return binary_emit_mov_reg_mem(&context->code, target_register,
                                     BINARY_GP_RBP, -offset);
    }
    return binary_emit_lea_reg_mem(&context->code, target_register,
                                   BINARY_GP_RBP, -offset);
  }

  if (symbol && symbol->scope && symbol->scope->type == CG_SCOPE_GLOBAL) {
    const char *link_name =
        code_generator_get_link_symbol_name(generator, symbol_name);
    if (!link_name || link_name[0] == '\0') {
      code_generator_set_error(generator,
                               "Invalid global string symbol '%s' in function "
                               "'%s'",
                               symbol_name, context->function_name);
      return 0;
    }
    return code_generator_binary_emit_symbol_address(
        generator, context, link_name, symbol->is_extern, target_register);
  }

  code_generator_set_error(generator,
                           "Unknown string symbol '%s' in function '%s'",
                           symbol_name, context->function_name);
  return 0;
}

static const BinaryMarshaledOperand *binary_marshaled_slot(
    const BinaryFunctionContext *context, const IROperand *operand) {
  if (!context || !operand || context->marshaled_operand_count == 0) {
    return NULL;
  }
  for (size_t i = 0; i < context->marshaled_operand_count; i++) {
    if (context->marshaled_operands[i].operand == operand) {
      return &context->marshaled_operands[i];
    }
  }
  return NULL;
}

int code_generator_binary_emit_operand_load(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand, BinaryGpRegister target_register) {
  if (!generator || !context || !operand) {
    return 0;
  }

  {
    const BinaryMarshaledOperand *slot =
        binary_marshaled_slot(context, operand);
    if (slot) {
      return binary_emit_mov_reg_mem(&context->code, target_register,
                                     (BinaryGpRegister)slot->base_register,
                                     slot->displacement);
    }
  }

  switch (operand->kind) {
  case IR_OPERAND_NONE:
    return binary_emit_mov_reg_imm64(&context->code, target_register, 0);

  case IR_OPERAND_INT:
    return binary_emit_mov_reg_imm64(&context->code, target_register,
                                     (uint64_t)operand->int_value);

  case IR_OPERAND_FLOAT: {
    if (operand->float_bits == 32) {
      union {
        float value;
        uint32_t bits;
      } encoded = {0};
      encoded.value = (float)operand->float_value;
      return binary_emit_mov_reg_imm64(&context->code, target_register,
                                       (uint64_t)encoded.bits);
    }
    union {
      double value;
      uint64_t bits;
    } encoded = {0};
    encoded.value = operand->float_value;
    return binary_emit_mov_reg_imm64(&context->code, target_register,
                                     encoded.bits);
  }

  case IR_OPERAND_STRING:
    return code_generator_binary_emit_string_literal_value_address(
        generator, context, operand->name ? operand->name : "",
        ir_operand_string_length(operand), target_register);

  case IR_OPERAND_TEMP: {
    int offset = code_generator_binary_get_temp_offset(context, operand->name);
    if (offset <= 0) {
      code_generator_set_error(generator, "Unknown IR temp '%s' in function '%s'",
                               operand->name ? operand->name : "<unnamed>",
                               context->function_name);
      return 0;
    }
    return code_generator_binary_emit_temp_stack_load(
        generator, context, offset, target_register, NULL);
  }

  case IR_OPERAND_SYMBOL: {
    const char *alias_target =
        binary_symbol_alias_table_get(&context->symbol_aliases, operand->name);
    const CgSym *symbol =
        code_generator_binary_value_symbol(generator, context, operand->name);
    const MtlcType *load_type = symbol ? symbol->type
                             : code_generator_binary_get_operand_type_in_context(
                                   generator, context, operand);
    int offset = code_generator_binary_get_symbol_offset(context, operand->name);
    BinaryGpRegister assigned_register = BINARY_GP_RAX;
    if (alias_target) {
      IROperand aliased = *operand;
      aliased.name = (char *)alias_target;
      return code_generator_binary_emit_operand_load(generator, context,
                                                     &aliased,
                                                     target_register);
    }
    if (offset > 0 &&
        binary_named_slot_table_get_offset(&context->string_symbols,
                                           operand->name) >= 0) {
      return binary_emit_lea_reg_mem(&context->code, target_register,
                                     BINARY_GP_RBP, -offset);
    }
    if (symbol && symbol->type && symbol->type->kind == MTLC_TYPE_STRING) {
      return code_generator_binary_emit_string_symbol_load(
          generator, context, operand->name, symbol, target_register);
    }
    if (code_generator_binary_symbol_assigned_register(
            generator, context, operand->name, &assigned_register)) {
      if (target_register == assigned_register) {
        return 1;
      }
      return code_generator_binary_emit_reg_reg_move(
          &context->code, target_register, assigned_register, load_type);
    }
    if (offset > 0 && symbol &&
        code_generator_binary_type_is_direct_aggregate(symbol->type)) {
      int size = (int)symbol->type->size;
      if (!binary_emit_lea_reg_mem(&context->code, target_register,
                                   BINARY_GP_RBP, -offset) ||
          !code_generator_binary_emit_load_from_address(
              generator, context, target_register, size, target_register)) {
        if (!generator->has_error) {
          code_generator_set_error(
              generator,
              "Out of memory while loading direct aggregate symbol '%s' in "
              "function '%s'",
              operand->name ? operand->name : "<unnamed>",
              context->function_name);
        }
        return 0;
      }
      return 1;
    }
    if (offset <= 0) {
      if (symbol && symbol->scope && symbol->scope->type == CG_SCOPE_GLOBAL) {
        const char *link_name =
            code_generator_get_link_symbol_name(generator, operand->name);
        uint64_t const_value = 0;
        if (!link_name || link_name[0] == '\0') {
          code_generator_set_error(generator,
                                   "Invalid global symbol '%s' in function '%s'",
                                   operand->name ? operand->name : "<unnamed>",
                                   context->function_name);
          return 0;
        }
        if (!code_generator_binary_symbol_is_scalar_accessible(generator,
                                                               operand->name)) {
          code_generator_set_error(
              generator,
              "Direct object backend cannot load aggregate global symbol '%s' "
              "directly in function '%s'",
              operand->name ? operand->name : "<unnamed>",
              context->function_name);
          return 0;
        }
        if (binary_global_const_table_get(operand->name, &const_value)) {
          return binary_emit_mov_reg_imm64(&context->code, target_register,
                                           const_value);
        }
        if (!code_generator_binary_emit_global_symbol_load(
                generator, context, link_name, symbol->type, symbol->is_extern,
                target_register)) {
          if (!generator->has_error) {
            code_generator_set_error(
                generator,
                "Out of memory while loading global symbol '%s' in function "
                "'%s'",
                operand->name ? operand->name : "<unnamed>",
                context->function_name);
          }
          return 0;
        }
        return 1;
      }

      code_generator_set_error(
          generator,
          "Direct object backend only supports parameter/local/global symbols "
          "(encountered '%s' in function '%s')",
          operand->name ? operand->name : "<unnamed>", context->function_name);
      return 0;
    }
    if (!code_generator_binary_symbol_is_scalar_accessible(generator,
                                                           operand->name)) {
      code_generator_set_error(
          generator,
          "Direct object backend cannot load aggregate symbol '%s' directly "
          "in function '%s'",
          operand->name ? operand->name : "<unnamed>", context->function_name);
      return 0;
    }
    return code_generator_binary_emit_symbol_stack_load(
        generator, context, load_type, offset, target_register);
  }

  default:
    code_generator_set_error(
        generator,
        "Direct object backend does not support operand kind %d in function "
        "'%s'",
        (int)operand->kind, context->function_name);
    return 0;
  }
}

int code_generator_binary_emit_memcpy_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  long long byte_count = 0;
  BinaryGpRegister dst_reg = BINARY_GP_R10;
  BinaryGpRegister src_reg = BINARY_GP_R11;

  if (!generator || !context || !instruction) {
    return 0;
  }

  if (instruction->rhs.kind == IR_OPERAND_INT) {
    byte_count = instruction->rhs.int_value;
  } else {
    code_generator_set_error(generator,
                             "memcpy_inline requires constant size in '%s'",
                             context->function_name);
    return 0;
  }

  if (byte_count <= 0) {
    return 1;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest, dst_reg) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs, src_reg)) {
    return 0;
  }

  if (byte_count >= 64 && (byte_count % 8) == 0) {
    return code_generator_binary_emit_rep_movsq(generator, context, src_reg,
                                                dst_reg,
                                                (size_t)(byte_count / 8));
  }

  return code_generator_binary_emit_rep_movsb(generator, context, src_reg,
                                              dst_reg, (size_t)byte_count);
}

int code_generator_binary_emit_local_string_store(
    CodeGenerator *generator, BinaryFunctionContext *context, int offset,
    BinaryGpRegister source_register) {
  BinaryGpRegister scratch =
      source_register == BINARY_GP_R10 ? BINARY_GP_RAX : BINARY_GP_R10;
  int chars_displacement = -offset;
  int length_displacement = 8 - offset;

  if (!generator || !context || offset <= 8) {
    return 0;
  }

  if (!binary_emit_mov_reg_mem(&context->code, scratch, source_register, 0) ||
      !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP,
                               chars_displacement, scratch) ||
      !binary_emit_mov_reg_mem(&context->code, scratch, source_register, 8) ||
      !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP,
                               length_displacement, scratch)) {
    code_generator_set_error(generator,
                             "Out of memory while storing string value in "
                             "function '%s'",
                             context->function_name);
    return 0;
  }

  return 1;
}

static int code_generator_binary_emit_global_string_store(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const char *symbol_name, int declare_external,
    BinaryGpRegister source_register) {
  BinaryGpRegister address = BINARY_GP_R10;
  BinaryGpRegister scratch = BINARY_GP_R11;

  if (!generator || !context || !symbol_name || symbol_name[0] == '\0') {
    return 0;
  }
  if (source_register == address) {
    address = BINARY_GP_RAX;
  }
  if (source_register == scratch) {
    scratch = BINARY_GP_RAX;
  }

  if (!code_generator_binary_emit_symbol_address(generator, context,
                                                 symbol_name, declare_external,
                                                 address)) {
    return 0;
  }
  if (!binary_emit_mov_reg_mem(&context->code, scratch, source_register, 0) ||
      !binary_emit_mov_mem_reg(&context->code, address, 0, scratch) ||
      !binary_emit_mov_reg_mem(&context->code, scratch, source_register, 8) ||
      !binary_emit_mov_mem_reg(&context->code, address, 8, scratch)) {
    code_generator_set_error(generator,
                             "Out of memory while storing string global '%s' "
                             "in function '%s'",
                             symbol_name, context->function_name);
    return 0;
  }
  return 1;
}

static int binary_canonicalize_narrow_reg_for_type(
    BinaryFunctionContext *context, const MtlcType *type, BinaryGpRegister reg);

int code_generator_binary_emit_destination_store(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *destination, BinaryGpRegister source_register) {
  if (!generator || !context || !destination) {
    return 0;
  }

  {
    const BinaryMarshaledOperand *slot =
        binary_marshaled_slot(context, destination);
    if (slot) {
      return binary_emit_mov_mem_reg(&context->code,
                                     (BinaryGpRegister)slot->base_register,
                                     slot->displacement, source_register);
    }
  }

  switch (destination->kind) {
  case IR_OPERAND_NONE:
    return 1;

  case IR_OPERAND_TEMP: {
    int offset =
        code_generator_binary_get_temp_offset(context, destination->name);
    if (offset <= 0) {
      code_generator_set_error(generator, "Unknown IR temp '%s' in function '%s'",
                               destination->name ? destination->name
                                                 : "<unnamed>",
                               context->function_name);
      return 0;
    }
    return binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP, -offset,
                                   source_register);
  }

  case IR_OPERAND_SYMBOL: {
    const CgSym *symbol = code_generator_binary_value_symbol(
        generator, context, destination->name);
    const MtlcType *dest_type = symbol && symbol->type
                          ? symbol->type
                          : code_generator_binary_get_operand_type_in_context(
                                generator, context, destination);
    int offset =
        code_generator_binary_get_symbol_offset(context, destination->name);
    BinaryGpRegister assigned_register = BINARY_GP_RAX;
    if (offset > 0 &&
        binary_named_slot_table_get_offset(&context->string_symbols,
                                           destination->name) >= 0) {
      return code_generator_binary_emit_local_string_store(
          generator, context, offset, source_register);
    }
    if (symbol && symbol->type && symbol->type->kind == MTLC_TYPE_STRING) {
      if (offset <= 0) {
        if (symbol->scope && symbol->scope->type == CG_SCOPE_GLOBAL) {
          const char *link_name =
              code_generator_get_link_symbol_name(generator, destination->name);
          if (link_name && link_name[0] != '\0') {
            return code_generator_binary_emit_global_string_store(
                generator, context, link_name, symbol->is_extern,
                source_register);
          }
          code_generator_set_error(
              generator, "Invalid global string symbol '%s' in function '%s'",
              destination->name ? destination->name : "<unnamed>",
              context->function_name);
        } else {
          code_generator_set_error(generator,
                                   "Unknown string symbol '%s' in function '%s'",
                                   destination->name ? destination->name
                                                     : "<unnamed>",
                                   context->function_name);
        }
        return 0;
      }

      if (symbol->kind == CG_SYM_PARAMETER) {
        return binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP, -offset,
                                       source_register);
      }

      return code_generator_binary_emit_local_string_store(
          generator, context, offset, source_register);
    }
    if (code_generator_binary_symbol_assigned_register(
            generator, context, destination->name, &assigned_register)) {
      if (assigned_register == source_register) {
        return binary_canonicalize_narrow_reg_for_type(context, dest_type,
                                                       assigned_register);
      }
      return code_generator_binary_emit_reg_reg_move(
          &context->code, assigned_register, source_register, dest_type);
    }
    if (offset > 0 && symbol &&
        code_generator_binary_type_is_direct_aggregate(symbol->type)) {
      int size = (int)symbol->type->size;
      BinaryGpRegister address_register =
          source_register == BINARY_GP_R10 ? BINARY_GP_RAX : BINARY_GP_R10;
      if (!binary_emit_lea_reg_mem(&context->code, address_register,
                                   BINARY_GP_RBP, -offset) ||
          !code_generator_binary_emit_store_to_address(
              generator, context, address_register, size, source_register)) {
        if (!generator->has_error) {
          code_generator_set_error(
              generator,
              "Out of memory while storing direct aggregate symbol '%s' in "
              "function '%s'",
              destination->name ? destination->name : "<unnamed>",
              context->function_name);
        }
        return 0;
      }
      return 1;
    }
    if (offset <= 0) {
      if (symbol && symbol->scope && symbol->scope->type == CG_SCOPE_GLOBAL) {
        const char *link_name =
            code_generator_get_link_symbol_name(generator, destination->name);
        if (!link_name || link_name[0] == '\0') {
          code_generator_set_error(generator,
                                   "Invalid global symbol '%s' in function '%s'",
                                   destination->name
                                       ? destination->name
                                       : "<unnamed>",
                                   context->function_name);
          return 0;
        }
        if (!code_generator_binary_symbol_is_scalar_accessible(generator,
                                                               destination->name)) {
          code_generator_set_error(
              generator,
              "Direct object backend cannot store aggregate global symbol '%s' "
              "directly in function '%s'",
              destination->name ? destination->name : "<unnamed>",
              context->function_name);
          return 0;
        }
        if (!code_generator_binary_emit_global_symbol_store(
                generator, context, link_name, symbol->type, symbol->is_extern,
                source_register)) {
          if (!generator->has_error) {
            code_generator_set_error(
                generator,
                "Out of memory while storing global symbol '%s' in function "
                "'%s'",
                destination->name ? destination->name : "<unnamed>",
                context->function_name);
          }
          return 0;
        }
        return 1;
      }

      code_generator_set_error(
          generator,
          "Direct object backend only supports stores to "
          "parameter/local/global symbols (encountered '%s' in function '%s')",
          destination->name ? destination->name : "<unnamed>",
          context->function_name);
      return 0;
    }
    if (!code_generator_binary_symbol_is_scalar_accessible(generator,
                                                           destination->name)) {
      code_generator_set_error(
          generator,
          "Direct object backend cannot store aggregate symbol '%s' directly "
          "in function '%s'",
          destination->name ? destination->name : "<unnamed>",
          context->function_name);
      return 0;
    }
    return code_generator_binary_emit_symbol_stack_store(
        generator, context, dest_type, offset, source_register);
  }

  default:
    code_generator_set_error(
        generator,
        "Direct object backend does not support destination kind %d in "
        "function '%s'",
        (int)destination->kind, context->function_name);
    return 0;
  }
}

static int binary_trap_out_of_memory(CodeGenerator *generator,
                                     BinaryFunctionContext *context) {
  if (!generator->has_error) {
    code_generator_set_error(generator,
                             "Out of memory while emitting runtime trap "
                             "call in function '%s'",
                             context->function_name);
  }
  return 0;
}

static int binary_emit_trap_message_address(CodeGenerator *generator,
                                            BinaryFunctionContext *context,
                                            const IROperand *message,
                                            BinaryGpRegister target) {
  if (message->kind == IR_OPERAND_STRING) {
    return code_generator_binary_emit_cstring_literal_address(
        generator, context, message->name ? message->name : "", target);
  }
  return code_generator_binary_emit_operand_load(generator, context, message,
                                                 target);
}

static int binary_emit_trap_puts(BinaryFunctionContext *context, int shadow) {
  size_t displacement_offset = 0;

  return binary_emit_sub_rsp_imm32(&context->code, shadow) &&
         binary_emit_call_placeholder(&context->code, &displacement_offset) &&
         binary_call_relocation_table_add(&context->call_relocations, "puts",
                                          displacement_offset) &&
         binary_emit_add_rsp_imm32(&context->code, shadow);
}

static int binary_emit_trap_abort(CodeGenerator *generator,
                                  BinaryFunctionContext *context,
                                  const IROperand *message) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  BinaryGpRegister arg0 = abi->int_param_registers[0];
  int shadow = abi->shadow_space_size;
  size_t displacement_offset = 0;

  if (!code_generator_binary_declare_external_symbol(generator, "puts") ||
      !code_generator_binary_declare_external_symbol(generator, "exit")) {
    return 0;
  }
  if (!binary_emit_trap_message_address(generator, context, message, arg0)) {
    return 0;
  }
  if (!binary_emit_trap_puts(context, shadow) ||
      !code_generator_binary_emit_cstring_literal_address(
          generator, context,
          "  rebuild with -s for the file, line and stack trace", arg0) ||
      !binary_emit_trap_puts(context, shadow) ||
      !binary_emit_mov_reg_imm64(&context->code, arg0, 1) ||
      !binary_emit_sub_rsp_imm32(&context->code, shadow) ||
      !binary_emit_call_placeholder(&context->code, &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, "exit",
                                        displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code, shadow)) {
    return binary_trap_out_of_memory(generator, context);
  }
  return 1;
}

static char *binary_open_trap_site(CodeGenerator *generator,
                                   BinaryFunctionContext *context,
                                   const IRInstruction *instruction,
                                   const char *trap_symbol, int is_trap_ex) {
  char *label = code_generator_generate_label(generator, "mettledbg_trap_pc");

  if (!label) {
    code_generator_set_error(generator,
                             "Out of memory while creating runtime trap label");
    return NULL;
  }
  if (!binary_label_table_define(&context->labels, label, context->code.size)) {
    code_generator_set_error(
        generator, "Failed to define runtime trap label in function '%s'",
        context->function_name);
    free(label);
    return NULL;
  }
  if (!code_generator_binary_record_debug_label_export(context, label,
                                                       context->code.size)) {
    code_generator_set_error(generator,
                             "Out of memory while recording runtime trap "
                             "label in function '%s'",
                             context->function_name);
    free(label);
    return NULL;
  }
  if (instruction->location.line > 0 &&
      !code_generator_binary_emit_runtime_location_marker(
          generator, context, instruction->location.line,
          instruction->location.column,
          code_generator_runtime_filename(generator,
                                          instruction->location.filename))) {
    free(label);
    return NULL;
  }
  if (is_trap_ex && instruction->argument_count >= 4) {
    uint32_t kind = instruction->arguments[0].kind == IR_OPERAND_INT
                        ? (uint32_t)instruction->arguments[0].int_value
                        : 0u;
    const char *message = instruction->arguments[1].kind == IR_OPERAND_STRING
                              ? instruction->arguments[1].name
                              : NULL;
    code_generator_record_runtime_trap_site(
        generator, label, kind, instruction->location.line,
        instruction->location.column,
        code_generator_runtime_filename(generator,
                                        instruction->location.filename),
        message, NULL);
  }
  if (!code_generator_binary_declare_external_symbol(generator, trap_symbol)) {
    free(label);
    return NULL;
  }
  return label;
}

static int binary_emit_trap_detail(CodeGenerator *generator,
                                   BinaryFunctionContext *context,
                                   const IROperand *detail,
                                   BinaryGpRegister target, int spill,
                                   int spill_offset) {
  if (detail->kind == IR_OPERAND_INT) {
    if (!binary_emit_mov_reg_imm64(&context->code, target,
                                   (unsigned long long)detail->int_value)) {
      return 0;
    }
  } else if (!code_generator_binary_emit_operand_load(generator, context,
                                                      detail, target)) {
    return 0;
  }
  if (spill && !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP,
                                        spill_offset, target)) {
    return 0;
  }
  return 1;
}

static int binary_emit_trap_detailed_call(CodeGenerator *generator,
                                          BinaryFunctionContext *context,
                                          const IRInstruction *instruction,
                                          const BinaryAbi *abi,
                                          const char *trap_symbol,
                                          const char *label) {
  size_t register_count = abi->int_param_count;
  int stacked = register_count < 6 ? 6 - (int)register_count : 0;
  int call_frame_size = abi->shadow_space_size + stacked * 8;
  BinaryGpRegister detail0 =
      register_count > 4 ? abi->int_param_registers[4] : BINARY_GP_RAX;
  BinaryGpRegister detail1 =
      register_count > 5 ? abi->int_param_registers[5] : BINARY_GP_RAX;
  size_t displacement_offset = 0;

  if (instruction->argument_count < 4 ||
      instruction->arguments[0].kind != IR_OPERAND_INT ||
      instruction->arguments[1].kind != IR_OPERAND_STRING ||
      register_count < 4) {
    code_generator_set_error(generator,
                             "Invalid mettle_crash_trap_ex call in function "
                             "'%s'",
                             context->function_name);
    return 0;
  }
  if (!binary_emit_sub_rsp_imm32(&context->code, call_frame_size) ||
      !binary_emit_mov_reg_imm64(
          &context->code, abi->int_param_registers[0],
          (unsigned long long)instruction->arguments[0].int_value) ||
      !code_generator_binary_emit_cstring_literal_address(
          generator, context,
          instruction->arguments[1].name ? instruction->arguments[1].name : "",
          abi->int_param_registers[1]) ||
      !binary_emit_lea_reg_rip_placeholder(&context->code,
                                           abi->int_param_registers[2],
                                           &displacement_offset) ||
      !binary_label_fixup_table_add(&context->label_fixups, label,
                                    displacement_offset) ||
      !binary_emit_mov_reg_reg(&context->code, abi->int_param_registers[3],
                               BINARY_GP_RBP) ||
      !binary_emit_trap_detail(generator, context, &instruction->arguments[2],
                               detail0, register_count <= 4,
                               abi->shadow_space_size) ||
      !binary_emit_trap_detail(generator, context, &instruction->arguments[3],
                               detail1, register_count <= 5,
                               abi->shadow_space_size + 8)) {
    return 0;
  }
  if (!binary_emit_call_placeholder(&context->code, &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, trap_symbol,
                                        displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code, call_frame_size)) {
    return binary_trap_out_of_memory(generator, context);
  }
  return 1;
}

static int binary_emit_trap_message_call(CodeGenerator *generator,
                                         BinaryFunctionContext *context,
                                         const IRInstruction *instruction,
                                         const BinaryAbi *abi,
                                         const char *trap_symbol,
                                         const char *label) {
  size_t displacement_offset = 0;

  if (!binary_emit_trap_message_address(generator, context,
                                        &instruction->arguments[0],
                                        abi->int_param_registers[0])) {
    return 0;
  }
  if (!binary_emit_lea_reg_rip_placeholder(&context->code,
                                           abi->int_param_registers[1],
                                           &displacement_offset) ||
      !binary_label_fixup_table_add(&context->label_fixups, label,
                                    displacement_offset) ||
      !binary_emit_mov_reg_reg(&context->code, abi->int_param_registers[2],
                               BINARY_GP_RBP) ||
      !binary_emit_sub_rsp_imm32(&context->code, abi->shadow_space_size) ||
      !binary_emit_call_placeholder(&context->code, &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, trap_symbol,
                                        displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code, abi->shadow_space_size)) {
    return binary_trap_out_of_memory(generator, context);
  }
  return 1;
}

int code_generator_binary_emit_runtime_trap_call(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  int is_trap_ex;
  const char *trap_symbol;
  size_t message_index;
  char *label;
  int ok;

  if (!generator || !context || !instruction ||
      instruction->argument_count == 0) {
    return 0;
  }
  is_trap_ex = instruction->text &&
               strcmp(instruction->text, "mettle_crash_trap_ex") == 0;
  trap_symbol = is_trap_ex ? "mettle_crash_trap_ex" : "mettle_crash_trap";
  message_index = is_trap_ex ? 1u : 0u;

  if (!generator->generate_stack_trace_support && !generator->live) {
    return binary_emit_trap_abort(
        generator, context,
        instruction->argument_count > message_index
            ? &instruction->arguments[message_index]
            : &instruction->arguments[0]);
  }
  label = binary_open_trap_site(generator, context, instruction, trap_symbol,
                                is_trap_ex);
  if (!label) {
    return 0;
  }
  ok = is_trap_ex ? binary_emit_trap_detailed_call(generator, context,
                                                   instruction, abi,
                                                   trap_symbol, label)
                  : binary_emit_trap_message_call(generator, context,
                                                  instruction, abi,
                                                  trap_symbol, label);
  free(label);
  return ok;
}

int code_generator_binary_load_needs_sign_extend(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *destination, int load_size) {
  const CgSym *symbol = NULL;

  if ((load_size != 1 && load_size != 2 && load_size != 4) || !destination) {
    return 0;
  }

  if (destination->kind == IR_OPERAND_SYMBOL && destination->name) {
    symbol = code_generator_binary_value_symbol(generator, context,
                                                destination->name);
    if (symbol && symbol->type &&
        code_generator_binary_resolved_type_scalar_size(symbol->type) == 4) {
      return code_generator_binary_resolved_type_is_signed_integer(symbol->type);
    }
  }

  if (destination->kind == IR_OPERAND_TEMP && destination->name) {
    return 1;
  }

  return 1;
}

int code_generator_binary_emit_unsigned_int_to_float(
    BinaryFunctionContext *context, int float_bits,
    BinaryXmmRegister destination, BinaryGpRegister source,
    BinaryGpRegister work, BinaryGpRegister odd) {
  size_t to_negative = 0;
  size_t to_done = 0;

  if (!context || work == odd || source == odd) {
    return 0;
  }
  if (source != work && !binary_emit_mov_reg_reg(&context->code, work, source)) {
    return 0;
  }
  if (!binary_emit_test_reg_reg(&context->code, work) ||
      !binary_emit_jcc_placeholder(&context->code, 0x88, &to_negative)) {
    return 0;
  }
  if (float_bits == 32) {
    if (!binary_emit_cvtsi2ss_xmm_reg(&context->code, destination, work)) {
      return 0;
    }
  } else if (!binary_emit_cvtsi2sd_xmm_reg(&context->code, destination, work)) {
    return 0;
  }
  if (!binary_emit_jmp_placeholder(&context->code, &to_done) ||
      !binary_function_context_patch_rel32(context, to_negative,
                                           context->code.size)) {
    return 0;
  }
  if (!binary_emit_mov_reg_reg(&context->code, odd, work) ||
      !binary_emit_alu_reg_imm32(&context->code, 4, odd, 1u) ||
      !binary_emit_shift_reg_imm8(&context->code, 5, work, 1) ||
      !binary_emit_alu_reg_reg(&context->code, 0x09, work, odd)) {
    return 0;
  }
  if (float_bits == 32) {
    if (!binary_emit_cvtsi2ss_xmm_reg(&context->code, destination, work) ||
        !binary_emit_addss_xmm_xmm(&context->code, destination, destination)) {
      return 0;
    }
  } else if (!binary_emit_cvtsi2sd_xmm_reg(&context->code, destination, work) ||
             !binary_emit_addsd_xmm_xmm(&context->code, destination,
                                        destination)) {
    return 0;
  }
  return binary_function_context_patch_rel32(context, to_done,
                                             context->code.size);
}

int code_generator_binary_emit_float_to_unsigned_int(
    BinaryFunctionContext *context, int float_bits,
    BinaryGpRegister destination, BinaryXmmRegister source,
    BinaryGpRegister work, BinaryXmmRegister scratch) {
  uint64_t bias = (float_bits == 32) ? 0x5F000000ull : 0x43E0000000000000ull;
  uint64_t minus_bias = (float_bits == 32) ? 0xDF000000ull
                                           : 0xC3E0000000000000ull;
  size_t to_small = 0;
  size_t to_done = 0;

  if (!context || source == scratch) {
    return 0;
  }
  if (!binary_emit_mov_reg_imm64(&context->code, work, bias)) {
    return 0;
  }
  if (float_bits == 32) {
    if (!binary_emit_movd_xmm_reg(&context->code, scratch, work) ||
        !binary_emit_ucomiss_xmm_xmm(&context->code, source, scratch)) {
      return 0;
    }
  } else if (!binary_emit_movq_xmm_reg(&context->code, scratch, work) ||
             !binary_emit_ucomisd_xmm_xmm(&context->code, source, scratch)) {
    return 0;
  }
  if (!binary_emit_jcc_placeholder(&context->code, 0x82, &to_small)) {
    return 0;
  }
  if (!binary_emit_mov_reg_imm64(&context->code, work, minus_bias)) {
    return 0;
  }
  if (float_bits == 32) {
    if (!binary_emit_movd_xmm_reg(&context->code, scratch, work) ||
        !binary_emit_addss_xmm_xmm(&context->code, scratch, source) ||
        !binary_emit_cvttss2si_reg_xmm(&context->code, destination, scratch)) {
      return 0;
    }
  } else if (!binary_emit_movq_xmm_reg(&context->code, scratch, work) ||
             !binary_emit_addsd_xmm_xmm(&context->code, scratch, source) ||
             !binary_emit_cvttsd2si_reg_xmm(&context->code, destination,
                                            scratch)) {
    return 0;
  }
  if (!binary_emit_mov_reg_imm64(&context->code, work, 0x8000000000000000ull) ||
      !binary_emit_alu_reg_reg(&context->code, 0x31, destination, work) ||
      !binary_emit_jmp_placeholder(&context->code, &to_done) ||
      !binary_function_context_patch_rel32(context, to_small,
                                           context->code.size)) {
    return 0;
  }
  if (float_bits == 32) {
    if (!binary_emit_cvttss2si_reg_xmm(&context->code, destination, source)) {
      return 0;
    }
  } else if (!binary_emit_cvttsd2si_reg_xmm(&context->code, destination,
                                            source)) {
    return 0;
  }
  return binary_function_context_patch_rel32(context, to_done,
                                             context->code.size);
}

const MtlcType *code_generator_binary_indirect_callee_type(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  const CgSym *symbol = NULL;

  if (!generator || !instruction ||
      instruction->lhs.kind != IR_OPERAND_SYMBOL || !instruction->lhs.name) {
    return NULL;
  }
  if (context && context->ir_function) {
    const IRInstruction *in = ir_function_find_declaration_frozen(
        context->ir_function, instruction->lhs.name);
    if (in) {
      const MtlcType *t =
          code_generator_binary_get_resolved_type(generator, in->text, 0);
      if (t && t->kind == MTLC_TYPE_FUNCTION_POINTER) {
        return t;
      }
    }
  }
  symbol = generator->ir_program
               ? code_generator_lookup_symbol(generator,
                                              instruction->lhs.name)
               : NULL;
  return (symbol && symbol->type &&
          symbol->type->kind == MTLC_TYPE_FUNCTION_POINTER)
             ? symbol->type
             : NULL;
}

static int binary_canonicalize_narrow_reg_for_type(
    BinaryFunctionContext *context, const MtlcType *type,
    BinaryGpRegister reg) {
  if (!context || !type || code_generator_type_is_aggregate(type) ||
      code_generator_binary_resolved_type_float_bits(type) != 0) {
    return 1;
  }
  int w = code_generator_binary_resolved_type_scalar_size(type);
  int is_signed = code_generator_binary_resolved_type_is_signed_integer(type);
  if (w == 4) {
    return is_signed ? binary_emit_movsxd_reg_reg32(&context->code, reg, reg)
                     : binary_emit_movzx_reg_reg32(&context->code, reg, reg);
  }
  if (w == 2) {
    return is_signed ? binary_emit_movsx_reg_reg16(&context->code, reg, reg)
                     : binary_emit_movzx_reg_reg16(&context->code, reg, reg);
  }
  if (w == 1) {
    return is_signed ? binary_emit_movsx_reg_reg8(&context->code, reg, reg)
                     : binary_emit_movzx_reg_reg8(&context->code, reg, reg);
  }
  return 1;
}

static int binary_emit_stack_probe_touch(BinaryCodeBuffer *code) {
  if (!code) {
    return 0;
  }
  return binary_code_buffer_append_u8(code, 0xF6) &&
         binary_code_buffer_append_u8(code, 0x04) &&
         binary_code_buffer_append_u8(code, 0x24) &&
         binary_code_buffer_append_u8(code, 0x00);
}

int binary_emit_frame_allocation(BinaryCodeBuffer *code, int frame_size) {
  if (frame_size <= 0) {
    return 1;
  }

  if (frame_size <= BINARY_STACK_PAGE_SIZE) {
    return binary_emit_sub_rsp_imm32(code, (uint32_t)frame_size);
  }

  int remaining = frame_size;
  while (remaining > BINARY_STACK_PAGE_SIZE) {
    if (!binary_emit_sub_rsp_imm32(code, (uint32_t)BINARY_STACK_PAGE_SIZE)) {
      return 0;
    }
    if (!binary_emit_stack_probe_touch(code)) {
      return 0;
    }
    remaining -= BINARY_STACK_PAGE_SIZE;
  }
  if (!binary_emit_sub_rsp_imm32(code, (uint32_t)remaining)) {
    return 0;
  }
  return binary_emit_stack_probe_touch(code);
}

int code_generator_binary_resolve_fixups(CodeGenerator *generator,
                                                BinaryFunctionContext *context,
                                                size_t return_offset) {
  if (!generator || !context) {
    return 0;
  }

  for (size_t i = 0; i < context->label_fixups.count; i++) {
    BinaryLabelFixup *fixup = &context->label_fixups.items[i];
    BinaryLabelEntry *label =
        binary_label_table_get(&context->labels, fixup->name);
    if (!label) {
      code_generator_set_error(
          generator,
          "Undefined IR label '%s' in direct object function '%s'",
          fixup->name ? fixup->name : "<unnamed>", context->function_name);
      return 0;
    }
    if (!binary_function_context_patch_rel32(
            context, fixup->displacement_offset, label->offset)) {
      code_generator_set_error(
          generator,
          "Branch target out of range while lowering function '%s'",
          context->function_name);
      return 0;
    }
  }

  for (size_t i = 0; i < context->return_fixups.count; i++) {
    if (!binary_function_context_patch_rel32(context,
                                             context->return_fixups.items[i],
                                             return_offset)) {
      code_generator_set_error(
          generator,
          "Return target out of range while lowering function '%s'",
          context->function_name);
      return 0;
    }
  }

  return 1;
}
