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

static int ir_lower_expression(IRLoweringContext *context, IRFunction *function,
                               ASTNode *expression, IROperand *out_value);
static int ir_lower_statement_with_defers(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *statement,
                                          IRDeferScope *defers);
static void ir_set_error(IRLoweringContext *context, const char *format, ...);
static char *ir_new_label_name(IRLoweringContext *context, const char *prefix);
static int ir_emit(IRLoweringContext *context, IRFunction *function,
                   const IRInstruction *instruction);
static int ir_emit_runtime_trap_ex(IRLoweringContext *context,
                                   IRFunction *function,
                                   SourceLocation location, uint32_t kind,
                                   const char *message, const IROperand *arg0,
                                   const IROperand *arg1);
static int ir_emit_null_check(IRLoweringContext *context, IRFunction *function,
                              SourceLocation location, const IROperand *value);
static int ir_emit_bounds_check(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *index, size_t array_size);
static int ir_push_control_frame(IRLoweringContext *context,
                                 const char *break_label,
                                 const char *continue_label);
static int ir_push_labeled_control_frame(IRLoweringContext *context,
                                         const char *break_label,
                                         const char *continue_label,
                                         const char *user_label);
static void ir_pop_control_frame(IRLoweringContext *context);
static int ir_emit_deferred_calls_filtered(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferStack *stack,
                                           int include_err);
static int ir_lower_deferred_statement(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *statement);
static int ir_emit_deferred_calls(IRLoweringContext *context,
                                  IRFunction *function,
                                  const IRDeferStack *stack);
static int ir_emit_return_with_defers(IRLoweringContext *context,
                                      IRFunction *function,
                                      IRDeferScope *defers, IROperand *value,
                                      SourceLocation location);
static int ir_coerce_string_operand_to_cstring(IRLoweringContext *context,
                                               IRFunction *function,
                                               IROperand *value,
                                               SourceLocation location);
static int ir_named_type_float_bits(IRLoweringContext *context,
                                    const char *type_name);
static void ir_assign_apply_float_bits(IRInstruction *instruction,
                                       IROperand *value, int bits);
static int ir_emit_jump_instruction(IRLoweringContext *context,
                                    IRFunction *function, const char *label,
                                    SourceLocation location);
static int ir_emit_label_instruction(IRLoweringContext *context,
                                     IRFunction *function, const char *label,
                                     SourceLocation location);
static int ir_make_temp_operand(IRLoweringContext *context,
                                IROperand *out_operand);
static int ir_lower_statement_or_expression(IRLoweringContext *context,
                                            IRFunction *function,
                                            ASTNode *node);
static int ir_emit_condition_false_branch(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *expression,
                                          const char *false_label);
static int ir_emit_condition_true_branch(IRLoweringContext *context,
                                         IRFunction *function,
                                         ASTNode *expression,
                                         const char *true_label);
static Type *ir_infer_expression_type(IRLoweringContext *context,
                                      ASTNode *expression);
static int ir_emit_address_of_symbol(IRLoweringContext *context,
                                     IRFunction *function, const char *name,
                                     SourceLocation location,
                                     IROperand *out_address);
static int ir_type_storage_size(Type *type);
static int ir_type_array_element_stride(Type *element_type);
static int ir_lower_switch_statement(IRLoweringContext *context,
                                     IRFunction *function, ASTNode *statement);
static int ir_lower_match_statement(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *statement,
                                    IRDeferScope *defers);
static int ir_lower_match_expression(IRLoweringContext *context,
                                     IRFunction *function,
                                     ASTNode *expression,
                                     IROperand *out_value);
static int ir_lower_tagged_enum_constructor_call(IRLoweringContext *context,
                                                 IRFunction *function,
                                                 ASTNode *expression,
                                                 Symbol *constructor_symbol,
                                                 IROperand *out_value);
static int ir_emit_tagged_enum_construct(IRLoweringContext *context,
                                         IRFunction *function,
                                         Symbol *constructor_symbol,
                                         ASTNode *payload_arg,
                                         SourceLocation location,
                                         IROperand *out_value);
static int ir_emit_jump_instruction(IRLoweringContext *context,
                                    IRFunction *function, const char *label,
                                    SourceLocation location) {
  if (!context || !function || !label) {
    return 0;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_JUMP;
  instruction.location = location;
  instruction.text = (char *)label;
  return ir_emit(context, function, &instruction);
}

static int ir_emit_label_instruction(IRLoweringContext *context,
                                     IRFunction *function, const char *label,
                                     SourceLocation location) {
  if (!context || !function || !label) {
    return 0;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_LABEL;
  instruction.location = location;
  instruction.text = (char *)label;
  return ir_emit(context, function, &instruction);
}

static int ir_type_is_cstring(Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "cstring") == 0;
}

static int ir_expression_is_string(IRLoweringContext *context,
                                   ASTNode *expression) {
  Type *type = ir_infer_expression_type(context, expression);
  return type && type->kind == TYPE_STRING;
}

static int ir_should_coerce_string_to_cstring(IRLoweringContext *context,
                                              Type *target_type,
                                              ASTNode *value_expression) {
  return ir_type_is_cstring(target_type) &&
         ir_expression_is_string(context, value_expression);
}

static int ir_coerce_string_operand_to_cstring(IRLoweringContext *context,
                                               IRFunction *function,
                                               IROperand *value,
                                               SourceLocation location) {
  if (!context || !function || !value || value->kind == IR_OPERAND_NONE) {
    return 0;
  }

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    return 0;
  }

  IRInstruction load_chars = {0};
  load_chars.op = IR_OP_LOAD;
  load_chars.location = location;
  load_chars.dest = destination;
  load_chars.lhs = *value;
  load_chars.rhs = ir_operand_int(8);
  if (!ir_emit(context, function, &load_chars)) {
    ir_operand_destroy(&destination);
    return 0;
  }

  ir_operand_destroy(value);
  *value = destination;
  return 1;
}

static int ir_lower_statement_or_expression(IRLoweringContext *context,
                                            IRFunction *function,
                                            ASTNode *node) {
  if (!node) {
    return 1;
  }
  // Treat known statement nodes as statements, otherwise treat as expression.
  switch (node->type) {
  case AST_VAR_DECLARATION:
  case AST_ASSIGNMENT:
  case AST_FUNCTION_CALL:
  case AST_RETURN_STATEMENT:
  case AST_IF_STATEMENT:
  case AST_WHILE_STATEMENT:
  case AST_FOR_STATEMENT:
  case AST_SWITCH_STATEMENT:
  case AST_MATCH_STATEMENT:
  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT:
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT:
  case AST_INLINE_ASM:
  case AST_PROGRAM:
    return ir_lower_statement_with_defers(context, function, node, NULL);
  default: {
    IROperand ignored = ir_operand_none();
    int ok = ir_lower_expression(context, function, node, &ignored);
    ir_operand_destroy(&ignored);
    return ok;
  }
  }
}

static int ir_emit_local_declaration(IRLoweringContext *context,
                                     IRFunction *function,
                                     const char *name, const char *type_name,
                                     SourceLocation location) {
  if (!context || !function || !name || !type_name) {
    return 0;
  }

  IRInstruction local = {0};
  local.op = IR_OP_DECLARE_LOCAL;
  local.location = location;
  local.dest = ir_operand_symbol(name);
  local.text = (char *)type_name;
  if (!local.dest.name) {
    ir_set_error(context, "Out of memory while declaring IR local '%s'", name);
    return 0;
  }

  if (!ir_emit(context, function, &local)) {
    ir_operand_destroy(&local.dest);
    return 0;
  }

  ir_operand_destroy(&local.dest);
  return 1;
}

static IROperand ir_clone_operand_local(const IROperand *operand) {
  if (!operand) {
    return ir_operand_none();
  }

  switch (operand->kind) {
  case IR_OPERAND_TEMP:
    return ir_operand_temp(operand->name);
  case IR_OPERAND_SYMBOL:
    return ir_operand_symbol(operand->name);
  case IR_OPERAND_INT:
    return ir_operand_int(operand->int_value);
  case IR_OPERAND_FLOAT:
    return ir_operand_float(operand->float_value);
  case IR_OPERAND_STRING:
    return ir_operand_string(operand->name);
  case IR_OPERAND_LABEL:
    return ir_operand_label(operand->name);
  case IR_OPERAND_NONE:
  default:
    return ir_operand_none();
  }
}

/* Whole-struct copy: IR_OP_ASSIGN only moves scalar width through RAX. When
 * both sides are the same by-reference struct on stack, memcpy via IR_OP_STORE.
 *
 * The symbol table scope of the function body has typically been popped by the
 * time IR lowering runs, so we cannot rely on symbol_table_lookup here. Instead
 * callers thread the resolved struct Type * (cached on AST nodes or fetched via
 * the type_checker by name). */
static int ir_try_emit_aggregate_symbol_memcpy(
    IRLoweringContext *context, IRFunction *function, const char *dest_name,
    const IROperand *value, Type *dest_type, SourceLocation location) {
  int nbytes = 0;

  if (!context || !function || !dest_name || !value ||
      value->kind != IR_OPERAND_SYMBOL || !value->name) {
    return 0;
  }
  if (!dest_type || dest_type->kind != TYPE_STRUCT) {
    return 0;
  }
  if (dest_type->size == 0 || dest_type->size > (size_t)INT_MAX) {
    return 0;
  }
  nbytes = (int)dest_type->size;
  if (nbytes <= 8) {
    return 0;
  }

  {
    IROperand dest_addr = ir_operand_none();
    IROperand src_addr = ir_operand_none();
    IRInstruction store = {0};
    int ok = 0;

    if (!ir_emit_address_of_symbol(context, function, dest_name, location,
                                     &dest_addr)) {
      return 0;
    }
    if (!ir_emit_address_of_symbol(context, function, value->name, location,
                                   &src_addr)) {
      ir_operand_destroy(&dest_addr);
      return 0;
    }

    store.op = IR_OP_STORE;
    store.location = location;
    store.dest = dest_addr;
    store.lhs = src_addr;
    store.rhs = ir_operand_int((long long)nbytes);
    ok = ir_emit(context, function, &store);
    ir_operand_destroy(&dest_addr);
    ir_operand_destroy(&src_addr);
    return ok;
  }
}

/* Resolve a named type via the type_checker (works even after scope pop). */
static Type *ir_resolve_named_type(IRLoweringContext *context,
                                   const char *name) {
  if (!context || !context->type_checker || !name) {
    return NULL;
  }
  return type_checker_get_type_by_name(context->type_checker, name);
}

/* Look up a symbol's type from the symbol's name; falls back to NULL once the
 * scope is gone. Callers must handle NULL. */
static Type *ir_lookup_symbol_type(IRLoweringContext *context,
                                   const char *name) {
  if (!context || !context->symbol_table || !name) {
    return NULL;
  }
  Symbol *sym = symbol_table_lookup(context->symbol_table, name);
  return sym ? sym->type : NULL;
}

static int ir_emit_symbol_assignment(IRLoweringContext *context,
                                     IRFunction *function,
                                     const char *name,
                                     const IROperand *value,
                                     SourceLocation location) {
  if (!context || !function || !name || !value) {
    return 0;
  }

  {
    Type *dest_type = ir_lookup_symbol_type(context, name);
    if (ir_try_emit_aggregate_symbol_memcpy(context, function, name, value,
                                             dest_type, location)) {
      return 1;
    }
  }

  {
    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = location;
    assign.dest = ir_operand_symbol(name);
    assign.lhs = *value;
    if (!assign.dest.name) {
      ir_set_error(context, "Out of memory while assigning IR local '%s'", name);
      return 0;
    }

    if (!ir_emit(context, function, &assign)) {
      ir_operand_destroy(&assign.dest);
      return 0;
    }

    ir_operand_destroy(&assign.dest);
    return 1;
  }
}

static int ir_emit_address_with_offset(IRLoweringContext *context,
                                       IRFunction *function,
                                       const IROperand *base_address,
                                       size_t offset,
                                       SourceLocation location,
                                       IROperand *out_address) {
  if (!context || !function || !base_address || !out_address) {
    return 0;
  }

  if (offset == 0) {
    *out_address = ir_clone_operand_local(base_address);
    return 1;
  }

  IROperand address = ir_operand_none();
  if (!ir_make_temp_operand(context, &address)) {
    return 0;
  }

  IRInstruction add = {0};
  add.op = IR_OP_BINARY;
  add.location = location;
  add.dest = address;
  add.lhs = *base_address;
  add.rhs = ir_operand_int((long long)offset);
  add.text = "+";
  if (!ir_emit(context, function, &add)) {
    ir_operand_destroy(&address);
    return 0;
  }

  *out_address = address;
  return 1;
}

// Emit the dispatch test for a range case `lo..hi`: if the switch value lies
// in [lo, hi], jump to case_label; otherwise fall through to the next test.
// Lowered with the existing comparison/branch primitives:
//   ge = (sv >= lo); if (!ge) goto skip;
//   le = (sv <= hi); if (!le) goto skip;
//   goto case_label;
//   skip:
static int ir_emit_switch_range_dispatch(IRLoweringContext *context,
                                         IRFunction *function,
                                         const IROperand *switch_value,
                                         ASTNode *lo_node, ASTNode *hi_node,
                                         const char *case_label,
                                         SourceLocation loc) {
  IROperand lo = ir_operand_none();
  IROperand hi = ir_operand_none();
  IROperand ge = ir_operand_none();
  IROperand le = ir_operand_none();
  char *skip_label = ir_new_label_name(context, "switch_range_skip");
  int ok = 0;
  if (!skip_label) {
    return 0;
  }

  if (!ir_lower_expression(context, function, lo_node, &lo) ||
      !ir_make_temp_operand(context, &ge)) {
    goto done;
  }
  IRInstruction cmp_lo = {0};
  cmp_lo.op = IR_OP_BINARY;
  cmp_lo.location = loc;
  cmp_lo.dest = ge;
  cmp_lo.lhs = *switch_value;
  cmp_lo.rhs = lo;
  cmp_lo.text = ">=";
  IRInstruction br_lo = {0};
  br_lo.op = IR_OP_BRANCH_ZERO;
  br_lo.location = loc;
  br_lo.lhs = ge;
  br_lo.text = skip_label;
  if (!ir_emit(context, function, &cmp_lo) ||
      !ir_emit(context, function, &br_lo)) {
    goto done;
  }

  if (!ir_lower_expression(context, function, hi_node, &hi) ||
      !ir_make_temp_operand(context, &le)) {
    goto done;
  }
  IRInstruction cmp_hi = {0};
  cmp_hi.op = IR_OP_BINARY;
  cmp_hi.location = loc;
  cmp_hi.dest = le;
  cmp_hi.lhs = *switch_value;
  cmp_hi.rhs = hi;
  cmp_hi.text = "<=";
  IRInstruction br_hi = {0};
  br_hi.op = IR_OP_BRANCH_ZERO;
  br_hi.location = loc;
  br_hi.lhs = le;
  br_hi.text = skip_label;
  if (!ir_emit(context, function, &cmp_hi) ||
      !ir_emit(context, function, &br_hi)) {
    goto done;
  }

  if (!ir_emit_jump_instruction(context, function, case_label, loc)) {
    goto done;
  }
  IRInstruction skip = {0};
  skip.op = IR_OP_LABEL;
  skip.location = loc;
  skip.text = skip_label;
  if (!ir_emit(context, function, &skip)) {
    goto done;
  }
  ok = 1;

done:
  ir_operand_destroy(&lo);
  ir_operand_destroy(&hi);
  ir_operand_destroy(&ge);
  ir_operand_destroy(&le);
  free(skip_label);
  return ok;
}

static int ir_lower_switch_statement(IRLoweringContext *context,
                                     IRFunction *function, ASTNode *statement) {
  if (!context || !function || !statement ||
      statement->type != AST_SWITCH_STATEMENT) {
    return 0;
  }

  SwitchStatement *switch_data = (SwitchStatement *)statement->data;
  if (!switch_data || !switch_data->expression) {
    ir_set_error(context, "Malformed switch statement");
    return 0;
  }

  char *end_label = ir_new_label_name(context, "switch_end");
  if (!end_label) {
    ir_set_error(context, "Out of memory while allocating switch labels");
    return 0;
  }

  IROperand switch_value = ir_operand_none();
  if (!ir_lower_expression(context, function, switch_data->expression,
                           &switch_value)) {
    free(end_label);
    return 0;
  }

  char **case_labels = NULL;
  if (switch_data->case_count > 0) {
    case_labels = calloc(switch_data->case_count, sizeof(char *));
    if (!case_labels) {
      ir_operand_destroy(&switch_value);
      free(end_label);
      ir_set_error(context,
                   "Out of memory while allocating switch case labels");
      return 0;
    }
    for (size_t i = 0; i < switch_data->case_count; i++) {
      case_labels[i] = ir_new_label_name(context, "case");
      if (!case_labels[i]) {
        for (size_t j = 0; j < i; j++) {
          free(case_labels[j]);
        }
        free(case_labels);
        ir_operand_destroy(&switch_value);
        free(end_label);
        ir_set_error(context,
                     "Out of memory while allocating switch case labels");
        return 0;
      }
    }
  }

  char *default_label = NULL;
  for (size_t i = 0; i < switch_data->case_count; i++) {
    ASTNode *case_node = switch_data->cases[i];
    CaseClause *clause = case_node ? (CaseClause *)case_node->data : NULL;
    if (clause && clause->is_default) {
      default_label = case_labels ? case_labels[i] : NULL;
      break;
    }
  }
  if (!default_label) {
    default_label = end_label;
  }

  // Dispatch chain: if (switch_value == case_value) jump case label.
  for (size_t i = 0; i < switch_data->case_count; i++) {
    ASTNode *case_node = switch_data->cases[i];
    CaseClause *clause = case_node ? (CaseClause *)case_node->data : NULL;
    if (!case_node || !clause) {
      continue;
    }
    if (clause->is_default) {
      continue;
    }
    if (!clause->value) {
      continue;
    }

    // Range case `lo..hi`: emit a two-sided bounds test instead of equality.
    if (clause->value_high) {
      if (!ir_emit_switch_range_dispatch(context, function, &switch_value,
                                         clause->value, clause->value_high,
                                         case_labels[i], statement->location)) {
        for (size_t j = 0; j < switch_data->case_count; j++) {
          free(case_labels[j]);
        }
        free(case_labels);
        ir_operand_destroy(&switch_value);
        free(end_label);
        return 0;
      }
      continue;
    }

    IROperand case_value = ir_operand_none();
    if (!ir_lower_expression(context, function, clause->value, &case_value)) {
      for (size_t j = 0; j < switch_data->case_count; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      ir_operand_destroy(&switch_value);
      free(end_label);
      return 0;
    }

    IRInstruction cmp = {0};
    cmp.op = IR_OP_BRANCH_EQ;
    cmp.location = statement->location;
    cmp.lhs = switch_value;
    cmp.rhs = case_value;
    cmp.text = case_labels[i];
    if (!ir_emit(context, function, &cmp)) {
      ir_operand_destroy(&case_value);
      for (size_t j = 0; j < switch_data->case_count; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      ir_operand_destroy(&switch_value);
      free(end_label);
      return 0;
    }
    ir_operand_destroy(&case_value);
  }

  // No match.
  if (!ir_emit_jump_instruction(context, function, default_label,
                                statement->location)) {
    for (size_t j = 0; j < switch_data->case_count; j++) {
      free(case_labels[j]);
    }
    free(case_labels);
    ir_operand_destroy(&switch_value);
    free(end_label);
    return 0;
  }

  // Emit cases.
  if (!ir_push_control_frame(context, end_label, NULL)) {
    for (size_t j = 0; j < switch_data->case_count; j++) {
      free(case_labels[j]);
    }
    free(case_labels);
    ir_operand_destroy(&switch_value);
    free(end_label);
    return 0;
  }

  for (size_t i = 0; i < switch_data->case_count; i++) {
    ASTNode *case_node = switch_data->cases[i];
    CaseClause *clause = case_node ? (CaseClause *)case_node->data : NULL;
    if (!case_node || !clause) {
      continue;
    }

    if (!ir_emit_label_instruction(context, function, case_labels[i],
                                   case_node->location)) {
      ir_pop_control_frame(context);
      for (size_t j = 0; j < switch_data->case_count; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      ir_operand_destroy(&switch_value);
      free(end_label);
      return 0;
    }

    if (clause->body && !ir_lower_statement_with_defers(context, function,
                                                        clause->body, NULL)) {
      ir_pop_control_frame(context);
      for (size_t j = 0; j < switch_data->case_count; j++) {
        free(case_labels[j]);
      }
      free(case_labels);
      ir_operand_destroy(&switch_value);
      free(end_label);
      return 0;
    }

    // Fallthrough to next case label unless body jumped/broke.
  }

  ir_pop_control_frame(context);

  if (!ir_emit_label_instruction(context, function, end_label,
                                 statement->location)) {
    for (size_t j = 0; j < switch_data->case_count; j++) {
      free(case_labels[j]);
    }
    free(case_labels);
    ir_operand_destroy(&switch_value);
    free(end_label);
    return 0;
  }

  for (size_t j = 0; j < switch_data->case_count; j++) {
    free(case_labels[j]);
  }
  free(case_labels);
  ir_operand_destroy(&switch_value);
  free(end_label);
  return 1;
}

static int ir_lower_match_statement(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *statement,
                                    IRDeferScope *defers) {
  MatchStatement *match = NULL;
  Type *subject_type = NULL;
  IROperand subject_value = ir_operand_none();
  IROperand subject_address = ir_operand_none();
  IROperand tag_value = ir_operand_none();
  char *owned_subject_name = NULL;
  const char *subject_name = NULL;
  char *end_label = NULL;
  char **arm_labels = NULL;
  char *default_label = NULL;
  int ok = 0;

  if (!context || !function || !statement ||
      statement->type != AST_MATCH_STATEMENT) {
    return 0;
  }

  match = (MatchStatement *)statement->data;
  if (!match || !match->expression) {
    ir_set_error(context, "Malformed match statement");
    return 0;
  }

  subject_type = ir_infer_expression_type(context, match->expression);
  if (!subject_type || subject_type->kind != TYPE_TAGGED_ENUM ||
      !subject_type->name) {
    ir_set_error(context, "IR match lowering requires a tagged-enum subject");
    return 0;
  }

  if (!ir_lower_expression(context, function, match->expression,
                           &subject_value)) {
    return 0;
  }

  if (subject_value.kind == IR_OPERAND_SYMBOL && subject_value.name) {
    subject_name = subject_value.name;
  } else {
    owned_subject_name = ir_new_label_name(context, "match_subject");
    if (!owned_subject_name) {
      ir_set_error(context,
                   "Out of memory while allocating match subject storage");
      goto cleanup;
    }
    if (!ir_emit_local_declaration(context, function, owned_subject_name,
                                   subject_type->name, statement->location) ||
        !ir_emit_symbol_assignment(context, function, owned_subject_name,
                                   &subject_value, statement->location)) {
      goto cleanup;
    }
    subject_name = owned_subject_name;
  }

  if (!ir_emit_address_of_symbol(context, function, subject_name,
                                 match->expression->location,
                                 &subject_address)) {
    goto cleanup;
  }

  if (!ir_make_temp_operand(context, &tag_value)) {
    goto cleanup;
  }

  {
    IRInstruction load_tag = {0};
    load_tag.op = IR_OP_LOAD;
    load_tag.location = match->expression->location;
    load_tag.dest = tag_value;
    load_tag.lhs = subject_address;
    load_tag.rhs = ir_operand_int(4);
    if (!ir_emit(context, function, &load_tag)) {
      goto cleanup;
    }
  }

  end_label = ir_new_label_name(context, "match_end");
  if (!end_label) {
    ir_set_error(context, "Out of memory while allocating match labels");
    goto cleanup;
  }

  if (match->arm_count > 0) {
    arm_labels = calloc(match->arm_count, sizeof(char *));
    if (!arm_labels) {
      ir_set_error(context, "Out of memory while allocating match labels");
      goto cleanup;
    }
    for (size_t i = 0; i < match->arm_count; i++) {
      arm_labels[i] = ir_new_label_name(context, "match_arm");
      if (!arm_labels[i]) {
        ir_set_error(context, "Out of memory while allocating match labels");
        goto cleanup;
      }
      if (match->arms[i].is_default) {
        default_label = arm_labels[i];
      }
    }
  }
  if (!default_label) {
    default_label = end_label;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;

    if (!arm || arm->is_default) {
      continue;
    }

    for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
      if (subject_type->tagged_variant_names[v] &&
          strcmp(subject_type->tagged_variant_names[v], arm->variant_name) ==
              0) {
        variant_idx = (int)v;
        break;
      }
    }

    if (variant_idx < 0) {
      ir_set_error(context, "Unknown tagged-enum variant '%s' in match",
                   arm->variant_name ? arm->variant_name : "<unnamed>");
      goto cleanup;
    }

    IRInstruction cmp = {0};
    cmp.op = IR_OP_BRANCH_EQ;
    cmp.location = statement->location;
    cmp.lhs = tag_value;
    cmp.rhs =
        ir_operand_int((long long)subject_type->tagged_variant_tags[variant_idx]);
    cmp.text = arm_labels[i];
    if (!ir_emit(context, function, &cmp)) {
      goto cleanup;
    }
  }

  if (!ir_emit_jump_instruction(context, function, default_label,
                                statement->location)) {
    goto cleanup;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;

    if (!arm) {
      continue;
    }

    if (!ir_emit_label_instruction(context, function, arm_labels[i],
                                   arm->body ? arm->body->location
                                             : statement->location)) {
      goto cleanup;
    }

    if (!arm->is_default) {
      for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
        if (subject_type->tagged_variant_names[v] &&
            strcmp(subject_type->tagged_variant_names[v], arm->variant_name) ==
                0) {
          variant_idx = (int)v;
          break;
        }
      }
    }

    if (arm->binding_name && variant_idx >= 0) {
      Type *payload_type = subject_type->tagged_variant_payloads[variant_idx];
      int payload_size = 0;
      IROperand payload_address = ir_operand_none();

      if (!payload_type || !payload_type->name) {
        ir_set_error(context, "Match binding '%s' has no payload to bind",
                     arm->binding_name);
        goto cleanup;
      }

      payload_size = (payload_type->size > 0) ? (int)payload_type->size
                                              : ir_type_storage_size(payload_type);
      if (!ir_emit_local_declaration(context, function, arm->binding_name,
                                     payload_type->name, statement->location) ||
          !ir_emit_address_with_offset(context, function, &subject_address,
                                       subject_type->tagged_data_offset,
                                       statement->location, &payload_address)) {
        ir_operand_destroy(&payload_address);
        goto cleanup;
      }

      if (payload_size > 8) {
        IROperand binding_address = ir_operand_none();
        IRInstruction copy = {0};

        if (!ir_emit_address_of_symbol(context, function, arm->binding_name,
                                       statement->location,
                                       &binding_address)) {
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        copy.op = IR_OP_STORE;
        copy.location = statement->location;
        copy.dest = binding_address;
        copy.lhs = payload_address;
        copy.rhs = ir_operand_int(payload_size);
        if (!ir_emit(context, function, &copy)) {
          ir_operand_destroy(&binding_address);
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        ir_operand_destroy(&binding_address);
      } else {
        IROperand payload_value = ir_operand_none();
        IRInstruction load = {0};

        if (!ir_make_temp_operand(context, &payload_value)) {
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        load.op = IR_OP_LOAD;
        load.location = statement->location;
        load.dest = payload_value;
        load.lhs = payload_address;
        load.rhs = ir_operand_int(payload_size);
        if (!ir_emit(context, function, &load) ||
            !ir_emit_symbol_assignment(context, function, arm->binding_name,
                                       &payload_value, statement->location)) {
          ir_operand_destroy(&payload_value);
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        ir_operand_destroy(&payload_value);
      }

      ir_operand_destroy(&payload_address);
    }

    if (arm->body &&
        !ir_lower_statement_with_defers(context, function, arm->body, defers)) {
      goto cleanup;
    }

    if (!ir_emit_jump_instruction(context, function, end_label,
                                  statement->location)) {
      goto cleanup;
    }
  }

  if (!ir_emit_label_instruction(context, function, end_label,
                                 statement->location)) {
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (arm_labels) {
    for (size_t i = 0; i < match->arm_count; i++) {
      free(arm_labels[i]);
    }
    free(arm_labels);
  }
  free(end_label);
  free(owned_subject_name);
  ir_operand_destroy(&tag_value);
  ir_operand_destroy(&subject_address);
  ir_operand_destroy(&subject_value);
  return ok;
}

// Lower a match used in expression position. Mirrors ir_lower_match_statement
// but allocates a result local; each arm lowers its body *expression* and
// stores the value into that local, which becomes the value of the match.
static int ir_lower_match_expression(IRLoweringContext *context,
                                     IRFunction *function,
                                     ASTNode *expression,
                                     IROperand *out_value) {
  MatchStatement *match = NULL;
  Type *subject_type = NULL;
  Type *result_type = NULL;
  IROperand subject_value = ir_operand_none();
  IROperand subject_address = ir_operand_none();
  IROperand tag_value = ir_operand_none();
  char *owned_subject_name = NULL;
  const char *subject_name = NULL;
  char *result_name = NULL;
  char *end_label = NULL;
  char **arm_labels = NULL;
  char *default_label = NULL;
  int ok = 0;

  if (!context || !function || !expression || !out_value ||
      expression->type != AST_MATCH_STATEMENT) {
    return 0;
  }

  match = (MatchStatement *)expression->data;
  if (!match || !match->expression || !match->is_expression) {
    ir_set_error(context, "Malformed match expression");
    return 0;
  }

  subject_type = ir_infer_expression_type(context, match->expression);
  if (!subject_type || subject_type->kind != TYPE_TAGGED_ENUM ||
      !subject_type->name) {
    ir_set_error(context, "IR match lowering requires a tagged-enum subject");
    return 0;
  }

  result_type = ir_infer_expression_type(context, expression);
  if (!result_type || !result_type->name) {
    ir_set_error(context, "Could not determine match expression result type");
    return 0;
  }

  if (!ir_lower_expression(context, function, match->expression,
                           &subject_value)) {
    return 0;
  }

  result_name = ir_new_label_name(context, "match_result");
  if (!result_name ||
      !ir_emit_local_declaration(context, function, result_name,
                                 result_type->name, expression->location)) {
    ir_set_error(context, "Out of memory while allocating match result");
    goto cleanup;
  }

  if (subject_value.kind == IR_OPERAND_SYMBOL && subject_value.name) {
    subject_name = subject_value.name;
  } else {
    owned_subject_name = ir_new_label_name(context, "match_subject");
    if (!owned_subject_name) {
      ir_set_error(context,
                   "Out of memory while allocating match subject storage");
      goto cleanup;
    }
    if (!ir_emit_local_declaration(context, function, owned_subject_name,
                                   subject_type->name,
                                   expression->location) ||
        !ir_emit_symbol_assignment(context, function, owned_subject_name,
                                   &subject_value, expression->location)) {
      goto cleanup;
    }
    subject_name = owned_subject_name;
  }

  if (!ir_emit_address_of_symbol(context, function, subject_name,
                                 match->expression->location,
                                 &subject_address)) {
    goto cleanup;
  }

  if (!ir_make_temp_operand(context, &tag_value)) {
    goto cleanup;
  }

  {
    IRInstruction load_tag = {0};
    load_tag.op = IR_OP_LOAD;
    load_tag.location = match->expression->location;
    load_tag.dest = tag_value;
    load_tag.lhs = subject_address;
    load_tag.rhs = ir_operand_int(4);
    if (!ir_emit(context, function, &load_tag)) {
      goto cleanup;
    }
  }

  end_label = ir_new_label_name(context, "match_end");
  if (!end_label) {
    ir_set_error(context, "Out of memory while allocating match labels");
    goto cleanup;
  }

  if (match->arm_count > 0) {
    arm_labels = calloc(match->arm_count, sizeof(char *));
    if (!arm_labels) {
      ir_set_error(context, "Out of memory while allocating match labels");
      goto cleanup;
    }
    for (size_t i = 0; i < match->arm_count; i++) {
      arm_labels[i] = ir_new_label_name(context, "match_arm");
      if (!arm_labels[i]) {
        ir_set_error(context, "Out of memory while allocating match labels");
        goto cleanup;
      }
      if (match->arms[i].is_default) {
        default_label = arm_labels[i];
      }
    }
  }
  if (!default_label) {
    default_label = end_label;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;

    if (!arm || arm->is_default) {
      continue;
    }

    for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
      if (subject_type->tagged_variant_names[v] &&
          strcmp(subject_type->tagged_variant_names[v], arm->variant_name) ==
              0) {
        variant_idx = (int)v;
        break;
      }
    }

    if (variant_idx < 0) {
      ir_set_error(context, "Unknown tagged-enum variant '%s' in match",
                   arm->variant_name ? arm->variant_name : "<unnamed>");
      goto cleanup;
    }

    IRInstruction cmp = {0};
    cmp.op = IR_OP_BRANCH_EQ;
    cmp.location = expression->location;
    cmp.lhs = tag_value;
    cmp.rhs = ir_operand_int(
        (long long)subject_type->tagged_variant_tags[variant_idx]);
    cmp.text = arm_labels[i];
    if (!ir_emit(context, function, &cmp)) {
      goto cleanup;
    }
  }

  if (!ir_emit_jump_instruction(context, function, default_label,
                                expression->location)) {
    goto cleanup;
  }

  for (size_t i = 0; i < match->arm_count; i++) {
    MatchArm *arm = &match->arms[i];
    int variant_idx = -1;
    IROperand arm_value = ir_operand_none();

    if (!arm) {
      continue;
    }

    if (!ir_emit_label_instruction(context, function, arm_labels[i],
                                   arm->body ? arm->body->location
                                             : expression->location)) {
      goto cleanup;
    }

    if (!arm->is_default) {
      for (size_t v = 0; v < subject_type->tagged_variant_count; v++) {
        if (subject_type->tagged_variant_names[v] &&
            strcmp(subject_type->tagged_variant_names[v], arm->variant_name) ==
                0) {
          variant_idx = (int)v;
          break;
        }
      }
    }

    if (arm->binding_name && variant_idx >= 0) {
      Type *payload_type = subject_type->tagged_variant_payloads[variant_idx];
      int payload_size = 0;
      IROperand payload_address = ir_operand_none();

      if (!payload_type || !payload_type->name) {
        ir_set_error(context, "Match binding '%s' has no payload to bind",
                     arm->binding_name);
        goto cleanup;
      }

      payload_size = (payload_type->size > 0)
                         ? (int)payload_type->size
                         : ir_type_storage_size(payload_type);
      if (!ir_emit_local_declaration(context, function, arm->binding_name,
                                     payload_type->name,
                                     expression->location) ||
          !ir_emit_address_with_offset(context, function, &subject_address,
                                       subject_type->tagged_data_offset,
                                       expression->location,
                                       &payload_address)) {
        ir_operand_destroy(&payload_address);
        goto cleanup;
      }

      if (payload_size > 8) {
        IROperand binding_address = ir_operand_none();
        IRInstruction copy = {0};

        if (!ir_emit_address_of_symbol(context, function, arm->binding_name,
                                       expression->location,
                                       &binding_address)) {
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        copy.op = IR_OP_STORE;
        copy.location = expression->location;
        copy.dest = binding_address;
        copy.lhs = payload_address;
        copy.rhs = ir_operand_int(payload_size);
        if (!ir_emit(context, function, &copy)) {
          ir_operand_destroy(&binding_address);
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        ir_operand_destroy(&binding_address);
      } else {
        IROperand payload_value = ir_operand_none();
        IRInstruction load = {0};

        if (!ir_make_temp_operand(context, &payload_value)) {
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        load.op = IR_OP_LOAD;
        load.location = expression->location;
        load.dest = payload_value;
        load.lhs = payload_address;
        load.rhs = ir_operand_int(payload_size);
        if (!ir_emit(context, function, &load) ||
            !ir_emit_symbol_assignment(context, function, arm->binding_name,
                                       &payload_value,
                                       expression->location)) {
          ir_operand_destroy(&payload_value);
          ir_operand_destroy(&payload_address);
          goto cleanup;
        }

        ir_operand_destroy(&payload_value);
      }

      ir_operand_destroy(&payload_address);
    }

    if (!arm->body) {
      ir_set_error(context, "match arm has no value expression");
      goto cleanup;
    }

    if (!ir_lower_expression(context, function, arm->body, &arm_value)) {
      goto cleanup;
    }
    if (!ir_emit_symbol_assignment(context, function, result_name, &arm_value,
                                   arm->body->location)) {
      ir_operand_destroy(&arm_value);
      goto cleanup;
    }
    ir_operand_destroy(&arm_value);

    if (!ir_emit_jump_instruction(context, function, end_label,
                                  expression->location)) {
      goto cleanup;
    }
  }

  if (!ir_emit_label_instruction(context, function, end_label,
                                 expression->location)) {
    goto cleanup;
  }

  *out_value = ir_operand_symbol(result_name);
  if (!out_value->name) {
    ir_set_error(context, "Out of memory while finalizing match expression");
    goto cleanup;
  }
  ok = 1;

cleanup:
  if (arm_labels) {
    for (size_t i = 0; i < match->arm_count; i++) {
      free(arm_labels[i]);
    }
    free(arm_labels);
  }
  free(end_label);
  free(result_name);
  free(owned_subject_name);
  ir_operand_destroy(&tag_value);
  ir_operand_destroy(&subject_address);
  ir_operand_destroy(&subject_value);
  return ok;
}

static int ir_emit_tagged_enum_construct(IRLoweringContext *context,
                                         IRFunction *function,
                                         Symbol *constructor_symbol,
                                         ASTNode *payload_arg,
                                         SourceLocation location,
                                         IROperand *out_value) {
  Type *enum_type = NULL;
  Type *payload_type = NULL;
  char *local_name = NULL;
  IROperand enum_address = ir_operand_none();

  if (!context || !function || !constructor_symbol || !out_value) {
    return 0;
  }

  enum_type = constructor_symbol->data.constructor.enum_type;
  payload_type = constructor_symbol->data.constructor.payload_type;
  if (!enum_type || !enum_type->name) {
    ir_set_error(context, "Malformed tagged-enum constructor");
    return 0;
  }

  /* Nullary constructor referenced bare (e.g. `var x: Option = None`) reaches
   * here with payload_arg == NULL and payload_type == NULL; that is valid. A
   * payload_arg without a payload_type, or vice versa, is a type-checker bug. */
  if ((payload_type != NULL) != (payload_arg != NULL)) {
    ir_set_error(context,
                 "Tagged-enum constructor arity mismatch (variant '%s')",
                 constructor_symbol->name ? constructor_symbol->name : "?");
    return 0;
  }

  local_name = ir_new_label_name(context, "tagged_ctor");
  if (!local_name) {
    ir_set_error(context,
                 "Out of memory while allocating tagged-enum constructor");
    return 0;
  }

  if (!ir_emit_local_declaration(context, function, local_name, enum_type->name,
                                 location) ||
      !ir_emit_address_of_symbol(context, function, local_name,
                                 location, &enum_address)) {
    ir_operand_destroy(&enum_address);
    free(local_name);
    return 0;
  }

  {
    IRInstruction store_tag = {0};
    store_tag.op = IR_OP_STORE;
    store_tag.location = location;
    store_tag.dest = enum_address;
    store_tag.lhs =
        ir_operand_int((long long)constructor_symbol->data.constructor.tag_value);
    store_tag.rhs = ir_operand_int(4);
    if (!ir_emit(context, function, &store_tag)) {
      ir_operand_destroy(&enum_address);
      free(local_name);
      return 0;
    }
  }

  if (payload_type) {
    int payload_size =
        (payload_type->size > 0) ? (int)payload_type->size
                                 : ir_type_storage_size(payload_type);
    IROperand payload_value = ir_operand_none();
    IROperand payload_address = ir_operand_none();
    IROperand payload_source = ir_operand_none();
    char *payload_temp_name = NULL;

    if (!ir_lower_expression(context, function, payload_arg,
                             &payload_value) ||
        !ir_emit_address_with_offset(context, function, &enum_address,
                                     enum_type->tagged_data_offset,
                                     location, &payload_address)) {
      ir_operand_destroy(&payload_address);
      ir_operand_destroy(&payload_value);
      ir_operand_destroy(&enum_address);
      free(local_name);
      return 0;
    }

    if (payload_size > 8) {
      payload_temp_name = ir_new_label_name(context, "tagged_payload");
      if (!payload_temp_name ||
          !ir_emit_local_declaration(context, function, payload_temp_name,
                                     payload_type->name, location) ||
          !ir_emit_symbol_assignment(context, function, payload_temp_name,
                                     &payload_value, location) ||
          !ir_emit_address_of_symbol(context, function, payload_temp_name,
                                     location, &payload_source)) {
        free(payload_temp_name);
        ir_operand_destroy(&payload_source);
        ir_operand_destroy(&payload_address);
        ir_operand_destroy(&payload_value);
        ir_operand_destroy(&enum_address);
        free(local_name);
        return 0;
      }
    } else {
      payload_source = payload_value;
    }

    {
      IRInstruction store_payload = {0};
      store_payload.op = IR_OP_STORE;
      store_payload.location = location;
      store_payload.dest = payload_address;
      store_payload.lhs = payload_source;
      store_payload.rhs = ir_operand_int(payload_size);
      if (!ir_emit(context, function, &store_payload)) {
        free(payload_temp_name);
        ir_operand_destroy(&payload_source);
        ir_operand_destroy(&payload_address);
        ir_operand_destroy(&payload_value);
        ir_operand_destroy(&enum_address);
        free(local_name);
        return 0;
      }
    }

    free(payload_temp_name);
    if (payload_size > 8) {
      ir_operand_destroy(&payload_source);
    }
    ir_operand_destroy(&payload_address);
    ir_operand_destroy(&payload_value);
  }

  ir_operand_destroy(&enum_address);
  *out_value = ir_operand_symbol(local_name);
  free(local_name);
  if (out_value->kind != IR_OPERAND_SYMBOL || !out_value->name) {
    ir_set_error(context,
                 "Out of memory while returning tagged-enum constructor");
    return 0;
  }

  return 1;
}

static int ir_lower_tagged_enum_constructor_call(IRLoweringContext *context,
                                                 IRFunction *function,
                                                 ASTNode *expression,
                                                 Symbol *constructor_symbol,
                                                 IROperand *out_value) {
  CallExpression *call = NULL;
  Type *payload_type = NULL;
  ASTNode *payload_arg = NULL;

  if (!context || !function || !expression || !constructor_symbol ||
      !out_value) {
    return 0;
  }

  call = (CallExpression *)expression->data;
  payload_type = constructor_symbol->data.constructor.payload_type;
  if (!call) {
    ir_set_error(context, "Malformed tagged-enum constructor call");
    return 0;
  }

  if (payload_type) {
    if (call->argument_count != 1 || !call->arguments ||
        !call->arguments[0]) {
      ir_set_error(context,
                   "Tagged-enum variant '%s' expects exactly one payload argument",
                   constructor_symbol->name ? constructor_symbol->name : "?");
      return 0;
    }
    payload_arg = call->arguments[0];
  } else if (call->argument_count != 0) {
    ir_set_error(context,
                 "Tagged-enum variant '%s' is nullary; pass no arguments",
                 constructor_symbol->name ? constructor_symbol->name : "?");
    return 0;
  }

  return ir_emit_tagged_enum_construct(context, function, constructor_symbol,
                                       payload_arg, expression->location,
                                       out_value);
}

static int ir_emit_deferred_calls(IRLoweringContext *context,
                                  IRFunction *function,
                                  const IRDeferStack *stack) {
  return ir_emit_deferred_calls_filtered(context, function, stack, 1);
}

static int ir_emit_deferred_calls_non_err(IRLoweringContext *context,
                                          IRFunction *function,
                                          const IRDeferStack *stack) {
  return ir_emit_deferred_calls_filtered(context, function, stack, 0);
}

static int ir_emit_deferred_scopes(IRLoweringContext *context,
                                   IRFunction *function,
                                   const IRDeferScope *scope) {
  for (const IRDeferScope *current = scope; current;
       current = current->parent) {
    if (!ir_emit_deferred_calls(context, function, &current->stack)) {
      return 0;
    }
  }
  return 1;
}

static int ir_emit_deferred_scopes_non_err(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferScope *scope) {
  for (const IRDeferScope *current = scope; current;
       current = current->parent) {
    if (!ir_emit_deferred_calls_non_err(context, function, &current->stack)) {
      return 0;
    }
  }
  return 1;
}

static int ir_emit_return_with_defers(IRLoweringContext *context,
                                      IRFunction *function,
                                      IRDeferScope *defers, IROperand *value,
                                      SourceLocation location) {
  if (!context || !function || !value) {
    return 0;
  }

  if (defers) {
    IROperand is_error = ir_operand_none();
    if (!ir_make_temp_operand(context, &is_error)) {
      return 0;
    }

    IRInstruction set_error = {0};
    set_error.op = IR_OP_ASSIGN;
    set_error.location = location;
    set_error.dest = is_error;
    set_error.lhs =
        (value->kind == IR_OPERAND_NONE) ? ir_operand_int(0) : *value;
    if (!ir_emit(context, function, &set_error)) {
      ir_operand_destroy(&is_error);
      return 0;
    }

    char *success_label = ir_new_label_name(context, "errdefer_ok");
    char *end_label = ir_new_label_name(context, "errdefer_end");
    if (!success_label || !end_label) {
      free(success_label);
      free(end_label);
      ir_operand_destroy(&is_error);
      ir_set_error(context,
                   "Out of memory while allocating errdefer return labels");
      return 0;
    }

    IRInstruction branch = {0};
    branch.op = IR_OP_BRANCH_ZERO;
    branch.location = location;
    branch.lhs = is_error;
    branch.text = success_label;
    if (!ir_emit(context, function, &branch)) {
      free(success_label);
      free(end_label);
      ir_operand_destroy(&is_error);
      return 0;
    }

    if (!ir_emit_deferred_scopes(context, function, defers) ||
        !ir_emit_jump_instruction(context, function, end_label, location) ||
        !ir_emit_label_instruction(context, function, success_label,
                                   location) ||
        !ir_emit_deferred_scopes_non_err(context, function, defers) ||
        !ir_emit_label_instruction(context, function, end_label, location)) {
      free(success_label);
      free(end_label);
      ir_operand_destroy(&is_error);
      return 0;
    }

    free(success_label);
    free(end_label);
    ir_operand_destroy(&is_error);
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_RETURN;
  instruction.location = location;
  instruction.lhs = *value;
  if (value->kind != IR_OPERAND_NONE) {
    int return_bits =
        ir_named_type_float_bits(context, context->current_return_type_name);
    if (return_bits) {
      ir_assign_apply_float_bits(&instruction, &instruction.lhs, return_bits);
      value->float_bits = instruction.lhs.float_bits;
    }
  }
  if (!ir_emit(context, function, &instruction)) {
    return 0;
  }

  *value = ir_operand_none();
  return 1;
}

static void ir_set_error(IRLoweringContext *context, const char *format, ...) {
  if (!context || context->error_message || !format) {
    return;
  }

  if (context->current_function_name) {
    mettle_compiler_ctx_set_function_name(context->current_function_name);
  }
  mettle_compiler_ctx_set_phase(METTLE_COMPILER_PHASE_IR_LOWERING);

  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, format, copy);
  va_end(copy);

  if (needed > 0) {
    context->error_message = malloc((size_t)needed + 1);
    if (context->error_message) {
      vsnprintf(context->error_message, (size_t)needed + 1, format, args);
    }
  }
  va_end(args);
}

static char *ir_new_temp_name(IRLoweringContext *context) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "t%d", context->next_temp_id++);
  return mettle_strdup(buffer);
}

static char *ir_new_label_name(IRLoweringContext *context, const char *prefix) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "ir_%s_%d", prefix ? prefix : "label",
           context->next_label_id++);
  return mettle_strdup(buffer);
}

static int ir_emit(IRLoweringContext *context, IRFunction *function,
                   const IRInstruction *instruction) {
  if (!ir_function_append_instruction(function, instruction)) {
    ir_set_error(context, "Out of memory while appending IR instruction");
    return 0;
  }
  return 1;
}

static int ir_emit_runtime_trap_ex(IRLoweringContext *context,
                                   IRFunction *function,
                                   SourceLocation location, uint32_t kind,
                                   const char *message, const IROperand *arg0,
                                   const IROperand *arg1) {
  if (!context || !function || !message) {
    return 0;
  }

  IRInstruction trap_call = {0};
  trap_call.op = IR_OP_CALL;
  trap_call.location = location;
  trap_call.text = "mettle_crash_trap_ex";
  trap_call.argument_count = 4;
  trap_call.arguments = calloc(4, sizeof(IROperand));
  if (!trap_call.arguments) {
    ir_set_error(context, "Out of memory while lowering runtime trap");
    return 0;
  }
  trap_call.arguments[0] = ir_operand_int((long long)kind);
  trap_call.arguments[1] = ir_operand_string(message);
  trap_call.arguments[2] = arg0 ? ir_operand_copy(arg0) : ir_operand_int(0);
  trap_call.arguments[3] = arg1 ? ir_operand_copy(arg1) : ir_operand_int(0);
  if (!ir_emit(context, function, &trap_call)) {
    ir_operand_destroy(&trap_call.arguments[0]);
    ir_operand_destroy(&trap_call.arguments[1]);
    ir_operand_destroy(&trap_call.arguments[2]);
    ir_operand_destroy(&trap_call.arguments[3]);
    free(trap_call.arguments);
    return 0;
  }
  ir_operand_destroy(&trap_call.arguments[0]);
  ir_operand_destroy(&trap_call.arguments[1]);
  ir_operand_destroy(&trap_call.arguments[2]);
  ir_operand_destroy(&trap_call.arguments[3]);
  free(trap_call.arguments);
  return 1;
}

static int ir_emit_null_check(IRLoweringContext *context, IRFunction *function,
                              SourceLocation location, const IROperand *value) {
  if (!context || !function || !value) {
    return 0;
  }
  if (!context->emit_runtime_checks) {
    return 1;
  }

  char *trap_label = ir_new_label_name(context, "trap_null");
  char *ok_label = ir_new_label_name(context, "nonnull");
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    ir_set_error(context, "Out of memory while lowering null check");
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = *value;
  branch.text = trap_label;
  if (!ir_emit(context, function, &branch)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction jump = {0};
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = ok_label;
  if (!ir_emit(context, function, &jump)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction trap = {0};
  trap.op = IR_OP_LABEL;
  trap.location = location;
  trap.text = trap_label;
  if (!ir_emit(context, function, &trap) ||
      !ir_emit_runtime_trap_ex(
          context, function, location, 1u,
          "Fatal error: Null pointer dereference", NULL, NULL)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  IRInstruction ok = {0};
  ok.op = IR_OP_LABEL;
  ok.location = location;
  ok.text = ok_label;
  if (!ir_emit(context, function, &ok)) {
    free(trap_label);
    free(ok_label);
    return 0;
  }

  free(trap_label);
  free(ok_label);
  return 1;
}

static int ir_emit_bounds_check(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *index, size_t array_size) {
  if (!context || !function || !index) {
    return 0;
  }
  if (!context->emit_runtime_checks) {
    return 1;
  }

  IROperand in_bounds = ir_operand_none();
  if (!ir_make_temp_operand(context, &in_bounds)) {
    return 0;
  }

  IRInstruction compare = {0};
  compare.op = IR_OP_BINARY;
  compare.location = location;
  compare.dest = in_bounds;
  compare.lhs = *index;
  compare.rhs = ir_operand_int((long long)array_size);
  compare.text = "<";
  if (!ir_emit(context, function, &compare)) {
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  char *trap_label = ir_new_label_name(context, "trap_bounds");
  char *ok_label = ir_new_label_name(context, "in_bounds");
  if (!trap_label || !ok_label) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    ir_set_error(context, "Out of memory while lowering bounds check");
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = in_bounds;
  branch.text = trap_label;
  if (!ir_emit(context, function, &branch)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction jump = {0};
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = ok_label;
  if (!ir_emit(context, function, &jump)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction trap = {0};
  trap.op = IR_OP_LABEL;
  trap.location = location;
  trap.text = trap_label;
  if (!ir_emit(context, function, &trap) ||
      !ir_emit_runtime_trap_ex(context, function, location, 2u,
                               "Fatal error: Array index out of bounds", index,
                               &compare.rhs)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  IRInstruction ok = {0};
  ok.op = IR_OP_LABEL;
  ok.location = location;
  ok.text = ok_label;
  if (!ir_emit(context, function, &ok)) {
    free(trap_label);
    free(ok_label);
    ir_operand_destroy(&in_bounds);
    return 0;
  }

  free(trap_label);
  free(ok_label);
  ir_operand_destroy(&in_bounds);
  return 1;
}

static int ir_push_labeled_control_frame(IRLoweringContext *context,
                                         const char *break_label,
                                         const char *continue_label,
                                         const char *user_label) {
  if (!context) {
    return 0;
  }

  if (context->control_count >= context->control_capacity) {
    size_t new_capacity =
        context->control_capacity == 0 ? 8 : context->control_capacity * 2;
    IRControlFrame *new_stack =
        realloc(context->control_stack, new_capacity * sizeof(IRControlFrame));
    if (!new_stack) {
      ir_set_error(context,
                   "Out of memory while growing IR control-flow stack");
      return 0;
    }
    context->control_stack = new_stack;
    context->control_capacity = new_capacity;
  }

  IRControlFrame *frame = &context->control_stack[context->control_count++];
  frame->break_label = break_label ? mettle_strdup(break_label) : NULL;
  frame->continue_label =
      continue_label ? mettle_strdup(continue_label) : NULL;
  frame->user_label = user_label ? mettle_strdup(user_label) : NULL;
  if ((break_label && !frame->break_label) ||
      (continue_label && !frame->continue_label) ||
      (user_label && !frame->user_label)) {
    free(frame->break_label);
    free(frame->continue_label);
    free(frame->user_label);
    frame->break_label = NULL;
    frame->continue_label = NULL;
    frame->user_label = NULL;
    context->control_count--;
    ir_set_error(context, "Out of memory while setting up control-flow labels");
    return 0;
  }
  return 1;
}

static int ir_push_control_frame(IRLoweringContext *context,
                                 const char *break_label,
                                 const char *continue_label) {
  return ir_push_labeled_control_frame(context, break_label, continue_label,
                                       NULL);
}

static void ir_pop_control_frame(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return;
  }

  IRControlFrame *frame = &context->control_stack[context->control_count - 1];
  free(frame->break_label);
  free(frame->continue_label);
  free(frame->user_label);
  frame->break_label = NULL;
  frame->continue_label = NULL;
  frame->user_label = NULL;
  context->control_count--;
}

static const char *ir_current_break_label(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return NULL;
  }
  return context->control_stack[context->control_count - 1].break_label;
}

static const char *ir_current_continue_label(IRLoweringContext *context) {
  if (!context || context->control_count == 0) {
    return NULL;
  }

  for (size_t i = context->control_count; i > 0; i--) {
    const char *label = context->control_stack[i - 1].continue_label;
    if (label) {
      return label;
    }
  }
  return NULL;
}

static const char *ir_find_labeled_break(IRLoweringContext *context,
                                         const char *user_label) {
  if (!context || !user_label) {
    return NULL;
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
      return frame->break_label;
    }
  }
  return NULL;
}

static const char *ir_find_labeled_continue(IRLoweringContext *context,
                                            const char *user_label) {
  if (!context || !user_label) {
    return NULL;
  }
  for (size_t i = context->control_count; i > 0; i--) {
    const IRControlFrame *frame = &context->control_stack[i - 1];
    if (frame->user_label && strcmp(frame->user_label, user_label) == 0) {
      return frame->continue_label;
    }
  }
  return NULL;
}

static int ir_defer_stack_push(IRLoweringContext *context, IRDeferStack *stack,
                               ASTNode *node, int is_err) {
  (void)context;
  if (!stack || !node) {
    return 0;
  }
  if (stack->count >= stack->capacity) {
    size_t new_capacity = stack->capacity == 0 ? 8 : stack->capacity * 2;
    void *grown =
        realloc(stack->entries, new_capacity * sizeof(*stack->entries));
    if (!grown) {
      return 0;
    }
    stack->entries = grown;
    stack->capacity = new_capacity;
  }
  stack->entries[stack->count].node = node;
  stack->entries[stack->count].is_err = is_err;
  stack->entries[stack->count].capture_call_name = NULL;
  stack->entries[stack->count].capture_arg_temps = NULL;
  stack->entries[stack->count].capture_arg_count = 0;
  stack->count++;
  return 1;
}

// Release a defer stack's entries along with any by-value capture metadata.
static void ir_defer_stack_free(IRDeferStack *stack) {
  if (!stack) {
    return;
  }
  for (size_t i = 0; i < stack->count; i++) {
    if (stack->entries[i].capture_arg_temps) {
      for (size_t j = 0; j < stack->entries[i].capture_arg_count; j++) {
        free(stack->entries[i].capture_arg_temps[j]);
      }
      free(stack->entries[i].capture_arg_temps);
    }
    free(stack->entries[i].capture_call_name);
  }
  free(stack->entries);
  stack->entries = NULL;
  stack->count = 0;
  stack->capacity = 0;
}

// If `defer_node` defers a direct function call with arguments, snapshot each
// argument value into a fresh temp local at the current (defer-point) position
// and record the call name + temp names for by-value replay. Returns 1 when the
// call was captured; 0 means the deferred statement should be replayed by
// re-lowering its AST (no arguments, a method/indirect call, or an argument
// whose type we cannot snapshot). Returns -1 if a real error occurred while
// emitting the snapshots (an error has been set on the context).
static int ir_defer_capture_call(IRLoweringContext *context,
                                 IRFunction *function, ASTNode *defer_node,
                                 char **out_call_name, char ***out_temps,
                                 size_t *out_count) {
  *out_call_name = NULL;
  *out_temps = NULL;
  *out_count = 0;

  DeferStatement *defer_stmt = (DeferStatement *)defer_node->data;
  if (!defer_stmt || !defer_stmt->statement ||
      defer_stmt->statement->type != AST_FUNCTION_CALL) {
    return 0;
  }
  CallExpression *call = (CallExpression *)defer_stmt->statement->data;
  if (!call || !call->function_name || call->argument_count == 0) {
    return 0;
  }
  // Method calls (with a receiver) and indirect calls keep by-reference replay.
  if (call->object || call->is_indirect_call) {
    return 0;
  }
  if (strcmp(call->function_name, "sizeof") == 0 ||
      strcmp(call->function_name, "static_assert") == 0) {
    return 0;
  }
  // Each argument needs a concrete resolved type so the snapshot local can be
  // declared with an explicit type (required by the binary backend).
  for (size_t i = 0; i < call->argument_count; i++) {
    if (!call->arguments[i] || !call->arguments[i]->resolved_type ||
        !call->arguments[i]->resolved_type->name) {
      return 0;
    }
  }

  char **temps = calloc(call->argument_count, sizeof(char *));
  if (!temps) {
    ir_set_error(context, "Out of memory capturing deferred call arguments");
    return -1;
  }

  for (size_t i = 0; i < call->argument_count; i++) {
    const char *type_name = call->arguments[i]->resolved_type->name;
    char *temp_name = ir_new_label_name(context, "defer_cap");
    if (!temp_name) {
      goto fail;
    }
    temps[i] = temp_name;

    IRInstruction decl = {0};
    decl.op = IR_OP_DECLARE_LOCAL;
    decl.location = defer_node->location;
    decl.dest = ir_operand_symbol(temp_name);
    decl.text = (char *)type_name;
    if (!decl.dest.name || !ir_emit(context, function, &decl)) {
      ir_operand_destroy(&decl.dest);
      goto fail;
    }
    ir_operand_destroy(&decl.dest);

    IROperand value = ir_operand_none();
    if (!ir_lower_expression(context, function, call->arguments[i], &value)) {
      goto fail;
    }
    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = defer_node->location;
    assign.dest = ir_operand_symbol(temp_name);
    assign.lhs = value;
    ir_assign_apply_float_bits(&assign, &assign.lhs,
                               ir_named_type_float_bits(context, type_name));
    if (!assign.dest.name || !ir_emit(context, function, &assign)) {
      ir_operand_destroy(&assign.dest);
      ir_operand_destroy(&value);
      goto fail;
    }
    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(&value);
  }

  *out_call_name = strdup(call->function_name);
  if (!*out_call_name) {
    goto fail;
  }
  *out_temps = temps;
  *out_count = call->argument_count;
  return 1;

fail:
  for (size_t i = 0; i < call->argument_count; i++) {
    free(temps[i]);
  }
  free(temps);
  return -1;
}

static int ir_emit_deferred_calls_filtered(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferStack *stack,
                                           int include_err) {
  if (!context || !function || !stack) {
    return 1;
  }
  for (size_t i = stack->count; i > 0; i--) {
    ASTNode *defer_node = stack->entries[i - 1].node;
    int is_err = stack->entries[i - 1].is_err;
    if (!include_err && is_err) {
      continue;
    }
    if (!defer_node || (defer_node->type != AST_DEFER_STATEMENT &&
                        defer_node->type != AST_ERRDEFER_STATEMENT)) {
      continue;
    }

    // By-value capture: replay the call against the argument snapshots taken at
    // the defer point instead of re-evaluating the original argument exprs.
    if (stack->entries[i - 1].capture_call_name) {
      size_t argc = stack->entries[i - 1].capture_arg_count;
      char **temps = stack->entries[i - 1].capture_arg_temps;
      IROperand dest = ir_operand_none();
      if (!ir_make_temp_operand(context, &dest)) {
        return 0;
      }
      IROperand *args = NULL;
      if (argc > 0) {
        args = calloc(argc, sizeof(IROperand));
        if (!args) {
          ir_operand_destroy(&dest);
          ir_set_error(context, "Out of memory replaying deferred call");
          return 0;
        }
        for (size_t a = 0; a < argc; a++) {
          args[a] = ir_operand_symbol(temps[a]);
        }
      }
      IRInstruction call_inst = {0};
      call_inst.op = IR_OP_CALL;
      call_inst.location = defer_node->location;
      call_inst.dest = dest;
      call_inst.text = stack->entries[i - 1].capture_call_name;
      call_inst.arguments = args;
      call_inst.argument_count = argc;
      int ok = ir_emit(context, function, &call_inst);
      for (size_t a = 0; a < argc; a++) {
        ir_operand_destroy(&args[a]);
      }
      free(args);
      ir_operand_destroy(&dest);
      if (!ok) {
        return 0;
      }
      continue;
    }

    DeferStatement *defer_stmt = (DeferStatement *)defer_node->data;
    if (!defer_stmt || !defer_stmt->statement) {
      continue;
    }
    if (!ir_lower_deferred_statement(context, function,
                                     defer_stmt->statement)) {
      return 0;
    }
  }
  return 1;
}

static int ir_lower_deferred_statement(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *statement) {
  if (!statement) {
    return 1;
  }

  IRDeferScope deferred_scope = {0};
  int ok = ir_lower_statement_with_defers(context, function, statement,
                                          &deferred_scope);
  if (ok) {
    ok = ir_emit_deferred_scopes_non_err(context, function, &deferred_scope);
  }
  ir_defer_stack_free(&deferred_scope.stack);
  return ok;
}

static int ir_lower_lvalue_address(IRLoweringContext *context,
                                   IRFunction *function, ASTNode *expression,
                                   IROperand *out_address, Type **out_type);

static int ir_expression_is_floating(IRLoweringContext *context,
                                     ASTNode *expression) {
  if (!context || !context->type_checker || !expression) {
    return 0;
  }

  Type *type = type_checker_infer_type(context->type_checker, expression);
  if (!type) {
    return 0;
  }

  return type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT64;
}

/* True only for a true 8-byte float64. The backend's "known float64" path
 * reinterprets the loaded 64 bits via `movq xmm, r64`; that is correct for
 * float64 but wrong for float32 or integer-width types, so gate strictly. */
static int ir_type_is_float64(Type *type) {
  return type && type->kind == TYPE_FLOAT64 && type->size == 8;
}

/* IEEE-754 width for a floating type: 32 for float32, otherwise 64. Callers
 * must already know the type is floating (use ir_type_is_float* / the type
 * checker). Returns 64 for NULL so non-float contexts get the safe default. */
static int ir_type_float_bits(Type *type) {
  return (type && type->kind == TYPE_FLOAT32) ? 32 : 64;
}

/* Float width for a named type (e.g. a declared variable / parameter type).
 * Returns 0 when the name does not resolve to a floating type, else 32/64. */
static int ir_named_type_float_bits(IRLoweringContext *context,
                                    const char *type_name) {
  Type *type = NULL;
  if (!context || !context->type_checker || !type_name) {
    return 0;
  }
  type = type_checker_get_type_by_name(context->type_checker, type_name);
  if (!type || (type->kind != TYPE_FLOAT32 && type->kind != TYPE_FLOAT64)) {
    return 0;
  }
  return ir_type_float_bits(type);
}

/* Stamp a freshly produced float operand with the requested IEEE-754 width.
 * No-op for non-float operands or when bits is 0. When narrowing a float64
 * literal to float32, round the constant through float so the stored bits are
 * the true single-precision value, not a truncated double pattern. */
static void ir_operand_apply_float_bits(IROperand *operand, int bits) {
  if (!operand || operand->kind != IR_OPERAND_FLOAT ||
      (bits != 32 && bits != 64)) {
    return;
  }
  if (bits == 32) {
    operand->float_value = (double)(float)operand->float_value;
  }
  operand->float_bits = bits;
}

/* Float width of a declared symbol (variable/parameter). 0 if not floating. */
static int ir_symbol_float_bits(IRLoweringContext *context, const char *name) {
  Symbol *symbol = NULL;
  if (!context || !context->symbol_table || !name) {
    return 0;
  }
  symbol = symbol_table_lookup(context->symbol_table, name);
  if (!symbol || !symbol->type ||
      (symbol->type->kind != TYPE_FLOAT32 &&
       symbol->type->kind != TYPE_FLOAT64)) {
    return 0;
  }
  return ir_type_float_bits(symbol->type);
}

/* Recover a local's declared float width (0/32/64) from the DECLARE_LOCAL the
 * lowering already emitted for it. The function-body symbol-table scope is
 * usually popped by lowering time, so symbol_table_lookup misses locals; the
 * emitted IR is the reliable record of a local's declared type name. Caller
 * should gate this on a floating RHS to avoid an O(n) scan on every assign. */
static int ir_local_declared_float_bits(IRLoweringContext *context,
                                        const IRFunction *function,
                                        const char *name) {
  if (!context || !function || !name) {
    return 0;
  }
  for (size_t i = function->instruction_count; i-- > 0;) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_DECLARE_LOCAL &&
        insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name &&
        insn->text && strcmp(insn->dest.name, name) == 0) {
      return ir_named_type_float_bits(context, insn->text);
    }
  }
  return 0;
}

/* Record, on an ASSIGN/STORE, the TARGET float precision (bits = 32/64) of the
 * destination. instruction->float_bits is the destination width; the source
 * value operand keeps its own width so the backend can detect a precision
 * mismatch (e.g. a float64 expression assigned to a float32 variable) and
 * emit the cvtsd2ss / cvtss2sd it needs. A bare float literal has no runtime
 * width, so re-round it to the target precision in place — no conversion is
 * required for it. No-op when bits is 0 (target is not floating). */
static void ir_assign_apply_float_bits(IRInstruction *instruction,
                                       IROperand *value, int bits) {
  if (!instruction || bits == 0) {
    return;
  }
  instruction->is_float = 1;
  instruction->float_bits = (bits == 32) ? 32 : 64;
  if (value && value->kind == IR_OPERAND_FLOAT) {
    ir_operand_apply_float_bits(value, instruction->float_bits);
    instruction->lhs.float_bits = value->float_bits;
  } else if (value) {
    /* Preserve the value's own width; the backend converts if it differs
     * from instruction->float_bits. */
    instruction->lhs.float_bits = value->float_bits;
  }
}

/* Mark a LOAD instruction (and its destination temp) as floating when the
 * loaded type is float32/float64, recording the width. Backends key off this
 * to pick movss/cvtss* vs movsd/cvtsd* and 4- vs 8-byte memory access. */
static void ir_load_apply_float_type(IRInstruction *load, Type *loaded_type) {
  if (!load || !loaded_type) {
    return;
  }
  if (loaded_type->kind != TYPE_FLOAT32 && loaded_type->kind != TYPE_FLOAT64) {
    return;
  }
  load->is_float = 1;
  load->float_bits = ir_type_float_bits(loaded_type);
  load->dest.float_bits = load->float_bits;
}

/* Resolve the float width of an expression via the type checker. Returns 0
 * when the expression is not floating, else 32 or 64. */
static int ir_expression_float_bits(IRLoweringContext *context,
                                    ASTNode *expression) {
  Type *type = NULL;
  if (!context || !context->type_checker || !expression) {
    return 0;
  }
  type = type_checker_infer_type(context->type_checker, expression);
  if (!type || (type->kind != TYPE_FLOAT32 && type->kind != TYPE_FLOAT64)) {
    return 0;
  }
  return ir_type_float_bits(type);
}

static int ir_binary_operator_is_comparison(const char *op) {
  return op && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0);
}

static int ir_binary_expression_operation_float_bits(IRLoweringContext *context,
                                                    ASTNode *expression,
                                                    BinaryExpression *binary) {
  int expression_bits = ir_expression_float_bits(context, expression);
  int left_bits = 0;
  int right_bits = 0;

  if (expression_bits != 0) {
    return expression_bits;
  }
  if (!binary || !ir_binary_operator_is_comparison(binary->operator)) {
    return 0;
  }

  left_bits = ir_expression_float_bits(context, binary->left);
  right_bits = ir_expression_float_bits(context, binary->right);
  if (left_bits == 64 || right_bits == 64) {
    return 64;
  }
  if (left_bits == 32 || right_bits == 32) {
    return 32;
  }
  return 0;
}

static int ir_type_storage_size(Type *type) {
  if (!type || type->size == 0) {
    return 8;
  }

  if (type->size == 1 || type->size == 2 || type->size == 4 ||
      type->size == 8) {
    return (int)type->size;
  }

  return 8;
}

/* Memory stride between consecutive elements in an array — must match
 * laid-out sizeof(element), including structs > 8 bytes. Prefer this over
 * ir_type_storage_size() for base + index * stride address math only. */
static int ir_type_array_element_stride(Type *element_type) {
  if (!element_type || element_type->size == 0 ||
      element_type->size > (size_t)INT_MAX) {
    return 8;
  }
  return (int)element_type->size;
}

static int ir_type_is_pointer(Type *type) {
  return type && type->kind == TYPE_POINTER && type->base_type;
}

static int ir_emit_binary_instruction(IRLoweringContext *context,
                                      IRFunction *function,
                                      SourceLocation location, const char *op,
                                      IROperand dest, IROperand lhs,
                                      IROperand rhs) {
  IRInstruction instruction = {0};
  instruction.op = IR_OP_BINARY;
  instruction.location = location;
  instruction.dest = dest;
  instruction.lhs = lhs;
  instruction.rhs = rhs;
  instruction.text = op;
  return ir_emit(context, function, &instruction);
}

static int ir_emit_scaled_index_offset(IRLoweringContext *context,
                                       IRFunction *function,
                                       SourceLocation location,
                                       const IROperand *index, int stride,
                                       IROperand *out_offset) {
  if (!context || !function || !index || !out_offset) {
    return 0;
  }

  if (stride == 1) {
    *out_offset = ir_clone_operand_local(index);
    return out_offset->kind != IR_OPERAND_NONE;
  }

  IROperand scaled = ir_operand_none();
  if (!ir_make_temp_operand(context, &scaled)) {
    return 0;
  }

  if (!ir_emit_binary_instruction(context, function, location, "*", scaled,
                                  *index, ir_operand_int(stride))) {
    ir_operand_destroy(&scaled);
    return 0;
  }

  *out_offset = scaled;
  return 1;
}

static int ir_try_lower_pointer_arithmetic(IRLoweringContext *context,
                                           IRFunction *function,
                                           BinaryExpression *binary,
                                           SourceLocation location,
                                           IROperand *out_value) {
  const char *op = NULL;
  Type *left_type = NULL;
  Type *right_type = NULL;
  int left_is_pointer = 0;
  int right_is_pointer = 0;

  if (!context || !function || !binary || !binary->operator || !out_value) {
    return 0;
  }

  op = binary->operator;
  if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0) {
    return 0;
  }

  left_type = ir_infer_expression_type(context, binary->left);
  right_type = ir_infer_expression_type(context, binary->right);
  if (!left_type || !right_type) {
    return 0;
  }

  left_is_pointer = ir_type_is_pointer(left_type);
  right_is_pointer = ir_type_is_pointer(right_type);
  if (!left_is_pointer && !right_is_pointer) {
    return 0;
  }

  if (strcmp(op, "+") == 0) {
    Type *pointer_type = NULL;
    ASTNode *pointer_expr = NULL;
    ASTNode *index_expr = NULL;

    if (left_is_pointer && type_checker_is_integer_type(right_type)) {
      pointer_type = left_type;
      pointer_expr = binary->left;
      index_expr = binary->right;
    } else if (right_is_pointer && type_checker_is_integer_type(left_type)) {
      pointer_type = right_type;
      pointer_expr = binary->right;
      index_expr = binary->left;
    } else {
      return 0;
    }

    IROperand base = ir_operand_none();
    IROperand index = ir_operand_none();
    IROperand offset = ir_operand_none();
    IROperand destination = ir_operand_none();
    int stride = ir_type_array_element_stride(pointer_type->base_type);

    if (!ir_lower_expression(context, function, pointer_expr, &base) ||
        !ir_lower_expression(context, function, index_expr, &index) ||
        !ir_emit_scaled_index_offset(context, function, location, &index,
                                     stride, &offset) ||
        !ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&offset);
      ir_operand_destroy(&index);
      ir_operand_destroy(&base);
      return 0;
    }

    if (!ir_emit_binary_instruction(context, function, location, "+",
                                    destination, base, offset)) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&offset);
      ir_operand_destroy(&index);
      ir_operand_destroy(&base);
      return 0;
    }

    ir_operand_destroy(&offset);
    ir_operand_destroy(&index);
    ir_operand_destroy(&base);
    *out_value = destination;
    return 1;
  }

  if (left_is_pointer && type_checker_is_integer_type(right_type)) {
    Type *pointer_type = left_type;
    IROperand base = ir_operand_none();
    IROperand index = ir_operand_none();
    IROperand offset = ir_operand_none();
    IROperand destination = ir_operand_none();
    int stride = ir_type_array_element_stride(pointer_type->base_type);

    if (!ir_lower_expression(context, function, binary->left, &base) ||
        !ir_lower_expression(context, function, binary->right, &index) ||
        !ir_emit_scaled_index_offset(context, function, location, &index,
                                     stride, &offset) ||
        !ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&offset);
      ir_operand_destroy(&index);
      ir_operand_destroy(&base);
      return 0;
    }

    if (!ir_emit_binary_instruction(context, function, location, "-",
                                    destination, base, offset)) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&offset);
      ir_operand_destroy(&index);
      ir_operand_destroy(&base);
      return 0;
    }

    ir_operand_destroy(&offset);
    ir_operand_destroy(&index);
    ir_operand_destroy(&base);
    *out_value = destination;
    return 1;
  }

  if (left_is_pointer && right_is_pointer && left_type->base_type &&
      right_type->base_type &&
      left_type->base_type->size == right_type->base_type->size &&
      left_type->base_type->kind == right_type->base_type->kind) {
    IROperand lhs = ir_operand_none();
    IROperand rhs = ir_operand_none();
    IROperand byte_diff = ir_operand_none();
    IROperand destination = ir_operand_none();
    int stride = ir_type_array_element_stride(left_type->base_type);

    if (!ir_lower_expression(context, function, binary->left, &lhs) ||
        !ir_lower_expression(context, function, binary->right, &rhs) ||
        !ir_make_temp_operand(context, &byte_diff)) {
      ir_operand_destroy(&rhs);
      ir_operand_destroy(&lhs);
      return 0;
    }

    if (!ir_emit_binary_instruction(context, function, location, "-", byte_diff,
                                    lhs, rhs)) {
      ir_operand_destroy(&byte_diff);
      ir_operand_destroy(&rhs);
      ir_operand_destroy(&lhs);
      return 0;
    }

    ir_operand_destroy(&rhs);
    ir_operand_destroy(&lhs);

    if (stride == 1) {
      *out_value = byte_diff;
      return 1;
    }

    if (!ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&byte_diff);
      return 0;
    }

    if (!ir_emit_binary_instruction(context, function, location, "/", destination,
                                    byte_diff, ir_operand_int(stride))) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&byte_diff);
      return 0;
    }

    ir_operand_destroy(&byte_diff);
    *out_value = destination;
    return 1;
  }

  return 0;
}

static int ir_make_temp_operand(IRLoweringContext *context,
                                IROperand *out_temp) {
  if (!context || !out_temp) {
    return 0;
  }

  char *temp_name = ir_new_temp_name(context);
  if (!temp_name) {
    ir_set_error(context, "Out of memory while allocating IR temp");
    return 0;
  }

  *out_temp = ir_operand_temp(temp_name);
  free(temp_name);
  if (out_temp->kind != IR_OPERAND_TEMP || !out_temp->name) {
    ir_set_error(context, "Failed to create IR temp operand");
    return 0;
  }

  return 1;
}

static int ir_emit_condition_false_branch(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *expression,
                                          const char *false_label) {
  if (!context || !function || !expression || !false_label) {
    return 0;
  }

  if (expression->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    if (binary && binary->operator && binary->left && binary->right) {
      if (strcmp(binary->operator, "&&") == 0) {
        return ir_emit_condition_false_branch(context, function, binary->left,
                                              false_label) &&
               ir_emit_condition_false_branch(context, function, binary->right,
                                              false_label);
      }

      if (strcmp(binary->operator, "||") == 0) {
        char *done_label = ir_new_label_name(context, "cond_done");
        if (!done_label) {
          ir_set_error(context, "Out of memory while allocating condition labels");
          return 0;
        }

        if (!ir_emit_condition_true_branch(context, function, binary->left,
                                           done_label) ||
            !ir_emit_condition_false_branch(context, function, binary->right,
                                            false_label) ||
            !ir_emit_label_instruction(context, function, done_label,
                                       expression->location)) {
          free(done_label);
          return 0;
        }

        free(done_label);
        return 1;
      }
    }
  }

  IROperand condition = ir_operand_none();
  if (!ir_lower_expression(context, function, expression, &condition)) {
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = expression->location;
  branch.lhs = condition;
  branch.text = (char *)false_label;
  if (!ir_emit(context, function, &branch)) {
    ir_operand_destroy(&condition);
    return 0;
  }
  ir_operand_destroy(&condition);
  return 1;
}

static int ir_emit_condition_true_branch(IRLoweringContext *context,
                                         IRFunction *function,
                                         ASTNode *expression,
                                         const char *true_label) {
  if (!context || !function || !expression || !true_label) {
    return 0;
  }

  if (expression->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    if (binary && binary->operator && binary->left && binary->right) {
      if (strcmp(binary->operator, "||") == 0) {
        return ir_emit_condition_true_branch(context, function, binary->left,
                                             true_label) &&
               ir_emit_condition_true_branch(context, function, binary->right,
                                             true_label);
      }

      if (strcmp(binary->operator, "&&") == 0) {
        char *done_label = ir_new_label_name(context, "cond_done");
        if (!done_label) {
          ir_set_error(context, "Out of memory while allocating condition labels");
          return 0;
        }

        if (!ir_emit_condition_false_branch(context, function, binary->left,
                                            done_label) ||
            !ir_emit_condition_true_branch(context, function, binary->right,
                                           true_label) ||
            !ir_emit_label_instruction(context, function, done_label,
                                       expression->location)) {
          free(done_label);
          return 0;
        }

        free(done_label);
        return 1;
      }
    }
  }

  IROperand condition = ir_operand_none();
  if (!ir_lower_expression(context, function, expression, &condition)) {
    return 0;
  }

  char *skip_label = ir_new_label_name(context, "cond_false");
  if (!skip_label) {
    ir_operand_destroy(&condition);
    ir_set_error(context, "Out of memory while allocating condition labels");
    return 0;
  }

  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = expression->location;
  branch.lhs = condition;
  branch.text = skip_label;
  if (!ir_emit(context, function, &branch) ||
      !ir_emit_jump_instruction(context, function, true_label,
                                expression->location) ||
      !ir_emit_label_instruction(context, function, skip_label,
                                 expression->location)) {
    ir_operand_destroy(&condition);
    free(skip_label);
    return 0;
  }

  ir_operand_destroy(&condition);
  free(skip_label);
  return 1;
}


static int ir_lower_call_expression(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *expression,
                                    IROperand *out_value) {
  CallExpression *call = (CallExpression *)expression->data;
  Symbol *callee_symbol = NULL;
  if (!call || !call->function_name) {
    ir_set_error(context, "Malformed call expression");
    return 0;
  }

  if (strcmp(call->function_name, "sizeof") == 0) {
    if (call->argument_count != 1 || !call->arguments ||
        !call->arguments[0] || call->arguments[0]->type != AST_IDENTIFIER) {
      ir_set_error(context, "Malformed sizeof expression");
      return 0;
    }

    Identifier *type_id = (Identifier *)call->arguments[0]->data;
    Type *type = (context->type_checker && type_id && type_id->name)
                     ? type_checker_get_type_by_name(context->type_checker,
                                                     type_id->name)
                     : NULL;
    if (!type || type->size > (size_t)LLONG_MAX) {
      ir_set_error(context, "Unable to lower sizeof expression");
      return 0;
    }

    *out_value = ir_operand_int((long long)type->size);
    return 1;
  }

  if (strcmp(call->function_name, "static_assert") == 0) {
    *out_value = ir_operand_none();
    return 1;
  }

  callee_symbol = context->symbol_table
                      ? symbol_table_lookup(context->symbol_table,
                                            call->function_name)
                      : NULL;
  if (callee_symbol &&
      callee_symbol->kind == SYMBOL_TAGGED_ENUM_CONSTRUCTOR) {
    return ir_lower_tagged_enum_constructor_call(
        context, function, expression, callee_symbol, out_value);
  }

  int is_func_ptr_var = call->is_indirect_call;

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    return 0;
  }

  IROperand *arguments = NULL;
  if (call->argument_count > 0) {
    arguments = calloc(call->argument_count, sizeof(IROperand));
    if (!arguments) {
      ir_operand_destroy(&destination);
      ir_set_error(context, "Out of memory while lowering call arguments");
      return 0;
    }
  }

  for (size_t i = 0; i < call->argument_count; i++) {
    if (!ir_lower_expression(context, function, call->arguments[i],
                             &arguments[i])) {
      for (size_t j = 0; j < i; j++) {
        ir_operand_destroy(&arguments[j]);
      }
      free(arguments);
      ir_operand_destroy(&destination);
      return 0;
    }
  }

  /* Give width-less float literal arguments the declared parameter precision
   * so a float32 parameter receives a single-precision value, not a truncated
   * double. Only direct calls expose declared parameter types. */
  if (callee_symbol && callee_symbol->kind == SYMBOL_FUNCTION) {
    size_t typed = callee_symbol->data.function.parameter_count;
    for (size_t i = 0; i < call->argument_count && i < typed; i++) {
      Type *ptype = callee_symbol->data.function.parameter_types
                        ? callee_symbol->data.function.parameter_types[i]
                        : NULL;
      if (ir_should_coerce_string_to_cstring(context, ptype,
                                             call->arguments[i])) {
        if (!ir_coerce_string_operand_to_cstring(
                context, function, &arguments[i], call->arguments[i]->location)) {
          for (size_t j = 0; j < call->argument_count; j++) {
            ir_operand_destroy(&arguments[j]);
          }
          free(arguments);
          ir_operand_destroy(&destination);
          return 0;
        }
        continue;
      }
      if (ptype && (ptype->kind == TYPE_FLOAT32 ||
                    ptype->kind == TYPE_FLOAT64)) {
        ir_operand_apply_float_bits(&arguments[i], ir_type_float_bits(ptype));
      }
    }
  }

  IRInstruction instruction = {0};
  instruction.location = expression->location;
  instruction.dest = destination;
  instruction.arguments = arguments;
  instruction.argument_count = call->argument_count;

  if (is_func_ptr_var) {
    instruction.op = IR_OP_CALL_INDIRECT;
    instruction.lhs = ir_operand_symbol(call->function_name);
    if (instruction.lhs.kind != IR_OPERAND_SYMBOL || !instruction.lhs.name) {
      ir_operand_destroy(&instruction.lhs);
      for (size_t i = 0; i < call->argument_count; i++) {
        ir_operand_destroy(&arguments[i]);
      }
      free(arguments);
      ir_operand_destroy(&destination);
      ir_set_error(context,
                   "Out of memory while lowering function pointer call");
      return 0;
    }
  } else {
    instruction.op = IR_OP_CALL;
    instruction.text = call->function_name;
  }

  if (!ir_emit(context, function, &instruction)) {
    for (size_t i = 0; i < call->argument_count; i++) {
      ir_operand_destroy(&arguments[i]);
    }
    free(arguments);
    ir_operand_destroy(&destination);
    return 0;
  }

  for (size_t i = 0; i < call->argument_count; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  free(arguments);

  *out_value = destination;
  return 1;
}

static Type *ir_infer_expression_type(IRLoweringContext *context,
                                      ASTNode *expression) {
  if (!context || !context->type_checker || !expression) {
    return NULL;
  }
  return type_checker_infer_type(context->type_checker, expression);
}

static int ir_emit_address_of_symbol(IRLoweringContext *context,
                                     IRFunction *function, const char *name,
                                     SourceLocation location,
                                     IROperand *out_address) {
  if (!context || !function || !name || !out_address) {
    return 0;
  }

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    return 0;
  }

  IROperand symbol = ir_operand_symbol(name);
  if (symbol.kind != IR_OPERAND_SYMBOL || !symbol.name) {
    ir_operand_destroy(&destination);
    ir_set_error(context, "Out of memory while lowering symbol address");
    return 0;
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_ADDRESS_OF;
  instruction.location = location;
  instruction.dest = destination;
  instruction.lhs = symbol;
  if (!ir_emit(context, function, &instruction)) {
    ir_operand_destroy(&destination);
    ir_operand_destroy(&symbol);
    return 0;
  }

  ir_operand_destroy(&symbol);
  *out_address = destination;
  return 1;
}

static int ir_lower_lvalue_address(IRLoweringContext *context,
                                   IRFunction *function, ASTNode *expression,
                                   IROperand *out_address, Type **out_type) {
  if (!context || !function || !expression || !out_address) {
    return 0;
  }

  *out_address = ir_operand_none();
  if (out_type) {
    *out_type = NULL;
  }

  switch (expression->type) {
  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      ir_set_error(context, "Malformed identifier lvalue");
      return 0;
    }

    if (out_type) {
      Symbol *symbol =
          context->symbol_table
              ? symbol_table_lookup(context->symbol_table, identifier->name)
              : NULL;

      if (symbol && symbol->kind == SYMBOL_CONSTANT) {
        ir_set_error(context, "Cannot take address of constant");
        return 0;
      }

      if (symbol && (symbol->kind == SYMBOL_VARIABLE ||
                     symbol->kind == SYMBOL_PARAMETER)) {
        *out_type = symbol->type;
      } else {
        *out_type = ir_infer_expression_type(context, expression);
      }
    }
    return ir_emit_address_of_symbol(context, function, identifier->name,
                                     expression->location, out_address);
  }

  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expression->data;
    if (!member || !member->object || !member->member) {
      ir_set_error(context, "Malformed member access lvalue");
      return 0;
    }

    IROperand object_address = ir_operand_none();
    Type *object_type = ir_infer_expression_type(context, member->object);

    if (object_type && object_type->kind == TYPE_POINTER) {
      if (!ir_lower_expression(context, function, member->object,
                               &object_address)) {
        return 0;
      }
      if (!ir_emit_null_check(context, function, expression->location,
                              &object_address)) {
        ir_operand_destroy(&object_address);
        return 0;
      }
      object_type = object_type->base_type;
    } else if (object_type && object_type->kind == TYPE_STRING) {
      // String values are represented as pointers to {chars, length} records.
      // Member access must operate on that value pointer, not on the variable's
      // stack slot address.
      if (!ir_lower_expression(context, function, member->object,
                               &object_address)) {
        return 0;
      }
    } else {
      if (!ir_lower_lvalue_address(context, function, member->object,
                                   &object_address, &object_type)) {
        return 0;
      }
    }
    if (!object_type || (object_type->kind != TYPE_STRUCT &&
                         object_type->kind != TYPE_STRING)) {
      ir_operand_destroy(&object_address);
      ir_set_error(context,
                   "Member access requires struct or string lvalue object");
      return 0;
    }

    Type *field_type = type_get_field_type(object_type, member->member);
    size_t field_offset = type_get_field_offset(object_type, member->member);
    if (!field_type || field_offset == (size_t)-1) {
      ir_operand_destroy(&object_address);
      ir_set_error(context, "Unknown struct field '%s'", member->member);
      return 0;
    }

    if (out_type) {
      *out_type = field_type;
    }

    IROperand field_address = ir_operand_none();
    if (!ir_make_temp_operand(context, &field_address)) {
      ir_operand_destroy(&object_address);
      return 0;
    }

    IRInstruction add = {0};
    add.op = IR_OP_BINARY;
    add.location = expression->location;
    add.dest = field_address;
    add.lhs = object_address;
    add.rhs = ir_operand_int((long long)field_offset);
    add.text = "+";
    if (!ir_emit(context, function, &add)) {
      ir_operand_destroy(&field_address);
      ir_operand_destroy(&object_address);
      return 0;
    }

    ir_operand_destroy(&object_address);
    *out_address = field_address;
    return 1;
  }

  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index_expression =
        (ArrayIndexExpression *)expression->data;
    if (!index_expression || !index_expression->array ||
        !index_expression->index) {
      ir_set_error(context, "Malformed index lvalue");
      return 0;
    }

    Type *array_type =
        ir_infer_expression_type(context, index_expression->array);
    if (!array_type ||
        (array_type->kind != TYPE_ARRAY && array_type->kind != TYPE_POINTER) ||
        !array_type->base_type) {
      ir_set_error(context, "Index lvalue requires array or pointer type");
      return 0;
    }

    if (out_type) {
      *out_type = array_type->base_type;
    }

    IROperand base = ir_operand_none();
    IROperand index = ir_operand_none();
    int lowered_base = 0;
    if (array_type->kind == TYPE_ARRAY) {
      // For inline arrays (including struct fields), indexing must use the
      // address of the array storage, not a loaded value.
      lowered_base = ir_lower_lvalue_address(context, function,
                                             index_expression->array, &base,
                                             NULL);
    } else {
      lowered_base =
          ir_lower_expression(context, function, index_expression->array, &base);
    }

    if (!lowered_base ||
        !ir_lower_expression(context, function, index_expression->index,
                             &index)) {
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }

    if (array_type->kind == TYPE_POINTER &&
        !ir_emit_null_check(context, function, expression->location, &base)) {
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }
    if (array_type->kind == TYPE_ARRAY &&
        !ir_emit_bounds_check(context, function, expression->location, &index,
                              array_type->array_size)) {
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }

    IROperand scaled = ir_operand_none();
    if (!ir_make_temp_operand(context, &scaled)) {
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }

    int element_size = ir_type_array_element_stride(array_type->base_type);
    IRInstruction multiply = {0};
    multiply.op = IR_OP_BINARY;
    multiply.location = expression->location;
    multiply.dest = scaled;
    multiply.lhs = index;
    multiply.rhs = ir_operand_int(element_size);
    multiply.text = "*";
    if (!ir_emit(context, function, &multiply)) {
      ir_operand_destroy(&scaled);
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }

    IROperand address = ir_operand_none();
    if (!ir_make_temp_operand(context, &address)) {
      ir_operand_destroy(&scaled);
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }

    IRInstruction add = {0};
    add.op = IR_OP_BINARY;
    add.location = expression->location;
    add.dest = address;
    add.lhs = base;
    add.rhs = scaled;
    add.text = "+";
    if (!ir_emit(context, function, &add)) {
      ir_operand_destroy(&address);
      ir_operand_destroy(&scaled);
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }

    ir_operand_destroy(&scaled);
    ir_operand_destroy(&base);
    ir_operand_destroy(&index);
    *out_address = address;
    return 1;
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    if (!unary || !unary->operator || !unary->operand ||
        strcmp(unary->operator, "*") != 0) {
      ir_set_error(context, "Unsupported unary lvalue");
      return 0;
    }

    Type *operand_type = ir_infer_expression_type(context, unary->operand);
    if (operand_type &&
        (operand_type->kind != TYPE_POINTER || !operand_type->base_type)) {
      ir_set_error(context, "Dereference lvalue requires pointer operand");
      return 0;
    }

    IROperand pointer_value = ir_operand_none();
    if (!ir_lower_expression(context, function, unary->operand,
                             &pointer_value)) {
      return 0;
    }

    if (!ir_emit_null_check(context, function, expression->location,
                            &pointer_value)) {
      ir_operand_destroy(&pointer_value);
      return 0;
    }

    if (out_type && operand_type && operand_type->kind == TYPE_POINTER &&
        operand_type->base_type) {
      *out_type = operand_type->base_type;
    }
    *out_address = pointer_value;
    return 1;
  }

  default:
    ir_set_error(context, "Expression is not assignable in IR lowering");
    return 0;
  }
}

static int ir_lower_expression(IRLoweringContext *context, IRFunction *function,
                               ASTNode *expression, IROperand *out_value) {
  if (!context || !function || !expression || !out_value) {
    return 0;
  }

  *out_value = ir_operand_none();

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal) {
      ir_set_error(context, "Malformed number literal");
      return 0;
    }
    if (literal->is_float) {
      *out_value = ir_operand_float(literal->float_value);
    } else {
      *out_value = ir_operand_int(literal->int_value);
    }
    return 1;
  }

  case AST_STRING_LITERAL: {
    StringLiteral *literal = (StringLiteral *)expression->data;
    if (!literal || !literal->value) {
      ir_set_error(context, "Malformed string literal");
      return 0;
    }
    *out_value = ir_operand_string(literal->value);
    if (!out_value->name) {
      ir_set_error(context, "Out of memory while lowering string literal");
      return 0;
    }
    return 1;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      ir_set_error(context, "Malformed identifier expression");
      return 0;
    }
    Symbol *symbol =
        context->symbol_table
            ? symbol_table_lookup(context->symbol_table, identifier->name)
            : NULL;
    if (symbol && symbol->kind == SYMBOL_CONSTANT) {
      *out_value = ir_operand_int(symbol->data.constant.value);
      return 1;
    }

    /* A bare nullary tagged-enum variant (e.g. `var a: Option = None`) names
     * a constructor symbol, not a runtime value. Construct an enum local with
     * just the tag set; payloadful variants must use call syntax `Some(x)`. */
    if (symbol && symbol->kind == SYMBOL_TAGGED_ENUM_CONSTRUCTOR &&
        symbol->data.constructor.payload_type == NULL) {
      return ir_emit_tagged_enum_construct(context, function, symbol,
                                           NULL, expression->location,
                                           out_value);
    }

    *out_value = ir_operand_symbol(identifier->name);
    if (!out_value->name) {
      ir_set_error(context, "Out of memory while lowering identifier");
      return 0;
    }
    return 1;
  }

  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    if (!binary || !binary->left || !binary->right || !binary->operator) {
      ir_set_error(context, "Malformed binary expression");
      return 0;
    }

    // Keep string concatenation in AST form for codegen. The current IR binary
    // fallback models '+' as integer arithmetic, which is invalid for string
    // records.
    if (strcmp(binary->operator, "+") == 0) {
      Type *expr_type = ir_infer_expression_type(context, expression);
      if (expr_type && expr_type->kind == TYPE_STRING) {
        IROperand destination = ir_operand_none();
        IROperand left = ir_operand_none();
        IROperand right = ir_operand_none();
        if (!ir_make_temp_operand(context, &destination)) {
          return 0;
        }
        if (!ir_lower_expression(context, function, binary->left, &left)) {
          ir_operand_destroy(&destination);
          return 0;
        }
        if (!ir_lower_expression(context, function, binary->right, &right)) {
          ir_operand_destroy(&left);
          ir_operand_destroy(&destination);
          return 0;
        }

        IRInstruction instruction = {0};
        instruction.op = IR_OP_BINARY;
        instruction.location = expression->location;
        instruction.dest = destination;
        instruction.lhs = left;
        instruction.rhs = right;
        instruction.text = binary->operator;
        instruction.ast_ref = expression;
        if (!ir_emit(context, function, &instruction)) {
          ir_operand_destroy(&right);
          ir_operand_destroy(&left);
          ir_operand_destroy(&destination);
          return 0;
        }

        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        *out_value = destination;
        return 1;
      }
    }

    if (strcmp(binary->operator, "&&") == 0 ||
        strcmp(binary->operator, "||") == 0) {
      int is_and = strcmp(binary->operator, "&&") == 0;
      IROperand destination = ir_operand_none();
      IROperand left = ir_operand_none();
      IROperand right = ir_operand_none();
      char *rhs_label = NULL;
      char *true_label = NULL;
      char *false_label = NULL;
      char *end_label = NULL;

      if (!ir_make_temp_operand(context, &destination)) {
        return 0;
      }
      if (!ir_lower_expression(context, function, binary->left, &left)) {
        ir_operand_destroy(&destination);
        return 0;
      }

      rhs_label = ir_new_label_name(context, "sc_rhs");
      true_label = ir_new_label_name(context, "sc_true");
      false_label = ir_new_label_name(context, "sc_false");
      end_label = ir_new_label_name(context, "sc_end");
      if (!rhs_label || !true_label || !false_label || !end_label) {
        ir_set_error(context,
                     "Out of memory while creating short-circuit labels");
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      IRInstruction instruction = {0};
      instruction.location = expression->location;

      instruction.op = IR_OP_BRANCH_ZERO;
      instruction.lhs = left;
      instruction.text = is_and ? false_label : rhs_label;
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      if (is_and) {
        instruction = (IRInstruction){0};
        instruction.op = IR_OP_LABEL;
        instruction.location = expression->location;
        instruction.text = rhs_label;
        if (!ir_emit(context, function, &instruction) ||
            !ir_lower_expression(context, function, binary->right, &right)) {
          free(rhs_label);
          free(true_label);
          free(false_label);
          free(end_label);
          ir_operand_destroy(&left);
          ir_operand_destroy(&destination);
          return 0;
        }

        instruction = (IRInstruction){0};
        instruction.op = IR_OP_BRANCH_ZERO;
        instruction.location = expression->location;
        instruction.lhs = right;
        instruction.text = false_label;
        if (!ir_emit(context, function, &instruction)) {
          free(rhs_label);
          free(true_label);
          free(false_label);
          free(end_label);
          ir_operand_destroy(&right);
          ir_operand_destroy(&left);
          ir_operand_destroy(&destination);
          return 0;
        }
      } else {
        instruction = (IRInstruction){0};
        instruction.op = IR_OP_JUMP;
        instruction.location = expression->location;
        instruction.text = true_label;
        if (!ir_emit(context, function, &instruction)) {
          free(rhs_label);
          free(true_label);
          free(false_label);
          free(end_label);
          ir_operand_destroy(&left);
          ir_operand_destroy(&destination);
          return 0;
        }

        instruction = (IRInstruction){0};
        instruction.op = IR_OP_LABEL;
        instruction.location = expression->location;
        instruction.text = rhs_label;
        if (!ir_emit(context, function, &instruction) ||
            !ir_lower_expression(context, function, binary->right, &right)) {
          free(rhs_label);
          free(true_label);
          free(false_label);
          free(end_label);
          ir_operand_destroy(&left);
          ir_operand_destroy(&destination);
          return 0;
        }

        instruction = (IRInstruction){0};
        instruction.op = IR_OP_BRANCH_ZERO;
        instruction.location = expression->location;
        instruction.lhs = right;
        instruction.text = false_label;
        if (!ir_emit(context, function, &instruction)) {
          free(rhs_label);
          free(true_label);
          free(false_label);
          free(end_label);
          ir_operand_destroy(&right);
          ir_operand_destroy(&left);
          ir_operand_destroy(&destination);
          return 0;
        }
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_LABEL;
      instruction.location = expression->location;
      instruction.text = true_label;
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_ASSIGN;
      instruction.location = expression->location;
      instruction.dest = destination;
      instruction.lhs = ir_operand_int(1);
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_JUMP;
      instruction.location = expression->location;
      instruction.text = end_label;
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_LABEL;
      instruction.location = expression->location;
      instruction.text = false_label;
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_ASSIGN;
      instruction.location = expression->location;
      instruction.dest = destination;
      instruction.lhs = ir_operand_int(0);
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      instruction = (IRInstruction){0};
      instruction.op = IR_OP_LABEL;
      instruction.location = expression->location;
      instruction.text = end_label;
      if (!ir_emit(context, function, &instruction)) {
        free(rhs_label);
        free(true_label);
        free(false_label);
        free(end_label);
        ir_operand_destroy(&right);
        ir_operand_destroy(&left);
        ir_operand_destroy(&destination);
        return 0;
      }

      free(rhs_label);
      free(true_label);
      free(false_label);
      free(end_label);
      ir_operand_destroy(&right);
      ir_operand_destroy(&left);
      *out_value = destination;
      return 1;
    }

    if (ir_try_lower_pointer_arithmetic(context, function, binary,
                                        expression->location, out_value)) {
      return 1;
    }

    IROperand left = ir_operand_none();
    IROperand right = ir_operand_none();
    if (!ir_lower_expression(context, function, binary->left, &left) ||
        !ir_lower_expression(context, function, binary->right, &right)) {
      ir_operand_destroy(&left);
      ir_operand_destroy(&right);
      return 0;
    }

    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&left);
      ir_operand_destroy(&right);
      return 0;
    }

    IRInstruction instruction = {0};
    instruction.op = IR_OP_BINARY;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.lhs = left;
    instruction.rhs = right;
    instruction.text = binary->operator;
    int operation_float_bits = ir_binary_expression_operation_float_bits(
        context, expression, binary);
    instruction.is_float = operation_float_bits != 0;
    if (instruction.is_float) {
      instruction.float_bits = operation_float_bits;
      if (!ir_binary_operator_is_comparison(binary->operator)) {
        instruction.dest.float_bits = instruction.float_bits;
        destination.float_bits = instruction.float_bits;
      }
    }

    if (!ir_emit(context, function, &instruction)) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&left);
      ir_operand_destroy(&right);
      return 0;
    }

    ir_operand_destroy(&left);
    ir_operand_destroy(&right);
    *out_value = destination;
    return 1;
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    if (!unary || !unary->operator || !unary->operand) {
      ir_set_error(context, "Malformed unary expression");
      return 0;
    }


    if (strcmp(unary->operator, "&") == 0) {
      Type *target_type = NULL;
      if (!ir_lower_lvalue_address(context, function, unary->operand, out_value,
                                   &target_type)) {
        return 0;
      }
      return 1;
    }

    if (strcmp(unary->operator, "*") == 0) {
      IROperand address = ir_operand_none();
      Type *target_type = NULL;
      if (!ir_lower_lvalue_address(context, function, expression, &address,
                                   &target_type)) {
        return 0;
      }
      if (!target_type) {
        ir_operand_destroy(&address);
        ir_set_error(context, "Cannot dereference unknown type");
        return 0;
      }

      IROperand destination = ir_operand_none();
      if (!ir_make_temp_operand(context, &destination)) {
        ir_operand_destroy(&address);
        return 0;
      }

      IRInstruction load = {0};
      load.op = IR_OP_LOAD;
      load.location = expression->location;
      load.dest = destination;
      load.lhs = address;
      load.rhs = ir_operand_int(ir_type_storage_size(target_type));
      ir_load_apply_float_type(&load, target_type);
      if (!ir_emit(context, function, &load)) {
        ir_operand_destroy(&destination);
        ir_operand_destroy(&address);
        return 0;
      }
      destination.float_bits = load.dest.float_bits;

      ir_operand_destroy(&address);
      *out_value = destination;
      return 1;
    }

    IROperand operand = ir_operand_none();
    if (!ir_lower_expression(context, function, unary->operand, &operand)) {
      return 0;
    }

    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&operand);
      return 0;
    }

    IRInstruction instruction = {0};
    instruction.op = IR_OP_UNARY;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.lhs = operand;
    instruction.text = unary->operator;
    instruction.is_float = ir_expression_is_floating(context, expression);
    if (instruction.is_float) {
      instruction.float_bits = ir_expression_float_bits(context, expression);
      if (instruction.float_bits == 0) {
        instruction.float_bits = 64;
      }
      instruction.dest.float_bits = instruction.float_bits;
      destination.float_bits = instruction.float_bits;
    }

    if (!ir_emit(context, function, &instruction)) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&operand);
      return 0;
    }

    ir_operand_destroy(&operand);
    *out_value = destination;
    return 1;
  }

  case AST_MEMBER_ACCESS: {
    MemberAccess *m = (MemberAccess *)expression->data;
    /* Qualified enum variant: `EnumName.Variant` lowers to either an integer
     * constant (plain enum) or a tagged-enum construction (tagged enum). */
    if (m && m->object && m->object->type == AST_IDENTIFIER && m->member) {
      Identifier *obj_id = (Identifier *)m->object->data;
      if (obj_id && obj_id->name && context->symbol_table) {
        Symbol *enum_sym =
            symbol_table_lookup(context->symbol_table, obj_id->name);
        if (enum_sym && enum_sym->kind == SYMBOL_ENUM) {
          Symbol *variant_sym =
              symbol_table_lookup(context->symbol_table, m->member);
          if (variant_sym && variant_sym->kind == SYMBOL_CONSTANT) {
            *out_value = ir_operand_int(variant_sym->data.constant.value);
            return 1;
          }
          if (variant_sym &&
              variant_sym->kind == SYMBOL_TAGGED_ENUM_CONSTRUCTOR &&
              variant_sym->data.constructor.payload_type == NULL) {
            return ir_emit_tagged_enum_construct(context, function, variant_sym,
                                                 NULL, expression->location,
                                                 out_value);
          }
        }
      }
    }
    /* Fall through to the lvalue-load path for struct/array member access. */
  }
  /* fallthrough */
  case AST_INDEX_EXPRESSION: {
    IROperand address = ir_operand_none();
    Type *value_type = NULL;
    if (!ir_lower_lvalue_address(context, function, expression, &address,
                                 &value_type)) {
      return 0;
    }
    if (!value_type) {
      ir_operand_destroy(&address);
      ir_set_error(context, "Cannot determine type for load");
      return 0;
    }

    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&address);
      return 0;
    }

    IRInstruction load = {0};
    load.op = IR_OP_LOAD;
    load.location = expression->location;
    load.dest = destination;
    load.lhs = address;
    load.rhs = ir_operand_int(ir_type_storage_size(value_type));
    ir_load_apply_float_type(&load, value_type);
    if (!ir_emit(context, function, &load)) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&address);
      return 0;
    }
    destination.float_bits = load.dest.float_bits;

    ir_operand_destroy(&address);
    *out_value = destination;
    return 1;
  }

  case AST_NEW_EXPRESSION: {
    NewExpression *new_expression = (NewExpression *)expression->data;
    if (!new_expression || !new_expression->type_name) {
      ir_set_error(context, "Invalid new expression");
      return 0;
    }

    Type *allocated_type = NULL;
    if (context->type_checker) {
      /*
       * Prefer the already-resolved expression type: `new T` infers to `T*`,
       * and using that avoids scope-sensitive type-name lookups here.
       */
      Type *new_expr_type =
          type_checker_infer_type(context->type_checker, expression);
      if (new_expr_type && new_expr_type->kind == TYPE_POINTER) {
        allocated_type = new_expr_type->base_type;
      }
      if (!allocated_type) {
        allocated_type = type_checker_get_type_by_name(context->type_checker,
                                                       new_expression->type_name);
      }
    }
    /*
     * Allocation must use the full concrete type size.
     * ir_type_storage_size() intentionally normalizes many operations to
     * register-width storage, which is incorrect for `new` on structs/arrays.
     */
    int allocation_size =
        (allocated_type && allocated_type->size > 0 &&
         allocated_type->size <= (size_t)INT_MAX)
            ? (int)allocated_type->size
            : 8;

    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      return 0;
    }

    IRInstruction instruction = {0};
    instruction.op = IR_OP_NEW;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.rhs = ir_operand_int(allocation_size);
    instruction.text = new_expression->type_name;
    if (!ir_emit(context, function, &instruction)) {
      ir_operand_destroy(&destination);
      return 0;
    }

    *out_value = destination;
    return 1;
  }

  case AST_CAST_EXPRESSION: {
    CastExpression *cast_expr = (CastExpression *)expression->data;
    if (!cast_expr || !cast_expr->type_name || !cast_expr->operand) {
      ir_set_error(context, "Invalid cast expression");
      return 0;
    }

    IROperand operand = ir_operand_none();
    if (!ir_lower_expression(context, function, cast_expr->operand, &operand)) {
      return 0;
    }

    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      ir_operand_destroy(&operand);
      return 0;
    }

    IRInstruction instruction = {0};
    instruction.op = IR_OP_CAST;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.lhs = operand;
    instruction.text = cast_expr->type_name;
    instruction.is_float =
        ir_expression_is_floating(context, cast_expr->operand);
    if (instruction.is_float) {
      /* float_bits on a CAST records the SOURCE operand width so the backend
       * can pick cvttss2si/cvtss2sd (f32) vs cvttsd2si (f64). The TARGET
       * width is resolved separately from instruction->text. */
      instruction.float_bits =
          ir_expression_float_bits(context, cast_expr->operand);
      if (instruction.float_bits == 0) {
        instruction.float_bits = 64;
      }
    }
    {
      /* Tag the destination with the target float width so a value produced
       * by e.g. (float32)x is recognized as float32 by later consumers. */
      int target_bits =
          ir_named_type_float_bits(context, cast_expr->type_name);
      if (target_bits) {
        instruction.dest.float_bits = target_bits;
        destination.float_bits = target_bits;
      }
    }

    if (!ir_emit(context, function, &instruction)) {
      ir_operand_destroy(&destination);
      ir_operand_destroy(&operand);
      return 0;
    }

    ir_operand_destroy(&operand);
    *out_value = destination;
    return 1;
  }

  case AST_FUNCTION_CALL:
    return ir_lower_call_expression(context, function, expression, out_value);

  case AST_FUNC_PTR_CALL: {
    FuncPtrCall *fp_call = (FuncPtrCall *)expression->data;
    if (!fp_call || !fp_call->function) {
      ir_set_error(context, "Invalid function pointer call");
      return 0;
    }

    IROperand destination = ir_operand_none();
    if (!ir_make_temp_operand(context, &destination)) {
      return 0;
    }

    IROperand func_ptr = ir_operand_none();
    if (!ir_lower_expression(context, function, fp_call->function, &func_ptr)) {
      ir_operand_destroy(&destination);
      return 0;
    }

    IROperand *arguments = NULL;
    if (fp_call->argument_count > 0) {
      arguments = calloc(fp_call->argument_count, sizeof(IROperand));
      if (!arguments) {
        ir_operand_destroy(&func_ptr);
        ir_operand_destroy(&destination);
        ir_set_error(
            context,
            "Out of memory while lowering function pointer call arguments");
        return 0;
      }
    }

    for (size_t i = 0; i < fp_call->argument_count; i++) {
      if (!ir_lower_expression(context, function, fp_call->arguments[i],
                               &arguments[i])) {
        for (size_t j = 0; j < i; j++) {
          ir_operand_destroy(&arguments[j]);
        }
        free(arguments);
        ir_operand_destroy(&func_ptr);
        ir_operand_destroy(&destination);
        return 0;
      }
    }

    Type *func_type = ir_infer_expression_type(context, fp_call->function);
    if (func_type && func_type->kind == TYPE_FUNCTION_POINTER &&
        func_type->fn_param_types) {
      for (size_t i = 0; i < fp_call->argument_count &&
                         i < func_type->fn_param_count;
           i++) {
        if (ir_should_coerce_string_to_cstring(
                context, func_type->fn_param_types[i], fp_call->arguments[i]) &&
            !ir_coerce_string_operand_to_cstring(
                context, function, &arguments[i],
                fp_call->arguments[i]->location)) {
          for (size_t j = 0; j < fp_call->argument_count; j++) {
            ir_operand_destroy(&arguments[j]);
          }
          free(arguments);
          ir_operand_destroy(&func_ptr);
          ir_operand_destroy(&destination);
          return 0;
        }
      }
    }

    IRInstruction instruction = {0};
    instruction.op = IR_OP_CALL_INDIRECT;
    instruction.location = expression->location;
    instruction.dest = destination;
    // For indirect calls, we use lhs to hold the function pointer operand
    instruction.lhs = func_ptr;
    instruction.arguments = arguments;
    instruction.argument_count = fp_call->argument_count;

    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < fp_call->argument_count; i++) {
        ir_operand_destroy(&arguments[i]);
      }
      free(arguments);
      ir_operand_destroy(&func_ptr);
      ir_operand_destroy(&destination);
      return 0;
    }

    for (size_t i = 0; i < fp_call->argument_count; i++) {
      ir_operand_destroy(&arguments[i]);
    }
    free(arguments);
    ir_operand_destroy(&func_ptr);

    *out_value = destination;
    return 1;
  }

  case AST_MATCH_STATEMENT:
    return ir_lower_match_expression(context, function, expression,
                                     out_value);

  default:
    ir_set_error(context, "Unsupported expression type in pure IR lowering");
    return 0;
  }
}

static int ir_lower_statement_with_defers(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *statement,
                                          IRDeferScope *defers) {
  if (!context || !function || !statement) {
    return 0;
  }

  switch (statement->type) {
  case AST_PROGRAM: {
    Program *program = (Program *)statement->data;
    if (!program) {
      return 1;
    }
    if (!defers) {
      for (size_t i = 0; i < program->declaration_count; i++) {
        if (!ir_lower_statement_with_defers(context, function,
                                            program->declarations[i], NULL)) {
          return 0;
        }
      }
      return 1;
    }

    IRDeferScope block_scope = {0};
    block_scope.parent = defers;
    for (size_t i = 0; i < program->declaration_count; i++) {
      if (!ir_lower_statement_with_defers(
              context, function, program->declarations[i], &block_scope)) {
        ir_defer_stack_free(&block_scope.stack);
        return 0;
      }
    }

    int ok =
        ir_emit_deferred_calls_non_err(context, function, &block_scope.stack);
    ir_defer_stack_free(&block_scope.stack);
    return ok;
  }

  case AST_VAR_DECLARATION: {
    VarDeclaration *declaration = (VarDeclaration *)statement->data;
    if (!declaration || !declaration->name) {
      ir_set_error(context, "Malformed variable declaration");
      return 0;
    }

    // Top-level `const` is folded at use sites (SYMBOL_CONSTANT) and never
    // reaches this local-statement path. A local `const` is an immutable local
    // variable: it gets normal storage and initialization here, and the type
    // checker rejects reassignment.

    IRInstruction local = {0};
    local.op = IR_OP_DECLARE_LOCAL;
    local.location = statement->location;
    local.dest = ir_operand_symbol(declaration->name);
    local.text = declaration->type_name;
    // For inferred-type locals (`var x = expr;`) the declaration carries no
    // type_name. The binary/direct-object backend resolves a local's type from
    // this textual payload, so fall back to the name of the type the checker
    // inferred for the initializer. The Type (and its name) outlives codegen,
    // matching the lifetime of the type_name pointer used above, and `text` is
    // never freed by the IR. Leaving it NULL is harmless for the asm backend.
    if (!local.text && declaration->initializer &&
        declaration->initializer->resolved_type) {
      local.text = declaration->initializer->resolved_type->name;
    }
    if (!local.dest.name) {
      ir_set_error(context,
                   "Out of memory while lowering variable declaration");
      return 0;
    }
    if (!ir_emit(context, function, &local)) {
      ir_operand_destroy(&local.dest);
      return 0;
    }
    ir_operand_destroy(&local.dest);

    if (declaration->initializer) {
      IROperand value = ir_operand_none();
      if (!ir_lower_expression(context, function, declaration->initializer,
                               &value)) {
        return 0;
      }
      Type *decl_type = ir_resolve_named_type(context, declaration->type_name);
      if (!decl_type) {
        decl_type = declaration->initializer
                        ? declaration->initializer->resolved_type
                        : NULL;
      }
      if (ir_should_coerce_string_to_cstring(context, decl_type,
                                             declaration->initializer) &&
          !ir_coerce_string_operand_to_cstring(
              context, function, &value, declaration->initializer->location)) {
        ir_operand_destroy(&value);
        return 0;
      }
      if (ir_try_emit_aggregate_symbol_memcpy(context, function,
                                              declaration->name, &value,
                                              decl_type, statement->location)) {
        ir_operand_destroy(&value);
      } else {
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = statement->location;
        assign.dest = ir_operand_symbol(declaration->name);
        assign.lhs = value;
        ir_assign_apply_float_bits(
            &assign, &assign.lhs,
            ir_named_type_float_bits(context, declaration->type_name));
        if (!assign.dest.name) {
          ir_operand_destroy(&value);
          ir_set_error(context,
                       "Out of memory while lowering variable initializer");
          return 0;
        }
        if (!ir_emit(context, function, &assign)) {
          ir_operand_destroy(&assign.dest);
          ir_operand_destroy(&value);
          return 0;
        }
        ir_operand_destroy(&assign.dest);
        ir_operand_destroy(&value);
      }
    }
    return 1;
  }

  case AST_ASSIGNMENT: {
    Assignment *assignment = (Assignment *)statement->data;
    if (!assignment || !assignment->value) {
      ir_set_error(context, "Malformed assignment statement");
      return 0;
    }

    IROperand value = ir_operand_none();
    if (!ir_lower_expression(context, function, assignment->value, &value)) {
      return 0;
    }

    if (assignment->variable_name) {
      Type *assign_type =
          ir_lookup_symbol_type(context, assignment->variable_name);
      if (!assign_type && assignment->value) {
        assign_type = assignment->value->resolved_type;
      }
      if (ir_should_coerce_string_to_cstring(context, assign_type,
                                             assignment->value) &&
          !ir_coerce_string_operand_to_cstring(
              context, function, &value, assignment->value->location)) {
        ir_operand_destroy(&value);
        return 0;
      }
      if (ir_try_emit_aggregate_symbol_memcpy(
              context, function, assignment->variable_name, &value,
              assign_type, statement->location)) {
        ir_operand_destroy(&value);
        return 1;
      }

      {
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = statement->location;
        assign.dest = ir_operand_symbol(assignment->variable_name);
        assign.lhs = value;
        /* Target float width for the narrowing/widening on store. Prefer the
         * symbol table, but its function scope is usually popped by lowering
         * time, so fall back to the declared type recorded on the emitted
         * DECLARE_LOCAL. Without this, a float64 expression assigned into a
         * float32 local kept the float64 width and stored the wrong 4 bytes.
         * Gate the IR scan on a floating RHS so non-float assigns stay O(1). */
        int target_float_bits =
            ir_symbol_float_bits(context, assignment->variable_name);
        if (target_float_bits == 0 && assignment->value &&
            assignment->value->resolved_type &&
            (assignment->value->resolved_type->kind == TYPE_FLOAT32 ||
             assignment->value->resolved_type->kind == TYPE_FLOAT64)) {
          target_float_bits = ir_local_declared_float_bits(
              context, function, assignment->variable_name);
        }
        ir_assign_apply_float_bits(&assign, &assign.lhs, target_float_bits);
        if (!assign.dest.name) {
          ir_operand_destroy(&value);
          ir_set_error(context, "Out of memory while lowering assignment target");
          return 0;
        }

        if (!ir_emit(context, function, &assign)) {
          ir_operand_destroy(&assign.dest);
          ir_operand_destroy(&value);
          return 0;
        }

        ir_operand_destroy(&assign.dest);
        ir_operand_destroy(&value);
        return 1;
      }
    }

    if (!assignment->target) {
      ir_operand_destroy(&value);
      ir_set_error(context, "Assignment target is missing");
      return 0;
    }

    IROperand address = ir_operand_none();
    Type *target_type = NULL;
    if (!ir_lower_lvalue_address(context, function, assignment->target,
                                 &address, &target_type)) {
      ir_operand_destroy(&value);
      return 0;
    }

    if (!target_type) {
      ir_operand_destroy(&address);
      ir_operand_destroy(&value);
      ir_set_error(context, "Cannot assign to unknown target type");
      return 0;
    }

    if (ir_should_coerce_string_to_cstring(context, target_type,
                                           assignment->value) &&
        !ir_coerce_string_operand_to_cstring(
            context, function, &value, assignment->value->location)) {
      ir_operand_destroy(&address);
      ir_operand_destroy(&value);
      return 0;
    }

    IRInstruction store = {0};
    store.op = IR_OP_STORE;
    store.location = statement->location;
    store.dest = address;
    store.lhs = value;
    store.rhs = ir_operand_int(ir_type_storage_size(target_type));
    if (target_type->kind == TYPE_FLOAT32 ||
        target_type->kind == TYPE_FLOAT64) {
      ir_assign_apply_float_bits(&store, &store.lhs,
                                 ir_type_float_bits(target_type));
    }
    if (!ir_emit(context, function, &store)) {
      ir_operand_destroy(&address);
      ir_operand_destroy(&value);
      return 0;
    }

    ir_operand_destroy(&address);
    ir_operand_destroy(&value);
    return 1;
  }

  case AST_FUNCTION_CALL: {
    IROperand ignored = ir_operand_none();
    int ok = ir_lower_expression(context, function, statement, &ignored);
    ir_operand_destroy(&ignored);
    return ok;
  }

  case AST_RETURN_STATEMENT: {
    ReturnStatement *ret = (ReturnStatement *)statement->data;
    IROperand value = ir_operand_none();
    if (ret && ret->value) {
      if (!ir_lower_expression(context, function, ret->value, &value)) {
        return 0;
      }
      Type *return_type =
          ir_resolve_named_type(context, context->current_return_type_name);
      if (ir_should_coerce_string_to_cstring(context, return_type,
                                             ret->value) &&
          !ir_coerce_string_operand_to_cstring(context, function, &value,
                                               ret->value->location)) {
        ir_operand_destroy(&value);
        return 0;
      }
    }
    if (!ir_emit_return_with_defers(context, function, defers, &value,
                                    statement->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
    ir_operand_destroy(&value);
    return 1;
  }

  case AST_INLINE_ASM: {
    InlineAsm *inline_asm = (InlineAsm *)statement->data;
    if (!inline_asm || !inline_asm->assembly_code) {
      ir_set_error(context, "Malformed inline assembly statement");
      return 0;
    }
    IRInstruction instruction = {0};
    instruction.op = IR_OP_INLINE_ASM;
    instruction.location = statement->location;
    instruction.text = inline_asm->assembly_code;
    return ir_emit(context, function, &instruction);
  }

  case AST_IF_STATEMENT: {
    IfStatement *if_data = (IfStatement *)statement->data;
    if (!if_data || !if_data->condition || !if_data->then_branch) {
      ir_set_error(context, "Malformed if statement");
      return 0;
    }

    char *end_label = ir_new_label_name(context, "if_end");
    if (!end_label) {
      ir_set_error(context, "Out of memory while allocating if labels");
      return 0;
    }

    ASTNode *current_cond = if_data->condition;
    ASTNode *current_body = if_data->then_branch;

    for (size_t i = 0; i <= if_data->else_if_count; i++) {
      char *next_label = ir_new_label_name(context, "if_next");
      if (!next_label) {
        free(end_label);
        return 0;
      }

      if (!ir_emit_condition_false_branch(context, function, current_cond,
                                          next_label)) {
        free(next_label);
        free(end_label);
        return 0;
      }

      if (!ir_lower_statement_with_defers(context, function, current_body,
                                          defers)) {
        free(next_label);
        free(end_label);
        return 0;
      }

      if (!ir_emit_jump_instruction(context, function, end_label,
                                    current_cond->location)) {
        free(next_label);
        free(end_label);
        return 0;
      }

      if (!ir_emit_label_instruction(context, function, next_label,
                                     current_cond->location)) {
        free(next_label);
        free(end_label);
        return 0;
      }
      free(next_label);

      if (i < if_data->else_if_count) {
        current_cond = if_data->else_ifs[i].condition;
        current_body = if_data->else_ifs[i].body;
      }
    }

    if (if_data->else_branch &&
        !ir_lower_statement_with_defers(context, function, if_data->else_branch,
                                        defers)) {
      free(end_label);
      return 0;
    }

    if (!ir_emit_label_instruction(context, function, end_label,
                                   statement->location)) {
      free(end_label);
      return 0;
    }

    free(end_label);
    return 1;
  }

  case AST_WHILE_STATEMENT: {
    WhileStatement *while_data = (WhileStatement *)statement->data;
    if (!while_data || !while_data->condition || !while_data->body) {
      ir_set_error(context, "Malformed while statement");
      return 0;
    }

    char *loop_start = ir_new_label_name(context, "while");
    char *loop_end = ir_new_label_name(context, "while_end");
    if (!loop_start || !loop_end) {
      free(loop_start);
      free(loop_end);
      ir_set_error(context, "Out of memory while allocating while labels");
      return 0;
    }

    if (!ir_emit_label_instruction(context, function, loop_start,
                                   statement->location)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    if (!ir_emit_condition_false_branch(context, function,
                                        while_data->condition, loop_end)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    if (!ir_push_labeled_control_frame(context, loop_end, loop_start,
                                       while_data->label)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    int body_ok = ir_lower_statement_with_defers(context, function,
                                                 while_data->body, defers);
    ir_pop_control_frame(context);
    if (!body_ok) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    if (!ir_emit_jump_instruction(context, function, loop_start,
                                  statement->location) ||
        !ir_emit_label_instruction(context, function, loop_end,
                                   statement->location)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    free(loop_start);
    free(loop_end);
    return 1;
  }

  case AST_FOR_STATEMENT: {
    ForStatement *for_data = (ForStatement *)statement->data;
    if (!for_data || !for_data->body) {
      ir_set_error(context, "Malformed for statement");
      return 0;
    }

    char *condition_label = ir_new_label_name(context, "for_cond");
    char *step_label = ir_new_label_name(context, "for_step");
    char *end_label = ir_new_label_name(context, "for_end");
    if (!condition_label || !step_label || !end_label) {
      free(condition_label);
      free(step_label);
      free(end_label);
      ir_set_error(context, "Out of memory while allocating for-loop labels");
      return 0;
    }

    if (!ir_lower_statement_or_expression(context, function,
                                          for_data->initializer)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (!ir_emit_label_instruction(context, function, condition_label,
                                   statement->location)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (for_data->condition) {
      if (!ir_emit_condition_false_branch(context, function,
                                          for_data->condition, end_label)) {
        free(condition_label);
        free(step_label);
        free(end_label);
        return 0;
      }
    }

    if (!ir_push_labeled_control_frame(context, end_label, step_label,
                                       for_data->label)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    int body_ok = ir_lower_statement_with_defers(context, function,
                                                 for_data->body, defers);
    ir_pop_control_frame(context);
    if (!body_ok) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (!ir_emit_label_instruction(context, function, step_label,
                                   statement->location)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (!ir_lower_statement_or_expression(context, function,
                                          for_data->increment)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (!ir_emit_jump_instruction(context, function, condition_label,
                                  statement->location) ||
        !ir_emit_label_instruction(context, function, end_label,
                                   statement->location)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    free(condition_label);
    free(step_label);
    free(end_label);
    return 1;
  }

  case AST_SWITCH_STATEMENT:
    return ir_lower_switch_statement(context, function, statement);

  case AST_MATCH_STATEMENT: {
    MatchStatement *m = (MatchStatement *)statement->data;
    if (m && m->is_expression) {
      // match used as an expression-statement: lower it and discard the value.
      IROperand discarded = ir_operand_none();
      int r = ir_lower_match_expression(context, function, statement,
                                        &discarded);
      ir_operand_destroy(&discarded);
      return r;
    }
    return ir_lower_match_statement(context, function, statement, defers);
  }

  case AST_BREAK_STATEMENT: {
    LoopControlStatement *ctrl = (LoopControlStatement *)statement->data;
    const char *user_label = ctrl ? ctrl->target_label : NULL;
    const char *target = user_label ? ir_find_labeled_break(context, user_label)
                                    : ir_current_break_label(context);
    if (!target) {
      if (user_label) {
        ir_set_error(context, "'break %s' has no matching labeled loop",
                     user_label);
      } else {
        ir_set_error(context, "'break' used outside loop/switch");
      }
      return 0;
    }
    return ir_emit_jump_instruction(context, function, target,
                                    statement->location);
  }

  case AST_CONTINUE_STATEMENT: {
    LoopControlStatement *ctrl = (LoopControlStatement *)statement->data;
    const char *user_label = ctrl ? ctrl->target_label : NULL;
    const char *target = user_label
                             ? ir_find_labeled_continue(context, user_label)
                             : ir_current_continue_label(context);
    if (!target) {
      if (user_label) {
        ir_set_error(context, "'continue %s' has no matching labeled loop",
                     user_label);
      } else {
        ir_set_error(context, "'continue' used outside loop");
      }
      return 0;
    }
    return ir_emit_jump_instruction(context, function, target,
                                    statement->location);
  }

  case AST_DEFER_STATEMENT: {
    if (!defers) {
      return 1;
    }
    // Snapshot argument values now so the deferred call captures them by value
    // rather than re-reading the variables at scope exit.
    char *cap_name = NULL;
    char **cap_temps = NULL;
    size_t cap_count = 0;
    int captured = ir_defer_capture_call(context, function, statement,
                                         &cap_name, &cap_temps, &cap_count);
    if (captured < 0) {
      return 0;
    }
    if (!ir_defer_stack_push(context, &defers->stack, statement, 0)) {
      for (size_t i = 0; i < cap_count; i++) {
        free(cap_temps[i]);
      }
      free(cap_temps);
      free(cap_name);
      ir_set_error(context, "Out of memory while recording defer statement");
      return 0;
    }
    if (captured > 0) {
      size_t idx = defers->stack.count - 1;
      defers->stack.entries[idx].capture_call_name = cap_name;
      defers->stack.entries[idx].capture_arg_temps = cap_temps;
      defers->stack.entries[idx].capture_arg_count = cap_count;
    }
    return 1;
  }

  case AST_ERRDEFER_STATEMENT: {
    if (!defers) {
      return 1;
    }
    if (!ir_defer_stack_push(context, &defers->stack, statement, 1)) {
      ir_set_error(context, "Out of memory while recording errdefer statement");
      return 0;
    }
    return 1;
  }

  default: {
    if (statement->type >= AST_IDENTIFIER &&
        statement->type <= AST_NEW_EXPRESSION) {
      IROperand ignored = ir_operand_none();
      int ok = ir_lower_expression(context, function, statement, &ignored);
      ir_operand_destroy(&ignored);
      return ok;
    }

    ir_set_error(context, "Unsupported statement type in pure IR lowering");
    return 0;
  }
  }
}

static IRFunction *ir_lower_function(IRLoweringContext *context,
                                     ASTNode *declaration) {
  if (!declaration || declaration->type != AST_FUNCTION_DECLARATION) {
    return NULL;
  }

  FunctionDeclaration *function_data = (FunctionDeclaration *)declaration->data;
  if (!function_data || !function_data->name) {
    ir_set_error(context, "Malformed function declaration");
    return NULL;
  }

  context->current_return_type_name = function_data->return_type;
  context->current_function_name = function_data->name;
  mettle_compiler_ctx_set_function_name(function_data->name);

  IRFunction *function = ir_function_create(function_data->name);
  if (!function) {
    ir_set_error(context, "Out of memory while creating IR function");
    return NULL;
  }
  if (!ir_function_set_parameters(function,
                                  (const char **)function_data->parameter_names,
                                  (const char **)function_data->parameter_types,
                                  function_data->parameter_count)) {
    ir_set_error(context,
                 "Out of memory while recording IR function parameters");
    ir_function_destroy(function);
    return NULL;
  }

  char *entry_label = ir_new_label_name(context, "entry");
  if (!entry_label) {
    ir_set_error(context,
                 "Out of memory while allocating function entry label");
    ir_function_destroy(function);
    return NULL;
  }
  if (!ir_emit_label_instruction(context, function, entry_label,
                                 declaration->location)) {
    free(entry_label);
    ir_function_destroy(function);
    return NULL;
  }
  free(entry_label);

  IRDeferScope defers = {0};
  if (function_data->body &&
      !ir_lower_statement_with_defers(context, function, function_data->body,
                                      &defers)) {
    ir_defer_stack_free(&defers.stack);
    ir_function_destroy(function);
    return NULL;
  }

  // Ensure fall-off path runs defers too by emitting a return if none exists.
  if (function->instruction_count == 0 ||
      function->instructions[function->instruction_count - 1].op !=
          IR_OP_RETURN) {
    IROperand implicit_value = ir_operand_none();
    if (!ir_emit_return_with_defers(context, function, &defers, &implicit_value,
                                    declaration->location)) {
      ir_operand_destroy(&implicit_value);
      ir_defer_stack_free(&defers.stack);
      ir_function_destroy(function);
      return NULL;
    }
    ir_operand_destroy(&implicit_value);
  }

  ir_defer_stack_free(&defers.stack);
  if (!ir_function_rebuild_cfg(function)) {
    ir_set_error(context, "Out of memory while building IR control-flow graph");
    ir_function_destroy(function);
    return NULL;
  }
  return function;
}

IRProgram *ir_lower_program(ASTNode *program, TypeChecker *type_checker,
                            SymbolTable *symbol_table, char **error_message,
                            int emit_runtime_checks) {
  if (error_message) {
    *error_message = NULL;
  }

  if (!program || program->type != AST_PROGRAM) {
    if (error_message) {
      *error_message =
          mettle_strdup("Expected AST_PROGRAM root for IR lowering");
    }
    return NULL;
  }

  IRProgram *ir_program = ir_program_create();
  if (!ir_program) {
    if (error_message) {
      *error_message = mettle_strdup("Failed to allocate IR program");
    }
    return NULL;
  }

  IRLoweringContext context = {0};
  context.type_checker = type_checker;
  context.symbol_table = symbol_table;
  context.emit_runtime_checks = emit_runtime_checks ? 1 : 0;

  Program *program_data = (Program *)program->data;
  if (!program_data) {
    return ir_program;
  }

  for (size_t i = 0; i < program_data->declaration_count; i++) {
    ASTNode *declaration = program_data->declarations[i];
    if (!declaration || declaration->type != AST_FUNCTION_DECLARATION) {
      continue;
    }
    FunctionDeclaration *function_data =
        (FunctionDeclaration *)declaration->data;
    if (!function_data) {
      ir_set_error(&context, "Malformed function declaration");
      ir_program_destroy(ir_program);
      for (size_t j = 0; j < context.control_count; j++) {
        free(context.control_stack[j].break_label);
        free(context.control_stack[j].continue_label);
        free(context.control_stack[j].user_label);
      }
      free(context.control_stack);
      if (error_message) {
        *error_message = context.error_message
                             ? context.error_message
                             : mettle_strdup("Unknown IR lowering error");
      } else {
        free(context.error_message);
      }
      return NULL;
    }
    if (!function_data->body) {
      continue;
    }

    IRFunction *function = ir_lower_function(&context, declaration);
    if (!function) {
      if (!context.error_message) {
        ir_set_error(&context, "Failed to lower function declaration to IR");
      }
      ir_program_destroy(ir_program);
      for (size_t j = 0; j < context.control_count; j++) {
        free(context.control_stack[j].break_label);
        free(context.control_stack[j].continue_label);
        free(context.control_stack[j].user_label);
      }
      free(context.control_stack);
      if (error_message) {
        *error_message = context.error_message
                             ? context.error_message
                             : mettle_strdup("Unknown IR lowering error");
      } else {
        free(context.error_message);
      }
      return NULL;
    }

    if (!ir_program_add_function(ir_program, function)) {
      ir_function_destroy(function);
      ir_set_error(&context, "Out of memory while appending IR function");
      ir_program_destroy(ir_program);
      for (size_t j = 0; j < context.control_count; j++) {
        free(context.control_stack[j].break_label);
        free(context.control_stack[j].continue_label);
        free(context.control_stack[j].user_label);
      }
      free(context.control_stack);
      if (error_message) {
        *error_message = context.error_message
                             ? context.error_message
                             : mettle_strdup("Unknown IR lowering error");
      } else {
        free(context.error_message);
      }
      return NULL;
    }
  }

  for (size_t j = 0; j < context.control_count; j++) {
    free(context.control_stack[j].break_label);
    free(context.control_stack[j].continue_label);
    free(context.control_stack[j].user_label);
  }
  free(context.control_stack);

  if (context.error_message) {
    if (error_message) {
      *error_message = context.error_message;
    } else {
      free(context.error_message);
    }
  }

  return ir_program;
}
