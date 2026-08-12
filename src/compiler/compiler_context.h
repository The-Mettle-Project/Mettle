#ifndef COMPILER_CONTEXT_H
#define COMPILER_CONTEXT_H

#include "ir/ir.h"

#include <stddef.h>
#include <stdio.h>

typedef enum {
  METTLE_COMPILER_PHASE_UNKNOWN = -1,
  METTLE_COMPILER_PHASE_READ_INPUT = 0,
  METTLE_COMPILER_PHASE_LEXICAL_VALIDATION,
  METTLE_COMPILER_PHASE_INIT,
  METTLE_COMPILER_PHASE_PARSE,
  METTLE_COMPILER_PHASE_PRELUDE,
  METTLE_COMPILER_PHASE_IMPORTS,
  METTLE_COMPILER_PHASE_MONOMORPHIZE,
  METTLE_COMPILER_PHASE_TYPE_CHECK,
  METTLE_COMPILER_PHASE_IR_LOWERING,
  METTLE_COMPILER_PHASE_IR_OPTIMIZATION,
  METTLE_COMPILER_PHASE_IR_DUMP,
  METTLE_COMPILER_PHASE_CODEGEN,
  METTLE_COMPILER_PHASE_WRITE_OUTPUT,
  METTLE_COMPILER_PHASE_DEBUG_INFO,
  METTLE_COMPILER_PHASE_CLEANUP,
  METTLE_COMPILER_PHASE_COUNT
} MettleCompilerPhase;

typedef struct {
  MettleCompilerPhase phase;
  const char *input_filename;
  const char *current_filename;
  const char *function_name;
  const char *pass_name;
  const char *last_action;
  size_t ir_instruction_index;
  const IRInstruction *ir_instruction;
  int fixpoint_iteration;
  int debug_compiler;
  int dump_ir;
  IRProgram *ir_program;
} MettleCompilerContext;

const char *mettle_compiler_phase_name(MettleCompilerPhase phase);

MettleCompilerContext *mettle_compiler_ctx(void);

void mettle_compiler_ctx_reset(void);

void mettle_compiler_ctx_set_phase(MettleCompilerPhase phase);

void mettle_compiler_ctx_set_input_filename(const char *filename);

void mettle_compiler_ctx_set_current_filename(const char *filename);

void mettle_compiler_ctx_set_function_name(const char *name);

void mettle_compiler_ctx_set_pass_name(const char *pass_name);

void mettle_compiler_ctx_set_fixpoint_iteration(int iteration);

void mettle_compiler_ctx_set_ir_instruction(size_t index,
                                            const IRInstruction *instruction);

void mettle_compiler_ctx_clear_ir_instruction(void);

void mettle_compiler_ctx_set_last_action(const char *action);

void mettle_compiler_ctx_set_ir_program(IRProgram *program);

void mettle_compiler_ctx_set_options(int debug_compiler, int dump_ir);

void mettle_compiler_ctx_write_report(FILE *output, const char *reason,
                                      const char *detail);

void mettle_compiler_ctx_write_snapshot(void);

#endif /* COMPILER_CONTEXT_H */
