#include "codegen/binary/internal.h"
#include "codegen/asm/x86_asm.h"
#include "codegen/target.h"
#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char **names;
  int *offsets;
  int *sizes;
  size_t count;
  size_t capacity;
} X86NarrowSlots;

typedef struct {
  CodeGenerator *generator;
  IRFunction *function;
  BinaryFunctionContext *context;
  X86NarrowSlots slots;
  int bits;
  int word;
  const char *accumulator;
  const char *base;
  const char *data;
  const char *counter;
  const char *frame;
  const char *stack;
  const char *word_keyword;
  const char *sign_extend;
  int frame_size;
  char *text;
  size_t length;
  size_t capacity;
  int label_serial;
  int failed;
} X86NarrowEmitter;

static int x86_narrow_slots_add(X86NarrowSlots *slots, const char *name,
                                int offset, int size) {
  size_t i;
  for (i = 0; i < slots->count; i++) {
    if (strcmp(slots->names[i], name) == 0) {
      return 1;
    }
  }
  if (slots->count == slots->capacity) {
    size_t capacity = slots->capacity ? slots->capacity * 2 : 16;
    char **names = (char **)realloc(slots->names, capacity * sizeof(char *));
    int *offsets = NULL;
    int *sizes = NULL;
    if (!names) {
      return 0;
    }
    slots->names = names;
    offsets = (int *)realloc(slots->offsets, capacity * sizeof(int));
    if (!offsets) {
      return 0;
    }
    slots->offsets = offsets;
    sizes = (int *)realloc(slots->sizes, capacity * sizeof(int));
    if (!sizes) {
      return 0;
    }
    slots->sizes = sizes;
    slots->capacity = capacity;
  }
  slots->names[slots->count] = mettle_strdup(name);
  if (!slots->names[slots->count]) {
    return 0;
  }
  slots->offsets[slots->count] = offset;
  slots->sizes[slots->count] = size;
  slots->count++;
  return 1;
}

static int x86_narrow_slots_find(const X86NarrowSlots *slots, const char *name,
                                 int *offset_out) {
  size_t i;
  if (!name) {
    return 0;
  }
  for (i = 0; i < slots->count; i++) {
    if (strcmp(slots->names[i], name) == 0) {
      *offset_out = slots->offsets[i];
      return 1;
    }
  }
  return 0;
}

static int x86_narrow_slot_size(const X86NarrowSlots *slots, const char *name) {
  size_t i;
  if (!name) {
    return 0;
  }
  for (i = 0; i < slots->count; i++) {
    if (strcmp(slots->names[i], name) == 0) {
      return slots->sizes[i];
    }
  }
  return 0;
}

static void x86_narrow_slots_destroy(X86NarrowSlots *slots) {
  size_t i;
  for (i = 0; i < slots->count; i++) {
    free(slots->names[i]);
  }
  free(slots->names);
  free(slots->offsets);
  free(slots->sizes);
  memset(slots, 0, sizeof(*slots));
}

static void x86_narrow_fail(X86NarrowEmitter *emitter, const char *format, ...) {
  char message[512];
  va_list arguments;
  if (emitter->failed) {
    return;
  }
  emitter->failed = 1;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  emitter->generator->has_user_error = 1;
  code_generator_set_error(emitter->generator, "%s", message);
}

static int x86_narrow_emit(X86NarrowEmitter *emitter, const char *format, ...) {
  va_list arguments;
  int written;
  if (emitter->failed) {
    return 0;
  }
  for (;;) {
    size_t available = emitter->capacity - emitter->length;
    va_start(arguments, format);
    written = vsnprintf(emitter->text + emitter->length, available, format,
                        arguments);
    va_end(arguments);
    if (written < 0) {
      x86_narrow_fail(emitter, "could not format %d-bit code", emitter->bits);
      return 0;
    }
    if ((size_t)written < available) {
      emitter->length += (size_t)written;
      return 1;
    }
    {
      size_t capacity = emitter->capacity ? emitter->capacity * 2 : 4096;
      char *grown;
      while (capacity < emitter->length + (size_t)written + 2) {
        capacity *= 2;
      }
      grown = (char *)realloc(emitter->text, capacity);
      if (!grown) {
        x86_narrow_fail(emitter, "out of memory while emitting %d-bit code",
                        emitter->bits);
        return 0;
      }
      emitter->text = grown;
      emitter->capacity = capacity;
    }
  }
}

static int x86_narrow_symbol_is_global(X86NarrowEmitter *emitter,
                                       const char *name) {
  const CgSym *symbol =
      emitter->generator && emitter->generator->ir_program
          ? code_generator_lookup_symbol(emitter->generator, name)
          : NULL;
  return symbol && symbol->scope && symbol->scope->type == CG_SCOPE_GLOBAL;
}

static int x86_narrow_operand_text(X86NarrowEmitter *emitter,
                                   const IROperand *operand, char *buffer,
                                   size_t size) {
  int offset = 0;
  switch (operand->kind) {
  case IR_OPERAND_INT:
    snprintf(buffer, size, "%lld", operand->int_value);
    return 1;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    if (!operand->name) {
      return 0;
    }
    if (x86_narrow_slots_find(&emitter->slots, operand->name, &offset)) {
      if (x86_narrow_slot_size(&emitter->slots, operand->name) >
          emitter->word) {
        return 0;
      }
      snprintf(buffer, size, "%s ptr [%s %c %d]", emitter->word_keyword,
               emitter->frame, offset < 0 ? '-' : '+',
               offset < 0 ? -offset : offset);
      return 1;
    }
    if (x86_narrow_symbol_is_global(emitter, operand->name)) {
      const char *link_name =
          code_generator_get_link_symbol_name(emitter->generator, operand->name);
      snprintf(buffer, size, "%s ptr [%s]", emitter->word_keyword,
               link_name && link_name[0] ? link_name : operand->name);
      return 1;
    }
    return 0;
  case IR_OPERAND_NONE:
    snprintf(buffer, size, "0");
    return 1;
  default:
    return 0;
  }
}

static int x86_narrow_load_into(X86NarrowEmitter *emitter, const char *reg,
                                const IROperand *operand) {
  char text[160];
  if (!x86_narrow_operand_text(emitter, operand, text, sizeof(text))) {
    x86_narrow_fail(emitter,
                    "'%s' targets %d-bit code, which cannot read operand '%s'",
                    emitter->function->name, emitter->bits,
                    operand->name ? operand->name : "<value>");
    return 0;
  }
  return x86_narrow_emit(emitter, "mov %s, %s\n", reg, text);
}

static int x86_narrow_store_from(X86NarrowEmitter *emitter,
                                 const IROperand *dest, const char *reg) {
  char text[160];
  if (dest->kind == IR_OPERAND_NONE) {
    return 1;
  }
  if (!x86_narrow_operand_text(emitter, dest, text, sizeof(text))) {
    x86_narrow_fail(emitter,
                    "'%s' targets %d-bit code, which cannot write operand '%s'",
                    emitter->function->name, emitter->bits,
                    dest->name ? dest->name : "<value>");
    return 0;
  }
  return x86_narrow_emit(emitter, "mov %s, %s\n", text, reg);
}

static int x86_narrow_condition_suffix(const char *op, int is_unsigned,
                                       const char **out) {
  if (strcmp(op, "==") == 0) {
    *out = "e";
  } else if (strcmp(op, "!=") == 0) {
    *out = "ne";
  } else if (strcmp(op, "<") == 0) {
    *out = is_unsigned ? "b" : "l";
  } else if (strcmp(op, "<=") == 0) {
    *out = is_unsigned ? "be" : "le";
  } else if (strcmp(op, ">") == 0) {
    *out = is_unsigned ? "a" : "g";
  } else if (strcmp(op, ">=") == 0) {
    *out = is_unsigned ? "ae" : "ge";
  } else {
    return 0;
  }
  return 1;
}

static int x86_narrow_binary(X86NarrowEmitter *emitter,
                             const IRInstruction *instruction) {
  const char *op = instruction->text;
  const char *condition = NULL;
  int is_unsigned = instruction->is_unsigned;
  char text[160];

  if (!op) {
    x86_narrow_fail(emitter, "'%s' has a binary operation with no operator",
                    emitter->function->name);
    return 0;
  }
  if (instruction->is_float) {
    x86_narrow_fail(emitter,
                    "'%s' uses floating point, which %d-bit code generation "
                    "does not support",
                    emitter->function->name, emitter->bits);
    return 0;
  }

  if (!x86_narrow_load_into(emitter, emitter->accumulator, &instruction->lhs)) {
    return 0;
  }
  if (!x86_narrow_operand_text(emitter, &instruction->rhs, text, sizeof(text))) {
    x86_narrow_fail(emitter, "'%s' cannot use that operand in %d-bit code",
                    emitter->function->name, emitter->bits);
    return 0;
  }

  if (x86_narrow_condition_suffix(op, is_unsigned, &condition)) {
    int label = emitter->label_serial++;
    if (instruction->rhs.kind == IR_OPERAND_INT) {
      if (!x86_narrow_emit(emitter, "cmp %s, %s\n", emitter->accumulator,
                           text)) {
        return 0;
      }
    } else if (!x86_narrow_emit(emitter, "mov %s, %s\ncmp %s, %s\n",
                                emitter->base, text, emitter->accumulator,
                                emitter->base)) {
      return 0;
    }
    if (!x86_narrow_emit(emitter,
                         "j%s .cmp_true_%d\nmov %s, 0\njmp short .cmp_end_%d\n"
                         ".cmp_true_%d:\nmov %s, 1\n.cmp_end_%d:\n",
                         condition, label, emitter->accumulator, label, label,
                         emitter->accumulator, label)) {
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);
  }

  {
    static const struct {
      const char *source;
      const char *mnemonic;
    } SIMPLE[] = {{"+", "add"}, {"-", "sub"}, {"&", "and"},
                  {"|", "or"},  {"^", "xor"}};
    size_t i;
    for (i = 0; i < sizeof(SIMPLE) / sizeof(SIMPLE[0]); i++) {
      if (strcmp(op, SIMPLE[i].source) != 0) {
        continue;
      }
      if (instruction->rhs.kind == IR_OPERAND_INT) {
        if (!x86_narrow_emit(emitter, "%s %s, %s\n", SIMPLE[i].mnemonic,
                             emitter->accumulator, text)) {
          return 0;
        }
      } else if (!x86_narrow_emit(emitter, "mov %s, %s\n%s %s, %s\n",
                                  emitter->base, text, SIMPLE[i].mnemonic,
                                  emitter->accumulator, emitter->base)) {
        return 0;
      }
      return x86_narrow_store_from(emitter, &instruction->dest,
                                   emitter->accumulator);
    }
  }

  if (strcmp(op, "*") == 0) {
    if (!x86_narrow_emit(emitter, "mov %s, %s\n%s %s\n", emitter->base, text,
                         is_unsigned ? "mul" : "imul", emitter->base)) {
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);
  }

  if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
    if (!x86_narrow_emit(emitter, "mov %s, %s\n", emitter->base, text)) {
      return 0;
    }
    if (is_unsigned) {
      if (!x86_narrow_emit(emitter, "xor %s, %s\ndiv %s\n", emitter->data,
                           emitter->data, emitter->base)) {
        return 0;
      }
    } else if (!x86_narrow_emit(emitter, "%s\nidiv %s\n", emitter->sign_extend,
                                emitter->base)) {
      return 0;
    }
    return x86_narrow_store_from(
        emitter, &instruction->dest,
        strcmp(op, "/") == 0 ? emitter->accumulator : emitter->data);
  }

  if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
    const char *mnemonic =
        strcmp(op, "<<") == 0 ? "shl" : (is_unsigned ? "shr" : "sar");
    if (instruction->rhs.kind == IR_OPERAND_INT) {
      if (!x86_narrow_emit(emitter, "mov cl, %lld\n%s %s, cl\n",
                           instruction->rhs.int_value, mnemonic,
                           emitter->accumulator)) {
        return 0;
      }
    } else if (!x86_narrow_emit(emitter, "mov %s, %s\nmov cl, bl\n%s %s, cl\n",
                                emitter->base, text, mnemonic,
                                emitter->accumulator)) {
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);
  }

  if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
    int label = emitter->label_serial++;
    if (!x86_narrow_emit(emitter, "mov %s, %s\n", emitter->base, text)) {
      return 0;
    }
    if (!x86_narrow_emit(emitter,
                         "test %s, %s\nj%s .bool_short_%d\ntest %s, %s\n"
                         ".bool_short_%d:\njnz .bool_true_%d\nmov %s, 0\n"
                         "jmp short .bool_end_%d\n.bool_true_%d:\nmov %s, 1\n"
                         ".bool_end_%d:\n",
                         emitter->accumulator, emitter->accumulator,
                         strcmp(op, "&&") == 0 ? "z" : "nz", label,
                         emitter->base, emitter->base, label, label,
                         emitter->accumulator, label, label,
                         emitter->accumulator, label)) {
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);
  }

  x86_narrow_fail(emitter,
                  "'%s' uses the operator `%s`, which %d-bit code generation "
                  "does not support",
                  emitter->function->name, op, emitter->bits);
  return 0;
}

static int x86_narrow_inline_asm(X86NarrowEmitter *emitter, const char *text) {
  size_t i = 0;
  while (text[i]) {
    if (text[i] == '{') {
      char name[192];
      size_t length = 0;
      IROperand operand;
      char rendered[160];
      i++;
      while (text[i] && text[i] != '}' && length + 1 < sizeof(name)) {
        name[length++] = text[i++];
      }
      name[length] = '\0';
      while (length > 0 &&
             (name[length - 1] == ' ' || name[length - 1] == '\t')) {
        name[--length] = '\0';
      }
      if (text[i] == '}') {
        i++;
      }
      memset(&operand, 0, sizeof(operand));
      operand.kind = IR_OPERAND_SYMBOL;
      operand.name = name;
      if (!x86_narrow_operand_text(emitter, &operand, rendered,
                                   sizeof(rendered))) {
        x86_narrow_fail(emitter,
                        "`{%s}` in the asm block of '%s' names no local, "
                        "parameter or global in scope",
                        name, emitter->function->name);
        return 0;
      }
      if (!x86_narrow_emit(emitter, "%s", rendered)) {
        return 0;
      }
      continue;
    }
    if (!x86_narrow_emit(emitter, "%c", text[i])) {
      return 0;
    }
    i++;
  }
  return x86_narrow_emit(emitter, "\n");
}

static int x86_narrow_type_fits(const X86NarrowEmitter *emitter,
                                const MtlcType *type) {
  if (!type) {
    return 1;
  }
  if (type->kind == MTLC_TYPE_POINTER ||
      type->kind == MTLC_TYPE_FUNCTION_POINTER) {
    return 1;
  }
  if (type->kind == MTLC_TYPE_VOID) {
    return 1;
  }
  return type->size <= (size_t)emitter->word;
}

static int x86_narrow_type_is_addressed_region(const MtlcType *type) {
  return type && (type->kind == MTLC_TYPE_ARRAY || type->kind == MTLC_TYPE_STRUCT);
}

static const char *const X86_NARROW_ISR_SAVED_16[] = {
    "ax", "bx", "cx", "dx", "si", "di", "ds", "es"};

static const char *const X86_NARROW_ISR_SAVED_32[] = {
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ds", "es"};

#define X86_NARROW_ISR_SAVED_COUNT 8

static const char *x86_narrow_isr_saved(const X86NarrowEmitter *emitter,
                                        int index) {
  return emitter->bits == 32 ? X86_NARROW_ISR_SAVED_32[index]
                             : X86_NARROW_ISR_SAVED_16[index];
}

static int x86_narrow_isr_frame_offset(const X86NarrowEmitter *emitter) {
  int offset = (X86_NARROW_ISR_SAVED_COUNT + 1) * emitter->word;
  if (emitter->function->parameter_count >= 2) {
    offset += emitter->word;
  }
  return offset;
}

static int x86_narrow_interrupt_entry(X86NarrowEmitter *emitter) {
  int i;
  if (!x86_narrow_emit(emitter, "cld\n")) {
    return 0;
  }
  for (i = 0; i < X86_NARROW_ISR_SAVED_COUNT; i++) {
    if (!x86_narrow_emit(emitter, "push %s\n",
                         x86_narrow_isr_saved(emitter, i))) {
      return 0;
    }
  }
  return 1;
}

static int x86_narrow_interrupt_parameters(X86NarrowEmitter *emitter) {
  size_t count = emitter->function->parameter_count;
  int slot = 0;
  if (count == 0) {
    return 1;
  }
  if (!x86_narrow_slots_find(&emitter->slots,
                             emitter->function->parameter_names[0], &slot)) {
    return 1;
  }
  if (!x86_narrow_emit(emitter, "lea %s, [%s + %d]\nmov [%s - %d], %s\n",
                       emitter->accumulator, emitter->frame,
                       x86_narrow_isr_frame_offset(emitter), emitter->frame,
                       -slot, emitter->accumulator)) {
    return 0;
  }
  if (count < 2 ||
      !x86_narrow_slots_find(&emitter->slots,
                             emitter->function->parameter_names[1], &slot)) {
    return 1;
  }
  return x86_narrow_emit(
      emitter, "mov %s, [%s + %d]\nmov [%s - %d], %s\n", emitter->accumulator,
      emitter->frame, (X86_NARROW_ISR_SAVED_COUNT + 1) * emitter->word,
      emitter->frame, -slot, emitter->accumulator);
}

static int x86_narrow_epilogue(X86NarrowEmitter *emitter) {
  int i;
  if (!x86_narrow_emit(emitter, "mov %s, %s\npop %s\n", emitter->stack,
                       emitter->frame, emitter->frame)) {
    return 0;
  }
  if (!emitter->function->is_interrupt) {
    return x86_narrow_emit(emitter, "ret\n");
  }
  for (i = X86_NARROW_ISR_SAVED_COUNT; i > 0; i--) {
    if (!x86_narrow_emit(emitter, "pop %s\n",
                         x86_narrow_isr_saved(emitter, i - 1))) {
      return 0;
    }
  }
  if (emitter->function->parameter_count >= 2 &&
      !x86_narrow_emit(emitter, "add %s, %d\n", emitter->stack,
                       emitter->word)) {
    return 0;
  }
  return x86_narrow_emit(emitter, "%s\n",
                         emitter->bits == 32 ? "iretd" : "iret");
}

static int x86_narrow_fill(X86NarrowEmitter *emitter, long long width) {
  long long words = width / emitter->word;
  long long bytes = width % emitter->word;
  long long i;

  if (words > 16) {
    int label = emitter->label_serial++;
    if (!x86_narrow_emit(emitter,
                         "mov %s, %lld\n.fill_%d:\nmov [%s], %s\n"
                         "add %s, %d\ndec %s\njnz .fill_%d\n",
                         emitter->counter, words, label, emitter->base,
                         emitter->accumulator, emitter->base, emitter->word,
                         emitter->counter, label)) {
      return 0;
    }
    words = 0;
    bytes = width % emitter->word;
  }
  for (i = 0; i < words; i++) {
    if (!x86_narrow_emit(emitter, "mov [%s + %lld], %s\n", emitter->base,
                         i * emitter->word, emitter->accumulator)) {
      return 0;
    }
  }
  for (i = 0; i < bytes; i++) {
    if (!x86_narrow_emit(emitter, "mov [%s + %lld], al\n", emitter->base,
                         words * emitter->word + i)) {
      return 0;
    }
  }
  return 1;
}

static int x86_narrow_instruction(X86NarrowEmitter *emitter,
                                  const IRInstruction *instruction) {
  switch (instruction->op) {
  case IR_OP_NOP:
    return 1;

  case IR_OP_DECLARE_LOCAL:
    if (!x86_narrow_type_is_addressed_region(instruction->value_type) &&
        !x86_narrow_type_fits(emitter, instruction->value_type)) {
      x86_narrow_fail(emitter,
                      "'%s' declares '%s' as `%s`, which does not fit in the "
                      "%d bits this target computes in",
                      emitter->function->name,
                      instruction->dest.name ? instruction->dest.name
                                             : "<local>",
                      instruction->text ? instruction->text : "that type",
                      emitter->bits);
      return 0;
    }
    return 1;

  case IR_OP_LABEL:
    if (!instruction->text) {
      return 1;
    }
    return x86_narrow_emit(emitter, "%s:\n", instruction->text);

  case IR_OP_JUMP:
    return x86_narrow_emit(emitter, "jmp %s\n", instruction->text);

  case IR_OP_BRANCH_ZERO:
    if (!x86_narrow_load_into(emitter, emitter->accumulator,
                              &instruction->lhs)) {
      return 0;
    }
    return x86_narrow_emit(emitter, "test %s, %s\njz %s\n",
                           emitter->accumulator, emitter->accumulator,
                           instruction->text);

  case IR_OP_BRANCH_EQ: {
    char text[160];
    if (!x86_narrow_load_into(emitter, emitter->accumulator,
                              &instruction->lhs)) {
      return 0;
    }
    if (!x86_narrow_operand_text(emitter, &instruction->rhs, text,
                                 sizeof(text))) {
      x86_narrow_fail(emitter, "'%s' cannot compare that operand in %d-bit code",
                      emitter->function->name, emitter->bits);
      return 0;
    }
    if (instruction->rhs.kind == IR_OPERAND_INT) {
      return x86_narrow_emit(emitter, "cmp %s, %s\nje %s\n",
                             emitter->accumulator, text, instruction->text);
    }
    return x86_narrow_emit(emitter, "mov %s, %s\ncmp %s, %s\nje %s\n",
                           emitter->base, text, emitter->accumulator,
                           emitter->base, instruction->text);
  }

  case IR_OP_ASSIGN:
  case IR_OP_CAST:
    if (!x86_narrow_load_into(emitter, emitter->accumulator,
                              &instruction->lhs)) {
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);

  case IR_OP_BINARY:
    return x86_narrow_binary(emitter, instruction);

  case IR_OP_UNARY: {
    const char *op = instruction->text ? instruction->text : "";
    if (!x86_narrow_load_into(emitter, emitter->accumulator,
                              &instruction->lhs)) {
      return 0;
    }
    if (strcmp(op, "-") == 0) {
      if (!x86_narrow_emit(emitter, "neg %s\n", emitter->accumulator)) {
        return 0;
      }
    } else if (strcmp(op, "~") == 0) {
      if (!x86_narrow_emit(emitter, "not %s\n", emitter->accumulator)) {
        return 0;
      }
    } else if (strcmp(op, "!") == 0) {
      int label = emitter->label_serial++;
      if (!x86_narrow_emit(emitter,
                           "test %s, %s\njz .not_true_%d\nmov %s, 0\n"
                           "jmp short .not_end_%d\n.not_true_%d:\nmov %s, 1\n"
                           ".not_end_%d:\n",
                           emitter->accumulator, emitter->accumulator, label,
                           emitter->accumulator, label, label,
                           emitter->accumulator, label)) {
        return 0;
      }
    } else if (strcmp(op, "+") != 0) {
      x86_narrow_fail(emitter,
                      "'%s' uses the unary operator `%s`, which %d-bit code "
                      "generation does not support",
                      emitter->function->name, op, emitter->bits);
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);
  }

  case IR_OP_ADDRESS_OF: {
    int offset = 0;
    if (instruction->lhs.name &&
        x86_narrow_slots_find(&emitter->slots, instruction->lhs.name,
                              &offset)) {
      if (!x86_narrow_emit(emitter, "lea %s, [%s %c %d]\n",
                           emitter->accumulator, emitter->frame,
                           offset < 0 ? '-' : '+',
                           offset < 0 ? -offset : offset)) {
        return 0;
      }
    } else if (instruction->lhs.name &&
               x86_narrow_symbol_is_global(emitter, instruction->lhs.name)) {
      const char *link_name = code_generator_get_link_symbol_name(
          emitter->generator, instruction->lhs.name);
      if (!x86_narrow_emit(emitter, "mov %s, %s\n", emitter->accumulator,
                           link_name && link_name[0] ? link_name
                                                     : instruction->lhs.name)) {
        return 0;
      }
    } else {
      x86_narrow_fail(emitter,
                      "'%s' takes the address of '%s', which %d-bit code "
                      "generation cannot place",
                      emitter->function->name,
                      instruction->lhs.name ? instruction->lhs.name : "<value>",
                      emitter->bits);
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);
  }

  case IR_OP_LOAD: {
    long long width = instruction->rhs.kind == IR_OPERAND_INT
                          ? instruction->rhs.int_value
                          : emitter->word;
    if (!x86_narrow_load_into(emitter, emitter->base, &instruction->lhs)) {
      return 0;
    }
    if (width == 1 || (width == 2 && emitter->word == 4)) {
      if (!x86_narrow_emit(emitter, "%s %s, %s ptr [%s]\n",
                           instruction->is_unsigned ? "movzx" : "movsx",
                           emitter->accumulator, width == 1 ? "byte" : "word",
                           emitter->base)) {
        return 0;
      }
    } else if (width == emitter->word) {
      if (!x86_narrow_emit(emitter, "mov %s, [%s]\n", emitter->accumulator,
                           emitter->base)) {
        return 0;
      }
    } else {
      x86_narrow_fail(emitter,
                      "'%s' loads %lld bytes, which %d-bit code generation "
                      "cannot hold in a register",
                      emitter->function->name, width, emitter->bits);
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);
  }

  case IR_OP_STORE: {
    long long width = instruction->rhs.kind == IR_OPERAND_INT
                          ? instruction->rhs.int_value
                          : emitter->word;
    if (!x86_narrow_load_into(emitter, emitter->accumulator,
                              &instruction->lhs)) {
      return 0;
    }
    if (!x86_narrow_load_into(emitter, emitter->base, &instruction->dest)) {
      return 0;
    }
    if (width == 1) {
      return x86_narrow_emit(emitter, "mov [%s], al\n", emitter->base);
    }
    if (width == 2) {
      return x86_narrow_emit(emitter, "mov [%s], ax\n", emitter->base);
    }
    if (width == 4 && emitter->word == 4) {
      return x86_narrow_emit(emitter, "mov [%s], eax\n", emitter->base);
    }
    if (width > emitter->word) {
      return x86_narrow_fill(emitter, width);
    }
    x86_narrow_fail(emitter,
                    "'%s' stores %lld bytes, which %d-bit code generation "
                    "cannot hold in a register",
                    emitter->function->name, width, emitter->bits);
    return 0;
  }

  case IR_OP_CALL: {
    size_t i;
    if (!instruction->text) {
      x86_narrow_fail(emitter, "'%s' has a call with no callee",
                      emitter->function->name);
      return 0;
    }
    for (i = instruction->argument_count; i > 0; i--) {
      char text[160];
      const IROperand *argument = &instruction->arguments[i - 1];
      if (!x86_narrow_operand_text(emitter, argument, text, sizeof(text))) {
        x86_narrow_fail(emitter,
                        "'%s' passes an argument %d-bit code generation cannot "
                        "place",
                        emitter->function->name, emitter->bits);
        return 0;
      }
      if (argument->kind == IR_OPERAND_INT) {
        if (!x86_narrow_emit(emitter, "mov %s, %s\npush %s\n",
                             emitter->accumulator, text,
                             emitter->accumulator)) {
          return 0;
        }
      } else if (!x86_narrow_emit(emitter, "push %s\n", text)) {
        return 0;
      }
    }
    if (!x86_narrow_emit(emitter, "call %s\n", instruction->text)) {
      return 0;
    }
    if (instruction->argument_count &&
        !x86_narrow_emit(emitter, "add %s, %zu\n", emitter->stack,
                         instruction->argument_count * (size_t)emitter->word)) {
      return 0;
    }
    return x86_narrow_store_from(emitter, &instruction->dest,
                                 emitter->accumulator);
  }

  case IR_OP_RETURN:
    if (instruction->lhs.kind != IR_OPERAND_NONE &&
        !x86_narrow_load_into(emitter, emitter->accumulator,
                              &instruction->lhs)) {
      return 0;
    }
    return x86_narrow_epilogue(emitter);

  case IR_OP_INLINE_ASM:
    if (!instruction->text) {
      return 1;
    }
    return x86_narrow_inline_asm(emitter, instruction->text);

  default:
    x86_narrow_fail(emitter,
                    "'%s' uses `%s`, which %d-bit code generation does not "
                    "support",
                    emitter->function->name, ir_opcode_name(instruction->op),
                    emitter->bits);
    return 0;
  }
}

static int x86_narrow_slot_bytes(const X86NarrowEmitter *emitter,
                                 const MtlcType *type) {
  int size = emitter->word;
  if (type && type->kind != MTLC_TYPE_POINTER &&
      type->kind != MTLC_TYPE_FUNCTION_POINTER &&
      type->size > (size_t)emitter->word) {
    size = (int)type->size;
  }
  while (size % emitter->word) {
    size++;
  }
  return size;
}

static int x86_narrow_plan_frame(X86NarrowEmitter *emitter) {
  IRFunction *function = emitter->function;
  size_t i;
  int used = 0;
  int next_local = 0;
  int next_parameter = 2 * emitter->word;

  for (i = 0; i < function->parameter_count; i++) {
    if (!function->parameter_names || !function->parameter_names[i]) {
      continue;
    }
    if (function->is_interrupt) {
      used += emitter->word;
      if (!x86_narrow_slots_add(&emitter->slots, function->parameter_names[i],
                                -used, emitter->word)) {
        return 0;
      }
      continue;
    }
    if (!x86_narrow_slots_add(&emitter->slots, function->parameter_names[i],
                              next_parameter, emitter->word)) {
      return 0;
    }
    next_parameter += emitter->word;
  }

  for (i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    int size;
    if (instruction->op != IR_OP_DECLARE_LOCAL || !instruction->dest.name) {
      continue;
    }
    if (x86_narrow_slots_find(&emitter->slots, instruction->dest.name,
                              &next_local) ||
        x86_narrow_symbol_is_global(emitter, instruction->dest.name)) {
      continue;
    }
    size = x86_narrow_slot_bytes(emitter, instruction->value_type);
    used += size;
    if (!x86_narrow_slots_add(&emitter->slots, instruction->dest.name, -used,
                              size)) {
      return 0;
    }
  }

  next_local = 0;
  for (i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    const IROperand *operands[3];
    size_t operand_index;
    size_t argument;
    operands[0] = &instruction->dest;
    operands[1] = &instruction->lhs;
    operands[2] = &instruction->rhs;
    for (operand_index = 0; operand_index < 3; operand_index++) {
      const IROperand *operand = operands[operand_index];
      int existing = 0;
      if ((operand->kind != IR_OPERAND_TEMP &&
           operand->kind != IR_OPERAND_SYMBOL) ||
          !operand->name) {
        continue;
      }
      if (x86_narrow_slots_find(&emitter->slots, operand->name, &existing) ||
          x86_narrow_symbol_is_global(emitter, operand->name)) {
        continue;
      }
      used += emitter->word;
      if (!x86_narrow_slots_add(&emitter->slots, operand->name, -used,
                                emitter->word)) {
        return 0;
      }
    }
    for (argument = 0; argument < instruction->argument_count; argument++) {
      const IROperand *operand = &instruction->arguments[argument];
      int existing = 0;
      if ((operand->kind != IR_OPERAND_TEMP &&
           operand->kind != IR_OPERAND_SYMBOL) ||
          !operand->name) {
        continue;
      }
      if (x86_narrow_slots_find(&emitter->slots, operand->name, &existing) ||
          x86_narrow_symbol_is_global(emitter, operand->name)) {
        continue;
      }
      used += emitter->word;
      if (!x86_narrow_slots_add(&emitter->slots, operand->name, -used,
                                emitter->word)) {
        return 0;
      }
    }
  }

  (void)next_local;
  emitter->frame_size = used;
  while (emitter->frame_size % emitter->word) {
    emitter->frame_size++;
  }
  return 1;
}

int code_generator_emit_binary_function_x86_16(
    CodeGenerator *generator, IRFunction *ir_function,
    BinaryFunctionContext *context) {
  X86NarrowEmitter emitter;
  size_t i;
  int result = 0;

  if (!generator || !ir_function || !context) {
    return 0;
  }

  memset(&emitter, 0, sizeof(emitter));
  emitter.generator = generator;
  emitter.function = ir_function;
  emitter.context = context;
  emitter.bits = mtlc_target()->code_bits == 32 ? 32 : 16;
  emitter.word = emitter.bits / 8;
  emitter.accumulator = emitter.bits == 32 ? "eax" : "ax";
  emitter.base = emitter.bits == 32 ? "ebx" : "bx";
  emitter.data = emitter.bits == 32 ? "edx" : "dx";
  emitter.counter = emitter.bits == 32 ? "ecx" : "cx";
  emitter.frame = emitter.bits == 32 ? "ebp" : "bp";
  emitter.stack = emitter.bits == 32 ? "esp" : "sp";
  emitter.word_keyword = emitter.bits == 32 ? "dword" : "word";
  emitter.sign_extend = emitter.bits == 32 ? "cdq" : "cwd";

  {
    const MtlcType *return_type = code_generator_binary_get_resolved_type(
        generator, ir_function->return_type_name, 1);
    if (!x86_narrow_type_fits(&emitter, return_type)) {
      x86_narrow_fail(&emitter,
                      "'%s' returns `%s`, which does not fit in the %d bits "
                      "this target computes in",
                      ir_function->name,
                      ir_function->return_type_name
                          ? ir_function->return_type_name
                          : "that type",
                      emitter.bits);
      goto cleanup;
    }
    for (i = 0; i < ir_function->parameter_count; i++) {
      const char *name = ir_function->parameter_types
                             ? ir_function->parameter_types[i]
                             : NULL;
      const MtlcType *parameter_type =
          code_generator_binary_get_resolved_type(generator, name, 0);
      if (!x86_narrow_type_fits(&emitter, parameter_type)) {
        x86_narrow_fail(&emitter,
                        "'%s' takes a `%s`, which does not fit in the %d bits "
                        "this target computes in",
                        ir_function->name, name ? name : "that type",
                        emitter.bits);
        goto cleanup;
      }
    }
  }

  if (!x86_narrow_plan_frame(&emitter)) {
    code_generator_set_error(generator,
                             "Out of memory while planning the %d-bit frame of "
                             "'%s'",
                             emitter.bits, ir_function->name);
    goto cleanup;
  }

  if (ir_function->is_interrupt &&
      !code_generator_binary_check_interrupt_signature(generator,
                                                       ir_function)) {
    goto cleanup;
  }
  if (ir_function->is_interrupt && !x86_narrow_interrupt_entry(&emitter)) {
    goto cleanup;
  }
  if (!x86_narrow_emit(&emitter, "push %s\nmov %s, %s\n", emitter.frame,
                       emitter.frame, emitter.stack)) {
    goto cleanup;
  }
  if (emitter.frame_size > 0 &&
      !x86_narrow_emit(&emitter, "sub %s, %d\n", emitter.stack,
                       emitter.frame_size)) {
    goto cleanup;
  }
  if (ir_function->is_interrupt &&
      !x86_narrow_interrupt_parameters(&emitter)) {
    goto cleanup;
  }

  for (i = 0; i < ir_function->instruction_count; i++) {
    if (!x86_narrow_instruction(&emitter, &ir_function->instructions[i])) {
      goto cleanup;
    }
  }

  if (!x86_narrow_epilogue(&emitter)) {
    goto cleanup;
  }

  result = code_generator_binary_assemble_text(generator, context, emitter.text,
                                               emitter.bits, 0,
                                               &ir_function->location, NULL);

cleanup:
  x86_narrow_slots_destroy(&emitter.slots);
  free(emitter.text);
  return result;
}
