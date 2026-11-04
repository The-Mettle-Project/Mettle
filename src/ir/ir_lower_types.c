#include "ir_lowering_internal.h"
#include <limits.h>

int ir_type_is_cstring(Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "cstring") == 0;
}

int ir_type_is_rawptr(Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "rawptr") == 0;
}

int ir_expression_is_string(IRLoweringContext *context,
                                   ASTNode *expression) {
  Type *type = ir_infer_expression_type(context, expression);
  return type && type->kind == TYPE_STRING;
}

int ir_should_coerce_string_to_cstring(IRLoweringContext *context,
                                              Type *target_type,
                                              ASTNode *value_expression) {
  return (ir_type_is_cstring(target_type) || ir_type_is_rawptr(target_type)) &&
         ir_expression_is_string(context, value_expression);
}

int ir_normalize_to_bool(IRLoweringContext *context, IRFunction *function,
                         Type *target_type, ASTNode *value_expression,
                         IROperand *value, SourceLocation location) {
  Type *source;
  IROperand destination = ir_operand_none();
  IRInstruction compare = {0};
  int ok;

  if (!target_type || target_type->kind != TYPE_BOOL || !value_expression) {
    return 1;
  }
  source = value_expression->resolved_type
               ? value_expression->resolved_type
               : ir_infer_expression_type(context, value_expression);
  if (!source || source->kind == TYPE_BOOL ||
      (!type_checker_is_integer_type(source) &&
       source->kind != TYPE_CHAR && source->kind != TYPE_POINTER &&
       source->kind != TYPE_ENUM && source->kind != TYPE_FLOAT32 &&
       source->kind != TYPE_FLOAT64)) {
    return 1;
  }
  if (value->kind == IR_OPERAND_INT) {
    value->int_value = value->int_value != 0;
    return 1;
  }
  if (!ir_make_temp_operand(context, &destination)) {
    return 0;
  }
  compare.op = IR_OP_BINARY;
  compare.location = location;
  compare.dest = destination;
  compare.lhs = *value;
  compare.text = (char *)"!=";
  if (source->kind == TYPE_FLOAT32 || source->kind == TYPE_FLOAT64) {
    compare.is_float = 1;
    compare.float_bits = source->kind == TYPE_FLOAT32 ? 32 : 64;
    compare.rhs = ir_operand_float_sized(0.0, compare.float_bits);
  } else {
    compare.is_unsigned = 1;
    compare.rhs = ir_operand_int(0);
  }
  ok = ir_emit(context, function, &compare);
  if (!ok) {
    ir_operand_destroy(&destination);
    return 0;
  }
  ir_operand_destroy(value);
  *value = destination;
  return 1;
}

int ir_should_decay_array_to_address(Type *target_type,
                                     ASTNode *value_expression) {
  return target_type && target_type->kind == TYPE_POINTER &&
         value_expression && value_expression->resolved_type &&
         value_expression->resolved_type->kind == TYPE_ARRAY;
}

int ir_should_build_slice_from_array(Type *target_type,
                                     ASTNode *value_expression) {
  return target_type && target_type->kind == TYPE_SLICE &&
         value_expression && value_expression->resolved_type &&
         value_expression->resolved_type->kind == TYPE_ARRAY;
}

int ir_build_slice_operand_from_array(IRLoweringContext *context,
                                      IRFunction *function, IROperand *value,
                                      Type *array_type, Type *slice_type,
                                      SourceLocation location) {
  char *slice_name = NULL;
  IROperand array_address = ir_operand_none();
  IROperand slice_address = ir_operand_none();
  IROperand slot = ir_operand_none();
  IRInstruction store = {0};

  if (!context || !function || !value || !array_type || !slice_type ||
      value->kind != IR_OPERAND_SYMBOL || !value->name) {
    return 0;
  }

  slice_name = ir_new_label_name(context, "slice");
  if (!slice_name ||
      !ir_emit_local_declaration(context, function, slice_name,
                                 slice_type->name, location)) {
    free(slice_name);
    return 0;
  }
  if (!ir_emit_address_of_symbol(context, function, value->name, location,
                                 &array_address) ||
      !ir_emit_address_of_symbol(context, function, slice_name, location,
                                 &slice_address)) {
    ir_operand_destroy(&array_address);
    ir_operand_destroy(&slice_address);
    free(slice_name);
    return 0;
  }

  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = ir_clone_operand_local(&slice_address);
  store.lhs = array_address;
  store.rhs = ir_operand_int(8);
  if (!ir_emit(context, function, &store)) {
    ir_operand_destroy(&store.dest);
    ir_operand_destroy(&array_address);
    ir_operand_destroy(&slice_address);
    free(slice_name);
    return 0;
  }
  ir_operand_destroy(&store.dest);
  ir_operand_destroy(&array_address);

  if (type_view_rank(slice_type) > 1) {
    size_t rank = type_view_rank(slice_type);
    Type *level = array_type;
    long long extents[16];
    long long stride = 1;
    if (rank > 16) {
      ir_operand_destroy(&slice_address);
      free(slice_name);
      return 0;
    }
    for (size_t k = 0; k < rank; k++) {
      if (!level || level->kind != TYPE_ARRAY) {
        ir_operand_destroy(&slice_address);
        free(slice_name);
        return 0;
      }
      extents[k] = (long long)level->array_size;
      level = level->base_type;
    }
    for (size_t k = 0; k < rank; k++) {
      IROperand extent = ir_operand_int(extents[k]);
      if (!ir_emit_store_word(context, function, &slice_address, 8 + 8 * k,
                              &extent, location)) {
        ir_operand_destroy(&slice_address);
        free(slice_name);
        return 0;
      }
    }
    for (size_t k = rank - 1; k > 0; k--) {
      IROperand lead;
      stride *= extents[k];
      lead = ir_operand_int(stride);
      if (!ir_emit_store_word(context, function, &slice_address,
                              8 + 8 * rank + 8 * (k - 1), &lead, location)) {
        ir_operand_destroy(&slice_address);
        free(slice_name);
        return 0;
      }
    }
    ir_operand_destroy(&slice_address);
    ir_operand_destroy(value);
    *value = ir_operand_symbol(slice_name);
    free(slice_name);
    return value->name != NULL;
  }

  if (!ir_emit_address_with_offset(context, function, &slice_address, 8,
                                   location, &slot)) {
    ir_operand_destroy(&slice_address);
    free(slice_name);
    return 0;
  }
  {
    IRInstruction length = {0};
    length.op = IR_OP_STORE;
    length.location = location;
    length.dest = slot;
    length.lhs = ir_operand_int((long long)array_type->array_size);
    length.rhs = ir_operand_int(8);
    if (!ir_emit(context, function, &length)) {
      ir_operand_destroy(&slot);
      ir_operand_destroy(&slice_address);
      free(slice_name);
      return 0;
    }
  }
  ir_operand_destroy(&slot);
  ir_operand_destroy(&slice_address);

  ir_operand_destroy(value);
  *value = ir_operand_symbol(slice_name);
  free(slice_name);
  return value->name != NULL;
}

static int ir_symbol_is_address_space_allocation(const IRFunction *function,
                                                 const char *name) {
  if (!function || !name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_ADDRESS_SPACE_ALLOC &&
        ir_operand_is_symbol(&instruction->dest) &&
        strcmp(instruction->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_decay_array_operand_to_address(IRLoweringContext *context,
                                      IRFunction *function, IROperand *value,
                                      SourceLocation location) {
  IROperand address = ir_operand_none();

  if (!context || !function || !value ||
      value->kind != IR_OPERAND_SYMBOL || !value->name) {
    return 0;
  }
  if (ir_symbol_is_address_space_allocation(function, value->name)) {
    return 1;
  }
  if (!ir_emit_address_of_symbol(context, function, value->name, location,
                                 &address)) {
    return 0;
  }
  ir_operand_destroy(value);
  *value = address;
  return 1;
}

static int ir_spill_string_temp_to_local(IRLoweringContext *context,
                                         IRFunction *function,
                                         IROperand *value,
                                         SourceLocation location) {
  char *name = ir_new_label_name(context, "strtmp");
  if (!name) {
    return 0;
  }
  if (!ir_emit_local_declaration(context, function, name, "string", location)) {
    free(name);
    return 0;
  }

  IRInstruction store = {0};
  store.op = IR_OP_ASSIGN;
  store.location = location;
  store.dest = ir_operand_symbol(name);
  store.lhs = *value;
  if (!store.dest.name || !ir_emit(context, function, &store)) {
    ir_operand_destroy(&store.dest);
    free(name);
    return 0;
  }

  ir_operand_destroy(&store.dest);
  ir_operand_destroy(value);
  *value = ir_operand_symbol(name);
  free(name);
  return value->name != NULL;
}

int ir_coerce_string_operand_to_cstring(IRLoweringContext *context,
                                               IRFunction *function,
                                               IROperand *value,
                                               SourceLocation location) {
  if (!context || !function || !value || value->kind == IR_OPERAND_NONE) {
    return 0;
  }

  if (value->kind == IR_OPERAND_TEMP &&
      !ir_spill_string_temp_to_local(context, function, value, location)) {
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

Type *ir_resolve_named_type(IRLoweringContext *context,
                                   const char *name) {
  if (!context || !context->type_checker || !name) {
    return NULL;
  }
  return type_checker_get_type_by_name(context->type_checker, name);
}

Type *ir_lookup_symbol_type(IRLoweringContext *context,
                                   const char *name) {
  if (!context || !context->symbol_table || !name) {
    return NULL;
  }
  Symbol *sym = symbol_table_lookup(context->symbol_table, name);
  return sym ? sym->type : NULL;
}

int ir_expression_is_floating(IRLoweringContext *context,
                                     ASTNode *expression) {
  if (!context || !context->type_checker || !expression) {
    return 0;
  }

  Type *type = type_checker_infer_type(context->type_checker, expression);
  if (!type) {
    return 0;
  }

  return type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT64 ||
         type->kind == TYPE_FLOAT16 || type->kind == TYPE_BFLOAT16;
}

int ir_type_is_float64(Type *type) {
  return type && type->kind == TYPE_FLOAT64 && type->size == 8;
}

int ir_type_float_bits(Type *type) {
  if (type && (type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT16 ||
               type->kind == TYPE_BFLOAT16)) {
    return 32;
  }
  return 64;
}

int ir_named_type_float_bits(IRLoweringContext *context,
                                    const char *type_name) {
  Type *type = NULL;
  if (!context || !context->type_checker || !type_name) {
    return 0;
  }
  type = type_checker_get_type_by_name(context->type_checker, type_name);
  if (!type || (type->kind != TYPE_FLOAT32 && type->kind != TYPE_FLOAT64 &&
                type->kind != TYPE_FLOAT16 && type->kind != TYPE_BFLOAT16)) {
    return 0;
  }
  return ir_type_float_bits(type);
}

void ir_operand_apply_float_bits(IROperand *operand, int bits) {
  if (!operand || operand->kind != IR_OPERAND_FLOAT ||
      (bits != 32 && bits != 64)) {
    return;
  }
  if (bits == 32) {
    operand->float_value = (double)(float)operand->float_value;
  }
  operand->float_bits = bits;
}

int ir_symbol_float_bits(IRLoweringContext *context, const char *name) {
  Symbol *symbol = NULL;
  if (!context || !context->symbol_table || !name) {
    return 0;
  }
  symbol = symbol_table_lookup(context->symbol_table, name);
  if (!symbol || !symbol->type ||
      (symbol->type->kind != TYPE_FLOAT32 &&
       symbol->type->kind != TYPE_FLOAT64 &&
       symbol->type->kind != TYPE_FLOAT16 &&
       symbol->type->kind != TYPE_BFLOAT16)) {
    return 0;
  }
  return ir_type_float_bits(symbol->type);
}

int ir_local_declared_float_bits(IRLoweringContext *context,
                                        const IRFunction *function,
                                        const char *name) {
  if (!context || !function || !name) {
    return 0;
  }
  for (size_t i = function->instruction_count; i-- > 0;) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol(&insn->dest) &&
        insn->text && strcmp(insn->dest.name, name) == 0) {
      return ir_named_type_float_bits(context, insn->text);
    }
  }
  return 0;
}

void ir_assign_apply_float_bits(IRInstruction *instruction,
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
    instruction->lhs.float_bits = value->float_bits;
  }
}

void ir_access_apply_alias_class(IRInstruction *access, Type *accessed_type) {
  if (!access || !accessed_type) {
    return;
  }
  if (accessed_type->is_volatile) {
    access->is_volatile = 1;
  }
  switch (accessed_type->kind) {
  case TYPE_POINTER:
  case TYPE_FUNCTION_POINTER:
  case TYPE_STRING:
    access->alias_class = IR_ALIAS_CLASS_POINTER;
    break;
  case TYPE_INT8:
  case TYPE_UINT8:
  case TYPE_BOOL:
    access->alias_class = IR_ALIAS_CLASS_I8;
    break;
  case TYPE_INT16:
  case TYPE_UINT16:
    access->alias_class = IR_ALIAS_CLASS_I16;
    break;
  case TYPE_INT32:
  case TYPE_UINT32:
    access->alias_class = IR_ALIAS_CLASS_I32;
    break;
  case TYPE_INT64:
  case TYPE_UINT64:
    access->alias_class = IR_ALIAS_CLASS_I64;
    break;
  case TYPE_FLOAT32:
    access->alias_class = IR_ALIAS_CLASS_F32;
    break;
  case TYPE_FLOAT64:
    access->alias_class = IR_ALIAS_CLASS_F64;
    break;
  case TYPE_FLOAT16:
    access->alias_class = IR_ALIAS_CLASS_F16;
    break;
  case TYPE_BFLOAT16:
    access->alias_class = IR_ALIAS_CLASS_BF16;
    break;
  default:
    access->alias_class = IR_ALIAS_CLASS_NONE;
    break;
  }
}

void ir_load_apply_float_type(IRInstruction *load, Type *loaded_type) {
  if (!load || !loaded_type) {
    return;
  }
  if (loaded_type->kind != TYPE_FLOAT32 && loaded_type->kind != TYPE_FLOAT64 &&
      loaded_type->kind != TYPE_FLOAT16 && loaded_type->kind != TYPE_BFLOAT16) {
    return;
  }
  load->is_float = 1;
  load->float_bits = ir_type_float_bits(loaded_type);
  load->dest.float_bits = load->float_bits;
}

void ir_load_apply_unsigned(IRInstruction *load, Type *loaded_type) {
  if (!load || !loaded_type) {
    return;
  }
  if (loaded_type->kind == TYPE_UINT8 || loaded_type->kind == TYPE_UINT16 ||
      loaded_type->kind == TYPE_UINT32 || loaded_type->kind == TYPE_UINT64 ||
      loaded_type->kind == TYPE_CHAR || loaded_type->kind == TYPE_BOOL) {
    load->is_unsigned = 1;
  }
}

int ir_expression_float_bits(IRLoweringContext *context,
                                    ASTNode *expression) {
  Type *type = NULL;
  if (!context || !context->type_checker || !expression) {
    return 0;
  }
  type = type_checker_infer_type(context->type_checker, expression);
  if (!type || (type->kind != TYPE_FLOAT32 && type->kind != TYPE_FLOAT64 &&
                type->kind != TYPE_FLOAT16 && type->kind != TYPE_BFLOAT16)) {
    return 0;
  }
  return ir_type_float_bits(type);
}

int ir_binary_operator_is_comparison(const char *op) {
  return op && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0);
}

int ir_binary_expression_operation_float_bits(IRLoweringContext *context,
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

int ir_type_storage_size(Type *type) {
  if (!type || type->size == 0) {
    return 8;
  }

  if (type->size == 1 || type->size == 2 || type->size == 4 ||
      type->size == 8) {
    return (int)type->size;
  }

  return 8;
}

int ir_type_array_element_stride(Type *element_type) {
  if (!element_type || element_type->size == 0 ||
      element_type->size > (size_t)INT_MAX) {
    return 8;
  }
  return (int)element_type->size;
}

int ir_type_is_unsigned_integer(const Type *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_UINT64:
    return 1;
  default:
    return 0;
  }
}

int ir_narrow_integer_shift_bits(Type *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case TYPE_INT8:
  case TYPE_UINT8:
    return 8;
  case TYPE_INT16:
  case TYPE_UINT16:
    return 16;
  case TYPE_INT32:
  case TYPE_UINT32:
    return 32;
  default:
    return 0;
  }
}

int ir_unary_constant_fits(const char *type_name, const char *op,
                           long long value) {
  long long folded;
  if (!type_name || !op) {
    return 0;
  }
  if (strcmp(op, "-") == 0) {
    if (value == LLONG_MIN) {
      return 0;
    }
    folded = -value;
  } else if (strcmp(op, "~") == 0) {
    folded = ~value;
  } else {
    return 0;
  }
  if (strcmp(type_name, "int8") == 0) {
    return folded >= -128 && folded <= 127;
  }
  if (strcmp(type_name, "int16") == 0) {
    return folded >= -32768 && folded <= 32767;
  }
  if (strcmp(type_name, "int32") == 0) {
    return folded >= -2147483648LL && folded <= 2147483647LL;
  }
  if (strcmp(type_name, "uint8") == 0) {
    return folded >= 0 && folded <= 255;
  }
  if (strcmp(type_name, "uint16") == 0) {
    return folded >= 0 && folded <= 65535;
  }
  if (strcmp(type_name, "uint32") == 0) {
    return folded >= 0 && folded <= 4294967295LL;
  }
  return 0;
}

const char *ir_narrow_integer_result_type(Type *type, const char *op) {
  if (!type || !op) {
    return NULL;
  }
  int is_division = strcmp(op, "/") == 0;
  if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 && strcmp(op, "*") != 0 &&
      strcmp(op, "<<") != 0 && strcmp(op, "~") != 0 && !is_division) {
    return NULL;
  }
  if (is_division && type->kind != TYPE_INT8 && type->kind != TYPE_INT16 &&
      type->kind != TYPE_INT32) {
    return NULL;
  }
  switch (type->kind) {
  case TYPE_INT8:
    return "int8";
  case TYPE_INT16:
    return "int16";
  case TYPE_INT32:
    return "int32";
  case TYPE_UINT8:
    return "uint8";
  case TYPE_UINT16:
    return "uint16";
  case TYPE_UINT32:
    return "uint32";
  default:
    return NULL;
  }
}

int ir_type_is_pointer(Type *type) {
  return type && type->kind == TYPE_POINTER && type->base_type;
}

Type *ir_infer_expression_type(IRLoweringContext *context,
                                      ASTNode *expression) {
  if (!context || !context->type_checker || !expression) {
    return NULL;
  }
  return type_checker_infer_type(context->type_checker, expression);
}
