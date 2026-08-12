#include "startup.h"

#include "arm64.h"
#include "internal.h"

#define STARTUP_ARGV_FRAME_SIZE 80

static int emit_getmainargs_prologue(BinaryCodeBuffer *code,
                                     BinaryCallRelocationTable *relocations) {
  size_t getmainargs_offset = 0;

  if (!binary_emit_sub_rsp_imm32(code, STARTUP_ARGV_FRAME_SIZE) ||
      !binary_emit_lea_reg_mem(code, BINARY_GP_RCX, BINARY_GP_RSP, 40) ||
      !binary_emit_lea_reg_mem(code, BINARY_GP_RDX, BINARY_GP_RSP, 48) ||
      !binary_emit_call_placeholder(code, &getmainargs_offset) ||
      !binary_call_relocation_table_add(relocations, "mettle_rt_getmainargs",
                                        getmainargs_offset) ||
      !binary_emit_mov_reg_mem32(code, BINARY_GP_RCX, BINARY_GP_RSP, 40) ||
      !binary_emit_mov_reg_mem(code, BINARY_GP_RDX, BINARY_GP_RSP, 48)) {
    return 0;
  }

  return 1;
}

/* Linux/SysV self-contained program entry. The kernel transfers control to
 * `_start` with the stack laid out as:
 *     [rsp]      = argc
 *     [rsp + 8]  = argv[0]
 *     ...        = argv[argc-1], NULL, envp..., NULL
 * rsp is 16-byte aligned at entry and there is no return address. We pass argc
 * (RDI) and &argv (RSI) to main per SysV, call it, then invoke the exit(2)
 * syscall with main's return value. No libc, no CRT object. */
static int binary_write_elf_start_object(const char *path, int profile_runtime,
                                         int stack_trace_init,
                                         int main_wants_argc_argv) {
  BinaryEmitter *emitter = NULL;
  BinaryCodeBuffer code = {0};
  BinaryCallRelocationTable relocations = {0};
  size_t text_section = 0;
  size_t function_offset = 0;
  size_t runtime_startup_offset = 0;
  size_t crash_startup_offset = 0;
  size_t main_call_offset = 0;
  size_t report_call_offset = 0;
  int result = 1;

  if (!path) {
    return 1;
  }

  emitter = binary_emitter_create(BINARY_TARGET_FORMAT_ELF_X64);
  if (!emitter) {
    return 1;
  }

  /* Give the runtime the kernel supplied argument and environment block. */
  if (!binary_emit_mov_reg_mem(&code, BINARY_GP_RDI, BINARY_GP_RSP, 0) ||
      !binary_emit_lea_reg_mem(&code, BINARY_GP_RSI, BINARY_GP_RSP, 8) ||
      !binary_emit_call_placeholder(&code, &runtime_startup_offset) ||
      !binary_call_relocation_table_add(&relocations, "mettle_rt_startup",
                                        runtime_startup_offset)) {
    goto cleanup;
  }

  /* Optional crash-handler init before main. It takes no args. */
  if (stack_trace_init) {
    if (!binary_emit_call_placeholder(&code, &crash_startup_offset) ||
        !binary_call_relocation_table_add(&relocations, "mettle_crash_startup",
                                          crash_startup_offset)) {
      goto cleanup;
    }
  }

  /* main(argc, argv): argc = [rsp], &argv = rsp + 8.
   * At _start, rsp % 16 == 0. The crash-startup call above (if emitted) pushed
   * and popped a return address symmetrically, so rsp is still 16-aligned here.
   * A direct `call main` with nothing else pushed satisfies SysV's "rsp % 16
   * == 0 immediately before call" rule. */
  if (main_wants_argc_argv) {
    if (!binary_emit_mov_reg_mem(&code, BINARY_GP_RDI, BINARY_GP_RSP, 0) ||
        !binary_emit_lea_reg_mem(&code, BINARY_GP_RSI, BINARY_GP_RSP, 8)) {
      goto cleanup;
    }
  }

  if (!binary_emit_call_placeholder(&code, &main_call_offset) ||
      !binary_call_relocation_table_add(&relocations, "main",
                                        main_call_offset)) {
    goto cleanup;
  }

  if (profile_runtime) {
    /* Preserve main's result across the report call. The kernel gives _start
     * a 16 byte aligned stack. A push moves it off that boundary, so reserve
     * one more word before call to keep the SysV call site aligned. */
    if (!binary_emit_push_reg(&code, BINARY_GP_RAX) ||
        !binary_emit_sub_rsp_imm32(&code, 8) ||
        !binary_emit_call_placeholder(&code, &report_call_offset) ||
        !binary_call_relocation_table_add(&relocations, "mettle_profile_report",
                                          report_call_offset) ||
        !binary_emit_add_rsp_imm32(&code, 8) ||
        !binary_emit_pop_reg(&code, BINARY_GP_RAX)) {
      goto cleanup;
    }
  }

  /* exit(main_result): syscall 60, status in RDI. main returns int32 in EAX;
   * move it to EDI (zero-extends into RDI), set RAX = 60, syscall. */
  if (!binary_emit_mov_reg_reg(&code, BINARY_GP_RDI, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_imm32_zero_extend(&code, BINARY_GP_RAX, 60) ||
      !binary_emit_syscall(&code)) {
    goto cleanup;
  }

  text_section = binary_emitter_get_or_create_section(
      emitter, ".text", BINARY_SECTION_TEXT, 0, BINARY_TEXT_SECTION_ALIGNMENT);
  if (text_section == (size_t)-1 ||
      !binary_emitter_align_section(emitter, text_section,
                                    BINARY_TEXT_SECTION_ALIGNMENT, 0x90)) {
    goto cleanup;
  }

  {
    BinarySection *section = binary_emitter_get_section(emitter, text_section);
    if (!section) {
      goto cleanup;
    }
    function_offset = section->size;
  }

  if (!binary_emitter_define_symbol(emitter, "_start", BINARY_SYMBOL_GLOBAL,
                                    text_section, function_offset, code.size) ||
      !binary_emitter_append_bytes(emitter, text_section, code.data, code.size,
                                   NULL) ||
      !binary_emitter_declare_external(emitter, "main") ||
      !binary_emitter_declare_external(emitter, "mettle_rt_startup")) {
    goto cleanup;
  }

  if (stack_trace_init &&
      !binary_emitter_declare_external(emitter, "mettle_crash_startup")) {
    goto cleanup;
  }
  if (profile_runtime &&
      !binary_emitter_declare_external(emitter, "mettle_profile_report")) {
    goto cleanup;
  }

  for (size_t i = 0; i < relocations.count; i++) {
    BinaryCallRelocation *relocation = &relocations.items[i];
    if (!binary_emitter_add_relocation(
            emitter, text_section,
            function_offset + relocation->displacement_offset,
            BINARY_RELOCATION_REL32, relocation->symbol_name, 0)) {
      goto cleanup;
    }
  }

  if (!binary_emitter_write_object_file(emitter, path)) {
    goto cleanup;
  }

  result = 0;

cleanup:
  binary_call_relocation_table_destroy(&relocations);
  binary_code_buffer_destroy(&code);
  binary_emitter_destroy(emitter);
  return result;
}

static int append_arm64_word(BinaryCodeBuffer *code, uint32_t word) {
  return binary_code_buffer_append_u32(code, word);
}

static int append_arm64_call(BinaryCodeBuffer *code,
                             BinaryCallRelocationTable *relocations,
                             const char *symbol_name) {
  size_t call_offset = code ? code->size : 0;
  return append_arm64_word(code, arm64_bl(0)) &&
         binary_call_relocation_table_add(relocations, symbol_name,
                                          call_offset);
}

/* AArch64 Linux entry with the same kernel stack contract as x86-64 Linux.
 * x19 keeps the initial stack pointer across calls. x20 keeps main's result
 * across the optional profile report. Both registers are callee saved under
 * AAPCS64. The kernel enters with SP aligned to 16 bytes, so every BL below has
 * the required alignment without a frame. */
static int binary_write_elf_arm64_start_object(const char *path,
                                               int profile_runtime,
                                               int stack_trace_init,
                                               int main_wants_argc_argv) {
  BinaryEmitter *emitter = NULL;
  BinaryCodeBuffer code = {0};
  BinaryCallRelocationTable relocations = {0};
  size_t text_section = 0;
  size_t function_offset = 0;
  int result = 1;

  if (!path) {
    return 1;
  }

  emitter = binary_emitter_create(BINARY_TARGET_FORMAT_ELF_ARM64);
  if (!emitter) {
    return 1;
  }

  if (!append_arm64_word(&code, arm64_mov_sp(ARM64_X19, ARM64_SP)) ||
      !append_arm64_word(&code,
                         arm64_ldr_imm(1, ARM64_X0, ARM64_X19, 0)) ||
      !append_arm64_word(&code,
                         arm64_add_imm(1, ARM64_X1, ARM64_X19, 8, 0)) ||
      !append_arm64_call(&code, &relocations, "mettle_rt_startup")) {
    goto cleanup;
  }

  if (stack_trace_init &&
      !append_arm64_call(&code, &relocations, "mettle_crash_startup")) {
    goto cleanup;
  }

  if (main_wants_argc_argv &&
      (!append_arm64_word(&code,
                          arm64_ldr_imm(1, ARM64_X0, ARM64_X19, 0)) ||
       !append_arm64_word(&code,
                          arm64_add_imm(1, ARM64_X1, ARM64_X19, 8, 0)))) {
    goto cleanup;
  }

  if (!append_arm64_call(&code, &relocations, "main") ||
      !append_arm64_word(&code,
                         arm64_mov_reg(0, ARM64_X20, ARM64_X0))) {
    goto cleanup;
  }

  if (profile_runtime &&
      !append_arm64_call(&code, &relocations, "mettle_profile_report")) {
    goto cleanup;
  }

  if (!append_arm64_word(&code,
                         arm64_mov_reg(0, ARM64_X0, ARM64_X20)) ||
      !append_arm64_word(&code, arm64_movz(1, ARM64_X8, 93, 0)) ||
      !append_arm64_word(&code, 0xD4000001u)) {
    goto cleanup;
  }

  text_section = binary_emitter_get_or_create_section(
      emitter, ".text", BINARY_SECTION_TEXT, 0, BINARY_TEXT_SECTION_ALIGNMENT);
  if (text_section == (size_t)-1 ||
      !binary_emitter_align_section(emitter, text_section,
                                    BINARY_TEXT_SECTION_ALIGNMENT, 0)) {
    goto cleanup;
  }

  {
    BinarySection *section = binary_emitter_get_section(emitter, text_section);
    if (!section) {
      goto cleanup;
    }
    function_offset = section->size;
  }

  if (!binary_emitter_define_symbol(emitter, "_start", BINARY_SYMBOL_GLOBAL,
                                    text_section, function_offset, code.size) ||
      !binary_emitter_append_bytes(emitter, text_section, code.data, code.size,
                                   NULL) ||
      !binary_emitter_declare_external(emitter, "main") ||
      !binary_emitter_declare_external(emitter, "mettle_rt_startup")) {
    goto cleanup;
  }
  if (stack_trace_init &&
      !binary_emitter_declare_external(emitter, "mettle_crash_startup")) {
    goto cleanup;
  }
  if (profile_runtime &&
      !binary_emitter_declare_external(emitter, "mettle_profile_report")) {
    goto cleanup;
  }

  for (size_t i = 0; i < relocations.count; i++) {
    BinaryCallRelocation *relocation = &relocations.items[i];
    if (!binary_emitter_add_relocation(
            emitter, text_section, function_offset + relocation->displacement_offset,
            BINARY_RELOCATION_ARM64_CALL26, relocation->symbol_name, 0)) {
      goto cleanup;
    }
  }

  if (!binary_emitter_write_object_file(emitter, path)) {
    goto cleanup;
  }
  result = 0;

cleanup:
  binary_call_relocation_table_destroy(&relocations);
  binary_code_buffer_destroy(&code);
  binary_emitter_destroy(emitter);
  return result;
}

int binary_write_program_startup_object_for_target(
    const char *path, BinaryTargetFormat target, int profile_runtime,
    int stack_trace_init, int main_wants_argc_argv) {
  if (target == BINARY_TARGET_FORMAT_ELF_X64) {
    return binary_write_elf_start_object(path, profile_runtime, stack_trace_init,
                                         main_wants_argc_argv);
  }
  if (target == BINARY_TARGET_FORMAT_ELF_ARM64) {
    return binary_write_elf_arm64_start_object(path, profile_runtime,
                                               stack_trace_init,
                                               main_wants_argc_argv);
  }
  BinaryEmitter *emitter = NULL;
  BinaryCodeBuffer code = {0};
  BinaryCallRelocationTable relocations = {0};
  size_t text_section = 0;
  size_t function_offset = 0;
  size_t crash_startup_offset = 0;
  size_t main_call_offset = 0;
  size_t report_call_offset = 0;
  size_t exit_call_offset = 0;
  int result = 1;

  if (!path) {
    return 1;
  }

  emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  if (!emitter) {
    return 1;
  }

  if (!binary_emit_sub_rsp_imm32(&code, BINARY_WIN64_SHADOW_SPACE_SIZE + 8)) {
    goto cleanup;
  }

  if (stack_trace_init) {
    if (!binary_emit_call_placeholder(&code, &crash_startup_offset) ||
        !binary_call_relocation_table_add(&relocations, "mettle_crash_startup",
                                          crash_startup_offset)) {
      goto cleanup;
    }
  }

  if (main_wants_argc_argv &&
      !emit_getmainargs_prologue(&code, &relocations)) {
    goto cleanup;
  }

  if (!binary_emit_call_placeholder(&code, &main_call_offset) ||
      !binary_call_relocation_table_add(&relocations, "main",
                                        main_call_offset)) {
    goto cleanup;
  }

  if (main_wants_argc_argv &&
      !binary_emit_add_rsp_imm32(&code, STARTUP_ARGV_FRAME_SIZE)) {
    goto cleanup;
  }

  if (profile_runtime) {
    if (!binary_emit_push_reg(&code, BINARY_GP_RAX) ||
        !binary_emit_sub_rsp_imm32(&code, BINARY_WIN64_SHADOW_SPACE_SIZE + 8) ||
        !binary_emit_call_placeholder(&code, &report_call_offset) ||
        !binary_call_relocation_table_add(&relocations, "mettle_profile_report",
                                          report_call_offset) ||
        !binary_emit_add_rsp_imm32(&code, BINARY_WIN64_SHADOW_SPACE_SIZE + 8) ||
        !binary_emit_pop_reg(&code, BINARY_GP_RAX)) {
      goto cleanup;
    }
  }

  if (!binary_emit_mov_reg_reg(&code, BINARY_GP_RCX, BINARY_GP_RAX) ||
      !binary_emit_call_placeholder(&code, &exit_call_offset) ||
      !binary_call_relocation_table_add(&relocations, "ExitProcess",
                                        exit_call_offset)) {
    goto cleanup;
  }

  text_section = binary_emitter_get_or_create_section(
      emitter, ".text", BINARY_SECTION_TEXT, 0, BINARY_TEXT_SECTION_ALIGNMENT);
  if (text_section == (size_t)-1 ||
      !binary_emitter_align_section(emitter, text_section,
                                    BINARY_TEXT_SECTION_ALIGNMENT, 0x90)) {
    goto cleanup;
  }

  {
    BinarySection *section = binary_emitter_get_section(emitter, text_section);
    if (!section) {
      goto cleanup;
    }
    function_offset = section->size;
  }

  if (!binary_emitter_define_symbol(emitter, "mettle_start",
                                    BINARY_SYMBOL_GLOBAL, text_section,
                                    function_offset, code.size) ||
      !binary_emitter_append_bytes(emitter, text_section, code.data, code.size,
                                   NULL) ||
      !binary_emitter_declare_external(emitter, "main") ||
      !binary_emitter_declare_external(emitter, "ExitProcess")) {
    goto cleanup;
  }

  if (main_wants_argc_argv &&
      !binary_emitter_declare_external(emitter, "mettle_rt_getmainargs")) {
    goto cleanup;
  }

  if (stack_trace_init &&
      !binary_emitter_declare_external(emitter, "mettle_crash_startup")) {
    goto cleanup;
  }

  if (profile_runtime &&
      !binary_emitter_declare_external(emitter, "mettle_profile_report")) {
    goto cleanup;
  }

  for (size_t i = 0; i < relocations.count; i++) {
    BinaryCallRelocation *relocation = &relocations.items[i];
    if (!binary_emitter_add_relocation(
            emitter, text_section,
            function_offset + relocation->displacement_offset,
            BINARY_RELOCATION_REL32, relocation->symbol_name, 0)) {
      goto cleanup;
    }
  }

  if (!binary_emitter_write_object_file(emitter, path)) {
    goto cleanup;
  }

  result = 0;

cleanup:
  binary_call_relocation_table_destroy(&relocations);
  binary_code_buffer_destroy(&code);
  binary_emitter_destroy(emitter);
  return result;
}

int binary_write_program_startup_object(const char *path, int profile_runtime,
                                        int stack_trace_init,
                                        int main_wants_argc_argv) {
  return binary_write_program_startup_object_for_target(
      path, binary_target_format_host_default(), profile_runtime,
      stack_trace_init, main_wants_argc_argv);
}
