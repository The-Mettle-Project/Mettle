#ifndef CODE_GENERATOR_H
#define CODE_GENERATOR_H

#include "../debug/debug_info.h"
#include "../ir/ir.h"
#include "../parser/ast.h"
#include "../semantic/register_allocator.h"
#include "../semantic/symbol_table.h"
#include "../semantic/type_checker.h"
#include "binary_emitter.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  CODEGEN_BACKEND_TEXT_ASSEMBLY = 0,
  CODEGEN_BACKEND_BINARY_OBJECT,
} CodegenBackendMode;

typedef struct {
  SymbolTable *symbol_table;
  TypeChecker *type_checker;
  RegisterAllocator *register_allocator;
  DebugInfo *debug_info;
  FILE *output_file;
  char *output_buffer;
  size_t buffer_size;
  size_t buffer_capacity;
  int current_label_id;
  int current_stack_offset;
  int function_stack_size;
  char *current_function_name;
  char *global_variables_buffer;
  size_t global_variables_size;
  size_t global_variables_capacity;
  size_t current_assembly_line;
  char **break_label_stack;
  char **continue_label_stack;
  size_t control_flow_stack_size;
  size_t control_flow_stack_capacity;
  int generate_debug_info;
  int generate_stack_trace_support;
  int emit_asm_comments;
  int eliminate_unreachable_functions;
  int has_error;
  char *error_message;
  IRProgram *ir_program;
  char **extern_symbols;
  size_t extern_symbol_count;
  size_t extern_symbol_capacity;
  size_t last_runtime_location_line;
  size_t last_runtime_location_column;
  CodegenBackendMode backend_mode;
  BinaryEmitter *binary_emitter;
  /* Redundant-spill peephole for the IR backend. When
   * store_ir_destination spills rax into an IR temp's stack slot, it records
   * that slot here along with the emit sequence number that produced the
   * spilling instruction. load_ir_operand can then skip the matching reload
   * iff (a) it wants the same slot and (b) no instruction has been emitted
   * since (emit_seq unchanged). emit_seq is bumped by every code_generator_emit
   * call, so any intervening instruction invalidates the cache automatically.
   * rax_cached_temp_offset == 0 means "nothing cached" (temp offsets are
   * always > 0). */
  unsigned long long emit_seq;
  int rax_cached_temp_offset;
  unsigned long long rax_cached_emit_seq;
  /* Deferred dead-spill elimination (IR backend). store_ir_destination for a
   * temp records the pending "mov [rbp-N], rax" here rather than emitting it.
   * - If the next IR operand load wants this same temp and the temp is
   *   single-use, the store is dropped entirely (dead) and the reload skipped:
   *   the value is already in rax.
   * - Otherwise code_generator_emit flushes the pending store before emitting
   *   anything else, so the slot is always written before any reader or any
   *   instruction that could clobber rax.
   * pending_spill_offset == 0 means "nothing pending". flushing_pending guards
   * the flush's own emit call against re-entry. pending_spill_single_use is
   * the temp's use count == 1, decided at store time. */
  int pending_spill_offset;
  int pending_spill_single_use;
  int flushing_pending;
  /* Borrowed (not owned) pointer to the current IR function's IRTempUseMap,
   * set for the duration of that function's emission loop and NULL otherwise.
   * Typed void* because IRTempUseMap is private to code_generator_ir.c. Used
   * to decide whether a deferred temp spill is dead (single-use). */
  void *current_temp_use_map;
  /* Per-function FIFO of frame offsets for indirect-return hidden out-pointer
   * slots. Populated in instruction order during the IR pre-pass that
   * computes the function's stack size; consumed in the same order by
   * emit_ir_call. Each entry is the slot's rbp-relative byte offset
   * (positive: the slot lives at [rbp - offset]). NULL between functions. */
  int *indirect_return_slot_offsets;
  size_t indirect_return_slot_count;
  size_t indirect_return_slot_capacity;
  size_t indirect_return_slot_cursor;
  /* Set during emission of a function whose return type is INDIRECT. The
   * hidden out-pointer lives at [rbp - 8] (home slot 0). IR_OP_RETURN
   * dispatches to memcpy-through-pointer when this is non-zero. The byte
   * count is the function's return-type size. */
  int current_fn_returns_indirect;
  size_t current_fn_indirect_return_size;
  int profile_runtime;
  char **profile_function_names;
  size_t profile_function_count;
  size_t profile_function_capacity;
} CodeGenerator;

// Function declarations
CodeGenerator *code_generator_create(SymbolTable *symbol_table,
                                     TypeChecker *type_checker,
                                     RegisterAllocator *allocator);
CodeGenerator *code_generator_create_with_debug(SymbolTable *symbol_table,
                                                TypeChecker *type_checker,
                                                RegisterAllocator *allocator,
                                                DebugInfo *debug_info);
void code_generator_destroy(CodeGenerator *generator);
void code_generator_set_ir_program(CodeGenerator *generator,
                                   IRProgram *ir_program);
void code_generator_set_backend_mode(CodeGenerator *generator,
                                     CodegenBackendMode mode);
int code_generator_generate_program(CodeGenerator *generator, ASTNode *program);
void code_generator_generate_function(CodeGenerator *generator,
                                      ASTNode *function);
void code_generator_generate_statement(CodeGenerator *generator,
                                       ASTNode *statement);
void code_generator_generate_expression(CodeGenerator *generator,
                                        ASTNode *expression);
void code_generator_set_emit_asm_comments(CodeGenerator *generator, int enable);
void code_generator_set_stack_trace_support(CodeGenerator *generator,
                                            int enable);
void code_generator_set_debug_sidecar_emission(CodeGenerator *generator,
                                               int enable);
void code_generator_set_eliminate_unreachable_functions(CodeGenerator *generator,
                                                        int enable);
void code_generator_set_profile_runtime(CodeGenerator *generator, int enable);
int code_generator_register_profile_function(CodeGenerator *generator,
                                             const char *name,
                                             uint32_t *id_out);
void code_generator_emit(CodeGenerator *generator, const char *format, ...);
void code_generator_flush_pending_spill(CodeGenerator *generator);
char *code_generator_get_output(CodeGenerator *generator);
BinaryEmitter *code_generator_get_binary_emitter(CodeGenerator *generator);

// Stack frame management
void code_generator_function_prologue(CodeGenerator *generator,
                                      const char *function_name,
                                      int stack_size);
void code_generator_function_epilogue(CodeGenerator *generator,
                                      Type *return_type);
char *code_generator_generate_label(CodeGenerator *generator,
                                    const char *prefix);

// Assembly helpers
void code_generator_emit_text_section(CodeGenerator *generator);

// Variable declaration helpers
void code_generator_generate_global_variable(CodeGenerator *generator,
                                             ASTNode *var_declaration);
void code_generator_generate_local_variable(CodeGenerator *generator,
                                            ASTNode *var_declaration);
void code_generator_generate_variable_initialization(CodeGenerator *generator,
                                                     Symbol *symbol,
                                                     ASTNode *initializer);
void code_generator_generate_variable_zero_initialization(
    CodeGenerator *generator, Symbol *symbol);
int code_generator_calculate_variable_size(CodeGenerator *generator,
                                           const char *type_name);

// Debug info integration
void code_generator_add_debug_symbol(CodeGenerator *generator, const char *name,
                                     DebugSymbolType type,
                                     const char *type_name, size_t line,
                                     size_t column);
void code_generator_add_line_mapping(CodeGenerator *generator,
                                     size_t source_line, size_t source_column,
                                     const char *filename);
void code_generator_emit_debug_label(CodeGenerator *generator,
                                     size_t source_line);
void code_generator_emit_runtime_location_marker(CodeGenerator *generator,
                                                 size_t source_line,
                                                 size_t source_column,
                                                 const char *filename);
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
void code_generator_emit_runtime_debug_tables(CodeGenerator *generator);
int code_generator_allocate_stack_space(CodeGenerator *generator, int size,
                                        int alignment);

// Function call helpers
void code_generator_generate_function_call(CodeGenerator *generator,
                                           ASTNode *call_expression);
void code_generator_generate_parameter_passing(CodeGenerator *generator,
                                               ASTNode **arguments,
                                               size_t argument_count,
                                               Symbol *func_symbol);
void code_generator_generate_parameter(CodeGenerator *generator,
                                       ASTNode *argument, int param_index,
                                       Type *param_type, Symbol *func_symbol);
void code_generator_align_stack_for_call(CodeGenerator *generator);
void code_generator_cleanup_stack_after_call(CodeGenerator *generator,
                                             ASTNode **arguments,
                                             size_t argument_count);
void code_generator_handle_return_value(CodeGenerator *generator,
                                        Type *return_type);
Type *code_generator_infer_expression_type(CodeGenerator *generator,
                                           ASTNode *expression);
int code_generator_is_floating_point_type(Type *type);
const char *code_generator_get_register_name(x86Register reg);
const char *code_generator_get_subregister_name(x86Register reg,
                                                int width_bits);

// Expression and assignment helpers
void code_generator_generate_binary_operation(CodeGenerator *generator,
                                              ASTNode *left, const char *op,
                                              ASTNode *right);
void code_generator_generate_unary_operation(CodeGenerator *generator,
                                             const char *op, ASTNode *operand);
void code_generator_generate_assignment_statement(CodeGenerator *generator,
                                                  ASTNode *assignment);
void code_generator_load_variable(CodeGenerator *generator,
                                  const char *variable_name);
void code_generator_store_variable(CodeGenerator *generator,
                                   const char *variable_name,
                                   const char *source_reg);
void code_generator_load_string_literal(CodeGenerator *generator,
                                        const char *string_value);
void code_generator_load_string_literal_as_cstring(CodeGenerator *generator,
                                                   const char *string_value);
const char *code_generator_get_arithmetic_instruction(const char *op,
                                                      int is_float);

// Struct and method helpers
void code_generator_generate_struct_declaration(CodeGenerator *generator,
                                                ASTNode *struct_declaration);
void code_generator_generate_method_call(CodeGenerator *generator,
                                         ASTNode *method_call, ASTNode *object);
void code_generator_generate_member_access(CodeGenerator *generator,
                                           ASTNode *member_access);
void code_generator_generate_array_index(CodeGenerator *generator,
                                         ASTNode *index_expression);
void code_generator_calculate_struct_layout(CodeGenerator *generator,
                                            Type *struct_type);
int code_generator_get_field_offset(CodeGenerator *generator, Type *struct_type,
                                    const char *field_name);
int code_generator_calculate_struct_alignment(int field_size);

// Inline assembly helpers
void code_generator_generate_inline_assembly(CodeGenerator *generator,
                                             ASTNode *inline_asm);
void code_generator_preserve_registers_for_inline_asm(CodeGenerator *generator);
void code_generator_restore_registers_after_inline_asm(
    CodeGenerator *generator);

#endif // CODE_GENERATOR_H
