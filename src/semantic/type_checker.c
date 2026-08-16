// Type checker: lifecycle, function-signature registration, program driver.
#include "type_checker_internal.h"

Symbol *type_checker_resolve_identifier(TypeChecker *checker,
                                        Identifier *identifier) {
  if (!checker || !checker->symbol_table || !identifier ||
      !identifier->name) {
    return NULL;
  }

  /* A generated module-scope declaration is checked with its `comptime for`
   * binding in effect. A module has no scope to hold it in, so it is asked for
   * first, and only while the declaration it belongs to is in flight. */
  Symbol *symbol =
      type_checker_lookup_expansion_binding(checker, identifier->name);
  if (symbol) {
    return symbol;
  }
  symbol = symbol_table_lookup(checker->symbol_table, identifier->name);
  /* `Kind` is registered on first mention rather than at startup, so a program
   * that never reflects allocates none of its type and interns none of its
   * member names -- and --report-expansion can say so rather than ask you to
   * assume it. */
  if (!symbol && strcmp(identifier->name, "Kind") == 0) {
    type_checker_register_kind_enum(checker);
    symbol = symbol_table_lookup(checker->symbol_table, identifier->name);
  }
  if (symbol && symbol->scope) {
    identifier->scope_id = symbol->scope->scope_id;
  }
  return symbol;
}

TypeChecker *type_checker_create(SymbolTable *symbol_table) {
  return type_checker_create_with_error_reporter(symbol_table, NULL);
}

TypeChecker *
type_checker_create_with_error_reporter(SymbolTable *symbol_table,
                                        ErrorReporter *error_reporter) {
  TypeChecker *checker = malloc(sizeof(TypeChecker));
  if (!checker)
    return NULL;

  checker->symbol_table = symbol_table;
  checker->has_error = 0;
  checker->error_message = NULL;
  checker->error_reporter = error_reporter;
  checker->current_function = NULL;
  checker->current_function_decl = NULL;
  checker->loop_depth = 0;
  checker->switch_depth = 0;
  checker->tracked_var_names = NULL;
  checker->tracked_var_initialized = NULL;
  checker->tracked_var_scope_depth = NULL;
  checker->tracked_var_count = 0;
  checker->tracked_var_capacity = 0;
  checker->tracked_scope_markers = NULL;
  checker->tracked_scope_count = 0;
  checker->tracked_scope_capacity = 0;
  checker->tracked_scope_depth = 0;
  checker->tracked_buffer_extents = NULL;
  checker->aggregate_target_type = NULL;

  // Initialize built-in type pointers to NULL
  checker->builtin_int8 = NULL;
  checker->builtin_int16 = NULL;
  checker->builtin_int32 = NULL;
  checker->builtin_int64 = NULL;
  checker->builtin_uint8 = NULL;
  checker->builtin_uint16 = NULL;
  checker->builtin_uint32 = NULL;
  checker->builtin_uint64 = NULL;
  checker->builtin_bool = NULL;
  checker->builtin_float32 = NULL;
  checker->builtin_float64 = NULL;
  checker->builtin_string = NULL;
  checker->builtin_cstring = NULL;
  checker->builtin_void = NULL;
  checker->builtin_type = NULL;
  checker->builtin_field = NULL;
  checker->builtin_sequence = NULL;
  checker->type_table = NULL;
  checker->type_table_count = 0;
  checker->type_table_capacity = 0;
  checker->expansions = NULL;
  checker->comptime_bindings = NULL;
  checker->comptime_binding_count = 0;
  checker->comptime_binding_capacity = 0;
  checker->builtin_kind = NULL;
  checker->sequences = NULL;
  checker->generic_enum_templates = NULL;
  checker->generic_enum_template_count = 0;

  // Initialize built-in types
  type_checker_init_builtin_types(checker);

  // Test builtins: assert(cond) / assert_eq(left, right). Registered always
  // so @test bodies type-check in every build; calling them outside a @test
  // function is rejected at the call site (they only execute under
  // `mettle test`, where the interpreter implements them natively).
  type_checker_register_test_builtin(checker, "assert", 1);
  type_checker_register_test_builtin(checker, "assert_eq", 2);

  return checker;
}

void type_checker_register_test_builtin(TypeChecker *checker, const char *name,
                                        size_t parameter_count) {
  if (!checker || !checker->symbol_table || parameter_count > 2) {
    return;
  }
  if (symbol_table_lookup_current_scope(checker->symbol_table, name)) {
    return;
  }
  Symbol *symbol = symbol_create(name, SYMBOL_FUNCTION, checker->builtin_void);
  if (!symbol) {
    return;
  }
  Type **param_types = malloc(parameter_count * sizeof(Type *));
  char **param_names = malloc(parameter_count * sizeof(char *));
  if (!param_types || !param_names) {
    free(param_types);
    free(param_names);
    symbol_destroy(symbol);
    return;
  }
  static const char *NAMES[2] = {"left", "right"};
  for (size_t i = 0; i < parameter_count; i++) {
    param_types[i] = checker->builtin_int64;
    param_names[i] = strdup(NAMES[i]);
  }
  symbol->data.function.parameter_count = parameter_count;
  symbol->data.function.parameter_types = param_types;
  symbol->data.function.parameter_names = param_names;
  symbol->data.function.return_type = checker->builtin_void;
  symbol->is_extern = 1;
  symbol->is_builtin = 1;
  symbol->is_initialized = 1;
  symbol->link_name = strdup(name);
  if (!symbol_table_declare(checker->symbol_table, symbol)) {
    symbol_destroy(symbol);
  }
}

void type_checker_destroy(TypeChecker *checker) {
  if (checker) {
    // Clean up built-in types
    type_destroy(checker->builtin_int8);
    type_destroy(checker->builtin_int16);
    type_destroy(checker->builtin_int32);
    type_destroy(checker->builtin_int64);
    type_destroy(checker->builtin_uint8);
    type_destroy(checker->builtin_uint16);
    type_destroy(checker->builtin_uint32);
    type_destroy(checker->builtin_uint64);
    type_destroy(checker->builtin_bool);
    type_destroy(checker->builtin_float32);
    type_destroy(checker->builtin_float64);
    type_destroy(checker->builtin_string);
    type_destroy(checker->builtin_cstring);
    type_destroy(checker->builtin_void);
    type_destroy(checker->builtin_type);
    type_destroy(checker->builtin_field);
    type_destroy(checker->builtin_sequence);
    free(checker->type_table);
    type_checker_expansions_destroy(checker->expansions);
    free(checker->comptime_bindings);
    type_checker_sequences_destroy(checker->sequences);
    free(checker->generic_enum_templates);

    for (size_t i = 0; i < checker->tracked_var_count; i++) {
      free(checker->tracked_var_names[i]);
    }
    free(checker->tracked_var_names);
    free(checker->tracked_var_initialized);
    free(checker->tracked_var_scope_depth);
    free(checker->tracked_scope_markers);
    type_checker_buffer_extent_clear(checker);

    free(checker->error_message);
    free(checker);
  }
}

int type_checker_register_function_signature(TypeChecker *checker,
                                                    ASTNode *declaration) {
  if (!checker || !declaration ||
      declaration->type != AST_FUNCTION_DECLARATION) {
    return 0;
  }

  FunctionDeclaration *func_decl = (FunctionDeclaration *)declaration->data;
  if (!func_decl || !func_decl->name)
    return 0;

  if (func_decl->return_type_count > 0 &&
      !type_checker_ensure_multi_return_type(checker, func_decl)) {
    return 0;
  }

  Symbol *existing =
      symbol_table_lookup_current_scope(checker->symbol_table, func_decl->name);
  if (existing)
    return 1;

  Type *return_type = NULL;
  if (func_decl->return_type) {
    return_type =
        type_checker_get_type_by_name(checker, func_decl->return_type);
    if (!return_type)
      return 0;
  } else {
    return_type = checker->builtin_void;
  }

  Type **param_types = NULL;
  if (func_decl->parameter_count > 0) {
    param_types = malloc(func_decl->parameter_count * sizeof(Type *));
    if (!param_types)
      return 0;
    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      param_types[i] =
          type_checker_get_type_by_name(checker, func_decl->parameter_types[i]);
      if (!param_types[i]) {
        free(param_types);
        return 0;
      }
    }
  }

  char **param_names_copy = NULL;
  if (func_decl->parameter_count > 0) {
    param_names_copy = malloc(func_decl->parameter_count * sizeof(char *));
    if (!param_names_copy) {
      free(param_types);
      return 0;
    }
    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      param_names_copy[i] = strdup(func_decl->parameter_names[i]);
    }
  }

  Symbol *func_symbol =
      symbol_create(func_decl->name, SYMBOL_FUNCTION, return_type);
  if (func_symbol) {
    func_symbol->decl_line = declaration->location.line;
    func_symbol->decl_column = declaration->location.column;
    func_symbol->decl_file = declaration->location.filename;
  }
  if (!func_symbol) {
    for (size_t i = 0; i < func_decl->parameter_count; i++)
      free(param_names_copy[i]);
    free(param_names_copy);
    free(param_types);
    return 0;
  }

  func_symbol->data.function.parameter_count = func_decl->parameter_count;
  func_symbol->data.function.parameter_names = param_names_copy;
  func_symbol->data.function.parameter_types = param_types;
  func_symbol->data.function.return_type = return_type;
  /* Kernel identity travels with the signature: this pre-registration is the
   * symbol a `dispatch` earlier in the file resolves against, so it has to
   * know the declaration was a `kernel` and what block shape it declared. */
  func_symbol->is_kernel = func_decl->is_kernel;
  func_symbol->kernel_block[0] = func_decl->kernel_block[0];
  func_symbol->kernel_block[1] = func_decl->kernel_block[1];
  func_symbol->kernel_block[2] = func_decl->kernel_block[2];
  func_symbol->kernel_threads_per_item = func_decl->kernel_threads_per_item;
  func_symbol->is_extern = func_decl->is_extern;
  if (func_decl->is_extern) {
    const char *effective_link_name = type_checker_decl_link_name(
        func_decl->name, func_decl->is_extern, func_decl->link_name);
    func_symbol->link_name =
        effective_link_name ? strdup(effective_link_name) : NULL;
    if (!func_symbol->link_name) {
      symbol_destroy(func_symbol);
      return 0;
    }
  }
  func_symbol->is_initialized = 0;
  func_symbol->is_forward_declaration = 1;

  if (!symbol_table_declare_forward(checker->symbol_table, func_symbol)) {
    symbol_destroy(func_symbol);
    return 0;
  }

  return 1;
}

/* Struct and enum registration, over either the declarations the programmer
 * wrote or the ones expansion generated. Two sweeps rather than one because
 * expansion sits between them: a directive reflects on a type, so the written
 * types have to exist before it runs, and the types it generates only exist
 * after. */
static int type_checker_register_types(TypeChecker *checker, Program *prog,
                                       int generated_only) {
  int ok = 1;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || (decl->type != AST_STRUCT_DECLARATION &&
                  decl->type != AST_ENUM_DECLARATION)) {
      continue;
    }
    ComptimeDeclScope expansion;
    type_checker_enter_expansion_decl(checker, decl, &expansion);
    int is_generated = expansion.bindings_pushed > 0;
    if (is_generated == generated_only) {
      if (decl->type == AST_STRUCT_DECLARATION
              ? !type_checker_process_struct_declaration(checker, decl)
              : !type_checker_process_enum_declaration(checker, decl)) {
        ok = 0;
      }
    }
    type_checker_leave_expansion_decl(checker, &expansion);
  }
  return ok;
}

int type_checker_check_program(TypeChecker *checker, ASTNode *program) {
  if (!checker || !program || program->type != AST_PROGRAM) {
    return 0;
  }

  Program *prog = (Program *)program->data;
  if (!prog)
    return 0;

  // Pass 1: Register struct and enum types. On failure keep going so every
  // bad declaration is reported in one compile, not one per rebuild.
  int ok = 1;
  if (!type_checker_register_types(checker, prog, 0)) {
    ok = 0;
  }

  /* Pass 1.5: expand every module-scope `comptime for` into the declarations it
     generates. It runs after the types are registered, because a directive
     reflects on one (`typeof(T).fields`), and before anything looks for a
     function to check, because what it generates is exactly that. From here on
     nothing downstream can tell a generated declaration from a written one,
     which is the point: contracts, diagnostics and the borrow checker all hold
     generated code to the standard hand-written code is held to. */
  if (!type_checker_expand_comptime_block(checker, program, 1)) {
    ok = 0;
  }
  if (!type_checker_check_composed_names(checker, program)) {
    ok = 0;
  }
  if (!ok)
    return 0;

  /* Types the expansion generated, registered in their own sweep so a
     generated struct can be referred to by a generated function declared
     before it, the same way written ones can. */
  if (!type_checker_register_types(checker, prog, 1)) {
    ok = 0;
  }

  // Pass 2: Register all function signatures so any function can call any other
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_FUNCTION_DECLARATION) {
      ComptimeDeclScope expansion;
      type_checker_enter_expansion_decl(checker, decl, &expansion);
      type_checker_register_function_signature(checker, decl);
      type_checker_leave_expansion_decl(checker, &expansion);
    }
  }

  // Pass 3: Process all declarations (type-check function bodies, etc.)
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type != AST_STRUCT_DECLARATION &&
        decl->type != AST_ENUM_DECLARATION) {
      ComptimeDeclScope expansion;
      type_checker_enter_expansion_decl(checker, decl, &expansion);
      if (!type_checker_process_declaration(checker, decl)) {
        ok = 0;
      }
      type_checker_leave_expansion_decl(checker, &expansion);
    }
  }

  if (!ok)
    return 0;

  // Pass 4: whole-program memory diagnostics. Ownership summaries are
  // inferred over the call graph, then cross-call use-after-free and leak
  // analysis runs with them (type_checker_memory.c). Skipped when earlier
  // passes failed: the AST is not fully typed.
  if (!getenv("METTLE_NO_MEM_INTERPROC") &&
      !type_checker_check_program_memory(checker, program)) {
    return 0;
  }

  return 1;
}

const char *type_checker_decl_link_name(const char *name, int is_extern,
                                               const char *link_name) {
  if (!is_extern) {
    return name;
  }
  if (link_name && link_name[0] != '\0') {
    return link_name;
  }
  return name;
}

const char *type_checker_symbol_link_name(const Symbol *symbol) {
  if (!symbol) {
    return NULL;
  }
  if (symbol->is_extern && symbol->link_name && symbol->link_name[0] != '\0') {
    return symbol->link_name;
  }
  return symbol->name;
}

int type_checker_link_name_matches_symbol(const Symbol *symbol,
                                                 const char *decl_name,
                                                 int decl_is_extern,
                                                 const char *decl_link_name) {
  const char *existing = type_checker_symbol_link_name(symbol);
  const char *incoming =
      type_checker_decl_link_name(decl_name, decl_is_extern, decl_link_name);
  if (!existing || !incoming) {
    return existing == incoming;
  }
  return strcmp(existing, incoming) == 0;
}
