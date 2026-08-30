#include "codegen/binary/internal.h"
#include "codegen/asm/x86_asm.h"
#include "codegen/target.h"
#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X86_16_WORD 2

typedef struct {
  char **names;
  int *offsets;
  size_t count;
  size_t capacity;
} X86_16Slots;

typedef struct {
  CodeGenerator *generator;
  IRFunction *function;
  BinaryFunctionContext *context;
  X86_16Slots slots;
  int frame_size;
  char *text;
  size_t length;
  size_t capacity;
  int compare_label;
  int failed;
} X86_16Emitter;

static int x86_16_slots_add(X86_16Slots *slots, const char *name, int offset) {
  size_t i;
  for (i = 0; i < slots->count; i++) {
    if (strcmp(slots->names[i], name) == 0) {
      return 1;
    }
  }
  if (slots->count == slots->capacity) {
    size_t capacity = slots->capacity ? slots->capacity * 2 : 16;
    char **names = (char **)realloc(slots->names, capacity * sizeof(char *));
    int *offsets = (int *)realloc(slots->offsets, capacity * sizeof(int));
    if (!names || !offsets) {
      free(names ? names : slots->names);
      free(offsets ? offsets : slots->offsets);
      slots->names = NULL;
      slots->offsets = NULL;
      return 0;
    }
    slots->names = names;
    slots->offsets = offsets;
    slots->capacity = capacity;
  }
  slots->names[slots->count] = mettle_strdup(name);
  if (!slots->names[slots->count]) {
    return 0;
  }
  slots->offsets[slots->count] = offset;
  slots->count++;
  return 1;
}

static int x86_16_slots_find(const X86_16Slots *slots, const char *name,
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

static void x86_16_slots_destroy(X86_16Slots *slots) {
  size_t i;
  for (i = 0; i < slots->count; i++) {
    free(slots->names[i]);
  }
  free(slots->names);
  free(slots->offsets);
  memset(slots, 0, sizeof(*slots));
}

static void x86_16_fail(X86_16Emitter *emitter, const char *format, ...) {
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

static int x86_16_emit(X86_16Emitter *emitter, const char *format, ...) {
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
      x86_16_fail(emitter, "could not format 16-bit code");
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
        x86_16_fail(emitter, "out of memory while emitting 16-bit code");
        return 0;
      }
      emitter->text = grown;
      emitter->capacity = capacity;
    }
  }
}

static int x86_16_symbol_is_global(X86_16Emitter *emitter, const char *name) {
  const CgSym *symbol =
      emitter->generator && emitter->generator->ir_program
          ? code_generator_lookup_symbol(emitter->generator, name)
          : NULL;
  return symbol && symbol->scope && symbol->scope->type == CG_SCOPE_GLOBAL;
}

static int x86_16_operand_text(X86_16Emitter *emitter, const IROperand *operand,
                               char *buffer, size_t size) {
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
    if (x86_16_slots_find(&emitter->slots, operand->name, &offset)) {
      if (offset < 0) {
        snprintf(buffer, size, "word ptr [bp - %d]", -offset);
      } else {
        snprintf(buffer, size, "word ptr [bp + %d]", offset);
      }
      return 1;
    }
    if (x86_16_symbol_is_global(emitter, operand->name)) {
      const char *link_name =
          code_generator_get_link_symbol_name(emitter->generator,
                                              operand->name);
      snprintf(buffer, size, "word ptr [%s]",
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

static int x86_16_load_into(X86_16Emitter *emitter, const char *reg,
                            const IROperand *operand) {
  char text[128];
  if (!x86_16_operand_text(emitter, operand, text, sizeof(text))) {
    x86_16_fail(emitter,
                "'%s' targets 16-bit code, which cannot read operand '%s'",
                emitter->function->name,
                operand->name ? operand->name : "<value>");
    return 0;
  }
  return x86_16_emit(emitter, "mov %s, %s\n", reg, text);
}

static int x86_16_store_from(X86_16Emitter *emitter, const IROperand *dest,
                             const char *reg) {
  char text[128];
  if (dest->kind == IR_OPERAND_NONE) {
    return 1;
  }
  if (!x86_16_operand_text(emitter, dest, text, sizeof(text))) {
    x86_16_fail(emitter,
                "'%s' targets 16-bit code, which cannot write operand '%s'",
                emitter->function->name,
                dest->name ? dest->name : "<value>");
    return 0;
  }
  return x86_16_emit(emitter, "mov %s, %s\n", text, reg);
}

static int x86_16_condition_suffix(const char *op, int is_unsigned,
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

static int x86_16_binary(X86_16Emitter *emitter,
                         const IRInstruction *instruction) {
  const char *op = instruction->text;
  const char *condition = NULL;
  int is_unsigned = instruction->is_unsigned;

  if (!op) {
    x86_16_fail(emitter, "'%s' has a binary operation with no operator",
                emitter->function->name);
    return 0;
  }
  if (instruction->is_float) {
    x86_16_fail(emitter,
                "'%s' uses floating point, which 16-bit real-mode code "
                "generation does not support",
                emitter->function->name);
    return 0;
  }

  if (!x86_16_load_into(emitter, "ax", &instruction->lhs)) {
    return 0;
  }

  if (x86_16_condition_suffix(op, is_unsigned, &condition)) {
    int label = emitter->compare_label++;
    char text[128];
    if (!x86_16_operand_text(emitter, &instruction->rhs, text, sizeof(text))) {
      x86_16_fail(emitter, "'%s' cannot compare against that operand in "
                           "16-bit code",
                  emitter->function->name);
      return 0;
    }
    if (instruction->rhs.kind == IR_OPERAND_INT) {
      if (!x86_16_emit(emitter, "cmp ax, %s\n", text)) {
        return 0;
      }
    } else {
      if (!x86_16_emit(emitter, "mov bx, %s\ncmp ax, bx\n", text)) {
        return 0;
      }
    }
    if (!x86_16_emit(emitter,
                     "j%s .cmp_true_%d\nmov ax, 0\njmp short .cmp_end_%d\n"
                     ".cmp_true_%d:\nmov ax, 1\n.cmp_end_%d:\n",
                     condition, label, label, label, label)) {
      return 0;
    }
    return x86_16_store_from(emitter, &instruction->dest, "ax");
  }

  {
    char text[128];
    if (!x86_16_operand_text(emitter, &instruction->rhs, text, sizeof(text))) {
      x86_16_fail(emitter, "'%s' cannot use that operand in 16-bit code",
                  emitter->function->name);
      return 0;
    }
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "&") == 0 ||
        strcmp(op, "|") == 0 || strcmp(op, "^") == 0) {
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
          if (!x86_16_emit(emitter, "%s ax, %s\n", SIMPLE[i].mnemonic, text)) {
            return 0;
          }
        } else if (!x86_16_emit(emitter, "mov bx, %s\n%s ax, bx\n", text,
                                SIMPLE[i].mnemonic)) {
          return 0;
        }
        return x86_16_store_from(emitter, &instruction->dest, "ax");
      }
    }
    if (strcmp(op, "*") == 0) {
      if (!x86_16_emit(emitter, "mov bx, %s\n%s bx\n", text,
                       is_unsigned ? "mul" : "imul")) {
        return 0;
      }
      return x86_16_store_from(emitter, &instruction->dest, "ax");
    }
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
      if (!x86_16_emit(emitter, "mov bx, %s\n", text)) {
        return 0;
      }
      if (is_unsigned) {
        if (!x86_16_emit(emitter, "xor dx, dx\ndiv bx\n")) {
          return 0;
        }
      } else if (!x86_16_emit(emitter, "cwd\nidiv bx\n")) {
        return 0;
      }
      return x86_16_store_from(emitter, &instruction->dest,
                               strcmp(op, "/") == 0 ? "ax" : "dx");
    }
    if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
      const char *mnemonic =
          strcmp(op, "<<") == 0 ? "shl" : (is_unsigned ? "shr" : "sar");
      if (instruction->rhs.kind == IR_OPERAND_INT) {
        if (!x86_16_emit(emitter, "mov cl, %lld\n%s ax, cl\n",
                         instruction->rhs.int_value, mnemonic)) {
          return 0;
        }
      } else if (!x86_16_emit(emitter, "mov bx, %s\nmov cl, bl\n%s ax, cl\n",
                              text, mnemonic)) {
        return 0;
      }
      return x86_16_store_from(emitter, &instruction->dest, "ax");
    }
    if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
      int label = emitter->compare_label++;
      if (!x86_16_emit(emitter, "mov bx, %s\n", text)) {
        return 0;
      }
      if (!x86_16_emit(emitter,
                       "test ax, ax\nj%s .bool_short_%d\ntest bx, bx\n"
                       ".bool_short_%d:\nj%s .bool_true_%d\nmov ax, 0\n"
                       "jmp short .bool_end_%d\n.bool_true_%d:\nmov ax, 1\n"
                       ".bool_end_%d:\n",
                       strcmp(op, "&&") == 0 ? "z" : "nz", label, label,
                       "nz", label, label, label, label)) {
        return 0;
      }
      return x86_16_store_from(emitter, &instruction->dest, "ax");
    }
  }

  x86_16_fail(emitter,
              "'%s' uses the operator `%s`, which 16-bit real-mode code "
              "generation does not support",
              emitter->function->name, op);
  return 0;
}

/* `{name}` inside an asm block names a Mettle local, parameter or global. The
 * 64-bit path resolves one through the emitter's frame tables; here the frame
 * is this file's own, so the binding is spelled out before the assembler ever
 * sees it. */
static int x86_16_inline_asm(X86_16Emitter *emitter, const char *text) {
  size_t i = 0;
  while (text[i]) {
    if (text[i] == '{') {
      char name[192];
      size_t length = 0;
      IROperand operand;
      char rendered[128];
      i++;
      while (text[i] && text[i] != '}' && length + 1 < sizeof(name)) {
        name[length++] = text[i++];
      }
      name[length] = 0;
      while (length > 0 && (name[length - 1] == ' ' || name[length - 1] == 9)) {
        name[--length] = 0;
      }
      if (text[i] == '}') {
        i++;
      }
      memset(&operand, 0, sizeof(operand));
      operand.kind = IR_OPERAND_SYMBOL;
      operand.name = name;
      if (!x86_16_operand_text(emitter, &operand, rendered, sizeof(rendered))) {
        x86_16_fail(emitter,
                    "`{%s}` in the asm block of '%s' names no local, parameter "
                    "or global in scope",
                    name, emitter->function->name);
        return 0;
      }
      if (!x86_16_emit(emitter, "%s", rendered)) {
        return 0;
      }
      continue;
    }
    if (!x86_16_emit(emitter, "%c", text[i])) {
      return 0;
    }
    i++;
  }
  return x86_16_emit(emitter, "\n");
}

static int x86_16_instruction(X86_16Emitter *emitter,
                              const IRInstruction *instruction) {
  switch (instruction->op) {
  case IR_OP_NOP:
  case IR_OP_DECLARE_LOCAL:
    return 1;

  case IR_OP_LABEL:
    if (!instruction->text) {
      return 1;
    }
    return x86_16_emit(emitter, "%s:\n", instruction->text);

  case IR_OP_JUMP:
    return x86_16_emit(emitter, "jmp %s\n", instruction->text);

  case IR_OP_BRANCH_ZERO:
    if (!x86_16_load_into(emitter, "ax", &instruction->lhs)) {
      return 0;
    }
    return x86_16_emit(emitter, "test ax, ax\njz %s\n", instruction->text);

  case IR_OP_BRANCH_EQ: {
    char text[128];
    if (!x86_16_load_into(emitter, "ax", &instruction->lhs)) {
      return 0;
    }
    if (!x86_16_operand_text(emitter, &instruction->rhs, text, sizeof(text))) {
      x86_16_fail(emitter, "'%s' cannot compare that operand in 16-bit code",
                  emitter->function->name);
      return 0;
    }
    if (instruction->rhs.kind == IR_OPERAND_INT) {
      return x86_16_emit(emitter, "cmp ax, %s\nje %s\n", text,
                         instruction->text);
    }
    return x86_16_emit(emitter, "mov bx, %s\ncmp ax, bx\nje %s\n", text,
                       instruction->text);
  }

  case IR_OP_ASSIGN:
  case IR_OP_CAST:
    if (!x86_16_load_into(emitter, "ax", &instruction->lhs)) {
      return 0;
    }
    return x86_16_store_from(emitter, &instruction->dest, "ax");

  case IR_OP_BINARY:
    return x86_16_binary(emitter, instruction);

  case IR_OP_UNARY: {
    const char *op = instruction->text ? instruction->text : "";
    if (!x86_16_load_into(emitter, "ax", &instruction->lhs)) {
      return 0;
    }
    if (strcmp(op, "-") == 0) {
      if (!x86_16_emit(emitter, "neg ax\n")) {
        return 0;
      }
    } else if (strcmp(op, "~") == 0) {
      if (!x86_16_emit(emitter, "not ax\n")) {
        return 0;
      }
    } else if (strcmp(op, "!") == 0) {
      int label = emitter->compare_label++;
      if (!x86_16_emit(emitter,
                       "test ax, ax\njz .not_true_%d\nmov ax, 0\n"
                       "jmp short .not_end_%d\n.not_true_%d:\nmov ax, 1\n"
                       ".not_end_%d:\n",
                       label, label, label, label)) {
        return 0;
      }
    } else if (strcmp(op, "+") != 0) {
      x86_16_fail(emitter,
                  "'%s' uses the unary operator `%s`, which 16-bit real-mode "
                  "code generation does not support",
                  emitter->function->name, op);
      return 0;
    }
    return x86_16_store_from(emitter, &instruction->dest, "ax");
  }

  case IR_OP_ADDRESS_OF: {
    int offset = 0;
    if (instruction->lhs.name &&
        x86_16_slots_find(&emitter->slots, instruction->lhs.name, &offset)) {
      if (!x86_16_emit(emitter, "lea ax, [bp %c %d]\n", offset < 0 ? '-' : '+',
                       offset < 0 ? -offset : offset)) {
        return 0;
      }
    } else if (instruction->lhs.name &&
               x86_16_symbol_is_global(emitter, instruction->lhs.name)) {
      const char *link_name = code_generator_get_link_symbol_name(
          emitter->generator, instruction->lhs.name);
      if (!x86_16_emit(emitter, "mov ax, %s\n",
                       link_name && link_name[0] ? link_name
                                                 : instruction->lhs.name)) {
        return 0;
      }
    } else {
      x86_16_fail(emitter, "'%s' takes the address of '%s', which 16-bit code "
                           "generation cannot place",
                  emitter->function->name,
                  instruction->lhs.name ? instruction->lhs.name : "<value>");
      return 0;
    }
    return x86_16_store_from(emitter, &instruction->dest, "ax");
  }

  case IR_OP_LOAD: {
    long long width = instruction->rhs.kind == IR_OPERAND_INT
                          ? instruction->rhs.int_value
                          : 2;
    if (!x86_16_load_into(emitter, "bx", &instruction->lhs)) {
      return 0;
    }
    if (width == 1) {
      if (!x86_16_emit(emitter, "mov al, [bx]\nmov ah, 0\n")) {
        return 0;
      }
      if (!instruction->is_unsigned &&
          !x86_16_emit(emitter, "cbw\n")) {
        return 0;
      }
    } else if (width == 2) {
      if (!x86_16_emit(emitter, "mov ax, [bx]\n")) {
        return 0;
      }
    } else {
      x86_16_fail(emitter,
                  "'%s' loads %lld bytes, and 16-bit real-mode code generation "
                  "handles one or two",
                  emitter->function->name, width);
      return 0;
    }
    return x86_16_store_from(emitter, &instruction->dest, "ax");
  }

  case IR_OP_STORE: {
    long long width = instruction->rhs.kind == IR_OPERAND_INT
                          ? instruction->rhs.int_value
                          : 2;
    if (!x86_16_load_into(emitter, "ax", &instruction->lhs)) {
      return 0;
    }
    if (!x86_16_load_into(emitter, "bx", &instruction->dest)) {
      return 0;
    }
    if (width == 1) {
      return x86_16_emit(emitter, "mov [bx], al\n");
    }
    if (width == 2) {
      return x86_16_emit(emitter, "mov [bx], ax\n");
    }
    x86_16_fail(emitter,
                "'%s' stores %lld bytes, and 16-bit real-mode code generation "
                "handles one or two",
                emitter->function->name, width);
    return 0;
  }

  case IR_OP_CALL: {
    size_t i;
    if (!instruction->text) {
      x86_16_fail(emitter, "'%s' has a call with no callee",
                  emitter->function->name);
      return 0;
    }
    for (i = instruction->argument_count; i > 0; i--) {
      char text[128];
      const IROperand *argument = &instruction->arguments[i - 1];
      if (!x86_16_operand_text(emitter, argument, text, sizeof(text))) {
        x86_16_fail(emitter,
                    "'%s' passes an argument 16-bit code generation cannot "
                    "place",
                    emitter->function->name);
        return 0;
      }
      if (argument->kind == IR_OPERAND_INT) {
        if (!x86_16_emit(emitter, "mov ax, %s\npush ax\n", text)) {
          return 0;
        }
      } else if (!x86_16_emit(emitter, "push %s\n", text)) {
        return 0;
      }
    }
    if (!x86_16_emit(emitter, "call %s\n", instruction->text)) {
      return 0;
    }
    if (instruction->argument_count &&
        !x86_16_emit(emitter, "add sp, %zu\n",
                     instruction->argument_count * X86_16_WORD)) {
      return 0;
    }
    return x86_16_store_from(emitter, &instruction->dest, "ax");
  }

  case IR_OP_RETURN:
    if (instruction->lhs.kind != IR_OPERAND_NONE &&
        !x86_16_load_into(emitter, "ax", &instruction->lhs)) {
      return 0;
    }
    return x86_16_emit(emitter, "mov sp, bp\npop bp\nret\n");

  case IR_OP_INLINE_ASM:
    if (!instruction->text) {
      return 1;
    }
    return x86_16_inline_asm(emitter, instruction->text);

  default:
    x86_16_fail(emitter,
                "'%s' uses `%s`, which 16-bit real-mode code generation does "
                "not support",
                emitter->function->name, ir_opcode_name(instruction->op));
    return 0;
  }
}

static int x86_16_plan_frame(X86_16Emitter *emitter) {
  IRFunction *function = emitter->function;
  size_t i;
  int next_local = -X86_16_WORD;
  int next_parameter = 2 * X86_16_WORD;

  for (i = 0; i < function->parameter_count; i++) {
    if (!function->parameter_names || !function->parameter_names[i]) {
      continue;
    }
    if (!x86_16_slots_add(&emitter->slots, function->parameter_names[i],
                          next_parameter)) {
      return 0;
    }
    next_parameter += X86_16_WORD;
  }

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
      if (x86_16_slots_find(&emitter->slots, operand->name, &existing)) {
        continue;
      }
      if (x86_16_symbol_is_global(emitter, operand->name)) {
        continue;
      }
      if (!x86_16_slots_add(&emitter->slots, operand->name, next_local)) {
        return 0;
      }
      next_local -= X86_16_WORD;
    }
    for (argument = 0; argument < instruction->argument_count; argument++) {
      const IROperand *operand = &instruction->arguments[argument];
      int existing = 0;
      if ((operand->kind != IR_OPERAND_TEMP &&
           operand->kind != IR_OPERAND_SYMBOL) ||
          !operand->name) {
        continue;
      }
      if (x86_16_slots_find(&emitter->slots, operand->name, &existing) ||
          x86_16_symbol_is_global(emitter, operand->name)) {
        continue;
      }
      if (!x86_16_slots_add(&emitter->slots, operand->name, next_local)) {
        return 0;
      }
      next_local -= X86_16_WORD;
    }
  }

  emitter->frame_size = -(next_local + X86_16_WORD);
  if (emitter->frame_size & 1) {
    emitter->frame_size++;
  }
  return 1;
}

int code_generator_emit_binary_function_x86_16(
    CodeGenerator *generator, IRFunction *ir_function,
    BinaryFunctionContext *context) {
  X86_16Emitter emitter;
  size_t i;
  int result = 0;

  if (!generator || !ir_function || !context) {
    return 0;
  }

  memset(&emitter, 0, sizeof(emitter));
  emitter.generator = generator;
  emitter.function = ir_function;
  emitter.context = context;

  if (!x86_16_plan_frame(&emitter)) {
    code_generator_set_error(generator,
                             "Out of memory while planning the 16-bit frame of "
                             "'%s'",
                             ir_function->name);
    goto cleanup;
  }

  if (!x86_16_emit(&emitter, "push bp\nmov bp, sp\n")) {
    goto cleanup;
  }
  if (emitter.frame_size > 0 &&
      !x86_16_emit(&emitter, "sub sp, %d\n", emitter.frame_size)) {
    goto cleanup;
  }

  for (i = 0; i < ir_function->instruction_count; i++) {
    if (!x86_16_instruction(&emitter, &ir_function->instructions[i])) {
      goto cleanup;
    }
  }

  if (!x86_16_emit(&emitter, "mov sp, bp\npop bp\nret\n")) {
    goto cleanup;
  }

  result = code_generator_binary_assemble_text(
      generator, context, emitter.text, 16, 0, &ir_function->location, NULL);

cleanup:
  x86_16_slots_destroy(&emitter.slots);
  free(emitter.text);
  return result;
}
