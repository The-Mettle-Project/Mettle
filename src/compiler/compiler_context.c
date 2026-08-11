#include "compiler_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_INSTRUCTION_INDEX_NONE ((size_t)-1)

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

static MettleCompilerContext g_default_compiler_context = {0};

#if defined(_WIN32) || defined(_WIN64)
static DWORD g_compiler_context_tls_index = TLS_OUT_OF_INDEXES;

static VOID WINAPI mettle_compiler_context_tls_free(PVOID ptr) {
  free(ptr);
}

static MettleCompilerContext *mettle_compiler_ctx_storage(void) {
  MettleCompilerContext *ctx = NULL;

  if (g_compiler_context_tls_index == TLS_OUT_OF_INDEXES) {
    g_compiler_context_tls_index = FlsAlloc(mettle_compiler_context_tls_free);
    if (g_compiler_context_tls_index == FLS_OUT_OF_INDEXES) {
      return &g_default_compiler_context;
    }
  }

  ctx = (MettleCompilerContext *)FlsGetValue(g_compiler_context_tls_index);
  if (!ctx) {
    ctx = (MettleCompilerContext *)calloc(1, sizeof(MettleCompilerContext));
    if (!ctx) {
      return &g_default_compiler_context;
    }
    ctx->phase = METTLE_COMPILER_PHASE_UNKNOWN;
    ctx->ir_instruction_index = IR_INSTRUCTION_INDEX_NONE;
    FlsSetValue(g_compiler_context_tls_index, ctx);
  }
  return ctx;
}
#else
typedef struct MettleCompilerContextSlot {
  unsigned long thread_id;
  int used;
  MettleCompilerContext context;
} MettleCompilerContextSlot;

#define METTLE_COMPILER_CONTEXT_SLOT_COUNT 64

extern unsigned int mettle_thread_current_id(void);

static volatile int g_compiler_context_lock = 0;
static MettleCompilerContextSlot
    g_compiler_context_slots[METTLE_COMPILER_CONTEXT_SLOT_COUNT];

static void mettle_compiler_context_lock(void) {
  while (__atomic_exchange_n(&g_compiler_context_lock, 1, __ATOMIC_ACQUIRE)) {
  }
}

static void mettle_compiler_context_unlock(void) {
  __atomic_store_n(&g_compiler_context_lock, 0, __ATOMIC_RELEASE);
}

static MettleCompilerContext *mettle_compiler_ctx_storage(void) {
  unsigned long thread_id = (unsigned long)mettle_thread_current_id();
  MettleCompilerContextSlot *free_slot = NULL;

  mettle_compiler_context_lock();
  for (size_t i = 0; i < METTLE_COMPILER_CONTEXT_SLOT_COUNT; i++) {
    MettleCompilerContextSlot *slot = &g_compiler_context_slots[i];
    if (slot->used && slot->thread_id == thread_id) {
      mettle_compiler_context_unlock();
      return &slot->context;
    }
    if (!slot->used && !free_slot) {
      free_slot = slot;
    }
  }

  if (!free_slot) {
    mettle_compiler_context_unlock();
    return &g_default_compiler_context;
  }
  memset(free_slot, 0, sizeof(*free_slot));
  free_slot->thread_id = thread_id;
  free_slot->used = 1;
  free_slot->context.phase = METTLE_COMPILER_PHASE_UNKNOWN;
  free_slot->context.ir_instruction_index = IR_INSTRUCTION_INDEX_NONE;
  mettle_compiler_context_unlock();
  return &free_slot->context;
}
#endif

const char *mettle_compiler_phase_name(MettleCompilerPhase phase) {
  static const char *phase_names[METTLE_COMPILER_PHASE_COUNT] = {
      "read input",       "lexical validation", "init",
      "parse",            "prelude injection",  "imports",
      "monomorphize",     "type check",         "IR lowering",
      "IR optimization",  "IR dump",            "codegen",
      "write output",     "debug info",         "cleanup",
  };

  if (phase < 0 || phase >= METTLE_COMPILER_PHASE_COUNT) {
    return "unknown";
  }
  return phase_names[phase];
}

MettleCompilerContext *mettle_compiler_ctx(void) {
  return mettle_compiler_ctx_storage();
}

void mettle_compiler_ctx_reset(void) {
  MettleCompilerContext *ctx = mettle_compiler_ctx_storage();
  const char *input = ctx->input_filename;
  int debug_compiler = ctx->debug_compiler;
  int dump_ir = ctx->dump_ir;

  memset(ctx, 0, sizeof(*ctx));
  ctx->phase = METTLE_COMPILER_PHASE_UNKNOWN;
  ctx->ir_instruction_index = IR_INSTRUCTION_INDEX_NONE;
  ctx->input_filename = input;
  ctx->debug_compiler = debug_compiler;
  ctx->dump_ir = dump_ir;
}

void mettle_compiler_ctx_set_phase(MettleCompilerPhase phase) {
  mettle_compiler_ctx()->phase = phase;
}

void mettle_compiler_ctx_set_input_filename(const char *filename) {
  mettle_compiler_ctx()->input_filename = filename;
}

void mettle_compiler_ctx_set_current_filename(const char *filename) {
  mettle_compiler_ctx()->current_filename = filename;
}

void mettle_compiler_ctx_set_function_name(const char *name) {
  mettle_compiler_ctx()->function_name = name;
}

void mettle_compiler_ctx_set_pass_name(const char *pass_name) {
  mettle_compiler_ctx()->pass_name = pass_name;
}

void mettle_compiler_ctx_set_fixpoint_iteration(int iteration) {
  mettle_compiler_ctx()->fixpoint_iteration = iteration;
}

void mettle_compiler_ctx_set_ir_instruction(size_t index,
                                            const IRInstruction *instruction) {
  MettleCompilerContext *ctx = mettle_compiler_ctx();
  ctx->ir_instruction_index = index;
  ctx->ir_instruction = instruction;
}

void mettle_compiler_ctx_clear_ir_instruction(void) {
  MettleCompilerContext *ctx = mettle_compiler_ctx();
  ctx->ir_instruction_index = IR_INSTRUCTION_INDEX_NONE;
  ctx->ir_instruction = NULL;
}

void mettle_compiler_ctx_set_last_action(const char *action) {
  MettleCompilerContext *ctx = mettle_compiler_ctx();
  if (ctx->debug_compiler) {
    ctx->last_action = action;
  }
}

void mettle_compiler_ctx_set_ir_program(IRProgram *program) {
  mettle_compiler_ctx()->ir_program = program;
}

void mettle_compiler_ctx_set_options(int debug_compiler, int dump_ir) {
  MettleCompilerContext *ctx = mettle_compiler_ctx();
  ctx->debug_compiler = debug_compiler;
  ctx->dump_ir = dump_ir;
}

static const char *mettle_compiler_ctx_active_filename(
    const MettleCompilerContext *ctx) {
  if (ctx->current_filename) {
    return ctx->current_filename;
  }
  return ctx->input_filename;
}

void mettle_compiler_ctx_write_report(FILE *output, const char *reason,
                                      const char *detail) {
  MettleCompilerContext *ctx = mettle_compiler_ctx();
  const char *file = mettle_compiler_ctx_active_filename(ctx);
  char instruction_buffer[512];

  if (!output) {
    return;
  }

  fprintf(output, "Mettle internal compiler error");
  if (reason && reason[0] != '\0') {
    fprintf(output, ": %s", reason);
  }
  if (detail && detail[0] != '\0') {
    fprintf(output, " (%s)", detail);
  }
  fprintf(output, "\n\n");

  fprintf(output, "Phase: %s\n",
          mettle_compiler_phase_name(ctx->phase));
  if (ctx->pass_name) {
    fprintf(output, "Pass: %s\n", ctx->pass_name);
    if (ctx->fixpoint_iteration > 0) {
      fprintf(output, "Fixpoint iteration: %d\n", ctx->fixpoint_iteration);
    }
  }
  if (file) {
    fprintf(output, "File: %s\n", file);
  }
  if (ctx->function_name) {
    fprintf(output, "Function: %s\n", ctx->function_name);
  }

  if (ctx->ir_instruction_index != IR_INSTRUCTION_INDEX_NONE) {
    fprintf(output, "IR instruction: #%zu\n", ctx->ir_instruction_index);
    if (ctx->ir_instruction &&
        ir_instruction_dump(ctx->ir_instruction,
                            instruction_buffer, sizeof(instruction_buffer))) {
      fprintf(output, "  %s\n", instruction_buffer);
    }
  }

  if (ctx->last_action) {
    fprintf(output, "\nLast action:\n  %s\n", ctx->last_action);
  }

  if (ctx->input_filename) {
    fprintf(output, "\nPlease rerun with:\n  mettle --dump-ir --debug-compiler %s\n",
            ctx->input_filename);
  }
}

void mettle_compiler_ctx_write_snapshot(void) {
  MettleCompilerContext *ctx = mettle_compiler_ctx();
  const char *input = ctx->input_filename;
  char path[512];
  FILE *output = NULL;

  if (!ctx->debug_compiler || !ctx->ir_program || !input) {
    return;
  }

  snprintf(path, sizeof(path), "%s.ice.ir", input);
  output = fopen(path, "w");
  if (!output) {
    return;
  }

  (void)ir_program_dump(ctx->ir_program, output);
  fclose(output);
  fprintf(stderr, "Wrote IR snapshot: %s\n", path);
}
