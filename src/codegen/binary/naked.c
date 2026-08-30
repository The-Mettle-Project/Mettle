#include "codegen/binary/internal.h"
#include "codegen/asm/x86_asm.h"
#include "codegen/target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const GP_REGISTER_NAMES[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

static const BinaryGpRegister INTERRUPT_SAVED[] = {
    BINARY_GP_RAX, BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_RBX,
    BINARY_GP_RBP, BINARY_GP_RSI, BINARY_GP_RDI, BINARY_GP_R8,
    BINARY_GP_R9,  BINARY_GP_R10, BINARY_GP_R11, BINARY_GP_R12,
    BINARY_GP_R13, BINARY_GP_R14, BINARY_GP_R15};

#define INTERRUPT_SAVED_COUNT                                                  \
  ((int)(sizeof(INTERRUPT_SAVED) / sizeof(INTERRUPT_SAVED[0])))

int code_generator_emit_binary_naked_function(CodeGenerator *generator,
                                              IRFunction *ir_function,
                                              BinaryFunctionContext *context) {
  size_t i;
  int bits = mtlc_target()->code_bits;
  int saw_asm = 0;

  if (!generator || !ir_function || !context) {
    return 0;
  }

  for (i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *instruction = &ir_function->instructions[i];
    switch (instruction->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_DECLARE_LOCAL:
      continue;
    case IR_OP_RETURN:
      if (instruction->lhs.kind != IR_OPERAND_NONE) {
        generator->has_user_error = 1;
        code_generator_set_error(
            generator,
            "'%s' is `@naked`, so it has no frame to return a value from; the "
            "asm block must leave the result in the return register itself",
            ir_function->name);
        return 0;
      }
      continue;
    case IR_OP_INLINE_ASM:
      if (!instruction->text || instruction->text[0] == '\0') {
        continue;
      }
      if (!code_generator_binary_assemble_text(generator, context,
                                               instruction->text, bits, 1,
                                               &instruction->location, &bits)) {
        return 0;
      }
      saw_asm = 1;
      continue;
    default:
      generator->has_user_error = 1;
      code_generator_set_error(
          generator,
          "'%s' is `@naked`: its body may hold only `asm` blocks, because a "
          "naked function has no prologue, no frame and no epilogue for "
          "compiled code to stand on (found `%s`)",
          ir_function->name, ir_opcode_name(instruction->op));
      return 0;
    }
  }

  if (!saw_asm) {
    generator->has_user_error = 1;
    code_generator_set_error(
        generator, "'%s' is `@naked` but its body holds no `asm` block",
        ir_function->name);
    return 0;
  }
  return 1;
}

int code_generator_binary_check_interrupt_signature(CodeGenerator *generator,
                                                    IRFunction *ir_function) {
  if (!generator || !ir_function) {
    return 0;
  }
  if (mtlc_target()->code_bits == 16 && ir_function->parameter_count >= 2) {
    generator->has_user_error = 1;
    code_generator_set_error(
        generator,
        "'%s' is `@interrupt` and takes an error code, which real mode never "
        "pushes; a 16-bit handler takes no parameters or the frame pointer "
        "alone",
        ir_function->name);
    return 0;
  }
  if (ir_function->parameter_count > 2) {
    generator->has_user_error = 1;
    code_generator_set_error(
        generator,
        "'%s' is `@interrupt`: it takes no parameters, a pointer to the "
        "interrupt frame, or that pointer and the error code the CPU pushed",
        ir_function->name);
    return 0;
  }
  if (ir_function->return_type_name &&
      strcmp(ir_function->return_type_name, "void") != 0) {
    generator->has_user_error = 1;
    code_generator_set_error(
        generator,
        "'%s' is `@interrupt`: an interrupt handler returns nothing, because "
        "the interrupt return has nowhere to hand a value back to",
        ir_function->name);
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_interrupt_entry(
    CodeGenerator *generator, IRFunction *ir_function,
    BinaryFunctionContext *context) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  size_t parameter_count = ir_function->parameter_count;
  char body[1024];
  size_t used = 0;
  int i;
  const char *frame_register = GP_REGISTER_NAMES[abi->int_param_registers[0]];
  const char *error_register = GP_REGISTER_NAMES[abi->int_param_registers[1]];

  if (!code_generator_binary_check_interrupt_signature(generator,
                                                       ir_function)) {
    return 0;
  }

  used += (size_t)snprintf(body + used, sizeof(body) - used, "cld\n");
  for (i = 0; i < INTERRUPT_SAVED_COUNT; i++) {
    used += (size_t)snprintf(body + used, sizeof(body) - used, "push %s\n",
                             GP_REGISTER_NAMES[INTERRUPT_SAVED[i]]);
  }

  if (parameter_count >= 2) {
    used += (size_t)snprintf(body + used, sizeof(body) - used,
                             "mov %s, [rsp + %d]\n", error_register,
                             INTERRUPT_SAVED_COUNT * 8);
    used += (size_t)snprintf(body + used, sizeof(body) - used,
                             "lea %s, [rsp + %d]\n", frame_register,
                             (INTERRUPT_SAVED_COUNT + 1) * 8);
  } else if (parameter_count == 1) {
    used += (size_t)snprintf(body + used, sizeof(body) - used,
                             "lea %s, [rsp + %d]\n", frame_register,
                             INTERRUPT_SAVED_COUNT * 8);
  }

  snprintf(body + used, sizeof(body) - used,
           "mov rbx, rsp\n"
           "and rsp, -16\n"
           "sub rsp, 8\n");

  return code_generator_binary_assemble_text(generator, context, body, 64, 0,
                                             &ir_function->location, NULL);
}

int code_generator_binary_emit_interrupt_exit(CodeGenerator *generator,
                                              IRFunction *ir_function,
                                              BinaryFunctionContext *context) {
  char body[1024];
  size_t used = 0;
  int i;

  if (!generator || !ir_function || !context) {
    return 0;
  }

  used += (size_t)snprintf(body + used, sizeof(body) - used, "mov rsp, rbx\n");
  for (i = INTERRUPT_SAVED_COUNT; i > 0; i--) {
    used += (size_t)snprintf(body + used, sizeof(body) - used, "pop %s\n",
                             GP_REGISTER_NAMES[INTERRUPT_SAVED[i - 1]]);
  }
  if (ir_function->parameter_count >= 2) {
    used += (size_t)snprintf(body + used, sizeof(body) - used, "add rsp, 8\n");
  }
  snprintf(body + used, sizeof(body) - used, "iretq\n");

  return code_generator_binary_assemble_text(generator, context, body, 64, 0,
                                             &ir_function->location, NULL);
}
