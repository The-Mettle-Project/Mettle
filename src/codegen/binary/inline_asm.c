#include "codegen/binary/internal.h"
#include "codegen/asm/x86_asm.h"
#include "codegen/target.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  CodeGenerator *generator;
  BinaryFunctionContext *context;
} BinaryAsmBindings;

static int binary_asm_slot_operand(BinaryFunctionContext *context, int offset,
                                   int bytes, X86AsmOperand *out) {
  memset(out, 0, sizeof(*out));
  out->kind = X86_ASM_OPERAND_MEM;
  out->scale = 1;
  out->has_base = 1;
  out->address_bytes = 8;
  out->mem_bytes = bytes;
  if (context->omit_frame_pointer) {
    out->base = BINARY_GP_RSP;
    out->disp = (long long)context->frame_size - offset;
  } else {
    out->base = BINARY_GP_RBP;
    out->disp = -(long long)offset;
  }
  return 1;
}

static int binary_asm_type_bytes(const MtlcType *type) {
  int size = 0;
  if (!type) {
    return 8;
  }
  size = (int)type->size;
  if (size == 1 || size == 2 || size == 4 || size == 8) {
    return size;
  }
  return 8;
}

static int binary_asm_resolve_binding(void *binding_context, const char *name,
                                      X86AsmOperand *out, char *error,
                                      size_t error_size) {
  BinaryAsmBindings *bindings = (BinaryAsmBindings *)binding_context;
  CodeGenerator *generator = bindings->generator;
  BinaryFunctionContext *context = bindings->context;
  BinaryGpRegister assigned = BINARY_GP_RAX;
  int offset = 0;
  const CgSym *symbol = NULL;

  if (!name || name[0] == '\0') {
    snprintf(error, error_size, "empty `{}` operand binding");
    return 0;
  }

  {
    int register_class = 0;
    if (x86_asm_lookup_register(name, &register_class, NULL, NULL, NULL)) {
      snprintf(error, error_size,
               "`{%s}` names a machine register; write `%s` directly", name,
               name);
      return 0;
    }
  }

  if (code_generator_binary_symbol_assigned_register(generator, context, name,
                                                     &assigned)) {
    memset(out, 0, sizeof(*out));
    out->kind = X86_ASM_OPERAND_REG;
    out->reg_class = X86_ASM_REG_GP;
    out->reg = (int)assigned;
    out->reg_bytes = 8;
    out->scale = 1;
    return 1;
  }

  offset = code_generator_binary_get_symbol_offset(context, name);
  if (offset > 0) {
    MtlcType *type = NULL;
    IROperand probe;
    memset(&probe, 0, sizeof(probe));
    probe.kind = IR_OPERAND_SYMBOL;
    probe.name = (char *)name;
    type = code_generator_binary_get_operand_type_in_context(generator, context,
                                                             &probe);
    return binary_asm_slot_operand(context, offset, binary_asm_type_bytes(type),
                                   out);
  }

  offset = code_generator_binary_get_temp_offset(context, name);
  if (offset > 0) {
    return binary_asm_slot_operand(context, offset, 8, out);
  }

  symbol = generator && generator->ir_program
               ? code_generator_lookup_symbol(generator, name)
               : NULL;
  if (symbol && symbol->scope && symbol->scope->type == CG_SCOPE_GLOBAL) {
    const char *link_name = code_generator_get_link_symbol_name(generator, name);
    if (!link_name || link_name[0] == '\0') {
      snprintf(error, error_size, "global `%s` has no link name", name);
      return 0;
    }
    if (symbol->is_extern &&
        !code_generator_binary_declare_external_symbol(generator, link_name)) {
      snprintf(error, error_size, "cannot declare `%s` as external", link_name);
      return 0;
    }
    memset(out, 0, sizeof(*out));
    out->kind = X86_ASM_OPERAND_MEM;
    out->scale = 1;
    out->rip_relative = 1;
    out->address_bytes = 8;
    out->mem_bytes = binary_asm_type_bytes(symbol->type);
    out->symbol = (char *)link_name;
    return 1;
  }

  snprintf(error, error_size,
           "`{%s}` names no local, parameter or global in scope", name);
  return 0;
}

int binary_asm_relocation_table_add(BinaryAsmRelocationTable *table,
                                    const char *symbol_name, size_t offset,
                                    int kind, int32_t addend) {
  if (!table || !symbol_name) {
    return 0;
  }
  if (table->count == table->capacity) {
    size_t capacity = table->capacity ? table->capacity * 2 : 4;
    BinaryAsmRelocation *grown = (BinaryAsmRelocation *)realloc(
        table->items, capacity * sizeof(BinaryAsmRelocation));
    if (!grown) {
      return 0;
    }
    table->items = grown;
    table->capacity = capacity;
  }
  table->items[table->count].symbol_name = mettle_strdup(symbol_name);
  if (!table->items[table->count].symbol_name) {
    return 0;
  }
  table->items[table->count].offset = offset;
  table->items[table->count].kind = kind;
  table->items[table->count].addend = addend;
  table->count++;
  return 1;
}

void binary_asm_relocation_table_destroy(BinaryAsmRelocationTable *table) {
  size_t i;
  if (!table) {
    return;
  }
  for (i = 0; i < table->count; i++) {
    free(table->items[i].symbol_name);
  }
  free(table->items);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
}

int code_generator_binary_assemble_text(CodeGenerator *generator,
                                        BinaryFunctionContext *context,
                                        const char *text, int bits,
                                        int allow_bits_directive,
                                        const SourceLocation *location,
                                        int *final_bits_out) {
  BinaryAsmBindings bindings;
  X86AsmConfig config;
  X86AsmResult result;
  char error[256];
  int error_line = 0;
  size_t base = 0;
  size_t i;

  if (!generator || !context || !text) {
    return 0;
  }

  bindings.generator = generator;
  bindings.context = context;

  memset(&config, 0, sizeof(config));
  config.bits = bits;
  config.origin = context->code.size;
  config.resolve_binding = binary_asm_resolve_binding;
  config.binding_context = &bindings;
  config.allow_bits_directive = allow_bits_directive;

  memset(&result, 0, sizeof(result));
  error[0] = '\0';
  if (!x86_asm_assemble(text, &config, &result, error, sizeof(error),
                        &error_line)) {
    generator->has_user_error = 1;
    code_generator_set_error(
        generator, "asm block in '%s' (line %d of the block): %s",
        context->function_name ? context->function_name : "<unknown>",
        error_line, error);
    (void)location;
    return 0;
  }

  base = context->code.size;
  if (!binary_code_buffer_append_bytes(&context->code, result.code, result.size)) {
    code_generator_set_error(generator,
                             "Out of memory while emitting an asm block");
    x86_asm_result_destroy(&result);
    return 0;
  }

  for (i = 0; i < result.fixup_count; i++) {
    const X86AsmFixup *fixup = &result.fixups[i];
    const char *symbol = fixup->symbol;
    int kind;
    long long value = fixup->addend;

    if (fixup->block_local) {
      symbol = context->function_name;
      value += (long long)base;
    }
    if (!symbol || symbol[0] == '\0') {
      code_generator_set_error(generator,
                               "asm block in '%s' references an unnamed symbol",
                               context->function_name);
      x86_asm_result_destroy(&result);
      return 0;
    }

    if (fixup->kind == X86_ASM_FIXUP_PC_RELATIVE) {
      if (fixup->bytes == 2) {
        kind = BINARY_RELOCATION_REL16;
        value += (long long)fixup->offset + 2 -
                 (long long)fixup->next_instruction_offset;
      } else if (fixup->bytes == 4) {
        kind = BINARY_RELOCATION_REL32;
        value += (long long)fixup->offset + 4 -
                 (long long)fixup->next_instruction_offset;
      } else {
        generator->has_user_error = 1;
        code_generator_set_error(
            generator,
            "asm block in '%s': a short branch cannot reach the symbol `%s`; "
            "use the near form",
            context->function_name, symbol);
        x86_asm_result_destroy(&result);
        return 0;
      }
    } else if (fixup->bytes == 8) {
      kind = BINARY_RELOCATION_ADDR64;
    } else if (fixup->bytes == 4) {
      kind = BINARY_RELOCATION_ADDR32NB;
    } else if (fixup->bytes == 2) {
      kind = BINARY_RELOCATION_ADDR16;
    } else {
      generator->has_user_error = 1;
      code_generator_set_error(
          generator,
          "asm block in '%s': a %d-byte reference to `%s` cannot be relocated "
          "in an object file",
          context->function_name, fixup->bytes, symbol);
      x86_asm_result_destroy(&result);
      return 0;
    }

    {
      int b;
      for (b = 0; b < fixup->bytes; b++) {
        context->code.data[base + fixup->offset + (size_t)b] =
            (unsigned char)(((unsigned long long)value >> (8 * b)) & 0xFFu);
      }
    }

    if (!binary_asm_relocation_table_add(&context->asm_relocations, symbol,
                                         base + fixup->offset, kind,
                                         (int32_t)value)) {
      code_generator_set_error(generator,
                               "Out of memory while relocating an asm block");
      x86_asm_result_destroy(&result);
      return 0;
    }
    if (!fixup->block_local &&
        !code_generator_find_ir_function_binary(generator, symbol) &&
        !code_generator_binary_declare_external_symbol(generator, symbol)) {
      x86_asm_result_destroy(&result);
      return 0;
    }
  }

  if (final_bits_out) {
    *final_bits_out = result.final_bits;
  }
  x86_asm_result_destroy(&result);
  return 1;
}

int code_generator_binary_emit_inline_asm(CodeGenerator *generator,
                                          BinaryFunctionContext *context,
                                          const IRInstruction *instruction) {
  if (!generator || !context || !instruction) {
    return 0;
  }
  if (!instruction->text || instruction->text[0] == '\0') {
    return 1;
  }
  return code_generator_binary_assemble_text(
      generator, context, instruction->text, mtlc_target()->code_bits, 0,
      &instruction->location, NULL);
}

int ir_function_has_inline_asm(const IRFunction *function) {
  size_t i;
  if (!function) {
    return 0;
  }
  for (i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_INLINE_ASM) {
      return 1;
    }
  }
  return 0;
}
