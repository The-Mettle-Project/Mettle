// AST->IR lowering: expression and call lowering.
#include "ir_lowering_internal.h"
#include "frontend/mtlc_frontend.h" // mtlc_type_from_frontend (value_type baking)

int ir_lower_statement_or_expression(IRLoweringContext *context,
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
  case AST_GPU_LAUNCH:
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


int ir_lower_call_expression(IRLoweringContext *context,
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

  if (call->is_gpu_async_copy) {
    IRInstruction instruction = {0};
    instruction.location = expression->location;
    if (!strcmp(call->function_name, "async_copy_workgroup")) {
      instruction.async_copy_element_count = call->async_copy_element_count;
      instruction.async_copy_transaction_bytes =
          call->async_copy_transaction_bytes;
      instruction.async_copy_cache = call->async_copy_cache;
      if (call->argument_count < 3 ||
          instruction.async_copy_element_count == 0) {
        ir_set_error(context,
                     "Invalid asynchronous workgroup copy reached IR lowering");
        return 0;
      }
      instruction.op = IR_OP_ASYNC_COPY;
      instruction.argument_count = 2;
      instruction.arguments = calloc(2, sizeof(*instruction.arguments));
      instruction.argument_types =
          calloc(2, sizeof(*instruction.argument_types));
      if (!instruction.arguments || !instruction.argument_types) {
        free(instruction.arguments);
        free(instruction.argument_types);
        ir_set_error(context, "Out of memory lowering asynchronous copy");
        return 0;
      }
      for (size_t i = 0; i < 2; i++) {
        ASTNode *argument = call->arguments[i];
        if (!argument ||
            !ir_lower_expression(context, function, argument,
                                 &instruction.arguments[i])) {
          for (size_t j = 0; j < i; j++)
            ir_operand_destroy(&instruction.arguments[j]);
          free(instruction.arguments);
          free(instruction.argument_types);
          return 0;
        }
        instruction.argument_types[i] =
            argument->resolved_type
                ? mtlc_type_from_frontend(argument->resolved_type)
                : NULL;
      }
    } else if (!strcmp(call->function_name, "async_copy_commit")) {
      instruction.op = IR_OP_ASYNC_COMMIT;
    } else if (!strcmp(call->function_name, "async_copy_wait")) {
      instruction.op = IR_OP_ASYNC_WAIT;
      instruction.async_copy_pending_groups =
          call->async_copy_pending_groups;
    } else {
      ir_set_error(context,
                   "Unknown asynchronous workgroup copy operation reached IR lowering");
      return 0;
    }
    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < instruction.argument_count; i++)
        ir_operand_destroy(&instruction.arguments[i]);
      free(instruction.arguments);
      free(instruction.argument_types);
      return 0;
    }
    for (size_t i = 0; i < instruction.argument_count; i++)
      ir_operand_destroy(&instruction.arguments[i]);
    free(instruction.arguments);
    free(instruction.argument_types);
    *out_value = ir_operand_none();
    return 1;
  }

  if (call->is_tensor_transfer) {
    int has_view = call->tensor_transfer_view_argument != SIZE_MAX;
    size_t count = ir_tensor_transfer_operand_count(
        &call->tensor_transfer_desc, has_view);
    size_t source_indices[3 + MTLC_TENSOR_MAX_RANK] = {0};
    size_t source_count = 0;
    IROperand *arguments = NULL;
    MtlcType **argument_types = NULL;
    source_indices[source_count++] = 0;
    source_indices[source_count++] = 1;
    if (has_view)
      source_indices[source_count++] = call->tensor_transfer_view_argument;
    for (uint8_t dimension = 0;
         dimension < call->tensor_transfer_desc.rank; dimension++)
      source_indices[source_count++] =
          call->tensor_transfer_coordinate_arguments[dimension];
    if (!count || count != source_count) {
      ir_set_error(context,
                   "Invalid tensor transfer descriptor reached IR lowering");
      return 0;
    }
    arguments = calloc(count, sizeof(*arguments));
    argument_types = calloc(count, sizeof(*argument_types));
    if (!arguments || !argument_types) {
      free(arguments);
      free(argument_types);
      ir_set_error(context, "Out of memory lowering tensor transfer");
      return 0;
    }
    for (size_t i = 0; i < count; i++) {
      size_t source_index = source_indices[i];
      ASTNode *source = source_index < call->argument_count
                            ? call->arguments[source_index]
                            : NULL;
      if (!source || !ir_lower_expression(context, function, source,
                                          &arguments[i])) {
        for (size_t j = 0; j < i; j++) ir_operand_destroy(&arguments[j]);
        free(arguments);
        free(argument_types);
        return 0;
      }
      argument_types[i] = source->resolved_type
                              ? mtlc_type_from_frontend(source->resolved_type)
                              : NULL;
    }
    IRInstruction instruction = {0};
    /* Stack block: ir_emit deep-copies it, and heap_owned == 0 means no destroy
     * path can try to free stack memory. */
    IRTensorAux tensor;
    ir_instruction_tensor_attach(&instruction, &tensor);
    tensor.transfer = call->tensor_transfer_desc;
    instruction.op = IR_OP_TENSOR_TRANSFER;
    instruction.location = expression->location;
    instruction.arguments = arguments;
    instruction.argument_types = argument_types;
    instruction.argument_count = count;
    instruction.tensor_transfer_has_prepared_view = has_view;
    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
      free(arguments);
      free(argument_types);
      return 0;
    }
    for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
    free(arguments);
    free(argument_types);
    *out_value = ir_operand_none();
    return 1;
  }

  if (call->is_tensor_epilogue) {
    size_t count =
        ir_tensor_epilogue_operand_count(&call->tensor_epilogue_desc);
    size_t source_indices[8] = {0, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                                SIZE_MAX, SIZE_MAX, SIZE_MAX};
    size_t source_count = 1;
    IROperand *arguments = NULL;
    MtlcType **argument_types = NULL;
    if (call->tensor_epilogue_bias_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_epilogue_bias_argument;
    if (call->tensor_epilogue_alpha_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_epilogue_alpha_argument;
    if (call->tensor_epilogue_beta_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_epilogue_beta_argument;
    if (call->tensor_epilogue_clamp_min_argument != SIZE_MAX)
      source_indices[source_count++] =
          call->tensor_epilogue_clamp_min_argument;
    if (call->tensor_epilogue_clamp_max_argument != SIZE_MAX)
      source_indices[source_count++] =
          call->tensor_epilogue_clamp_max_argument;
    if (call->tensor_epilogue_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_epilogue_stride_argument;
    if (call->tensor_epilogue_bias_stride_argument != SIZE_MAX)
      source_indices[source_count++] =
          call->tensor_epilogue_bias_stride_argument;
    if (!count || count != source_count) {
      ir_set_error(context,
                   "Invalid tensor epilogue descriptor reached IR lowering");
      return 0;
    }
    arguments = calloc(count, sizeof(*arguments));
    argument_types = calloc(count, sizeof(*argument_types));
    if (!arguments || !argument_types) {
      free(arguments);
      free(argument_types);
      ir_set_error(context, "Out of memory lowering tensor epilogue");
      return 0;
    }
    for (size_t i = 0; i < count; i++) {
      size_t source_index = source_indices[i];
      ASTNode *source = source_index < call->argument_count
                            ? call->arguments[source_index]
                            : NULL;
      if (!source || !ir_lower_expression(context, function, source,
                                          &arguments[i])) {
        for (size_t j = 0; j < i; j++) ir_operand_destroy(&arguments[j]);
        free(arguments);
        free(argument_types);
        return 0;
      }
      argument_types[i] = source->resolved_type
                              ? mtlc_type_from_frontend(source->resolved_type)
                              : NULL;
    }
    IRInstruction instruction = {0};
    IRTensorAux tensor;
    ir_instruction_tensor_attach(&instruction, &tensor);
    tensor.epilogue = call->tensor_epilogue_desc;
    instruction.op = IR_OP_TENSOR_EPILOGUE;
    instruction.location = expression->location;
    instruction.arguments = arguments;
    instruction.argument_types = argument_types;
    instruction.argument_count = count;
    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
      free(arguments);
      free(argument_types);
      return 0;
    }
    for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
    free(arguments);
    free(argument_types);
    *out_value = ir_operand_none();
    return 1;
  }

  if (call->is_tensor_mma || call->is_tensor_matmul) {
    size_t count = call->is_tensor_matmul
                       ? ir_tensor_matmul_operand_count(&call->tensor_mma_desc)
                       : ir_tensor_mma_operand_count(&call->tensor_mma_desc);
    size_t source_indices[16] = {0, 1, 2, 3, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                                 SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                                 SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX,
                                 SIZE_MAX};
    size_t source_count = 4;
    IROperand *arguments = NULL;
    MtlcType **argument_types = NULL;
    if (call->tensor_metadata_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_metadata_argument;
    if (call->tensor_a_scale_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_a_scale_argument;
    if (call->tensor_b_scale_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_b_scale_argument;
    if (call->tensor_a_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_a_stride_argument;
    if (call->tensor_b_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_b_stride_argument;
    if (call->tensor_c_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_c_stride_argument;
    if (call->tensor_d_stride_argument != SIZE_MAX)
      source_indices[source_count++] = call->tensor_d_stride_argument;
    if (call->is_tensor_matmul) {
      for (size_t i = 4; i < 9; i++) source_indices[source_count++] = i;
    }
    if (!count || count != source_count) {
      ir_set_error(context,
                   "Invalid tensor matrix descriptor reached IR lowering");
      return 0;
    }
    arguments = calloc(count, sizeof(*arguments));
    argument_types = calloc(count, sizeof(*argument_types));
    if (!arguments || !argument_types) {
      free(arguments);
      free(argument_types);
      ir_set_error(context, "Out of memory lowering tensor matrix operation");
      return 0;
    }
    for (size_t i = 0; i < count; i++) {
      size_t source_index = source_indices[i];
      ASTNode *source = source_index < call->argument_count
                            ? call->arguments[source_index]
                            : NULL;
      if (!source || !ir_lower_expression(context, function, source,
                                          &arguments[i])) {
        for (size_t j = 0; j < i; j++) ir_operand_destroy(&arguments[j]);
        free(arguments);
        free(argument_types);
        return 0;
      }
      argument_types[i] = source->resolved_type
                              ? mtlc_type_from_frontend(source->resolved_type)
                              : NULL;
    }
    IRInstruction instruction = {0};
    IRTensorAux tensor;
    ir_instruction_tensor_attach(&instruction, &tensor);
    tensor.mma = call->tensor_mma_desc;
    instruction.op = call->is_tensor_matmul ? IR_OP_TENSOR_MATMUL
                                            : IR_OP_TENSOR_MMA;
    instruction.location = expression->location;
    instruction.arguments = arguments;
    instruction.argument_types = argument_types;
    instruction.argument_count = count;
    if (!ir_emit(context, function, &instruction)) {
      for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
      free(arguments);
      free(argument_types);
      return 0;
    }
    for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
    free(arguments);
    free(argument_types);
    *out_value = ir_operand_none();
    return 1;
  }

  if (call->is_gpu_atomic) {
    MtlcIntrinsic intrinsic = ir_intrinsic_from_name(call->function_name);
    int arity = ir_intrinsic_arity(intrinsic);
    int returns_void =
        ir_intrinsic_atomic_result_kind(intrinsic) == MTLC_TYPE_VOID;
    IROperand destination = ir_operand_none();
    IROperand *arguments = NULL;
    if (!ir_intrinsic_is_atomic(intrinsic) || arity < 0 ||
        call->argument_count < (size_t)arity ||
        call->atomic_address_space == MTLC_ADDRESS_SPACE_DEFAULT ||
        call->atomic_memory_order == MTLC_MEMORY_ORDER_DEFAULT ||
        call->atomic_memory_scope == MTLC_MEMORY_SCOPE_DEFAULT) {
      ir_set_error(context, "Invalid native atomic reached IR lowering");
      return 0;
    }
    if (!returns_void && !ir_make_temp_operand(context, &destination))
      return 0;
    arguments = calloc((size_t)arity, sizeof(*arguments));
    if (!arguments) {
      ir_operand_destroy(&destination);
      ir_set_error(context, "Out of memory lowering native atomic");
      return 0;
    }
    for (int i = 0; i < arity; i++) {
      if (!ir_lower_expression(context, function, call->arguments[i],
                               &arguments[i])) {
        for (int j = 0; j < i; j++) ir_operand_destroy(&arguments[j]);
        free(arguments);
        ir_operand_destroy(&destination);
        return 0;
      }
    }
    IRInstruction instruction = {0};
    instruction.op = IR_OP_CALL;
    instruction.location = expression->location;
    instruction.dest = destination;
    instruction.arguments = arguments;
    instruction.argument_count = (size_t)arity;
    instruction.text = call->function_name;
    instruction.intrinsic = intrinsic;
    instruction.address_space = call->atomic_address_space;
    instruction.memory_order = call->atomic_memory_order;
    instruction.failure_memory_order = call->atomic_failure_order;
    instruction.memory_scope = call->atomic_memory_scope;
    instruction.value_type = expression->resolved_type
                                 ? mtlc_type_from_frontend(
                                       expression->resolved_type)
                                 : NULL;
    int ok = ir_emit(context, function, &instruction);
    for (int i = 0; i < arity; i++) ir_operand_destroy(&arguments[i]);
    free(arguments);
    if (!ok) {
      ir_operand_destroy(&destination);
      return 0;
    }
    *out_value = destination;
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
  /* A void call has no SSA result. Keeping a synthetic destination used to be
   * mostly harmless for the host backend, but it gives target-neutral device
   * calls a false value and makes a frontend detail leak into both GPU ABIs. */
  int returns_void = expression->resolved_type &&
                     expression->resolved_type->kind == TYPE_VOID;
  if (!returns_void && !ir_make_temp_operand(context, &destination)) {
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

  if (call->callee_closure_env) {
    /* Closure call: the variable holds an 8-byte pointer to a heap record whose
     * field 0 is the code pointer. Load the code pointer, then call it passing
     * the record pointer (the environment) as a hidden leading argument that the
     * lifted function receives as its first parameter. */
    IROperand code = ir_operand_none();
    if (!ir_make_temp_operand(context, &code)) {
      for (size_t i = 0; i < call->argument_count; i++)
        ir_operand_destroy(&arguments[i]);
      free(arguments);
      ir_operand_destroy(&destination);
      return 0;
    }
    IRInstruction load = {0};
    load.op = IR_OP_LOAD;
    load.location = expression->location;
    load.dest = code;
    load.lhs = ir_operand_symbol(call->function_name);
    load.rhs = ir_operand_int(8);
    int load_ok = ir_emit(context, function, &load);
    ir_operand_destroy(&load.lhs);
    if (!load_ok) {
      for (size_t i = 0; i < call->argument_count; i++)
        ir_operand_destroy(&arguments[i]);
      free(arguments);
      ir_operand_destroy(&code);
      ir_operand_destroy(&destination);
      return 0;
    }

    IROperand *cargs = calloc(call->argument_count + 1, sizeof(IROperand));
    if (!cargs) {
      for (size_t i = 0; i < call->argument_count; i++)
        ir_operand_destroy(&arguments[i]);
      free(arguments);
      ir_operand_destroy(&code);
      ir_operand_destroy(&destination);
      return 0;
    }
    cargs[0] = ir_operand_symbol(call->function_name);
    for (size_t i = 0; i < call->argument_count; i++)
      cargs[i + 1] = arguments[i];
    free(arguments);

    IRInstruction cinstr = {0};
    cinstr.op = IR_OP_CALL_INDIRECT;
    cinstr.location = expression->location;
    cinstr.dest = destination;
    cinstr.lhs = code;
    cinstr.arguments = cargs;
    cinstr.argument_count = call->argument_count + 1;
    int ok = ir_emit(context, function, &cinstr);
    for (size_t i = 0; i < call->argument_count + 1; i++)
      ir_operand_destroy(&cargs[i]);
    free(cargs);
    ir_operand_destroy(&code);
    if (!ok) {
      ir_operand_destroy(&destination);
      return 0;
    }
    *out_value = destination;
    return 1;
  }

  IRInstruction instruction = {0};
  instruction.location = expression->location;
  instruction.dest = destination;
  instruction.arguments = arguments;
  instruction.argument_count = call->argument_count;
  instruction.value_type = expression->resolved_type
                               ? mtlc_type_from_frontend(expression->resolved_type)
                               : NULL;

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
    instruction.intrinsic = ir_intrinsic_from_name(call->function_name);
    if (ir_intrinsic_is_atomic(instruction.intrinsic)) {
      instruction.address_space = MTLC_ADDRESS_SPACE_GLOBAL;
      instruction.memory_order = MTLC_MEMORY_ORDER_RELAXED;
      instruction.failure_memory_order = MTLC_MEMORY_ORDER_RELAXED;
      instruction.memory_scope = MTLC_MEMORY_SCOPE_DEVICE;
    }
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

int ir_emit_condition_false_branch(IRLoweringContext *context,
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

int ir_emit_condition_true_branch(IRLoweringContext *context,
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

int ir_lower_expression(IRLoweringContext *context, IRFunction *function,
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
        /* Bake the result type onto the IR so codegen reads it instead of
         * re-inferring from the AST (replaces code_generator_infer_expression_type;
         * mirrors its primary path). */
        instruction.value_type = mtlc_type_from_frontend(
            type_checker_infer_type(context->type_checker, expression));
        /* String '+' becomes a heap-allocating concat kernel in codegen; mark
         * it so the `@noalloc` contract checker can see the allocation. */
        instruction.allocates = 1;
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

  case AST_CLOSURE_ADAPT_EXPRESSION: {
    /* A thin value (`&func` or a non-capturing lambda) wrapped by the
     * closure-adapt pass to satisfy an `Fn(...)` boundary: lower the thin value,
     * then call the generated adapter constructor with it as the sole argument
     * to produce a real closure value. */
    ClosureAdapt *adapt = (ClosureAdapt *)expression->data;
    if (!adapt || !adapt->ctor_name || !adapt->inner) {
      ir_set_error(context, "Internal: closure adapter was not synthesized");
      return 0;
    }
    IROperand thin_val = ir_operand_none();
    if (!ir_lower_expression(context, function, adapt->inner, &thin_val)) {
      return 0;
    }
    IROperand dest = ir_operand_none();
    if (!ir_make_temp_operand(context, &dest)) {
      ir_operand_destroy(&thin_val);
      return 0;
    }
    IROperand args[1];
    args[0] = thin_val;
    IRInstruction call = {0};
    call.op = IR_OP_CALL;
    call.location = expression->location;
    call.dest = dest;
    call.text = adapt->ctor_name;
    call.arguments = args;
    call.argument_count = 1;
    int ok = ir_emit(context, function, &call);
    ir_operand_destroy(&thin_val);
    if (!ok) {
      ir_operand_destroy(&dest);
      return 0;
    }
    *out_value = dest;
    return 1;
  }

  case AST_LAMBDA_EXPRESSION: {
    FunctionDeclaration *lam = (FunctionDeclaration *)expression->data;
    if (!lam || !lam->name) {
      ir_set_error(context, "Internal: lambda was not converted");
      return 0;
    }
    if (lam->captured_count > 0) {
      /* Capturing closure value: call the synthesized constructor with the
       * current value of each captured variable; it allocates and populates the
       * environment record and returns the 8-byte closure pointer. */
      IROperand dest = ir_operand_none();
      if (!ir_make_temp_operand(context, &dest)) {
        return 0;
      }
      IROperand *args = calloc(lam->captured_count, sizeof(IROperand));
      if (!args) {
        ir_operand_destroy(&dest);
        return 0;
      }
      for (size_t i = 0; i < lam->captured_count; i++)
        args[i] = ir_operand_symbol(lam->captured_names[i]);
      IRInstruction call = {0};
      call.op = IR_OP_CALL;
      call.location = expression->location;
      call.dest = dest;
      call.text = lam->name;
      call.arguments = args;
      call.argument_count = lam->captured_count;
      int ok = ir_emit(context, function, &call);
      for (size_t i = 0; i < lam->captured_count; i++)
        ir_operand_destroy(&args[i]);
      free(args);
      if (!ok) {
        ir_operand_destroy(&dest);
        return 0;
      }
      *out_value = dest;
      return 1;
    }
    /* A non-capturing lambda is the address of its lifted top-level function. */
    return ir_emit_address_of_symbol(context, function, lam->name,
                                     expression->location, out_value);
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
      ir_load_apply_unsigned(&load, target_type);
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

    /* Aggregate-typed member/index read by value (a struct or array, e.g.
     * `cfg.rect` passed by value). The backend cannot LOAD more than a register
     * word, and ir_type_storage_size() collapses the rest to 8 bytes -- which
     * would copy only the first word and leave the remainder stack garbage.
     * Instead copy the whole aggregate into a fresh named local via the
     * wide-STORE memcpy path and yield that local as the value. */
    if (value_type->name &&
        (value_type->kind == TYPE_STRUCT || value_type->kind == TYPE_ARRAY) &&
        value_type->size > 8 && value_type->size <= (size_t)INT_MAX) {
      char *agg_name = ir_new_label_name(context, "agg_byval");
      IROperand dest_addr = ir_operand_none();
      IRInstruction store = {0};
      int ok = 0;

      ir_operand_destroy(&destination);
      if (!agg_name) {
        ir_operand_destroy(&address);
        ir_set_error(context, "Out of memory while copying aggregate value");
        return 0;
      }
      ok = ir_emit_local_declaration(context, function, agg_name,
                                     value_type->name, expression->location) &&
           ir_emit_address_of_symbol(context, function, agg_name,
                                     expression->location, &dest_addr);
      if (!ok) {
        free(agg_name);
        ir_operand_destroy(&dest_addr);
        ir_operand_destroy(&address);
        return 0;
      }

      store.op = IR_OP_STORE;
      store.location = expression->location;
      store.dest = dest_addr;
      store.lhs = address;
      store.rhs = ir_operand_int((long long)value_type->size);
      ok = ir_emit(context, function, &store);
      ir_operand_destroy(&dest_addr);
      ir_operand_destroy(&address);
      if (!ok) {
        free(agg_name);
        return 0;
      }

      *out_value = ir_operand_symbol(agg_name);
      free(agg_name);
      if (!out_value->name) {
        ir_set_error(context, "Out of memory while copying aggregate value");
        return 0;
      }
      return 1;
    }

    IRInstruction load = {0};
    load.op = IR_OP_LOAD;
    load.location = expression->location;
    load.dest = destination;
    load.lhs = address;
    load.rhs = ir_operand_int(ir_type_storage_size(value_type));
    ir_load_apply_float_type(&load, value_type);
    ir_load_apply_unsigned(&load, value_type);
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

    if (func_type && func_type->kind == TYPE_FUNCTION_POINTER &&
        func_type->closure_env) {
      /* Closure value: func_ptr is the environment pointer. Load the code
       * pointer from field 0 and pass the environment as a hidden leading
       * argument. */
      IROperand code = ir_operand_none();
      if (!ir_make_temp_operand(context, &code)) {
        for (size_t i = 0; i < fp_call->argument_count; i++)
          ir_operand_destroy(&arguments[i]);
        free(arguments);
        ir_operand_destroy(&func_ptr);
        ir_operand_destroy(&destination);
        return 0;
      }
      IRInstruction load = {0};
      load.op = IR_OP_LOAD;
      load.location = expression->location;
      load.dest = code;
      load.lhs = func_ptr;
      load.rhs = ir_operand_int(8);
      int load_ok = ir_emit(context, function, &load);
      if (!load_ok) {
        for (size_t i = 0; i < fp_call->argument_count; i++)
          ir_operand_destroy(&arguments[i]);
        free(arguments);
        ir_operand_destroy(&func_ptr);
        ir_operand_destroy(&code);
        ir_operand_destroy(&destination);
        return 0;
      }
      IROperand *cargs = calloc(fp_call->argument_count + 1, sizeof(IROperand));
      if (!cargs) {
        for (size_t i = 0; i < fp_call->argument_count; i++)
          ir_operand_destroy(&arguments[i]);
        free(arguments);
        ir_operand_destroy(&func_ptr);
        ir_operand_destroy(&code);
        ir_operand_destroy(&destination);
        return 0;
      }
      cargs[0] = func_ptr;
      for (size_t i = 0; i < fp_call->argument_count; i++)
        cargs[i + 1] = arguments[i];
      free(arguments);
      IRInstruction cinstr = {0};
      cinstr.op = IR_OP_CALL_INDIRECT;
      cinstr.location = expression->location;
      cinstr.dest = destination;
      cinstr.lhs = code;
      cinstr.arguments = cargs;
      cinstr.argument_count = fp_call->argument_count + 1;
      int ok = ir_emit(context, function, &cinstr);
      for (size_t i = 0; i < fp_call->argument_count + 1; i++)
        ir_operand_destroy(&cargs[i]);
      free(cargs);
      ir_operand_destroy(&code);
      if (!ok) {
        ir_operand_destroy(&destination);
        return 0;
      }
      *out_value = destination;
      return 1;
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
