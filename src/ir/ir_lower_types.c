// AST->IR lowering: type / float-width / string-coercion utilities.
#include "ir_lowering_internal.h"

int ir_type_is_cstring(Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "cstring") == 0;
}

int ir_expression_is_string(IRLoweringContext *context,
                                   ASTNode *expression) {
  Type *type = ir_infer_expression_type(context, expression);
  return type && type->kind == TYPE_STRING;
}

int ir_should_coerce_string_to_cstring(IRLoweringContext *context,
                                              Type *target_type,
                                              ASTNode *value_expression) {
  return ir_type_is_cstring(target_type) &&
         ir_expression_is_string(context, value_expression);
}

int ir_coerce_string_operand_to_cstring(IRLoweringContext *context,
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

/* Resolve a named type via the type_checker (works even after scope pop). */
Type *ir_resolve_named_type(IRLoweringContext *context,
                                   const char *name) {
  if (!context || !context->type_checker || !name) {
    return NULL;
  }
  return type_checker_get_type_by_name(context->type_checker, name);
}

/* Look up a symbol's type from the symbol's name; falls back to NULL once the
 * scope is gone. Callers must handle NULL. */
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

  return type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT64;
}

/* True only for a true 8-byte float64. The backend's "known float64" path
 * reinterprets the loaded 64 bits via `movq xmm, r64`; that is correct for
 * float64 but wrong for float32 or integer-width types, so gate strictly. */
int ir_type_is_float64(Type *type) {
  return type && type->kind == TYPE_FLOAT64 && type->size == 8;
}

/* IEEE-754 width for a floating type: 32 for float32, otherwise 64. Callers
 * must already know the type is floating (use ir_type_is_float* / the type
 * checker). Returns 64 for NULL so non-float contexts get the safe default. */
int ir_type_float_bits(Type *type) {
  return (type && type->kind == TYPE_FLOAT32) ? 32 : 64;
}

/* Float width for a named type (e.g. a declared variable / parameter type).
 * Returns 0 when the name does not resolve to a floating type, else 32/64. */
int ir_named_type_float_bits(IRLoweringContext *context,
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

/* Float width of a declared symbol (variable/parameter). 0 if not floating. */
int ir_symbol_float_bits(IRLoweringContext *context, const char *name) {
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
int ir_local_declared_float_bits(IRLoweringContext *context,
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
    /* Preserve the value's own width; the backend converts if it differs
     * from instruction->float_bits. */
    instruction->lhs.float_bits = value->float_bits;
  }
}

/* Mark a LOAD instruction (and its destination temp) as floating when the
 * loaded type is float32/float64, recording the width. Backends key off this
 * to pick movss/cvtss* vs movsd/cvtsd* and 4- vs 8-byte memory access. */
void ir_load_apply_float_type(IRInstruction *load, Type *loaded_type) {
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

/* Record that a load reads an UNSIGNED integer, so the backend zero-extends it
 * (instead of the default sign-extension for a 4-byte load into a temp). Without
 * this a uint32 loaded from a uint32* lands in the register sign-extended, and
 * later 64-bit ops (compare/divide/(int64) widening) read the wrong value. */
void ir_load_apply_unsigned(IRInstruction *load, Type *loaded_type) {
  if (!load || !loaded_type) {
    return;
  }
  if (loaded_type->kind == TYPE_UINT8 || loaded_type->kind == TYPE_UINT16 ||
      loaded_type->kind == TYPE_UINT32 || loaded_type->kind == TYPE_UINT64) {
    load->is_unsigned = 1;
  }
}

/* Resolve the float width of an expression via the type checker. Returns 0
 * when the expression is not floating, else 32 or 64. */
int ir_expression_float_bits(IRLoweringContext *context,
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

/* Memory stride between consecutive elements in an array — must match
 * laid-out sizeof(element), including structs > 8 bytes. Prefer this over
 * ir_type_storage_size() for base + index * stride address math only. */
int ir_type_array_element_stride(Type *element_type) {
  if (!element_type || element_type->size == 0 ||
      element_type->size > (size_t)INT_MAX) {
    return 8;
  }
  return (int)element_type->size;
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
