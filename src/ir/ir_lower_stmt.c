#include "ir_lowering_internal.h"
#include "frontend/mtlc_frontend.h"
#include "string_intern.h"

static const char *ir_quiesce_helper_name(void) {
  return g_ir_lowering_live ? "mettle_live_quiesce" : "mettle_swap_apply";
}

static void ir_declare_swap_apply_helper(IRLoweringContext *context) {
  IRModuleSymbol entry = {0};
  if (!context || !context->program || !context->type_checker) {
    return;
  }
  if (ir_program_lookup_symbol(context->program, ir_quiesce_helper_name())) {
    return;
  }
  entry.name = (char *)ir_quiesce_helper_name();
  entry.kind = IR_MODSYM_FUNCTION;
  entry.is_extern = 1;
  entry.type = mtlc_type_from_frontend(context->type_checker->builtin_int64);
  entry.return_type = entry.type;
  entry.param_types = NULL;
  entry.param_count = 0;
  ir_program_add_symbol(context->program, &entry);
}

static int ir_lower_multi_return_value(IRLoweringContext *context,
                                       IRFunction *function,
                                       ReturnStatement *return_statement,
                                       IROperand *out_value,
                                       SourceLocation location) {
  Type *tuple_type = ir_resolve_named_type(context,
                                           context->current_return_type_name);
  char *tuple_name = NULL;

  if (!tuple_type || tuple_type->kind != TYPE_STRUCT ||
      !return_statement || return_statement->value_count != tuple_type->field_count) {
    ir_set_error(context, "Malformed multiple return value");
    return 0;
  }
  tuple_name = ir_new_label_name(context, "multi_return_value");
  if (!tuple_name ||
      !ir_emit_local_declaration(context, function, tuple_name,
                                 tuple_type->name, location)) {
    free(tuple_name);
    return 0;
  }

  for (size_t i = 0; i < tuple_type->field_count; i++) {
    Type *field_type = tuple_type->field_types[i];
    ASTNode *source = return_statement->values[i];
    IROperand component = ir_operand_none();
    IROperand base = ir_operand_none();
    IROperand field_address = ir_operand_none();
    IRInstruction add = {0};
    IRInstruction store = {0};

    if (field_type && field_type->kind == TYPE_ARRAY) {
      ir_set_error(context,
                   "An array cannot be one of several return values; return a "
                   "pointer to it, or wrap it in a struct");
      free(tuple_name);
      return 0;
    }
    if (!field_type ||
        !ir_lower_expression(context, function, source, &component) ||
        !ir_emit_address_of_symbol(context, function, tuple_name, location,
                                   &base) ||
        !ir_make_temp_operand(context, &field_address)) {
      ir_operand_destroy(&component);
      ir_operand_destroy(&base);
      ir_operand_destroy(&field_address);
      free(tuple_name);
      return 0;
    }
    add.op = IR_OP_BINARY;
    add.location = location;
    add.dest = field_address;
    add.lhs = base;
    add.rhs = ir_operand_int((long long)tuple_type->field_offsets[i]);
    add.text = "+";
    if (!ir_emit(context, function, &add)) {
      ir_operand_destroy(&component);
      ir_operand_destroy(&field_address);
      ir_operand_destroy(&base);
      free(tuple_name);
      return 0;
    }
    if (ir_try_emit_aggregate_address_memcpy(context, function, &field_address,
                                             &component, field_type,
                                             location)) {
      ir_operand_destroy(&component);
      ir_operand_destroy(&field_address);
      ir_operand_destroy(&base);
      continue;
    }
    store.op = IR_OP_STORE;
    store.location = location;
    store.dest = field_address;
    store.lhs = component;
    store.rhs = ir_operand_int(ir_type_storage_size(field_type));
    ir_access_apply_alias_class(&store, field_type);
    if (field_type->kind == TYPE_FLOAT32 || field_type->kind == TYPE_FLOAT64 ||
        field_type->kind == TYPE_FLOAT16 || field_type->kind == TYPE_BFLOAT16) {
      ir_assign_apply_float_bits(&store, &store.lhs,
                                 ir_type_float_bits(field_type));
    }
    if (!ir_emit(context, function, &store)) {
      ir_operand_destroy(&component);
      ir_operand_destroy(&field_address);
      ir_operand_destroy(&base);
      free(tuple_name);
      return 0;
    }
    ir_operand_destroy(&component);
    ir_operand_destroy(&field_address);
    ir_operand_destroy(&base);
  }

  *out_value = ir_operand_symbol(tuple_name);
  free(tuple_name);
  return out_value->name != NULL;
}

static int ir_lower_multi_assignment(IRLoweringContext *context,
                                      IRFunction *function,
                                      Assignment *assignment,
                                      SourceLocation location) {
  Type *tuple_type = assignment && assignment->value
                         ? assignment->value->resolved_type
                         : NULL;
  IROperand value = ir_operand_none();
  char *tuple_name = NULL;
  int ok = 0;

  if (!assignment || assignment->target_count < 2 || !assignment->value ||
      !tuple_type || tuple_type->kind != TYPE_STRUCT ||
      tuple_type->field_count != assignment->target_count) {
    ir_set_error(context, "Malformed multiple return assignment");
    return 0;
  }
  if (!ir_lower_expression(context, function, assignment->value, &value)) {
    return 0;
  }

  tuple_name = ir_new_label_name(context, "multi_return");
  if (!tuple_name ||
      !ir_emit_local_declaration(context, function, tuple_name,
                                 tuple_type->name, location) ||
      (!ir_try_emit_aggregate_symbol_memcpy(context, function, tuple_name,
                                             &value, tuple_type, location) &&
       !ir_emit_symbol_assignment(context, function, tuple_name, &value,
                                  location))) {
    ir_operand_destroy(&value);
    free(tuple_name);
    return 0;
  }

  for (size_t i = 0; i < assignment->target_count; i++) {
    ASTNode *target = assignment->targets[i];
    Identifier *identifier = target ? (Identifier *)target->data : NULL;
    Type *field_type = tuple_type->field_types[i];
    IROperand base = ir_operand_none();
    IROperand field_address = ir_operand_none();
    IROperand field_value = ir_operand_none();
    IRInstruction add = {0};
    IRInstruction load = {0};
    IRInstruction store = {0};

    if (!target || target->type != AST_IDENTIFIER || !identifier ||
        !identifier->name || !field_type ||
        field_type->kind == TYPE_ARRAY) {
      ir_set_error(context,
                   "This multiple-return assignment needs a plain variable for "
                   "each value, and no value may be an array");
      goto cleanup;
    }
    if (!ir_emit_address_of_symbol(context, function, tuple_name, location,
                                   &base) ||
        !ir_make_temp_operand(context, &field_address)) {
      goto cleanup;
    }
    add.op = IR_OP_BINARY;
    add.location = location;
    add.dest = field_address;
    add.lhs = base;
    add.rhs = ir_operand_int((long long)tuple_type->field_offsets[i]);
    add.text = "+";
    if (!ir_emit(context, function, &add) ||
        !ir_make_temp_operand(context, &field_value)) {
      goto cleanup;
    }
    if (field_type->size != 0 && field_type->size != 1 &&
        field_type->size != 2 && field_type->size != 4 &&
        field_type->size != 8 && field_type->size <= (size_t)INT_MAX) {
      IROperand target_address = ir_operand_none();
      IRInstruction copy = {0};
      int copied = 0;

      if (!ir_emit_address_of_symbol(context, function,
                                     ir_local_ir_name(context,
                                                      identifier->name),
                                     location, &target_address)) {
        goto cleanup;
      }
      copy.op = IR_OP_STORE;
      copy.location = location;
      copy.dest = target_address;
      copy.lhs = field_address;
      copy.rhs = ir_operand_int((long long)field_type->size);
      copied = ir_emit(context, function, &copy);
      ir_operand_destroy(&target_address);
      if (!copied) {
        goto cleanup;
      }
      ir_operand_destroy(&field_value);
      ir_operand_destroy(&field_address);
      ir_operand_destroy(&base);
      continue;
    }
    load.op = IR_OP_LOAD;
    load.location = location;
    load.dest = field_value;
    load.lhs = field_address;
    load.rhs = ir_operand_int(ir_type_storage_size(field_type));
    ir_load_apply_float_type(&load, field_type);
    ir_load_apply_unsigned(&load, field_type);
    ir_access_apply_alias_class(&load, field_type);
    if (!ir_emit(context, function, &load)) {
      goto cleanup;
    }
    field_value.float_bits = load.dest.float_bits;

    store.op = IR_OP_ASSIGN;
    store.location = location;
    store.dest = ir_operand_symbol(
        ir_local_ir_name(context, identifier->name));
    store.lhs = field_value;
    {
      const IRLocalBinding *binding =
          ir_local_binding_find(context, identifier->name);
      int target_bits =
          binding ? ir_named_type_float_bits(context, binding->type_text)
                  : ir_symbol_float_bits(context, identifier->name);
      ir_assign_apply_float_bits(&store, &store.lhs, target_bits);
    }
    if (!store.dest.name || !ir_emit(context, function, &store)) {
      goto cleanup;
    }
    ir_operand_destroy(&store.dest);
    ir_operand_destroy(&field_value);
    ir_operand_destroy(&field_address);
    ir_operand_destroy(&base);
    continue;

  cleanup:
    ir_operand_destroy(&store.dest);
    ir_operand_destroy(&field_value);
    ir_operand_destroy(&field_address);
    ir_operand_destroy(&base);
    ir_operand_destroy(&value);
    free(tuple_name);
    return 0;
  }

  ok = 1;
  ir_operand_destroy(&value);
  free(tuple_name);
  return ok;
}

static int ir_expression_names(const ASTNode *node, const char *target) {
  if (!node || !target) {
    return node ? 1 : 0;
  }

  switch (node->type) {
  case AST_IDENTIFIER: {
    const Identifier *identifier = (const Identifier *)node->data;
    return identifier && identifier->name &&
           strcmp(identifier->name, target) == 0;
  }
  case AST_NUMBER_LITERAL:
  case AST_STRING_LITERAL:
    return 0;
  case AST_BINARY_EXPRESSION: {
    const BinaryExpression *binary = (const BinaryExpression *)node->data;
    return !binary || ir_expression_names(binary->left, target) ||
           ir_expression_names(binary->right, target);
  }
  case AST_UNARY_EXPRESSION: {
    const UnaryExpression *unary = (const UnaryExpression *)node->data;
    return !unary || ir_expression_names(unary->operand, target);
  }
  case AST_MEMBER_ACCESS: {
    const MemberAccess *member = (const MemberAccess *)node->data;
    return !member || ir_expression_names(member->object, target);
  }
  case AST_INDEX_EXPRESSION: {
    const ArrayIndexExpression *index =
        (const ArrayIndexExpression *)node->data;
    return !index || ir_expression_names(index->array, target) ||
           ir_expression_names(index->index, target);
  }
  case AST_CAST_EXPRESSION: {
    const CastExpression *cast = (const CastExpression *)node->data;
    return !cast || ir_expression_names(cast->operand, target);
  }
  case AST_FUNCTION_CALL: {
    const CallExpression *call = (const CallExpression *)node->data;
    if (!call) {
      return 1;
    }
    if (call->object && ir_expression_names(call->object, target)) {
      return 1;
    }
    for (size_t i = 0; i < call->argument_count; i++) {
      if (ir_expression_names(call->arguments[i], target)) {
        return 1;
      }
    }
    return 0;
  }
  default:
    return 1;
  }
}

static int ir_lvalue_byte_range(const ASTNode *target, const char *root_name,
                                Type *root_type, size_t *offset_out,
                                size_t *size_out, Type **type_out) {
  if (!target || !root_name || !root_type) {
    return 0;
  }

  switch (target->type) {
  case AST_IDENTIFIER: {
    const Identifier *identifier = (const Identifier *)target->data;
    if (!identifier || !identifier->name ||
        strcmp(identifier->name, root_name) != 0) {
      return 0;
    }
    *offset_out = 0;
    *size_out = root_type->size;
    *type_out = root_type;
    return 1;
  }
  case AST_MEMBER_ACCESS: {
    const MemberAccess *member = (const MemberAccess *)target->data;
    size_t base_offset = 0;
    size_t base_size = 0;
    Type *base_type = NULL;
    if (!member || !member->member ||
        !ir_lvalue_byte_range(member->object, root_name, root_type,
                              &base_offset, &base_size, &base_type) ||
        !base_type || base_type->kind != TYPE_STRUCT ||
        !base_type->field_names || !base_type->field_offsets ||
        !base_type->field_types) {
      return 0;
    }
    for (size_t i = 0; i < base_type->field_count; i++) {
      if (!base_type->field_names[i] ||
          strcmp(base_type->field_names[i], member->member) != 0) {
        continue;
      }
      Type *field_type = base_type->field_types[i];
      if (!field_type || field_type->size == 0) {
        return 0;
      }
      *offset_out = base_offset + base_type->field_offsets[i];
      *size_out = field_type->size;
      *type_out = field_type;
      return 1;
    }
    return 0;
  }
  case AST_INDEX_EXPRESSION: {
    const ArrayIndexExpression *index =
        (const ArrayIndexExpression *)target->data;
    size_t base_offset = 0;
    size_t base_size = 0;
    Type *base_type = NULL;
    if (!index || !index->index ||
        index->index->type != AST_NUMBER_LITERAL ||
        !ir_lvalue_byte_range(index->array, root_name, root_type, &base_offset,
                              &base_size, &base_type) ||
        !base_type || base_type->kind != TYPE_ARRAY || !base_type->base_type ||
        base_type->base_type->size == 0) {
      return 0;
    }
    const NumberLiteral *literal = (const NumberLiteral *)index->index->data;
    if (!literal || literal->is_float || literal->int_value < 0 ||
        (size_t)literal->int_value >= base_type->array_size) {
      return 0;
    }
    *offset_out =
        base_offset + (size_t)literal->int_value * base_type->base_type->size;
    *size_out = base_type->base_type->size;
    *type_out = base_type->base_type;
    return 1;
  }
  default:
    return 0;
  }
}

static int ir_zero_fill_is_dead(IRLoweringContext *context, const char *name,
                                Type *type) {
  if (!context || !name || !type || type->size == 0 || type->size > 4096u ||
      !context->block_statements ||
      context->block_statement_index >= context->block_statement_count) {
    return 0;
  }

  unsigned char *covered = calloc(type->size, 1);
  size_t remaining = type->size;
  int dead = 0;

  if (!covered) {
    return 0;
  }

  for (size_t i = context->block_statement_index + 1;
       i < context->block_statement_count && !dead; i++) {
    ASTNode *statement = context->block_statements[i];
    if (!statement) {
      break;
    }

    if (statement->type == AST_VAR_DECLARATION) {
      const VarDeclaration *declaration =
          (const VarDeclaration *)statement->data;
      if (!declaration || !declaration->name ||
          strcmp(declaration->name, name) == 0 ||
          ir_expression_names(declaration->initializer, name)) {
        break;
      }
      continue;
    }

    if (statement->type != AST_ASSIGNMENT) {
      if (ir_expression_names(statement, name)) {
        break;
      }
      continue;
    }

    const Assignment *assignment = (const Assignment *)statement->data;
    if (!assignment || assignment->target_count > 0) {
      break;
    }
    if (ir_expression_names(assignment->value, name)) {
      break;
    }

    size_t offset = 0;
    size_t width = 0;
    Type *written_type = NULL;
    if (assignment->target) {
      if (!ir_expression_names(assignment->target, name)) {
        continue;
      }
      if (!ir_lvalue_byte_range(assignment->target, name, type, &offset, &width,
                                &written_type)) {
        break;
      }
    } else if (assignment->variable_name &&
               strcmp(assignment->variable_name, name) == 0) {
      offset = 0;
      width = type->size;
    } else {
      continue;
    }

    if (offset > type->size || width > type->size - offset) {
      break;
    }
    for (size_t b = offset; b < offset + width; b++) {
      if (!covered[b]) {
        covered[b] = 1;
        remaining--;
      }
    }
    dead = remaining == 0;
  }

  free(covered);
  return dead;
}

static int ir_lower_gpu_launch(IRLoweringContext *context,
                               IRFunction *function, ASTNode *statement);

int ir_store_named_local_value(IRLoweringContext *context, IRFunction *function,
                               const char *local_name, Type *local_type,
                               IROperand *value, SourceLocation location) {
  if (ir_try_emit_aggregate_symbol_memcpy(context, function, local_name, value,
                                          local_type, location)) {
    return 1;
  }
  if (ir_small_float_local(context, function, local_name, local_name,
                           local_type)) {
    return ir_emit_small_float_home_store(context, function, local_name,
                                          local_type, value, location);
  }
  IRInstruction assign = {0};
  assign.op = IR_OP_ASSIGN;
  assign.location = location;
  assign.dest = ir_operand_symbol(local_name);
  assign.lhs = *value;
  ir_assign_apply_float_bits(&assign, &assign.lhs,
                             ir_named_type_float_bits(context, local_type->name));
  if (!assign.dest.name) {
    ir_set_error(context, "Out of memory while storing a value");
    return 0;
  }
  int ok = ir_emit(context, function, &assign);
  ir_operand_destroy(&assign.dest);
  return ok;
}

static int ir_lower_var_initializer(IRLoweringContext *context,
                                    IRFunction *function,
                                    VarDeclaration *declaration,
                                    Type *decl_type, const char *local_name,
                                    ASTNode *statement) {
  IROperand value = ir_operand_none();
  if (!ir_lower_expression(context, function, declaration->initializer,
                           &value)) {
    return 0;
  }
  if (!ir_normalize_to_bool(context, function, decl_type,
                            declaration->initializer, &value,
                            declaration->initializer->location)) {
    ir_operand_destroy(&value);
    return 0;
  }
  if (ir_should_decay_array_to_address(decl_type,
                                       declaration->initializer) &&
      !ir_decay_array_operand_to_address(
          context, function, &value, declaration->initializer->location)) {
    ir_operand_destroy(&value);
    return 0;
  }
  if (ir_should_build_slice_from_array(decl_type,
                                       declaration->initializer) &&
      !ir_build_slice_operand_from_array(
          context, function, &value,
          declaration->initializer->resolved_type, decl_type,
          declaration->initializer->location)) {
    ir_operand_destroy(&value);
    return 0;
  }
  if (ir_should_coerce_string_to_cstring(context, decl_type,
                                         declaration->initializer) &&
      !ir_coerce_string_operand_to_cstring(
          context, function, &value, declaration->initializer->location)) {
    ir_operand_destroy(&value);
    return 0;
  }
  if (ir_try_emit_aggregate_symbol_memcpy(context, function,
                                          local_name, &value,
                                          decl_type, statement->location)) {
    ir_operand_destroy(&value);
  } else if (ir_small_float_local(context, function, declaration->name,
                                  local_name, decl_type)) {
    int stored = ir_emit_small_float_home_store(
        context, function, local_name, decl_type, &value,
        statement->location);
    ir_operand_destroy(&value);
    if (!stored) {
      return 0;
    }
  } else {
    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = statement->location;
    assign.dest = ir_operand_symbol(local_name);
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
  return 1;
}

static int ir_lower_address_space_local(IRLoweringContext *context,
                                        VarDeclaration *declaration,
                                        Type *decl_type, IRInstruction *local) {
  int is_static_storage =
      decl_type && decl_type->kind == TYPE_ARRAY && decl_type->base_type &&
      decl_type->array_size > 0 && decl_type->array_size <= UINT32_MAX;
  int is_dynamic_workgroup_view =
      decl_type && decl_type->kind == TYPE_POINTER && decl_type->base_type &&
      declaration->address_space == AST_ADDRESS_SPACE_WORKGROUP;
  long long static_view_elements = 0;
  if (decl_type && decl_type->kind == TYPE_SLICE && decl_type->base_type &&
      decl_type->view_extents[0] > 0) {
    static_view_elements = 1;
    for (size_t e = 0; e < 4 && decl_type->view_extents[e]; e++) {
      static_view_elements *= (long long)decl_type->view_extents[e];
    }
  }
  if (!is_static_storage && !static_view_elements &&
      !is_dynamic_workgroup_view) {
    ir_operand_destroy(&local->dest);
    ir_set_error(context,
                 "Invalid GPU address-space declaration '%s' reached IR "
                 "lowering",
                 declaration->name);
    return 0;
  }
  MtlcAddressSpace address_space =
      declaration->address_space == AST_ADDRESS_SPACE_WORKGROUP
          ? MTLC_ADDRESS_SPACE_WORKGROUP
          : MTLC_ADDRESS_SPACE_PRIVATE;
  MtlcType *element_type =
      mtlc_type_from_frontend(decl_type->base_type);
  const MtlcType *pointer_type =
      mtlc_type_pointer_in(element_type, address_space);
  if (!element_type || !pointer_type) {
    ir_operand_destroy(&local->dest);
    ir_set_error(context,
                 "Unable to lower GPU address-space type for '%s'",
                 declaration->name);
    return 0;
  }
  local->op = IR_OP_ADDRESS_SPACE_ALLOC;
  local->rhs = ir_operand_int(
      is_static_storage ? (long long)decl_type->array_size
                        : static_view_elements);
  local->text = decl_type->base_type->name;
  local->value_type = (MtlcType *)pointer_type;
  local->address_space = address_space;
  // A dynamic view's declared align(N) rides on lhs: the storage the
  // backend declares for it is at least that aligned.
  if (is_dynamic_workgroup_view && decl_type->declared_align > 0)
    local->lhs = ir_operand_int((long long)decl_type->declared_align);
  return 1;
}

static int ir_lower_zeroed_aggregate_local(IRLoweringContext *context,
                                           IRFunction *function,
                                           ASTNode *statement,
                                           Type *decl_type,
                                           const char *local_name,
                                           const char *declared_name) {
  int position_is_tracked =
      context->block_statements &&
      context->block_statement_index < context->block_statement_count &&
      context->block_statements[context->block_statement_index] == statement;
  if (!(position_is_tracked &&
        ir_zero_fill_is_dead(context, declared_name, decl_type)) &&
      !ir_emit_zero_fill_local(context, function, local_name, decl_type,
                               statement->location)) {
    return 0;
  }
  return 1;
}

static int ir_lower_zeroed_scalar_local(IRLoweringContext *context,
                                        IRFunction *function, ASTNode *statement,
                                        const VarDeclaration *declaration,
                                        Type *decl_type,
                                        const char *local_name) {
  int float_bits = ir_named_type_float_bits(context, declaration->type_name);
  IRInstruction assign = {0};
  int ok;
  if (decl_type->kind == TYPE_TAGGED_ENUM ||
      (float_bits != 0 && float_bits != 32 && float_bits != 64) ||
      (decl_type->size != 1 && decl_type->size != 2 && decl_type->size != 4 &&
       decl_type->size != 8)) {
    return ir_emit_zero_fill_local(context, function, local_name, decl_type,
                                   statement->location);
  }
  assign.op = IR_OP_ASSIGN;
  assign.location = statement->location;
  assign.dest = ir_operand_symbol(local_name);
  assign.lhs = float_bits ? ir_operand_float_sized(0.0, float_bits)
                          : ir_operand_int(0);
  ir_assign_apply_float_bits(&assign, &assign.lhs, float_bits);
  ok = assign.dest.name && ir_emit(context, function, &assign);
  ir_operand_destroy(&assign.dest);
  ir_operand_destroy(&assign.lhs);
  if (!ok) {
    ir_set_error(context, "Out of memory while zeroing a local");
  }
  return ok;
}

static void ir_note_declared_type_facts(const VarDeclaration *declaration,
                                        Type *decl_type) {
  double bound_lo = 0.0;
  double bound_hi = 0.0;
  double bound_err = 0.0;
  if (!declaration->type_name || !decl_type) {
    return;
  }
  if (type_checker_float_bound(decl_type, &bound_lo, &bound_hi, &bound_err)) {
    ir_declare_float_bound(string_intern(declaration->type_name), bound_lo,
                           bound_hi);
  }
  if (type_checker_type_excludes_zero(decl_type)) {
    ir_declare_nonzero_type(string_intern(declaration->type_name));
  }
}

static int ir_lower_zero_unset_local(IRLoweringContext *context,
                                     IRFunction *function, ASTNode *statement,
                                     VarDeclaration *declaration,
                                     Type *decl_type, const char *local_name) {
  if (declaration->initializer || !decl_type ||
      declaration->address_space != AST_ADDRESS_SPACE_DEFAULT ||
      function->is_kernel) {
    return 1;
  }
  if (decl_type->kind == TYPE_ARRAY || decl_type->kind == TYPE_STRUCT ||
      decl_type->kind == TYPE_SLICE || decl_type->kind == TYPE_STRING) {
    return ir_lower_zeroed_aggregate_local(context, function, statement,
                                           decl_type, local_name,
                                           declaration->name);
  }
  return ir_lower_zeroed_scalar_local(context, function, statement, declaration,
                                      decl_type, local_name);
}

static int ir_lower_var_declaration(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement) {
  VarDeclaration *declaration = (VarDeclaration *)statement->data;
  if (!declaration || !declaration->name) {
    ir_set_error(context, "Malformed variable declaration");
    return 0;
  }

  if (declaration->is_const) {
    Type *const_type = ir_resolve_named_type(context, declaration->type_name);
    if (!const_type && declaration->initializer) {
      const_type = declaration->initializer->resolved_type;
    }
    if (type_is_comptime_only(const_type)) {
      return 1;
    }
  }

  IRInstruction local = {0};
  Type *decl_type = ir_resolve_named_type(context, declaration->type_name);
  if (!decl_type && declaration->initializer) {
    decl_type = declaration->initializer->resolved_type;
  }
  if (decl_type && type_checker_is_tile(decl_type)) {
    return ir_lower_tile_declaration(context, function, statement, declaration,
                                     decl_type);
  }
  const char *decl_type_text = ir_backend_type_name(declaration->type_name);
  if (!decl_type_text && declaration->initializer &&
      declaration->initializer->resolved_type) {
    decl_type_text = ir_backend_type_name(
        declaration->initializer->resolved_type->name);
  }
  const char *local_name =
      ir_local_bind(context, declaration->name, decl_type_text);
  local.op = IR_OP_DECLARE_LOCAL;
  local.location = statement->location;
  local.dest = ir_operand_symbol(local_name);
  local.text = (char *)ir_backend_type_name(declaration->type_name);
  ir_note_declared_type_facts(declaration, decl_type);
  local.value_type = mtlc_type_from_frontend(decl_type);
  if (declaration->address_space != AST_ADDRESS_SPACE_DEFAULT &&
      !ir_lower_address_space_local(context, declaration, decl_type, &local)) {
    return 0;
  }
  if (!local.text && declaration->initializer &&
      declaration->initializer->resolved_type) {
    local.text = (char *)ir_backend_type_name(
        declaration->initializer->resolved_type->name);
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

  if (!ir_lower_zero_unset_local(context, function, statement, declaration,
                                 decl_type, local_name)) {
    return 0;
  }

  if (declaration->initializer &&
      declaration->initializer->type == AST_AGGREGATE_LITERAL) {
    return ir_emit_aggregate_literal_copy_to_symbol(
        context, function, local_name, declaration->initializer,
        decl_type, statement->location);
  }

  if (declaration->initializer &&
      !ir_lower_var_initializer(context, function, declaration, decl_type,
                                local_name, statement)) {
    return 0;
  }
  return 1;
}

static int ir_lower_named_assignment(IRLoweringContext *context,
                                     IRFunction *function, ASTNode *statement,
                                     Assignment *assignment, IROperand *value) {
  const IRLocalBinding *binding =
      ir_local_binding_find(context, assignment->variable_name);
  const char *target_name =
      binding ? binding->ir_name : assignment->variable_name;
  Type *assign_type =
      ir_lookup_symbol_type(context, assignment->variable_name);
  if (!assign_type && assignment->value) {
    assign_type = assignment->value->resolved_type;
  }
  Type *decay_target =
      ir_lookup_symbol_type(context, assignment->variable_name);
  if (!decay_target && binding) {
    decay_target = ir_resolve_named_type(context, binding->type_text);
  }
  if (!ir_normalize_to_bool(context, function, decay_target, assignment->value,
                            value, assignment->value->location)) {
    ir_operand_destroy(value);
    return 0;
  }
  if (ir_should_build_slice_from_array(decay_target, assignment->value) &&
      !ir_build_slice_operand_from_array(
          context, function, value, assignment->value->resolved_type,
          decay_target, assignment->value->location)) {
    ir_operand_destroy(value);
    return 0;
  }
  if (ir_should_decay_array_to_address(decay_target, assignment->value) &&
      !ir_decay_array_operand_to_address(context, function, value,
                                         assignment->value->location)) {
    ir_operand_destroy(value);
    return 0;
  }
  if (ir_should_coerce_string_to_cstring(context, assign_type,
                                         assignment->value) &&
      !ir_coerce_string_operand_to_cstring(
          context, function, value, assignment->value->location)) {
    ir_operand_destroy(value);
    return 0;
  }
  if (ir_try_emit_aggregate_symbol_memcpy(
          context, function, target_name, value,
          assign_type, statement->location)) {
    ir_operand_destroy(value);
    return 1;
  }
  if (ir_small_float_local(context, function, assignment->variable_name,
                           target_name, assign_type)) {
    int stored = ir_emit_small_float_home_store(
        context, function, target_name, assign_type, value,
        statement->location);
    ir_operand_destroy(value);
    return stored;
  }

  {
    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = statement->location;
    assign.dest = ir_operand_symbol(target_name);
    assign.lhs = *value;
    int target_float_bits =
        binding ? ir_named_type_float_bits(context, binding->type_text)
                : ir_symbol_float_bits(context, assignment->variable_name);
    if (target_float_bits == 0 && assignment->value &&
        assignment->value->resolved_type &&
        (assignment->value->resolved_type->kind == TYPE_FLOAT32 ||
         assignment->value->resolved_type->kind == TYPE_FLOAT64 ||
         assignment->value->resolved_type->kind == TYPE_FLOAT16 ||
         assignment->value->resolved_type->kind == TYPE_BFLOAT16)) {
      target_float_bits = ir_local_declared_float_bits(
          context, function, target_name);
    }
    ir_assign_apply_float_bits(&assign, &assign.lhs, target_float_bits);
    if (!assign.dest.name) {
      ir_operand_destroy(value);
      ir_set_error(context, "Out of memory while lowering assignment target");
      return 0;
    }

    if (!ir_emit(context, function, &assign)) {
      ir_operand_destroy(&assign.dest);
      ir_operand_destroy(value);
      return 0;
    }

    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(value);
    return 1;
  }
  return -1;
}

static int ir_lower_assignment(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement) {
  Assignment *assignment = (Assignment *)statement->data;
  if (!assignment || !assignment->value) {
    ir_set_error(context, "Malformed assignment statement");
    return 0;
  }

  if (assignment->target_count > 0) {
    return ir_lower_multi_assignment(context, function, assignment,
                                     statement->location);
  }

  if (assignment->target &&
      ir_expression_is_column_element(context, assignment->target)) {
    return ir_lower_column_element_store(context, function, assignment->target,
                                         assignment->value,
                                         statement->location);
  }

  if (assignment->value->type == AST_AGGREGATE_LITERAL) {
    Type *literal_type = assignment->value->resolved_type;
    if (assignment->variable_name) {
      Type *assign_type =
          ir_lookup_symbol_type(context, assignment->variable_name);
      return ir_emit_aggregate_literal_copy_to_symbol(
          context, function,
          ir_local_ir_name(context, assignment->variable_name),
          assignment->value,
          assign_type ? assign_type : literal_type, statement->location);
    }
    if (!assignment->target) {
      ir_set_error(context, "Assignment target is missing");
      return 0;
    }
    IROperand literal_address = ir_operand_none();
    Type *literal_target_type = NULL;
    if (!ir_lower_lvalue_address(context, function, assignment->target,
                                 &literal_address, &literal_target_type)) {
      return 0;
    }
    int ok = ir_emit_aggregate_literal_copy(
        context, function, &literal_address, assignment->value,
        literal_target_type ? literal_target_type : literal_type,
        statement->location);
    ir_operand_destroy(&literal_address);
    return ok;
  }

  if (assignment->variable_name) {
    const IRLocalBinding *binding =
        ir_local_binding_find(context, assignment->variable_name);
    Type *assign_type =
        binding && binding->type_text
            ? ir_resolve_named_type(context, binding->type_text)
            : NULL;
    if (assign_type && type_checker_is_tile(assign_type)) {
      IROperand dest = ir_operand_symbol(
          ir_local_ir_name(context, assignment->variable_name));
      int ok = ir_lower_tile_store_into(context, function, assign_type, &dest,
                                        assignment->value,
                                        statement->location);
      ir_operand_destroy(&dest);
      return ok;
    }
  }

  IROperand value = ir_operand_none();
  if (!ir_lower_expression(context, function, assignment->value, &value)) {
    return 0;
  }

  if (assignment->variable_name) {
    int handled = ir_lower_named_assignment(context, function, statement,
                                            assignment, &value);
    if (handled >= 0) {
      return handled;
    }
  }

  if (!assignment->target) {
    ir_operand_destroy(&value);
    ir_set_error(context, "Assignment target is missing");
    return 0;
  }

  if (ir_expression_is_packed_field(context, assignment->target)) {
    int stored = ir_lower_packed_store(context, function, assignment->target,
                                       &value, statement->location);
    ir_operand_destroy(&value);
    return stored;
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

  if (!ir_normalize_to_bool(context, function, target_type, assignment->value,
                            &value, assignment->value->location)) {
    ir_operand_destroy(&address);
    ir_operand_destroy(&value);
    return 0;
  }
  if (ir_should_build_slice_from_array(target_type, assignment->value) &&
      !ir_build_slice_operand_from_array(
          context, function, &value, assignment->value->resolved_type,
          target_type, assignment->value->location)) {
    ir_operand_destroy(&value);
    return 0;
  }
  if (ir_should_decay_array_to_address(target_type, assignment->value) &&
      !ir_decay_array_operand_to_address(context, function, &value,
                                         assignment->value->location)) {
    ir_operand_destroy(&address);
    ir_operand_destroy(&value);
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

  if (ir_try_emit_aggregate_address_memcpy(context, function, &address, &value,
                                           target_type,
                                           statement->location)) {
    ir_operand_destroy(&address);
    ir_operand_destroy(&value);
    return 1;
  }

  IRInstruction store = {0};
  store.op = IR_OP_STORE;
  store.location = statement->location;
  store.dest = address;
  store.lhs = value;
  store.rhs = ir_operand_int(ir_type_storage_size(target_type));
  ir_access_apply_alias_class(&store, target_type);
  if (target_type->kind == TYPE_FLOAT32 ||
      target_type->kind == TYPE_FLOAT64 ||
      target_type->kind == TYPE_FLOAT16 ||
      target_type->kind == TYPE_BFLOAT16) {
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

static int ir_lower_for_statement(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
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

  int for_simd_mode = for_data->simd_mode != SIMD_ATTR_NONE
                          ? for_data->simd_mode
                          : context->current_function_simd_default;
  if (for_simd_mode == SIMD_ATTR_NONE && g_ir_lowering_explain) {
    for_simd_mode = SIMD_ATTR_REPORT;
  }
  int for_simd_id = -1;
  if (for_simd_mode != SIMD_ATTR_NONE) {
    for_simd_id = context->next_simd_request_id++;
    if (!ir_emit_simd_marker(context, function, 'B', for_simd_id,
                             for_simd_mode, statement->location)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }
  }

  ir_local_scope_enter(context);
  if (!ir_lower_statement_or_expression(context, function,
                                        for_data->initializer)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  if (for_data->parallel_mode &&
      !ir_emit_parallel_marker(context, function, condition_label,
                               statement->location)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  if (for_data->unroll_factor > 1 &&
      !ir_emit_unroll_marker(context, function, for_data->unroll_factor,
                             statement->location)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  if (!ir_emit_label_instruction(context, function, condition_label,
                                 statement->location)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  if (for_data->condition) {
    size_t for_branch_before = function->instruction_count;
    if (!ir_ownership_after_unit(context, function, for_data->condition,
                                 for_data->condition->location) ||
        !ir_emit_condition_false_branch(context, function,
                                        for_data->condition, end_label)) {
      ir_local_scope_leave(context);
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }
    if (for_data->uniform_mode == 3) {
      ir_mark_branches_uniform(function, for_branch_before);
    }
  }

  size_t for_body_before = function->instruction_count;
  if (!ir_push_labeled_control_frame(context, end_label, step_label,
                                     for_data->label, defers)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  int body_ok = ir_lower_statement_with_defers(context, function,
                                               for_data->body, defers);
  if (for_data->uniform_mode != 3) {
    ir_mark_calls_divergent(function, for_body_before,
                            for_data->warp_uniform ? IR_DIVERGENT_BLOCK
                                                   : IR_DIVERGENT_FULL);
  }
  ir_pop_control_frame(context);
  if (!body_ok) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  if (!ir_emit_label_instruction(context, function, step_label,
                                 statement->location)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  if (!ir_lower_statement_or_expression(context, function,
                                        for_data->increment)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  if (!ir_emit_jump_instruction(context, function, condition_label,
                                statement->location) ||
      !ir_emit_label_instruction(context, function, end_label,
                                 statement->location)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  if (for_simd_id >= 0 &&
      !ir_emit_simd_marker(context, function, 'E', for_simd_id, 0,
                           statement->location)) {
    ir_local_scope_leave(context);
    free(condition_label);
    free(step_label);
    free(end_label);
    return 0;
  }

  ir_local_scope_leave(context);
  free(condition_label);
  free(step_label);
  free(end_label);
  return 1;
}

static int ir_lower_while_statement(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
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

  int while_simd_mode = while_data->simd_mode != SIMD_ATTR_NONE
                            ? while_data->simd_mode
                            : context->current_function_simd_default;
  if (while_simd_mode == SIMD_ATTR_NONE && g_ir_lowering_explain) {
    while_simd_mode = SIMD_ATTR_REPORT;
  }
  int while_simd_id = -1;
  if (while_simd_mode != SIMD_ATTR_NONE) {
    while_simd_id = context->next_simd_request_id++;
    if (!ir_emit_simd_marker(context, function, 'B', while_simd_id,
                             while_simd_mode, statement->location)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }
  }

  if (while_data->parallel_mode &&
      !ir_emit_parallel_marker(context, function, loop_start,
                               statement->location)) {
    free(loop_start);
    free(loop_end);
    return 0;
  }

  if (while_data->unroll_factor > 1 &&
      !ir_emit_unroll_marker(context, function, while_data->unroll_factor,
                             statement->location)) {
    free(loop_start);
    free(loop_end);
    return 0;
  }

  if (!ir_emit_label_instruction(context, function, loop_start,
                                 statement->location)) {
    free(loop_start);
    free(loop_end);
    return 0;
  }

  size_t while_branch_before = function->instruction_count;
  if (!ir_ownership_after_unit(context, function, while_data->condition,
                               while_data->condition->location) ||
      !ir_emit_condition_false_branch(context, function,
                                      while_data->condition, loop_end)) {
    free(loop_start);
    free(loop_end);
    return 0;
  }
  if (while_data->uniform_mode == 3) {
    ir_mark_branches_uniform(function, while_branch_before);
  }

  if (!ir_push_labeled_control_frame(context, loop_end, loop_start,
                                     while_data->label, defers)) {
    free(loop_start);
    free(loop_end);
    return 0;
  }

  size_t while_body_before = function->instruction_count;
  int body_ok = ir_lower_statement_with_defers(context, function,
                                               while_data->body, defers);
  if (while_data->uniform_mode != 3) {
    ir_mark_calls_divergent(function, while_body_before,
                            while_data->warp_uniform ? IR_DIVERGENT_BLOCK
                                                     : IR_DIVERGENT_FULL);
  }
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

  if (while_simd_id >= 0 &&
      !ir_emit_simd_marker(context, function, 'E', while_simd_id, 0,
                           statement->location)) {
    free(loop_start);
    free(loop_end);
    return 0;
  }

  free(loop_start);
  free(loop_end);
  return 1;
}

static int ir_lower_if_statement(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
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

    size_t branches_before = function->instruction_count;
    if (!ir_ownership_after_unit(context, function, current_cond,
                                 current_cond->location) ||
        !ir_emit_condition_false_branch(context, function, current_cond,
                                        next_label)) {
      free(next_label);
      free(end_label);
      return 0;
    }
    if (if_data->uniform_mode == 3) {
      ir_mark_branches_uniform(function, branches_before);
    } else if (i == 0 && if_data->condition_uniform) {
      ir_mark_branch_conditions_uniform(function, branches_before);
    }

    {
      size_t arm_before = function->instruction_count;
      if (!ir_lower_statement_with_defers(context, function, current_body,
                                          defers)) {
        free(next_label);
        free(end_label);
        return 0;
      }
      if (if_data->uniform_mode != 3) {
        ir_mark_calls_divergent(function, arm_before,
                                if_data->warp_uniform ? IR_DIVERGENT_BLOCK
                                                      : IR_DIVERGENT_FULL);
      }
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

  size_t else_before = function->instruction_count;
  if (if_data->else_branch &&
      !ir_lower_statement_with_defers(context, function, if_data->else_branch,
                                      defers)) {
    free(end_label);
    return 0;
  }
  if (if_data->else_branch && if_data->uniform_mode != 3) {
    ir_mark_calls_divergent(function, else_before,
                            if_data->warp_uniform ? IR_DIVERGENT_BLOCK
                                                  : IR_DIVERGENT_FULL);
  }

  if (!ir_emit_label_instruction(context, function, end_label,
                                 statement->location)) {
    free(end_label);
    return 0;
  }

  free(end_label);
  return 1;
}

static int ir_lower_return_statement(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
  ReturnStatement *ret = (ReturnStatement *)statement->data;
  IROperand value = ir_operand_none();
  if (ret && ret->value) {
    if (ret->value_count <= 1 && ret->value->type == AST_AGGREGATE_LITERAL &&
        ret->value->resolved_type) {
      Type *literal_type = ret->value->resolved_type;
      char *home = ir_new_label_name(context, "ret_literal");
      if (!home ||
          !ir_emit_local_declaration(context, function, home,
                                     literal_type->name,
                                     ret->value->location) ||
          !ir_emit_aggregate_literal_copy_to_symbol(
              context, function, home, ret->value, literal_type,
              ret->value->location)) {
        free(home);
        return 0;
      }
      value = ir_operand_symbol(home);
      free(home);
      if (!value.name) {
        return 0;
      }
    } else if (ret->value_count > 1
            ? !ir_lower_multi_return_value(context, function, ret, &value,
                                           statement->location)
            : !ir_lower_expression(context, function, ret->value, &value)) {
      return 0;
    }
    Type *return_type =
        ir_resolve_named_type(context, context->current_return_type_name);
    if (ir_should_build_slice_from_array(return_type, ret->value) &&
        !ir_build_slice_operand_from_array(context, function, &value,
                                           ret->value->resolved_type,
                                           return_type,
                                           ret->value->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
    if (!ir_normalize_to_bool(context, function, return_type, ret->value, &value,
                              ret->value->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
    if (ir_should_decay_array_to_address(return_type, ret->value) &&
        !ir_decay_array_operand_to_address(context, function, &value,
                                           ret->value->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
    if (ir_should_coerce_string_to_cstring(context, return_type,
                                           ret->value) &&
        !ir_coerce_string_operand_to_cstring(context, function, &value,
                                             ret->value->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
  }
  const ASTNode *saved_exit = context->ownership_exit;
  context->ownership_exit = statement;
  int returned =
      ir_ownership_after_unit(context, function, statement,
                              statement->location) &&
      ir_emit_return_with_defers(context, function, defers, &value,
                                 statement->location);
  context->ownership_exit = saved_exit;
  if (!returned) {
    ir_operand_destroy(&value);
    return 0;
  }
  ir_operand_destroy(&value);
  return 1;
}

static int ir_lower_block(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
  Program *program = (Program *)statement->data;
  if (!program) {
    return 1;
  }
  const char *saved_expansion_note = context->current_expansion_note;
  const char *block_note =
      context->type_checker
          ? type_checker_expansion_note(context->type_checker, statement,
                                        NULL)
          : NULL;
  if (block_note) {
    context->current_expansion_note = block_note;
  }
  if (!defers) {
    ir_local_scope_enter(context);
    for (size_t i = 0; i < program->declaration_count; i++) {
      context->block_statements = program->declarations;
      context->block_statement_count = program->declaration_count;
      context->block_statement_index = i;
      if (!ir_lower_statement_with_defers(context, function,
                                          program->declarations[i], NULL)) {
        ir_local_scope_leave(context);
        context->current_expansion_note = saved_expansion_note;
        return 0;
      }
    }
    ir_local_scope_leave(context);
    context->current_expansion_note = saved_expansion_note;
    return 1;
  }

  IRDeferScope block_scope = {0};
  block_scope.parent = defers;
  ir_local_scope_enter(context);
  for (size_t i = 0; i < program->declaration_count; i++) {
    context->block_statements = program->declarations;
    context->block_statement_count = program->declaration_count;
    context->block_statement_index = i;
    if (!ir_lower_statement_with_defers(
            context, function, program->declarations[i], &block_scope)) {
      ir_defer_stack_free(&block_scope.stack);
      ir_local_scope_leave(context);
      context->current_expansion_note = saved_expansion_note;
      return 0;
    }
  }

  const ASTNode *saved_exit = context->ownership_exit;
  context->ownership_exit = statement;
  int ok =
      ir_emit_deferred_calls_non_err(context, function, &block_scope.stack);
  context->ownership_exit = saved_exit;
  ir_defer_stack_free(&block_scope.stack);
  ir_local_scope_leave(context);
  context->current_expansion_note = saved_expansion_note;
  return ok;
}

static int ir_lower_break_statement(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
  LoopControlStatement *ctrl = (LoopControlStatement *)statement->data;
  const char *user_label = ctrl ? ctrl->target_label : NULL;
  const IRControlFrame *frame = ir_break_target_frame(context, user_label);
  const char *target = frame ? frame->break_label : NULL;
  if (!target) {
    if (user_label) {
      ir_set_error(context, "'break %s' has no matching labeled loop",
                   user_label);
    } else {
      ir_set_error(context, "'break' used outside loop/switch");
    }
    return 0;
  }
  if (!ir_ownership_after_unit(context, function, statement,
                               statement->location) ||
      !ir_emit_defers_at_exit(context, function, defers, frame->defers,
                              statement)) {
    return 0;
  }
  return ir_emit_jump_instruction(context, function, target,
                                  statement->location);
}

static int ir_lower_continue_statement(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
  LoopControlStatement *ctrl = (LoopControlStatement *)statement->data;
  const char *user_label = ctrl ? ctrl->target_label : NULL;
  const IRControlFrame *frame = ir_continue_target_frame(context, user_label);
  const char *target = frame ? frame->continue_label : NULL;
  if (!target) {
    if (user_label) {
      ir_set_error(context, "'continue %s' has no matching labeled loop",
                   user_label);
    } else {
      ir_set_error(context, "'continue' used outside loop");
    }
    return 0;
  }
  if (!ir_ownership_after_unit(context, function, statement,
                               statement->location) ||
      !ir_emit_defers_at_exit(context, function, defers, frame->defers,
                              statement)) {
    return 0;
  }
  return ir_emit_jump_instruction(context, function, target,
                                  statement->location);
}

static int ir_lower_defer_statement(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
  if (!defers) {
    return 1;
  }
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
  ir_ownership_capture_defer_root(
      context, statement,
      &defers->stack.entries[defers->stack.count - 1].drop_root_name,
      &defers->stack.entries[defers->stack.count - 1].drop_root_index);
  return 1;
}

static int ir_lower_fallthrough_statement(IRLoweringContext *context, IRFunction *function,
                                   ASTNode *statement, IRDeferScope *defers) {
  const IRControlFrame *frame = ir_current_fallthrough_frame(context);
  if (!frame || !frame->fallthrough_label) {
    ir_set_error(context, "'fallthrough' outside a switch case with a case "
                          "after it");
    return 0;
  }
  if (!ir_emit_defers_at_exit(context, function, defers, frame->defers,
                              statement)) {
    return 0;
  }
  return ir_emit_jump_instruction(context, function,
                                  frame->fallthrough_label,
                                  statement->location);
}

static int ir_lower_statement_unmarked(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *statement,
                                       IRDeferScope *defers);

int ir_lower_statement_with_defers(IRLoweringContext *context,
                                   IRFunction *function, ASTNode *statement,
                                   IRDeferScope *defers) {
  int marked;
  int ok;
  if (!context || !function || !statement) {
    return 0;
  }
  marked = ir_lower_live_marks_statement(function, (int)statement->type);
  if (marked &&
      !ir_lower_live_mark(context, function, 1, statement->location)) {
    return 0;
  }
  IRDeferScope *saved_defers = context->current_defers;
  if (defers) {
    context->current_defers = defers;
  }
  ok = ir_lower_statement_unmarked(context, function, statement, defers);
  context->current_defers = saved_defers;
  if (ok && statement->type != AST_RETURN_STATEMENT &&
      statement->type != AST_YIELD_STATEMENT &&
      statement->type != AST_BREAK_STATEMENT &&
      statement->type != AST_CONTINUE_STATEMENT) {
    ok = ir_ownership_after_unit(context, function, statement,
                                 statement->location);
  }
  if (ok && marked) {
    ok = ir_lower_live_mark(context, function, 0, statement->location);
  }
  return ok;
}

static int ir_lower_statement_unmarked(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *statement,
                                       IRDeferScope *defers) {
  if (!context || !function || !statement) {
    return 0;
  }

  switch (statement->type) {
  case AST_PROGRAM:
    return ir_lower_block(context, function, statement, defers);

  case AST_QUIESCE_STATEMENT: {
    if (g_ir_lowering_live) {
      return ir_lower_live_site(context, function, statement->location);
    }
    ir_declare_swap_apply_helper(context);
    IRInstruction apply = {0};
    apply.op = IR_OP_CALL;
    apply.location = statement->location;
    apply.dest = ir_operand_none();
    apply.text = (char *)ir_quiesce_helper_name();
    apply.arguments = NULL;
    apply.argument_count = 0;
    return ir_emit(context, function, &apply);
  }

  case AST_VAR_DECLARATION:
    return ir_lower_var_declaration(context, function, statement);

  case AST_ASSIGNMENT:
    return ir_lower_assignment(context, function, statement);

  case AST_FUNCTION_CALL: {
    IROperand ignored = ir_operand_none();
    int ok = ir_lower_expression(context, function, statement, &ignored);
    ir_operand_destroy(&ignored);
    return ok;
  }

  case AST_BARRIER_STATEMENT: {
    BarrierStatement *source = (BarrierStatement *)statement->data;
    if (!source) {
      ir_set_error(context, "Malformed barrier statement");
      return 0;
    }
    IRInstruction barrier = {0};
    barrier.op = IR_OP_BARRIER;
    barrier.location = statement->location;
    barrier.memory_scope = source->subgroup ? MTLC_MEMORY_SCOPE_SUBGROUP
                                            : MTLC_MEMORY_SCOPE_WORKGROUP;
    if (source->memory_regions & AST_MEMORY_REGION_WORKGROUP)
      barrier.memory_regions |= MTLC_MEMORY_REGION_WORKGROUP;
    if (source->memory_regions & AST_MEMORY_REGION_GLOBAL)
      barrier.memory_regions |= MTLC_MEMORY_REGION_GLOBAL;
    switch (source->memory_order) {
    case AST_MEMORY_ORDER_ACQUIRE:
      barrier.memory_order = MTLC_MEMORY_ORDER_ACQUIRE;
      break;
    case AST_MEMORY_ORDER_RELEASE:
      barrier.memory_order = MTLC_MEMORY_ORDER_RELEASE;
      break;
    case AST_MEMORY_ORDER_ACQ_REL:
      barrier.memory_order = MTLC_MEMORY_ORDER_ACQ_REL;
      break;
    case AST_MEMORY_ORDER_SEQ_CST:
      barrier.memory_order = MTLC_MEMORY_ORDER_SEQ_CST;
      break;
    default:
      ir_set_error(context, "Invalid barrier memory order");
      return 0;
    }
    return ir_emit(context, function, &barrier);
  }

  case AST_GPU_LAUNCH:
    return ir_lower_gpu_launch(context, function, statement);

  case AST_RETURN_STATEMENT:
    return ir_lower_return_statement(context, function, statement, defers);

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

  case AST_IF_STATEMENT:
    return ir_lower_if_statement(context, function, statement, defers);

  case AST_WHILE_STATEMENT:
    return ir_lower_while_statement(context, function, statement, defers);

  case AST_FOR_STATEMENT:
    return ir_lower_for_statement(context, function, statement, defers);

  case AST_SWITCH_STATEMENT:
    return ir_lower_switch_statement(context, function, statement, defers);

  case AST_MATCH_STATEMENT:
    return ir_lower_match_statement(context, function, statement, defers);

  case AST_VALUE_SCOPE: {
    IROperand discarded = ir_operand_none();
    int lowered = ir_lower_value_scope(context, function, statement, &discarded);
    ir_operand_destroy(&discarded);
    return lowered;
  }

  case AST_YIELD_STATEMENT:
    return ir_lower_yield_statement(context, function, statement, defers);

  case AST_FALLTHROUGH_STATEMENT:
    return ir_lower_fallthrough_statement(context, function, statement, defers);

  case AST_BREAK_STATEMENT:
    return ir_lower_break_statement(context, function, statement, defers);

  case AST_CONTINUE_STATEMENT:
    return ir_lower_continue_statement(context, function, statement, defers);

  case AST_DEFER_STATEMENT:
    return ir_lower_defer_statement(context, function, statement, defers);

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
    int is_statement_expression =
        (statement->type >= AST_IDENTIFIER &&
         statement->type <= AST_NEW_EXPRESSION) ||
        statement->type == AST_FUNC_PTR_CALL ||
        statement->type == AST_CAST_EXPRESSION ||
        statement->type == AST_LAMBDA_EXPRESSION ||
        statement->type == AST_CLOSURE_ADAPT_EXPRESSION;
    if (is_statement_expression) {
      IROperand ignored = ir_operand_none();
      int ok = ir_lower_expression(context, function, statement, &ignored);
      ir_operand_destroy(&ignored);
      return ok;
    }

    ir_set_error(context,
                 "Unsupported statement type %d in pure IR lowering",
                 (int)statement->type);
    return 0;
  }
  }
}

static int ir_lower_gpu_launch_work(IRLoweringContext *context,
                                    IRFunction *function,
                                    GpuLaunchStatement *launch,
                                    ASTNode *statement,
                                    IROperand *arguments) {
if (launch->work) {
  long long block_volume = (long long)launch->kernel_block[0] *
                           (launch->kernel_block[1] > 0
                                ? launch->kernel_block[1]
                                : 1) *
                           (launch->kernel_block[2] > 0
                                ? launch->kernel_block[2]
                                : 1);
  long long threads_per_item = launch->kernel_threads_per_item > 0
                                   ? launch->kernel_threads_per_item
                                   : 1;
  block_volume /= threads_per_item;
  IROperand work_value = ir_operand_none();
  IROperand biased = ir_operand_none();
  if (block_volume < 1) block_volume = 1;
  if (!ir_lower_expression(context, function, launch->work, &work_value)) {
    return 0;
  }
  if (!ir_make_temp_operand(context, &biased) ||
      !ir_emit_binary_instruction(context, function, statement->location,
                                  "+", biased, work_value,
                                  ir_operand_int(block_volume - 1))) {
    ir_operand_destroy(&work_value);
    ir_operand_destroy(&biased);
    return 0;
  }
  ir_operand_destroy(&work_value);
  if (!ir_make_temp_operand(context, &arguments[0]) ||
      !ir_emit_binary_instruction(context, function, statement->location,
                                  "/", arguments[0], biased,
                                  ir_operand_int(block_volume))) {
    ir_operand_destroy(&biased);
    return 0;
  }
  ir_operand_destroy(&biased);
  arguments[1] = ir_operand_int(1);
  arguments[2] = ir_operand_int(1);
  for (size_t d = 0; d < 3; d++) {
    arguments[3 + d] =
        ir_operand_int(launch->kernel_block[d] > 0 ? launch->kernel_block[d]
                                                   : 1);
  }
} else {
  for (size_t d = 0; d < 3; d++) {
    if (!ir_lower_expression(context, function, launch->grid[d],
                             &arguments[d]) ||
        !ir_lower_expression(context, function, launch->block[d],
                             &arguments[3 + d])) {
      return 0;
    }
  }
  if (launch->grid_indirect) {
    ir_operand_destroy(&arguments[0]);
    if (!ir_lower_expression(context, function, launch->grid_indirect,
                             &arguments[0])) {
      return 0;
    }
  }
}
  return 1;
}

static int ir_lower_gpu_launch(IRLoweringContext *context,
                               IRFunction *function, ASTNode *statement) {
  GpuLaunchStatement *launch = (GpuLaunchStatement *)statement->data;
  const size_t controls = IR_GPU_LAUNCH_CONTROL_ARGS;
  const size_t total = controls + (launch ? launch->argument_count : 0u);
  IROperand kernel = ir_operand_none();
  IROperand *arguments = NULL;
  MtlcType **argument_types = NULL;
  if (!launch || !launch->kernel || !launch->dynamic_shared_bytes ||
      !launch->stream) {
    ir_set_error(context, "Malformed GPU launch statement");
    return 0;
  }
  arguments = calloc(total, sizeof(*arguments));
  argument_types = calloc(total, sizeof(*argument_types));
  if (!arguments || !argument_types) {
    free(arguments);
    free(argument_types);
    ir_set_error(context, "Out of memory while lowering GPU launch");
    return 0;
  }
  if (launch->typed_kernel && launch->kernel &&
      launch->kernel->type == AST_IDENTIFIER && launch->kernel->data) {
    const char *kernel_name = ((Identifier *)launch->kernel->data)->name;
    Symbol *kernel_symbol =
        kernel_name && context->type_checker
            ? symbol_table_lookup(context->type_checker->symbol_table,
                                  kernel_name)
            : NULL;
    if (kernel_symbol && kernel_symbol->is_extern && kernel_symbol->link_name &&
        kernel_symbol->link_name[0]) {
      kernel_name = kernel_symbol->link_name;
    }
    IROperand name_argument = ir_operand_string(kernel_name ? kernel_name : "");
    IRInstruction resolve = {0};
    if (!ir_make_temp_operand(context, &kernel)) {
      ir_operand_destroy(&name_argument);
      free(arguments);
      free(argument_types);
      return 0;
    }
    resolve.op = IR_OP_CALL;
    resolve.location = statement->location;
    resolve.dest = kernel;
    resolve.text = "mtlc_gpu_kernel_handle";
    resolve.arguments = &name_argument;
    resolve.argument_count = 1;
    if (!ir_emit(context, function, &resolve)) {
      ir_operand_destroy(&name_argument);
      goto gpu_launch_lower_fail;
    }
    ir_operand_destroy(&name_argument);
  } else if (!ir_lower_expression(context, function, launch->kernel,
                                  &kernel)) {
    free(arguments);
    free(argument_types);
    return 0;
  }
  if (!ir_lower_gpu_launch_work(context, function, launch, statement,
                                arguments)) {
    goto gpu_launch_lower_fail;
  }
  if (!ir_lower_expression(context, function, launch->dynamic_shared_bytes,
                           &arguments[6]) ||
      !ir_lower_expression(context, function, launch->stream,
                           &arguments[7])) {
    goto gpu_launch_lower_fail;
  }
  for (size_t i = 0; i < launch->argument_count; i++) {
    ASTNode *source_arg = launch->arguments[i];
    Type *source_type = source_arg ? source_arg->resolved_type : NULL;
    if (!ir_lower_expression(context, function, source_arg,
                             &arguments[controls + i])) {
      goto gpu_launch_lower_fail;
    }
    if (!source_type) {
      source_type = ir_infer_expression_type(context, source_arg);
    }
    if (launch->typed_kernel && source_arg &&
        source_arg->type == AST_NUMBER_LITERAL && launch->kernel &&
        launch->kernel->type == AST_IDENTIFIER && launch->kernel->data &&
        context->type_checker) {
      const char *kernel_name = ((Identifier *)launch->kernel->data)->name;
      Symbol *kernel_symbol =
          kernel_name ? symbol_table_lookup(context->type_checker->symbol_table,
                                            kernel_name)
                      : NULL;
      Type *declared =
          kernel_symbol && kernel_symbol->kind == SYMBOL_FUNCTION &&
                  kernel_symbol->data.function.parameter_types &&
                  i < kernel_symbol->data.function.parameter_count
              ? kernel_symbol->data.function.parameter_types[i]
              : NULL;
      if (declared && (declared->kind == TYPE_FLOAT32 ||
                       declared->kind == TYPE_FLOAT64 ||
                       declared->kind == TYPE_FLOAT16 ||
                       declared->kind == TYPE_BFLOAT16 ||
                       type_checker_is_integer_type(declared))) {
        source_type = declared;
      }
    }
    argument_types[controls + i] =
        mtlc_type_from_frontend(source_type);
    if (!argument_types[controls + i]) {
      ir_set_error(context, "GPU launch argument has no backend ABI type");
      goto gpu_launch_lower_fail;
    }
  }

  {
    IRInstruction instruction = {0};
    instruction.op = IR_OP_GPU_LAUNCH;
    instruction.location = statement->location;
    instruction.gpu_launch_indirect = launch->grid_indirect != NULL;
    instruction.lhs = kernel;
    instruction.arguments = arguments;
    instruction.argument_types = argument_types;
    instruction.argument_count = total;
    instruction.ast_ref = statement;
    if (!ir_emit(context, function, &instruction)) {
      goto gpu_launch_lower_fail;
    }
  }
  ir_operand_destroy(&kernel);
  for (size_t i = 0; i < total; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  free(arguments);
  free(argument_types);
  return 1;

gpu_launch_lower_fail:
  ir_operand_destroy(&kernel);
  for (size_t i = 0; i < total; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  free(arguments);
  free(argument_types);
  return 0;
}
