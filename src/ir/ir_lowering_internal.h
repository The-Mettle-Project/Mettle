#ifndef IR_LOWERING_INTERNAL_H
#define IR_LOWERING_INTERNAL_H

// Shared internals for the AST->IR lowering pass, split across ir_lower*.c
// modules. The public entry point ir_lower_program lives in ir.h; this
// header exposes the cross-module lowering context, helper structs, and
// static-helper prototypes.

#include "ir.h"
#include "../common.h"
#include "compiler/compiler_context.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *break_label;
  char *continue_label;
  char *user_label; // optional source-level label for labeled break/continue
} IRControlFrame;

typedef struct {
  int next_temp_id;
  int next_label_id;
  IRControlFrame *control_stack;
  size_t control_count;
  size_t control_capacity;
  char *error_message;
  TypeChecker *type_checker;
  SymbolTable *symbol_table;
  int emit_runtime_checks;
  /* Declared return type name of the function currently being lowered. Used
   * to give a width-less float literal in `return <lit>;` the correct
   * single/double precision (literals always infer to float64 otherwise). */
  const char *current_return_type_name;
  const char *current_function_name;
  /* Monotonic id handed to each `@simd` loop's begin/end marker pair so the
   * release-stage contract verifier can match them. */
  int next_simd_request_id;
  /* Default SimdAttr from a function-level `@simd` decorator. A counted loop in
   * the body with no `@simd` of its own inherits this mode. */
  int current_function_simd_default;
} IRLoweringContext;

typedef struct {
  struct {
    ASTNode *node;
    int is_err;
    /* By-value capture for `defer fn(args...)`: when capture_call_name is
     * non-NULL, the argument values were snapshotted into the named temp
     * locals at the defer point, and the deferred call is replayed against
     * those temps instead of re-evaluating the original argument expressions
     * (which would observe their later, scope-exit values). */
    char *capture_call_name;
    char **capture_arg_temps;
    size_t capture_arg_count;
  } *entries;
  size_t count;
  size_t capacity;
} IRDeferStack;

typedef struct IRDeferScope {
  IRDeferStack stack;
  struct IRDeferScope *parent;
} IRDeferScope;

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

int ir_type_is_cstring(Type *type);

int ir_expression_is_string(IRLoweringContext *context,
                                   ASTNode *expression);

int ir_should_coerce_string_to_cstring(IRLoweringContext *context,
                                              Type *target_type,
                                              ASTNode *value_expression);

int ir_coerce_string_operand_to_cstring(IRLoweringContext *context,
                                               IRFunction *function,
                                               IROperand *value,
                                               SourceLocation location);

int ir_lower_statement_or_expression(IRLoweringContext *context,
                                            IRFunction *function,
                                            ASTNode *node);

int ir_emit_local_declaration(IRLoweringContext *context,
                                     IRFunction *function,
                                     const char *name, const char *type_name,
                                     SourceLocation location);

IROperand ir_clone_operand_local(const IROperand *operand);

int ir_try_emit_aggregate_symbol_memcpy(
    IRLoweringContext *context, IRFunction *function, const char *dest_name,
    const IROperand *value, Type *dest_type, SourceLocation location);

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
                                     IRFunction *function, ASTNode *statement);

int ir_lower_match_statement(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *statement,
                                    IRDeferScope *defers);

int ir_lower_match_expression(IRLoweringContext *context,
                                     IRFunction *function,
                                     ASTNode *expression,
                                     IROperand *out_value);

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

int ir_emit_runtime_trap_ex(IRLoweringContext *context,
                                   IRFunction *function,
                                   SourceLocation location, uint32_t kind,
                                   const char *message, const IROperand *arg0,
                                   const IROperand *arg1);

int ir_emit_null_check(IRLoweringContext *context, IRFunction *function,
                              SourceLocation location, const IROperand *value);

int ir_emit_bounds_check(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *index, size_t array_size);

int ir_push_labeled_control_frame(IRLoweringContext *context,
                                         const char *break_label,
                                         const char *continue_label,
                                         const char *user_label);

int ir_push_control_frame(IRLoweringContext *context,
                                 const char *break_label,
                                 const char *continue_label);

void ir_pop_control_frame(IRLoweringContext *context);

const char *ir_current_break_label(IRLoweringContext *context);

const char *ir_current_continue_label(IRLoweringContext *context);

const char *ir_find_labeled_break(IRLoweringContext *context,
                                         const char *user_label);

const char *ir_find_labeled_continue(IRLoweringContext *context,
                                            const char *user_label);

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

void ir_load_apply_float_type(IRInstruction *load, Type *loaded_type);

void ir_load_apply_unsigned(IRInstruction *load, Type *loaded_type);

int ir_expression_float_bits(IRLoweringContext *context,
                                    ASTNode *expression);

int ir_binary_operator_is_comparison(const char *op);

int ir_binary_expression_operation_float_bits(IRLoweringContext *context,
                                                    ASTNode *expression,
                                                    BinaryExpression *binary);

int ir_type_storage_size(Type *type);

int ir_type_array_element_stride(Type *element_type);

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

Type *ir_infer_expression_type(IRLoweringContext *context,
                                      ASTNode *expression);

int ir_emit_address_of_symbol(IRLoweringContext *context,
                                     IRFunction *function, const char *name,
                                     SourceLocation location,
                                     IROperand *out_address);

int ir_lower_lvalue_address(IRLoweringContext *context,
                                   IRFunction *function, ASTNode *expression,
                                   IROperand *out_address, Type **out_type);

int ir_lower_expression(IRLoweringContext *context, IRFunction *function,
                               ASTNode *expression, IROperand *out_value);

int ir_lower_statement_with_defers(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *statement,
                                          IRDeferScope *defers);

IRFunction *ir_lower_function(IRLoweringContext *context,
                                     ASTNode *declaration);

#endif // IR_LOWERING_INTERNAL_H
