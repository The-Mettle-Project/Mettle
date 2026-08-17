// AST->IR lowering: lvalue address, symbol assignment, pointer arithmetic.
#include "ir_lowering_internal.h"
#include "frontend/mtlc_frontend.h" // mtlc_type_from_frontend

/* Spell an access the way the source did, for a --safe failure message. The
 * report already carries the file and line; this is what makes it read as the
 * programmer's own expression rather than a temp number. Anything the walk
 * does not recognize contributes "?", which is honest and still points at the
 * right shape. */
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

/* Park a local aggregate literal's folded image in the module as a hidden
 * constant, and hand back its name.
 *
 * An aggregate literal is a compile-time constant wherever it appears, so a
 * local one is the same value a global one would be: laying it out once in the
 * object file and copying from it beats emitting a store per element, and it
 * costs nothing when the literal is large. Returns an owned name, or NULL on
 * failure (with the error already set). */
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
  free(relocs); /* the program deep-copied them */
  if (!added) {
    free(name);
    ir_set_error(context, "Out of memory while interning aggregate literal");
    return NULL;
  }
  return name;
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

  /* A block move is spelled as a STORE whose value operand is the source
   * ADDRESS, which the backend only reads that way past one machine word. An
   * aggregate that is exactly one machine load wide goes through a register
   * instead: load the word, then store it.
   *
   * Only 1/2/4/8 qualify. A 3-, 5-, 6-, or 7-byte aggregate (`struct Rgb { r:
   * uint8; g: uint8; b: uint8; }`) has no single load that covers it exactly --
   * widening to the next power of two would read past the object and, on the
   * store side, clobber whatever follows it -- so it takes the block move,
   * which is byte-exact for any size. */
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
  int ok = ir_emit(context, function, &store);
  ir_operand_destroy(&store.dest);
  ir_operand_destroy(&value);
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

/* The frontend's nested scopes have been popped by IR lowering time. The IR
 * declaration stream is therefore the authoritative scoped record for whether
 * an array-shaped source binding already IS an address-space pointer (rather
 * than inline host storage whose address must be taken). Scan backwards so a
 * later shadowing declaration wins. */
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
    const IRFunction *function, const ASTNode *expression) {
  if (!expression || expression->type != AST_IDENTIFIER || !expression->data) {
    return 0;
  }
  const Identifier *identifier = (const Identifier *)expression->data;
  return identifier->name &&
         ir_symbol_is_address_space_allocation(function, identifier->name);
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
int ir_try_emit_aggregate_symbol_memcpy(
    IRLoweringContext *context, IRFunction *function, const char *dest_name,
    const IROperand *value, Type *dest_type, SourceLocation location) {
  int nbytes = 0;

  if (!context || !function || !dest_name || !value ||
      value->kind != IR_OPERAND_SYMBOL || !value->name) {
    return 0;
  }
  /* `string` copies whole, like the struct it is: sixteen bytes, not the one
   * word a plain store would move, which would leave the length reading
   * whatever happened to sit beside the pointer. */
  if (!dest_type ||
      (dest_type->kind != TYPE_STRUCT && dest_type->kind != TYPE_STRING)) {
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

/* Gives an aggregate value a home that has an address.
 *
 * The wide-copy paths below build their source operand with
 * ir_emit_address_of_symbol, so they need the value to be a named symbol. A
 * call returning a struct yields a temp instead, which has no address to copy
 * from -- so those paths declined and the caller fell back to a single
 * word-sized store, dropping everything past the first 8 bytes. That is what
 * corrupted `cfg.rect = ui_rect_xywh(...)`: the rect arrived as a temp, only
 * its first word landed in the field, and controls were then created from
 * garbage width and height.
 *
 * Spilling through a fresh local uses only paths already known good: symbol
 * assignment moves a whole aggregate correctly, and the local then supplies the
 * address the wide store needs. Returns 1 and fills `out_symbol` when a spill
 * happened, 0 when the value was already usable or cannot be spilled. */
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
    return 0;               /* already addressable */
  }
  /* A string literal is a value with no address of its own here, the same
   * problem a call result has. Giving it a local makes `h.name = "x"` a
   * sixteen-byte copy like every other whole-record assignment, instead of a
   * one-word store that leaves the length beside it untouched. */
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

/* Whole-struct copy into an arbitrary lvalue address (e.g. `cfg.rect = r;`).
 *
 * Mirrors ir_try_emit_aggregate_symbol_memcpy, but the destination is an
 * already-computed address operand rather than a named symbol. Without this,
 * the lvalue-store path emits a single word-sized IR_OP_STORE for an aggregate
 * RHS and silently drops everything past the first 8 bytes. */
int ir_try_emit_aggregate_address_memcpy(IRLoweringContext *context,
                                         IRFunction *function,
                                         const IROperand *dest_addr,
                                         const IROperand *value, Type *dest_type,
                                         SourceLocation location) {
  int nbytes = 0;

  if (!context || !function || !dest_addr || !value) {
    return 0;
  }
  /* `string` copies whole, like the struct it is: sixteen bytes, not the one
   * word a plain store would move, which would leave the length reading
   * whatever happened to sit beside the pointer. */
  if (!dest_type ||
      (dest_type->kind != TYPE_STRUCT && dest_type->kind != TYPE_STRING)) {
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
    IROperand spilled = ir_operand_none();
    const IROperand *source = value;
    IROperand src_addr = ir_operand_none();
    IROperand dest_copy = ir_operand_none();
    IRInstruction store = {0};
    int ok = 0;

    /* A call result arrives as a temp, which has no address to copy from. */
    if (ir_spill_aggregate_value_to_local(context, function, value, dest_type,
                                          location, &spilled)) {
      source = &spilled;
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
  instruction.text = op;
  return ir_emit(context, function, &instruction);
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
    return ir_emit_address_of_symbol(context, function,
                                     ir_local_ir_name(context,
                                                      identifier->name),
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
    /* A field reached through a pointer is checked once the field's offset and
     * width are known, a few statements below. Reaching one through an inline
     * struct needs no check: the object's own storage is what bounds it, and
     * whatever produced that storage was checked already. */
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
      /* `string` takes the struct path. It is a {chars, length} record, so the
       * fields are measured from the record's address, and that is what the
       * lvalue walk yields for a local, a parameter, or a field of another
       * aggregate alike. Reading the object as a VALUE here was the other half
       * of the two-representations problem: it worked for a parameter, whose
       * slot happens to hold a pointer, and read a local's own first eight
       * bytes as the base address. */
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
    int is_address_space_allocation =
        ir_expression_is_address_space_allocation(function,
                                                  index_expression->array);
    if (array_type->kind == TYPE_ARRAY && is_address_space_allocation) {
      /* Workgroup/private arrays lower to pointer-valued storage bindings. */
      lowered_base =
          ir_lower_expression(context, function, index_expression->array, &base);
    } else if (array_type->kind == TYPE_ARRAY) {
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

    /* --safe supersedes both legacy checks here. Its check traps on a null
     * base with a better message, and it compares the scaled byte offset
     * without sign, so a negative index fails the same comparison as an
     * oversized one instead of slipping past a signed `index < length`. */
    if (!context->emit_safety_checks) {
      if (array_type->kind == TYPE_POINTER && !is_address_space_allocation &&
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

    /* An inline array carries its own length, so the check is a comparison
     * against a constant the compiler already holds. Through a pointer, only
     * the runtime knows how large the allocation is.
     *
     * Address-space allocations are workgroup and private GPU storage, whose
     * bounds the device enforces and whose address is not a host pointer the
     * shadow map could describe. */
    if (!is_address_space_allocation) {
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
    /* An aggregate rvalue -- a struct returned by a call, as in
     * `make_point().x` -- has no storage to point at, so a member or index
     * access on one had nowhere to read from and this reported "not
     * assignable". Give it storage: declare a synthetic local of the value's
     * type, assign the value into it, and hand back that local's address. This
     * is the same shape the frontend already produces for
     * `var t: S = make_point(); t.x`, just without the source-level name. */
    Type *value_type = ir_infer_expression_type(context, expression);
    if (!value_type || !value_type->name ||
        (value_type->kind != TYPE_STRUCT && value_type->kind != TYPE_ARRAY &&
         value_type->kind != TYPE_STRING)) {
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
