// Type checker: type construction, builtins, numeric promotion, conversions.
#include "type_checker_internal.h"
#include "../string_intern.h"

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

  errno = 0;
  char *end_ptr = NULL;
  unsigned long long array_size_ull = strtoull(size_start, &end_ptr, 10);
  if (errno != 0 || !end_ptr || end_ptr != rbracket || array_size_ull == 0 ||
      array_size_ull > SIZE_MAX) {
    return NULL;
  }

  size_t array_size = (size_t)array_size_ull;
  if (base_type->size > 0 && array_size > SIZE_MAX / base_type->size) {
    return NULL;
  }

  Type *array_type = type_create(TYPE_ARRAY, name);
  if (!array_type) {
    return NULL;
  }

  array_type->base_type = base_type;
  array_type->array_size = array_size;
  array_type->size = base_type->size * array_size;
  array_type->alignment = base_type->alignment;

  return array_type;
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
    pointer_type->size = 8;
    pointer_type->alignment = 8;
    current = pointer_type;
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
    return type_checker_types_equal(lhs->base_type, rhs->base_type);
  case TYPE_ARRAY:
    return lhs->array_size == rhs->array_size &&
           type_checker_types_equal(lhs->base_type, rhs->base_type);
  case TYPE_STRUCT:
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

  // Create built-in floating-point types
  checker->builtin_float32 = type_create(TYPE_FLOAT32, "float32");
  checker->builtin_float64 = type_create(TYPE_FLOAT64, "float64");

  // C interop alias: cstring -> uint8*
  checker->builtin_cstring = type_create(TYPE_POINTER, "cstring");
  if (checker->builtin_cstring) {
    checker->builtin_cstring->base_type = checker->builtin_uint8;
    checker->builtin_cstring->size = 8;
    checker->builtin_cstring->alignment = 8;
  }

  // Create built-in string type backed by a uint8* and length
  checker->builtin_string = type_create(TYPE_STRING, "string");
  if (checker->builtin_string) {
    checker->builtin_string->size = 16;
    checker->builtin_string->alignment = 8;

    checker->builtin_string->field_count = 2;
    checker->builtin_string->field_names = malloc(2 * sizeof(char *));
    checker->builtin_string->field_types = malloc(2 * sizeof(Type *));
    checker->builtin_string->field_offsets = malloc(2 * sizeof(size_t));

    checker->builtin_string->field_names[0] = strdup("chars");
    checker->builtin_string->field_types[0] =
        type_create(TYPE_POINTER, "uint8*");
    checker->builtin_string->field_types[0]->base_type = checker->builtin_uint8;
    checker->builtin_string->field_types[0]->size = 8;
    checker->builtin_string->field_types[0]->alignment = 8;
    checker->builtin_string->field_offsets[0] = 0;

    checker->builtin_string->field_names[1] = strdup("length");
    checker->builtin_string->field_types[1] = checker->builtin_uint64;
    checker->builtin_string->field_offsets[1] = 8;
  }

  // Create built-in void type
  checker->builtin_void = type_create(TYPE_VOID, "void");
  if (checker->builtin_void) {
    checker->builtin_void->size = 0;
    checker->builtin_void->alignment = 1;
  }

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
  if (strcmp(name, "void") == 0)
    return checker->builtin_void;

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
      // Extract the single type argument (first one, up to ',' or '>')
      const char *arg_start = lt + 1;
      const char *arg_end = strchr(arg_start, ',');
      if (!arg_end)
        arg_end = name + strlen(name) - 1; // points to '>'
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
  case TYPE_ENUM:
    return 1;
  default:
    return 0;
  }
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

  if (type_checker_types_equal(from, to))
    return 1;

  // Numeric ↔ numeric
  if (type_checker_is_numeric_type(from) && type_checker_is_numeric_type(to))
    return 1;

  // Pointer ↔ pointer
  if (from->kind == TYPE_POINTER && to->kind == TYPE_POINTER)
    return 1;

  // Integer ↔ pointer
  if ((type_checker_is_integer_type(from) && to->kind == TYPE_POINTER) ||
      (from->kind == TYPE_POINTER && type_checker_is_integer_type(to))) {
    return 1;
  }

  // Pointer ↔ function pointer
  if ((from->kind == TYPE_POINTER && to->kind == TYPE_FUNCTION_POINTER) ||
      (from->kind == TYPE_FUNCTION_POINTER && to->kind == TYPE_POINTER)) {
    return 1;
  }

  // Integer ↔ function pointer
  if ((type_checker_is_integer_type(from) &&
       to->kind == TYPE_FUNCTION_POINTER) ||
      (from->kind == TYPE_FUNCTION_POINTER &&
       type_checker_is_integer_type(to))) {
    return 1;
  }

  // Function pointer ↔ function pointer
  if (from->kind == TYPE_FUNCTION_POINTER &&
      to->kind == TYPE_FUNCTION_POINTER) {
    return 1;
  }

  return 0;
}

// Type compatibility and conversion functions implementation

int type_checker_is_assignable(TypeChecker *checker, Type *dest_type,
                               Type *src_type) {
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

int type_checker_is_implicitly_convertible(Type *from_type, Type *to_type) {
  if (!from_type || !to_type)
    return 0;

  // Same type is always convertible
  if (from_type->kind == to_type->kind) {
    return type_checker_types_equal(from_type, to_type);
  }

  // Integer to integer conversions, including narrowing.
  if (type_checker_is_integer_type(from_type) &&
      type_checker_is_integer_type(to_type)) {
    return 1;
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
   * unary minus (-2147483648 via -(int64)…). Hex/binary infer uint32 in the
   * (INT32_MAX, UINT32_MAX] range so 0xFFFFFFFF and similar stay uint32-ish.
   */
  if (radix == 10u) {
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
