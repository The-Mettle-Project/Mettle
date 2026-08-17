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

#if !defined(_WIN32) && !defined(_WIN64)
/* Three more of the same: the POSIX arm of the owned runtime backs these for
 * a real compiler, and this harness is a hosted build without it. The report
 * under test is raised deliberately rather than by a fault, so the signal
 * installer is never armed and the readability probe is only consulted while
 * symbolizing. */
#include <signal.h>
#include <pthread.h>

int mettle_install_signal_handler(int signal_number,
                                  void (*handler)(int, void *, void *));
unsigned int mettle_thread_current_id(void);
int mettle_address_is_readable(const void *address, unsigned long long length);

int mettle_install_signal_handler(int signal_number,
                                  void (*handler)(int, void *, void *)) {
  (void)signal_number;
  (void)handler;
  return 1;
}

unsigned int mettle_thread_current_id(void) {
  return (unsigned int)(unsigned long)pthread_self();
}

int mettle_address_is_readable(const void *address, unsigned long long length) {
  return address != 0 && length > 0;
}
#endif

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
