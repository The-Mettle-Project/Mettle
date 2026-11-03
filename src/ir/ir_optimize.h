#ifndef IR_OPTIMIZE_H
#define IR_OPTIMIZE_H

#include "ir.h"

typedef struct IRGlobalIntConst {
  const char *name;
  long long value;
} IRGlobalIntConst;

typedef struct {
  int preserve_function_boundaries;
  int simd_report;
  int explain;
  const char *explain_focus_file;
  const IRGlobalIntConst *global_int_consts;
  size_t global_int_const_count;
  int whole_program;
  int target_neutral_only;
  int gpu_device_only;
} IROptimizeOptions;

int ir_optimize_safety_analysis(IRProgram *program, int preserve_boundaries);

int ir_optimize_program(IRProgram *program,
                        const IROptimizeOptions *options);

typedef struct {
  const char *name;
  const char *point;
  int (*run)(IRFunction *function, void *context, int *changed);
  void *context;
} IRLibraryPass;

void ir_library_passes_set(const IRLibraryPass *passes, size_t count);
int ir_optimizer_point_known(const char *point);

typedef int (*IRBuiltinLibraryRun)(const char *name, IRFunction *function,
                                   int *changed);
void ir_builtin_library_set(IRBuiltinLibraryRun run);

int ir_optimize_had_user_error(void);
void ir_optimize_note_user_error(void);
IRProgram *ir_optimize_get_program(void);
int ir_pass_name_is_skipped(const char *pass_name);
int ir_instruction_writes_temp(const IRInstruction *instruction);
void ir_note_parallel_loops_unverified(IRProgram *program);

int ir_explain_enabled(void);
void ir_explain_backend_function(const char *function_name,
                                 const char *filename, int ok,
                                 const char *detail, size_t instructions);
void ir_explain_backend_flush(void);
void ir_explain_backend_loop(const char *function_name, const char *filename,
                             size_t head_line, size_t tail_line, int depth,
                             int cycles_per_iter, const char *bottleneck,
                             int has_kernel, int estimated);
void ir_explain_backend_cost(const char *function_name, const char *filename,
                             int spills, int regs_used, int total_rthru,
                             long hot_cost, int vec_ops, int estimated_spans);
void ir_explain_target_flush(const char *target_name);
void ir_explain_set_output_path(const char *path);
void ir_explain_set_json(int enabled);
void ir_explain_set_filter(const char *selector);
void ir_explain_set_retain_remarks(int enabled);
size_t ir_explain_remark_count(void);
int ir_explain_remark_at(size_t index, const char **function_name,
                         const char **entity, size_t *line, int *positive,
                         const char **headline, const char **reason,
                         const char **fix, const char **verified, size_t *depth);
void ir_explain_ml_opt(const char *path);

void ir_note_simd_contracts_unverified(IRProgram *program);

#endif
