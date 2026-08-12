#ifndef CODE_GENERATOR_H
#define CODE_GENERATOR_H

#include "../debug/debug_info.h"
#include "../ir/ir.h"
#include "binary_emitter.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  DebugInfo *debug_info;
  int current_label_id;
  char *current_function_name;
  int generate_debug_info;
  int generate_stack_trace_support;
  int eliminate_unreachable_functions;
  int has_error;
  char *error_message;
  IRProgram *ir_program;
  size_t last_runtime_location_line;
  size_t last_runtime_location_column;
  BinaryEmitter *binary_emitter;
  int profile_runtime;
  int debug_hooks;
  char **profile_function_names;
  size_t profile_function_count;
  size_t profile_function_capacity;
} CodeGenerator;

// Function declarations
CodeGenerator *code_generator_create(void);
CodeGenerator *code_generator_create_with_debug(DebugInfo *debug_info);
void code_generator_destroy(CodeGenerator *generator);
void code_generator_set_ir_program(CodeGenerator *generator,
                                   IRProgram *ir_program);
int code_generator_generate_program(CodeGenerator *generator);
void code_generator_set_stack_trace_support(CodeGenerator *generator,
                                            int enable);
void code_generator_set_debug_sidecar_emission(CodeGenerator *generator,
                                               int enable);
void code_generator_set_eliminate_unreachable_functions(CodeGenerator *generator,
                                                        int enable);
void code_generator_set_profile_runtime(CodeGenerator *generator, int enable);
void code_generator_set_debug_hooks(CodeGenerator *generator, int enable);
int code_generator_register_profile_function(CodeGenerator *generator,
                                             const char *name,
                                             uint32_t *id_out);
BinaryEmitter *code_generator_get_binary_emitter(CodeGenerator *generator);

char *code_generator_generate_label(CodeGenerator *generator,
                                    const char *prefix);

void code_generator_add_runtime_function_mapping(CodeGenerator *generator,
                                                 const char *function_name,
                                                 const char *start_label,
                                                 const char *end_label,
                                                 size_t source_line,
                                                 size_t source_column,
                                                 const char *filename);
const char *code_generator_runtime_filename(CodeGenerator *generator,
                                              const char *node_filename);
void code_generator_record_runtime_trap_site(
    CodeGenerator *generator, const char *trap_pc_label, uint32_t kind,
    size_t line, size_t column, const char *filename,
    const char *message_template, const char *static_context);
int code_generator_is_floating_point_type(const MtlcType *type);

#endif // CODE_GENERATOR_H
