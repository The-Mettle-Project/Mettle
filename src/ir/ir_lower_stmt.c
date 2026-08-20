// AST->IR lowering: statement lowering (with defer scopes).
#include "ir_lowering_internal.h"
#include "frontend/mtlc_frontend.h"

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

    if (!field_type || field_type->kind == TYPE_STRUCT ||
        field_type->kind == TYPE_ARRAY ||
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
    store.op = IR_OP_STORE;
    store.location = location;
    store.dest = field_address;
    store.lhs = component;
    store.rhs = ir_operand_int(ir_type_storage_size(field_type));
    ir_access_apply_alias_class(&store, field_type);
    if (field_type->kind == TYPE_FLOAT32 || field_type->kind == TYPE_FLOAT64) {
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
        !identifier->name || !field_type || field_type->kind == TYPE_STRUCT ||
        field_type->kind == TYPE_ARRAY) {
      ir_set_error(context,
                   "Multiple return assignment currently needs scalar targets");
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

int ir_lower_statement_with_defers(IRLoweringContext *context,
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
    /* A block the expander generated carries the note naming its iteration.
     * Stamp it for the duration so `trace` can attribute the values, and
     * restore afterwards so a sibling block is not credited to it. */
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
      if (!ir_lower_statement_with_defers(
              context, function, program->declarations[i], &block_scope)) {
        ir_defer_stack_free(&block_scope.stack);
        ir_local_scope_leave(context);
        context->current_expansion_note = saved_expansion_note;
        return 0;
      }
    }

    int ok =
        ir_emit_deferred_calls_non_err(context, function, &block_scope.stack);
    ir_defer_stack_free(&block_scope.stack);
    ir_local_scope_leave(context);
    context->current_expansion_note = saved_expansion_note;
    return ok;
  }

  /* The one place a staged swap is allowed to take effect. Applying it
   * anywhere else, or on a timer, or at a safepoint the compiler chose, would
   * be control flow at a point the programmer did not write. The call is the
   * whole cost, and a program with no quiesce point never emits it and never
   * links the swap runtime. */
  case AST_QUIESCE_STATEMENT: {
    IRInstruction apply = {0};
    apply.op = IR_OP_CALL;
    apply.location = statement->location;
    apply.dest = ir_operand_none();
    apply.text = "mettle_swap_apply";
    apply.arguments = NULL;
    apply.argument_count = 0;
    return ir_emit(context, function, &apply);
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
    //
    // Type/Field consts are the exception: they have no runtime representation,
    // so they must not become locals even inside a function.
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
    /* Bind before anything is emitted: a name already declared in this
     * function at a different type gets one of its own, so the two do not
     * share a frame slot (and a type) in the backends. */
    const char *decl_type_text = declaration->type_name;
    if (!decl_type_text && declaration->initializer &&
        declaration->initializer->resolved_type) {
      decl_type_text = declaration->initializer->resolved_type->name;
    }
    const char *local_name =
        ir_local_bind(context, declaration->name, decl_type_text);
    local.op = IR_OP_DECLARE_LOCAL;
    local.location = statement->location;
    local.dest = ir_operand_symbol(local_name);
    local.text = declaration->type_name;
    local.value_type = mtlc_type_from_frontend(decl_type);
    if (declaration->address_space != AST_ADDRESS_SPACE_DEFAULT) {
      int is_static_storage =
          decl_type && decl_type->kind == TYPE_ARRAY && decl_type->base_type &&
          decl_type->array_size > 0 && decl_type->array_size <= UINT32_MAX;
      int is_dynamic_workgroup_view =
          decl_type && decl_type->kind == TYPE_POINTER && decl_type->base_type &&
          declaration->address_space == AST_ADDRESS_SPACE_WORKGROUP;
      if (!is_static_storage && !is_dynamic_workgroup_view) {
        ir_operand_destroy(&local.dest);
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
        ir_operand_destroy(&local.dest);
        ir_set_error(context,
                     "Unable to lower GPU address-space type for '%s'",
                     declaration->name);
        return 0;
      }
      local.op = IR_OP_ADDRESS_SPACE_ALLOC;
      /* Zero is the neutral dynamic-workgroup-arena sentinel. It is never
       * accepted for private storage or a fixed source array. */
      local.rhs =
          ir_operand_int(is_static_storage ? (long long)decl_type->array_size
                                           : 0);
      local.text = decl_type->base_type->name;
      local.value_type = (MtlcType *)pointer_type;
      local.address_space = address_space;
    }
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

    if (declaration->initializer &&
        declaration->initializer->type == AST_AGGREGATE_LITERAL) {
      /* The literal was folded to a constant image at type-check time; copy it
       * in wholesale rather than lowering it as an expression. */
      return ir_emit_aggregate_literal_copy_to_symbol(
          context, function, local_name, declaration->initializer,
          decl_type, statement->location);
    }

    if (declaration->initializer) {
      IROperand value = ir_operand_none();
      if (!ir_lower_expression(context, function, declaration->initializer,
                               &value)) {
        return 0;
      }
      if (ir_should_decay_array_to_address(decl_type,
                                           declaration->initializer) &&
          !ir_decay_array_operand_to_address(
              context, function, &value, declaration->initializer->location)) {
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
    }
    return 1;
  }

  case AST_ASSIGNMENT: {
    Assignment *assignment = (Assignment *)statement->data;
    if (!assignment || !assignment->value) {
      ir_set_error(context, "Malformed assignment statement");
      return 0;
    }

    if (assignment->target_count > 0) {
      return ir_lower_multi_assignment(context, function, assignment,
                                       statement->location);
    }

    /* An aggregate literal on the right is a folded constant, not something to
     * evaluate: copy its image into the destination. */
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

    IROperand value = ir_operand_none();
    if (!ir_lower_expression(context, function, assignment->value, &value)) {
      return 0;
    }

    if (assignment->variable_name) {
      const IRLocalBinding *binding =
          ir_local_binding_find(context, assignment->variable_name);
      const char *target_name =
          binding ? binding->ir_name : assignment->variable_name;
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
              context, function, target_name, &value,
              assign_type, statement->location)) {
        ir_operand_destroy(&value);
        return 1;
      }

      {
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = statement->location;
        assign.dest = ir_operand_symbol(target_name);
        assign.lhs = value;
        /* Target float width for the narrowing/widening on store. A local's
         * own binding is authoritative -- the symbol table is keyed by source
         * name, so a shadowed local resolves there to whichever declaration
         * won. Otherwise the symbol table, then (for an inferred local, which
         * has no declared type text) the emitted DECLARE_LOCAL. Gate that IR
         * scan on a floating RHS so non-float assigns stay O(1). */
        int target_float_bits =
            binding ? ir_named_type_float_bits(context, binding->type_text)
                    : ir_symbol_float_bits(context, assignment->variable_name);
        if (target_float_bits == 0 && assignment->value &&
            assignment->value->resolved_type &&
            (assignment->value->resolved_type->kind == TYPE_FLOAT32 ||
             assignment->value->resolved_type->kind == TYPE_FLOAT64)) {
          target_float_bits = ir_local_declared_float_bits(
              context, function, target_name);
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

    /* Aggregate destinations (struct fields, indexed struct elements) must copy
     * the whole struct. A plain IR_OP_STORE of an aggregate RHS only moves one
     * word, silently dropping everything past the first 8 bytes. */
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

  case AST_BARRIER_STATEMENT: {
    BarrierStatement *source = (BarrierStatement *)statement->data;
    if (!source) {
      ir_set_error(context, "Malformed barrier statement");
      return 0;
    }
    IRInstruction barrier = {0};
    barrier.op = IR_OP_BARRIER;
    barrier.location = statement->location;
    barrier.memory_scope = MTLC_MEMORY_SCOPE_WORKGROUP;
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

  case AST_GPU_LAUNCH: {
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
    /* A typed dispatch names a declared `extern kernel` rather than holding a
     * handle in a host variable: resolve it by name against the loaded module.
     * The runtime caches by the (compile-time constant) name pointer, so a
     * per-token launch pays a pointer compare, not a driver lookup. */
    if (launch->typed_kernel && launch->kernel &&
        launch->kernel->type == AST_IDENTIFIER && launch->kernel->data) {
      const char *kernel_name = ((Identifier *)launch->kernel->data)->name;
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
    /* `work: N` launches ceil(N / block volume) blocks of the kernel's
     * declared shape, so the host stops mirroring that arithmetic at every
     * call site. */
    if (launch->work) {
      long long block_volume = (long long)launch->kernel_block[0] *
                               (launch->kernel_block[1] > 0
                                    ? launch->kernel_block[1]
                                    : 1) *
                               (launch->kernel_block[2] > 0
                                    ? launch->kernel_block[2]
                                    : 1);
      /* One block covers block_volume threads, but a `per = warp` kernel
       * spends 32 of them per work item, so it covers that many fewer. */
      long long threads_per_item = launch->kernel_threads_per_item > 0
                                       ? launch->kernel_threads_per_item
                                       : 1;
      block_volume /= threads_per_item;
      IROperand work_value = ir_operand_none();
      IROperand biased = ir_operand_none();
      if (block_volume < 1) block_volume = 1;
      if (!ir_lower_expression(context, function, launch->work, &work_value)) {
        goto gpu_launch_lower_fail;
      }
      if (!ir_make_temp_operand(context, &biased) ||
          !ir_emit_binary_instruction(context, function, statement->location,
                                      "+", biased, work_value,
                                      ir_operand_int(block_volume - 1))) {
        ir_operand_destroy(&work_value);
        ir_operand_destroy(&biased);
        goto gpu_launch_lower_fail;
      }
      ir_operand_destroy(&work_value);
      if (!ir_make_temp_operand(context, &arguments[0]) ||
          !ir_emit_binary_instruction(context, function, statement->location,
                                      "/", arguments[0], biased,
                                      ir_operand_int(block_volume))) {
        ir_operand_destroy(&biased);
        goto gpu_launch_lower_fail;
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
          goto gpu_launch_lower_fail;
        }
      }
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

  case AST_RETURN_STATEMENT: {
    ReturnStatement *ret = (ReturnStatement *)statement->data;
    IROperand value = ir_operand_none();
    if (ret && ret->value) {
      if (ret->value_count > 1
              ? !ir_lower_multi_return_value(context, function, ret, &value,
                                             statement->location)
              : !ir_lower_expression(context, function, ret->value, &value)) {
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

    if (!ir_emit_condition_false_branch(context, function,
                                        while_data->condition, loop_end)) {
      free(loop_start);
      free(loop_end);
      return 0;
    }

    if (!ir_push_labeled_control_frame(context, loop_end, loop_start,
                                       while_data->label, defers)) {
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

    if (!ir_lower_statement_or_expression(context, function,
                                          for_data->initializer)) {
      free(condition_label);
      free(step_label);
      free(end_label);
      return 0;
    }

    if (for_data->unroll_factor > 1 &&
        !ir_emit_unroll_marker(context, function, for_data->unroll_factor,
                               statement->location)) {
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
                                       for_data->label, defers)) {
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

    if (for_simd_id >= 0 &&
        !ir_emit_simd_marker(context, function, 'E', for_simd_id, 0,
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
    return ir_lower_switch_statement(context, function, statement, defers);

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
    // The jump leaves every scope between here and the loop, so their
    // deferred statements run before it.
    if (!ir_emit_defers_until_scope(context, function, defers,
                                    frame->defers)) {
      return 0;
    }
    return ir_emit_jump_instruction(context, function, target,
                                    statement->location);
  }

  case AST_CONTINUE_STATEMENT: {
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
    // The iteration ends here, so the body's deferred statements run, exactly
    // as they would on the path that falls off the end of the body.
    if (!ir_emit_defers_until_scope(context, function, defers,
                                    frame->defers)) {
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
    /* Any expression usable as a bare statement (result discarded), e.g. a
     * call for its side effects. The AST_IDENTIFIER..AST_NEW_EXPRESSION range
     * covers most expression kinds contiguously; a few were added later at
     * other enum positions and are listed explicitly, notably
     * AST_FUNC_PTR_CALL: a call through a function-pointer/closure struct
     * field or expression result, used as a statement (`obj.callback(args);`).
     */
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

    ir_set_error(context, "Unsupported statement type in pure IR lowering");
    return 0;
  }
  }
}
