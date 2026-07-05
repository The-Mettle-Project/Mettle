#include "codegen/binary/internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Valid after `push rsi; push rdi`: recover the original string-copy
 * register value when the source address was already in RSI or RDI. */
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

static int binary_emit_windows_heap_alloc(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister size_register, uint32_t flags);
static int binary_emit_windows_zeroed_heap_alloc(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister size_register);

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
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to declare external symbol");
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
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to create .rdata section");
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
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit string literal");
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
    const char *value, BinaryGpRegister target_register) {
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

  rdata_section = binary_emitter_get_or_create_section(
      emitter, ".rdata", BINARY_SECTION_RDATA, 0, 8);
  if (rdata_section == (size_t)-1) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to create .rdata section");
    goto cleanup;
  }

  length = strlen(value);
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
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit string literal");
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
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit string literal");
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
    CodeGenerator *generator, const char *link_name, const char *value) {
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
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to create .data section");
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
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to create .rdata section");
      free(chars_label);
      return 0;
    }

    length = strlen(value);
    string_length = (uint64_t)length;
    if (!binary_emitter_append_bytes(emitter, rdata_section, value, length,
                                     &chars_offset) ||
        !binary_emitter_append_bytes(emitter, rdata_section, &terminator, 1,
                                     NULL) ||
        !binary_emitter_define_symbol(emitter, chars_label, BINARY_SYMBOL_LOCAL,
                                      rdata_section, chars_offset,
                                      length + 1)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit global string characters");
      free(chars_label);
      return 0;
    }
  }

  if (!binary_emitter_align_section(emitter, data_section, 8, 0) ||
      !binary_emitter_append_zeros(emitter, data_section, 16, &struct_offset)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to reserve global string storage");
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
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit global string relocation");
      free(chars_label);
      return 0;
    }
  }

  if (!binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                    data_section, struct_offset, 16)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to define global string symbol");
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
    /* Multi-byte aggregate (e.g. struct memcpy): rep movsb, RSI=src, RDI=dst,
     * RCX=count. Save non-volatile RSI/RDI on Win64. */
    uint64_t n = (uint64_t)size;
    if (n != (uint64_t)size || n == 0) {
      code_generator_set_error(
          generator,
          "Invalid aggregate store size %d in function '%s'",
          size, context->function_name);
      return 0;
    }
    return binary_emit_push_reg(&context->code, BINARY_GP_RSI) &&
           binary_emit_push_reg(&context->code, BINARY_GP_RDI) &&
           binary_emit_mov_reg_from_saved_string_source(
               &context->code, BINARY_GP_RSI, source_register) &&
           binary_emit_mov_reg_from_saved_string_source(
               &context->code, BINARY_GP_RDI, address_register) &&
           binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RCX, n) &&
           binary_code_buffer_append_u8(&context->code, 0xF3) &&
           binary_code_buffer_append_u8(&context->code, 0xA4) &&
           binary_emit_pop_reg(&context->code, BINARY_GP_RDI) &&
           binary_emit_pop_reg(&context->code, BINARY_GP_RSI);
  }
  }
}

/* Side-table helpers: which IR temps in the current binary function hold
 * pointers to indirect-returned structs. */
int binary_indirect_temp_add(BinaryFunctionContext *context,
                                    const char *name, size_t size) {
  if (!context || !name) return 0;
  if (context->indirect_temp_count >= context->indirect_temp_capacity) {
    size_t new_cap =
        context->indirect_temp_capacity ? context->indirect_temp_capacity * 2 : 8;
    char **g_names = realloc(context->indirect_temp_names,
                             new_cap * sizeof(char *));
    if (!g_names) return 0;
    context->indirect_temp_names = g_names;
    size_t *g_sizes = realloc(context->indirect_temp_sizes,
                              new_cap * sizeof(size_t));
    if (!g_sizes) return 0;
    context->indirect_temp_sizes = g_sizes;
    context->indirect_temp_capacity = new_cap;
  }
  context->indirect_temp_names[context->indirect_temp_count] = (char *)name;
  context->indirect_temp_sizes[context->indirect_temp_count] = size;
  context->indirect_temp_count++;
  return 1;
}

size_t binary_indirect_temp_get(BinaryFunctionContext *context,
                                       const char *name) {
  if (!context || !name) return 0;
  for (size_t i = 0; i < context->indirect_temp_count; i++) {
    const char *n = context->indirect_temp_names[i];
    if (n == name || (n && strcmp(n, name) == 0)) {
      return context->indirect_temp_sizes[i];
    }
  }
  return 0;
}

int code_generator_binary_parameter_is_indirect(
    CodeGenerator *generator, BinaryFunctionContext *context, const char *name) {
  if (!context || !name) {
    return 0;
  }

  Symbol *symbol = generator && generator->symbol_table
                       ? symbol_table_lookup(generator->symbol_table, name)
                       : NULL;
  if (symbol && symbol->kind == SYMBOL_PARAMETER &&
      symbol->data.variable.is_indirect_param) {
    return 1;
  }

  FunctionDeclaration *function_data = context->function_data;
  if (!function_data || !function_data->parameter_names ||
      !function_data->parameter_types) {
    return 0;
  }

  for (size_t i = 0; i < function_data->parameter_count; i++) {
    const char *parameter_name = function_data->parameter_names[i];
    if (parameter_name && strcmp(parameter_name, name) == 0) {
      Type *parameter_type = code_generator_binary_get_resolved_type(
          generator, function_data->parameter_types[i], 0);
      return code_generator_abi_classify(parameter_type) == ABI_PASS_INDIRECT;
    }
  }

  return 0;
}

int code_generator_binary_emit_struct_destination_address(
    CodeGenerator *generator, BinaryFunctionContext *context, const char *name,
    BinaryGpRegister target_register) {
  if (!generator || !context || !name || name[0] == '\0') {
    return 0;
  }

  int param_offset = code_generator_binary_get_parameter_offset(context, name);
  if (param_offset > 0) {
    if (code_generator_binary_parameter_is_indirect(generator, context, name)) {
      return binary_emit_mov_reg_mem(&context->code, target_register,
                                     BINARY_GP_RBP, -param_offset);
    }
    return binary_emit_lea_reg_mem(&context->code, target_register,
                                   BINARY_GP_RBP, -param_offset);
  }

  int local_offset = code_generator_binary_get_local_offset(context, name);
  if (local_offset > 0) {
    return binary_emit_lea_reg_mem(&context->code, target_register,
                                   BINARY_GP_RBP, -local_offset);
  }

  Symbol *symbol = generator->symbol_table
                       ? symbol_table_lookup(generator->symbol_table, name)
                       : NULL;
  if (symbol && symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
    const char *resolved = code_generator_get_link_symbol_name(generator, name);
    if (!resolved) {
      code_generator_set_error(generator,
                               "Invalid global symbol for struct destination");
      return 0;
    }
    return code_generator_binary_emit_symbol_address(
        generator, context, resolved, symbol->is_extern, target_register);
  }

  code_generator_set_error(
      generator, "Cannot resolve address of struct destination '%s' in function '%s'",
      name, context->function_name);
  return 0;
}

/* Load the address of an INDIRECT struct operand (arg or return) into
 * `target_register`. */
int code_generator_binary_emit_indirect_source_address(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand, BinaryGpRegister target_register) {
  if (!generator || !context || !operand) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_SYMBOL) {
    if (!operand->name) {
      code_generator_set_error(generator,
                               "Malformed IR symbol operand (indirect arg)");
      return 0;
    }
    int param_offset =
        code_generator_binary_get_parameter_offset(context, operand->name);
    if (param_offset > 0) {
      if (code_generator_binary_parameter_is_indirect(generator, context,
                                                     operand->name)) {
        return binary_emit_mov_reg_mem(&context->code, target_register,
                                       BINARY_GP_RBP, -param_offset);
      }
      return binary_emit_lea_reg_mem(&context->code, target_register,
                                     BINARY_GP_RBP, -param_offset);
    }
    int local_offset = code_generator_binary_get_local_offset(context,
                                                              operand->name);
    if (local_offset > 0) {
      return binary_emit_lea_reg_mem(&context->code, target_register,
                                     BINARY_GP_RBP, -local_offset);
    }
    Symbol *symbol = symbol_table_lookup(generator->symbol_table, operand->name);
    if (!symbol) {
      code_generator_set_error(generator,
                               "Unknown symbol '%s' for indirect call arg",
                               operand->name);
      return 0;
    }
    if (symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
      const char *resolved =
          code_generator_get_link_symbol_name(generator, operand->name);
      if (!resolved) {
        code_generator_set_error(generator,
                                 "Invalid global symbol for indirect arg");
        return 0;
      }
      return code_generator_binary_emit_symbol_address(
          generator, context, resolved, symbol->is_extern, target_register);
    }
    code_generator_set_error(
        generator,
        "Cannot resolve address of struct symbol '%s' in function '%s'",
        operand->name, context->function_name);
    return 0;
  }
  if (operand->kind == IR_OPERAND_TEMP) {
    if (!operand->name) {
      code_generator_set_error(generator,
                               "Malformed IR temp operand (indirect arg)");
      return 0;
    }
    /* If the temp is tagged as an indirect-return pointer, the temp's slot
     * holds the value (a pointer); load it. Otherwise take its address. */
    int offset =
        code_generator_binary_get_temp_offset(context, operand->name);
    if (offset <= 0) {
      code_generator_set_error(generator,
                               "Unknown IR temp '%s' for indirect arg",
                               operand->name);
      return 0;
    }
    if (binary_indirect_temp_get(context, operand->name) > 0) {
      return binary_emit_mov_reg_mem(&context->code, target_register,
                                     BINARY_GP_RBP, -offset);
    }
    return binary_emit_lea_reg_mem(&context->code, target_register,
                                   BINARY_GP_RBP, -offset);
  }
  code_generator_set_error(
      generator, "Indirect call argument must be a struct value (kind=%d)",
      operand->kind);
  return 0;
}

/* Emit `rep movsb` of `size` bytes from [src_addr_reg] to [dst_addr_reg].
 * Preserves RSI/RDI because the register promoter may keep live values there;
 * RCX is consumed as the string count. */
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
      /* cld (DF=0) — ensure forward direction. One byte 0xFC. */
      !binary_code_buffer_append_u8(&context->code, 0xFC) ||
      /* rep movsb: 0xF3 0xA4. */
      !binary_code_buffer_append_u8(&context->code, 0xF3) ||
      !binary_code_buffer_append_u8(&context->code, 0xA4) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RDI) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RSI)) {
    return 0;
  }
  return 1;
}

/* rep movsq: RCX = qword count, RSI/RDI = src/dst. Requires 8-byte alignment
 * for correctness on strict platforms; benchmark buffers are int32-aligned. */
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
    const char *symbol_name, Type *type, int declare_external,
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
    const char *symbol_name, Type *type, int declare_external,
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

static int code_generator_binary_promoted_symbol_is_global(
    CodeGenerator *generator, const char *name, Symbol **symbol_out) {
  Symbol *symbol = generator && generator->symbol_table && name
                       ? symbol_table_lookup(generator->symbol_table, name)
                       : NULL;
  if (symbol_out) {
    *symbol_out = symbol;
  }
  return symbol && symbol->scope && symbol->scope->type == SCOPE_GLOBAL;
}

int code_generator_binary_emit_promoted_global_loads(
    CodeGenerator *generator, BinaryFunctionContext *context) {
  if (!generator || !context) {
    return 0;
  }

  for (size_t i = 0; i < context->register_symbols.count; i++) {
    const char *name = context->register_symbols.items[i].name;
    BinaryGpRegister reg =
        (BinaryGpRegister)context->register_symbols.items[i].offset;
    Symbol *symbol = NULL;
    if (!code_generator_binary_promoted_symbol_is_global(generator, name,
                                                         &symbol)) {
      continue;
    }

    const char *link_name = code_generator_get_link_symbol_name(generator, name);
    if (!link_name || link_name[0] == '\0' ||
        !code_generator_binary_emit_global_symbol_load(
            generator, context, link_name, symbol->type, symbol->is_extern,
            reg)) {
      if (!generator->has_error) {
        code_generator_set_error(
            generator, "Out of memory while loading promoted global '%s'",
            name ? name : "<unnamed>");
      }
      return 0;
    }
  }

  return 1;
}

int code_generator_binary_emit_promoted_global_stores(
    CodeGenerator *generator, BinaryFunctionContext *context) {
  if (!generator || !context) {
    return 0;
  }

  for (size_t i = 0; i < context->register_symbols.count; i++) {
    const char *name = context->register_symbols.items[i].name;
    BinaryGpRegister reg =
        (BinaryGpRegister)context->register_symbols.items[i].offset;
    Symbol *symbol = NULL;
    if (!code_generator_binary_promoted_symbol_is_global(generator, name,
                                                         &symbol)) {
      continue;
    }

    const char *link_name = code_generator_get_link_symbol_name(generator, name);
    if (!link_name || link_name[0] == '\0' ||
        !code_generator_binary_emit_global_symbol_store(
            generator, context, link_name, symbol->type, symbol->is_extern,
            reg)) {
      if (!generator->has_error) {
        code_generator_set_error(
            generator, "Out of memory while storing promoted global '%s'",
            name ? name : "<unnamed>");
      }
      return 0;
    }
  }

  return 1;
}

int code_generator_binary_operand_is_known_float64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand) {
  Symbol *symbol = NULL;

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

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && generator &&
      generator->symbol_table) {
    symbol = symbol_table_lookup(generator->symbol_table, operand->name);
    return symbol && code_generator_binary_resolved_type_is_float64(symbol->type);
  }

  return 0;
}

/* IEEE-754 width of a value operand: 32, 64, or 0 (not floating). Resolution
 * order: the operand's own IR-carried float_bits (authoritative, set by
 * ir_lowering), then a width recorded for the named symbol/temp, then the
 * declared symbol type. This is the single place backends ask "what float
 * precision is this value" so single vs double is never re-guessed ad hoc. */
int code_generator_binary_operand_float_bits(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand) {
  Symbol *symbol = NULL;

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

  if (operand->kind == IR_OPERAND_SYMBOL && operand->name && generator &&
      generator->symbol_table) {
    symbol = symbol_table_lookup(generator->symbol_table, operand->name);
    if (symbol) {
      return code_generator_binary_resolved_type_float_bits(symbol->type);
    }
  }

  return 0;
}

int code_generator_binary_instruction_result_is_float64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  Symbol *symbol = NULL;
  Type *function_type = NULL;
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
    /* Any floating return (float32 or float64) lands in XMM0 and must mark the
     * dest temp as float so downstream loads bit-copy via movd/movq instead of
     * treating the slot as an integer. resolved_type_float_bits returns 32 for
     * float32, which result_float_bits then narrows correctly. Using the
     * float64-only predicate here dropped float32 returns to the integer path
     * and lost the value. */
    symbol = generator && generator->symbol_table && instruction->text
                 ? symbol_table_lookup(generator->symbol_table,
                                       instruction->text)
                 : NULL;
    return symbol && symbol->kind == SYMBOL_FUNCTION &&
           code_generator_binary_resolved_type_float_bits(
               symbol->data.function.return_type) != 0;

  case IR_OP_CALL_INDIRECT:
    symbol = generator && generator->symbol_table &&
                     instruction->lhs.kind == IR_OPERAND_SYMBOL &&
                     instruction->lhs.name
                 ? symbol_table_lookup(generator->symbol_table,
                                       instruction->lhs.name)
                 : NULL;
    function_type =
        (symbol && symbol->type && symbol->type->kind == TYPE_FUNCTION_POINTER)
            ? symbol->type
            : NULL;
    return code_generator_binary_resolved_type_float_bits(
               function_type ? function_type->fn_return_type : NULL) != 0;

  case IR_OP_CAST:
    return code_generator_binary_named_type_is_float64(generator,
                                                       instruction->text, 0);

  case IR_OP_LOAD:
    /* A value dereferenced from a float* / struct member is floating in the
     * machine sense even though no symbol carries that type. ir_lowering sets
     * is_float on float32/float64 loads; honor it so the destination temp is
     * marked and reaches xmm via movd/movq (bit copy) rather than cvtsi2s*
     * (integer->float conversion of the raw bit pattern). */
    return instruction->is_float;

  default:
    return 0;
  }
}

/* Float width (0/32/64) of an instruction's destination value. Generalizes
 * code_generator_binary_instruction_result_is_float64 so the symbol-marking
 * pass can record single vs double precision per temp/symbol. */
int code_generator_binary_instruction_result_float_bits(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  Symbol *symbol = NULL;
  Type *function_type = NULL;

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
    symbol = generator && generator->symbol_table && instruction->text
                 ? symbol_table_lookup(generator->symbol_table,
                                       instruction->text)
                 : NULL;
    return (symbol && symbol->kind == SYMBOL_FUNCTION)
               ? code_generator_binary_resolved_type_float_bits(
                     symbol->data.function.return_type)
               : 64;

  case IR_OP_CALL_INDIRECT:
    symbol = generator && generator->symbol_table &&
                     instruction->lhs.kind == IR_OPERAND_SYMBOL &&
                     instruction->lhs.name
                 ? symbol_table_lookup(generator->symbol_table,
                                       instruction->lhs.name)
                 : NULL;
    function_type =
        (symbol && symbol->type && symbol->type->kind == TYPE_FUNCTION_POINTER)
            ? symbol->type
            : NULL;
    return function_type ? code_generator_binary_resolved_type_float_bits(
                               function_type->fn_return_type)
                         : 64;

  case IR_OP_CAST: {
    Type *t = generator && generator->type_checker
                  ? type_checker_get_type_by_name(generator->type_checker,
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
    const char *symbol_name, const Symbol *symbol,
    BinaryGpRegister target_register) {
  int offset = 0;

  if (!generator || !context || !symbol_name || symbol_name[0] == '\0') {
    return 0;
  }

  offset = code_generator_binary_get_symbol_offset(context, symbol_name);
  if (offset > 0) {
    if (symbol && symbol->kind == SYMBOL_PARAMETER) {
      return binary_emit_mov_reg_mem(&context->code, target_register,
                                     BINARY_GP_RBP, -offset);
    }
    return binary_emit_lea_reg_mem(&context->code, target_register,
                                   BINARY_GP_RBP, -offset);
  }

  if (symbol && symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
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

/* Materialize an operand into an XMM register at the requested precision
 * (want_bits = 32 or 64).
 *   - A floating operand carries raw IEEE-754 bits in RAX: copy them with
 *     movd (32) or movq (64) according to the operand's OWN width, then
 *     widen/narrow to want_bits with cvtss2sd / cvtsd2ss if they differ.
 *   - An integer operand is converted to float with cvtsi2ss / cvtsi2sd at
 *     want_bits (matches the surrounding float expression's precision). */
int code_generator_binary_emit_float_operand_to_xmm_bits(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand, BinaryXmmRegister target_register,
    int want_bits) {
  int operand_bits = 0;

  if (!generator || !context || !operand) {
    return 0;
  }
  if (want_bits != 32 && want_bits != 64) {
    want_bits = 64;
  }

  if (!code_generator_binary_emit_operand_load(generator, context, operand,
                                               BINARY_GP_RAX)) {
    return 0;
  }

  operand_bits =
      code_generator_binary_operand_float_bits(generator, context, operand);

  if (operand_bits == 32) {
    if (!binary_emit_movd_xmm_reg(&context->code, target_register,
                                  BINARY_GP_RAX)) {
      return 0;
    }
    if (want_bits == 64) {
      return binary_emit_cvtss2sd_xmm_xmm(&context->code, target_register,
                                          target_register);
    }
    return 1;
  }

  if (operand_bits == 64) {
    if (!binary_emit_movq_xmm_reg(&context->code, target_register,
                                  BINARY_GP_RAX)) {
      return 0;
    }
    if (want_bits == 32) {
      return binary_emit_cvtsd2ss_xmm_xmm(&context->code, target_register,
                                          target_register);
    }
    return 1;
  }

  /* Integer value used in a float context: convert at the target precision. */
  if (want_bits == 32) {
    return binary_emit_cvtsi2ss_xmm_reg(&context->code, target_register,
                                        BINARY_GP_RAX);
  }
  return binary_emit_cvtsi2sd_xmm_reg(&context->code, target_register,
                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_float_operand_to_xmm(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand, BinaryXmmRegister target_register) {
  return code_generator_binary_emit_float_operand_to_xmm_bits(
      generator, context, operand, target_register, 64);
}

/* Reinterpret the float bits held in `gp_register` from src_bits precision to
 * dst_bits precision, in place, using XMM0 as scratch. No-op when the widths
 * already match or either side is not a float (src/dst 0). Used by ASSIGN /
 * STORE / RETURN when a float64 value lands in a float32 slot or vice versa. */
int code_generator_binary_emit_float_reg_convert(
    BinaryFunctionContext *context, BinaryGpRegister gp_register,
    int src_bits, int dst_bits) {
  if (!context || src_bits == 0 || dst_bits == 0 || src_bits == dst_bits) {
    return 1;
  }

  if (src_bits == 64 && dst_bits == 32) {
    return binary_emit_movq_xmm_reg(&context->code, BINARY_XMM0,
                                    gp_register) &&
           binary_emit_cvtsd2ss_xmm_xmm(&context->code, BINARY_XMM0,
                                        BINARY_XMM0) &&
           binary_emit_movd_reg_xmm(&context->code, gp_register, BINARY_XMM0);
  }
  if (src_bits == 32 && dst_bits == 64) {
    return binary_emit_movd_xmm_reg(&context->code, BINARY_XMM0,
                                    gp_register) &&
           binary_emit_cvtss2sd_xmm_xmm(&context->code, BINARY_XMM0,
                                        BINARY_XMM0) &&
           binary_emit_movq_reg_xmm(&context->code, gp_register, BINARY_XMM0);
  }
  return 1;
}

int code_generator_binary_emit_operand_load(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand, BinaryGpRegister target_register) {
  if (!generator || !context || !operand) {
    return 0;
  }

  switch (operand->kind) {
  case IR_OPERAND_NONE:
    return binary_emit_mov_reg_imm64(&context->code, target_register, 0);

  case IR_OPERAND_INT:
    return binary_emit_mov_reg_imm64(&context->code, target_register,
                                     (uint64_t)operand->int_value);

  case IR_OPERAND_FLOAT: {
    if (operand->float_bits == 32) {
      /* Materialize the true 32-bit IEEE-754 single pattern (zero-extended).
       * Encoding it as the low half of a double would store 0 for most
       * values. */
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
        target_register);

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
    Symbol *symbol = generator && generator->symbol_table
                         ? symbol_table_lookup(generator->symbol_table,
                                               operand->name)
                         : NULL;
    Type *load_type = symbol ? symbol->type
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
    if (symbol && symbol->type && symbol->type->kind == TYPE_STRING) {
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
      if (symbol && symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
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
    /* A local's symbol is usually out of scope in the symbol table by codegen
     * time (the scope was popped), so symbol_table_lookup returns NULL and the
     * stack load would default to a signed 8-byte read — sign-extending a
     * narrow unsigned local (e.g. uint32) and corrupting its value. Resolve the
     * type from the IR (parameter signature / DECLARE_LOCAL) so the load uses
     * the correct width and signedness. */
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
  BinaryGpRegister dst_reg = BINARY_GP_RDI;
  BinaryGpRegister src_reg = BINARY_GP_RSI;

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

static int code_generator_binary_emit_memset_call_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  if (!generator || !context || !instruction ||
      instruction->argument_count != 3 || !instruction->arguments) {
    return 0;
  }

  if (!binary_emit_push_reg(&context->code, BINARY_GP_RDI) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RCX) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RDI, BINARY_GP_R10) ||
      !binary_code_buffer_append_u8(&context->code, 0xFC) ||
      !binary_code_buffer_append_u8(&context->code, 0xF3) ||
      !binary_code_buffer_append_u8(&context->code, 0xAA) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RAX, BINARY_GP_R10) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RDI)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int code_generator_binary_emit_memcpy_call_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  if (!generator || !context || !instruction ||
      instruction->argument_count != 3 || !instruction->arguments) {
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_R11) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RCX) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RAX, BINARY_GP_R10) ||
      !binary_emit_push_reg(&context->code, BINARY_GP_RSI) ||
      !binary_emit_push_reg(&context->code, BINARY_GP_RDI) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RDI, BINARY_GP_R10) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RSI, BINARY_GP_R11) ||
      !binary_code_buffer_append_u8(&context->code, 0xFC) ||
      !binary_code_buffer_append_u8(&context->code, 0xF3) ||
      !binary_code_buffer_append_u8(&context->code, 0xA4) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RDI) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RSI)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int code_generator_binary_emit_memmove_call_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t j_done = 0;
  size_t j_forward = 0;
  size_t j_after_backward = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count != 3 || !instruction->arguments) {
    return 0;
  }

  b = &context->code;
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_R11) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RCX) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !binary_emit_cmp_reg_imm32(b, BINARY_GP_RCX, 0) ||
      !wcs_jcc(b, 0x84 /* je */, &j_done) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_R10, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x84 /* je */, &j_done) ||
      !wcs_jcc(b, 0x82 /* jb */, &j_forward) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R8, BINARY_GP_R11) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_forward) ||
      !binary_emit_push_reg(b, BINARY_GP_RSI) ||
      !binary_emit_push_reg(b, BINARY_GP_RDI) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RDI, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RDI, BINARY_GP_RCX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDI, 1, 1) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RSI, BINARY_GP_R11) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RSI, BINARY_GP_RCX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RSI, 1, 1) ||
      !binary_code_buffer_append_u8(b, 0xFD) ||
      !binary_code_buffer_append_u8(b, 0xF3) ||
      !binary_code_buffer_append_u8(b, 0xA4) ||
      !binary_code_buffer_append_u8(b, 0xFC) ||
      !binary_emit_pop_reg(b, BINARY_GP_RDI) ||
      !binary_emit_pop_reg(b, BINARY_GP_RSI) ||
      !wcs_jcc(b, 0, &j_after_backward) ||
      !wcs_patch_here(b, j_forward) ||
      !binary_emit_push_reg(b, BINARY_GP_RSI) ||
      !binary_emit_push_reg(b, BINARY_GP_RDI) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RDI, BINARY_GP_R10) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RSI, BINARY_GP_R11) ||
      !binary_code_buffer_append_u8(b, 0xFC) ||
      !binary_code_buffer_append_u8(b, 0xF3) ||
      !binary_code_buffer_append_u8(b, 0xA4) ||
      !binary_emit_pop_reg(b, BINARY_GP_RDI) ||
      !binary_emit_pop_reg(b, BINARY_GP_RSI) ||
      !wcs_patch_here(b, j_after_backward) ||
      !wcs_patch_here(b, j_done)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int code_generator_binary_emit_memcmp_call_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_diff = 0;
  size_t j_back = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count != 3 || !instruction->arguments) {
    return 0;
  }

  b = &context->code;
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_R11) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RCX) ||
      !binary_emit_xor_reg_reg32(b, BINARY_GP_RAX)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_RCX, 0) ||
      !wcs_jcc(b, 0x84 /* je */, &j_done) ||
      !binary_emit_movzx_reg_mem8(b, BINARY_GP_R8, BINARY_GP_R10, 0) ||
      !binary_emit_movzx_reg_mem8(b, BINARY_GP_R9, BINARY_GP_R11, 0) ||
      !binary_emit_cmp_reg_reg32(b, BINARY_GP_R8, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x85 /* jne */, &j_diff) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R10, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 1, 1) ||
      !wcs_jcc(b, 0, &j_back) ||
      !wcs_patch_to(b, j_back, loop_top) ||
      !wcs_patch_here(b, j_diff) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_R9) ||
      !wcs_patch_here(b, j_done)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int binary_emit_windows_heap_free_value(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister ptr_register) {
  size_t get_heap_displacement_offset = 0;
  size_t heap_free_displacement_offset = 0;

  if (!code_generator_binary_declare_external_symbol(generator,
                                                     "GetProcessHeap") ||
      !code_generator_binary_declare_external_symbol(generator, "HeapFree")) {
    return 0;
  }

  return binary_emit_push_reg(&context->code, ptr_register) &&
         binary_emit_sub_rsp_imm32(&context->code,
                                   BINARY_WIN64_SHADOW_SPACE_SIZE) &&
         binary_emit_call_placeholder(&context->code,
                                      &get_heap_displacement_offset) &&
         binary_call_relocation_table_add(&context->call_relocations,
                                          "GetProcessHeap",
                                          get_heap_displacement_offset) &&
         binary_emit_add_rsp_imm32(&context->code,
                                   BINARY_WIN64_SHADOW_SPACE_SIZE) &&
         binary_emit_pop_reg(&context->code, BINARY_GP_R8) &&
         binary_emit_mov_reg_reg(&context->code, BINARY_GP_RCX,
                                 BINARY_GP_RAX) &&
         binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RDX, 0) &&
         binary_emit_sub_rsp_imm32(&context->code,
                                   BINARY_WIN64_SHADOW_SPACE_SIZE + 8) &&
         binary_emit_call_placeholder(&context->code,
                                      &heap_free_displacement_offset) &&
         binary_call_relocation_table_add(&context->call_relocations,
                                          "HeapFree",
                                          heap_free_displacement_offset) &&
         binary_emit_add_rsp_imm32(&context->code,
                                   BINARY_WIN64_SHADOW_SPACE_SIZE + 8);
}

static int code_generator_binary_emit_malloc_call_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  if (!generator || !context || !instruction ||
      instruction->argument_count != 1 || !instruction->arguments) {
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !binary_emit_windows_heap_alloc(generator, context, BINARY_GP_R8, 0)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int code_generator_binary_emit_calloc_call_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  if (!generator || !context || !instruction ||
      instruction->argument_count != 2 || !instruction->arguments) {
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_R10) ||
      !binary_emit_imul_reg_reg(&context->code, BINARY_GP_R8, BINARY_GP_R10) ||
      !binary_emit_windows_zeroed_heap_alloc(generator, context,
                                             BINARY_GP_R8)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int code_generator_binary_emit_free_call_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  char *done_label = NULL;
  size_t fixup = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count != 1 || !instruction->arguments) {
    return 0;
  }

  done_label = code_generator_generate_label(generator, "free_done");
  if (!done_label) {
    code_generator_set_error(generator,
                             "Out of memory while creating free label");
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R10) ||
      !binary_emit_cmp_reg_imm32(&context->code, BINARY_GP_R10, 0) ||
      !binary_emit_je_placeholder(&context->code, &fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, done_label,
                                    fixup) ||
      !binary_emit_windows_heap_free_value(generator, context, BINARY_GP_R10) ||
      !binary_label_table_define(&context->labels, done_label,
                                 context->code.size)) {
    free(done_label);
    return 0;
  }

  free(done_label);
  return 1;
}

static int code_generator_binary_emit_realloc_call_inline(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  char *malloc_label = NULL;
  char *free_label = NULL;
  char *done_label = NULL;
  size_t fixup = 0;
  size_t get_heap_displacement_offset = 0;
  size_t heap_realloc_displacement_offset = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count != 2 || !instruction->arguments) {
    return 0;
  }

  malloc_label = code_generator_generate_label(generator, "realloc_malloc");
  free_label = code_generator_generate_label(generator, "realloc_free");
  done_label = code_generator_generate_label(generator, "realloc_done");
  if (!malloc_label || !free_label || !done_label) {
    free(malloc_label);
    free(free_label);
    free(done_label);
    code_generator_set_error(generator,
                             "Out of memory while creating realloc labels");
    return 0;
  }

  if (!code_generator_binary_declare_external_symbol(generator,
                                                     "GetProcessHeap") ||
      !code_generator_binary_declare_external_symbol(generator,
                                                     "HeapReAlloc")) {
    free(malloc_label);
    free(free_label);
    free(done_label);
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_R11) ||
      !binary_emit_cmp_reg_imm32(&context->code, BINARY_GP_R10, 0) ||
      !binary_emit_je_placeholder(&context->code, &fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, malloc_label,
                                    fixup) ||
      !binary_emit_cmp_reg_imm32(&context->code, BINARY_GP_R11, 0) ||
      !binary_emit_je_placeholder(&context->code, &fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, free_label,
                                    fixup) ||
      !binary_emit_push_reg(&context->code, BINARY_GP_R10) ||
      !binary_emit_push_reg(&context->code, BINARY_GP_R11) ||
      !binary_emit_sub_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE + 8) ||
      !binary_emit_call_placeholder(&context->code,
                                    &get_heap_displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations,
                                        "GetProcessHeap",
                                        get_heap_displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE + 8) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_R9) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_R8) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RCX, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RDX, 0) ||
      !binary_emit_sub_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE + 8) ||
      !binary_emit_call_placeholder(&context->code,
                                    &heap_realloc_displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations,
                                        "HeapReAlloc",
                                        heap_realloc_displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE + 8) ||
      !binary_emit_jmp_placeholder(&context->code, &fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, done_label,
                                    fixup) ||
      !binary_label_table_define(&context->labels, malloc_label,
                                 context->code.size) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_R8, BINARY_GP_R11) ||
      !binary_emit_windows_heap_alloc(generator, context, BINARY_GP_R8, 0) ||
      !binary_emit_jmp_placeholder(&context->code, &fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, done_label,
                                    fixup) ||
      !binary_label_table_define(&context->labels, free_label,
                                 context->code.size) ||
      !binary_emit_windows_heap_free_value(generator, context, BINARY_GP_R10) ||
      !binary_emit_xor_reg_reg32(&context->code, BINARY_GP_RAX) ||
      !binary_label_table_define(&context->labels, done_label,
                                 context->code.size)) {
    free(malloc_label);
    free(free_label);
    free(done_label);
    return 0;
  }

  free(malloc_label);
  free(free_label);
  free(done_label);
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_call_argument_load(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand, Type *parameter_type,
    BinaryGpRegister target_register) {
  Type *operand_type = NULL;

  if (!generator || !context || !operand) {
    return 0;
  }

  if (code_generator_binary_type_is_cstring(parameter_type) &&
      operand->kind == IR_OPERAND_STRING) {
    return code_generator_binary_emit_cstring_literal_address(
        generator, context, operand->name ? operand->name : "",
        target_register);
  }

  if (!code_generator_binary_emit_operand_load(generator, context, operand,
                                               target_register)) {
    return 0;
  }

  operand_type = code_generator_binary_get_operand_type(generator, operand);
  if (code_generator_binary_type_is_cstring(parameter_type) &&
      code_generator_binary_type_is_string(operand_type)) {
    return binary_emit_mov_reg_mem(&context->code, target_register,
                                   target_register, 0);
  }

  return 1;
}

/* Load a float register-argument and place it in its Win64 XMM parameter
 * register at the parameter's precision. param_fbits is 32 or 64. The raw
 * IEEE bits arrive in RAX; movd transfers a single, movq a double. */
int code_generator_binary_emit_float_call_argument(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand, Type *parameter_type, int param_fbits,
    BinaryXmmRegister xmm_register) {
  /* The argument may be stored at a DIFFERENT precision than the parameter
   * expects. In particular every float binary-op result is tracked as float64
   * (instruction_result_is_float64), so `f32_param(a / b)` produces a double
   * temp; movd-ing its low 32 bits reads 0 for values like 1.25/2.0/8.0 (whose
   * double low word is zero). Move the raw bits into the xmm at the OPERAND's
   * stored precision, then convert to the parameter's precision. */
  int operand_fbits =
      code_generator_binary_operand_float_bits(generator, context, operand);
  if (operand_fbits != 32 && operand_fbits != 64) {
    operand_fbits = param_fbits; /* unknown: assume it matches the parameter */
  }
  if (!code_generator_binary_emit_call_argument_load(
          generator, context, operand, parameter_type, BINARY_GP_RAX)) {
    return 0;
  }
  if (operand_fbits == 32) {
    if (!binary_emit_movd_xmm_reg(&context->code, xmm_register,
                                  BINARY_GP_RAX)) {
      return 0;
    }
    if (param_fbits == 64) {
      return binary_emit_cvtss2sd_xmm_xmm(&context->code, xmm_register,
                                          xmm_register);
    }
    return 1;
  }
  /* operand is stored as a 64-bit double */
  if (!binary_emit_movq_xmm_reg(&context->code, xmm_register, BINARY_GP_RAX)) {
    return 0;
  }
  if (param_fbits == 32) {
    return binary_emit_cvtsd2ss_xmm_xmm(&context->code, xmm_register,
                                        xmm_register);
  }
  return 1;
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

static int binary_canonicalize_narrow_reg_for_type(
    BinaryFunctionContext *context, Type *type, BinaryGpRegister reg);

int code_generator_binary_emit_destination_store(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *destination, BinaryGpRegister source_register) {
  if (!generator || !context || !destination) {
    return 0;
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
    Symbol *symbol = generator && generator->symbol_table
                         ? symbol_table_lookup(generator->symbol_table,
                                               destination->name)
                         : NULL;
    /* The symbol table has popped function scope by codegen time, so the
     * lookup returns NULL for locals/params; fall back to the IR-derived type
     * (function signature + DECLARE_LOCAL). Without it a narrow local's store
     * defaults to 8 bytes, losing the type's truncation semantics (and
     * over-writing a 4-byte stack slot). */
    Type *dest_type = symbol && symbol->type
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
    if (symbol && symbol->type && symbol->type->kind == TYPE_STRING) {
      if (offset <= 0) {
        if (symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
          code_generator_set_error(
              generator,
              "Direct object backend does not yet support string global stores "
              "in function '%s'",
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

      if (symbol->kind == SYMBOL_PARAMETER) {
        return binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP, -offset,
                                       source_register);
      }

      return code_generator_binary_emit_local_string_store(
          generator, context, offset, source_register);
    }
    if (code_generator_binary_symbol_assigned_register(
            generator, context, destination->name, &assigned_register)) {
      if (assigned_register == source_register) {
        /* Same register: still canonicalize a narrow value in place. */
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
      if (symbol && symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
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

int code_generator_binary_validate_call(CodeGenerator *generator,
                                               BinaryFunctionContext *context,
                                               const IRInstruction *instruction) {
  if (!generator || !context || !instruction || !instruction->text ||
      instruction->text[0] == '\0') {
    return 0;
  }

  Symbol *symbol = generator->symbol_table
                       ? symbol_table_lookup(generator->symbol_table,
                                             instruction->text)
                       : NULL;
  if (!symbol || symbol->kind != SYMBOL_FUNCTION) {
    return 1;
  }

  if (!code_generator_binary_resolved_type_is_abi_supported(
          symbol->data.function.return_type, 1)) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports integer/pointer/string/float64 call "
        "returns (callee '%s' in function '%s')",
        instruction->text, context->function_name);
    return 0;
  }

  if (instruction->argument_count != symbol->data.function.parameter_count) {
    code_generator_set_error(
        generator,
        "Call argument mismatch while lowering direct object function '%s'",
        context->function_name);
    return 0;
  }

  for (size_t i = 0; i < symbol->data.function.parameter_count; i++) {
    Type *parameter_type = symbol->data.function.parameter_types
                               ? symbol->data.function.parameter_types[i]
                               : NULL;
    if (parameter_type &&
        !code_generator_binary_resolved_type_is_abi_supported(parameter_type, 0)) {
      code_generator_set_error(
          generator,
          "Direct object backend only supports integer/pointer/string/float64 call "
          "arguments (callee '%s' in function '%s')",
          instruction->text, context->function_name);
      return 0;
    }
  }

  return 1;
}

int code_generator_binary_emit_runtime_trap_call(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  char *trap_pc_label = NULL;
  size_t displacement_offset = 0;
  int is_trap_ex =
      instruction && instruction->text &&
      strcmp(instruction->text, "mettle_crash_trap_ex") == 0;
  const char *trap_symbol =
      is_trap_ex ? "mettle_crash_trap_ex" : "mettle_crash_trap";
  size_t message_arg_index = is_trap_ex ? 1u : 0u;

  if (!generator || !context || !instruction ||
      instruction->argument_count == 0) {
    return 0;
  }

  if (!generator->generate_stack_trace_support) {
    const char *puts_symbol = "puts";
    const char *exit_symbol = "exit";
    const IROperand *message_operand =
        instruction->argument_count > message_arg_index
            ? &instruction->arguments[message_arg_index]
            : &instruction->arguments[0];
    /* Abort via libc puts(message) + exit(1). The first argument register and
     * the shadow-space reservation come from the active ABI (MS-x64 RCX + 32B
     * shadow; SysV RDI + no shadow) so the calls into the C runtime are correct
     * on both platforms. */
    const BinaryAbi *abi = code_generator_binary_active_abi();
    BinaryGpRegister arg0 = abi->int_param_registers[0];
    int shadow = abi->shadow_space_size;

    if (!code_generator_binary_declare_external_symbol(generator, puts_symbol) ||
        !code_generator_binary_declare_external_symbol(generator, exit_symbol)) {
      return 0;
    }
    if (message_operand->kind == IR_OPERAND_STRING) {
      if (!code_generator_binary_emit_cstring_literal_address(
              generator, context,
              message_operand->name ? message_operand->name : "", arg0)) {
        return 0;
      }
    } else if (!code_generator_binary_emit_operand_load(
                   generator, context, message_operand, arg0)) {
      return 0;
    }
    if (!binary_emit_sub_rsp_imm32(&context->code, shadow) ||
        !binary_emit_call_placeholder(&context->code, &displacement_offset) ||
        !binary_call_relocation_table_add(&context->call_relocations,
                                          puts_symbol, displacement_offset) ||
        !binary_emit_add_rsp_imm32(&context->code, shadow) ||
        !binary_emit_mov_reg_imm64(&context->code, arg0, 1) ||
        !binary_emit_sub_rsp_imm32(&context->code, shadow) ||
        !binary_emit_call_placeholder(&context->code, &displacement_offset) ||
        !binary_call_relocation_table_add(&context->call_relocations,
                                          exit_symbol, displacement_offset) ||
        !binary_emit_add_rsp_imm32(&context->code, shadow)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting runtime trap "
                                 "call in function '%s'",
                                 context->function_name);
      }
      return 0;
    }
    return 1;
  }

  trap_pc_label = code_generator_generate_label(generator, "mettledbg_trap_pc");
  if (!trap_pc_label) {
    code_generator_set_error(generator,
                             "Out of memory while creating runtime trap label");
    return 0;
  }

  if (!binary_label_table_define(&context->labels, trap_pc_label,
                                 context->code.size)) {
    code_generator_set_error(
        generator,
        "Failed to define runtime trap label in function '%s'",
        context->function_name);
    free(trap_pc_label);
    return 0;
  }

  if (!code_generator_binary_record_debug_label_export(
          context, trap_pc_label, context->code.size)) {
    code_generator_set_error(generator,
                             "Out of memory while recording runtime trap "
                             "label in function '%s'",
                             context->function_name);
    free(trap_pc_label);
    return 0;
  }

  if (instruction->location.line > 0) {
    if (!code_generator_binary_emit_runtime_location_marker(
            generator, context, instruction->location.line,
            instruction->location.column,
            code_generator_runtime_filename(generator,
                                            instruction->location.filename))) {
      free(trap_pc_label);
      return 0;
    }
  }

  if (is_trap_ex && instruction->argument_count >= 4) {
    uint32_t kind = 0;
    const char *message = NULL;
    if (instruction->arguments[0].kind == IR_OPERAND_INT) {
      kind = (uint32_t)instruction->arguments[0].int_value;
    }
    if (instruction->arguments[1].kind == IR_OPERAND_STRING) {
      message = instruction->arguments[1].name;
    }
    code_generator_record_runtime_trap_site(
        generator, trap_pc_label, kind, instruction->location.line,
        instruction->location.column,
        code_generator_runtime_filename(generator,
                                        instruction->location.filename),
        message, NULL);
  }

  if (!code_generator_binary_declare_external_symbol(generator, trap_symbol)) {
    free(trap_pc_label);
    return 0;
  }

  if (is_trap_ex) {
    if (instruction->argument_count < 4 ||
        instruction->arguments[0].kind != IR_OPERAND_INT ||
        instruction->arguments[1].kind != IR_OPERAND_STRING) {
      code_generator_set_error(
          generator,
          "Invalid mettle_crash_trap_ex call in function '%s'",
          context->function_name);
      free(trap_pc_label);
      return 0;
    }

    if (!binary_emit_sub_rsp_imm32(&context->code, 48) ||
        !binary_emit_mov_reg_imm64(
            &context->code, BINARY_GP_RCX,
            (unsigned long long)instruction->arguments[0].int_value) ||
        !code_generator_binary_emit_cstring_literal_address(
            generator, context,
            instruction->arguments[1].name ? instruction->arguments[1].name : "",
            BINARY_GP_RDX) ||
        !binary_emit_lea_reg_rip_placeholder(&context->code, BINARY_GP_R8,
                                             &displacement_offset) ||
        !binary_label_fixup_table_add(&context->label_fixups, trap_pc_label,
                                      displacement_offset) ||
        !binary_emit_mov_reg_reg(&context->code, BINARY_GP_R9, BINARY_GP_RBP)) {
      free(trap_pc_label);
      return 0;
    }

    if (instruction->arguments[2].kind == IR_OPERAND_INT) {
      if (!binary_emit_mov_reg_imm64(
              &context->code, BINARY_GP_RAX,
              (unsigned long long)instruction->arguments[2].int_value) ||
          !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, 32,
                                   BINARY_GP_RAX)) {
        free(trap_pc_label);
        return 0;
      }
    } else if (!code_generator_binary_emit_operand_load(
                   generator, context, &instruction->arguments[2],
                   BINARY_GP_RAX) ||
               !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, 32,
                                        BINARY_GP_RAX)) {
      free(trap_pc_label);
      return 0;
    }

    if (instruction->arguments[3].kind == IR_OPERAND_INT) {
      if (!binary_emit_mov_reg_imm64(
              &context->code, BINARY_GP_RAX,
              (unsigned long long)instruction->arguments[3].int_value) ||
          !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, 40,
                                   BINARY_GP_RAX)) {
        free(trap_pc_label);
        return 0;
      }
    } else if (!code_generator_binary_emit_operand_load(
                   generator, context, &instruction->arguments[3],
                   BINARY_GP_RAX) ||
               !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, 40,
                                        BINARY_GP_RAX)) {
      free(trap_pc_label);
      return 0;
    }

    if (!binary_emit_call_placeholder(&context->code, &displacement_offset) ||
        !binary_call_relocation_table_add(&context->call_relocations,
                                            trap_symbol, displacement_offset) ||
        !binary_emit_add_rsp_imm32(&context->code, 48)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting runtime trap "
                                 "call in function '%s'",
                                 context->function_name);
      }
      free(trap_pc_label);
      return 0;
    }

    free(trap_pc_label);
    return 1;
  }

  if (instruction->arguments[0].kind == IR_OPERAND_STRING) {
    if (!code_generator_binary_emit_cstring_literal_address(
            generator, context,
            instruction->arguments[0].name ? instruction->arguments[0].name
                                             : "",
            BINARY_GP_RCX)) {
      free(trap_pc_label);
      return 0;
    }
  } else if (!code_generator_binary_emit_operand_load(
                 generator, context, &instruction->arguments[0],
                 BINARY_GP_RCX)) {
    free(trap_pc_label);
    return 0;
  }

  if (!binary_emit_lea_reg_rip_placeholder(&context->code, BINARY_GP_RDX,
                                           &displacement_offset) ||
      !binary_label_fixup_table_add(&context->label_fixups, trap_pc_label,
                                    displacement_offset) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_R8, BINARY_GP_RBP) ||
      !binary_emit_sub_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE) ||
      !binary_emit_call_placeholder(&context->code, &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, trap_symbol,
                                        displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE)) {
    if (!generator->has_error) {
      code_generator_set_error(generator,
                               "Out of memory while emitting runtime trap "
                               "call in function '%s'",
                               context->function_name);
    }
    free(trap_pc_label);
    return 0;
  }

  free(trap_pc_label);
  return 1;
}

int code_generator_binary_emit_address_of(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  Symbol *symbol = NULL;
  int offset = 0;
  int is_function_symbol = 0;

  if (!generator || !context || !instruction ||
      instruction->lhs.kind != IR_OPERAND_SYMBOL || !instruction->lhs.name) {
    code_generator_set_error(generator,
                             "IR addr_of requires symbol operand in function "
                             "'%s'",
                             context ? context->function_name : "<unknown>");
    return 0;
  }

  symbol = generator->symbol_table
               ? symbol_table_lookup(generator->symbol_table,
                                     instruction->lhs.name)
               : NULL;
  is_function_symbol =
      (symbol && symbol->kind == SYMBOL_FUNCTION) ||
      code_generator_find_ir_function_binary(generator, instruction->lhs.name) !=
          NULL;

  if (is_function_symbol) {
    const char *link_name =
        code_generator_get_link_symbol_name(generator, instruction->lhs.name);
    if (!link_name || link_name[0] == '\0') {
      code_generator_set_error(generator,
                               "Invalid function symbol in IR addr_of");
      return 0;
    }
    if (!code_generator_binary_emit_symbol_address(
            generator, context, link_name, symbol && symbol->is_extern,
            BINARY_GP_RAX)) {
      return 0;
    }
  } else {
    if (symbol && symbol->type && symbol->type->kind == TYPE_STRING) {
      if (!code_generator_binary_emit_string_symbol_load(
              generator, context, instruction->lhs.name, symbol,
              BINARY_GP_RAX)) {
        return 0;
      }
    } else {
    offset =
        code_generator_binary_get_symbol_offset(context, instruction->lhs.name);
    if (offset > 0) {
      int address_ok =
          code_generator_binary_parameter_is_indirect(
              generator, context, instruction->lhs.name)
              ? binary_emit_mov_reg_mem(&context->code, BINARY_GP_RAX,
                                        BINARY_GP_RBP, -offset)
              : binary_emit_lea_reg_mem(&context->code, BINARY_GP_RAX,
                                        BINARY_GP_RBP, -offset);
      if (!address_ok) {
        code_generator_set_error(
            generator,
            "Out of memory while emitting local address in function '%s'",
            context->function_name);
        return 0;
      }
    } else if (symbol && symbol->scope && symbol->scope->type == SCOPE_GLOBAL) {
      const char *link_name =
          code_generator_get_link_symbol_name(generator, instruction->lhs.name);
      if (!link_name || link_name[0] == '\0') {
        code_generator_set_error(generator,
                                 "Invalid global symbol in IR addr_of");
        return 0;
      }
      if (!code_generator_binary_emit_symbol_address(
              generator, context, link_name, symbol->is_extern,
              BINARY_GP_RAX)) {
        return 0;
      }
    } else {
      code_generator_set_error(generator,
                               "Unknown addr_of symbol '%s' in function '%s'",
                               instruction->lhs.name, context->function_name);
      return 0;
    }
    }
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_load_needs_sign_extend(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *destination, int load_size) {
  Symbol *symbol = NULL;
  (void)context;

  if (load_size != 4 || !destination) {
    return 0;
  }

  if (destination->kind == IR_OPERAND_SYMBOL && destination->name &&
      generator->symbol_table) {
    symbol = symbol_table_lookup(generator->symbol_table, destination->name);
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

int code_generator_binary_emit_load(CodeGenerator *generator,
                                           BinaryFunctionContext *context,
                                           const IRInstruction *instruction) {
  int size = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }

  size = code_generator_binary_get_access_size(generator, context,
                                               &instruction->rhs);
  if (size <= 0) {
    return 0;
  }

  /* When the loaded value's destination is a promoted register DR, land the
   * value (and its sign-extension) directly in DR instead of computing in RAX
   * and copying back. Saves the trailing `mov DR, rax` on every pointer
   * dereference whose result is register-resident (the insertion-sort
   * `current = *prev` is exactly this). Falls back to RAX when the dest is
   * memory-homed. (The address operand is handled just below and may also stay
   * in its own register.) */
  BinaryGpRegister value_register = BINARY_GP_RAX;
  int value_in_dest_register =
      !instruction->is_float && instruction->dest.kind == IR_OPERAND_SYMBOL &&
      instruction->dest.name &&
      code_generator_binary_symbol_assigned_register(
          generator, context, instruction->dest.name, &value_register);
  if (!value_in_dest_register) {
    value_register = BINARY_GP_RAX;
  }

  /* If the address operand is itself a promoted pointer register, dereference
   * it directly rather than copying it into RAX first. (`current = *prev` with
   * prev in a register becomes `mov DR, [prev_reg]`.) The address register is
   * only read, never written by the load, so using it in place is safe even
   * when it differs from the value register. */
  BinaryGpRegister address_register = BINARY_GP_RAX;
  int address_in_register =
      instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name &&
      code_generator_binary_symbol_assigned_register(
          generator, context, instruction->lhs.name, &address_register);
  if (!address_in_register) {
    address_register = BINARY_GP_RAX;
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_RAX)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting IR load in "
                                 "function '%s'",
                                 context->function_name);
      }
      return 0;
    }
  }

  if (!code_generator_binary_emit_load_from_address(generator, context,
                                                    address_register, size,
                                                    value_register)) {
    if (!generator->has_error) {
      code_generator_set_error(generator,
                               "Out of memory while emitting IR load in "
                               "function '%s'",
                               context->function_name);
    }
    return 0;
  }
  /* x86-64: 32-bit integer loads into the low half zero-extend the register.
   * Signed int32 must sign-extend to int64 when held in a 64-bit slot/register.
   * Skip when dest is int32. A load tagged is_unsigned (uint8/16/32 pointee, set
   * at lowering) must stay zero-extended -- without this its high bits get sign-
   * extended and 64-bit ops (compare/divide/(int64) widening) read garbage. */
  if (size == 4 && !instruction->is_float && !instruction->is_unsigned &&
      code_generator_binary_load_needs_sign_extend(generator, context,
                                                   &instruction->dest, size) &&
      !binary_emit_movsxd_reg_reg32(&context->code, value_register,
                                    value_register)) {
    if (!generator->has_error) {
      code_generator_set_error(generator,
                               "Out of memory while emitting IR load in "
                               "function '%s'",
                               context->function_name);
    }
    return 0;
  }

  /* Value already resides in the destination register; no store needed. */
  if (value_in_dest_register) {
    return 1;
  }

  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    if (!generator->has_error) {
      code_generator_set_error(generator,
                               "Out of memory while emitting IR load in "
                               "function '%s'",
                               context->function_name);
    }
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_store(CodeGenerator *generator,
                                            BinaryFunctionContext *context,
                                            const IRInstruction *instruction) {
  int size = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }

  size = code_generator_binary_get_access_size(generator, context,
                                               &instruction->rhs);
  if (size <= 0) {
    return 0;
  }

  /* Materialize the stored value in BINARY_GP_STORE_VALUE by default. If the
   * value operand is itself a promoted, non-float register, store straight
   * from that register and skip the copy (`*scan = current` with current in a
   * register becomes `mov [addr], current_reg`). Never use RCX/RDX/R8/R9 here:
   * optimized IR may keep a reused address temp in an arg register across a
   * preceding load and the following store. */
  BinaryGpRegister value_register = BINARY_GP_STORE_VALUE;
  int value_in_register =
      !instruction->is_float && instruction->lhs.kind == IR_OPERAND_SYMBOL &&
      instruction->lhs.name &&
      code_generator_binary_symbol_assigned_register(
          generator, context, instruction->lhs.name, &value_register);
  if (!value_in_register) {
    value_register = BINARY_GP_STORE_VALUE;
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_STORE_VALUE)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting IR store in "
                                 "function '%s'",
                                 context->function_name);
      }
      return 0;
    }
  }

  /* Narrow/widen the value to the destination's float precision when the
   * stored expression's width differs (e.g. float64 expression -> float32
   * member). instruction->float_bits is the destination width. */
  if (instruction->is_float && instruction->float_bits) {
    int value_bits = code_generator_binary_operand_float_bits(
        generator, context, &instruction->lhs);
    if (value_bits &&
        !code_generator_binary_emit_float_reg_convert(
            context, value_register, value_bits, instruction->float_bits)) {
      code_generator_set_error(generator,
                               "Out of memory while converting float store "
                               "precision in function '%s'",
                               context->function_name);
      return 0;
    }
  }

  /* If the store address is a promoted pointer register, store through it
   * directly instead of copying it into RAX first (`*scan = current` with scan
   * in a register becomes `mov [scan_reg], r10`). The value is already in the
   * store-value register and the address register is only read. */
  BinaryGpRegister store_address_register = BINARY_GP_RAX;
  int store_address_in_register =
      instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
      code_generator_binary_symbol_assigned_register(
          generator, context, instruction->dest.name, &store_address_register);
  if (!store_address_in_register) {
    store_address_register = BINARY_GP_RAX;
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->dest,
                                                 BINARY_GP_RAX)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting IR store in "
                                 "function '%s'",
                                 context->function_name);
      }
      return 0;
    }
  }

  if (!code_generator_binary_emit_store_to_address(generator, context,
                                                   store_address_register, size,
                                                   value_register)) {
    if (!generator->has_error) {
      code_generator_set_error(generator,
                               "Out of memory while emitting IR store in "
                               "function '%s'",
                               context->function_name);
    }
    return 0;
  }

  return 1;
}

static int binary_emit_windows_heap_alloc(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister size_register, uint32_t flags) {
  size_t get_heap_displacement_offset = 0;
  size_t heap_alloc_displacement_offset = 0;

  if (!code_generator_binary_declare_external_symbol(generator,
                                                     "GetProcessHeap") ||
      !code_generator_binary_declare_external_symbol(generator, "HeapAlloc")) {
    return 0;
  }

  if (!binary_emit_push_reg(&context->code, size_register) ||
      !binary_emit_sub_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE) ||
      !binary_emit_call_placeholder(&context->code,
                                    &get_heap_displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations,
                                        "GetProcessHeap",
                                        get_heap_displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_R8) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RCX, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RDX, flags) ||
      !binary_emit_sub_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE + 8) ||
      !binary_emit_call_placeholder(&context->code,
                                    &heap_alloc_displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations,
                                        "HeapAlloc",
                                        heap_alloc_displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE + 8)) {
    return 0;
  }

  return 1;
}

static int binary_emit_windows_zeroed_heap_alloc(
    CodeGenerator *generator, BinaryFunctionContext *context,
    BinaryGpRegister size_register) {
  return binary_emit_windows_heap_alloc(generator, context, size_register, 8);
}

int code_generator_binary_emit_new(CodeGenerator *generator,
                                           BinaryFunctionContext *context,
                                           const IRInstruction *instruction) {
  size_t displacement_offset = 0;
  const char *allocator_name = "calloc";
  const BinaryAbi *abi = code_generator_binary_active_abi();
  BinaryGpRegister count_register = abi->int_param_registers[0];
  BinaryGpRegister size_register = abi->shadow_space_size > 0
                                       ? BINARY_GP_R8
                                       : abi->int_param_registers[1];
  int shadow = abi->shadow_space_size;

  if (!generator || !context || !instruction) {
    return 0;
  }

  if (abi->shadow_space_size == 0 &&
      !code_generator_binary_declare_external_symbol(generator, allocator_name)) {
    return 0;
  }

  if (instruction->rhs.kind == IR_OPERAND_INT && instruction->rhs.int_value > 0) {
    if (!binary_emit_mov_reg_imm64(&context->code, size_register,
                                   (uint64_t)instruction->rhs.int_value)) {
      code_generator_set_error(generator,
                                "Out of memory while emitting allocation size");
      return 0;
    }
  } else if (instruction->rhs.kind == IR_OPERAND_NONE ||
             (instruction->rhs.kind == IR_OPERAND_INT &&
               instruction->rhs.int_value <= 0)) {
    if (!binary_emit_mov_reg_imm64(&context->code, size_register, 8)) {
      code_generator_set_error(generator,
                                "Out of memory while emitting allocation size");
      return 0;
    }
  } else if (!code_generator_binary_emit_operand_load(
                  generator, context, &instruction->rhs, size_register)) {
    return 0;
  }

  if (abi->shadow_space_size > 0) {
    if (!binary_emit_windows_zeroed_heap_alloc(generator, context,
                                              size_register)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting Win32 heap "
                                 "allocation in function '%s'",
                                 context->function_name);
      }
      return 0;
    }
  } else if (!binary_emit_mov_reg_imm64(&context->code, count_register, 1) ||
             !binary_emit_sub_rsp_imm32(&context->code, shadow) ||
             !binary_emit_call_placeholder(&context->code,
                                           &displacement_offset) ||
             !binary_call_relocation_table_add(&context->call_relocations,
                                               allocator_name,
                                               displacement_offset) ||
             !binary_emit_add_rsp_imm32(&context->code, shadow)) {
    if (!generator->has_error) {
      code_generator_set_error(generator,
                               "Out of memory while emitting IR new in "
                               "function '%s'",
                               context->function_name);
    }
    return 0;
  }

  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    if (!generator->has_error) {
      code_generator_set_error(generator,
                               "Out of memory while emitting IR new in "
                               "function '%s'",
                               context->function_name);
    }
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_cast(CodeGenerator *generator,
                                           BinaryFunctionContext *context,
                                           const IRInstruction *instruction) {
  Type *target_type = NULL;
  int target_is_float = 0;
  int target_is_unsigned = 0;
  int target_size = 8;

  if (!generator || !context || !instruction || !instruction->text) {
    return 0;
  }

  target_type = generator->type_checker
                    ? type_checker_get_type_by_name(generator->type_checker,
                                                    instruction->text)
                    : NULL;
  target_is_float =
      target_type ? code_generator_is_floating_point_type(target_type) : 0;
  if (target_type) {
    target_is_unsigned = target_type->kind == TYPE_UINT8 ||
                         target_type->kind == TYPE_UINT16 ||
                         target_type->kind == TYPE_UINT32 ||
                         target_type->kind == TYPE_UINT64;
    target_size = (int)target_type->size;
    if (target_type->kind == TYPE_POINTER ||
        target_type->kind == TYPE_FUNCTION_POINTER) {
      target_size = 8;
    }
  }
  if (target_size <= 0) {
    target_size = 8;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RAX)) {
    return 0;
  }

  /* Source float width is carried on the CAST instruction (set by
   * ir_lowering); target float width is derived from the cast's named type. */
  int src_fbits = (instruction->float_bits == 32) ? 32 : 64;
  int dst_fbits =
      code_generator_binary_resolved_type_float_bits(target_type);

  if (instruction->is_float && !target_is_float) {
    /* float -> int: truncate at the SOURCE precision. */
    if (src_fbits == 32) {
      if (!binary_emit_movd_xmm_reg(&context->code, BINARY_XMM0,
                                    BINARY_GP_RAX) ||
          !binary_emit_cvttss2si_reg_xmm(&context->code, BINARY_GP_RAX,
                                         BINARY_XMM0)) {
        goto emit_failure;
      }
    } else if (!binary_emit_movq_xmm_reg(&context->code, BINARY_XMM0,
                                         BINARY_GP_RAX) ||
               !binary_emit_cvttsd2si_reg_xmm(&context->code, BINARY_GP_RAX,
                                              BINARY_XMM0)) {
      goto emit_failure;
    }
  } else if (!instruction->is_float && target_is_float) {
    /* int -> float: produce a value at the TARGET precision. */
    if (dst_fbits == 32) {
      if (!binary_emit_cvtsi2ss_xmm_reg(&context->code, BINARY_XMM0,
                                        BINARY_GP_RAX) ||
          !binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                    BINARY_XMM0)) {
        goto emit_failure;
      }
    } else if (!binary_emit_cvtsi2sd_xmm_reg(&context->code, BINARY_XMM0,
                                             BINARY_GP_RAX) ||
               !binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                         BINARY_XMM0)) {
      goto emit_failure;
    }
  } else if (instruction->is_float && target_is_float) {
    /* float -> float: convert precision only when the widths differ. */
    if (src_fbits == 32 && dst_fbits == 64) {
      if (!binary_emit_movd_xmm_reg(&context->code, BINARY_XMM0,
                                    BINARY_GP_RAX) ||
          !binary_emit_cvtss2sd_xmm_xmm(&context->code, BINARY_XMM0,
                                        BINARY_XMM0) ||
          !binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                    BINARY_XMM0)) {
        goto emit_failure;
      }
    } else if (src_fbits == 64 && dst_fbits == 32) {
      if (!binary_emit_movq_xmm_reg(&context->code, BINARY_XMM0,
                                    BINARY_GP_RAX) ||
          !binary_emit_cvtsd2ss_xmm_xmm(&context->code, BINARY_XMM0,
                                        BINARY_XMM0) ||
          !binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                    BINARY_XMM0)) {
        goto emit_failure;
      }
    }
    /* same width -> raw bits already correct, nothing to emit. */
  } else if (target_size == 1) {
    if ((target_is_unsigned &&
         !binary_emit_movzx_eax_al(&context->code)) ||
        (!target_is_unsigned &&
         !binary_emit_movsx_rax_al(&context->code))) {
      goto emit_failure;
    }
  } else if (target_size == 2) {
    if ((target_is_unsigned &&
         !binary_emit_movzx_eax_ax(&context->code)) ||
        (!target_is_unsigned &&
         !binary_emit_movsx_rax_ax(&context->code))) {
      goto emit_failure;
    }
  } else if (target_size == 4) {
    if ((target_is_unsigned &&
         !binary_emit_mov_eax_eax(&context->code)) ||
        (!target_is_unsigned &&
         !binary_emit_movsxd_rax_eax(&context->code))) {
      goto emit_failure;
    }
  }

  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    return 0;
  }

  return 1;

emit_failure:
  code_generator_set_error(
      generator,
      "Out of memory while emitting IR cast in function '%s'",
      context->function_name);
  return 0;
}

int code_generator_binary_validate_indirect_call(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  Symbol *symbol = NULL;
  Type *function_type = NULL;

  if (!generator || !context || !instruction ||
      instruction->lhs.kind != IR_OPERAND_SYMBOL || !instruction->lhs.name) {
    return 1;
  }

  symbol = generator->symbol_table
               ? symbol_table_lookup(generator->symbol_table,
                                     instruction->lhs.name)
               : NULL;
  if (!symbol || !symbol->type || symbol->type->kind != TYPE_FUNCTION_POINTER) {
    return 1;
  }

  function_type = symbol->type;
  if (!code_generator_binary_resolved_type_is_abi_supported(
          function_type->fn_return_type, 1)) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports integer/pointer/string/float64 indirect "
        "call returns in function '%s'",
        context->function_name);
    return 0;
  }

  if (instruction->argument_count != function_type->fn_param_count) {
    code_generator_set_error(
        generator,
        "Indirect call argument mismatch while lowering direct object "
        "function '%s'",
        context->function_name);
    return 0;
  }

  for (size_t i = 0; i < function_type->fn_param_count; i++) {
    if (!code_generator_binary_resolved_type_is_abi_supported(
            function_type->fn_param_types[i], 0)) {
      code_generator_set_error(
          generator,
          "Direct object backend only supports integer/pointer/string/float64 "
          "indirect call arguments in function '%s'",
          context->function_name);
      return 0;
    }
  }

  return 1;
}

int code_generator_binary_emit_call(CodeGenerator *generator,
                                           BinaryFunctionContext *context,
                                           const IRInstruction *instruction) {
  Symbol *function_symbol = NULL;
  IRFunction *target_ir_function = NULL;

  if (!generator || !context || !instruction || !instruction->text ||
      instruction->text[0] == '\0') {
    return 0;
  }

  if (strcmp(instruction->text, "mettle_crash_trap") == 0 ||
      strcmp(instruction->text, "mettle_crash_trap_ex") == 0) {
    return code_generator_binary_emit_runtime_trap_call(generator, context,
                                                        instruction);
  }

  if (strcmp(instruction->text, "mettle_profile_enter") == 0) {
    uint32_t profile_id = 0;
    if (instruction->argument_count != 1 ||
        instruction->arguments[0].kind != IR_OPERAND_INT) {
      code_generator_set_error(generator,
                               "Invalid mettle_profile_enter call in '%s'",
                               context->function_name);
      return 0;
    }
    profile_id = (uint32_t)instruction->arguments[0].int_value;
    return code_generator_binary_emit_profile_enter(generator, context,
                                                    profile_id);
  }

  if (strcmp(instruction->text, "mettle_profile_exit") == 0) {
    return code_generator_binary_emit_profile_exit(generator, context);
  }

  if (strcmp(instruction->text, "mettle_profile_op") == 0) {
    uint32_t op_class = 0;
    uint64_t amount = 0;
    if (instruction->argument_count != 2 ||
        instruction->arguments[0].kind != IR_OPERAND_INT ||
        instruction->arguments[1].kind != IR_OPERAND_INT) {
      code_generator_set_error(generator,
                               "Invalid mettle_profile_op call in '%s'",
                               context->function_name);
      return 0;
    }
    op_class = (uint32_t)instruction->arguments[0].int_value;
    amount = (uint64_t)instruction->arguments[1].int_value;
    return code_generator_binary_emit_profile_op(generator, context, op_class,
                                                 amount);
  }

  if (code_generator_binary_active_abi()->shadow_space_size > 0) {
    if (strcmp(instruction->text, "malloc") == 0 &&
        instruction->argument_count == 1) {
      return code_generator_binary_emit_malloc_call_inline(generator, context,
                                                          instruction);
    }

    if (strcmp(instruction->text, "calloc") == 0 &&
        instruction->argument_count == 2) {
      return code_generator_binary_emit_calloc_call_inline(generator, context,
                                                          instruction);
    }

    if (strcmp(instruction->text, "realloc") == 0 &&
        instruction->argument_count == 2) {
      return code_generator_binary_emit_realloc_call_inline(generator, context,
                                                           instruction);
    }

    if (strcmp(instruction->text, "free") == 0 &&
        instruction->argument_count == 1) {
      return code_generator_binary_emit_free_call_inline(generator, context,
                                                        instruction);
    }
  }

  if (strcmp(instruction->text, "memset") == 0 &&
      instruction->argument_count == 3) {
    return code_generator_binary_emit_memset_call_inline(generator, context,
                                                        instruction);
  }

  if (strcmp(instruction->text, "memcpy") == 0 &&
      instruction->argument_count == 3) {
    return code_generator_binary_emit_memcpy_call_inline(generator, context,
                                                        instruction);
  }

  if (strcmp(instruction->text, "memmove") == 0 &&
      instruction->argument_count == 3) {
    return code_generator_binary_emit_memmove_call_inline(generator, context,
                                                         instruction);
  }

  if (strcmp(instruction->text, "memcmp") == 0 &&
      instruction->argument_count == 3) {
    return code_generator_binary_emit_memcmp_call_inline(generator, context,
                                                        instruction);
  }

  if (!code_generator_binary_validate_call(generator, context, instruction)) {
    return 0;
  }

  function_symbol = generator->symbol_table
                        ? symbol_table_lookup(generator->symbol_table,
                                              instruction->text)
                        : NULL;
  target_ir_function =
      code_generator_find_ir_function_binary(generator, instruction->text);

  /* Per-arg INDIRECT classification and per-call indirect-temp region. The
   * region lives at [rsp + 0 .. indirect_temp_region) within the call's
   * sub-rsp window, followed by shadow space and any stack arg slots. */
  size_t argument_count = instruction->argument_count;
  int *is_indirect_arg =
      argument_count > 0 ? calloc(argument_count, sizeof(int)) : NULL;
  int *indirect_arg_offset =
      argument_count > 0 ? calloc(argument_count, sizeof(int)) : NULL;
  size_t *indirect_arg_size =
      argument_count > 0 ? calloc(argument_count, sizeof(size_t)) : NULL;
  if (argument_count > 0 &&
      (!is_indirect_arg || !indirect_arg_offset || !indirect_arg_size)) {
    free(is_indirect_arg);
    free(indirect_arg_offset);
    free(indirect_arg_size);
    code_generator_set_error(generator,
                             "Out of memory planning indirect args");
    return 0;
  }
  int indirect_temp_region = 0;
  for (size_t i = 0; i < argument_count; i++) {
    Type *param_t =
        function_symbol && function_symbol->kind == SYMBOL_FUNCTION &&
                function_symbol->data.function.parameter_types
            ? function_symbol->data.function.parameter_types[i]
            : NULL;
    if (code_generator_abi_classify(param_t) == ABI_PASS_INDIRECT) {
      is_indirect_arg[i] = 1;
      size_t sz = code_generator_abi_type_size(param_t);
      indirect_arg_size[i] = sz;
      indirect_arg_offset[i] = indirect_temp_region;
      indirect_temp_region += (int)((sz + 7u) & ~(size_t)7);
    }
  }
  if (indirect_temp_region > 0) {
    indirect_temp_region = (indirect_temp_region + 15) & ~15;
  }

  /* INDIRECT-return classification. The hidden out-pointer (Win64: rcx)
   * occupies ABI slot 0 and shifts every user arg up by one. */
  Type *call_return_type = NULL;
  if (function_symbol && function_symbol->kind == SYMBOL_FUNCTION) {
    call_return_type = function_symbol->data.function.return_type
                           ? function_symbol->data.function.return_type
                           : function_symbol->type;
  }
  int return_is_indirect =
      (code_generator_abi_classify(call_return_type) == ABI_PASS_INDIRECT) ? 1
                                                                           : 0;
  size_t hidden_arg_count = return_is_indirect ? 1 : 0;
  int return_slot_rbp_offset = 0;
  if (return_is_indirect) {
    if (context->indirect_return_slot_cursor >=
        context->indirect_return_slot_count) {
      free(is_indirect_arg);
      free(indirect_arg_offset);
      free(indirect_arg_size);
      code_generator_set_error(
          generator,
          "Indirect-return frame slot not assigned for call '%s'",
          instruction->text);
      return 0;
    }
    return_slot_rbp_offset = context->indirect_return_slot_offsets
                                 [context->indirect_return_slot_cursor++];
  }

  /* Effective ABI argument count includes the hidden out-pointer. Compute the
   * per-argument layout under the active convention; the hidden out-pointer is
   * a leading integer argument. */
  const BinaryAbi *abi = code_generator_binary_active_abi();
  size_t effective_arg_count = argument_count + hidden_arg_count;
  int *arg_is_float = effective_arg_count > 0
                          ? calloc(effective_arg_count, sizeof(int))
                          : NULL;
  BinaryArgLocation *arg_locations =
      effective_arg_count > 0
          ? calloc(effective_arg_count, sizeof(BinaryArgLocation))
          : NULL;
  if (effective_arg_count > 0 && (!arg_is_float || !arg_locations)) {
    free(is_indirect_arg);
    free(indirect_arg_offset);
    free(indirect_arg_size);
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator, "Out of memory planning call layout");
    return 0;
  }
  for (size_t i = 0; i < argument_count; i++) {
    Type *param_t =
        function_symbol && function_symbol->kind == SYMBOL_FUNCTION &&
                function_symbol->data.function.parameter_types
            ? function_symbol->data.function.parameter_types[i]
            : NULL;
    /* INDIRECT args pass a pointer (integer class). */
    arg_is_float[i + hidden_arg_count] =
        (!is_indirect_arg[i] &&
         code_generator_binary_resolved_type_float_bits(param_t))
            ? 1
            : 0;
  }

  int stack_bytes = 0;
  if (effective_arg_count > 0 &&
      !code_generator_binary_compute_arg_layout(abi, arg_is_float,
                                                effective_arg_count,
                                                arg_locations, &stack_bytes)) {
    free(is_indirect_arg);
    free(indirect_arg_offset);
    free(indirect_arg_size);
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator, "Failed to compute call layout");
    return 0;
  }
  if (stack_bytes > INT_MAX - 64) {
    free(is_indirect_arg);
    free(indirect_arg_offset);
    free(indirect_arg_size);
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator,
                             "Too many call arguments in function '%s'",
                             context->function_name);
    return 0;
  }

  int call_stack_total =
      indirect_temp_region + abi->shadow_space_size + stack_bytes;
  if (!binary_align_up_int(call_stack_total, 16, &call_stack_total)) {
    free(is_indirect_arg);
    free(indirect_arg_offset);
    free(indirect_arg_size);
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator,
                             "Call frame too large in function '%s'",
                             context->function_name);
    return 0;
  }

  if (call_stack_total > 0 &&
      !binary_emit_sub_rsp_imm32(&context->code, (uint32_t)call_stack_total)) {
    free(is_indirect_arg);
    free(indirect_arg_offset);
    free(indirect_arg_size);
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator,
                             "Out of memory while emitting call frame");
    return 0;
  }

  /* Materialize INDIRECT args: memcpy each struct into its per-call temp. */
  for (size_t i = 0; i < argument_count; i++) {
    if (!is_indirect_arg[i]) continue;
    /* src into rax */
    if (!code_generator_binary_emit_indirect_source_address(
            generator, context, &instruction->arguments[i], BINARY_GP_RAX)) {
      free(is_indirect_arg);
      free(indirect_arg_offset);
      free(indirect_arg_size);
      free(arg_is_float);
      free(arg_locations);
      return 0;
    }
    /* dst = lea rdx, [rsp + offset] (offset within indirect_temp_region) */
    if (!binary_emit_lea_reg_mem(&context->code, BINARY_GP_RDX,
                                 BINARY_GP_RSP, indirect_arg_offset[i]) ||
        !code_generator_binary_emit_rep_movsb(
            generator, context, BINARY_GP_RAX, BINARY_GP_RDX,
            indirect_arg_size[i])) {
      free(is_indirect_arg);
      free(indirect_arg_offset);
      free(indirect_arg_size);
      free(arg_is_float);
      free(arg_locations);
      code_generator_set_error(generator,
                               "Out of memory copying INDIRECT call arg");
      return 0;
    }
  }

  /* Stack args: arguments the layout placed on the stack. Their rsp-relative
   * home is the indirect-temp region, then shadow space, then the layout's
   * outgoing offset. */
  for (size_t i = 0; i < argument_count; i++) {
    const BinaryArgLocation *loc = &arg_locations[i + hidden_arg_count];
    if (loc->kind != BINARY_ARG_ON_STACK) continue;
    int slot_offset =
        indirect_temp_region + abi->shadow_space_size + loc->stack_offset;
    if (is_indirect_arg[i]) {
      /* Place &temp into the stack slot. */
      if (!binary_emit_lea_reg_mem(&context->code, BINARY_GP_RAX,
                                   BINARY_GP_RSP, indirect_arg_offset[i]) ||
          !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, slot_offset,
                                   BINARY_GP_RAX)) {
        free(is_indirect_arg);
        free(indirect_arg_offset);
        free(indirect_arg_size);
        free(arg_is_float);
        free(arg_locations);
        code_generator_set_error(generator,
                                 "Out of memory writing INDIRECT stack arg");
        return 0;
      }
      continue;
    }
    Type *parameter_type =
        function_symbol && function_symbol->kind == SYMBOL_FUNCTION &&
                function_symbol->data.function.parameter_types
            ? function_symbol->data.function.parameter_types[i]
            : NULL;
    if (!code_generator_binary_emit_call_argument_load(
            generator, context, &instruction->arguments[i], parameter_type,
            BINARY_GP_RAX) ||
        !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, slot_offset,
                                 BINARY_GP_RAX)) {
      free(is_indirect_arg);
      free(indirect_arg_offset);
      free(indirect_arg_size);
      free(arg_is_float);
      free(arg_locations);
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while materializing call args");
      }
      return 0;
    }
  }

  /* Register args: arguments the layout placed in a GP or XMM register. */
  for (size_t i = 0; i < argument_count; i++) {
    const BinaryArgLocation *loc = &arg_locations[i + hidden_arg_count];
    if (loc->kind == BINARY_ARG_ON_STACK) continue;
    if (is_indirect_arg[i]) {
      /* INDIRECT args always pass a pointer in a GP register. */
      if (loc->kind != BINARY_ARG_IN_GP_REGISTER ||
          !binary_emit_lea_reg_mem(&context->code, loc->gp_register,
                                   BINARY_GP_RSP, indirect_arg_offset[i])) {
        free(is_indirect_arg);
        free(indirect_arg_offset);
        free(indirect_arg_size);
        free(arg_is_float);
        free(arg_locations);
        code_generator_set_error(generator,
                                 "Out of memory loading INDIRECT arg ptr");
        return 0;
      }
      continue;
    }
    Type *parameter_type =
        function_symbol && function_symbol->kind == SYMBOL_FUNCTION &&
                function_symbol->data.function.parameter_types
            ? function_symbol->data.function.parameter_types[i]
            : NULL;
    int param_fbits =
        code_generator_binary_resolved_type_float_bits(parameter_type);
    if ((loc->kind == BINARY_ARG_IN_XMM_REGISTER &&
         !code_generator_binary_emit_float_call_argument(
             generator, context, &instruction->arguments[i], parameter_type,
             param_fbits, loc->xmm_register)) ||
        (loc->kind == BINARY_ARG_IN_GP_REGISTER &&
         !code_generator_binary_emit_call_argument_load(
             generator, context, &instruction->arguments[i], parameter_type,
             loc->gp_register))) {
      free(is_indirect_arg);
      free(indirect_arg_offset);
      free(indirect_arg_size);
      free(arg_is_float);
      free(arg_locations);
      return 0;
    }
  }

  /* The layout is fully consumed; the hidden out-pointer is loaded below from
   * the ABI's dedicated register, not the layout. */
  free(arg_is_float);
  free(arg_locations);
  arg_is_float = NULL;
  arg_locations = NULL;

  /* Hidden out-pointer for INDIRECT return: load &return_slot into the ABI's
   * dedicated out-pointer register (MS-x64 RCX, SysV RDI) LAST, after any
   * user-arg work that may have clobbered it. The slot lives in the caller's
   * function frame, so it survives the call's stack teardown. */
  if (return_is_indirect) {
    if (!binary_emit_lea_reg_mem(&context->code, abi->indirect_return_register,
                                 BINARY_GP_RBP, -return_slot_rbp_offset)) {
      free(is_indirect_arg);
      free(indirect_arg_offset);
      free(indirect_arg_size);
      code_generator_set_error(generator,
                               "Out of memory loading hidden out-ptr");
      return 0;
    }
  }
  free(is_indirect_arg);
  free(indirect_arg_offset);
  free(indirect_arg_size);

  size_t displacement_offset = 0;
  const char *link_target =
      code_generator_get_link_symbol_name(generator, instruction->text);
  if (!link_target || link_target[0] == '\0') {
    code_generator_set_error(generator, "Invalid call target '%s'",
                             instruction->text);
    return 0;
  }

  if (!target_ir_function &&
      !code_generator_binary_declare_external_symbol(generator, link_target)) {
    return 0;
  }

  if (!binary_emit_call_placeholder(&context->code, &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, link_target,
                                        displacement_offset)) {
    code_generator_set_error(generator,
                             "Out of memory while emitting call relocation");
    return 0;
  }

  if (call_stack_total > 0 &&
      !binary_emit_add_rsp_imm32(&context->code, (uint32_t)call_stack_total)) {
    code_generator_set_error(generator,
                             "Out of memory while restoring call frame");
    return 0;
  }

  /* INDIRECT return: rax should hold the slot address by ABI; re-materialize
   * from our known frame slot for safety (some callees may not preserve
   * exactly; the slot lives in our frame so the lea is always correct). */
  if (return_is_indirect) {
    if (!binary_emit_lea_reg_mem(&context->code, BINARY_GP_RAX, BINARY_GP_RBP,
                                 -return_slot_rbp_offset)) {
      code_generator_set_error(generator,
                               "Out of memory materializing INDIRECT result");
      return 0;
    }
    /* Caller-side disposition: if dest is a struct symbol, memcpy into its
     * storage. If dest is a temp, register the temp in the side-table so
     * downstream IR_OP_ASSIGN / indirect-arg consumption knows the temp
     * carries a pointer-to-struct semantics. */
    if (instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name) {
      Symbol *dest_sym =
          symbol_table_lookup(generator->symbol_table, instruction->dest.name);
      if (!dest_sym || !dest_sym->type ||
          code_generator_type_is_aggregate(dest_sym->type)) {
        if (!code_generator_binary_emit_struct_destination_address(
                generator, context, instruction->dest.name, BINARY_GP_RDX)) {
          return 0;
        }
        if (!code_generator_binary_emit_rep_movsb(
                generator, context, BINARY_GP_RAX, BINARY_GP_RDX,
                code_generator_abi_type_size(call_return_type))) {
          code_generator_set_error(
              generator, "Out of memory copying INDIRECT call result");
          return 0;
        }
        return 1;
      }
    }
    if (instruction->dest.kind == IR_OPERAND_TEMP && instruction->dest.name) {
      if (!binary_indirect_temp_add(
              context, instruction->dest.name,
              code_generator_abi_type_size(call_return_type))) {
        code_generator_set_error(generator,
                                 "Out of memory tagging INDIRECT-return temp");
        return 0;
      }
    }
    /* Default store (8-byte spill of the pointer) keeps the pointer alive
     * in the temp's slot for downstream consumers. */
    if (!code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX)) {
      return 0;
    }
    return 1;
  }

  if (function_symbol && function_symbol->kind == SYMBOL_FUNCTION) {
    int ret_fbits = code_generator_binary_resolved_type_float_bits(
        function_symbol->data.function.return_type);
    if (((ret_fbits == 32 &&
          !binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                    BINARY_XMM0)) ||
         (ret_fbits == 64 &&
          !binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                    BINARY_XMM0)))) {
      code_generator_set_error(generator,
                               "Out of memory while materializing float call "
                               "return in function '%s'",
                               context->function_name);
      return 0;
    }
  }

  if (function_symbol && function_symbol->kind == SYMBOL_FUNCTION &&
      instruction->dest.kind == IR_OPERAND_TEMP && instruction->dest.name) {
    Type *ret_type = function_symbol->data.function.return_type;
    int ret_width = code_generator_binary_type_scalar_width(ret_type);
    int offset =
        code_generator_binary_get_temp_offset(context, instruction->dest.name);
    if (offset <= 0) {
      code_generator_set_error(generator, "Unknown IR temp '%s' in function '%s'",
                               instruction->dest.name, context->function_name);
      return 0;
    }
    /* Temp slots are 8 bytes and are loaded full-width (the load site has no
     * type to narrow with). A sub-64-bit integer return must therefore be
     * extended into all 8 bytes here, or the slot's upper bytes keep stale
     * bits from a prior occupant and corrupt any later 64-bit use (e.g. a
     * `ptr + header_size()` address computation). Widen RAX in place and store
     * the full register. */
    if (ret_width > 0 && ret_width < 8 &&
        code_generator_binary_resolved_type_is_supported(ret_type, 0)) {
      int ret_signed =
          code_generator_binary_resolved_type_is_signed_integer(ret_type);
      int ok = 1;
      if (ret_width == 4) {
        ok = ret_signed ? binary_emit_movsxd_rax_eax(&context->code)
                        : binary_emit_mov_eax_eax(&context->code);
      } else if (ret_width == 2) {
        ok = ret_signed ? binary_emit_movsx_rax_ax(&context->code)
                        : binary_emit_movzx_eax_ax(&context->code);
      } else if (ret_width == 1) {
        ok = ret_signed ? binary_emit_movsx_rax_al(&context->code)
                        : binary_emit_movzx_eax_al(&context->code);
      }
      if (!ok) {
        code_generator_set_error(
            generator,
            "Out of memory while extending call return in function '%s'",
            context->function_name);
        return 0;
      }
      return code_generator_binary_emit_temp_stack_store(
          generator, context, offset, BINARY_GP_RAX, NULL);
    }
    if (!code_generator_binary_emit_temp_stack_store(
            generator, context, offset, BINARY_GP_RAX,
            function_symbol->data.function.return_type)) {
      return 0;
    }
    return 1;
  }

  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_call_indirect(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  Symbol *symbol = NULL;
  Type *function_type = NULL;
  size_t stack_argument_count = 0;
  int call_stack_total = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }

  if (!code_generator_binary_validate_indirect_call(generator, context,
                                                    instruction)) {
    return 0;
  }

  symbol = generator->symbol_table
               ? symbol_table_lookup(generator->symbol_table,
                                     instruction->lhs.name)
               : NULL;
  function_type =
      (symbol && symbol->type && symbol->type->kind == TYPE_FUNCTION_POINTER)
          ? symbol->type
          : NULL;

  const BinaryAbi *abi = code_generator_binary_active_abi();
  size_t indirect_arg_count = instruction->argument_count;
  int *arg_is_float =
      indirect_arg_count > 0 ? calloc(indirect_arg_count, sizeof(int)) : NULL;
  BinaryArgLocation *arg_locations =
      indirect_arg_count > 0
          ? calloc(indirect_arg_count, sizeof(BinaryArgLocation))
          : NULL;
  if (indirect_arg_count > 0 && (!arg_is_float || !arg_locations)) {
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator,
                             "Out of memory planning indirect call layout");
    return 0;
  }
  for (size_t i = 0; i < indirect_arg_count; i++) {
    Type *parameter_type = function_type && function_type->fn_param_types
                               ? function_type->fn_param_types[i]
                               : NULL;
    arg_is_float[i] =
        code_generator_binary_resolved_type_float_bits(parameter_type) ? 1 : 0;
  }

  int stack_bytes = 0;
  if (indirect_arg_count > 0 &&
      !code_generator_binary_compute_arg_layout(abi, arg_is_float,
                                                indirect_arg_count,
                                                arg_locations, &stack_bytes)) {
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator, "Failed to compute indirect layout");
    return 0;
  }
  (void)stack_argument_count;
  if (stack_bytes > INT_MAX - 64) {
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator,
                             "Too many indirect call arguments in function "
                             "'%s'",
                             context->function_name);
    return 0;
  }

  call_stack_total = abi->shadow_space_size + stack_bytes;
  if (!binary_align_up_int(call_stack_total, 16, &call_stack_total)) {
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator,
                             "Indirect call frame too large in function '%s'",
                             context->function_name);
    return 0;
  }

  if (call_stack_total > 0 &&
      !binary_emit_sub_rsp_imm32(&context->code, (uint32_t)call_stack_total)) {
    free(arg_is_float);
    free(arg_locations);
    code_generator_set_error(generator,
                             "Out of memory while emitting indirect call frame");
    return 0;
  }

  for (size_t i = 0; i < indirect_arg_count; i++) {
    const BinaryArgLocation *loc = &arg_locations[i];
    if (loc->kind != BINARY_ARG_ON_STACK) continue;
    int slot_offset = abi->shadow_space_size + loc->stack_offset;
    Type *parameter_type =
        function_type && function_type->fn_param_types
            ? function_type->fn_param_types[i]
            : NULL;
    if (!code_generator_binary_emit_call_argument_load(
            generator, context, &instruction->arguments[i], parameter_type,
            BINARY_GP_R10) ||
        !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, slot_offset,
                                 BINARY_GP_R10)) {
      free(arg_is_float);
      free(arg_locations);
      if (!generator->has_error) {
        code_generator_set_error(
            generator, "Out of memory while materializing indirect call args");
      }
      return 0;
    }
  }

  for (size_t i = 0; i < indirect_arg_count; i++) {
    const BinaryArgLocation *loc = &arg_locations[i];
    if (loc->kind == BINARY_ARG_ON_STACK) continue;
    Type *parameter_type =
        function_type && function_type->fn_param_types
            ? function_type->fn_param_types[i]
            : NULL;
    int param_fbits =
        code_generator_binary_resolved_type_float_bits(parameter_type);
    if ((loc->kind == BINARY_ARG_IN_XMM_REGISTER &&
         !code_generator_binary_emit_float_call_argument(
             generator, context, &instruction->arguments[i], parameter_type,
             param_fbits, loc->xmm_register)) ||
        (loc->kind == BINARY_ARG_IN_GP_REGISTER &&
         !code_generator_binary_emit_call_argument_load(
             generator, context, &instruction->arguments[i], parameter_type,
             loc->gp_register))) {
      free(arg_is_float);
      free(arg_locations);
      return 0;
    }
  }
  free(arg_is_float);
  free(arg_locations);

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RAX) ||
      !binary_emit_call_reg(&context->code, BINARY_GP_RAX)) {
    if (!generator->has_error) {
      code_generator_set_error(
          generator,
          "Out of memory while emitting indirect call in function '%s'",
          context->function_name);
    }
    return 0;
  }

  if (call_stack_total > 0 &&
      !binary_emit_add_rsp_imm32(&context->code, (uint32_t)call_stack_total)) {
    code_generator_set_error(
        generator, "Out of memory while restoring indirect call frame");
    return 0;
  }

  if (function_type) {
    int ret_fbits = code_generator_binary_resolved_type_float_bits(
        function_type->fn_return_type);
    if ((ret_fbits == 32 &&
         !binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                   BINARY_XMM0)) ||
        (ret_fbits == 64 &&
         !binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                   BINARY_XMM0))) {
      code_generator_set_error(generator,
                               "Out of memory while materializing float "
                               "indirect call return in function '%s'",
                               context->function_name);
      return 0;
    }
  }

  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_rotate_add(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryGpRegister reg_next = BINARY_GP_RAX;
  BinaryGpRegister reg_a = BINARY_GP_R10;
  BinaryGpRegister reg_b = BINARY_GP_R11;
  int has_next = 0;
  int has_a = 0;
  int has_b = 0;

  if (!generator || !context || !instruction ||
      instruction->dest.kind != IR_OPERAND_SYMBOL || !instruction->dest.name ||
      instruction->lhs.kind != IR_OPERAND_SYMBOL || !instruction->lhs.name ||
      instruction->rhs.kind != IR_OPERAND_SYMBOL || !instruction->rhs.name) {
    code_generator_set_error(generator, "Malformed IR rotate_add in '%s'",
                             context->function_name);
    return 0;
  }

  has_next = code_generator_binary_symbol_assigned_register(
      generator, context, instruction->dest.name, &reg_next);
  has_a = code_generator_binary_symbol_assigned_register(
      generator, context, instruction->lhs.name, &reg_a);
  has_b = code_generator_binary_symbol_assigned_register(
      generator, context, instruction->rhs.name, &reg_b);

  if (has_next && has_a && has_b) {
    if ((!binary_emit_lea_reg_reg(&context->code, reg_next, reg_a, reg_b) &&
         (!binary_emit_mov_reg_reg(&context->code, reg_next, reg_a) ||
          !binary_emit_alu_reg_reg(&context->code, 0x01, reg_next, reg_b))) ||
        !binary_emit_mov_reg_reg(&context->code, reg_a, reg_b) ||
        !binary_emit_mov_reg_reg(&context->code, reg_b, reg_next)) {
      return 0;
    }
    return 1;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RAX)) {
    return 0;
  }
  if (has_b) {
    if (!binary_emit_lea_reg_reg(&context->code, BINARY_GP_RAX,
                                 BINARY_GP_RAX, reg_b) &&
        !binary_emit_alu_reg_reg(&context->code, 0x01, BINARY_GP_RAX, reg_b)) {
      return 0;
    }
  } else if (!code_generator_binary_emit_operand_load(generator, context,
                                                      &instruction->rhs,
                                                      BINARY_GP_R10) ||
             (!binary_emit_lea_reg_reg(&context->code, BINARY_GP_RAX,
                                       BINARY_GP_RAX, BINARY_GP_R10) &&
              !binary_emit_alu_reg_reg(&context->code, 0x01, BINARY_GP_RAX,
                                       BINARY_GP_R10))) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(&context->code, BINARY_GP_R11, BINARY_GP_RAX)) {
    return 0;
  }
  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_R11)) {
    return 0;
  }

  if (has_a && has_b) {
    if (!binary_emit_mov_reg_reg(&context->code, reg_a, reg_b)) {
      return 0;
    }
  } else if (has_a) {
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->rhs,
                                                 BINARY_GP_R10) ||
        !binary_emit_mov_reg_reg(&context->code, reg_a, BINARY_GP_R10)) {
      return 0;
    }
  } else if (!code_generator_binary_emit_operand_load(generator, context,
                                                      &instruction->rhs,
                                                      BINARY_GP_R10) ||
             !code_generator_binary_emit_destination_store(
                 generator, context, &instruction->lhs, BINARY_GP_R10)) {
    return 0;
  }

  if (has_b) {
    if (!binary_emit_mov_reg_reg(&context->code, reg_b, BINARY_GP_R11)) {
      return 0;
    }
  } else if (!code_generator_binary_emit_destination_store(
                 generator, context, &instruction->rhs, BINARY_GP_R11)) {
    return 0;
  }

  return 1;
}

static int binary_emit_string_concat(CodeGenerator *generator,
                                     BinaryFunctionContext *context,
                                     const IRInstruction *instruction) {
  size_t loop_fixup = 0;
  char *left_done_label = NULL;
  char *left_loop_label = NULL;
  char *right_done_label = NULL;
  char *right_loop_label = NULL;

  left_done_label = code_generator_generate_label(generator, "concat_left_done");
  left_loop_label = code_generator_generate_label(generator, "concat_left_loop");
  right_done_label =
      code_generator_generate_label(generator, "concat_right_done");
  right_loop_label =
      code_generator_generate_label(generator, "concat_right_loop");
  if (!left_done_label || !left_loop_label || !right_done_label ||
      !right_loop_label) {
    code_generator_set_error(generator,
                             "Out of memory while creating concat labels in "
                             "function '%s'",
                             context->function_name);
    free(left_done_label);
    free(left_loop_label);
    free(right_done_label);
    free(right_loop_label);
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RAX) ||
      !binary_emit_push_reg(&context->code, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R10) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_RCX, BINARY_GP_RAX,
                               8) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_RDX, BINARY_GP_R10,
                               8) ||
      !binary_emit_alu_reg_reg(&context->code, 0x01, BINARY_GP_RCX,
                               BINARY_GP_RDX) ||
      !binary_emit_sub_rsp_imm32(&context->code, 24) ||
      !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, 0,
                               BINARY_GP_R10) ||
      !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, 8,
                               BINARY_GP_RAX) ||
      !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RSP, 16,
                               BINARY_GP_RCX) ||
      !binary_emit_add_reg_imm32(&context->code, BINARY_GP_RCX, 17) ||
      !binary_emit_windows_zeroed_heap_alloc(generator, context,
                                             BINARY_GP_RCX) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_RCX, BINARY_GP_RSP,
                               16) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_RDX, BINARY_GP_RSP,
                               8) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_R10, BINARY_GP_RSP,
                               0) ||
      !binary_emit_add_rsp_imm32(&context->code, 24) ||
      !binary_emit_lea_reg_mem(&context->code, BINARY_GP_R8, BINARY_GP_RAX,
                               16) ||
      !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RAX, 0,
                               BINARY_GP_R8) ||
      !binary_emit_mov_mem_reg(&context->code, BINARY_GP_RAX, 8,
                               BINARY_GP_RCX) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_R9, BINARY_GP_RDX,
                               8) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_R11, BINARY_GP_RDX,
                               0) ||
      !binary_emit_test_reg_reg(&context->code, BINARY_GP_R9) ||
      !binary_emit_je_placeholder(&context->code, &loop_fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, left_done_label,
                                    loop_fixup) ||
      !binary_label_table_define(&context->labels, left_loop_label,
                                 context->code.size) ||
      !binary_emit_movzx_reg_mem8(&context->code, BINARY_GP_RCX,
                                  BINARY_GP_R11, 0) ||
      !binary_emit_mov_mem_reg8(&context->code, BINARY_GP_R8, 0,
                                BINARY_GP_RCX) ||
      !binary_emit_add_reg_imm32(&context->code, BINARY_GP_R11, 1) ||
      !binary_emit_add_reg_imm32(&context->code, BINARY_GP_R8, 1) ||
      !binary_emit_sub_reg_imm32(&context->code, BINARY_GP_R9, 1) ||
      !binary_emit_test_reg_reg(&context->code, BINARY_GP_R9) ||
      !binary_emit_je_placeholder(&context->code, &loop_fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, left_done_label,
                                    loop_fixup) ||
      !binary_emit_jmp_placeholder(&context->code, &loop_fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, left_loop_label,
                                    loop_fixup) ||
      !binary_label_table_define(&context->labels, left_done_label,
                                 context->code.size) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_R9, BINARY_GP_R10,
                               8) ||
      !binary_emit_mov_reg_mem(&context->code, BINARY_GP_R11, BINARY_GP_R10,
                               0) ||
      !binary_emit_test_reg_reg(&context->code, BINARY_GP_R9) ||
      !binary_emit_je_placeholder(&context->code, &loop_fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, right_done_label,
                                    loop_fixup) ||
      !binary_label_table_define(&context->labels, right_loop_label,
                                 context->code.size) ||
      !binary_emit_movzx_reg_mem8(&context->code, BINARY_GP_RCX,
                                  BINARY_GP_R11, 0) ||
      !binary_emit_mov_mem_reg8(&context->code, BINARY_GP_R8, 0,
                                BINARY_GP_RCX) ||
      !binary_emit_add_reg_imm32(&context->code, BINARY_GP_R11, 1) ||
      !binary_emit_add_reg_imm32(&context->code, BINARY_GP_R8, 1) ||
      !binary_emit_sub_reg_imm32(&context->code, BINARY_GP_R9, 1) ||
      !binary_emit_test_reg_reg(&context->code, BINARY_GP_R9) ||
      !binary_emit_je_placeholder(&context->code, &loop_fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, right_done_label,
                                    loop_fixup) ||
      !binary_emit_jmp_placeholder(&context->code, &loop_fixup) ||
      !binary_label_fixup_table_add(&context->label_fixups, right_loop_label,
                                    loop_fixup) ||
      !binary_label_table_define(&context->labels, right_done_label,
                                 context->code.size) ||
      !binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RCX, 0) ||
      !binary_emit_mov_mem_reg8(&context->code, BINARY_GP_R8, 0,
                                BINARY_GP_RCX) ||
      !code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    if (!generator->has_error) {
      code_generator_set_error(
          generator,
          "Out of memory while emitting string concat in function '%s'",
          context->function_name);
    }
    free(left_done_label);
    free(left_loop_label);
    free(right_done_label);
    free(right_loop_label);
    return 0;
  }

  free(left_done_label);
  free(left_loop_label);
  free(right_done_label);
  free(right_loop_label);
  return 1;
}

static int binary_emit_binary_float(CodeGenerator *generator,
                                    BinaryFunctionContext *context,
                                    const IRInstruction *instruction) {
  const char *op = instruction->text;
  unsigned char condition_opcode = 0;
  int is_compare = 0;
  int fbits = (instruction->float_bits == 32) ? 32 : 64;
  int arith_ok = 0;
  int reg_move_ok = 0;
  op = instruction->text;
  /* Bring both operands in at the operation's precision so single- and
   * double-precision expressions stay in their own domain. */
  if (!code_generator_binary_emit_float_operand_to_xmm_bits(
          generator, context, &instruction->rhs, BINARY_XMM1, fbits) ||
      !code_generator_binary_emit_float_operand_to_xmm_bits(
          generator, context, &instruction->lhs, BINARY_XMM0, fbits)) {
    goto emit_failure;
  }

  if (strcmp(op, "+") == 0) {
    arith_ok = (fbits == 32)
                   ? binary_emit_addss_xmm_xmm(&context->code, BINARY_XMM0,
                                               BINARY_XMM1)
                   : binary_emit_addsd_xmm_xmm(&context->code, BINARY_XMM0,
                                               BINARY_XMM1);
    reg_move_ok =
        (fbits == 32)
            ? binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                       BINARY_XMM0)
            : binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                       BINARY_XMM0);
    if (!arith_ok || !reg_move_ok) {
      goto emit_failure;
    }
  } else if (strcmp(op, "-") == 0) {
    arith_ok = (fbits == 32)
                   ? binary_emit_subss_xmm_xmm(&context->code, BINARY_XMM0,
                                               BINARY_XMM1)
                   : binary_emit_subsd_xmm_xmm(&context->code, BINARY_XMM0,
                                               BINARY_XMM1);
    reg_move_ok =
        (fbits == 32)
            ? binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                       BINARY_XMM0)
            : binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                       BINARY_XMM0);
    if (!arith_ok || !reg_move_ok) {
      goto emit_failure;
    }
  } else if (strcmp(op, "*") == 0) {
    arith_ok = (fbits == 32)
                   ? binary_emit_mulss_xmm_xmm(&context->code, BINARY_XMM0,
                                               BINARY_XMM1)
                   : binary_emit_mulsd_xmm_xmm(&context->code, BINARY_XMM0,
                                               BINARY_XMM1);
    reg_move_ok =
        (fbits == 32)
            ? binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                       BINARY_XMM0)
            : binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                       BINARY_XMM0);
    if (!arith_ok || !reg_move_ok) {
      goto emit_failure;
    }
  } else if (strcmp(op, "/") == 0) {
    arith_ok = (fbits == 32)
                   ? binary_emit_divss_xmm_xmm(&context->code, BINARY_XMM0,
                                               BINARY_XMM1)
                   : binary_emit_divsd_xmm_xmm(&context->code, BINARY_XMM0,
                                               BINARY_XMM1);
    reg_move_ok =
        (fbits == 32)
            ? binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                       BINARY_XMM0)
            : binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                       BINARY_XMM0);
    if (!arith_ok || !reg_move_ok) {
      goto emit_failure;
    }
  } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
             strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
             strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
    int cmp_ok = (fbits == 32)
                     ? binary_emit_ucomiss_xmm_xmm(&context->code,
                                                   BINARY_XMM0, BINARY_XMM1)
                     : binary_emit_ucomisd_xmm_xmm(&context->code,
                                                   BINARY_XMM0, BINARY_XMM1);
    if (!cmp_ok) {
      goto emit_failure;
    }

    if (strcmp(op, "==") == 0) {
      if (!binary_emit_setcc_reg8(&context->code, 0x94, BINARY_GP_RAX) ||
          !binary_emit_setcc_reg8(&context->code, 0x9B, BINARY_GP_RCX) ||
          !binary_emit_alu_reg8_reg8(&context->code, 0x20, BINARY_GP_RAX,
                                     BINARY_GP_RCX) ||
          !binary_emit_movzx_eax_al(&context->code)) {
        goto emit_failure;
      }
    } else if (strcmp(op, "!=") == 0) {
      if (!binary_emit_setcc_reg8(&context->code, 0x95, BINARY_GP_RAX) ||
          !binary_emit_setcc_reg8(&context->code, 0x9A, BINARY_GP_RCX) ||
          !binary_emit_alu_reg8_reg8(&context->code, 0x08, BINARY_GP_RAX,
                                     BINARY_GP_RCX) ||
          !binary_emit_movzx_eax_al(&context->code)) {
        goto emit_failure;
      }
    } else if (strcmp(op, "<") == 0) {
      condition_opcode = 0x92;
      is_compare = 1;
    } else if (strcmp(op, "<=") == 0) {
      condition_opcode = 0x96;
      is_compare = 1;
    } else if (strcmp(op, ">") == 0) {
      condition_opcode = 0x97;
      is_compare = 1;
    } else if (strcmp(op, ">=") == 0) {
      condition_opcode = 0x93;
      is_compare = 1;
    }

    if (is_compare &&
        (!binary_emit_setcc_reg8(&context->code, condition_opcode,
                                 BINARY_GP_RAX) ||
         !binary_emit_movzx_eax_al(&context->code))) {
      goto emit_failure;
    }
  } else {
    code_generator_set_error(
        generator,
        "Direct object backend does not yet support float binary operator "
        "'%s' in function '%s'",
        op, context->function_name);
    return 0;
  }

  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    return 0;
  }

  return 1;

emit_failure:
  code_generator_set_error(
      generator,
      "Out of memory while emitting IR binary operator in function '%s'",
      context->function_name);
  return 0;
}

/* True when `operand` is a SYMBOL currently held in a promoted GP register,
 * writing that register to *reg_out. Used by the register-register ALU fast
 * path to avoid the load-into-RAX round-trip for already-resident operands. */
static int code_generator_binary_symbol_operand_register(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *operand, BinaryGpRegister *reg_out) {
  if (!operand || operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
    return 0;
  }
  return code_generator_binary_symbol_assigned_register(
      generator, context, operand->name, reg_out);
}

static int binary_canonicalize_narrow_reg_for_type(
    BinaryFunctionContext *context, Type *type, BinaryGpRegister reg) {
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

/* If `dest` is a narrow integer variable promoted to `dest_reg`, extend the
 * register in place: the in-place ALU fast paths compute in 64 bits, and
 * narrow home semantics wrap to the destination width, so the promoted home
 * must hold the canonical sign- or zero-extended value (mirrors MIR). */
static int binary_canonicalize_narrow_dest_reg(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IROperand *dest, BinaryGpRegister dest_reg) {
  Type *t = code_generator_binary_get_operand_type_in_context(generator,
                                                              context, dest);
  return binary_canonicalize_narrow_reg_for_type(context, t, dest_reg);
}

static int binary_emit_binary_integer(CodeGenerator *generator,
                                      BinaryFunctionContext *context,
                                      const IRInstruction *instruction) {
  const char *op = instruction->text;
  unsigned char condition_opcode = 0;
  int is_compare = 0;

  op = instruction->text;
  if (!instruction->is_float &&
      (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) &&
      instruction->rhs.kind == IR_OPERAND_INT) {
    /* Signedness is the dividend's type: the power-of-two reduction below and
     * the signed magic-multiply both assume signed arithmetic (sar, sign-bias).
     * For an unsigned dividend those are wrong on high-bit-set values, so route
     * unsigned constant division to its own magic path (logical shifts, MUL). */
    Type *dividend_type = code_generator_binary_get_operand_type_in_context(
        generator, context, &instruction->lhs);
    int dividend_unsigned =
        dividend_type &&
        (dividend_type->kind == TYPE_UINT8 ||
         dividend_type->kind == TYPE_UINT16 ||
         dividend_type->kind == TYPE_UINT32 ||
         dividend_type->kind == TYPE_UINT64);

    if (dividend_unsigned && instruction->rhs.int_value >= 2) {
      int u_handled = 0;
      if (!code_generator_binary_emit_operand_load(generator, context,
                                                   &instruction->lhs,
                                                   BINARY_GP_RAX) ||
          !code_generator_binary_try_emit_unsigned_const_divmod(
              context, op, (unsigned long long)instruction->rhs.int_value,
              &u_handled)) {
        goto emit_failure;
      }
      if (u_handled) {
        if (!code_generator_binary_emit_destination_store(
                generator, context, &instruction->dest, BINARY_GP_RAX)) {
          return 0;
        }
        return 1;
      }
    }

    unsigned int shift = 0;
    unsigned long long mask = 0;
    if (!dividend_unsigned &&
        code_generator_binary_extract_positive_power_of_two(
            instruction->rhs.int_value, &shift, &mask)) {
      if (!code_generator_binary_emit_operand_load(generator, context,
                                                   &instruction->lhs,
                                                   BINARY_GP_RAX)) {
        return 0;
      }

      if (strcmp(op, "/") == 0) {
        if (shift != 0 &&
            (!binary_emit_mov_reg_reg(&context->code, BINARY_GP_RCX,
                                      BINARY_GP_RAX) ||
             !binary_emit_shift_reg_imm8(&context->code, 7, BINARY_GP_RCX,
                                         63) ||
             !code_generator_binary_emit_and_mask(context, BINARY_GP_RCX,
                                                  mask) ||
             !binary_emit_alu_reg_reg(&context->code, 0x01, BINARY_GP_RAX,
                                      BINARY_GP_RCX) ||
             !binary_emit_shift_reg_imm8(&context->code, 7, BINARY_GP_RAX,
                                         (unsigned char)shift))) {
          goto emit_failure;
        }
      } else {
        if (shift == 0) {
          if (!binary_emit_mov_reg_imm64(&context->code, BINARY_GP_RAX, 0)) {
            goto emit_failure;
          }
        } else if (!binary_emit_mov_reg_reg(&context->code, BINARY_GP_R11,
                                            BINARY_GP_RAX) ||
                   !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RCX,
                                            BINARY_GP_RAX) ||
                   !binary_emit_shift_reg_imm8(&context->code, 7,
                                               BINARY_GP_RCX, 63) ||
                   !code_generator_binary_emit_and_mask(context,
                                                        BINARY_GP_RCX, mask) ||
                   !binary_emit_alu_reg_reg(&context->code, 0x01,
                                            BINARY_GP_RAX, BINARY_GP_RCX) ||
                   !binary_emit_shift_reg_imm8(&context->code, 7,
                                               BINARY_GP_RAX,
                                               (unsigned char)shift) ||
                   !binary_emit_shift_reg_imm8(&context->code, 4,
                                               BINARY_GP_RAX,
                                               (unsigned char)shift) ||
                   !binary_emit_alu_reg_reg(&context->code, 0x29,
                                            BINARY_GP_R11, BINARY_GP_RAX) ||
                   !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RAX,
                                            BINARY_GP_R11)) {
          goto emit_failure;
        }
      }

      if (!code_generator_binary_emit_destination_store(generator, context,
                                                        &instruction->dest,
                                                        BINARY_GP_RAX)) {
        return 0;
      }
      return 1;
    }

    int handled = 0;
    if (!dividend_unsigned) {
      if (!code_generator_binary_emit_operand_load(generator, context,
                                                   &instruction->lhs,
                                                   BINARY_GP_RAX) ||
          !code_generator_binary_try_emit_signed_const_divmod(
              context, op, instruction->rhs.int_value, &handled)) {
        goto emit_failure;
      }
    }
    if (handled) {
      if (!code_generator_binary_emit_destination_store(generator, context,
                                                        &instruction->dest,
                                                        BINARY_GP_RAX)) {
        return 0;
      }
      return 1;
    }
  }

  if (!instruction->is_float) {
    const IROperand *value_operand = NULL;
    long long immediate = 0;
    int immediate_on_rhs = 0;
    int commutative =
        strcmp(op, "+") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
        strcmp(op, "^") == 0 || strcmp(op, "==") == 0 ||
        strcmp(op, "!=") == 0;
    int rhs_immediate_supported =
        commutative || strcmp(op, "-") == 0 || strcmp(op, "<<") == 0 ||
        strcmp(op, ">>") == 0 || strcmp(op, "<") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, ">=") == 0;

    if (rhs_immediate_supported && instruction->rhs.kind == IR_OPERAND_INT &&
        code_generator_binary_immediate_fits_signed_32(
            instruction->rhs.int_value)) {
      value_operand = &instruction->lhs;
      immediate = instruction->rhs.int_value;
      immediate_on_rhs = 1;
    } else if (commutative && instruction->lhs.kind == IR_OPERAND_INT &&
               code_generator_binary_immediate_fits_signed_32(
                   instruction->lhs.int_value)) {
      value_operand = &instruction->rhs;
      immediate = instruction->lhs.int_value;
    }

    if (value_operand) {
      int handled = 1;

      /* In-place fast path: when the result symbol is promoted to a register
       * DR, compute `DR = value_operand <op> imm` directly in DR instead of
       * routing through RAX and copying back. This removes the
       * `mov rax,SR; <op> rax,imm; mov DR,rax` triple that dominates loop
       * counters and pointer bumps (i++, scan-=4, ...). Only the arithmetic,
       * bitwise, and shift ops are eligible; the comparison ops below need RAX
       * for setcc/movzx, so they fall through to the RAX path. dest==value
       * (e.g. `i = i + 1`) needs no preparatory move at all. */
      {
        BinaryGpRegister dest_reg = BINARY_GP_RAX;
        int inplace_op =
            (strcmp(op, "+") == 0) ||
            (strcmp(op, "-") == 0 && immediate_on_rhs) ||
            (strcmp(op, "*") == 0) || (strcmp(op, "&") == 0) ||
            (strcmp(op, "|") == 0) || (strcmp(op, "^") == 0) ||
            (((strcmp(op, "<<") == 0) || (strcmp(op, ">>") == 0)) &&
             immediate_on_rhs && immediate >= 0 && immediate < 64);
        if (inplace_op && instruction->dest.kind == IR_OPERAND_SYMBOL &&
            instruction->dest.name &&
            code_generator_binary_symbol_assigned_register(
                generator, context, instruction->dest.name, &dest_reg)) {
          /* Place value_operand into dest_reg. If value_operand is itself a
           * promoted register equal to dest_reg, nothing to do; otherwise load
           * (a register-register mov for promoted operands, a memory/imm load
           * otherwise). emit_operand_load handles all operand kinds and emits
           * nothing when the source already equals the target register. */
          if (!code_generator_binary_emit_operand_load(generator, context,
                                                       value_operand,
                                                       dest_reg)) {
            return 0;
          }

          int ok = 1;
          if (strcmp(op, "+") == 0) {
            ok = binary_emit_add_reg_imm32(&context->code, dest_reg,
                                           (uint32_t)(int32_t)immediate);
          } else if (strcmp(op, "-") == 0) {
            ok = binary_emit_sub_reg_imm32(&context->code, dest_reg,
                                           (uint32_t)(int32_t)immediate);
          } else if (strcmp(op, "*") == 0) {
            int multiply_handled = 0;
            ok = code_generator_binary_try_emit_reg_multiply_immediate(
                context, dest_reg, immediate, &multiply_handled);
            if (ok && !multiply_handled) {
              ok = binary_emit_imul_reg_reg_imm32(
                  &context->code, dest_reg, dest_reg,
                  (uint32_t)(int32_t)immediate);
            }
          } else if (strcmp(op, "&") == 0) {
            ok = binary_emit_and_reg_imm32(&context->code, dest_reg,
                                           (uint32_t)(int32_t)immediate);
          } else if (strcmp(op, "|") == 0) {
            ok = binary_emit_or_reg_imm32(&context->code, dest_reg,
                                          (uint32_t)(int32_t)immediate);
          } else if (strcmp(op, "^") == 0) {
            ok = binary_emit_xor_reg_imm32(&context->code, dest_reg,
                                           (uint32_t)(int32_t)immediate);
          } else { /* << or >> */
            ok = binary_emit_shift_reg_imm8(&context->code,
                                            strcmp(op, "<<") == 0 ? 4 : 7,
                                            dest_reg, (unsigned char)immediate);
          }
          if (!ok) {
            goto emit_failure;
          }
          if (!binary_canonicalize_narrow_dest_reg(
                  generator, context, &instruction->dest, dest_reg)) {
            goto emit_failure;
          }
          return 1;
        }
      }

      if (!code_generator_binary_emit_operand_load(generator, context,
                                                   value_operand,
                                                   BINARY_GP_RAX)) {
        return 0;
      }

      if (strcmp(op, "+") == 0) {
        if (!binary_emit_add_reg_imm32(&context->code, BINARY_GP_RAX,
                                       (uint32_t)(int32_t)immediate)) {
          goto emit_failure;
        }
      } else if (strcmp(op, "-") == 0 && immediate_on_rhs) {
        if (!binary_emit_sub_reg_imm32(&context->code, BINARY_GP_RAX,
                                       (uint32_t)(int32_t)immediate)) {
          goto emit_failure;
        }
      } else if (strcmp(op, "*") == 0) {
        int multiply_handled = 0;
        if (!code_generator_binary_try_emit_reg_multiply_immediate(
                context, BINARY_GP_RAX, immediate, &multiply_handled)) {
          goto emit_failure;
        }
        if (!multiply_handled &&
            !binary_emit_imul_reg_reg_imm32(&context->code, BINARY_GP_RAX,
                                            BINARY_GP_RAX,
                                            (uint32_t)(int32_t)immediate)) {
          goto emit_failure;
        }
      } else if (strcmp(op, "&") == 0) {
        if (!binary_emit_and_reg_imm32(&context->code, BINARY_GP_RAX,
                                       (uint32_t)(int32_t)immediate)) {
          goto emit_failure;
        }
      } else if (strcmp(op, "|") == 0) {
        if (!binary_emit_or_reg_imm32(&context->code, BINARY_GP_RAX,
                                      (uint32_t)(int32_t)immediate)) {
          goto emit_failure;
        }
      } else if (strcmp(op, "^") == 0) {
        if (!binary_emit_xor_reg_imm32(&context->code, BINARY_GP_RAX,
                                       (uint32_t)(int32_t)immediate)) {
          goto emit_failure;
        }
      } else if ((strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) &&
                 immediate_on_rhs && immediate >= 0 && immediate < 64) {
        if (!binary_emit_shift_reg_imm8(
                &context->code, strcmp(op, "<<") == 0 ? 4 : 7,
                BINARY_GP_RAX, (unsigned char)immediate)) {
          goto emit_failure;
        }
      } else if (strcmp(op, "/") == 0 && immediate_on_rhs && immediate == 2) {
        /* Signed divide-by-2 with truncation toward zero:
         * q = (x + ((x >> 63) & 1)) >> 1
         * Avoids costly idiv in binary-search midpoint loops and similar code. */
        if (!binary_emit_mov_reg_reg(&context->code, BINARY_GP_R11,
                                     BINARY_GP_RAX) ||
            !binary_emit_shift_reg_imm8(&context->code, 7, BINARY_GP_R11,
                                        63) ||
            !binary_emit_and_reg_imm32(&context->code, BINARY_GP_R11, 1) ||
            !binary_emit_alu_reg_reg(&context->code, 0x01, BINARY_GP_RAX,
                                     BINARY_GP_R11) ||
            !binary_emit_shift_reg_imm8(&context->code, 7, BINARY_GP_RAX, 1)) {
          goto emit_failure;
        }
      } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                 (immediate_on_rhs &&
                  (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0))) {
        if (strcmp(op, "==") == 0) {
          condition_opcode = 0x94;
        } else if (strcmp(op, "!=") == 0) {
          condition_opcode = 0x95;
        } else if (strcmp(op, "<") == 0) {
          condition_opcode = 0x9C;
        } else if (strcmp(op, "<=") == 0) {
          condition_opcode = 0x9E;
        } else if (strcmp(op, ">") == 0) {
          condition_opcode = 0x9F;
        } else {
          condition_opcode = 0x9D;
        }

        if (!binary_emit_cmp_reg_imm32(&context->code, BINARY_GP_RAX,
                                       (uint32_t)(int32_t)immediate) ||
            !binary_emit_setcc_al(&context->code, condition_opcode) ||
            !binary_emit_movzx_eax_al(&context->code)) {
          goto emit_failure;
        }
      } else {
        handled = 0;
      }

      if (handled) {
        if (!code_generator_binary_emit_destination_store(generator, context,
                                                          &instruction->dest,
                                                          BINARY_GP_RAX)) {
          return 0;
        }
        return 1;
      }
    }
  }

  /* Register-register in-place fast path: when the result symbol is promoted to
   * a register DR and the op is a simple ALU op, compute directly in DR instead
   * of the load-both-into-RAX/R10, operate, store-back sequence. This removes
   * the ~4-instruction `mov r10,B; mov rax,A; <op> rax,r10; mov DR,rax` shape
   * that dominates register-resident accumulator loops (a=a+i, c=c+a-b, ...).
   * Restricted to the commutative/sub ALU ops with a known reg,reg encoding;
   * everything else (compares, shifts-by-reg, mul, div) keeps the RAX path. */
  {
    BinaryGpRegister dest_reg = BINARY_GP_RAX;
    unsigned char alu_opcode = 0;
    int is_sub = (strcmp(op, "-") == 0);
    int alu_ok = (strcmp(op, "+") == 0 && (alu_opcode = 0x01, 1)) ||
                 (is_sub && (alu_opcode = 0x29, 1)) ||
                 (strcmp(op, "&") == 0 && (alu_opcode = 0x21, 1)) ||
                 (strcmp(op, "|") == 0 && (alu_opcode = 0x09, 1)) ||
                 (strcmp(op, "^") == 0 && (alu_opcode = 0x31, 1));
    if (alu_ok && instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        code_generator_binary_symbol_assigned_register(
            generator, context, instruction->dest.name, &dest_reg)) {
      BinaryGpRegister lhs_reg = BINARY_GP_RAX;
      BinaryGpRegister rhs_reg = BINARY_GP_RAX;
      int lhs_in_reg = code_generator_binary_symbol_operand_register(
          generator, context, &instruction->lhs, &lhs_reg);
      int rhs_in_reg = code_generator_binary_symbol_operand_register(
          generator, context, &instruction->rhs, &rhs_reg);
      int commutative_alu = !is_sub; /* +,&,|,^ are commutative; - is not */

      /* Case A: dest already holds lhs (e.g. a = a + i). Just `op DR, rhs`,
       * loading rhs into a scratch only if it isn't already in a register. */
      if (lhs_in_reg && lhs_reg == dest_reg && rhs_in_reg &&
          rhs_reg != dest_reg) {
        if (!binary_emit_alu_reg_reg(&context->code, alu_opcode, dest_reg,
                                     rhs_reg) ||
            !binary_canonicalize_narrow_dest_reg(
                generator, context, &instruction->dest, dest_reg)) {
          goto emit_failure;
        }
        return 1;
      }
      /* Case B (commutative): dest already holds rhs. `op DR, lhs`. */
      if (commutative_alu && rhs_in_reg && rhs_reg == dest_reg && lhs_in_reg &&
          lhs_reg != dest_reg) {
        if (!binary_emit_alu_reg_reg(&context->code, alu_opcode, dest_reg,
                                     lhs_reg) ||
            !binary_canonicalize_narrow_dest_reg(
                generator, context, &instruction->dest, dest_reg)) {
          goto emit_failure;
        }
        return 1;
      }
      /* Case C: both operands in registers, dest distinct. `mov DR,lhs; op DR,rhs`.
       * Safe only when rhs_reg != dest_reg (the mov would clobber rhs first). */
      if (lhs_in_reg && rhs_in_reg && rhs_reg != dest_reg) {
        if (!binary_emit_mov_reg_reg(&context->code, dest_reg, lhs_reg) ||
            !binary_emit_alu_reg_reg(&context->code, alu_opcode, dest_reg,
                                     rhs_reg) ||
            !binary_canonicalize_narrow_dest_reg(
                generator, context, &instruction->dest, dest_reg)) {
          goto emit_failure;
        }
        return 1;
      }
    }
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RAX)) {
    return 0;
  }

  if (strcmp(op, "+") == 0) {
    if (!binary_emit_lea_reg_reg(&context->code, BINARY_GP_RAX, BINARY_GP_RAX,
                                 BINARY_GP_R10) &&
        !binary_emit_alu_reg_reg(&context->code, 0x01, BINARY_GP_RAX,
                                 BINARY_GP_R10)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "-") == 0) {
    if (!binary_emit_alu_reg_reg(&context->code, 0x29, BINARY_GP_RAX,
                                 BINARY_GP_R10)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "*") == 0) {
    if (!binary_emit_imul_reg_reg(&context->code, BINARY_GP_RAX,
                                  BINARY_GP_R10)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
    /* Unsigned dividend: zero-extend into RDX and use DIV, not CQO/IDIV.
     * Applying signed division to a high-bit-set unsigned value gives the wrong
     * quotient and remainder. Signedness is the dividend's declared type. */
    Type *rt_dividend_type = code_generator_binary_get_operand_type_in_context(
        generator, context, &instruction->lhs);
    int rt_unsigned =
        rt_dividend_type &&
        (rt_dividend_type->kind == TYPE_UINT8 ||
         rt_dividend_type->kind == TYPE_UINT16 ||
         rt_dividend_type->kind == TYPE_UINT32 ||
         rt_dividend_type->kind == TYPE_UINT64);
    if (rt_unsigned) {
      if (!binary_emit_xor_reg_reg32(&context->code, BINARY_GP_RDX) ||
          !binary_emit_div_reg(&context->code, BINARY_GP_R10)) {
        goto emit_failure;
      }
    } else if (!binary_emit_cqo(&context->code) ||
               !binary_emit_idiv_reg(&context->code, BINARY_GP_R10)) {
      goto emit_failure;
    }
    if (strcmp(op, "%") == 0 &&
        !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RAX,
                                 BINARY_GP_RDX)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "&") == 0) {
    if (!binary_emit_alu_reg_reg(&context->code, 0x21, BINARY_GP_RAX,
                                 BINARY_GP_R10)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "|") == 0) {
    if (!binary_emit_alu_reg_reg(&context->code, 0x09, BINARY_GP_RAX,
                                 BINARY_GP_R10)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "^") == 0) {
    if (!binary_emit_alu_reg_reg(&context->code, 0x31, BINARY_GP_RAX,
                                 BINARY_GP_R10)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "<<") == 0) {
    if (!binary_emit_mov_reg_reg(&context->code, BINARY_GP_RCX,
                                 BINARY_GP_R10) ||
        !binary_emit_shift_reg_cl(&context->code, 4, BINARY_GP_RAX)) {
      goto emit_failure;
    }
  } else if (strcmp(op, ">>") == 0) {
    if (!binary_emit_mov_reg_reg(&context->code, BINARY_GP_RCX,
                                 BINARY_GP_R10) ||
        !binary_emit_shift_reg_cl(&context->code, 7, BINARY_GP_RAX)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "&&") == 0) {
    if (!binary_emit_alu_reg_reg(&context->code, 0x21, BINARY_GP_RAX,
                                 BINARY_GP_R10) ||
        !binary_emit_test_reg_reg(&context->code, BINARY_GP_RAX) ||
        !binary_emit_setcc_al(&context->code, 0x95) ||
        !binary_emit_movzx_eax_al(&context->code)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "||") == 0) {
    if (!binary_emit_alu_reg_reg(&context->code, 0x09, BINARY_GP_RAX,
                                 BINARY_GP_R10) ||
        !binary_emit_test_reg_reg(&context->code, BINARY_GP_RAX) ||
        !binary_emit_setcc_al(&context->code, 0x95) ||
        !binary_emit_movzx_eax_al(&context->code)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "==") == 0) {
    condition_opcode = 0x94;
    is_compare = 1;
  } else if (strcmp(op, "!=") == 0) {
    condition_opcode = 0x95;
    is_compare = 1;
  } else if (strcmp(op, "<") == 0) {
    condition_opcode = 0x9C;
    is_compare = 1;
  } else if (strcmp(op, "<=") == 0) {
    condition_opcode = 0x9E;
    is_compare = 1;
  } else if (strcmp(op, ">") == 0) {
    condition_opcode = 0x9F;
    is_compare = 1;
  } else if (strcmp(op, ">=") == 0) {
    condition_opcode = 0x9D;
    is_compare = 1;
  } else {
    code_generator_set_error(generator,
                             "Direct object backend does not yet support IR "
                             "binary operator '%s' in function '%s'",
                             op, context->function_name);
    return 0;
  }

  if (is_compare &&
      (!code_generator_binary_emit_reg_reg_compare(
          &context->code, BINARY_GP_RAX, BINARY_GP_R10,
          code_generator_binary_instruction_compare_width(generator, context,
                                                          instruction)) ||
       !binary_emit_setcc_al(&context->code, condition_opcode) ||
       !binary_emit_movzx_eax_al(&context->code))) {
    goto emit_failure;
  }

  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    return 0;
  }

  return 1;

emit_failure:
  code_generator_set_error(
      generator,
      "Out of memory while emitting IR binary operator in function '%s'",
      context->function_name);
  return 0;
}

int code_generator_binary_emit_binary(CodeGenerator *generator,
                                             BinaryFunctionContext *context,
                                             const IRInstruction *instruction) {
  if (!generator || !context || !instruction || !instruction->text) {
    return 0;
  }

  Type *result_type = instruction->ast_ref
                          ? code_generator_infer_expression_type(
                                generator, instruction->ast_ref)
                          : NULL;
  if (result_type && result_type->kind == TYPE_STRING &&
      strcmp(instruction->text, "+") == 0) {
    return binary_emit_string_concat(generator, context, instruction);
  }

  if (instruction->is_float) {
    return binary_emit_binary_float(generator, context, instruction);
  }

  return binary_emit_binary_integer(generator, context, instruction);
}

int code_generator_binary_emit_unary(CodeGenerator *generator,
                                            BinaryFunctionContext *context,
                                            const IRInstruction *instruction) {
  const char *op = NULL;

  if (!generator || !context || !instruction || !instruction->text) {
    return 0;
  }

  if (instruction->is_float) {
    int fbits = (instruction->float_bits == 32) ? 32 : 64;
    op = instruction->text;
    if (!code_generator_binary_emit_float_operand_to_xmm_bits(
            generator, context, &instruction->lhs, BINARY_XMM0, fbits)) {
      goto emit_failure;
    }

    if (strcmp(op, "-") == 0) {
      /* Negate as 0 - x at the operand precision. */
      int neg_ok =
          binary_emit_pxor_xmm_xmm(&context->code, BINARY_XMM1, BINARY_XMM1) &&
          (fbits == 32
               ? binary_emit_subss_xmm_xmm(&context->code, BINARY_XMM1,
                                           BINARY_XMM0)
               : binary_emit_subsd_xmm_xmm(&context->code, BINARY_XMM1,
                                           BINARY_XMM0)) &&
          (fbits == 32
               ? binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                          BINARY_XMM1)
               : binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                          BINARY_XMM1));
      if (!neg_ok) {
        goto emit_failure;
      }
    } else if (strcmp(op, "+") == 0) {
      int mv_ok = (fbits == 32)
                      ? binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                                 BINARY_XMM0)
                      : binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                                 BINARY_XMM0);
      if (!mv_ok) {
        goto emit_failure;
      }
    } else {
      code_generator_set_error(
          generator,
          "Direct object backend does not yet support float unary operator "
          "'%s' in function '%s'",
          op, context->function_name);
      return 0;
    }

    if (!code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX)) {
      return 0;
    }

    return 1;
  }

  op = instruction->text;
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RAX)) {
    return 0;
  }

  if (strcmp(op, "-") == 0) {
    if (!binary_emit_neg_reg(&context->code, BINARY_GP_RAX)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "!") == 0) {
    if (!binary_emit_test_reg_reg(&context->code, BINARY_GP_RAX) ||
        !binary_emit_setcc_al(&context->code, 0x94) ||
        !binary_emit_movzx_eax_al(&context->code)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "~") == 0) {
    if (!binary_emit_not_reg(&context->code, BINARY_GP_RAX)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "popcnt") == 0) {
    if (!wcs_popcnt(&context->code, BINARY_GP_RAX, BINARY_GP_RAX)) {
      goto emit_failure;
    }
  } else if (strcmp(op, "+") == 0) {
    /* No-op */
  } else {
    code_generator_set_error(generator,
                             "Direct object backend does not yet support IR "
                             "unary operator '%s' in function '%s'",
                             op, context->function_name);
    return 0;
  }

  if (!code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_RAX)) {
    return 0;
  }

  return 1;

emit_failure:
  code_generator_set_error(
      generator,
      "Out of memory while emitting IR unary operator in function '%s'",
      context->function_name);
  return 0;
}

int code_generator_binary_emit_instruction(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  if (!generator || !context || !instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_NOP:
    return 1;

  case IR_OP_LABEL:
    if (!instruction->text || instruction->text[0] == '\0') {
      code_generator_set_error(generator, "Malformed IR label in function '%s'",
                               context->function_name);
      return 0;
    }
    if (!binary_label_table_define(&context->labels, instruction->text,
                                   context->code.size)) {
      code_generator_set_error(generator,
                               "Duplicate or invalid IR label '%s' in "
                               "function '%s'",
                               instruction->text, context->function_name);
      return 0;
    }
    return 1;

  case IR_OP_JUMP: {
    size_t displacement_offset = 0;
    if (!instruction->text || instruction->text[0] == '\0') {
      code_generator_set_error(generator,
                               "Malformed IR jump target in function '%s'",
                               context->function_name);
      return 0;
    }
    if (!binary_emit_jmp_placeholder(&context->code, &displacement_offset) ||
        !binary_label_fixup_table_add(&context->label_fixups, instruction->text,
                                      displacement_offset)) {
      code_generator_set_error(generator,
                               "Out of memory while emitting IR jump");
      return 0;
    }
    return 1;
  }

  case IR_OP_BRANCH_ZERO: {
    size_t displacement_offset = 0;
    if (!instruction->text || instruction->text[0] == '\0') {
      code_generator_set_error(
          generator, "Malformed IR branch target in function '%s'",
          context->function_name);
      return 0;
    }
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_RAX) ||
        !binary_emit_test_reg_reg(&context->code, BINARY_GP_RAX) ||
        !binary_emit_je_placeholder(&context->code, &displacement_offset) ||
        !binary_label_fixup_table_add(&context->label_fixups, instruction->text,
                                      displacement_offset)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting branch_zero");
      }
      return 0;
    }
    return 1;
  }

  case IR_OP_BRANCH_EQ: {
    size_t displacement_offset = 0;
    if (!instruction->text || instruction->text[0] == '\0') {
      code_generator_set_error(
          generator, "Malformed IR branch target in function '%s'",
          context->function_name);
      return 0;
    }
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->rhs,
                                                 BINARY_GP_R10) ||
        !code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_RAX) ||
        !code_generator_binary_emit_reg_reg_compare(
            &context->code, BINARY_GP_RAX, BINARY_GP_R10,
            code_generator_binary_instruction_compare_width(generator, context,
                                                            instruction)) ||
        !binary_emit_je_placeholder(&context->code, &displacement_offset) ||
        !binary_label_fixup_table_add(&context->label_fixups, instruction->text,
                                      displacement_offset)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting branch_eq");
      }
      return 0;
    }
    return 1;
  }

  case IR_OP_ASSIGN: {
    const char *alias_target = NULL;
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name) {
      alias_target = binary_symbol_alias_table_get(&context->symbol_aliases,
                                                   instruction->dest.name);
      if (alias_target && instruction->lhs.kind == IR_OPERAND_SYMBOL &&
          instruction->lhs.name &&
          strcmp(alias_target, instruction->lhs.name) == 0) {
        return 1;
      }
    }
    /* Whole-struct copy: source is an aggregate symbol (a struct local/param/
     * global being copied by value, e.g. the inliner's lowering of `return
     * rect`). The default RAX round-trip below would copy only the low 8 bytes,
     * silently truncating any struct wider than a register. Handle the two
     * destinations the inliner produces:
     *   - dest is a TEMP: stash &source in the temp slot and tag the temp with
     *     the struct size, so the downstream TEMP->SYMBOL assign (handled just
     *     below) memcpys the full struct -- mirrors INDIRECT-return temps.
     *   - dest is an aggregate SYMBOL: memcpy &source -> &dest directly. */
    if (instruction->lhs.kind == IR_OPERAND_SYMBOL && instruction->lhs.name) {
      Type *src_type = code_generator_binary_get_operand_type_in_context(
          generator, context, &instruction->lhs);
      if (src_type && code_generator_type_is_aggregate(src_type)) {
        size_t struct_bytes = code_generator_abi_type_size(src_type);
        if (struct_bytes > 8 && instruction->dest.kind == IR_OPERAND_TEMP &&
            instruction->dest.name) {
          if (!code_generator_binary_emit_struct_destination_address(
                  generator, context, instruction->lhs.name, BINARY_GP_RAX)) {
            return 0;
          }
          if (!binary_indirect_temp_add(context, instruction->dest.name,
                                        struct_bytes)) {
            code_generator_set_error(
                generator, "Out of memory tagging struct-copy temp");
            return 0;
          }
          /* 8-byte spill keeps the pointer alive in the temp's slot. */
          if (!code_generator_binary_emit_destination_store(
                  generator, context, &instruction->dest, BINARY_GP_RAX)) {
            return 0;
          }
          return 1;
        }
        /* Exact-size copy for any aggregate that is not exactly one register
         * wide. A small struct whose size is not 1/2/4/8 (e.g. 3 uint8
         * fields) is allocated EXACTLY its size with alignment 1, so the
         * default 8-byte RAX round-trip below would write past the
         * destination slot and silently clobber whatever local is adjacent
         * (the copy SOURCE itself, in `var copy = orig` layouts). */
        if (struct_bytes != 8 && instruction->dest.kind == IR_OPERAND_SYMBOL &&
            instruction->dest.name) {
          Symbol *dest_sym = symbol_table_lookup(generator->symbol_table,
                                                 instruction->dest.name);
          if (!dest_sym || !dest_sym->type ||
              code_generator_type_is_aggregate(dest_sym->type)) {
            if (!code_generator_binary_emit_struct_destination_address(
                    generator, context, instruction->lhs.name, BINARY_GP_RAX) ||
                !code_generator_binary_emit_struct_destination_address(
                    generator, context, instruction->dest.name,
                    BINARY_GP_RDX) ||
                !code_generator_binary_emit_rep_movsb(generator, context,
                                                      BINARY_GP_RAX,
                                                      BINARY_GP_RDX,
                                                      struct_bytes)) {
              if (!generator->has_error) {
                code_generator_set_error(
                    generator, "Out of memory copying struct-to-struct assign");
              }
              return 0;
            }
            return 1;
          }
        }
      }
    }
    /* Indirect-return propagation: source is a temp tagged as holding a
     * pointer to a struct returned from an INDIRECT-returning call; dest
     * is a struct symbol. Memcpy from *src_ptr into &dest. */
    if (instruction->lhs.kind == IR_OPERAND_TEMP && instruction->lhs.name &&
        instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name) {
      size_t bytes = binary_indirect_temp_get(context, instruction->lhs.name);
      if (bytes > 0) {
        Symbol *dest_sym = symbol_table_lookup(generator->symbol_table,
                                               instruction->dest.name);
        if (!dest_sym || !dest_sym->type ||
            code_generator_type_is_aggregate(dest_sym->type)) {
          /* Load src pointer (the temp slot stores the pointer value). */
          int src_offset =
              code_generator_binary_get_temp_offset(context,
                                                    instruction->lhs.name);
          if (src_offset <= 0) {
            code_generator_set_error(
                generator,
                "Cannot resolve temp '%s' for INDIRECT-return assign",
                instruction->lhs.name);
            return 0;
          }
          if (!binary_emit_mov_reg_mem(&context->code, BINARY_GP_RAX,
                                       BINARY_GP_RBP, -src_offset) ||
              !code_generator_binary_emit_struct_destination_address(
                  generator, context, instruction->dest.name, BINARY_GP_RDX) ||
              !code_generator_binary_emit_rep_movsb(generator, context,
                                                    BINARY_GP_RAX,
                                                    BINARY_GP_RDX, bytes)) {
            if (!generator->has_error) {
              code_generator_set_error(
                  generator, "Out of memory copying INDIRECT-return assign");
            }
            return 0;
          }
          return 1;
        }
      }
    }
    /* Fast path: assigning into a promoted destination register. Load the
     * source straight into the destination register instead of routing through
     * RAX and copying back (`scan = prev` between two promoted registers
     * collapses to a single `mov DR, SR`, or nothing when DR==SR). Restricted
     * to non-float assigns; float assigns may need a precision conversion that
     * the RAX path handles below. */
    if (!instruction->is_float && instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name) {
      BinaryGpRegister assign_dest_reg = BINARY_GP_RAX;
      if (code_generator_binary_symbol_assigned_register(
              generator, context, instruction->dest.name, &assign_dest_reg)) {
        Type *assign_dest_type =
            code_generator_binary_get_operand_type_in_context(
                generator, context, &instruction->dest);
        int dest_scalar_width =
            code_generator_binary_type_scalar_width(assign_dest_type);
        int dest_is_cstring =
            code_generator_binary_type_is_cstring(assign_dest_type) ||
            binary_named_slot_table_get_offset(&context->cstring_symbols,
                                               instruction->dest.name) >= 0;
        Type *effective_dest_type =
            assign_dest_type ? assign_dest_type
                             : (generator->type_checker
                                    ? generator->type_checker->builtin_cstring
                                    : NULL);
        int assign_ok = 0;
        int assign_canonicalized = 0;
        if (instruction->lhs.kind == IR_OPERAND_TEMP &&
            instruction->lhs.name && dest_scalar_width == 4) {
          int offset = code_generator_binary_get_temp_offset(
              context, instruction->lhs.name);
          assign_ok = offset > 0 &&
                      code_generator_binary_emit_temp_stack_load(
                          generator, context, offset, assign_dest_reg,
                          assign_dest_type);
          assign_canonicalized = assign_ok; /* 32-bit load extends */
        } else {
          assign_ok = dest_is_cstring
                          ? code_generator_binary_emit_call_argument_load(
                                generator, context, &instruction->lhs,
                                effective_dest_type, assign_dest_reg)
                          : code_generator_binary_emit_operand_load(
                                generator, context, &instruction->lhs,
                                assign_dest_reg);
        }
        if (!assign_ok) {
          if (!generator->has_error) {
            code_generator_set_error(generator,
                                     "Out of memory while emitting assign");
          }
          return 0;
        }
        /* A promoted narrow destination must hold its canonical wrapped value
         * after assignment, matching stack homes and the MIR backend. */
        if (!assign_canonicalized &&
            !binary_canonicalize_narrow_reg_for_type(
                context, assign_dest_type, assign_dest_reg)) {
          return 0;
        }
        return 1;
      }
    }

    Type *assign_dest_type =
        code_generator_binary_get_operand_type(generator, &instruction->dest);
    int dest_is_cstring =
        code_generator_binary_type_is_cstring(assign_dest_type) ||
        (instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
         binary_named_slot_table_get_offset(&context->cstring_symbols,
                                            instruction->dest.name) >= 0);
    if (dest_is_cstring) {
      Type *effective_dest_type =
          assign_dest_type ? assign_dest_type
                           : (generator->type_checker
                                  ? generator->type_checker->builtin_cstring
                                  : NULL);
      if (!code_generator_binary_emit_call_argument_load(
              generator, context, &instruction->lhs, effective_dest_type,
              BINARY_GP_RAX)) {
        if (!generator->has_error) {
          code_generator_set_error(generator,
                                   "Out of memory while emitting cstring assign");
        }
        return 0;
      }
    } else if (!code_generator_binary_emit_operand_load(generator, context,
                                                        &instruction->lhs,
                                                        BINARY_GP_RAX)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting assign");
      }
      return 0;
    }
    /* Convert when a float value is assigned into a destination of a
     * different float precision (instruction->float_bits = target width,
     * set by ir_lowering from the declared/symbol type). */
    if (instruction->is_float && instruction->float_bits) {
      int value_bits = code_generator_binary_operand_float_bits(
          generator, context, &instruction->lhs);
      if (value_bits &&
          !code_generator_binary_emit_float_reg_convert(
              context, BINARY_GP_RAX, value_bits, instruction->float_bits)) {
        code_generator_set_error(generator,
                                 "Out of memory while converting float assign "
                                 "precision in function '%s'",
                                 context->function_name);
        return 0;
      }
    }
    if (!code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting assign");
      }
      return 0;
    }
    return 1;
  }

  case IR_OP_ADDRESS_OF:
    return code_generator_binary_emit_address_of(generator, context,
                                                 instruction);

  case IR_OP_LOAD:
    return code_generator_binary_emit_load(generator, context, instruction);

  case IR_OP_STORE:
    return code_generator_binary_emit_store(generator, context, instruction);

  case IR_OP_PREFETCH:
    /* Advisory cache hint: materialize the precomputed address and emit
     * prefetcht0 [reg]. Never faults, so no guards are needed here. */
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_RAX) ||
        !binary_emit_prefetcht0_mem(&context->code, BINARY_GP_RAX, 0)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting prefetch in "
                                 "function '%s'",
                                 context->function_name);
      }
      return 0;
    }
    return 1;

  case IR_OP_SELECT: {
    /* dest = (cond != 0) ? then : else. Materialize else into RDX (the
     * result register), then into RCX, cond into RAX, and cmovnz RDX<-RCX. */
    const IROperand *else_val =
        instruction->argument_count > 0 ? &instruction->arguments[0] : NULL;
    if (!else_val ||
        !code_generator_binary_emit_operand_load(generator, context, else_val,
                                                 BINARY_GP_RDX) ||
        !code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->rhs,
                                                 BINARY_GP_RCX) ||
        !code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_RAX) ||
        !binary_emit_alu_reg_reg(&context->code, 0x85, BINARY_GP_RAX,
                                 BINARY_GP_RAX) || /* test rax, rax */
        !binary_emit_cmovcc_reg_reg(&context->code, 0x45, BINARY_GP_RDX,
                                    BINARY_GP_RCX) || /* cmovnz rdx, rcx */
        !code_generator_binary_emit_destination_store(
            generator, context, &instruction->dest, BINARY_GP_RDX)) {
      if (!generator->has_error) {
        code_generator_set_error(generator,
                                 "Out of memory while emitting select in "
                                 "function '%s'",
                                 context->function_name);
      }
      return 0;
    }
    return 1;
  }

  case IR_OP_BINARY:
    return code_generator_binary_emit_binary(generator, context, instruction);

  case IR_OP_ROTATE_ADD:
    return code_generator_binary_emit_rotate_add(generator, context,
                                                 instruction);

  case IR_OP_UNARY:
    return code_generator_binary_emit_unary(generator, context, instruction);

  case IR_OP_CALL:
    return code_generator_binary_emit_call(generator, context, instruction);

  case IR_OP_CALL_INDIRECT:
    return code_generator_binary_emit_call_indirect(generator, context,
                                                    instruction);

  case IR_OP_NEW:
    return code_generator_binary_emit_new(generator, context, instruction);

  case IR_OP_CAST:
    return code_generator_binary_emit_cast(generator, context, instruction);

  case IR_OP_RETURN: {
    size_t displacement_offset = 0;
    /* INDIRECT return: memcpy the source struct through the hidden out-ptr
     * stored at [rbp - 8], then put that pointer into rax. */
    if (context->returns_indirect &&
        instruction->lhs.kind != IR_OPERAND_NONE) {
      if (!code_generator_binary_emit_indirect_source_address(
              generator, context, &instruction->lhs, BINARY_GP_RAX)) {
        return 0;
      }
      /* dst = qword [rbp - 8]; rep movsb. */
      if (!binary_emit_mov_reg_mem(&context->code, BINARY_GP_RDX,
                                   BINARY_GP_RBP, -8) ||
          !code_generator_binary_emit_rep_movsb(generator, context,
                                                BINARY_GP_RAX, BINARY_GP_RDX,
                                                context->indirect_return_size) ||
          !binary_emit_mov_reg_mem(&context->code, BINARY_GP_RAX,
                                   BINARY_GP_RBP, -8)) {
        code_generator_set_error(generator,
                                 "Out of memory emitting indirect return");
        return 0;
      }
      if (!binary_emit_jmp_placeholder(&context->code, &displacement_offset) ||
          !binary_offset_table_add(&context->return_fixups,
                                   displacement_offset)) {
        code_generator_set_error(
            generator, "Out of memory while emitting function return");
        return 0;
      }
      return 1;
    }
    if (instruction->lhs.kind != IR_OPERAND_NONE &&
        !code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_RAX)) {
      return 0;
    }
    /* Canonicalize a narrow integer return to 64 bits (mirrors the MIR
     * backend's scalar_return_width handling): the value may be a 64-bit
     * arithmetic temp carrying garbage above the return type's width, and a
     * caller using the full register (e.g. `(int64)narrow_fn()`) would read
     * that garbage. Zero-extend unsigned / sign-extend signed. */
    if (instruction->lhs.kind != IR_OPERAND_NONE && !instruction->is_float &&
        context->function_data) {
      Type *ret_type = code_generator_binary_get_resolved_type(
          generator, context->function_data->return_type, 1);
      if (ret_type && !code_generator_type_is_aggregate(ret_type) &&
          code_generator_binary_resolved_type_float_bits(ret_type) == 0) {
        int rw = code_generator_binary_resolved_type_scalar_size(ret_type);
        int rsigned =
            code_generator_binary_resolved_type_is_signed_integer(ret_type);
        int ok = 1;
        if (rw == 4) {
          ok = rsigned ? binary_emit_movsxd_reg_reg32(&context->code,
                                                      BINARY_GP_RAX,
                                                      BINARY_GP_RAX)
                       : binary_emit_movzx_reg_reg32(&context->code,
                                                     BINARY_GP_RAX,
                                                     BINARY_GP_RAX);
        } else if (rw == 2) {
          ok = rsigned ? binary_emit_movsx_reg_reg16(&context->code,
                                                     BINARY_GP_RAX,
                                                     BINARY_GP_RAX)
                       : binary_emit_movzx_reg_reg16(&context->code,
                                                     BINARY_GP_RAX,
                                                     BINARY_GP_RAX);
        } else if (rw == 1) {
          ok = rsigned ? binary_emit_movsx_reg_reg8(&context->code,
                                                    BINARY_GP_RAX,
                                                    BINARY_GP_RAX)
                       : binary_emit_movzx_reg_reg8(&context->code,
                                                    BINARY_GP_RAX,
                                                    BINARY_GP_RAX);
        }
        if (!ok) {
          return 0;
        }
      }
    }
    /* Convert the returned value to the function's float return precision
     * (instruction->float_bits set by ir_lowering) so the epilogue's
     * RAX->XMM0 transfer carries correctly-rounded bits. */
    if (instruction->lhs.kind != IR_OPERAND_NONE && instruction->is_float &&
        instruction->float_bits) {
      int value_bits = code_generator_binary_operand_float_bits(
          generator, context, &instruction->lhs);
      if (value_bits &&
          !code_generator_binary_emit_float_reg_convert(
              context, BINARY_GP_RAX, value_bits, instruction->float_bits)) {
        code_generator_set_error(generator,
                                 "Out of memory while converting float return "
                                 "precision in function '%s'",
                                 context->function_name);
        return 0;
      }
    }
    if (!binary_emit_jmp_placeholder(&context->code, &displacement_offset) ||
        !binary_offset_table_add(&context->return_fixups, displacement_offset)) {
      code_generator_set_error(generator,
                               "Out of memory while emitting function return");
      return 0;
    }
    return 1;
  }

  case IR_OP_DECLARE_LOCAL:
    if (instruction->dest.kind != IR_OPERAND_SYMBOL ||
        !instruction->dest.name || instruction->dest.name[0] == '\0' ||
        code_generator_binary_get_local_offset(context, instruction->dest.name) <=
            0) {
      code_generator_set_error(generator,
                               "Malformed local declaration in function '%s'",
                               context->function_name);
      return 0;
    }
    return 1;

  case IR_OP_COUNT_WORD_STARTS:
    return code_generator_binary_emit_count_word_starts(generator, context,
                                                        instruction);

  case IR_OP_MEMCPY_INLINE:
    return code_generator_binary_emit_memcpy_inline(generator, context,
                                                    instruction);

  case IR_OP_SIMD_SUM_I32:
    return code_generator_binary_emit_simd_sum_i32(generator, context,
                                                   instruction);

  case IR_OP_SIMD_LCG_U32:
    return code_generator_binary_emit_simd_lcg_u32(generator, context,
                                                   instruction);

  case IR_OP_SIMD_SUM_U8:
    return code_generator_binary_emit_simd_sum_u8(generator, context,
                                                  instruction);

  case IR_OP_SIMD_BYTE_MAP:
    return code_generator_binary_emit_simd_byte_map(generator, context,
                                                    instruction);

  case IR_OP_SIMD_FILL:
    return code_generator_binary_emit_simd_fill(generator, context,
                                                instruction);

  case IR_OP_SIMD_DOT_I32:
    return code_generator_binary_emit_simd_dot_i32(generator, context,
                                                   instruction);

  case IR_OP_SIMD_DOT_I8:
    return code_generator_binary_emit_simd_dot_i8(generator, context,
                                                  instruction);

  case IR_OP_SIMD_SLP_MAC_I32:
    return code_generator_binary_emit_simd_slp_mac_i32(generator, context,
                                                       instruction);

  case IR_OP_SIMD_SLP_MAC_I8:
    return code_generator_binary_emit_simd_slp_mac_i8(generator, context,
                                                      instruction);

  case IR_OP_SIMD_MATMUL_N32:
    return code_generator_binary_emit_simd_matmul_n32(generator, context,
                                                      instruction);

  case IR_OP_SIMD_INSERTION_SORT_I32:
    return code_generator_binary_emit_simd_insertion_sort_i32(generator, context,
                                                              instruction);

  case IR_OP_SIMD_SCALE_I32:
    return code_generator_binary_emit_simd_scale_i32(generator, context,
                                                       instruction);

  case IR_OP_SIMD_CLAMP_I32:
    return code_generator_binary_emit_simd_clamp_i32(generator, context,
                                                     instruction);

  case IR_OP_SIMD_REVERSE_COPY_I32:
    return code_generator_binary_emit_simd_reverse_copy_i32(generator, context,
                                                            instruction);

  case IR_OP_LOWER_BOUND_I32:
    return code_generator_binary_emit_lower_bound_i32(generator, context,
                                                      instruction);

  case IR_OP_PREFIX_SUM_I32:
    return code_generator_binary_emit_prefix_sum_i32(generator, context,
                                                     instruction);

  case IR_OP_SIMD_MINMAX_I32:
    return code_generator_binary_emit_simd_minmax_i32(generator, context,
                                                      instruction);

  case IR_OP_SIMD_SUM_F64:
    return code_generator_binary_emit_simd_sum_f64(generator, context,
                                                   instruction);
  case IR_OP_SIMD_SUM_F32:
    return code_generator_binary_emit_simd_sum_f32(generator, context,
                                                   instruction);
  case IR_OP_SIMD_DOT_F64:
    return code_generator_binary_emit_simd_dot_f64(generator, context,
                                                   instruction);
  case IR_OP_SIMD_DOT_F32:
    return code_generator_binary_emit_simd_dot_f32(generator, context,
                                                   instruction);
  case IR_OP_SIMD_AFFINE_MAP_F64:
    return code_generator_binary_emit_simd_affine_map_f64(generator, context,
                                                          instruction);
  case IR_OP_SIMD_EXP_F32:
    return code_generator_binary_emit_simd_exp_f32(generator, context,
                                                   instruction);
  case IR_OP_SIMD_SILU_F32:
    return code_generator_binary_emit_simd_silu_f32(generator, context,
                                                    instruction);

  case IR_OP_SIMD_AFFINE_MAP_F32:
    return code_generator_binary_emit_simd_affine_map_f32(generator, context,
                                                          instruction);
  case IR_OP_SIMD_I2F_REDUCE_F64:
    return code_generator_binary_emit_simd_i2f_reduce_f64(generator, context,
                                                          instruction);
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
    return code_generator_binary_emit_simd_vloop_f64(generator, context,
                                                     instruction, 0);
  case IR_OP_SIMD_FIND:
    return code_generator_binary_emit_simd_find(generator, context,
                                                instruction);
  case IR_OP_SIMD_OUTER_LANE_F64:
    return code_generator_binary_emit_simd_outer_lane_f64(generator, context,
                                                          instruction);

  default:
    code_generator_set_error(
        generator,
        "Direct object backend does not yet support IR opcode %d in "
        "function '%s'",
        (int)instruction->op, context->function_name);
    return 0;
  }
}

/* Windows reserves the stack lazily behind a single guard page: touching the
 * guard page commits it and moves the guard down by one page. A prologue that
 * lowers rsp by more than a page in one `sub` can step *over* the guard page
 * without ever touching it, so the first write into the new frame faults --
 * exactly the crash seen on functions with very large frames (e.g. main() with
 * hundreds of call sites). Microsoft's ABI requires a stack probe for frames
 * larger than a page: touch each page as rsp descends so the guard moves down
 * one page at a time. We do an unrolled probe (no helper call): for each 4 KiB
 * step, `sub rsp, 4096` then write to [rsp], then handle the remainder. RAX is
 * scratch here (prologue runs before any value is live in it). */
static int binary_emit_stack_probe_touch(BinaryCodeBuffer *code) {
  if (!code) {
    return 0;
  }
  /* test byte ptr [rsp], 0 */
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
    /* Touch the freshly-stepped page so the guard page is hit in order. */
    if (!binary_emit_stack_probe_touch(code)) {
      return 0;
    }
    remaining -= BINARY_STACK_PAGE_SIZE;
  }
  /* Final (sub-page) remainder; touch it too to commit the last page. */
  if (!binary_emit_sub_rsp_imm32(code, (uint32_t)remaining)) {
    return 0;
  }
  return binary_emit_stack_probe_touch(code);
}

int code_generator_binary_emit_prologue(CodeGenerator *generator,
                                               BinaryFunctionContext *context,
                                               FunctionDeclaration *function_data) {
  if (!generator || !context || !function_data) {
    return 0;
  }

  if (!binary_emit_push_reg(&context->code, BINARY_GP_RBP) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_RBP, BINARY_GP_RSP)) {
    code_generator_set_error(generator,
                             "Out of memory while emitting function prologue");
    return 0;
  }

  if (!binary_emit_frame_allocation(&context->code, context->frame_size)) {
    code_generator_set_error(generator,
                             "Out of memory while allocating stack frame");
    return 0;
  }

  for (size_t i = 0; i < context->saved_register_count; i++) {
    if (!binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP,
                                 -context->saved_register_offsets[i],
                                 context->saved_registers[i])) {
      code_generator_set_error(generator,
                               "Out of memory while saving callee registers");
      return 0;
    }
  }

  if (!code_generator_binary_emit_promoted_global_loads(generator, context)) {
    return 0;
  }

  const BinaryAbi *abi = code_generator_binary_active_abi();

  /* Hidden return out-pointer: stash it at the fixed home slot [rbp - 8] before
   * homing user parameters. It arrives in the ABI's indirect-return register
   * (MS-x64 RCX, SysV RDI) and occupies the leading ABI argument slot, so user
   * params shift one slot when an INDIRECT return is in use. */
  if (context->returns_indirect) {
    if (!binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP, -8,
                                 abi->indirect_return_register)) {
      code_generator_set_error(generator,
                               "Out of memory homing hidden return ptr");
      return 0;
    }
  }

  /* Build the incoming-argument layout: the hidden out-pointer (if any) is a
   * leading integer argument, followed by the user parameters classified by
   * float-ness. The layout tells us each argument's register or stack home
   * under the active convention. */
  size_t hidden = context->returns_indirect ? 1u : 0u;
  size_t layout_count = function_data->parameter_count + hidden;
  if (layout_count > 0) {
    int *is_float = calloc(layout_count, sizeof(int));
    BinaryArgLocation *locations =
        calloc(layout_count, sizeof(BinaryArgLocation));
    if (!is_float || !locations) {
      free(is_float);
      free(locations);
      code_generator_set_error(generator,
                               "Out of memory computing parameter layout");
      return 0;
    }
    /* Hidden out-pointer is integer (is_float[0] already 0). */
    for (size_t i = 0; i < function_data->parameter_count; i++) {
      int fbits = code_generator_binary_named_type_float_bits(
          generator, function_data->parameter_types
                         ? function_data->parameter_types[i]
                         : NULL);
      is_float[i + hidden] = fbits ? 1 : 0;
    }
    if (!code_generator_binary_compute_arg_layout(abi, is_float, layout_count,
                                                  locations, NULL)) {
      free(is_float);
      free(locations);
      code_generator_set_error(generator, "Failed to compute parameter layout");
      return 0;
    }

    for (size_t i = 0; i < function_data->parameter_count; i++) {
      const char *parameter_name = function_data->parameter_names[i];
      int parameter_fbits = code_generator_binary_named_type_float_bits(
          generator, function_data->parameter_types
                         ? function_data->parameter_types[i]
                         : NULL);
      BinaryGpRegister assigned_register = BINARY_GP_RAX;
      int parameter_in_register =
          code_generator_binary_symbol_assigned_register(
              generator, context, parameter_name, &assigned_register);
      int home_offset =
          code_generator_binary_get_parameter_offset(context, parameter_name);
      if (home_offset <= 0) {
        free(is_float);
        free(locations);
        code_generator_set_error(
            generator, "Missing parameter home for '%s' in function '%s'",
            parameter_name ? parameter_name : "<unnamed>",
            context->function_name);
        return 0;
      }

      const BinaryArgLocation *loc = &locations[i + hidden];
      /* Callee-side address of a stack argument: above saved rbp + return
       * address (16) and the callee-owned shadow space, then the layout's
       * outgoing offset. */
      int incoming_stack_offset =
          16 + abi->shadow_space_size + loc->stack_offset;

      int home_ok = 1;
      if (parameter_in_register) {
        /* Symbol pinned to a specific GP register by the allocator. */
        if (loc->kind == BINARY_ARG_IN_GP_REGISTER) {
          home_ok = binary_emit_mov_reg_reg(&context->code, assigned_register,
                                            loc->gp_register);
        } else if (loc->kind == BINARY_ARG_IN_XMM_REGISTER) {
          home_ok = (parameter_fbits == 32
                         ? binary_emit_movd_reg_xmm(&context->code,
                                                    assigned_register,
                                                    loc->xmm_register)
                         : binary_emit_movq_reg_xmm(&context->code,
                                                    assigned_register,
                                                    loc->xmm_register));
        } else {
          home_ok = binary_emit_mov_reg_mem(&context->code, assigned_register,
                                            BINARY_GP_RBP,
                                            incoming_stack_offset);
        }
        if (!home_ok) {
          free(is_float);
          free(locations);
          code_generator_set_error(
              generator, "Out of memory while homing register parameters");
          return 0;
        }
        continue;
      }

      if (loc->kind == BINARY_ARG_IN_XMM_REGISTER) {
        /* Float params arrive in XMM; copy the bits to GP at the param's
         * precision before homing to the stack slot. */
        home_ok = (parameter_fbits == 32
                       ? binary_emit_movd_reg_xmm(&context->code, BINARY_GP_RAX,
                                                  loc->xmm_register)
                       : binary_emit_movq_reg_xmm(&context->code, BINARY_GP_RAX,
                                                  loc->xmm_register)) &&
                  binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP,
                                          -home_offset, BINARY_GP_RAX);
      } else if (loc->kind == BINARY_ARG_IN_GP_REGISTER) {
        home_ok = binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP,
                                          -home_offset, loc->gp_register);
      } else {
        home_ok =
            binary_emit_mov_reg_mem(&context->code, BINARY_GP_RAX, BINARY_GP_RBP,
                                    incoming_stack_offset) &&
            binary_emit_mov_mem_reg(&context->code, BINARY_GP_RBP, -home_offset,
                                    BINARY_GP_RAX);
      }
      if (!home_ok) {
        free(is_float);
        free(locations);
        code_generator_set_error(generator,
                                 "Out of memory while homing parameters");
        return 0;
      }
    }

    free(is_float);
    free(locations);
  }

  return 1;
}

int code_generator_binary_emit_profile_enter(
    CodeGenerator *generator, BinaryFunctionContext *context, uint32_t fn_id) {
  size_t displacement_offset = 0;
  const char *symbol = "mettle_profile_enter";

  if (!generator || !context) {
    return 0;
  }

  if (!code_generator_binary_declare_external_symbol(generator, symbol)) {
    return 0;
  }

  if (!binary_emit_mov_reg_imm32_zero_extend(&context->code, BINARY_GP_RCX,
                                             fn_id) ||
      !binary_emit_sub_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE) ||
      !binary_emit_call_placeholder(&context->code, &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, symbol,
                                        displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE)) {
    if (!generator->has_error) {
      code_generator_set_error(
          generator,
          "Out of memory while emitting profile enter in function '%s'",
          context->function_name);
    }
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_profile_exit(CodeGenerator *generator,
                                            BinaryFunctionContext *context) {
  size_t displacement_offset = 0;
  const char *symbol = "mettle_profile_exit";

  if (!generator || !context) {
    return 0;
  }

  if (!code_generator_binary_declare_external_symbol(generator, symbol)) {
    return 0;
  }

  if (!binary_emit_push_reg(&context->code, BINARY_GP_RAX) ||
      !binary_emit_sub_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE) ||
      !binary_emit_call_placeholder(&context->code, &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, symbol,
                                        displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE) ||
      !binary_emit_pop_reg(&context->code, BINARY_GP_RAX)) {
    if (!generator->has_error) {
      code_generator_set_error(
          generator,
          "Out of memory while emitting profile exit in function '%s'",
          context->function_name);
    }
    return 0;
  }

  return 1;
}

int code_generator_binary_emit_profile_op(CodeGenerator *generator,
                                          BinaryFunctionContext *context,
                                          uint32_t op_class, uint64_t amount) {
  size_t displacement_offset = 0;
  const char *symbol = "mettle_profile_op";

  if (!generator || !context) {
    return 0;
  }

  if (!code_generator_binary_declare_external_symbol(generator, symbol)) {
    return 0;
  }

  if (!binary_emit_mov_reg_imm32_zero_extend(&context->code, BINARY_GP_RCX,
                                             op_class) ||
      !binary_emit_mov_reg_imm32_zero_extend(&context->code, BINARY_GP_RDX,
                                             (uint32_t)amount) ||
      !binary_emit_sub_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE) ||
      !binary_emit_call_placeholder(&context->code, &displacement_offset) ||
      !binary_call_relocation_table_add(&context->call_relocations, symbol,
                                        displacement_offset) ||
      !binary_emit_add_rsp_imm32(&context->code,
                                 BINARY_WIN64_SHADOW_SPACE_SIZE)) {
    if (!generator->has_error) {
      code_generator_set_error(
          generator,
          "Out of memory while emitting profile op counter in function '%s'",
          context->function_name);
    }
    return 0;
  }

  return 1;
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
