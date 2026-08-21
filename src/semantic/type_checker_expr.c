// Type checker: expression type inference and checking.
#include "type_checker_internal.h"
#include "monomorphize.h"
#include "string_intern.h"

/* `p->m()` and `(*p).m()` both parse to a method call whose object is a deref of
 * a pointer-to-struct. That spelling selects the lifted method that takes the
 * receiver as a pointer, so the call passes `p` itself and the method can write
 * through it. Returns the pointer expression, or NULL for a value receiver. */
static ASTNode *type_checker_pointer_receiver(TypeChecker *checker,
                                              ASTNode *object) {
  UnaryExpression *unary = NULL;
  Type *inner_type = NULL;

  if (!object || object->type != AST_UNARY_EXPRESSION) {
    return NULL;
  }
  unary = (UnaryExpression *)object->data;
  if (!unary || !unary->operator|| strcmp(unary->operator, "*") != 0 ||
      !unary->operand) {
    return NULL;
  }
  inner_type = type_checker_infer_type(checker, unary->operand);
  if (!inner_type || inner_type->kind != TYPE_POINTER || !inner_type->base_type ||
      inner_type->base_type->kind != TYPE_STRUCT) {
    return NULL;
  }
  return unary->operand;
}

Type *type_checker_method_receiver_struct_type(Type *receiver_type) {
  if (!receiver_type) {
    return NULL;
  }
  if (receiver_type->kind == TYPE_STRUCT) {
    return receiver_type;
  }
  if (receiver_type->kind == TYPE_POINTER && receiver_type->base_type &&
      receiver_type->base_type->kind == TYPE_STRUCT) {
    return receiver_type->base_type;
  }
  return NULL;
}

int type_checker_desugar_struct_method_call(TypeChecker *checker,
                                                   ASTNode *expression,
                                                   CallExpression *call) {
  Type *receiver_type = NULL;
  Type *struct_type = NULL;
  ASTNode *pointer_receiver = NULL;
  char *mangled_name = NULL;
  ASTNode **new_args = NULL;
  size_t name_len = 0;

  if (!checker || !expression || !call || !call->object ||
      !call->function_name) {
    return 1;
  }

  receiver_type = type_checker_infer_type(checker, call->object);
  struct_type = type_checker_method_receiver_struct_type(receiver_type);
  if (!struct_type || !struct_type->name) {
    const char *receiver_name =
        (receiver_type && receiver_type->name) ? receiver_type->name : "unknown";
    type_checker_set_error_at_location(
        checker, expression->location,
        "Method call receiver must be a struct or pointer-to-struct, got '%s'",
        receiver_name);
    return 0;
  }

  /* A pointer receiver resolves to the pointer-taking form of the method, so
   * that writes to `this` reach the caller's struct. Falling back to the value
   * form keeps a hand-written `S_m(s: S)` free function callable as `p->m()`. */
  pointer_receiver = type_checker_pointer_receiver(checker, call->object);
  if (pointer_receiver) {
    name_len = strlen(struct_type->name) + 1 + strlen(call->function_name) +
               strlen(MONO_PTR_RECEIVER_SUFFIX) + 1;
    mangled_name = malloc(name_len);
    if (!mangled_name) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Out of memory while resolving struct method call");
      return 0;
    }
    snprintf(mangled_name, name_len, "%s_%s%s", struct_type->name,
             call->function_name, MONO_PTR_RECEIVER_SUFFIX);
    if (symbol_table_lookup(checker->symbol_table, mangled_name)) {
      /* Drop the deref that the parser wrapped around the pointer, leaving the
       * pointer itself as the receiver. The deref owned it as a child, so the
       * call node adopts it before the now-empty deref is freed. */
      ASTNode *deref = call->object;
      UnaryExpression *unary = (UnaryExpression *)deref->data;
      for (size_t i = 0; i < expression->child_count; i++) {
        if (expression->children[i] == deref) {
          expression->children[i] = pointer_receiver;
          break;
        }
      }
      unary->operand = NULL;
      deref->child_count = 0;
      ast_destroy_node(deref);
      call->object = pointer_receiver;
    } else {
      free(mangled_name);
      mangled_name = NULL;
      pointer_receiver = NULL;
    }
  }

  if (!mangled_name) {
    name_len = strlen(struct_type->name) + 1 + strlen(call->function_name) + 1;
    mangled_name = malloc(name_len);
    if (!mangled_name) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Out of memory while resolving struct method call");
      return 0;
    }
    snprintf(mangled_name, name_len, "%s_%s", struct_type->name,
             call->function_name);
  }

  if (!symbol_table_lookup(checker->symbol_table, mangled_name)) {
    /* No method by this name. If the receiver struct has a function-pointer or
     * closure FIELD of this name, `obj.field(args)` is a call THROUGH that
     * field: rewrite the node into a function-pointer call on `obj.field`,
     * which handles both thin pointers and closures (the call site loads the
     * code pointer and, for a closure, threads the environment). */
    Type *field_type = type_get_field_type(struct_type, call->function_name);
    if (field_type && field_type->kind == TYPE_FUNCTION_POINTER) {
      free(mangled_name);
      ASTNode *obj = call->object;
      ASTNode **args = call->arguments;
      size_t argc = call->argument_count;
      ASTNode *member = ast_create_member_access(obj, call->function_name,
                                                 expression->location);
      if (!member) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Out of memory while resolving field call");
        return 0;
      }
      FuncPtrCall *fp = malloc(sizeof(FuncPtrCall));
      if (!fp) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Out of memory while resolving field call");
        return 0;
      }
      fp->function = member;
      fp->arguments = args;
      fp->argument_count = argc;
      /* The argument array is reused; `obj` now belongs to `member`. The old
       * CallExpression payload is intentionally left unfreed - a small bounded
       * compile-time allocation - to avoid any ownership mismatch. */
      expression->child_count = 0;
      expression->type = AST_FUNC_PTR_CALL;
      expression->data = fp;
      expression->resolved_type = NULL;
      ast_add_child(expression, member);
      for (size_t i = 0; i < argc; i++) {
        if (args[i]) {
          ast_add_child(expression, args[i]);
        }
      }
      return 1;
    }

    type_checker_set_error_at_location(
        checker, expression->location,
        "Undefined method '%s.%s' (expected function '%s')",
        struct_type->name, call->function_name, mangled_name);
    free(mangled_name);
    return 0;
  }

  new_args = malloc((call->argument_count + 1) * sizeof(ASTNode *));
  if (!new_args) {
    free(mangled_name);
    type_checker_set_error_at_location(
        checker, expression->location,
        "Out of memory while rewriting struct method call");
    return 0;
  }

  new_args[0] = call->object;
  for (size_t i = 0; i < call->argument_count; i++) {
    new_args[i + 1] = call->arguments[i];
  }
  free(call->arguments);
  call->arguments = new_args;
  call->argument_count++;
  call->object = NULL;

  mettle_free_string(call->function_name);
  call->function_name = mangled_name;
  return 1;
}

/* Parser-recognized thread/block index member access. The parser preserves a
 * marker so a host function explicitly declaring an extern named gpu_tid_x is
 * not accidentally captured as language syntax. IR lowering maps the neutral
 * semantic alias to MtlcIntrinsic; no backend opcode enters this layer. */
static Type *type_checker_gpu_index_builtin(TypeChecker *checker,
                                            ASTNode *expression,
                                            CallExpression *call,
                                            int *handled) {
  *handled = 0;
  if (!call || !call->is_gpu_index) return NULL;
  *handled = 1;
  FunctionDeclaration *owner =
      checker->current_function_decl &&
              checker->current_function_decl->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)checker->current_function_decl->data
          : NULL;
  if (!owner || !owner->is_kernel) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "GPU index built-ins are only legal directly inside a GPU kernel");
    return NULL;
  }
  if (call->argument_count != 0) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "GPU index built-ins take no arguments");
    return NULL;
  }
  return checker->builtin_int32;
}

/* Reference-frontend syntax for the target-neutral subgroup intrinsic family.
 * The selected IR identity is type-specific, but no PTX/SPIR-V concept enters
 * semantic analysis. Other frontends construct the same identities directly. */
static Type *type_checker_subgroup_builtin(TypeChecker *checker,
                                           ASTNode *expression,
                                           CallExpression *call,
                                           int *handled) {
  const char *selected = NULL;
  Type *value_type = NULL;
  *handled = 0;
  if (!call || !call->function_name || call->object) return NULL;
  int is_local_id = strcmp(call->function_name, "subgroup_local_id") == 0;
  int is_size = strcmp(call->function_name, "subgroup_size") == 0;
  int is_broadcast = strcmp(call->function_name, "subgroup_broadcast") == 0;
  int is_shuffle = strcmp(call->function_name, "subgroup_shuffle") == 0;
  int is_ballot = strcmp(call->function_name, "subgroup_ballot") == 0;
  int is_any = strcmp(call->function_name, "subgroup_any") == 0;
  int is_all = strcmp(call->function_name, "subgroup_all") == 0;
  int is_reduce_add =
      strcmp(call->function_name, "subgroup_reduce_add") == 0;
  int is_reduce_min =
      strcmp(call->function_name, "subgroup_reduce_min") == 0;
  int is_reduce_max =
      strcmp(call->function_name, "subgroup_reduce_max") == 0;
  int is_scan_inclusive =
      strcmp(call->function_name, "subgroup_scan_inclusive_add") == 0;
  int is_scan_exclusive =
      strcmp(call->function_name, "subgroup_scan_exclusive_add") == 0;
  if (!is_local_id && !is_size && !is_broadcast && !is_shuffle &&
      !is_ballot && !is_any && !is_all && !is_reduce_add &&
      !is_reduce_min && !is_reduce_max && !is_scan_inclusive &&
      !is_scan_exclusive) {
    return NULL;
  }
  *handled = 1;

  FunctionDeclaration *owner =
      checker->current_function_decl &&
              checker->current_function_decl->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)checker->current_function_decl->data
          : NULL;
  if (!owner || !owner->is_kernel) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Subgroup built-ins are only legal directly inside a GPU kernel");
    return NULL;
  }
  size_t expected = (is_local_id || is_size) ? 0
                    : (is_broadcast || is_shuffle || is_ballot) ? 2
                                                                  : 1;
  if (call->argument_count != expected) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Subgroup built-in '%s' expects %llu arguments, got %llu",
        call->function_name, (unsigned long long)expected,
        (unsigned long long)call->argument_count);
    return NULL;
  }
  if (is_local_id || is_size) return checker->builtin_uint32;

  if (is_ballot || is_any || is_all) {
    Type *predicate_type =
        type_checker_infer_type(checker, call->arguments[0]);
    if (!predicate_type) return NULL;
    if (predicate_type != checker->builtin_bool) {
      type_checker_set_error_at_location(
          checker, call->arguments[0]->location,
          "Subgroup '%s' predicate must be bool, got '%s'",
          is_ballot ? "ballot" : is_any ? "any" : "all",
          predicate_type->name ? predicate_type->name : "unknown");
      return NULL;
    }
    if (is_ballot) {
      Type *word_type =
          type_checker_infer_type(checker, call->arguments[1]);
      if (!word_type) return NULL;
      if (!type_checker_is_integer_type(word_type)) {
        type_checker_set_error_at_location(
            checker, call->arguments[1]->location,
            "Subgroup ballot word index must be an integer, got '%s'",
            word_type->name ? word_type->name : "unknown");
        return NULL;
      }
      selected = "subgroup_ballot_word";
    } else {
      selected = is_any ? "subgroup_any" : "subgroup_all";
    }
    char *replacement = strdup(selected);
    if (!replacement) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Out of memory resolving subgroup built-in");
      return NULL;
    }
    mettle_free_string(call->function_name);
    call->function_name = replacement;
    return is_ballot ? checker->builtin_uint32 : checker->builtin_bool;
  }

  value_type = type_checker_infer_type(checker, call->arguments[0]);
  if (!value_type) return NULL;
  if (value_type != checker->builtin_uint32 &&
      value_type != checker->builtin_float32) {
    const char *operation = is_broadcast          ? "broadcast"
                            : is_shuffle          ? "shuffle"
                            : is_reduce_add        ? "reduce_add"
                            : is_reduce_min        ? "reduce_min"
                            : is_reduce_max        ? "reduce_max"
                            : is_scan_inclusive    ? "scan_inclusive_add"
                                                   : "scan_exclusive_add";
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "Subgroup '%s' value must be uint32 or float32, got '%s'",
        operation,
        value_type->name ? value_type->name : "unknown");
    return NULL;
  }
  if (is_broadcast || is_shuffle) {
    Type *lane_type = type_checker_infer_type(checker, call->arguments[1]);
    if (!lane_type) return NULL;
    if (!type_checker_is_integer_type(lane_type)) {
      type_checker_set_error_at_location(
          checker, call->arguments[1]->location,
          "Subgroup %s source lane must be an integer, got '%s'",
          is_shuffle ? "shuffle" : "broadcast",
          lane_type->name ? lane_type->name : "unknown");
      return NULL;
    }
  }

  if (is_broadcast) {
    selected = value_type == checker->builtin_float32
                   ? "subgroup_broadcast_f32"
                   : "subgroup_broadcast_u32";
  } else if (is_shuffle) {
    selected = value_type == checker->builtin_float32
                   ? "subgroup_shuffle_f32"
                   : "subgroup_shuffle_u32";
  } else if (is_reduce_add) {
    selected = value_type == checker->builtin_float32
                   ? "subgroup_reduce_add_f32"
                   : "subgroup_reduce_add_u32";
  } else if (is_reduce_min) {
    selected = value_type == checker->builtin_float32
                   ? "subgroup_reduce_min_f32"
                   : "subgroup_reduce_min_u32";
  } else if (is_reduce_max) {
    selected = value_type == checker->builtin_float32
                   ? "subgroup_reduce_max_f32"
                   : "subgroup_reduce_max_u32";
  } else if (is_scan_inclusive) {
    selected = value_type == checker->builtin_float32
                   ? "subgroup_scan_inclusive_add_f32"
                   : "subgroup_scan_inclusive_add_u32";
  } else {
    selected = value_type == checker->builtin_float32
                   ? "subgroup_scan_exclusive_add_f32"
                   : "subgroup_scan_exclusive_add_u32";
  }
  char *replacement = strdup(selected);
  if (!replacement) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Out of memory resolving subgroup built-in");
    return NULL;
  }
  mettle_free_string(call->function_name);
  call->function_name = replacement;
  return value_type;
}

const char *type_checker_tensor_option_identifier(ASTNode *node) {
  if (!node || node->type != AST_IDENTIFIER || !node->data) return NULL;
  return ((Identifier *)node->data)->name;
}

static MtlcMemoryOrder type_checker_atomic_order_name(const char *name) {
  if (!name) return MTLC_MEMORY_ORDER_DEFAULT;
  if (!strcmp(name, "relaxed")) return MTLC_MEMORY_ORDER_RELAXED;
  if (!strcmp(name, "acquire")) return MTLC_MEMORY_ORDER_ACQUIRE;
  if (!strcmp(name, "release")) return MTLC_MEMORY_ORDER_RELEASE;
  if (!strcmp(name, "acq_rel")) return MTLC_MEMORY_ORDER_ACQ_REL;
  if (!strcmp(name, "seq_cst")) return MTLC_MEMORY_ORDER_SEQ_CST;
  return MTLC_MEMORY_ORDER_DEFAULT;
}

static MtlcMemoryScope type_checker_atomic_scope_name(const char *name) {
  if (!name) return MTLC_MEMORY_SCOPE_DEFAULT;
  if (!strcmp(name, "work_item")) return MTLC_MEMORY_SCOPE_WORK_ITEM;
  if (!strcmp(name, "subgroup")) return MTLC_MEMORY_SCOPE_SUBGROUP;
  if (!strcmp(name, "workgroup")) return MTLC_MEMORY_SCOPE_WORKGROUP;
  if (!strcmp(name, "device")) return MTLC_MEMORY_SCOPE_DEVICE;
  if (!strcmp(name, "system")) return MTLC_MEMORY_SCOPE_SYSTEM;
  return MTLC_MEMORY_SCOPE_DEFAULT;
}

static MtlcAddressSpace type_checker_atomic_space_name(const char *name) {
  if (name && !strcmp(name, "global")) return MTLC_ADDRESS_SPACE_GLOBAL;
  if (name && !strcmp(name, "workgroup"))
    return MTLC_ADDRESS_SPACE_WORKGROUP;
  return MTLC_ADDRESS_SPACE_DEFAULT;
}

static int type_checker_atomic_failure_valid(MtlcMemoryOrder success,
                                             MtlcMemoryOrder failure) {
  if (failure != MTLC_MEMORY_ORDER_RELAXED &&
      failure != MTLC_MEMORY_ORDER_ACQUIRE &&
      failure != MTLC_MEMORY_ORDER_SEQ_CST)
    return 0;
  switch (success) {
  case MTLC_MEMORY_ORDER_RELAXED:
    return failure == MTLC_MEMORY_ORDER_RELAXED;
  case MTLC_MEMORY_ORDER_ACQUIRE:
  case MTLC_MEMORY_ORDER_ACQ_REL:
    return failure == MTLC_MEMORY_ORDER_RELAXED ||
           failure == MTLC_MEMORY_ORDER_ACQUIRE;
  case MTLC_MEMORY_ORDER_RELEASE:
    return failure == MTLC_MEMORY_ORDER_RELAXED;
  case MTLC_MEMORY_ORDER_SEQ_CST:
    return 1;
  default:
    return 0;
  }
}

/* Native reference-frontend surface for the neutral atomic load/store/RMW/CAS
 * family.
 * The syntax is type-directed (uint32/uint64), while the AST records the exact
 * address-space/order/scope contract consumed by neutral IR lowering. */
static Type *type_checker_atomic_builtin(TypeChecker *checker,
                                         ASTNode *expression,
                                         CallExpression *call,
                                         int *handled) {
  const char *stem = NULL;
  size_t positional_count = 3;
  int is_compare_exchange = 0;
  int is_load = 0, is_store = 0;
  *handled = 0;
  if (!call || !call->function_name || call->object) return NULL;
  if (!strcmp(call->function_name, "atomic_fetch_add")) stem = "atomic_add";
  else if (!strcmp(call->function_name, "atomic_fetch_sub")) stem = "atomic_sub";
  else if (!strcmp(call->function_name, "atomic_fetch_min")) stem = "atomic_min";
  else if (!strcmp(call->function_name, "atomic_fetch_max")) stem = "atomic_max";
  else if (!strcmp(call->function_name, "atomic_fetch_and")) stem = "atomic_and";
  else if (!strcmp(call->function_name, "atomic_fetch_or")) stem = "atomic_or";
  else if (!strcmp(call->function_name, "atomic_fetch_xor")) stem = "atomic_xor";
  else if (!strcmp(call->function_name, "atomic_load")) {
    stem = "atomic_load";
    positional_count = 2;
    is_load = 1;
  }
  else if (!strcmp(call->function_name, "atomic_store")) {
    stem = "atomic_store";
    positional_count = 3;
    is_store = 1;
  }
  else if (!strcmp(call->function_name, "atomic_exchange")) stem = "atomic_exchange";
  else if (!strcmp(call->function_name, "atomic_compare_exchange")) {
    stem = "atomic_compare_exchange";
    positional_count = 4;
    is_compare_exchange = 1;
  } else {
    return NULL;
  }
  *handled = 1;

  FunctionDeclaration *owner =
      checker->current_function_decl &&
              checker->current_function_decl->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)checker->current_function_decl->data
          : NULL;
  if (!owner || !owner->is_kernel) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Atomic GPU built-ins are only legal directly inside a GPU kernel");
    return NULL;
  }
  if (call->argument_count < positional_count) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Atomic built-in '%s' expects %llu positional operands before named options",
        call->function_name, (unsigned long long)positional_count);
    return NULL;
  }
  for (size_t i = 0; i < positional_count; i++) {
    if (call->argument_names && call->argument_names[i]) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "The first %llu atomic operands are positional",
          (unsigned long long)positional_count);
      return NULL;
    }
  }

  Type *buffer_type =
      type_checker_infer_type(checker, call->arguments[0]);
  Type *value_type =
      buffer_type &&
              (buffer_type->kind == TYPE_POINTER ||
               buffer_type->kind == TYPE_ARRAY)
          ? buffer_type->base_type
          : NULL;
  if (!value_type || (value_type != checker->builtin_uint32 &&
                      value_type != checker->builtin_uint64)) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "Atomic storage must be a uint32* or uint64* (or matching workgroup array)");
    return NULL;
  }
  Type *index_type =
      type_checker_infer_type(checker, call->arguments[1]);
  if (!index_type || !type_checker_is_integer_type(index_type)) {
    type_checker_set_error_at_location(
        checker, call->arguments[1]->location,
        "Atomic element index must have integer type");
    return NULL;
  }
  for (size_t i = 2; i < positional_count; i++) {
    Type *operand_type =
        type_checker_infer_type(checker, call->arguments[i]);
    int compatible = operand_type == value_type;
    if (!compatible && call->arguments[i]->type == AST_NUMBER_LITERAL) {
      NumberLiteral *literal = (NumberLiteral *)call->arguments[i]->data;
      compatible = literal && !literal->is_float && literal->int_value >= 0 &&
                   (value_type == checker->builtin_uint64 ||
                    (unsigned long long)literal->int_value <= UINT32_MAX);
    }
    if (!compatible) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "Atomic value operand %llu must match storage element type '%s'",
          (unsigned long long)(i - 1),
          value_type->name ? value_type->name : "unsigned integer");
      return NULL;
    }
  }

  MtlcAddressSpace inferred_space = MTLC_ADDRESS_SPACE_GLOBAL;
  int fixed_space = 0;
  if (call->arguments[0]->type == AST_IDENTIFIER &&
      call->arguments[0]->data) {
    Identifier *identifier = (Identifier *)call->arguments[0]->data;
    Symbol *symbol = type_checker_resolve_identifier(checker, identifier);
    if (symbol && symbol->address_space != MTLC_ADDRESS_SPACE_DEFAULT) {
      inferred_space = symbol->address_space;
      fixed_space = symbol->is_address_space_binding ||
                    symbol->address_space == MTLC_ADDRESS_SPACE_GLOBAL;
    }
  }
  if (inferred_space != MTLC_ADDRESS_SPACE_GLOBAL &&
      inferred_space != MTLC_ADDRESS_SPACE_WORKGROUP) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "Atomics require global or workgroup storage");
    return NULL;
  }

  MtlcAddressSpace address_space = inferred_space;
  MtlcMemoryOrder order = MTLC_MEMORY_ORDER_SEQ_CST;
  MtlcMemoryOrder failure_order = MTLC_MEMORY_ORDER_DEFAULT;
  MtlcMemoryScope scope = MTLC_MEMORY_SCOPE_DEFAULT;
  int have_failure_order = 0;
  for (size_t i = positional_count; i < call->argument_count; i++) {
    const char *option = call->argument_names ? call->argument_names[i] : NULL;
    const char *identifier =
        type_checker_tensor_option_identifier(call->arguments[i]);
    if (!option) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "Atomic configuration operands must be named");
      return NULL;
    }
    for (size_t prior = positional_count; prior < i; prior++) {
      if (call->argument_names[prior] &&
          !strcmp(call->argument_names[prior], option)) {
        type_checker_set_error_at_location(checker,
                                           call->arguments[i]->location,
                                           "Duplicate atomic option '%s'",
                                           option);
        return NULL;
      }
    }
    if (!strcmp(option, "order") || !strcmp(option, "success_order")) {
      order = type_checker_atomic_order_name(identifier);
      if (order == MTLC_MEMORY_ORDER_DEFAULT) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Atomic order must be relaxed, acquire, release, acq_rel, or seq_cst");
        return NULL;
      }
    } else if (!strcmp(option, "failure_order")) {
      failure_order = type_checker_atomic_order_name(identifier);
      have_failure_order = 1;
      if (failure_order == MTLC_MEMORY_ORDER_DEFAULT) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Atomic failure_order must be relaxed, acquire, or seq_cst");
        return NULL;
      }
    } else if (!strcmp(option, "scope")) {
      scope = type_checker_atomic_scope_name(identifier);
      if (scope == MTLC_MEMORY_SCOPE_DEFAULT) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Atomic scope must be work_item, subgroup, workgroup, device, or system");
        return NULL;
      }
    } else if (!strcmp(option, "space")) {
      address_space = type_checker_atomic_space_name(identifier);
      if (address_space == MTLC_ADDRESS_SPACE_DEFAULT) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Atomic space must be global or workgroup");
        return NULL;
      }
      if (fixed_space && address_space != inferred_space) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Atomic space conflicts with the storage operand's declared address space");
        return NULL;
      }
    } else {
      type_checker_set_error_at_location(checker,
                                         call->arguments[i]->location,
                                         "Unknown atomic option '%s'", option);
      return NULL;
    }
  }
  if (!is_compare_exchange && have_failure_order) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "failure_order is only valid for atomic_compare_exchange");
    return NULL;
  }
  if (is_compare_exchange) {
    if (!have_failure_order) {
      failure_order = order == MTLC_MEMORY_ORDER_SEQ_CST
                          ? MTLC_MEMORY_ORDER_SEQ_CST
                      : (order == MTLC_MEMORY_ORDER_ACQUIRE ||
                         order == MTLC_MEMORY_ORDER_ACQ_REL)
                          ? MTLC_MEMORY_ORDER_ACQUIRE
                          : MTLC_MEMORY_ORDER_RELAXED;
    }
    if (!type_checker_atomic_failure_valid(order, failure_order)) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Atomic failure_order may not be release/acq_rel or stronger than success order");
      return NULL;
    }
  } else {
    failure_order = MTLC_MEMORY_ORDER_RELAXED;
  }
  if (is_load && order != MTLC_MEMORY_ORDER_RELAXED &&
      order != MTLC_MEMORY_ORDER_ACQUIRE &&
      order != MTLC_MEMORY_ORDER_SEQ_CST) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Atomic load order must be relaxed, acquire, or seq_cst");
    return NULL;
  }
  if (is_store && order != MTLC_MEMORY_ORDER_RELAXED &&
      order != MTLC_MEMORY_ORDER_RELEASE &&
      order != MTLC_MEMORY_ORDER_SEQ_CST) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Atomic store order must be relaxed, release, or seq_cst");
    return NULL;
  }
  if (scope == MTLC_MEMORY_SCOPE_DEFAULT) {
    scope = address_space == MTLC_ADDRESS_SPACE_WORKGROUP
                ? MTLC_MEMORY_SCOPE_WORKGROUP
                : MTLC_MEMORY_SCOPE_DEVICE;
  }
  if (address_space == MTLC_ADDRESS_SPACE_WORKGROUP &&
      scope > MTLC_MEMORY_SCOPE_WORKGROUP) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Workgroup atomics cannot request device or system scope");
    return NULL;
  }

  char selected[64];
  snprintf(selected, sizeof(selected), "%s_%s", stem,
           value_type == checker->builtin_uint64 ? "u64" : "u32");
  char *replacement = strdup(selected);
  if (!replacement) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Out of memory resolving atomic built-in");
    return NULL;
  }
  mettle_free_string(call->function_name);
  call->function_name = replacement;
  call->is_gpu_atomic = 1;
  call->atomic_address_space = address_space;
  call->atomic_memory_order = order;
  call->atomic_failure_order = failure_order;
  call->atomic_memory_scope = scope;
  return is_store ? checker->builtin_void : value_type;
}

/* Neutral per-work-item staging surface. The copy span is expressed as a
 * compile-time element count so frontends retain element typing while a
 * backend may divide the byte span into native transactions. Commit/wait are
 * explicit because completion is not implied by an ordinary barrier. */
static Type *type_checker_async_copy_builtin(TypeChecker *checker,
                                             ASTNode *expression,
                                             CallExpression *call,
                                             int *handled) {
  int is_copy = 0, is_commit = 0, is_wait = 0;
  *handled = 0;
  if (!call || !call->function_name || call->object) return NULL;
  if (!strcmp(call->function_name, "async_copy_workgroup")) {
    is_copy = 1;
  } else if (!strcmp(call->function_name, "async_copy_commit")) {
    is_commit = 1;
  } else if (!strcmp(call->function_name, "async_copy_wait")) {
    is_wait = 1;
  } else {
    return NULL;
  }
  *handled = 1;

  FunctionDeclaration *owner =
      checker->current_function_decl &&
              checker->current_function_decl->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)checker->current_function_decl->data
          : NULL;
  if (!owner || !owner->is_kernel) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Asynchronous workgroup copies are only legal directly inside a GPU kernel");
    return NULL;
  }

  call->is_gpu_async_copy = 1;
  call->async_copy_cache = MTLC_ASYNC_CACHE_ALL;
  call->async_copy_transaction_bytes = 4;
  if (is_commit) {
    if (call->argument_count != 0) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "async_copy_commit expects no arguments");
      return NULL;
    }
    return checker->builtin_void;
  }
  if (is_wait) {
    NumberLiteral *literal =
        call->argument_count == 1 && call->arguments[0] &&
                call->arguments[0]->type == AST_NUMBER_LITERAL
            ? (NumberLiteral *)call->arguments[0]->data
            : NULL;
    if (!literal || literal->is_float || literal->int_value < 0 ||
        literal->int_value > 7 ||
        (call->argument_names && call->argument_names[0])) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "async_copy_wait expects one compile-time pending-group count in [0, 7]");
      return NULL;
    }
    call->async_copy_pending_groups = (uint32_t)literal->int_value;
    return checker->builtin_void;
  }

  if (!is_copy || call->argument_count < 3) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "async_copy_workgroup expects destination, source, and a compile-time element count");
    return NULL;
  }
  for (size_t i = 0; i < 3; i++) {
    if (call->argument_names && call->argument_names[i]) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "The first three async_copy_workgroup operands are positional");
      return NULL;
    }
  }
  Type *destination = type_checker_infer_type(checker, call->arguments[0]);
  Type *source = type_checker_infer_type(checker, call->arguments[1]);
  Type *destination_element =
      destination && (destination->kind == TYPE_POINTER ||
                      destination->kind == TYPE_ARRAY)
          ? destination->base_type
          : NULL;
  Type *source_element =
      source && (source->kind == TYPE_POINTER || source->kind == TYPE_ARRAY)
          ? source->base_type
          : NULL;
  int scalar_element =
      destination_element &&
      (destination_element->kind == TYPE_INT8 ||
       destination_element->kind == TYPE_INT16 ||
       destination_element->kind == TYPE_INT32 ||
       destination_element->kind == TYPE_INT64 ||
       destination_element->kind == TYPE_UINT8 ||
       destination_element->kind == TYPE_UINT16 ||
       destination_element->kind == TYPE_UINT32 ||
       destination_element->kind == TYPE_UINT64 ||
       destination_element->kind == TYPE_BOOL ||
       destination_element->kind == TYPE_FLOAT32 ||
       destination_element->kind == TYPE_FLOAT64);
  if (!destination_element || !source_element ||
      !scalar_element ||
      destination_element->kind != source_element->kind ||
      destination_element->size != source_element->size ||
      destination_element->size == 0 || destination_element->size > 8) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "async_copy_workgroup requires matching scalar source and destination element types");
    return NULL;
  }
  NumberLiteral *count =
      call->arguments[2] && call->arguments[2]->type == AST_NUMBER_LITERAL
          ? (NumberLiteral *)call->arguments[2]->data
          : NULL;
  if (!count || count->is_float || count->int_value <= 0 ||
      count->int_value > 4096 ||
      (unsigned long long)count->int_value >
          65536ull / destination_element->size) {
    type_checker_set_error_at_location(
        checker, call->arguments[2]->location,
        "async_copy_workgroup element count must be a compile-time value producing 1..65536 bytes");
    return NULL;
  }
  uint32_t element_count = (uint32_t)count->int_value;
  size_t copy_bytes = destination_element->size * (size_t)element_count;
  if ((copy_bytes & 3u) != 0) {
    type_checker_set_error_at_location(
        checker, call->arguments[2]->location,
        "async_copy_workgroup byte span must be a multiple of four");
    return NULL;
  }

  for (size_t i = 3; i < call->argument_count; i++) {
    const char *option = call->argument_names ? call->argument_names[i] : NULL;
    const char *value = type_checker_tensor_option_identifier(call->arguments[i]);
    if (!option ||
        (strcmp(option, "cache") != 0 &&
         strcmp(option, "transaction") != 0)) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "async_copy_workgroup accepts only named cache and transaction options");
      return NULL;
    }
    for (size_t prior = 3; prior < i; prior++) {
      if (call->argument_names[prior] &&
          !strcmp(call->argument_names[prior], option)) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Duplicate async_copy_workgroup option '%s'", option);
        return NULL;
      }
    }
    if (!strcmp(option, "cache") && value && !strcmp(value, "all")) {
      call->async_copy_cache = MTLC_ASYNC_CACHE_ALL;
    } else if (!strcmp(option, "cache") && value &&
               !strcmp(value, "global")) {
      call->async_copy_cache = MTLC_ASYNC_CACHE_GLOBAL;
    } else if (!strcmp(option, "transaction")) {
      NumberLiteral *transaction =
          call->arguments[i] &&
                  call->arguments[i]->type == AST_NUMBER_LITERAL
              ? (NumberLiteral *)call->arguments[i]->data
              : NULL;
      if (!transaction || transaction->is_float ||
          (transaction->int_value != 4 && transaction->int_value != 8 &&
           transaction->int_value != 16)) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "async copy transaction must be 4, 8, or 16 bytes");
        return NULL;
      }
      call->async_copy_transaction_bytes =
          (uint32_t)transaction->int_value;
    } else {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "async copy cache must be all or global");
      return NULL;
    }
  }
  if (copy_bytes % call->async_copy_transaction_bytes != 0) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "async copy byte span must be divisible by its transaction size");
    return NULL;
  }
  if (call->async_copy_cache == MTLC_ASYNC_CACHE_GLOBAL &&
      call->async_copy_transaction_bytes != 16) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "global-only async copy caching requires 16-byte transactions");
    return NULL;
  }
  call->async_copy_element_count = element_count;
  return checker->builtin_void;
}

int type_checker_tensor_option_u32(TypeChecker *checker, ASTNode *node,
                                   const char *name, uint32_t maximum,
                                   uint32_t *out_value) {
  NumberLiteral *literal =
      node && node->type == AST_NUMBER_LITERAL
          ? (NumberLiteral *)node->data
          : NULL;
  if (!literal || literal->is_float || literal->int_value <= 0 ||
      (unsigned long long)literal->int_value > maximum) {
    type_checker_set_error_at_location(
        checker, node ? node->location : (SourceLocation){0},
        "Tensor option '%s' must be an integer in [1, %u]", name,
        (unsigned)maximum);
    return 0;
  }
  *out_value = (uint32_t)literal->int_value;
  return 1;
}

static int type_checker_tensor_option_bool(TypeChecker *checker, ASTNode *node,
                                           const char *name,
                                           uint8_t *out_value) {
  const char *identifier = type_checker_tensor_option_identifier(node);
  if (identifier && (strcmp(identifier, "true") == 0 ||
                     strcmp(identifier, "false") == 0)) {
    *out_value = strcmp(identifier, "true") == 0;
    return 1;
  }
  NumberLiteral *literal =
      node && node->type == AST_NUMBER_LITERAL
          ? (NumberLiteral *)node->data
          : NULL;
  if (literal && !literal->is_float &&
      (literal->int_value == 0 || literal->int_value == 1)) {
    *out_value = (uint8_t)literal->int_value;
    return 1;
  }
  type_checker_set_error_at_location(
      checker, node ? node->location : (SourceLocation){0},
      "Tensor option '%s' must be true or false", name);
  return 0;
}

MtlcTensorElement type_checker_tensor_element_name(const char *name) {
  if (!name) return MTLC_TENSOR_ELEMENT_INVALID;
  if (!strcmp(name, "f16") || !strcmp(name, "float16"))
    return MTLC_TENSOR_ELEMENT_FLOAT16;
  if (!strcmp(name, "bf16") || !strcmp(name, "bfloat16"))
    return MTLC_TENSOR_ELEMENT_BFLOAT16;
  if (!strcmp(name, "tf32")) return MTLC_TENSOR_ELEMENT_TFLOAT32;
  if (!strcmp(name, "f32") || !strcmp(name, "float32"))
    return MTLC_TENSOR_ELEMENT_FLOAT32;
  if (!strcmp(name, "f64") || !strcmp(name, "float64"))
    return MTLC_TENSOR_ELEMENT_FLOAT64;
  if (!strcmp(name, "e4m3") || !strcmp(name, "fp8_e4m3"))
    return MTLC_TENSOR_ELEMENT_FLOAT8_E4M3;
  if (!strcmp(name, "e5m2") || !strcmp(name, "fp8_e5m2"))
    return MTLC_TENSOR_ELEMENT_FLOAT8_E5M2;
  if (!strcmp(name, "e2m3") || !strcmp(name, "fp6_e2m3"))
    return MTLC_TENSOR_ELEMENT_FLOAT6_E2M3;
  if (!strcmp(name, "e3m2") || !strcmp(name, "fp6_e3m2"))
    return MTLC_TENSOR_ELEMENT_FLOAT6_E3M2;
  if (!strcmp(name, "e2m1") || !strcmp(name, "fp4_e2m1"))
    return MTLC_TENSOR_ELEMENT_FLOAT4_E2M1;
  if (!strcmp(name, "ue8m0")) return MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
  if (!strcmp(name, "ue4m3")) return MTLC_TENSOR_ELEMENT_SCALE_UE4M3;
  if (!strcmp(name, "i8") || !strcmp(name, "int8"))
    return MTLC_TENSOR_ELEMENT_INT8;
  if (!strcmp(name, "u8") || !strcmp(name, "uint8"))
    return MTLC_TENSOR_ELEMENT_UINT8;
  if (!strcmp(name, "i4") || !strcmp(name, "int4"))
    return MTLC_TENSOR_ELEMENT_INT4;
  if (!strcmp(name, "u4") || !strcmp(name, "uint4"))
    return MTLC_TENSOR_ELEMENT_UINT4;
  if (!strcmp(name, "b1") || !strcmp(name, "bit1"))
    return MTLC_TENSOR_ELEMENT_BIT1;
  if (!strcmp(name, "i32") || !strcmp(name, "int32"))
    return MTLC_TENSOR_ELEMENT_INT32;
  return MTLC_TENSOR_ELEMENT_INVALID;
}

MtlcTensorLayout type_checker_tensor_layout_name(const char *name) {
  if (name && (!strcmp(name, "row") || !strcmp(name, "row_major")))
    return MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  if (name && (!strcmp(name, "col") || !strcmp(name, "column_major")))
    return MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  return MTLC_TENSOR_LAYOUT_INVALID;
}

static TypeKind type_checker_tensor_storage_kind(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return TYPE_UINT16;
  case MTLC_TENSOR_ELEMENT_TFLOAT32:
  case MTLC_TENSOR_ELEMENT_FLOAT32:
    return TYPE_FLOAT32;
  case MTLC_TENSOR_ELEMENT_FLOAT64:
    return TYPE_FLOAT64;
  case MTLC_TENSOR_ELEMENT_INT8:
    return TYPE_INT8;
  case MTLC_TENSOR_ELEMENT_INT32:
    return TYPE_INT32;
  default:
    return TYPE_UINT8;
  }
}

int type_checker_tensor_pointer_matches(Type *type,
                                        MtlcTensorElement element) {
  return type && type->kind == TYPE_POINTER && type->base_type &&
         type->base_type->kind == type_checker_tensor_storage_kind(element);
}

static int type_checker_tensor_dimension_option(const char *option,
                                                const char *prefix,
                                                unsigned *dimension) {
  size_t prefix_length;
  if (!option || !prefix || !dimension) return 0;
  prefix_length = strlen(prefix);
  if (strncmp(option, prefix, prefix_length) != 0 ||
      option[prefix_length] < '0' || option[prefix_length] > '4' ||
      option[prefix_length + 1] != '\0')
    return 0;
  *dimension = (unsigned)(option[prefix_length] - '0');
  return 1;
}

static int type_checker_tensor_option_u64(TypeChecker *checker, ASTNode *node,
                                          const char *name,
                                          uint64_t *out_value) {
  NumberLiteral *literal =
      node && node->type == AST_NUMBER_LITERAL
          ? (NumberLiteral *)node->data
          : NULL;
  if (!literal || literal->is_float || literal->int_value <= 0) {
    type_checker_set_error_at_location(
        checker, node ? node->location : (SourceLocation){0},
        "Tensor option '%s' must be a positive compile-time integer", name);
    return 0;
  }
  *out_value = (uint64_t)literal->int_value;
  return 1;
}

/* A rank-aware transfer is deliberately described in ordinary tensor
 * geometry. `view` is an optional provider-prepared acceleration handle; the
 * raw pointer/extents/strides remain complete semantics for portable replay. */
static Type *type_checker_tensor_transfer_builtin(TypeChecker *checker,
                                                  ASTNode *expression,
                                                  CallExpression *call,
                                                  int *handled) {
  MtlcTensorTransferDesc desc = {0};
  int have_rank = 0, have_element = 0, have_direction = 0;
  int have_extent[MTLC_TENSOR_MAX_RANK] = {0};
  int have_stride[MTLC_TENSOR_MAX_RANK] = {0};
  int have_tile[MTLC_TENSOR_MAX_RANK] = {0};
  int have_element_stride[MTLC_TENSOR_MAX_RANK] = {0};
  size_t coordinate_arguments[MTLC_TENSOR_MAX_RANK];
  size_t view_argument = SIZE_MAX;
  *handled = 0;
  if (!call || !call->function_name || call->object ||
      strcmp(call->function_name, "tensor_transfer_workgroup") != 0)
    return NULL;
  *handled = 1;
  for (size_t dimension = 0; dimension < MTLC_TENSOR_MAX_RANK; dimension++)
    coordinate_arguments[dimension] = SIZE_MAX;
  FunctionDeclaration *owner =
      checker->current_function_decl &&
              checker->current_function_decl->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)checker->current_function_decl->data
          : NULL;
  if (!owner || !owner->is_kernel) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Tensor transfers are only legal directly inside a GPU kernel");
    return NULL;
  }
  if (call->argument_count < 2 ||
      (call->argument_names &&
       (call->argument_names[0] || call->argument_names[1]))) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "tensor_transfer_workgroup expects positional destination and source pointers followed by named geometry");
    return NULL;
  }
  desc.direction = MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP;
  desc.packing = MTLC_TENSOR_PACKING_LOGICAL;
  desc.bounds = MTLC_TENSOR_BOUNDS_ZERO;
  desc.scope = MTLC_MEMORY_SCOPE_WORKGROUP;
  for (size_t i = 2; i < call->argument_count; i++) {
    const char *option = call->argument_names ? call->argument_names[i] : NULL;
    const char *identifier =
        type_checker_tensor_option_identifier(call->arguments[i]);
    unsigned dimension = 0;
    if (!option) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "Tensor transfer geometry and coordinates must be named");
      return NULL;
    }
    for (size_t prior = 2; prior < i; prior++) {
      if (call->argument_names[prior] &&
          !strcmp(call->argument_names[prior], option)) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Duplicate tensor transfer option '%s'", option);
        return NULL;
      }
    }
    if (!strcmp(option, "rank")) {
      uint32_t rank = 0;
      if (!type_checker_tensor_option_u32(checker, call->arguments[i], option,
                                          MTLC_TENSOR_MAX_RANK, &rank))
        return NULL;
      desc.rank = (uint8_t)rank;
      have_rank = 1;
    } else if (!strcmp(option, "element")) {
      desc.element = type_checker_tensor_element_name(identifier);
      if (desc.element == MTLC_TENSOR_ELEMENT_INVALID) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Unknown tensor transfer element format");
        return NULL;
      }
      have_element = 1;
    } else if (!strcmp(option, "direction")) {
      if (identifier &&
          (!strcmp(identifier, "load") ||
           !strcmp(identifier, "global_to_workgroup"))) {
        desc.direction = MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP;
      } else if (identifier &&
                 (!strcmp(identifier, "store") ||
                  !strcmp(identifier, "workgroup_to_global"))) {
        desc.direction = MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL;
      } else {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Tensor transfer direction must be load/global_to_workgroup or store/workgroup_to_global");
        return NULL;
      }
      have_direction = 1;
    } else if (!strcmp(option, "bounds")) {
      if (!identifier || strcmp(identifier, "zero")) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "Tensor transfer bounds mode must be zero");
        return NULL;
      }
    } else if (!strcmp(option, "view")) {
      view_argument = i;
    } else if (type_checker_tensor_dimension_option(option, "extent",
                                                    &dimension)) {
      if (!type_checker_tensor_option_u64(checker, call->arguments[i], option,
                                          &desc.global_extent[dimension]))
        return NULL;
      have_extent[dimension] = 1;
    } else if (type_checker_tensor_dimension_option(option, "stride",
                                                    &dimension)) {
      if (!type_checker_tensor_option_u64(
              checker, call->arguments[i], option,
              &desc.global_stride_bytes[dimension]))
        return NULL;
      have_stride[dimension] = 1;
    } else if (type_checker_tensor_dimension_option(option, "tile",
                                                    &dimension)) {
      if (!type_checker_tensor_option_u32(
              checker, call->arguments[i], option, UINT32_MAX,
              &desc.tile_extent[dimension]))
        return NULL;
      have_tile[dimension] = 1;
    } else if (type_checker_tensor_dimension_option(option, "element_stride",
                                                    &dimension)) {
      if (!type_checker_tensor_option_u32(
              checker, call->arguments[i], option, UINT32_MAX,
              &desc.element_stride[dimension]))
        return NULL;
      have_element_stride[dimension] = 1;
    } else if (type_checker_tensor_dimension_option(option, "coordinate",
                                                    &dimension)) {
      coordinate_arguments[dimension] = i;
    } else {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "Unknown tensor transfer option '%s'", option);
      return NULL;
    }
  }
  if (!have_rank || !have_element) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Tensor transfer requires rank and element options");
    return NULL;
  }
  (void)have_direction;
  for (size_t dimension = 0; dimension < MTLC_TENSOR_MAX_RANK; dimension++) {
    if (dimension < desc.rank) {
      if (!have_extent[dimension] || !have_stride[dimension] ||
          !have_tile[dimension] || coordinate_arguments[dimension] == SIZE_MAX) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Tensor transfer rank %u requires extent%u, stride%u, tile%u, and coordinate%u",
            (unsigned)desc.rank, (unsigned)dimension, (unsigned)dimension,
            (unsigned)dimension, (unsigned)dimension);
        return NULL;
      }
      if (!have_element_stride[dimension])
        desc.element_stride[dimension] = 1;
      Type *coordinate = type_checker_infer_type(
          checker, call->arguments[coordinate_arguments[dimension]]);
      if (coordinate != checker->builtin_int32) {
        type_checker_set_error_at_location(
            checker, call->arguments[coordinate_arguments[dimension]]->location,
            "Tensor transfer coordinate%u must have int32 type",
            (unsigned)dimension);
        return NULL;
      }
    } else if (have_extent[dimension] || have_stride[dimension] ||
               have_tile[dimension] || have_element_stride[dimension] ||
               coordinate_arguments[dimension] != SIZE_MAX) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Tensor transfer option for dimension %u exceeds rank %u",
          (unsigned)dimension, (unsigned)desc.rank);
      return NULL;
    }
  }
  Type *destination =
      type_checker_infer_type(checker, call->arguments[0]);
  Type *source = type_checker_infer_type(checker, call->arguments[1]);
  if (!type_checker_tensor_pointer_matches(destination, desc.element) ||
      !type_checker_tensor_pointer_matches(source, desc.element)) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Tensor transfer source and destination pointer storage must match the element format");
    return NULL;
  }
  if (view_argument != SIZE_MAX) {
    Type *view = type_checker_infer_type(checker,
                                         call->arguments[view_argument]);
    if (!view || view->kind != TYPE_POINTER || !view->base_type ||
        view->base_type->kind != TYPE_UINT8) {
      type_checker_set_error_at_location(
          checker, call->arguments[view_argument]->location,
          "Tensor transfer view must be a uint8* provider handle");
      return NULL;
    }
  }
  if (!mtlc_tensor_transfer_desc_is_valid(&desc)) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Invalid tensor transfer descriptor (check byte strides, tile volume, and unused dimensions)");
    return NULL;
  }
  call->is_tensor_transfer = 1;
  call->tensor_transfer_desc = desc;
  call->tensor_transfer_view_argument = view_argument;
  memcpy(call->tensor_transfer_coordinate_arguments, coordinate_arguments,
         sizeof(coordinate_arguments));
  return checker->builtin_void;
}

static int type_checker_tensor_unsigned_scalar(const Type *type) {
  if (!type) return 0;
  return type->kind == TYPE_UINT8 || type->kind == TYPE_UINT16 ||
         type->kind == TYPE_UINT32 || type->kind == TYPE_UINT64;
}

static Type *type_checker_tensor_mma_builtin(TypeChecker *checker,
                                             ASTNode *expression,
                                             CallExpression *call,
                                             int *handled) {
  MtlcTensorMmaDesc desc = {0};
  int have_m = 0, have_n = 0, have_k = 0;
  int have_a = 0, have_b = 0, have_accumulator = 0, have_result = 0;
  int have_shape = 0;
  int have_stride[4] = {0, 0, 0, 0};
  *handled = 0;
  if (!call || !call->function_name || call->object) {
    return NULL;
  }
  int is_matmul = strcmp(call->function_name, "tensor_matmul") == 0;
  if (!is_matmul && strcmp(call->function_name, "tensor_mma") != 0)
    return NULL;
  const char *operation = is_matmul ? "tensor_matmul" : "tensor_mma";
  size_t positional_count = is_matmul ? 9u : 4u;
  *handled = 1;
  FunctionDeclaration *owner =
      checker->current_function_decl &&
              checker->current_function_decl->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)checker->current_function_decl->data
          : NULL;
  if (!owner || !owner->is_kernel) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "%s is only legal directly inside a GPU kernel", operation);
    return NULL;
  }
  if (call->argument_count <= positional_count) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "%s expects %llu positional operands followed by named compile-time options",
        operation, (unsigned long long)positional_count);
    return NULL;
  }
  for (size_t i = 0; i < positional_count; i++) {
    if (call->argument_names && call->argument_names[i]) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "The first %llu %s operands are positional",
          (unsigned long long)positional_count, operation);
      return NULL;
    }
  }
  desc.math_mode = MTLC_TENSOR_MATH_MULTIPLY_ADD;
  desc.sparsity = MTLC_TENSOR_SPARSITY_DENSE;
  desc.a_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.c_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.d_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.rounding = MTLC_TENSOR_ROUND_DEFAULT;
  desc.overflow = MTLC_TENSOR_OVERFLOW_WRAP;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  call->tensor_metadata_argument = SIZE_MAX;
  call->tensor_a_scale_argument = SIZE_MAX;
  call->tensor_b_scale_argument = SIZE_MAX;
  call->tensor_a_stride_argument = SIZE_MAX;
  call->tensor_b_stride_argument = SIZE_MAX;
  call->tensor_c_stride_argument = SIZE_MAX;
  call->tensor_d_stride_argument = SIZE_MAX;

  for (size_t i = positional_count; i < call->argument_count; i++) {
    const char *option = call->argument_names ? call->argument_names[i] : NULL;
    ASTNode *value = call->arguments[i];
    const char *identifier = type_checker_tensor_option_identifier(value);
    if (!option) {
      type_checker_set_error_at_location(
          checker, value->location,
          "Tensor configuration arguments after the positional operands must be named");
      return NULL;
    }
    for (size_t prior = positional_count; prior < i; prior++) {
      if (call->argument_names[prior] &&
          strcmp(call->argument_names[prior], option) == 0) {
        type_checker_set_error_at_location(checker, value->location,
                                           "Duplicate tensor option '%s'",
                                           option);
        return NULL;
      }
    }
    if (!strcmp(option, "shape")) {
      unsigned m = 0, n = 0, k = 0;
      char tail = 0;
      if (!identifier ||
          sscanf(identifier, "m%un%uk%u%c", &m, &n, &k, &tail) != 3 ||
          m == 0 || n == 0 || k == 0 || m > UINT16_MAX || n > UINT16_MAX ||
          k > UINT16_MAX || have_m || have_n || have_k) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor shape must be an identifier like m16n16k16 and cannot be mixed with m/n/k");
        return NULL;
      }
      desc.m = (uint16_t)m;
      desc.n = (uint16_t)n;
      desc.k = (uint16_t)k;
      have_m = have_n = have_k = have_shape = 1;
    } else if (!strcmp(option, "m") || !strcmp(option, "n") ||
               !strcmp(option, "k")) {
      uint32_t dimension = 0;
      if (have_shape ||
          !type_checker_tensor_option_u32(checker, value, option, UINT16_MAX,
                                          &dimension)) {
        if (have_shape)
          type_checker_set_error_at_location(
              checker, value->location,
              "Tensor shape cannot be mixed with explicit m/n/k options");
        return NULL;
      }
      if (!strcmp(option, "m")) desc.m = (uint16_t)dimension, have_m = 1;
      if (!strcmp(option, "n")) desc.n = (uint16_t)dimension, have_n = 1;
      if (!strcmp(option, "k")) desc.k = (uint16_t)dimension, have_k = 1;
    } else if (!strcmp(option, "input_type") ||
               !strcmp(option, "a_type") || !strcmp(option, "b_type") ||
               !strcmp(option, "output_type") ||
               !strcmp(option, "accumulator_type") ||
               !strcmp(option, "result_type")) {
      MtlcTensorElement element = type_checker_tensor_element_name(identifier);
      if (element == MTLC_TENSOR_ELEMENT_INVALID) {
        type_checker_set_error_at_location(checker, value->location,
                                           "Unknown tensor element format");
        return NULL;
      }
      if (!strcmp(option, "input_type")) {
        desc.a_element = desc.b_element = element;
        have_a = have_b = 1;
      } else if (!strcmp(option, "a_type")) {
        desc.a_element = element;
        have_a = 1;
      } else if (!strcmp(option, "b_type")) {
        desc.b_element = element;
        have_b = 1;
      } else if (!strcmp(option, "output_type")) {
        desc.accumulator_element = desc.result_element = element;
        have_accumulator = have_result = 1;
      } else if (!strcmp(option, "accumulator_type")) {
        desc.accumulator_element = element;
        have_accumulator = 1;
      } else {
        desc.result_element = element;
        have_result = 1;
      }
    } else if (!strcmp(option, "a_layout") ||
               !strcmp(option, "b_layout") ||
               !strcmp(option, "c_layout") ||
               !strcmp(option, "d_layout")) {
      MtlcTensorLayout layout = type_checker_tensor_layout_name(identifier);
      if (layout == MTLC_TENSOR_LAYOUT_INVALID) {
        type_checker_set_error_at_location(checker, value->location,
                                           "Tensor layout must be row or col");
        return NULL;
      }
      if (option[0] == 'a') desc.a_layout = layout;
      if (option[0] == 'b') desc.b_layout = layout;
      if (option[0] == 'c') desc.c_layout = layout;
      if (option[0] == 'd') desc.d_layout = layout;
    } else if (!strcmp(option, "lda") || !strcmp(option, "ldb") ||
               !strcmp(option, "ldc") || !strcmp(option, "ldd")) {
      int slot = option[2] - 'a';
      long long constant = 0;
      uint32_t *descriptor_stride =
          slot == 0   ? &desc.a_leading_dimension
          : slot == 1 ? &desc.b_leading_dimension
          : slot == 2 ? &desc.c_leading_dimension
                      : &desc.d_leading_dimension;
      size_t *runtime_argument =
          slot == 0   ? &call->tensor_a_stride_argument
          : slot == 1 ? &call->tensor_b_stride_argument
          : slot == 2 ? &call->tensor_c_stride_argument
                      : &call->tensor_d_stride_argument;
      have_stride[slot] = 1;
      if (type_checker_eval_integer_constant(value, &constant)) {
        if (constant <= 0 || (unsigned long long)constant > UINT32_MAX) {
          type_checker_set_error_at_location(
              checker, value->location,
              "Tensor option '%s' must be an integer in [1, %u] or a runtime integer expression",
              option, UINT32_MAX);
          return NULL;
        }
        *descriptor_stride = (uint32_t)constant;
      } else {
        Type *stride_type = type_checker_infer_type(checker, value);
        if (!stride_type || !type_checker_is_integer_type(stride_type)) {
          type_checker_set_error_at_location(
              checker, value->location,
              "Runtime tensor option '%s' must have integer type", option);
          return NULL;
        }
        *descriptor_stride = 0;
        *runtime_argument = i;
      }
    } else if (!strcmp(option, "math")) {
      if (identifier && !strcmp(identifier, "multiply_add"))
        desc.math_mode = MTLC_TENSOR_MATH_MULTIPLY_ADD;
      else if (identifier && !strcmp(identifier, "xor_popcount"))
        desc.math_mode = MTLC_TENSOR_MATH_XOR_POPCOUNT;
      else if (identifier && !strcmp(identifier, "and_popcount"))
        desc.math_mode = MTLC_TENSOR_MATH_AND_POPCOUNT;
      else {
        type_checker_set_error_at_location(checker, value->location,
                                           "Unknown tensor math mode");
        return NULL;
      }
    } else if (!strcmp(option, "sparsity")) {
      if (identifier && !strcmp(identifier, "dense"))
        desc.sparsity = MTLC_TENSOR_SPARSITY_DENSE;
      else if (identifier && !strcmp(identifier, "structured_1_to_2"))
        desc.sparsity = MTLC_TENSOR_SPARSITY_STRUCTURED_1_TO_2;
      else if (identifier && !strcmp(identifier, "structured_2_to_4"))
        desc.sparsity = MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4;
      else if (identifier && !strcmp(identifier, "structured_4_to_8"))
        desc.sparsity = MTLC_TENSOR_SPARSITY_STRUCTURED_4_TO_8;
      else {
        type_checker_set_error_at_location(checker, value->location,
                                           "Unknown tensor sparsity mode");
        return NULL;
      }
    } else if (!strcmp(option, "rounding")) {
      if (identifier && !strcmp(identifier, "default"))
        desc.rounding = MTLC_TENSOR_ROUND_DEFAULT;
      else if (identifier && !strcmp(identifier, "nearest_even"))
        desc.rounding = MTLC_TENSOR_ROUND_NEAREST_EVEN;
      else if (identifier && !strcmp(identifier, "toward_zero"))
        desc.rounding = MTLC_TENSOR_ROUND_TOWARD_ZERO;
      else if (identifier && !strcmp(identifier, "down"))
        desc.rounding = MTLC_TENSOR_ROUND_DOWN;
      else if (identifier && !strcmp(identifier, "up"))
        desc.rounding = MTLC_TENSOR_ROUND_UP;
      else {
        type_checker_set_error_at_location(checker, value->location,
                                           "Unknown tensor rounding mode");
        return NULL;
      }
    } else if (!strcmp(option, "overflow")) {
      if (identifier && !strcmp(identifier, "wrap"))
        desc.overflow = MTLC_TENSOR_OVERFLOW_WRAP;
      else if (identifier && !strcmp(identifier, "saturate_finite"))
        desc.overflow = MTLC_TENSOR_OVERFLOW_SATURATE_FINITE;
      else {
        type_checker_set_error_at_location(checker, value->location,
                                           "Unknown tensor overflow mode");
        return NULL;
      }
    } else if (!strcmp(option, "a_scale_mode") ||
               !strcmp(option, "b_scale_mode")) {
      MtlcTensorScaleMode mode = MTLC_TENSOR_SCALE_NONE;
      if (identifier && !strcmp(identifier, "none"))
        mode = MTLC_TENSOR_SCALE_NONE;
      else if (identifier && !strcmp(identifier, "per_tensor"))
        mode = MTLC_TENSOR_SCALE_PER_TENSOR;
      else if (identifier && !strcmp(identifier, "block16"))
        mode = MTLC_TENSOR_SCALE_BLOCK_16;
      else if (identifier && !strcmp(identifier, "block32"))
        mode = MTLC_TENSOR_SCALE_BLOCK_32;
      else {
        type_checker_set_error_at_location(checker, value->location,
                                           "Unknown tensor scale mode");
        return NULL;
      }
      if (option[0] == 'a') desc.a_scale_mode = mode;
      else desc.b_scale_mode = mode;
    } else if (!strcmp(option, "a_scale_type") ||
               !strcmp(option, "b_scale_type")) {
      MtlcTensorElement element = type_checker_tensor_element_name(identifier);
      if (element != MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
          element != MTLC_TENSOR_ELEMENT_SCALE_UE4M3) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor scale type must be ue8m0 or ue4m3");
        return NULL;
      }
      if (option[0] == 'a') desc.a_scale_element = element;
      else desc.b_scale_element = element;
    } else if (!strcmp(option, "a_packing") ||
               !strcmp(option, "b_packing")) {
      MtlcTensorPacking packing;
      if (identifier && (!strcmp(identifier, "logical") ||
                         !strcmp(identifier, "unpacked")))
        packing = MTLC_TENSOR_PACKING_LOGICAL;
      else if (identifier && (!strcmp(identifier, "packed") ||
                              !strcmp(identifier, "dense_subbyte")))
        packing = MTLC_TENSOR_PACKING_DENSE_SUBBYTE;
      else {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor packing must be logical or dense_subbyte");
        return NULL;
      }
      if (option[0] == 'a') desc.a_packing = packing;
      else desc.b_packing = packing;
    } else if (!strcmp(option, "ldsa") || !strcmp(option, "ldsb") ||
               !strcmp(option, "a_scale_ld") ||
               !strcmp(option, "b_scale_ld")) {
      uint32_t dimension = 0;
      if (!type_checker_tensor_option_u32(checker, value, option, UINT32_MAX,
                                          &dimension))
        return NULL;
      if (option[0] == 'a' || option[3] == 'a')
        desc.a_scale_leading_dimension = dimension;
      else
        desc.b_scale_leading_dimension = dimension;
    } else if (!strcmp(option, "transpose_a") ||
               !strcmp(option, "transpose_b")) {
      uint8_t transpose = 0;
      if (!type_checker_tensor_option_bool(checker, value, option, &transpose))
        return NULL;
      if (option[10] == 'a') desc.transpose_a = transpose;
      else desc.transpose_b = transpose;
    } else if (!strcmp(option, "scope")) {
      if (identifier && !strcmp(identifier, "subgroup"))
        desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
      else if (identifier && !strcmp(identifier, "workgroup"))
        desc.scope = MTLC_MEMORY_SCOPE_WORKGROUP;
      else {
        type_checker_set_error_at_location(checker, value->location,
                                           "Tensor scope must be subgroup or workgroup");
        return NULL;
      }
    } else if (!strcmp(option, "metadata")) {
      call->tensor_metadata_argument = i;
    } else if (!strcmp(option, "a_scale")) {
      call->tensor_a_scale_argument = i;
    } else if (!strcmp(option, "b_scale")) {
      call->tensor_b_scale_argument = i;
    } else {
      type_checker_set_error_at_location(checker, value->location,
                                         "Unknown tensor option '%s'", option);
      return NULL;
    }
  }
  if (!have_m || !have_n || !have_k || !have_a || !have_b ||
      !have_accumulator || !have_result) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "%s requires shape (or m/n/k), input types, and accumulator/result types",
        operation);
    return NULL;
  }
  if (is_matmul &&
      (!have_stride[0] || !have_stride[1] || !have_stride[2] ||
       !have_stride[3])) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "tensor_matmul requires explicit lda, ldb, ldc, and ldd for whole matrices");
    return NULL;
  }
  uint32_t a_storage_k = desc.k;
  if (desc.sparsity != MTLC_TENSOR_SPARSITY_DENSE)
    a_storage_k = desc.k / 2u;
  uint32_t a_rows = desc.transpose_a ? a_storage_k : desc.m;
  uint32_t a_columns = desc.transpose_a ? desc.m : a_storage_k;
  uint32_t b_rows = desc.transpose_b ? desc.n : desc.k;
  uint32_t b_columns = desc.transpose_b ? desc.k : desc.n;
  if (!have_stride[0])
    desc.a_leading_dimension =
        desc.a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? a_columns : a_rows;
  if (!have_stride[1])
    desc.b_leading_dimension =
        desc.b_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? b_columns : b_rows;
  if (!have_stride[2])
    desc.c_leading_dimension =
        desc.c_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? desc.n : desc.m;
  if (!have_stride[3])
    desc.d_leading_dimension =
        desc.d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? desc.n : desc.m;
  if (!mtlc_tensor_mma_desc_is_valid(&desc)) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Invalid tensor MMA descriptor");
    return NULL;
  }
  Type *operand_types[4];
  for (size_t i = 0; i < 4; i++) {
    operand_types[i] = type_checker_infer_type(checker, call->arguments[i]);
    if (!operand_types[i]) return NULL;
  }
  if (is_matmul) {
    static const char *control_names[5] = {
        "row origin", "column origin", "problem M", "problem N", "problem K"};
    for (size_t i = 0; i < 5; i++) {
      Type *type = type_checker_infer_type(checker, call->arguments[4 + i]);
      if (!type_checker_tensor_unsigned_scalar(type)) {
        type_checker_set_error_at_location(
            checker, call->arguments[4 + i]->location,
            "tensor_matmul %s must have unsigned integer type",
            control_names[i]);
        return NULL;
      }
    }
  }
  MtlcTensorElement expected_elements[4] = {
      desc.a_element, desc.b_element, desc.accumulator_element,
      desc.result_element};
  const char *operand_names[4] = {"A", "B", "C", "D"};
  for (size_t i = 0; i < 4; i++) {
    if (!type_checker_tensor_pointer_matches(operand_types[i],
                                             expected_elements[i])) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "Tensor operand %s has storage type '%s' incompatible with its element format",
          operand_names[i], operand_types[i]->name ? operand_types[i]->name
                                                   : "unknown");
      return NULL;
    }
  }
  int needs_metadata = desc.sparsity != MTLC_TENSOR_SPARSITY_DENSE;
  int needs_a_scale = desc.a_scale_mode != MTLC_TENSOR_SCALE_NONE;
  int needs_b_scale = desc.b_scale_mode != MTLC_TENSOR_SCALE_NONE;
  if (needs_metadata != (call->tensor_metadata_argument != SIZE_MAX) ||
      needs_a_scale != (call->tensor_a_scale_argument != SIZE_MAX) ||
      needs_b_scale != (call->tensor_b_scale_argument != SIZE_MAX)) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Tensor metadata/scale operands must exactly match sparsity and scale modes");
    return NULL;
  }
  if (call->tensor_metadata_argument != SIZE_MAX) {
    Type *type = type_checker_infer_type(
        checker, call->arguments[call->tensor_metadata_argument]);
    if (!type_checker_tensor_pointer_matches(
            type, MTLC_TENSOR_ELEMENT_UINT8)) {
      type_checker_set_error_at_location(
          checker, call->arguments[call->tensor_metadata_argument]->location,
          "Tensor metadata operand must be a uint8 pointer");
      return NULL;
    }
  }
  size_t scale_indices[2] = {call->tensor_a_scale_argument,
                             call->tensor_b_scale_argument};
  MtlcTensorElement scale_elements[2] = {desc.a_scale_element,
                                         desc.b_scale_element};
  const char *scale_names[2] = {"A", "B"};
  for (size_t i = 0; i < 2; i++) {
    if (scale_indices[i] == SIZE_MAX) continue;
    Type *type =
        type_checker_infer_type(checker, call->arguments[scale_indices[i]]);
    if (!type_checker_tensor_pointer_matches(type, scale_elements[i])) {
      type_checker_set_error_at_location(
          checker, call->arguments[scale_indices[i]]->location,
          "Tensor %s scale has storage type '%s' incompatible with its scale format",
          scale_names[i], type && type->name ? type->name : "unknown");
      return NULL;
    }
  }
  size_t stride_indices[4] = {call->tensor_a_stride_argument,
                              call->tensor_b_stride_argument,
                              call->tensor_c_stride_argument,
                              call->tensor_d_stride_argument};
  for (size_t i = 0; i < 4; i++) {
    if (stride_indices[i] == SIZE_MAX) continue;
    Type *type =
        type_checker_infer_type(checker, call->arguments[stride_indices[i]]);
    if (!type || !type_checker_is_integer_type(type)) {
      type_checker_set_error_at_location(
          checker, call->arguments[stride_indices[i]]->location,
          "Runtime tensor leading dimension must have integer type");
      return NULL;
    }
    if (is_matmul && !type_checker_tensor_unsigned_scalar(type)) {
      type_checker_set_error_at_location(
          checker, call->arguments[stride_indices[i]]->location,
          "Runtime tensor_matmul leading dimensions must have unsigned integer type");
      return NULL;
    }
    if (is_matmul && type->kind == TYPE_UINT64) {
      type_checker_set_error_at_location(
          checker, call->arguments[stride_indices[i]]->location,
          "Runtime tensor_matmul leading dimensions must fit the descriptor's uint32 range");
      return NULL;
    }
  }
  call->is_tensor_mma = !is_matmul;
  call->is_tensor_matmul = is_matmul;
  call->tensor_mma_desc = desc;
  return checker->builtin_void;
}

Type *type_checker_infer_type(TypeChecker *checker, ASTNode *expression) {
  if (!checker || !expression)
    return NULL;

  if (expression->resolved_type) {
    return expression->resolved_type;
  }

  Type *type = type_checker_infer_type_internal(checker, expression);
  expression->resolved_type = type;
  return type;
}

Type *type_checker_infer_type_internal(TypeChecker *checker,
                                              ASTNode *expression) {
  if (!checker || !expression)
    return NULL;

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (literal->is_float) {
      // Floating literals default to float64
      return checker->builtin_float64;
    }
    /* `'a'` is a character, not the number 97. It still widens into every
     * integer silently, so `var code: int32 = 'a';` needs no cast. */
    if (literal->is_char) {
      return checker->builtin_char;
    }

    return type_checker_default_integer_literal_type(checker, literal);
  }

  case AST_STRING_LITERAL:
    // String literals are string type
    return checker->builtin_string;

  case AST_AGGREGATE_LITERAL: {
    /* The literal takes the type of what it initializes; whoever knows that
     * type parked it on the checker just before this call. Consume it so a
     * nested inference cannot pick up a stale target. */
    Type *target = checker->aggregate_target_type;
    checker->aggregate_target_type = NULL;
    return type_checker_check_aggregate_literal(checker, expression, target);
  }

  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)expression->data;
    Symbol *symbol = type_checker_resolve_identifier(checker, id);
    if (!symbol) {
      Type *named = id && id->name
                        ? type_checker_get_type_by_name(checker, id->name)
                        : NULL;
      if (named) {
        return type_checker_type_value(checker, named, expression);
      }
      type_checker_report_undefined_symbol(checker, expression->location,
                                           id->name, "variable");
      return NULL;
    }
    if (symbol->kind == SYMBOL_STRUCT || symbol->kind == SYMBOL_ENUM) {
      if (!symbol->type) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Type '%s' has no complete definition", id->name);
        return NULL;
      }
      return type_checker_type_value(checker, symbol->type, expression);
    }
    if (type_is_comptime_only(symbol->type)) {
      return symbol->type;
    }
    if (checker->current_function &&
        (symbol->kind == SYMBOL_VARIABLE || symbol->kind == SYMBOL_PARAMETER) &&
        symbol->scope && symbol->scope->type != SCOPE_GLOBAL) {
      int skip_uninit_check =
          symbol->type && (symbol->type->kind == TYPE_ARRAY ||
                           symbol->type->kind == TYPE_STRUCT ||
                           symbol->type->kind == TYPE_STRING);
      int known = 0;
      int initialized =
          type_checker_init_tracker_is_initialized(checker, id->name, &known);
      if (!skip_uninit_check && known && !initialized) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Variable '%s' may be used before initialization", id->name);
        return NULL;
      }
    }
    return symbol->type;
  }

  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binop = (BinaryExpression *)expression->data;
    return type_checker_check_binary_expression(checker, binop,
                                                expression->location);
  }

  case AST_CLOSURE_ADAPT_EXPRESSION: {
    /* The closure-adapt pass wrapped a thin function value (`&func`, or a
     * non-capturing lambda) that flowed into an `Fn(...)` boundary. The wrapper
     * calls a generated adapter constructor at IR-lowering time; here it simply
     * types as the closure signature it was synthesized for. */
    ClosureAdapt *adapt = (ClosureAdapt *)expression->data;
    if (!adapt || !adapt->ctor_name || !adapt->inner) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Internal: closure adapter was not synthesized");
      return NULL;
    }
    if (!type_checker_infer_type(checker, adapt->inner)) {
      return NULL;
    }
    Type **ptypes = NULL;
    if (adapt->param_count > 0) {
      ptypes = malloc(adapt->param_count * sizeof(Type *));
      if (!ptypes) {
        return NULL;
      }
      for (size_t i = 0; i < adapt->param_count; i++) {
        ptypes[i] =
            type_checker_get_type_by_name(checker, adapt->param_types[i]);
        if (!ptypes[i]) {
          type_checker_set_error_at_location(
              checker, expression->location,
              "Unknown adapter parameter type '%s'", adapt->param_types[i]);
          free(ptypes);
          return NULL;
        }
      }
    }
    Type *adapt_return_type =
        adapt->return_type
            ? type_checker_get_type_by_name(checker, adapt->return_type)
            : checker->builtin_void;
    if (!adapt_return_type) {
      adapt_return_type = checker->builtin_void;
    }
    Type *closure_type = type_create_function_pointer(
        ptypes, adapt->param_count, adapt_return_type);
    free(ptypes);
    if (!closure_type) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Failed to create adapted closure type");
      return NULL;
    }
    char adapt_sig[1024];
    {
      size_t off = 0;
      int wrote = snprintf(adapt_sig, sizeof(adapt_sig), "Fn(");
      if (wrote > 0)
        off += (size_t)wrote;
      for (size_t i = 0; i < adapt->param_count && off < sizeof(adapt_sig);
           i++) {
        wrote = snprintf(adapt_sig + off, sizeof(adapt_sig) - off, "%s%s",
                         i ? "," : "", adapt->param_types[i]);
        if (wrote > 0)
          off += (size_t)wrote;
      }
      if (off < sizeof(adapt_sig))
        snprintf(adapt_sig + off, sizeof(adapt_sig) - off, ")->%s",
                 adapt->return_type ? adapt->return_type : "void");
    }
    closure_type->name = (char *)string_intern(adapt_sig);
    closure_type->closure_env = type_checker_closure_env_sentinel();
    return closure_type;
  }

  case AST_LAMBDA_EXPRESSION: {
    /* Closure conversion lifted the lambda body and recorded the symbol its
     * value derives from. A non-capturing lambda is the address of its lifted
     * function (a thin function pointer, like `&func`). A capturing lambda has
     * the user-facing type fn(params)->R tagged with its environment struct so
     * call sites know to dispatch through the captured environment. */
    FunctionDeclaration *lam = (FunctionDeclaration *)expression->data;
    if (!lam || !lam->name) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Internal: lambda was not converted");
      return NULL;
    }

    /* The lambda value is an 8-byte function pointer (thin) or closure pointer.
     * Name its type with its canonical signature `fn(a,b)->R` (no spaces) so an
     * inferred `var f = <lambda>` local is sized as a pointer by the backend. */
    char sig[1024];
    {
      size_t off = 0;
      int wrote = snprintf(sig, sizeof(sig), "fn(");
      if (wrote > 0)
        off += (size_t)wrote;
      for (size_t i = 0; i < lam->parameter_count && off < sizeof(sig); i++) {
        wrote = snprintf(sig + off, sizeof(sig) - off, "%s%s", i ? "," : "",
                         lam->parameter_types[i]);
        if (wrote > 0)
          off += (size_t)wrote;
      }
      if (off < sizeof(sig))
        snprintf(sig + off, sizeof(sig) - off, ")->%s",
                 lam->return_type ? lam->return_type : "void");
    }

    if (lam->captured_count > 0) {
      Type **ptypes = NULL;
      if (lam->parameter_count > 0) {
        ptypes = malloc(lam->parameter_count * sizeof(Type *));
        if (!ptypes) {
          return NULL;
        }
        for (size_t i = 0; i < lam->parameter_count; i++) {
          ptypes[i] =
              type_checker_get_type_by_name(checker, lam->parameter_types[i]);
          if (!ptypes[i]) {
            type_checker_set_error_at_location(
                checker, expression->location,
                "Unknown lambda parameter type '%s'", lam->parameter_types[i]);
            free(ptypes);
            return NULL;
          }
        }
      }
      Type *return_type =
          lam->return_type
              ? type_checker_get_type_by_name(checker, lam->return_type)
              : checker->builtin_void;
      if (!return_type) {
        return_type = checker->builtin_void;
      }
      Type *closure_type = type_create_function_pointer(
          ptypes, lam->parameter_count, return_type);
      free(ptypes);
      if (!closure_type) {
        type_checker_set_error_at_location(checker, expression->location,
                                           "Failed to create closure type");
        return NULL;
      }
      /* The closure_env tag, not the name, drives call dispatch. */
      closure_type->name = (char *)string_intern(sig);
      closure_type->closure_env =
          type_checker_get_type_by_name(checker, lam->env_struct_name);
      return closure_type;
    }

    Symbol *sym = symbol_table_lookup(checker->symbol_table, lam->name);
    if (!sym || sym->kind != SYMBOL_FUNCTION) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Internal: lifted lambda function '%s' "
                                         "not found",
                                         lam->name);
      return NULL;
    }
    Type *return_type = sym->data.function.return_type;
    if (!return_type) {
      return_type = checker->builtin_void;
    }
    Type *fp_type = type_create_function_pointer(
        sym->data.function.parameter_types,
        sym->data.function.parameter_count, return_type);
    if (!fp_type) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Failed to create lambda type");
      return NULL;
    }
    fp_type->name = (char *)string_intern(sig);
    return fp_type;
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unop = (UnaryExpression *)expression->data;
    if (!unop || !unop->operator || !unop->operand) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Invalid unary expression");
      return NULL;
    }

    if (strcmp(unop->operator, "&") == 0) {
      // Check if operand is an identifier that refers to a function
      if (unop->operand->type == AST_IDENTIFIER) {
        Identifier *id = (Identifier *)unop->operand->data;
        if (id && id->name) {
          Symbol *sym = type_checker_resolve_identifier(checker, id);
          if (sym && sym->kind == SYMBOL_FUNCTION) {
            // Taking address of a function - create function pointer type
            Type **param_types = sym->data.function.parameter_types;
            size_t param_count = sym->data.function.parameter_count;
            Type *return_type = sym->data.function.return_type;
            if (!return_type) {
              return_type = checker->builtin_void;
            }
            Type *fp_type = type_create_function_pointer(
                param_types, param_count, return_type);
            if (!fp_type) {
              type_checker_set_error_at_location(
                  checker, expression->location,
                  "Failed to create function pointer type");
              return NULL;
            }
            return fp_type;
          }
        }
      }

      // Not a function reference - treat as regular address-of
      if (!type_checker_is_lvalue_expression(unop->operand)) {
        type_checker_set_error_at_location(
            checker, unop->operand->location,
            "Address-of operator requires an assignable expression");
        return NULL;
      }

      Type *operand_type = type_checker_infer_type(checker, unop->operand);
      if (!operand_type) {
        return NULL;
      }
      if (type_checker_reject_comptime_escape(checker, unop->operand->location,
                                              operand_type)) {
        return NULL;
      }

      const char *operand_name =
          operand_type->name ? operand_type->name : "unknown";
      size_t pointer_name_len = strlen(operand_name) + 2;
      char *pointer_name = malloc(pointer_name_len);
      if (!pointer_name) {
        type_checker_set_error_at_location(checker, expression->location,
                                           "Memory allocation failed");
        return NULL;
      }
      snprintf(pointer_name, pointer_name_len, "%s*", operand_name);

      Type *pointer_type = type_checker_get_type_by_name(checker, pointer_name);
      free(pointer_name);
      if (!pointer_type) {
        /* The spelling is not a registered name, which is ordinary for a
         * function-pointer or other structural operand. Build the pointer
         * from the type instead of from its spelling. */
        pointer_type = type_checker_pointer_to(checker, operand_type);
      }
      if (!pointer_type) {
        type_checker_set_error_at_location(checker, expression->location,
                                           "Failed to resolve pointer type");
        return NULL;
      }

      return pointer_type;
    }

    if (strcmp(unop->operator, "*") == 0) {
      Type *operand_type = type_checker_infer_type(checker, unop->operand);
      if (!operand_type) {
        return NULL;
      }
      if (type_checker_reject_comptime_escape(checker, unop->operand->location,
                                              operand_type)) {
        return NULL;
      }
      if (type_checker_is_null_pointer_constant(unop->operand)) {
        type_checker_set_error_at_location(checker, expression->location,
                                           "Null pointer dereference");
        return NULL;
      }
      if (operand_type->kind != TYPE_POINTER || !operand_type->base_type) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "Dereference operator requires a pointer operand");
        return NULL;
      }
      if (type_checker_is_rawptr_type(operand_type) &&
          type_checker_reject_rawptr_element_use(checker, expression->location,
                                                 "dereference")) {
        return NULL;
      }
      return operand_type->base_type;
    }

    Type *operand_type = type_checker_infer_type(checker, unop->operand);
    if (!operand_type) {
      return NULL;
    }
    if (type_checker_reject_comptime_escape(checker, unop->operand->location,
                                            operand_type)) {
      return NULL;
    }

    if (strcmp(unop->operator, "+") == 0 || strcmp(unop->operator, "-") == 0) {
      if (!type_checker_is_numeric_type(operand_type)) {
        type_checker_report_type_mismatch(checker, unop->operand->location,
                                          "numeric type", operand_type->name);
        return NULL;
      }
      return operand_type;
    }

    if (strcmp(unop->operator, "~") == 0) {
      if (!type_checker_is_integer_type(operand_type)) {
        type_checker_report_type_mismatch(checker, unop->operand->location,
                                          "integer type", operand_type->name);
        return NULL;
      }
      return operand_type;
    }

    if (strcmp(unop->operator, "!") == 0) {
      if (!type_checker_is_numeric_type(operand_type) &&
          operand_type->kind != TYPE_POINTER) {
        type_checker_report_type_mismatch(checker, unop->operand->location,
                                          "numeric or pointer type",
                                          operand_type->name);
        return NULL;
      }
      // Logical NOT always produces an int32 (0 or 1)
      return checker->builtin_int32;
    }

    type_checker_set_error_at_location(checker, expression->location,
                                       "Unsupported unary operator '%s'",
                                       unop->operator);
    return NULL;
  }

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    if (call && call->function_name) {
      if (strcmp(call->function_name, "sizeof") == 0) {
        Type *sized_type =
            type_checker_resolve_sizeof_argument(checker, call,
                                                 expression->location);
        return sized_type ? checker->builtin_int64 : NULL;
      }

      if (strcmp(call->function_name, "offsetof") == 0) {
        long long offset = 0;
        if (!type_checker_eval_offsetof(checker, call, expression->location,
                                        &offset)) {
          return NULL;
        }
        return checker->builtin_int64;
      }

      if (strcmp(call->function_name, "typeof") == 0) {
        Type *referred = type_checker_resolve_typeof_argument(
            checker, call, expression->location);
        if (!referred) {
          return NULL;
        }
        return type_checker_type_value(checker, referred, expression);
      }

      if (strcmp(call->function_name, "layoutof") == 0) {
        long long digest = 0;
        if (!type_checker_eval_layoutof(checker, call, expression->location,
                                        &digest)) {
          return NULL;
        }
        return checker->builtin_int64;
      }

      if (strcmp(call->function_name, "fieldof") == 0) {
        ComptimeValue field = comptime_none();
        if (!type_checker_eval_fieldof(checker, call, expression->location,
                                       &field)) {
          return NULL;
        }
        Type *owner = type_checker_type_from_index(
            checker, field.as.field_ref.type_index);
        return type_checker_field_value(checker, owner,
                                        field.as.field_ref.field_index,
                                        expression);
      }

      if (strcmp(call->function_name, "static_assert") == 0) {
        return type_checker_validate_static_assert(checker, call,
                                                   expression->location)
                   ? checker->builtin_void
                   : NULL;
      }

      /* String interpolation conversion, synthesized by the parser for each
       * "{expr}" part. It types as string for every value the runtime can
       * render; IR lowering picks the mettle_string_from_* helper. */
      if (strcmp(call->function_name, "__mtl_interp") == 0) {
        if (call->argument_count != 1 || !call->arguments ||
            !call->arguments[0]) {
          type_checker_set_error_at_location(
              checker, expression->location,
              "String interpolation takes exactly one value");
          return NULL;
        }
        Type *value_type =
            type_checker_infer_type(checker, call->arguments[0]);
        if (!value_type) {
          return NULL;
        }
        switch (value_type->kind) {
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
        case TYPE_FLOAT32:
        case TYPE_FLOAT64:
        case TYPE_STRING:
          return checker->builtin_string;
        default:
          type_checker_set_error_at_location(
              checker, call->arguments[0]->location,
              "Cannot interpolate a value of type '%s' into a string; "
              "interpolation takes integers, characters, booleans, floats, "
              "and strings",
              value_type->name ? value_type->name : "?");
          return NULL;
        }
      }

    }

    {
      int handled = 0;
      Type *index_type = type_checker_gpu_index_builtin(
          checker, expression, call, &handled);
      if (handled) return index_type;
    }

    {
      int handled = 0;
      Type *atomic_type = type_checker_atomic_builtin(
          checker, expression, call, &handled);
      if (handled) return atomic_type;
    }

    {
      int handled = 0;
      Type *async_type = type_checker_async_copy_builtin(
          checker, expression, call, &handled);
      if (handled) return async_type;
    }

    {
      int handled = 0;
      Type *transfer_type = type_checker_tensor_transfer_builtin(
          checker, expression, call, &handled);
      if (handled) return transfer_type;
    }

    {
      int handled = 0;
      Type *tensor_type = type_checker_tensor_mma_builtin(
          checker, expression, call, &handled);
      if (handled) return tensor_type;
    }

    {
      int handled = 0;
      Type *epilogue_type = type_checker_tensor_epilogue_builtin(
          checker, expression, call, &handled);
      if (handled) return epilogue_type;
    }

    {
      int handled = 0;
      Type *subgroup_type = type_checker_subgroup_builtin(
          checker, expression, call, &handled);
      if (handled) return subgroup_type;
    }

    /* Qualified tagged-enum constructor `EnumName.Variant(args)`: the parser
     * shapes this as a method call whose receiver is the enum-name identifier.
     * Strip the receiver so downstream code treats it as a direct constructor
     * call on `Variant`, the variant constructor symbol already exists in the
     * global scope (registered at enum-decl time). */
    if (call && call->object && call->object->type == AST_IDENTIFIER &&
        call->function_name) {
      Identifier *recv_id = (Identifier *)call->object->data;
      if (recv_id && recv_id->name) {
        Symbol *recv_sym = type_checker_resolve_identifier(checker, recv_id);
        if (recv_sym && recv_sym->kind == SYMBOL_ENUM) {
          const char *enum_name =
              recv_sym->type && recv_sym->type->name ? recv_sym->type->name
                                                     : recv_sym->name;
          size_t qualified_len =
              strlen(enum_name) + 2 + strlen(call->function_name) + 1;
          char *qualified = malloc(qualified_len);
          if (!qualified) {
            return NULL;
          }
          snprintf(qualified, qualified_len, "%s__%s", enum_name,
                   call->function_name);
          if (symbol_table_lookup(checker->symbol_table, qualified)) {
            if (!string_is_interned(call->function_name)) {
              free(call->function_name);
            }
            call->function_name = (char *)string_intern(qualified);
          }
          free(qualified);
          call->object = NULL;
        }
      }
    }

    // Method calls on threading types:
    // Thread.join(), Mutex.new(), mutex.lock(), guard (unlock via drop),
    // Atomic.new(), atomic.load/store/fetch_add/fetch_sub/cas(),
    // channel(), tx.send(), rx.recv()
    if (call && call->object) {
      if (!type_checker_desugar_struct_method_call(checker, expression, call)) {
        return NULL;
      }
      /* The desugar may have rewritten a closure/fn-pointer field call
       * (`obj.field(args)`) into a function-pointer call; re-dispatch on the new
       * node kind, since the CallExpression `call` is no longer valid. */
      if (expression->type != AST_FUNCTION_CALL) {
        return type_checker_infer_type_internal(checker, expression);
      }
    }

    Symbol *func_symbol =
        symbol_table_lookup(checker->symbol_table, call->function_name);
    if (!func_symbol) {
      type_checker_report_undefined_symbol(
          checker, expression->location,
          call->written_name ? call->written_name : call->function_name,
          "function");
      return NULL;
    }

    /* Variable with function pointer type can be called like a function */
    if ((func_symbol->kind == SYMBOL_VARIABLE ||
         func_symbol->kind == SYMBOL_PARAMETER) &&
        func_symbol->type &&
        func_symbol->type->kind == TYPE_FUNCTION_POINTER) {
      call->is_indirect_call = 1;
      Type *fp_type = func_symbol->type;
      call->callee_closure_env = fp_type->closure_env;
      if (call->argument_count != fp_type->fn_param_count) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg),
                 "Function pointer expects %llu arguments, got %llu",
                 (unsigned long long)fp_type->fn_param_count,
                 (unsigned long long)call->argument_count);
        type_checker_set_error_at_location(checker, expression->location,
                                           error_msg);
        return NULL;
      }
      for (size_t i = 0; i < call->argument_count; i++) {
        Type *arg_type = type_checker_infer_type(checker, call->arguments[i]);
        if (!arg_type)
          return NULL;
        if (type_checker_reject_comptime_escape(
                checker, call->arguments[i]->location, arg_type)) {
          return NULL;
        }
        Type *param_type = fp_type->fn_param_types[i];
        int is_null =
            (param_type && param_type->kind == TYPE_POINTER &&
             type_checker_is_null_pointer_constant(call->arguments[i]));
        if (!is_null &&
            !type_checker_is_assignable_from(checker, param_type, arg_type,
                                             call->arguments[i])) {
          type_checker_report_assign_mismatch(checker, call->arguments[i],
                                              call->arguments[i]->location,
                                              param_type, arg_type);
          return NULL;
        }
      }
      return fp_type->fn_return_type;
    }

    if (func_symbol->kind == SYMBOL_TAGGED_ENUM_CONSTRUCTOR) {
      Type *enum_type = func_symbol->data.constructor.enum_type;
      Type *payload_type = func_symbol->data.constructor.payload_type;
      size_t expected_args = payload_type ? 1 : 0;

      if (call->argument_count != expected_args) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg),
                 "Constructor '%s' expects %llu arguments, got %llu",
                 call->function_name, (unsigned long long)expected_args,
                 (unsigned long long)call->argument_count);
        type_checker_set_error_at_location(checker, expression->location,
                                           error_msg);
        return NULL;
      }

      if (payload_type && call->argument_count == 1) {
        Type *arg_type = type_checker_infer_type(checker, call->arguments[0]);
        if (!arg_type) {
          return NULL;
        }
        if (!type_checker_is_assignable_from(checker, payload_type, arg_type,
                                             call->arguments[0])) {
          type_checker_report_assign_mismatch(checker, call->arguments[0],
                                              call->arguments[0]->location,
                                              payload_type, arg_type);
          return NULL;
        }
      }

      return enum_type;
    }

    if (func_symbol->kind != SYMBOL_FUNCTION) {
      const char *symbol_type =
          (func_symbol->kind == SYMBOL_VARIABLE ||
           func_symbol->kind == SYMBOL_PARAMETER) ? "variable"
          : (func_symbol->kind == SYMBOL_STRUCT) ? "struct"
                                                 : "symbol";
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg), "'%s' is a %s, not a function",
               call->function_name, symbol_type);
      type_checker_set_error_at_location(checker, expression->location,
                                         error_msg);
      return NULL;
    }

    // assert/assert_eq are `mettle test` builtins: they exist only in the
    // compile-time interpreter, so reject them outside @test functions
    // (where they would survive into codegen and fail at link).
    if (func_symbol->is_builtin &&
        (strcmp(call->function_name, "assert") == 0 ||
         strcmp(call->function_name, "assert_eq") == 0)) {
      FunctionDeclaration *current_fn =
          checker->current_function_decl && checker->current_function_decl->data
              ? (FunctionDeclaration *)checker->current_function_decl->data
              : NULL;
      if (!current_fn || !current_fn->is_test) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                 "'%s' is a compile-time test builtin and can only be called "
                 "inside a @test function",
                 call->function_name);
        checker->has_error = 1;
        free(checker->error_message);
        checker->error_message = strdup(error_msg);
        if (checker->error_reporter) {
          SourceSpan span = source_span_from_location(
              expression->location, strlen(call->function_name));
          span = error_reporter_span_snap_to_token(checker->error_reporter,
                                                   span, call->function_name);
          error_reporter_add_error_with_span_and_suggestion(
              checker->error_reporter, ERROR_SEMANTIC, span, error_msg,
              "mark the enclosing function @test and run it with `mettle "
              "test`, or use an if + return instead");
        }
        return NULL;
      }
    }

    // Check argument count
    if (call->argument_count != func_symbol->data.function.parameter_count) {
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg),
               "Function '%s' expects %llu arguments, got %llu",
               call->function_name,
               (unsigned long long)func_symbol->data.function.parameter_count,
               (unsigned long long)call->argument_count);
      checker->has_error = 1;
      free(checker->error_message);
      checker->error_message = strdup(error_msg);
      if (checker->error_reporter) {
        SourceSpan span = source_span_from_location(
            expression->location, strlen(call->function_name));
        /* The call node's location points at '('; walk back onto the name. */
        if (span.column > strlen(call->function_name))
          span.column -= strlen(call->function_name);
        span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                                 call->function_name);
        error_reporter_add_error_with_span(checker->error_reporter,
                                           ERROR_SEMANTIC, span, error_msg);
        char label[128];
        snprintf(label, sizeof(label), "expected %llu argument%s, got %llu",
                 (unsigned long long)func_symbol->data.function.parameter_count,
                 func_symbol->data.function.parameter_count == 1 ? "" : "s",
                 (unsigned long long)call->argument_count);
        error_reporter_set_last_label(checker->error_reporter, label);
        type_checker_note_declared_here(checker, func_symbol, "function");
      }
      return NULL;
    }

    // Check each argument type
    for (size_t i = 0; i < call->argument_count; i++) {
      Type *arg_type = type_checker_infer_type(checker, call->arguments[i]);
      if (!arg_type) {
        // Error already set by type inference
        return NULL;
      }
      if (type_checker_reject_comptime_escape(
              checker, call->arguments[i]->location, arg_type)) {
        return NULL;
      }

      Type *param_type = func_symbol->data.function.parameter_types[i];
      int is_null_pointer_arg =
          (param_type && param_type->kind == TYPE_POINTER &&
           type_checker_is_null_pointer_constant(call->arguments[i]));
      if (!is_null_pointer_arg &&
           !type_checker_is_assignable_from(checker, param_type, arg_type,
                                            call->arguments[i])) {
        type_checker_report_assign_mismatch(checker, call->arguments[i],
                                            call->arguments[i]->location,
                                            param_type, arg_type);
        if (func_symbol->data.function.parameter_names &&
            func_symbol->data.function.parameter_names[i] &&
            checker->error_reporter) {
          char label[192];
          snprintf(label, sizeof(label),
                   "parameter '%s' expects '%s', this argument is '%s'",
                   func_symbol->data.function.parameter_names[i],
                   param_type->name, arg_type->name);
          error_reporter_set_last_label(checker->error_reporter, label);
        }
        type_checker_note_declared_here(checker, func_symbol, "function");
        return NULL;
      }

    }

    type_checker_warn_recv_buffer_bounds(checker, call);
    type_checker_warn_memcpy_buffer_bounds(checker, call);

    return func_symbol->data.function.return_type;
  }

  case AST_FUNC_PTR_CALL: {
    FuncPtrCall *fp_call = (FuncPtrCall *)expression->data;
    if (!fp_call || !fp_call->function) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Invalid function pointer call");
      return NULL;
    }

    Type *func_type = type_checker_infer_type(checker, fp_call->function);
    if (!func_type) {
      return NULL;
    }

    /* If expression is identifier resolving to a function, synthesize function
     * pointer type */
    if (func_type->kind != TYPE_FUNCTION_POINTER &&
        fp_call->function->type == AST_IDENTIFIER) {
      Identifier *id = (Identifier *)fp_call->function->data;
      Symbol *sym = type_checker_resolve_identifier(checker, id);
      if (sym && sym->kind == SYMBOL_FUNCTION) {
        Type **param_types = sym->data.function.parameter_types;
        size_t param_count = sym->data.function.parameter_count;
        Type *return_type = sym->data.function.return_type;
        if (!return_type)
          return_type = checker->builtin_void;
        func_type =
            type_create_function_pointer(param_types, param_count, return_type);
        if (!func_type) {
          type_checker_set_error_at_location(
              checker, expression->location,
              "Failed to create function pointer type");
          return NULL;
        }
      }
    }

    if (func_type->kind != TYPE_FUNCTION_POINTER) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "Cannot call non-function-pointer expression");
      return NULL;
    }

    // Check argument count
    if (fp_call->argument_count != func_type->fn_param_count) {
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg),
               "Function pointer expects %llu arguments, got %llu",
               (unsigned long long)func_type->fn_param_count,
               (unsigned long long)fp_call->argument_count);
      type_checker_set_error_at_location(checker, expression->location,
                                         error_msg);
      return NULL;
    }

    // Check each argument type
    for (size_t i = 0; i < fp_call->argument_count; i++) {
      Type *arg_type = type_checker_infer_type(checker, fp_call->arguments[i]);
      if (!arg_type) {
        return NULL;
      }

      Type *param_type = func_type->fn_param_types[i];
      int is_null_pointer_arg =
          (param_type && param_type->kind == TYPE_POINTER &&
           type_checker_is_null_pointer_constant(fp_call->arguments[i]));
      if (!is_null_pointer_arg &&
          !type_checker_is_assignable_from(checker, param_type, arg_type,
                                           fp_call->arguments[i])) {
        type_checker_report_assign_mismatch(checker, fp_call->arguments[i],
                                            fp_call->arguments[i]->location,
                                            param_type, arg_type);
        return NULL;
      }
    }

    // Return the function pointer's return type
    return func_type->fn_return_type;
  }

  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expression->data;

    /* Qualified enum access: `EnumName.Variant`.
     *  - Plain enum:  yields the variant's integer value, typed as the enum.
     *  - Tagged enum, nullary variant: yields a tagged-enum value.
     *  - Tagged enum, payloadful variant: only valid as the callee of a
     *    CallExpression (handled by the call type-checker, which sees the
     *    member-access and looks up the constructor symbol). Here we still
     *    return the enum type so downstream code keeps making progress; the
     *    constructor arity is enforced at call-check time.
     * The object must be an identifier naming an ENUM symbol. */
    if (member->object && member->object->type == AST_IDENTIFIER) {
      Identifier *obj_id = (Identifier *)member->object->data;
      if (obj_id && obj_id->name) {
        Symbol *enum_sym = type_checker_resolve_identifier(checker, obj_id);
        if (enum_sym && enum_sym->kind == SYMBOL_ENUM && enum_sym->type) {
          Type *enum_ty = enum_sym->type;
          if (enum_ty->kind == TYPE_ENUM) {
            /* The type table records every plain enum's members, so resolve
             * against that first. A user enum also publishes its variants as
             * bare globals and is found either way; `Kind` deliberately
             * publishes none, and is only reachable through here. */
            for (size_t i = 0; i < enum_ty->enum_member_count; i++) {
              if (enum_ty->enum_member_names[i] &&
                  strcmp(enum_ty->enum_member_names[i], member->member) == 0) {
                return enum_ty;
              }
            }
            /* Fall back to the bare global for enums declared before the
             * member table was populated. */
            Symbol *variant_sym =
                symbol_table_lookup(checker->symbol_table, member->member);
            if (variant_sym && variant_sym->kind == SYMBOL_CONSTANT &&
                variant_sym->type == enum_ty) {
              return enum_ty;
            }
            type_checker_set_error_at_location(
                checker, expression->location,
                "Enum '%s' has no variant '%s'", obj_id->name, member->member);
            return NULL;
          }
          if (enum_ty->kind == TYPE_TAGGED_ENUM) {
            for (size_t i = 0; i < enum_ty->tagged_variant_count; i++) {
              if (enum_ty->tagged_variant_names &&
                  enum_ty->tagged_variant_names[i] &&
                  strcmp(enum_ty->tagged_variant_names[i], member->member) ==
                      0) {
                return enum_ty;
              }
            }
            type_checker_set_error_at_location(
                checker, expression->location,
                "Tagged enum '%s' has no variant '%s'", obj_id->name,
                member->member);
            return NULL;
          }
        }
      }
    }

    Type *object_type = type_checker_infer_type(checker, member->object);
    /* Member access through a pointer-to-struct auto-dereferences (like C's
     * `->`), matching what IR lowering already does. */
    if (object_type && object_type->kind == TYPE_POINTER &&
        object_type->base_type) {
      object_type = object_type->base_type;
    }
    if (object_type && object_type->kind == TYPE_TYPE) {
      ComptimeValue owner = comptime_none();
      if (!type_checker_eval_comptime(checker, member->object, &owner) ||
          owner.kind != COMPTIME_TYPE_REF) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "field access on 'Type' requires a compile-time type value");
        return NULL;
      }
      Type *referred =
          type_checker_type_from_index(checker, owner.as.type_ref.type_index);
      if (!referred) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "field access on 'Type' refers to an unknown type");
        return NULL;
      }
      /* A declared field wins over a query of the same name: reflection must
       * never shadow what the program itself wrote. */
      int field_index = type_get_field_index(referred, member->member);
      if (field_index >= 0) {
        return type_checker_field_value(checker, referred,
                                        (uint32_t)field_index, expression);
      }
      if (!type_checker_type_member_exists(member->member)) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "type '%s' has no field or query '%s'; a Type answers 'kind', "
            "'name', 'size', 'align', 'fields', 'pointee', 'element', "
            "and 'len'",
            referred->name ? referred->name : "<anonymous>", member->member);
        return NULL;
      }
      ComptimeValue queried = comptime_none();
      if (!type_checker_eval_type_member(checker, owner, member->member,
                                         &queried)) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "type '%s' cannot answer '.%s'",
            referred->name ? referred->name : "<anonymous>", member->member);
        return NULL;
      }
      if (strcmp(member->member, "kind") == 0) {
        return type_checker_kind_result(checker, queried, expression);
      }
      return type_checker_comptime_result(checker, queried, expression);
    }
    if (object_type && object_type->kind == TYPE_SEQUENCE) {
      ComptimeValue answered = comptime_none();
      if (!type_checker_eval_comptime(checker, expression, &answered)) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "a compile-time sequence answers only '.len'; '%s' is not a query",
            member->member);
        return NULL;
      }
      return type_checker_comptime_result(checker, answered, expression);
    }
    if (object_type && object_type->kind == TYPE_FIELD) {
      if (!type_checker_field_member_exists(member->member)) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "'Field' has no member '%s'; it has 'name', 'type', 'offset', and "
            "'index'. Ask '.type' about the field's type, for example "
            "'%s.type.size'",
            member->member,
            member->object && member->object->type == AST_IDENTIFIER &&
                    member->object->data
                ? ((Identifier *)member->object->data)->name
                : "field");
        return NULL;
      }
      ComptimeValue value = comptime_none();
      if (!type_checker_eval_comptime(checker, expression, &value)) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "'.%s' needs a compile-time Field on its left", member->member);
        return NULL;
      }
      return type_checker_comptime_result(checker, value,
                                          expression);
    }
    if (object_type && (object_type->kind == TYPE_STRUCT ||
                        object_type->kind == TYPE_STRING)) {
      // Look up the field type in the struct
      Type *field_type = type_get_field_type(object_type, member->member);
      if (field_type) {
        return field_type;
      } else {
        // Field not found in struct - this is an error
        SourceLocation location = expression->location;
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg),
                 "Field '%s' not found in type '%s'", member->member,
                 object_type->name);
        type_checker_set_error_at_location(checker, location, error_msg);
        return NULL;
      }
    } else if (object_type) {
      // Trying to access member on non-struct type
      SourceLocation location = expression->location;
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg),
               "Cannot access field on non-struct type '%s'",
               object_type->name);
      type_checker_set_error_at_location(checker, location, error_msg);
      return NULL;
    }
    return NULL;
  }

  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *idx = (ArrayIndexExpression *)expression->data;
    if (!idx || !idx->array || !idx->index) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Invalid array indexing expression");
      return NULL;
    }

    Type *array_type = type_checker_infer_type(checker, idx->array);
    if (!array_type) {
      return NULL;
    }

    /* `typeof(T).fields[i]` is answered here, not loaded: a sequence has no
     * storage, and the subscript has to be a compile-time constant because
     * there is nothing to index at run time. */
    if (array_type->kind == TYPE_SEQUENCE) {
      ComptimeValue element = comptime_none();
      if (!type_checker_eval_comptime(checker, expression, &element)) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "a compile-time sequence needs a constant index that is in range");
        return NULL;
      }
      return type_checker_comptime_result(checker, element, expression);
    }

    Type *index_type = type_checker_infer_type(checker, idx->index);
    if (!index_type) {
      return NULL;
    }
    if (type_checker_reject_comptime_escape(checker, idx->array->location,
                                            array_type) ||
        type_checker_reject_comptime_escape(checker, idx->index->location,
                                            index_type)) {
      return NULL;
    }

    if (!type_checker_is_integer_type(index_type)) {
      type_checker_report_type_mismatch(checker, idx->index->location,
                                        "integer type", index_type->name);
      return NULL;
    }

    if (array_type->kind == TYPE_ARRAY || array_type->kind == TYPE_POINTER) {
      if (!array_type->base_type) {
        type_checker_set_error_at_location(checker, expression->location,
                                           "Indexed type has no element type");
        return NULL;
      }
      if (type_checker_is_rawptr_type(array_type) &&
          type_checker_reject_rawptr_element_use(checker, expression->location,
                                                 "index")) {
        return NULL;
      }
      if (array_type->kind == TYPE_POINTER &&
          type_checker_is_null_pointer_constant(idx->array)) {
        type_checker_set_error_at_location(checker, idx->array->location,
                                           "Null pointer dereference");
        return NULL;
      }
      if (array_type->kind == TYPE_ARRAY) {
        long long constant_index = 0;
        if (type_checker_eval_integer_constant(idx->index, &constant_index)) {
          if (constant_index < 0 ||
              (unsigned long long)constant_index >=
                  (unsigned long long)array_type->array_size) {
            type_checker_set_error_at_location(
                checker, idx->index->location,
                "Array index %lld is out of bounds for '%s' (size %zu)",
                constant_index, array_type->name ? array_type->name : "array",
                array_type->array_size);
            return NULL;
          }
        }
      }
      return array_type->base_type;
    }

    /* `s[i]` is the i'th character. A string is a borrowed view of bytes, so
     * this reads one and answers a `char`; writing through it is not offered,
     * because the view may point at a literal in rodata. */
    if (array_type->kind == TYPE_STRING) {
      return checker->builtin_char;
    }

    type_checker_set_error_at_location(checker, expression->location,
                                       "Cannot index non-array type '%s'",
                                       array_type->name);
    return NULL;
  }

  case AST_ASSIGNMENT: {
    Assignment *assignment = (Assignment *)expression->data;
    if (assignment && assignment->value) {
      return type_checker_infer_type(checker, assignment->value);
    }
    return NULL;
  }

  case AST_NEW_EXPRESSION: {
    NewExpression *new_expr = (NewExpression *)expression->data;
    if (!new_expr || !new_expr->type_name) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Invalid 'new' expression");
      return NULL;
    }

    // Look up the type by name
    Symbol *type_symbol =
        symbol_table_lookup(checker->symbol_table, new_expr->type_name);
    if (!type_symbol || type_symbol->kind != SYMBOL_STRUCT) {
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg),
               "Struct type '%s' not found for allocation",
               new_expr->type_name);
      type_checker_set_error_at_location(checker, expression->location,
                                         error_msg);
      return NULL;
    }

    size_t pointer_name_len = strlen(new_expr->type_name) + 2;
    char *pointer_name = malloc(pointer_name_len);
    if (!pointer_name) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Memory allocation failed");
      return NULL;
    }
    snprintf(pointer_name, pointer_name_len, "%s*", new_expr->type_name);

    Type *pointer_type = type_checker_get_type_by_name(checker, pointer_name);
    free(pointer_name);
    if (!pointer_type) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Failed to resolve pointer type");
      return NULL;
    }

    return pointer_type;
  }

  case AST_CAST_EXPRESSION: {
    CastExpression *cast_expr = (CastExpression *)expression->data;
    if (!cast_expr || !cast_expr->type_name || !cast_expr->operand) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Invalid cast expression");
      return NULL;
    }

    Type *target_type =
        type_checker_get_type_by_name(checker, cast_expr->type_name);
    if (!target_type) {
      type_checker_set_error_at_location(checker, expression->location,
                                         "Unknown target type for cast");
      return NULL;
    }
    if (type_checker_reject_no_runtime_repr(checker, expression->location,
                                            target_type)) {
      return NULL;
    }

    Type *operand_type = type_checker_infer_type(checker, cast_expr->operand);
    if (!operand_type) {
      return NULL; // Error already reported
    }
    if (type_checker_reject_comptime_escape(checker, cast_expr->operand->location,
                                            operand_type)) {
      return NULL;
    }

    if (!type_checker_is_cast_valid(operand_type, target_type)) {
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg),
               "Cannot cast from type '%s' to type '%s'", operand_type->name,
               target_type->name);
      type_checker_set_error_at_location(checker, expression->location,
                                         error_msg);
      return NULL;
    }

    type_checker_warn_potential_misaligned_cast(checker, expression, cast_expr,
                                                target_type);

    return target_type;
  }

  case AST_MATCH_STATEMENT: {
    MatchStatement *m = (MatchStatement *)expression->data;
    if (!m || !m->is_expression) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "statement-form 'match' does not yield a value; use "
          "'match (x) { case A: v, default: w }' expression form");
      return NULL;
    }
    return type_checker_check_match_expression(checker, expression);
  }

  default:
    return NULL;
  }
}

int type_checker_check_expression(TypeChecker *checker, ASTNode *expression) {
  if (!checker || !expression)
    return 0;

  // Use type inference to validate the expression
  Type *expr_type = type_checker_infer_type(checker, expression);
  if (!expr_type) {
    return 0;
  }
  if (type_checker_reject_comptime_escape(checker, expression->location,
                                          expr_type)) {
    return 0;
  }
  return 1;
}

// Enhanced binary expression type checking
Type *type_checker_check_binary_expression(TypeChecker *checker,
                                           BinaryExpression *binop,
                                           SourceLocation location) {
  if (!checker || !binop)
    return NULL;

  Type *left_type = type_checker_infer_type(checker, binop->left);
  Type *right_type = type_checker_infer_type(checker, binop->right);

  if (!left_type || !right_type) {
    return NULL; // Error already reported
  }
  if (type_checker_reject_comptime_escape(checker, binop->left->location,
                                          left_type) ||
      type_checker_reject_comptime_escape(checker, binop->right->location,
                                          right_type)) {
    return NULL;
  }

  const char *op = binop->operator;

  // String concatenation
  if (strcmp(op, "+") == 0) {
    if (left_type == checker->builtin_string &&
        right_type == checker->builtin_string) {
      return checker->builtin_string;
    }
  }

  // Pointer arithmetic: allow pointer +/- integer and pointer - pointer.
  if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) {
    int left_is_pointer = left_type->kind == TYPE_POINTER;
    int right_is_pointer = right_type->kind == TYPE_POINTER;
    int left_is_integer = type_checker_is_integer_type(left_type);
    int right_is_integer = type_checker_is_integer_type(right_type);

    if (left_is_pointer || right_is_pointer) {
      if ((type_checker_is_rawptr_type(left_type) ||
           type_checker_is_rawptr_type(right_type)) &&
          type_checker_reject_rawptr_element_use(checker, location,
                                                 "offset")) {
        return NULL;
      }
      if (strcmp(op, "+") == 0) {
        if (left_is_pointer && right_is_integer) {
          return left_type;
        }
        if (right_is_pointer && left_is_integer) {
          return right_type;
        }
      } else { // "-"
        if (left_is_pointer && right_is_integer) {
          return left_type;
        }
        if (left_is_pointer && right_is_pointer &&
            type_checker_types_equal(left_type, right_type)) {
          return checker->builtin_int64;
        }
      }

      type_checker_set_error_at_location(
          checker, location,
          "Pointer arithmetic requires pointer +/- integer or pointer - "
          "pointer of same type");
      return NULL;
    }
  }

  // Arithmetic operators require numeric types
  if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
      strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {

    if (!type_checker_is_numeric_type(left_type)) {
      type_checker_report_type_mismatch(checker, binop->left->location,
                                        "numeric type", left_type->name);
      return NULL;
    }

    if (!type_checker_is_numeric_type(right_type)) {
      type_checker_report_type_mismatch(checker, binop->right->location,
                                        "numeric type", right_type->name);
      return NULL;
    }

    // Modulo operator requires integer types
    if (strcmp(op, "%") == 0) {
      if (!type_checker_is_integer_type(left_type)) {
        type_checker_report_type_mismatch(checker, binop->left->location,
                                          "integer type", left_type->name);
        return NULL;
      }

      if (!type_checker_is_integer_type(right_type)) {
        type_checker_report_type_mismatch(checker, binop->right->location,
                                          "integer type", right_type->name);
        return NULL;
      }
    }

    return type_checker_promote_types(checker, left_type, right_type, op);
  }

  // Bitwise operators
  if (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
      strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
    if (!type_checker_is_integer_type(left_type)) {
      type_checker_report_type_mismatch(checker, binop->left->location,
                                        "integer type", left_type->name);
      return NULL;
    }
    if (!type_checker_is_integer_type(right_type)) {
      type_checker_report_type_mismatch(checker, binop->right->location,
                                        "integer type", right_type->name);
      return NULL;
    }
    return type_checker_promote_types(checker, left_type, right_type, op);
  }

  // Comparison operators
  if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 ||
      strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
    int is_equality = (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
    /* Function pointers compare like any other pointer, which is how code
     * checks whether an entry point was resolved: `if (handler == 0)`. */
    int left_is_pointer = type_checker_type_accepts_null_pointer(left_type);
    int right_is_pointer = type_checker_type_accepts_null_pointer(right_type);

    if (left_is_pointer || right_is_pointer) {
      if (!is_equality) {
        type_checker_set_error_at_location(
            checker, location,
            "Pointer ordering comparisons are not supported");
        return NULL;
      }

      int left_is_null = type_checker_is_null_pointer_constant(binop->left);
      int right_is_null = type_checker_is_null_pointer_constant(binop->right);
      /* A rawptr names no element type, so it compares against a pointer of
       * any element type for the same reason it converts to one: both sides
       * are an address. `if (memcpy(dst, src, n) != dst)` is the shape. */
      int rawptr_pair = left_is_pointer && right_is_pointer &&
                        (type_checker_is_rawptr_type(left_type) ||
                         type_checker_is_rawptr_type(right_type));
      int comparable = (left_is_pointer && right_is_pointer &&
                        type_checker_types_equal(left_type, right_type)) ||
                       rawptr_pair || (left_is_pointer && right_is_null) ||
                       (right_is_pointer && left_is_null);

      if (!comparable) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), "Cannot compare '%s' with '%s'",
                 left_type->name, right_type->name);
        type_checker_set_error_at_location(checker, location, error_msg);
        return NULL;
      }

      return checker->builtin_bool;
    }

    // Both operands should be comparable (same type or compatible)
    if (!type_checker_are_compatible(left_type, right_type)) {
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg), "Cannot compare '%s' with '%s'",
               left_type->name, right_type->name);
      type_checker_set_error_at_location(checker, location, error_msg);
      return NULL;
    }

    return checker->builtin_bool;
  }

  // Logical operators
  if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
    // Both operands should be bool or any integer (treated as boolean)
    int left_ok = type_checker_is_numeric_type(left_type) ||
                  left_type->kind == TYPE_BOOL;
    int right_ok = type_checker_is_numeric_type(right_type) ||
                   right_type->kind == TYPE_BOOL;
    if (!left_ok) {
      type_checker_report_type_mismatch(checker, binop->left->location,
                                        "bool or numeric type", left_type->name);
      return NULL;
    }
    if (!right_ok) {
      type_checker_report_type_mismatch(checker, binop->right->location,
                                        "bool or numeric type", right_type->name);
      return NULL;
    }
    return checker->builtin_bool;
  }

  // Unknown operator
  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg), "Unknown binary operator '%s'", op);
  type_checker_set_error_at_location(checker, location, error_msg);
  return NULL;
}
