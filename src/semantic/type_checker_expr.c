// Type checker: expression type inference and checking.
#include "type_checker_internal.h"
#include "../string_intern.h"

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

    return type_checker_default_integer_literal_type(checker, literal);
  }

  case AST_STRING_LITERAL:
    // String literals are string type
    return checker->builtin_string;

  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)expression->data;
    Symbol *symbol = symbol_table_lookup(checker->symbol_table, id->name);
    if (!symbol) {
      type_checker_report_undefined_symbol(checker, expression->location,
                                           id->name, "variable");
      return NULL;
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
          Symbol *sym = symbol_table_lookup(checker->symbol_table, id->name);
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
      return operand_type->base_type;
    }

    Type *operand_type = type_checker_infer_type(checker, unop->operand);
    if (!operand_type) {
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

      if (strcmp(call->function_name, "static_assert") == 0) {
        return type_checker_validate_static_assert(checker, call,
                                                   expression->location)
                   ? checker->builtin_void
                   : NULL;
      }

    }

    /* Qualified tagged-enum constructor `EnumName.Variant(args)`: the parser
     * shapes this as a method call whose receiver is the enum-name identifier.
     * Strip the receiver so downstream code treats it as a direct constructor
     * call on `Variant` — the variant constructor symbol already exists in the
     * global scope (registered at enum-decl time). */
    if (call && call->object && call->object->type == AST_IDENTIFIER &&
        call->function_name) {
      Identifier *recv_id = (Identifier *)call->object->data;
      if (recv_id && recv_id->name) {
        Symbol *recv_sym =
            symbol_table_lookup(checker->symbol_table, recv_id->name);
        if (recv_sym && recv_sym->kind == SYMBOL_ENUM) {
          /* Drop the receiver — leak-free: the identifier node is owned by
           * the AST tree and freed when the program is freed. */
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
      type_checker_report_undefined_symbol(checker, expression->location,
                                           call->function_name, "function");
      return NULL;
    }

    /* Variable with function pointer type can be called like a function */
    if (func_symbol->kind == SYMBOL_VARIABLE && func_symbol->type &&
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
        Type *param_type = fp_type->fn_param_types[i];
        int is_null =
            (param_type && param_type->kind == TYPE_POINTER &&
             type_checker_is_null_pointer_constant(call->arguments[i]));
        if (!is_null &&
            !type_checker_is_assignable(checker, param_type, arg_type)) {
          type_checker_report_type_mismatch(checker,
                                            call->arguments[i]->location,
                                            param_type->name, arg_type->name);
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
        if (!type_checker_is_assignable(checker, payload_type, arg_type)) {
          type_checker_report_type_mismatch(checker,
                                            call->arguments[0]->location,
                                            payload_type->name, arg_type->name);
          return NULL;
        }
      }

      return enum_type;
    }

    if (func_symbol->kind != SYMBOL_FUNCTION) {
      const char *symbol_type =
          (func_symbol->kind == SYMBOL_VARIABLE) ? "variable"
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

      Type *param_type = func_symbol->data.function.parameter_types[i];
      int is_null_pointer_arg =
          (param_type && param_type->kind == TYPE_POINTER &&
           type_checker_is_null_pointer_constant(call->arguments[i]));
      if (!is_null_pointer_arg &&
           !type_checker_is_assignable(checker, param_type, arg_type)) {
        type_checker_report_type_mismatch_node(checker, call->arguments[i],
                                               param_type->name,
                                               arg_type->name);
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
      Symbol *sym = symbol_table_lookup(checker->symbol_table, id->name);
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
          !type_checker_is_assignable(checker, param_type, arg_type)) {
        type_checker_report_type_mismatch(checker,
                                          fp_call->arguments[i]->location,
                                          param_type->name, arg_type->name);
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
        Symbol *enum_sym =
            symbol_table_lookup(checker->symbol_table, obj_id->name);
        if (enum_sym && enum_sym->kind == SYMBOL_ENUM && enum_sym->type) {
          Type *enum_ty = enum_sym->type;
          if (enum_ty->kind == TYPE_ENUM) {
            /* Plain enum variants live as global SYMBOL_CONSTANTs of the enum
             * type. Look up the variant by its bare name and confirm it
             * belongs to this enum. */
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

    Type *index_type = type_checker_infer_type(checker, idx->index);
    if (!index_type) {
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

    Type *operand_type = type_checker_infer_type(checker, cast_expr->operand);
    if (!operand_type) {
      return NULL; // Error already reported
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
  return expr_type != NULL; // Error already reported if NULL
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
    int left_is_pointer = left_type->kind == TYPE_POINTER;
    int right_is_pointer = right_type->kind == TYPE_POINTER;

    if (left_is_pointer || right_is_pointer) {
      if (!is_equality) {
        type_checker_set_error_at_location(
            checker, location,
            "Pointer ordering comparisons are not supported");
        return NULL;
      }

      int left_is_null = type_checker_is_null_pointer_constant(binop->left);
      int right_is_null = type_checker_is_null_pointer_constant(binop->right);
      int comparable = (left_is_pointer && right_is_pointer &&
                        type_checker_types_equal(left_type, right_type)) ||
                       (left_is_pointer && right_is_null) ||
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
