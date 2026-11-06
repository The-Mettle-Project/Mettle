#include "ir_lowering_internal.h"
#include "../semantic/ownership.h"
#include "../string_intern.h"

static int ir_own_is_place(const ASTNode *node) {
  if (!node) {
    return 0;
  }
  if (node->type == AST_IDENTIFIER || node->type == AST_MEMBER_ACCESS ||
      node->type == AST_INDEX_EXPRESSION) {
    return 1;
  }
  return node->type == AST_UNARY_EXPRESSION && node->data &&
         ((UnaryExpression *)node->data)->operator &&
         strcmp(((UnaryExpression *)node->data)->operator, "*") == 0;
}

static Type *ir_own_strip(Type *type) {
  while (type && type->refined_base) {
    type = type->refined_base;
  }
  return type;
}

static size_t ir_own_step_offset(Type *base, const OwnDropStep *step) {
  size_t offset = 0;
  Type *type = ir_own_strip(base);
  for (int i = 0; i < step->depth && type; i++) {
    int field = step->path[i];
    if (field < 0 || (size_t)field >= type->field_count) {
      return offset;
    }
    offset += type->field_offsets ? type->field_offsets[field] : 0;
    type = ir_own_strip(type->field_types[field]);
  }
  return offset;
}

static int ir_own_call(IRLoweringContext *context, IRFunction *function,
                       const char *callee, const IROperand *address,
                       SourceLocation location, ASTNode *drop_call) {
  IROperand arguments[1];
  IRInstruction call = {0};
  arguments[0] = *address;
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = (char *)string_intern(callee);
  call.arguments = arguments;
  call.argument_count = 1;
  call.ast_ref = drop_call;
  return ir_emit(context, function, &call);
}

static int ir_own_emit_step(IRLoweringContext *context, IRFunction *function,
                            const char *callee, const IROperand *base,
                            size_t offset, int guard,
                            SourceLocation location, ASTNode *drop_call) {
  IROperand address = ir_operand_none();
  char *skip = NULL;
  int ok = 1;
  if (!ir_emit_address_with_offset(context, function, base, offset, location,
                                   &address)) {
    return 0;
  }
  if (guard >= 0 && ownership_bit_name(guard)) {
    IRInstruction branch = {0};
    skip = ir_new_label_name(context, "drop_skip");
    branch.op = IR_OP_BRANCH_ZERO;
    branch.location = location;
    branch.lhs = ir_operand_symbol(ownership_bit_name(guard));
    branch.text = skip;
    ok = skip && branch.lhs.name && ir_emit(context, function, &branch);
    ir_operand_destroy(&branch.lhs);
  }
  ok = ok &&
       ir_own_call(context, function, callee, &address, location, drop_call);
  ir_operand_destroy(&address);
  if (skip) {
    ok = ok && ir_emit_label_instruction(context, function, skip, location);
    free(skip);
  }
  return ok;
}

static int ir_own_materialize(IRLoweringContext *context, IRFunction *function,
                              ASTNode *value_node, Type *type,
                              IROperand *out_address) {
  char *name = ir_new_label_name(context, "dropped");
  IROperand value = ir_operand_none();
  int ok = 0;
  if (!name || !type || !type->name ||
      !ir_emit_local_declaration(context, function, name, type->name,
                                 value_node->location)) {
    free(name);
    return 0;
  }
  if (value_node->type == AST_AGGREGATE_LITERAL) {
    ok = ir_emit_aggregate_literal_copy_to_symbol(
        context, function, name, value_node, type, value_node->location);
  } else if (ir_lower_expression(context, function, value_node, &value)) {
    ok = ir_try_emit_aggregate_symbol_memcpy(context, function, name, &value,
                                             type, value_node->location);
    if (!ok) {
      IRInstruction take = {0};
      take.op = IR_OP_ASSIGN;
      take.location = value_node->location;
      take.dest = ir_operand_symbol(name);
      take.lhs = value;
      ok = take.dest.name && ir_emit(context, function, &take);
      ir_operand_destroy(&take.dest);
    }
    ir_operand_destroy(&value);
  }
  ok = ok && ir_emit_address_of_symbol(context, function, name,
                                       value_node->location, out_address);
  free(name);
  return ok;
}

static int ir_own_discard(IRLoweringContext *context, IRFunction *function,
                          ASTNode *value_node) {
  IROperand ignored = ir_operand_none();
  int ok = ir_lower_expression(context, function, value_node, &ignored);
  ir_operand_destroy(&ignored);
  return ok;
}

static void ir_own_note(const ASTNode *expression, const OwnDropStep *steps,
                        size_t count, const char *first_callee) {
  char text[256];
  if (count == 0) {
    ownership_note_compiled(expression,
                            "no code at this exit: the value had moved");
    return;
  }
  snprintf(text, sizeof(text), "%zu call%s to %s%s", count,
           count == 1 ? "" : "s", first_callee ? first_callee : "?",
           steps[0].guard >= 0 ? ", behind a live bit" : "");
  ownership_note_compiled(expression, text);
}

int ir_lower_drop_call(IRLoweringContext *context, IRFunction *function,
                       ASTNode *expression, IROperand *out_value) {
  CallExpression *call = (CallExpression *)expression->data;
  ASTNode *arg = call && call->argument_count == 1 ? call->arguments[0]
                                                   : NULL;
  Type *type = arg ? arg->resolved_type : NULL;
  const ASTNode *exit = context->lowering_deferred ? context->ownership_exit
                                                   : NULL;
  const OwnDropPlan *plan = NULL;
  OwnDropStep whole;
  const OwnDropStep *steps = &whole;
  size_t count = 1;
  IROperand base = ir_operand_none();
  const char *first = NULL;
  SourceLocation at = exit && exit->location.line > 0 ? exit->location
                                                      : expression->location;
  *out_value = ir_operand_none();
  if (!arg) {
    ir_set_error(context, "drop takes one value");
    return 0;
  }
  if (!type) {
    type = ir_infer_expression_type(context, arg);
  }
  if (strcmp(call->function_name, "forget") == 0 ||
      !ownership_type_owns(context->type_checker, type)) {
    return ir_own_is_place(arg) ? 1 : ir_own_discard(context, function, arg);
  }
  plan = ownership_drop_plan(expression, exit);
  if (!plan && ownership_call_is_planned(expression)) {
    ir_set_error(context,
                 "internal error: no drop plan for this exit from line %zu",
                 expression->location.line);
    return 0;
  }
  memset(&whole, 0, sizeof(whole));
  whole.type = type;
  whole.guard = -1;
  if (plan) {
    steps = plan->steps;
    count = plan->step_count;
  }
  if (count == 0) {
    if (!plan || !plan->unreachable) {
      ir_own_note(expression, steps, 0, NULL);
    }
    return 1;
  }
  if (!(ir_own_is_place(arg)
            ? ir_lower_lvalue_address(context, function, arg, &base, NULL)
            : ir_own_materialize(context, function, arg, type, &base))) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    const char *callee =
        ownership_drop_callee(context->type_checker, steps[i].type);
    char held[400];
    if (!callee) {
      continue;
    }
    snprintf(held, sizeof(held), "%s", callee);
    first = first ? first : string_intern(held);
    if (!ir_own_emit_step(context, function, held, &base,
                          ir_own_step_offset(type, &steps[i]), steps[i].guard,
                          at, expression)) {
      ir_operand_destroy(&base);
      return 0;
    }
  }
  ir_operand_destroy(&base);
  ir_own_note(expression, steps, count, first);
  return 1;
}

int ir_ownership_after_unit(IRLoweringContext *context, IRFunction *function,
                            const void *unit, SourceLocation location) {
  size_t count = 0;
  const OwnBitWrite *writes = ownership_unit_writes(unit, &count);
  for (size_t i = 0; i < count; i++) {
    IRInstruction assign = {0};
    const char *name = ownership_bit_name(writes[i].bit);
    if (!name) {
      continue;
    }
    assign.op = IR_OP_ASSIGN;
    assign.location = location;
    assign.dest = ir_operand_symbol(name);
    assign.lhs = ir_operand_int(writes[i].value);
    if (!assign.dest.name || !ir_emit(context, function, &assign)) {
      ir_operand_destroy(&assign.dest);
      return 0;
    }
    ir_operand_destroy(&assign.dest);
  }
  return 1;
}

int ir_ownership_function_entry(IRLoweringContext *context,
                                IRFunction *function, ASTNode *declaration) {
  size_t count = 0;
  const int *bits = ownership_function_bits(declaration, &count);
  context->current_function_node = declaration;
  for (size_t i = 0; i < count; i++) {
    const char *name = ownership_bit_name(bits[i]);
    IRInstruction assign = {0};
    if (!name || !ir_emit_local_declaration(context, function, name, "int32",
                                            declaration->location)) {
      return 0;
    }
    assign.op = IR_OP_ASSIGN;
    assign.location = declaration->location;
    assign.dest = ir_operand_symbol(name);
    assign.lhs = ir_operand_int(0);
    if (!assign.dest.name || !ir_emit(context, function, &assign)) {
      ir_operand_destroy(&assign.dest);
      return 0;
    }
    ir_operand_destroy(&assign.dest);
  }
  return ir_ownership_after_unit(context, function, declaration,
                                 declaration->location);
}

void ir_ownership_capture_defer_root(IRLoweringContext *context,
                                     ASTNode *defer_statement,
                                     const char **root_name,
                                     long *root_index) {
  DeferStatement *defer = (DeferStatement *)defer_statement->data;
  ASTNode *statement = defer ? defer->statement : NULL;
  CallExpression *call = statement && statement->type == AST_FUNCTION_CALL
                             ? (CallExpression *)statement->data
                             : NULL;
  ASTNode *root = NULL;
  *root_name = NULL;
  *root_index = -1;
  if (!call || call->object || call->argument_count != 1 ||
      !call->function_name ||
      (strcmp(call->function_name, "drop") != 0 &&
       strcmp(call->function_name, "forget") != 0)) {
    return;
  }
  root = call->arguments[0];
  while (root && root->type == AST_MEMBER_ACCESS) {
    root = ((MemberAccess *)root->data)->object;
  }
  if (!root || root->type != AST_IDENTIFIER || !root->data) {
    return;
  }
  *root_name = ((Identifier *)root->data)->name;
  *root_index = ir_local_binding_index(context, *root_name);
}
