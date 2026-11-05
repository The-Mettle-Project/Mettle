#ifndef IR_LOWERING_INTERNAL_H
#define IR_LOWERING_INTERNAL_H

#include "ir_lowering.h"
#include "../common.h"
#include "compiler/compiler_context.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct IRDeferScope IRDeferScope;

typedef struct {
  char *break_label;
  char *continue_label;
  char *user_label;
  const char *fallthrough_label;
  IRDeferScope *defers;
} IRControlFrame;

typedef struct {
  int scope_id;
  const char *result_name;
  Type *result_type;
  const char *end_label;
  IRDeferScope *defers;
} IRValueFrame;

typedef struct {
  const char *name;
  const char *ir_name;
  const char *type_text;
  int depth;
  int active;
  int owns_ir_name;
} IRLocalBinding;

typedef struct {
  int next_temp_id;
  int next_label_id;
  IRControlFrame *control_stack;
  size_t control_count;
  size_t control_capacity;
  char *error_message;
  TypeChecker *type_checker;
  SymbolTable *symbol_table;
  const char *current_expansion_note;
  int emit_runtime_checks;
  int emit_safety_checks;
  int freeing_column_data;
  int emit_refinement_checks;
  int emit_task_checks;
  int emitted_task_check;
  int emit_overflow_checks;
  int assume_no_signed_overflow;
  const char *refine_binding_name;
  IROperand refine_binding_value;
  int refine_binding_active;
  const char *current_return_type_name;
  const char *current_function_name;
  int next_simd_request_id;
  int current_function_simd_default;
  IRProgram *program;
  IRLocalBinding *local_bindings;
  size_t local_binding_count;
  size_t local_binding_capacity;
  int local_scope_depth;
  int local_rename_serial;
  ASTNode **block_statements;
  size_t block_statement_count;
  size_t block_statement_index;
  int address_width_depth;
  IRDeferScope *current_defers;
  IRValueFrame *value_frames;
  size_t value_frame_count;
  size_t value_frame_capacity;
  int pin_identifier_reads;
} IRLoweringContext;

int ir_lower_index_expression(IRLoweringContext *context, IRFunction *function,
                              ASTNode *expression, IROperand *out_value);

typedef struct {
  struct {
    ASTNode *node;
    int is_err;
    char *capture_call_name;
    char **capture_arg_temps;
    size_t capture_arg_count;
    char *release_string;
  } *entries;
  size_t count;
  size_t capacity;
} IRDeferStack;

struct IRDeferScope {
  IRDeferStack stack;
  struct IRDeferScope *parent;
};

extern int g_ir_lowering_explain;

int ir_emit_jump_instruction(IRLoweringContext *context,
                                    IRFunction *function, const char *label,
                                    SourceLocation location);

int ir_emit_label_instruction(IRLoweringContext *context,
                                     IRFunction *function, const char *label,
                                     SourceLocation location);

int ir_emit_simd_marker(IRLoweringContext *context, IRFunction *function,
                               char which, int id, int mode,
                               SourceLocation location);

int ir_emit_parallel_marker(IRLoweringContext *context,
                            IRFunction *function, const char *loop_label,
                            SourceLocation location);
int ir_emit_unroll_marker(IRLoweringContext *context, IRFunction *function,
                          int factor, SourceLocation location);

int ir_type_is_cstring(Type *type);
int ir_type_is_rawptr(Type *type);

int ir_expression_is_string(IRLoweringContext *context,
                                   ASTNode *expression);

int ir_should_coerce_string_to_cstring(IRLoweringContext *context,
                                              Type *target_type,
                                              ASTNode *value_expression);

int ir_coerce_string_operand_to_cstring(IRLoweringContext *context,
                                               IRFunction *function,
                                               IROperand *value,
                                               SourceLocation location);

int ir_should_build_slice_from_array(Type *target_type,
                                     ASTNode *value_expression);
int ir_build_slice_operand_from_array(IRLoweringContext *context,
                                      IRFunction *function, IROperand *value,
                                      Type *array_type, Type *slice_type,
                                      SourceLocation location);

int ir_should_decay_array_to_address(Type *target_type,
                                     ASTNode *value_expression);

int ir_decay_array_operand_to_address(IRLoweringContext *context,
                                      IRFunction *function, IROperand *value,
                                      SourceLocation location);

int ir_lower_statement_or_expression(IRLoweringContext *context,
                                            IRFunction *function,
                                            ASTNode *node);

void ir_local_scope_enter(IRLoweringContext *context);

void ir_local_scope_leave(IRLoweringContext *context);

void ir_local_bindings_reset(IRLoweringContext *context);

const char *ir_local_bind(IRLoweringContext *context, const char *name,
                          const char *type_text);

const char *ir_local_bind_parameter(IRLoweringContext *context,
                                    const char *name, const char *type_text);

const char *ir_local_ir_name(IRLoweringContext *context, const char *name);

const IRLocalBinding *ir_local_binding_find(IRLoweringContext *context,
                                            const char *name);

int ir_emit_local_declaration(IRLoweringContext *context,
                                     IRFunction *function,
                                     const char *name, const char *type_name,
                                     SourceLocation location);

IROperand ir_clone_operand_local(const IROperand *operand);

int ir_try_emit_aggregate_symbol_memcpy(
    IRLoweringContext *context, IRFunction *function, const char *dest_name,
    const IROperand *value, Type *dest_type, SourceLocation location);

int ir_emit_aggregate_literal_copy(IRLoweringContext *context,
                                   IRFunction *function,
                                   const IROperand *dest_address,
                                   ASTNode *literal_node, Type *dest_type,
                                   SourceLocation location);

int ir_emit_aggregate_literal_copy_to_symbol(IRLoweringContext *context,
                                             IRFunction *function,
                                             const char *dest_name,
                                             ASTNode *literal_node,
                                             Type *dest_type,
                                             SourceLocation location);

int ir_emit_zero_fill_local(IRLoweringContext *context, IRFunction *function,
                            const char *local_name, Type *type,
                            SourceLocation location);

int ir_try_emit_aggregate_address_memcpy(IRLoweringContext *context,
                                         IRFunction *function,
                                         const IROperand *dest_addr,
                                         const IROperand *value, Type *dest_type,
                                         SourceLocation location);

Type *ir_resolve_named_type(IRLoweringContext *context,
                                   const char *name);

Type *ir_lookup_symbol_type(IRLoweringContext *context,
                                   const char *name);

int ir_emit_symbol_assignment(IRLoweringContext *context,
                                     IRFunction *function,
                                     const char *name,
                                     const IROperand *value,
                                     SourceLocation location);

int ir_emit_address_with_offset(IRLoweringContext *context,
                                       IRFunction *function,
                                       const IROperand *base_address,
                                       size_t offset,
                                       SourceLocation location,
                                       IROperand *out_address);

int ir_emit_switch_range_dispatch(IRLoweringContext *context,
                                         IRFunction *function,
                                         const IROperand *switch_value,
                                         ASTNode *lo_node, ASTNode *hi_node,
                                         const char *case_label,
                                         SourceLocation loc);

int ir_lower_switch_statement(IRLoweringContext *context,
                                     IRFunction *function, ASTNode *statement,
                                     IRDeferScope *defers);

int ir_lower_match_statement(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *statement,
                                    IRDeferScope *defers);

int ir_lower_value_scope(IRLoweringContext *context, IRFunction *function,
                         ASTNode *expression, IROperand *out_value);

int ir_lower_yield_statement(IRLoweringContext *context, IRFunction *function,
                             ASTNode *statement, IRDeferScope *defers);

int ir_store_named_local_value(IRLoweringContext *context, IRFunction *function,
                               const char *local_name, Type *local_type,
                               IROperand *value, SourceLocation location);

int ir_emit_tagged_enum_construct(IRLoweringContext *context,
                                         IRFunction *function,
                                         Symbol *constructor_symbol,
                                         ASTNode *payload_arg,
                                         SourceLocation location,
                                         IROperand *out_value);

int ir_lower_tagged_enum_constructor_call(IRLoweringContext *context,
                                                 IRFunction *function,
                                                 ASTNode *expression,
                                                 Symbol *constructor_symbol,
                                                 IROperand *out_value);

int ir_emit_deferred_calls(IRLoweringContext *context,
                                  IRFunction *function,
                                  const IRDeferStack *stack);

int ir_emit_deferred_calls_non_err(IRLoweringContext *context,
                                          IRFunction *function,
                                          const IRDeferStack *stack);

int ir_emit_deferred_scopes(IRLoweringContext *context,
                                   IRFunction *function,
                                   const IRDeferScope *scope);

int ir_emit_deferred_scopes_non_err(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferScope *scope);

int ir_emit_return_with_defers(IRLoweringContext *context,
                                      IRFunction *function,
                                      IRDeferScope *defers, IROperand *value,
                                      SourceLocation location);

void ir_set_error(IRLoweringContext *context, const char *format, ...);

char *ir_new_temp_name(IRLoweringContext *context);

char *ir_new_label_name(IRLoweringContext *context, const char *prefix);

int ir_emit(IRLoweringContext *context, IRFunction *function,
                   const IRInstruction *instruction);

extern int g_ir_lowering_refinement_checks;
extern int g_ir_lowering_task_checks;
extern int g_ir_lowering_live;
int ir_lower_live_site(IRLoweringContext *context, IRFunction *function,
                       SourceLocation location);
int ir_lower_live_program(IRLoweringContext *context, IRProgram *program);
void ir_lower_live_forget_sources(void);
void ir_lower_live_forget_heap_types(void);
int ir_lower_live_heap_type(const Type *type);
int ir_lower_live_note_own_allocator(const Type *type);
int ir_lower_live_new(IRLoweringContext *context, IRFunction *function,
                      const IROperand *pointer, const IROperand *bytes,
                      const Type *type, const IROperand *count,
                      SourceLocation location);
void ir_lower_live_register_symbols(IRLoweringContext *context,
                                    IRProgram *program);
int ir_lower_live_marks_statement(const IRFunction *function,
                                  int statement_type);
int ir_lower_live_mark(IRLoweringContext *context, IRFunction *function,
                       int begin, SourceLocation location);
extern int g_ir_lowering_overflow_checks;
extern int g_ir_lowering_assume_no_signed_overflow;
int ir_emit_refinement_predicate(IRLoweringContext *context,
                                 IRFunction *function, SourceLocation location,
                                 const IROperand *value, const Type *refined,
                                 struct ASTNode *predicate,
                                 const char *binding);
int ir_emit_refinement_check(IRLoweringContext *context, IRFunction *function,
                             SourceLocation location, const IROperand *value,
                             const Type *refined);
int ir_emit_runtime_trap_ex(IRLoweringContext *context,
                                   IRFunction *function,
                                   SourceLocation location, uint32_t kind,
                                   const char *message, const IROperand *arg0,
                                   const IROperand *arg1);

int ir_emit_overflow_check_narrow(IRLoweringContext *context,
                                  IRFunction *function, SourceLocation location,
                                  const IROperand *wide,
                                  const IROperand *narrow, const char *op,
                                  const char *type_name);
int ir_emit_overflow_check_wide(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *result, const IROperand *left,
                                const IROperand *right, const char *op,
                                const char *type_name);
extern size_t g_ir_overflow_emitted;
extern size_t g_ir_overflow_proved;

int ir_emit_null_check(IRLoweringContext *context, IRFunction *function,
                              SourceLocation location, const IROperand *value);

int ir_small_float_local(IRLoweringContext *context, IRFunction *function,
                         const char *source_name, const char *ir_name,
                         Type *type);
int ir_emit_small_float_home_store(IRLoweringContext *context,
                                   IRFunction *function, const char *ir_name,
                                   Type *type, const IROperand *value,
                                   SourceLocation location);
int ir_emit_small_float_home_load(IRLoweringContext *context,
                                  IRFunction *function, const char *ir_name,
                                  Type *type, SourceLocation location,
                                  IROperand *out_value);
int ir_emit_load_word(IRLoweringContext *context, IRFunction *function,
                      const IROperand *base_address, size_t offset,
                      SourceLocation location, IROperand *out_value);
int ir_emit_store_word(IRLoweringContext *context, IRFunction *function,
                       const IROperand *base_address, size_t offset,
                       const IROperand *value, SourceLocation location);
int ir_emit_binary_temp(IRLoweringContext *context, IRFunction *function,
                        const char *operator_text, const IROperand *lhs,
                        const IROperand *rhs, SourceLocation location,
                        IROperand *out_value);
int ir_emit_slice_bounds_check(IRLoweringContext *context, IRFunction *function,
                               SourceLocation location,
                               const IROperand *slice_address,
                               const IROperand *index);

int ir_emit_bounds_check(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *index, size_t array_size);

int ir_emit_safety_check(IRLoweringContext *context, IRFunction *function,
                         SourceLocation location, const IROperand *base,
                         const IROperand *offset, long long access_size,
                         long long extent, int access_kind, const char *what);

int ir_push_labeled_control_frame(IRLoweringContext *context,
                                         const char *break_label,
                                         const char *continue_label,
                                         const char *user_label,
                                         IRDeferScope *defers);

int ir_push_control_frame(IRLoweringContext *context,
                                 const char *break_label,
                                 const char *continue_label,
                                 IRDeferScope *defers);

void ir_pop_control_frame(IRLoweringContext *context);

const char *ir_current_break_label(IRLoweringContext *context);

const IRControlFrame *ir_current_fallthrough_frame(IRLoweringContext *context);

void ir_set_fallthrough_label(IRLoweringContext *context, const char *label);

const char *ir_current_continue_label(IRLoweringContext *context);

const char *ir_find_labeled_break(IRLoweringContext *context,
                                         const char *user_label);

const char *ir_find_labeled_continue(IRLoweringContext *context,
                                            const char *user_label);

const IRControlFrame *ir_break_target_frame(IRLoweringContext *context,
                                            const char *user_label);

const IRControlFrame *ir_continue_target_frame(IRLoweringContext *context,
                                               const char *user_label);

int ir_emit_defers_until_scope(IRLoweringContext *context,
                               IRFunction *function, const IRDeferScope *from,
                               const IRDeferScope *stop);

int ir_string_temp_is_owned(IRLoweringContext *context, ASTNode *node);
int ir_emit_string_free(IRLoweringContext *context, IRFunction *function,
                        const IROperand *value, SourceLocation location);
int ir_defer_stack_push_string_release(IRLoweringContext *context,
                                       IRDeferStack *stack, ASTNode *node,
                                       const char *local_name);
int ir_defer_stack_push(IRLoweringContext *context, IRDeferStack *stack,
                               ASTNode *node, int is_err);

void ir_defer_stack_free(IRDeferStack *stack);

int ir_defer_capture_call(IRLoweringContext *context,
                                 IRFunction *function, ASTNode *defer_node,
                                 char **out_call_name, char ***out_temps,
                                 size_t *out_count);

int ir_emit_deferred_calls_filtered(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferStack *stack,
                                           int include_err);

int ir_lower_deferred_statement(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *statement);

int ir_expression_is_floating(IRLoweringContext *context,
                                     ASTNode *expression);

int ir_type_is_float64(Type *type);

int ir_type_float_bits(Type *type);

int ir_named_type_float_bits(IRLoweringContext *context,
                                    const char *type_name);

void ir_operand_apply_float_bits(IROperand *operand, int bits);

int ir_symbol_float_bits(IRLoweringContext *context, const char *name);

int ir_local_declared_float_bits(IRLoweringContext *context,
                                        const IRFunction *function,
                                        const char *name);

void ir_assign_apply_float_bits(IRInstruction *instruction,
                                       IROperand *value, int bits);

void ir_access_apply_alias_class(IRInstruction *access, Type *accessed_type);
void ir_load_apply_float_type(IRInstruction *load, Type *loaded_type);

void ir_load_apply_unsigned(IRInstruction *load, Type *loaded_type);

int ir_expression_float_bits(IRLoweringContext *context,
                                    ASTNode *expression);

int ir_binary_operator_is_comparison(const char *op);

int ir_binary_expression_operation_float_bits(IRLoweringContext *context,
                                                    ASTNode *expression,
                                                    BinaryExpression *binary);

int ir_normalize_to_bool(IRLoweringContext *context, IRFunction *function,
                         Type *target_type, ASTNode *value_expression,
                         IROperand *value, SourceLocation location);

int ir_type_storage_size(Type *type);

int ir_type_array_element_stride(Type *element_type);

int ir_type_is_unsigned_integer(const Type *type);
const char *ir_narrow_integer_result_type(Type *type, const char *op);
int ir_narrow_integer_shift_bits(Type *type);
int ir_unary_constant_fits(const char *type_name, const char *op,
                           long long value);
int ir_type_is_pointer(Type *type);

int ir_emit_binary_instruction(IRLoweringContext *context,
                                      IRFunction *function,
                                      SourceLocation location, const char *op,
                                      IROperand dest, IROperand lhs,
                                      IROperand rhs);

int ir_emit_scaled_index_offset(IRLoweringContext *context,
                                       IRFunction *function,
                                       SourceLocation location,
                                       const IROperand *index, int stride,
                                       IROperand *out_offset);

int ir_try_lower_pointer_arithmetic(IRLoweringContext *context,
                                           IRFunction *function,
                                           BinaryExpression *binary,
                                           SourceLocation location,
                                           IROperand *out_value);

int ir_make_temp_operand(IRLoweringContext *context,
                                IROperand *out_temp);

int ir_emit_condition_false_branch(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *expression,
                                          const char *false_label);

int ir_emit_condition_true_branch(IRLoweringContext *context,
                                         IRFunction *function,
                                         ASTNode *expression,
                                         const char *true_label);

int ir_lower_call_expression(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *expression,
                                    IROperand *out_value);

int ir_lower_make_element_ref(IRLoweringContext *context, IRFunction *function,
                              Type *ref_type, const IROperand *base,
                              const IROperand *index, const IROperand *count,
                              SourceLocation location, IROperand *out_value);
int ir_lower_address_of_element(IRLoweringContext *context, IRFunction *function,
                                ASTNode *operand, Type *ref_type,
                                SourceLocation location, IROperand *out_value);
int ir_lower_element_ref_arithmetic(IRLoweringContext *context,
                                    IRFunction *function, BinaryExpression *binary,
                                    SourceLocation location,
                                    IROperand *out_value);
int ir_lower_element_ref_compare(IRLoweringContext *context, IRFunction *function,
                                 BinaryExpression *binary, SourceLocation location,
                                 IROperand *out_value);
Type *ir_infer_expression_type(IRLoweringContext *context,
                                      ASTNode *expression);

int ir_emit_address_of_symbol(IRLoweringContext *context,
                                     IRFunction *function, const char *name,
                                     SourceLocation location,
                                     IROperand *out_address);

int ir_lower_lvalue_address(IRLoweringContext *context,
                                   IRFunction *function, ASTNode *expression,
                                   IROperand *out_address, Type **out_type);

void ir_layout_refuse(IRLoweringContext *context, SourceLocation location,
                      const char *message);
int ir_expression_is_column_element(IRLoweringContext *context,
                                    ASTNode *expression);
int ir_lower_column_element_load(IRLoweringContext *context,
                                 IRFunction *function, ASTNode *expression,
                                 IROperand *out_value);
int ir_lower_column_element_store(IRLoweringContext *context,
                                  IRFunction *function, ASTNode *target,
                                  ASTNode *value_node, SourceLocation location);
int ir_emit_column_bytes(IRLoweringContext *context, IRFunction *function,
                         Type *element, const IROperand *count,
                         SourceLocation location, IROperand *out);
int ir_emit_layout_allocation(IRLoweringContext *context, IRFunction *function,
                              const Type *element, const IROperand *bytes,
                              const IROperand *dest, SourceLocation location);
int ir_expression_is_packed_field(IRLoweringContext *context,
                                  ASTNode *expression);
int ir_lower_packed_load(IRLoweringContext *context, IRFunction *function,
                         ASTNode *expression, IROperand *out_value);
int ir_lower_packed_store(IRLoweringContext *context, IRFunction *function,
                          ASTNode *target, const IROperand *value,
                          SourceLocation location);

int ir_lower_expression(IRLoweringContext *context, IRFunction *function,
                               ASTNode *expression, IROperand *out_value);

int ir_lower_tile_expression(IRLoweringContext *context, IRFunction *function,
                             ASTNode *expression, const IROperand *dest,
                             IROperand *out);

int ir_lower_tile_store_into(IRLoweringContext *context, IRFunction *function,
                             Type *type, const IROperand *dest, ASTNode *value,
                             SourceLocation location);

int ir_lower_tile_declaration(IRLoweringContext *context, IRFunction *function,
                              ASTNode *statement, VarDeclaration *declaration,
                              Type *type);

int ir_lower_tile_call(IRLoweringContext *context, IRFunction *function,
                       ASTNode *expression, IROperand *out_value);

int ir_call_is_tile_statement(const CallExpression *call);

int ir_lower_statement_with_defers(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *statement,
                                          IRDeferScope *defers);

IRFunction *ir_lower_function(IRLoweringContext *context,
                                     ASTNode *declaration);

void ir_mark_branches_uniform(IRFunction *function, size_t from);
void ir_mark_branch_conditions_uniform(IRFunction *function, size_t from);
void ir_mark_calls_divergent(IRFunction *function, size_t from, int level);

int ir_lower_static_view_offset(IRLoweringContext *context,
                                IRFunction *function, Type *view_type,
                                IROperand *row, IROperand *column,
                                SourceLocation location, IROperand *out_offset);

#endif
