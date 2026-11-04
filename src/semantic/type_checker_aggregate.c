
#include "type_checker_internal.h"

typedef struct {
  unsigned char *image;
  size_t image_size;
  AggregateReloc *relocs;
  size_t reloc_count;
  size_t reloc_capacity;
  AggregateRuntimeStore *runtime_stores;
  size_t runtime_store_count;
  size_t runtime_store_capacity;
  const Type *column_element;
  size_t column_index;
  size_t column_count;
} AggregateImage;

static void aggregate_image_free(AggregateImage *out) {
  for (size_t i = 0; i < out->reloc_count; i++) {
    free(out->relocs[i].symbol);
    free(out->relocs[i].string);
  }
  free(out->relocs);
  free(out->image);
  free(out->runtime_stores);
  out->image = NULL;
  out->relocs = NULL;
  out->image_size = 0;
  out->reloc_count = 0;
  out->reloc_capacity = 0;
  out->runtime_stores = NULL;
  out->runtime_store_count = 0;
  out->runtime_store_capacity = 0;
}

static int aggregate_image_add_runtime_store(AggregateImage *out, size_t offset,
                                             ASTNode *element, Type *type) {
  if (out->runtime_store_count == out->runtime_store_capacity) {
    size_t next =
        out->runtime_store_capacity ? out->runtime_store_capacity * 2 : 4;
    AggregateRuntimeStore *grown =
        realloc(out->runtime_stores, next * sizeof(*grown));
    if (!grown) {
      return 0;
    }
    out->runtime_stores = grown;
    out->runtime_store_capacity = next;
  }
  out->runtime_stores[out->runtime_store_count].offset = offset;
  out->runtime_stores[out->runtime_store_count].element = element;
  out->runtime_stores[out->runtime_store_count].element_type = type;
  out->runtime_stores[out->runtime_store_count].packed_place = NULL;
  out->runtime_store_count++;
  return 1;
}

static int aggregate_image_add_reloc(AggregateImage *out, size_t offset,
                                     char *symbol, char *string,
                                     size_t string_length,
                                     int string_wants_record) {
  if (out->reloc_count == out->reloc_capacity) {
    size_t next = out->reloc_capacity ? out->reloc_capacity * 2 : 4;
    AggregateReloc *grown = realloc(out->relocs, next * sizeof(*grown));
    if (!grown) {
      free(symbol);
      free(string);
      return 0;
    }
    out->relocs = grown;
    out->reloc_capacity = next;
  }
  out->relocs[out->reloc_count].offset = offset;
  out->relocs[out->reloc_count].symbol = symbol;
  out->relocs[out->reloc_count].string = string;
  out->relocs[out->reloc_count].string_length = string_length;
  out->relocs[out->reloc_count].string_wants_record = string_wants_record;
  out->reloc_count++;
  return 1;
}

typedef struct {
  int is_float;
  long long int_value;
  double float_value;
} AggregateNumber;

static double aggregate_number_as_double(const AggregateNumber *value) {
  return value->is_float ? value->float_value : (double)value->int_value;
}

static int aggregate_fold_binary(TypeChecker *checker, ASTNode *expression,
                                 AggregateNumber *out);

static int aggregate_fold_number(TypeChecker *checker, ASTNode *expression,
                                 AggregateNumber *out) {
  if (!expression || !out) {
    return 0;
  }

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal) {
      return 0;
    }
    out->is_float = literal->is_float ? 1 : 0;
    out->int_value = literal->is_float ? (long long)literal->float_value
                                       : literal->int_value;
    out->float_value = literal->is_float ? literal->float_value
                                         : (double)literal->int_value;
    return 1;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      return 0;
    }
    if (strcmp(identifier->name, "true") == 0 ||
        strcmp(identifier->name, "false") == 0) {
      out->is_float = 0;
      out->int_value = strcmp(identifier->name, "true") == 0;
      out->float_value = (double)out->int_value;
      return 1;
    }
    Symbol *symbol =
        symbol_table_lookup(checker->symbol_table, identifier->name);
    if (symbol && symbol->has_constant_value &&
        (symbol->kind == SYMBOL_CONSTANT || symbol->is_immutable)) {
      out->is_float = symbol->constant_is_float;
      out->float_value = symbol->constant_is_float
                             ? symbol->constant_float_value
                             : (double)symbol->constant_integer_value;
      out->int_value = symbol->constant_is_float
                           ? (long long)symbol->constant_float_value
                           : symbol->constant_integer_value;
      symbol->is_used = 1;
      return 1;
    }
    long long value = 0;
    if (!type_checker_eval_integer_constant_with_checker(checker, expression,
                                                         &value)) {
      return 0;
    }
    out->is_float = 0;
    out->int_value = value;
    out->float_value = (double)value;
    return 1;
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    AggregateNumber operand = {0};
    if (!unary || !unary->operator|| !unary->operand ||
        !aggregate_fold_number(checker, unary->operand, &operand)) {
      return 0;
    }
    if (strcmp(unary->operator, "+") == 0) {
      *out = operand;
      return 1;
    }
    if (strcmp(unary->operator, "-") == 0) {
      out->is_float = operand.is_float;
      out->int_value = -operand.int_value;
      out->float_value = -aggregate_number_as_double(&operand);
      return 1;
    }
    if (strcmp(unary->operator, "!") == 0) {
      out->is_float = 0;
      out->int_value = aggregate_number_as_double(&operand) == 0.0;
      out->float_value = (double)out->int_value;
      return 1;
    }
    if (strcmp(unary->operator, "~") == 0 && !operand.is_float) {
      out->is_float = 0;
      out->int_value = ~operand.int_value;
      out->float_value = (double)out->int_value;
      return 1;
    }
    return 0;
  }

  case AST_BINARY_EXPRESSION:
    return aggregate_fold_binary(checker, expression, out);

  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    AggregateNumber operand = {0};
    if (!cast || !cast->operand ||
        !aggregate_fold_number(checker, cast->operand, &operand)) {
      return 0;
    }
    Type *target = cast->type_name
                       ? type_checker_get_type_by_name(checker, cast->type_name)
                       : NULL;
    if (target && type_checker_is_floating_type(target)) {
      out->is_float = 1;
      out->float_value = aggregate_number_as_double(&operand);
      out->int_value = (long long)out->float_value;
    } else {
      out->is_float = 0;
      out->int_value = operand.is_float ? (long long)operand.float_value
                                        : operand.int_value;
      out->float_value = (double)out->int_value;
    }
    return 1;
  }

  case AST_FUNCTION_CALL: {
    long long value = 0;
    if (!type_checker_eval_integer_constant_with_checker(checker, expression,
                                                         &value)) {
      return 0;
    }
    out->is_float = 0;
    out->int_value = value;
    out->float_value = (double)value;
    return 1;
  }

  default:
    return 0;
  }
}

static int aggregate_fold_binary(TypeChecker *checker, ASTNode *expression,
                                 AggregateNumber *out) {
  BinaryExpression *binary = (BinaryExpression *)expression->data;
  AggregateNumber left = {0};
  AggregateNumber right = {0};
  if (!binary || !binary->operator|| !binary->left || !binary->right ||
      !aggregate_fold_number(checker, binary->left, &left) ||
      !aggregate_fold_number(checker, binary->right, &right)) {
    return 0;
  }
  const char *op = binary->operator;
  if (left.is_float || right.is_float) {
    double l = aggregate_number_as_double(&left);
    double r = aggregate_number_as_double(&right);
    double result = 0.0;
    if (strcmp(op, "+") == 0) {
      result = l + r;
    } else if (strcmp(op, "-") == 0) {
      result = l - r;
    } else if (strcmp(op, "*") == 0) {
      result = l * r;
    } else if (strcmp(op, "/") == 0) {
      if (r == 0.0) {
        return 0;
      }
      result = l / r;
    } else {
      return 0;
    }
    out->is_float = 1;
    out->float_value = result;
    out->int_value = (long long)result;
    return 1;
  }
  long long value = 0;
  if (!type_checker_eval_integer_constant_with_checker(checker, expression,
                                                       &value)) {
    long long l = left.int_value;
    long long r = right.int_value;
    if (strcmp(op, "+") == 0) {
      value = l + r;
    } else if (strcmp(op, "-") == 0) {
      value = l - r;
    } else if (strcmp(op, "*") == 0) {
      value = l * r;
    } else if (strcmp(op, "/") == 0) {
      if (r == 0) {
        return 0;
      }
      value = l / r;
    } else if (strcmp(op, "%") == 0) {
      if (r == 0) {
        return 0;
      }
      value = l % r;
    } else if (strcmp(op, "<<") == 0 && r >= 0 && r < 64) {
      value = (long long)((unsigned long long)l << r);
    } else if (strcmp(op, ">>") == 0 && r >= 0 && r < 64) {
      value = l >> r;
    } else if (strcmp(op, "&") == 0) {
      value = l & r;
    } else if (strcmp(op, "|") == 0) {
      value = l | r;
    } else if (strcmp(op, "^") == 0) {
      value = l ^ r;
    } else {
      return 0;
    }
  }
  out->is_float = 0;
  out->int_value = value;
  out->float_value = (double)value;
  return 1;
}

static const char *aggregate_address_of_name(ASTNode *expression) {
  if (!expression || expression->type != AST_UNARY_EXPRESSION) {
    return NULL;
  }
  UnaryExpression *unary = (UnaryExpression *)expression->data;
  if (!unary || !unary->operator|| strcmp(unary->operator, "&") != 0) {
    return NULL;
  }
  if (!unary->operand || unary->operand->type != AST_IDENTIFIER) {
    return NULL;
  }
  Identifier *identifier = (Identifier *)unary->operand->data;
  if (!identifier || !identifier->name || identifier->name[0] == '\0') {
    return NULL;
  }
  return identifier->name;
}

static void aggregate_store_bits(unsigned char *at, uint64_t bits,
                                 size_t width) {
  for (size_t i = 0; i < width && i < 8; i++) {
    at[i] = (unsigned char)((bits >> (8 * i)) & 0xFFu);
  }
}

static int aggregate_fold_element(TypeChecker *checker, ASTNode *element,
                                  Type *type, size_t offset,
                                  AggregateImage *out);

static int aggregate_fold_scalar(TypeChecker *checker, ASTNode *element,
                                 Type *type, size_t offset,
                                 AggregateImage *out) {
  unsigned char *at = out->image + offset;

  if (type->kind == TYPE_STRING || type_checker_is_cstring_type(type)) {
    if (element->type != AST_STRING_LITERAL) {
      return aggregate_image_add_runtime_store(out, offset, element, type);
    }
    StringLiteral *literal = (StringLiteral *)element->data;
    const char *value = literal && literal->value ? literal->value : "";
    size_t value_length = literal && literal->value ? literal->length : 0;
    char *copy = malloc(value_length + 1);
    if (copy) {
      memcpy(copy, value, value_length);
      copy[value_length] = '\0';
    }
    if (!copy) {
      type_checker_set_error_at_location(checker, element->location,
                                         "Out of memory folding aggregate "
                                         "literal");
      return 0;
    }
    (void)at;
    return aggregate_image_add_reloc(out, offset, NULL, copy, value_length,
                                     type->kind == TYPE_STRING);
  }

  if (type->kind == TYPE_POINTER || type->kind == TYPE_FUNCTION_POINTER) {
    if (type->closure_env) {
      type_checker_set_error_at_location(
          checker, element->location,
          "a closure cannot appear in an aggregate literal: its environment is "
          "built at run time");
      return 0;
    }
    const char *referenced = aggregate_address_of_name(element);
    if (referenced) {
      Symbol *symbol = symbol_table_lookup(checker->symbol_table, referenced);
      if (symbol && symbol->kind != SYMBOL_FUNCTION && symbol->scope &&
          symbol->scope->type != SCOPE_GLOBAL) {
        symbol->is_used = 1;
        return aggregate_image_add_runtime_store(out, offset, element, type);
      }
      char *copy = strdup(referenced);
      if (!copy) {
        type_checker_set_error_at_location(
            checker, element->location, "Out of memory folding aggregate "
                                        "literal");
        return 0;
      }
      if (symbol) {
        symbol->is_used = 1;
      }
      return aggregate_image_add_reloc(out, offset, copy, NULL, 0, 0);
    }
    if (type_checker_is_null_pointer_constant(element)) {
      return 1;
    }
    return aggregate_image_add_runtime_store(out, offset, element, type);
  }

  AggregateNumber value = {0};
  if (!aggregate_fold_number(checker, element, &value)) {
    return aggregate_image_add_runtime_store(out, offset, element, type);
  }

  if (type->kind == TYPE_FLOAT32) {
    float narrowed = (float)aggregate_number_as_double(&value);
    uint32_t bits = 0;
    memcpy(&bits, &narrowed, sizeof(bits));
    aggregate_store_bits(at, bits, 4);
    return 1;
  }
  if (type->kind == TYPE_FLOAT64) {
    double widened = aggregate_number_as_double(&value);
    uint64_t bits = 0;
    memcpy(&bits, &widened, sizeof(bits));
    aggregate_store_bits(at, bits, 8);
    return 1;
  }
  if (type->kind == TYPE_FLOAT16) {
    float narrowed = (float)aggregate_number_as_double(&value);
    uint32_t b = 0;
    memcpy(&b, &narrowed, sizeof(b));
    aggregate_store_bits(at, (uint64_t)mettle_f32bits_to_f16bits(b), 2);
    return 1;
  }
  if (type->kind == TYPE_BFLOAT16) {
    float narrowed = (float)aggregate_number_as_double(&value);
    uint32_t b = 0;
    memcpy(&b, &narrowed, sizeof(b));
    aggregate_store_bits(at, (uint64_t)mettle_f32bits_to_bf16bits(b), 2);
    return 1;
  }
  if (type_checker_is_integer_type(type) || type->kind == TYPE_BOOL ||
      type->kind == TYPE_ENUM) {
    if (value.is_float) {
      type_checker_set_error_at_location(
          checker, element->location,
          "a floating-point constant does not fit the integer element type "
          "'%s'; add a cast",
          type->name ? type->name : "?");
      return 0;
    }
    size_t width = type->size ? type->size : 8;
    aggregate_store_bits(
        at, type->kind == TYPE_BOOL ? (uint64_t)(value.int_value != 0)
                                    : (uint64_t)value.int_value,
        width);
    return 1;
  }

  type_checker_set_error_at_location(
      checker, element->location,
      "type '%s' cannot appear in an aggregate literal",
      type->name ? type->name : "?");
  return 0;
}

static int aggregate_fold_array(TypeChecker *checker, ASTNode *expression,
                                AggregateLiteral *literal, Type *type,
                                size_t offset, AggregateImage *out) {
  Type *element_type = type->base_type;
  size_t capacity = type->array_size;

  if (!element_type || capacity == 0 || element_type->size == 0) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "array type '%s' has no fixed element layout to initialize",
        type->name ? type->name : "?");
    return 0;
  }

  size_t stride = element_type->size;

  if (type_is_columnar(element_type)) {
    size_t written = literal->repeat_count ? 0 : literal->element_count;
    if (literal->repeat_count) {
      long long repeated = 0;
      if (!type_checker_eval_integer_constant_with_checker(
              checker, literal->repeat_count, &repeated) ||
          repeated < 0 || (size_t)repeated > capacity) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "the repeat count does not fit '%s'",
            type->name ? type->name : "?");
        return 0;
      }
      written = (size_t)repeated;
    }
    if (written > capacity) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "%zu elements do not fit '%s', which holds %zu", written,
          type->name ? type->name : "?", capacity);
      return 0;
    }
    for (size_t i = 0; i < written; i++) {
      ASTNode *element =
          literal->elements[literal->repeat_count ? 0 : i];
      if (element->type != AST_AGGREGATE_LITERAL) {
        type_checker_set_error_at_location(
            checker, element->location,
            "an element of '%s' keeps its fields in separate regions, so a "
            "literal for it spells each field",
            element_type->name ? element_type->name : "?");
        return 0;
      }
      out->column_element = element_type;
      out->column_index = i;
      out->column_count = capacity;
      int folded =
          aggregate_fold_element(checker, element, element_type, offset, out);
      out->column_element = NULL;
      if (!folded) {
        return 0;
      }
    }
    return 1;
  }

  if (literal->repeat_count) {
    long long count = 0;
    if (!type_checker_eval_integer_constant_with_checker(
            checker, literal->repeat_count, &count)) {
      type_checker_set_error_at_location(
          checker, literal->repeat_count->location,
          "the repeat count of an array literal must be a compile-time integer "
          "constant");
      return 0;
    }
    if (count < 0 || (size_t)count > capacity) {
      type_checker_set_error_at_location(
          checker, literal->repeat_count->location,
          "repeat count %lld does not fit '%s', which holds %zu elements",
          count, type->name ? type->name : "?", capacity);
      return 0;
    }
    for (long long i = 0; i < count; i++) {
      if (!aggregate_fold_element(checker, literal->elements[0], element_type,
                                  offset + (size_t)i * stride, out)) {
        return 0;
      }
    }
    return 1;
  }

  if (literal->element_count > capacity) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "%zu elements do not fit '%s', which holds %zu",
        literal->element_count, type->name ? type->name : "?", capacity);
    return 0;
  }

  for (size_t i = 0; i < literal->element_count; i++) {
    if (!aggregate_fold_element(checker, literal->elements[i], element_type,
                                offset + i * stride, out)) {
      return 0;
    }
  }
  return 1;
}

static int aggregate_fold_struct(TypeChecker *checker, ASTNode *expression,
                                 AggregateLiteral *literal, Type *type,
                                 size_t offset, AggregateImage *out) {
  if (type->field_count == 0) {
    type_checker_set_error_at_location(
        checker, expression->location, "struct '%s' has no fields to initialize",
        type->name ? type->name : "?");
    return 0;
  }

  for (size_t i = 0; i < literal->element_count; i++) {
    const char *name = literal->field_names ? literal->field_names[i] : NULL;
    if (!name) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Malformed struct literal");
      return 0;
    }

    size_t field = type->field_count;
    for (size_t f = 0; f < type->field_count; f++) {
      if (type->field_names[f] && strcmp(type->field_names[f], name) == 0) {
        field = f;
        break;
      }
    }
    if (field == type->field_count) {
      type_checker_set_error_at_location(
          checker, literal->elements[i]->location,
          "struct '%s' has no field '%s'", type->name ? type->name : "?", name);
      return 0;
    }
    for (size_t earlier = 0; earlier < i; earlier++) {
      if (literal->field_names[earlier] &&
          strcmp(literal->field_names[earlier], name) == 0) {
        type_checker_set_error_at_location(
            checker, literal->elements[i]->location,
            "field '%s' is initialized twice in this struct literal", name);
        return 0;
      }
    }

    size_t field_offset = offset + type->field_offsets[field];
    if (out->column_element == type) {
      const TypeMetaPlace *place = type_field_place(type, field);
      field_offset =
          offset +
          type_columns_region_base(type, place->region, out->column_count) +
          out->column_index * type->meta_layout->regions[place->region].stride +
          place->offset;
    }

    if (type_field_is_packed(type, field)) {
      Type *element_type =
          type_checker_infer_type(checker, literal->elements[i]);
      if (!element_type) {
        return 0;
      }
      if (!(type_checker_takes_null(type->field_types[field], literal->elements[i])) &&
          !type_checker_is_assignable_from(checker, type->field_types[field],
                                           element_type,
                                           literal->elements[i])) {
        type_checker_report_assign_mismatch(
            checker, literal->elements[i], literal->elements[i]->location,
            type->field_types[field], element_type);
        return 0;
      }
      if (!aggregate_image_add_runtime_store(out, field_offset,
                                             literal->elements[i],
                                             type->field_types[field])) {
        return 0;
      }
      out->runtime_stores[out->runtime_store_count - 1].packed_place =
          type_field_place(type, field);
      continue;
    }

    const Type *held_element = out->column_element;
    out->column_element = NULL;
    int field_folded = aggregate_fold_element(checker, literal->elements[i],
                                              type->field_types[field],
                                              field_offset, out);
    out->column_element = held_element;
    if (!field_folded) {
      return 0;
    }
  }
  return 1;
}

static int aggregate_fold_element(TypeChecker *checker, ASTNode *element,
                                  Type *type, size_t offset,
                                  AggregateImage *out) {
  if (!element || !type) {
    return 0;
  }

  if (element->type == AST_AGGREGATE_LITERAL) {
    AggregateLiteral *literal = (AggregateLiteral *)element->data;
    if (!literal) {
      type_checker_set_error_at_location(checker, element->location,
                                         "Malformed aggregate literal");
      return 0;
    }
    if (literal->is_struct && type->kind != TYPE_STRUCT &&
        type->kind != TYPE_SLICE) {
      type_checker_set_error_at_location(
          checker, element->location,
          "'{ ... }' initializes a struct, but '%s' is not one",
          type->name ? type->name : "?");
      return 0;
    }
    if (!literal->is_struct && type->kind != TYPE_ARRAY) {
      type_checker_set_error_at_location(
          checker, element->location,
          "'[ ... ]' initializes an array, but '%s' is not one",
          type->name ? type->name : "?");
      return 0;
    }
    element->resolved_type = type;
    if (offset + type->size > out->image_size) {
      type_checker_set_error_at_location(checker, element->location,
                                         "Aggregate literal overruns its "
                                         "target layout");
      return 0;
    }
    return literal->is_struct
               ? aggregate_fold_struct(checker, element, literal, type, offset,
                                       out)
               : aggregate_fold_array(checker, element, literal, type, offset,
                                      out);
  }

  if (type->kind == TYPE_STRUCT || type->kind == TYPE_ARRAY ||
      type->kind == TYPE_SLICE) {
    Type *value_type = type_checker_infer_type(checker, element);
    if (value_type &&
        type_checker_is_assignable_from(checker, type, value_type, element)) {
      return aggregate_image_add_runtime_store(out, offset, element, type);
    }
    if (value_type) {
      type_checker_report_assign_mismatch(checker, element, element->location,
                                          type, value_type);
      return 0;
    }
    type_checker_set_error_at_location(
        checker, element->location,
        "'%s' needs an aggregate literal here: write '%s'",
        type->name ? type->name : "?",
        type->kind == TYPE_ARRAY ? "[ value, ... ]" : "{ field: value, ... }");
    return 0;
  }

  if (offset + (type->size ? type->size : 8) > out->image_size) {
    type_checker_set_error_at_location(
        checker, element->location, "Aggregate literal overruns its target "
                                    "layout");
    return 0;
  }

  Type *element_type = type_checker_infer_type(checker, element);
  if (!element_type) {
    return 0;
  }
  if (!(type_checker_takes_null(type, element)) &&
      !type_checker_is_assignable_from(checker, type, element_type, element)) {
    type_checker_report_assign_mismatch(checker, element, element->location,
                                        type, element_type);
    return 0;
  }

  return aggregate_fold_scalar(checker, element, type, offset, out);
}

Type *type_checker_check_aggregate_literal(TypeChecker *checker,
                                           ASTNode *expression, Type *target,
                                           int requires_constant) {
  if (!checker || !expression || expression->type != AST_AGGREGATE_LITERAL) {
    return NULL;
  }

  AggregateLiteral *literal = (AggregateLiteral *)expression->data;
  if (!literal) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Malformed aggregate literal");
    return NULL;
  }

  if (!target) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "an aggregate literal takes the type of what it initializes, so it "
        "needs a target type here; use it to initialize a typed 'var' or "
        "'const', or to assign to one");
    return NULL;
  }

  if (target->kind != TYPE_STRUCT && target->kind != TYPE_ARRAY &&
      target->kind != TYPE_SLICE) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "'%s' is not a struct or array, so an aggregate literal cannot "
        "initialize it",
        target->name ? target->name : "?");
    return NULL;
  }
  if (target->kind == TYPE_SLICE && !literal->is_struct && target->base_type) {
    size_t written = literal->element_count;
    char array_name[128];
    Type *sized = NULL;
    if (literal->repeat_count &&
        !type_checker_eval_integer_constant_with_checker(
            checker, literal->repeat_count, (long long *)&written)) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "the repeat count of an array literal must be a compile-time "
          "integer constant");
      return NULL;
    }
    if (written == 0) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "an empty literal gives '%s' no elements and no length to carry",
          target->name ? target->name : "?");
      return NULL;
    }
    snprintf(array_name, sizeof(array_name), "%s[%zu]",
             target->base_type->name ? target->base_type->name : "?", written);
    sized = type_checker_get_type_by_name(checker, array_name);
    if (!sized) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "'%s' has no fixed form to build before it becomes a slice",
          target->name ? target->name : "?");
      return NULL;
    }
    target = sized;
  }

  if (target->size == 0) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "'%s' has no known size to initialize",
        target->name ? target->name : "?");
    return NULL;
  }

  AggregateImage out = {0};
  out.image = calloc(target->size, 1);
  if (!out.image) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Out of memory folding aggregate "
                                       "literal");
    return NULL;
  }
  out.image_size = target->size;

  if (!aggregate_fold_element(checker, expression, target, 0, &out)) {
    aggregate_image_free(&out);
    checker->has_error = 1;
    return NULL;
  }

  free(literal->image);
  for (size_t i = 0; i < literal->reloc_count; i++) {
    free(literal->relocs[i].symbol);
    free(literal->relocs[i].string);
  }
  free(literal->relocs);
  if (requires_constant && out.runtime_store_count > 0) {
    type_checker_set_error_at_location(
        checker, out.runtime_stores[0].element->location,
        "a constant and a module-scope variable are laid out before the "
        "program runs, so every element has to be known while compiling, and "
        "this one is not. Make it a constant, or build the value in a "
        "function");
    aggregate_image_free(&out);
    checker->has_error = 1;
    return NULL;
  }

  free(literal->runtime_stores);
  literal->image = out.image;
  literal->image_size = out.image_size;
  literal->relocs = out.relocs;
  literal->reloc_count = out.reloc_count;
  literal->runtime_stores = out.runtime_stores;
  literal->runtime_store_count = out.runtime_store_count;

  expression->resolved_type = target;
  return target;
}
