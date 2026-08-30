// Type checker: type construction, builtins, numeric promotion, conversions.
#include "type_checker_internal.h"
#include "string_intern.h"

/* A shared non-NULL marker used as closure_env for a boundary closure type
 * (`Fn(...)->R`), where the specific environment layout is opaque. Call dispatch
 * only checks closure_env for non-NULL; the concrete env is known to the callee. */
Type *type_checker_closure_env_sentinel(void) {
  static Type *sentinel = NULL;
  if (!sentinel) {
    sentinel = type_create(TYPE_STRUCT, "__closure_env");
  }
  return sentinel;
}

Type *type_checker_parse_array_type(TypeChecker *checker,
                                           const char *name) {
  if (!checker || !name)
    return NULL;

  const char *lbracket = strchr(name, '[');
  const char *rbracket = lbracket ? strchr(lbracket, ']') : NULL;
  if (!lbracket || !rbracket || rbracket[1] != '\0') {
    return NULL;
  }

  size_t base_len = (size_t)(lbracket - name);
  if (base_len == 0) {
    return NULL;
  }

  char *base_name = malloc(base_len + 1);
  if (!base_name) {
    return NULL;
  }
  memcpy(base_name, name, base_len);
  base_name[base_len] = '\0';

  Type *base_type = type_checker_get_type_by_name(checker, base_name);
  free(base_name);
  if (!base_type) {
    return NULL;
  }

  const char *size_start = lbracket + 1;
  if (size_start == rbracket) {
    return NULL;
  }

  /* Scanned here rather than through strtoull: the owned runtime's strtoull
   * wraps modulo 2^64 and never reports ERANGE, so `int64[2^64 + 1]` came back
   * as an array of one element. A digit past the range is the type not naming
   * an array size at all. */
  unsigned long long array_size_ull = 0;
  int size_is_literal = size_start < rbracket;
  for (const char *digit = size_start; digit < rbracket; digit++) {
    if (*digit < '0' || *digit > '9') {
      size_is_literal = 0;
      break;
    }
    if (array_size_ull > ULLONG_MAX / 10ull) {
      return NULL;
    }
    array_size_ull *= 10ull;
    if (array_size_ull > ULLONG_MAX - (unsigned long long)(*digit - '0')) {
      return NULL;
    }
    array_size_ull += (unsigned long long)(*digit - '0');
  }
  if (!size_is_literal) {
    size_t size_name_len = (size_t)(rbracket - size_start);
    char *size_name = malloc(size_name_len + 1);
    if (!size_name) {
      return NULL;
    }
    memcpy(size_name, size_start, size_name_len);
    size_name[size_name_len] = '\0';
    Symbol *size_symbol = symbol_table_lookup(checker->symbol_table,
                                              size_name);
    free(size_name);
    if (!size_symbol ||
        (!size_symbol->has_constant_value &&
         size_symbol->kind != SYMBOL_CONSTANT) ||
        size_symbol->constant_is_float ||
        !type_checker_is_integer_type(size_symbol->type)) {
      return NULL;
    }
    long long constant_size = size_symbol->has_constant_value
                                  ? size_symbol->constant_integer_value
                                  : size_symbol->data.constant.value;
    if (constant_size <= 0) {
      return NULL;
    }
    array_size_ull = (unsigned long long)constant_size;
  }
  if (array_size_ull == 0 || array_size_ull > SIZE_MAX) {
    return NULL;
  }

  size_t array_size = (size_t)array_size_ull;
  /* SIZE_MAX is not a bound any object can actually reach: the backend keeps
   * frame offsets and local storage sizes in `int`, so an array whose bytes
   * pass INT_MAX arrived there as a negative size and was reported as an
   * internal compiler error. int64[1152921504606846976] fit under SIZE_MAX/8
   * and did exactly that. */
  if (base_type->size > 0 &&
      array_size > (size_t)INT_MAX / base_type->size) {
    return NULL;
  }

  Type *array_type = type_create(TYPE_ARRAY, name);
  if (!array_type) {
    return NULL;
  }

  array_type->base_type = base_type;
  array_type->array_size = array_size;
  if (!type_compute_layout(array_type)) {
    type_destroy(array_type);
    return NULL;
  }
  return type_checker_canon_type(checker, array_type);
}

int type_checker_ensure_multi_return_type(TypeChecker *checker,
                                          FunctionDeclaration *function,
                                          SourceLocation location) {
  if (!checker || !function || function->return_type_count < 2 ||
      !function->return_type) {
    return 0;
  }

  Type *existing = type_checker_get_type_by_name(checker, function->return_type);
  if (existing) {
    return existing->kind == TYPE_STRUCT &&
           existing->field_count == function->return_type_count;
  }

  Type **field_types = calloc(function->return_type_count, sizeof(Type *));
  char **field_names = calloc(function->return_type_count, sizeof(char *));
  if (!field_types || !field_names) {
    free(field_types);
    free(field_names);
    return 0;
  }

  for (size_t i = 0; i < function->return_type_count; i++) {
    field_types[i] = type_checker_get_type_by_name(
        checker, function->return_types[i]);
    /* Every other type is copied whole into the tuple and back out again. An
     * array is not: it would decay to its first element's address and the
     * caller would read a slot that has already been reused. Reject it here,
     * where the function's own signature is what the message can point at. */
    if (field_types[i] && field_types[i]->kind == TYPE_ARRAY) {
      type_checker_set_error_at_location(
          checker, location,
          "Function '%s' returns an array as value %zu of %zu; return a "
          "pointer to it, or wrap it in a struct",
          function->name ? function->name : "?", i + 1,
          function->return_type_count);
      for (size_t j = 0; j <= i; j++) {
        free(field_names[j]);
      }
      free(field_names);
      free(field_types);
      return 0;
    }
    field_names[i] = malloc(32);
    if (!field_types[i] || !field_names[i]) {
      for (size_t j = 0; j <= i; j++) {
        free(field_names[j]);
      }
      free(field_names);
      free(field_types);
      return 0;
    }
    snprintf(field_names[i], 32, "_%zu", i);
  }

  Type *tuple_type = type_create_struct(function->return_type, field_names,
                                        field_types, function->return_type_count);
  for (size_t i = 0; i < function->return_type_count; i++) {
    free(field_names[i]);
  }
  free(field_names);
  free(field_types);
  if (!tuple_type) {
    return 0;
  }

  Symbol *tuple_symbol = symbol_create(function->return_type, SYMBOL_STRUCT,
                                        tuple_type);
  if (!tuple_symbol ||
      !symbol_table_declare(checker->symbol_table, tuple_symbol)) {
    symbol_destroy(tuple_symbol);
    type_destroy(tuple_type);
    return 0;
  }
  return 1;
}

/* Pointer to an arbitrary type, built from the type rather than from its
 * spelling. Address-of used to mangle "<name>*" and look the result up, which
 * works while the name is a plain identifier and fails the moment it is not:
 * `&slot` on a `fn(int32) -> int32` global asked for a type named
 * "fn(int32) -> int32*", which nothing registers. */
Type *type_checker_pointer_to(TypeChecker *checker, Type *base) {
  if (!checker || !base) {
    return NULL;
  }
  const char *base_name = base->name ? base->name : "ptr";
  size_t pointer_name_len = strlen(base_name) + 2;
  char *pointer_name = malloc(pointer_name_len);
  if (!pointer_name) {
    return NULL;
  }
  snprintf(pointer_name, pointer_name_len, "%s*", base_name);

  Type *existing = type_checker_get_type_by_name(checker, pointer_name);
  if (existing) {
    free(pointer_name);
    return existing;
  }

  Type *pointer_type = type_create(TYPE_POINTER, pointer_name);
  free(pointer_name);
  if (!pointer_type) {
    return NULL;
  }
  pointer_type->base_type = base;
  if (!type_compute_layout(pointer_type)) {
    type_destroy(pointer_type);
    return NULL;
  }
  return type_checker_canon_type(checker, pointer_type);
}

Type *type_checker_volatile_of(TypeChecker *checker, Type *base) {
  if (!checker || !base) {
    return NULL;
  }
  if (base->is_volatile) {
    return base;
  }
  {
    const char *base_name = base->name ? base->name : "value";
    size_t length = strlen(base_name) + 10;
    char *qualified_name = malloc(length);
    Type *qualified = NULL;
    size_t i;
    if (!qualified_name) {
      return NULL;
    }
    snprintf(qualified_name, length, "volatile %s", base_name);
    for (i = 0; i < checker->type_table_count; i++) {
      Type *existing = checker->type_table[i];
      if (existing && existing->is_volatile && existing->name &&
          strcmp(existing->name, qualified_name) == 0) {
        free(qualified_name);
        return existing;
      }
    }
    qualified = type_create(base->kind, qualified_name);
    free(qualified_name);
    if (!qualified) {
      return NULL;
    }
    qualified->is_volatile = 1;
    qualified->base_type = base->base_type;
    qualified->array_size = base->array_size;
    qualified->fn_param_types = base->fn_param_types;
    qualified->fn_param_count = base->fn_param_count;
    qualified->fn_return_type = base->fn_return_type;
    qualified->closure_env = base->closure_env;
    qualified->field_names = base->field_names;
    qualified->field_types = base->field_types;
    qualified->field_offsets = base->field_offsets;
    qualified->field_bit_offsets = base->field_bit_offsets;
    qualified->field_bit_widths = base->field_bit_widths;
    qualified->field_count = base->field_count;
    qualified->tagged_variant_names = base->tagged_variant_names;
    qualified->tagged_variant_tags = base->tagged_variant_tags;
    qualified->tagged_variant_payloads = base->tagged_variant_payloads;
    qualified->tagged_variant_count = base->tagged_variant_count;
    qualified->enum_member_names = base->enum_member_names;
    qualified->enum_member_values = base->enum_member_values;
    qualified->enum_member_count = base->enum_member_count;
    qualified->tagged_data_offset = base->tagged_data_offset;
    qualified->tagged_data_size = base->tagged_data_size;
    qualified->size = base->size;
    qualified->alignment = base->alignment;
    if (type_checker_intern_type(checker, qualified) == UINT32_MAX) {
      return base;
    }
    return qualified;
  }
}

Type *type_checker_parse_pointer_type(TypeChecker *checker,
                                             const char *name) {
  if (!checker || !name) {
    return NULL;
  }

  size_t name_len = strlen(name);
  size_t pointer_depth = 0;
  while (name_len > 0 && name[name_len - 1] == '*') {
    pointer_depth++;
    name_len--;
  }

  if (pointer_depth == 0 || name_len == 0) {
    return NULL;
  }

  char *base_name = malloc(name_len + 1);
  if (!base_name) {
    return NULL;
  }
  memcpy(base_name, name, name_len);
  base_name[name_len] = '\0';

  Type *base_type = type_checker_get_type_by_name(checker, base_name);
  free(base_name);
  if (!base_type) {
    return NULL;
  }

  Type *current = base_type;
  for (size_t i = 0; i < pointer_depth; i++) {
    const char *current_name = current && current->name ? current->name : "ptr";
    size_t pointer_name_len = strlen(current_name) + 2;
    char *pointer_name = malloc(pointer_name_len);
    if (!pointer_name) {
      return NULL;
    }
    snprintf(pointer_name, pointer_name_len, "%s*", current_name);

    Type *pointer_type = type_create(TYPE_POINTER, pointer_name);
    free(pointer_name);
    if (!pointer_type) {
      return NULL;
    }

    pointer_type->base_type = current;
    if (!type_compute_layout(pointer_type)) {
      type_destroy(pointer_type);
      return NULL;
    }
    current = type_checker_canon_type(checker, pointer_type);
  }

  return current;
}

Type *type_checker_parse_function_pointer_type(TypeChecker *checker,
                                                      const char *name) {
  if (!checker || !name) {
    return NULL;
  }

  // Check if it's a function pointer type: fn(param1,param2)->returntype (thin)
  // or Fn(...)->returntype (a stateful closure type). Both prefixes are 3 chars.
  int is_closure_type = 0;
  if (strlen(name) < 4 || strncmp(name, "fn(", 3) != 0) {
    if (strlen(name) >= 4 && strncmp(name, "Fn(", 3) == 0) {
      is_closure_type = 1;
    } else {
      return NULL;
    }
  }

  size_t close_index = 0;
  int paren_depth = 0;
  int found_close = 0;
  for (size_t i = 2; name[i] != '\0'; i++) {
    if (name[i] == '(') {
      paren_depth++;
    } else if (name[i] == ')') {
      paren_depth--;
      if (paren_depth < 0) {
        return NULL;
      }
      if (paren_depth == 0) {
        close_index = i;
        found_close = 1;
        break;
      }
    }
  }

  if (!found_close || name[close_index + 1] != '-' ||
      name[close_index + 2] != '>') {
    return NULL;
  }

  // Parse parameter types
  const char *params_start = name + 3; // skip "fn("
  const char *params_end = name + close_index;
  size_t params_len = params_end - params_start;

  Type **param_types = NULL;
  size_t param_count = 0;
  char *params_copy = NULL;

  if (params_len > 0) {
    // Parse comma-separated parameter types, splitting only on top-level commas.
    params_copy = malloc(params_len + 1);
    if (!params_copy) {
      return NULL;
    }
    memcpy(params_copy, params_start, params_len);
    params_copy[params_len] = '\0';

    // Count top-level parameters.
    param_count = 1;
    int angle_depth = 0;
    int bracket_depth = 0;
    paren_depth = 0;
    for (size_t i = 0; i < params_len; i++) {
      if (params_copy[i] == '<') {
        angle_depth++;
      } else if (params_copy[i] == '>') {
        if (angle_depth > 0) {
          angle_depth--;
        }
      } else if (params_copy[i] == '[') {
        bracket_depth++;
      } else if (params_copy[i] == ']') {
        bracket_depth--;
      } else if (params_copy[i] == '(') {
        paren_depth++;
      } else if (params_copy[i] == ')') {
        paren_depth--;
      } else if (params_copy[i] == ',' && angle_depth == 0 &&
                 bracket_depth == 0 && paren_depth == 0) {
        param_count++;
      }

      if (angle_depth < 0 || bracket_depth < 0 || paren_depth < 0) {
        free(params_copy);
        return NULL;
      }
    }

    param_types = calloc(param_count, sizeof(Type *));
    if (!param_types) {
      free(params_copy);
      return NULL;
    }

    // Parse each top-level parameter type.
    size_t param_start = 0;
    size_t param_idx = 0;
    angle_depth = 0;
    bracket_depth = 0;
    paren_depth = 0;
    for (size_t i = 0; i <= params_len; i++) {
      char ch = params_copy[i];
      int is_end = (ch == '\0');

      if (!is_end) {
        if (ch == '<') {
          angle_depth++;
        } else if (ch == '>') {
          if (angle_depth > 0) {
            angle_depth--;
          }
        } else if (ch == '[') {
          bracket_depth++;
        } else if (ch == ']') {
          bracket_depth--;
        } else if (ch == '(') {
          paren_depth++;
        } else if (ch == ')') {
          paren_depth--;
        }
      }

      if (angle_depth < 0 || bracket_depth < 0 || paren_depth < 0) {
        free(params_copy);
        free(param_types);
        return NULL;
      }

      int is_separator =
          is_end || (ch == ',' && angle_depth == 0 && bracket_depth == 0 &&
                     paren_depth == 0);
      if (!is_separator) {
        continue;
      }

      size_t start = param_start;
      size_t end = i;
      while (start < end && isspace((unsigned char)params_copy[start])) {
        start++;
      }
      while (end > start && isspace((unsigned char)params_copy[end - 1])) {
        end--;
      }
      if (end <= start) {
        free(params_copy);
        free(param_types);
        return NULL;
      }

      char saved = params_copy[end];
      params_copy[end] = '\0';
      Type *param_type =
          type_checker_get_type_by_name(checker, params_copy + start);
      params_copy[end] = saved;
        if (!param_type) {
          free(params_copy);
          free(param_types);
          return NULL;
        }
        if (param_idx < param_count) {
          param_types[param_idx++] = param_type;
        }
        param_start = i + 1;
      }

    if (param_idx != param_count) {
      free(params_copy);
      free(param_types);
      return NULL;
    }
  }

  // Parse return type
  const char *return_type_start = name + close_index + 3; // skip ")->"
  if (*return_type_start == '\0') {
    free(params_copy);
    free(param_types);
    return NULL;
  }
  char *return_copy = strdup(return_type_start);
  if (!return_copy) {
    free(params_copy);
    free(param_types);
    return NULL;
  }
  size_t return_start = 0;
  size_t return_end = strlen(return_copy);
  while (return_start < return_end &&
         isspace((unsigned char)return_copy[return_start])) {
    return_start++;
  }
  while (return_end > return_start &&
         isspace((unsigned char)return_copy[return_end - 1])) {
    return_end--;
  }
  if (return_end <= return_start) {
    free(params_copy);
    free(param_types);
    free(return_copy);
    return NULL;
  }
  return_copy[return_end] = '\0';

  Type *return_type =
      type_checker_get_type_by_name(checker, return_copy + return_start);
  if (!return_type) {
    free(params_copy);
    free(param_types);
    free(return_copy);
    return NULL;
  }

  Type *fp_type =
      type_create_function_pointer(param_types, param_count, return_type);
  free(params_copy);
  free(param_types);
  free(return_copy);
  if (!fp_type) {
    return NULL;
  }
  if (is_closure_type) {
    /* Name it with the resolvable `Fn(...)->R` string so an inferred closure
     * local is sized as an 8-byte pointer by the backend, and mark it a
     * closure so calls dispatch through the environment. */
    fp_type->name = (char *)string_intern(name);
    fp_type->closure_env = type_checker_closure_env_sentinel();
  }

  return fp_type;
}


int type_checker_types_equal(const Type *lhs, const Type *rhs) {
  if (lhs == rhs) {
    return 1;
  }
  if (!lhs || !rhs) {
    return 0;
  }
  if (lhs->kind != rhs->kind) {
    return 0;
  }

  switch (lhs->kind) {
  case TYPE_POINTER:
  case TYPE_SLICE:
    return type_checker_types_equal(lhs->base_type, rhs->base_type);
  case TYPE_ARRAY:
    return lhs->array_size == rhs->array_size &&
           type_checker_types_equal(lhs->base_type, rhs->base_type);
  case TYPE_STRUCT:
  case TYPE_ENUM:
  case TYPE_TAGGED_ENUM:
    if (lhs->name && rhs->name) {
      return strcmp(lhs->name, rhs->name) == 0;
    }
    return lhs->name == rhs->name;
  case TYPE_FUNCTION_POINTER:
    // Function pointer types with same signature are equal
    if (lhs->fn_param_count != rhs->fn_param_count) {
      return 0;
    }
    // Check return type
    if (!type_checker_types_equal(lhs->fn_return_type, rhs->fn_return_type)) {
      return 0;
    }
    // Check parameter types
    for (size_t i = 0; i < lhs->fn_param_count; i++) {
      if (!type_checker_types_equal(lhs->fn_param_types[i],
                                    rhs->fn_param_types[i])) {
        return 0;
      }
    }
    return 1;
  default:
    return 1;
  }
}

int type_checker_is_cstring_type(const Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "cstring") == 0;
}

int type_checker_is_rawptr_type(const Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "rawptr") == 0;
}

// Built-in type system functions implementation

void type_checker_init_builtin_types(TypeChecker *checker) {
  if (!checker)
    return;

  // Create built-in integer types
  checker->builtin_int8 = type_create(TYPE_INT8, "int8");
  checker->builtin_int16 = type_create(TYPE_INT16, "int16");
  checker->builtin_int32 = type_create(TYPE_INT32, "int32");
  checker->builtin_int64 = type_create(TYPE_INT64, "int64");

  // Create built-in unsigned integer types
  checker->builtin_uint8 = type_create(TYPE_UINT8, "uint8");
  checker->builtin_uint16 = type_create(TYPE_UINT16, "uint16");
  checker->builtin_uint32 = type_create(TYPE_UINT32, "uint32");
  checker->builtin_uint64 = type_create(TYPE_UINT64, "uint64");

  // Create first-class bool type (1-byte integer, distinct from uint8)
  checker->builtin_bool = type_create(TYPE_BOOL, "bool");
  if (checker->builtin_bool) {
    checker->builtin_bool->size = 1;
    checker->builtin_bool->alignment = 1;
  }

  // Create first-class char type (1-byte character, distinct from uint8)
  checker->builtin_char = type_create(TYPE_CHAR, "char");
  if (checker->builtin_char) {
    checker->builtin_char->size = 1;
    checker->builtin_char->alignment = 1;
  }

  // Create built-in floating-point types
  checker->builtin_float32 = type_create(TYPE_FLOAT32, "float32");
  checker->builtin_float64 = type_create(TYPE_FLOAT64, "float64");

  // C interop alias: cstring -> uint8*
  checker->builtin_cstring = type_create(TYPE_POINTER, "cstring");
  if (checker->builtin_cstring) {
    checker->builtin_cstring->base_type = checker->builtin_uint8;
    type_compute_layout(checker->builtin_cstring);
  }

  // Create built-in string type backed by a uint8* and length
  checker->builtin_string = type_create(TYPE_STRING, "string");
  if (checker->builtin_string) {
    checker->builtin_string->size = 16;
    checker->builtin_string->alignment = 8;

    if (!type_alloc_fields(checker->builtin_string, 2)) {
      return;
    }
    Type *chars = type_create(TYPE_POINTER, "uint8*");
    if (chars) {
      chars->base_type = checker->builtin_uint8;
      type_compute_layout(chars);
      chars = type_checker_canon_type(checker, chars);
    }
    type_set_field(checker->builtin_string, 0, "chars", chars, 0);
    type_set_field(checker->builtin_string, 1, "length", checker->builtin_uint64,
                   0);
    type_compute_layout(checker->builtin_string);
  }

  // Create built-in void type
  checker->builtin_void = type_create(TYPE_VOID, "void");
  if (checker->builtin_void) {
    checker->builtin_void->size = 0;
    checker->builtin_void->alignment = 1;
  }

  /* An address with no element type. The allocator hands one out and the
   * deallocator takes one, so releasing an int32 buffer no longer requires
   * claiming it holds characters. It converts to and from every pointer type,
   * and only to them: with no element size there is nothing to index or offset
   * by, and the checker's pointer arithmetic refuses it on those grounds. */
  checker->builtin_rawptr = type_create(TYPE_POINTER, "rawptr");
  if (checker->builtin_rawptr) {
    checker->builtin_rawptr->base_type = checker->builtin_void;
    checker->builtin_rawptr->size = 8;
    checker->builtin_rawptr->alignment = 8;
  }

  /* Type and Field are comptime-only: size 0, no backend kind. */
  checker->builtin_type = type_create(TYPE_TYPE, "Type");
  if (checker->builtin_type) {
    checker->builtin_type->size = 0;
    checker->builtin_type->alignment = 0;
  }
  checker->builtin_field = type_create(TYPE_FIELD, "Field");
  if (checker->builtin_field) {
    checker->builtin_field->size = 0;
    checker->builtin_field->alignment = 0;
  }
  checker->builtin_sequence = type_create(TYPE_SEQUENCE, "Sequence");
  if (checker->builtin_sequence) {
    checker->builtin_sequence->size = 0;
    checker->builtin_sequence->alignment = 0;
  }

  type_checker_intern_type(checker, checker->builtin_int8);
  type_checker_intern_type(checker, checker->builtin_int16);
  type_checker_intern_type(checker, checker->builtin_int32);
  type_checker_intern_type(checker, checker->builtin_int64);
  type_checker_intern_type(checker, checker->builtin_uint8);
  type_checker_intern_type(checker, checker->builtin_uint16);
  type_checker_intern_type(checker, checker->builtin_uint32);
  type_checker_intern_type(checker, checker->builtin_uint64);
  type_checker_intern_type(checker, checker->builtin_bool);
  type_checker_intern_type(checker, checker->builtin_float32);
  type_checker_intern_type(checker, checker->builtin_float64);
  type_checker_intern_type(checker, checker->builtin_string);
  type_checker_intern_type(checker, checker->builtin_cstring);
  type_checker_intern_type(checker, checker->builtin_rawptr);
  type_checker_intern_type(checker, checker->builtin_void);
  type_checker_intern_type(checker, checker->builtin_type);
  type_checker_intern_type(checker, checker->builtin_field);
  type_checker_intern_type(checker, checker->builtin_sequence);

  // Register 'true' and 'false' as global bool constants so user code can
  // reference them as plain identifiers without any extra keyword machinery.
  if (checker->builtin_bool && checker->symbol_table) {
    Symbol *true_sym =
        symbol_create("true", SYMBOL_CONSTANT, checker->builtin_bool);
    if (true_sym) {
      true_sym->data.constant.value = 1;
      true_sym->is_initialized = 1;
      symbol_table_insert(checker->symbol_table, true_sym);
    }
    Symbol *false_sym =
        symbol_create("false", SYMBOL_CONSTANT, checker->builtin_bool);
    if (false_sym) {
      false_sym->data.constant.value = 0;
      false_sym->is_initialized = 1;
      symbol_table_insert(checker->symbol_table, false_sym);
    }
  }
}

Type *type_checker_get_type_by_name(TypeChecker *checker, const char *name) {
  if (!checker || !name)
    return NULL;

  // Check built-in types by name
  if (strcmp(name, "bool") == 0)
    return checker->builtin_bool;
  if (strcmp(name, "char") == 0)
    return checker->builtin_char;
  if (strcmp(name, "int8") == 0)
    return checker->builtin_int8;
  if (strcmp(name, "int16") == 0)
    return checker->builtin_int16;
  if (strcmp(name, "int32") == 0)
    return checker->builtin_int32;
  if (strcmp(name, "int64") == 0)
    return checker->builtin_int64;
  if (strcmp(name, "uint8") == 0)
    return checker->builtin_uint8;
  if (strcmp(name, "uint16") == 0)
    return checker->builtin_uint16;
  if (strcmp(name, "uint32") == 0)
    return checker->builtin_uint32;
  if (strcmp(name, "uint64") == 0)
    return checker->builtin_uint64;
  if (strcmp(name, "float32") == 0)
    return checker->builtin_float32;
  if (strcmp(name, "float64") == 0)
    return checker->builtin_float64;
  if (strcmp(name, "string") == 0)
    return checker->builtin_string;
  if (strcmp(name, "cstring") == 0)
    return checker->builtin_cstring;
  if (strcmp(name, "rawptr") == 0)
    return checker->builtin_rawptr;
  if (strcmp(name, "void") == 0)
    return checker->builtin_void;
  if (strcmp(name, "Type") == 0)
    return checker->builtin_type;
  if (strcmp(name, "Field") == 0)
    return checker->builtin_field;
  if (strcmp(name, "Kind") == 0) {
    type_checker_register_kind_enum(checker);
    return checker->builtin_kind;
  }

  // Check for function pointer types: fn(...)->R (thin) or Fn(...)->R (closure).
  if (strncmp(name, "fn(", 3) == 0 || strncmp(name, "Fn(", 3) == 0) {
    Type *fp_type = type_checker_parse_function_pointer_type(checker, name);
    if (fp_type) {
      return fp_type;
    }
  }

  if (strchr(name, '[') && strchr(name, ']')) {
    Type *array_type = type_checker_parse_array_type(checker, name);
    if (array_type) {
      return array_type;
    }
  }

  if (strchr(name, '*')) {
    Type *pointer_type = type_checker_parse_pointer_type(checker, name);
    if (pointer_type) {
      return pointer_type;
    }
  }

  /* `volatile T`. The qualifier binds to the value being accessed, so
   * `volatile uint16*` is a pointer to volatile uint16: the pointer branch
   * above strips the `*` first and lands back here on the element. */
  if (strncmp(name, "volatile ", 9) == 0) {
    Type *base = type_checker_get_type_by_name(checker, name + 9);
    if (base) {
      return type_checker_volatile_of(checker, base);
    }
    return NULL;
  }

  // Check for user-defined types in symbol table
  Symbol *struct_symbol = symbol_table_lookup(checker->symbol_table, name);
  if (struct_symbol && (struct_symbol->kind == SYMBOL_STRUCT ||
                        struct_symbol->kind == SYMBOL_ENUM)) {
    return struct_symbol->type;
  }

  // Check for generic enum instantiation: "Option<int32>", "Result<int64,string>"
  // Syntax stored by the parser as "Name<arg>" or "Name<arg1,arg2>"
  const char *lt = strchr(name, '<');
  if (lt && name[strlen(name) - 1] == '>') {
    size_t base_len = (size_t)(lt - name);
    char *base_name = malloc(base_len + 1);
    if (base_name) {
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      const char *arg_start = lt + 1;
      const char *arg_end = name + strlen(name) - 1;
      size_t arg_len = (size_t)(arg_end - arg_start);
      char *arg_str = malloc(arg_len + 1);
      if (arg_str) {
        memcpy(arg_str, arg_start, arg_len);
        arg_str[arg_len] = '\0';
        Type *result =
            type_checker_instantiate_generic_enum(checker, base_name, arg_str);
        free(arg_str);
        free(base_name);
        if (result)
          return result;
      } else {
        free(base_name);
      }
    }
  }

  return NULL;
}

int type_checker_is_integer_type(Type *type) {
  if (!type)
    return 0;

  switch (type->kind) {
  case TYPE_INT8:
  case TYPE_INT16:
  case TYPE_INT32:
  case TYPE_INT64:
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_UINT64:
  case TYPE_BOOL:
  case TYPE_CHAR:
    return 1;
  default:
    return 0;
  }
}

int type_checker_is_discrete_type(Type *type) {
  return type_checker_is_integer_type(type) ||
         (type && type->kind == TYPE_ENUM);
}

int type_checker_is_floating_type(Type *type) {
  if (!type)
    return 0;

  switch (type->kind) {
  case TYPE_FLOAT32:
  case TYPE_FLOAT64:
    return 1;
  default:
    return 0;
  }
}

int type_checker_is_numeric_type(Type *type) {
  return type_checker_is_integer_type(type) ||
         type_checker_is_floating_type(type);
}

// Type inference and promotion functions implementation

Type *type_checker_promote_types(TypeChecker *checker, Type *left, Type *right,
                                 const char *operator) {
  if (!checker || !left || !right || !operator)
    return NULL;

  // For comparison operators, result is always int32 (boolean represented as
  // int)
  if (strcmp(operator, "==") == 0 || strcmp(operator, "!=") == 0 ||
      strcmp(operator, "<") == 0 || strcmp(operator, "<=") == 0 ||
      strcmp(operator, ">") == 0 || strcmp(operator, ">=") == 0) {
    return checker->builtin_int32;
  }

  /* Character arithmetic promotes to int32, the way C promotes a char. `c -
   * 'a'` is an index and `c + 1` is the next code point; neither is a
   * character, and leaving them as one would print the answer as text.
   * Comparison is unaffected: it returned above, and `c == 'h'` still asks
   * whether two characters match. */
  if (left->kind == TYPE_CHAR) {
    left = checker->builtin_int32;
  }
  if (right->kind == TYPE_CHAR) {
    right = checker->builtin_int32;
  }

  // For arithmetic operators, promote to larger type
  if (strcmp(operator, "+") == 0 || strcmp(operator, "-") == 0 ||
      strcmp(operator, "*") == 0 || strcmp(operator, "/") == 0 ||
      strcmp(operator, "%") == 0) {

    // If either operand is floating-point, result is floating-point
    if (type_checker_is_floating_type(left) ||
        type_checker_is_floating_type(right)) {
      return type_checker_get_larger_type(checker, left, right);
    }

    // Both are integers, promote to larger integer type
    if (type_checker_is_integer_type(left) &&
        type_checker_is_integer_type(right)) {
      return type_checker_get_larger_type(checker, left, right);
    }
  }

  // For logical operators, result is int32 (boolean)
  if (strcmp(operator, "&&") == 0 || strcmp(operator, "||") == 0) {
    return checker->builtin_int32;
  }

  // Default: return left type
  return left;
}

Type *type_checker_get_larger_type(TypeChecker *checker, Type *type1,
                                   Type *type2) {
  if (!checker || !type1 || !type2)
    return NULL;

  int rank1 = type_checker_get_type_rank(type1);
  int rank2 = type_checker_get_type_rank(type2);

  // Return the type with higher rank
  return (rank1 >= rank2) ? type1 : type2;
}

int type_checker_get_type_rank(Type *type) {
  if (!type)
    return -1;

  // Type promotion ranking (higher number = higher rank)
  switch (type->kind) {
  case TYPE_INT8:
  case TYPE_UINT8:
  case TYPE_CHAR:
    return 1;
  case TYPE_INT16:
  case TYPE_UINT16:
    return 2;
  case TYPE_INT32:
  case TYPE_UINT32:
    return 3;
  case TYPE_FLOAT32:
    return 4;
  case TYPE_INT64:
  case TYPE_UINT64:
    return 5;
  case TYPE_FLOAT64:
    return 6;
  case TYPE_STRING:
    return 10; // Special case - strings don't promote with numbers
  default:
    return 0;
  }
}

// Type compatibility and conversion functions implementation

int type_checker_is_cast_valid(Type *from, Type *to) {
  if (!from || !to)
    return 0;

  /* Reflection types have no runtime representation, so they cannot be
   * cast to or from anything, including each other. */
  if (type_is_comptime_only(from) || type_is_comptime_only(to))
    return type_checker_types_equal(from, to);

  if (type_checker_types_equal(from, to))
    return 1;

  // Numeric <-> numeric
  if (type_checker_is_numeric_type(from) && type_checker_is_numeric_type(to))
    return 1;

  if ((from->kind == TYPE_ENUM && type_checker_is_integer_type(to)) ||
      (type_checker_is_integer_type(from) && to->kind == TYPE_ENUM)) {
    return 1;
  }

  // Pointer <-> pointer
  if (from->kind == TYPE_POINTER && to->kind == TYPE_POINTER)
    return 1;

  // Integer <-> pointer
  if ((type_checker_is_integer_type(from) && to->kind == TYPE_POINTER) ||
      (from->kind == TYPE_POINTER && type_checker_is_integer_type(to))) {
    return 1;
  }

  // Pointer <-> function pointer
  if ((from->kind == TYPE_POINTER && to->kind == TYPE_FUNCTION_POINTER) ||
      (from->kind == TYPE_FUNCTION_POINTER && to->kind == TYPE_POINTER)) {
    return 1;
  }

  // Integer <-> function pointer
  if ((type_checker_is_integer_type(from) &&
       to->kind == TYPE_FUNCTION_POINTER) ||
      (from->kind == TYPE_FUNCTION_POINTER &&
       type_checker_is_integer_type(to))) {
    return 1;
  }

  // Function pointer <-> function pointer
  if (from->kind == TYPE_FUNCTION_POINTER &&
      to->kind == TYPE_FUNCTION_POINTER) {
    return 1;
  }

  return 0;
}

// Type compatibility and conversion functions implementation

int type_checker_is_assignable(TypeChecker *checker, Type *dest_type,
                               Type *src_type) {
  if (type_is_comptime_only(dest_type) || type_is_comptime_only(src_type)) {
    return dest_type && src_type &&
           type_checker_types_equal(dest_type, src_type);
  }
  if (!checker || !dest_type || !src_type)
    return 0;

  /* A closure (function-pointer type carrying an environment) and a thin
   * function pointer are not interchangeable: a thin call site dispatches
   * without the environment, and a closure call site reads a code pointer the
   * thin value does not carry. Closures cross boundaries only as `Fn(...)->R`. */
  {
    int src_is_closure = src_type->kind == TYPE_FUNCTION_POINTER &&
                         src_type->closure_env;
    int dst_is_closure = dest_type->kind == TYPE_FUNCTION_POINTER &&
                         dest_type->closure_env;
    int src_is_thin_fn =
        src_type->kind == TYPE_FUNCTION_POINTER && !src_type->closure_env;
    int dst_is_thin_fn =
        dest_type->kind == TYPE_FUNCTION_POINTER && !dest_type->closure_env;
    if ((src_is_closure && dst_is_thin_fn) ||
        (dst_is_closure && src_is_thin_fn)) {
      return 0;
    }
  }

  if (type_checker_types_equal(dest_type, src_type)) {
    return 1;
  }

  /* A Mettle string can flow to a cstring by exposing its chars pointer. */
  if (type_checker_is_cstring_type(dest_type) &&
      src_type->kind == TYPE_STRING) {
    return 1;
  }

  /* A rawptr is an address with no element type, so it converts to and from
   * every pointer in both directions. That is the whole of the opaque-pointer
   * contract, and it is what lets `var a: int32* = malloc(n);` be written
   * without a cast and `free(a)` without pretending the bytes are characters.
   * An array decays to it the same way it decays to a typed pointer, and a
   * string's bytes are an address like any other -- every rawptr consumer
   * takes an explicit length, so no terminator is implied the way a cstring
   * implies one. */
  if (type_checker_is_rawptr_type(dest_type) &&
      (src_type->kind == TYPE_POINTER || src_type->kind == TYPE_ARRAY ||
       src_type->kind == TYPE_FUNCTION_POINTER ||
       src_type->kind == TYPE_STRING)) {
    return 1;
  }
  if (type_checker_is_rawptr_type(src_type) &&
      (dest_type->kind == TYPE_POINTER ||
       dest_type->kind == TYPE_FUNCTION_POINTER)) {
    return 1;
  }

  /* Allow int8* (e.g. from &array[0] for int8[]) to cstring (uint8*) for C interop */
  if (dest_type->kind == TYPE_POINTER && src_type->kind == TYPE_POINTER &&
      dest_type->name && strcmp(dest_type->name, "cstring") == 0 &&
      src_type->base_type && src_type->base_type->name &&
      strcmp(src_type->base_type->name, "int8") == 0) {
    return 1;
  }

  /* Allow array to pointer decay (T[N] to T*) for function arguments */
  if (dest_type->kind == TYPE_POINTER && src_type->kind == TYPE_ARRAY &&
      dest_type->base_type && src_type->base_type &&
      type_checker_types_equal(dest_type->base_type, src_type->base_type)) {
    return 1;
  }

  if (dest_type->kind == TYPE_POINTER || src_type->kind == TYPE_POINTER ||
      dest_type->kind == TYPE_ARRAY || src_type->kind == TYPE_ARRAY ||
      dest_type->kind == TYPE_STRUCT || src_type->kind == TYPE_STRUCT) {
    return 0;
  }

  // Check for safe implicit conversions
  return type_checker_is_implicitly_convertible(src_type, dest_type);
}

int type_checker_is_assignable_from(TypeChecker *checker, Type *dest_type,
                                    Type *src_type, ASTNode *src_expr) {
  long long folded = 0;

  if (type_checker_is_assignable(checker, dest_type, src_type)) {
    return 1;
  }
  /* Only the integer range rule has a constant escape: the destination is a
   * range, the source folds to one number, and containment is decided rather
   * than assumed. Everything else stays a type mismatch. */
  if (!src_expr || !checker || !dest_type || !src_type ||
      !type_checker_is_integer_type(dest_type) ||
      !type_checker_is_integer_type(src_type) ||
      dest_type->kind == TYPE_ENUM || src_type->kind == TYPE_ENUM) {
    return 0;
  }
  if (!type_checker_eval_integer_constant_with_checker(checker, src_expr,
                                                       &folded)) {
    return 0;
  }
  return type_checker_constant_fits_type(dest_type, src_type, folded);
}

int type_checker_integer_bounds(const Type *type, long long *out_min,
                                unsigned long long *out_max) {
  long long min = 0;
  unsigned long long max = 0;

  if (!type) {
    return 0;
  }
  switch (type->kind) {
  /* A bool holds 0 or 1, so it widens into every integer type. */
  case TYPE_BOOL:   min = 0;         max = 1ULL;       break;
  case TYPE_INT8:   min = INT8_MIN;  max = INT8_MAX;   break;
  case TYPE_INT16:  min = INT16_MIN; max = INT16_MAX;  break;
  case TYPE_INT32:  min = INT32_MIN; max = INT32_MAX;  break;
  case TYPE_INT64:  min = INT64_MIN; max = INT64_MAX;  break;
  case TYPE_UINT8:  min = 0;         max = UINT8_MAX;  break;
  case TYPE_CHAR:   min = 0;         max = UINT8_MAX;  break;
  case TYPE_UINT16: min = 0;         max = UINT16_MAX; break;
  case TYPE_UINT32: min = 0;         max = UINT32_MAX; break;
  case TYPE_UINT64: min = 0;         max = UINT64_MAX; break;
  default:
    return 0;
  }
  if (out_min) {
    *out_min = min;
  }
  if (out_max) {
    *out_max = max;
  }
  return 1;
}

int type_checker_int_conversion_is_value_preserving(const Type *from,
                                                    const Type *to) {
  long long from_min = 0, to_min = 0;
  unsigned long long from_max = 0, to_max = 0;

  if (!from || !to) {
    return 0;
  }
  if (!type_checker_integer_bounds(to, &to_min, &to_max)) {
    return 0;
  }

  /* An enum's value set is written down, so containment is decidable exactly
   * rather than approximated by width: the conversion is value-preserving when
   * every declared member fits. */
  if (from->kind == TYPE_ENUM) {
    if (from->enum_member_count == 0 || !from->enum_member_values) {
      return 0;
    }
    for (size_t i = 0; i < from->enum_member_count; i++) {
      long long value = from->enum_member_values[i];
      if (value < to_min) {
        return 0;
      }
      if (value >= 0 && (unsigned long long)value > to_max) {
        return 0;
      }
    }
    return 1;
  }

  if (!type_checker_integer_bounds(from, &from_min, &from_max)) {
    return 0;
  }
  return to_min <= from_min && to_max >= from_max;
}

int type_checker_constant_fits_type(const Type *dest_type, const Type *src_type,
                                    long long value) {
  long long dest_min = 0;
  unsigned long long dest_max = 0;

  if (!type_checker_integer_bounds(dest_type, &dest_min, &dest_max)) {
    return 0;
  }
  /* The folder carries every constant in a long long, so a value typed
   * unsigned above INT64_MAX arrives as a negative bit pattern. Read it back
   * with the signedness the source was given, not the container's. */
  if (src_type && (src_type->kind == TYPE_UINT8 ||
                   src_type->kind == TYPE_UINT16 ||
                   src_type->kind == TYPE_UINT32 ||
                   src_type->kind == TYPE_UINT64)) {
    return (unsigned long long)value <= dest_max;
  }
  if (value < 0) {
    return dest_min <= value;
  }
  return (unsigned long long)value <= dest_max;
}

int type_checker_is_implicitly_convertible(Type *from_type, Type *to_type) {
  if (!from_type || !to_type)
    return 0;

  // Same type is always convertible
  if (from_type->kind == to_type->kind) {
    return type_checker_types_equal(from_type, to_type);
  }

  /* Integer to integer: widen silently, narrow loudly. A conversion that can
   * change the value is written at the site, where a reader can see it; one
   * that cannot is not worth writing. Two destinations sit outside the rule
   * because they are not integer range conversions at all: `bool` is a truth
   * coercion (a comparison's result is an int32 that every `var b: bool = x >
   * y;` stores), and an enum names a set rather than a range. */
  if (type_checker_is_integer_type(from_type) &&
      type_checker_is_integer_type(to_type)) {
    if (to_type->kind == TYPE_BOOL) {
      return 1;
    }
    return type_checker_int_conversion_is_value_preserving(from_type, to_type);
  }

  // Integer to floating point conversions
  if (type_checker_is_integer_type(from_type) &&
      type_checker_is_floating_type(to_type)) {
    return 1; // Generally safe
  }

  // Floating point to floating point conversions, including narrowing.
  if (type_checker_is_floating_type(from_type) &&
      type_checker_is_floating_type(to_type)) {
    return 1;
  }

  // No other implicit conversions are allowed
  return 0;
}

int type_checker_are_compatible(Type *type1, Type *type2) {
  if (!type1 || !type2)
    return 0;

  if (type_checker_types_equal(type1, type2)) {
    return 1;
  }

  /* Comparison and match-arm unification, not assignment. The narrowing rule
   * governs where a value is stored; `i < len` stores nothing, so both sides
   * are read at their own width and every integer stays comparable with every
   * other. */
  if (type_checker_is_integer_type(type1) &&
      type_checker_is_integer_type(type2)) {
    return 1;
  }

  if (type1->kind == TYPE_POINTER || type2->kind == TYPE_POINTER ||
      type1->kind == TYPE_ARRAY || type2->kind == TYPE_ARRAY ||
      type1->kind == TYPE_STRUCT || type2->kind == TYPE_STRUCT) {
    return 0;
  }

  // Check for implicit numeric conversions
  return type_checker_is_implicitly_convertible(type1, type2) ||
         type_checker_is_implicitly_convertible(type2, type1);
}

Type *type_checker_default_integer_literal_type(TypeChecker *checker,
                                                     NumberLiteral *literal) {
  if (!checker || !literal || literal->is_float) {
    return checker ? checker->builtin_int32 : NULL;
  }

  unsigned long long u_bitpat = (unsigned long long)literal->int_value;
  unsigned char radix = literal->int_radix;
  if (radix != 2u && radix != 16u) {
    radix = 10u;
  }

  /*
   * Decimal defaults follow signed widening so large magnitudes usable with
   * unary minus (-2147483648 via -(int64)...). Hex/binary infer uint32 in the
   * (INT32_MAX, UINT32_MAX] range so 0xFFFFFFFF and similar stay uint32-ish.
   */
  if (radix == 10u) {
    /* A literal is never negative in source -- a leading '-' lexes as unary
     * minus -- so a negative bit pattern here is a decimal past LLONG_MAX that
     * the parser re-read unsigned. It is a uint64, and typing it int32 by its
     * bit pattern (18446744073709551615 reading as -1) is how it used to reach
     * codegen as the right bits for the wrong reason. */
    if (literal->int_value < 0) {
      return checker->builtin_uint64;
    }
    if (literal->int_value >= INT32_MIN && literal->int_value <= INT32_MAX) {
      return checker->builtin_int32;
    }
    if (u_bitpat <= (unsigned long long)INT64_MAX) {
      return checker->builtin_int64;
    }
    return checker->builtin_uint64;
  }

  if (u_bitpat <= (unsigned long long)INT32_MAX) {
    return checker->builtin_int32;
  }
  if (u_bitpat <= UINT32_MAX) {
    return checker->builtin_uint32;
  }
  if (u_bitpat <= (unsigned long long)INT64_MAX) {
    return checker->builtin_int64;
  }
  return checker->builtin_uint64;
}

Type *type_checker_canon_type(TypeChecker *checker, Type *type) {
  if (!checker || !type) {
    return type;
  }
  if (type->type_table_index != UINT32_MAX) {
    return type;
  }
  for (size_t i = 0; i < checker->type_table_count; i++) {
    Type *existing = checker->type_table[i];
    if (!existing || existing == type ||
        !type_checker_types_equal(existing, type)) {
      continue;
    }
    /* cstring and uint8* are both pointer-to-uint8 but are distinct types. */
    if (existing->name && type->name &&
        strcmp(existing->name, type->name) != 0) {
      continue;
    }
    type_destroy(type);
    return existing;
  }
  if (type_checker_intern_type(checker, type) == UINT32_MAX) {
    return type;
  }
  return type;
}

uint32_t type_checker_intern_type(TypeChecker *checker, Type *type) {
  if (!checker || !type) {
    return UINT32_MAX;
  }
  if (type->type_table_index != UINT32_MAX) {
    return type->type_table_index;
  }
  if (checker->type_table_count == checker->type_table_capacity) {
    size_t next = checker->type_table_capacity
                      ? checker->type_table_capacity * 2
                      : 32;
    Type **grown = realloc(checker->type_table, next * sizeof(Type *));
    if (!grown) {
      return UINT32_MAX;
    }
    checker->type_table = grown;
    checker->type_table_capacity = next;
  }
  if (checker->type_table_count > UINT32_MAX) {
    return UINT32_MAX;
  }
  uint32_t index = (uint32_t)checker->type_table_count;
  checker->type_table[checker->type_table_count++] = type;
  type->type_table_index = index;
  return index;
}

Type *type_checker_type_from_index(const TypeChecker *checker, uint32_t index) {
  if (!checker || index == UINT32_MAX ||
      (size_t)index >= checker->type_table_count) {
    return NULL;
  }
  return checker->type_table[index];
}

/* A type graph has cycles: `struct ArenaChunk { next: ArenaChunk* }` reaches
 * itself through its own field. Aggregates are recorded on the way down so a
 * cycle is walked once rather than forever. Only aggregates need recording --
 * pointer, array, slice and function types can only cycle by passing through
 * one. */
typedef struct {
  const Type **types;
  size_t count;
  size_t capacity;
} TypeVisitSet;

static int type_visit_set_enter(TypeVisitSet *seen, const Type *type) {
  for (size_t i = 0; i < seen->count; i++) {
    if (seen->types[i] == type) {
      return 0;
    }
  }
  if (seen->count == seen->capacity) {
    size_t next = seen->capacity ? seen->capacity * 2 : 16;
    const Type **grown = realloc(seen->types, next * sizeof(const Type *));
    if (!grown) {
      return 0; /* treat as already seen: stop descending rather than crash */
    }
    seen->types = grown;
    seen->capacity = next;
  }
  seen->types[seen->count++] = type;
  return 1;
}

static int type_contains_comptime_only_seen(const Type *type,
                                            TypeVisitSet *seen) {
  if (!type) {
    return 0;
  }
  if (type_is_comptime_only(type)) {
    return 1;
  }
  if (type->kind == TYPE_POINTER || type->kind == TYPE_ARRAY ||
      type->kind == TYPE_SLICE) {
    return type_contains_comptime_only_seen(type->base_type, seen);
  }
  if (type->kind == TYPE_FUNCTION_POINTER) {
    if (type_contains_comptime_only_seen(type->fn_return_type, seen)) {
      return 1;
    }
    for (size_t i = 0; i < type->fn_param_count; i++) {
      if (type_contains_comptime_only_seen(type->fn_param_types[i], seen)) {
        return 1;
      }
    }
    return 0;
  }
  if (type->kind == TYPE_STRUCT) {
    if (!type_visit_set_enter(seen, type)) {
      return 0;
    }
    for (size_t i = 0; i < type->field_count; i++) {
      if (type_contains_comptime_only_seen(type->field_types[i], seen)) {
        return 1;
      }
    }
  }
  if (type->kind == TYPE_TAGGED_ENUM) {
    if (!type_visit_set_enter(seen, type)) {
      return 0;
    }
    for (size_t i = 0; i < type->tagged_variant_count; i++) {
      if (type_contains_comptime_only_seen(type->tagged_variant_payloads[i],
                                           seen)) {
        return 1;
      }
    }
  }
  return 0;
}

int type_contains_comptime_only(const Type *type) {
  TypeVisitSet seen = {NULL, 0, 0};
  int result = type_contains_comptime_only_seen(type, &seen);
  free(seen.types);
  return result;
}

Type *type_checker_type_value(TypeChecker *checker, Type *referred,
                              ASTNode *expression) {
  if (!checker || !referred || !checker->builtin_type) {
    return NULL;
  }
  uint32_t index = type_checker_intern_type(checker, referred);
  if (index == UINT32_MAX) {
    if (expression) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Out of memory while interning type '%s'",
          referred->name ? referred->name : "<anonymous>");
    }
    return NULL;
  }
  return checker->builtin_type;
}

Type *type_checker_field_value(TypeChecker *checker, Type *owner,
                               uint32_t field_index, ASTNode *expression) {
  if (!checker || !owner || !checker->builtin_field) {
    return NULL;
  }
  uint32_t index = type_checker_intern_type(checker, owner);
  if (index == UINT32_MAX) {
    if (expression) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Out of memory while interning type '%s'",
          owner->name ? owner->name : "<anonymous>");
    }
    return NULL;
  }
  (void)field_index;
  return checker->builtin_field;
}

/* True for the decimal literal `9223372036854775808`, the magnitude of int64's
 * minimum. A literal is never negative in source, so that number alone is past
 * LLONG_MAX and types uint64; negating it was then refused against a range the
 * diagnostic itself printed as containing the answer, which made int64's
 * minimum the one value nobody could write. Under a unary minus the number is
 * an int64 and the minus belongs to it. */
int type_checker_is_int64_min_magnitude(const ASTNode *operand) {
  const NumberLiteral *literal;
  if (!operand || operand->type != AST_NUMBER_LITERAL) {
    return 0;
  }
  literal = (const NumberLiteral *)operand->data;
  return literal && !literal->is_float && !literal->is_char &&
         literal->int_radix == 10 &&
         (unsigned long long)literal->int_value ==
             (unsigned long long)INT64_MAX + 1ull;
}
