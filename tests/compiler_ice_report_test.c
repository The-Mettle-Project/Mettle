#include "../src/compiler/compiler_context.h"
#include "../src/compiler/compiler_crash.h"

#include <stdio.h>
#include <string.h>

/* crash_handler.c reaches for these, but they live in the freestanding runtime
 * and this harness is an ordinary hosted build that does not link it. */
void mettle_crash_write_stderr_bytes(const char *text, size_t length);
void mettle_crash_write_stderr(const char *text);
long long (*mettle_crash_heap_classifier)(void *address) = NULL;

void mettle_crash_write_stderr_bytes(const char *text, size_t length) {
  fwrite(text, 1, length, stderr);
}

void mettle_crash_write_stderr(const char *text) {
  mettle_crash_write_stderr_bytes(text, strlen(text));
}

int main(void) {
  IRInstruction instruction = {0};
  char line[256];

  mettle_compiler_ctx_reset();
  mettle_compiler_ctx_set_input_filename("examples/grep/grep.mettle");
  mettle_compiler_ctx_set_current_filename("examples/grep/grep.mettle");
  mettle_compiler_ctx_set_phase(METTLE_COMPILER_PHASE_IR_OPTIMIZATION);
  mettle_compiler_ctx_set_pass_name("memcpy_inline");
  mettle_compiler_ctx_set_function_name("fill_buffer");
  mettle_compiler_ctx_set_options(1, 1);
  mettle_compiler_ctx_set_last_action(
      "collecting temp uses for IR_OP_MEMCPY_INLINE");

  instruction.op = IR_OP_MEMCPY_INLINE;
  instruction.dest = ir_operand_temp("tmp42");
  instruction.lhs = ir_operand_temp("src");
  instruction.rhs = ir_operand_temp("size");

  mettle_compiler_ctx_set_ir_instruction(184, &instruction);
  if (!ir_instruction_dump(&instruction, line, sizeof(line))) {
    fprintf(stderr, "ir_instruction_dump failed\n");
    return 1;
  }

  mettle_compiler_ice_report("access violation", "0xC0000005");
  return 0;
}
