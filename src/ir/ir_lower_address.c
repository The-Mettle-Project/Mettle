#include "ir_lowering_internal.h"
#include "ir_explain_ledger.h"
#include "ir_explain_safety.h"
#include "frontend/mtlc_frontend.h"

static int ir_emit_packed_store(IRLoweringContext *context, IRFunction *function,
                                const IROperand *word_address,
                                const TypeMetaPlace *place,
                                const IROperand *value,
                                SourceLocation location);

static void ir_safety_describe(const ASTNode *expression, char *buffer,
                               size_t capacity, int depth) {
  if (!buffer || capacity == 0) {
    return;
  }
  buffer[0] = '\0';
  if (!expression || depth > 4) {
    snprintf(buffer, capacity, "?");
    return;
  }

  switch (expression->type) {
  case AST_IDENTIFIER: {
    const Identifier *identifier = (const Identifier *)expression->data;
    snprintf(buffer, capacity, "%s",
             identifier && identifier->name ? identifier->name : "?");
    return;
  }
  case AST_INDEX_EXPRESSION: {
    const ArrayIndexExpression *index =
        (const ArrayIndexExpression *)expression->data;
    char inner[96];
    ir_safety_describe(index ? index->array : NULL, inner, sizeof(inner),
                       depth + 1);
    snprintf(buffer, capacity, "%s[]", inner);
    return;
  }
  case AST_MEMBER_ACCESS: {
    const MemberAccess *member = (const MemberAccess *)expression->data;
    char inner[96];
    ir_safety_describe(member ? member->object : NULL, inner, sizeof(inner),
                       depth + 1);
    snprintf(buffer, capacity, "%s.%s", inner,
             member && member->member ? member->member : "?");
    return;
  }
  case AST_UNARY_EXPRESSION: {
    const UnaryExpression *unary = (const UnaryExpression *)expression->data;
    char inner[96];
    ir_safety_describe(unary ? unary->operand : NULL, inner, sizeof(inner),
                       depth + 1);
    snprintf(buffer, capacity, "*%s", inner);
    return;
  }
  default:
    snprintf(buffer, capacity, "?");
    return;
  }
}

static char *ir_intern_aggregate_literal(IRLoweringContext *context,
                                         ASTNode *literal_node,
                                         Type *dest_type) {
  AggregateLiteral *literal =
      literal_node && literal_node->type == AST_AGGREGATE_LITERAL
          ? (AggregateLiteral *)literal_node->data
          : NULL;

  if (!context || !context->program || !literal || !literal->image ||
      !dest_type) {
    ir_set_error(context, "Aggregate literal reached lowering without a folded "
                          "constant image");
    return NULL;
  }

  char *name = ir_new_label_name(context, "agg_const");
  if (!name) {
    ir_set_error(context, "Out of memory while interning aggregate literal");
    return NULL;
  }

  IRInitReloc *relocs = NULL;
  if (literal->reloc_count > 0) {
    relocs = calloc(literal->reloc_count, sizeof(IRInitReloc));
    if (!relocs) {
      free(name);
      ir_set_error(context, "Out of memory while interning aggregate literal");
      return NULL;
    }
    for (size_t i = 0; i < literal->reloc_count; i++) {
      relocs[i].offset = literal->relocs[i].offset;
      relocs[i].symbol = literal->relocs[i].symbol;
      relocs[i].string = literal->relocs[i].string;
      relocs[i].string_length = literal->relocs[i].string_length;
      relocs[i].string_wants_record = literal->relocs[i].string_wants_record;
    }
  }

  IRModuleSymbol entry = {0};
  entry.name = name;
  entry.kind = IR_MODSYM_VARIABLE;
  entry.type = mtlc_type_from_frontend(dest_type);
  entry.init_bytes = literal->image;
  entry.init_bytes_size = literal->image_size;
  entry.init_relocs = relocs;
  entry.init_reloc_count = literal->reloc_count;
  IRModuleSymbol *added = ir_program_add_symbol(context->program, &entry);
  free(relocs);
  if (!added) {
    free(name);
    ir_set_error(context, "Out of memory while interning aggregate literal");
    return NULL;
  }
  return name;
}

static int ir_emit_aggregate_runtime_stores(IRLoweringContext *context,
                                            IRFunction *function,
                                            const IROperand *dest_address,
                                            ASTNode *literal_node,
                                            SourceLocation location) {
  AggregateLiteral *literal =
      literal_node && literal_node->type == AST_AGGREGATE_LITERAL
          ? (AggregateLiteral *)literal_node->data
          : NULL;
  size_t i;

  if (!literal || literal->runtime_store_count == 0) {
    return 1;
  }

  for (i = 0; i < literal->runtime_store_count; i++) {
    AggregateRuntimeStore *entry = &literal->runtime_stores[i];
    Type *element_type = (Type *)entry->element_type;
    IROperand value = ir_operand_none();
    IROperand slot = ir_operand_none();
    IRInstruction store = {0};

    if (!element_type ||
        !ir_lower_expression(context, function, entry->element, &value)) {
      ir_operand_destroy(&value);
      return 0;
    }
    if (!ir_normalize_to_bool(context, function, element_type, entry->element,
                              &value, entry->element->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
    if (ir_should_decay_array_to_address(element_type, entry->element) &&
        !ir_decay_array_operand_to_address(context, function, &value,
                                           entry->element->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
    if (ir_should_build_slice_from_array(element_type, entry->element) &&
        !ir_build_slice_operand_from_array(
            context, function, &value, entry->element->resolved_type,
            element_type, entry->element->location)) {
      ir_operand_destroy(&value);
      return 0;
    }
    if (!ir_emit_address_with_offset(context, function, dest_address,
                                     entry->offset, location, &slot)) {
      ir_operand_destroy(&value);
      return 0;
    }
    if (entry->packed_place) {
      int stored = ir_emit_packed_store(
          context, function, &slot, (const TypeMetaPlace *)entry->packed_place,
          &value, location);
      ir_operand_destroy(&value);
      ir_operand_destroy(&slot);
      if (!stored) {
        return 0;
      }
      continue;
    }
    if (ir_try_emit_aggregate_address_memcpy(context, function, &slot, &value,
                                             element_type, location)) {
      ir_operand_destroy(&value);
      ir_operand_destroy(&slot);
      continue;
    }
    store.op = IR_OP_STORE;
    store.location = location;
    store.dest = slot;
    store.lhs = value;
    store.rhs = ir_operand_int(ir_type_storage_size(element_type));
    ir_access_apply_alias_class(&store, element_type);
    if (element_type->kind == TYPE_FLOAT32 ||
        element_type->kind == TYPE_FLOAT64 ||
        element_type->kind == TYPE_FLOAT16 ||
        element_type->kind == TYPE_BFLOAT16) {
      ir_assign_apply_float_bits(&store, &store.lhs,
                                 ir_type_float_bits(element_type));
    }
    if (!ir_emit(context, function, &store)) {
      ir_operand_destroy(&value);
      ir_operand_destroy(&slot);
      return 0;
    }
    ir_operand_destroy(&value);
    ir_operand_destroy(&slot);
  }
  return 1;
}

int ir_emit_aggregate_literal_copy(IRLoweringContext *context,
                                   IRFunction *function,
                                   const IROperand *dest_address,
                                   ASTNode *literal_node, Type *dest_type,
                                   SourceLocation location) {
  if (!context || !function || !dest_address || !dest_type ||
      dest_type->size == 0 || dest_type->size > (size_t)INT_MAX) {
    ir_set_error(context, "Cannot copy aggregate literal into a target of "
                          "unknown size");
    return 0;
  }

  char *source_name = ir_intern_aggregate_literal(context, literal_node,
                                                  dest_type);
  if (!source_name) {
    return 0;
  }

  IROperand source_address = ir_operand_none();
  if (!ir_emit_address_of_symbol(context, function, source_name, location,
                                 &source_address)) {
    free(source_name);
    return 0;
  }
  free(source_name);

  IROperand value = source_address;
  if (dest_type->size == 1 || dest_type->size == 2 || dest_type->size == 4 ||
      dest_type->size == 8) {
    IROperand loaded = ir_operand_none();
    if (!ir_make_temp_operand(context, &loaded)) {
      ir_operand_destroy(&source_address);
      return 0;
    }
    IRInstruction load = {0};
    load.op = IR_OP_LOAD;
    load.location = location;
    load.dest = loaded;
    load.lhs = source_address;
    load.rhs = ir_operand_int((long long)dest_type->size);
    if (!ir_emit(context, function, &load)) {
      ir_operand_destroy(&loaded);
      ir_operand_destroy(&source_address);
      return 0;
    }
    ir_operand_destroy(&source_address);
    value = loaded;
  }

  IRInstruction store = {0};
  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = ir_clone_operand_local(dest_address);
  store.lhs = value;
  store.rhs = ir_operand_int((long long)dest_type->size);
  ir_access_apply_alias_class(&store, dest_type);
  int ok = ir_emit(context, function, &store);
  ir_operand_destroy(&store.dest);
  ir_operand_destroy(&value);
  if (!ok) {
    return 0;
  }
  return ir_emit_aggregate_runtime_stores(context, function, dest_address,
                                          literal_node, location);
}

#define IR_ZERO_FILL_STORE_MAX 256u

int ir_emit_zero_fill_local(IRLoweringContext *context, IRFunction *function,
                            const char *local_name, Type *type,
                            SourceLocation location) {
  if (!context || !function || !local_name || !type) {
    return 0;
  }
  if (type->size == 0 || type->size > (size_t)INT_MAX) {
    return 1;
  }

  IROperand address = ir_operand_none();
  if (!ir_emit_address_of_symbol(context, function, local_name, location,
                                 &address)) {
    return 0;
  }

  if (type->size <= IR_ZERO_FILL_STORE_MAX) {
    static const size_t widths[] = {8, 4, 2, 1};
    size_t offset = 0;
    size_t w = 0;
    int ok = 1;

    for (w = 0; w < sizeof(widths) / sizeof(widths[0]) && ok; w++) {
      while (ok && type->size - offset >= widths[w]) {
        IROperand slot = ir_operand_none();
        if (!ir_emit_address_with_offset(context, function, &address, offset,
                                         location, &slot)) {
          ok = 0;
          break;
        }
        IRInstruction store = {0};
        store.op = IR_OP_STORE;
        store.location = location;
        store.dest = slot;
        store.lhs = ir_operand_int(0);
        store.rhs = ir_operand_int((long long)widths[w]);
        ir_access_apply_alias_class(&store, type);
        ok = ir_emit(context, function, &store);
        ir_operand_destroy(&store.dest);
        ir_operand_destroy(&store.lhs);
        offset += widths[w];
      }
    }
    ir_operand_destroy(&address);
    return ok;
  }

  IRInstruction call = {0};
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = "memset";
  call.argument_count = 3;
  call.arguments = calloc(3, sizeof(IROperand));
  if (!call.arguments) {
    ir_operand_destroy(&address);
    ir_set_error(context, "Out of memory while zeroing local '%s'", local_name);
    return 0;
  }
  call.arguments[0] = address;
  call.arguments[1] = ir_operand_int(0);
  call.arguments[2] = ir_operand_int((long long)type->size);

  int ok = ir_emit(context, function, &call);
  ir_operand_destroy(&call.arguments[0]);
  ir_operand_destroy(&call.arguments[1]);
  ir_operand_destroy(&call.arguments[2]);
  free(call.arguments);
  return ok;
}

int ir_emit_aggregate_literal_copy_to_symbol(IRLoweringContext *context,
                                             IRFunction *function,
                                             const char *dest_name,
                                             ASTNode *literal_node,
                                             Type *dest_type,
                                             SourceLocation location) {
  IROperand dest_address = ir_operand_none();
  if (!ir_emit_address_of_symbol(context, function, dest_name, location,
                                 &dest_address)) {
    return 0;
  }
  int ok = ir_emit_aggregate_literal_copy(context, function, &dest_address,
                                          literal_node, dest_type, location);
  ir_operand_destroy(&dest_address);
  return ok;
}

static int ir_symbol_is_address_space_allocation(const IRFunction *function,
                                                 const char *name) {
  if (!function || !name) return 0;
  for (size_t i = function->instruction_count; i-- > 0;) {
    const IRInstruction *instruction = &function->instructions[i];
    if ((instruction->op != IR_OP_ADDRESS_SPACE_ALLOC &&
         instruction->op != IR_OP_DECLARE_LOCAL) ||
        instruction->dest.kind != IR_OPERAND_SYMBOL ||
        !instruction->dest.name || strcmp(instruction->dest.name, name) != 0) {
      continue;
    }
    return instruction->op == IR_OP_ADDRESS_SPACE_ALLOC;
  }
  return 0;
}

static int ir_expression_is_address_space_allocation(
    IRLoweringContext *context, const IRFunction *function,
    const ASTNode *expression) {
  if (!expression || expression->type != AST_IDENTIFIER || !expression->data) {
    return 0;
  }
  const Identifier *identifier = (const Identifier *)expression->data;
  return identifier->name &&
         ir_symbol_is_address_space_allocation(
             function, ir_local_ir_name(context, identifier->name));
}

int ir_emit_local_declaration(IRLoweringContext *context,
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
  local.text = (char *)ir_backend_type_name(type_name);
  {
    Type *resolved = ir_resolve_named_type(context, type_name);
    if (resolved) {
      local.value_type = mtlc_type_from_frontend(resolved);
    }
  }
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

IROperand ir_clone_operand_local(const IROperand *operand) {
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
    return ir_operand_string_n(operand->name,
                               ir_operand_string_length(operand));
  case IR_OPERAND_LABEL:
    return ir_operand_label(operand->name);
  case IR_OPERAND_NONE:
  default:
    return ir_operand_none();
  }
}

static int ir_spill_aggregate_value_to_local(IRLoweringContext *context,
                                             IRFunction *function,
                                             const IROperand *value,
                                             Type *dest_type,
                                             SourceLocation location,
                                             IROperand *out_symbol);

static int ir_function_has_parameter(const IRFunction *function,
                                     const char *name) {
  for (size_t i = 0; i < function->parameter_count; i++) {
    if (function->parameter_names[i] &&
        strcmp(function->parameter_names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_try_emit_aggregate_symbol_memcpy(
    IRLoweringContext *context, IRFunction *function, const char *dest_name,
    const IROperand *value, Type *dest_type, SourceLocation location) {
  int nbytes = 0;

  if (!context || !function || !dest_name || !value) {
    return 0;
  }
  if (!dest_type ||
      (dest_type->kind != TYPE_STRUCT && dest_type->kind != TYPE_STRING &&
       dest_type->kind != TYPE_SLICE &&
       dest_type->kind != TYPE_TAGGED_ENUM)) {
    return 0;
  }
  if (dest_type->size == 0 || dest_type->size > (size_t)INT_MAX) {
    return 0;
  }
  nbytes = (int)dest_type->size;
  if (nbytes <= 8) {
    return 0;
  }
  if (value->kind != IR_OPERAND_SYMBOL || !value->name) {
    IROperand spilled = ir_operand_none();
    int copied = 0;
    if (ir_function_has_parameter(function, dest_name) &&
        ir_spill_aggregate_value_to_local(context, function, value, dest_type,
                                          location, &spilled)) {
      copied = ir_try_emit_aggregate_symbol_memcpy(
          context, function, dest_name, &spilled, dest_type, location);
      ir_operand_destroy(&spilled);
    }
    return copied;
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

static int ir_spill_aggregate_value_to_local(IRLoweringContext *context,
                                             IRFunction *function,
                                             const IROperand *value,
                                             Type *dest_type,
                                             SourceLocation location,
                                             IROperand *out_symbol) {
  char *name = NULL;
  IRInstruction assign = {0};
  int ok = 0;

  if (!context || !function || !value || !dest_type || !out_symbol) {
    return 0;
  }
  if (value->kind == IR_OPERAND_SYMBOL) {
    return 0;
  }
  if ((value->kind != IR_OPERAND_TEMP && value->kind != IR_OPERAND_STRING) ||
      !dest_type->name) {
    return 0;
  }

  name = ir_new_label_name(context, "agg_ret");
  if (!name) {
    ir_set_error(context, "Out of memory while spilling aggregate value");
    return 0;
  }
  if (!ir_emit_local_declaration(context, function, name, dest_type->name,
                                 location)) {
    free(name);
    return 0;
  }

  assign.op = IR_OP_ASSIGN;
  assign.location = location;
  assign.dest = ir_operand_symbol(name);
  assign.lhs = ir_clone_operand_local(value);
  if (!assign.dest.name || assign.lhs.kind == IR_OPERAND_NONE) {
    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(&assign.lhs);
    free(name);
    ir_set_error(context, "Out of memory while spilling aggregate value");
    return 0;
  }
  ok = ir_emit(context, function, &assign);
  ir_operand_destroy(&assign.dest);
  ir_operand_destroy(&assign.lhs);
  if (!ok) {
    free(name);
    return 0;
  }

  *out_symbol = ir_operand_symbol(name);
  free(name);
  if (!out_symbol->name) {
    ir_set_error(context, "Out of memory while spilling aggregate value");
    return 0;
  }
  return 1;
}

static int ir_emit_small_array_copy(IRLoweringContext *context,
                                    IRFunction *function,
                                    const IROperand *dest_addr,
                                    const IROperand *value, int nbytes,
                                    SourceLocation location) {
  IROperand src_addr = ir_operand_none();
  IROperand word = ir_operand_none();
  IRInstruction load = {0};
  IRInstruction store = {0};
  int ok = 0;

  if (value->kind == IR_OPERAND_SYMBOL && value->name) {
    if (!ir_emit_address_of_symbol(context, function, value->name, location,
                                   &src_addr)) {
      return 0;
    }
  } else if (value->kind == IR_OPERAND_TEMP) {
    src_addr = ir_clone_operand_local(value);
  } else {
    return 0;
  }
  if (!ir_make_temp_operand(context, &word)) {
    ir_operand_destroy(&src_addr);
    return 0;
  }
  load.op = IR_OP_LOAD;
  load.location = location;
  load.dest = word;
  load.lhs = src_addr;
  load.rhs = ir_operand_int(nbytes);
  load.is_unsigned = 1;
  ok = ir_emit(context, function, &load);
  ir_operand_destroy(&src_addr);
  if (ok) {
    store.op = IR_OP_STORE;
    store.location = location;
    store.dest = ir_clone_operand_local(dest_addr);
    store.lhs = word;
    store.rhs = ir_operand_int(nbytes);
    ok = ir_emit(context, function, &store);
    ir_operand_destroy(&store.dest);
  }
  ir_operand_destroy(&word);
  return ok;
}

int ir_try_emit_aggregate_address_memcpy(IRLoweringContext *context,
                                         IRFunction *function,
                                         const IROperand *dest_addr,
                                         const IROperand *value, Type *dest_type,
                                         SourceLocation location) {
  int nbytes = 0;

  if (!context || !function || !dest_addr || !value) {
    return 0;
  }
  if (!dest_type ||
      (dest_type->kind != TYPE_STRUCT && dest_type->kind != TYPE_STRING &&
       dest_type->kind != TYPE_SLICE &&
       dest_type->kind != TYPE_TAGGED_ENUM &&
       dest_type->kind != TYPE_ARRAY)) {
    return 0;
  }
  if (dest_type->size == 0 || dest_type->size > (size_t)INT_MAX) {
    return 0;
  }
  nbytes = (int)dest_type->size;
  if (nbytes == 1 || nbytes == 2 || nbytes == 4 || nbytes == 8) {
    return dest_type->kind == TYPE_ARRAY
               ? ir_emit_small_array_copy(context, function, dest_addr, value,
                                          nbytes, location)
               : 0;
  }

  {
    IROperand spilled = ir_operand_none();
    const IROperand *source = value;
    IROperand src_addr = ir_operand_none();
    IROperand dest_copy = ir_operand_none();
    IRInstruction store = {0};
    int ok = 0;

    if (dest_type->kind != TYPE_ARRAY &&
        ir_spill_aggregate_value_to_local(context, function, value, dest_type,
                                          location, &spilled)) {
      source = &spilled;
    }
    if (dest_type->kind == TYPE_ARRAY && value->kind == IR_OPERAND_TEMP) {
      dest_copy = ir_clone_operand_local(dest_addr);
      store.op = IR_OP_STORE;
      store.location = location;
      store.dest = dest_copy;
      store.lhs = ir_clone_operand_local(value);
      store.rhs = ir_operand_int((long long)nbytes);
      ok = ir_emit(context, function, &store);
      ir_operand_destroy(&store.dest);
      ir_operand_destroy(&store.lhs);
      return ok;
    }
    if (source->kind != IR_OPERAND_SYMBOL || !source->name) {
      ir_operand_destroy(&spilled);
      return 0;
    }

    dest_copy = ir_clone_operand_local(dest_addr);
    if (dest_copy.kind == IR_OPERAND_NONE) {
      ir_operand_destroy(&spilled);
      return 0;
    }
    if (!ir_emit_address_of_symbol(context, function, source->name, location,
                                   &src_addr)) {
      ir_operand_destroy(&dest_copy);
      ir_operand_destroy(&spilled);
      return 0;
    }

    store.op = IR_OP_STORE;
    store.location = location;
    store.dest = dest_copy;
    store.lhs = src_addr;
    store.rhs = ir_operand_int((long long)nbytes);
    ok = ir_emit(context, function, &store);
    ir_operand_destroy(&dest_copy);
    ir_operand_destroy(&src_addr);
    ir_operand_destroy(&spilled);
    return ok;
  }
}

int ir_emit_symbol_assignment(IRLoweringContext *context,
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

int ir_emit_address_with_offset(IRLoweringContext *context,
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

int ir_emit_address_of_symbol(IRLoweringContext *context,
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

int ir_emit_scaled_index_offset(IRLoweringContext *context,
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

int ir_lower_index_expression(IRLoweringContext *context, IRFunction *function,
                              ASTNode *expression, IROperand *out_value) {
  int ok;
  context->address_width_depth++;
  ok = ir_lower_expression(context, function, expression, out_value);
  context->address_width_depth--;
  return ok;
}

int ir_try_lower_pointer_arithmetic(IRLoweringContext *context,
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
  if (type_is_element_ref(left_type) || type_is_element_ref(right_type)) {
    return ir_lower_element_ref_arithmetic(context, function, binary, location,
                                           out_value);
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
    if (type_is_columnar(pointer_type->base_type)) {
      ir_layout_refuse(context, location,
                       "Pointer arithmetic needs one element size, and this "
                       "type keeps its fields in separate regions");
      return 0;
    }

    if (!ir_lower_expression(context, function, pointer_expr, &base) ||
        !ir_lower_index_expression(context, function, index_expr, &index) ||
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
    if (type_is_columnar(pointer_type->base_type)) {
      ir_layout_refuse(context, location,
                       "Pointer arithmetic needs one element size, and this "
                       "type keeps its fields in separate regions");
      return 0;
    }

    if (!ir_lower_expression(context, function, binary->left, &base) ||
        !ir_lower_index_expression(context, function, binary->right, &index) ||
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

int ir_emit_binary_instruction(IRLoweringContext *context,
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
  instruction.text = (char *)op;
  return ir_emit(context, function, &instruction);
}

static int ir_lower_member_address(IRLoweringContext *context,
                                   IRFunction *function,
                                   ASTNode *expression,
                                   IROperand *out_address,
                                   Type **out_type);

int ir_lower_static_view_offset(IRLoweringContext *context,
                                       IRFunction *function, Type *view_type,
                                       IROperand *row, IROperand *column,
                                       SourceLocation location,
                                       IROperand *out_offset) {
  size_t rows = view_type->view_extents[0];
  size_t columns = view_type->view_extents[1];
  size_t element_size = view_type->base_type ? view_type->base_type->size : 4;
  IROperand scaled = ir_operand_none();
  IROperand extent = ir_operand_none();

  switch (view_type->view_layout) {
  case VIEW_LAYOUT_COL: {
    extent = ir_operand_int((long long)rows);
    return ir_emit_binary_temp(context, function, "*", column, &extent,
                               location, &scaled) &&
           ir_emit_binary_temp(context, function, "+", &scaled, row, location,
                               out_offset);
  }
  case VIEW_LAYOUT_INTERLEAVE: {
    long long group = view_type->view_layout_param;
    IROperand group_operand;
    IROperand block_operand;
    IROperand outer = ir_operand_none();
    IROperand outer_block = ir_operand_none();
    IROperand inner = ir_operand_none();
    IROperand within = ir_operand_none();
    IROperand partial = ir_operand_none();
    if (group <= 0) {
      ir_set_error(context, "a view's interleave count must be positive");
      return 0;
    }
    group_operand = ir_operand_int(group);
    block_operand = ir_operand_int((long long)rows * group);
    return ir_emit_binary_temp(context, function, "/", column, &group_operand,
                               location, &outer) &&
           ir_emit_binary_temp(context, function, "*", &outer, &block_operand,
                               location, &outer_block) &&
           ir_emit_binary_temp(context, function, "*", row, &group_operand,
                               location, &inner) &&
           ir_emit_binary_temp(context, function, "%", column, &group_operand,
                               location, &within) &&
           ir_emit_binary_temp(context, function, "+", &outer_block, &inner,
                               location, &partial) &&
           ir_emit_binary_temp(context, function, "+", &partial, &within,
                               location, out_offset);
  }
  case VIEW_LAYOUT_SWIZZLE32:
  case VIEW_LAYOUT_SWIZZLE64:
  case VIEW_LAYOUT_SWIZZLE128: {
    long long chunk_bytes =
        view_type->view_layout == VIEW_LAYOUT_SWIZZLE32   ? 4
        : view_type->view_layout == VIEW_LAYOUT_SWIZZLE64 ? 8
                                                          : 16;
    long long chunk = element_size ? chunk_bytes / (long long)element_size : 1;
    long long chunks_per_row = chunk > 0 ? (long long)columns / chunk : 0;
    IROperand chunk_operand;
    IROperand chunks_operand;
    IROperand row_base = ir_operand_none();
    IROperand column_chunk = ir_operand_none();
    IROperand row_chunk = ir_operand_none();
    IROperand swizzled = ir_operand_none();
    IROperand swizzled_elements = ir_operand_none();
    IROperand within = ir_operand_none();
    IROperand partial = ir_operand_none();
    if (chunk <= 0 || chunks_per_row <= 0) {
      ir_set_error(context,
                   "a swizzled view's rows must hold whole chunks of the "
                   "layout's width");
      return 0;
    }
    chunk_operand = ir_operand_int(chunk);
    chunks_operand = ir_operand_int(chunks_per_row);
    extent = ir_operand_int((long long)columns);
    return ir_emit_binary_temp(context, function, "*", row, &extent, location,
                               &row_base) &&
           ir_emit_binary_temp(context, function, "/", column, &chunk_operand,
                               location, &column_chunk) &&
           ir_emit_binary_temp(context, function, "%", row, &chunks_operand,
                               location, &row_chunk) &&
           ir_emit_binary_temp(context, function, "^", &column_chunk,
                               &row_chunk, location, &swizzled) &&
           ir_emit_binary_temp(context, function, "*", &swizzled,
                               &chunk_operand, location, &swizzled_elements) &&
           ir_emit_binary_temp(context, function, "%", column, &chunk_operand,
                               location, &within) &&
           ir_emit_binary_temp(context, function, "+", &row_base,
                               &swizzled_elements, location, &partial) &&
           ir_emit_binary_temp(context, function, "+", &partial, &within,
                               location, out_offset);
  }
  case VIEW_LAYOUT_FRAGMENT_A:
  case VIEW_LAYOUT_FRAGMENT_B:
  case VIEW_LAYOUT_FRAGMENT_C:
    ir_set_error(context,
                 "a fragment layout holds one work item's share of a tile in "
                 "registers, so it has no element address; hand it to a tensor "
                 "operation instead");
    return 0;
  default:
    break;
  }
  extent = ir_operand_int((long long)columns);
  return ir_emit_binary_temp(context, function, "*", row, &extent, location,
                             &scaled) &&
         ir_emit_binary_temp(context, function, "+", &scaled, column, location,
                             out_offset);
}

static int ir_lower_static_view_element(IRLoweringContext *context,
                                        IRFunction *function,
                                        ASTNode *expression,
                                        ArrayIndexExpression *outer,
                                        ArrayIndexExpression *inner,
                                        Type *view_type, IROperand *out_address,
                                        Type **out_type) {
  IROperand base = ir_operand_none();
  IROperand row = ir_operand_none();
  IROperand column = ir_operand_none();
  IROperand offset = ir_operand_none();
  IROperand bytes = ir_operand_none();
  IROperand element_size = ir_operand_int(
      (long long)ir_type_array_element_stride(view_type->base_type));
  int ok = 0;

  if (out_type) {
    *out_type = view_type->base_type;
  }
  if (!ir_lower_expression(context, function, inner->array, &base) ||
      !ir_lower_index_expression(context, function, inner->index, &row) ||
      !ir_lower_index_expression(context, function, outer->index, &column)) {
    goto done;
  }
  if (!ir_lower_static_view_offset(context, function, view_type, &row, &column,
                                   expression->location, &offset) ||
      !ir_emit_binary_temp(context, function, "*", &offset, &element_size,
                           expression->location, &bytes) ||
      !ir_emit_binary_temp(context, function, "+", &base, &bytes,
                           expression->location, out_address)) {
    goto done;
  }
  ok = 1;

done:
  ir_operand_destroy(&base);
  ir_operand_destroy(&row);
  ir_operand_destroy(&column);
  ir_operand_destroy(&offset);
  ir_operand_destroy(&bytes);
  return ok;
}

static int ir_lower_view_row_address(IRLoweringContext *context,
                                     IRFunction *function,
                                     ASTNode *expression,
                                     ArrayIndexExpression *index_expression,
                                     Type *view_type, IROperand *out_address,
                                     Type **out_type) {
  size_t rank = type_view_rank(view_type);
  Type *row_type = ir_infer_expression_type(context, expression);
  IROperand view_address = ir_operand_none();
  IROperand index = ir_operand_none();
  IROperand base = ir_operand_none();
  IROperand lead = ir_operand_none();
  IROperand scaled = ir_operand_none();
  IROperand bytes = ir_operand_none();
  IROperand data = ir_operand_none();
  IROperand row_address = ir_operand_none();
  IROperand element_size = ir_operand_none();
  char *row_name = NULL;
  int ok = 0;

  if (!row_type || row_type->kind != TYPE_SLICE || !view_type->base_type) {
    ir_set_error(context, "View row reached lowering without a row type");
    return 0;
  }
  if (!ir_lower_lvalue_address(context, function, index_expression->array,
                               &view_address, NULL)) {
    return 0;
  }
  if (!ir_lower_index_expression(context, function, index_expression->index,
                                 &index)) {
    goto done;
  }
  if (!ir_emit_slice_bounds_check(context, function, expression->location,
                                  &view_address, &index)) {
    goto done;
  }
  element_size =
      ir_operand_int(ir_type_array_element_stride(view_type->base_type));
  if (!ir_emit_load_word(context, function, &view_address, 0,
                         expression->location, &base) ||
      !ir_emit_load_word(context, function, &view_address, 8 + 8 * rank,
                         expression->location, &lead) ||
      !ir_emit_binary_temp(context, function, "*", &index, &lead,
                           expression->location, &scaled) ||
      !ir_emit_binary_temp(context, function, "*", &scaled, &element_size,
                           expression->location, &bytes) ||
      !ir_emit_binary_temp(context, function, "+", &base, &bytes,
                           expression->location, &data)) {
    goto done;
  }
  row_name = ir_new_label_name(context, "row");
  if (!row_name ||
      !ir_emit_local_declaration(context, function, row_name, row_type->name,
                                 expression->location) ||
      !ir_emit_address_of_symbol(context, function, row_name,
                                 expression->location, &row_address) ||
      !ir_emit_store_word(context, function, &row_address, 0, &data,
                          expression->location)) {
    goto done;
  }
  for (size_t k = 1; k < rank; k++) {
    IROperand word = ir_operand_none();
    int stored =
        ir_emit_load_word(context, function, &view_address, 8 + 8 * k,
                          expression->location, &word) &&
        ir_emit_store_word(context, function, &row_address, 8 + 8 * (k - 1),
                           &word, expression->location);
    ir_operand_destroy(&word);
    if (!stored) {
      goto done;
    }
  }
  for (size_t k = 1; k + 1 < rank; k++) {
    IROperand word = ir_operand_none();
    int stored =
        ir_emit_load_word(context, function, &view_address,
                          8 + 8 * rank + 8 * k, expression->location, &word) &&
        ir_emit_store_word(context, function, &row_address,
                           8 + 8 * (rank - 1) + 8 * (k - 1), &word,
                           expression->location);
    ir_operand_destroy(&word);
    if (!stored) {
      goto done;
    }
  }
  *out_address = row_address;
  row_address = ir_operand_none();
  if (out_type) {
    *out_type = row_type;
  }
  ok = 1;

done:
  free(row_name);
  ir_operand_destroy(&view_address);
  ir_operand_destroy(&index);
  ir_operand_destroy(&base);
  ir_operand_destroy(&lead);
  ir_operand_destroy(&scaled);
  ir_operand_destroy(&bytes);
  ir_operand_destroy(&data);
  ir_operand_destroy(&row_address);
  return ok;
}

int ir_lower_lvalue_address(IRLoweringContext *context,
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
      const IRLocalBinding *binding =
          ir_local_binding_find(context, identifier->name);
      Type *bound_type = (binding && binding->type_text)
                             ? ir_resolve_named_type(context,
                                                     binding->type_text)
                             : NULL;

      Symbol *symbol =
          bound_type || !context->symbol_table
              ? NULL
              : symbol_table_lookup(context->symbol_table, identifier->name);

      if (symbol && symbol->kind == SYMBOL_CONSTANT) {
        ir_set_error(context, "Cannot take address of constant");
        return 0;
      }

      if (bound_type) {
        *out_type = bound_type;
      } else if (symbol && (symbol->kind == SYMBOL_VARIABLE ||
                            symbol->kind == SYMBOL_PARAMETER)) {
        *out_type = symbol->type;
      } else {
        *out_type = ir_infer_expression_type(context, expression);
      }
    }
    return ir_emit_address_of_symbol(context, function,
                                     ir_local_ir_name(context,
                                                      identifier->name),
                                     expression->location, out_address);
  }

  case AST_MEMBER_ACCESS:
    return ir_lower_member_address(context, function, expression,
                                   out_address, out_type);

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
    int base_is_string = array_type && array_type->kind == TYPE_STRING;
    int base_is_slice = array_type && array_type->kind == TYPE_SLICE;
    if (index_expression->array &&
        index_expression->array->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *inner =
          (ArrayIndexExpression *)index_expression->array->data;
      Type *outer_type =
          inner ? ir_infer_expression_type(context, inner->array) : NULL;
      if (outer_type && outer_type->kind == TYPE_SLICE &&
          outer_type->view_extents[0] > 0 && type_view_rank(outer_type) == 2) {
        return ir_lower_static_view_element(context, function, expression,
                                            index_expression, inner,
                                            outer_type, out_address, out_type);
      }
    }
    if (base_is_slice && array_type->view_extents[0] > 0) {
      ir_set_error(context,
                   "a view whose extents are in its type is indexed with every "
                   "index at once");
      return 0;
    }
    if (base_is_slice && type_view_rank(array_type) > 1) {
      return ir_lower_view_row_address(context, function, expression,
                                       index_expression, array_type,
                                       out_address, out_type);
    }
    Type *element_type = base_is_string
                             ? ir_resolve_named_type(context, "char")
                             : (array_type ? array_type->base_type : NULL);
    if (!array_type || !element_type ||
        (!base_is_string && !base_is_slice &&
         array_type->kind != TYPE_ARRAY &&
         array_type->kind != TYPE_POINTER)) {
      ir_set_error(context, "Index lvalue requires array or pointer type");
      return 0;
    }

    if (type_is_columnar(element_type)) {
      char message[400];
      snprintf(message, sizeof(message),
               array_type->kind == TYPE_POINTER
                   ? "'%s' keeps its fields in separate regions, so indexing "
                     "it needs an element count, and a pointer has none. Use "
                     "an array or a slice"
                   : "An element of '%s' keeps its fields in separate regions, "
                     "so it has no single address. Take '&a[i].field', or copy "
                     "the element",
               element_type->name ? element_type->name : "?");
      ir_layout_refuse(context, expression->location, message);
      return 0;
    }

    if (out_type) {
      *out_type = element_type;
    }

    IROperand base = ir_operand_none();
    IROperand index = ir_operand_none();
    int lowered_base = 0;
    int is_address_space_allocation =
        ir_expression_is_address_space_allocation(context, function,
                                                  index_expression->array);
    IROperand slice_address = ir_operand_none();
    int string_checked =
        base_is_string && context->emit_runtime_checks &&
        !context->emit_safety_checks && index_expression->array &&
        (index_expression->array->type == AST_IDENTIFIER ||
         index_expression->array->type == AST_MEMBER_ACCESS);
    if (base_is_string && !string_checked) {
      lowered_base = ir_lower_expression(context, function,
                                         index_expression->array, &base) &&
                     ir_coerce_string_operand_to_cstring(
                         context, function, &base, expression->location);
    } else if (base_is_slice || string_checked) {
      lowered_base =
          ir_lower_lvalue_address(context, function, index_expression->array,
                                  &slice_address, NULL) &&
          ir_make_temp_operand(context, &base);
      if (lowered_base) {
        IRInstruction load_data = {0};
        load_data.op = IR_OP_LOAD;
        load_data.location = expression->location;
        load_data.dest = base;
        load_data.lhs = ir_clone_operand_local(&slice_address);
        load_data.rhs = ir_operand_int(8);
        load_data.alias_class = IR_ALIAS_CLASS_POINTER;
        if (array_type->field_types && array_type->field_count > 0) {
          load_data.value_type =
              mtlc_type_from_frontend(array_type->field_types[0]);
        }
        lowered_base = ir_emit(context, function, &load_data);
        ir_operand_destroy(&load_data.lhs);
      }
    } else if (array_type->kind == TYPE_ARRAY && is_address_space_allocation) {
      lowered_base =
          ir_lower_expression(context, function, index_expression->array, &base);
    } else if (array_type->kind == TYPE_ARRAY) {
      lowered_base = ir_lower_lvalue_address(context, function,
                                             index_expression->array, &base,
                                             NULL);
    } else {
      lowered_base =
          ir_lower_expression(context, function, index_expression->array, &base);
    }

    if (!lowered_base ||
        !ir_lower_index_expression(context, function, index_expression->index,
                                   &index)) {
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }

    int index_proven_by_type =
        array_type->kind == TYPE_ARRAY && index_expression->index &&
        type_checker_refined_index_fits(index_expression->index->resolved_type,
                                        array_type->array_size);
    if (index_proven_by_type) {
      const Type *index_type = index_expression->index->resolved_type;
      ir_explain_safety_typed_note(
          expression->location.filename, expression->location.line,
          function ? function->name : NULL, index_type->name,
          index_type->refine_min, index_type->refine_max,
          array_type->array_size);
    }
    if (!context->emit_safety_checks) {
      int base_never_null = type_checker_type_excludes_zero(array_type);
      if (base_never_null) {
        ir_explain_type_payoff(
            expression->location.filename, expression->location.line,
            function ? function->name : NULL,
            array_type->name ? array_type->name : "?",
            "no null check emitted",
            "rules the pointer out of being zero, so the check could never "
            "fire; consumed by lowering, which decides check emission per "
            "access");
      }
      if (array_type->kind == TYPE_POINTER && !is_address_space_allocation &&
          !base_never_null &&
          !ir_emit_null_check(context, function, expression->location, &base)) {
        ir_operand_destroy(&base);
        ir_operand_destroy(&index);
        return 0;
      }
      if (array_type->kind == TYPE_ARRAY && !index_proven_by_type &&
          !ir_emit_bounds_check(context, function, expression->location, &index,
                                array_type->array_size)) {
        ir_operand_destroy(&base);
        ir_operand_destroy(&index);
        return 0;
      }
      if ((base_is_slice || string_checked) &&
          !ir_emit_slice_bounds_check(context, function, expression->location,
                                      &slice_address, &index)) {
        ir_operand_destroy(&base);
        ir_operand_destroy(&index);
        ir_operand_destroy(&slice_address);
        return 0;
      }
    }
    ir_operand_destroy(&slice_address);

    IROperand scaled = ir_operand_none();
    if (!ir_make_temp_operand(context, &scaled)) {
      ir_operand_destroy(&base);
      ir_operand_destroy(&index);
      return 0;
    }

    int element_size = ir_type_array_element_stride(element_type);
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

    if (!is_address_space_allocation && !index_proven_by_type) {
      long long extent = IR_SAFETY_EXTENT_UNKNOWN;
      if (array_type->kind == TYPE_ARRAY && array_type->array_size > 0) {
        extent = (long long)array_type->array_size * element_size;
      }
      char described[128];
      ir_safety_describe(expression, described, sizeof(described), 0);
      if (!ir_emit_safety_check(context, function, expression->location, &base,
                                &scaled, element_size, extent,
                                IR_SAFETY_ACCESS_READ, described)) {
        ir_operand_destroy(&scaled);
        ir_operand_destroy(&base);
        ir_operand_destroy(&index);
        return 0;
      }
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
    if (unary && unary->operator && strcmp(unary->operator, "&") == 0 &&
        type_is_element_ref(ir_infer_expression_type(context, expression))) {
      IROperand made = ir_operand_none();
      if (!ir_lower_expression(context, function, expression, &made)) {
        return 0;
      }
      if (out_type) {
        *out_type = ir_infer_expression_type(context, expression);
      }
      int took = ir_emit_address_of_symbol(context, function, made.name,
                                           expression->location, out_address);
      ir_operand_destroy(&made);
      return took;
    }
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

    if (context->emit_safety_checks) {
      long long pointee_size =
          operand_type && operand_type->base_type
              ? (long long)operand_type->base_type->size
              : 0;
      char described[128];
      ir_safety_describe(expression, described, sizeof(described), 0);
      IROperand zero_offset = ir_operand_int(0);
      int checked = ir_emit_safety_check(
          context, function, expression->location, &pointer_value, &zero_offset,
          pointee_size, IR_SAFETY_EXTENT_UNKNOWN, IR_SAFETY_ACCESS_READ,
          described);
      ir_operand_destroy(&zero_offset);
      if (!checked) {
        ir_operand_destroy(&pointer_value);
        return 0;
      }
    } else if (!ir_emit_null_check(context, function, expression->location,
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

  default: {
    Type *value_type = ir_infer_expression_type(context, expression);
    if (!value_type || !value_type->name ||
        (value_type->kind != TYPE_STRUCT && value_type->kind != TYPE_ARRAY &&
         value_type->kind != TYPE_SLICE && value_type->kind != TYPE_STRING)) {
      ir_set_error(context, "Expression is not assignable in IR lowering");
      return 0;
    }

    char temp_local_name[48];
    snprintf(temp_local_name, sizeof(temp_local_name), ".aggregate_tmp%d",
             context->next_temp_id++);

    IRInstruction local = {0};
    local.op = IR_OP_DECLARE_LOCAL;
    local.location = expression->location;
    local.dest = ir_operand_symbol(temp_local_name);
    local.text = value_type->name;
    local.value_type = mtlc_type_from_frontend(value_type);
    if (!local.dest.name) {
      ir_set_error(context, "Out of memory materializing aggregate rvalue");
      return 0;
    }
    int local_ok = ir_emit(context, function, &local);
    ir_operand_destroy(&local.dest);
    if (!local_ok) {
      return 0;
    }

    IROperand value = ir_operand_none();
    if (!ir_lower_expression(context, function, expression, &value)) {
      return 0;
    }

    IRInstruction assign = {0};
    assign.op = IR_OP_ASSIGN;
    assign.location = expression->location;
    assign.dest = ir_operand_symbol(temp_local_name);
    assign.lhs = value;
    if (!assign.dest.name) {
      ir_operand_destroy(&value);
      ir_set_error(context, "Out of memory materializing aggregate rvalue");
      return 0;
    }
    int assign_ok = ir_emit(context, function, &assign);
    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(&value);
    if (!assign_ok) {
      return 0;
    }

    if (out_type) {
      *out_type = value_type;
    }
    return ir_emit_address_of_symbol(context, function, temp_local_name,
                                     expression->location, out_address);
  }
  }
}

void ir_layout_refuse(IRLoweringContext *context, SourceLocation location,
                      const char *message) {
  if (context->type_checker && context->type_checker->error_reporter) {
    error_reporter_add_error(context->type_checker->error_reporter,
                             ERROR_SEMANTIC, location, message);
    error_reporter_set_last_code(context->type_checker->error_reporter,
                                 "L0003");
  }
  ir_set_error(context, "%s", message);
}

typedef struct {
  Type *element;
  IROperand base;
  IROperand index;
  IROperand count;
  IROperand slice_address;
  IROperand held_index;
  IROperand offset;
  long long known_count;
  long long count_offset;
} IRColumnAccess;

static void ir_column_access_end(IRColumnAccess *access) {
  ir_operand_destroy(&access->base);
  ir_operand_destroy(&access->index);
  ir_operand_destroy(&access->count);
  ir_operand_destroy(&access->slice_address);
  ir_operand_destroy(&access->held_index);
  ir_operand_destroy(&access->offset);
}

static void ir_column_mark_last(IRFunction *function, unsigned char mark) {
  if (function->instruction_count > 0) {
    function->instructions[function->instruction_count - 1].column_base = mark;
  }
}

static int ir_column_load_count(IRLoweringContext *context,
                                IRFunction *function, IRColumnAccess *access,
                                SourceLocation location) {
  IROperand slot = ir_operand_none();
  IRInstruction load = {0};
  if (access->count.kind != IR_OPERAND_NONE) {
    return 1;
  }
  if (!ir_emit_address_with_offset(context, function, &access->slice_address,
                                   access->count_offset, location, &slot) ||
      !ir_make_temp_operand(context, &access->count)) {
    return 0;
  }
  load.op = IR_OP_LOAD;
  load.location = location;
  load.dest = access->count;
  load.lhs = slot;
  load.rhs = ir_operand_int(8);
  load.alias_class =
      access->count_offset == 16 ? IR_ALIAS_CLASS_POINTER : IR_ALIAS_CLASS_I64;
  int ok = ir_emit(context, function, &load);
  ir_operand_destroy(&slot);
  return ok;
}

static int ir_column_load_count(IRLoweringContext *context,
                                IRFunction *function, IRColumnAccess *access,
                                SourceLocation location);

static int ir_emit_ref_bounds_check(IRLoweringContext *context,
                                    IRFunction *function, SourceLocation location,
                                    IRColumnAccess *access) {
  IROperand zero = ir_operand_int(0);
  IROperand low = ir_operand_none();
  IROperand high = ir_operand_none();
  if (!ir_column_load_count(context, function, access, location) ||
      !ir_emit_binary_temp(context, function, "<", &access->index, &access->count,
                           location, &high) ||
      !ir_emit_binary_temp(context, function, ">=", &access->index, &zero,
                           location, &low)) {
    return 0;
  }
  char *trap = ir_new_label_name(context, "ir_trap_ref_bounds");
  char *fine = ir_new_label_name(context, "ir_in_ref_bounds");
  IRInstruction branch = {0};
  branch.op = IR_OP_BRANCH_ZERO;
  branch.location = location;
  branch.lhs = high;
  branch.text = trap;
  if (!ir_emit(context, function, &branch)) {
    return 0;
  }
  branch.lhs = low;
  if (!ir_emit(context, function, &branch)) {
    return 0;
  }
  IRInstruction jump = {0};
  jump.op = IR_OP_JUMP;
  jump.location = location;
  jump.text = fine;
  if (!ir_emit(context, function, &jump)) {
    return 0;
  }
  IRInstruction label = {0};
  label.op = IR_OP_LABEL;
  label.location = location;
  label.text = trap;
  if (!ir_emit(context, function, &label)) {
    return 0;
  }
  IROperand arguments[4];
  arguments[0] = ir_operand_int(2);
  arguments[1] = ir_operand_string("Fatal error: element reference index out of bounds");
  arguments[2] = ir_clone_operand_local(&access->index);
  arguments[3] = ir_clone_operand_local(&access->count);
  IRInstruction call = {0};
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = "mettle_crash_trap_ex";
  call.arguments = arguments;
  call.argument_count = 4;
  int ok = ir_emit(context, function, &call);
  for (int a = 0; a < 4; a++) {
    ir_operand_destroy(&arguments[a]);
  }
  label.text = fine;
  ok = ok && ir_emit(context, function, &label);
  ir_operand_destroy(&low);
  ir_operand_destroy(&high);
  return ok;
}

static int ir_ref_load_word(IRLoweringContext *context, IRFunction *function,
                            const IROperand *ref_address, long long offset,
                            SourceLocation location, IROperand *out) {
  IROperand slot = ir_operand_none();
  IRInstruction load = {0};
  if (!ir_emit_address_with_offset(context, function, ref_address, offset,
                                   location, &slot) ||
      !ir_make_temp_operand(context, out)) {
    return 0;
  }
  load.op = IR_OP_LOAD;
  load.location = location;
  load.dest = *out;
  load.lhs = slot;
  load.rhs = ir_operand_int(8);
  load.alias_class = IR_ALIAS_CLASS_POINTER;
  int ok = ir_emit(context, function, &load);
  ir_operand_destroy(&slot);
  return ok;
}

static int ir_ref_store_word(IRLoweringContext *context, IRFunction *function,
                             const IROperand *ref_address, long long offset,
                             const IROperand *value, SourceLocation location) {
  IROperand slot = ir_operand_none();
  IRInstruction store = {0};
  if (!ir_emit_address_with_offset(context, function, ref_address, offset,
                                   location, &slot)) {
    return 0;
  }
  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = slot;
  store.lhs = ir_clone_operand_local(value);
  store.rhs = ir_operand_int(8);
  store.alias_class = IR_ALIAS_CLASS_POINTER;
  int ok = ir_emit(context, function, &store);
  ir_operand_destroy(&store.dest);
  ir_operand_destroy(&store.lhs);
  return ok;
}

static int ir_column_access_from_ref(IRLoweringContext *context,
                                     IRFunction *function, ASTNode *ref_node,
                                     ASTNode *index_node, SourceLocation location,
                                     IRColumnAccess *out) {
  Type *ref_type = ir_infer_expression_type(context, ref_node);
  IROperand ref_address = ir_operand_none();
  memset(out, 0, sizeof(*out));
  out->known_count = -1;
  out->count_offset = 16;
  out->slice_address = ir_operand_none();
  out->held_index = ir_operand_none();
  out->offset = ir_operand_none();
  out->element = ref_type->base_type;
  if (!ir_lower_lvalue_address(context, function, ref_node, &ref_address, NULL) ||
      !ir_ref_load_word(context, function, &ref_address, 0, location, &out->base) ||
      !ir_ref_load_word(context, function, &ref_address, 8, location,
                        &out->held_index)) {
    return 0;
  }
  out->slice_address = ref_address;
  if (!context->emit_safety_checks &&
      !ir_emit_null_check(context, function, location, &out->base)) {
    return 0;
  }
  if (index_node) {
    if (!ir_lower_index_expression(context, function, index_node, &out->offset) ||
        !ir_emit_binary_temp(context, function, "+", &out->held_index, &out->offset,
                             location, &out->index)) {
      return 0;
    }
  } else {
    out->index = ir_clone_operand_local(&out->held_index);
  }
  if ((context->emit_safety_checks || context->emit_runtime_checks) &&
      !ir_emit_ref_bounds_check(context, function, location, out)) {
    return 0;
  }
  return 1;
}

static int ir_column_access_begin(IRLoweringContext *context,
                                  IRFunction *function, ASTNode *index_node,
                                  IRColumnAccess *out) {
  if (index_node->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *unary = (UnaryExpression *)index_node->data;
    return ir_column_access_from_ref(context, function, unary->operand, NULL,
                                     index_node->location, out);
  }
  ArrayIndexExpression *indexing = (ArrayIndexExpression *)index_node->data;
  Type *array_type = ir_infer_expression_type(context, indexing->array);
  IROperand slice_address = ir_operand_none();
  SourceLocation location = index_node->location;
  if (type_is_element_ref(array_type)) {
    return ir_column_access_from_ref(context, function, indexing->array,
                                     indexing->index, location, out);
  }
  memset(out, 0, sizeof(*out));
  out->known_count = -1;
  out->count_offset = 8;
  out->slice_address = ir_operand_none();
  out->held_index = ir_operand_none();
  out->offset = ir_operand_none();
  if (!array_type || !array_type->base_type) {
    ir_set_error(context, "Column access reached lowering without an element "
                          "type");
    return 0;
  }
  out->element = array_type->base_type;
  if (array_type->kind == TYPE_ARRAY) {
    if (!ir_lower_lvalue_address(context, function, indexing->array, &out->base,
                                 NULL)) {
      return 0;
    }
    out->known_count = (long long)array_type->array_size;
    out->count = ir_operand_int(out->known_count);
  } else if (array_type->kind == TYPE_SLICE && type_view_rank(array_type) == 1 &&
             !array_type->view_extents[0]) {
    IROperand length_slot = ir_operand_none();
    IRInstruction load = {0};
    if (!ir_lower_lvalue_address(context, function, indexing->array,
                                 &slice_address, NULL) ||
        !ir_make_temp_operand(context, &out->base)) {
      return 0;
    }
    load.op = IR_OP_LOAD;
    load.location = location;
    load.dest = out->base;
    load.lhs = ir_clone_operand_local(&slice_address);
    load.rhs = ir_operand_int(8);
    load.alias_class = IR_ALIAS_CLASS_POINTER;
    if (!ir_emit(context, function, &load)) {
      return 0;
    }
    ir_operand_destroy(&load.lhs);
    out->slice_address = ir_clone_operand_local(&slice_address);
  } else {
    char message[400];
    snprintf(message, sizeof(message),
             "'%s' keeps its fields in separate regions, so indexing it needs "
             "an element count, and a pointer has none. Use an array or a "
             "slice",
             out->element->name ? out->element->name : "?");
    ir_layout_refuse(context, location, message);
    return 0;
  }
  if (!ir_lower_index_expression(context, function, indexing->index,
                                 &out->index)) {
    return 0;
  }
  int runtime_checks = context->emit_runtime_checks;
  context->emit_runtime_checks = runtime_checks || context->emit_safety_checks;
  int bounded =
      (array_type->kind != TYPE_ARRAY ||
       type_checker_refined_index_fits(indexing->index->resolved_type,
                                       array_type->array_size) ||
       ir_emit_bounds_check(context, function, location, &out->index,
                            array_type->array_size)) &&
      (array_type->kind != TYPE_SLICE ||
       ir_emit_slice_bounds_check(context, function, location, &slice_address,
                                  &out->index));
  context->emit_runtime_checks = runtime_checks;
  if (!bounded) {
    return 0;
  }
  ir_operand_destroy(&slice_address);
  return 1;
}

static int ir_column_region_base(IRLoweringContext *context,
                                 IRFunction *function, IRColumnAccess *access,
                                 size_t region, SourceLocation location,
                                 IROperand *out) {
  const TypeMetaLayout *layout = access->element->meta_layout;
  if (access->known_count >= 0) {
    *out = ir_operand_int((long long)type_columns_region_base(
        access->element, region, (size_t)access->known_count));
    return 1;
  }
  if (region > 0 && !ir_column_load_count(context, function, access, location)) {
    return 0;
  }
  IROperand running = ir_operand_int(0);
  size_t r = 0;
  while (r < region) {
    size_t strides = 0;
    size_t last = r;
    while (last < region) {
      strides += layout->regions[last].stride;
      if (layout->regions[last].stride % layout->regions[last + 1].alignment !=
          0) {
        break;
      }
      last++;
    }
    size_t rounded_after = last < region ? last : region - 1;
    size_t next_align = layout->regions[rounded_after + 1].alignment;
    IROperand per_element = ir_operand_int((long long)strides);
    IROperand bytes = ir_operand_none();
    IROperand sum = ir_operand_none();
    if (!ir_emit_binary_temp(context, function, "*", &access->count,
                             &per_element, location, &bytes)) {
      return 0;
    }
    ir_column_mark_last(function, 1);
    if (running.kind == IR_OPERAND_INT && running.int_value == 0) {
      sum = bytes;
    } else {
      if (!ir_emit_binary_temp(context, function, "+", &running, &bytes,
                               location, &sum)) {
        return 0;
      }
      ir_column_mark_last(function, 1);
      ir_operand_destroy(&bytes);
    }
    ir_operand_destroy(&running);
    running = sum;
    if (last < region) {
      IROperand up = ir_operand_int((long long)next_align - 1);
      IROperand down = ir_operand_int(-(long long)next_align);
      IROperand raised = ir_operand_none();
      IROperand aligned = ir_operand_none();
      if (!ir_emit_binary_temp(context, function, "+", &running, &up, location,
                               &raised)) {
        return 0;
      }
      ir_column_mark_last(function, 1);
      if (!ir_emit_binary_temp(context, function, "&", &raised, &down, location,
                               &aligned)) {
        return 0;
      }
      ir_column_mark_last(function, 1);
      ir_operand_destroy(&raised);
      ir_operand_destroy(&running);
      running = aligned;
    }
    r = last + 1;
    if (last == region) {
      break;
    }
  }
  *out = running;
  return 1;
}

static int ir_column_region_address(IRLoweringContext *context,
                                    IRFunction *function,
                                    IRColumnAccess *access, size_t region,
                                    SourceLocation location, IROperand *out) {
  const TypeMetaLayout *layout = access->element->meta_layout;
  IROperand region_base = ir_operand_none();
  IROperand stride = ir_operand_int((long long)layout->regions[region].stride);
  IROperand scaled = ir_operand_none();
  IROperand column = ir_operand_none();
  int split = !context->emit_safety_checks &&
              access->held_index.kind != IR_OPERAND_NONE &&
              access->offset.kind != IR_OPERAND_NONE;
  if (!ir_column_region_base(context, function, access, region, location,
                             &region_base) ||
      !ir_emit_binary_temp(context, function, "*",
                           split ? &access->offset : &access->index, &stride,
                           location, &scaled)) {
    return 0;
  }
  if (context->emit_safety_checks) {
    IROperand offset = ir_operand_none();
    long long extent = access->known_count >= 0
                           ? (long long)type_columns_bytes(
                                 access->element, (size_t)access->known_count)
                           : IR_SAFETY_EXTENT_UNKNOWN;
    if (!ir_emit_binary_temp(context, function, "+", &region_base, &scaled,
                             location, &offset) ||
        !ir_emit_safety_check(context, function, location, &access->base,
                              &offset, (long long)layout->regions[region].stride,
                              extent, IR_SAFETY_ACCESS_READ, "column element")) {
      return 0;
    }
    ir_operand_destroy(&offset);
  }
  column = ir_clone_operand_local(&access->base);
  if (split) {
    IROperand held_scaled = ir_operand_none();
    IROperand from_held = ir_operand_none();
    if (!ir_emit_binary_temp(context, function, "*", &access->held_index, &stride,
                             location, &held_scaled)) {
      return 0;
    }
    ir_column_mark_last(function, 1);
    if (!ir_emit_binary_temp(context, function, "+", &column, &held_scaled, location,
                             &from_held)) {
      return 0;
    }
    ir_column_mark_last(function, 2);
    ir_operand_destroy(&held_scaled);
    ir_operand_destroy(&column);
    column = from_held;
  }
  if (region_base.kind != IR_OPERAND_INT || region_base.int_value != 0) {
    IROperand in_region = ir_operand_none();
    if (!ir_emit_binary_temp(context, function, "+", &column, &region_base,
                             location, &in_region)) {
      return 0;
    }
    ir_column_mark_last(function, 2);
    ir_operand_destroy(&column);
    column = in_region;
  }
  ir_operand_destroy(&region_base);
  if (!ir_emit_binary_temp(context, function, "+", &column, &scaled, location,
                           out)) {
    return 0;
  }
  ir_operand_destroy(&column);
  ir_operand_destroy(&scaled);
  return 1;
}

static int ir_emit_bytes_copy(IRLoweringContext *context, IRFunction *function,
                              const IROperand *dest, const IROperand *source,
                              size_t size, SourceLocation location) {
  IRInstruction store = {0};
  IROperand value = ir_operand_none();
  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = ir_clone_operand_local(dest);
  store.rhs = ir_operand_int((long long)size);
  if (size == 1 || size == 2 || size == 4 || size == 8) {
    IRInstruction load = {0};
    if (!ir_make_temp_operand(context, &value)) {
      return 0;
    }
    load.op = IR_OP_LOAD;
    load.location = location;
    load.dest = value;
    load.lhs = ir_clone_operand_local(source);
    load.rhs = ir_operand_int((long long)size);
    load.is_unsigned = 1;
    if (!ir_emit(context, function, &load)) {
      return 0;
    }
    ir_operand_destroy(&load.lhs);
    store.lhs = value;
  } else {
    store.lhs = ir_clone_operand_local(source);
  }
  int ok = ir_emit(context, function, &store);
  ir_operand_destroy(&store.dest);
  ir_operand_destroy(&store.lhs);
  return ok;
}

int ir_expression_is_column_element(IRLoweringContext *context,
                                    ASTNode *expression) {
  if (!expression) {
    return 0;
  }
  if (expression->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    return unary && unary->operator && strcmp(unary->operator, "*") == 0 &&
           type_is_element_ref(ir_infer_expression_type(context, unary->operand));
  }
  if (expression->type != AST_INDEX_EXPRESSION) {
    return 0;
  }
  return type_is_columnar(ir_infer_expression_type(context, expression));
}

static int ir_ref_declare(IRLoweringContext *context, IRFunction *function,
                          Type *ref_type, SourceLocation location,
                          char **out_name, IROperand *out_address) {
  char *name = ir_new_label_name(context, "element_ref");
  if (!name ||
      !ir_emit_local_declaration(context, function, name, ref_type->name,
                                 location) ||
      !ir_emit_address_of_symbol(context, function, name, location, out_address)) {
    free(name);
    return 0;
  }
  *out_name = name;
  return 1;
}

int ir_lower_make_element_ref(IRLoweringContext *context, IRFunction *function,
                              Type *ref_type, const IROperand *base,
                              const IROperand *index, const IROperand *count,
                              SourceLocation location, IROperand *out_value) {
  char *name = NULL;
  IROperand address = ir_operand_none();
  if (!ir_ref_declare(context, function, ref_type, location, &name, &address) ||
      !ir_ref_store_word(context, function, &address, 0, base, location) ||
      !ir_ref_store_word(context, function, &address, 8, index, location) ||
      !ir_ref_store_word(context, function, &address, 16, count, location)) {
    return 0;
  }
  ir_operand_destroy(&address);
  *out_value = ir_operand_symbol(name);
  free(name);
  return out_value->name != NULL;
}

int ir_lower_address_of_element(IRLoweringContext *context, IRFunction *function,
                                ASTNode *operand, Type *ref_type,
                                SourceLocation location, IROperand *out_value) {
  if (ir_expression_is_column_element(context, operand)) {
    IRColumnAccess access;
    if (!ir_column_access_begin(context, function, operand, &access) ||
        !ir_column_load_count(context, function, &access, location)) {
      return 0;
    }
    int ok = ir_lower_make_element_ref(context, function, ref_type, &access.base,
                                       &access.index, &access.count, location,
                                       out_value);
    ir_column_access_end(&access);
    return ok;
  }
  IROperand address = ir_operand_none();
  IROperand zero = ir_operand_int(0);
  IROperand one = ir_operand_int(1);
  if (!ir_lower_lvalue_address(context, function, operand, &address, NULL)) {
    return 0;
  }
  int ok = ir_lower_make_element_ref(context, function, ref_type, &address, &zero,
                                     &one, location, out_value);
  ir_operand_destroy(&address);
  return ok;
}

int ir_lower_element_ref_arithmetic(IRLoweringContext *context,
                                    IRFunction *function, BinaryExpression *binary,
                                    SourceLocation location,
                                    IROperand *out_value) {
  Type *left_type = ir_infer_expression_type(context, binary->left);
  Type *right_type = ir_infer_expression_type(context, binary->right);
  int both = type_is_element_ref(left_type) && type_is_element_ref(right_type);
  int left_is_ref = type_is_element_ref(left_type);
  ASTNode *ref_node = left_is_ref ? binary->left : binary->right;
  ASTNode *other = left_is_ref ? binary->right : binary->left;
  Type *ref_type = left_is_ref ? left_type : right_type;
  IROperand address = ir_operand_none();
  IROperand index = ir_operand_none();
  if (!ir_lower_lvalue_address(context, function, ref_node, &address, NULL) ||
      !ir_ref_load_word(context, function, &address, 8, location, &index)) {
    return 0;
  }
  if (both) {
    IROperand other_address = ir_operand_none();
    IROperand other_index = ir_operand_none();
    if (!ir_lower_lvalue_address(context, function, other, &other_address, NULL) ||
        !ir_ref_load_word(context, function, &other_address, 8, location,
                          &other_index) ||
        !ir_emit_binary_temp(context, function, "-", &index, &other_index, location,
                             out_value)) {
      return 0;
    }
    ir_operand_destroy(&address);
    ir_operand_destroy(&index);
    ir_operand_destroy(&other_address);
    ir_operand_destroy(&other_index);
    return 1;
  }
  IROperand base = ir_operand_none();
  IROperand count = ir_operand_none();
  IROperand offset = ir_operand_none();
  IROperand moved = ir_operand_none();
  if (!ir_ref_load_word(context, function, &address, 0, location, &base) ||
      !ir_ref_load_word(context, function, &address, 16, location, &count) ||
      !ir_lower_index_expression(context, function, other, &offset) ||
      !ir_emit_binary_temp(context, function, binary->operator, &index, &offset,
                           location, &moved)) {
    return 0;
  }
  int ok = ir_lower_make_element_ref(context, function, ref_type, &base, &moved,
                                     &count, location, out_value);
  ir_operand_destroy(&address);
  ir_operand_destroy(&index);
  ir_operand_destroy(&base);
  ir_operand_destroy(&count);
  ir_operand_destroy(&offset);
  ir_operand_destroy(&moved);
  return ok;
}

int ir_lower_element_ref_compare(IRLoweringContext *context, IRFunction *function,
                                 BinaryExpression *binary, SourceLocation location,
                                 IROperand *out_value) {
  Type *left_type = ir_infer_expression_type(context, binary->left);
  Type *right_type = ir_infer_expression_type(context, binary->right);
  int equals = strcmp(binary->operator, "==") == 0;
  IROperand left = ir_operand_none();
  IROperand left_base = ir_operand_none();
  if (!type_is_element_ref(left_type) || !type_is_element_ref(right_type)) {
    ASTNode *ref_node = type_is_element_ref(left_type) ? binary->left : binary->right;
    IROperand zero = ir_operand_int(0);
    if (!ir_lower_lvalue_address(context, function, ref_node, &left, NULL) ||
        !ir_ref_load_word(context, function, &left, 0, location, &left_base) ||
        !ir_emit_binary_temp(context, function, equals ? "==" : "!=", &left_base,
                             &zero, location, out_value)) {
      return 0;
    }
    ir_operand_destroy(&left);
    ir_operand_destroy(&left_base);
    return 1;
  }
  IROperand right = ir_operand_none();
  IROperand right_base = ir_operand_none();
  IROperand left_index = ir_operand_none();
  IROperand right_index = ir_operand_none();
  IROperand same_base = ir_operand_none();
  IROperand same_index = ir_operand_none();
  IROperand both = ir_operand_none();
  if (!ir_lower_lvalue_address(context, function, binary->left, &left, NULL) ||
      !ir_lower_lvalue_address(context, function, binary->right, &right, NULL) ||
      !ir_ref_load_word(context, function, &left, 0, location, &left_base) ||
      !ir_ref_load_word(context, function, &right, 0, location, &right_base) ||
      !ir_ref_load_word(context, function, &left, 8, location, &left_index) ||
      !ir_ref_load_word(context, function, &right, 8, location, &right_index) ||
      !ir_emit_binary_temp(context, function, "==", &left_base, &right_base,
                           location, &same_base) ||
      !ir_emit_binary_temp(context, function, "==", &left_index, &right_index,
                           location, &same_index) ||
      !ir_emit_binary_temp(context, function, "&", &same_base, &same_index, location,
                           &both)) {
    return 0;
  }
  if (equals) {
    *out_value = both;
    return 1;
  }
  IROperand zero = ir_operand_int(0);
  return ir_emit_binary_temp(context, function, "==", &both, &zero, location,
                             out_value);
}

static int ir_column_element_copy(IRLoweringContext *context,
                                  IRFunction *function, ASTNode *index_node,
                                  const IROperand *lone_address, int into_column,
                                  SourceLocation location) {
  IRColumnAccess access;
  if (!ir_column_access_begin(context, function, index_node, &access)) {
    return 0;
  }
  const TypeMetaLayout *layout = access.element->meta_layout;
  for (size_t r = 0; r < layout->region_count; r++) {
    IROperand column = ir_operand_none();
    IROperand lone = ir_operand_none();
    size_t lone_offset = type_columns_region_base(access.element, r, 1);
    if (!ir_column_region_address(context, function, &access, r, location,
                                  &column) ||
        !ir_emit_address_with_offset(context, function, lone_address,
                                     (long long)lone_offset, location, &lone)) {
      ir_column_access_end(&access);
      return 0;
    }
    int ok = ir_emit_bytes_copy(context, function, into_column ? &column : &lone,
                                into_column ? &lone : &column,
                                layout->regions[r].stride, location);
    ir_operand_destroy(&column);
    ir_operand_destroy(&lone);
    if (!ok) {
      ir_column_access_end(&access);
      return 0;
    }
  }
  ir_column_access_end(&access);
  return 1;
}

int ir_lower_column_element_load(IRLoweringContext *context,
                                 IRFunction *function, ASTNode *expression,
                                 IROperand *out_value) {
  Type *element = ir_infer_expression_type(context, expression);
  char *name = ir_new_label_name(context, "column_element");
  IROperand address = ir_operand_none();
  if (!name ||
      !ir_emit_local_declaration(context, function, name, element->name,
                                 expression->location) ||
      !ir_emit_address_of_symbol(context, function, name, expression->location,
                                 &address) ||
      !ir_column_element_copy(context, function, expression, &address, 0,
                              expression->location)) {
    free(name);
    return 0;
  }
  ir_operand_destroy(&address);
  *out_value = ir_operand_symbol(name);
  free(name);
  return out_value->name != NULL;
}

int ir_lower_column_element_store(IRLoweringContext *context,
                                  IRFunction *function, ASTNode *target,
                                  ASTNode *value_node, SourceLocation location) {
  Type *element = ir_infer_expression_type(context, target);
  char *name = ir_new_label_name(context, "column_element");
  IROperand value = ir_operand_none();
  IROperand address = ir_operand_none();
  if (!name || !ir_emit_local_declaration(context, function, name,
                                          element->name, location)) {
    free(name);
    return 0;
  }
  if (!ir_emit_address_of_symbol(context, function, name, location, &address)) {
    free(name);
    return 0;
  }
  if (value_node->type == AST_AGGREGATE_LITERAL) {
    if (!ir_emit_aggregate_literal_copy(context, function, &address, value_node,
                                        element, location)) {
      free(name);
      return 0;
    }
  } else {
    IRInstruction assign = {0};
    if (!ir_lower_expression(context, function, value_node, &value)) {
      free(name);
      return 0;
    }
    assign.op = IR_OP_ASSIGN;
    assign.location = location;
    assign.dest = ir_operand_symbol(name);
    assign.lhs = value;
    int ok = ir_emit(context, function, &assign);
    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(&value);
    if (!ok) {
      free(name);
      return 0;
    }
  }
  free(name);
  int ok = ir_column_element_copy(context, function, target, &address, 1,
                                  location);
  ir_operand_destroy(&address);
  return ok;
}

int ir_emit_column_bytes(IRLoweringContext *context, IRFunction *function,
                         Type *element, const IROperand *count,
                         SourceLocation location, IROperand *out) {
  const TypeMetaLayout *layout = element->meta_layout;
  IRColumnAccess access;
  size_t last = layout->region_count - 1;
  IROperand region_base = ir_operand_none();
  IROperand stride = ir_operand_int((long long)layout->regions[last].stride);
  IROperand bytes = ir_operand_none();
  IROperand sum = ir_operand_none();
  memset(&access, 0, sizeof(access));
  access.element = element;
  access.known_count = -1;
  access.count = ir_clone_operand_local(count);
  access.slice_address = ir_operand_none();
  if (!ir_column_region_base(context, function, &access, last, location,
                             &region_base) ||
      !ir_emit_binary_temp(context, function, "*", count, &stride, location,
                           &bytes) ||
      !ir_emit_binary_temp(context, function, "+", &region_base, &bytes,
                           location, &sum)) {
    return 0;
  }
  ir_operand_destroy(&access.count);
  ir_operand_destroy(&region_base);
  ir_operand_destroy(&bytes);
  *out = sum;
  return 1;
}

int ir_emit_layout_allocation(IRLoweringContext *context, IRFunction *function,
                              const Type *element, const IROperand *bytes,
                              const IROperand *dest, SourceLocation location) {
  IROperand arguments[2];
  IRInstruction call = {0};
  arguments[0] = ir_clone_operand_local(bytes);
  arguments[1] = ir_operand_int((long long)element->alignment);
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = (char *)element->meta_layout->allocator;
  call.dest = *dest;
  call.arguments = arguments;
  call.argument_count = 2;
  int ok = ir_emit(context, function, &call);
  ir_operand_destroy(&arguments[0]);
  ir_operand_destroy(&arguments[1]);
  return ok;
}

static int ir_lower_member_address_any(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *expression,
                                       IROperand *out_address,
                                       Type **out_type);

static const TypeMetaPlace *ir_member_packed_place(IRLoweringContext *context,
                                                   ASTNode *expression,
                                                   Type **out_field_type) {
  MemberAccess *member;
  Type *object_type;
  int index;
  if (!expression || expression->type != AST_MEMBER_ACCESS ||
      !expression->data) {
    return NULL;
  }
  member = (MemberAccess *)expression->data;
  if (!member->object || !member->member) {
    return NULL;
  }
  object_type = ir_infer_expression_type(context, member->object);
  if (object_type && object_type->kind == TYPE_POINTER) {
    object_type = object_type->base_type;
  }
  if (!object_type || !object_type->meta_layout) {
    return NULL;
  }
  index = type_get_field_index(object_type, member->member);
  if (index < 0 || !type_field_is_packed(object_type, (size_t)index)) {
    return NULL;
  }
  if (out_field_type) {
    *out_field_type = object_type->field_types[index];
  }
  return type_field_place(object_type, (size_t)index);
}

int ir_expression_is_packed_field(IRLoweringContext *context,
                                  ASTNode *expression) {
  return ir_member_packed_place(context, expression, NULL) != NULL;
}

static int ir_emit_shift_mask(IRLoweringContext *context, IRFunction *function,
                              const char *op, const IROperand *value,
                              long long amount, SourceLocation location,
                              IROperand *out) {
  IROperand right = ir_operand_int(amount);
  return ir_emit_binary_temp(context, function, op, value, &right, location, out);
}

int ir_lower_packed_load(IRLoweringContext *context, IRFunction *function,
                         ASTNode *expression, IROperand *out_value) {
  Type *field_type = NULL;
  const TypeMetaPlace *place =
      ir_member_packed_place(context, expression, &field_type);
  IROperand address = ir_operand_none();
  IROperand value = ir_operand_none();
  IROperand next = ir_operand_none();
  IRInstruction load = {0};
  SourceLocation location = expression->location;
  if (!place ||
      !ir_lower_member_address_any(context, function, expression, &address,
                                   NULL) ||
      !ir_make_temp_operand(context, &value)) {
    return 0;
  }
  load.op = IR_OP_LOAD;
  load.location = location;
  load.dest = value;
  load.lhs = address;
  load.rhs = ir_operand_int(place->word);
  load.is_unsigned = 1;
  if (!ir_emit(context, function, &load)) {
    return 0;
  }
  ir_operand_destroy(&address);
  if (place->bit > 0) {
    if (!ir_emit_shift_mask(context, function, ">>", &value, place->bit,
                            location, &next)) {
      return 0;
    }
    ir_operand_destroy(&value);
    value = next;
  }
  if (place->width < 64) {
    if (!ir_emit_shift_mask(context, function, "&", &value,
                            (long long)((1ull << place->width) - 1), location,
                            &next)) {
      return 0;
    }
    ir_operand_destroy(&value);
    value = next;
  }
  int is_signed = field_type->kind == TYPE_INT8 ||
                  field_type->kind == TYPE_INT16 ||
                  field_type->kind == TYPE_INT32 ||
                  field_type->kind == TYPE_INT64;
  if (is_signed && place->width < 64) {
    IROperand raised = ir_operand_none();
    if (!ir_emit_shift_mask(context, function, "<<", &value, 64 - place->width,
                            location, &raised) ||
        !ir_emit_shift_mask(context, function, ">>", &raised, 64 - place->width,
                            location, &next)) {
      return 0;
    }
    ir_operand_destroy(&raised);
    ir_operand_destroy(&value);
    value = next;
  }
  if (place->scale > 0) {
    if (!ir_emit_shift_mask(context, function, "<<", &value, place->scale,
                            location, &next)) {
      return 0;
    }
    ir_operand_destroy(&value);
    value = next;
  }
  *out_value = value;
  return 1;
}

static int ir_emit_packed_store(IRLoweringContext *context, IRFunction *function,
                                const IROperand *word_address,
                                const TypeMetaPlace *place,
                                const IROperand *value,
                                SourceLocation location) {
  unsigned long long mask =
      place->width >= 64 ? ~0ull : (1ull << place->width) - 1;
  IROperand word = ir_operand_none();
  IROperand shifted = ir_operand_none();
  IROperand masked = ir_operand_none();
  IROperand placed = ir_operand_none();
  IROperand kept = ir_operand_none();
  IROperand merged = ir_operand_none();
  IRInstruction load = {0};
  IRInstruction store = {0};
  if (!ir_make_temp_operand(context, &word)) {
    return 0;
  }
  load.op = IR_OP_LOAD;
  load.location = location;
  load.dest = word;
  load.lhs = ir_clone_operand_local(word_address);
  load.rhs = ir_operand_int(place->word);
  load.is_unsigned = 1;
  if (!ir_emit(context, function, &load)) {
    return 0;
  }
  ir_operand_destroy(&load.lhs);
  if (!ir_emit_shift_mask(context, function, ">>", value, place->scale, location,
                          &shifted) ||
      !ir_emit_shift_mask(context, function, "&", &shifted, (long long)mask,
                          location, &masked) ||
      !ir_emit_shift_mask(context, function, "<<", &masked, place->bit, location,
                          &placed) ||
      !ir_emit_shift_mask(context, function, "&", &word,
                          (long long)~(mask << place->bit), location, &kept) ||
      !ir_emit_binary_temp(context, function, "|", &kept, &placed, location,
                           &merged)) {
    return 0;
  }
  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = ir_clone_operand_local(word_address);
  store.lhs = merged;
  store.rhs = ir_operand_int(place->word);
  int ok = ir_emit(context, function, &store);
  ir_operand_destroy(&store.dest);
  ir_operand_destroy(&merged);
  ir_operand_destroy(&word);
  ir_operand_destroy(&shifted);
  ir_operand_destroy(&masked);
  ir_operand_destroy(&placed);
  ir_operand_destroy(&kept);
  return ok;
}

int ir_lower_packed_store(IRLoweringContext *context, IRFunction *function,
                          ASTNode *target, const IROperand *value,
                          SourceLocation location) {
  const TypeMetaPlace *place = ir_member_packed_place(context, target, NULL);
  IROperand address = ir_operand_none();
  if (!place || !ir_lower_member_address_any(context, function, target, &address,
                                             NULL)) {
    return 0;
  }
  int ok =
      ir_emit_packed_store(context, function, &address, place, value, location);
  ir_operand_destroy(&address);
  return ok;
}

static int ir_lower_member_address(IRLoweringContext *context,
                                   IRFunction *function,
                                   ASTNode *expression,
                                   IROperand *out_address,
                                   Type **out_type) {
  if (ir_expression_is_packed_field(context, expression)) {
    char message[300];
    MemberAccess *member = (MemberAccess *)expression->data;
    snprintf(message, sizeof(message),
             "Field '%s' is packed into part of a word, so it has no address",
             member->member);
    ir_layout_refuse(context, expression->location, message);
    return 0;
  }
  return ir_lower_member_address_any(context, function, expression, out_address,
                                     out_type);
}

static int ir_lower_column_member_address(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *expression,
                                          MemberAccess *member,
                                          IROperand *out_address,
                                          Type **out_type) {
  IRColumnAccess access;
  IROperand region = ir_operand_none();
  Type *held = ir_infer_expression_type(context, member->object);
  if (type_is_element_ref(held)) {
    if (!ir_column_access_from_ref(context, function, member->object, NULL,
                                   expression->location, &access)) {
      return 0;
    }
  } else if (!ir_column_access_begin(context, function, member->object, &access)) {
    return 0;
  }
  int field = type_get_field_index(access.element, member->member);
  if (field < 0) {
    ir_column_access_end(&access);
    ir_set_error(context, "Unknown struct field '%s'", member->member);
    return 0;
  }
  const TypeMetaPlace *place = type_field_place(access.element, (size_t)field);
  if (!ir_column_region_address(context, function, &access, place->region,
                                expression->location, &region) ||
      !ir_emit_address_with_offset(context, function, &region,
                                   (long long)place->offset,
                                   expression->location, out_address)) {
    ir_column_access_end(&access);
    return 0;
  }
  if (out_type) {
    *out_type = access.element->field_types[field];
  }
  ir_operand_destroy(&region);
  ir_column_access_end(&access);
  return 1;
}

static int ir_lower_member_address_any(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *expression,
                                       IROperand *out_address,
                                       Type **out_type) {
  MemberAccess *member = (MemberAccess *)expression->data;
  if (!member || !member->object || !member->member) {
    ir_set_error(context, "Malformed member access lvalue");
    return 0;
  }
  if (ir_expression_is_column_element(context, member->object) ||
      (type_is_element_ref(ir_infer_expression_type(context, member->object)) &&
       strncmp(member->member, "__", 2) != 0)) {
    return ir_lower_column_member_address(context, function, expression, member,
                                          out_address, out_type);
  }

  IROperand object_address = ir_operand_none();
  Type *object_type = ir_infer_expression_type(context, member->object);
  int base_is_pointer = 0;

  if (object_type && object_type->kind == TYPE_POINTER) {
    if (!ir_lower_expression(context, function, member->object,
                             &object_address)) {
      return 0;
    }
    base_is_pointer = 1;
    if (!context->emit_safety_checks &&
        !ir_emit_null_check(context, function, expression->location,
                            &object_address)) {
      ir_operand_destroy(&object_address);
      return 0;
    }
    object_type = object_type->base_type;
  } else {
    if (!ir_lower_lvalue_address(context, function, member->object,
                                 &object_address, &object_type)) {
      return 0;
    }
  }
  if (!object_type || (object_type->kind != TYPE_STRUCT &&
                       object_type->kind != TYPE_STRING &&
                       object_type->kind != TYPE_SLICE)) {
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

  if (base_is_pointer) {
    char described[128];
    ir_safety_describe(expression, described, sizeof(described), 0);
    IROperand field_offset_operand = ir_operand_int((long long)field_offset);
    int checked = ir_emit_safety_check(
        context, function, expression->location, &object_address,
        &field_offset_operand, (long long)field_type->size,
        IR_SAFETY_EXTENT_UNKNOWN, IR_SAFETY_ACCESS_READ, described);
    ir_operand_destroy(&field_offset_operand);
    if (!checked) {
      ir_operand_destroy(&object_address);
      return 0;
    }
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
